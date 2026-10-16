#include "Onboarding.h"
#include "PracticeSession.h"

#include "../Debug/MvmDebugLog.h"
#include "../Movie/DemoScrub.h" // seek: land paused on a lesson's moment
#include "../Movie/CameraPath.h" // placekey; a playing path takes every click
#include "../Movie/MovieMode.h"  // pov: first person for the weapon lessons
#include "../../WrpConsole.h"
#include "../../../Shared/AfxConsole.h"

#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>
#include <cstring>

namespace Filmmaker {

namespace {

OnboardingState g_state;

// The director hotkeys linear mode governs, by the names the panel uses in `lock on <names>`.
struct LockKey { const char* name; int vkey; };
const LockKey kLockKeys[] = {
	{ "SPACE", 0x20 }, { "LEFT", 0x25 }, { "RIGHT", 0x27 }, { "J", 0x4A }, { "K", 0x4B },
	{ "L", 0x4C }, { "F", 0x46 }, { "T", 0x54 }, { "ESC", 0x1B }, { "F8", 0x77 },
	{ "DELETE", 0x2E }, { "Z", 0x5A },
};
constexpr int kLockKeyCount = (int)(sizeof(kLockKeys) / sizeof(kLockKeys[0]));
bool g_allowed[kLockKeyCount] = {};

// Where clicks may land while a step is locked, in client fractions (x0 y0 x1 y1), sent by the
// panel as `clicks only ...`. `clicks free` (or no zones sent) leaves the mouse alone.
struct ClickZone { float x0, y0, x1, y1; };
bool g_clicksGated = false;
std::vector<ClickZone> g_clickZones;
std::vector<ClickZone> g_asstZones; // where Clippy is on screen (`asst ...`), for every step
std::mutex g_clickMutex; // the window procedure reads the zones; console commands write them

// The routes the panel script has lessons for (Panorama/OnboardingRoutesJs.h). Anything else is
// rejected rather than shown as an empty bubble. cs2 / app are the two opening paths;
// `more` is the deeper-rundown menu and the rest are its topics.
const char* const kRoutes[] = { "cs2", "app", "more", "render", "keys", "graph", "path", "recoil", "fov", "config" };
// Steps per route plus the finale. The panel script owns the words; this only bounds the input.
constexpr int kMaxStep = 99; // a sanity bound; the panel knows each route's real length

const char* CanonicalRoute(const char* name) {
	if (!name) return nullptr;
	for (const char* r : kRoutes)
		if (0 == _stricmp(name, r)) return r;
	return nullptr;
}

void Changed(const char* why) {
	++g_state.rev;
	// The tutorial runs as a practice session: defaults in, nothing saved, everything read back
	// when it is switched off (PracticeSession.h). The close question keeps it running.
	if (g_state.on) PracticeSession_Begin();
	else PracticeSession_End();
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("onboarding", "%s: on=%d closed=%d route=%s step=%d voice=%d rev=%u",
			why, g_state.on ? 1 : 0, g_state.closed ? 1 : 0, g_state.route[0] ? g_state.route : "-",
			g_state.step, g_state.voice ? 1 : 0, g_state.rev);
}

void PrintStatus() {
	advancedfx::Message("onboarding: %s, route=%s step=%d chatter=%s\n",
		!g_state.on ? "off" : (g_state.closed ? "paused" : "open"),
		g_state.route[0] ? g_state.route : "(opening choices)", g_state.step, g_state.voice ? "on" : "off");
}

void PrintUsage(const char* cmd) {
	advancedfx::Message(
		"%s                  - open Clippy's tutorial (resumes where you left it)\n"
		"%s restart          - back to the opening choices\n"
		"%s close            - ask whether to leave (keep going, stop here, start over)\n"
		"%s resume           - keep going: back to the same step\n"
		"%s off              - hide everything\n"
		"%s chatter on|off   - chatter sound while Clippy talks\n"
		"%s go <route> <step> - jump to a lesson step (cs2, app, more, render, keys, graph, path, recoil, fov, config)\n"
		"%s pos reset        - put him back in his own spot after a drag\n"
		"%s status\n",
		cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
}

} // namespace

const OnboardingState& Onboarding_State() { return g_state; }

bool Onboarding_HoldsDemo() {
	if (!g_state.on || g_state.closed || !g_state.locked || g_state.playToTick >= 0) return false;
	for (int i = 0; i < kLockKeyCount; ++i)
		if ((kLockKeys[i].vkey == 0x20 || kLockKeys[i].vkey == 0x4A) && g_allowed[i]) return false;
	return true;
}

void Onboarding_PlayToDone() { g_state.playToTick = -1; }

void Onboarding_SeekIssued() { g_state.seekTick = -1; }

bool Onboarding_ClickOnAssistant(int clientX, int clientY, int clientW, int clientH) {
	if (!g_state.on || clientW <= 0 || clientH <= 0) return false;
	std::lock_guard<std::mutex> lock(g_clickMutex);
	const float fx = (float)clientX / clientW, fy = (float)clientY / clientH;
	for (const ClickZone& z : g_asstZones)
		if (fx >= z.x0 && fx <= z.x1 && fy >= z.y0 && fy <= z.y1) return true;
	return false;
}

bool Onboarding_BlocksClick(int clientX, int clientY, int clientW, int clientH) {
	if (!g_state.on || g_state.closed || !g_state.locked || !g_clicksGated) return false;
	// A camera path playing: a click anywhere is how the user gets the mouse back, and the
	// director swallows it before anything else sees it (MovieMode::OnUiClick).
	if (CameraPathRef().GetMode() == CameraPath::Mode::PreviewPlaying) return false;
	if (clientW <= 0 || clientH <= 0) return false;
	std::lock_guard<std::mutex> lock(g_clickMutex);
	const float fx = (float)clientX / clientW, fy = (float)clientY / clientH;
	for (const ClickZone& z : g_clickZones)
		if (fx >= z.x0 && fx <= z.x1 && fy >= z.y0 && fy <= z.y1) return false;
	return true;
}

bool Onboarding_BlocksKey(int vkey) {
	if (!g_state.on || g_state.closed || !g_state.locked) return false;
	for (int i = 0; i < kLockKeyCount; ++i)
		if (kLockKeys[i].vkey == vkey) return !g_allowed[i];
	return false;
}

void Onboarding_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd, int firstArg) {
	const char* sub = argc > firstArg ? args->ArgV(firstArg) : "open";
	const char* a1 = argc > firstArg + 1 ? args->ArgV(firstArg + 1) : nullptr;
	const char* a2 = argc > firstArg + 2 ? args->ArgV(firstArg + 2) : nullptr;

	if (0 == _stricmp(sub, "open") || 0 == _stricmp(sub, "on") || 0 == _stricmp(sub, "show")) {
		g_state.on = true;
		g_state.closed = false;
		Changed("open");
		PrintStatus();
		return;
	}
	if (0 == _stricmp(sub, "restart")) {
		g_state.on = true;
		g_state.closed = false;
		g_state.route = "";
		g_state.step = 0;
		Changed("restart");
		return;
	}
	if (0 == _stricmp(sub, "close") || 0 == _stricmp(sub, "exit")) {
		if (!g_state.on) return;
		g_state.closed = true;
		Changed("close");
		return;
	}
	if (0 == _stricmp(sub, "resume")) {
		g_state.on = true;
		g_state.closed = false;
		Changed("resume");
		return;
	}
	if (0 == _stricmp(sub, "off") || 0 == _stricmp(sub, "hide")) {
		g_state.on = false;
		g_state.closed = false;
		g_state.locked = false;
		g_state.playToTick = -1;
		g_state.seekTick = -1;
		{ std::lock_guard<std::mutex> lock(g_clickMutex); g_clicksGated = false; g_clickZones.clear(); g_asstZones.clear(); }
		Changed("off");
		return;
	}
	if (0 == _stricmp(sub, "chatter") || 0 == _stricmp(sub, "voice")) {
		if (!a1 || 0 == _stricmp(a1, "toggle")) g_state.voice = !g_state.voice;
		else g_state.voice = (0 == _stricmp(a1, "on") || 0 == strcmp(a1, "1"));
		// No rev bump: turning chatter on must not restart the line he is in the middle of.
		if (MvmDebugLog_Active()) MvmDebugLog_LinefAlways("onboarding", "chatter=%d", g_state.voice ? 1 : 0);
		return;
	}
	if (0 == _stricmp(sub, "go")) {
		const char* route = CanonicalRoute(a1);
		const int step = a2 ? atoi(a2) : 0;
		if (!route || step < 0 || step > kMaxStep) {
			advancedfx::Warning("usage: %s go <cs2|app|more|render|keys|graph|path|recoil|fov|config> <step 0..%d>\n", cmd, kMaxStep);
			return;
		}
		g_state.on = true;
		g_state.closed = false;
		g_state.route = route;
		g_state.step = step;
		Changed("go");
		return;
	}
	if (0 == _stricmp(sub, "pos")) {
		// The panel sends this when a drag ends. No rev bump: moving him is not a new line.
		if (a1 && 0 == _stricmp(a1, "reset")) {
			g_state.posX = g_state.posY = -1.0;
		} else {
			const double x = a1 ? atof(a1) : -1.0, y = a2 ? atof(a2) : -1.0;
			if (!(x >= 0.0 && x <= 1.0 && y >= 0.0 && y <= 1.0)) {
				advancedfx::Warning("usage: %s pos <x 0..1> <y 0..1> | pos reset\n", cmd);
				return;
			}
			g_state.posX = x; g_state.posY = y;
		}
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("onboarding", "pos=%.4f,%.4f", g_state.posX, g_state.posY);
		return;
	}
	if (0 == _stricmp(sub, "lock")) {
		// `lock off`, or `lock on [KEY ...]` with the hotkeys the step allows (kLockKeys names).
		// Sent by the panel on every step; not a new line, so no rev bump.
		const bool on = a1 && 0 == _stricmp(a1, "on");
		g_state.locked = on;
		for (int i = 0; i < kLockKeyCount; ++i) g_allowed[i] = false;
		for (int arg = firstArg + 2; on && arg < argc; ++arg)
			for (int i = 0; i < kLockKeyCount; ++i)
				if (0 == _stricmp(args->ArgV(arg), kLockKeys[i].name)) g_allowed[i] = true;
		if (MvmDebugLog_Active()) {
			std::string keys;
			for (int i = 0; i < kLockKeyCount; ++i) if (g_allowed[i]) { keys += ' '; keys += kLockKeys[i].name; }
			MvmDebugLog_LinefAlways("onboarding", "lock %s%s", on ? "on" : "off", keys.c_str());
		}
		return;
	}
	if (0 == _stricmp(sub, "seek")) {
		// A lesson jumps the practice demo to its moment and leaves it PAUSED there: the user
		// presses Space when the step says so. DemoScrub lands exactly on the tick and handles
		// the engine's quirks (a forward demo_gototick is ignored while paused; demo_pause
		// toggles). The panel driver sends it once the demo is running: sent while a demo is
		// still loading, it landed and then the engine's own start of playback rolled it on
		// (measured: 5960 -> 5977). No rev bump: the step does not change.
		const int tick = a1 ? atoi(a1) : -1;
		if (tick < 0) { advancedfx::Warning("usage: %s seek <tick>\n", cmd); return; }
		g_state.seekTick = tick;
		++g_state.seekSerial;
		if (MvmDebugLog_Active()) MvmDebugLog_LinefAlways("onboarding", "seek %d queued", tick);
		return;
	}
	if (0 == _stricmp(sub, "asst")) {
		// asst none | asst <x0 y0 x1 y1>...: where Clippy is, in client fractions.
		std::lock_guard<std::mutex> lock(g_clickMutex);
		g_asstZones.clear();
		for (int arg = firstArg + 1; arg + 3 < argc; arg += 4) {
			ClickZone z;
			z.x0 = (float)atof(args->ArgV(arg)); z.y0 = (float)atof(args->ArgV(arg + 1));
			z.x1 = (float)atof(args->ArgV(arg + 2)); z.y1 = (float)atof(args->ArgV(arg + 3));
			g_asstZones.push_back(z);
		}
		return;
	}
	if (0 == _stricmp(sub, "clicks")) {
		// clicks free | clicks only <x0 y0 x1 y1>...: the zones a locked step lets clicks into.
		std::lock_guard<std::mutex> lock(g_clickMutex);
		g_clickZones.clear();
		g_clicksGated = a1 && 0 == _stricmp(a1, "only");
		for (int arg = firstArg + 2; g_clicksGated && arg + 3 < argc; arg += 4) {
			ClickZone z;
			z.x0 = (float)atof(args->ArgV(arg)); z.y0 = (float)atof(args->ArgV(arg + 1));
			z.x1 = (float)atof(args->ArgV(arg + 2)); z.y1 = (float)atof(args->ArgV(arg + 3));
			g_clickZones.push_back(z);
		}
		return;
	}
	if (0 == _stricmp(sub, "playto")) {
		// playto <tick>: the demo plays on to <tick> and pauses there (between two camera keys).
		// The panel driver resumes it, watches the tick and pauses it; the hold stands aside.
		const int tick = a1 ? atoi(a1) : -1;
		if (tick < 0) { advancedfx::Warning("usage: %s playto <tick>\n", cmd); return; }
		g_state.playToTick = tick;
		if (MvmDebugLog_Active()) MvmDebugLog_LinefAlways("onboarding", "playto %d", tick);
		return;
	}
	if (0 == _stricmp(sub, "placekey")) {
		// placekey <tick> <x> <y> <z> <pitch> <yaw>: what the user would have placed with K, for a
		// camera step they skipped, so the next step (playing the path) has keys to play.
		if (argc < firstArg + 7) { advancedfx::Warning("usage: %s placekey <tick> <x> <y> <z> <pitch> <yaw>\n", cmd); return; }
		double v[6];
		for (int i = 0; i < 6; ++i) v[i] = atof(args->ArgV(firstArg + 1 + i));
		CameraPathRef().PlaceMarkerAt((int)v[0], v[1], v[2], v[3], v[4], v[5], 90.0);
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("onboarding", "placekey tick=%d at %.0f,%.0f,%.0f", (int)v[0], v[1], v[2], v[3]);
		return;
	}
	if (0 == _stricmp(sub, "pov")) {
		// First person through the spectated player, the way the demo bar's camera button does it:
		// our own free cam lets go first (MovieMode::ApplyMode).
		MovieModeRef().SetMode(MovieMode::Mode::Default);
		return;
	}
	if (0 == _stricmp(sub, "status")) { PrintStatus(); return; }
	PrintUsage(cmd);
}

} // namespace Filmmaker

CON_COMMAND(mvm_onboarding, "Open Clippy's onboarding tutorial (prototype). mvm_onboarding [restart|close|resume|off|chatter on|off|status].") {
	Filmmaker::Onboarding_RunCommand(args->ArgC(), args, "mvm_onboarding", 1);
}
