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

// A paused demo freezes particles: plays the demo for `seconds`, then pauses and goes back to the
// tick it was on, so Test and Play now show their effect moving. Nothing while it plays. Main thread.
void EventFx_PreviewWhilePaused(double seconds);

// Every event effect placed at a spot, for its icon in the view (Props.cpp AppendMarkers). Any thread.
struct EventFxSpot {
	int id = 0;
	std::string name, trigger;
	float pos[3] = { 0, 0, 0 };
	bool enabled = true, selected = false;
};
void EventFx_Spots(std::vector<EventFxSpot>& out);

// State JSON and console commands (`mirv_filmmaker eventfx ...`)
void EventFx_AppendStateJson(std::ostringstream& out);
void EventFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
