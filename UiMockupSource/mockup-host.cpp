// Browser host for the Source 1 workspace UI. It compiles the same C++ the game hook runs
// (AfxHookSource/Filmmaker/Ui: the Camera Editor shell, the CS:S style, CS2's layout and mouse
// rules) to WebAssembly and stands in for the two things a browser does not have:
//
//   - the game: a still frame (assets/backdrop.png, a local CS:S capture) or a drawn stand-in,
//     letterboxed into the preview exactly where the game renderer puts the real frame, and turned
//     by the mouse while the game has it;
//   - the demo: a playback clock over the SourceTV test demo's real length and events (stand-in
//     player names), so the demo bar's buttons, dial, scrubber and markers all respond, and a
//     stand-in spectator camera (mode, player, FOV, speed) for the movie director outside the
//     workspaces;
//   - the world: a stand-in camera path ("markers on") ahead of the stand-in camera, drawn by the
//     game's own path overlay (Ui/PathOverlay.h) over the frame, and a stand-in list of its
//     cameras that the PATH CAMERA inspector edits (Ui/PathInspector.h) as the game's path would,
//     with F opening the CAMERA MARKER card and its banner (Ui/MarkerPopup.h) on them; its keys,
//     in CS2's own key model, for the TIMELINE dock (Ui/TimelineDock.h) to edit;
//   - the window: index.html forwards DOM mouse and key events here as the game host's window
//     procedure forwards window messages, and asks for pointer lock while the game has the mouse.
//
// Everything else, from which panel takes a click to when a right press becomes a look, is the
// game's code. Built by build-mockup.mjs.

#include "../AfxHookSource/Filmmaker/SourceDemoBarModel.h"
#include "../AfxHookSource/Filmmaker/Ui/CameraEditorShell.h"
#include "../AfxHookSource/Filmmaker/Ui/DemosBrowser.h"
#include "../AfxHookSource/Filmmaker/Ui/DirectorHud.h"
#include "../AfxHookSource/Filmmaker/Ui/DirectorMath.h"
#include "../AfxHookSource/Filmmaker/Ui/DirectorMouse.h"
#include "../AfxHookSource/Filmmaker/Ui/MarkerPopup.h"
#include "../AfxHookSource/Filmmaker/Ui/PathOverlay.h"
#include "../AfxHookSource/Filmmaker/Ui/UiKeys.h"
#include "../AfxHookSource/Filmmaker/Ui/WorkspaceMouse.h"
#include "../AfxHookSource2/Deadlock/DeadlockOverlayMath.h"
#include "../AfxHookSource2/Filmmaker/Movie/CamMarkers.h"
#include "../AfxHookSource2/Filmmaker/Movie/GraphExpModel.h"
#include "../AfxHookSource2/Filmmaker/Movie/RageShake.h"
#include "../AfxHookSource2/Filmmaker/Movie/RecoilPivotMarkerMath.h"
#include "../AfxHookSource2/Filmmaker/Movie/WeaponPivot.h"
#include "../AfxHookSource2/Filmmaker/Panorama/HudReadoutCadenceMath.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <GLES3/gl3.h>
#include <emscripten.h>
#include <emscripten/html5.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>

using namespace SourceFilmmaker;
using namespace SourceFilmmaker::Ui;

