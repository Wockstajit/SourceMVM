#pragma once

// The Source 1 hook's UI host: owns the ImGui context, builds the Camera Editor workspace on the main
// thread, hands each frame to the render thread, and routes the mouse the way CS2's workspaces do
// (UiClickMath.h):
//
//   * UI mode (default while a workspace is open): the cursor is free and visible, panels take
//     clicks, and the game sees no mouse movement or buttons.
//   * A left click in the preview hands the mouse to the game (GAME mode); G hands it back.
//   * A right press in the preview that is held or dragged (RightPressBecomesLook) is a look: the
//     game has the mouse until the button is released, then the cursor returns where it was.
//   * ESC closes the workspace.
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

// Draws the latest UI frame (and the preview letterbox) into the back buffer. Present's thread.
void Ui_RenderThreadPresent(IDirect3DDevice9* device);
void Ui_DeviceLost();

// Cursor and input filters. Each returns true when it decided the call's outcome.
bool Ui_OnSetCursorPos(int x, int y);
bool Ui_OnGetCursorPos(LPPOINT point);
bool Ui_OnSetCursor(HCURSOR cursor, HCURSOR& result);
// Zeroes the mouse part of a raw input packet while the UI owns the mouse.
void Ui_FilterRawInput(UINT result, UINT command, LPVOID data);

// mirv_filmmaker editor [on|off|toggle|state]
void Ui_EditorCommand(int argc, advancedfx::ICommandArgs* args);
bool Ui_EditorOpen();

// mirv_filmmaker director state: the director's mouse, bar, cards and where each bar control is.
void Ui_DirectorState();

} // namespace SourceFilmmaker
