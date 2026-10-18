#pragma once

// Sun controls -- where it sits, how big it is, how bright, and how hot its glow ring burns.
//
// TWO SEPARATE THINGS, deliberately in one module because they are one thing to the operator:
//
//   1. THE SUN IN THE SKY -- parameters on the sky material (sky.vfx):
//        SolarPosition   vec3, a UNIT DIRECTION. Measured on sky_de_nuke: (0.4226, 0, 0.9063),
//                        which norms to exactly 1.0 and is elevation 65 / azimuth 0.
//        SolarRadius     float, the ANGULAR DIAMETER IN DEGREES. Ships at 0.51 -- the real sun's
//                        angular size. This is the "how big is the disc" knob.
//        SolarIrradiance vec3, RGB brightness. Ships around (2.11, 2.12, 1.79) -- slightly warm.
//
//   2. THE GLOW RING -- a separate material (materials/effects/glows/sun_disc_glow_*) drawn with
//      csgo_effects.vfx, the SAME shader as the clouds. Its g_flColorBoost ships at ~130, which
//      is why the halo blows out so hard on some maps. That is the knob to pull when the ring is
//      too bright, NOT SolarIrradiance -- they are different objects.
//
// Both are material-parameter writes through SceneSystem_FindMaterialParam, the mechanism already
// proven in-game by the cloud tint. Nothing here touches entity memory.
//
// SIZE AND BRIGHTNESS ARE MULTIPLIERS on the map's own values, for the same reason as CloudFx:
// every map authors its sun differently, so 1.00 means "untouched" everywhere and restore is
// exact. POSITION is absolute (azimuth/elevation in degrees), because "the sun 30 degrees round
// from wherever the mapper put it" is not how anyone thinks about framing a shot.
//
// See Docs/effects/sun.md.

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {
void SunFx_OnLevelInit();

constexpr float kSunMulDefault = 1.0f;

// Absolute, degrees. Azimuth wraps; elevation is clamped to [-20, 90] (below about -20 the sun is
// far enough under the horizon that nothing changes on screen).
void  SunFx_SetAzimuth(float degrees);
void  SunFx_SetElevation(float degrees);

// Multipliers on the map's own values.
void  SunFx_SetSize(float mul);        // SolarRadius        -- disc size
void  SunFx_SetBrightness(float mul);  // SolarIrradiance    -- sun brightness
void  SunFx_SetGlow(float mul);        // g_flColorBoost     -- the halo ring
void  SunFx_SetGlowSize(float mul);    // draw-time billboard scale for the halo ring
void  SunFx_SetVisible(bool visible);

float SunFx_Azimuth();
float SunFx_Elevation();
float SunFx_Size();
float SunFx_Brightness();
float SunFx_Glow();
float SunFx_GlowSize();
bool  SunFx_Visible();

// True once the map's own sun direction has been read, so the UI can seed its sliders from the
// map rather than from an arbitrary zero.
bool  SunFx_HasBaseline();
bool SunFx_PositionOverridden();

// Re-writes every sun/sky material from the current values. Call after a map change.
void SunFx_Apply();

// Is anything about the sun actually overridden right now? Lets a caller skip the whole material
// walk when there is nothing to re-assert -- ApplyToSky SNAPSHOTS each material the first time it
// sees it, so walking for no reason is not free and not harmless.
bool SunFx_HasOverride();

// SUN DIRECTION, via the light ENTITY's transform rather than the render-time light object.
// Writing the render buffer crashed CS2 repeatedly; the entity is where the direction originates,
// so rotating it lets the shadow/cascade setup re-derive consistently. Main thread, per frame.
void  SunFx_RunFrame();
void  SunFx_SetLightYaw(float degrees);
void  SunFx_SetLightPitch(float degrees);
float SunFx_LightYaw();
float SunFx_LightPitch();
int   SunFx_SunLightCount();
// Number of loaded sky materials exposing SolarPosition.  This distinguishes the entity/material
// sun path (Nuke-style) from a sun baked into the sky texture (Mirage-style).
int   SunFx_SolarPositionCount();
void  SunFx_ResetLightAngles();

// THE SUN AS THE SKY DRAWS IT, for the lens flare (LensFxMath.h "the sun"): SolarPosition of the
// first sky material that has one, read live so a moved or rotated sun is followed. Refresh is a
// read-only material walk on the main thread, at most once a second; the direction is then
// readable from any thread. False on a map whose sun is painted into the sky texture.
void SunFx_RefreshVisibleSun();
bool SunFx_VisibleSunDirection(float out[3]);

// Keeps sky.vfx's parameterized solar disc on the same delta as Scene Lights. This is the
// material fallback for maps that do not submit a separate sun_disc_glow sprite.
void SunFx_SetSceneLightRotation(float yawDegrees, float pitchDegrees);

// Keeps the visible solar disc and its glow material on the same colour/amount/mode as the
// physical sun light. colorOnly preserves authored peak brightness; filter/dim multiplies the
// authored RGB and therefore permits a genuinely dark or black sun.
void SunFx_SetSceneLightTint(int r, int g, int b, float amount, bool colorOnly);

// Restores every touched material and forgets the snapshots.
void SunFx_ResetAll();

int SunFx_SkyMaterialCount();   // sky materials carrying Solar* parameters
int SunFx_GlowMaterialCount();  // sun_disc_glow materials found

// Arms a bounded probe that logs, per material, WHICH of these parameters actually resolved.
// The first thing to run on a new map or after a CS2 update: a sky material that does not
// override Solar* may or may not expose it, and that is a question only the runtime can answer.
void SunFx_Probe();

void SunFx_AppendStateJson(std::ostringstream& o);

void SunFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