namespace {

using Mode = WorkspaceMouse::Mode;
using Route = WorkspaceMouse::Route;
using DirectorMode = DirectorMouse::Mode;
using DirectorRoute = DirectorMouse::Route;

const char* ModeName(Mode m) {
	switch (m) {
	case Mode::Ui: return "ui";
	case Mode::Game: return "game";
	default: return "look";
	}
}

// The demo the mockup plays: mvm_css_tv.dem's length, tick rate and events (Docs/source/README.md
// has how it was recorded); names and teams are stand-ins.
struct SimDemo {
	bool playing = true;
	bool paused = false;
	double tick = 0;
	int total = 4410;
	double interval = 0.015;
	double timescale = 1;
	int observerMode = DirectorMath::kInEye;
	int target = 0;      // index into spectatable
	double fov = 0;      // Ctrl+wheel's FOV, 0 while the camera's own
	double speed = 1;    // the free cam's speed, x
	bool freeCam = false;   // HLAE's free camera (mirv_input camera) flies the view
	double freeCamSpeed = 1;
	double freeCamFov = 90;
	std::vector<std::string> spectatable = { "Josh", "Xander", "Kurt", "Nate", "Eli" };
	bool loopOn = false;
	DemoBarMath::LoopRegion loop;
	DemoMarkers markers;
	std::unordered_map<int, std::pair<std::string, int>> players; // user id -> name, team
};

DemoFile::Field IntField(const char* key, int value) {
	DemoFile::Field f;
	f.key = key;
	f.value = value;
	return f;
}

DemoFile::Field TextField(const char* key, const char* text) {
	DemoFile::Field f;
	f.key = key;
	f.type = DemoFile::Field::Type::String;
	f.text = text;
	return f;
}

DemoFile::Event SimEvent(int tick, const char* name, std::vector<DemoFile::Field> fields) {
	DemoFile::Event e;
	e.tick = tick;
	e.name = name;
	e.fields = std::move(fields);
	return e;
}

SimDemo MakeSimDemo() {
	SimDemo demo;
	const std::vector<DemoFile::Event> events = {
		SimEvent(490, "player_death", { IntField("userid", 21), IntField("attacker", 15), TextField("weapon", "usp"), IntField("headshot", 1) }),
		SimEvent(490, "round_end", { IntField("winner", 3) }),
		SimEvent(917, "round_start", {}),
		SimEvent(3284, "bomb_planted", { IntField("userid", 21) }),
		SimEvent(4250, "player_death", { IntField("userid", 14), IntField("attacker", 16), TextField("weapon", "m4a1") }),
		SimEvent(4310, "player_death", { IntField("userid", 16), IntField("attacker", 17), TextField("weapon", "mp5navy") }),
	};
	demo.markers = BuildDemoMarkers(events, demo.total);
	demo.players = { { 15, { "Josh", 3 } }, { 21, { "Xander", 2 } }, { 14, { "Kurt", 3 } }, { 16, { "Nate", 2 } }, { 17, { "Eli", 3 } } };
	return demo;
}

struct Host {
	EMSCRIPTEN_WEBGL_CONTEXT_HANDLE gl = 0;
	FontSet fonts;
	CameraEditorState editor;
	WorkspaceMouse mouse;
	DirectorState director;
	DirectorMouse directorMouse;
	DirectorCardsModel cards;   // the cards' readouts, refreshed at CS2's 10 Hz
	Filmmaker::HudReadoutCadence cadence;
	Rect bar;                   // last frame's director bar, client px
	Rect preview;               // last frame's preview, client px
	bool panelHovered = false;  // last frame: the pointer is over a panel
	int width = 854, height = 480;
	double lastMs = 0;
	unsigned frames = 0;
	GLuint backdrop = 0;
	int backdropW = 0, backdropH = 0;
	float yaw = 0, pitch = 0;   // the stand-in game camera, degrees
	bool pathOn = false;        // the stand-in camera path is drawn
	bool pathFreeze = false;    // in Freeze blue, else Live gold
	PathInspectorModel path;    // the cameras the PATH CAMERA tab edits
	FollowInspectorModel follow; // the follow camera the FOLLOW CAMERA tab edits
	std::vector<FollowInspectorCandidate> followPlayers; // the Player list; the Grenade list is StandInThrows
	int followThrowTick = -1;     // the throw Go To Throw waits for, -1 for none
	double followPickedMs = -1e9; // when a target was last picked: its box shows for five seconds
	bool cardOpen = false;      // the CAMERA MARKER card (F)
	bool autoSnap = false;
	MarkerCardState card;
	Filmmaker::GraphExpModel timeline; // the TIMELINE dock's keys
	struct Shake { int s = 0, n = 5, p = 50; };
	std::map<int, Shake> shakes;       // the camera path's shake keys, by tick (StandInShakeCommand)
	unsigned timelineEpoch = 1;        // bumps when the keys are seeded: the dock re-fits its window
	bool timelineDrive = true, timelineAuto = false;
	bool timelineScrubbing = false;    // the dock's playhead is held by a drag
	bool scrubCache = true;            // the dock's Cache
	double timelineTick = 0;
	SimDemo demo = MakeSimDemo();
	ConfigModel config;         // the Config cards' stand-in camera modules (StandInConfigCommand)
	// The demos browser over the stand-in main menu (no demo playing): its library and organiser,
	// answered by StandInDemosCommand as SourceDemos.cpp answers the game's.
	bool demosOpen = false;
	DemosBrowserState demosState;
	DemosBrowserModel demos;
	std::vector<bool> demoHidden;     // by library slot, as the game's hidden list
	std::vector<DemosBrowserMath::DemoRow> library; // every stand-in demo, hidden ones too
	int nextGroup = 1;
	double convertDoneMs = -1;        // an old demo's stand-in conversion finishes then
	int convertIndex = -1;
	Rect menuButton;                  // the main menu's DEMOS button, last frame
	bool menuButtonHover = false;
	std::string reply;          // backing store for strings handed to JavaScript
};
Host g;

// The game host logs these as mvm_debug lines; here they go to the browser console.
void LogLine(const char* channel, const char* text) { std::printf("[%s] %s\n", channel, text); }

// The movie director runs while a demo plays and no workspace or the demos browser is open.
bool DirectorActive() { return !g.editor.open && !g.demosOpen && g.demo.playing; }

// The main menu: no demo plays and nothing is open over it.
bool MainMenu() { return !g.editor.open && !g.demosOpen && !g.demo.playing; }

const char* DirectorModeName(DirectorMode m) { return m == DirectorMode::Look ? "look" : "free"; }

void ApplyMode(Mode before, const char* reason) {
	const Mode now = g.mouse.GetMode();
	if (now == before) return;
	char line[96];
	std::snprintf(line, sizeof(line), "mode=%s reason=%s", ModeName(now), reason);
	LogLine("ui.mouse", line);
}

// Opens a workspace, or swaps the open one for it, as the game host does.
void Open(Workspace ws, const char* reason) {
	const char* name = ws == Workspace::Config ? "config" : "editor";
	g.editor.workspace = ws;
	if (g.editor.open) {
		LogLine("ui.editor", (std::string("workspace=") + name + " reason=" + reason).c_str());
		return;
	}
	g.editor.open = true;
	g.mouse.Reset();
	LogLine("ui.editor", (std::string("open workspace=") + name + " reason=" + reason).c_str());
}

bool ConfigOpen() { return g.editor.open && g.editor.workspace == Workspace::Config; }

void Close(const char* reason) {
	if (!g.editor.open) return;
	g.editor.open = false;
	g.mouse.Reset();
	g.preview = Rect();
	LogLine("ui.editor", (std::string("close reason=") + reason).c_str());
}

void EnsureFonts() {
	const float scale = (float)g.height / kStageHeight;
	if (g.fonts.scale > 0 && std::fabs(g.fonts.scale - scale) < 0.001f) return;
	ImGuiIO& io = ImGui::GetIO();
	ImGui_ImplOpenGL3_DestroyFontsTexture();
	g.fonts = BuildFonts(io.Fonts, "/fonts", scale);
	ApplyStyle(ImGui::GetStyle(), scale);
	ImGui_ImplOpenGL3_CreateFontsTexture();
	int w = 0, h = 0;
	unsigned char* pixels = nullptr;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
	char line[96];
	std::snprintf(line, sizeof(line), "scale=%.3f atlas=%dx%d files=%s", scale, w, h,
		g.fonts.ok ? "ok" : "MISSING (built-in fallback)");
	LogLine("ui.fonts", line);
}

// The game, drawn into r: the backdrop scrolled by the stand-in camera, or a horizon and a floor
// grid when no capture is present. The game renderer letterboxes the real frame into the same rect.
void DrawGame(ImDrawList* list, const Rect& r) {
	const ImVec2 p0(r.x0, r.y0), p1(r.x1, r.y1);
	if (g.backdrop) {
		const float u = g.yaw / 120.0f;
		const float v = g.pitch / 360.0f;
		list->AddImage((ImTextureID)(intptr_t)g.backdrop, p0, p1, ImVec2(u, v), ImVec2(u + 1, v + 1));
		return;
	}
	const float horizon = r.y0 + r.H() * (0.5f + g.pitch / 120.0f);
	const float h = horizon < r.y0 ? r.y0 : (horizon > r.y1 ? r.y1 : horizon);
	list->AddRectFilledMultiColor(p0, ImVec2(r.x1, h), IM_COL32(96, 112, 120, 255), IM_COL32(96, 112, 120, 255),
		IM_COL32(170, 160, 136, 255), IM_COL32(170, 160, 136, 255));
	list->AddRectFilledMultiColor(ImVec2(r.x0, h), p1, IM_COL32(120, 104, 78, 255), IM_COL32(120, 104, 78, 255),
		IM_COL32(64, 54, 40, 255), IM_COL32(64, 54, 40, 255));
	const float step = r.W() / 8.0f;
	const float shift = std::fmod(g.yaw * r.W() / 120.0f, step);
	for (float x = r.x0 - shift - step; x < r.x1 + step; x += step) {
		const float cx = r.x0 + r.W() * 0.5f;
		const float a = x < r.x0 ? r.x0 : (x > r.x1 ? r.x1 : x);
		list->AddLine(ImVec2(a, h), ImVec2(cx + (x - cx) * 3.0f, r.y1), IM_COL32(0, 0, 0, 60), 1.0f);
	}
}

void BuildDemoModel(DemoBarModel& model) {
	const SimDemo& d = g.demo;
	model = DemoBarModel();
	model.demo = d.playing;
	model.paused = d.paused;
	model.tick = (int)d.tick;
	model.lastTick = d.total;
	model.intervalPerTick = d.interval;
	model.timescale = d.timescale;
	model.observed = d.spectatable[(size_t)d.target];
	model.cameraMode = DirectorMath::CameraButtonName(d.observerMode);
	DemoBarFillMarkers(model, d.markers, [](int userId, std::string& name, int& team) {
		auto it = g.demo.players.find(userId);
		if (it == g.demo.players.end()) return false;
		name = it->second.first;
		team = it->second.second;
		return true;
	});
	model.loopOn = d.loopOn;
	model.loop = d.loop;
}

void SeekSim(int tick) {
	g.demo.tick = tick < 0 ? 0 : (tick > g.demo.total ? g.demo.total : tick);
	LogLine("demo.bar", ("seek " + std::to_string((int)g.demo.tick)).c_str());
}

// A camera change drops the Ctrl+wheel FOV, which belongs to the camera it was set in (CS2).
void SetCamera(int mode) {
	if (g.demo.freeCam) {
		g.demo.freeCam = false; // one of the game's cameras takes the view back
		LogLine("director", "free cam off");
	}
	if (mode == g.demo.observerMode) return;
	g.demo.observerMode = mode;
	g.demo.fov = 0;
	LogLine("director", (std::string("camera ") + DirectorMath::CameraButtonName(mode)).c_str());
}

void StepPlayer(int direction) {
	SimDemo& d = g.demo;
	const int n = (int)d.spectatable.size();
	d.target = ((d.target + direction) % n + n) % n;
	LogLine("director", ("spectating " + d.spectatable[(size_t)d.target]).c_str());
}

void DirectorWheel(int notches, bool ctrl, bool shift) {
	SimDemo& d = g.demo;
	char line[96];
	switch (DirectorMath::WheelFor(ctrl, shift, d.observerMode, d.freeCam)) {
	case DirectorMath::Wheel::Camera: SetCamera(DirectorMath::StepCamera(d.observerMode, notches)); return;
	case DirectorMath::Wheel::Fov:
		d.fov = DirectorMath::StepFov(d.fov > 0 ? d.fov : 90, notches);
		std::snprintf(line, sizeof(line), "fov %.0f", d.fov);
		LogLine("director", line);
		return;
	case DirectorMath::Wheel::Speed:
		d.speed = DirectorMath::StepSpeed(d.speed, notches);
		std::snprintf(line, sizeof(line), "speed %.3f", d.speed);
		LogLine("director", line);
		return;
	case DirectorMath::Wheel::FreeCamFov:
		d.freeCamFov -= notches * Filmmaker::kFreeCamFovStepDeg; // up zooms in, as MirvInput::NudgeFov
		std::snprintf(line, sizeof(line), "free cam fov %.0f", d.freeCamFov);
		LogLine("director", line);
		return;
	case DirectorMath::Wheel::FreeCamSpeed:
		d.freeCamSpeed = DirectorMath::StepSpeed(d.freeCamSpeed, notches);
		std::snprintf(line, sizeof(line), "free cam speed %.3f", d.freeCamSpeed);
		LogLine("director", line);
		return;
	case DirectorMath::Wheel::Nothing: return;
	}
}

void DirectorKey(DirectorMath::Key key) {
	SimDemo& d = g.demo;
	switch (key) {
	case DirectorMath::Key::TogglePause: d.paused = !d.paused; LogLine("director", d.paused ? "pause" : "resume"); break;
	case DirectorMath::Key::SkipBack: SeekSim(DirectorMath::SkipTarget((int)d.tick, -1, d.interval, d.total)); break;
	case DirectorMath::Key::SkipForward: SeekSim(DirectorMath::SkipTarget((int)d.tick, 1, d.interval, d.total)); break;
	case DirectorMath::Key::Blackout: {
		DirectorMath::HudState s = { g.director.cards, g.director.blackout };
		s = DirectorMath::PressF8(s);
		g.director.cards = s.cards;
		g.director.blackout = s.blackout;
		LogLine("director", g.director.blackout ? "blackout on" : "blackout off");
		break;
	}
	case DirectorMath::Key::None: break;
	}
}

void BuildCards(DirectorCardsModel& cards) {
	const SimDemo& d = g.demo;
	cards = DirectorCardsModel();
	cards.freeCam = d.freeCam;
	cards.mode = d.freeCam ? "Free cam" : DirectorMath::CameraCardName(d.observerMode);
	cards.playing = d.playing;
	cards.paused = d.paused;
	cards.speed = d.freeCam ? d.freeCamSpeed : d.speed;
	cards.pos[0] = -412.5; cards.pos[1] = 1296.0; cards.pos[2] = 64.0; // a stand-in spot on de_dust2
	cards.ang[0] = g.pitch; cards.ang[1] = g.yaw;
	cards.fov = d.freeCam ? d.freeCamFov : (d.fov > 0 ? d.fov : 90);
	cards.haveTick = true;
	cards.tick = (int)d.tick;
	cards.time = d.tick * d.interval;
}

void ApplyDemoActions(const std::vector<DemoBarAction>& actions) {
	SimDemo& d = g.demo;
	for (const DemoBarAction& a : actions) {
		switch (a.kind) {
		case DemoBarAction::Kind::TogglePause: d.paused = !d.paused; LogLine("demo.bar", d.paused ? "pause" : "resume"); break;
		case DemoBarAction::Kind::Seek: SeekSim(a.tick); break;
		case DemoBarAction::Kind::Scrub: SeekSim(a.tick); break; // the simulated world follows the drag at once
		case DemoBarAction::Kind::Speed: d.timescale = a.value; LogLine("demo.bar", ("timescale " + std::to_string(a.value)).c_str()); break;
		case DemoBarAction::Kind::ToggleLoop:
			d.loopOn = !d.loopOn;
			if (d.loopOn) {
				if (!d.loop.Valid()) d.loop = DemoBarMath::SeedLoop((int)d.tick, d.total);
				if (d.tick < d.loop.in || d.tick >= d.loop.out) SeekSim(d.loop.in);
			}
			LogLine("demo.bar", d.loopOn ? "loop on" : "loop off");
			break;
		case DemoBarAction::Kind::LoopRange:
			d.loop.in = a.tick;
			d.loop.out = (int)a.value;
			LogLine("demo.loop", ("range " + std::to_string(a.tick) + " " + std::to_string((int)a.value)).c_str());
			break;
		case DemoBarAction::Kind::NextCamera: SetCamera(DirectorMath::StepCamera(d.observerMode, 1)); break;
		case DemoBarAction::Kind::CameraEditor: Open(Workspace::Editor, "demo bar"); break;
		case DemoBarAction::Kind::Config: Open(Workspace::Config, "demo bar"); break;
		}
	}
}

void AdvanceDemo(float dt) {
	SimDemo& d = g.demo;
	if (!d.playing || d.paused) return;
	d.tick += dt / d.interval * d.timescale;
	if (DemoBarMath::LoopWraps(d.loop, (int)d.tick, d.loopOn)) SeekSim(d.loop.in);
	if (d.tick >= d.total) {
		d.tick = d.total;
		d.paused = true;
	}
}

// The stand-in camera path: three markers ahead of the stand-in camera (Source axes, X ahead, Z up)
// and a Catmull-Rom line through them; the playhead runs along it with the demo's tick. The game
// fills the same model from its real path (SourceCameraPath.cpp CameraPath_Overlay).
PathOverlayModel StandInPath() {
	PathOverlayModel model;
	const double keys[3][6] = { // x y z pitch yaw roll
		{ 230, 110, 40, 5, -25, 0 }, { 340, 10, 70, 12, 0, 0 }, { 270, -120, 45, 4, 30, 0 } };
	for (const auto& k : keys) {
		PathOverlayMarker m;
		for (int i = 0; i < 3; ++i) { m.pos[i] = k[i]; m.ang[i] = k[3 + i]; }
		m.fov = 106.26; // 90 at 4:3, widened for 16:9
		model.markers.push_back(m);
	}
	const auto at = [&](int i, int c) { return keys[i < 0 ? 0 : (i > 2 ? 2 : i)][c]; };
	for (int seg = 0; seg < 2; ++seg)
		for (int s = seg == 0 ? 0 : 1; s <= 24; ++s) {
			const double t = s / 24.0, t2 = t * t, t3 = t2 * t;
			for (int c = 0; c < 3; ++c) {
				const double p0 = at(seg - 1, c), p1 = at(seg, c), p2 = at(seg + 1, c), p3 = at(seg + 2, c);
				model.trajectory.push_back(0.5 * (2 * p1 + (p2 - p0) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (3 * p1 - p0 - 3 * p2 + p3) * t3));
			}
		}
	const size_t points = model.trajectory.size() / 3;
	const size_t head = (size_t)(g.demo.tick / 8) % points;
	model.head = true;
	for (int c = 0; c < 3; ++c) model.headPos[c] = model.trajectory[head * 3 + c];
	Deadlock::OverlayVec3 aim[3];
	for (int i = 0; i < 3; ++i) aim[i] = { keys[i][0], keys[i][1], keys[i][2] };
	model.aimed = Deadlock::PickAimedMarker({ 0, 0, 64 }, g.pitch, -g.yaw, aim, 3);
	model.freeze = g.pathFreeze;
	return model;
}

// The follow camera's aids, as the game's follow camera publishes them (FollowCamera.cpp): its
// placed Lock-on camera while it is not live, at a stand-in spot looking back at the stand-in
// camera, and a box around the picked target for five seconds, at a stand-in spot ahead.
bool StandInFollowAids(PathOverlayModel& model) {
	const FollowInspectorModel& f = g.follow;
	if (!g.editor.open) return false;
	model.followCamera = f.hasCamera && f.mode == 0 && !f.enabled;
	if (model.followCamera) {
		const double pose[6] = { 300, -60, 110, 12, 160, 0 };
		for (int i = 0; i < 3; ++i) { model.followMarker.pos[i] = pose[i]; model.followMarker.ang[i] = pose[3 + i]; }
		model.followMarker.fov = 106.26;
	}
	model.targetBox = f.targetIndex >= 0 && !f.enabled && emscripten_get_now() - g.followPickedMs < 5000;
	if (model.targetBox && f.type == 1) {
		model.boxPos[0] = 380; model.boxPos[1] = 20; model.boxPos[2] = 21; // around a grenade in the air
		model.boxHalf = 9;
		model.boxHeight = 18;
	} else if (model.targetBox) {
		model.boxPos[0] = 420; model.boxPos[1] = 40; model.boxPos[2] = -20; // a player's feet
		model.boxHalf = 18;
		model.boxHeight = 72;
	}
	return model.followCamera || model.targetBox;
}

bool StandInWorldShown() {
	PathOverlayModel aids;
	return g.pathOn || StandInFollowAids(aids) || (g.config.pvr.orb && g.config.wrOn) || (g.config.pvb.orb && g.config.wbOn);
}

// The path over the game frame in r, as the game lays it over the whole back buffer before its
// letterbox scales the frame into the preview.
// The weapon pivots' markers the Config cards' eye switches show (CS2's RecoilPivotMarker): at the
// stand-in rifle's hinge in front of the stand-in camera, slid out past the near plane with the game's
// own math, while the effect is on.
void StandInPivots(PathOverlayModel& model, const PathOverlayView& view) {
	const double kDeg = 3.14159265358979323846 / 180.0;
	const double p = view.angles[0] * kDeg, y = view.angles[1] * kDeg;
	const double fwd[3] = { std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p) };
	const double right[3] = { std::sin(y), -std::cos(y), 0 };
	const double up[3] = { std::sin(p) * std::cos(y), std::sin(p) * std::sin(y), std::cos(p) };
	for (int owner = 0; owner < 2; ++owner) {
		const ConfigPivot& pv = owner == 0 ? g.config.pvr : g.config.pvb;
		if (!pv.orb || !(owner == 0 ? g.config.wrOn : g.config.wbOn)) continue;
		const double* h = pv.hinge[1]; // the rifle's
		const Filmmaker::RecoilPivotDrawPoint place =
			Filmmaker::RecoilPivotPlaceMarker(h[0], h[1], h[2], Filmmaker::kRecoilPivotMarkerRadius);
		if (!place.visible) continue;
		PathOverlayPivot m;
		for (int i = 0; i < 3; ++i) m.pos[i] = view.origin[i] + fwd[i] * place.fwd + right[i] * place.right + up[i] * place.up;
		m.radius = place.radius;
		const Filmmaker::WeaponPivotColour c = Filmmaker::WeaponPivotOwnerColour((Filmmaker::WeaponPivotOwner)owner);
		m.rgb[0] = c.r;
		m.rgb[1] = c.g;
		m.rgb[2] = c.b;
		model.pivots.push_back(m);
	}
}

