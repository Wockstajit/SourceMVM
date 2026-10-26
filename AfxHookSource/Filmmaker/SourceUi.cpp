#include "stdafx.h"

#include "SourceUi.h"

#include "Ui/CameraEditorShell.h"
#include "Ui/UiRenderDx9.h"
#include "Ui/UiStyle.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <AfxHookSource2/Filmmaker/Movie/UiClickMath.h>
#include <Shared/AfxConsole.h>

#include <imgui.h>

#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace SourceFilmmaker {

namespace {

using namespace Ui;

enum class MouseMode { Ui, Game, Look };

const char* MouseModeName(MouseMode m) {
	switch (m) {
	case MouseMode::Ui: return "ui";
	case MouseMode::Game: return "game";
	default: return "look";
	}
}

struct RightPress {
	bool pending = false;
	int x = 0, y = 0;
	POINT screen = { 0, 0 };
	std::chrono::steady_clock::time_point at;
};

// Main-thread state.
struct Host {
	ImGuiContext* context = nullptr;
	FontSet fonts;
	std::string fontDir;
	CameraEditorState editor;
	HWND window = nullptr;
	MouseMode mouse = MouseMode::Ui;
	Rect preview;          // last frame's preview, client px
	bool panelHovered = false; // last frame: the pointer is over a panel
	RightPress right;
	POINT gameCursor = { 0, 0 };
	bool haveGameCursor = false;
	int shownCursorSteps = 0;  // ShowCursor(TRUE) calls we made to make the arrow visible
	LARGE_INTEGER lastTime = {};
	LARGE_INTEGER frequency = {};
	unsigned frames = 0;
};
Host g;

// Read by the cursor and raw-input filters.
std::atomic<bool> g_open{ false };
std::atomic<bool> g_uiOwnsMouse{ false };

// Hand-off to the render thread.
std::mutex g_frameMutex;
std::shared_ptr<DrawSnapshot> g_latest;
FontPixels g_fontPixels;
// Render thread only.
RendererDx9 g_renderer;
FontPixels g_renderFont;

std::string FontsFolder() {
	PWSTR path = nullptr;
	std::string out;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &path)) && path) {
		const int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
		if (n > 1) {
			out.resize((size_t)n - 1);
			WideCharToMultiByte(CP_UTF8, 0, path, -1, &out[0], n, nullptr, nullptr);
		}
	}
	if (path) CoTaskMemFree(path);
	return out.empty() ? std::string("C:\\Windows\\Fonts") : out;
}

void Publish(std::shared_ptr<DrawSnapshot> snapshot) {
	std::lock_guard<std::mutex> lock(g_frameMutex);
	g_latest = std::move(snapshot);
}

// While the UI owns the mouse the arrow must be visible even if the game hid it with ShowCursor.
void ShowArrow(bool show) {
	if (show) {
		while (ShowCursor(TRUE) < 0 && g.shownCursorSteps < 16) ++g.shownCursorSteps;
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
	} else {
		for (; g.shownCursorSteps > 0; --g.shownCursorSteps) ShowCursor(FALSE);
	}
}

void SetMouse(MouseMode mode, const char* reason) {
	if (g.mouse == mode) return;
	g.mouse = mode;
	g.right.pending = false;
	const bool uiOwns = g_open.load() && mode == MouseMode::Ui;
	g_uiOwnsMouse.store(uiOwns);
	g.haveGameCursor = false;
	ShowArrow(uiOwns);
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("ui.mouse", "mode=%s reason=%s", MouseModeName(mode), reason);
}

void Open(const char* reason) {
	if (g_open.load()) return;
	g_open.store(true);
	g.editor.open = true;
	g.mouse = MouseMode::Game; // so SetMouse below runs its transition
	SetMouse(MouseMode::Ui, reason);
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("ui.editor", "open reason=%s", reason);
}

void Close(const char* reason) {
	if (!g_open.load()) return;
	g_open.store(false);
	g.editor.open = false;
	g_uiOwnsMouse.store(false);
	g.right.pending = false;
	g.mouse = MouseMode::Ui;
	ShowArrow(false);
	Publish(nullptr);
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("ui.editor", "close reason=%s", reason);
}

void EnsureContext() {
	if (g.context) return;
	IMGUI_CHECKVERSION();
	g.context = ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	io.BackendPlatformName = "AfxHookSource";
	io.BackendRendererName = "AfxHookSource DX9";
	io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
	g.fontDir = FontsFolder();
	QueryPerformanceFrequency(&g.frequency);
	QueryPerformanceCounter(&g.lastTime);
}

