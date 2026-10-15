#pragma once

// The follow camera's settings: a placed camera that locks on to a hero, or attaches to a point on
// its model.
//
// WHAT THIS IS AND IS NOT. Deadlock's replay is third-person, so this is never a POV camera. The
// two modes are deliberately different shapes, per CS2's own Movie/FollowCamera.cpp (the parity
// spec -- see DeadlockFollowSolveMath.h for the exact CS2 line numbers every rule below is grounded
// in):
//
//   LOCK-ON keeps the placed camera POSITION FIXED FOREVER and only turns to aim. `offX/offY/offZ`
//   are a WORLD-axis offset on the AIM POINT (not the camera), and `rotPitch/rotYaw/rotRoll` TRIM
//   the resulting look. This replaced an earlier, WRONG Deadlock-only model that orbited
//   yaw/pitch/distance/height/lateral sliders around the target every frame -- that model is gone;
//   there is no function anywhere in this port that recomputes the lock-on camera's position from a
//   slider.
//
//   ATTACH rides a point on the hero's model: `offX/offY/offZ` are a LOCAL offset (forward / right /
//   up in the mount's own frame) and `rotPitch/rotYaw/rotRoll` are a rotation COMPOSED onto the
//   mount's own orientation (not a trim -- attach and lock-on use their six shared numbers
//   differently, exactly as CS2's Framing page does for its two modes).
//
// ITS OWN MODULE. The shot (DeadlockShot) owns markers and the path; this owns one camera and its
// target. They are separate features with separate state, separate commands and separate lifetimes
// -- a follow camera is not a marker and is not saved with the path -- so they do not share a file.
//
// WHERE THE HERO COMES FROM. `Deadlock::Entities` walks the client entity list and hands back every
// live hero pawn with its position, health, team and real name.
//
// WHAT IS STILL APPROXIMATE, stated here because the UI offers it. ATTACH's mount ORIGIN (chest,
// head, right hand, weapon muzzle) is still an offset from the pawn's entity origin, rotated by the
// hero's own yaw -- not a read of the actual animated bone. The bone NAMES themselves are real,
// measured data (Automation/data/deadlock-hero-bones.json, resolved through
// Automation/tools/deadlock-hero-bones.py) and pin down exactly which bone each mount SHOULD read
// once a live bone transform read exists; reading it live needs the model's per-bone world-transform
// array, which is not a schema field and has not yet been found by a live probe (see
// Docs/deadlock/cameras.md and the P0 findings for what has and has not been measured). So picking
// Head really does raise the mount, using a real per-hero bone-height figure is not yet possible,
// and the resolution is documented rather than implied.

#include <string>

namespace Filmmaker {

enum class FollowMode {
	LockOn = 0,   // placed camera, turns to keep the hero framed
	Attach = 1,   // rides a point on the hero's model
};

struct FollowSettings {
	bool on = false;
	bool placed = false;
	FollowMode mode = FollowMode::LockOn;

	// The hero's entity index, or -1 for none. A string key would have to be resolved twice.
	int hero = -1;
	std::string attach = "chest"; // mount name: origin | chest | head | hand_r | weapon

	// Framing: SIX NUMBERS, meaning differently per mode -- see the file header. Same shape, same
	// command names (`offx`, `offy`, `offz`, `rotpitch`, `rotyaw`, `rotroll`), same UI tiles, for
	// both modes -- exactly CS2's own Framing page.
	double offX = 0.0, offY = 0.0, offZ = 0.0;
	double rotPitch = 0.0, rotYaw = 0.0, rotRoll = 0.0;
	double fov = 68.0;

	// Tracking: how hard it chases. `look`/`pos` are smoothing HALF-TIMES in seconds (a smaller
	// number chases harder), matching DeadlockFollowSolveMath.h's HalfTimeAlpha -- these numbers
	// were already being passed straight into that exact formula before this file existed, so their
	// meaning and defaults are unchanged by the P5 rewrite.
	double look = 0.35, pos = 0.40, predict = 0.08;
	double deadzone = 1.5, maxTurn = 420.0, deathHold = 2.0;

	bool releaseOnDeath = true;
	bool holdLast = false;
	bool retarget = true;
};

const FollowSettings& FollowCamera_Get();

// Every setter takes ONE value, because the UI writes one value at a time and a compound setter
// makes "which control changed this" unanswerable from a log. Each clamps to the range the column
// advertises, so a console command cannot put the camera somewhere the UI says is impossible.
void FollowCamera_SetOn(bool on);
void FollowCamera_Toggle();
void FollowCamera_Place();
void FollowCamera_Reposition();
void FollowCamera_Clear();
void FollowCamera_SetMode(FollowMode mode);
void FollowCamera_SetHero(int entityIndex);
void FollowCamera_SetAttach(const char* name);

// `which` is one of the framing or tracking field names the column uses -- offx, offy, offz,
// rotpitch, rotyaw, rotroll, fov, look, pos, predict, deadzone, maxturn, deathhold. Returns false
// for a name it does not know, so a typo in a command is an error rather than a silent no-op.
bool FollowCamera_SetValue(const char* which, double value);
bool FollowCamera_SetFlag(const char* which, bool value);

void FollowCamera_ResetFraming();
void FollowCamera_ResetTracking();

// True when the camera actually produced a view this frame. Always false until the port can
// enumerate entities; see the header comment.
bool FollowCamera_Solve(double& x, double& y, double& z,
                        double& pitchOut, double& yawOut, double& rollOut, double& fovOut);
bool FollowCamera_OwnsView();

// Solves and pushes the pose, once per main-thread frame.
//
// WHO OUTRANKS WHOM, and why the order is this way round:
//
//   1. the SHOT -- a camera path that is playing, or a timeline being scrubbed. That is an
//      explicit, timed camera move and nothing may interrupt it.
//   2. the GRAPH, while it is driving. Same argument: the user is watching a curve edit.
//   3. the FOLLOW camera.
//
// The caller passes whether either of the first two owns the view this frame, rather than this
// module reaching into them: the shot does not depend on the follow camera and must not start.
void FollowCamera_RunFrame(bool somethingElseOwnsTheView);

void FollowCamera_AppendStateJson(std::string& out);

// The `heroes` array: who the follow camera could pick, straight from the entity list.
void FollowCamera_AppendHeroesJson(std::string& out);

void FollowCamera_Report();

} // namespace Filmmaker
