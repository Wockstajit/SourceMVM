#pragma once

// BOT TAKES: record yourself in a private match, then have real bots perform it, over and over,
// while you shoot them and record the demo. See Docs/features/bot-takes.md.

#include "TakesMath.h"

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker::Takes {

// `mirv_filmmaker takes ...`
void RunCommand(int argc, advancedfx::ICommandArgs *args, const char *cmd);
// Main thread, once per frame. Nothing runs here unless takes are in use.
void RunMainThreadFrame();
// Late main-thread frame (after AnimGraph2): poses local preview, multi-actor performers, picker preview, and demo companions.
void RunMainThreadLateFrame(bool panelOpen);
// Last main-thread hook of the pass: poses again, because the engine's own pose sometimes lands after the late frame.
void RunMainThreadPostFrame(bool panelOpen);
// `,"takes":{...}` for the panel.
void AppendStateJson(std::ostringstream &out);
// True while something is being recorded, for the on-screen indicator.
bool Recording();
// The line shown at the top of the screen; empty when nothing is happening. Main thread.
std::string HudLine(bool &recording);
// F6 records, F7 plays, in a private match. True when the key was claimed. Any thread.
bool HandleKey(unsigned vkey, bool down);
// Called for each WM_KEYDOWN before HandleKey: whether Windows says the key was already down (bit 30
// of lParam). A held key then acts once, with no state that a missed release could leave behind.
void NoteKeyRepeat(bool repeat);

// Scene Editor integration API (thread-safe via Takes mutex).
bool Available();
bool SnapshotScene(Scene &outScene, int &outSceneTick, bool &outPlaying, bool &outRecording, bool &outLoop, std::string &outPhase);
bool SetTrackOffset(int trackIdx, const float offsetPos[3], float offsetYaw, bool save = true);
bool ResetTrackOffset(int trackIdx);
bool SetTrackDelayTicks(int trackIdx, int32_t delayTicks);
bool SetTrackEnabled(int trackIdx, bool enabled);
bool SetTrackTeam(int trackIdx, uint8_t team);
bool SetTrackHoldFire(int trackIdx, bool holdFire);
bool DeleteTrack(int trackIdx);
bool AddOrUpdateTrackCue(int trackIdx, int cueIdx, const ActorAnim::Cue &cue);
bool RemoveTrackCue(int trackIdx, int cueIdx);
void SetScrubPreview(bool active, int sceneTick);
void Play();
void Stop();
void SetLoop(bool loop);
void RecordForSceneEditor(const char *name = "");
bool PlaceActorForSceneEditor(const char *clipId, const float *optPos = nullptr, float optYaw = 0.f);
bool EnsureMapSceneAndBotsReady();

} // namespace Filmmaker::Takes
