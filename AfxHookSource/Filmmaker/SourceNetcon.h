#pragma once

// The Source 1 hook's command port. CS:S 64-bit's engine.dll has no -netconport, so the hook serves
// the same protocol itself (Shared/NetconServer) when the game is started with
// `-mvmnetconport <port>`. Without that flag nothing listens.
//
// Console output reaches clients through SourceConsole's spew chain. Commands run on the main
// thread: the listener posts a registered message to the game window, and the hook's window
// procedure drains the queue into the engine's command buffer.

#include <windows.h>

namespace SourceFilmmaker {

// Starts the port if -mvmnetconport was given. Call once the engine interfaces exist.
void Netcon_Init();

// From the hook's window procedure, for every message. Returns true when the message was the
// netcon wake-up and has been handled.
bool Netcon_OnWindowMessage(HWND hwnd, UINT msg);
// Stops the port and joins its threads, as the client shuts down.
void Netcon_Shutdown();

// Main-thread drains of the command queue so far.
unsigned long long Netcon_PumpCount();

} // namespace SourceFilmmaker
