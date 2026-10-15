#include "DeadlockWorkspaces.h"

#include "DeadlockStyleJs.h"
#include "DeadlockWorkspaceJs.h"
#include "DeadlockCameraBodyJs.h"
#include "DeadlockConfigRenderBodyJs.h"
#include "DeadlockGraphBodyJs.h"
#include "DeadlockDock.h"
#include "DeadlockDockRibbonJs.h"
#include "DeadlockNativeHudJs.h"
#include "DeadlockMovieHud.h"
#include "DeadlockMovieHudJs.h"
#include "DeadlockDrawJs.h"
#include "DeadlockControlsJs.h"
#include "DeadlockSpeedDialJs.h"
#include "DeadlockInspectorGripJs.h"
#include "DeadlockRegularTimelineJs.h"
#include "DeadlockTimelineJs.h"
#include "DeadlockWorkspaceLayoutMath.h"
#include "DeadlockWorkspacePreview.h"
#include "DeadlockPlaybackLoop.h"
#include "DeadlockPathShake.h"

#include "DeadlockFollowCamera.h"

#include "../Panorama/PanoramaBridge.h"
#include "../Panorama/PanoramaFindPanel.h"
#include "../Movie/CamMarkers.h"
#include "DeadlockShot.h"
#include "DeadlockGraph.h"
#include "DeadlockRender.h"
#include "../Debug/MvmDebugLog.h"
#include "../../Deadlock/DeadlockCameraRuntime.h"
#include "../../Deadlock/DeadlockOverlayMath.h"
#include <cmath>
#include "../../Deadlock/DeadlockEngine.h"
#include "../../Deadlock/DeadlockHudBlackout.h"
#include "../../Deadlock/DeadlockEntities.h"
#include "../../Deadlock/DeadlockOverlayDrawer.h"
#include "../../Deadlock/DeadlockPlaybackSpeed.h"
#include "../../Deadlock/DeadlockViewSetup.h"
#include "../../Deadlock/DeadlockInput.h"
#include "../../Deadlock/DeadlockPanorama.h"
#include "../../Deadlock/DeadlockUi.h"
#include "../../../Shared/AfxConsole.h"

#include <cstdio>
#include <chrono>
#include <string>
#include <vector>

