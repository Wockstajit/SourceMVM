#pragma once
// Where scene object rigs put each rigged object this frame (scene-object-rigs.md). Published by
// SceneRig on the main thread (FrameStageNotify, then again at view setup with the interpolated
// pose the models are drawn at) and read by the Environment when it captures a frame on the
// engine thread. Immutable snapshots swapped atomically, like WorldTransient: the reader never
// touches entities or the rig's state. Objects absent from the map are not rigged this frame.
#include <atomic>
#include <map>
#include <memory>

namespace Filmmaker::RigTransient {
struct Entry {
    bool pose = true;          // the rig places it (attached or keyed); false: only its light is keyed
    double pos[3] = {0, 0, 0};
    double ang[3] = {0, 0, 0}; // pitch, yaw, roll
    double fov = 90;
    bool visible = true;       // false: its parent is missing and the policy hides it
    // A light's keyed settings this frame (Rig::LightChannel order: brightness, range, beam angle,
    // emitter size, width, height); bit i of lightMask says light[i] is keyed.
    double light[6] = {};
    unsigned lightMask = 0;
};
struct State {
    std::map<int, Entry> objects;
};
inline std::shared_ptr<const State> published = std::make_shared<const State>();
inline std::shared_ptr<const State> Read() { return std::atomic_load(&published); }
inline void Publish(State state) { std::atomic_store(&published, std::make_shared<const State>(std::move(state))); }
} // namespace Filmmaker::RigTransient
