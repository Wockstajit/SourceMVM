#pragma once

// ALTERNATIVE ENVIRONMENT (CONFIG > ENVIRONMENT > Map): the open demo plays on another map -- a
// night or rain remake from the Workshop, another installed map, a neutral room -- with every
// player, weapon, grenade and effect from the demo on top. Docs/features/alternative-environment.md
// owns the contract.
//
// How: CS2 loads whatever world a demo names. Demo/DemoWorldSwap.h writes a copy of the demo that
// names the replacement map (and mounts its Workshop addon), and places that world with the inverse
// of the alignment as a spawn-group world offset, so the demo's own coordinates -- players, cameras,
// grenades, the hook's camera paths -- stay untouched. The engine loads the replacement map's assets
// during the demo's own level load, so nothing hitches during playback, and seeking and pausing are
// the engine's own.
//
// The alignment view draws both maps from above (AltEnvironmentMath.h floor rasters, served as PNGs
// over LocalImageServer) with the players' walked paths and live positions on the demo layer, and
// AUTO ALIGN searches offset and yaw. Settings live in the demo's project file ("environment").
//
// Threads: everything here runs on the main thread; map scans, pictures, the players' paths, AUTO
// ALIGN and copy builds run on their own workers and hand results back through futures.

#include "AltEnvironmentMath.h"

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker::AltEnvironment {

// Main thread, once a frame.
void RunMainThreadFrame();
// `mirv_filmmaker altenv ...` (RunCommand prints the verbs).
void RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);
// ,"altenv":{...} for the Config state.
void AppendStateJson(std::ostringstream& out);
// One line for `mirv_filmmaker health`.
std::string HealthLine();

// What the engine should load for `original` once its playable file (repaired or keyframed) is
// `playable`: `playable` itself when the demo plays on its own map, its environment copy when the
// project asks for another map and the copy is current, or "" while that copy is being written --
// the build then starts playback itself (Filmmaker::PlayResolvedDemo) when it finishes.
std::wstring ResolvePlayable(const std::wstring& original, const std::wstring& playable);
// The file an environment copy was written from, or "" when `file` is not one.
std::wstring SourceOf(const std::wstring& file);
// The environment copy carries a keyframed copy's dense full packets (Movie/DemoScrub.cpp).
bool IsKeyframedCopy(const std::wstring& file);
// The engine plays a copy whose world is placed at an offset or turned: map data read from the
// replacement map's files is not where that world is drawn (Environment/EnvironmentScene.cpp holds it).
bool WorldMoved();
// Keeps only `original`'s environment copies (the playing one and its newest) on disk.
void KeepOnly(const std::wstring& original, const std::wstring& playingFile);

void Shutdown();

} // namespace Filmmaker::AltEnvironment