namespace Filmmaker {

namespace {

PanoramaBridge g_bridge;
bool g_built = false;
bool g_rebuildRequested = false;
int g_buildAttempts = 0;

void* g_root = nullptr;

DeadlockWorkspace g_workspace = DeadlockWorkspace::Off;

// The screen, in VIRTUAL pixels, as the panel measured it. Panorama is the only side that knows
// this -- actuallayoutwidth divided by actualuiscale_x -- so the panel reports it with a console
// command whenever it changes and C++ owns every number derived from it after that.
//
// One owner for the geometry is the whole point. The frame drawn around the preview and the
// viewport the world renders into come out of a single Solve(), so they cannot disagree; two copies
// of the same arithmetic disagreeing by two pixels shows as a seam of un-shrunk world along one
// edge, and that is not a bug anyone finds quickly.
// Whether the game's own HUD is SCALED into the preview (the default, and what a reviewer usually
// wants to see) or hidden outright for a clean plate.
bool g_hudHidden = false;

int g_screenW = 0;
int g_screenH = 0;

// The column and the dock. Literals rather than settings for now; both are the CS2 editor's, and
// the solver clamps the column against the screen so a small window cannot be left without a
// preview.
// The column's REQUESTED width. A setting rather than a literal: the CS2 editor's column is
// resized by dragging its edge, and so is this one (DeadlockInspectorGripJs.h). The solver
// clamps it against the screen, so an absurd request cannot leave the preview without room.
const double kInspectorWDefault = 520.0;
double g_inspectorW = kInspectorWDefault;
const double kHeaderH = 44.0;

// The accent band the chrome draws across the FOOT of the preview area: a 44px translucent strip
// closed by a 2px rule. The solver is told about it so the picture is letterboxed above it rather
// than underneath it -- see WorkspaceLayoutInput::bottomBandH.
const double kBottomBandH = 46.0;

// The render workspace's JOB column, pinned to the LEFT of the band. It states what is about to be
// captured and why it cannot start, so it is always on screen while the render workspace is open
// rather than being one more card to scroll to. The panel reads this back out of the solved layout;
// keep it in step with JOB_W in DeadlockConfigRenderBodyJs.h.
const double kJobColumnW = 340.0;

// The dock's height comes from the dock, not from here. Its three tabs reserve different amounts
// of screen -- the game's own replay bar, our timeline, the graph -- and a constant would either
// overlap the tallest or strand a band of dead screen under the shortest.
double DockHeight() { return (double)Dock_Height(); }

// EVERY DOCK TAB INSETS THE PREVIEW.
//
// The Regular Timeline used to be exempt, on the reasoning that that band IS Deadlock's own
// translucent replay bar with the game rendering behind it. Every leg of that is now false in
// tree: the bar is force-hidden on all three tabs (DeadlockNativeHudJs.h holdReplayBar), the
// Regular Timeline is our own opaque panel, and the ribbon above it is opaque too. Reserving
// nothing for it let the solver run the picture 51px past the dock's top edge, so the chrome's
// own band and rule were drawn over live world -- the gold line across the render at y=904 and
// the strip of picture that vanished under the band. Measured at 1920x1080, native tab.

WorkspaceLayout g_layout;

// Per-workspace preview ownership. CONFIG is a sidebar and the camera editor is the full
// workspace, so the editor outranks it -- a stated rule rather than "whichever ran last".
PreviewOwner OwnerFor(DeadlockWorkspace workspace) {
	switch (workspace) {
	case DeadlockWorkspace::Off: return PreviewOwner::None;
	case DeadlockWorkspace::Config: return PreviewOwner::Config;
	default: return PreviewOwner::CameraEditor;
	}
}

// Appends a JSON string value with the handful of escapes a level name or file path can actually
// contain. Deliberately not a general JSON encoder: this feeds one attribute read by one parser.
void AppendJsonString(std::string& out, const char* value) {
	out += '"';
	if (value) {
		for (const char* p = value; *p; ++p) {
			switch (*p) {
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': break;
			case '\t': out += "\\t"; break;
			default:
				if ((unsigned char)*p < 0x20) break;
				out += *p;
			}
		}
	}
	out += '"';
}

void AppendNumber(std::string& out, double value, int decimals) {
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
	out += buf;
}

// Runs the tested solver against the last reported screen size, and publishes the result to the
// preview owner. Called once per frame, before the state JSON is built, so the panel and the engine
// are looking at the same rectangle.

void SolveLayout(bool demoLoaded) {
	WorkspaceLayoutInput in;
	in.screenW = g_screenW;
	in.screenH = g_screenH;
	in.headerH = kHeaderH;

	// NO DEMO, NO WORKSPACE -- and the workspace it was left in is remembered, so loading the
	// next replay brings it straight back.
	//
	// This is not tidiness. Everything the editor does to the game is keyed off this flag: the
	// blit that shrinks the world into the preview rect, the transform that scales the game's
	// own HUD to match, the widened replay bar. When a replay ended, all of it stayed on -- and
	// Deadlock's post-game screen is a full-screen Panorama layer ABOVE the HUD, so the result
	// was the match summary squashed into the preview rectangle with black around it and our own
	// panels invisible behind it. Reported as 'the demo ended and the UI disappeared'.
	// !HudBlackout_Active(): F8 hands the whole frame back to the game, the same way CS2's blackout
	// "releases the workspace preview scaling" -- otherwise the world would stay shrunk into the
	// preview rect while every panel drawing chrome around it is hidden, which is a small picture
	// surrounded by black rather than a clean screen.
	const bool open = demoLoaded && g_workspace != DeadlockWorkspace::Off
		&& !Deadlock::HudBlackout_Active();
	in.inspectorW = open ? g_inspectorW : 0.0;
	in.leftW = (open && g_workspace == DeadlockWorkspace::Render) ? kJobColumnW : 0.0;
	in.dockH = open ? DockHeight() : 0.0;
	in.bottomBandH = open ? kBottomBandH : 0.0;

	// Letterbox to the ENGINE's aspect, not the screen's. They differ whenever the game is
	// rendering at a non-native aspect, and using the screen's would frame the preview around a
	// picture the engine is not producing.
	int viewW = 0, viewH = 0;
	Deadlock::CameraRuntime_GetViewportSize(viewW, viewH);
	if (viewW > 0 && viewH > 0) in.worldAspect = (double)viewW / (double)viewH;
	else if (g_screenW > 0 && g_screenH > 0) in.worldAspect = (double)g_screenW / (double)g_screenH;

	g_layout = WorkspaceLayout_Solve(in);

	const PreviewOwner owner = OwnerFor(g_workspace);
	if (open && g_layout.valid && owner != PreviewOwner::None) {
		WorkspacePreview_Submit(owner,
			(float)g_layout.blitX0, (float)g_layout.blitY0,
			(float)g_layout.blitX1, (float)g_layout.blitY1);
	} else {
		WorkspacePreview_Release(owner);
	}
}

void AppendLayoutJson(std::string& s) {
	s += ",\"layout\":{\"valid\":";
	s += g_layout.valid ? "true" : "false";
	s += ",\"screenW\":";
	AppendNumber(s, g_screenW, 0);
	s += ",\"screenH\":";
	AppendNumber(s, g_screenH, 0);
	s += ",\"inspectorW\":";
	AppendNumber(s, g_layout.inspectorW, 0);
	s += ",\"leftW\":";
	AppendNumber(s, g_layout.leftW, 0);
	// THE DOCK HEIGHT THE SOLVER USED, not a second opinion about it. Publishing DockHeight()
	// unconditionally while SolveLayout passed 0 on the native tab is exactly how the chrome ended
	// up framing a band the picture was not in.
	s += ",\"dockH\":";
	AppendNumber(s, g_workspace != DeadlockWorkspace::Off ? DockHeight() : 0.0, 0);
	s += ",\"headerH\":";
	AppendNumber(s, kHeaderH, 0);
	// The chrome's own rects, solved rather than re-derived in JavaScript from literals.
	s += ",\"frameTopY\":";
	AppendNumber(s, g_layout.frameTopY, 0);
	s += ",\"frameBottomY\":";
	AppendNumber(s, g_layout.frameBottomY, 0);
	s += ",\"bandBottomY\":";
	AppendNumber(s, g_layout.bandBottomY, 0);
	s += ",\"areaX\":";
	AppendNumber(s, g_layout.areaX, 0);
	s += ",\"areaY\":";
	AppendNumber(s, g_layout.areaY, 0);
	s += ",\"areaW\":";
	AppendNumber(s, g_layout.areaW, 0);
	s += ",\"areaH\":";
	AppendNumber(s, g_layout.areaH, 0);
	s += ",\"previewX\":";
	AppendNumber(s, g_layout.previewX, 0);
	s += ",\"previewY\":";
	AppendNumber(s, g_layout.previewY, 0);
	s += ",\"previewW\":";
	AppendNumber(s, g_layout.previewW, 0);
	s += ",\"previewH\":";
	AppendNumber(s, g_layout.previewH, 0);
	s += "}";
}

std::string BuildStateJson(bool demoLoaded) {
	std::string s;
	s.reserve(2048);
	s += "{";

	// The SAME condition the layout solved against. Two answers to "is the editor open" is how
	// the panel ends up drawing chrome around a preview that has already been released.
	s += "\"open\":";
	s += (demoLoaded && g_workspace != DeadlockWorkspace::Off) ? "true" : "false";

	// F8. Kept separate from "open" rather than folded into it: "open" is the tool's own
	// persistent state (what SolveLayout/rebuild logic keys off, and what comes back unchanged
	// once the blackout lifts), while this is a render-time-only flag the JS side checks to hide
	// every panel -- ours and the game's -- without forgetting what was actually open.
	s += ",\"blackout\":";
	s += Deadlock::HudBlackout_Active() ? "true" : "false";

	s += ",\"workspace\":";
	switch (g_workspace) {
	case DeadlockWorkspace::Graph: AppendJsonString(s, "graph"); break;
	case DeadlockWorkspace::Config: AppendJsonString(s, "config"); break;
	case DeadlockWorkspace::Render: AppendJsonString(s, "render"); break;
	default: AppendJsonString(s, "camera"); break;
	}

	s += ",\"demo\":";
	s += demoLoaded ? "true" : "false";

	s += ",\"paused\":";
	s += Deadlock::Engine_IsDemoPaused() ? "true" : "false";

	s += ",\"uiMouse\":";
	s += Deadlock::Input_UiMouse() ? "true" : "false";

	// The playback dial, so both speed sliders seed from ONE number rather than from a copy each.
	s += ",\"speed\":";
	AppendNumber(s, Deadlock::PlaybackSpeed(), 3);

	// Which half of the timeline's Linear | Smooth pair is lit. The graph owns the setting; the
	// timeline only draws it, so neither surface can claim a mode the other is not in.
	s += ",\"pathSmooth\":";
	s += Graph_Smooth() ? "true" : "false";

	// THE CURSOR. This Panorama context gets no mouse-move event at all, so a panel cannot hit-test
	// a drag from anything of its own -- the OS-side probe is the only source there is. Published
	// in window-client pixels, the same space the dock's rectangle is in.
	{
		int mx = 0, my = 0;
		bool lmb = false, rmb = false, shift = false;
		unsigned seq = 0;
		Deadlock::CameraRuntime_GetUiCursor(mx, my, lmb, rmb, shift, seq);
		s += ",\"mx\":";
		AppendNumber(s, mx, 0);
		s += ",\"my\":";
		AppendNumber(s, my, 0);
		s += ",\"lmb\":";
		s += lmb ? "true" : "false";
		s += ",\"rmb\":";
		s += rmb ? "true" : "false";
		s += ",\"shift\":";
		s += shift ? "true" : "false";
	}

    unsigned numberKeys = 0;
    if (Deadlock::Input_Focused() && Deadlock::Input_UiMouse()) {
        for (int digit = 0; digit < 10; ++digit)
            if ((GetAsyncKeyState('0' + digit) | GetAsyncKeyState(VK_NUMPAD0 + digit)) & 0x8000) numberKeys |= 1u << digit;
        if (GetAsyncKeyState(VK_BACK) & 0x8000) numberKeys |= 1u << 10;
        if (GetAsyncKeyState(VK_RETURN) & 0x8000) numberKeys |= 1u << 11;
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) numberKeys |= 1u << 12;
    }
    s += ",\"numberKeys\":" + std::to_string(numberKeys);

