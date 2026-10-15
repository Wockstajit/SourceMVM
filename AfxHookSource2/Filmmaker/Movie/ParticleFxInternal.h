#pragma once

// INTERNAL shared surface of the ParticleFx subsystem. Not part of the public API --
// include ParticleFx.h for that. This header exists so the subsystem can be split into
// focused translation units while the runtime behavior (one mutex over the rule/telemetry
// state, lock-free atomics for the hook's hot path) stays exactly what the original
// single-file implementation had:
//
//   ParticleFx.cpp            core: engine binding, apply-reseek, public class methods,
//                             the `fx` console command dispatch
//   ParticleFxRules.cpp       category classification + the FXRULE swap tables +
//                             target selection + swap-target pre-queueing
//   ParticleFxHook.cpp        SEH-guarded engine reads, vtable resolution, the
//                             create-collection detour, handle resolution + JIT redirect
//   ParticleFxMoney.cpp       money-on-headshot candidates + game-event plumbing
//   ParticleFxDiagnostics.cpp FxDebugHud state, telemetry ring/name stats, agent logs,
//                             watched-player fire window
//   ParticleFxSettings.cpp    JSON persistence (filmmaker_fx.json)
//
// Everything here lives in Filmmaker::fx (internal); the public entry points stay in
// Filmmaker as declared by ParticleFx.h.

