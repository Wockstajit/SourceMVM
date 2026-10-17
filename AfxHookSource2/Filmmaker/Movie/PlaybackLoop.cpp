#include "PlaybackLoop.h"

#include "../Filmmaker.h"                 // PlayingDemoPath()
#include "../Demo/DemoProjectStore.h"
#include "../Platform/JsonParser.h"
#include "../Render/RenderCommand.h"      // Render_GetRange / Render_SetRange
#include "../Render/RenderRange.h"
#include "CameraPath.h"
#include "DemoScrub.h"
#include "../../MirvTime.h"

#include "../../../Shared/AfxConsole.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <Windows.h>
#include <cstdio>
#include <sstream>
#include <string>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

bool s_enabled = false;
int s_inTick = -1;
int s_outTick = -1;

std::wstring s_path;      // demo the region above belongs to
bool s_loaded = false;    // the project.mvm section has been read for s_path

// Seek debounce. demo_gototick replays packets over several frames; during that the current
// tick still reads at/past Out, so without this the first wrap would issue a seek every frame
// until it landed -- a seek storm on the single most expensive engine operation we have.
bool s_seeking = false;
unsigned long long s_seekDeadlineMs = 0;

// How far past In the tick must fall before we believe the seek landed. A seek does not settle
// exactly on the requested tick, and the engine can report a tick a little ahead of it while
// the replay finishes.
constexpr int kSeekLandedSlackTicks = 64;
constexpr unsigned long long kSeekTimeoutMs = 15000;

// A loop shorter than this is almost certainly a mis-click (both markers on the same frame),
// and looping it would seek every few frames forever.
constexpr int kMinLoopTicks = 8;

// Region length used when the loop is armed with nothing marked yet. Five seconds is about
// one action beat -- long enough to read, short enough to come round again while you are still
// looking at the thing you turned it on for.
constexpr int kDefaultLoopTicks = 5 * 64;

void DemoCmd(const char* c) {
	if (g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, c, true);
}

void Save() {
	if (s_path.empty()) return;
	// Nothing worth remembering: remove the section rather than leaving an empty one behind, so
	// a cleared loop does not resurrect itself as "off with stale markers" next session.
	if (s_inTick < 0 && s_outTick < 0) {
		DemoProjectStore_WriteSection(ProjectGame::Cs2, s_path, ProjectSection::Loop, "");
		return;
	}
	std::ostringstream out;
	out << "{\"version\":1,\"enabled\":" << (s_enabled ? "true" : "false")
		<< ",\"in\":" << s_inTick
		<< ",\"out\":" << s_outTick << "}\n";
	DemoProjectStore_WriteSection(ProjectGame::Cs2, s_path, ProjectSection::Loop, out.str());
}

void Load() {
	s_enabled = false;
	s_inTick = -1;
	s_outTick = -1;
	if (s_path.empty()) return;
	std::string text;
	if (!DemoProjectStore_ReadSection(ProjectGame::Cs2, s_path, ProjectSection::Loop, text))
		return;
	JsonValue root;
	if (!JsonParse(text, root) || root.type != JsonValue::Type::Object) return;
	if (const JsonValue* v = root.Find("in")) s_inTick = v->AsInt(-1);
	if (const JsonValue* v = root.Find("out")) s_outTick = v->AsInt(-1);
	if (const JsonValue* v = root.Find("enabled")) s_enabled = v->AsBool(false);
}

// Re-point at whatever demo is playing, loading its sidecar once. Returns false when there is
// no demo to work with.
bool SyncDemo() {
	std::wstring path = PlayingDemoPath();
	if (path.empty()) return false;
	if (path != s_path || !s_loaded) {
		s_path = path;
		s_loaded = true;
		s_seeking = false;
		Load();
	}
	return true;
}

bool RangeUsable() {
	return s_inTick >= 0 && s_outTick > s_inTick && (s_outTick - s_inTick) >= kMinLoopTicks;
}

