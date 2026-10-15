#include "../Movie/SceneKeys.h"
#include "EffectsPersistence.h"
#include "../World/LightFx.h"

#include "FilmmakerSettingsStore.h"  // BeginQuietReset: a keep-off reset must not reach the file

#include "../World/CloudFx.h"        // re-assert: cloud material parameters are level-owned
#include "../World/SkyboxLibrary.h"  // re-assert: the sky material precache is level-owned
#include "../World/SunFx.h"          // re-assert: sun disc / glow parameters are level-owned
#include "../MvmTest.h"                    // the live-map half of the world-ready gate
#include "../Debug/MvmDebugLog.h" // MvmDebugLog_Linef
#include "../../RenderSystemDX11Hooks.h"
#include "../../../Shared/AfxConsole.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstring>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

bool g_enabled = true; // hard kill switch, for bisecting a crash against this module

// One flag per CONFIG tab, all defaulting ON: what you set up carries into the next demo.
enum KeepGroup { kKeepCamera = 0, kKeepEffects, kKeepRagdolls, kKeepCount };
bool g_keep[kKeepCount] = { true, true, true };
const char* const kKeepNames[kKeepCount] = { "camera", "effects", "ragdolls" };

int KeepIndex(const char* group) {
	if (!group) return -1;
	for (int i = 0; i < kKeepCount; ++i)
		if (0 == _stricmp(group, kKeepNames[i])) return i;
	return -1;
}

// CAMERA and RAGDOLLS are pure module state, so "do not keep" can only be honoured by putting
// them back to their defaults. Both the Config footers and keep-off resets execute
// these lists, including every effect switch and pivot marker.
//
// `settings reset <group>` replays the snapshot taken before the saved file was read, so its
// idea of "default" is the modules' own constructors and cannot drift from the code.
const char* const kCameraReset[] = {
	"mirv_filmmaker settings reset smoothView",
	"mirv_filmmaker settings reset viewFx",
	"mirv_filmmaker settings reset handheld",
	"mirv_filmmaker settings reset viewmodel",
	"mirv_filmmaker settings reset camFov",
	"mirv_filmmaker weaponrecoil reset quiet",
	"mirv_filmmaker weaponbob reset",
	"mirv_filmmaker pivot recoil reset",
	"mirv_filmmaker pivot bob reset",
	"mirv_filmmaker pivotorb recoil off quiet",
	"mirv_filmmaker pivotorb bob off quiet",
	"mirv_filmmaker lens reset all quiet",
	"mirv_filmmaker fisheye off",
	"mirv_filmmaker xhair off",
};
const char* const kRagdollReset[] = {
	"mirv_filmmaker ragdollphys reset",
	"mirv_filmmaker deathanim reset",
	"mirv_filmmaker nervous reset",
	"mirv_filmmaker ragdollweapon reset",
	"mirv_filmmaker methodacting reset",
};
// The tab footers call effects reset <group>, so manual resets and keep-off resets
// execute these same lists. EFFECTS covers every card on its tab.
const char* const kEffectsReset[] = {
	// VFX -- modes first, then the master: SetMode auto-enables the master for any non-Off mode,
	// so clearing the master first would just be switched back on by the row below it.
	"mirv_filmmaker fx set impacts off quiet",
	"mirv_filmmaker fx set tracers off quiet",
	"mirv_filmmaker fx set weaponfx off quiet",
	"mirv_filmmaker fx set explosions off quiet",
	"mirv_filmmaker fx set bombfx off quiet",
	"mirv_filmmaker fx set flashbang off quiet",
	"mirv_filmmaker fx set molotov off quiet",
	"mirv_filmmaker fx moneyshot off",
	"mirv_filmmaker fx flashblind on",
	"mirv_filmmaker fx off quiet",
	// LIGHTING
	"mirv_filmmaker world exposure auto quiet",
	"mirv_filmmaker world light reset",
	"mirv_filmmaker world sun reset",
	"mirv_filmmaker world shadows reset",
	"mirv_filmmaker world customshadow reset",
	// ATMOSPHERE
	"mirv_colors smokes t default",
	"mirv_colors smokes ct default",
	"mirv_filmmaker world smoke appearance reset",
	"mirv_filmmaker world modulate reset",
	// WEATHER & SKY
	"mirv_filmmaker world sky default",
	"mirv_filmmaker world skyorient reset",
	"mirv_sky color default",
	"mirv_filmmaker world clouds reset",
	"mirv_sky clouds color default",
};

