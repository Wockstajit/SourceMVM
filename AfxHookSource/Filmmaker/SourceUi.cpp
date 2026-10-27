#include "stdafx.h"

#include "SourceUi.h"
#include "SourceCameraPath.h"
#include "SourceCapture.h"
#include "SourceDemoBar.h"
#include "SourceDirector.h"
#include "SourceEngine.h"
#include "SourceMainThread.h"

#include "Ui/CameraEditorShell.h"
#include "Ui/DirectorHud.h"
#include "Ui/DirectorMouse.h"
#include "Ui/MarkerPopup.h"
#include "Ui/PathOverlay.h"
#include "Ui/UiRenderDx9.h"
#include "Ui/UiKeys.h"
#include "Ui/UiStyle.h"
#include "Ui/WorkspaceMouse.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <AfxHookSource2/Filmmaker/Panorama/HudReadoutCadenceMath.h>
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

const char* DirectorModeName(DirectorMode m) { return m == DirectorMode::Look ? "look" : "free"; }

int64_t NowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Main-thread state.
struct Host {
	ImGuiContext* context = nullptr;
	FontSet fonts;
	std::string fontDir;
	CameraEditorState editor;
	HWND window = nullptr;
	WorkspaceMouse mouse;  // who has the mouse (CS2's rules)
	DirectorMouse directorMouse; // the same outside the workspaces, while the director runs
	DirectorCardsModel cards;    // the director cards' readouts, refreshed at CS2's 10 Hz
	Filmmaker::HudReadoutCadence cadence;
	Rect bar;              // last frame's director bar panel, client px
	int wheelRemainder = 0; // wheel delta short of a notch
	Rect preview;          // last frame's preview, client px; empty while a dialog is up, so clicks stay the UI's
	bool panelHovered = false; // last frame: the pointer is over a panel
	POINT gameCursor = { 0, 0 };
	bool haveGameCursor = false;
	int shownCursorSteps = 0;  // ShowCursor(TRUE) calls we made to make the arrow visible
	LARGE_INTEGER lastTime = {};
	LARGE_INTEGER frequency = {};
	unsigned frames = 0;
	bool wasRepositioning = false; // last frame: a camera was being repositioned
	MarkerCardState markerCard;    // the CAMERA MARKER card (F on an aimed marker)
	int64_t repositionPressAt = -1; // a left press while repositioning outside the editor, ms
	std::unique_ptr<ImDrawList> worldList; // the camera path in the world, built with each rendered view
	unsigned worldFrames = 0;
	int worldVertices = 0;
	int64_t worldLogAt = 0;
};
Host g;

// Read by the cursor and raw-input filters.
std::atomic<bool> g_open{ false };
std::atomic<bool> g_director{ false }; // the movie director runs (a demo, no workspace)
std::atomic<bool> g_uiOwnsMouse{ false };
std::atomic<bool> g_markerMenu{ false }; // the CAMERA MARKER card is up: every press and notch is the card's

// Hand-off to the render thread.
std::mutex g_frameMutex;
std::shared_ptr<DrawSnapshot> g_latest;
std::shared_ptr<DrawSnapshot> g_latestWorld; // drawn over the game's frame, under the UI and its letterbox
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
	if (!snapshot) g_latestWorld.reset(); // nothing of ours over the game
	g_latest = std::move(snapshot);
}

