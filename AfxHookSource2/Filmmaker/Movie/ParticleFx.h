#pragma once

// Runtime particle-effect modifiers (the EFFECTS section of the Config panel): per-category
// On / Less / Modern / Off control (each category offers only the modes it has real assets
// for) over a demo's visual effects -- bullet impacts, muzzle flash + shell smoke, tracers,
// blood, HE-grenade explosions, the bomb blast, molotov fire, map ambience. The
// hook can only swap to Source 2 particle resources (.vpcf/.vpcf_c) known to CS2. The old
// The Insurgency 2 sources in FX/sources/insurgency2/ are Source 1 .pcf/.vmt/.vtf files and
// must be ported with source1import before they can be used here; stock placeholder swaps
// are intentionally not used.
//
// HOW: one Detours hook on particles.dll's CParticleSystemMgr create-collection
// implementation -- the single chokepoint every particle-system instantiation funnels
// through (name-based creates, resource-handle creates and the async client path all end
// there). The hook reads the system's resource name off the handle ([[handle+8]] -> char*,
// guarded), classifies it by path prefix, and either passes it through or SWAPS the handle
// for a different system resolved by name via the manager's own FindParticleSystem. Per-
// category "Off" is vanilla CS2 pass-through; only explicit custom block rules swap to
// the engine's stock empty system (particles/dev/empty.vpcf). Any failed swap fails OPEN
// (original effect plays).
//
// Resolution is deliberately pattern-free: CreateInterface("ParticleSystemMgr003") ->
// vtable slots (FindParticleSystem +0x78, create-by-handle stub +0x88, create-by-name
// +0x90), then the stub's tail-jmp is followed to the shared body so the detour also
// catches the internal direct-call (profiling-duplicate) path. Every resolved pointer is
// bounds-checked against the particles.dll image and the stub/body byte shapes are
// validated; any mismatch after a CS2 update degrades the feature to INACTIVE with a
// console warning -- effects then simply play unmodified (repo convention: pattern misses
// degrade, never crash).
//
// SMOKE GRENADES ARE DELIBERATELY NOT TOUCHED: CS2 smoke is a gameplay-coupled volumetric
// voxel system, not a swappable particle system.

#include <string>

namespace advancedfx { class ICommandArgs; }
namespace SOURCESDK { namespace CS2 { class CGameEvent; } }
class CEntityInstance;

namespace Filmmaker {

// Category order is also the persisted/UI order. kFxTracers is matched BEFORE kFxWeaponFx
// (tracers live under the same weapon-fx path prefixes and are split out by "tracer" in the
// name) so the two can be controlled independently. kFxBombFx is likewise split out of the
// explosion folders by "c4"/"bomb" in the name (2026-07-03): the user wants the CS2 bomb
// blast and the HE grenade independently swappable (e.g. Insurgency bomb + MW2019 HE).
enum FxCategory {
	kFxImpacts = 0,   // particles/impact_fx/ + water_impact/ + breakable_fx/ (bullet-surface impacts)
	kFxTracers,       // .../weapons paths + "tracer"    (bullet tracers)
	kFxWeaponFx,      // particles/weapons/cs_weapon_fx/ + particles/unified_weapon_fx/
	kFxExplosions,    // explosion folders, minus the bomb (HE grenade + generic env explosions)
	kFxBombFx,        // explosion folders + "c4"/"bomb" in the name (the planted-bomb blast)
	kFxMolotov,       // particles/burning_fx/ + particles/inferno_fx*
	kFxFlashbang,     // explosion folders + "flashbang" in the name (on = the MW2019 flash grenade)
	kFxCategoryCount
};

// Not every category supports every mode -- Less exists only where the mod's less folders
// actually differ from classic-updated (verified on the converted trees 2026-07-03: only
// impact_fx and explosions_fx differ), and Modern exists only where the converted MW2019
// pack ships real assets (flashes/shell smoke, tracers, HE explosion). SetMode/LoadSettings
// normalize unsupported modes to On; the Config panel only offers the supported subset.
enum class FxMode : unsigned char {
	On = 0,     // converted Insurgency 2 (the full-detail systems)
	Less = 1,   // legacy persisted value; normalized to On (Impact On uses reduced assets internally)
	Off = 2,    // vanilla CS2 pass-through for this category
	More = 3,   // LEGACY alias of On (dropped 2026-07-02: its asset folder was byte-identical to
	            // On's, so the modes never looked different; kept so old persisted configs load)
	Modern = 4  // converted MW2019 (ARC9/GMod) pack under particles/filmmaker/modern/
};

// True when `mode` is a real, selectable mode for `cat` (Off/On always are).
bool FxModeSupported(FxCategory cat, FxMode mode);

class ParticleFx {
public:
	// Resolves the manager + installs the detour. Cheap when already installed; returns
	// false (and only warns ONCE for hard shape mismatches) when the hook cannot arm yet.
	bool EnsureInstalled();
	bool Installed() const;
	// True when the master switch plus an active category/auxiliary feature need the hook;
	// explicit diagnostic logging can also force it while effects remain disabled.
	bool WantsHook() const;
	// Lazy install retry, called once per main-thread frame (internally rate-limited).
	void PumpMainThread();

	bool Enabled() const;            // master switch; fresh runtime default is Off
	void SetEnabled(bool on);        // auto-saves; Off cancels pending resolves
	FxMode Mode(FxCategory cat) const;
	void SetMode(FxCategory cat, FxMode mode); // non-Off auto-enables master; auto-saves

	void SetLogging(bool on);        // capture creations to the recent-ring (+ mvm_debug)
	bool Logging() const;

	// Optional money/headshot effect. It is event-gated: candidate headshot particles are
	// only eligible immediately after a real player_hurt/player_death headshot event.
	bool MoneyHeadshot() const;
	void SetMoneyHeadshot(bool on);
	// Flash blindness: false = flashbangs never blind anyone (see g_noFlashBlind).
	bool FlashBlind() const;
	void SetFlashBlind(bool on);
	bool MollyLights() const;
	void SetMollyLights(bool on);
	bool MollyLightsReady() const;
	bool GrenadeLights(bool flash) const;
	void SetGrenadeLights(bool flash,bool on);

	void LoadSettings();             // tolerant: missing/malformed file = defaults
	bool SaveSettings() const;       // skipped (reported done) during a practice session
	// A practice session (Clippy's tutorial) begins: every effect back to its construction default.
	// It ends: the defaults, then the file over them. Both install and warm up what is switched on,
	// as a setter would. Main thread.
	void ResetSettingsToDefaults();
	void ReloadSettings();
	void AfterSettingsReplaced();

	std::string DebugStateJson() const;
};

ParticleFx& ParticleFxRef();

// Lowercase JSON/command tokens ("on"/"less"/"off", "impacts"/"tracers"/...).
const char* FxModeName(FxMode mode);
const char* FxCategoryKey(FxCategory cat);

// MAIN THREAD. Load a particle system by name through CS2's just-in-time MANIFEST loader (the path
// map precache uses) and report whether it is create-ready. Anything that creates a system CS2 did
// not load for this map must call this first: a naked load leaves fallback fields unfixed and the
// first create crashes particles.dll (ParticleFxHook.cpp CRASH LESSON 5/6).
bool ParticleFx_PrecacheSystem(const char* name);

// Console entry: handles "mirv_filmmaker fx ..." (dispatched from FilmmakerCommand.cpp).
void ParticleFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// Called from the engine's LevelInitPreEntity detour (main.cpp). A level teardown frees
// every loaded particle definition, so every cached swap-target handle must be dropped
// before the new level can create anything -- a demo reloading its own level mid-playback
// (round-boundary jump-back) otherwise crashes the engine on the first post-reload swap.
// See CRASH LESSON 7 in ParticleFxHook.cpp.
void ParticleFx_OnLevelInit();

// FxDebugHud support ("mirv_filmmaker fx debughud on|off"): on-screen labeled squares.
// Each class carries the last swap wall-clock (GetTickCount64 ms; 0 = never this session)
// AND a per-event counter: the HUD blinks a square per EVENT (counter parity toggles it
// 1-0-1-0 per bullet, robust to demo_timescale) and darkens it once the timestamp goes
// stale (weapon switch / fire stop).
struct FxDebugHudState {
	bool enabled = false;
	unsigned long long nowMs = 0;
	unsigned long long muzzleMs = 0;
	unsigned long long tracerMs = 0;
	unsigned long long onSmokeMs = 0;
	unsigned long long modSmokeMs = 0;
	unsigned long long muzzleN = 0;
	unsigned long long tracerN = 0;
	unsigned long long onSmokeN = 0;
	unsigned long long modSmokeN = 0;
};
bool ParticleFx_DebugHudEnabled();
void ParticleFx_GetDebugHudState(FxDebugHudState& out);

// Called from the global game-event hook so effect decisions can be tied to real demo
// events instead of generic particle names.
void ParticleFx_OnGameEvent(SOURCESDK::CS2::CGameEvent* event);

// Camera-detached first-person-FX suppression: while the rendered camera is away from
// the spectated eye (third person / free cam), the engine still spawns the viewmodel's
// _fp/_fps weapon FX anchored to the CAMERA -- floating mid-air muzzle flashes. This
// gate blocks those creations to dev/empty. Independent of the EFFECTS modes; driven
// from Filmmaker::RunMainThreadFrame alongside r_drawviewmodel.
void ParticleFx_SetFirstPersonFxSuppressed(bool on);
bool ParticleFx_FirstPersonFxSuppressed();

// Blood suppression (the UwU easter egg): every particle system with "blood" in its path is
// blocked to dev/empty -- hit sprays, headshot bursts, pools, the decals they paint. CS2 ignores
// violence_hblood for demo playback, so the create hook is the place. Independent of the EFFECTS
// modes, like the first-person gate.
void ParticleFx_SetBloodSuppressed(bool on);
unsigned long long ParticleFx_BloodBlocked();
// The same switch, read by the scene draw hook (SceneSystem.cpp): while on, draw batches whose
// materials are ALL blood (the splats left on floors and walls) are skipped. Bullet holes stay.
bool ParticleFx_BloodSuppressed();
void ParticleFx_NoteBloodDecalSkipped();
unsigned long long ParticleFx_BloodDecalsSkipped();

// True when the master EFFECTS switch and Nade Trails are enabled. The projectile-trail
// hook uses this for smoke grenade flight only; CS2's volumetric smoke cloud stays stock.
bool ParticleFx_NadeTrailsEnabled();

// Per-entity dispatch bookkeeping for the smoke-grenade flight particle. The actual effect is
// owned by CS2's normal entity particle-property path; this table only prevents the main-thread
// pump from dispatching a duplicate and is cleared by New_OnRemoveEntity.
int ParticleFx_SmokeTrailLookup(CEntityInstance* ent);
void ParticleFx_SmokeTrailTrack(CEntityInstance* ent, int particleIndex);
void ParticleFx_SmokeTrailForget(CEntityInstance* ent);

// True while `demoTick` is inside the spectated player's weapon_fire window (the same
// watched-player gate FxDebugHud uses). Lets FxAlign attribute the spectated player's
// THIRD-PERSON muzzle effects (no _fps marker) to them; tracked while FxDebugHud or the
// alignment probe is enabled.
bool ParticleFx_WatchedFireWindow(int demoTick);

} // namespace Filmmaker
