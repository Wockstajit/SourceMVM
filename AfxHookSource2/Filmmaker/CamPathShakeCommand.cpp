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
#include <string>
#include <vector>

namespace Filmmaker {

namespace {

int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// A camera key's tick by its 0-based index, -1 when there is no such key.
int TickForMarker(int index) {
	const std::vector<CamMarker>& mk = CameraPathRef().Markers();
	return index >= 0 && index < (int)mk.size() ? mk[(size_t)index].tick : -1;
}

// Reads the current setting, seeding a sane default when the keyframe has none yet -- so the
// menu's first "next style" click turns a bare keyframe into a working shake in one press.
CamPathShakeSetting GetOrSeed(int owner, int tick) {
	CamPathShakeSetting cfg = CamPathShake_GetAt(owner, tick);
	if (!cfg.Active()) {
		// Seed from the MENU, not from style 0 of the raw table -- the raw table is alphabetical
		// and starts on ASSAULT_RIFLE_RECOIL_SHAKE, which is not a camera move.
		const int seed = RageShake_MenuStyle(0);
		cfg.style = (seed >= 0) ? seed : 0;
		cfg.intensity = 5;
		cfg.speed = 50;
	}
	return cfg;
}

// Where a key sits, for messages: "key #3" on the path, "camera 7 @1500" on a camera object.
std::string Where(int owner, int tick) {
	if (!owner) {
		const std::vector<CamMarker>& mk = CameraPathRef().Markers();
		for (size_t i = 0; i < mk.size(); ++i) if (mk[i].tick == tick) return "key #" + std::to_string(i + 1);
		return "tick " + std::to_string(tick);
	}
	return "camera " + std::to_string(owner) + " @" + std::to_string(tick);
}

void Report(int owner, int tick, const CamPathShakeSetting& cfg) {
	const std::string where = Where(owner, tick);
	if (cfg.Stop()) { advancedfx::Message("[pathshake] %s: the shake stops here.\n", where.c_str()); return; }
	if (!cfg.Active()) { advancedfx::Message("[pathshake] %s: no shake key.\n", where.c_str()); return; }
	advancedfx::Message("[pathshake] %s: %s (%s)  intensity %d/10  speed %d/100.\n",
		where.c_str(), RageShake_StyleLabel(cfg.style), RageShake_StyleName(cfg.style),
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
	// `obj <id>` picks a camera object's shake track (scene-object-rigs.md); without it, the path's.
	int owner = 0, base = 0;
	if (argc >= 4 && 0 == _stricmp(args->ArgV(2), "obj")) {
		if (!IsAllDigits(args->ArgV(3)) || atoi(args->ArgV(3)) <= 0) {
			advancedfx::Warning("[pathshake] '%s' is not a camera object id.\n", args->ArgV(3));
			return;
		}
		owner = atoi(args->ArgV(3));
		base = 2;
	}
	if (argc < 3 + base) {
		advancedfx::Message(
			"%s pathshake on|off|toggle - master switch for the shake layer.\n"
			"%s pathshake [obj <id>] set <key> <style|index> [intensity 1-10] [speed 10-100].\n"
			"%s pathshake [obj <id>] style <key> next|prev|<name>|<index>.\n"
			"%s pathshake [obj <id>] intensity <key> next|prev|<1-10>.\n"
			"%s pathshake [obj <id>] speed <key> next|prev|<10-100>.\n"
			"%s pathshake [obj <id>] stop <key> - the shake running into this key ends there.\n"
			"%s pathshake applyall <key> - copy this camera key's shake onto every camera key.\n"
			"%s pathshake [obj <id>] clear <key>  |  pathshake [obj <id>] clearall.\n"
			"%s pathshake styles [all] - list the camera shake styles.\n"
			"%s pathshake state - what is set where.\n"
			"  <key> is @<tick>, or on the camera path its 0-based camera-key index (the timeline shows\n"
			"  #1-based). `obj <id>` works on camera object <id>, whose keys sit on any tick. A key runs\n"
			"  until the next key of its track (the same style ramps to it), or to the end of the shot.\n",
			cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
		return;
	}

	const char* a = args->ArgV(2 + base);
	const char* a3 = (argc >= 4 + base) ? args->ArgV(3 + base) : "";
	const char* a4 = (argc >= 5 + base) ? args->ArgV(4 + base) : "";
	const char* a5 = (argc >= 6 + base) ? args->ArgV(5 + base) : "";
	const char* a6 = (argc >= 7 + base) ? args->ArgV(6 + base) : "";
	const int n = argc - base; // the argument count as the path's form would have it

	if (!owner && (0 == _stricmp(a, "on") || 0 == _stricmp(a, "off") || 0 == _stricmp(a, "toggle"))) {
		const bool on = (0 == _stricmp(a, "on")) ? true
			: (0 == _stricmp(a, "off")) ? false : !CamPathShake_Enabled();
		CamPathShake_SetEnabled(on);
		advancedfx::Message("[pathshake] shake layer %s.\n", on ? "ON" : "OFF");
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
		advancedfx::Message("[pathshake] layer %s; the path has %d shake key(s) over %d camera key(s).\n",
			CamPathShake_Enabled() ? "ON" : "OFF", CamPathShake_Count(), CameraPathRef().Count());
		for (const CamMarker& m : CameraPathRef().Markers()) {
			const CamPathShakeSetting cfg = CamPathShake_GetAt(0, m.tick);
			if (cfg.IsKey()) Report(0, m.tick, cfg);
		}
		if (owner) advancedfx::Message("[pathshake] camera %d has %d shake key(s).\n", owner, CamPathShake_TrackKeys(owner));
		return;
	}

	if (0 == _stricmp(a, "clearall")) {
		const int had = CamPathShake_TrackKeys(owner);
		CamPathShake_ClearTrack(owner);
		const std::string whose = owner ? "camera " + std::to_string(owner) : std::string("the camera path");
		advancedfx::Message("[pathshake] removed %d shake key(s) from %s.\n", had, whose.c_str());
		return;
	}

	// Everything below takes a key: @<tick>, or on the path a camera-key index.
	if (n < 4) {
		advancedfx::Warning("usage: %s pathshake %s<verb> <key> ...\n", cmd, owner ? "obj <id> " : "");
		return;
	}
	int tick = -1;
	if (a3[0] == '@' && IsAllDigits(a3 + 1)) tick = atoi(a3 + 1);
	else if (IsAllDigits(a3)) tick = owner ? atoi(a3) : TickForMarker(atoi(a3)); // atoi("abc")==0 would be key #0
	if (tick < 0) {
		advancedfx::Warning("[pathshake] '%s' is not a key here (the path has %d camera keys).\n", a3, CameraPathRef().Count());
		return;
	}
	// The path's keys sit on its camera keys (CamPathShake_SetAt refuses any other tick).
	auto put = [&](const CamPathShakeSetting& cfg) {
		if (!CamPathShake_SetAt(owner, tick, cfg)) {
			advancedfx::Warning("[pathshake] no camera key at tick %d: a path shake key sits on a camera key.\n", tick);
			return;
		}
		Report(owner, tick, CamPathShake_GetAt(owner, tick));
	};

	if (0 == _stricmp(a, "clear")) {
		CamPathShake_SetAt(owner, tick, CamPathShakeSetting());
		advancedfx::Message("[pathshake] %s: shake key removed.\n", Where(owner, tick).c_str());
		return;
	}

	if (0 == _stricmp(a, "stop")) {
		CamPathShakeSetting stop;
		stop.style = kCamPathShakeStop;
		put(stop);
		return;
	}

	if (0 == _stricmp(a, "applyall")) {
		int index = -1;
		const std::vector<CamMarker>& mk = CameraPathRef().Markers();
		for (size_t i = 0; i < mk.size(); ++i) if (mk[i].tick == tick) index = (int)i;
		const int written = (owner || index < 0) ? 0 : CamPathShake_ApplyToAll(index);
		if (written <= 0) {
			advancedfx::Warning("[pathshake] %s has no shake to copy (applyall is the camera path's).\n", Where(owner, tick).c_str());
			return;
		}
		const CamPathShakeSetting cfg = CamPathShake_GetAt(0, tick);
		advancedfx::Message("[pathshake] %s %d/10 %d/100 applied to %d camera key(s).\n",
			RageShake_StyleLabel(cfg.style), cfg.intensity, cfg.speed, written);
		return;
	}

	if (0 == _stricmp(a, "set")) {
		if (n < 5) { advancedfx::Warning("usage: %s pathshake set <key> <style> [intensity] [speed]\n", cmd); return; }
		CamPathShakeSetting cfg = GetOrSeed(owner, tick);
		const int idx = ParseStyleArg(a4);
		if (idx < 0) {
			advancedfx::Warning("[pathshake] unknown style '%s' (see: %s pathshake styles).\n", a4, cmd);
			return;
		}
		cfg.style = idx;
		if (n >= 6) cfg.intensity = ClampInt(atoi(a5), 1, 10);
		if (n >= 7) cfg.speed = ClampInt(atoi(a6), 10, 100);
		put(cfg);
		return;
	}

	if (0 == _stricmp(a, "style")) {
		if (n < 5) { advancedfx::Warning("usage: %s pathshake style <key> next|prev|<name>|<index>\n", cmd); return; }
		CamPathShakeSetting cfg = GetOrSeed(owner, tick);
		int delta = 0;
		if (StepToken(a4, delta)) {
			// The arrows walk the MENU order (Rockstar Editor five first, then the extras), not
			// the raw alphabetical table -- otherwise one press of ▶ from "Hand" wades through
			// forty weapon recoils. A style set from the console that is off the menu steps onto
			// the menu at position 0 rather than dead-ending.
			const int count = RageShake_MenuCount();
			if (count > 0) {
				int m = RageShake_MenuIndexOfStyle(cfg.style);
				m = (m < 0) ? 0 : (((m + delta) % count + count) % count);
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
		put(cfg);
		return;
	}

	if (0 == _stricmp(a, "intensity")) {
		if (n < 5) { advancedfx::Warning("usage: %s pathshake intensity <key> next|prev|<1-10>\n", cmd); return; }
		CamPathShakeSetting cfg = GetOrSeed(owner, tick);
		int delta = 0;
		if (StepToken(a4, delta)) cfg.intensity = ClampInt(cfg.intensity + delta, 1, 10);
		else if (IsAllDigits(a4)) cfg.intensity = ClampInt(atoi(a4), 1, 10);
		else { advancedfx::Warning("usage: %s pathshake intensity <key> next|prev|<1-10>\n", cmd); return; }
		put(cfg);
		return;
	}

	if (0 == _stricmp(a, "speed")) {
		if (n < 5) { advancedfx::Warning("usage: %s pathshake speed <key> next|prev|<10-100>\n", cmd); return; }
		CamPathShakeSetting cfg = GetOrSeed(owner, tick);
		int delta = 0;
		// Steps of 10 so the 10..100 range is ten presses end to end, matching the intensity feel.
		if (StepToken(a4, delta)) cfg.speed = ClampInt(cfg.speed + delta * 10, 10, 100);
		else if (IsAllDigits(a4)) cfg.speed = ClampInt(atoi(a4), 10, 100);
		else { advancedfx::Warning("usage: %s pathshake speed <key> next|prev|<10-100>\n", cmd); return; }
		put(cfg);
		return;
	}

	advancedfx::Warning("%s pathshake: unknown action '%s'\n", cmd, a);
}

} // namespace Filmmaker
