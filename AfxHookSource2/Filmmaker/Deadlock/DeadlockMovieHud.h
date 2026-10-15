#pragma once

// Director status: a small always-on HUD card showing what the camera is doing during a replay --
// CS2's MovieHud (AfxHookSource2/Filmmaker/Panorama/MovieHud.cpp) ported to read Deadlock's own
// runtime state (DeadlockCameraRuntime's owner/pose, DeadlockPlaybackSpeed) instead of Filmmaker's
// MovieMode/CameraBridge/ThirdPersonCamera, which Deadlock has no equivalent of.
//
// UNLIKE CS2's MovieHud, this owns no separate PanoramaBridge or panel. Deadlock's whole workspace
// (DeadlockWorkspaces.cpp) already builds and pumps ONE script against the game's HUD context every
// frame, whether or not the editor is open -- DeadlockNativeHudJs.h's own driver runs the same way,
// for the same reason: a second bridge instance would recompile its own script against the same
// context for no gain. So the director card is one more fragment of that script
// (DeadlockMovieHudJs.h), built as a sibling of the workspace root rather than a child of it, and
// this file only supplies the state fragment DeadlockWorkspaces.cpp folds into its own per-frame
// JSON.

#include <string>

namespace Filmmaker {

// Appends `,"director":{...}}` -- meant for the middle of an object, same convention as
// AppendLayoutJson and friends in DeadlockWorkspaces.cpp.
void MovieHud_AppendStateJson(std::string& out);

} // namespace Filmmaker
