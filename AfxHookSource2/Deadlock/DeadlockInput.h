#pragma once
namespace Deadlock {
void Input_RunFrame();
void Input_Detach();
void Input_Report();
bool Input_OwnsCamera();

// Does Deadlock's window have the foreground? Anything that reads the keyboard with
// GetAsyncKeyState must ask this first: that call is global, so a key pressed in another
// application is indistinguishable from one pressed in the game.
bool Input_Focused();
void Input_SetWorkspaceOpen(bool open);
void Input_SetUiMouse(bool enabled);
bool Input_UiMouse();

// Is the OS cursor on screen? The game's real mode, and the thing our uiMouse flag must follow.
bool Input_CursorShowing();

// True windowed, as opposed to borderless or exclusive fullscreen.
bool Input_IsWindowedMode();
}