// Builds the font atlas and style for this window height when the scale changed.
void EnsureFonts(float clientH) {
	const float scale = clientH / kStageHeight;
	if (g.fonts.scale > 0 && std::fabs(g.fonts.scale - scale) < 0.001f) return;
	ImGuiIO& io = ImGui::GetIO();
	g.fonts = BuildFonts(io.Fonts, g.fontDir, scale);
	ApplyStyle(ImGui::GetStyle(), scale);
	unsigned char* pixels = nullptr;
	int w = 0, h = 0;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
	io.Fonts->SetTexID(kFontTextureId);
	{
		std::lock_guard<std::mutex> lock(g_frameMutex);
		g_fontPixels.width = w;
		g_fontPixels.height = h;
		g_fontPixels.rgba.assign(pixels, pixels + (size_t)w * h * 4);
		++g_fontPixels.version;
	}
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("ui.fonts", "scale=%.3f atlas=%dx%d files=%s dir=%s", scale, w, h,
			g.fonts.ok ? "ok" : "MISSING (built-in fallback)", g.fontDir.c_str());
}

// A right press in the preview becomes a look once it is held or dragged.
void UpdateRightPress(int x, int y, bool moved) {
	if (!g.right.pending) return;
	const int heldMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - g.right.at).count();
	const int dx = moved ? x - g.right.x : 0, dy = moved ? y - g.right.y : 0;
	if (!Filmmaker::UiClick::RightPressBecomesLook(heldMs, dx, dy)) return;
	g.right.pending = false;
	SetMouse(MouseMode::Look, "right hold");
}

// By geometry, not by ImGui's hover state: that is last frame's, and a press that arrives in the same
// message batch as the move to it would be judged by where the pointer was before. No panel overlaps
// the preview (the layout keeps them apart), so the rect alone decides.
bool InPreview(int x, int y) { return g.preview.Contains((float)x, (float)y); }

ImGuiKey VirtualKeyToImGui(WPARAM vk) {
	if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
	if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
	switch (vk) {
	case VK_TAB: return ImGuiKey_Tab;
	case VK_LEFT: return ImGuiKey_LeftArrow;
	case VK_RIGHT: return ImGuiKey_RightArrow;
	case VK_UP: return ImGuiKey_UpArrow;
	case VK_DOWN: return ImGuiKey_DownArrow;
	case VK_HOME: return ImGuiKey_Home;
	case VK_END: return ImGuiKey_End;
	case VK_DELETE: return ImGuiKey_Delete;
	case VK_BACK: return ImGuiKey_Backspace;
	case VK_SPACE: return ImGuiKey_Space;
	case VK_RETURN: return ImGuiKey_Enter;
	case VK_SHIFT: return ImGuiKey_LeftShift;
	case VK_CONTROL: return ImGuiKey_LeftCtrl;
	case VK_MENU: return ImGuiKey_LeftAlt;
	default: return ImGuiKey_None;
	}
}

void FeedKey(WPARAM vk, bool down) {
	if (!g.context) return;
	ImGuiIO& io = ImGui::GetIO();
	if (vk == VK_SHIFT) io.AddKeyEvent(ImGuiMod_Shift, down);
	if (vk == VK_CONTROL) io.AddKeyEvent(ImGuiMod_Ctrl, down);
	if (vk == VK_MENU) io.AddKeyEvent(ImGuiMod_Alt, down);
	const ImGuiKey key = VirtualKeyToImGui(vk);
	if (key != ImGuiKey_None) io.AddKeyEvent(key, down);
}

