#pragma once

// Work for the game's main thread. The window procedure usually runs there, but CS:S also
// dispatches window messages from another thread while the main thread is busy (a demo restart or
// seek; measured 2026-10-26: a K and a J press handled on a second thread). Input handlers decide on
// the spot whether a message is theirs, and hand what it does to MainThread_Run, as CS2's director
// queues its actions for FlushActions.

#include <functional>

namespace SourceFilmmaker {

// Main thread, first thing each frame (FRAME_RENDER_START): notes the thread and runs what was posted.
void MainThread_Frame();

// Runs fn now on the main thread, or posts it for the main thread's next frame.
void MainThread_Run(std::function<void()> fn);

} // namespace SourceFilmmaker
