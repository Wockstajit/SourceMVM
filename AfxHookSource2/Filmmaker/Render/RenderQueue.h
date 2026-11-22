#pragma once

// The render queue: several shots lined up in the Render workspace and rendered one after another
// (`mirv_filmmaker render queue ...`, the JOB column's ADD TO QUEUE / RENDER QUEUE).
//
// An entry is a copy of every setting a job starts from at the moment it was added: the resolved
// In / Out, the camera source and path extension, the output and the passes, plus what the camera
// needs to come back the same way: the watched player for a POV shot (by SteamID, so it survives a
// reconnect of the slot order) and the camera path's keys for a Camera Path shot. Running the queue
// puts each entry's settings back through the workspace's own state, starts an ordinary job and
// waits for it to end before the next one. The user's own settings and path are put back when the
// queue ends, however it ends.
//
// Main thread only.

#include <sstream>

namespace Filmmaker {

void RenderQueue_RunCommand(int argc, const char* const* argv, const char* cmd);
void RenderQueue_RunFrame();
bool RenderQueue_Running();
// Appends `,"queue":{...}` to the Render workspace state.
void RenderQueue_AppendStateJson(std::ostringstream& o);

} // namespace Filmmaker