void ExecCmd(const char* command) {
	if (!g_pEngineToClient || !command) return;
	g_pEngineToClient->ExecuteClientCmd(0, command, true);
}
int  g_readyFrames = 0; // observed completed presents with the world up
unsigned long long g_lastPresent = 0;
bool g_armed = false;   // a level init happened and the work has not run yet

// WHY THIS IS COUNTED FROM "WORLD IS UP" AND NOT FROM THE LEVEL-INIT DETOUR.
//
// LevelInitPreEntity fires BEFORE the new level's entities and materials exist, so a fixed frame
// count from there guarantees nothing -- during a slow demo load the frame pump can reach any
// count while the resource system is still populating. Both re-assert calls walk the engine's
// live material array and SNAPSHOT each material the first time they see it, so running early
// does not merely apply the wrong value: it records a half-loaded material as "the map's authored
// value" and then multiplies against it forever. That is also a walk over an array another part
// of the engine is still growing.
//
// So the counter only advances on frames where the world is genuinely up, and it restarts if the
// world goes away again mid-load.
//
// ~0.9s / ~2.3s / ~4.7s of WORLD-UP time at 64fps. The later passes cover materials that stream
// in after the world is nominally ready; the early one means a fast load does not sit on the
// map's own sky for five seconds first.
constexpr int kPass1 = 56;
constexpr int kPass2 = 148;
constexpr int kPass3 = 300;

// The same predicate ParticleFx uses -- INCLUDING its MvmTest_Active() condition, which is the
// whole point of it and which an earlier version of this function dropped.
//
// A non-empty level name is NOT "the operator is looking at a scene". CS2 sits in UI maps: the
// main menu, and `ui/xpshop_item` for the item viewer. Both report a real level name and both
// carry sky materials. Observed 2026-08-05 over netcon: parked in ui/xpshop_item this module
// reached all three passes and wrote the saved sun override into that map's 2 sky materials --
// `mirv_filmmaker world sun` reported "glowSize 2.28 | 2 sky, 0 glow material(s)" on a map that
// has no scene at all. Nothing good can come of carrying a demo's look onto a UI map.
//
// So: a demo that is actually playing (or paused, which is still an active demo), or a live map
// that is a real mvm_test session. Everything else answers false.
bool WorldReady() {
	if (!g_pEngineToClient) return false;
	if (auto pDemo = g_pEngineToClient->GetDemoFile()) {
		if (pDemo->IsPlayingDemo() || pDemo->IsDemoPaused()) return true;
	}
	// MvmTest_CanUseMenu() is already Active && LevelLoaded && !PlayingDemo, so it carries the
	// level-name check itself -- repeating it here would just be a second copy to keep in step.
	return MvmTest_CanUseMenu();
}

// TURNING THE CARRY-OVER OFF RESETS THE LIVE MODULES -- AND MUST NOT TOUCH THE SAVED FILE.
//
// The toggle promises one thing in the panel ("Each new demo starts from the defaults."), so all
// three groups have to honour it the same way: on the first settled pass after a level init, put
// the group back to its constructed defaults.
//
// An earlier version did exactly that for EFFECTS and it was withdrawn, because it destroyed the
// operator's settings: those commands write MODULE state, the settings store snapshots module
// state every frame, and 500ms later the saved tint / sun / clouds / smoke colours were overwritten
// on disk with the defaults. Switching a preference off silently deleted the preferences.
//
// The reset is not the problem, the autosave following it is -- so that is what is fixed. Every
// keep-off reset here is bracketed by FilmmakerSettings_BeginQuietReset(), which holds the settings
// file at its current contents until the operator actually changes something. The demo you load
// starts from the defaults; the settings you tuned are still in the file, and turning keep back on
// brings them back. Nothing is lost either way.
//
// (CAMERA and RAGDOLLS were already resetting, and were already clobbering the file for the same
// reason. The quiet window covers all three.)

// Re-apply what the level owns, and ONLY where the operator actually overrode something.
//
// Every call here is skippable and every skip is the safe answer: an un-overridden module has
// nothing to carry across, so walking its materials would be pure risk for no effect. Everything
// else in the EFFECTS tab (sky orientation, exposure, world tint, baked/live shadows, particle
// categories, smoke colour) is re-written by its own module's per-frame pump and needs nothing
// here at all.
void Reassert(int pass) {
	SceneKeys_Reassert();
	const std::string& sky = SkyboxLibrary_Current();
	const bool doSky = !sky.empty();
	const bool doClouds = CloudFx_HasOverride();
	const bool doSun = SunFx_HasOverride();

	if (LightFx_SunYaw()!=0 || LightFx_SunPitch()!=0) LightFx_SetSunYaw(LightFx_SunYaw());
	if (doSky) SkyboxLibrary_Apply(sky.c_str());
	if (doClouds) CloudFx_Apply();
	if (doSun) SunFx_Apply();

	MvmDebugLog_LinefAlways("effects.keep",
		"pass %d: sky=%s(\"%s\") clouds=%s sun=%s",
		pass, doSky ? "APPLY" : "skip", sky.c_str(),
		doClouds ? "APPLY" : "skip", doSun ? "APPLY" : "skip");
}

} // namespace

