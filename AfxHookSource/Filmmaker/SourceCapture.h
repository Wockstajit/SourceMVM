#pragma once

// The game window's frame, saved by the hook: the back buffer at Present, after the filmmaker's UI is
// drawn, written as a 24-bit BMP. Window captures go through the desktop compositor (PrintWindow,
// screen copies), which on 2026-10-26 stalled for a minute or more and then hung outright; this
// reads the frame the game is about to show instead, overlay included.

struct IDirect3DDevice9;

namespace SourceFilmmaker {

// Main thread: asks for the next presented frame to be written to path (mirv_filmmaker capture).
void Capture_Request(const char* path);

// Present's thread, after the UI is drawn: writes a requested frame.
void Capture_RenderThread(IDirect3DDevice9* device);

} // namespace SourceFilmmaker
