// ParticleFx core: engine binding + demo state, the apply-now reseek, the ParticleFx
// class' public methods, the main-thread pump, and the `fx` console command dispatch.
// The subsystem is split into focused translation units -- see the map in
// ParticleFxInternal.h (rules / hook / money / settings / diagnostics).

#include "ParticleFxInternal.h"

#include "FxAlign.h"                       // Modern muzzle-FX alignment probe (fx align)
#include "PerWeaponFx.h"                   // per-weapon overrides for shared flash systems
#include "../Filmmaker.h"                  // PlayingDemoPath (level-change detection)
#include "../MvmTest.h"                    // offline live-match FX testing (mvm_test)
#include "../Config/EffectsPersistence.h"  // EffectsPersistence_SceneActive: no effects at the main menu
#include "../Debug/MvmDebugLog.h" // MvmDebugLog_* (thread-safe flight recorder)
#include "../Platform/JsonBuilder.h"
#include "../Platform/TextEncoding.h"      // WideToUtf8 (FxLevelCacheKey)
#include "../../MirvTime.h"                // engine framecount (level-reset backstop)
#include "../../MirvColors.h"              // native smoke-grenade flight-particle pump
#include "../../../Shared/AfxConsole.h"    // advancedfx::Message/Warning + ICommandArgs

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

// Engine pointer (same one CameraPath/MovieMode use) for the toggle-time re-apply seek.
extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Filmmaker {
namespace fx {

// ============================== core state =========================================

// One mutex guards all mutable rule/telemetry state. Creation frequency is low (dozens per
// second at the wildest), so a plain mutex in the hook is fine; the engine call used for
// swap resolution is made OUTSIDE the lock.
std::mutex g_mx;
bool g_enabled = false;
FxMode g_modes[kFxCategoryCount] = {
	FxMode::Off, FxMode::Off, FxMode::Off,
	FxMode::Off, FxMode::Off, FxMode::Off, FxMode::Off
};
std::vector<CustomRule> g_customRules;
bool g_logging = false;
bool g_moneyHeadshot = false; // guarded by g_mx
std::atomic<bool> g_noFlashBlind{ false };
std::atomic<bool> g_mollyLights{ false };
std::atomic<bool> g_heLights{ false }, g_flashLights{ false };
// Published by PumpMainThread each frame: the hook runs on whatever thread creates
// particles, and polling the engine's demo interface there is not established as
// safe, so it reads this atomic instead.
std::atomic<int> g_demoTickNow{ -1 };
// Resource-context generation (see ParticleFxInternal.h and CRASH LESSON 7 in
// ParticleFxHook.cpp). Starts at 1 so a default-constructed cache entry (epoch 0) can
// never look current.
std::atomic<unsigned> g_levelEpoch{ 1 };

// ============================== apply-now reseek ===================================
// The hook only affects particle systems at CREATION time; systems already alive (and
// frozen on screen while the demo is paused) are untouched by a toggle, which reads as
// "the setting does nothing" / "it stays loaded after I change it". The engine-native
// fix: a tiny backward demo seek destroys every live particle (full update) and replays
// the recent event stream, re-creating the moment's effects UNDER THE NEW RULES -- More,
// On, Less, and Off/default swap/pass-through choices visibly replace each other. Any
// settings change requests this (debounced 400ms so a burst of UI clicks costs one seek);
// the pump performs it only while the demo is PAUSED --
// during playback new effects appear within seconds anyway and a seek would hitch.
// (`fx apply` forces one immediately regardless of pause.) Note surface DECALS (blood
// splatter on walls) are not particles and are unaffected.
std::atomic<unsigned long long> g_applyAtMs{ 0 };

// Camera-detached FP-FX suppression (see ParticleFxInternal.h). Driven from
// Filmmaker::RunMainThreadFrame whenever the free cam / third-person orbit camera
// takes or releases the view.
std::atomic<bool> g_fpFxSuppress{ false };
std::atomic<bool> g_bloodSuppress{ false };
std::atomic<unsigned long long> g_bloodBlocked{ 0 };

bool DemoIsPlaying() {
	if (!g_pEngineToClient) return false;
	if (auto pDemo = g_pEngineToClient->GetDemoFile()) return pDemo->IsPlayingDemo();
	return false;
}
bool DemoIsPaused() {
	if (!g_pEngineToClient) return false;
	if (auto pDemo = g_pEngineToClient->GetDemoFile()) return pDemo->IsDemoPaused();
	return false;
}
int DemoTick() {
	if (!g_pEngineToClient) return -1;
	if (auto pDemo = g_pEngineToClient->GetDemoFile()) return pDemo->GetDemoTick();
	return -1;
}

// Demo playback OR mvm_test on a loaded live map: main-thread precache/warmup is safe. Never the
// main menu or a UI map: effects are not switched on, installed or applied there.
bool FxMainThreadContextActive() {
	return EffectsPersistence_SceneActive();
}

// FxMainThreadContextActive() as of this main-thread frame, for the create hook (it runs on
// whatever thread creates particles, where the engine's demo state must not be read).
std::atomic<bool> g_sceneActive{ false };

// The cache key must change whenever the engine's PARTICLE RESOURCE CONTEXT changes, not
// merely when the user opens a different demo. A demo can reload its own level mid-playback
// (the round-boundary jump-back in mvm_debug_20260802_143443 did exactly that), which frees
// every particle definition while the demo path stays identical -- so the epoch is part of
// the key. See CRASH LESSON 7 in ParticleFxHook.cpp.
std::string FxLevelCacheKey() {
	std::string ctx;
	if (DemoIsPlaying()) {
		ctx = WideToUtf8(PlayingDemoPath());
	} else if (MvmTest_Active() && g_pEngineToClient) {
		const char* lvl = g_pEngineToClient->GetLevelNameShort();
		if (lvl && lvl[0])
			ctx = std::string("live:") + lvl;
	}
	if (ctx.empty())
		return ctx; // main menu: no context, and the pump never resolves there anyway
	char suffix[24];
	std::snprintf(suffix, sizeof(suffix), "#%u", g_levelEpoch.load(std::memory_order_acquire));
	return ctx + suffix;
}

// Last engine framecount the pump observed, for the level-reset backstop. Namespace-level
// (not a pump-local static) so ParticleFx_OnLevelInit can re-baseline it: the reset to 0
// that follows a normal level init is EXPECTED and must not bump the epoch a second time.
std::atomic<int> g_lastFrameCount{ 0 };

// Drop every cached handle because the engine is about to free (or has just freed) the
// resources they point at. Main thread; must not be called with g_mx held.
void BumpLevelEpoch(const char* reason) {
	const unsigned epoch = g_levelEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
	size_t dropped = 0;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		dropped = g_handleCache.size();
		g_handleCache.clear();
		g_resolveQueue.clear();
		RebuildActiveSwapTargetsLocked(false);
	}
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("fx.level", "%s -> epoch=%u, %zu cached handle(s) dropped", reason, epoch, dropped);
}

void ApplyReseekNow() {
	const int tick = DemoTick();
	if (tick <= 1 || !g_pEngineToClient)
		return;
	// One tick back: guarantees a real backward seek (goal == current can no-op), and
	// ~15ms of position change is imperceptible.
	char cmd[64];
	std::snprintf(cmd, sizeof(cmd), "demo_gototick %d", tick - 1);
	g_pEngineToClient->ExecuteClientCmd(0, cmd, true);
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("fx.apply", "reseek to %d (paused=%d) to re-create live effects", tick - 1,
			DemoIsPaused() ? 1 : 0);
}

void RequestApplyReseek() {
	{
		std::lock_guard<std::mutex> lock(g_mx);
		if (!g_enabled) {
			g_applyAtMs.store(0);
			return;
		}
	}
	if (!DemoIsPlaying())
		return;
	g_applyAtMs.store(GetTickCount64() + 400);
}

constexpr unsigned long long kPausedResolveIntervalMs = 500;
constexpr unsigned long long kPlaybackResolveIntervalMs = 1000;
constexpr unsigned long long kHeavyResolveMs = 15;
constexpr unsigned long long kPausedHeavyResolveBackoffMs = 250;
constexpr unsigned long long kPlaybackHeavyResolveBackoffMs = 2000;
constexpr unsigned long long kResolveAfterLevelSettleMs = 3000;
unsigned long long g_nextResolveAtMs = 0; // main-thread only

std::string PopResolveTargetLocked() {
	if (g_resolveQueue.empty())
		return std::string();
	std::string name = g_resolveQueue.front();
	g_resolveQueue.erase(g_resolveQueue.begin());
	return name;
}

void DrainResolveQueueMainThread() {
	if (!FxMainThreadContextActive())
		return;
	const bool paused = DemoIsPaused();
	const unsigned long long now = GetTickCount64();
	if (now < g_nextResolveAtMs)
		return;

	std::string name;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		name = PopResolveTargetLocked();
	}
	if (name.empty())
		return;

	const unsigned long long beforeMs = GetTickCount64();
	ResolveHandleOnMainThread(name.c_str());
	const unsigned long long afterMs = GetTickCount64();
	const unsigned long long costMs = afterMs - beforeMs;
	if (paused) {
		g_nextResolveAtMs = afterMs
			+ (costMs >= kHeavyResolveMs ? kPausedHeavyResolveBackoffMs : kPausedResolveIntervalMs);
	} else {
		g_nextResolveAtMs = afterMs
			+ (costMs >= kHeavyResolveMs ? kPlaybackHeavyResolveBackoffMs : kPlaybackResolveIntervalMs);
	}
}