int CurrentTickOr(int fallback) {
	int tick = fallback;
	g_MirvTime.GetCurrentDemoTick(tick);
	return tick;
}

// Enabling a saved loop, or moving an enabled loop, should never leave the playhead stranded
// outside the region. Start from Loop In so playback immediately demonstrates the range the user
// just armed. Do not fight an actively playing camera path; it owns its own loop restart.
void SeekToLoopStartIfOutside() {
	if (!s_enabled || !RangeUsable()) return;
	if (CameraPathRef().GetMode() == CameraPath::Mode::PreviewPlaying) return;
	const int tick = CurrentTickOr(-1);
	if (tick < 0 || (tick >= s_inTick && tick < s_outTick)) return;
	s_seeking = true;
	s_seekDeadlineMs = GetTickCount64() + kSeekTimeoutMs;
	char cmd[64];
	sprintf_s(cmd, "demo_gototick %d", s_inTick);
	DemoCmd(cmd);
}

} // namespace

// Arming the loop with nothing marked used to produce a region that could never run: the
// button looked dead, nothing wrapped, no flags appeared, and the only clue was a status line
// reading "(incomplete)". Turning it on now MEANS something -- there is always a region to
// loop and, in the UI, something to drag.
//
// The camera path's own bounds are the better default when there is one: if you are looking at
// a shot, the shot is what you want to see again. Otherwise it starts at the playhead, which
// is where you were looking.
void SeedDefaultRegion() {
	const CameraPath& path = CameraPathRef();
	const int first = path.FirstPathTick();
	const int last = path.LastPathTick();
	if (path.Count() >= 2 && first >= 0 && last >= first + kMinLoopTicks) {
		s_inTick = first;
		s_outTick = last;
		return;
	}
	int tick = CurrentTickOr(-1);
	if (tick < 0) tick = 0;
	s_inTick = tick;
	s_outTick = tick + kDefaultLoopTicks;
}

void PlaybackLoop_SetEnabled(bool enabled) {
	if (enabled && !RangeUsable()) SeedDefaultRegion();
	s_enabled = enabled;
	s_seeking = false;
	if (enabled) SeekToLoopStartIfOutside();
	Save();
}

bool PlaybackLoop_Enabled() { return s_enabled; }

void PlaybackLoop_SetIn(int tick) {
	if (tick < 0) tick = 0;
	s_inTick = tick;
	// Setting In past Out would leave a region that can never close. Drop the stale Out instead
	// of silently swapping them: the user just told us where the region starts, and inventing
	// an end for them is how you get a loop nobody asked for.
	if (s_outTick >= 0 && s_outTick <= s_inTick) s_outTick = -1;
	s_seeking = false;
	Save();
}

void PlaybackLoop_SetOut(int tick) {
	if (tick < 0) tick = 0;
	s_outTick = tick;
	if (s_inTick >= 0 && s_inTick >= s_outTick) s_inTick = -1;
	s_seeking = false;
	Save();
}

void PlaybackLoop_SetRange(int inTick, int outTick) {
	if (inTick > outTick) std::swap(inTick, outTick);
	if (inTick < 0) inTick = 0;
	if (outTick < inTick + kMinLoopTicks) outTick = inTick + kMinLoopTicks;
	s_inTick = inTick;
	s_outTick = outTick;
	s_seeking = false;
	SeekToLoopStartIfOutside();
	Save();
}

void PlaybackLoop_Clear() {
	s_inTick = -1;
	s_outTick = -1;
	s_enabled = false;
	s_seeking = false;
	Save();
}

bool PlaybackLoop_Range(int& inTick, int& outTick) {
	inTick = s_inTick;
	outTick = s_outTick;
	return RangeUsable();
}