bool EffectsPersistence_Keep(const char* group) {
	const int i = KeepIndex(group);
	return i < 0 ? true : g_keep[i];
}

void EffectsPersistence_SetKeep(const char* group, bool on) {
	const int i = KeepIndex(group);
	if (i >= 0) g_keep[i] = on;
}

void EffectsPersistence_OnLevelInit() {
	g_armed = true;
	g_readyFrames = 0;
	g_lastPresent = RenderCompletedPresents();
	MvmDebugLog_LinefAlways("effects.keep",
		"level init: armed (enabled=%d keep camera=%d effects=%d ragdolls=%d)",
		g_enabled ? 1 : 0, g_keep[kKeepCamera] ? 1 : 0,
		g_keep[kKeepEffects] ? 1 : 0, g_keep[kKeepRagdolls] ? 1 : 0);
}

void EffectsPersistence_RunFrame() {
	if (!g_enabled || !g_armed) return;

	if (!WorldReady()) {
		g_lastPresent = RenderCompletedPresents();
		// Still loading, or back at the menu. Restart the settle so the passes are always
		// measured against an actually-present world.
		if (g_readyFrames > 0) {
			MvmDebugLog_LinefAlways("effects.keep", "world went away at frame %d; settle restarted",
				g_readyFrames);
			g_readyFrames = 0;
		}
		return;
	}

	const auto presents = RenderCompletedPresents();
	if (presents == g_lastPresent) return; // A live main pump is not a rendered frame.
	if (presents < g_lastPresent) g_readyFrames = 0;
	g_lastPresent = presents;
	const int frame = ++g_readyFrames;
	if (frame == 1)
		MvmDebugLog_LinefAlways("effects.keep", "world ready; settling");

	// Every keep-off group acts ONCE, on the first settled pass. Nothing about them streams in
	// late, so there is nothing for the later passes to catch.
	if (frame == kPass1) {
		const bool anyReset = !g_keep[kKeepCamera] || !g_keep[kKeepRagdolls] || !g_keep[kKeepEffects];
		// BEFORE the first command, never after: the store samples every frame, so a window opened
		// afterwards would already have the reset baked into its "saved" baseline.
		if (anyReset) FilmmakerSettings_BeginQuietReset();
		if (!g_keep[kKeepCamera]) {
			for (const char* c : kCameraReset) ExecCmd(c);
			MvmDebugLog_LinefAlways("effects.keep", "camera keep off: reset to defaults");
		}
		if (!g_keep[kKeepRagdolls]) {
			for (const char* c : kRagdollReset) ExecCmd(c);
			MvmDebugLog_LinefAlways("effects.keep", "ragdolls keep off: reset to defaults");
		}
		if (!g_keep[kKeepEffects]) {
			for (const char* c : kEffectsReset) ExecCmd(c);
			MvmDebugLog_LinefAlways("effects.keep", "effects keep off: reset to defaults (%d commands)",
				(int)(sizeof(kEffectsReset) / sizeof(kEffectsReset[0])));
		}
	}

	if (!g_keep[kKeepEffects]) {
		// Reset, and deliberately NOT re-asserted: the later passes exist only to catch materials
		// that stream in after the world is nominally ready, and there is nothing to re-assert.
		if (frame == kPass1) g_armed = false;
		return;
	}
	if (frame == kPass1 || frame == kPass2) Reassert(frame == kPass1 ? 1 : 2);
	else if (frame >= kPass3) { Reassert(3); g_armed = false; }
}

