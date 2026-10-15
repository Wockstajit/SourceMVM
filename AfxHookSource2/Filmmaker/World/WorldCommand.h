#pragma once

// `mirv_filmmaker world <sub>` dispatcher for the World/Effects features. Owns nothing itself;
// it routes to each feature module's own command handler so the modules stay independent.

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

void World_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame. Fans out to every
// World/Effects module's per-frame work. Cheap no-op while nothing is enabled.
void World_RunFrame();

} // namespace Filmmaker