void PublishWorld(std::shared_ptr<DrawSnapshot> world) {
	std::lock_guard<std::mutex> lock(g_frameMutex);
	g_latestWorld = std::move(world);
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

// Applies the cursor side of a mode change: the arrow is visible while the UI has the mouse, the
// game's own capture is left alone otherwise, and a finished look puts the cursor back where the
// right press started.
void ApplyMode(Mode before, const char* reason) {
	const Mode now = g.mouse.GetMode();
	if (now == before) return;
	const bool uiOwns = g_open.load() && now == Mode::Ui;
	g_uiOwnsMouse.store(uiOwns);
	g.haveGameCursor = false;
	ShowArrow(uiOwns);
	if (before == Mode::Look && now == Mode::Ui && g.window) {
		POINT p = { (LONG)g.mouse.LookX(), (LONG)g.mouse.LookY() };
		ClientToScreen(g.window, &p);
		SetCursorPos(p.x, p.y);
	}
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("ui.mouse", "mode=%s reason=%s", ModeName(now), reason);
}

void Open(const char* reason) {
	if (g_open.load()) return;
	g_open.store(true);
	g.editor.open = true;
	g.mouse.Reset();
	g_uiOwnsMouse.store(true);
	ShowArrow(true);
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("ui.editor", "open reason=%s", reason);
}

void Close(const char* reason) {
	if (!g_open.load()) return;
	g_open.store(false);
	g.editor.open = false;
	g_uiOwnsMouse.store(false);
	g.mouse.Reset();
	ShowArrow(false);
	Publish(nullptr);
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("ui.editor", "close reason=%s", reason);
}

// The director's cursor side: while it is free the arrow shows and the game gets no mouse; a look
// gives the game the mouse, and its end puts the cursor back where the press was.
void ApplyDirectorMode(DirectorMode before, const char* reason) {
	const DirectorMode now = g.directorMouse.GetMode();
	if (now == before) return;
	const bool uiOwns = g_director.load() && now == DirectorMode::Free;
	g_uiOwnsMouse.store(uiOwns);
	g.haveGameCursor = false;
	ShowArrow(uiOwns);
	if (before == DirectorMode::Look && now == DirectorMode::Free && g.window) {
		POINT p = { (LONG)g.directorMouse.LookX(), (LONG)g.directorMouse.LookY() };
		ClientToScreen(g.window, &p);
		SetCursorPos(p.x, p.y);
	}
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("director.mouse", "mode=%s reason=%s", DirectorModeName(now), reason);
}

void EnterDirector() {
	g_director.store(true);
	g.directorMouse.Reset();
	g.wheelRemainder = 0;
	g_uiOwnsMouse.store(true);
	g.haveGameCursor = false;
	ShowArrow(true);
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("director", "on");
}

void LeaveDirector() {
	g_director.store(false);
	g.directorMouse.Reset();
	g.bar = Rect();
	if (!g_open.load()) { // a workspace that opened keeps the mouse
		g_uiOwnsMouse.store(false);
		ShowArrow(false);
		Publish(nullptr);
	}
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("director", "off");
}

bool OnDirectorMouse(UINT msg, WPARAM wParam, LPARAM lParam) {
	const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
	ImGuiIO* io = g.context ? &ImGui::GetIO() : nullptr;
	const int64_t now = NowMs();
	const DirectorMode before = g.directorMouse.GetMode();
	DirectorRoute route = DirectorRoute::Swallow;
	const char* reason = "input";
	switch (msg) {
	case WM_MOUSEMOVE:
		route = g.directorMouse.Move((float)x, (float)y, now);
		if (route == DirectorRoute::Ui && io) io->AddMousePosEvent((float)x, (float)y);
		reason = "drag";
		break;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONDBLCLK:
	case WM_MBUTTONDOWN: {
		const int button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) ? DirectorMouse::Left
			: (msg == WM_MBUTTONDOWN ? DirectorMouse::Middle : DirectorMouse::Right);
		if (CameraPath_OwnsView() && !g.bar.Contains((float)x, (float)y) && g.directorMouse.GetMode() == DirectorMode::Free) {
			route = DirectorRoute::Swallow; // the path flies the view: a press is no look and no player step (CS2)
			break;
		}
		// While the CAMERA MARKER card is up, the whole window is the UI's (its catcher).
		const Rect ui = g_markerMenu.load() ? MakeRect(-1e6f, -1e6f, 1e6f, 1e6f) : g.bar;
		if (button == DirectorMouse::Left && CameraPath_Repositioning() && !ui.Contains((float)x, (float)y)) g.repositionPressAt = now;
		route = g.directorMouse.ButtonDown(button, (float)x, (float)y, now, ui, Director_FreeCamOn());
		if (route == DirectorRoute::Ui && io) {
			io->AddMousePosEvent((float)x, (float)y);
			io->AddMouseButtonEvent(button, true);
		}
		break;
	}
	case WM_LBUTTONUP:
	case WM_RBUTTONUP:
	case WM_MBUTTONUP: {
		const int button = msg == WM_LBUTTONUP ? DirectorMouse::Left : (msg == WM_MBUTTONUP ? DirectorMouse::Middle : DirectorMouse::Right);
		DirectorMouse::Click click = DirectorMouse::Click::None;
		route = g.directorMouse.ButtonUp(button, now, click);
		if (route == DirectorRoute::Ui && io) io->AddMouseButtonEvent(button, false);
		if (CameraPath_Repositioning()) {
			// A click drops the camera being repositioned where the view is; a held press still looks.
			// The free cam turns every press into a look at once (DirectorMouse's steer), so a click
			// is a left press let go before it would have become one (UiClickMath.h's 200 ms).
			const bool quick = button == DirectorMouse::Left && g.repositionPressAt >= 0
				&& !Filmmaker::UiClick::RightPressBecomesLook((int)(now - g.repositionPressAt), 0, 0);
			if (click == DirectorMouse::Click::NextPlayer || quick) MainThread_Run(&CameraPath_PlaceReposition);
			if (button == DirectorMouse::Left) g.repositionPressAt = -1;
		} else if (click != DirectorMouse::Click::None) {
			MainThread_Run([click] { Director_Click(click); });
		}
		reason = "look released";
		break;
	}
	case WM_MOUSEWHEEL: {
		if (g_markerMenu.load()) { // the card is up: the notch is its own
			if (io) io->AddMouseWheelEvent(0.0f, (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA);
			route = DirectorRoute::Ui;
			break;
		}
		// Every notch is the director's while a demo plays, over the bar too (CS2's MovieMode).
		g.wheelRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
		const int notches = g.wheelRemainder / WHEEL_DELTA;
		g.wheelRemainder -= notches * WHEEL_DELTA;
		const WORD keys = GET_KEYSTATE_WPARAM(wParam);
		const bool ctrl = (keys & MK_CONTROL) || GetKeyState(VK_CONTROL) < 0, shift = (keys & MK_SHIFT) || GetKeyState(VK_SHIFT) < 0;
		if (notches) MainThread_Run([notches, ctrl, shift] { Director_Wheel(notches, ctrl, shift); });
		break;
	}
	default:
		return false;
	}
	ApplyDirectorMode(before, reason);
	if (msg != WM_MOUSEMOVE && Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("director.input", "msg=0x%x x=%d y=%d mode=%s->%s route=%d", msg, x, y,
			DirectorModeName(before), DirectorModeName(g.directorMouse.GetMode()), (int)route);
	return route != DirectorRoute::Game;
}

// The director's keys inside the editor, as CS2's MovieMode keeps them there: Space, Left and Right
// on the demo, X and Z roll the free cam, Shift slows it. F8's blackout stays the director's own.
void FeedKey(WPARAM vk, bool down);

bool EditorDirectorKey(UINT msg, WPARAM wParam, LPARAM lParam) {
	const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
	if (wParam == VK_SHIFT) {
		MainThread_Run([down] { Director_SetSlow(down); }); // not used up: ImGui's fine drag needs it too
		return false;
	}
	if (Director_RollKey((int)wParam, down, GetKeyState(VK_CONTROL) < 0)) return true;
	const DirectorMath::Key key = DirectorMath::KeyFor((int)wParam);
	if (key != DirectorMath::Key::TogglePause && key != DirectorMath::Key::SkipBack && key != DirectorMath::Key::SkipForward)
		return false;
	if (g.context && ImGui::GetIO().WantCaptureKeyboard) return false; // a focused control has it
	if (down && !(lParam & (1 << 30))) MainThread_Run([key] { Director_Key(key); });
	return true;
}

bool OnDirectorMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_KEYDOWN:
	case WM_KEYUP: {
		if (wParam == VK_SHIFT) {
			const bool slow = msg == WM_KEYDOWN;
			MainThread_Run([slow] { Director_SetSlow(slow); }); // not used up: the game and mirv_input see it too
			FeedKey(wParam, slow); // the bar's fine drag (the LOOP flags) reads it too
			return false;
		}
		if (CameraPath_OnKey((int)wParam, msg == WM_KEYDOWN, msg == WM_KEYDOWN && (lParam & (1 << 30)))) return true;
		if (Director_RollKey((int)wParam, msg == WM_KEYDOWN, GetKeyState(VK_CONTROL) < 0)) return true;
		const DirectorMath::Key key = DirectorMath::KeyFor((int)wParam);
		if (key == DirectorMath::Key::None) return false;
		if (msg == WM_KEYDOWN && !(lParam & (1 << 30))) MainThread_Run([key] { Director_Key(key); }); // auto-repeat is ignored
		return true; // both edges are the director's, so Space never jumps
	}
	case WM_SETCURSOR:
		if (g_uiOwnsMouse.load() && LOWORD(lParam) == HTCLIENT) {
			SetCursor(LoadCursor(nullptr, IDC_ARROW));
			return true;
		}
		return false;
	case WM_KILLFOCUS: {
		const DirectorMode before = g.directorMouse.GetMode();
		g.directorMouse.FocusLost();
		ApplyDirectorMode(before, "focus lost");
		return false;
	}
	case WM_MOUSEMOVE:
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_RBUTTONDBLCLK:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_MOUSEWHEEL:
		return OnDirectorMouse(msg, wParam, lParam);
	default:
		return false;
	}
}