// ============================== warm-up burst ======================================
// Swap targets are PRECACHED up front, at demo/level entry and on a settings change, not
// lazily on first use (repo precache rule: a swap that hitches in-game is a precache bug).
//
// The demand-only trickle this replaces resolved one target per ~2s of playback, and every
// one of those was an engine "was not precached correctly. Performing blocking load" stall
// landing mid-action (mvm_debug_20260802_130814: 24 of them spread across one match). The
// total load cost is the same either way -- what changes is WHEN it is paid.
//
// An earlier burst attempt was disabled for stalling ~78 ms in a single paused frame
// (mvm_debug_20260726_041859). Two things fix that rather than the burst being wrong:
//   - PrefetchFxPackOnce() warms the OS file cache off-thread first, so each blocking load
//     hits cached pages. It exists for exactly this and had been left unreferenced.
//   - kBurstBudgetOpenMs is a per-FRAME budget; at 100 ms it WAS the stall it was blamed
//     for. It is now sized to spread the same work over more frames.
// A blocking load cannot be made free on the main thread; it can only be moved to where it
// is not perceived (demo entry / paused) and made cheap (warm file cache).

enum class WarmupPhase { Idle, WaitSettle, Bursting };
WarmupPhase g_warmupPhase = WarmupPhase::Idle; // main-thread only (like all state below)
unsigned long long g_warmupArmedAtMs = 0;
int g_warmupTickAtArm = -1;
int g_warmupTickAdvanceSeen = 0;
unsigned long long g_warmupOpenWindowUntilMs = 0; // full-budget window after demo open
unsigned g_warmupResolved = 0;
unsigned long long g_warmupTotalMs = 0, g_warmupMaxMs = 0;
std::string g_warmupMaxName;
unsigned g_warmupFrames = 0;

// Per-FRAME time budgets. One resolve can overrun either (the budget is checked after the
// call, and a cold blocking load is 15-40 ms), so these bound the STEADY rate, not the
// worst single frame -- keep them near one frame's worth of work.
constexpr unsigned long long kBurstBudgetOpenMs = 30;   // paused, or just-opened demo
constexpr unsigned long long kBurstBudgetPlayingMs = 20; // settings switch mid-playback
constexpr unsigned long long kWarmupOpenWindowMs = 10000;
constexpr unsigned long long kWarmupMaxDurationMs = 20000; // safety bail to the trickle

void FinishWarmup(const char* why) {
	g_warmupPhase = WarmupPhase::Idle;
	g_nextResolveAtMs = 0; // let the trickle pick up strays immediately
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("fx.resolve", "warmup done (%s): %u resolved, %llums total, max %llums (%s), %u frame(s)",
			why, g_warmupResolved, g_warmupTotalMs, g_warmupMaxMs,
			g_warmupMaxName.empty() ? "-" : g_warmupMaxName.c_str(), g_warmupFrames);
	// A paused frame the user is staring at should recompose under the now-resolved
	// swaps (reuses the debounced reseek; it self-skips if the user unpauses).
	if (g_warmupResolved && DemoIsPlaying() && DemoIsPaused())
		RequestApplyReseek();
}

// Earliest provably-safe burst moment after a level change: the demo tick advancing on
// two consecutive pumps shows the new level is simulating, i.e. resolves land in the
// NEW resource context (the CRASH LESSON 4 hazard is stale-context handles). The old
// 3s settle delay remains as the wall-clock fallback ceiling.
bool WarmupSettleReachedMainThread() {
	if (!FxMainThreadContextActive())
		return false;
	if (GetTickCount64() - g_warmupArmedAtMs >= kResolveAfterLevelSettleMs)
		return true;
	if (!DemoIsPlaying())
		return false;
	if (DemoTick() > g_warmupTickAtArm) {
		if (++g_warmupTickAdvanceSeen >= 2)
			return true;
	} else {
		g_warmupTickAdvanceSeen = 0;
	}
	return false;
}