void DrawStandInPath(ImDrawList& list, const Rect& r, ImFont* digits) {
	PathOverlayView view;
	view.origin[2] = 64;
	view.angles[0] = g.pitch; // the backdrop scrolls up as the stand-in camera looks down
	view.angles[1] = -g.yaw;  // and left as it turns right
	view.width = (float)g.width;
	view.height = (float)g.height;
	view.fov = 2.0 * std::atan(std::tan(45.0 * 3.14159265358979323846 / 180.0) * (view.width / view.height) * 0.75) * 180.0 / 3.14159265358979323846;
	const int first = list.VtxBuffer.Size;
	PathOverlayModel model = g.pathOn ? StandInPath() : PathOverlayModel();
	StandInFollowAids(model);
	StandInPivots(model, view);
	DrawPathOverlay(list, model, view, digits);
	const float sx = r.W() / view.width, sy = r.H() / view.height;
	for (int i = first; i < list.VtxBuffer.Size; ++i) {
		ImDrawVert& v = list.VtxBuffer[i];
		v.pos = ImVec2(r.x0 + v.pos.x * sx, r.y0 + v.pos.y * sy);
	}
}

// ---- the FOLLOW CAMERA tab's stand-in -------------------------------------------------------
// The players of the stand-in demo as the follow camera lists them (FollowCameraStateJson.cpp):
// nearest first, with CS2's distance levels. The tab's commands (mirv_filmmaker follow ...) are
// applied here as FollowCommand.cpp applies them to the follow camera.

int DistanceLevel(double d) { return d >= 7000 ? 3 : (d >= 3500 ? 2 : (d >= 1800 ? 1 : 0)); }

void SeedFollow() {
	FollowInspectorModel& f = g.follow;
	f = FollowInspectorModel();
	f.typeName = "Player";
	f.look = 0.10;
	f.position = 0.04;
	f.maxTurn = 720;
	f.deathHold = 3;
	f.autoDead = true;
	f.holdLast = true;
	f.attachment = "head";
	struct P { int index; const char* name; const char* team; bool alive; double distance; };
	const P players[] = {
		{ 4, "Josh", "CT", true, 345 }, { 5, "Brad", "CT", true, 568 }, { 12, "Mike", "CT", true, 1210 },
		{ 9, "Ted", "CT", true, 2150 }, { 3, "Chris", "T", true, 3246 }, { 6, "Greg", "T", true, 3466 },
		{ 11, "Jason", "T", true, 3532 }, { 8, "Doug", "T", false, 3480 }, { 10, "Xander", "T", false, 7420 },
	};
	for (const P& p : players) {
		FollowInspectorPlayer pl;
		pl.index = p.index;
		pl.name = p.name;
		pl.team = p.team;
		pl.alive = p.alive;
		f.players.push_back(pl);
		FollowInspectorCandidate c;
		c.index = p.index;
		c.handle = 0x100000u + (uint64_t)p.index;
		c.name = p.name;
		c.className = "CCSPlayer";
		c.team = p.team;
		c.alive = p.alive;
		c.status = p.alive ? "alive" : "dead";
		c.distance = p.distance;
		c.distanceLevel = DistanceLevel(p.distance);
		// The mounts the game offers on a CS:S player (SourceFollowCamera.cpp kPlayerMounts). A dead
		// player keeps them all on its ragdoll, its eyes on the ragdoll's head.
		const char* const mounts[][3] = { { "entity", "Entity", "entity" }, { "eyes", "Eyes", "virtual" },
			{ "head", "Head", "body-mount" }, { "chest", "Chest", "body-mount" }, { "pelvis", "Pelvis", "body-mount" },
			{ "feet", "Feet", "virtual" }, { "weapon_hand_R", "Right hand", "attachment" }, { "weapon_hand_L", "Left hand", "attachment" } };
		for (const auto& m : mounts) {
			FollowInspectorPoint point;
			point.id = m[0];
			point.label = m[1];
			point.kind = m[2];
			point.valid = true;
			c.attachPoints.push_back(point);
		}
		f.candidates.push_back(c);
	}
	g.followPlayers = f.candidates;
	f.eventStatus = 3;
	f.eventError = "no demo event scan for this game yet";
}

// The Grenade list. The stand-in demo has no grenades (neither has mvm_css_tv.dem), so these are
// placed in it the way a round's grenades fall, and listed as the game's grenade index lists them
// (SourceFollowCamera.cpp FollowHost_GrenadeSightings): the ones played past, in the air now or seen
// earlier, nearest the playhead first.
struct StandInThrow {
	uint64_t handle;
	int tick;
	const char* type;
	const char* thrower;
	const char* team;
	int flightTicks; // how long it is in the world
};
const StandInThrow kStandInThrows[] = {
	{ 0x20005a, 640, "Flashbang", "Chris", "T", 110 },
	{ 0x20005c, 1180, "HE Grenade", "Brad", "CT", 110 },
	{ 0x20005d, 1215, "Smoke", "Greg", "T", 1300 },
	{ 0x200061, 3020, "HE Grenade", "Jason", "T", 110 },
};

void StandInThrows(FollowInspectorModel& f) {
	f.candidates.clear();
	const int now = (int)g.demo.tick;
	// The index remembers what it has seen, so a seek back still lists the throws played past.
	static int seenUpTo = 0;
	if (now > seenUpTo) seenUpTo = now;
	for (const StandInThrow& t : kStandInThrows) {
		if (t.tick > seenUpTo) continue;
		FollowInspectorCandidate c;
		const bool active = now <= t.tick + t.flightTicks;
		c.index = active ? (int)(t.handle & 0xFFF) : -1;
		c.handle = t.handle;
		c.name = t.type;
		c.grenadeType = t.type;
		c.className = "CBaseCSGrenadeProjectile";
		c.ownerName = t.thrower;
		c.team = t.team;
		c.throwTick = t.tick;
		c.tickDelta = t.tick - now;
		c.status = active ? "active projectile" : "recorded nearby";
		c.alive = active;
		FollowInspectorPoint entity;
		entity.id = "entity";
		entity.label = "Entity";
		entity.kind = "entity";
		entity.valid = true;
		c.attachPoints.push_back(entity);
		f.candidates.push_back(c);
	}
	std::sort(f.candidates.begin(), f.candidates.end(), [](const FollowInspectorCandidate& a, const FollowInspectorCandidate& b) {
		const int ad = std::abs(a.tickDelta), bd = std::abs(b.tickDelta);
		return ad == bd ? a.throwTick < b.throwTick : ad < bd;
	});
}

// Each frame: the type's list, and a Go To Throw that waits for its throw.
void StandInFollowFrame() {
	FollowInspectorModel& f = g.follow;
	if (f.type == 1) StandInThrows(f);
	else f.candidates = g.followPlayers;
	if (g.followThrowTick >= 0 && (int)g.demo.tick >= g.followThrowTick) {
		f.grenadePending = false;
		f.message = "Tracking " + f.targetName + " from throw tick " + std::to_string(g.followThrowTick) + ".";
		g.followThrowTick = -1;
	}
}

const FollowInspectorCandidate* FollowCandidate(int index) {
	for (const FollowInspectorCandidate& c : g.follow.candidates)
		if (c.index == index) return &c;
	return nullptr;
}

void FollowSelect(const FollowInspectorCandidate& c) {
	FollowInspectorModel& f = g.follow;
	g.followPickedMs = emscripten_get_now();
	f.targetIndex = c.index;
	f.targetHandle = c.handle;
	f.targetValid = true;
	f.targetName = c.name;
	f.targetDistance = c.distance;
	f.distanceLevel = c.distanceLevel;
	f.attachPoints = c.attachPoints;
	f.message = "Selected " + c.name + ".";
}

void ApplyFollowCommands(const std::vector<std::string>& commands) {
	FollowInspectorModel& f = g.follow;
	for (const std::string& line : commands) {
		LogLine("follow.ui", line.c_str());
		const std::string prefix = "mirv_filmmaker follow ";
		if (line.compare(0, prefix.size(), prefix) != 0) continue;
		const std::string rest = line.substr(prefix.size());
		const size_t space = rest.find(' ');
		const std::string verb = rest.substr(0, space), arg = space == std::string::npos ? "" : rest.substr(space + 1);
		const double v = std::atof(arg.c_str());
		if (verb == "mode") f.mode = arg == "attach" ? 1 : 0;
		else if (verb == "place") { f.hasCamera = true; f.message = "Follow Camera placed @ tick " + std::to_string((int)g.demo.tick) + "."; }
		else if (verb == "reposition") f.repositioning = !f.repositioning;
		else if (verb == "clear") { f.hasCamera = false; f.enabled = false; f.targetIndex = -1; f.targetValid = false; f.targetName.clear(); f.message = "Follow Camera cleared."; }
		else if (verb == "preview") {
			if (f.mode == 0 && !f.hasCamera) f.message = "Place the camera first, or switch to Attach mode.";
			else if (f.targetIndex < 0) f.message = "Select a target first.";
			else f.enabled = true;
		}
		else if (verb == "stop") { f.enabled = false; f.message = "Tracking stopped: user."; }
		else if (verb == "type") {
			const int type = arg == "grenade" ? 1 : (arg == "weapon" ? 2 : 0);
			if (type != f.type) {
				// As FollowCamera::SetTargetType: a new type starts with no target.
				f.targetIndex = -1;
				f.targetHandle = 0;
				f.targetValid = false;
				f.targetName.clear();
				f.grenadePending = false;
				g.followThrowTick = -1;
				f.attachment = type == 0 ? "head" : "entity";
			}
			f.type = type;
			f.typeName = f.type == 1 ? "Grenade" : (f.type == 2 ? "Weapon / C4" : "Player");
			StandInFollowFrame();
		}
		else if (verb == "select") { if (const FollowInspectorCandidate* c = FollowCandidate(std::atoi(arg.c_str()))) FollowSelect(*c); }
		else if (verb == "selecthandle") {
			const uint64_t handle = std::strtoull(arg.c_str(), nullptr, 10);
			for (const FollowInspectorCandidate& c : f.candidates)
				if (c.handle == handle) { FollowSelect(c); break; }
		}
		else if (verb == "nearest") {
			for (const FollowInspectorCandidate& c : f.candidates)
				if (c.alive || f.type == 1) { FollowSelect(c); break; }
		}
		else if (verb == "previewtick" && f.type == 1) {
			// As FollowCamera::TrackSelectedGrenade: seek 40 ticks before the throw, then wait for it.
			const FollowInspectorCandidate* picked = nullptr;
			for (const FollowInspectorCandidate& c : f.candidates)
				if (f.targetValid && c.handle == f.targetHandle) picked = &c;
			if (!f.hasCamera) f.message = "Place the Follow Camera before tracking a grenade.";
			else if (!picked) f.message = "Select a grenade from the nearby list first.";
			else {
				const int seek = picked->throwTick - 40 < 0 ? 0 : picked->throwTick - 40;
				g.followThrowTick = picked->throwTick;
				f.grenadePending = true;
				f.enabled = true;
				SeekSim(seek);
				g.demo.paused = false;
				f.message = "Seeking to tick " + std::to_string(seek) + ", 40 ticks before the throw.";
			}
		}
		else if (verb == "goto") f.message = "Moved the camera to the selected target.";
		else if (verb == "weaponplayer") { f.weaponPlayerIndex = f.weaponFilterPlayer = std::atoi(arg.c_str()); f.message = "Weapon target scoped to the selected player."; }
		else if (verb == "weaponsource") f.weaponSource = arg == "held" ? 1 : (arg == "dropped" ? 2 : 0);
		else if (verb == "offsetx") f.offset[0] = v;
		else if (verb == "offsety") f.offset[1] = v;
		else if (verb == "offsetz") f.offset[2] = v;
		else if (verb == "rotpitch") f.rotation[0] = v;
		else if (verb == "rotyaw") f.rotation[1] = v;
		else if (verb == "rotroll") f.rotation[2] = v;
		else if (verb == "fov") f.fov = v;
		else if (verb == "look") f.look = v;
		else if (verb == "position") f.position = v;
		else if (verb == "prediction") f.prediction = v;
		else if (verb == "deadzone") f.deadzone = v;
		else if (verb == "maxturn") f.maxTurn = v;
		else if (verb == "autodead") f.autoDead = v != 0;
		else if (verb == "deathhold") f.deathHold = v;
		else if (verb == "switchweapon") f.switchWeapon = v != 0;
		else if (verb == "switchbomb") f.switchBomb = v != 0;
		else if (verb == "hold") f.holdLast = v != 0;
		else if (verb == "bone") f.attachment = arg;
		else if (verb == "reset") {
			if (arg == "framing") { f.offset[0] = f.offset[1] = f.offset[2] = 0; f.rotation[0] = f.rotation[1] = f.rotation[2] = 0; f.fov = 90; }
			else { f.look = 0.10; f.position = 0.04; f.prediction = 0; f.deadzone = 0; f.maxTurn = 720; f.autoDead = true; f.deathHold = 3; f.switchWeapon = f.switchBomb = false; f.holdLast = true; }
		}
	}
}

