#pragma once

// Night Mode / exposure control. Pins CS2's auto-exposure tonemapper to a fixed value by
// clamping every C_PostProcessingVolume's min/max exposure pair to the same number.
//
// Because the TONEMAPPER is pinned rather than a dark quad being drawn over the frame, the
// result composites correctly: muzzle flashes still bloom, the sky still reads as sky, and it
// survives the render pipeline because it is part of the scene rather than a post-draw overlay.
//
// APPLIES TO EVERY VOLUME, not just the camera's active one. The reference ports
// (reference/coding help/fog-night-mode/{626888,741472}) resolve
// CPlayer_CameraServices::m_hActivePostProcessingVolume and write only that volume, which is
// right for a cheat and wrong for a filmmaking tool twice over: a flying camera crossing a
// volume boundary would pop mid-shot, and a demo observer standing in no volume at all would
// get nothing written (the feature would look broken). The active handle is still read, but
// only as a `world health` diagnostic.
//
// See Docs/effects/night-mode.md.

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Sentinel for "auto" -- restore the map's own exposure and stop overriding.
constexpr float kExposureAuto = -1.0f;

// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame. Entity-touching, so it
// follows the same threading rule as Cosmetics_RunFrame. Cheap no-op while
// disabled and nothing is snapshotted.
void NightMode_RunFrame();

// kExposureAuto disables the override and restores every volume this module touched.
// Any other value is clamped to [kExposureMin, kExposureMax] and pinned.
void NightMode_SetExposure(float exposure);
float NightMode_Exposure();       // kExposureAuto when not overriding
bool  NightMode_Enabled();        // exposure != kExposureAuto

constexpr float kExposureMin = 0.02f;
constexpr float kExposureMax = 4.0f;

// False when the required schema fields did not resolve on this CS2 build. `outReason` receives
// the name of the first missing field so the refusal can say WHICH assumption broke rather than
// silently no-opping (Docs/agent/cs2-update-recovery.md 2.4).
bool NightMode_Available(std::string* outReason);

// Live diagnostics for `mirv_filmmaker world health`.
int  NightMode_VolumeCount();     // C_PostProcessingVolume entities found on the last scan
int  NightMode_ActiveVolumeIndex(); // entity index the camera is inside, or -1

// Restores every overridden volume and forgets all snapshots. Called on disable and on
// shutdown; also safe to call at any time.
void NightMode_RestoreAll();

void NightMode_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