void BurstDrainMainThread() {
	if (!FxMainThreadContextActive())
		return; // never resolve at the main menu (same invariant as the trickle)
	const unsigned long long start = GetTickCount64();
	if (start - g_warmupArmedAtMs > kWarmupMaxDurationMs) {
		FinishWarmup("max duration");
		return;
	}
	const unsigned long long budget = (DemoIsPaused() || start < g_warmupOpenWindowUntilMs)
		? kBurstBudgetOpenMs : kBurstBudgetPlayingMs;
	for (;;) {
		std::string name;
		{
			std::lock_guard<std::mutex> lock(g_mx);
			name = PopResolveTargetLocked();
		}
		if (name.empty()) {
			++g_warmupFrames;
			FinishWarmup("queue empty");
			return;
		}
		const unsigned long long t0 = GetTickCount64();
		ResolveHandleOnMainThread(name.c_str());
		const unsigned long long costMs = GetTickCount64() - t0;
		++g_warmupResolved;
		g_warmupTotalMs += costMs;
		if (costMs > g_warmupMaxMs) {
			g_warmupMaxMs = costMs;
			g_warmupMaxName = name;
		}
		if (GetTickCount64() - start >= budget)
			break; // yield the frame; continue next pump
	}
	++g_warmupFrames;
}

// Main thread; caller must NOT hold g_mx. afterLevelChange waits for level settle
// before bursting; a settings switch bursts on the next pump.
void ArmWarmup(bool afterLevelChange) {
	// Off-thread, once per process, no engine calls: makes the blocking loads below cheap.
	PrefetchFxPackOnce();
	size_t queued = 0;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		QueueAllUncachedActiveTargetsLocked();
		queued = g_resolveQueue.size();
	}
	if (!queued) {
		g_warmupPhase = WarmupPhase::Idle;
		g_warmupOpenWindowUntilMs = 0;
		return; // everything already hot in this resource context
	}
	// A level change must not resolve until the new context is actually live (the settle
	// gate); a settings switch is already in a live context and bursts on the next pump.
	g_warmupPhase = afterLevelChange ? WarmupPhase::WaitSettle : WarmupPhase::Bursting;
	g_warmupArmedAtMs = GetTickCount64();
	g_warmupTickAtArm = DemoTick();
	g_warmupTickAdvanceSeen = 0;
	g_warmupOpenWindowUntilMs = 0; // WaitSettle->Bursting opens the full-budget window
	g_warmupResolved = 0;
	g_warmupTotalMs = 0;
	g_warmupMaxMs = 0;
	g_warmupMaxName.clear();
	g_warmupFrames = 0;
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("fx.resolve", "warmup armed (levelChange=%d): %zu target(s) to precache",
			afterLevelChange ? 1 : 0, queued);
}

} // namespace fx

// ============================== public API =========================================

using namespace fx;

ParticleFx& ParticleFxRef() {
	static ParticleFx s_instance;
	return s_instance;
}

const char* FxModeName(FxMode mode) { return kModeNames[(int)mode < kModeCount ? (int)mode : 0]; }
const char* FxCategoryKey(FxCategory cat) {
	return (cat >= 0 && cat < kFxCategoryCount) ? kCategoryKeys[cat] : "?";
}
bool FxModeSupported(FxCategory cat, FxMode mode) {
	return cat >= 0 && cat < kFxCategoryCount && ModeSupported(cat, mode);
}

bool ParticleFx::EnsureInstalled() {
	// The particle manager is only detoured once a demo or live test map is up. At the main menu
	// the saved settings stay stored and the hook arms from PumpMainThread when a scene loads.
	if (!FxMainThreadContextActive())
		return g_installed.load();
	return TryInstall();
}
bool ParticleFx::Installed() const { return g_installed.load(); }

void ParticleFx_OnLevelInit() {
	MollyFx_OnLevelInit();
	// Main thread, called from the engine's LevelInitPreEntity detour -- i.e. BEFORE the new
	// level creates its first entity or particle, which is the only ordering that makes the
	// invalidation airtight. Takes g_mx and touches plain containers only: no engine calls,
	// so it is safe this early in level setup.
	if (!g_installed.load())
		return;
	BumpLevelEpoch("level init");
	// The engine is about to reset its frame counter as part of this very load. Re-baseline
	// so the pump's backstop does not read that expected reset as a SECOND, unhandled reload
	// and pay for a redundant re-warm. A reload that never reaches here leaves the baseline
	// high, which is exactly when the backstop should fire.
	g_lastFrameCount.store(0, std::memory_order_release);
}

void ParticleFx_SetFirstPersonFxSuppressed(bool on) {
	if (g_fpFxSuppress.exchange(on, std::memory_order_relaxed) == on)
		return;
	if (on)
		ParticleFxRef().EnsureInstalled(); // the gate lives in the create hook
	// Keep dev/empty resolvable even when every EFFECTS category is Off.
	RebuildActiveSwapTargets(false);
}

bool ParticleFx_FirstPersonFxSuppressed() {
	return g_fpFxSuppress.load(std::memory_order_relaxed);
}

void ParticleFx_SetBloodSuppressed(bool on) {
	if (g_bloodSuppress.exchange(on, std::memory_order_relaxed) == on)
		return;
	if (on)
		ParticleFxRef().EnsureInstalled(); // the gate lives in the create hook
	RebuildActiveSwapTargets(false);   // keep dev/empty resolvable
}

unsigned long long ParticleFx_BloodBlocked() {
	return g_bloodBlocked.load(std::memory_order_relaxed);
}

static std::atomic<unsigned long long> g_bloodDecalsSkipped{ 0 };
bool ParticleFx_BloodSuppressed() { return g_bloodSuppress.load(std::memory_order_relaxed); }
void ParticleFx_NoteBloodDecalSkipped() { g_bloodDecalsSkipped.fetch_add(1, std::memory_order_relaxed); }
unsigned long long ParticleFx_BloodDecalsSkipped() { return g_bloodDecalsSkipped.load(std::memory_order_relaxed); }

bool ParticleFx_NadeTrailsEnabled() {
	std::lock_guard<std::mutex> lock(g_mx);
	return g_enabled && g_modes[kFxMolotov] != FxMode::Off;
}

