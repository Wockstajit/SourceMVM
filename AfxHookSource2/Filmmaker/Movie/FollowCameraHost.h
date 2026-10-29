#pragma once

// What the follow camera (FollowCamera.cpp) asks the game it runs in, so the same behaviour runs in
// CS2 and in Counter-Strike: Source, as SmoothViewHost.h does for Smooth View. CS2 answers in
// FollowCameraHostSource2.cpp, CS:S in AfxHookSource/Filmmaker/SourceFollowCamera.cpp. The view
// itself (free cam, pose, the placed camera's marker, the target box, path drawing) goes through
// CameraBridge.h, which both games answer. The game finds the targets; scoping, ordering and
// picking them is shared (FollowTargetDiscovery.cpp). Main thread only, except where noted.

#include "FollowCamera.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Filmmaker {

// The demo's clock: the tick (0 without a demo), seconds per tick (0 when unknown), the render
// frame count, and whether playback is paused.
int FollowHost_DemoTick();
double FollowHost_IntervalPerTick();
int FollowHost_FrameCount();
bool FollowHost_DemoPaused();
// Resumes a paused demo: a grenade picked before its throw waits for the throw in playback.
void FollowHost_ResumeDemo();
// Jumps the demo to a tick.
void FollowHost_SeekDemo(int tick);
// The playing demo's file, "" when none.
std::string FollowHost_DemoPath();
// The demo's recorded weapon and C4 drops and pickups, for the Events list. Runs on a worker
// thread and gives up when *cancel turns true. False with an error when the scan failed.
bool FollowHost_ReadDemoEvents(const std::string& path, std::atomic<bool>* cancel,
	std::vector<FollowEventRecord>& events, std::string& helperPath, std::string& error);

// The Camera Editor: whether it is open, and handing the mouse to the UI (true) or the game.
bool FollowHost_EditorActive();
void FollowHost_SetEditorCursor(bool ui);

// Live takes the view from the camera path's preview and scrub and the TIMELINE dock's drive.
// Stopping Live gives the dock its drive back when the editor's timeline cursor wants it.
void FollowHost_TakeViewFromOtherCameras();
void FollowHost_ReturnViewToTimeline();

// The target the camera follows: the provider for the state's type, mode and mount.
std::unique_ptr<IFollowTargetProvider> FollowHost_MakeProvider(const FollowCameraState& state, FollowPlayerPose* playerPose);

// Every player, or every weapon and C4, in the scene: not scoped or sorted, each with its distance
// from `from` (a player's from its eyes). A player's entityIndex is the body the camera follows.
// With attachPoints each lists its mount points. None for Grenade: see FollowHost_GrenadeSightings.
std::vector<FollowTargetCandidate> FollowHost_FindTargets(FollowTargetType type, bool attachPoints, const FollowVec3& from);
// The player being spectated, -1 when there is none (free cam and third person have none).
int FollowHost_SpectatedPlayer();
// A player's user id for the UI, -1 when unknown.
int FollowHost_PlayerUserId(int entityIndex);
// Every grenade seen thrown this session, in the air now or earlier.
const std::vector<GrenadeSighting>& FollowHost_GrenadeSightings();
// The mount points an entity offers when followed as the type.
std::vector<FollowAttachPoint> FollowHost_AttachPoints(int entityIndex, FollowTargetType type);

// An entity's handle value, 0 for none.
uint64_t FollowHost_EntityHandle(int entityIndex);
// An entity's origin, and whether it is a player (whose target box stands on its feet rather than
// around its centre). False when there is no such entity.
bool FollowHost_EntityOrigin(int entityIndex, FollowVec3& origin, bool& isPlayer);
// A living player target's active weapon and the C4 it carries (-1 for none), remembered for the
// retarget after its death. False when the entity is not a living player.
bool FollowHost_LivePlayerItems(int entityIndex, int& activeWeaponIndex, int& bombIndex);

// What a mount resolved to, for the diagnostics in mvm_debug and the attach debug readout.
struct FollowMountDescription {
	bool valid = false;
	uint64_t entityHandle = 0;
	std::string className;
	std::string modelName;
	std::string attachKind;
	int attachIndex = -1;
	std::string source;
};
FollowMountDescription FollowHost_DescribeMount(int entityIndex, FollowTargetType type, const std::string& mount,
	FollowPlayerPose* playerPose, bool rigid);
// "follow targetprobe": prints where each body mount of a player resolves.
void FollowHost_PrintTargetProbe(int entityIndex);

} // namespace Filmmaker