void DrawMarkerPopups(const Rect& gameArea, float w, float h, std::vector<MarkerCardAction>& actions);
void ApplyMarkerActions(const std::vector<MarkerCardAction>& actions);

// The director's frame: the bar and the cards over the game.
void DirectorFrameBuild(float w, float h) {
	ImGuiIO& io = ImGui::GetIO();
	{
		const DirectorMode before = g.directorMouse.GetMode();
		g.directorMouse.Tick(NowMs()); // a press held long enough looks even without moving
		ApplyDirectorMode(before, "hold");
	}
	if (g.directorMouse.GetMode() != DirectorMode::Free) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);

	DemoBarModel demo;
	DemoBar_BuildModel(demo);
	if (g.cadence.Ready((uint64_t)NowMs(), 10, false)) Director_BuildCards(g.cards);
	const float spectatorBar = Director_SpectatorBarHeight(h);
	ImGui::NewFrame();
	const DirectorFrame frame = DrawDirector(Director_State(), demo, g.cards, g.fonts, w, h, spectatorBar, spectatorBar);
	std::vector<MarkerCardAction> markerActions;
	if (!Director_State().blackout) DrawMarkerPopups(MakeRect(0, 0, w, h), w, h, markerActions);
	ImGui::Render();
	g.bar = frame.bar;
	++g.frames;
	ApplyMarkerActions(markerActions);
	if (DemoBar_Apply(frame.demoActions)) {
		Open("cam editor button");
		return;
	}
	std::shared_ptr<DrawSnapshot> snapshot = TakeSnapshot(ImGui::GetDrawData());
	snapshot->letterbox = false;
	Publish(std::move(snapshot));
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

