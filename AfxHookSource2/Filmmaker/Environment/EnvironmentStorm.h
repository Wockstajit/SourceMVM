#pragma once
#include "EnvironmentScene.h"
#include "EnvironmentStormMath.h"
namespace Filmmaker::Environment {
// Only the render snapshot receives the ephemeral emitter. Presets, Undo and editor objects
// retain their authored identities. Never evict an authored light or its shadow allocation.
inline bool AddStormLight(Frame &frame, const StormSample &storm) {
    if (frame.scene.climate[CL_storm] < .5f || frame.scene.climate[CL_enabled] < .5f || storm.flash <= 0) return false;
    int lights = 0, shadows = 0;
    for (const auto &o : frame.scene.objects) if (o.enabled && o.IsLight()) {
        ++lights; if (o.shadows) ++shadows;
    }
    if (lights >= 16 || shadows >= 4 || !frame.occlusion) return false;
    Object light; light.id = -1; light.kind = "point"; light.shadows = true;
    light.position = storm.source; light.range = storm.range;
    light.color = {.64f, .77f, 1}; light.intensity = storm.flash * 5;
    // First emitter also scatters into fog; capacity was checked before insertion.
    frame.scene.objects.insert(frame.scene.objects.begin(), light);
    return true;
}
} // namespace Filmmaker::Environment