// The PATH CAMERA tab's requests, applied to the stand-in list as SourceCameraPath.cpp applies them
// to the game's path.
void ApplyPathActions(const std::vector<PathInspectorAction>& actions) {
	using Kind = PathInspectorAction::Kind;
	PathInspectorModel& p = g.path;
	const int count = (int)p.keys.size();
	for (const PathInspectorAction& a : actions) {
		const bool valid = a.index >= 0 && a.index < count;
		switch (a.kind) {
		case Kind::Select: if (valid) { p.selected = a.index; SeekSim(p.keys[(size_t)a.index].tick); } break;
		case Kind::SelectDelta:
			if (count > 0) {
				int next = (p.selected < 0 ? 0 : p.selected) + a.index;
				p.selected = next < 0 ? 0 : (next >= count ? count - 1 : next);
				SeekSim(p.keys[(size_t)p.selected].tick);
			}
			break;
		case Kind::Add: {
			PathInspectorKey k;
			k.tick = (int)g.demo.tick;
			k.fov = g.demo.freeCamFov;
			size_t at = 0;
			while (at < p.keys.size() && p.keys[at].tick < k.tick) ++at;
			if (at < p.keys.size() && p.keys[at].tick == k.tick) { p.selected = (int)at; break; } // one per tick
			p.keys.insert(p.keys.begin() + (long)at, k);
			p.selected = (int)at;
			break;
		}
		case Kind::ClearAll: p.keys.clear(); p.selected = -1; break;
		case Kind::Delete:
			if (valid) {
				p.keys.erase(p.keys.begin() + a.index);
				if (p.selected >= (int)p.keys.size()) p.selected = (int)p.keys.size() - 1;
			}
			break;
		case Kind::MoveToPlayhead:
			if (valid) {
				int t = a.tick;
				if (a.index > 0 && t <= p.keys[(size_t)a.index - 1].tick) t = p.keys[(size_t)a.index - 1].tick + 1;
				if (a.index < count - 1 && t >= p.keys[(size_t)a.index + 1].tick) t = p.keys[(size_t)a.index + 1].tick - 1;
				p.keys[(size_t)a.index].tick = t;
			}
			break;
		case Kind::EditValue:
			if (valid) (a.channel == 6 ? p.keys[(size_t)a.index].fov : p.keys[(size_t)a.index].roll) = a.value;
			break;
		case Kind::Reposition: p.repositioning = valid; break;
		case Kind::Interp: p.interp = a.index; break;
		case Kind::Ease: if (valid) p.keys[(size_t)a.index].ease = (int)a.value; break;
		case Kind::SpeedMode: p.speedMode = a.index; break;
		case Kind::SpeedMul: if (valid && a.index < count - 1) p.keys[(size_t)a.index].speedMul = (float)a.value; break;
		case Kind::ConstSpeed: p.constSpeed = (float)a.value; break;
		case Kind::Timing: p.timing = a.index; break;
		default: break;
		}
		char line[96];
		std::snprintf(line, sizeof(line), "kind=%d index=%d value=%.2f", (int)a.kind, a.index, a.value);
		LogLine("path.inspector", line);
	}
}

// The CAMERA MARKER card's requests on the stand-in list, as SourceUi.cpp applies them to the path.
void ApplyCardActions(const std::vector<MarkerCardAction>& actions) {
	using Kind = MarkerCardAction::Kind;
	PathInspectorModel& p = g.path;
	const int count = (int)p.keys.size();
	for (const MarkerCardAction& a : actions) {
		switch (a.kind) {
		case Kind::Previous: if (count) p.selected = p.selected > 0 ? p.selected - 1 : 0; break;
		case Kind::Next: if (count) p.selected = p.selected < count - 1 ? p.selected + 1 : count - 1; break;
		case Kind::CycleSpeedMode: p.speedMode = (p.speedMode + 1) % 3; break;
		case Kind::CycleInterp: p.interp = 1 - p.interp; break;
		case Kind::ToggleTiming: p.timing = 1 - p.timing; break;
		case Kind::ToggleAutoSnap: g.autoSnap = !g.autoSnap; break;
		case Kind::ConstSpeed: p.constSpeed = a.value; break;
		case Kind::SpeedMul: if (p.selected >= 0 && p.selected < count) p.keys[(size_t)p.selected].speedMul = a.value; break;
		case Kind::Reposition: p.repositioning = p.selected >= 0; g.cardOpen = false; break;
		case Kind::Remove:
			if (p.selected >= 0 && p.selected < count) {
				p.keys.erase(p.keys.begin() + p.selected);
				if (p.selected >= (int)p.keys.size()) p.selected = (int)p.keys.size() - 1;
			}
			if (p.keys.empty()) g.cardOpen = false;
			break;
		case Kind::RemoveAll: p.keys.clear(); p.selected = -1; g.cardOpen = false; break;
		case Kind::Close: g.cardOpen = false; break;
		default: break;
		}
		char line[64];
		std::snprintf(line, sizeof(line), "kind=%d value=%.2f", (int)a.kind, a.value);
		LogLine("path.card", line);
	}
}

// The stand-in path's keys for the TIMELINE dock: the three cameras StandInPath draws, at the ticks
// the PATH CAMERA tab lists, seeded the way the game seeds the dock from its path.
void SeedTimeline() {
	const double keys[3][7] = { { 230, 110, 40, 5, -25, 0, 90 }, { 340, 10, 70, 12, 0, 0, 75 }, { 270, -120, 45, 4, 30, 0, 90 } };
	std::vector<Filmmaker::CamMarker> markers;
	for (size_t i = 0; i < 3 && i < g.path.keys.size(); ++i) {
		Filmmaker::CamMarker m;
		m.x = keys[i][0]; m.y = keys[i][1]; m.z = keys[i][2];
		m.pitch = keys[i][3]; m.yaw = keys[i][4]; m.roll = keys[i][5]; m.fov = keys[i][6];
		m.tick = g.path.keys[i].tick;
		markers.push_back(m);
	}
	g.timeline.SeedFromMarkers(markers);
	if (g.path.interp == 1) g.timeline.SetInterpAll(true, false);
	++g.timelineEpoch;
}

// The stand-in camera keys: the ticks of the stand-in path's keys.
std::vector<double> StandInCameraTicks() {
	std::vector<double> ticks;
	for (const auto& key : g.timeline.GetChannel(0).keys) ticks.push_back(std::round(key.tick));
	std::sort(ticks.begin(), ticks.end());
	ticks.erase(std::unique(ticks.begin(), ticks.end()), ticks.end());
	return ticks;
}

// The Config cards' commands on the stand-in camera modules, as the game's handlers apply them
// (SmoothView, HandheldCam and WeaponBob's RunCommand): "<setting> <value> quiet" clamps to the
// setting's range, "<switch> toggle" flips it, reset puts the module back. Logged as config.ui.
void StandInConfigCommand(const std::string& command) {
	LogLine("config.ui", command.c_str());
	std::istringstream in(command);
	std::vector<std::string> t;
	for (std::string w; in >> w;) t.push_back(w);
	if (t.size() < 3 || t[0] != "mirv_filmmaker") return;
	ConfigModel& m = g.config;
	const ConfigModel d;
	const std::string& module = t[1];
	const std::string& a = t[2];
	const double v = t.size() > 3 ? std::atof(t[3].c_str()) : 0;
	auto set = [&](double& field, double lo, double hi) { field = std::clamp(v, lo, hi); };
	auto resetSmoothView = [&]() {
		m.svOn = d.svOn; m.svFalling = d.svFalling; m.svJumping = d.svJumping; m.svHealthSway = d.svHealthSway;
		m.svTiltEnabled = d.svTiltEnabled; m.svTiltInv = d.svTiltInv; m.svBreathX = d.svBreathX; m.svBreathY = d.svBreathY;
		m.svStandAmp = d.svStandAmp; m.svWalkAmp = d.svWalkAmp; m.svRunAmp = d.svRunAmp; m.svTilt = d.svTilt; m.svTiltSpeed = d.svTiltSpeed;
	};
	auto resetHandheld = [&]() {
		m.handheldOn = d.handheldOn; m.hhVmDecoupled = d.hhVmDecoupled; m.hhCenterLockOn = d.hhCenterLockOn;
		m.hhCenterLock = d.hhCenterLock; m.hhDrift = d.hhDrift; m.hhDriftDir = d.hhDriftDir; m.hhTilt = d.hhTilt;
		m.hhVariance = d.hhVariance; m.hhCurve = d.hhCurve; m.hhSpeed = d.hhSpeed; m.hhRegrip = d.hhRegrip; m.hhPeriod = d.hhPeriod;
		m.hhIdle = d.hhIdle; m.hhWalk = d.hhWalk; m.hhRun = d.hhRun;
	};
	auto resetWeaponBob = [&]() {
		m.wbOn = d.wbOn; m.wbAmount = d.wbAmount; m.wbSway = d.wbSway; m.wbTilt = d.wbTilt; m.wbRate = d.wbRate;
		m.wbStep = d.wbStep; m.wbLook = d.wbLook; m.wbInertia = d.wbInertia; m.wbIdle = d.wbIdle; m.wbCam = d.wbCam;
	};
	if (module == "smoothview") {
		if (a == "toggle") m.svOn = !m.svOn;
		else if (a == "reset") resetSmoothView();
		else if (a == "falling") m.svFalling = !m.svFalling;
		else if (a == "jumping") m.svJumping = !m.svJumping;
		else if (a == "healthSway") m.svHealthSway = !m.svHealthSway;
		else if (a == "tiltEnabled") m.svTiltEnabled = !m.svTiltEnabled;
		else if (a == "tiltInv") m.svTiltInv = !m.svTiltInv;
		else if (a == "breathX") set(m.svBreathX, 0.1, 2);
		else if (a == "breathY") set(m.svBreathY, 0.1, 2);
		else if (a == "standAmp") set(m.svStandAmp, 0, 2);
		else if (a == "walkAmp") set(m.svWalkAmp, 0, 2);
		else if (a == "runAmp") set(m.svRunAmp, 0, 2);
		else if (a == "tilt") set(m.svTilt, 0, 60);
		else if (a == "tiltSpeed") set(m.svTiltSpeed, 0.5, 30);
	} else if (module == "handheld") {
		if (a == "toggle") {
			m.handheldOn = !m.handheldOn;
			if (m.handheldOn && m.hhDrift == 0 && m.hhCurve == 0) { m.hhDrift = 35; m.hhCurve = 75; } // the backend's seed
		} else if (a == "reset") resetHandheld();
		else if (a == "viewmodel") m.hhVmDecoupled = !m.hhVmDecoupled;
		else if (a == "centerlock") m.hhCenterLockOn = !m.hhCenterLockOn;
		else if (a == "centerlockpct") set(m.hhCenterLock, 0, 100);
		else if (a == "drift") set(m.hhDrift, 0, 150);
		else if (a == "driftdir") m.hhDriftDir = std::fmod(std::fmod(v, 360) + 360, 360);
		else if (a == "tilt") set(m.hhTilt, 0, 150);
		else if (a == "variance") set(m.hhVariance, 0, 100);
		else if (a == "curve") set(m.hhCurve, 0, 150);
		else if (a == "speed") set(m.hhSpeed, 25, 300);
		else if (a == "regrip") set(m.hhRegrip, 0, 150);
		else if (a == "period") set(m.hhPeriod, 2, 60);
		else if (a == "preset" && t.size() > 4) (t[3] == "idle" ? m.hhIdle : (t[3] == "walk" ? m.hhWalk : m.hhRun)) = t[4];
	} else if (module == "weaponbob") {
		if (a == "toggle") m.wbOn = !m.wbOn;
		else if (a == "reset") resetWeaponBob();
		else if (a == "amount") set(m.wbAmount, 0, 150);
		else if (a == "sway") set(m.wbSway, 0, 150);
		else if (a == "tilt") set(m.wbTilt, 0, 150);
		else if (a == "looksway") set(m.wbLook, 0, 150);
		else if (a == "inertia") set(m.wbInertia, 0, 150);
		else if (a == "idle") set(m.wbIdle, 0, 150);
		else if (a == "camera") set(m.wbCam, 0, 100);
		else if (a == "stepsync") set(m.wbStep, 0, 100);
		else if (a == "rate") set(m.wbRate, 25, 400);
	} else if (module == "weaponrecoil") {
		auto resetRecoil = [&]() {
			m.wrOn = d.wrOn; m.wrAmount = d.wrAmount; m.wrPunch = d.wrPunch; m.wrRot = d.wrRot; m.wrSnap = d.wrSnap;
			m.wrRecovery = d.wrRecovery; m.wrCamRot = d.wrCamRot; m.wrCamPos = d.wrCamPos; m.wrCamRoll = d.wrCamRoll;
			m.wrStyle = d.wrStyle; m.wrScope = d.wrScope; m.wrScopeKey = d.wrScopeKey; m.wrScopeOwn = false; m.wrOverrides = 0;
		};
		auto knob = [&](double& field, double lo, double hi) {
			set(field, lo, hi);
			if (m.wrScope != "all" && !m.wrScopeOwn) { m.wrScopeOwn = true; ++m.wrOverrides; } // the first write makes the override
		};
		if (a == "toggle") m.wrOn = !m.wrOn;
		else if (a == "reset") resetRecoil();
		else if (a == "style" && t.size() > 3) m.wrStyle = t[3];
		else if (a == "scope" && t.size() > 3) {
			m.wrScope = t[3];
			m.wrScopeKey = m.wrScope == "all" || t.size() < 5 || t[4] == "quiet" ? std::string() : t[4];
			m.wrScopeOwn = false;
		} else if (a == "scopeclear" && m.wrScopeOwn) { m.wrScopeOwn = false; --m.wrOverrides; }
		else if (a == "amount") knob(m.wrAmount, 0, 150);
		else if (a == "punch") knob(m.wrPunch, 0, 150);
		else if (a == "rotation") knob(m.wrRot, 0, 150);
		else if (a == "snap") knob(m.wrSnap, 25, 400);
		else if (a == "recovery") knob(m.wrRecovery, 25, 400);
		else if (a == "camerarot") knob(m.wrCamRot, 0, 100);
		else if (a == "camerapos") knob(m.wrCamPos, 0, 100);
		else if (a == "cameraroll") knob(m.wrCamRoll, 0, 100);
		else if (a == "preset" && t.size() > 4) {
			// The name is quoted and may hold spaces: everything after the verb, quotes stripped.
			std::string name = command.substr(command.find(t[3]) + t[3].size());
			name.erase(std::remove(name.begin(), name.end(), '"'), name.end());
			while (!name.empty() && name.front() == ' ') name.erase(name.begin());
			auto at = std::find_if(m.wrPresets.begin(), m.wrPresets.end(), [&](const ConfigPreset& p) { return p.name == name; });
			if (t[3] == "save") {
				if (at == m.wrPresets.end()) m.wrPresets.push_back({ name, m.wrScope, m.wrScopeKey });
			} else if (t[3] == "delete" && at != m.wrPresets.end()) {
				m.wrPresets.erase(at);
			}
		}
	} else if (module == "pivotorb" && t.size() > 3) {
		ConfigPivot& pv = t[2] == "bob" ? m.pvb : m.pvr;
		if (t[3] == "toggle") pv.orb = !pv.orb;
		else if (t[3] == "on") pv.orb = true;
		else if (t[3] == "off") pv.orb = false;
	} else if (module == "pivot" && t.size() > 3) {
		ConfigPivot& pv = t[2] == "bob" ? m.pvb : m.pvr;
		static const char* const kScopes[8] = { "all", "rifle", "smg", "pistol", "shotgun", "sniper", "knife", "utility" };
		if (t[3] == "reset") pv = ConfigPivot();
		else if (t[3] == "set" && t.size() > 6) {
			for (int i = 0; i < 8; ++i)
				if (t[4] == kScopes[i]) pv.hinge[i][t[5] == "fwd" ? 0 : (t[5] == "right" ? 1 : 2)] = std::clamp(std::atof(t[6].c_str()), -30.0, 30.0);
			pv.atDefault = false;
		}
	} else if (module == "effects" && a == "keep" && t.size() > 4) {
		m.keepCamera = t[4] == "on" || (t[4] == "toggle" && !m.keepCamera);
	} else if (module == "effects" && a == "reset") {
		resetSmoothView();
		resetHandheld();
		resetWeaponBob();
		m.wrOn = d.wrOn; m.wrAmount = d.wrAmount; m.wrPunch = d.wrPunch; m.wrRot = d.wrRot; m.wrSnap = d.wrSnap;
		m.wrRecovery = d.wrRecovery; m.wrCamRot = d.wrCamRot; m.wrCamPos = d.wrCamPos; m.wrCamRoll = d.wrCamRoll;
		m.wrStyle = d.wrStyle; m.wrScope = d.wrScope; m.wrScopeKey = d.wrScopeKey; m.wrScopeOwn = false; m.wrOverrides = 0;
		m.pvr = ConfigPivot();
		m.pvb = ConfigPivot();
	}
	// The cards' Reset buttons dim while a module is at its defaults.
	m.smoothViewDefault = m.svOn == d.svOn && m.svFalling == d.svFalling && m.svJumping == d.svJumping
		&& m.svHealthSway == d.svHealthSway && m.svTiltEnabled == d.svTiltEnabled && m.svTiltInv == d.svTiltInv
		&& m.svBreathX == d.svBreathX && m.svBreathY == d.svBreathY && m.svStandAmp == d.svStandAmp && m.svWalkAmp == d.svWalkAmp
		&& m.svRunAmp == d.svRunAmp && m.svTilt == d.svTilt && m.svTiltSpeed == d.svTiltSpeed;
	m.handheldDefault = m.handheldOn == d.handheldOn && m.hhVmDecoupled == d.hhVmDecoupled && m.hhCenterLockOn == d.hhCenterLockOn
		&& m.hhCenterLock == d.hhCenterLock && m.hhDrift == d.hhDrift && m.hhDriftDir == d.hhDriftDir && m.hhTilt == d.hhTilt
		&& m.hhVariance == d.hhVariance && m.hhCurve == d.hhCurve && m.hhSpeed == d.hhSpeed && m.hhRegrip == d.hhRegrip
		&& m.hhPeriod == d.hhPeriod && m.hhIdle == d.hhIdle && m.hhWalk == d.hhWalk && m.hhRun == d.hhRun;
	m.weaponBobDefault = m.wbOn == d.wbOn && m.wbAmount == d.wbAmount && m.wbSway == d.wbSway && m.wbTilt == d.wbTilt
		&& m.wbRate == d.wbRate && m.wbStep == d.wbStep && m.wbLook == d.wbLook && m.wbInertia == d.wbInertia && m.wbIdle == d.wbIdle
		&& m.wbCam == d.wbCam && m.pvb.atDefault;
	m.weaponRecoilDefault = m.wrOn == d.wrOn && m.wrAmount == d.wrAmount && m.wrPunch == d.wrPunch && m.wrRot == d.wrRot
		&& m.wrSnap == d.wrSnap && m.wrRecovery == d.wrRecovery && m.wrCamRot == d.wrCamRot && m.wrCamPos == d.wrCamPos
		&& m.wrCamRoll == d.wrCamRoll && m.wrStyle == d.wrStyle && m.wrOverrides == 0 && m.pvr.atDefault;
}

