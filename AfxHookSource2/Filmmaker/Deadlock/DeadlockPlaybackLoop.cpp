#include "DeadlockPlaybackLoop.h"

#include "DeadlockRender.h"
#include "DeadlockShot.h"
#include "../Demo/DemoProjectStore.h"
#include "../Platform/JsonParser.h"
#include "../Platform/TextEncoding.h"
#include "../../Deadlock/DeadlockEngine.h"
#include "../../../Shared/AfxConsole.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>

namespace Filmmaker {

namespace {

bool g_enabled = false;
int g_inTick = -1;
int g_outTick = -1;

std::wstring g_path;   // the replay the region above belongs to
bool g_loaded = false; // the project.mvm section has been read for g_path

// Seek debounce. A seek replays packets over several frames and the current tick still reads at or
// past Out throughout; without this the first wrap issues a seek every frame until it lands.
bool g_seeking = false;
unsigned long long g_seekDeadlineMs = 0;

// How far past In the tick must fall before the seek is believed to have landed. A seek does not
// settle exactly on the requested tick, and the engine can report a tick a little ahead of it while
// the replay finishes.
const int kSeekLandedSlackTicks = 64;
const unsigned long long kSeekTimeoutMs = 15000;

// A loop shorter than this is almost certainly both flags on one frame, and looping it would seek
// every few frames forever.
const int kMinLoopTicks = 8;

// The region length used when the loop is armed with nothing marked. About one action beat: long
// enough to read, short enough to come round again while you are still looking at the thing you
// turned it on for.
const int kDefaultLoopSeconds = 5;

void DemoCmd(const char* c) { Deadlock::Engine_ExecuteClientCmd(c); }

int DefaultLoopTicks() {
	const double interval = Deadlock::Engine_IntervalPerTick();
	const int perSecond = (interval > 0.0001) ? (int)(1.0 / interval + 0.5) : 64;
	return kDefaultLoopSeconds * (perSecond > 0 ? perSecond : 64);
}

std::wstring PlayingDemoPathWide() {
	const std::string demo = Deadlock::Engine_DemoFilePath();
	if (demo.empty()) return {};
	return Utf8ToWide(demo);
}

void Save() {
	const std::wstring demo = PlayingDemoPathWide();
	if (demo.empty()) return;
	// Nothing worth remembering: remove the section rather than leave an empty one, so a cleared
	// loop does not come back next session as "off, with stale markers".
	if (g_inTick < 0 && g_outTick < 0) {
		DemoProjectStore_WriteSection(ProjectGame::Deadlock, demo, ProjectSection::Loop, "");
		return;
	}
	std::ostringstream out;
	out << "{\"version\":1,\"enabled\":" << (g_enabled ? "true" : "false")
	    << ",\"in\":" << g_inTick
	    << ",\"out\":" << g_outTick << "}\n";
	DemoProjectStore_WriteSection(ProjectGame::Deadlock, demo, ProjectSection::Loop, out.str());
}

void Load() {
	g_enabled = false;
	g_inTick = -1;
	g_outTick = -1;
	const std::wstring demo = PlayingDemoPathWide();
	if (demo.empty()) return;
	std::string text;
	if (!DemoProjectStore_ReadSection(ProjectGame::Deadlock, demo, ProjectSection::Loop, text))
		return;
	JsonValue root;
	if (!JsonParse(text, root) || root.type != JsonValue::Type::Object) return;
	if (const JsonValue* v = root.Find("in")) g_inTick = v->AsInt(-1);
	if (const JsonValue* v = root.Find("out")) g_outTick = v->AsInt(-1);
	if (const JsonValue* v = root.Find("enabled")) g_enabled = v->AsBool(false);
}

// Re-point at whatever replay is playing, loading its project.mvm once. False when there is
// nothing to work with.
bool SyncDemo() {
	const std::wstring path = PlayingDemoPathWide();
	if (path.empty()) return false;
	if (path != g_path || !g_loaded) {
		g_path = path;
		g_loaded = true;
		g_seeking = false;
		Load();
	}
	return true;
}

bool RangeUsable() {
	return g_inTick >= 0 && g_outTick > g_inTick && (g_outTick - g_inTick) >= kMinLoopTicks;
}

int CurrentTickOr(int fallback) {
	const int tick = Deadlock::Engine_DemoTick();
	return tick < 0 ? fallback : tick;
}

void SeekTo(int tick) {
	g_seeking = true;
	g_seekDeadlineMs = GetTickCount64() + kSeekTimeoutMs;
	char cmd[64];
	std::snprintf(cmd, sizeof(cmd), "demo_gototick %d", tick);
	DemoCmd(cmd);
}

// Arming a saved loop, or moving an armed one, should never leave the playhead stranded outside the
// region. Start from In so playback immediately demonstrates what was just armed. Do not fight a
// playing shot: it owns its own playhead.
void SeekToLoopStartIfOutside() {
	if (!g_enabled || !RangeUsable()) return;
	if (Shot_Playing()) return;
	const int tick = CurrentTickOr(-1);
	if (tick < 0 || (tick >= g_inTick && tick < g_outTick)) return;
	SeekTo(g_inTick);
}

// Arming with nothing marked used to produce a region that could never run: the button looked dead,
// nothing wrapped, no flags appeared. Turning it on now MEANS something -- there is always a region
// to loop and something to drag.
//
// The shot's own bounds are the better default when there is one: if you are looking at a camera
// move, the camera move is what you want to see again. Otherwise it starts at the playhead, which is
// where you were already looking.
void SeedDefaultRegion() {
	const int count = Shot_MarkerCount();
	const int first = Shot_FirstTick();
	const int last = Shot_LastTick();
	if (count >= 2 && first >= 0 && last >= first + kMinLoopTicks) {
		g_inTick = first;
		g_outTick = last;
		return;
	}
	int tick = CurrentTickOr(-1);
	if (tick < 0) tick = 0;
	g_inTick = tick;
	g_outTick = tick + DefaultLoopTicks();
}

// A 0..1 position along the visible strip, as the draggable flags send it, turned into a tick.
int FractionToTick(double fraction) {
	const int first = Shot_MarkerCount() > 0 ? Shot_FirstTick() : 0;
	int last = Shot_MarkerCount() > 0 ? Shot_LastTick() : 0;
	if (last <= first) {
		const int tick = CurrentTickOr(0);
		last = first + (tick > first ? tick - first : DefaultLoopTicks());
	}
	if (fraction < 0) fraction = 0;
	if (fraction > 1) fraction = 1;
	return first + (int)(fraction * (last - first) + 0.5);
}

void ReportRange() {
	if (g_inTick < 0 && g_outTick < 0) {
		advancedfx::Message("mirv_filmmaker: loop region not set.\n");
		return;
	}
	if (!RangeUsable()) {
		advancedfx::Message(
			"mirv_filmmaker: loop region incomplete (in=%d out=%d) -- set both, at least %d ticks apart.\n",
			g_inTick, g_outTick, kMinLoopTicks);
		return;
	}
	const int ticks = g_outTick - g_inTick;
	double interval = Deadlock::Engine_IntervalPerTick();
	if (!(interval > 0.00001)) interval = 1.0 / 64.0;
	// Where the playhead is relative to the region. "Is it actually looping me?" is the question
	// this command gets opened to answer, and without this the only way to answer it is to watch.
	const int tick = CurrentTickOr(-1);
	const char* where = (tick < 0) ? ""
		: (tick < g_inTick) ? " [before]"
		: (tick < g_outTick) ? " [inside]" : " [past]";
	advancedfx::Message(
		"mirv_filmmaker: loop %s -- in %d, out %d (%d ticks, %.2fs); tick %d%s.\n",
		g_enabled ? "ON" : "off", g_inTick, g_outTick, ticks, ticks * interval, tick, where);
}

} // namespace

void PlaybackLoop_SetEnabled(bool enabled) {
	if (enabled && !RangeUsable()) SeedDefaultRegion();
	g_enabled = enabled;
	g_seeking = false;
	if (enabled) SeekToLoopStartIfOutside();
	Save();
}

bool PlaybackLoop_Enabled() { return g_enabled; }
bool PlaybackLoop_Ready() { return RangeUsable(); }

void PlaybackLoop_SetIn(int tick) {
	if (tick < 0) tick = 0;
	g_inTick = tick;
	// Setting In past Out would leave a region that can never close. Drop the stale Out rather than
	// silently swapping them: the user just said where the region starts, and inventing an end for
	// them is how you get a loop nobody asked for.
	if (g_outTick >= 0 && g_outTick <= g_inTick) g_outTick = -1;
	g_seeking = false;
	Save();
}

void PlaybackLoop_SetOut(int tick) {
	if (tick < 0) tick = 0;
	g_outTick = tick;
	if (g_inTick >= 0 && g_inTick >= g_outTick) g_inTick = -1;
	g_seeking = false;
	Save();
}

void PlaybackLoop_SetRange(int inTick, int outTick) {
	if (inTick > outTick) std::swap(inTick, outTick);
	if (inTick < 0) inTick = 0;
	if (outTick < inTick + kMinLoopTicks) outTick = inTick + kMinLoopTicks;
	g_inTick = inTick;
	g_outTick = outTick;
	g_seeking = false;
	SeekToLoopStartIfOutside();
	Save();
}

void PlaybackLoop_Clear() {
	g_inTick = -1;
	g_outTick = -1;
	g_enabled = false;
	g_seeking = false;
	Save();
}

bool PlaybackLoop_Range(int& inTick, int& outTick) {
	inTick = g_inTick;
	outTick = g_outTick;
	return RangeUsable();
}

void PlaybackLoop_AppendStateJson(std::string& out) {
	char buf[160];
	std::snprintf(buf, sizeof(buf),
		",\"loopOn\":%s,\"loopIn\":%d,\"loopOut\":%d,\"loopReady\":%s",
		g_enabled ? "true" : "false", g_inTick, g_outTick,
		RangeUsable() ? "true" : "false");
	out += buf;
}

void PlaybackLoop_RunFrame() {
	const bool playing = Deadlock::Engine_IsPlayingDemo();
	const bool paused = Deadlock::Engine_IsDemoPaused();
	if (!playing && !paused) {
		// IsPlayingDemo also blips false transiently DURING the seek just issued. Dropping state
		// here would re-read the still-past-Out tick next frame and seek again.
		if (g_seeking && GetTickCount64() <= g_seekDeadlineMs) return;
		g_loaded = false;
		g_seeking = false;
		return;
	}

	if (!SyncDemo()) return;
	if (!g_enabled || !RangeUsable()) return;

	// A PLAYING shot owns the playhead. Its markers already define the range and it restarts itself;
	// seeking underneath it here would tear the camera move apart mid-shot.
	if (Shot_Playing()) return;

	const int tick = Deadlock::Engine_DemoTick();
	if (tick < 0) return;

	if (g_seeking) {
		if (tick < g_outTick - kSeekLandedSlackTicks || GetTickCount64() > g_seekDeadlineMs)
			g_seeking = false;
		else
			return;
	}

	// Paused is left alone on purpose: parking the playhead past Out to look at something is a
	// normal thing to do, and a loop that yanked you back every frame would make it impossible.
	if (paused) return;

	if (tick < g_outTick) return;

	SeekTo(g_inTick);
}

void PlaybackLoop_Report() {
	advancedfx::Message(
		"  loop:      %s  in=%d out=%d  %s\n",
		g_enabled ? "ON" : "off", g_inTick, g_outTick,
		RangeUsable() ? "region usable" : "region unusable (both ends, 8+ ticks apart)");
}

// ============================== command ========================================================

void PlaybackLoop_RunCommand(int argc, advancedfx::ICommandArgs* args) {
	const char* action = (argc >= 3) ? args->ArgV(2) : "";
	const char* value = (argc >= 4) ? args->ArgV(3) : "";

	if (0 == _stricmp(action, "on") || 0 == _stricmp(action, "off")
		|| 0 == _stricmp(action, "toggle")) {
		const bool on = (0 == _stricmp(action, "toggle")) ? !g_enabled
		                                                  : (0 == _stricmp(action, "on"));
		PlaybackLoop_SetEnabled(on);
		ReportRange();
		return;
	}

	// No tick argument means the playhead. That is the common case: you are watching, you see the
	// start of the bit you care about, you set In there.
	if (0 == _stricmp(action, "in") || 0 == _stricmp(action, "out")) {
		const bool isIn = (0 == _stricmp(action, "in"));
		const int tick = *value ? std::atoi(value) : CurrentTickOr(-1);
		if (tick < 0) {
			advancedfx::Warning("mirv_filmmaker: no replay tick available to set loop %s from.\n",
			                    isIn ? "in" : "out");
			return;
		}
		if (isIn) PlaybackLoop_SetIn(tick); else PlaybackLoop_SetOut(tick);
		ReportRange();
		return;
	}

	// The draggable flags speak in strip fractions, because that is what a Slider gives them. They
	// are a separate verb from `in`/`out` so a drag cannot be confused with a deliberate tick.
	if (0 == _stricmp(action, "inat") || 0 == _stricmp(action, "outat")) {
		if (!*value) return;
		const int tick = FractionToTick(std::atof(value));
		if (0 == _stricmp(action, "inat")) PlaybackLoop_SetRange(tick, g_outTick >= 0 ? g_outTick : tick + kMinLoopTicks);
		else PlaybackLoop_SetRange(g_inTick >= 0 ? g_inTick : tick - kMinLoopTicks, tick);
		return;
	}

	if (0 == _stricmp(action, "range")) {
		if (argc < 5) {
			advancedfx::Warning("usage: mirv_filmmaker loop range <inTick> <outTick>\n");
			return;
		}
		PlaybackLoop_SetRange(std::atoi(value), std::atoi(args->ArgV(4)));
		ReportRange();
		return;
	}

	if (0 == _stricmp(action, "clear")) {
		PlaybackLoop_Clear();
		advancedfx::Message("mirv_filmmaker: loop region cleared.\n");
		return;
	}

	if (0 == _stricmp(action, "status")) {
		ReportRange();
		return;
	}

	// Explicit hand-off between the loop and the render range. These exist BECAUSE the two are
	// separate: copying is a thing you ask for, never something a loop nudge does behind your back.
	if (0 == _stricmp(action, "torender")) {
		if (!RangeUsable()) {
			advancedfx::Warning("mirv_filmmaker: no usable loop region to copy.\n");
			return;
		}
		Render_SetRange(g_inTick, g_outTick);
		advancedfx::Message("mirv_filmmaker: render range set from the loop (%d..%d).\n",
		                    g_inTick, g_outTick);
		return;
	}

	if (0 == _stricmp(action, "fromrender")) {
		const RenderSettings r = Render_GetSettings();
		if (r.endTick <= r.startTick) {
			advancedfx::Warning("mirv_filmmaker: the render range is not usable.\n");
			return;
		}
		PlaybackLoop_SetRange(r.startTick, r.endTick);
		ReportRange();
		return;
	}

	advancedfx::Message(
		"mirv_filmmaker loop on|off|toggle  - loop playback over the region below\n"
		"mirv_filmmaker loop in [<tick>]    - region start; no tick = the current playhead\n"
		"mirv_filmmaker loop out [<tick>]   - region end; no tick = the current playhead\n"
		"mirv_filmmaker loop range <a> <b>  - both ends at once\n"
		"mirv_filmmaker loop clear          - forget the region and switch the loop off\n"
		"mirv_filmmaker loop status         - the region, and whether it can run\n"
		"mirv_filmmaker loop torender       - copy the loop into the RENDER range (explicit)\n"
		"mirv_filmmaker loop fromrender     - copy the render range into the loop\n");
}

} // namespace Filmmaker
