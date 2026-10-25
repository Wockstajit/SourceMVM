#pragma once

// The takes already rendered from the playing demo, read back from the manifest.json each take
// wrote (RenderManifest.cpp), so the Render workspace can start a new job from one: re-render a
// shot with one change, or add a pass for exactly the same In / Out (RenderTakeImportMath.h).
//
// The output folder is scanned on a worker thread, never on the main thread: it can be a slow or
// network drive. A scan runs when the workspace opens, when a job ends, when the output folder
// changes, and on `render takes refresh`.

#include <sstream>
#include <string>

namespace Filmmaker {

void RenderTakeHistory_Refresh();
// Main thread, once per frame: picks up a finished scan.
void RenderTakeHistory_PumpMainThread();
// ,"takes":[...]: the playing demo's takes, newest first.
void RenderTakeHistory_AppendJson(std::ostringstream& o);
// Rebuilds take `index` (as published) as the current job. False with a reason when it cannot.
bool RenderTakeHistory_Import(int index, std::string& outSummary, std::string& outError);
// `render takes [refresh]`.
void RenderTakeHistory_PrintList(const char* cmd);

} // namespace Filmmaker