	const int tick = Deadlock::Engine_DemoTick();
	s += ",\"tick\":";
	AppendNumber(s, tick < 0 ? 0 : tick, 0);

	s += ",\"time\":";
	AppendNumber(s, Deadlock::Engine_DemoTime(), 3);

	s += ",\"level\":";
	AppendJsonString(s, Deadlock::Engine_LevelName().c_str());

	// The marker model is the shared, engine-free CamMarkers, owned by DeadlockShot -- one owner
	// for the shot, read by every view of it.
	const int markerCount = Shot_MarkerCount();
	const int selected = Shot_Selected();
	s += ",\"markers\":";
	AppendNumber(s, markerCount, 0);

	int startTick = tick < 0 ? 0 : tick;
	int endTick = startTick + 1;
	if (markerCount > 0) {
		startTick = Shot_FirstTick();
		endTick = Shot_LastTick();
		if (endTick <= startTick) endTick = startTick + 1;
	}
	s += ",\"startTick\":";
	AppendNumber(s, startTick, 0);
	s += ",\"endTick\":";
	AppendNumber(s, endTick, 0);

	s += ",\"playhead\":";
	AppendNumber(s, Shot_PlayheadTick(), 1);
	s += ",\"playing\":";
	s += Shot_Playing() ? "true" : "false";