void FeedKey(WPARAM vk, bool down) {
	if (g.context) FeedImGuiKey(ImGui::GetIO(), (int)vk, down);
}

bool OnMouse(UINT msg, WPARAM wParam, LPARAM lParam) {
	const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
	ImGuiIO* io = g.context ? &ImGui::GetIO() : nullptr;
	const int64_t now = NowMs();
	const Mode before = g.mouse.GetMode();
	Route route = Route::Game;
	const char* reason = "input";
	switch (msg) {
	case WM_MOUSEMOVE:
		route = g.mouse.Move((float)x, (float)y, now);
		if (route == Route::Ui && io) io->AddMousePosEvent((float)x, (float)y);
		reason = "right drag";
		break;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONDBLCLK: {
		const int button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) ? WorkspaceMouse::Left : WorkspaceMouse::Right;
		if (button == WorkspaceMouse::Left && before != Mode::Ui && CameraPath_Repositioning()) {
			// Repositioning while the game has the mouse: the click drops the camera where the view is.
			MainThread_Run(&CameraPath_PlaceReposition);
			return true;
		}
		route = g.mouse.ButtonDown(button, (float)x, (float)y, now, g.preview);
		if (route == Route::Ui && io) {
			io->AddMousePosEvent((float)x, (float)y);
			io->AddMouseButtonEvent(button, true);
		}
		reason = button == WorkspaceMouse::Left ? "click in preview" : "right press";
		break;
	}
	case WM_LBUTTONUP:
	case WM_RBUTTONUP: {
		const int button = msg == WM_LBUTTONUP ? WorkspaceMouse::Left : WorkspaceMouse::Right;
		route = g.mouse.ButtonUp(button, now);
		if (route == Route::Ui && io) io->AddMouseButtonEvent(button, false);
		reason = "look released";
		break;
	}
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
		route = before == Mode::Ui ? Route::Ui : Route::Game;
		if (route == Route::Ui && io) io->AddMouseButtonEvent(WorkspaceMouse::Middle, msg == WM_MBUTTONDOWN);
		break;
	case WM_MOUSEWHEEL: {
		POINT p = { x, y };
		if (g.window) ScreenToClient(g.window, &p); // wheel coordinates are screen coordinates
		route = g.mouse.Wheel((float)p.x, (float)p.y, g.preview);
		if (route == Route::Ui && io)
			io->AddMouseWheelEvent(0.0f, (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA);
		if (route == Route::Game) {
			// Over the game, the wheel is the director's here too, as CS2's MovieMode keeps it in the
			// editor: Ctrl is FOV, Shift the free cam's speed, a plain notch the camera.
			g.wheelRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
			const int notches = g.wheelRemainder / WHEEL_DELTA;
			g.wheelRemainder -= notches * WHEEL_DELTA;
			const WORD keys = GET_KEYSTATE_WPARAM(wParam);
			const bool ctrl = (keys & MK_CONTROL) || GetKeyState(VK_CONTROL) < 0, shift = (keys & MK_SHIFT) || GetKeyState(VK_SHIFT) < 0;
			if (notches) MainThread_Run([notches, ctrl, shift] { Director_Wheel(notches, ctrl, shift); });
			route = Route::Swallow;
		}
		break;
	}
	default:
		return false;
	}
	ApplyMode(before, reason);
	return route != Route::Game;
}

