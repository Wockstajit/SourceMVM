#pragma once

// Scene lighting colour -- tints every light the scene system submits.
//
// RECOVERED BY REVERSE ENGINEERING, not ported from a leak. Every published signature for this
// function was dead on the current build; the function was found instead by following the
// `lb_enable_lights` convar string to its one code xref, to the ConVar object, to the code that
// reads it. The colour offsets were then confirmed from the disassembly rather than trusted:
//
//     lea   rsi, [rdi + 0xe0]      ; rdi = arg2, the scene light object
//     movss xmm0, [rsi + 4]        ; +0xE4   r
//     ucomiss xmm0, xmm6           ; == 0.0 ?
//     movss xmm0, [rsi + 8]        ; +0xE8   g
//     movss xmm0, [rsi + 0xc]      ; +0xEC   b
//     je    <skip>                 ; all three zero -> the engine culls this light
//
// Full derivation, including how to re-run it after a CS2 update: Docs/effects/sun.md 3.3.
//
// ENERGY-BOUNDED TINT. Uniform mode replaces hue while preserving each light's original peak HDR
// radiance. Filtered mode applies the chosen colour channel-by-channel. Neither can amplify a
// light beyond its engine-authored HDR ceiling, so the tint does not become a glow/exposure pass.
//
// The temporary colour is applied around the original call and UNDONE immediately after. The
// light buffer belongs to the engine and survives across frames; leaving a modified value in it
// would compound or destroy authored state.
//
// THREADING: the hook runs on whatever thread the scene system submits lights from. It only
// reads plain scalars published from the main thread and never touches Panorama or entities.

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Installed from HookSceneSystem (SceneSystem.cpp) -- that function is the home for
// scenesystem.dll hooks, so the detour is attached there and the policy lives here.
// Non-fatal: a failed pattern scan disables the feature with a reason rather than erroring out.
void LightFx_Install(void* sceneSystemDll);

bool LightFx_Available(std::string* outReason);
bool LightFx_RotationAvailable(std::string* outReason);

// 0..255 per channel, 255 255 255 = untouched.
void LightFx_SetTint(int r, int g, int b);
void LightFx_SetAmount(float amount); // 0..1 blend from authored colour to chosen tint
void LightFx_SetUniform(bool uniform); // true = hue replacement, false = authored channel filter
// Direct scene lights only. Does not write either shadow switch or baked material lighting.
void LightFx_SetSceneLightsEnabled(bool enabled);
void LightFx_SetEnvironmentSunSuppressed(bool suppressed);
bool LightFx_SceneLightsEnabled();
void LightFx_Reset();
void LightFx_OnLevelInit();

// SUN / SHADOW DIRECTION. The scene-submit hook snapshots each authored normalized direction at
// object +0x184, applies a rigid yaw/pitch rotation without touching the adjacent +0x190 field,
// and restores the snapshot when reset. The hidden lb_allow_shadow_rotation gate is enabled for
// the engine's own shadow-update path. SkyGpuRotation consumes the same direction for the live
// visible sun disc (Docs/effects/sun.md §3.5).
void  LightFx_SetSunYaw(float degrees);
void  LightFx_SetSunPitch(float degrees);
float LightFx_SunYaw();
float LightFx_SunPitch();
// The map's own sun direction (toward the sun, before our yaw/pitch). Yaw circles the sun about
// world up and pitch raises it about the horizontal axis across THIS direction, so the sun disc,
// the sky sprite and the light direction all share one rotation (SunRotationMath::SunRotation).
// SunFx publishes it from the sky material; before that, a light's own direction (which points
// toward the sun) stands in.
void LightFx_SetSunReference(const float towardSun[3]);
bool LightFx_SunReference(float towardSun[3]);

void LightFx_GetTint(int* r, int* g, int* b);
float LightFx_Amount();
bool  LightFx_Uniform();
bool  LightFx_Enabled();

// How many lights the hook has seen since install -- 0 means the detour never fired, which
// distinguishes "not hooked" from "hooked but this map has no dynamic lights".
unsigned long long LightFx_LightsSeen();

void LightFx_AppendStateJson(std::ostringstream& o);

void LightFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