	s += ",\"selected\":";
	AppendNumber(s, selected, 0);

	// Whether the framed preview is the whole rendered frame or a crop of it. The two look the
	// same until you compare against a full-screen capture, so the panel is told which it is.
	s += ",\"hudHidden\":";
	s += g_hudHidden ? "true" : "false";

	s += ",\"previewScaled\":";
	s += WorkspacePreview_Scaling() ? "true" : "false";

	// The camera path owns the view while a shot plays; otherwise the frame is showing the game's
	// own camera. The preview frame changes colour on this, so "which camera am I looking at" is
	// never a guess.
	s += ",\"live\":";
	s += (Shot_OwnsView() || Filmmaker::Graph_OwnsView()) ? "false" : "true";

	// Whether one of OUR cameras (free cam, path, parked path hold, follow, graph) wrote this frame's
	// view rather than the game's spectator camera. The native HUD hides Deadlock's "SPECTATING
	// <hero>" strip on it: CS2's free-cam preview shows no spectated-player panel, and the strip's
	// claim is simply false while we own the view.
	s += ",\"camOwned\":";
	s += (Deadlock::CameraRuntime_LastAppliedOwner() != Deadlock::ViewOwner::Game) ? "true" : "false";

	{
		int viewW = 0, viewH = 0;
		Deadlock::CameraRuntime_GetViewportSize(viewW, viewH);
		s += ",\"viewportW\":";
		AppendNumber(s, viewW, 0);
		s += ",\"viewportH\":";
		AppendNumber(s, viewH, 0);
	}

	PlaybackLoop_AppendStateJson(s);
	Graph_AppendHotJson(s);
	Render_AppendHotJson(s);
	MovieHud_AppendStateJson(s);