#include "ParticleFx.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Filmmaker {
namespace fx {

// ============================== shared types =======================================

struct CustomRule {
	std::string match;  // lowercase substring of the full resource path
	std::string target; // exact target resource path to swap to; empty = block (empty system)
};

struct FxEvent {
	std::string name;   // as read off the handle
	char action;        // '=' pass, 'X' blocked, '>' swapped
	std::string target; // for '>'
};

struct NameStat {
	unsigned long long seen = 0;
	unsigned long long acted = 0;
};

struct HandleCacheEntry {
	void* handle = nullptr;
	unsigned long long retryAtMs = 0; // for failed resolves: don't re-resolve until then
	// Resource-context generation this handle was resolved in (see g_levelEpoch). A handle
	// from an older epoch points into freed resource memory and must never reach a create.
	unsigned epoch = 0;
	// The resource name read back OFF the handle at resolve time (NOT the name we asked
	// for -- the engine may normalize it). Re-read and compared at use time so a handle
	// whose block was freed AND reused is rejected instead of created from.
	std::string identity;
};

constexpr size_t kRingCap = 128;
constexpr size_t kNameStatsCap = 8192;

// The engine's stock do-nothing system: what "blocked" categories are swapped to, so the
// engine always gets a valid collection back (returning null risks crashing callers).
constexpr const char* kEmptySystem = "particles/dev/empty.vpcf";
// Flash grenade On: the MW2019 flash, created at the detonation point piece by piece
// (ParticleFxFlashbang.cpp): the childless core glow, then each of its children.
constexpr const char* kMw2019FlashbangParts[] = {
	"particles/filmmaker/modern/mw2019_explosions_pak/generic_explo_flash_core.vpcf",
	"particles/filmmaker/modern/mw2019_explosions_pak/generic_explo_flash_glow2.vpcf",
	"particles/filmmaker/modern/mw2019_explosions_pak/generic_explo_emp_sparks.vpcf",
	"particles/filmmaker/modern/mw2019_explosions_pak/generic_explo_high_debris.vpcf",
	"particles/filmmaker/modern/mw2019_explosions_pak/generic_explo_high_debris_sheet.vpcf",
	"particles/filmmaker/modern/mw2019_explosions_pak/generic_explo_high_smoke_shock.vpcf",
	"particles/filmmaker/modern/mw2019_explosions_pak/generic_explo_high_smoke.vpcf",
	"particles/filmmaker/modern/mw2019_explosions_pak/generic_explo_high_shrapnel.vpcf",
};
// flashbang_detonate position (any thread) -> created by the main-thread pump.
void FlashbangFx_OnDetonate(float x, float y, float z);
void FlashbangFx_PumpMainThread();
// Main thread. Creates `name` at a world point via the game particle manager; -1 on failure.
int SpawnParticleAt(const char* name, const float* position);
// Money-on-headshot lives in its OWN converted namespace (FX\sources\povarehok-money\ ->
// particles/filmmaker/money/), not inside a mode pack: it is an independent toggle that
// composes with any FxMode. It is also the one effect kept from the retired CS:GO Povarehok
// mod through the 2026-09-01 Insurgency 2 rebake, because Insurgency ships no money burst.
// Keep in sync with MONEY_ROOT_SYSTEM in FX/tools/extract-povarehok-money.py.
constexpr const char* kMoneyBurst = "particles/filmmaker/money/impact_fxmoney/impact_helmet_headshot.vpcf";

// On-mode Insurgency impacts keep the flying smoke-ribbon children on this many of every
// ten mapped hits. The rest use the generated `_nosmoketrails` closure. Particle burst
// counts are integers, so the probability lives here instead of inside the VPCF.
constexpr unsigned kInsurgencySmokeTrailKeepPerTen = 6u;

// ============================== core state (ParticleFx.cpp) ========================

// One mutex guards all mutable rule/telemetry state. Creation frequency is low (dozens per
// second at the wildest), so a plain mutex in the hook is fine; the engine call used for
// swap resolution is made OUTSIDE the lock.
extern std::mutex g_mx;
extern bool g_enabled;
extern FxMode g_modes[kFxCategoryCount];
extern std::vector<CustomRule> g_customRules;
extern bool g_logging;
extern bool g_moneyHeadshot; // guarded by g_mx
// Flash blindness off: flashbangs still go off (sound, particles), but no player is ever blinded,
// so the detonation can be seen and filmed. Atomic: read every main-thread frame.
extern std::atomic<bool> g_noFlashBlind;
extern std::atomic<bool> g_mollyLights;
extern std::atomic<bool> g_heLights, g_flashLights;
void NativeLightFx_OnDetonate(bool flash,int entity,float x,float y,float z);
bool FxMainThreadContextActive();
void MollyFx_PumpMainThread();
void MollyFx_OnLevelInit();
bool MollyFx_Ready();
extern std::atomic<unsigned long long> g_applyAtMs;
// Resource-context generation. Bumped every time the engine tears the level down and
// rebuilds it (LevelInitPreEntity, plus an engine-framecount-reset backstop), which frees
// every loaded particle definition. Cached handles are stamped with the epoch they were
// resolved in and rejected once it moves on -- see CRASH LESSON 7 in ParticleFxHook.cpp.
extern std::atomic<unsigned> g_levelEpoch;
// Published by PumpMainThread each frame: the hook runs on whatever thread creates
// particles, and polling the engine's demo interface there is not established as
// safe, so it reads this atomic instead.
extern std::atomic<int> g_demoTickNow;

bool DemoIsPlaying();
bool DemoIsPaused();
int DemoTick();
void ApplyReseekNow();
void RequestApplyReseek();

// ============================== rules (ParticleFxRules.cpp) ========================

extern const char* kCategoryKeys[kFxCategoryCount];
extern const char* kModeNames[5];
constexpr int kModeCount = 5;

void LowerCopy(const char* in, char* out, size_t cap);
int ClassifyLower(const char* n);                    // lowercase name -> FxCategory or -1
bool ModeSupported(int cat, FxMode mode);
FxMode NormalizeMode(int cat, FxMode mode);
int CategoryFromKey(const char* key);
int ModeFromName(const char* name);
const char* VariantTargetLower(int cat, FxMode mode, const char* n);
const char* ImpactHybridTargetLower(const char* n);
std::string ImpactNoSmokeTrailTarget(const char* target);
// Per-surface vanilla wrapper name for a final impact target; empty if not an impact path.
std::string ImpactSurfaceLayeredTarget(const char* target, const char* vanillaName);
// g_mx held. True only when the master switch and at least one category/auxiliary FX
// feature are active. Logging is deliberately excluded: it needs the hook, not assets.
bool HasActiveFxLocked();
// Reconcile the authoritative active-target set and pending resolver queue with current
// settings. Master-off always clears active/pending targets. Resolved handles are retained
// on master-off, but an explicit mode/rule change can invalidate obsolete handles.
void RebuildActiveSwapTargetsLocked(bool invalidateObsoleteHandles);
void RebuildActiveSwapTargets(bool invalidateObsoleteHandles);
// g_mx held. Warm-up entry point: queue EVERY active target that has no cached handle
// (unlike the eager-only queueing in RebuildActiveSwapTargetsLocked). The main-thread
// burst drain in ParticleFx.cpp then front-loads all cold blocking loads at demo open /
// settings switch instead of hitching playback one lazy miss at a time.
void QueueAllUncachedActiveTargetsLocked();

// ============================== hook (ParticleFxHook.cpp) ==========================

extern std::atomic<bool> g_installed;
extern bool g_installFailedHard;     // shape mismatch: stop retrying, feature inactive
extern unsigned long long g_lastInstallTryMs;
extern bool g_jitRedirectInstalled;
extern std::unordered_map<std::string, HandleCacheEntry> g_handleCache; // guarded by g_mx
// Names current settings permit the hook to resolve. Checking this set prevents an
// in-flight creation from requeueing an obsolete target after FX is disabled or changed.
extern std::vector<std::string> g_activeTargets; // guarded by g_mx
// Swap-target names waiting for MAIN-THREAD resolution (guarded by g_mx).
extern std::vector<std::string> g_resolveQueue;

bool TryInstall();
// MAIN THREAD ONLY (see the crash lesson at its definition).
void ResolveHandleOnMainThread(const char* name);

// Camera-detached first-person-FX suppression (ParticleFx.cpp): while the rendered
// camera is away from the spectated eye (third person / free cam), _fp/_fps viewmodel
// weapon FX are blocked to dev/empty -- they anchor to the CAMERA and would float in
// mid-air. Independent of g_enabled (a camera-correctness gate, not an EFFECTS mode).
extern std::atomic<bool> g_fpFxSuppress;
// UwU's blood gate (ParticleFx_SetBloodSuppressed): "blood" systems -> dev/empty.
extern std::atomic<bool> g_bloodSuppress;
extern std::atomic<unsigned long long> g_bloodBlocked;

// ====================== prefetch (ParticleFxPrefetch.cpp) ==========================

// Once per process: detached background thread that plain-reads every file of the
// mounted FX asset pack (USRLOCALCSGO) to warm the OS file cache. Touches no engine
// APIs, so it is thread-safe; it just makes the main-thread blocking resolves cheap.
void PrefetchFxPackOnce();

// ============================== money (ParticleFxMoney.cpp) ========================

bool IsMoneyHeadshotCandidateLower(const char* n);

// ==================== diagnostics (ParticleFxDiagnostics.cpp) ======================

// Telemetry (guarded by g_mx).
extern std::vector<FxEvent> g_ring; // capped ring, g_ringNext = oldest slot
extern size_t g_ringNext;
extern std::map<std::string, NameStat> g_nameStats;
extern std::atomic<unsigned long long> g_totalSeen;
extern std::atomic<unsigned long long> g_totalNoName;
extern std::atomic<unsigned long long> g_totalBlocked;
extern std::atomic<unsigned long long> g_totalSwapped;

// FxDebugHud state (see the block comment in ParticleFxDiagnostics.cpp).
extern std::atomic<bool> g_debugHudEnabled;
extern std::atomic<unsigned long long> g_dbgMuzzleMs, g_dbgTracerMs, g_dbgOnSmokeMs,
	g_dbgModSmokeMs;
extern std::atomic<unsigned long long> g_dbgMuzzleN, g_dbgTracerN, g_dbgOnSmokeN,
	g_dbgModSmokeN;
// Watched-player gate: light squares only when a swap coincides with weapon_fire from the
// spectated POV player (not every other player shooting elsewhere in the demo).
extern std::atomic<int> g_watchedUserId;
extern std::atomic<int> g_watchedFireDemoTick;
extern std::atomic<unsigned long long> g_watchedFireWallMs;

// Flicker-investigation dedup map (agent log, session 7803fe).
extern std::mutex g_dbgFlickerMx;
extern std::unordered_map<std::string, unsigned long long> g_dbgLastCreateMs;

void RecordEventLocked(const char* name, char action, const char* target); // g_mx held
void RecordNameLocked(const char* lowName, bool acted);                    // g_mx held
int UserIdForSpectatedPawn();
bool IsFirstPersonWeaponFxPath(const char* low);
bool IsWithinWatchedFireWindow(int demoTick);
bool ShouldUpdateFxDebugHud(const char* vanillaLow, int demoTick, char action);

// #region agent log helpers (debug sessions 43a665 / 7803fe)
void DbgAgentLog(const char* hypothesisId, const char* location, const char* message,
	const char* vanilla, const char* baseTarget, const char* finalTarget, int demoTick);
bool DbgIsBarrelSmokePath(const char* s);
void DbgFlickerLog(const char* hypothesisId, const char* message, const char* raw,
	const char* target, bool isComposition, bool isDuplicate, long long msSinceSameName,
	int demoTick);
bool DbgIsFlickerRelevant(const char* s);
const char* DbgModernAlignClass(const std::string& target);
void DbgWatchLog(const char* hypothesisId, const char* message, int uid, int watched,
	int demoTick, const char* path = nullptr);
// #endregion

} // namespace fx
} // namespace Filmmaker
