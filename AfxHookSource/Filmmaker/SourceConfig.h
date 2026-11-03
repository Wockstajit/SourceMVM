#pragma once

// The Config workspace's host in CS:S (the inspector is Ui/ConfigInspector.h): the camera modules'
// state for its cards, read through the same AppendStateJson calls CS2's ConfigHud state JSON is
// built from, so a card says what CS2's says; the console commands its edits send; and
// `mirv_filmmaker effects reset camera`, which its footer sends.
//
// Main thread.

#include "Ui/ConfigInspector.h"

#include <string>
#include <vector>

namespace advancedfx { class ICommandArgs; }

namespace SourceFilmmaker {

void Config_BuildModel(Ui::ConfigModel& model);

void Config_RunCommands(const std::vector<std::string>& commands);

// mirv_filmmaker effects reset camera: every CAMERA card back to its defaults, as CS2's footer does.
void Config_EffectsCommand(int argc, advancedfx::ICommandArgs* args);

} // namespace SourceFilmmaker
