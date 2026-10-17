#pragma once

#include <sstream>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// PROPS: real CS2 models (trees, plants, props) placed into the scene as client-side entities,
// drawn, lit and shadowed by the engine itself -- they need no Environment pass. Placed props
// belong to the map and are saved per map; they respawn when that map loads and whenever a demo
// seek clears client entities.
void Props_RunMainThreadFrame();
void Props_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);
// `,"props":{...}` for the Config panel state: the catalog search page and the placed props.
void Props_AppendStateJson(std::ostringstream& out);
// Placing or dragging in the view: Esc cancels it (any thread).
bool Props_EditorBusy();
// Drops an unfinished placement and puts a dragged prop back. Main thread.
void Props_StopEditing();
// The client is shutting down cleanly: recent prop spawns did not crash it, so the crash-loop
// guard must not hold this map's props back on the next load.
void Props_OnShutdown();
// A practice session ended: read the current map's saved props again (and spawn them) on the next
// check. During the session props are neither read nor saved. Main thread.
void Props_ReloadAfterPractice();

} // namespace Filmmaker
