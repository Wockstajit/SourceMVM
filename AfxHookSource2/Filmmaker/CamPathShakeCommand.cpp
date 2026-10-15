// Console surface for the per-keyframe GTA V camera shake: "mirv_filmmaker pathshake ...".
//
// The timeline's right-click keyframe menu drives these; they also work straight from the
// console. Its own file next to the dispatcher, per the *Command.cpp file-per-feature pattern.
//
// NOTE the name. "camshake" is already taken by CameraShakeStack (Handheld / Recoil / Weapon
// Recoil diagnostics), which is a different, non-authored layer that is GATED OFF during camera
// path playback. This one is "pathshake": authored, per keyframe, part of the shot.

#include "Movie/CamPathShake.h"
#include "Movie/CameraPath.h"
#include "Movie/RageShake.h"

#include "../../Shared/AfxConsole.h"

#include <cstdlib>
#include <cstring>

namespace Filmmaker {

namespace {

int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool ValidKey(int index) {
	return index >= 0 && index < CameraPathRef().Count();
}

// Reads the current setting, seeding a sane default when the keyframe has none yet -- so the
// menu's first "next style" click turns a bare keyframe into a working shake in one press.
CamPathShakeSetting GetOrSeed(int index) {
	CamPathShakeSetting cfg = CamPathShake_Get(index);
	if (cfg.style < 0) {
		// Seed from the MENU, not from style 0 of the raw table -- the raw table is alphabetical
		// and starts on ASSAULT_RIFLE_RECOIL_SHAKE, which is not a camera move.
		const int seed = RageShake_MenuStyle(0);
		cfg.style = (seed >= 0) ? seed : 0;
		cfg.intensity = 5;
		cfg.speed = 50;
	}
	return cfg;
}

void Report(int index, const CamPathShakeSetting& cfg) {
	if (!cfg.Active()) {
		advancedfx::Message("[pathshake] key #%d: no shake.\n", index + 1);
		return;
	}
	advancedfx::Message("[pathshake] key #%d: %s (%s)  intensity %d/10  speed %d/100.\n",
		index + 1, RageShake_StyleLabel(cfg.style), RageShake_StyleName(cfg.style),
		cfg.intensity, cfg.speed);
}

// "next"/"prev"/"+1"/"-1" or an absolute value. Returns false when `a` is not a step token.
bool StepToken(const char* a, int& outDelta) {
	if (0 == _stricmp(a, "next") || 0 == _stricmp(a, "up") || 0 == _stricmp(a, "+")) { outDelta = 1; return true; }
	if (0 == _stricmp(a, "prev") || 0 == _stricmp(a, "down") || 0 == _stricmp(a, "-")) { outDelta = -1; return true; }
	return false;
}

// Strictly digits. Everything numeric in this command goes through this before atoi, because
// atoi("HAND_SHAK")==0 -- a typo would otherwise silently become style #0 / intensity 1 instead
// of an error the user can see.
bool IsAllDigits(const char* a) {
	if (!a || !*a) return false;
	for (const char* p = a; *p; ++p)
		if (*p < '0' || *p > '9') return false;
	return true;
}

// Style argument: a name ("REPLAY_HAND_SHAKE"), or a numeric table index. -1 = unknown.
int ParseStyleArg(const char* a) {
	int idx = RageShake_FindStyle(a);
	if (idx >= 0) return idx;
	if (!IsAllDigits(a)) return -1;
	idx = atoi(a);
	return (idx >= 0 && idx < RageShake_StyleCount()) ? idx : -1;
}

} // namespace

void CamPathShake_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 3) {
		advancedfx::Message(
			"%s pathshake on|off|toggle - master switch for the per-keyframe shake layer.\n"
			"%s pathshake set <key> <style|index> [intensity 1-10] [speed 10-100].\n"
			"%s pathshake style <key> next|prev|<name>|<index>.\n"
			"%s pathshake intensity <key> next|prev|<1-10>.\n"
			"%s pathshake speed <key> next|prev|<10-100>.\n"
			"%s pathshake applyall <key> - copy this keyframe's shake onto every keyframe.\n"
			"%s pathshake clear <key>  |  pathshake clearall.\n"
			"%s pathshake styles [all] - list the camera shake styles.\n"
			"%s pathshake state - what is set on which keyframe.\n"
			"  <key> is the 0-based keyframe index (the timeline shows #1-based).\n",
			cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
		return;
	}

	const char* a = args->ArgV(2);
	const char* a3 = (argc >= 4) ? args->ArgV(3) : "";
	const char* a4 = (argc >= 5) ? args->ArgV(4) : "";
	const char* a5 = (argc >= 6) ? args->ArgV(5) : "";
	const char* a6 = (argc >= 7) ? args->ArgV(6) : "";

	if (0 == _stricmp(a, "on") || 0 == _stricmp(a, "off") || 0 == _stricmp(a, "toggle")) {
		const bool on = (0 == _stricmp(a, "on")) ? true
			: (0 == _stricmp(a, "off")) ? false : !CamPathShake_Enabled();
		CamPathShake_SetEnabled(on);
		advancedfx::Message("[pathshake] keyframe shake layer %s.\n", on ? "ON" : "OFF");
		return;
	}

	if (0 == _stricmp(a, "styles")) {
		const bool all = (0 == _stricmp(a3, "all"));
		const int menuN = RageShake_MenuCount();
		advancedfx::Message("[pathshake] %d camera shake styles (the timeline menu's list):\n", menuN);
		for (int i = 0; i < menuN; ++i) {
			const int si = RageShake_MenuStyle(i);
			advancedfx::Message("  %-20s %s\n", RageShake_StyleLabel(si), RageShake_StyleName(si));
		}
		if (!all) {
			advancedfx::Message(
				"  (%d more native styles exist -- weapon recoil, reloads, hit reactions. They are\n"
				"   settable by name but kept off the menu. `%s pathshake styles all` lists them.)\n",
				RageShake_StyleCount() - menuN, cmd);
			return;
		}
		advancedfx::Message("[pathshake] all %d shake styles:\n", RageShake_StyleCount());
		for (int i = 0; i < RageShake_StyleCount(); ++i) {
			advancedfx::Message("  %s%s\n", RageShake_StyleName(i),
				(RageShake_MenuIndexOfStyle(i) >= 0) ? "  (on menu)" : "");
		}
		return;
	}

	if (0 == _stricmp(a, "state")) {
		advancedfx::Message("[pathshake] layer %s, %d keyframe(s) carry a shake (of %d).\n",
			CamPathShake_Enabled() ? "ON" : "OFF", CamPathShake_Count(), CameraPathRef().Count());
		for (int i = 0; i < CameraPathRef().Count(); ++i) {
			const CamPathShakeSetting cfg = CamPathShake_Get(i);
			if (cfg.Active()) Report(i, cfg);
		}
		return;
	}

	if (0 == _stricmp(a, "clearall")) {
		const int had = CamPathShake_Count();
		CamPathShake_ClearAll();
		advancedfx::Message("[pathshake] removed the shake from %d keyframe(s).\n", had);
		return;
	}

	// Everything below takes a keyframe index.
	if (argc < 4) {
		advancedfx::Warning("usage: %s pathshake %s <key> ...\n", cmd, a);
		return;
	}
	if (!IsAllDigits(a3)) { // atoi("abc")==0 would silently target keyframe #0
		advancedfx::Warning("[pathshake] '%s' is not a keyframe index.\n", a3);
		return;
	}
	const int key = atoi(a3);
	if (!ValidKey(key)) {
		advancedfx::Warning("[pathshake] no keyframe #%d (path has %d).\n", key, CameraPathRef().Count());
		return;
	}

	if (0 == _stricmp(a, "clear")) {
		CamPathShake_Clear(key);
		advancedfx::Message("[pathshake] key #%d: shake removed.\n", key + 1);
		return;
	}

	if (0 == _stricmp(a, "applyall")) {
		const int n = CamPathShake_ApplyToAll(key);
		if (n <= 0) {
			advancedfx::Warning("[pathshake] key #%d has no shake to copy.\n", key + 1);
			return;
		}
		const CamPathShakeSetting cfg = CamPathShake_Get(key);
		advancedfx::Message("[pathshake] %s %d/10 %d/100 applied to %d keyframe(s).\n",
			RageShake_StyleLabel(cfg.style), cfg.intensity, cfg.speed, n);
		return;
	}

	if (0 == _stricmp(a, "set")) {
		if (argc < 5) { advancedfx::Warning("usage: %s pathshake set <key> <style> [intensity] [speed]\n", cmd); return; }
		CamPathShakeSetting cfg = GetOrSeed(key);
		const int idx = ParseStyleArg(a4);
		if (idx < 0) {
			advancedfx::Warning("[pathshake] unknown style '%s' (see: %s pathshake styles).\n", a4, cmd);
			return;
		}
		cfg.style = idx;
		if (argc >= 6) cfg.intensity = ClampInt(atoi(a5), 1, 10);
		if (argc >= 7) cfg.speed = ClampInt(atoi(a6), 10, 100);
		CamPathShake_Set(key, cfg);
		Report(key, CamPathShake_Get(key));
		return;
	}

	if (0 == _stricmp(a, "style")) {
		if (argc < 5) { advancedfx::Warning("usage: %s pathshake style <key> next|prev|<name>|<index>\n", cmd); return; }
		CamPathShakeSetting cfg = GetOrSeed(key);
		int delta = 0;
		if (StepToken(a4, delta)) {
			// The arrows walk the MENU order (Rockstar Editor five first, then the extras), not
			// the raw alphabetical table -- otherwise one press of ▶ from "Hand" wades through
			// forty weapon recoils. A style set from the console that is off the menu steps onto
			// the menu at position 0 rather than dead-ending.
			const int n = RageShake_MenuCount();
			if (n > 0) {
				int m = RageShake_MenuIndexOfStyle(cfg.style);
				m = (m < 0) ? 0 : (((m + delta) % n + n) % n);
				const int si = RageShake_MenuStyle(m);
				if (si >= 0) cfg.style = si;
			}
		} else {
			const int idx = ParseStyleArg(a4);
			if (idx < 0) {
				advancedfx::Warning("[pathshake] unknown style '%s' (see: %s pathshake styles).\n", a4, cmd);
				return;
			}
			cfg.style = idx;
		}
		CamPathShake_Set(key, cfg);
		Report(key, CamPathShake_Get(key));
		return;
	}

	if (0 == _stricmp(a, "intensity")) {
		if (argc < 5) { advancedfx::Warning("usage: %s pathshake intensity <key> next|prev|<1-10>\n", cmd); return; }
		CamPathShakeSetting cfg = GetOrSeed(key);
		int delta = 0;
		if (StepToken(a4, delta)) cfg.intensity = ClampInt(cfg.intensity + delta, 1, 10);
		else if (IsAllDigits(a4)) cfg.intensity = ClampInt(atoi(a4), 1, 10);
		else { advancedfx::Warning("usage: %s pathshake intensity <key> next|prev|<1-10>\n", cmd); return; }
		CamPathShake_Set(key, cfg);
		Report(key, CamPathShake_Get(key));
		return;
	}

	if (0 == _stricmp(a, "speed")) {
		if (argc < 5) { advancedfx::Warning("usage: %s pathshake speed <key> next|prev|<10-100>\n", cmd); return; }
		CamPathShakeSetting cfg = GetOrSeed(key);
		int delta = 0;
		// Steps of 10 so the 10..100 range is ten presses end to end, matching the intensity feel.
		if (StepToken(a4, delta)) cfg.speed = ClampInt(cfg.speed + delta * 10, 10, 100);
		else if (IsAllDigits(a4)) cfg.speed = ClampInt(atoi(a4), 10, 100);
		else { advancedfx::Warning("usage: %s pathshake speed <key> next|prev|<10-100>\n", cmd); return; }
		CamPathShake_Set(key, cfg);
		Report(key, CamPathShake_Get(key));
		return;
	}

	advancedfx::Warning("%s pathshake: unknown action '%s'\n", cmd, a);
}

} // namespace Filmmaker
