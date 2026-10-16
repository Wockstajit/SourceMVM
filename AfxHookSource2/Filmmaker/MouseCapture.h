#pragma once

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Keeps the desktop cursor free while CS2 has focus until you actually click into the game.
//
// This is the DEFAULT behaviour of the hook, not an opt-in: focus alone -- launching, alt-tabbing
// back, or switching between first / third / free cam -- never captures the cursor. The first
// unhandled game click restores normal CS2 mouse-look, and losing focus re-arms the free cursor.
// It used to be gated on a `-afxFreeMouse` launch parameter, which meant the same build grabbed
// the mouse or not depending on which shortcut or script started it.
//
// clickToCapture=false is the explicit opt-out (`-afxGrabMouse`, or `mirv_filmmaker mouse game`):
// CS2 behaves exactly as it does without the hook.
void MouseCapture_Initialize(bool clickToCapture);
bool MouseCapture_IsFree();
void MouseCapture_SetFree(bool freeMouse);
void MouseCapture_SetClickToCapture(bool clickToCapture);
// Entering a camera that is steered with the mouse (third person, free cam, or G back to flying in a
// workspace) is the same opt-in as clicking into the game: in the default click-to-capture mode it
// hands the mouse to the game right away. Explicit `mouse free` (automation) is left alone.
void MouseCapture_TakeForCamera(const char* reason);
// The mirror image: handing the cursor to a workspace UI (UI-mouse mode on) gives the mouse back,
// so CS2's own mouse-look and capture stand down under the panel instead of grabbing the pointer
// on the next right-click. Same default-mode-only rule as TakeForCamera.
void MouseCapture_ReleaseForUi(const char* reason);
void MouseCapture_OnActivate(bool active);
void MouseCapture_OnGameClick();
void MouseCapture_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