	s += "}";
	return s;
}

// ---- the cold half ---------------------------------------------------------------------------
//
// WHY THE STATE IS IN TWO HALVES. The workspace is driven by RunScript, and every call compiles
// its source: the bridge passes a fresh origin line each time (see PanoramaBridge::RunScript), so
// nothing is ever cached, and pinning the origin breaks panel building. Compile cost follows source
// size, and once the follow camera, the graph and the render job were added, the one-object state
// was ~15 KB -- measured at ~159 ms per frame, a 6 fps replay against 194 fps with the workspace
// off. A tiny per-frame script on the same uncached path measured 215 fps.
//
// So BuildStateJson above now carries only what moves every frame, and everything else is cut
// into these chunks. Each is rebuilt each frame (cheap), compared against the last one it shipped,
// and pushed into $.MvmCold.<name> only when it differs. The per-frame script then merges
// $.MvmCold under the hot object before calling render, so the panel sees the same one object it
// always did. Chunks are diffed INDEPENDENTLY: the heroes list refreshes at 10 Hz and must not drag
// the 12 KB of graph curves along with it.
namespace {

enum ColdChunk { kChunkMarkers, kChunkFollow, kChunkHeroes, kChunkLayout, kChunkDock,
                 kChunkGraph, kChunkRender, kChunkCount };
const char* const kChunkNames[kChunkCount] = {
	"markers", "follow", "heroes", "layout", "dock", "graph", "render" };
std::string g_lastChunk[kChunkCount];
unsigned long long g_chunkPushes = 0;
unsigned long long g_chunkPushesBy[kChunkCount] = {};

// Every appender writes `,"key":...` fragments meant for the middle of an object. A chunk is that
// fragment made into an object of its own.
std::string WrapChunk(std::string frag) {
	if (!frag.empty() && frag[0] == ',') frag.erase(0, 1);
	frag.insert(0, "{");
	frag += "}";
	return frag;
}

void BuildColdChunks(bool demoLoaded, std::string out[kChunkCount]) {
	{
		std::string f;
		const int markerCount = Shot_MarkerCount();
		const int selected = Shot_Selected();
		f += ",\"markerList\":[";
		for (int i = 0; i < markerCount; ++i) {
			const CamMarker* m = Shot_Marker(i);
			if (!m) continue;
			if (i) f += ",";
			f += "{\"tick\":";
			AppendNumber(f, m->tick, 0);
			f += ",\"ease\":";
			AppendNumber(f, (int)m->ease, 0);
			f += ",\"pos\":";
			char buf[96];
			std::snprintf(buf, sizeof(buf), "%.0f %.0f %.0f", m->x, m->y, m->z);
			AppendJsonString(f, buf);
			f += ",\"selected\":";
			f += (i == selected) ? "true" : "false";
			f += "}";
		}
		f += "]";
		// The selected marker's own values and the path-wide settings, so the PATH card's tiles
		// read the shot rather than remembering what they were last set to.
		Shot_AppendCameraStateJson(f);
		DeadlockPathShake_AppendStateJson(f);
		out[kChunkMarkers] = WrapChunk(f);
	}
	{
		std::string f;
		FollowCamera_AppendStateJson(f);
		out[kChunkFollow] = WrapChunk(f);
	}
	{
		std::string f;
		if (demoLoaded) {
			// The real roster. FollowCamera_AppendHeroesJson's own scan is throttled to 10 Hz
			// internally (Entities_AppendHeroesJson), so calling it every main-thread frame here
			// costs a string compare on every frame the list has not changed, not a rescan.
			FollowCamera_AppendHeroesJson(f);
		} else {
			// NO REPLAY, NO HEROES. Without a demo the entity table holds nothing the picker can
			// offer, and walking it anyway is what ran the main menu at 15 fps.
			f += ",\"heroes\":[]";
		}
		out[kChunkHeroes] = WrapChunk(f);
	}
	{ std::string f; AppendLayoutJson(f); out[kChunkLayout] = WrapChunk(f); }
	{ std::string f; Dock_AppendStateJson(f); out[kChunkDock] = WrapChunk(f); }
	{ std::string f; Graph_AppendStateJson(f); out[kChunkGraph] = WrapChunk(f); }
	{ std::string f; Render_AppendStateJson(f); out[kChunkRender] = WrapChunk(f); }
}

// After a (re)build the panel's script context may be new, so every chunk ships again.
void ForgetColdChunks() {
	for (std::string& c : g_lastChunk) c.clear();
}

} // namespace

bool BuildPanels() {
	g_bridge.Init();
	if (!g_bridge.CanRunScript() || !g_bridge.ContextPanel())
		return false;

	// Order matters: every fragment shares one JavaScript scope, and the style fragment defines
	// the helpers the rest call. Bodies before the shell, because the shell asks each body to
	// build itself into the workspace container.
	if (!g_bridge.RunScript(DEADLOCK_STYLE_JS)) return false;
	if (!g_bridge.RunScript(DEADLOCK_DRAW_JS)) return false;
	if (!g_bridge.RunScript(DEADLOCK_CONTROLS_JS)) return false;
	// The dial registers before the native HUD, the timeline and the graph, each of which builds
	// one. The grip registers before the shell, which builds it onto the column's edge.
	if (!g_bridge.RunScript(DEADLOCK_SPEED_DIAL_JS)) return false;
	if (!g_bridge.RunScript(DEADLOCK_INSPECTOR_GRIP_JS)) return false;
	if (!g_bridge.RunScript(DEADLOCK_CAMERA_BODY_JS)) return false;
	if (!g_bridge.RunScript(DEADLOCK_GRAPH_BODY_JS)) return false;
	if (!g_bridge.RunScript(DEADLOCK_CONFIG_BODY_JS)) return false;
	if (!g_bridge.RunScript(DEADLOCK_RENDER_BODY_JS)) return false;
	// The three docked editors register themselves before the shell, because the shell's build()
	// is what asks each of them to build into the dock body.
	if (!g_bridge.RunScript(DEADLOCK_REGULAR_TIMELINE_JS)) return false;
	if (!g_bridge.RunScript(DEADLOCK_TIMELINE_JS)) return false;
	if (!g_bridge.RunScript(DEADLOCK_DOCK_RIBBON_JS)) return false;
	if (!g_bridge.RunScript(DEADLOCK_NATIVE_HUD_JS)) return false;
	// Builds its own panel lazily on first visible frame (see the file header on why: this
	// fragment's own `if ($.MvmDirectorHud) return;` guard is never cleared by a rebuild, unlike
	// $.MvmWorkspace and friends, so it cannot depend on being re-run to pick up a fresh context).
	if (!g_bridge.RunScript(DEADLOCK_MOVIE_HUD_JS)) return false;
	if (!g_bridge.RunScript(DEADLOCK_WORKSPACE_JS)) return false;
	if (!g_bridge.RunScript("(function(){try{$.MvmWorkspace.build();}catch(e){}})();"))
		return false;

	g_root = FindChildById(g_bridge.ContextPanel(), "MvmWorkspaceRoot", 0);
	if (!g_root) return false;

	advancedfx::Message("AfxHookDeadlock: workspace panels built (attempt %d).\n",
	                    g_buildAttempts);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("deadlock.ui",
			"built root=%p state=direct attempts=%d", g_root, g_buildAttempts);
	return true;
}

} // namespace