// The CAMERA MARKER card's model, read from the camera path.
MarkerCardModel BuildMarkerModel() {
	MarkerCardModel m;
	const PathState s = CameraPath_State();
	const Filmmaker::CamMarkers& markers = CameraPath_Markers();
	m.selected = s.selected;
	m.count = s.count;
	m.speedMode = s.settings.speedMode;
	m.interp = s.settings.interp;
	m.timing = s.settings.timing;
	m.autoSnap = s.settings.autoSnap;
	m.constSpeed = s.settings.constSpeed;
	if (markers.ValidIndex(s.selected)) {
		m.speedMul = markers.At(s.selected).speedMul;
		m.isLast = s.selected == markers.Count() - 1;
	}
	return m;
}

void ApplyMarkerActions(const std::vector<MarkerCardAction>& actions) {
	using Kind = MarkerCardAction::Kind;
	for (const MarkerCardAction& a : actions) {
		const PathState s = CameraPath_State();
		if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("path.card", "kind=%d value=%.2f", (int)a.kind, a.value);
		switch (a.kind) {
		case Kind::Previous: CameraPath_SelectStep(-1); break;
		case Kind::Next: CameraPath_SelectStep(1); break;
		case Kind::CycleSpeedMode: CameraPath_SetSpeedMode((s.settings.speedMode + 1) % 3); break;
		case Kind::CycleInterp: CameraPath_SetInterp(s.settings.interp == 1 ? 0 : 1); break;
		case Kind::ToggleTiming: CameraPath_SetTiming(s.settings.timing == 1 ? 0 : 1); break;
		case Kind::ToggleAutoSnap: CameraPath_SetAutoSnap(!s.settings.autoSnap); break;
		case Kind::ConstSpeed: CameraPath_SetConstSpeed(a.value); break;
		case Kind::SpeedMul: CameraPath_SetSpeedMul(s.selected, a.value); break;
		case Kind::Reposition:
			CameraPath_BeginReposition();
			if (g_open.load() && CameraPath_Repositioning() && g.mouse.GetMode() == Mode::Ui) { // the free cam is the user's to fly
				const Mode before = g.mouse.GetMode();
				g.mouse.ToggleGame();
				ApplyMode(before, "reposition");
			}
			break;
		case Kind::Remove: CameraPath_Delete(s.selected); break;
		case Kind::RemoveAll: CameraPath_DeleteAll(); break;
		case Kind::Close: CameraPath_CloseMenu(); break;
		default: break;
		}
	}
}