// ---- smoke-trail particle bookkeeping (see ParticleFx.h) --------------------------------
namespace {
constexpr int kMaxTrackedSmokeTrails = 16;
struct TrackedSmokeTrail {
	CEntityInstance* ent = nullptr;
	int particleIndex = -1;
};
std::mutex g_smokeTrailMx;
TrackedSmokeTrail g_smokeTrails[kMaxTrackedSmokeTrails];
} // namespace

int ParticleFx_SmokeTrailLookup(CEntityInstance* ent) {
	if (!ent) return -1;
	std::lock_guard<std::mutex> lock(g_smokeTrailMx);
	for (auto& t : g_smokeTrails)
		if (t.ent == ent) return t.particleIndex;
	return -1;
}

void ParticleFx_SmokeTrailTrack(CEntityInstance* ent, int particleIndex) {
	if (!ent) return;
	std::lock_guard<std::mutex> lock(g_smokeTrailMx);
	for (auto& t : g_smokeTrails) {
		if (t.ent == ent) { t.particleIndex = particleIndex; return; }
	}
	for (auto& t : g_smokeTrails) {
		if (!t.ent) { t = { ent, particleIndex }; return; }
	}
	// Table full (>16 smoke grenades airborne at once never happens in practice): overwrite
	// the oldest slot rather than silently dropping tracking for this entity.
	g_smokeTrails[0] = { ent, particleIndex };
}

void ParticleFx_SmokeTrailForget(CEntityInstance* ent) {
	if (!ent) return;
	std::lock_guard<std::mutex> lock(g_smokeTrailMx);
	for (auto& t : g_smokeTrails)
		if (t.ent == ent) { t.ent = nullptr; t.particleIndex = -1; }
}

bool ParticleFx::WantsHook() const {
	std::lock_guard<std::mutex> lock(g_mx);
	if (g_logging)
		return true;
	return HasActiveFxLocked();
}

bool ParticleFx::MollyLights() const { return g_mollyLights.load(); }
bool ParticleFx::MollyLightsReady() const { return MollyFx_Ready(); }
void ParticleFx::SetMollyLights(bool on) {
    {
        std::lock_guard<std::mutex> lock(g_mx);
        g_mollyLights.store(on);if(on)g_enabled=true;
    }
    if(on){EnsureInstalled();if(DemoIsPaused())RequestApplyReseek();}
    SaveSettings();
}
bool ParticleFx::GrenadeLights(bool flash) const { return (flash?g_flashLights:g_heLights).load(); }
void ParticleFx::SetGrenadeLights(bool flash,bool on) {
    {std::lock_guard<std::mutex> lock(g_mx);(flash?g_flashLights:g_heLights).store(on);if(on)g_enabled=true;}
    if(on){EnsureInstalled();if(DemoIsPaused())RequestApplyReseek();}
    SaveSettings();
}
bool ParticleFx::FlashBlind() const { return !g_noFlashBlind.load(); }

void ParticleFx::SetFlashBlind(bool on) {
	g_noFlashBlind.store(!on);
	SaveSettings();
}

bool ParticleFx::MoneyHeadshot() const {
	std::lock_guard<std::mutex> lock(g_mx);
	return g_moneyHeadshot;
}

void ParticleFx::SetMoneyHeadshot(bool on) {
	bool wantsHook = false;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		const bool changed = g_moneyHeadshot != on;
		const bool enablingMaster = on && !g_enabled;
		g_moneyHeadshot = on;
		if (on)
			g_enabled = true;
		RebuildActiveSwapTargetsLocked(changed || enablingMaster);
		wantsHook = HasActiveFxLocked();
	}
	if (wantsHook)
		EnsureInstalled();
	SaveSettings();
	RequestApplyReseek();
	if (FxMainThreadContextActive())
		ArmWarmup(false);
}

