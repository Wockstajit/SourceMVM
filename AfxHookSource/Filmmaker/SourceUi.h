#pragma once

// The Source 1 hook's UI host: owns the ImGui context, builds the Camera Editor workspace on the main
// thread, hands each frame to the render thread, and routes the mouse the way CS2's workspaces do
// (UiClickMath.h):
//
//   * UI mode (default while a workspace is open): the cursor is free and visible, panels take
//     clicks, and the game sees no mouse movement or buttons.
//   * A left press steers the preview until release. G explicitly latches GAME mode.
//   * A right press in the preview that is held or dragged (RightPressBecomesLook) is a look: the
//     game has the mouse until the button is released, then the cursor returns where it was.
//   * ESC closes the workspace.
//
// At the main menu a DEMOS button opens the demos browser (Ui/DemosBrowser.h), a full page that has
// the mouse and the keys until ESC or a demo plays.
//
// With no workspace open, while a demo plays, the movie director runs instead (SourceDirector.h):
// the demo bar along the bottom and the director cards, the cursor free, a click stepping the
// spectated player and a held button looking around (Ui/DirectorMouse.h), the wheel cycling the
// camera, Space, Left, Right and F8 (Ui/DirectorMath.h). It stands aside while the game's menu or
// console is up.
//
// Threading: everything here runs on the main thread except RenderThreadPresent and DeviceLost,
// which run on the thread that presents. The cursor and raw-input filters run where the input
// system calls them, which is the main thread.

#include <windows.h>

struct IDirect3DDevice9;

namespace advancedfx { class ICommandArgs; }

namespace SourceFilmmaker {

// From the hook's window procedure. Returns true when the message was consumed by the UI.
bool Ui_OnWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Builds this frame's UI. Main thread, once per frame (FRAME_RENDER_START).
void Ui_MainThreadFrame();

// The view the game just rendered, after every override: the camera path drawn in the world
// (Ui/PathOverlay.h) is projected with it, so it moves with the frame it is laid over. Main thread.
void Ui_ViewRendered(const double origin[3], const double angles[3], double fov);
void Ui_PrintOverlayHealth();

// Draws the latest UI frame (and the preview letterbox) into the back buffer, the camera path's
// world overlay under both. Present's thread.
void Ui_RenderThreadPresent(IDirect3DDevice9* device);
void Ui_DeviceLost();

// Cursor and input filters. Each returns true when it decided the call's outcome. Besides the UI's
// own rules they follow CS2's click-to-capture (MouseCapture.h): the game may not hide, move or
// capture the cursor until a click in its window reaches it, and losing focus frees it again.
bool Ui_OnSetCursorPos(int x, int y);
bool Ui_OnGetCursorPos(LPPOINT point);
bool Ui_OnSetCursor(HCURSOR cursor, HCURSOR& result);
bool Ui_OnSetCapture(HWND window, HWND& result);
// Zeroes the mouse part of a raw input packet while the game may not have the mouse.
void Ui_FilterRawInput(UINT result, UINT command, LPVOID data);
// Routes the cursor calls of the modules main.cpp's import hooks leave out (vguimatsurface, client,
// the engine's ShowCursor) through the filters above. Once, after the client loaded.
void Ui_InstallCursorHooks();
void Ui_PrintMouseHealth();

// Netcon input replay through the real UI handlers; does not activate a window or post OS input.
void Ui_InputCommand(int argc, advancedfx::ICommandArgs* args);

// mirv_filmmaker editor [on|off|toggle|state]
void Ui_EditorCommand(int argc, advancedfx::ICommandArgs* args);
// mirv_filmmaker config [on|off|toggle|state]: the Config workspace, which shares the Camera
// Editor's preview and dock; opening one closes the other, as in CS2.
void Ui_ConfigCommand(int argc, advancedfx::ICommandArgs* args);
// mirv_filmmaker demos [on|off|toggle|state|<library command>]: the demos browser, a full page the
// main menu's DEMOS button opens (SourceDemos.h has the library's commands).
void Ui_DemosCommand(int argc, advancedfx::ICommandArgs* args);
// The Camera Editor is the open workspace.
bool Ui_EditorOpen();
// A workspace is open: the Camera Editor or Config.
bool Ui_WorkspaceOpen();
// The open Camera Editor's mouse: the UI's (true) or the game's, to fly the free cam, as G trades
// it. The follow camera's reposition hands it to the game and back (CS2's CameraEditor_SetCursorMode).
// A right-button look in progress keeps it.
void Ui_SetEditorMouse(bool ui);
// Opens the Camera Editor on its TIMELINE dock, or puts the REGULAR TIMELINE dock back.
void Ui_ShowTimelineDock(bool show);

// mirv_filmmaker director state: the director's mouse, bar, cards and where each bar control is.
void Ui_DirectorState();

// HLAE's free camera (mirv_input) waits while the cursor is ours: a workspace's UI mode, or the
// director's free cursor. It flies while the game has the mouse (a held press, GAME mode), as CS2's
// free cam does, and its held keys are dropped while it waits.
bool Ui_SuspendMirvInput();

} // namespace SourceFilmmaker