// Over the game (gameArea, client px): the CAMERA MARKER card while it is up, else CS2's prompt
// banner for a notice or a camera being repositioned. Inside an ImGui frame.
void DrawMarkerPopups(const Rect& gameArea, float w, float h, std::vector<MarkerCardAction>& actions) {
	const PathState s = CameraPath_State();
	g_markerMenu.store(s.menuOpen);
	if (s.menuOpen) {
		DrawMarkerCard(g.markerCard, BuildMarkerModel(), g.fonts, w, h, actions);
		return;
	}
	g.markerCard.confirmRemoveAll = false;
	char title[64];
	PathBanner banner;
	if (*s.notice) {
		banner.title = "CAMERA PATH";
		banner.line = s.notice;
	} else if (s.mode == PathMode::Reposition) {
		std::snprintf(title, sizeof(title), "REPOSITIONING CAMERA #%d", s.selected + 1);
		banner.title = title;
		banner.line = "Move to the new position, then left-click to place.";
		banner.keys = "LEFT CLICK / F  place        ESC  cancel";
	} else {
		return;
	}
	DrawPathBanner(ImGui::GetForegroundDrawList(), banner, g.fonts, gameArea);
}

// The PATH CAMERA tab's model, read from the camera path.
PathInspectorModel BuildPathModel() {
	PathInspectorModel m;
	const Filmmaker::CamMarkers& markers = CameraPath_Markers();
	const PathState s = CameraPath_State();
	m.keys.resize((size_t)markers.Count());
	for (int i = 0; i < markers.Count(); ++i) {
		const Filmmaker::CamMarker& marker = markers.At(i);
		PathInspectorKey& k = m.keys[(size_t)i];
		k.tick = marker.tick;
		k.fov = marker.fov;
		k.roll = marker.roll;
		k.ease = (int)marker.ease;
		k.speedMul = marker.speedMul;
	}
	m.selected = s.selected;
	m.interp = s.settings.interp;
	m.speedMode = s.settings.speedMode;
	m.timing = s.settings.timing;
	m.constSpeed = s.settings.constSpeed;
	m.tick = Demo_State().tick;
	m.repositioning = s.mode == PathMode::Reposition;
	return m;
}

// What the PATH CAMERA tab asked for, through the camera path as CS2's tab goes through its own.
void ApplyPathActions(const std::vector<PathInspectorAction>& actions) {
	using Kind = PathInspectorAction::Kind;
	for (const PathInspectorAction& a : actions) {
		if (Filmmaker::MvmDebugLog_Active() && a.kind != Kind::EditValue)
			Filmmaker::MvmDebugLog_Linef("path.inspector", "kind=%d index=%d channel=%d tick=%d value=%.3f", (int)a.kind, a.index,
				a.channel, a.tick, a.value);
		switch (a.kind) {
		case Kind::Select: CameraPath_SelectForEditor(a.index); break;
		case Kind::SelectDelta: CameraPath_SelectDelta(a.index); break;
		case Kind::Add: CameraPath_Place(); break;
		case Kind::ClearAll: CameraPath_DeleteAll(); break;
		case Kind::Reposition:
			CameraPath_BeginReposition();
			if (CameraPath_Repositioning() && g.mouse.GetMode() == Mode::Ui) { // the free cam is the user's to fly
				const Mode before = g.mouse.GetMode();
				g.mouse.ToggleGame();
				ApplyMode(before, "reposition");
			}
			break;
		case Kind::MoveToPlayhead: CameraPath_MoveKey(a.index, a.tick); break;
		case Kind::Delete: CameraPath_Delete(a.index); break;
		case Kind::EditBegin: CameraPath_EditBegin(); break;
		case Kind::EditValue: CameraPath_EditValue(a.index, a.channel, a.value); break;
		case Kind::EditEnd: CameraPath_EditEnd(); break;
		case Kind::Interp: CameraPath_SetInterp(a.index); break;
		case Kind::Ease: CameraPath_SetEase(a.index, (Filmmaker::Ease)(int)a.value); break;
		case Kind::SpeedMode: CameraPath_SetSpeedMode(a.index); break;
		case Kind::SpeedMul: CameraPath_SetSpeedMul(a.index, (float)a.value); break;
		case Kind::ConstSpeed: CameraPath_SetConstSpeed((float)a.value); break;
		case Kind::Timing: CameraPath_SetTiming(a.index); break;
		default: break;
		}
	}
}

} // namespace