void ParticleFx::PumpMainThread() {
	g_sceneActive.store(FxMainThreadContextActive(), std::memory_order_relaxed);
	// Spray-heat clock for the create hook (safe engine access is main-thread/pump
	// only; the hook reads the atomic). Live mvm_test uses a synthetic tick.
	int tickNow = -1;
	if (DemoIsPlaying()) {
		tickNow = DemoTick();
	} else if (MvmTest_Active() && MvmTest_LevelLoaded()) {
		tickNow = (int)(GetTickCount64() / 15);
	}
	g_demoTickNow.store(tickNow, std::memory_order_relaxed);
	// Smoke flight trails use C_MolotovProjectile's real entity-owned particle dispatch.
	// Keep this on the normal FrameStageNotify pump; it does not depend on spectator/X-ray.
	MirvColors_PumpSmokeGrenadeTrails();
	// The spectated-pawn -> userid lookup intermittently misses for single frames during
	// playback (observed live 2026-07-04: it flickered to -1 BETWEEN weapon_fire events,
	// so the FxDebugHud/FxAlign fire-window never armed). Hold the last good value for a
	// short decay so one bad frame doesn't drop the watched player.
	{
		static int s_lastGoodUid = -1;                 // main-thread only
		static unsigned long long s_lastGoodMs = 0;
		int uid = DemoIsPlaying() ? UserIdForSpectatedPawn() : -1;
		const unsigned long long nowMs = GetTickCount64();
		if (uid >= 0) {
			s_lastGoodUid = uid;
			s_lastGoodMs = nowMs;
		} else if (DemoIsPlaying() && s_lastGoodUid >= 0 && nowMs - s_lastGoodMs < 2000) {
			uid = s_lastGoodUid;
		}
		g_watchedUserId.store(uid, std::memory_order_relaxed);
	}
	// Alignment probe: drain queued swap creations (entity + attachment resolution is
	// main-thread work). No-op while "fx align" is off.
	FxAlign_PumpMainThread();
	// Flash grenade On: place the MW2019 flash at each queued detonation.
	FlashbangFx_PumpMainThread();
	MollyFx_PumpMainThread();
	// Cache the firing weapon for PerWeaponFx_Override. Must be main-thread: the particle
	// hook resolves swaps on whatever thread creates particles, where entity access is unsafe.
	PerWeaponFx_SampleMainThread();
	if (g_installed.load()) {
		// Backstop for CRASH LESSON 7: the engine's global frame counter only ever goes
		// BACKWARDS when the level is torn down and rebuilt. ParticleFx_OnLevelInit is the
		// primary signal (it fires before the new level's first frame); this catches any
		// reload that does not reach it -- e.g. a CS2 update moving the LevelInitPreEntity
		// vtable slot, which would otherwise silently reinstate the crash.
		{
			const int frameCount = g_MirvTime.framecount_get();
			if (frameCount < g_lastFrameCount.exchange(frameCount, std::memory_order_acq_rel))
				BumpLevelEpoch("engine framecount reset");
		}
		// CRASH LESSON 4 (2026-07-02, mvm_debug_091656 caught it): handles resolved at
		// the MAIN MENU dangle once a demo loads (map transition purges resources); the
		// first swap then hands the engine freed memory and its child-walk dies at
		// particles.dll+0x3E3xx. So the cache is tied to the level: any demo/level
		// change drops every cached handle and re-resolves in the NEW resource context.
		static std::string s_lastLevelKey; // main-thread only (demo path or live:map)
		const std::string cur = FxLevelCacheKey();
		if (cur != s_lastLevelKey) {
			s_lastLevelKey = cur;
			{
				std::lock_guard<std::mutex> lock(g_mx);
				g_handleCache.clear();
				g_resolveQueue.clear();
				RebuildActiveSwapTargetsLocked(false);
				g_nextResolveAtMs = GetTickCount64() + kResolveAfterLevelSettleMs;
				if (MvmDebugLog_Active())
					MvmDebugLog_Linef("fx.resolve", "level change -> handle cache cleared, %zu target(s) requeued",
						g_resolveQueue.size());
			}
			// Front-loaded precache: queue the FULL active set and burst it once the new
			// level settles, so playback starts with every swap target hot.
			ArmWarmup(true);
		}
		// Drain swap-target resolves here, in the engine's own precache context. During a
		// warm-up the queue is burst-drained under a per-frame time budget; otherwise stray
		// lazy misses get the slow demand-only trickle. The main menu never drains either way.
		switch (g_warmupPhase) {
		case WarmupPhase::WaitSettle:
			if (WarmupSettleReachedMainThread()) {
				g_warmupPhase = WarmupPhase::Bursting;
				g_warmupOpenWindowUntilMs = GetTickCount64() + kWarmupOpenWindowMs;
				if (MvmDebugLog_Active()) {
					size_t queued = 0;
					{
						std::lock_guard<std::mutex> lock(g_mx);
						queued = g_resolveQueue.size();
					}
					MvmDebugLog_Linef("fx.resolve", "warmup burst start: %zu queued, paused=%d",
						queued, DemoIsPaused() ? 1 : 0);
				}
				BurstDrainMainThread();
			}
			break;
		case WarmupPhase::Bursting:
			BurstDrainMainThread();
			break;
		default:
			DrainResolveQueueMainThread();
			break;
		}
		// Debounced apply-now: re-create the paused moment's live effects under the new
		// rules (see the g_applyAtMs comment). Skipped while playing -- self-corrects.
		const unsigned long long applyAt = g_applyAtMs.load();
		if (applyAt && GetTickCount64() >= applyAt) {
			g_applyAtMs.store(0);
			if (DemoIsPlaying() && DemoIsPaused())
				ApplyReseekNow();
		}
		return;
	}
	if (g_installFailedHard)
		return;
	if (!WantsHook() || !g_sceneActive.load(std::memory_order_relaxed))
		return;
	const unsigned long long now = GetTickCount64();
	if (now - g_lastInstallTryMs < 3000)
		return;
	g_lastInstallTryMs = now;
	TryInstall();
}

bool ParticleFx::Enabled() const {
	std::lock_guard<std::mutex> lock(g_mx);
	return g_enabled;
}

void ParticleFx::SetEnabled(bool on) {
	bool wantsHook = false;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		const bool changed = g_enabled != on;
		g_enabled = on;
		RebuildActiveSwapTargetsLocked(changed && on);
		wantsHook = HasActiveFxLocked();
	}
	if (!on)
		g_applyAtMs.store(0);
	if (wantsHook)
		EnsureInstalled();
	SaveSettings();
	if (on) {
		RequestApplyReseek();
		if (FxMainThreadContextActive())
			ArmWarmup(false);
	}
}

FxMode ParticleFx::Mode(FxCategory cat) const {
	if (cat < 0 || cat >= kFxCategoryCount)
		return FxMode::Off;
	std::lock_guard<std::mutex> lock(g_mx);
	return g_modes[cat];
}

void ParticleFx::SetMode(FxCategory cat, FxMode mode) {
	if (cat < 0 || cat >= kFxCategoryCount)
		return;
	mode = NormalizeMode(cat, mode); // More -> On; Less/Modern only where real variants exist
	bool wantsHook = false;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		const bool changed = g_modes[cat] != mode;
		const bool enablingMaster = mode != FxMode::Off && !g_enabled;
		g_modes[cat] = mode;
		if (mode != FxMode::Off)
			g_enabled = true;
		RebuildActiveSwapTargetsLocked(changed || enablingMaster);
		wantsHook = HasActiveFxLocked();
	}
	if (wantsHook)
		EnsureInstalled();
	SaveSettings();
	RequestApplyReseek();
	if (FxMainThreadContextActive())
		ArmWarmup(false);
}

void ParticleFx::SetLogging(bool on) {
	{
		std::lock_guard<std::mutex> lock(g_mx);
		g_logging = on;
	}
	if (on)
		EnsureInstalled();
}

bool ParticleFx::Logging() const {
	std::lock_guard<std::mutex> lock(g_mx);
	return g_logging;
}

std::string ParticleFx::DebugStateJson() const {
	JsonBuilder b;
	b.BeginObject();
	b.BoolField("installed", g_installed.load());
	b.BoolField("installFailedHard", g_installFailedHard);
	b.BoolField("align", FxAlign_Enabled());
	b.BoolField("jitRedirect", g_jitRedirectInstalled);
	// 0 = Idle (trickle), 1 = WaitSettle, 2 = Bursting. Main-thread state read here for
	// diagnostics only; the console command runs on the engine's main thread.
	b.IntField("warmupPhase", (int64_t)g_warmupPhase);
	b.IntField("seen", (int64_t)g_totalSeen.load());
	b.IntField("noName", (int64_t)g_totalNoName.load());
	b.IntField("blocked", (int64_t)g_totalBlocked.load());
	b.IntField("swapped", (int64_t)g_totalSwapped.load());
	{
		std::lock_guard<std::mutex> lock(g_mx);
		b.BoolField("enabled", g_enabled);
		b.BoolField("logging", g_logging);
		b.BoolField("moneyHeadshot", g_moneyHeadshot);
		b.BoolField("flashBlind", !g_noFlashBlind.load());
		b.BoolField("mollyLights",g_mollyLights.load());
		b.BoolField("heLights",g_heLights.load());
		b.BoolField("flashLights",g_flashLights.load());
		b.BoolField("mollyLightsReady",MollyFx_Ready());
		b.Key("modes");
		b.BeginObject();
		for (int i = 0; i < kFxCategoryCount; ++i)
			b.StringField(kCategoryKeys[i], kModeNames[(int)g_modes[i]]);
		b.EndObject();
		b.IntField("customRules", (int64_t)g_customRules.size());
		b.IntField("namesTracked", (int64_t)g_nameStats.size());
		b.IntField("handleCache", (int64_t)g_handleCache.size());
		b.IntField("resolveQueue", (int64_t)g_resolveQueue.size());
	}
	b.EndObject();
	return b.Str();
}

