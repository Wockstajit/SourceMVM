#pragma once

// UiLayout: which layout the filmmaker UI uses, and where its windows are in MODULAR layout.
//
//   WORKSPACE  (default) CONFIG and the Camera Editor each frame the game as a scaled preview with
//              their column docked on the right. One of them is open at a time.
//   MODULAR    the game stays full screen. A header bar along the top opens CONFIG, the Camera
//              Editor, the TIMELINE dock and Render; CONFIG and the Camera Editor are movable
//              windows and may be open together (Docs/panorama/modular-layout.md).
//
// The choice is a UI key in the settings store (`layout`), so it survives restarts and the panels
// read it from their `ui` object like every other UI key. Switch it with
// `mirv_filmmaker layout [workspace|modular|toggle]`.
//
// WINDOW RECTS. Each window's script publishes its on-screen rect every frame (client px, the
// space CameraBridge_GetUiCursor reports), and its C++ pump hands it here on the main thread. The
// input thread (MovieMode: which panel a click or a wheel notch belongs to) reads them back, so
// every access takes the lock.

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

enum class UiPanel { None = 0, Header, Config, Editor };

// The stored choice. UiLayout_Modular is safe from any thread; the other two run on the main
// thread, and MainThreadFrame (once a frame, before the panels pump) picks up a change written
// straight to the settings store (`mirv_filmmaker settings ui layout modular`, a settings load).
bool UiLayout_Modular();
void UiLayout_SetModular(bool modular);
void UiLayout_MainThreadFrame();

// A rect in client px, or w <= 0 for "not on screen". Main thread.
void UiLayout_SetPanelRect(UiPanel panel, int x, int y, int w, int h);
// Parses "x y w h" as the scripts publish it; an empty or malformed string clears the rect.
void UiLayout_SetPanelRectString(UiPanel panel, const char* xywh);
// The topmost panel at (x, y): the header first, then whichever window was pressed last. Any
// thread. None when nothing is there (or x / y is negative).
UiPanel UiLayout_PanelAt(int x, int y);
// Which window draws on top when the two overlap. Main thread (the scripts report it).
void UiLayout_SetFrontWindow(UiPanel panel);

// `mirv_filmmaker layout [workspace|modular|toggle]`.
void UiLayout_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

}  // namespace Filmmaker
