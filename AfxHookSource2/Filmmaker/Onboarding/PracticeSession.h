#pragma once

namespace Filmmaker {

// The practice session behind Clippy's tutorial: the tutorial starts from a clean slate and leaves
// nothing behind.
//
// Begin writes what the user had (pending settings, scene-key and environment edits), then puts
// everything on its defaults in memory: no camera keys, shakes, loop or scene keys in any demo, the
// CAM EDITOR layout and UI state, CONFIG's camera feel, effects, ragdolls, environment, props and
// light choices, and the render workspace. From then on no store writes to disk
// (Config/PracticeSessionFlag.h), so the user's files are exactly as they were.
//
// End reads everything back from disk (the current demo's keys, the settings and effects files,
// the map's environment and props) and puts the render workspace back as the user left it. The
// render output folder is kept throughout: renders land where the user's renders always land.
//
// Main thread. Each is a no-op when the session is already in that state.
void PracticeSession_Begin();
void PracticeSession_End();

} // namespace Filmmaker