// The dock's camera shake edits on the stand-in track, as "mirv_filmmaker pathshake" applies them
// (CamPathShakeCommand.cpp): a key sits on a camera key; set, intensity, speed, stop, clear, applyall.
void StandInShakeCommand(const std::string& text) {
	LogLine("timeline.shake", ("pathshake " + text).c_str());
	char verb[16] = {};
	int tick = 0, a = 0, b = -1, c = -1;
	const int got = std::sscanf(text.c_str(), "%15s @%d %d %d %d", verb, &tick, &a, &b, &c);
	if (got < 2) return;
	const std::vector<double> cams = StandInCameraTicks();
	if (std::find(cams.begin(), cams.end(), (double)tick) == cams.end()) return;
	const std::string v = verb;
	auto& shakes = g.shakes;
	if (v == "clear") { shakes.erase(tick); return; }
	auto seeded = [&]() -> Host::Shake& {
		auto it = shakes.find(tick);
		if (it == shakes.end() || it->second.s < 0) {
			Host::Shake fresh;
			fresh.s = Filmmaker::RageShake_MenuStyle(0);
			it = shakes.insert_or_assign(tick, fresh).first;
		}
		return it->second;
	};
	if (v == "set" && got >= 3) {
		Host::Shake& k = seeded();
		k.s = a;
		if (got >= 4) k.n = std::clamp(b, 1, 10);
		if (got >= 5) k.p = std::clamp(c, 10, 100);
	} else if (v == "intensity" && got >= 3) seeded().n = std::clamp(a, 1, 10);
	else if (v == "speed" && got >= 3) seeded().p = std::clamp(a, 10, 100);
	else if (v == "stop") { Host::Shake stop; stop.s = TimelineDockMath::kShakeStop; shakes[tick] = stop; }
	else if (v == "applyall" && shakes.count(tick)) {
		const Host::Shake from = shakes[tick];
		for (double t : cams) shakes[(int)t] = from;
	}
}

TimelineDockModel BuildTimelineModel() {
	TimelineDockModel m;
	m.keys = &g.timeline;
	m.demo = g.demo.playing;
	m.configOpen = g.editor.workspace == Workspace::Config;
	m.viewEpoch = g.timelineEpoch;
	if (g.timeline.TotalKeys() > 0) {
		const double lo = g.timeline.MinTick(), hi = std::max(g.timeline.MaxTick(), lo + 1), pad = (hi - lo) * 0.04 + 64;
		m.viewT0 = std::max(0.0, lo - pad);
		m.viewT1 = hi + pad;
	} else {
		m.viewT0 = g.demo.tick - 1000;
		m.viewT1 = g.demo.tick + 1000;
	}
	m.domainT0 = 0;
	m.domainT1 = g.demo.total;
	m.playhead = g.timelineScrubbing ? g.timelineTick : g.demo.tick;
	m.scrubbing = g.timelineScrubbing;
	m.playing = g.demo.playing && !g.demo.paused;
	m.drive = g.timelineDrive;
	m.autoPath = g.timelineAuto;
	m.smoothPath = g.path.interp == 1;
	m.tickRate = 1.0 / g.demo.interval;
	m.timescale = g.demo.timescale;
	m.scrubCache = g.scrubCache;
	const double live[7] = { 0, 0, 64, g.pitch, -g.yaw, 0, g.demo.freeCamFov };
	for (int c = 0; c < 7; ++c) m.live[c] = live[c];
	// The shake track, as the game's CamPathShake_AppendDockJson gives it, on RageShake's own styles.
	m.pathKeys = StandInCameraTicks();
	for (const auto& e : g.shakes) {
		TimelineDockMath::ShakeKey k;
		k.t = e.first;
		k.s = e.second.s;
		k.n = e.second.n;
		k.p = e.second.p;
		k.oneShot = k.s >= 0 && Filmmaker::RageShake_Finished(Filmmaker::RageShake_Style(k.s), 1e6, 1.0);
		m.shakes.push_back(k);
		m.shakeLabels.push_back(k.s < 0 ? "Stop" : Filmmaker::RageShake_StyleLabel(k.s));
	}
	for (int i = 0; i < Filmmaker::RageShake_MenuCount(); ++i) {
		const int style = Filmmaker::RageShake_MenuStyle(i);
		m.shakeStyles.emplace_back(style, Filmmaker::RageShake_StyleLabel(style));
	}
	m.shakeEssentials = Filmmaker::RageShake_RockstarEditorCount();
	return m;
}

// The TIMELINE dock's requests on the stand-in keys, as SourceTimelineDock.cpp applies them before
// it writes them into the path.
void ApplyTimelineActions(const std::vector<TimelineDockAction>& actions) {
	using Kind = TimelineDockAction::Kind;
	Filmmaker::GraphExpModel& k = g.timeline;
	for (const TimelineDockAction& a : actions) {
		switch (a.kind) {
		case Kind::Select: k.Select(a.ch, a.id, a.flag); break;
		case Kind::SelectSet:
			k.ClearSelection();
			for (const auto& m : a.keys) k.SelectAdd(m.first, m.second);
			break;
		case Kind::SelectAll: k.SelectAll(); break;
		case Kind::SelectClear: k.ClearSelection(); break;
		case Kind::EditBegin: k.BeginEdit(); break;
		case Kind::MoveSelected: k.MoveSelectedBy(a.a, a.b); break;
		case Kind::AddKey: k.Select(a.ch, k.AddKey(a.ch, a.a, a.b), false); break;
		case Kind::Handle: k.SetHandle(a.ch, a.id, a.side, a.a, a.b, a.flag); break;
		case Kind::AlignKey:
			if (const Filmmaker::GraphExpModel::Keyframe* key = k.FindKeyConst(a.ch, a.id)) {
				k.BeginEdit();
				k.AlignSelectionToTick(key->tick);
			}
			break;
		case Kind::DeleteSelected: {
			k.BeginEdit();
			std::vector<std::pair<int, int>> doomed;
			for (int c = 0; c < Filmmaker::GraphExpModel::kChannelCount; ++c)
				for (const auto& key : k.GetChannel(c).keys)
					if (k.IsSelected(c, key.id)) doomed.emplace_back(c, key.id);
			for (const auto& d : doomed) k.DeleteKey(d.first, d.second);
			k.ClearSelection();
			break;
		}
		case Kind::Ease: k.BeginEdit(); k.SetEase(a.id, a.flag); break;
		case Kind::Interp:
			k.BeginEdit();
			k.SetInterpAll(a.flag, a.id == 1);
			if (a.id != 1) g.path.interp = a.flag ? 1 : 0;
			break;
		case Kind::Undo: k.Undo(); break;
		case Kind::Redo: k.Redo(); break;
		case Kind::Drive: g.timelineDrive = a.flag; break;
		case Kind::AutoPath: g.timelineAuto = a.flag; break;
		case Kind::Playhead: g.timelineScrubbing = true; g.timelineTick = a.a; break;
		case Kind::PlayheadRelease: g.timelineScrubbing = false; SeekSim((int)a.a); break;
		case Kind::PlayPause: g.demo.paused = !g.demo.paused; break;
		case Kind::Speed: g.demo.timescale = a.a; break;
		case Kind::ScrubCache: g.scrubCache = a.flag; LogLine("scrub.cache", a.flag ? "toggle on" : "toggle off"); break;
		case Kind::ShakeCommand: StandInShakeCommand(a.text); break;
		case Kind::Workspace: Open(a.flag ? Workspace::Config : Workspace::Editor, "dock"); break;
		}
		k.RefreshAutoTangents();
		if (a.kind != Kind::Playhead) {
			char line[96];
			std::snprintf(line, sizeof(line), "kind=%d ch=%d id=%d a=%.2f b=%.2f flag=%d keys=%d", (int)a.kind, a.ch, a.id, a.a, a.b,
				a.flag ? 1 : 0, (int)a.keys.size());
			LogLine("timeline.dock", line);
		}
	}
}

