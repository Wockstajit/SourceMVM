#pragma once

// A standalone, always-on-top readout that shows the window's pixel size and aspect ratio while
// the window is being resized.
//
// WHY THIS IS NOT A PANORAMA PANEL. Dragging a window border puts Windows into a MODAL resize
// loop inside DefWindowProc. The game's own frame loop is parked inside DispatchMessage for the
// entire drag, so nothing renders, Panorama never runs, and any in-game HUD is frozen on the last
// frame drawn before the drag started. The one thing the user asked for -- a readout that changes
// as you drag -- is the exact thing an in-game panel cannot do. A separate layered Win32 window
// owned by the same thread still receives and paints its messages from that modal loop, so it
// updates live.
//
// It is deliberately its own module with no CS2, SDL, Panorama or Filmmaker dependency: it is
// handed a size and two strings and it draws them. Nothing else in the project may grow features
// into it.
//
// THREADING: every entry point must be called from the thread that owns the anchor window (the
// game main/message thread). Window creation, painting and destruction all happen there.

#include <string>

namespace Filmmaker {

// Shows (creating on first use) the readout centered on (centerX, centerY) in SCREEN coordinates
// and repaints it immediately -- synchronously, so it is correct even when called from inside a
// modal resize loop that is not returning to any pump. Repeat calls just update text and position.
//
// The centre is passed in rather than derived from the anchor window because during a drag the
// window has NOT moved or resized yet: WM_SIZING carries the rect Windows is *about* to apply, and
// dragging a left or top edge moves the window origin as well as its size. Centering on the
// anchor's live rect would leave the card a step behind the pointer and visibly off-centre.
// `anchorHwnd` is still needed, for DPI.
void WindowSizeOverlay_Show(void* anchorHwnd, int centerX, int centerY,
	const std::string& primary, const std::string& secondary);

// Re-centres a readout that is ALREADY visible, keeping its text and its pending hide timer. Does
// nothing when the card is hidden, so a window that is only being moved never makes one appear.
//
// This is what keeps the numbers in the middle of the window while the window is dragged by its
// title bar: without it the card stays where the resize left it and the window slides out from
// under it.
void WindowSizeOverlay_MoveTo(int centerX, int centerY);

// Hides the readout after `holdMs`. The timer runs on the overlay window itself, so the fade-out
// still happens while the game's frame loop is parked in a modal loop.
void WindowSizeOverlay_HideAfter(unsigned holdMs);

// Main-thread fallback when the host filters native timer messages.
void WindowSizeOverlay_Pump();

// Immediate hide.
void WindowSizeOverlay_Hide();

bool WindowSizeOverlay_Visible();

// Destroys the window and unregisters the class. Called from Filmmaker shutdown.
void WindowSizeOverlay_Shutdown();

} // namespace Filmmaker
