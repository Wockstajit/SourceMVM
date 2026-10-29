#pragma once

// The takes already rendered from the playing demo, read back from the manifest.json each take
// wrote (RenderManifest.cpp), so the Render workspace can start a new job from one: re-render a
// shot with one change, or add a pass for exactly the same In / Out (RenderTakeImportMath.h).
//
// The output folder is scanned on a worker thread, never on the main thread: it can be a slow or
// network drive. A scan runs when the workspace opens, when a job ends, when the output folder
// changes, and on `render takes refresh`.

#include "RenderTakeImportMath.h" // RenderTakeSettings

#include <sstream>
#include <string>
#include <vector>

namespace Filmmaker {

void RenderTakeHistory_Refresh();
// Main thread, once per frame: picks up a finished scan.
void RenderTakeHistory_PumpMainThread();
// ,"takes":[...]: the playing demo's takes, newest first.
void RenderTakeHistory_AppendJson(std::ostringstream& o);
// The same takes for a native panel (CS:S), in the same order `render import <n>` counts.
std::vector<RenderTakeSettings> RenderTakeHistory_Listed();
// Rebuilds take `index` (as published) as the current job. False with a reason when it cannot.
bool RenderTakeHistory_Import(int index, std::string& outSummary, std::string& outError);
// `render takes [refresh]`.
void RenderTakeHistory_PrintList(const char* cmd);

} // namespace Filmmaker
