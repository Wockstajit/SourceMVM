#pragma once

// Event-Driven FX Presets:
// Turns game events (kill, headshot, flashbang/HE/smoke/molotov pop, bomb blast) and timeline
// marker crossings into reusable visual cues (exposure pulse, camera shake, FOV kick, native
// light burst, chromatic pulse) -- and can bake any preset envelope into editable Scene Keyframes.

#include "EventFxMath.h"

#include <d3d11.h>
#include <sstream>
#include <string>

namespace SOURCESDK { namespace CS2 { class CGameEvent; } }
namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Main thread hooks
void EventFx_OnGameEvent(SOURCESDK::CS2::CGameEvent* event);
void EventFx_RunMainThreadFrame();
void EventFx_OnLevelInit();

// View-setup additive camera impulse (pitch/yaw/roll/fov in degrees), called from main.cpp.
bool EventFx_ApplyCameraImpulse(double curTime, float& pitch, float& yaw, float& roll, float& fov);

// Render thread post-process pulse (exposure, tint flash, chromatic, vignette).
void EventFx_BeginDevice(ID3D11Device* device);
void EventFx_EndDevice();
bool EventFx_RenderPost(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target, double demoTime);

// State JSON and console commands (`mirv_filmmaker eventfx ...`)
void EventFx_AppendStateJson(std::ostringstream& out);
void EventFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
