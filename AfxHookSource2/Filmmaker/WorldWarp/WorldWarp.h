#pragma once

// World Effects (WorldWarp): Little Planet, Inception, Rolling Horizon, World Wave, World Fold.
// Main-thread settings, presets, scene origin placement, JSON state, console commands, and
// cubemap face view override for 360-degree Little Planet capture.

#include "WorldWarpMath.h"

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

WorldWarp::Settings WorldWarp_Get();
bool WorldWarp_Active();
bool WorldWarp_WantsCubemap();
bool WorldWarp_WantsVertexWarp();
uint32_t WorldWarp_SettingsGeneration();
void WorldWarp_SetMode(WorldWarp::Mode m, bool applyPresetDefaults = false);
void WorldWarp_Reset();
bool WorldWarp_PlaceOriginAtCamera();

// Called from CSetupView when Little Planet is active to orient CS2's camera for the current
// cubemap face (0..5). Returns true when an override was written.
bool WorldWarp_OverrideCubemapFaceView(int faceIndex, float baseYaw,
	float& pitch, float& yaw, float& roll, float& fov);

// Lock-step CSetupView -> BeforeUi cubemap face handshake (0..5 cycling, -1 when inactive).
int WorldWarp_PeekCubemapFace(float baseYaw);
// Render thread, after a cubemap face rendered at demoTime: returns the face to capture, or -1 when
// the set's demo time moved and the face is dropped. outPublish is set when the six faces are complete.
int WorldWarp_ConsumeCapturedFace(double demoTime, bool& outPublish);
// Main thread: holds the demo on one time while a Little Planet set is captured.
void WorldWarp_RunMainThreadFrame();
void WorldWarp_SetActiveCubemapFace(int faceIndex, float baseYaw);
int WorldWarp_ActiveCubemapFace(float* outBaseYaw = nullptr);

void WorldWarp_AppendStateJson(std::ostringstream& out);
void WorldWarp_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