bool Ui_OnWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (!g.window) g.window = hwnd;
	if (!g_open.load()) return g_director.load() && OnDirectorMessage(msg, wParam, lParam);

	switch (msg) {
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (!(g.context && ImGui::GetIO().WantTextInput)) {
			if (CameraPath_OnKey((int)wParam, true, (lParam & (1 << 30)) != 0)) return true;
			if (EditorDirectorKey(msg, wParam, lParam)) return true;
		}
		switch (WorkspaceKeyFor((int)wParam, g.context && ImGui::GetIO().WantTextInput)) {
		case WorkspaceKey::Close:
			Close("esc");
			return true;
		case WorkspaceKey::ToggleGame:
			if (!(lParam & (1 << 30))) { // not auto-repeat
				const Mode before = g.mouse.GetMode();
				g.mouse.ToggleGame();
				ApplyMode(before, "G");
			}
			return true;
		default:
			break;
		}
		FeedKey(wParam, true);
		return g.context && ImGui::GetIO().WantCaptureKeyboard;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		if (!(g.context && ImGui::GetIO().WantTextInput)) {
			if (CameraPath_OnKey((int)wParam, false, false)) return true;
			if (EditorDirectorKey(msg, wParam, lParam)) return true;
		}
		FeedKey(wParam, false);
		if (WorkspaceKeyFor((int)wParam, g.context && ImGui::GetIO().WantTextInput) != WorkspaceKey::None) return true;
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
	case WM_KILLFOCUS: {
		const Mode before = g.mouse.GetMode();
		g.mouse.FocusLost();
		ApplyMode(before, "focus lost");
		return false;
	}
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
		const Mode before = g.mouse.GetMode();
		const bool consumed = OnMouse(msg, wParam, lParam);
		if (msg != WM_MOUSEMOVE && Filmmaker::MvmDebugLog_Active())
			Filmmaker::MvmDebugLog_Linef("ui.input", "msg=0x%x x=%d y=%d mode=%s->%s consumed=%d panelHovered=%d",
				msg, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), ModeName(before), ModeName(g.mouse.GetMode()),
				consumed ? 1 : 0, g.panelHovered ? 1 : 0);
		return consumed;
	}
	default:
		return false;
	}
}

void Ui_MainThreadFrame() {
	Director_Frame();
	if (!g.window) return;
	const bool director = !g_open.load() && Director_Wanted();
	if (director != g_director.load()) {
		if (director) EnterDirector();
		else LeaveDirector();
	}
	if (!g_open.load() && !director) return;
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
	if (director) {
		DirectorFrameBuild(w, h);
		return;
	}
	{
		const Mode before = g.mouse.GetMode();
		g.mouse.Tick(NowMs()); // a right press held long enough looks even without moving
		ApplyMode(before, "right hold");
	}
	if (g.mouse.GetMode() != Mode::Ui) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); // nothing hovers while the game has the mouse

	// A camera placed by F (or cancelled) while the game had the mouse for it: the cursor comes back.
	const bool repositioning = CameraPath_Repositioning();
	if (g.wasRepositioning && !repositioning && g.mouse.GetMode() == Mode::Game) {
		const Mode before = g.mouse.GetMode();
		g.mouse.ToggleGame();
		ApplyMode(before, "reposition done");
	}
	g.wasRepositioning = repositioning;

	DemoBarModel demo;
	DemoBar_BuildModel(demo);
	const PathInspectorModel path = BuildPathModel();
	ImGui::NewFrame();
	const CameraEditorFrame frame = DrawCameraEditor(g.editor, g.fonts, w, h, demo, path);
	std::vector<MarkerCardAction> markerActions;
	DrawMarkerPopups(frame.previewClient, w, h, markerActions);
	ImGui::Render();
	DemoBar_Apply(frame.demoActions); // CAM EDITOR is not on the bar inside the editor
	ApplyPathActions(frame.pathActions);
	ApplyMarkerActions(markerActions);

	g.preview = frame.modal || g_markerMenu.load() ? Rect() : frame.previewClient;
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
	std::shared_ptr<DrawSnapshot> snapshot, world;
	{
		std::lock_guard<std::mutex> lock(g_frameMutex);
		snapshot = g_latest;
		world = g_latestWorld;
		if ((snapshot || world) && g_renderFont.version != g_fontPixels.version) g_renderFont = g_fontPixels;
	}
	if (snapshot || world) g_renderer.Render(device, world.get(), snapshot.get(), g_renderFont);
	Capture_RenderThread(device); // a requested frame, the UI just drawn into it
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

