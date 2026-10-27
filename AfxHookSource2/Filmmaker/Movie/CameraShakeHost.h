#pragma once

#include "WeaponPivot.h" // WeaponPivotOwner

#include <string>

namespace Filmmaker {

// What the camera shake stack (CameraShakeStack.cpp), its layers (HandheldCam, WeaponRecoil,
// WeaponBob) and the pivot orbs (RecoilPivotMarker.cpp) ask the game they run in. The stack's
// tracking, mode gate, event filtering and summing are the same in every game; how a game answers
// these is not. CS2 answers in CameraShakeHostSource2.cpp, CS:S in
// AfxHookSource/Filmmaker/SourceCameraShake.cpp.

// A demo is open, playing or paused. A paused demo still counts: pausing to compose a shot must not
// drop the springs.
bool ShakeHost_DemoLoaded();
// The game's entities can be read: a demo, or CS2's offline live-match harness. Not the main menu
// or a level load.
bool ShakeHost_EntitiesLive();
// The demo clock: paused, its timescale (1 when unknown) and its tick (-1 when unknown).
bool ShakeHost_DemoPaused();
double ShakeHost_DemoTimescale();
int ShakeHost_DemoTick();
// A render is capturing, so oscillators lock to the demo's own dt and never to wall time.
bool ShakeHost_Recording();

// The user id of the player the view follows in first person, as weapon_fire and player_footstep
// name a player; -1 when there is none.
int ShakeHost_SpectatedUserId();
// The class name of the gun that player holds ("weapon_ak47"); false when none resolves.
bool ShakeHost_SpectatedWeapon(std::string& out);
// Rounds in that gun's magazine; -1 when it cannot be read, which callers treat as "allow", never
// as empty. outWeaponIndex receives the gun's entity index (-1 when unresolved), so a weapon switch
// is never taken for a dry fire.
int ShakeHost_SpectatedClip(int* outWeaponIndex);
// The living player whose eyes are at (x, y, z), the view's origin: its world velocity (units/s),
// whether it stands on the ground and whether it ducks. False when no living player is under the
// camera (a dead or unspawned one included).
bool ShakeHost_BodyAt(double x, double y, double z, double outVelocity[3], bool* outOnGround, bool* outDucking);

// The gun's own transform channel, which Weapon Recoil and Weapon Bob move the viewmodel through.
// Ensure installs it and returns false when the game has none, with why in whyNot; the layers then
// move the camera only.
bool ShakeHost_EnsureWeaponChannel(const char** whyNot);
void ShakeHost_SetWeaponKick(double x, double y, double z, double pitch, double yaw, double roll);

// Draws a pivot orb in the world in its owner's colour (WeaponPivotOwnerColour), or hides it.
void ShakeHost_DrawPivotOrb(WeaponPivotOwner owner, bool enabled, double x, double y, double z, double radius);
// The game's overlays are blacked out (F8), so no orb is drawn.
bool ShakeHost_OverlaysHidden();

} // namespace Filmmaker
