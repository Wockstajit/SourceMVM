#pragma once

// The kill feed (CS2's death notices, HudDeathNotice) for movies: players can be shown under other
// names, and the feed can be cut down to the kills the watched player is in. `mirv_filmmaker
// killfeed ...`; the Render workspace's Passes tab has the controls.
//
// CS2 draws the feed. The names and the filter are applied in Panorama on the notices it built
// (CameraTimelineJs.h, killFeedApply), from the `killFeed` block this module publishes in the
// timeline state, so they show live, in every HUD mode, and in renders. A render with No HUD keeps
// the feed when the Render workspace's Kill feed switch is on (RenderPasses::killFeed).
//
// Main thread.

#include <sstream>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

void KillFeed_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);
// Appends `,"killFeed":{...}`: the players, their shown names, the filter and the watched player.
void KillFeed_AppendStateJson(std::ostringstream& o);
// A new demo: the names belong to the old one's players.
void KillFeed_Reset();

} // namespace Filmmaker
