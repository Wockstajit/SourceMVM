#pragma once
// The experimental relighting engine: owns the authored light set for the current map,
// the live edits made to it, and the `mirv_relight` console surface.
//
// Separate from the Environment feature on purpose. Environment layers weather, coatings
// and a few user-placed studio lights over a map's baked lighting; this replaces that
// baked lighting with the map's own authored lights and drives CS2's forward material
// shaders directly. The rendering itself still runs through the shared native-material
// interception, because that is where the draw hook lives.
#include "RelightMath.h"
#include <string>

namespace Filmmaker::Relight {

// The set the render thread should use this frame. Empty and disabled until loaded.
Scene CaptureScene(const std::string &map);
// Main thread only; preparation publishes a checksum-verified entity export.
bool UpdateMapLighting(const std::string &map,const std::string &metadataPath);
// Map changes drop the enable but keep the loaded lights, so re-enabling on the same map
// does not need another load.
void OnLevelChanged();
// One line for the always-on heartbeat: whether relighting and its GPU passes are on. Main thread.
std::string HeartbeatText();

} // namespace Filmmaker::Relight
