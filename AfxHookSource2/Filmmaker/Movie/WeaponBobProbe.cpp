#include "WeaponBobProbe.h"

#include "FollowTargetProviders.h"          // EntityAt, EntityClass
#include "../Cosmetics/CosmeticModelSwap.h" // ResolveViewmodelWeaponEntityIndex
#include "../Debug/MvmDebugLog.h"  // MvmDebugLog_*
#include "../Platform/JsonParser.h"
#include "../../ClientEntitySystem.h"       // AfxGetSpectatedPawnIndex/UserId, CEntityInstance
#include "../../SchemaSystem.h"             // m_iClip1 / m_bInReload / m_vecVelocity / m_fFlags
#include "../../MirvTime.h"                 // GetCurrentDemoTick
#include "../../../Shared/AfxConsole.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/igameevents.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/tier1/utlstring.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace Filmmaker {
namespace {

std::atomic<bool> g_armed{ false };

// Counters so `bobprobe state` can distinguish "the probe saw nothing happen" from "the probe
// never ran" -- the single most common way a negative result is misread.
std::atomic<unsigned long long> g_frames{ 0 };   // pump calls while armed
std::atomic<unsigned long long> g_sampled{ 0 };  // frames that resolved a muzzle
std::atomic<unsigned long long> g_evReload{ 0 };
std::atomic<unsigned long long> g_evFootstep{ 0 };
std::atomic<unsigned long long> g_evFire{ 0 };

int DemoTick() {
	int tick = -1;
	if (!g_MirvTime.GetCurrentDemoTick(tick)) return -1;
	return tick;
}

// ---- SEH-guarded primitive reads -----------------------------------------------------------
//
// Every one of these can fault: the first-person weapon is a client-only entity that is
// rebuilt on weapon switch, and a schema offset that moved in a CS2 update points at nothing
// in particular. A probe that takes the game down is worse than no probe.

bool SehReadInt(void* base, ptrdiff_t off, int& out) {
	if (!base || 0 == off) return false;
	__try {
		out = *(int*)((unsigned char*)base + off);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool SehReadBool(void* base, ptrdiff_t off, bool& out) {
	if (!base || 0 == off) return false;
	__try {
		out = 0 != *(unsigned char*)((unsigned char*)base + off);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool SehReadVec3(void* base, ptrdiff_t off, float out[3]) {
	if (!base || 0 == off) return false;
	__try {
		const float* p = (const float*)((unsigned char*)base + off);
		out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Named-attachment world pose, SEH-guarded. Copied in spirit from FxAlign's SehReadAttachment
// (same hazard, same guard); kept local so this throwaway probe pulls in no shipping module.
bool SehReadAttachmentPos(CEntityInstance* ent, const char* name, float outPos[3]) {
	if (!ent || !name) return false;
	SOURCESDK::Vector origin;
	SOURCESDK::Quaternion q;
	bool ok = false;
	__try {
		const unsigned char idx = ent->LookupAttachment(name);
		if (idx)
			ok = ent->GetAttachment(idx, origin, q);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (!ok) return false;
	outPos[0] = origin.x; outPos[1] = origin.y; outPos[2] = origin.z;
	return true;
}

// ---- camera basis --------------------------------------------------------------------------
//
// The muzzle's WORLD position is useless for question 4: walking moves it because the player
// moved, which swamps the animation. What matters is the muzzle relative to the eye, in the
// eye's own basis -- that is what the viewer actually sees, and it is what an animation-derived
// onset detector would run on. Standard Source forward/right/up from pitch/yaw (roll ignored;
// it does not move the muzzle relative to the camera enough to matter for a feasibility read).
void CameraRelative(const float eye[3], const float ang[3], const float world[3], float out[3]) {
	const double kDeg = 3.14159265358979323846 / 180.0;
	const double p = ang[0] * kDeg, y = ang[1] * kDeg;
	const double cp = std::cos(p), sp = std::sin(p);
	const double cy = std::cos(y), sy = std::sin(y);
	const double fwd[3] = { cp * cy, cp * sy, -sp };
	const double right[3] = { sy, -cy, 0.0 };
	const double up[3] = { sp * cy, sp * sy, cp };
	const double d[3] = { world[0] - eye[0], world[1] - eye[1], world[2] - eye[2] };
	out[0] = (float)(d[0] * right[0] + d[1] * right[1] + d[2] * right[2]); // right
	out[1] = (float)(d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2]);       // forward
	out[2] = (float)(d[0] * up[0] + d[1] * up[1] + d[2] * up[2]);          // up
}

// ---- event payload -------------------------------------------------------------------------

typedef bool (*SaveKV3AsJSON_t)(const SOURCESDK::CS2::KeyValues3* kv,
	SOURCESDK::CS2::CUtlString* error, SOURCESDK::CS2::CUtlString* output);

SaveKV3AsJSON_t GetSaveKV3AsJSON() {
	static bool s_tried = false;
	static SaveKV3AsJSON_t s_fn = nullptr;
	if (!s_tried) {
		s_tried = true;
		if (HMODULE tier0 = GetModuleHandleA("tier0.dll"))
			s_fn = (SaveKV3AsJSON_t)GetProcAddress(tier0,
				"?SaveKV3AsJSON@@YA_NPEBVKeyValues3@@PEAVCUtlString@@1@Z");
	}
	return s_fn;
}

int EventUserId(SOURCESDK::CS2::CGameEvent* event) {
	SaveKV3AsJSON_t fn = GetSaveKV3AsJSON();
	if (!fn) return -1;
	SOURCESDK::CS2::CUtlString error;
	SOURCESDK::CS2::CUtlString text;
	if (!fn(event->GetDataKeys(), &error, &text) || !text.Get()) return -1;
	JsonValue root;
	if (!JsonParse(text.Get(), root) || root.type != JsonValue::Type::Object) return -1;
	if (const JsonValue* v = root.Find("userid")) return v->AsInt(-1);
	return -1;
}

} // namespace

// ================================ per-frame sampler =========================================

void WeaponBobProbe_RunMainThreadFrame() {
	if (!g_armed.load(std::memory_order_relaxed)) return;
	if (!MvmDebugLog_Active()) return;

	g_frames.fetch_add(1, std::memory_order_relaxed);

	// Throttled: a per-frame line fflushes every write and stalls the game. Every 4th frame is
	// still ~16 Hz at 64 fps -- fast enough to resolve a mag-out transient, which lasts ~100ms.
	static unsigned s_throttle = 0;
	if ((s_throttle++ % 4) != 0) return;

	const int tick = DemoTick();

	// Spectated pawn, with the same fallback order cosmetics and FxAlign use: in a locally
	// recorded demo there is no observer target and the player on screen IS the local viewer.
	const int pawnIdx = AfxGetSpectatedPawnIndex();
	CEntityInstance* pawn = pawnIdx >= 0 ? EntityAt(pawnIdx) : nullptr;
	if (!pawn || !pawn->IsPlayerPawn())
		pawn = AfxGetLocalViewerPawn();
	if (!pawn || !pawn->IsPlayerPawn()) {
		MvmDebugLog_Linef("bob.frame", "tick=%d no-pawn spectatedIdx=%d", tick, pawnIdx);
		return;
	}

	float eye[3] = {};
	float eyeAng[3] = {};
	pawn->GetRenderEyeOrigin(eye);
	pawn->GetRenderEyeAngles(eyeAng);

	// ---- movement inputs a bob would be driven by (question 5's independent variable) ----
	float vel[3] = {};
	const bool haveVel = SehReadVec3(pawn, g_clientDllOffsets.C_BaseEntity.m_vecVelocity, vel);
	const double planar = haveVel ? std::sqrt((double)vel[0] * vel[0] + (double)vel[1] * vel[1]) : -1.0;
	int flags = 0;
	const bool haveFlags = SehReadInt(pawn, g_clientDllOffsets.C_BaseEntity.m_fFlags, flags);
	const int onGround = haveFlags ? ((flags & 1) ? 1 : 0) : -1; // FL_ONGROUND

	// ---- weapon state (questions 3) ----
	int wi = -1;
	{
		const auto h = pawn->GetActiveWeaponHandle();
		if (h.IsValid()) wi = h.GetEntryIndex();
	}
	CEntityInstance* weapon = wi >= 0 ? EntityAt(wi) : nullptr;
	const std::string wclass = EntityClass(weapon);

	int clip = -1;
	SehReadInt(weapon, g_clientDllOffsets.C_BasePlayerWeapon.m_iClip1, clip);
	bool inReload = false;
	const bool haveReload = SehReadBool(weapon, g_clientDllOffsets.C_CSWeaponBase.m_bInReload, inReload);

	// Reload edges get their own undeduplicated line -- the whole point is WHEN it flips, and a
	// deduplicated bob.frame line would collapse every reload in the demo into one entry.
	{
		static int s_lastReload = -1;
		static int s_lastClip = -999;
		const int now = haveReload ? (inReload ? 1 : 0) : -1;
		if (now != s_lastReload || clip != s_lastClip) {
			MvmDebugLog_LinefAlways("bob.reload",
				"tick=%d weapon=%s inReload=%d(have=%d) clip=%d prevClip=%d off_inReload=%lld",
				tick, wclass.empty() ? "-" : wclass.c_str(), now, haveReload ? 1 : 0,
				clip, s_lastClip, (long long)g_clientDllOffsets.C_CSWeaponBase.m_bInReload);
			s_lastReload = now;
			s_lastClip = clip;
		}
	}

	// ---- first-person weapon muzzle (questions 4 and 5) ----
	CEntityInstance* vm = nullptr;
	int vmIndex = -1;
	{
		char dbg[192] = {};
		void* vmEnt = nullptr;
		vmIndex = ResolveViewmodelWeaponEntityIndex((unsigned char*)pawn,
			wclass.empty() ? nullptr : wclass.c_str(), dbg, sizeof(dbg), &vmEnt);
		vm = (CEntityInstance*)vmEnt; // client-only: use the pointer, never EntityAt(vmIndex)
	}

	float mw[3] = {};
	const char* att = nullptr;
	if (vm) {
		if (SehReadAttachmentPos(vm, "muzzle", mw)) att = "muzzle";
		else if (SehReadAttachmentPos(vm, "muzzle_flash", mw)) att = "muzzle_flash";
	}

	if (!att) {
		MvmDebugLog_Linef("bob.frame",
			"tick=%d no-vm-muzzle vmIdx=%d vmEnt=%d weapon=%s speed=%.1f",
			tick, vmIndex, vm ? 1 : 0, wclass.empty() ? "-" : wclass.c_str(), planar);
		return;
	}

	float cam[3] = {};
	CameraRelative(eye, eyeAng, mw, cam);

	// dCam is THE number this probe exists to produce: how far the muzzle moved relative to the
	// camera since the previous sample. If it is ~0 while the gun is visibly reloading on
	// screen, the attachment is not animating per frame and route B is dead. If it oscillates
	// with walking speed while every one of our modules is off, CS2 has its own viewmodel bob
	// and we should scale that rather than add on top of it.
	static bool s_havePrev = false;
	static float s_prev[3] = {};
	static int s_prevTick = -1;
	double d = -1.0;
	if (s_havePrev) {
		const double dx = cam[0] - s_prev[0], dy = cam[1] - s_prev[1], dz = cam[2] - s_prev[2];
		d = std::sqrt(dx * dx + dy * dy + dz * dz);
	}
	// A weapon switch or a seek teleports the muzzle; that is not animation.
	const bool discontinuity = (s_prevTick >= 0 && (tick < s_prevTick || tick - s_prevTick > 16));
	s_prev[0] = cam[0]; s_prev[1] = cam[1]; s_prev[2] = cam[2];
	s_prevTick = tick;
	s_havePrev = true;
	g_sampled.fetch_add(1, std::memory_order_relaxed);

	MvmDebugLog_Linef("bob.frame",
		"tick=%d weapon=%s att=%s cam=(%.3f,%.3f,%.3f) dCam=%.4f cut=%d "
		"speed=%.1f vz=%.1f ground=%d clip=%d inReload=%d",
		tick, wclass.empty() ? "-" : wclass.c_str(), att,
		cam[0], cam[1], cam[2], d, discontinuity ? 1 : 0,
		planar, haveVel ? vel[2] : 0.0f, onGround, clip, inReload ? 1 : 0);
}

// ================================ event tap =================================================

void WeaponBobProbe_OnGameEvent(SOURCESDK::CS2::CGameEvent* event) {
	if (!g_armed.load(std::memory_order_relaxed)) return;
	if (!MvmDebugLog_Active()) return;
	if (!event) return;

	const char* name = event->GetName();
	if (!name) return;

	const bool isReload = 0 == _stricmp(name, "weapon_reload");
	const bool isFootstep = 0 == _stricmp(name, "player_footstep");
	const bool isFire = 0 == _stricmp(name, "weapon_fire");
	if (!isReload && !isFootstep && !isFire) return;

	if (isReload) g_evReload.fetch_add(1, std::memory_order_relaxed);
	else if (isFootstep) g_evFootstep.fetch_add(1, std::memory_order_relaxed);
	else g_evFire.fetch_add(1, std::memory_order_relaxed);

	const int userId = EventUserId(event);
	const int spectated = AfxGetSpectatedUserId();

	// Never deduplicated: the question is how MANY arrive and at which ticks. Dedup would
	// collapse every footstep in the demo to a single line and destroy the answer.
	MvmDebugLog_LinefAlways("bob.event", "tick=%d name=%s userid=%d spectated=%d mine=%d",
		DemoTick(), name, userId, spectated,
		(userId >= 0 && userId == spectated) ? 1 : 0);
}

// ================================ console ===================================================

void WeaponBobProbe_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* sub = argc >= 3 ? args->ArgV(2) : "";

	if (0 == _stricmp(sub, "on") || 0 == _stricmp(sub, "off")) {
		const bool on = 0 == _stricmp(sub, "on");
		g_armed.store(on, std::memory_order_relaxed);
		if (on) {
			g_frames.store(0); g_sampled.store(0);
			g_evReload.store(0); g_evFootstep.store(0); g_evFire.store(0);
		}
		advancedfx::Message("bobprobe: %s%s\n", on ? "ON" : "OFF",
			(on && !MvmDebugLog_Active()) ? " (WARNING: no log open -- run `mvm_debug start`)" : "");
		return;
	}

	if (0 == _stricmp(sub, "state")) {
		advancedfx::Message(
			"bobprobe: armed=%d log=%d frames=%llu sampled=%llu "
			"events(reload=%llu footstep=%llu fire=%llu) "
			"offsets(inReload=%lld clip1=%lld vel=%lld flags=%lld)\n",
			g_armed.load() ? 1 : 0, MvmDebugLog_Active() ? 1 : 0,
			(unsigned long long)g_frames.load(), (unsigned long long)g_sampled.load(),
			(unsigned long long)g_evReload.load(), (unsigned long long)g_evFootstep.load(),
			(unsigned long long)g_evFire.load(),
			(long long)g_clientDllOffsets.C_CSWeaponBase.m_bInReload,
			(long long)g_clientDllOffsets.C_BasePlayerWeapon.m_iClip1,
			(long long)g_clientDllOffsets.C_BaseEntity.m_vecVelocity,
			(long long)g_clientDllOffsets.C_BaseEntity.m_fFlags);
		return;
	}

	advancedfx::Message("%s bobprobe on|off|state - Phase 0 weapon-bob / reload research probe.\n", cmd);
}

} // namespace Filmmaker
