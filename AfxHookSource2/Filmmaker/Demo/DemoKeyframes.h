#pragma once

// Keyframed demo copies: fast BACKWARD seeks, so the timeline scrubs backward in real time.
//
// CS2 seeks backward by restoring the newest DEM_FullPacket at or before the target and replaying
// from there (CDemoPlayer::SkipToTick / FullPacketGoto in engine2), and Valve writes one full
// packet every 3840 ticks -- so going back replays up to a minute of demo, 0.5 to several seconds.
// FilmmakerDemoKeyframes.exe (FilmmakerDemoKeyframesGo/) writes a copy with a synthesized full
// packet every 32 ticks, and every 2 ticks in windows around the camera-path markers; the engine's
// own seek then restores in one ~50-80 ms frame and replays almost nothing.
//
//   * Playback is never held for it: the first play of a demo uses the normal file and the copy
//     is built in the background; every later play uses the copy.
//   * The copy lives in the demo's source_mvm project folder as keyframed.dem or keyframed-b.dem
//     (two slots, because the engine holds the playing one open while markers move and a denser
//     rebuild is written). Each slot's marker (<slot>.src) holds the original's and the helper's
//     size:mtime -- a slot is rebuilt when either changes -- and the dense windows it was built with.
//     It is built from the repaired copy when the demo needed repair.
//   * One copy: opening a demo deletes every other demo's copy.
//   * The original .dem is never modified; sidecars stay keyed to it (DemoProjectPath.cpp).
//
// Contract: Docs/features/filmmaker_demo_scrubbing.md.

#include <string>
#include <vector>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

enum class KeyframeState {
	Idle,
	Building,
	Ready,
	Failed
};

// The newest slot of `demoPath`'s keyframed copy that is valid for the current demo file and
// helper; empty when there is none.
std::wstring DemoKeyframes_ReadyPath(const std::wstring& demoPath);

// Non-blocking. Starts building `demoPath`'s keyframed copy from `sourcePath` (the demo itself,
// or its repaired copy), dense around `markerTicks` -- unless a valid copy already covers those
// ticks, a build is running, or the helper is missing. Writes the slot that is not being played.
void DemoKeyframes_Build(const std::wstring& demoPath, const std::wstring& sourcePath,
	const std::vector<int>& markerTicks);

// One copy: deletes every other demo's keyframed copy (both slots, markers, logs, unfinished
// builds), and this demo's slot that is neither `playingFile` nor its newest valid one. A file
// the engine (or another game instance) holds open stays until it is free.
void DemoKeyframes_KeepOnly(const std::wstring& demoPath, const std::wstring& playingFile);

// Main thread, every frame, with the demo being played (the original's path) and its camera
// markers' ticks. Rebuilds the copy with new dense windows once the markers have settled for 5 s
// outside the windows of the copy (never while a render job runs), tidies the old slot once the
// engine lets go of it. True on the frame a rebuild for this demo finished while it plays from an
// older file: the caller tells the user that reopening it scrubs finer.
bool DemoKeyframes_RunFrame(const std::wstring& demoPath, const std::wstring& playingFile,
	const std::vector<int>& markerTicks, bool renderJobActive);

KeyframeState DemoKeyframes_State();
std::string DemoKeyframes_StatusText();
bool DemoKeyframes_HelperAvailable();

// Waits for an in-flight build (shutdown).
void DemoKeyframes_Reset();

// Console: `mirv_filmmaker keyframes [status]`.
void DemoKeyframes_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);


} // namespace Filmmaker