bool OnMouse(UINT msg, WPARAM wParam, LPARAM lParam) {
	const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
	ImGuiIO* io = g.context ? &ImGui::GetIO() : nullptr;

	if (g.mouse == MouseMode::Look) {
		if (msg == WM_RBUTTONUP) {
			SetMouse(MouseMode::Ui, "look released");
			SetCursorPos(g.right.screen.x, g.right.screen.y);
			return true;
		}
		return false; // the game is looking around
	}
	if (g.mouse == MouseMode::Game) return false;

	// UI mode.
	switch (msg) {
	case WM_MOUSEMOVE:
		if (io) io->AddMousePosEvent((float)x, (float)y);
		UpdateRightPress(x, y, true);
		return true;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK: {
		Filmmaker::UiClick::Input in;
		in.uiModeOn = true;
		in.workspace = true;
		in.preview.valid = g.preview.Valid();
		in.preview.x0 = g.preview.x0; in.preview.y0 = g.preview.y0;
		in.preview.x1 = g.preview.x1; in.preview.y1 = g.preview.y1;
		in.x = (float)x; in.y = (float)y;
		if (Filmmaker::UiClick::ClickSwitchesToGameMode(in)) {
			SetMouse(MouseMode::Game, "click in preview");
			return false; // the click carries on to the game
		}
		if (io) { io->AddMousePosEvent((float)x, (float)y); io->AddMouseButtonEvent(0, true); }
		return true;
	}
	case WM_LBUTTONUP:
		if (io) io->AddMouseButtonEvent(0, false);
		return true;
	case WM_RBUTTONDOWN:
	case WM_RBUTTONDBLCLK:
		if (InPreview(x, y)) {
			g.right.pending = true;
			g.right.x = x;
			g.right.y = y;
			g.right.at = std::chrono::steady_clock::now();
			GetCursorPos(&g.right.screen);
			return true;
		}
		if (io) io->AddMouseButtonEvent(1, true);
		return true;
	case WM_RBUTTONUP:
		if (g.right.pending) {
			// Released before it became a look: a click. CS2 opens the object menu here; there is
			// nothing to pick in the CS:S preview yet.
			g.right.pending = false;
			return true;
		}
		if (io) io->AddMouseButtonEvent(1, false);
		return true;
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
		if (io) io->AddMouseButtonEvent(2, msg == WM_MBUTTONDOWN);
		return true;
	case WM_MOUSEWHEEL: {
		POINT p = { x, y };
		if (g.window) ScreenToClient(g.window, &p); // wheel coordinates are screen coordinates
		if (InPreview(p.x, p.y)) return false;       // over the preview the wheel is the game's
		if (io) io->AddMouseWheelEvent(0.0f, (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA);
		return true;
	}
	default:
		return false;
	}
}

} // namespace

bool Ui_OnWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (!g.window) g.window = hwnd;
	if (!g_open.load()) return false;

	switch (msg) {
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (wParam == VK_ESCAPE) { Close("esc"); return true; }
		if (wParam == 'G' && !(g.context && ImGui::GetIO().WantTextInput)) {
			if (!(lParam & (1 << 30))) // not auto-repeat
				SetMouse(g.mouse == MouseMode::Ui ? MouseMode::Game : MouseMode::Ui, "G");
			return true;
		}
		FeedKey(wParam, true);
		return g.context && ImGui::GetIO().WantCaptureKeyboard;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		FeedKey(wParam, false);
		if (wParam == VK_ESCAPE || wParam == 'G') return true;
		return g.context && ImGui::GetIO().WantCaptureKeyboard;
	case WM_CHAR:
		if (g.context && ImGui::GetIO().WantTextInput) {
			ImGui::GetIO().AddInputCharacterUTF16((unsigned short)wParam);
			return true;
		}
		return false;
	case WM_SETCURSOR:
		if (g_uiOwnsMouse.load() && LOWORD(lParam) == HTCLIENT) {
			SetCursor(LoadCursor(nullptr, IDC_ARROW));
			return true;
		}
		return false;
	case WM_KILLFOCUS:
		g.right.pending = false;
		if (g.mouse == MouseMode::Look) SetMouse(MouseMode::Ui, "focus lost");
		return false;
	case WM_MOUSEMOVE:
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_RBUTTONDBLCLK:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_MOUSEWHEEL: {
		const MouseMode before = g.mouse;
		const bool consumed = OnMouse(msg, wParam, lParam);
		if (msg != WM_MOUSEMOVE && Filmmaker::MvmDebugLog_Active())
			Filmmaker::MvmDebugLog_Linef("ui.input", "msg=0x%x x=%d y=%d mode=%s->%s consumed=%d panelHovered=%d",
				msg, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), MouseModeName(before), MouseModeName(g.mouse),
				consumed ? 1 : 0, g.panelHovered ? 1 : 0);
		return consumed;
	}
	default:
		return false;
	}
}

