#pragma once

// LIVE skybox orientation -- yaw, pitch and mirror, applied every frame with no rebuild.
//
// HOW, given sky.vfx has no rotation parameter: env_sky is an ordinary entity and its cubemap is
// sampled through the entity's own scene-node transform, which is why mappers orient a skybox by
// rotating the env_sky in Hammer. So this writes the transform
// (entity -> m_CBodyComponent -> m_pSceneNode -> m_angRotation / m_angAbsRotation) rather than
// touching the material at all.
//
// This REPLACES the baked rotation that build-skybox.ps1 does with -Rotation/-Flip. Baking
// produced a second copy of the skybox for every angle, which is the wrong shape: you want to
// drag a slider and watch the sun move. The script keeps its switches for authoring a skybox
// that should ship pre-oriented, but the UI drives this instead.
//
// Mirroring is a NEGATIVE SCALE on the same node. It costs nothing and needs no second texture.
//
// Values are re-asserted every frame while active: the engine recomputes the sky transform on
// map load, on demo seek, and whenever it re-resolves the skybox, any of which would otherwise
// silently drop the rotation mid-shot.
//
// See Docs/effects/skybox.md.

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {
void SkyOrient_OnLevelInit();

// Degrees. Yaw spins the sky about the vertical axis (the useful one -- it moves the sun round
// the horizon). Pitch tilts it, which is mostly a stylistic effect.
void  SkyOrient_SetYaw(float degrees);
void  SkyOrient_SetPitch(float degrees);
void  SkyOrient_SetMirror(bool mirrored);
void  SkyOrient_SetSkyEnabled(bool enabled);
void  SkyOrient_SetFogEnabled(bool enabled);

float SkyOrient_Yaw();
float SkyOrient_Pitch();
bool  SkyOrient_Mirror();
bool  SkyOrient_SkyEnabled();
bool  SkyOrient_FogEnabled();

// Back to the map's own orientation, restoring the transform this module captured.
void SkyOrient_Reset();

// Main/UI thread, once per frame from World_RunFrame. Cheap no-op when nothing is overridden.
void SkyOrient_RunFrame();

// False when the scene-node chain did not resolve on this CS2 build; `outReason` names the first
// missing field so a refusal says WHICH assumption broke.
bool SkyOrient_Available(std::string* outReason);

// env_sky entities found on the last pass -- 0 means the write had nothing to act on.
int SkyOrient_SkyCount();

void SkyOrient_AppendStateJson(std::ostringstream& o);

void SkyOrient_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
