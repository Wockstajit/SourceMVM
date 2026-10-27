#pragma once

#include "CameraShakeTypes.h"

#include <functional>
#include <string>

namespace SOURCESDK { namespace CS2 { class CGameEvent; } }
namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// The camera-shake layer stack: the ONE place main.cpp talks to for HandheldCam and
// WeaponRecoil.
//
// Why this exists rather than separate calls in main.cpp: both modules need the same pose
// tracker (planar velocity, vertical speed, eye height vs a baseline, seek detection) and the
// same camera-mode gate. Computing that once here keeps the modules independent of each other
// and keeps main.cpp's already-crowded view-setup trampoline to a single new block.
//
// IMPORTANT -- how this differs from ViewFx. ViewFx applies only when NOTHING else claimed the
// pose that frame (plain spectate). These layers are meant to ride ON TOP of the attach cameras
	// attach previews — they should feel alive, not rigid — so they run after the
// whole override stack and gate on an explicit allow-list instead:
//
//     Pov        -> yes   (plain first-person spectate)
	//     Attach     -> yes   (FollowCamera attach previews)
//     ThirdPerson-> yes   (orbit camera -- used pushed INSIDE the pawn as a POV-style shot)
//     FreeCam    -> no    hand-flown
//     CameraPath -> no    hand-authored
//     Other      -> no
//
// Everything runs on the demo clock (curtime), never wall clock, so a rendered movie is
// reproducible frame-for-frame and a paused demo holds a still frame you can compose against.

// Applies the summed shake to the view for this frame. Returns true if anything changed.
//
// Call from the view-setup trampoline AFTER the override stack and BEFORE the write-back.
// `curTime` is the demo clock (g_MirvTime.curtime_get()). Angles in degrees.
//
// Always call this, even in a disallowed mode: the tracker needs to know it was gated so the
// next allowed frame is treated as a fresh sample rather than integrating across the gap.
// `appliedHandheld` receives only the post-center-lock handheld contribution selected for
// viewmodel decoupling; recoil remains independent.
bool CameraShake_Apply(
	double curTime, ShakeCamMode mode,
	float& x, float& y, float& z,
	float& pitch, float& yaw, float& roll,
	float& fov, ShakePose* appliedHandheld = nullptr);

// The fields of a weapon_fire or player_footstep event the stack reads: who, and with which gun.
struct ShakeEventFields {
	int userId = -1;
	std::string weapon;
};

// Game-event fan-in, from the game's event listener. `read` fills the event's fields and is called
// only once they are needed, because reading them costs something (CS2 converts KV3 to JSON).
// Filters to the spectated player, de-duplicates a double delivery, and forwards to whichever
// module cares:
//   weapon_fire     -> WeaponRecoil
//   player_footstep -> WeaponBob's step sync
void CameraShake_OnEvent(const char* name, const std::function<bool(ShakeEventFields&)>& read);

// CS2's listener entry (CameraShakeHostSource2.cpp). Called from GameEvents.cpp on both the server
// and client-side paths (demo events arrive on the client-side one).
void CameraShake_OnGameEvent(SOURCESDK::CS2::CGameEvent* event);

// Main-thread per-frame pump: drops integrated state when no demo is playing, so re-opening a
// demo never inherits the previous one's springs. Called from Filmmaker::RunMainThreadFrame.
void CameraShake_RunFrame();

// True while either module is enabled. Lets callers skip work cheaply.
bool CameraShake_Active();

// Whether a shake would actually be SEEN right now. False in free cam / on a baked path
// (gated by design) and when no first-person POV resolves. `outWhy` receives a short reason
// for the operator. Exists so a Test Shot can say why nothing happened instead of claiming
// success -- the spring runs either way, but the result is discarded before it is drawn.
bool CameraShake_ShakeVisible(const char** outWhy);

// The classname of the gun the spectated player is currently holding ("weapon_ak47"), or false
// when no POV / weapon resolves. Two callers need it, both for the same reason: `weapon_fire`'s
// weapon field is optional, so the recoil scope resolver needs a second source before it decides
// a shot belongs to no category -- and a Test Shot needs to know what is actually in shot.
bool CameraShake_SpectatedWeapon(std::string& out);

// The viewed pawn's own movement: world-space velocity (x, y, z units/sec) and FL_ONGROUND. For
// effects that need the player's real movement rather than the camera's, which is only a position
// difference over a demo clock that holds and jitters. (x, y, z) is the current camera origin, used
// to keep hold of the right pawn. False when no living pawn is under the camera.
bool CameraShake_ViewedPawnMotion(double x, double y, double z, double outVel[3], bool* outOnGround);

// Drops every module's integrated state (seek, spectator switch, demo change).
void CameraShake_Reset();

// Console entry: "mirv_filmmaker camshake ..." -- stack-level diagnostics only. The per-module
// commands are `handheld` and `recoil`.
void CameraShake_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
