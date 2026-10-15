// Flash grenade = MW2019: the flashbang's detonation effect is OURS, created at the detonation
// point, instead of a swap of CS2's own particle.
//
// WHY NOT A SWAP (measured 2026-10-09, frame captures in build/flashcap): CS2 creates the
// flashbang's particle and stops it at once, so only what a system emits on its very first
// update survives -- Valve's own explosion_flashbang is three top-level sprites for exactly that
// reason. The MW2019 flash is one 0.1 s glow plus seven CHILD systems (smoke, sparks, debris,
// shrapnel); swapped in, the children never started and the flashbang drew nothing. The smoke
// child swapped in on its own showed one puff of its ten; a 30 ms child delay changed nothing.
//
// So while the category is on, the native particle is blocked (ParticleFxRules.cpp
// kVariantFlashbang -> particles/dev/empty.vpcf) and flashbang_detonate places the MW2019 effect
// through CS2's game particle manager (the create/set-control-point pair RagdollWeapon and native
// rain use). Nothing stops it, so it plays out in full and the manager retires it when it ends.
//
// PIECE BY PIECE (measured live on de_mirage, 2026-10-09): the manager creates a system and only
// then takes its position, while children copy the parent's control points when they are created
// -- so the parent landed at the detonation and all seven children at the world origin. Each
// piece is therefore created as its own top-level system at the point: a childless copy of the
// parent's glow (generic_explo_flash_core.vpcf) and the seven children it referenced.

#include "ParticleFxInternal.h"

#include "../Debug/MvmDebugLog.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

// Shared engine entry points resolved by MirvColors.
struct ParticleData;
extern unsigned char* (__fastcall *g_Original_getParticleManager)();
extern int* (__fastcall *g_Original_createParticle)(unsigned char*, int*, const char*, int,
	int64_t, int64_t, int64_t, int64_t);
extern void (__fastcall *g_Original_updateParticle)(unsigned char*, int, int, ParticleData*, int);

namespace Filmmaker {
namespace fx {

namespace {

struct Detonation {
	float position[3];
	int tick;                      // g_demoTickNow when the event arrived
	unsigned long long wallMs;
};

std::mutex g_detonationMx;
std::vector<Detonation> g_detonations; // queued for the pump
std::vector<Detonation> g_recent;      // accepted ones, kept for the duplicate check

// FireEvent and FireEventClientSide both deliver a demo's events, and a copy can also arrive a
// few ticks LATER (measured: tick 2576, then again at 2581/2591 when the demo was next paused).
// Same spot within this window, or same spot a few ticks after the first, is that one detonation.
// The same tick again later is a replay after a seek back, and plays.
constexpr unsigned long long kDuplicateMs = 250;
constexpr int kEchoTicks = 64;
// A demo seek replays every event it skips over within a frame or two. Those flashes are in the
// past: the world's tick has moved on by the time the pump runs, so they are not played.
constexpr int kStaleTicks = 8;
constexpr unsigned long long kStaleMs = 500;

} // namespace

int SpawnParticleAt(const char* name, const float* position) {
	if (!g_Original_getParticleManager || !g_Original_createParticle || !g_Original_updateParticle)
		return -1;
	int index = -1;
	__try {
		if (unsigned char* manager = g_Original_getParticleManager()) {
			g_Original_createParticle(manager, &index, name, 8, 0, 0, 0, 0);
			if (index >= 0)
				g_Original_updateParticle(manager, index, 0,
					reinterpret_cast<ParticleData*>(const_cast<float*>(position)), 0);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		index = -1;
	}
	return index;
}

namespace {

bool FlashbangModeOn() {
	std::lock_guard<std::mutex> lock(g_mx);
	return g_enabled && g_modes[kFxFlashbang] != FxMode::Off;
}

} // namespace

void FlashbangFx_OnDetonate(float x, float y, float z) {
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !FlashbangModeOn())
		return;
	const unsigned long long now = GetTickCount64();
	std::lock_guard<std::mutex> lock(g_detonationMx);
	const int tick = g_demoTickNow.load(std::memory_order_relaxed);
	for (const Detonation& d : g_recent) {
		const bool samePlace = std::fabs(d.position[0] - x) < 1.0f && std::fabs(d.position[1] - y) < 1.0f
			&& std::fabs(d.position[2] - z) < 1.0f;
		const bool echo = tick >= 0 && d.tick >= 0 && tick > d.tick && tick - d.tick <= kEchoTicks;
		if (samePlace && (now - d.wallMs <= kDuplicateMs || echo))
			return;
	}
	const Detonation accepted{ { x, y, z }, tick, now };
	if (g_recent.size() >= 16)
		g_recent.erase(g_recent.begin());
	g_recent.push_back(accepted);
	if (g_detonations.size() >= 16)
		g_detonations.erase(g_detonations.begin());
	g_detonations.push_back(accepted);
}

void FlashbangFx_PumpMainThread() {
	std::vector<Detonation> pending;
	{
		std::lock_guard<std::mutex> lock(g_detonationMx);
		if (g_detonations.empty())
			return;
		pending.swap(g_detonations);
	}
	if (!g_Original_getParticleManager || !g_Original_createParticle || !g_Original_updateParticle
		|| !FlashbangModeOn())
		return;
	const int tickNow = g_demoTickNow.load(std::memory_order_relaxed);
	const unsigned long long now = GetTickCount64();
	for (const Detonation& d : pending) {
		const bool stale = now - d.wallMs > kStaleMs
			|| (tickNow >= 0 && d.tick >= 0 && std::abs(tickNow - d.tick) > kStaleTicks);
		int created = 0;
		if (!stale)
			for (const char* part : kMw2019FlashbangParts)
				if (SpawnParticleAt(part, d.position) >= 0)
					++created;
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("fx.flashbang", "detonate at %.1f %.1f %.1f eventTick=%d tick=%d %s pieces=%d",
				d.position[0], d.position[1], d.position[2], d.tick, tickNow,
				stale ? "SKIPPED (seek replay)" : "created", created);
	}
}

} // namespace fx
} // namespace Filmmaker
