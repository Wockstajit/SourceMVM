#pragma once

// The small line at the top of the screen that says a take is recording or playing, so the state
// is visible with the Config panel closed. Display only.

namespace Filmmaker::Takes {

// Main thread, once per frame.
void RunHudFrame();

} // namespace Filmmaker::Takes
