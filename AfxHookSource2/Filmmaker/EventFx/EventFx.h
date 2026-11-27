#pragma once

// Event FX: CS2's own particle effects (explosions, fire, blood, sparks, fireworks) spawned in the
// world when a demo event happens -- a kill, a headshot, a grenade going off, the bomb, a timeline
// marker -- at the event itself or in front of the camera, with an optional light burst and camera
// shake. A preset can also be placed in the map as Scene Objects that play on the same event
// (Props.h, `props set <id> play`). Docs/features/event-fx.md.

#include "EventFxMath.h"

#include <sstream>
#include <string>

namespace SOURCESDK { namespace CS2 { class CGameEvent; } }
namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Main thread hooks
void EventFx_OnGameEvent(SOURCESDK::CS2::CGameEvent* event);
void EventFx_RunMainThreadFrame();
void EventFx_OnLevelInit();

// View-setup additive camera shake (pitch/yaw/roll in degrees), called from main.cpp.
bool EventFx_ApplyCameraImpulse(double curTime, float& pitch, float& yaw, float& roll, float& fov);

// State JSON and console commands (`mirv_filmmaker eventfx ...`)
void EventFx_AppendStateJson(std::ostringstream& out);
void EventFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
