#pragma once
// Eye adaptation for CS2's tonemapper, modelled on Unreal Engine's auto exposure.
//
// The render thread meters the finished pre-tonemap HDR scene (a center-weighted log-luminance
// histogram, averaged between its 10th and 90th percentiles, read back two frames late so it
// never stalls). The main thread turns that into a target exposure and eases toward it at the
// chosen speed (AutoExposureMath.h), driving mat_tonemap_force_scale through NightMode.
//
// A pinned exposure (Time of day presets, the Exposure slider) always wins; adaptation only runs
// while exposure is on auto.

#include <d3d11.h>

namespace Filmmaker {

// Render thread, at the before-UI boundary (the HDR scene is complete). Cheap no-op while off.
void AutoExposure_RenderThread(ID3D11DeviceContext* immediate);

// Main thread, once per frame. True while adaptation is driving the tonemapper; outScale is the
// exposure to apply this frame.
bool AutoExposure_MainThread(float& outScale);

bool  AutoExposure_Enabled();
void  AutoExposure_SetEnabled(bool enabled);
float AutoExposure_Speed();            // stops per second
void  AutoExposure_SetSpeed(float stopsPerSecond);
float AutoExposure_Compensation();     // stops
void  AutoExposure_SetCompensation(float stops);
float AutoExposure_CurrentScale();     // last applied exposure, 0 before the first meter
bool  AutoExposure_MeteredLog2(float& out);

inline constexpr float kAutoExposureSpeedDefault = 2.f;
inline constexpr float kAutoExposureSpeedMin = .1f;
inline constexpr float kAutoExposureSpeedMax = 20.f;
inline constexpr float kAutoExposureCompensationMax = 4.f;

} // namespace Filmmaker
