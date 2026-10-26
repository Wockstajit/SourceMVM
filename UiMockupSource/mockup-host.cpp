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
//   - the window: index.html forwards DOM mouse and key events here as the game host's window
//     procedure forwards window messages, and asks for pointer lock while the game has the mouse.
//
// Everything else, from which panel takes a click to when a right press becomes a look, is the
// game's code. Built by build-mockup.mjs.

#include "../AfxHookSource/Filmmaker/SourceDemoBarModel.h"
#include "../AfxHookSource/Filmmaker/Ui/CameraEditorShell.h"
#include "../AfxHookSource/Filmmaker/Ui/DirectorHud.h"
#include "../AfxHookSource/Filmmaker/Ui/DirectorMath.h"
#include "../AfxHookSource/Filmmaker/Ui/DirectorMouse.h"
#include "../AfxHookSource/Filmmaker/Ui/UiKeys.h"
#include "../AfxHookSource/Filmmaker/Ui/WorkspaceMouse.h"
#include "../AfxHookSource2/Filmmaker/Panorama/HudReadoutCadenceMath.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <GLES3/gl3.h>
#include <emscripten.h>
#include <emscripten/html5.h>

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
	SimDemo demo = MakeSimDemo();
	std::string reply;          // backing store for strings handed to JavaScript
};
Host g;

// The game host logs these as mvm_debug lines; here they go to the browser console.
void LogLine(const char* channel, const char* text) { std::printf("[%s] %s\n", channel, text); }

// The movie director runs while a demo plays and no workspace is open.
bool DirectorActive() { return !g.editor.open && g.demo.playing; }

const char* DirectorModeName(DirectorMode m) { return m == DirectorMode::Look ? "look" : "free"; }

void ApplyMode(Mode before, const char* reason) {
	const Mode now = g.mouse.GetMode();
	if (now == before) return;
	char line[96];
	std::snprintf(line, sizeof(line), "mode=%s reason=%s", ModeName(now), reason);
	LogLine("ui.mouse", line);
}

void Open(const char* reason) {
	if (g.editor.open) return;
	g.editor.open = true;
	g.mouse.Reset();
	LogLine("ui.editor", (std::string("open reason=") + reason).c_str());
}

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
		case DemoBarAction::Kind::Speed: d.timescale = a.value; LogLine("demo.bar", ("timescale " + std::to_string(a.value)).c_str()); break;
		case DemoBarAction::Kind::ToggleLoop:
			d.loopOn = !d.loopOn;
			if (d.loopOn) {
				if (!d.loop.Valid()) d.loop = DemoBarMath::SeedLoop((int)d.tick, d.total);
				if (d.tick < d.loop.in || d.tick >= d.loop.out) SeekSim(d.loop.in);
			}
			LogLine("demo.bar", d.loopOn ? "loop on" : "loop off");
			break;
		case DemoBarAction::Kind::NextCamera: SetCamera(DirectorMath::StepCamera(d.observerMode, 1)); break;
		case DemoBarAction::Kind::CameraEditor: Open("cam editor button"); break;
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
	DemoBarModel demo;
	BuildDemoModel(demo);
	CameraEditorFrame frame;
	DirectorFrame directorFrame;
	if (g.editor.open) {
		frame = DrawCameraEditor(g.editor, g.fonts, (float)g.width, (float)g.height, demo);
	} else if (director) {
		if (g.cadence.Ready((uint64_t)nowMs, 10, false)) BuildCards(g.cards);
		// The backdrop is a CS:S spectator capture, so its spectator bars are where the game's are.
		const float spectatorBar = (float)g.height * DirectorMath::kSpectatorBarShare;
		directorFrame = DrawDirector(g.director, demo, g.cards, g.fonts, (float)g.width, (float)g.height, spectatorBar, spectatorBar);
	}
	ImGui::Render();
	ApplyDemoActions(frame.demoActions);
	ApplyDemoActions(directorFrame.demoActions);
	g.bar = directorFrame.bar;
	g.preview = frame.previewClient;
	g.panelHovered = g.editor.open && io.WantCaptureMouse;
	++g.frames;

	glViewport(0, 0, g.width, g.height);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	ImDrawList game(ImGui::GetDrawListSharedData());
	game._ResetForNewFrame();
	game.PushClipRect(ImVec2(0, 0), ImVec2((float)g.width, (float)g.height));
	game.PushTextureID(io.Fonts->TexID);
	DrawGame(&game, g.editor.open ? frame.previewClient : MakeRect(0, 0, (float)g.width, (float)g.height));
	RenderList(game);
	if (g.editor.open || director) ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

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
	if (!g.editor.open) {
		if (!DirectorActive()) return 0;
		const DirectorMath::Key key = DirectorMath::KeyFor(vk);
		if (key == DirectorMath::Key::None) return 0;
		if (down && !repeat) DirectorKey(key); // both edges are the director's; auto-repeat is ignored
		return 1;
	}
	ImGuiIO& io = ImGui::GetIO();
	const WorkspaceKey key = WorkspaceKeyFor(vk, io.WantTextInput);
	if (down) {
		switch (key) {
		case WorkspaceKey::Close:
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
	if (0 == std::strncmp(action, "editor", 6)) action += 6;
	while (*action == ' ') ++action;
	if (0 == std::strcmp(action, "on")) Open("command");
	else if (0 == std::strcmp(action, "off")) Close("command");
	else if (0 == std::strcmp(action, "toggle")) { if (g.editor.open) Close("command"); else Open("command"); }
	else if (*action && 0 != std::strcmp(action, "state")) {
		g.reply = "mirv_filmmaker editor [on|off|toggle|state]";
		return g.reply.c_str();
	}
	char buf[256];
	std::snprintf(buf, sizeof(buf), "editor: %s, mouse %s, tab %d, dock %s, preview (%.0f %.0f %.0f %.0f), panel hovered %d, fonts %s at scale %.3f, frames %u",
		g.editor.open ? "open" : "closed", ModeName(g.mouse.GetMode()), (int)g.editor.tab,
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