void Ui_ViewRendered(const double origin[3], const double angles[3], double fov) {
	std::shared_ptr<DrawSnapshot> world;
	PathOverlayModel model;
	const ImVec2 size = g.context ? ImGui::GetIO().DisplaySize : ImVec2(0, 0);
	// Built from the first UI frame on (it sets the draw lists' shared data), while ours is on screen.
	if (g.frames > 0 && (g_open.load() || g_director.load()) && !Director_State().blackout && size.x > 0 && size.y > 0
		&& CameraPath_Overlay(model, size.x, size.y)) {
		PathOverlayView view;
		for (int i = 0; i < 3; ++i) {
			view.origin[i] = origin[i];
			view.angles[i] = angles[i];
		}
		view.fov = View_ScreenFov(fov, size.x, size.y);
		view.width = size.x;
		view.height = size.y;
		if (!g.worldList) g.worldList = std::make_unique<ImDrawList>(ImGui::GetDrawListSharedData());
		ImDrawList& list = *g.worldList;
		list._ResetForNewFrame();
		list.PushTextureID(ImGui::GetIO().Fonts->TexID);
		list.PushClipRect(ImVec2(0, 0), size);
		DrawPathOverlay(list, model, view, g.fonts.Get(FontRole::Display));
		ImDrawData data;
		data.Valid = true;
		data.DisplaySize = size;
		data.AddDrawList(&list);
		world = TakeSnapshot(&data);
		++g.worldFrames;
		g.worldVertices = (int)world->vertices.size();
		const int64_t now = NowMs();
		if (Filmmaker::MvmDebugLog_Active() && now - g.worldLogAt >= 1000) {
			g.worldLogAt = now;
			Filmmaker::MvmDebugLog_Linef("path.overlay", "markers=%d line=%d aimed=%d head=%d vertices=%d view=(%.1f %.1f %.1f) (%.1f %.1f %.1f) fov %.1f->%.1f %.0fx%.0f",
				(int)model.markers.size(), (int)model.trajectory.size() / 3, model.aimed, model.head ? 1 : 0, g.worldVertices,
				origin[0], origin[1], origin[2], angles[0], angles[1], angles[2], fov, view.fov, size.x, size.y);
		}
	} else {
		g.worldVertices = 0;
	}
	PublishWorld(std::move(world));
}

void Ui_PrintOverlayHealth() {
	advancedfx::Message("  path overlay: %u frames built, %d vertices last frame\n", g.worldFrames, g.worldVertices);
}

bool Ui_SuspendMirvInput() {
	if (g_open.load()) return g.mouse.GetMode() == Mode::Ui;
	return g_director.load() && g.directorMouse.GetMode() == DirectorMode::Free;
}

void Ui_DirectorState() {
	advancedfx::Message("director: %s, mouse %s, bar (%.0f %.0f %.0f %.0f), cards %s, blackout %s, frames %u\n",
		g_director.load() ? "on" : "off", DirectorModeName(g.directorMouse.GetMode()), g.bar.x0, g.bar.y0, g.bar.x1, g.bar.y1,
		Director_State().cards ? "on" : "off", Director_State().blackout ? "on" : "off", g.frames);
	advancedfx::Message("cards: %s\n", DirectorCardsText(g.cards).c_str());
	if (g_director.load() && g.bar.Valid())
		advancedfx::Message("demo bar: %s\n", DemoBarLayoutText(Director_State().bar.layout).c_str());
}

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
		g_open.load() ? "open" : "closed", ModeName(g.mouse.GetMode()), (int)g.editor.tab,
		g.editor.dock == DockTab::Timeline ? "timeline" : "regular",
		g.preview.x0, g.preview.y0, g.preview.x1, g.preview.y1, g.panelHovered ? 1 : 0,
		g.fonts.ok ? "ok" : (g.fonts.scale > 0 ? "FALLBACK" : "not built"), g.fonts.scale, g.frames);
	if (g_open.load() && g.editor.dock == DockTab::RegularTimeline)
		advancedfx::Message("demo bar: %s\n", DemoBarLayoutText(g.editor.demoBar.layout).c_str());
}

} // namespace SourceFilmmaker