// The card while it is up, else the reposition banner over the game rect.
void DrawCardAndBanner(const Rect& game, std::vector<MarkerCardAction>& actions) {
	const PathInspectorModel& p = g.path;
	if (g.cardOpen) {
		MarkerCardModel m;
		m.selected = p.selected;
		m.count = (int)p.keys.size();
		m.speedMode = p.speedMode;
		m.interp = p.interp;
		m.timing = p.timing;
		m.autoSnap = g.autoSnap;
		m.constSpeed = p.constSpeed;
		if (p.selected >= 0 && p.selected < m.count) {
			m.speedMul = p.keys[(size_t)p.selected].speedMul;
			m.isLast = p.selected == m.count - 1;
		}
		DrawMarkerCard(g.card, m, g.fonts, (float)g.width, (float)g.height, actions);
		return;
	}
	g.card.confirmRemoveAll = false;
	if (!p.repositioning) return;
	char title[64];
	std::snprintf(title, sizeof(title), "REPOSITIONING CAMERA #%d", p.selected + 1);
	PathBanner banner;
	banner.title = title;
	banner.line = "Move to the new position, then left-click to place.";
	banner.keys = "LEFT CLICK / F  place        ESC  cancel";
	DrawPathBanner(ImGui::GetForegroundDrawList(), banner, g.fonts, game);
}

// ---- the demos browser's stand-in ---------------------------------------------------------------
// A library as the game's scan lists it from a CS:S install: the test demos Docs/source/README.md
// records (SourceTV on de_dust2), SourceTV recordings and old v34 demos in replays\, and one demo added
// from elsewhere. Dates are days before now; names, servers and lengths are stand-ins.
const char kStandInGameDir[] = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Source\\cstrike";

void AddStandInDemo(const std::string& folder, const char* file, const char* map, float seconds, double daysAgo, int protocol,
	const char* recorder, const char* server, int64_t now) {
	DemosBrowserMath::DemoRow d;
	d.fileName = file;
	d.folder = folder;
	d.key = DemosBrowserMath::Lower(folder + "\\" + file);
	d.map = map;
	d.recorder = recorder;
	d.server = server;
	d.seconds = seconds;
	d.ticks = (int)(seconds * (protocol == 7 ? 100.0f : 66.67f));
	d.date = now - (int64_t)(daysAgo * 86400);
	d.protocol = protocol;
	d.compat = protocol == 24 ? DemosBrowserMath::Compat::Current
		: (protocol == 7 ? DemosBrowserMath::Compat::Old : DemosBrowserMath::Compat::Other);
	g.library.push_back(d);
}

void SeedDemos() {
	const int64_t now = (int64_t)std::time(nullptr);
	const std::string game = kStandInGameDir, replays = game + "\\replays", added = "D:\\Frag movies\\source";
	AddStandInDemo(game, "mvm_css_tv.dem", "de_dust2", 66.1f, 0.2, 24, "SourceTV", "Counter-Strike: Source", now);
	AddStandInDemo(game, "mvm_css_nades.dem", "de_dust2", 96.4f, 0.3, 24, "SourceTV", "Counter-Strike: Source", now);
	AddStandInDemo(game, "mvm_css_test.dem", "de_dust2", 41.0f, 1.4, 24, "Player", "Counter-Strike: Source", now);
	AddStandInDemo(replays, "scrim_inferno_half1.dem", "de_inferno", 1712, 3.1, 24, "SourceTV", "Scrim server #2", now);
	AddStandInDemo(replays, "scrim_inferno_half2.dem", "de_inferno", 1655, 3.0, 24, "SourceTV", "Scrim server #2", now);
	AddStandInDemo(replays, "office_pug.dem", "cs_office", 2290, 9.5, 24, "SourceTV", "Friday PUG", now);
	AddStandInDemo(replays, "aim_map_warmup.dem", "aim_map", 512, 12.2, 24, "Player", "Aim practice", now);
	AddStandInDemo(replays, "cal_final_nuke_2007.dem", "de_nuke", 2861, 6400, 7, "SourceTV", "CAL-i final", now);
	AddStandInDemo(replays, "cal_final_train_2007.dem", "de_train", 2740, 6399, 7, "SourceTV", "CAL-i final", now);
	AddStandInDemo(replays, "lan_aztec_2006.dem", "de_aztec", 2108, 7100, 7, "SourceTV", "LAN 2006", now);
	AddStandInDemo(replays, "broken_header.dem", "", 0, 30, 0, "", "", now);
	AddStandInDemo(added, "ace_dust2_ak.dem", "de_dust2", 135, 2.0, 24, "Player", "Public server", now);
	g.library[10].compat = DemosBrowserMath::Compat::Unknown; // broken_header.dem: no HL2DEMO header
	std::sort(g.library.begin(), g.library.end(), [](const DemosBrowserMath::DemoRow& a, const DemosBrowserMath::DemoRow& b) { return a.date > b.date; });
	for (size_t i = 0; i < g.library.size(); ++i) g.library[i].index = (int)i;
	g.demoHidden.assign(g.library.size(), false);
	g.library[1].played = now - 3600; // mvm_css_nades, an hour ago: the gold edge
	g.library[2].played = now - 2 * 86400;
}

// The model the page draws, as SourceDemos.cpp builds it: the listed (not hidden) demos.
void BuildDemosModel() {
	DemosBrowserModel& m = g.demos;
	m.demos.clear();
	m.hidden = 0;
	for (size_t i = 0; i < g.library.size(); ++i) {
		if (g.demoHidden[i]) ++m.hidden;
		else m.demos.push_back(g.library[i]);
	}
	m.now = (int64_t)std::time(nullptr);
	m.converting = g.convertIndex >= 0;
}

DemosBrowserMath::DemoRow* StandInDemo(int index) {
	return index >= 0 && index < (int)g.library.size() ? &g.library[(size_t)index] : nullptr;
}

void OpenDemos(bool open, const char* reason) {
	if (open == g.demosOpen) return;
	g.demosOpen = open;
	if (open && g.editor.open) Close("demos browser");
	LogLine("ui.demos", (std::string(open ? "open" : "close") + " reason=" + reason).c_str());
}

// The page's commands on the stand-in library and organiser, as SourceDemos.cpp applies them (the
// organiser's verbs are DemoOrganizerStore::Apply's).
void StandInDemosCommand(const std::string& line) {
	LogLine("demos.cmd", line.c_str());
	std::istringstream in(line);
	std::string verb;
	in >> verb;
	DemosBrowserModel& m = g.demos;
	auto rest = [&in]() {
		std::string r;
		std::getline(in, r);
		if (!r.empty() && r[0] == ' ') r.erase(0, 1);
		return r;
	};
	if (verb == "watch") {
		int index = -1;
		in >> index;
		DemosBrowserMath::DemoRow* d = StandInDemo(index);
		if (!d) return;
		d->played = (int64_t)std::time(nullptr);
		if (d->compat == DemosBrowserMath::Compat::Old) { // converted on a worker first, then played
			g.convertIndex = index;
			g.convertDoneMs = emscripten_get_now() + 2500;
			return;
		}
		g.demo.playing = true;
		g.demo.tick = 0;
		OpenDemos(false, "watch");
	} else if (verb == "hide") {
		int index = -1;
		in >> index;
		if (index >= 0 && index < (int)g.demoHidden.size()) g.demoHidden[(size_t)index] = true;
	} else if (verb == "clearall") {
		g.demoHidden.assign(g.library.size(), true);
	} else if (verb == "unhideall") {
		g.demoHidden.assign(g.library.size(), false);
	} else if (verb == "renamedemo") {
		int index = -1;
		in >> index;
		const std::string file = DemosBrowserMath::DemoFileName(rest());
		if (DemosBrowserMath::DemoRow* d = StandInDemo(index)) {
			if (!file.empty()) {
				d->fileName = file;
				d->key = DemosBrowserMath::Lower(d->folder + "\\" + file);
			}
		}
	} else if (verb == "org") {
		std::string what;
		in >> what;
		const std::string args = rest();
		if (what == "sort") m.sort = args;
		else if (what == "newfolder" && !args.empty()) m.groups.push_back({ g.nextGroup++, args.substr(0, 40), false });
		else if (what == "rename" || what == "delfolder" || what == "collapse") {
			std::istringstream a(args);
			int id = -1;
			a >> id;
			std::string tail;
			std::getline(a, tail);
			if (!tail.empty() && tail[0] == ' ') tail.erase(0, 1);
			if (what == "collapse" && id < 0) m.rootCollapsed = std::atoi(tail.c_str()) != 0;
			for (size_t i = 0; i < m.groups.size(); ++i) {
				if (m.groups[i].id != id) continue;
				if (what == "rename" && !tail.empty()) m.groups[i].name = tail.substr(0, 40);
				else if (what == "collapse") m.groups[i].collapsed = std::atoi(tail.c_str()) != 0;
				else if (what == "delfolder") {
					m.groups.erase(m.groups.begin() + (long)i);
					for (auto& d : g.library)
						if (d.group == id) d.group = -1;
				}
				break;
			}
		} else if (what == "assign") {
			std::istringstream a(args);
			int id = -1;
			a >> id;
			std::string key;
			std::getline(a, key);
			if (!key.empty() && key[0] == ' ') key.erase(0, 1);
			for (auto& d : g.library)
				if (d.key == key) d.group = id;
		}
	}
	// refresh, adddemo, addfolder and openfolder have nothing to stand in for: a scan, Windows' pickers
	// and Explorer are the game's. They are logged above.
}

// Each frame: a stand-in conversion finishing plays its demo, as SourceOldDemos.cpp does.
void StandInDemosFrame() {
	if (g.convertIndex >= 0 && emscripten_get_now() >= g.convertDoneMs) {
		if (DemosBrowserMath::DemoRow* d = StandInDemo(g.convertIndex)) d->compat = DemosBrowserMath::Compat::Converted;
		g.convertIndex = -1;
		g.demo.playing = true;
		g.demo.tick = 0;
		OpenDemos(false, "old demo converted and playing");
	}
}

// The main menu while no demo plays: the backdrop dimmed, a few menu lines where CS:S puts them,
// and the DEMOS button.
void DrawMainMenu(ImDrawList* list) {
	const float s = (float)g.height / kStageHeight;
	list->AddRectFilled(ImVec2(0, 0), ImVec2((float)g.width, (float)g.height), IM_COL32(0, 0, 0, 120));
	ImFont* body = g.fonts.Get(FontRole::Body);
	const char* items[] = { "Find Servers", "Create Server", "Achievements", "Options", "Quit" };
	float y = g.height * 0.55f;
	for (const char* item : items) {
		list->AddText(body, body->FontSize * 1.2f, ImVec2(64 * s, y), IM_COL32(221, 221, 221, 230), item);
		y += body->FontSize * 1.9f;
	}
}

// Renders a draw list built outside the ImGui frame through the same backend as the UI.
void RenderList(ImDrawList& list) {
	ImDrawData data;
	data.Valid = true;
	data.DisplayPos = ImVec2(0, 0);
	data.DisplaySize = ImVec2((float)g.width, (float)g.height);
	data.FramebufferScale = ImVec2(1, 1);
	data.AddDrawList(&list);
	ImGui_ImplOpenGL3_RenderDrawData(&data);
}