void Ui_MainThreadFrame() {
	if (!g_open.load() || !g.window) return;
	RECT rc;
	if (!GetClientRect(g.window, &rc)) return;
	const float w = (float)(rc.right - rc.left), h = (float)(rc.bottom - rc.top);
	if (!(w > 0) || !(h > 0)) return;

	EnsureContext();
	EnsureFonts(h);

	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(w, h);
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	float dt = g.frequency.QuadPart > 0 ? (float)(now.QuadPart - g.lastTime.QuadPart) / (float)g.frequency.QuadPart : 0.0f;
	g.lastTime = now;
	io.DeltaTime = dt > 0.0f ? (dt < 0.25f ? dt : 0.25f) : 1.0f / 60.0f;
	if (g.mouse != MouseMode::Ui) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); // nothing hovers while the game has the mouse
	UpdateRightPress(0, 0, false); // a press held long enough looks even without moving

	ImGui::NewFrame();
	const CameraEditorFrame frame = DrawCameraEditor(g.editor, g.fonts, w, h);
	ImGui::Render();

	g.preview = frame.previewClient;
	if (g.panelHovered != io.WantCaptureMouse && Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("ui.hover", "panelHovered=%d mouse=(%.0f %.0f) tab=%d dock=%d",
			io.WantCaptureMouse ? 1 : 0, io.MousePos.x, io.MousePos.y, (int)g.editor.tab, (int)g.editor.dock);
	g.panelHovered = io.WantCaptureMouse;
	++g.frames;

	if (frame.exitClicked) {
		Close("exit chip");
		return;
	}
	std::shared_ptr<DrawSnapshot> snapshot = TakeSnapshot(ImGui::GetDrawData());
	snapshot->letterbox = true;
	snapshot->preview = frame.previewClient;
	Publish(std::move(snapshot));
}

void Ui_RenderThreadPresent(IDirect3DDevice9* device) {
	std::shared_ptr<DrawSnapshot> snapshot;
	{
		std::lock_guard<std::mutex> lock(g_frameMutex);
		snapshot = g_latest;
		if (snapshot && g_renderFont.version != g_fontPixels.version) g_renderFont = g_fontPixels;
	}
	if (!snapshot) return;
	g_renderer.Render(device, *snapshot, g_renderFont);
}

void Ui_DeviceLost() { g_renderer.ReleaseDeviceObjects(); }

bool Ui_OnSetCursorPos(int x, int y) {
	if (!g_uiOwnsMouse.load()) return false;
	// The game recentres the cursor for mouse look; while the UI has the mouse it may not move it.
	g.gameCursor.x = x;
	g.gameCursor.y = y;
	g.haveGameCursor = true;
	return true;
}

bool Ui_OnGetCursorPos(LPPOINT point) {
	if (!g_uiOwnsMouse.load() || !g.haveGameCursor || !point) return false;
	// Where the game last put the cursor, so it measures no movement.
	*point = g.gameCursor;
	return true;
}

bool Ui_OnSetCursor(HCURSOR cursor, HCURSOR& result) {
	(void)cursor;
	if (!g_uiOwnsMouse.load()) return false;
	result = SetCursor(LoadCursor(nullptr, IDC_ARROW));
	return true;
}

void Ui_FilterRawInput(UINT result, UINT command, LPVOID data) {
	if (!g_uiOwnsMouse.load() || command != RID_INPUT || !data || result == (UINT)-1 || result < sizeof(RAWINPUTHEADER))
		return;
	RAWINPUT* input = (RAWINPUT*)data;
	if (input->header.dwType != RIM_TYPEMOUSE) return;
	input->data.mouse.lLastX = 0;
	input->data.mouse.lLastY = 0;
	input->data.mouse.usButtonFlags = 0;
	input->data.mouse.usButtonData = 0;
}

bool Ui_EditorOpen() { return g_open.load(); }

void Ui_EditorCommand(int argc, advancedfx::ICommandArgs* args) {
	const char* action = argc >= 3 ? args->ArgV(2) : "state";
	if (0 == _stricmp(action, "on")) Open("command");
	else if (0 == _stricmp(action, "off")) Close("command");
	else if (0 == _stricmp(action, "toggle")) { if (g_open.load()) Close("command"); else Open("command"); }
	else if (0 != _stricmp(action, "state")) {
		advancedfx::Message("mirv_filmmaker editor [on|off|toggle|state]\n");
		return;
	}
	advancedfx::Message("editor: %s, mouse %s, tab %d, dock %s, preview (%.0f %.0f %.0f %.0f), panel hovered %d, fonts %s at scale %.3f, frames %u\n",
		g_open.load() ? "open" : "closed", MouseModeName(g.mouse), (int)g.editor.tab,
		g.editor.dock == DockTab::Timeline ? "timeline" : "regular",
		g.preview.x0, g.preview.y0, g.preview.x1, g.preview.y1, g.panelHovered ? 1 : 0,
		g.fonts.ok ? "ok" : (g.fonts.scale > 0 ? "FALLBACK" : "not built"), g.fonts.scale, g.frames);
}

} // namespace SourceFilmmaker