void PlaybackLoop_AppendStateJson(std::ostringstream& o) {
	o << ",\"loopOn\":" << (s_enabled ? "true" : "false")
		<< ",\"loopIn\":" << s_inTick
		<< ",\"loopOut\":" << s_outTick
		// The UI needs to know whether the region can actually run, so an unusable one can be
		// shown as incomplete rather than as a loop that mysteriously never fires.
		<< ",\"loopReady\":" << (RangeUsable() ? "true" : "false");
}

void PlaybackLoop_ReloadProject() {
	s_loaded = false;
	s_inTick = s_outTick = -1;
	s_enabled = false;
}

void PlaybackLoop_RunFrame() {
	if (!g_pEngineToClient) return;
	// A scrub steps the demo forward in short real-playback bursts; those must not trip the loop's
	// Out point and seek the world back to In under the user's drag.
	if (DemoScrub_Active()) return;

	auto* demo = g_pEngineToClient->GetDemoFile();
	const bool playing = demo && demo->IsPlayingDemo();
	const bool paused = demo && demo->IsDemoPaused(); // a paused demo reports IsPlayingDemo() false
	if (!playing && !paused) {
		// IsPlayingDemo() also blips false transiently DURING the seek we just issued. Dropping
		// state here would re-read the still-past-Out tick next frame and seek again.
		if (s_seeking && GetTickCount64() <= s_seekDeadlineMs) return;
		s_loaded = false;
		s_seeking = false;
		return;
	}

	if (!SyncDemo()) return;
	if (!s_enabled || !RangeUsable()) return;

	// A PLAYING camera path owns the playhead. Its markers already define the range, and it
	// loops itself (CameraPath::RestartPathPlayback) off this same switch -- so seeking
	// underneath it here would fight the dolly and tear the shot apart mid-move.
	if (CameraPathRef().GetMode() == CameraPath::Mode::PreviewPlaying) return;

	int tick = 0;
	if (!g_MirvTime.GetCurrentDemoTick(tick)) return;

	if (s_seeking) {
		if (tick < s_outTick - kSeekLandedSlackTicks || GetTickCount64() > s_seekDeadlineMs)
			s_seeking = false;
		else
			return;
	}

	// Paused is left alone on purpose: parking the playhead past Out to look at something is a
	// normal thing to do, and a loop that yanked you back every frame would make it impossible.
	if (paused) return;

	if (tick < s_outTick) return;

	s_seeking = true;
	s_seekDeadlineMs = GetTickCount64() + kSeekTimeoutMs;
	char cmd[64];
	sprintf_s(cmd, "demo_gototick %d", s_inTick);
	DemoCmd(cmd);
}

// ============================== command ============================================

namespace {

bool QuietArg(int argc, advancedfx::ICommandArgs* args) {
	for (int i = 3; i < argc; ++i)
		if (0 == _stricmp(args->ArgV(i), "quiet")) return true;
	return false;
}

void ReportRange() {
	if (s_inTick < 0 && s_outTick < 0) {
		advancedfx::Message("mirv_filmmaker: loop region not set.\n");
		return;
	}
	if (!RangeUsable()) {
		advancedfx::Message(
			"mirv_filmmaker: loop region incomplete (in=%d out=%d) -- set both, at least %d ticks apart.\n",
			s_inTick, s_outTick, kMinLoopTicks);
		return;
	}
	const int ticks = s_outTick - s_inTick;
	float interval = g_MirvTime.interval_per_tick_get();
	if (!(interval > 0.00001f)) interval = 1.0f / 64.0f;
	// Where the playhead is relative to the region. A loop status that does not say where you
	// are is half a status -- "is it actually looping me?" is the question you open it to ask,
	// and without this the only way to answer it is to watch the screen.
	const int tick = CurrentTickOr(-1);
	const char* where = (tick < 0) ? ""
		: (tick < s_inTick) ? " [before]"
		: (tick < s_outTick) ? " [inside]" : " [past]";
	advancedfx::Message("mirv_filmmaker: loop %s -- in %d, out %d (%d ticks, %.2fs); tick %d%s.\n",
		s_enabled ? "ON" : "off", s_inTick, s_outTick, ticks, ticks * interval, tick, where);
}

} // namespace