void Frame() {
	ImGuiIO& io = ImGui::GetIO();
	ImGui_ImplOpenGL3_NewFrame(); // creates the backend's objects on the first frame
	EnsureFonts();
	io.DisplaySize = ImVec2((float)g.width, (float)g.height);
	const double nowMs = emscripten_get_now();
	const float dt = g.lastMs > 0 ? (float)((nowMs - g.lastMs) / 1000.0) : 1.0f / 60.0f;
	g.lastMs = nowMs;
	io.DeltaTime = dt > 0.0f ? (dt < 0.25f ? dt : 0.25f) : 1.0f / 60.0f;
	{
		const Mode before = g.mouse.GetMode();
		g.mouse.Tick((int64_t)nowMs); // a right press held long enough looks even without moving
		ApplyMode(before, "right hold");
	}
	g.directorMouse.Tick((int64_t)nowMs);
	const bool director = DirectorActive();
	if (g.editor.open ? g.mouse.GetMode() != Mode::Ui : (director && g.directorMouse.GetMode() != DirectorMode::Free))
		io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); // nothing hovers while the game has the mouse

	ImGui::NewFrame();
	AdvanceDemo(io.DeltaTime);
	StandInDemosFrame();
	DemosBrowserFrame demosFrame;
	const bool mainMenu = MainMenu();
	if (g.demosOpen) {
		BuildDemosModel();
		demosFrame = DrawDemosBrowser(g.demosState, g.fonts, (float)g.width, (float)g.height, g.demos);
	} else if (mainMenu) {
		g.menuButton = DrawDemosMenuButton(g.fonts, (float)g.width, (float)g.height, g.menuButtonHover);
	}
	DemoBarModel demo;
	BuildDemoModel(demo);
	CameraEditorFrame frame;
	DirectorFrame directorFrame;
	if (g.editor.open) {
		g.path.tick = (int)g.demo.tick;
		StandInFollowFrame();
		g.config.demo = g.demo.playing;
		g.config.viewmodelChannel = true; // the game moves the gun (SourceViewModel.cpp)
		g.config.pivotMarkers = true;     // and draws the pivot markers
		// The stand-in demo's watched player holds an AK-47: the gun in shot for Weapon Recoil and the pivots.
		g.config.wrLive = "weapon_ak47";
		g.config.wrLiveType = "rifle";
		g.config.wrLiveSlot = "primary";
		g.config.wrScopeCovers = g.config.wrScope == "all" || g.config.wrScopeKey == (g.config.wrScope == "slot" ? "primary" : "rifle");
		g.config.pvr.cls = g.config.pvb.cls = "rifle";
		g.config.pvr.weapon = g.config.pvb.weapon = "weapon_ak47";
		frame = DrawCameraEditor(g.editor, g.fonts, (float)g.width, (float)g.height, demo, g.path, g.follow, BuildTimelineModel(), g.config);
	} else if (director) {
		if (g.cadence.Ready((uint64_t)nowMs, 10, false)) BuildCards(g.cards);
		// The backdrop is a CS:S spectator capture, so its spectator bars are where the game's are.
		const float spectatorBar = (float)g.height * DirectorMath::kSpectatorBarShare;
		directorFrame = DrawDirector(g.director, demo, g.cards, g.fonts, (float)g.width, (float)g.height, spectatorBar, spectatorBar);
	}
	std::vector<MarkerCardAction> cardActions;
	if ((g.editor.open || director) && !g.director.blackout)
		DrawCardAndBanner(g.editor.open ? frame.previewClient : MakeRect(0, 0, (float)g.width, (float)g.height), cardActions);
	ImGui::Render();
	for (const std::string& command : demosFrame.commands) StandInDemosCommand(command);
	if (demosFrame.close) OpenDemos(false, "close chip");
	ApplyCardActions(cardActions);
	ApplyDemoActions(frame.demoActions);
	ApplyDemoActions(directorFrame.demoActions);
	ApplyPathActions(frame.pathActions);
	ApplyFollowCommands(frame.followCommands);
	ApplyTimelineActions(frame.timelineActions);
	for (const std::string& command : frame.configCommands) StandInConfigCommand(command);
	g.bar = directorFrame.bar;
	g.preview = frame.modal || g.cardOpen ? Rect() : frame.previewClient;
	g.panelHovered = g.editor.open && io.WantCaptureMouse;
	++g.frames;

	glViewport(0, 0, g.width, g.height);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	ImDrawList game(ImGui::GetDrawListSharedData());
	game._ResetForNewFrame();
	game.PushClipRect(ImVec2(0, 0), ImVec2((float)g.width, (float)g.height));
	game.PushTextureID(io.Fonts->TexID);
	const Rect gameRect = g.editor.open ? frame.previewClient : MakeRect(0, 0, (float)g.width, (float)g.height);
	DrawGame(&game, gameRect);
	if (StandInWorldShown() && !g.director.blackout && (g.editor.open || director)) DrawStandInPath(game, gameRect, g.fonts.Get(FontRole::Display));
	if (!g.demo.playing && !g.editor.open) DrawMainMenu(&game);
	RenderList(game);
	if (g.editor.open || director || g.demosOpen || mainMenu) ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

	if (frame.exitClicked) Close("exit chip");
}

std::string StateJson() {
	char buf[1024];
	std::snprintf(buf, sizeof(buf),
		"{\"open\":%s,\"mode\":\"%s\",\"rightPending\":%s,\"tab\":%d,\"dock\":\"%s\","
		"\"preview\":[%.1f,%.1f,%.1f,%.1f],\"panelHovered\":%s,\"fonts\":\"%s\",\"scale\":%.4f,"
		"\"width\":%d,\"height\":%d,\"frames\":%u,\"yaw\":%.1f,\"pitch\":%.1f,\"backdrop\":%s,"
		"\"demo\":{\"playing\":%s,\"paused\":%s,\"tick\":%d,\"timescale\":%.4f,\"camera\":%d,\"loop\":%s,\"in\":%d,\"out\":%d}}",
		g.editor.open ? "true" : "false", ModeName(g.mouse.GetMode()), g.mouse.RightPending() ? "true" : "false",
		(int)g.editor.tab, g.editor.dock == DockTab::Timeline ? "timeline" : "regular",
		g.preview.x0, g.preview.y0, g.preview.x1, g.preview.y1, g.panelHovered ? "true" : "false",
		g.fonts.ok ? "ok" : (g.fonts.scale > 0 ? "fallback" : "not built"), g.fonts.scale,
		g.width, g.height, g.frames, g.yaw, g.pitch, g.backdrop ? "true" : "false",
		g.demo.playing ? "true" : "false", g.demo.paused ? "true" : "false", (int)g.demo.tick, g.demo.timescale,
		g.demo.observerMode, g.demo.loopOn ? "true" : "false", g.demo.loop.in, g.demo.loop.out);
	std::string json = buf;
	json.pop_back(); // reopen the object for the director and the bar's layout
	const bool director = DirectorActive();
	std::snprintf(buf, sizeof(buf),
		",\"director\":{\"active\":%s,\"mouse\":\"%s\",\"cards\":%s,\"blackout\":%s,\"target\":\"%s\",\"fov\":%.1f,\"speed\":%.3f,"
		"\"freecam\":%s,\"freecamSpeed\":%.3f,\"freecamFov\":%.1f,\"bar\":[%.1f,%.1f,%.1f,%.1f]}",
		director ? "true" : "false", DirectorModeName(g.directorMouse.GetMode()), g.director.cards ? "true" : "false",
		g.director.blackout ? "true" : "false", g.demo.spectatable[(size_t)g.demo.target].c_str(), g.demo.fov, g.demo.speed,
		g.demo.freeCam ? "true" : "false", g.demo.freeCamSpeed, g.demo.freeCamFov, g.bar.x0, g.bar.y0, g.bar.x1, g.bar.y1);
	json += buf;
	const DemoBarLayout& l = director ? g.director.bar.layout : g.editor.demoBar.layout;
	auto add = [&](const char* name, const Rect& r) {
		if (!r.Valid()) return;
		char part[96];
		std::snprintf(part, sizeof(part), ",\"%s\":[%.1f,%.1f,%.1f,%.1f]", name, r.x0, r.y0, r.x1, r.y1);
		json += part;
	};
	json += ",\"bar\":{\"_\":0";
	add("band", l.band); add("play", l.play); add("back", l.back); add("forward", l.forward);
	add("roundprev", l.roundPrev); add("roundnext", l.roundNext); add("nextkill", l.nextKill);
	add("camera", l.camera); add("loop", l.loop); add("dial", l.dial);
	json += "}}";
	return json;
}

} // namespace