void Workspaces_RunMainThreadFrame(bool demoLoaded) {
	// Measure the complete pump, including early returns, without flushing a log per frame.
	struct FrameProbe {
		using Clock = std::chrono::steady_clock;
		bool active = MvmDebugLog_Active();
		Clock::time_point start = active ? Clock::now() : Clock::time_point{};
		~FrameProbe() {
			if (!active) return;
			static unsigned frames = 0;
			static double total = 0, peak = 0;
			static auto last = Clock::now();
			const auto now = Clock::now();
			const double ms = std::chrono::duration<double, std::milli>(now - start).count();
			total += ms; if (ms > peak) peak = ms; ++frames;
			if (now - last < std::chrono::seconds(2)) return;
			MvmDebugLog_Linef("deadlock.perf", "workspace=%s frames=%u meanMs=%.3f peakMs=%.3f",
				Workspaces_Name(g_workspace), frames, total / frames, peak);
			frames = 0; total = peak = 0; last = now;
		}
	} frameProbe;

	// THE PREVIEW RECT IS PUBLISHED ON EVERY PATH OUT OF THIS FUNCTION.
	//
	// Commit() used to be the last statement, so each of the four early returns below -- the
	// once-a-second build gate, the attempt cap, a failed build, a vanished root -- skipped it and
	// left ViewportBlit holding the LAST request it was given. A blit latched active with a stale
	// rect keeps clearing the backbuffer and rescaling the frame for a workspace that is no longer
	// there, which is a black game nobody can get out of. Making it a scope guard means "nothing
	// ran this frame" publishes the same answer as "nothing wants the preview".
	struct CommitGuard { ~CommitGuard() { WorkspacePreview_Commit(); } } commitGuard;

	// The shot advances whether or not the workspace is on screen: closing the panel should not
	// stop a running camera move, and a scrub preview has to survive the panel being hidden.
	Shot_RunFrame();
	DeadlockPathShake_RunFrame();

	// The graph polls the OS cursor probe, so it has to run whether or not its panel is on screen:
	// a drag that started in the graph and continued after a workspace switch would otherwise be
	// left half-applied. Render advances its own state machine for the same reason -- a capture
	// must not stop because someone changed tab.
	Graph_RunMainThreadFrame();
	Render_RunMainThreadFrame();

	// The follow camera runs after both, and stands down while either owns the view. A shot that
	// is playing and a follow camera that is on are not a conflict to resolve per frame -- the
	// shot is the explicit camera move and it wins for as long as it lasts.
	//
	// RE-ENABLED (P5): this call was missing entirely -- FollowCamera_RunFrame existed, compiled,
	// and was fully correct, but nothing in the frame pump ever invoked it, so lock-on/attach never
	// produced a pose regardless of any state the column set. `mirv_filmmaker follow` reported
	// "not solving" unconditionally. Runs whether or not the workspace panel is on screen, same as
	// the shot and the graph above: closing the editor to watch the follow camera work should not
	// stop it.
	FollowCamera_RunFrame(Shot_OwnsView() || Graph_OwnsView());

	// Hero boxes: an EDITING AID, not a permanent HUD. Shown only with the Camera Editor open on a
	// loaded replay, and hidden the moment anything else is actually driving the view (a playing
	// shot, the graph, a live follow camera) or a take is capturing -- matching the plan's own
	// rule ("hidden during shot playback, live follow and render capture") and CS2's own precedent
	// (CampathDrawer's gizmos are suppressed the same way for a recording, DeadlockCameraRuntime.cpp).
	{
		// CS2's CameraPath::EnsureDrawState draws the gizmo whenever a path exists and no live camera
		// owns the view -- no editor panel required, which is what makes K visibly place a camera
		// with the editor closed. Hidden for shot playback, graph drive, live follow and any capture.
		const bool showGizmos = demoLoaded
			&& !Shot_OwnsView() && !Graph_OwnsView() && !FollowCamera_OwnsView()
			&& Render_GetState() == RenderState::Idle;
		// CS2 boxes the follow TARGET only (CampathDrawer's FollowTargetBox), not every player.
		const int selectedHero = FollowCamera_Get().hero;
		if (showGizmos && selectedHero >= 0) {
			std::vector<Deadlock::HeroEntry> heroes;
			Deadlock::Entities_HeroesCached(heroes);
			std::vector<Deadlock::OverlayHeroBox> boxes;
			for (const Deadlock::HeroEntry& h : heroes) {
				if (!h.alive || h.index != selectedHero) continue;
				Deadlock::OverlayHeroBox box;
				box.feetX = h.origin[0]; box.feetY = h.origin[1]; box.feetZ = h.origin[2];
				boxes.push_back(box);
			}
			Deadlock::OverlayDrawer_SetHeroes(boxes.data(), (int)boxes.size());
		} else {
			Deadlock::OverlayDrawer_SetHeroes(nullptr, 0);
		}

		// `path draw|drawmarkers 0` is the console override, on by default.
		const bool showPath = showGizmos && Shot_MarkerCount() > 0;
		Deadlock::OverlayDrawer_SetPathStyle(Shot_Freeze());
		if (showPath && Shot_DrawPath()) {
			static std::vector<double> xyz;
			xyz.resize(1024 * 3);
			const int n = Shot_SamplePath(xyz.data(), 1024);
			static std::vector<Deadlock::OverlayPathPoint> pts;
			pts.resize(n);
			for (int i = 0; i < n; ++i) pts[i] = { xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2] };
			Deadlock::OverlayDrawer_SetPath(pts.data(), n);
		} else {
			Deadlock::OverlayDrawer_SetPath(nullptr, 0);
		}
		// CS2's crosshair pick (CameraPathPicking.cpp), computed whenever there are markers whether or
		// not they are drawn, because F / L act on it. Each marker carries its own angles and FOV:
		// CS2's gizmo is a little camera shaped by them.
		static std::vector<Deadlock::OverlayMarker> marks;
		static std::vector<Deadlock::OverlayVec3> markerPositions;
		marks.clear();
		markerPositions.clear();
		if (showPath) {
			for (int i = 0; i < Shot_MarkerCount(); ++i) {
				const CamMarker* m = Shot_Marker(i);
				if (!m) continue;
				marks.push_back({ m->x, m->y, m->z, m->pitch, m->yaw, m->roll, m->fov, false });
				markerPositions.push_back({ m->x, m->y, m->z });
			}
		}
		int hovered = -1;
		if (!markerPositions.empty()) {
			double origin[3], angles[3], fov;
			Deadlock::CameraRuntime_GetLastCamera(origin, angles, fov);
			hovered = Deadlock::PickAimedMarker({ origin[0], origin[1], origin[2] }, angles[0], angles[1],
				markerPositions.data(), (int)markerPositions.size());
			if (hovered >= 0) marks[hovered].hovered = true;
		}
		Shot_SetHoveredMarker(hovered);
		if (showPath && Shot_DrawMarkers()) Deadlock::OverlayDrawer_SetMarkers(marks.data(), (int)marks.size());
		else Deadlock::OverlayDrawer_SetMarkers(nullptr, 0);
	}

	// The loop runs whether or not the workspace is on screen. It is a playback behaviour, not a
	// panel behaviour: closing the editor to watch something should not silently stop the loop you
	// armed to watch it with.
	PlaybackLoop_RunFrame();

	if (g_rebuildRequested) {
		g_rebuildRequested = false;
		g_built = false;
		ForgetColdChunks();
		g_root = nullptr;
		g_buildAttempts = 0;
		// Drop the old panel and the registered objects, so a rebuild is a rebuild rather than a
		// second copy stacked on the first.
		g_bridge.RunScript(
			"(function(){try{if($.MvmNativeHud)$.MvmNativeHud.teardown();}catch(e){}})();");
		g_bridge.RunScript(
			"(function(){try{var r=$.GetContextPanel().FindChildTraverse('MvmWorkspaceRoot');"
			"if(r)r.DeleteAsync(0.0);}catch(e){}"
			"$.MvmWorkspace=null;$.MvmCameraBody=null;$.MvmGraphBody=null;"
			"$.MvmConfigBody=null;$.MvmRenderBody=null;$.MvmTimeline=null;"
			"$.MvmDockRibbon=null;"
			"$.MvmStyle=null;})();");
	}

	if (!g_built) {
		// The HUD panel does not exist in the dashboard, so keep trying until a map or demo loads --
		// but NOT EVERY FRAME. A build attempt runs the entire workspace source, fourteen scripts and
		// some six thousand lines, through an uncached RunScript, and until the HUD exists it fails
		// AFTER doing all of that. Retried per frame, that was a full recompile per frame for the
		// whole of a replay's pre-round: measured at 7 fps with the editor shut and nothing else
		// running, and 145 fps the moment the build had succeeded. One attempt a second is more than
		// prompt enough for a panel that appears once per replay.
		//
		// Capped so a genuinely broken RunScript does not burn attempts forever: 60 attempts at one
		// per second is the same minute the old per-frame cap of 3600 frames gave.
		static unsigned s_frameGate = 0;
		if ((++s_frameGate % 60) != 1) return;
		if (++g_buildAttempts > 60) {
			if (g_buildAttempts == 61)
				advancedfx::Warning(
					"AfxHookDeadlock: could not build the workspace panels. Check "
					"'mirv_filmmaker health' -- RunScript or the HUD panel did not resolve.\n");
			return;
		}
		g_built = BuildPanels();
		if (!g_built) return;
		ForgetColdChunks();
	}

	// A panel that vanished under us: leaving a demo tears the HUD down and the root goes with it.
	if (!g_root) {
		g_built = false;
		return;
	}

	SolveLayout(demoLoaded);

	// Cold first, and only what changed: each push is its own small compile.
	{
		std::string chunks[kChunkCount];
		BuildColdChunks(demoLoaded, chunks);
		for (int c = 0; c < kChunkCount; ++c) {
			if (chunks[c] == g_lastChunk[c]) continue;
			g_lastChunk[c] = chunks[c];
			++g_chunkPushes;
			++g_chunkPushesBy[c];
			std::string push("(function(){try{$.MvmCold=$.MvmCold||{};$.MvmCold.");
			push += kChunkNames[c];
			push += "=";
			push += chunks[c];
			push += ";}catch(e){$.Msg('mvm cold error: ' + e);}})();";
			g_bridge.RunScript(push);
		}
	}

	// Then the hot half every frame -- small, so its compile is cheap -- merged under the cold
	// chunks into the one object the panel has always been given. graphHot and renderHot are
	// folded back onto graph.* and render.* so no panel code had to learn about the split.
	const std::string hot = BuildStateJson(demoLoaded);
	std::string script("(function(){try{var c=$.MvmCold||{},h=");
	script += hot;
	script += ";for(var n in c){var o=c[n];for(var k in o)if(!(k in h))h[k]=o[k];}"
	          "if(h.graph&&h.graphHot){h.graph.playhead=h.graphHot.playhead;"
	          "var ch=h.graph.channels||[],cu=h.graphHot.current||[];"
	          "for(var i=0;i<ch.length;i++)ch[i].current=cu[i];}"
	          "if(h.render&&h.renderHot){var r=h.renderHot;for(var k2 in r)h.render[k2]=r[k2];}"
	          "$.MvmWorkspace.render(h);}catch(e){$.Msg('mvm render error: ' + e);}})();";
	g_bridge.RunScript(script);
	// The commit itself happens in commitGuard's destructor, above: once per frame, after every
	// panel has submitted, on every path out.
}

