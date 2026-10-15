#pragma once

// Scene objects keep two undo histories: CS2 assets (Props.cpp) and the Environment scene with its
// lights and meshes (EnvironmentScene.cpp). Each step is stamped from this one clock, so Ctrl+Z in
// Config takes back whichever change was made last, across both.

#include <atomic>
#include <cstdint>

namespace Filmmaker {

inline std::atomic<uint64_t> g_sceneUndoClock{0};
inline uint64_t NextSceneUndoStamp() { return ++g_sceneUndoClock; }

} // namespace Filmmaker
