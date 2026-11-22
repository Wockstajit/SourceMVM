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
void WorldWarp_SetMode(WorldWarp::Mode m, bool applyPresetDefaults = false);
void WorldWarp_Reset();
bool WorldWarp_PlaceOriginAtCamera();

// Called from CSetupView when Little Planet is active to orient CS2's camera for the current
// cubemap face (0..5). Returns true when an override was written.
bool WorldWarp_OverrideCubemapFaceView(int faceIndex, float baseYaw,
	float& pitch, float& yaw, float& roll, float& fov);

// Lock-step CSetupView -> BeforeUi cubemap face handshake (0..5 cycling, -1 when inactive).
int WorldWarp_PeekCubemapFace(float baseYaw);
int WorldWarp_ConsumeCapturedFace();
void WorldWarp_SetActiveCubemapFace(int faceIndex, float baseYaw);
int WorldWarp_ActiveCubemapFace(float* outBaseYaw = nullptr);

void WorldWarp_AppendStateJson(std::ostringstream& out);
void WorldWarp_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
