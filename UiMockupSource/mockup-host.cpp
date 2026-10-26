// Browser host for the Source 1 workspace UI. It compiles the same C++ the game hook runs
// (AfxHookSource/Filmmaker/Ui: the Camera Editor shell, the CS:S style, CS2's layout and mouse
// rules) to WebAssembly and stands in for the two things a browser does not have:
//
//   - the game: a still frame (assets/backdrop.png, a local CS:S capture) or a drawn stand-in,
//     letterboxed into the preview exactly where the game renderer puts the real frame, and turned
//     by the mouse while the game has it;
//   - the window: index.html forwards DOM mouse and key events here as the game host's window
//     procedure forwards window messages, and asks for pointer lock while the game has the mouse.
//
// Everything else, from which panel takes a click to when a right press becomes a look, is the
// game's code. Built by build-mockup.mjs.

#include "../AfxHookSource/Filmmaker/Ui/CameraEditorShell.h"
#include "../AfxHookSource/Filmmaker/Ui/UiKeys.h"
#include "../AfxHookSource/Filmmaker/Ui/WorkspaceMouse.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <GLES3/gl3.h>
#include <emscripten.h>
#include <emscripten/html5.h>

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using namespace SourceFilmmaker::Ui;

namespace {

using Mode = WorkspaceMouse::Mode;
using Route = WorkspaceMouse::Route;

const char* ModeName(Mode m) {
	switch (m) {
	case Mode::Ui: return "ui";
	case Mode::Game: return "game";
	default: return "look";
	}
}

struct Host {
	EMSCRIPTEN_WEBGL_CONTEXT_HANDLE gl = 0;
	FontSet fonts;
	CameraEditorState editor;
	WorkspaceMouse mouse;
	Rect preview;               // last frame's preview, client px
	bool panelHovered = false;  // last frame: the pointer is over a panel
	int width = 854, height = 480;
	double lastMs = 0;
	unsigned frames = 0;
	GLuint backdrop = 0;
	int backdropW = 0, backdropH = 0;
	float yaw = 0, pitch = 0;   // the stand-in game camera, degrees
	std::string reply;          // backing store for strings handed to JavaScript
};
Host g;

// The game host logs these as mvm_debug lines; here they go to the browser console.
void LogLine(const char* channel, const char* text) { std::printf("[%s] %s\n", channel, text); }

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
	if (g.mouse.GetMode() != Mode::Ui) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); // nothing hovers while the game has the mouse

	ImGui::NewFrame();
	CameraEditorFrame frame;
	if (g.editor.open) frame = DrawCameraEditor(g.editor, g.fonts, (float)g.width, (float)g.height);
	ImGui::Render();
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
	if (g.editor.open) ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

	if (frame.exitClicked) Close("exit chip");
}

std::string StateJson() {
	char buf[512];
	std::snprintf(buf, sizeof(buf),
		"{\"open\":%s,\"mode\":\"%s\",\"rightPending\":%s,\"tab\":%d,\"dock\":\"%s\","
		"\"preview\":[%.1f,%.1f,%.1f,%.1f],\"panelHovered\":%s,\"fonts\":\"%s\",\"scale\":%.4f,"
		"\"width\":%d,\"height\":%d,\"frames\":%u,\"yaw\":%.1f,\"pitch\":%.1f,\"backdrop\":%s}",
		g.editor.open ? "true" : "false", ModeName(g.mouse.GetMode()), g.mouse.RightPending() ? "true" : "false",
		(int)g.editor.tab, g.editor.dock == DockTab::Timeline ? "timeline" : "regular",
		g.preview.x0, g.preview.y0, g.preview.x1, g.preview.y1, g.panelHovered ? "true" : "false",
		g.fonts.ok ? "ok" : (g.fonts.scale > 0 ? "fallback" : "not built"), g.fonts.scale,
		g.width, g.height, g.frames, g.yaw, g.pitch, g.backdrop ? "true" : "false");
	return buf;
}

} // namespace

extern "C" {

// Mouse input in canvas px. type: 0 move, 1 button down, 2 button up, 3 wheel (delta in notches,
// positive away from the user). Returns where the event went: 0 the UI, 1 the game, 2 nowhere.
EMSCRIPTEN_KEEPALIVE int mockup_mouse(int type, int button, float x, float y, float wheel) {
	if (!g.editor.open) return 1;
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
	if (g.editor.open && g.mouse.GetMode() == Mode::Ui) return;
	g.yaw += dx * 0.1f;
	g.pitch += dy * 0.1f;
	if (g.pitch > 60) g.pitch = 60;
	if (g.pitch < -60) g.pitch = -60;
}

// A key, as a DOM keyCode (the Windows virtual-key number). Returns 1 when the workspace used it.
EMSCRIPTEN_KEEPALIVE int mockup_key(int vk, int down, int repeat) {
	if (!g.editor.open) return 0;
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
}

// The browser let go of the pointer lock on its own (Esc, or the tab lost focus) while the game had
// the mouse. The page cannot keep the game's capture, so the mouse comes back like G.
EMSCRIPTEN_KEEPALIVE void mockup_lock_lost() {
	const Mode before = g.mouse.GetMode();
	if (before == Mode::Game) g.mouse.ToggleGame();
	else g.mouse.FocusLost();
	ApplyMode(before, "pointer lock lost");
}

EMSCRIPTEN_KEEPALIVE int mockup_mode() { return (int)g.mouse.GetMode(); }

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
