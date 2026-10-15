#include "EasterEggWorld.h"

#include "../Movie/ParticleFxInternal.h" // fx::SpawnParticleAt + the MW2019 explosion part names
#include "../Movie/RagdollBodies.h"
#include "../Movie/CameraBridge.h"
#include "../Platform/JsonParser.h"
#include "../Debug/MvmDebugLog.h"
#include "../../ClientEntitySystem.h"
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/igameevents.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

// MICHAEL BAY MODE. Every death gets an explosion where the victim stood: the MW2019 high-
// explosive parts from our own FX pack (the same files the flashbang On mode already creates by
// name, so they are known to load and spawn). The overlay adds a warm screen flash per blast.
//
// Deaths arrive on the game-event thread and are only QUEUED there; the particles are created on
// the main thread. Deaths replayed by a demo seek arrive as a burst in one frame -- a burst is
// dropped whole, so scrubbing never carpet-bombs the map.
//
// PRECACHE: every part is loaded through ParticleFx_PrecacheSystem when the mode turns on, before
// the first blast (a naked create of an unloaded system crashes particles.dll).

namespace Filmmaker {
namespace {

// The stock HE grenade explosion first (always present), then the MW2019 debris and smoke for size.
// The MW2019 flash core/glow pair is left out: created this way it logs "invalid particle
// definition" (it needs the flashbang path's setup).
const char* const kParts[] = {
	"particles/explosions_fx/explosion_hegrenade_brief.vpcf",
	"particles/explosions_fx/explosion_basic.vpcf",
	"particles/filmmaker/modern/mw2019_explosions_pak/generic_explo_high_debris.vpcf",
	"particles/filmmaker/modern/mw2019_explosions_pak/generic_explo_high_shrapnel.vpcf",
	"particles/filmmaker/modern/mw2019_explosions_pak/generic_explo_high_smoke.vpcf",
	"particles/filmmaker/modern/mw2019_explosions_pak/generic_explo_high_smoke_shock.vpcf",
	"particles/filmmaker/modern/mw2019_explosions_pak/generic_explo_emp_sparks.vpcf",
};
constexpr int kBurstLimit = 4;           // more deaths than this in one frame = a seek replay
constexpr unsigned long long kDedupMs = 800;

struct Death { int userid; unsigned long long ms; };

std::atomic<bool> g_on{ false };
bool g_precacheWanted = false;
std::mutex g_mx;
std::vector<Death> g_queue;
unsigned long long g_lastUserMs[128] = {};
std::atomic<int> g_blasts{ 0 };

void PrecacheParts() {
	int ok = 0;
	for (const char* p : kParts) if (ParticleFx_PrecacheSystem(p)) ++ok;
	MvmDebugLog_LinefAlways("egg.bay", "precache %d/%d", ok, (int)(sizeof(kParts) / sizeof(kParts[0])));
}

} // namespace

void EasterEggBay_Set(bool on) {
	g_on.store(on);
	if (on) g_precacheWanted = true;
	std::lock_guard<std::mutex> lock(g_mx);
	g_queue.clear();
}

void EasterEggBay_OnGameEvent(SOURCESDK::CS2::CGameEvent* event) {
	if (!g_on.load() || !event) return;
	const char* name = event->GetName();
	if (!name || 0 != _stricmp(name, "player_death")) return;
	JsonValue r;
	if (!RagdollBodies::EventJson(event, r)) return;
	const JsonValue* uid = r.Find("userid");
	if (!uid) return;
	const int userid = uid->AsInt(-1);
	if (userid < 0 || userid >= 128) return;
	std::lock_guard<std::mutex> lock(g_mx);
	g_queue.push_back({ userid, GetTickCount64() });
}

void EasterEggBay_RunFrame() {
	if (g_precacheWanted) { g_precacheWanted = false; PrecacheParts(); }
	std::vector<Death> pending;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		pending.swap(g_queue);
	}
	if (pending.empty() || !g_on.load()) return;
	if ((int)pending.size() > kBurstLimit) {
		MvmDebugLog_LinefAlways("egg.bay", "dropped a burst of %zu deaths (seek replay)", pending.size());
		return;
	}
	const unsigned long long now = GetTickCount64();
	for (const Death& d : pending) {
		// Demo and live delivery can both report one death.
		if (g_lastUserMs[d.userid] && now - g_lastUserMs[d.userid] < kDedupMs) continue;
		g_lastUserMs[d.userid] = now;
		CEntityInstance* pawn = RagdollBodies::PawnOfUser(d.userid, nullptr);
		float at[3];
		if (!pawn || !RagdollBodies::SafeOrigin(pawn, at)) continue;
		at[2] += 36.0f; // waist height: the fireball wraps the body instead of the floor
		int made = 0;
		for (const char* p : kParts) if (fx::SpawnParticleAt(p, at) >= 0) ++made;
		g_blasts.fetch_add(1);
		MvmDebugLog_LinefAlways("egg.bay", "boom userid=%d at %.0f %.0f %.0f parts=%d", d.userid, at[0], at[1], at[2], made);
	}
}

int EasterEggBay_BlastCount() { return g_blasts.load(); }

void EasterEggBay_TestBlast() {
	PrecacheParts();
	double origin[3] = {}, angles[3] = {}, fov = 90.0;
	CameraBridge_GetCurrentCamera(origin, angles, fov);
	const double yaw = angles[1] * 3.14159265358979 / 180.0;
	float at[3] = { (float)(origin[0] + std::cos(yaw) * 300.0), (float)(origin[1] + std::sin(yaw) * 300.0), (float)(origin[2] - 30.0) };
	int made = 0;
	for (const char* p : kParts) if (fx::SpawnParticleAt(p, at) >= 0) ++made;
	g_blasts.fetch_add(1);
	MvmDebugLog_LinefAlways("egg.bay", "test boom at %.0f %.0f %.0f parts=%d", at[0], at[1], at[2], made);
}

} // namespace Filmmaker