void PlaybackLoop_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* action = (argc >= 3) ? args->ArgV(2) : "";
	const char* value = (argc >= 4) ? args->ArgV(3) : "";
	const bool quiet = QuietArg(argc, args);

	if (0 == _stricmp(action, "on") || 0 == _stricmp(action, "off")
		|| 0 == _stricmp(action, "toggle")) {
		const bool on = (0 == _stricmp(action, "toggle")) ? !s_enabled : (0 == _stricmp(action, "on"));
		PlaybackLoop_SetEnabled(on);
		if (quiet) return;
		if (on && !RangeUsable())
			advancedfx::Message(
				"mirv_filmmaker: loop armed, but the region is not set yet -- `%s loop in` / `%s loop out`.\n",
				cmd, cmd);
		else
			ReportRange();
		return;
	}

	// No tick argument = use the playhead. That is the common case: you are watching, you see
	// the start of the bit you care about, you set In there.
	if (0 == _stricmp(action, "in") || 0 == _stricmp(action, "out")) {
		const bool isIn = (0 == _stricmp(action, "in"));
		const int tick = *value ? atoi(value) : CurrentTickOr(-1);
		if (tick < 0) {
			advancedfx::Warning("mirv_filmmaker: no demo tick available to set the loop %s from.\n",
				isIn ? "in" : "out");
			return;
		}
		if (isIn) PlaybackLoop_SetIn(tick);
		else PlaybackLoop_SetOut(tick);
		if (!quiet) ReportRange();
		return;
	}

	if (0 == _stricmp(action, "range")) {
		if (argc < 5) {
			advancedfx::Warning("usage: %s loop range <inTick> <outTick> [quiet]\n", cmd);
			return;
		}
		PlaybackLoop_SetRange(atoi(value), atoi(args->ArgV(4)));
		if (!quiet) ReportRange();
		return;
	}

	if (0 == _stricmp(action, "clear")) {
		PlaybackLoop_Clear();
		if (!quiet) advancedfx::Message("mirv_filmmaker: loop region cleared.\n");
		return;
	}

	if (0 == _stricmp(action, "status")) {
		ReportRange();
		return;
	}

	// Explicit hand-off between the loop and the render range. These exist BECAUSE the two are
	// separate: copying is a thing you ask for, never something a loop nudge does to your
	// carefully placed render range behind your back.
	if (0 == _stricmp(action, "torender")) {
		if (!RangeUsable()) {
			advancedfx::Warning("mirv_filmmaker: no usable loop region to copy.\n");
			return;
		}
		RenderRange r = Render_GetRange();
		r.mode = RenderRangeMode::CustomTicks;
		r.startTick = s_inTick;
		r.endTickExclusive = s_outTick;
		Render_SetRange(r);
		advancedfx::Message("mirv_filmmaker: render range set from the loop (%d..%d).\n",
			s_inTick, s_outTick);
		return;
	}

	if (0 == _stricmp(action, "fromrender")) {
		const RenderRange& r = Render_GetRange();
		if (!r.Valid()) {
			advancedfx::Warning("mirv_filmmaker: the render range is not usable.\n");
			return;
		}
		s_inTick = r.startTick;
		s_outTick = r.endTickExclusive;
		s_seeking = false;
		Save();
		ReportRange();
		return;
	}

	advancedfx::Message(
		"%s loop on|off|toggle - loop playback over the region below.\n"
		"%s loop in [<tick>] - region start; no tick = the current playhead.\n"
		"%s loop out [<tick>] - region end; no tick = the current playhead.\n"
		"%s loop clear - forget the region and switch the loop off.\n"
		"%s loop status - report the region and whether it can run.\n"
		"%s loop torender - copy the loop region into the RENDER range (explicit; never automatic).\n"
		"%s loop fromrender - copy the render range into the loop region.\n",
		cmd, cmd, cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker
