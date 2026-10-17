// ParticleFx money-on-headshot: the headshot-particle candidate list the hook swaps to
// the mod's money burst, plus the game-event plumbing (ParticleFx_OnGameEvent) that
// tracks the watched player's weapon_fire for FxDebugHud/FxAlign and logs headshot
// events as mvm_debug breadcrumbs.
//
// Money-on-headshot swaps the game's HEADSHOT-SPECIFIC particles to the mod's money burst.
// The Source 1 mod implements this by replacing impact_fx.pcf with impact_fxmoney.pcf --
// a pure file swap with NO event logic, and that is the correct model here too: the
// candidate systems are only ever created by the engine on an actual headshot hit,
// so the particle's own creation IS the "confirmed headshot" signal.
//
// LESSON (2026-07-02, user report "money only fired when a guy sprayed his teammate's
// head, never on headshot kills"): the previous design required a player_hurt/player_death
// game event to arrive FIRST and arm a 1.5s window the swap then consumed. But within a
// demo tick the impact/blood particles are created BEFORE the hurt/death game events are
// dispatched, so a single lethal headshot armed the window only after its blood had
// already spawned unswapped -- the only thing that ever hit the armed window was a SECOND
// headshot following within 1.5s (i.e. spraying someone's head). Event-gating a particle
// that is already headshot-exclusive was pure downside.
//
// LESSON (2026-10-17, user report "suppressed weapons do not do the money on headshot, at least
// the USP"): current CS2 never creates blood_impact_headshot at all -- a headshot bleeds with the
// ordinary blood_impact_high / med / low (mvm_debug fx log on the MoNaTy USP-S demo; no log since
// has a single blood_impact_headshot). The one headshot-only system left is the helmet spark, so
// money only came on HELMETED heads, and pistol / eco rounds (the USP) rarely have a helmet. The
// swap still covers a helmeted head; any other headshot (player_hurt, hitgroup head) now spawns
// the money burst at the victim's head itself (MoneyShot_PumpMainThread).

#include "ParticleFxInternal.h"

#include "FxAlign.h"                       // FxAlign_Enabled (weapon_fire window arming)
#include "RagdollBodies.h"                 // PawnOfUser
#include "../../ClientEntitySystem.h"      // CEntityInstance: the victim's render eye
#include "../Debug/MvmDebugLog.h" // MvmDebugLog_* (thread-safe flight recorder)
#include "../Platform/JsonParser.h"
#include "../Environment/EnvironmentVfxLighting.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/igameevents.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/tier1/utlstring.h"

