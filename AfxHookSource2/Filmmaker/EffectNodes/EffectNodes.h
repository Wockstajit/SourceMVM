#pragma once

// Effect Nodes controller: manages the node graph, starter presets, graph edits, compilation,
// JSON state for the NODES tab + interactive graph editor, and console commands.

#include "EffectNodesMath.h"

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker::EffectNodes {

bool Active();
bool NeedsWorldDepthRef();
CompiledPlan SnapshotPlan();

void LoadPreset(const std::string& presetName, bool enable = true);
void SetEnabled(bool on);
void Reset();

void AppendStateJson(std::ostringstream& out);
void RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker::EffectNodes
