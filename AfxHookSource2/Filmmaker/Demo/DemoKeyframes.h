#pragma once

// Keyframed demo copies: fast BACKWARD seeks, so the timeline scrubs backward in real time.
//
// CS2 seeks backward by restoring the newest DEM_FullPacket at or before the target and replaying
// from there (CDemoPlayer::SkipToTick / FullPacketGoto in engine2), and Valve writes one full
// packet every 3840 ticks -- so going back replays up to a minute of demo, 0.5 to several seconds.
// FilmmakerDemoKeyframes.exe (FilmmakerDemoKeyframesGo/) writes a copy with a synthesized full
// packet every 32 ticks; the engine's own seek then restores in one ~50-80 ms frame.
//
//   * Playback is never held for it: the first play of a demo uses the normal file and the copy
//     is built in the background; every later play uses the copy.
//   * The copy lives in the demo's source_mvm project folder as keyframed.dem, marked with the
//     original's and the helper's size:mtime (keyframed.dem.src), and is rebuilt when either
//     changes. It is built from the repaired copy when the demo needed repair.
//   * The original .dem is never modified; sidecars stay keyed to it (DemoProjectPath.cpp).
//
// Contract: Docs/features/filmmaker_demo_scrubbing.md.

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

enum class KeyframeState {
	Idle,
	Building,
	Ready,
	Failed
};

// The keyframed copy of `demoPath` when it exists and is valid for the current demo file and
// helper; empty otherwise.
std::wstring DemoKeyframes_ReadyPath(const std::wstring& demoPath);

// Non-blocking. Starts building the keyframed copy of `demoPath` from `sourcePath` (the demo
// itself, or its repaired copy) unless one is valid, one is being built, or the helper is missing.
void DemoKeyframes_Build(const std::wstring& demoPath, const std::wstring& sourcePath);

KeyframeState DemoKeyframes_State();
std::string DemoKeyframes_StatusText();
bool DemoKeyframes_HelperAvailable();

// Waits for an in-flight build (shutdown).
void DemoKeyframes_Reset();

// Console: `mirv_filmmaker keyframes [status]`.
void DemoKeyframes_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);


} // namespace Filmmaker