extern "C" {

// The movie director's mouse (DirectorMouse.h), as SourceUi.cpp routes it.
int DirectorMouseEvent(int type, int button, float x, float y, float wheel, int mods) {
	ImGuiIO& io = ImGui::GetIO();
	const int64_t now = (int64_t)emscripten_get_now();
	const DirectorMode before = g.directorMouse.GetMode();
	DirectorRoute route = DirectorRoute::Swallow;
	DirectorMouse::Click click = DirectorMouse::Click::None;
	switch (type) {
	case 0:
		route = g.directorMouse.Move(x, y, now);
		if (route == DirectorRoute::Ui) io.AddMousePosEvent(x, y);
		break;
	case 1:
		route = g.directorMouse.ButtonDown(button, x, y, now, g.bar, g.demo.freeCam);
		if (route == DirectorRoute::Ui) {
			io.AddMousePosEvent(x, y);
			io.AddMouseButtonEvent(button, true);
		}
		break;
	case 2:
		route = g.directorMouse.ButtonUp(button, now, click);
		if (route == DirectorRoute::Ui) io.AddMouseButtonEvent(button, false);
		if (click == DirectorMouse::Click::NextPlayer) StepPlayer(1);
		if (click == DirectorMouse::Click::PreviousPlayer) StepPlayer(-1);
		break;
	case 3:
		DirectorWheel(wheel > 0 ? 1 : -1, (mods & 1) != 0, (mods & 2) != 0);
		route = DirectorRoute::Swallow;
		break;
	default:
		return 1;
	}
	if (g.directorMouse.GetMode() != before) {
		LogLine("director.mouse", (std::string("mode=") + DirectorModeName(g.directorMouse.GetMode())).c_str());
	}
	return route == DirectorRoute::Ui ? 0 : (route == DirectorRoute::Game ? 1 : 2);
}

// Mouse input in canvas px. type: 0 move, 1 button down, 2 button up, 3 wheel (delta in notches,
// positive away from the user); mods: 1 Ctrl, 2 Shift. Returns where the event went: 0 the UI,
// 1 the game, 2 nowhere.
EMSCRIPTEN_KEEPALIVE int mockup_mouse(int type, int button, float x, float y, float wheel, int mods) {
	if (g.demosOpen) { // the page has the whole window and the mouse, as in game
		ImGuiIO& io = ImGui::GetIO();
		io.AddMousePosEvent(x, y);
		if (type == 1 || type == 2) io.AddMouseButtonEvent(button, type == 1);
		if (type == 3) io.AddMouseWheelEvent(0.0f, wheel);
		return 0;
	}
	if (MainMenu()) { // only the DEMOS button is ours; the rest is the game's menu
		const bool over = g.menuButton.Contains(x, y);
		g.menuButtonHover = over;
		if (type == 1 && button == 0 && over) OpenDemos(true, "menu button");
		return over ? 0 : 1;
	}
	if (!g.editor.open) return DirectorActive() ? DirectorMouseEvent(type, button, x, y, wheel, mods) : 1;
	ImGuiIO& io = ImGui::GetIO();
	const int64_t now = (int64_t)emscripten_get_now();
	const Mode before = g.mouse.GetMode();
	Route route = Route::Game;
	const char* reason = "input";
	switch (type) {
	case 0:
		route = g.mouse.Move(x, y, now);
		if (route == Route::Ui) io.AddMousePosEvent(x, y);
		reason = "right drag";
		break;
	case 1:
		if (button == WorkspaceMouse::Middle) {
			route = before == Mode::Ui ? Route::Ui : Route::Game;
		} else {
			route = g.mouse.ButtonDown(button, x, y, now, g.preview);
			reason = button == WorkspaceMouse::Left ? "click in preview" : "right press";
		}
		if (route == Route::Ui) {
			io.AddMousePosEvent(x, y);
			io.AddMouseButtonEvent(button, true);
		}
		break;
	case 2:
		if (button == WorkspaceMouse::Middle) route = before == Mode::Ui ? Route::Ui : Route::Game;
		else route = g.mouse.ButtonUp(button, now);
		if (route == Route::Ui) io.AddMouseButtonEvent(button, false);
		reason = "look released";
		break;
	case 3:
		route = g.mouse.Wheel(x, y, g.preview);
		if (route == Route::Ui) io.AddMouseWheelEvent(0.0f, wheel);
		break;
	default:
		return 1;
	}
	if (type != 0) {
		char line[128];
		std::snprintf(line, sizeof(line), "type=%d button=%d x=%.0f y=%.0f mode=%s->%s route=%d panelHovered=%d",
			type, button, x, y, ModeName(before), ModeName(g.mouse.GetMode()), (int)route, g.panelHovered ? 1 : 0);
		LogLine("ui.input", line);
	}
	ApplyMode(before, reason);
	return (int)route;
}

// Relative mouse motion while the game has the mouse (pointer lock or not): turns the stand-in
// camera, as the game's mouse look would.
EMSCRIPTEN_KEEPALIVE void mockup_motion(float dx, float dy) {
	const bool gameHasMouse = g.editor.open ? g.mouse.GetMode() != Mode::Ui
		: (DirectorActive() && g.directorMouse.GetMode() == DirectorMode::Look);
	if (!gameHasMouse) return;
	g.yaw += dx * 0.1f;
	g.pitch += dy * 0.1f;
	if (g.pitch > 60) g.pitch = 60;
	if (g.pitch < -60) g.pitch = -60;
}

// A key, as a DOM keyCode (the Windows virtual-key number). Returns 1 when the workspace used it.
EMSCRIPTEN_KEEPALIVE int mockup_key(int vk, int down, int repeat) {
	if (g.demosOpen) { // Esc closes what is open on the page, then the page; every key is the page's
		if (WorkspaceKeyFor(vk, true) == WorkspaceKey::Close) {
			if (down && !repeat && !DemosBrowserEscape(g.demosState)) OpenDemos(false, "esc");
			return 1;
		}
		FeedImGuiKey(ImGui::GetIO(), vk, down != 0);
		return 1;
	}
	if ((g.editor.open && !ConfigOpen()) || DirectorActive()) { // the camera path's keys come first, in both places
		if (g.path.repositioning && (vk == 'F' || vk == 27)) { // F drops the camera, Esc cancels
			if (down && !repeat) {
				g.path.repositioning = false;
				LogLine("path.inspector", vk == 'F' ? "reposition placed" : "reposition cancelled");
			}
			return 1;
		}
		if (g.pathOn && (vk == 'F' || (vk == 27 && g.cardOpen))) { // the CAMERA MARKER card
			if (down && !repeat) g.cardOpen = vk == 'F' ? !g.cardOpen && !g.path.keys.empty() : false;
			return 1;
		}
	}
	if (!g.editor.open) {
		if (!DirectorActive()) return 0;
		if (vk == 16) { // Shift: the bar's fine drag reads it, and the game sees it too
			FeedImGuiKey(ImGui::GetIO(), vk, down != 0);
			return 0;
		}
		const DirectorMath::Key key = DirectorMath::KeyFor(vk);
		if (key == DirectorMath::Key::None) return 0;
		if (down && !repeat) DirectorKey(key); // both edges are the director's; auto-repeat is ignored
		return 1;
	}
	ImGuiIO& io = ImGui::GetIO();
	if (ConfigOpen() && vk == 'T' && !io.WantTextInput) { // a Weapon Recoil test shot, as in game
		if (down && !repeat) StandInConfigCommand("mirv_filmmaker weaponrecoil test");
		return 1;
	}
	const WorkspaceKey key = WorkspaceKeyFor(vk, io.WantTextInput);
	if (down) {
		switch (key) {
		case WorkspaceKey::Close:
			if (g.editor.dock == DockTab::Timeline && TimelineDockEscape(g.editor.timeline)) return 1;
			if (ConfigOpen() && ConfigEscape(g.editor.config)) return 1;
			Close("esc");
			return 1;
		case WorkspaceKey::ToggleGame:
			if (!repeat) {
				const Mode before = g.mouse.GetMode();
				g.mouse.ToggleGame();
				ApplyMode(before, "G");
			}
			return 1;
		default:
			break;
		}
	}
	FeedImGuiKey(io, vk, down != 0);
	if (key != WorkspaceKey::None) return 1;
	return io.WantCaptureKeyboard ? 1 : 0;
}

// A typed character, for a text field that is taking typing (the game gets WM_CHAR).
EMSCRIPTEN_KEEPALIVE int mockup_char(int codepoint) {
	if (!(g.editor.open || g.demosOpen) || !ImGui::GetIO().WantTextInput) return 0;
	ImGui::GetIO().AddInputCharacter((unsigned)codepoint);
	return 1;
}

EMSCRIPTEN_KEEPALIVE void mockup_focus_lost() {
	const Mode before = g.mouse.GetMode();
	g.mouse.FocusLost();
	ApplyMode(before, "focus lost");
	g.directorMouse.FocusLost();
}

// The browser let go of the pointer lock on its own (Esc, or the tab lost focus) while the game had
// the mouse. The page cannot keep the game's capture, so the mouse comes back like G.
EMSCRIPTEN_KEEPALIVE void mockup_lock_lost() {
	const Mode before = g.mouse.GetMode();
	if (before == Mode::Game) g.mouse.ToggleGame();
	else g.mouse.FocusLost();
	ApplyMode(before, "pointer lock lost");
	g.directorMouse.FocusLost();
}

// 0 while the UI has the mouse; otherwise the game has it (the page locks the pointer).
EMSCRIPTEN_KEEPALIVE int mockup_mode() {
	if (g.demosOpen) return 0;
	if (g.editor.open) return (int)g.mouse.GetMode();
	return DirectorActive() && g.directorMouse.GetMode() == DirectorMode::Look ? (int)Mode::Look : 0;
}

// Runs frames now, for test scripts: a background browser tab gets about one animation frame a
// second, so a script steps the UI itself between the events it sends.
EMSCRIPTEN_KEEPALIVE void mockup_step(int frames) {
	for (int i = 0; i < frames && i < 600; ++i) Frame();
}

EMSCRIPTEN_KEEPALIVE void mockup_resize(int width, int height) {
	if (width < 64 || height < 64) return;
	g.width = width;
	g.height = height;
	emscripten_set_canvas_element_size("#canvas", width, height);
}

// RGBA pixels of the stand-in game frame; the page decodes assets/backdrop.png when it exists.
EMSCRIPTEN_KEEPALIVE void mockup_set_backdrop(const unsigned char* rgba, int width, int height) {
	if (!g.backdrop) glGenTextures(1, &g.backdrop);
	glBindTexture(GL_TEXTURE_2D, g.backdrop);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	g.backdropW = width;
	g.backdropH = height;
}

// The game's console surface for the workspace: "editor on|off|toggle|state", as
// mirv_filmmaker editor takes it. Returns the reply line.
EMSCRIPTEN_KEEPALIVE const char* mockup_command(const char* command) {
	const char* action = command ? command : "";
	if (0 == std::strncmp(action, "demo ", 5)) {
		const char* what = action + 5;
		if (0 == std::strcmp(what, "on")) g.demo.playing = true;
		else if (0 == std::strcmp(what, "off")) g.demo.playing = false;
		else if (0 == std::strcmp(what, "pause")) g.demo.paused = true;
		else if (0 == std::strncmp(what, "seek ", 5)) SeekSim(std::atoi(what + 5));
		g.reply = "demo: " + StateJson();
		return g.reply.c_str();
	}
	if (0 == std::strncmp(action, "mirv_input ", 11)) { // HLAE's free camera: camera | end
		g.demo.freeCam = 0 == std::strcmp(action + 11, "camera");
		g.reply = std::string("free cam: ") + (g.demo.freeCam ? "on" : "off");
		return g.reply.c_str();
	}
	if (0 == std::strncmp(action, "markers ", 8)) { // the stand-in camera path: on | off | freeze | live
		const char* what = action + 8;
		if (0 == std::strcmp(what, "on")) {
			g.pathOn = true;
			if (g.path.keys.empty()) { // the stand-in path's three cameras for the inspector
				const int ticks[] = { 3000, 3300, 3600 };
				for (int tick : ticks) {
					PathInspectorKey k;
					k.tick = tick;
					g.path.keys.push_back(k);
				}
				g.path.selected = 1;
			}
			if (g.timeline.TotalKeys() == 0) SeedTimeline();
		}
		else if (0 == std::strcmp(what, "off")) g.pathOn = false;
		else if (0 == std::strcmp(what, "freeze")) g.pathFreeze = true;
		else if (0 == std::strcmp(what, "live")) g.pathFreeze = false;
		g.reply = std::string("markers: ") + (g.pathOn ? "on" : "off") + (g.pathFreeze ? ", freeze" : ", live");
		return g.reply.c_str();
	}
	if (0 == std::strncmp(action, "grapheditor ", 12)) { // the TIMELINE dock: on | off | state, as in game
		const bool state = 0 == std::strcmp(action + 12, "state");
		const bool on = state ? g.editor.dock == DockTab::Timeline : 0 == std::strcmp(action + 12, "on");
		if (!state) {
			if (on) Open(Workspace::Editor, "timeline dock");
			g.editor.dock = on ? DockTab::Timeline : DockTab::RegularTimeline;
		}
		char buf[160];
		std::snprintf(buf, sizeof(buf), "timeline: dock %s, keys %d, selected %d, view %.0f..%.0f, snap %d%s", on ? "open" : "closed",
			g.timeline.TotalKeys(), g.timeline.SelectionCount(), g.editor.timeline.window.t0, g.editor.timeline.window.t1,
			g.editor.timeline.snap, g.editor.timeline.snapMenu ? " (menu open)" : "");
		g.reply = buf;
		return g.reply.c_str();
	}
	if (0 == std::strncmp(action, "hud ", 4) || 0 == std::strncmp(action, "blackout ", 9)) {
		const bool hud = action[0] == 'h';
		const char* what = action + (hud ? 4 : 9);
		bool& flag = hud ? g.director.cards : g.director.blackout;
		if (0 == std::strcmp(what, "on")) flag = true;
		else if (0 == std::strcmp(what, "off")) flag = false;
		else if (0 == std::strcmp(what, "toggle")) flag = !flag;
		g.reply = std::string(hud ? "hud: " : "blackout: ") + (flag ? "on" : "off");
		return g.reply.c_str();
	}
	if (0 == std::strncmp(action, "config", 6)) { // the Config workspace: on | off | toggle | state, as in game
		const char* what = action + 6;
		while (*what == ' ') ++what;
		if (0 == std::strcmp(what, "on")) Open(Workspace::Config, "command");
		else if (0 == std::strcmp(what, "off")) { if (ConfigOpen()) Close("command"); }
		else if (0 == std::strcmp(what, "toggle") || !*what) { if (ConfigOpen()) Close("command"); else Open(Workspace::Config, "command"); }
		g.reply = std::string("config: ") + (ConfigOpen() ? "open" : "closed") + ", card " + ConfigCardName(g.editor.config.card);
		return g.reply.c_str();
	}
	if (0 == std::strncmp(action, "demos", 5)) { // the demos browser: on | off | toggle | state, or a page command
		const char* what = action + 5;
		while (*what == ' ') ++what;
		if (0 == std::strcmp(what, "on")) OpenDemos(true, "command");
		else if (0 == std::strcmp(what, "off")) OpenDemos(false, "command");
		else if (0 == std::strcmp(what, "toggle")) OpenDemos(!g.demosOpen, "command");
		else if (*what && 0 != std::strcmp(what, "state")) StandInDemosCommand(what);
		BuildDemosModel();
		char buf[256];
		std::snprintf(buf, sizeof(buf), "demos: %s, %d listed, %d hidden, %d folders, sort %s, selected %s%s",
			g.demosOpen ? "open" : "closed", (int)g.demos.demos.size(), g.demos.hidden, (int)g.demos.groups.size(),
			g.demos.sort.c_str(), g.demosState.selected.empty() ? "none" : g.demosState.selected.c_str(),
			g.convertIndex >= 0 ? ", converting" : "");
		g.reply = buf;
		return g.reply.c_str();
	}
	if (0 == std::strncmp(action, "editor", 6)) action += 6;
	while (*action == ' ') ++action;
	const bool editorOpen = g.editor.open && !ConfigOpen();
	if (0 == std::strcmp(action, "on")) Open(Workspace::Editor, "command");
	else if (0 == std::strcmp(action, "off")) { if (editorOpen) Close("command"); }
	else if (0 == std::strcmp(action, "toggle")) { if (editorOpen) Close("command"); else Open(Workspace::Editor, "command"); }
	else if (*action && 0 != std::strcmp(action, "state")) {
		g.reply = "mirv_filmmaker editor [on|off|toggle|state]";
		return g.reply.c_str();
	}
	char buf[256];
	std::snprintf(buf, sizeof(buf), "editor: %s, mouse %s, tab %d, dock %s, preview (%.0f %.0f %.0f %.0f), panel hovered %d, fonts %s at scale %.3f, frames %u",
		editorOpen ? "open" : (g.editor.open ? "closed (config open)" : "closed"), ModeName(g.mouse.GetMode()), (int)g.editor.tab,
		g.editor.dock == DockTab::Timeline ? "timeline" : "regular",
		g.preview.x0, g.preview.y0, g.preview.x1, g.preview.y1, g.panelHovered ? 1 : 0,
		g.fonts.ok ? "ok" : (g.fonts.scale > 0 ? "FALLBACK" : "not built"), g.fonts.scale, g.frames);
	g.reply = buf;
	return g.reply.c_str();
}

EMSCRIPTEN_KEEPALIVE const char* mockup_state() {
	g.reply = StateJson();
	return g.reply.c_str();
}

} // extern "C"

int main() {
	EmscriptenWebGLContextAttributes attrs;
	emscripten_webgl_init_context_attributes(&attrs);
	attrs.majorVersion = 2;
	attrs.alpha = false;
	attrs.antialias = false;
	attrs.preserveDrawingBuffer = true; // so captures and the comparison scripts can read the canvas
	g.gl = emscripten_webgl_create_context("#canvas", &attrs);
	if (g.gl <= 0) {
		std::printf("[mockup] WebGL2 is not available\n");
		return 1;
	}
	emscripten_webgl_make_context_current(g.gl);
	emscripten_set_canvas_element_size("#canvas", g.width, g.height);
	SeedFollow();
	SeedDemos();

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	io.BackendPlatformName = "UiMockupSource";
	io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
	ImGui_ImplOpenGL3_Init("#version 300 es");
	emscripten_set_main_loop(Frame, 0, false);
	return 0;
}