void Workspaces_SetInspectorWidth(int px) {
	// Same floor the solver applies; anything narrower is a measurement, not a request.
	if (px < 320 || px > 4096) return;
	if ((double)px == g_inspectorW) return;
	g_inspectorW = (double)px;
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("deadlock.ws", "inspector width -> %d", px);
}

int Workspaces_InspectorWidth() { return (int)g_inspectorW; }

void Workspaces_SetScreenSize(int width, int height) {
	// Rejected rather than clamped. A zero or absurd size means the panel measured before layout
	// settled, and remembering it would place every panel against a screen that does not exist --
	// which is exactly the half-second of everything-squashed-into-the-corner this avoids.
	if (width < 64 || height < 64 || width > 16384 || height > 16384) return;
	if (width == g_screenW && height == g_screenH) return;
	g_screenW = width;
	g_screenH = height;
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("deadlock.ui", "screen %dx%d", width, height);
}

void Workspaces_GetScreenSize(int& width, int& height) {
	width = g_screenW;
	height = g_screenH;
}

void Workspaces_SetHudHidden(bool hidden) { g_hudHidden = hidden; }
bool Workspaces_HudHidden() { return g_hudHidden; }

DeadlockWorkspace Workspaces_Current() { return g_workspace; }

void Workspaces_Set(DeadlockWorkspace workspace) {
	if (g_workspace == workspace) return;
	g_workspace = workspace;

	// Opening the editor does not seize the mouse. Deadlock's native J binding toggles it, and the
	// input layer mirrors that state to Panorama while still forwarding J to the game.
	Deadlock::Input_SetWorkspaceOpen(workspace != DeadlockWorkspace::Off);

	// Releasing here as well as in SolveLayout covers the frame the workspace closes on: the pending
	// slot is per-frame, so a panel that stops submitting releases the rect anyway, but saying so
	// explicitly keeps the transition out of the debug log's noise.
	if (workspace == DeadlockWorkspace::Off) {
		WorkspacePreview_Release(PreviewOwner::CameraEditor);
		WorkspacePreview_Release(PreviewOwner::Config);
	}

	advancedfx::Message("AfxHookDeadlock: workspace -> %s\n", Workspaces_Name(workspace));
}