void EffectsPersistence_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* op = argc >= 3 ? args->ArgV(2) : "";
	if (0 == _stricmp(op, "reset")) {
		const char* group = argc >= 4 ? args->ArgV(3) : "";
		const int i = KeepIndex(group);
		if (i == kKeepCamera) for (const char* c : kCameraReset) ExecCmd(c);
		else if (i == kKeepEffects) for (const char* c : kEffectsReset) ExecCmd(c);
		else if (i == kKeepRagdolls) for (const char* c : kRagdollReset) ExecCmd(c);
		else { advancedfx::Warning("Use effects reset camera|effects|ragdolls.\n"); return; }
		MvmDebugLog_LinefAlways("config.reset", "tab=%s disabled and reset", group);
		return;
	}
	if (0 == _stricmp(op, "keep")) {
		const char* group = argc >= 4 ? args->ArgV(3) : "";
		const int i = KeepIndex(group);
		if (i < 0) {
			advancedfx::Message("effects keep: camera=%s effects=%s ragdolls=%s\n"
				"Usage: %s effects keep camera|effects|ragdolls on|off|toggle\n",
				g_keep[kKeepCamera] ? "on" : "off", g_keep[kKeepEffects] ? "on" : "off",
				g_keep[kKeepRagdolls] ? "on" : "off", cmd);
			return;
		}
		if (argc >= 5) {
			const char* v = args->ArgV(4);
			g_keep[i] = (0 == _stricmp(v, "toggle")) ? !g_keep[i]
				: (0 == _stricmp(v, "on") || 0 == _stricmp(v, "1") || 0 == _stricmp(v, "true"));
		}
		advancedfx::Message("mirv_filmmaker: %s keep = %s.\n", kKeepNames[i], g_keep[i] ? "on" : "off");
		return;
	}
	if (0 == _stricmp(op, "on") || 0 == _stricmp(op, "off") || 0 == _stricmp(op, "toggle")) {
		g_enabled = (0 == _stricmp(op, "toggle")) ? !g_enabled : (0 == _stricmp(op, "on"));
		if (!g_enabled) g_armed = false;
		advancedfx::Message("mirv_filmmaker: effects carry-over module = %s%s.\n",
			g_enabled ? "on" : "OFF",
			g_enabled ? "" : " (nothing runs on level change; use this to rule the module out)");
		return;
	}
	if (0 == _stricmp(op, "reapply")) {
		if (!WorldReady()) {
			advancedfx::Warning("mirv_filmmaker: no level is up; nothing to re-assert onto.\n");
			return;
		}
		Reassert(0);
		advancedfx::Message("mirv_filmmaker: effects re-asserted onto the current level.\n");
		return;
	}
	if (0 == _stricmp(op, "status")) {
		// The format string and the argument list MUST stay in step. This one had 8 conversions
		// against 10 arguments (the single `keep=%s` predates the split into three per-tab flags),
		// so `worldReady=%s` was handed the readyFrames INT and dereferenced it as a char* --
		// `mirv_filmmaker effects status` took CS2 down with an ACCESS_VIOLATION read at 0x38
		// inside ucrtbase every single time it was typed. Counted: 4 + 3 + 3 = 10.
		advancedfx::Message(
			"effects carry-over: module=%s armed=%s worldReady=%s readyFrames=%d\n"
			"  keep: camera=%s effects=%s ragdolls=%s\n"
			"  overrides: sky=\"%s\" clouds=%s sun=%s\n",
			g_enabled ? "on" : "off", g_armed ? "yes" : "no",
			WorldReady() ? "yes" : "no", g_readyFrames,
			g_keep[kKeepCamera] ? "on" : "off", g_keep[kKeepEffects] ? "on" : "off",
			g_keep[kKeepRagdolls] ? "on" : "off",
			SkyboxLibrary_Current().c_str(),
			CloudFx_HasOverride() ? "yes" : "no", SunFx_HasOverride() ? "yes" : "no");
		return;
	}
	advancedfx::Message(
		"%s effects keep <group> on|off|toggle - group = camera|effects|ragdolls. On (default) the\n"
		"                                tab follows you into the next demo; off, the next demo\n"
		"                                starts from that tab's defaults. The settings FILE is\n"
		"                                held while a keep-off reset runs, so nothing is lost.\n"
		"%s effects on|off|toggle      - the whole carry-over module. Off = it does nothing at all\n"
		"                                on a level change; use it to rule this module out.\n"
		"%s effects reapply            - re-assert the current settings onto this level now.\n"
		"%s effects status             - what it thinks the world and the overrides are.\n"
		"Current: module = %s, keep: camera=%s effects=%s ragdolls=%s.\n",
		cmd, cmd, cmd, cmd, g_enabled ? "on" : "off",
		g_keep[kKeepCamera] ? "on" : "off", g_keep[kKeepEffects] ? "on" : "off",
		g_keep[kKeepRagdolls] ? "on" : "off");
}

} // namespace Filmmaker