// ============================== console command ====================================

namespace {

void PrintFxHelp(const char* cmd) {
	advancedfx::Message(
		"%s fx set <category> <on|less|modern|off> - control one effect category (non-Off enables the master). Categories:\n"
		"    impacts    - randomized reduced Povarhok + Insurgency impacts (on|off)\n"
		"    tracers    - bullet tracers (on|modern|off)\n"
		"    weaponfx   - muzzle flash, muzzle/shell smoke, brass (on|modern|off)\n"
		"    blood      - blood impacts (on|off)\n"
		"    explosions - HE grenade / generic explosions (on|modern|off)\n"
		"    bombfx     - the planted bomb blast (on|off)\n"
		"    flashbang  - flash grenade: on = the MW2019 flashbang (flash, smoke, sparks, debris) (on|off)\n"
		"    molotov    - NADE TRAILS: in-flight smoke trail on thrown grenades (on|off).\n"
		"                 Molotov/incendiary get the full trail (ribbon + drips + glow);\n"
		"                 Smoke grenades get the smoke ribbon alone. Molotov GROUND\n"
		"                 fire always plays vanilla CS2 (Insurgency has no pool effect).\n"
		"    mapfx      - map ambience particles (on|off)\n"
		"  (impact on = reduced Povarhok + Insurgency hybrid; weaponfx on = Insurgency 2\n"
		"   flash plus layered old On-mode weapon smoke; other on = Insurgency 2,\n"
		"   modern = converted MW2019 ARC9 pack, off = default CS2 pass-through; 'more'\n"
		"   is accepted as a legacy alias of on. Unsupported modes snap to on.\n"
		"   If the asset pack is not mounted, the original CS2 effect plays. Smoke grenades keep\n"
		"   their volumetric cloud; Nade Trails adds only a separate flight ribbon.)\n"
		"%s fx on|off - master switch over all category modes + rules (Off cancels pending precache).\n"
		"%s fx state - status + counters.\n"
		"%s fx log on|off - capture every particle creation (view with 'fx recent').\n"
		"%s fx recent [n] - print the last n captured/acted creations (default 30).\n"
		"%s fx names [filter] - aggregated creation counts seen so far.\n"
		"%s fx block <substr> | unblock <substr> - custom block rule (name substring).\n"
		"%s fx swap <substr> <target.vpcf> | unswap <substr> - custom swap rule.\n"
		"%s fx rules - list custom rules.  %s fx test <name> - dry-run classify a name.\n"
		"%s fx apply - re-create the current moment's live effects under the current\n"
		"    settings (tiny backward seek; toggles do this automatically while paused).\n"
		"%s fx mollylights on|off - native CS2 lights for held/thrown Molotovs and ground fire.\n"
        "    fx helights on|off / fx flashlights on|off - native detonation light bursts.\n"
		"%s fx moneyshot on|off - money burst replacing the headshot blood/helmet-spark particles (they only spawn on real headshot hits).\n"
		"%s fx flashblind on|off - off: flashbangs still detonate (sound, particles) but never blind anyone.\n"
		"%s fx debughud on|off - on-screen squares that blink per shot as each effect-class\n"
		"    swap fires (1-0-1-0 per bullet): cyan = Modern, magenta = On/Insurgency.\n"
		"%s fx align ... - Modern muzzle-FX alignment probe: measures muzzle-to-spawn distance\n"
		"    in Source units per weapon class ('%s fx align' for its own help).\n",
		cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
}

// Dry-run of the hook's decision logic for one name (no engine calls).
void FxTestName(const char* name) {
	char low[260];
	LowerCopy(name, low, sizeof(low));
	const int cat = ClassifyLower(low);
	char action = '=';
	std::string target;
	std::string why = "untracked category";
	{
		std::lock_guard<std::mutex> lock(g_mx);
		if (!g_enabled) {
			why = "master OFF";
		} else {
			for (const CustomRule& r : g_customRules) {
				if (std::strstr(low, r.match.c_str())) {
					action = r.target.empty() ? 'X' : '>';
					target = r.target;
					why = std::string("custom rule '") + r.match + "'";
					break;
				}
			}
			if (action == '=' && cat >= 0) {
				const FxMode m = g_modes[cat];
				why = std::string("category ") + kCategoryKeys[cat] + " = " + kModeNames[(int)m];
				if (m == FxMode::Off) {
					why += " (default CS2 pass-through)";
				} else {
					if (const char* t = VariantTargetLower(cat, m, low)) {
						action = '>';
						target = t;
						why += " (converted variant table match)";
						// Weapons sharing one CS2 particle name can still need different
						// assets (SCAR-20 vs G3SG1). Applied AFTER the table, so an override
						// only ever redirects a target the table already chose.
						if (const char* pw = PerWeaponFx_Override(t)) {
							target = pw;
							why += " + per-weapon override";
						}
					} else {
						why += " (no converted variant table entry, passes through)";
					}
				}
			}
		}
	}
	if (action == 'X')
		target = kEmptySystem;
	advancedfx::Message("fx test: %s\n  category: %s\n  decision: %s%s%s\n  reason: %s\n",
		name, cat >= 0 ? kCategoryKeys[cat] : "(other)",
		action == '=' ? "PASS" : (action == 'X' ? "BLOCK -> " : "SWAP -> "),
		action == '=' ? "" : target.c_str(), "", why.c_str());
}

} // namespace

void ParticleFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	ParticleFx& fx = ParticleFxRef();
	const char* action = (argc >= 3) ? args->ArgV(2) : "";