bool Workspaces_FromName(const char* name, DeadlockWorkspace* out) {
	if (!name || !out) return false;
	if (0 == _stricmp(name, "off") || 0 == _stricmp(name, "close")) {
		*out = DeadlockWorkspace::Off; return true;
	}
	if (0 == _stricmp(name, "camera") || 0 == _stricmp(name, "editor")) {
		*out = DeadlockWorkspace::CameraEditor; return true;
	}
	if (0 == _stricmp(name, "graph")) { *out = DeadlockWorkspace::Graph; return true; }
	if (0 == _stricmp(name, "config")) { *out = DeadlockWorkspace::Config; return true; }
	if (0 == _stricmp(name, "render")) { *out = DeadlockWorkspace::Render; return true; }
	return false;
}

const char* Workspaces_Name(DeadlockWorkspace workspace) {
	switch (workspace) {
	case DeadlockWorkspace::Off: return "off";
	case DeadlockWorkspace::CameraEditor: return "camera";
	case DeadlockWorkspace::Graph: return "graph";
	case DeadlockWorkspace::Config: return "config";
	case DeadlockWorkspace::Render: return "render";
	}
	return "?";
}

bool Workspaces_Open() { return g_workspace != DeadlockWorkspace::Off; }
bool Workspaces_Built() { return g_built; }
void Workspaces_RequestRebuild() { g_rebuildRequested = true; }

bool Workspaces_Eval(const char* js) {
	if (!js || !*js) return false;
	if (!g_bridge.CanRunScript()) {
		advancedfx::Warning("AfxHookDeadlock: RunScript is not available yet.\n");
		return false;
	}
	// Wrapped so a throw inside the snippet does not abort the rest of the panel's script state,
	// and reported rather than swallowed -- a silent no-op here is indistinguishable from a
	// snippet that ran and did nothing.
	std::string wrapped("(function(){try{");
	wrapped += js;
	wrapped += "}catch(e){$.Msg('mvm eval error: ' + e);}})();";
	return g_bridge.RunScript(wrapped);
}

void Workspaces_Report() {
	advancedfx::Message("  ui cold:   chunk pushes=%llu  by chunk:", g_chunkPushes);
	for (int c = 0; c < kChunkCount; ++c)
		advancedfx::Message(" %s=%llu", kChunkNames[c], g_chunkPushesBy[c]);
	advancedfx::Message("\n             (a chunk whose count climbs every frame is not stable and is recompiled per frame)\n");
	advancedfx::Message(
		"  ui:        workspace=%s  panelsBuilt=%s  runScript=%s  state=direct\n",
		Workspaces_Name(g_workspace),
		g_built ? "yes" : "no",
		g_bridge.HasRunScript() ? "resolved" : "MISSING");
	advancedfx::Message(
		"  layout:    screen=%dx%d  %s  column=%d dock=%d  preview=%.0f,%.0f %.0fx%.0f\n",
		g_screenW, g_screenH, g_layout.valid ? "solved" : "NOT SOLVED",
		(int)g_layout.inspectorW,
		(int)(g_workspace != DeadlockWorkspace::Off ? DockHeight() : 0.0),
		g_layout.previewX, g_layout.previewY, g_layout.previewW, g_layout.previewH);
	if (g_screenW <= 0)
		advancedfx::Warning(
			"  the panel has not reported a screen size yet, so nothing is placed. If this persists,\n"
			"  the workspace JS threw before its first render -- check 'mirv_filmmaker ui rebuild'.\n");
	WorkspacePreview_Report();
	Dock_Report();
	PlaybackLoop_Report();
}

} // namespace Filmmaker