#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace Filmmaker {
namespace fx {

std::atomic<int> g_helmetMoneyTick{ -1000 };

namespace {

// A headshot the helmet swap did not cover, waiting for the main thread to spawn its money.
struct PendingMoneyShot { int userid = 0; int tick = -1; unsigned long long wallMs = 0; };
std::mutex g_moneyShotMx;
std::vector<PendingMoneyShot> g_moneyShots;
// A seek replays the events it skips over: those are not hits on screen now (same rule as the
// flashbang detonations, ParticleFxFlashbang.cpp).
constexpr int kMoneyStaleTicks = 8;
constexpr unsigned long long kMoneyStaleMs = 500;

// The victim's head (its render eye, a few units down, as the follow camera's head point), dead or
// alive; false for anything that is not a player pawn or reads no position.
bool HeadOf(CEntityInstance* pawn, float out[3]) {
	__try {
		if (!pawn->IsPlayerPawn()) return false;
		float eye[3] = {};
		pawn->GetRenderEyeOrigin(eye);
		if (!std::isfinite(eye[0]) || !std::isfinite(eye[1]) || !std::isfinite(eye[2])
			|| (eye[0] == 0 && eye[1] == 0 && eye[2] == 0)) return false;
		out[0] = eye[0]; out[1] = eye[1]; out[2] = eye[2] - 3.0f;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::atomic<unsigned long long> g_lastMoneyHeadshotSignature{ 0 };
std::atomic<unsigned long long> g_lastMoneyHeadshotSignatureMs{ 0 };
constexpr unsigned long long kMoneyHeadshotDuplicateMs = 75;

unsigned long long MixMoneyHeadshotSignature(unsigned long long h, unsigned int v) {
	h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2);
	return h ? h : 0xCBF29CE484222325ull;
}

bool MoneyHeadshotEventIsDuplicate(unsigned long long signature) {
	const unsigned long long now = GetTickCount64();
	const unsigned long long lastSignature = g_lastMoneyHeadshotSignature.load(std::memory_order_relaxed);
	const unsigned long long lastMs = g_lastMoneyHeadshotSignatureMs.load(std::memory_order_relaxed);
	if (signature == lastSignature && lastMs && now - lastMs <= kMoneyHeadshotDuplicateMs)
		return true;
	g_lastMoneyHeadshotSignature.store(signature, std::memory_order_relaxed);
	g_lastMoneyHeadshotSignatureMs.store(now, std::memory_order_relaxed);
	return false;
}

typedef bool (*SaveKV3AsJSON_t)(const SOURCESDK::CS2::KeyValues3* kv,
	SOURCESDK::CS2::CUtlString* error, SOURCESDK::CS2::CUtlString* output);

SaveKV3AsJSON_t GetSaveKV3AsJSON() {
	static bool s_tried = false;
	static SaveKV3AsJSON_t s_fn = nullptr;
	if (!s_tried) {
		s_tried = true;
		HMODULE tier0 = GetModuleHandleA("tier0.dll");
		if (tier0)
			s_fn = (SaveKV3AsJSON_t)GetProcAddress(tier0, "?SaveKV3AsJSON@@YA_NPEBVKeyValues3@@PEAVCUtlString@@1@Z");
	}
	return s_fn;
}

bool GameEventDataToJson(SOURCESDK::CS2::CGameEvent* event, JsonValue& out) {
	SaveKV3AsJSON_t fn = GetSaveKV3AsJSON();
	if (!fn)
		return false;
	SOURCESDK::CS2::CUtlString error;
	SOURCESDK::CS2::CUtlString text;
	if (!fn(event->GetDataKeys(), &error, &text) || !text.Get())
		return false;
	return JsonParse(text.Get(), out) && out.type == JsonValue::Type::Object;
}

int JsonEventInt(const JsonValue& root, const char* key, int def = 0) {
	if (const JsonValue* v = root.Find(key))
		return v->AsInt(def);
	return def;
}

bool JsonEventBool(const JsonValue& root, const char* key, bool def = false) {
	if (const JsonValue* v = root.Find(key))
		return v->AsBool(def) || v->AsInt(def ? 1 : 0) != 0;
	return def;
}

bool JsonEventHasPositiveInt(const JsonValue& root, const char* key) {
	if (const JsonValue* v = root.Find(key))
		return v->AsInt(0) > 0;
	return false;
}

struct MoneyHeadshotGateEvent {
	int userid = 0;
	int attacker = 0;
	int hitgroup = 0;
	int dmgHealth = 0;
	bool death = false;
	unsigned long long signature = 0;
};

bool GameEventIsValidMoneyHeadshot(SOURCESDK::CS2::CGameEvent* event, const char* name, MoneyHeadshotGateEvent& out) {
	JsonValue root;
	if (!GameEventDataToJson(event, root))
		return false;

	out.userid = JsonEventInt(root, "userid");
	out.attacker = JsonEventInt(root, "attacker");
	if (out.userid <= 0 || out.attacker <= 0 || out.userid == out.attacker)
		return false;

	bool isHeadshot = false;
	if (0 == _stricmp(name, "player_death")) {
		out.death = true;
		isHeadshot = JsonEventBool(root, "headshot");
	} else if (0 == _stricmp(name, "player_hurt")) {
		out.hitgroup = JsonEventInt(root, "hitgroup");
		out.dmgHealth = JsonEventInt(root, "dmg_health");
		isHeadshot = JsonEventBool(root, "headshot") || out.hitgroup == 1;
		if (!JsonEventHasPositiveInt(root, "dmg_health"))
			return false;
	} else {
		return false;
	}
	if (!isHeadshot)
		return false;

	unsigned long long h = 0xCBF29CE484222325ull;
	h = MixMoneyHeadshotSignature(h, out.death ? 1u : 0u);
	h = MixMoneyHeadshotSignature(h, (unsigned int)out.userid);
	h = MixMoneyHeadshotSignature(h, (unsigned int)out.attacker);
	h = MixMoneyHeadshotSignature(h, (unsigned int)out.hitgroup);
	h = MixMoneyHeadshotSignature(h, (unsigned int)out.dmgHealth);
	out.signature = h;
	return true;
}

} // namespace

bool IsMoneyHeadshotCandidateLower(const char* n) {
	return 0 == std::strcmp(n, "particles/blood_impact/blood_impact_headshot.vpcf")
		|| 0 == std::strcmp(n, "particles/blood_impact/blood_impact_light_headshot.vpcf")
		|| 0 == std::strcmp(n, "particles/impact_fx/impact_helmet_headshot.vpcf");
}

void MoneyShot_PumpMainThread() {
	std::vector<PendingMoneyShot> pending;
	{
		std::lock_guard<std::mutex> lock(g_moneyShotMx);
		if (g_moneyShots.empty()) return;
		pending.swap(g_moneyShots);
	}
	{
		std::lock_guard<std::mutex> lock(g_mx);
		if (!g_enabled || !g_moneyHeadshot) return;
	}
	const int tickNow = g_demoTickNow.load(std::memory_order_relaxed);
	const int helmetTick = g_helmetMoneyTick.load(std::memory_order_relaxed);
	const unsigned long long now = GetTickCount64();
	for (const PendingMoneyShot& m : pending) {
		const bool stale = now - m.wallMs > kMoneyStaleMs
			|| (tickNow >= 0 && m.tick >= 0 && std::abs(tickNow - m.tick) > kMoneyStaleTicks);
		// The helmet spark is created before the hurt event in the same tick and was already swapped.
		const bool helmet = m.tick >= 0 && std::abs(helmetTick - m.tick) <= 1;
		const char* outcome = stale ? "SKIPPED (seek replay)" : helmet ? "covered by the helmet swap" : "no head";
		float at[3] = {};
		if (!stale && !helmet) {
			// The victim's head this tick, read directly: a lethal headshot has already taken its
			// health to 0, which the follow-camera's head point refuses (ResolvePlayerVirtualPoint).
			CEntityInstance* pawn = RagdollBodies::PawnOfUser(m.userid, nullptr);
			if (!pawn) outcome = "no pawn for the victim";
			else if (!HeadOf(pawn, at)) outcome = "no head position";
			else outcome = SpawnParticleAt(kMoneyBurst, at) >= 0 ? "spawned at the head" : "spawn failed";
		}
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("fx.money", "headshot userid=%d eventTick=%d tick=%d helmetTick=%d at %.1f %.1f %.1f: %s",
				m.userid, m.tick, tickNow, helmetTick, at[0], at[1], at[2], outcome);
	}
}

} // namespace fx

// ============================== public API =========================================

using namespace fx;

void ParticleFx_OnGameEvent(SOURCESDK::CS2::CGameEvent* event) {
	if (!event)
		return;
	const char* name = event->GetName();
	if (!name)
		return;
	if (Environment::IsVfxLightingEvent(name)) {
		JsonValue data;
		if (GameEventDataToJson(event,data)) Environment::OnVfxLightingEvent(name,data);
	}
    if (!_stricmp(name,"hegrenade_detonate") || !_stricmp(name,"flashbang_detonate")) {
        JsonValue data;const JsonValue *x,*y,*z;
        if(GameEventDataToJson(event,data)&&(x=data.Find("x"))&&(y=data.Find("y"))&&(z=data.Find("z"))){
            const auto id=data.Find("entityid");
            NativeLightFx_OnDetonate(!_stricmp(name,"flashbang_detonate"),id?int(id->AsNumber(-1)):-1,
                float(x->AsNumber(0)),float(y->AsNumber(0)),float(z->AsNumber(0)));
        }
    }
	if (0 == _stricmp(name, "flashbang_detonate")) {
		JsonValue data;
		const JsonValue *x, *y, *z;
		if (GameEventDataToJson(event, data) && (x = data.Find("x")) && (y = data.Find("y")) && (z = data.Find("z")))
			FlashbangFx_OnDetonate((float)x->AsNumber(0), (float)y->AsNumber(0), (float)z->AsNumber(0));
	}

	// FxDebugHud + FxAlign: only the spectated POV player's weapon_fire should drive the
	// squares / attribute alignment samples, so the fire tick is tracked while either is on.
	if ((g_debugHudEnabled.load(std::memory_order_relaxed) || FxAlign_Enabled())
		&& 0 == _stricmp(name, "weapon_fire")) {
		JsonValue root;
		if (GameEventDataToJson(event, root)) {
			const int uid = JsonEventInt(root, "userid");
			const int watched = g_watchedUserId.load(std::memory_order_relaxed);
			if (watched >= 0 && uid == watched) {
				const int tick = g_demoTickNow.load(std::memory_order_relaxed);
				g_watchedFireDemoTick.store(tick, std::memory_order_relaxed);
				g_watchedFireWallMs.store(GetTickCount64(), std::memory_order_relaxed);
				// #region agent log
				DbgWatchLog("H-watch-fire", "weapon_fire from watched player", uid, watched, tick);
				// #endregion
			}
		}
	}

	{
		std::lock_guard<std::mutex> lock(g_mx);
		if (!g_enabled || !g_moneyHeadshot)
			return;
	}
	if (0 != _stricmp(name, "player_hurt") && 0 != _stricmp(name, "player_death"))
		return;

	MoneyHeadshotGateEvent gate;
	if (!GameEventIsValidMoneyHeadshot(event, name, gate))
		return;
	if (MoneyHeadshotEventIsDuplicate(gate.signature))
		return;
	// The hit itself (player_hurt to the head): the main thread spawns its money unless the helmet
	// swap covered it. player_death repeats the same hit, so it only leaves the breadcrumb.
	if (!gate.death && gate.hitgroup == 1) {
		std::lock_guard<std::mutex> lock(g_moneyShotMx);
		if (g_moneyShots.size() >= 16) g_moneyShots.erase(g_moneyShots.begin());
		g_moneyShots.push_back({ gate.userid, g_demoTickNow.load(std::memory_order_relaxed), GetTickCount64() });
	}

	// Debug breadcrumb only: the swap itself is no longer event-gated (headshot particles
	// are created BEFORE these events within the tick -- see the file comment above). This
	// line lets a log correlate "headshot happened" with the fx.create swap lines around it.
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("fx.event", "moneyshot headshot event %s userid=%d attacker=%d hitgroup=%d dmg_health=%d",
			name, gate.userid, gate.attacker, gate.hitgroup, gate.dmgHealth);
}

} // namespace Filmmaker