	if (0 == _stricmp(action, "state")) {
		advancedfx::Message("[fx][state] %s\n", fx.DebugStateJson().c_str());
		return;
	}
	if (0 == _stricmp(action, "install")) {
		const bool ok = fx.EnsureInstalled();
		advancedfx::Message("fx: hook %s.\n", ok ? "installed" : "NOT installed (see warnings)");
		return;
	}
	if (0 == _stricmp(action, "on") || 0 == _stricmp(action, "off")) {
		const bool turningOn = (0 == _stricmp(action, "on"));
		if (turningOn && !EffectsPersistence_AllowEnable("Particle effects"))
			return;
		fx.SetEnabled(turningOn);
		// The DEBUG SQUARES ARE NOT TURNED ON HERE. This used to force
		// g_debugHudEnabled = true on every `fx on`, so the developer overlay came back every
		// time the particle pack was switched on -- including from the VFX card's own switch in
		// CONFIG, which sends exactly this command. Switching the squares off did not stick,
		// because the next toggle of the pack re-armed them; from the outside that reads as an
		// overlay that turns itself on.
		//
		// It is a diagnostic, so it stays off until it is asked for: `fx debughud on`. Anyone
		// who wants both still gets both, in the order they typed them.
		if (!(argc >= 4 && 0 == _stricmp(args->ArgV(3), "quiet")))
			advancedfx::Message("fx: effects control %s.%s\n", fx.Enabled() ? "ON" : "off",
				turningOn ? " (fx debughud on for the per-shot debug squares)" : "");
		return;
	}
	if (0 == _stricmp(action, "set")) {
		if (argc < 5) { advancedfx::Warning("usage: %s fx set <category> <on|less|modern|off>\n", cmd); return; }
		const int cat = CategoryFromKey(args->ArgV(3));
		const int mode = ModeFromName(args->ArgV(4));
		if (cat < 0) { advancedfx::Warning("fx: unknown category '%s' (see '%s fx').\n", args->ArgV(3), cmd); return; }
		if (mode < 0) { advancedfx::Warning("fx: unknown mode '%s' (on|less|modern|off).\n", args->ArgV(4)); return; }
		if ((FxMode)mode != FxMode::Off && !EffectsPersistence_AllowEnable("Particle effects"))
			return;
		if (!ModeSupported(cat, (FxMode)mode) && (FxMode)mode != FxMode::More)
			advancedfx::Warning("fx: %s does not support '%s' (no distinct assets); using 'on'.\n",
				kCategoryKeys[cat], args->ArgV(4));
		fx.SetMode((FxCategory)cat, (FxMode)mode);
		// Report what was STORED, not what was asked for. SetMode normalizes (More -> On, and an
		// unsupported Less/Modern -> On), so echoing the requested name printed
		// "explosions = less" immediately under "explosions does not support 'less'; using 'on'"
		// -- two lines that contradict each other, with the wrong one last.
		const FxMode stored = fx.Mode((FxCategory)cat);
		if (!(argc >= 6 && 0 == _stricmp(args->ArgV(5), "quiet")))
			advancedfx::Message("fx: %s = %s.%s\n", kCategoryKeys[cat], kModeNames[(int)stored],
				fx.Installed() ? "" : " (hook not armed yet - arms once a demo/map has loaded)");
		return;
	}
	if (0 == _stricmp(action, "log")) {
		const char* v = (argc >= 4) ? args->ArgV(3) : "";
		if (0 == _stricmp(v, "on")) fx.SetLogging(true);
		else if (0 == _stricmp(v, "off")) fx.SetLogging(false);
		else { advancedfx::Message("fx: logging is %s (fx log on|off).\n", fx.Logging() ? "ON" : "off"); return; }
		advancedfx::Message("fx: logging %s.%s\n", fx.Logging() ? "ON" : "off",
			fx.Logging() ? " Play the demo, then 'fx recent' / 'fx names'." : "");
		return;
	}
	if (0 == _stricmp(action, "recent")) {
		const size_t n = (argc >= 4) ? (size_t)atoi(args->ArgV(3)) : 30;
		std::lock_guard<std::mutex> lock(g_mx);
		const size_t count = g_ring.size();
		const size_t want = n && n < count ? n : count;
		advancedfx::Message("fx: last %zu creation(s) (of %zu captured; = pass, X blocked, > swapped):\n", want, count);
		for (size_t i = count - want; i < count; ++i) {
			// ring order: g_ringNext is the OLDEST once full
			const FxEvent& e = g_ring[(g_ringNext + i) % (count < kRingCap ? count : kRingCap)];
			if (e.action == '=')
				advancedfx::Message("  = %s\n", e.name.c_str());
			else
				advancedfx::Message("  %c %s -> %s\n", e.action, e.name.c_str(), e.target.c_str());
		}
		return;
	}
	if (0 == _stricmp(action, "names")) {
		const char* filter = (argc >= 4) ? args->ArgV(3) : nullptr;
		std::vector<std::pair<std::string, NameStat>> rows;
		{
			std::lock_guard<std::mutex> lock(g_mx);
			for (const auto& [name, stat] : g_nameStats)
				if (!filter || std::strstr(name.c_str(), filter))
					rows.push_back({ name, stat });
		}
		std::sort(rows.begin(), rows.end(),
			[](const auto& a, const auto& b) { return a.second.seen > b.second.seen; });
		const size_t cap = 60;
		advancedfx::Message("fx: %zu name(s)%s (top %zu by count; seen/acted):\n",
			rows.size(), filter ? " matching filter" : "", rows.size() < cap ? rows.size() : cap);
		for (size_t i = 0; i < rows.size() && i < cap; ++i)
			advancedfx::Message("  %6llu %5llu  %s\n", rows[i].second.seen, rows[i].second.acted,
				rows[i].first.c_str());
		return;
	}
	if (0 == _stricmp(action, "block") || 0 == _stricmp(action, "swap")) {
		const bool isSwap = 0 == _stricmp(action, "swap");
		if (argc < (isSwap ? 5 : 4)) {
			advancedfx::Warning("usage: %s fx %s <substr>%s\n", cmd, action, isSwap ? " <target.vpcf>" : "");
			return;
		}
		if (!EffectsPersistence_AllowEnable("Particle rules"))
			return;
		CustomRule r;
		r.match = args->ArgV(3);
		std::transform(r.match.begin(), r.match.end(), r.match.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });
		if (isSwap)
			r.target = args->ArgV(4);
		{
			std::lock_guard<std::mutex> lock(g_mx);
			g_customRules.push_back(r);
			g_enabled = true;
			RebuildActiveSwapTargetsLocked(true);
		}
		fx.EnsureInstalled();
		fx.SaveSettings();
		RequestApplyReseek();
		if (FxMainThreadContextActive())
			ArmWarmup(false);
		advancedfx::Message("fx: rule added: '%s' -> %s.\n", r.match.c_str(),
			r.target.empty() ? "BLOCK" : r.target.c_str());
		return;
	}
	if (0 == _stricmp(action, "unblock") || 0 == _stricmp(action, "unswap")) {
		if (argc < 4) { advancedfx::Warning("usage: %s fx %s <substr>\n", cmd, action); return; }
		std::string match = args->ArgV(3);
		std::transform(match.begin(), match.end(), match.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });
		size_t removed = 0;
		{
			std::lock_guard<std::mutex> lock(g_mx);
			const size_t before = g_customRules.size();
			g_customRules.erase(std::remove_if(g_customRules.begin(), g_customRules.end(),
				[&](const CustomRule& r) { return r.match == match; }), g_customRules.end());
			removed = before - g_customRules.size();
			RebuildActiveSwapTargetsLocked(removed != 0);
		}
		fx.SaveSettings();
		RequestApplyReseek();
		if (FxMainThreadContextActive())
			ArmWarmup(false);
		advancedfx::Message("fx: removed %zu rule(s) matching '%s'.\n", removed, match.c_str());
		return;
	}
	if (0 == _stricmp(action, "spawnat")) {
		// Diagnostic: create a system at a world point through the game particle manager (the
		// path the MW2019 flash grenade uses), to tell an asset that never draws from a bad spawn.
		if (argc < 7) { advancedfx::Warning("usage: %s fx spawnat <particles/....vpcf> <x> <y> <z>\n", cmd); return; }
		const float p[3] = { (float)atof(args->ArgV(4)), (float)atof(args->ArgV(5)), (float)atof(args->ArgV(6)) };
		const int index = SpawnParticleAt(args->ArgV(3), p);
		advancedfx::Message("fx: spawnat %s at %.1f %.1f %.1f -> index %d\n", args->ArgV(3), p[0], p[1], p[2], index);
		return;
	}
    if (!_stricmp(action,"helights") || !_stricmp(action,"flashlights")) {
        const bool flash=!_stricmp(action,"flashlights");const char* value=argc>=4?args->ArgV(3):"";
        if(!_stricmp(value,"on")&&!EffectsPersistence_AllowEnable("Dynamic lights"))return;
        if(!_stricmp(value,"on"))fx.SetGrenadeLights(flash,true);
        else if(!_stricmp(value,"off"))fx.SetGrenadeLights(flash,false);
        else if(*value){advancedfx::Warning("usage: %s fx %s on|off\n",cmd,action);return;}
        advancedfx::Message("fx: %s dynamic lights %s (native CS2 lighting).\n",flash?"Flash":"HE",fx.GrenadeLights(flash)?"ON":"OFF");return;
    }
	if (0 == _stricmp(action, "mollylights")) {
        const char* value=argc>=4?args->ArgV(3):"";
        if(!_stricmp(value,"on")&&!EffectsPersistence_AllowEnable("Dynamic lights"))return;
        if(!_stricmp(value,"on"))fx.SetMollyLights(true);
        else if(!_stricmp(value,"off"))fx.SetMollyLights(false);
        else if(*value){advancedfx::Warning("usage: %s fx mollylights on|off\n",cmd);return;}
        advancedfx::Message("fx: Molly dynamic lights %s (native CS2 lighting).\n",fx.MollyLights()?"ON":"OFF");return;
    }
	if (0 == _stricmp(action, "flashblind")) {
		const char* v = (argc >= 4) ? args->ArgV(3) : "";
		if (0 == _stricmp(v, "on")) fx.SetFlashBlind(true);
		else if (0 == _stricmp(v, "off") && EffectsPersistence_AllowEnable("No-blind flashbangs")) fx.SetFlashBlind(false);
		advancedfx::Message("fx: flash blindness %s.\n", fx.FlashBlind() ? "ON (CS2 default)" : "OFF (flashbangs never blind)");
		return;
	}
	if (0 == _stricmp(action, "moneyshot")) {
		const char* v = (argc >= 4) ? args->ArgV(3) : "";
		if (0 == _stricmp(v, "on") && !EffectsPersistence_AllowEnable("Money on headshot")) return;
		if (0 == _stricmp(v, "on")) fx.SetMoneyHeadshot(true);
		else if (0 == _stricmp(v, "off")) fx.SetMoneyHeadshot(false);
		else { advancedfx::Message("fx: moneyshot is %s (fx moneyshot on|off).\n", fx.MoneyHeadshot() ? "ON" : "off"); return; }
		advancedfx::Message("fx: money-on-headshot %s.\n",
			fx.MoneyHeadshot() ? "ON (swaps the headshot-only particles; requires converted money asset)" : "off");
		return;
	}
	if (0 == _stricmp(action, "apply")) {
		// Manual, immediate, regardless of pause state: re-create the current moment's
		// live effects under the current rules (tiny backward seek).
		ApplyReseekNow();
		advancedfx::Message("fx: re-applied settings to live effects (reseek).\n");
		return;
	}
	if (0 == _stricmp(action, "rules")) {
		std::lock_guard<std::mutex> lock(g_mx);
		if (g_customRules.empty()) { advancedfx::Message("fx: no custom rules.\n"); return; }
		for (size_t i = 0; i < g_customRules.size(); ++i)
			advancedfx::Message("  %zu | '%s' -> %s\n", i, g_customRules[i].match.c_str(),
				g_customRules[i].target.empty() ? "BLOCK" : g_customRules[i].target.c_str());
		return;
	}
	if (0 == _stricmp(action, "test")) {
		if (argc < 4) { advancedfx::Warning("usage: %s fx test <particles/....vpcf>\n", cmd); return; }
		FxTestName(args->ArgV(3));
		return;
	}
	if (0 == _stricmp(action, "align")) {
		// Modern muzzle-FX alignment probe (unit-distance measurement; see FxAlign.h).
		FxAlign_RunCommand(argc, args, cmd);
		return;
	}
	if (0 == _stricmp(action, "debughud")) {
		const char* v = (argc >= 4) ? args->ArgV(3) : "";
		if (0 == _stricmp(v, "on")) g_debugHudEnabled.store(true, std::memory_order_relaxed);
		else if (0 == _stricmp(v, "off")) g_debugHudEnabled.store(false, std::memory_order_relaxed);
		else {
			advancedfx::Message("fx: debughud is %s (%s fx debughud on|off).\n",
				g_debugHudEnabled.load(std::memory_order_relaxed) ? "ON" : "off", cmd);
			return;
		}
		advancedfx::Message("fx: debughud %s. Labeled squares blink per shot (each swap "
			"toggles them 1-0-1-0) for muzzle / tracer / On-smoke / Modern-smoke; "
			"they go dark ~1.5s after the last swap.\n",
			g_debugHudEnabled.load(std::memory_order_relaxed) ? "ON" : "off");
		return;
	}
	PrintFxHelp(cmd);
	advancedfx::Message("[fx][state] %s\n", fx.DebugStateJson().c_str());
}

} // namespace Filmmaker
