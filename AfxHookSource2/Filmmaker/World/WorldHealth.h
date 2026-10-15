#pragma once

// `mirv_filmmaker world health` -- the single triage command for every World/Effects feature
// after a CS2 update. See Docs/effects/update-resilience.md.
//
// Prints, for every schema field these features depend on: the class and field name AS QUERIED,
// the offset the live game resolved it to (or MISS), then a per-feature LIVE/DEAD verdict naming
// the exact field that killed anything dead, then live entity counts for the classes involved.
//
// The same report goes to the mvm_debug log (`world.health` lines), because a console reply
// cannot be read after the fact and this is exactly the information wanted post-mortem.

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

void WorldHealth_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// Counts live client entities whose class name matches. Shared with the feature modules so the
// health report and the features agree about what exists.
int WorldHealth_CountEntities(const char* clientClassName);

} // namespace Filmmaker
