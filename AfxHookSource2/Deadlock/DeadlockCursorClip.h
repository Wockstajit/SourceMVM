#pragma once

// LET THE MOUSE LEAVE THE WINDOW while the editor's UI mode is on, in WINDOWED mode only.
//
// Deadlock confines the cursor to its window with ClipCursor, which is right for playing and wrong
// for editing: with the workspace open and the cursor mode on, the mouse cannot reach a second
// monitor, the console, or anything else, and the only way out is alt-tab.
//
// WINDOWED ONLY, and that is a deliberate limit rather than an unfinished one. On a borderless or
// exclusive-fullscreen display the window covers the monitor, so there is nowhere for the cursor to
// go and releasing the clip buys nothing; on a true window there is desktop on every side. The
// distinction is read from the window's own style (WS_CAPTION / WS_THICKFRAME), not from a setting
// we would then have to keep in step with the game's.
//
// HOW. ClipCursor is detoured rather than fought. Calling ClipCursor(nullptr) on a timer would race
// the game's own call every frame and produce a cursor that escapes and snaps back; intercepting
// the call means the confinement is simply never applied while we do not want it. The game's last
// requested rect is remembered, so the moment the condition stops holding the clip is restored
// exactly as the game asked for it rather than approximated.

#include <windows.h>

namespace Deadlock {

bool CursorClip_Install();
bool CursorClip_Installed();

// Called once per frame. Applies or releases the confinement according to the current mode.
void CursorClip_RunFrame();

void CursorClip_Report();

} // namespace Deadlock
