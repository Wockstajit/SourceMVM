#include "DeadlockFollowCamera.h"

#include "DeadlockFollowSolveMath.h"
#include "../Movie/FollowCameraMath.h"
#include "../Movie/CameraBridge.h"
#include "../../Deadlock/DeadlockCameraRuntime.h"
#include "../../Deadlock/DeadlockEngine.h"
#include "../../Deadlock/DeadlockEntities.h"
#include "../Debug/MvmDebugLog.h"
#include "../../../Shared/AfxConsole.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace Filmmaker {

namespace {

namespace FS = Deadlock::FollowSolve;

FollowSettings g_s;

// --- the solve's own state -----------------------------------------------------------------
//
// The camera is a FILTER, not a formula: where it ends up this frame depends on where it was last
// frame, which is the whole difference between a follow camera and a rigidly parented one. So the
// smoothed pose lives here, and it is reset rather than carried whenever continuity is broken --
// a different hero, a re-place, a mode change, a seek. Carrying it across a seek is how a follow
// camera ends up flying across the map over two seconds after a scrub.
struct SolveState {
	bool valid = false;      // true once camPos/outAngles hold a real solved pose
	FS::Vec3 camPos;         // OUTPUT position: == placedPos every frame in lock-on; smoothed in attach
	FS::Angles outAngles;    // OUTPUT angles, after all smoothing/trim/composition
	FS::Vec3 smoothedAim;    // LOCK-ON ONLY: the smoothed AIM POINT (target + offset + prediction)
	FS::LockOnAngleFilter lockOnAngles; // untrimmed history; outAngles is only the final view

	// The target's own motion, for the prediction term. One frame of history is enough: the
	// prediction is a lead, not an extrapolated trajectory.
	bool hadTarget = false;
	FS::Vec3 lastTarget;
	int lastTick = -1;

	// Death handling. `deadFor` seconds of holding the last position before the camera lets go.
	bool targetDead = false;
	double deadFor = 0.0;
	bool released = false;

	int hero = -1;
};

SolveState g_solve;

// LOCK-ON's placed pose. Written ONLY by Place()/Reposition(), and read every frame by Solve() --
// deliberately OUTSIDE SolveState, so that switching the target hero, switching mode, or a seek
// (all of which call ResetSolve()) can never zero it out from under a placed camera. That exact bug
// -- ResetSolve() wiping this alongside the smoothing history it actually owns -- is why this is
// its own variable instead of a field on SolveState like the rest of the per-frame filter state.
struct PlacedPose {
	FS::Vec3 pos;
	FS::Angles ang;
};
PlacedPose g_placed;

// THE LAST POSE THE FRAME ACTUALLY PRODUCED, kept so the report can state it without re-solving.
//
// A report that re-runs the solve is not a report: the solve advances the smoothing filter and
// consumes a frame of target history, so asking "where is the camera" MOVES the camera. That made
// two consecutive reads of the same static frame return different answers, and made a mode change
// followed by a report land somewhere neither mode would have put it.
struct LastSolved {
	bool valid = false;
	double x = 0, y = 0, z = 0, pitch = 0, yaw = 0, roll = 0, fov = 0;
};
LastSolved g_last;

void ResetSolve() {
	g_solve = SolveState();
	g_last = LastSolved();
}

double Clamp(double v, double lo, double hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

bool Is(const char* a, const char* b) { return 0 == _stricmp(a, b); }

void AppendJsonString(std::string& out, const char* s) {
	out += '"';
	for (const char* p = s; p && *p; ++p) {
		if (*p == '"' || *p == '\\') { out += '\\'; out += *p; }
		else if ((unsigned char)*p >= 0x20) out += *p;
	}
	out += '"';
}

void AppendNum(std::string& out, double v, int decimals) {
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
	out += buf;
}

} // namespace

const FollowSettings& FollowCamera_Get() { return g_s; }

void FollowCamera_SetOn(bool on) { g_s.on = on; }
void FollowCamera_Toggle() { g_s.on = !g_s.on; }

void FollowCamera_Place() {
	// Placing again replaces the camera; there is only ever one. A list of follow cameras would be
	// a second path system with no timeline to sequence it.
	//
	// PLACING ALSO SEEDS THE SOLVE from the free camera's current pose, so the camera starts where
	// you were looking rather than snapping in from wherever the filter was left. With no hero
	// picked yet it also takes the nearest one, because "place a camera" and "then pick what it
	// watches" is two clicks for one intention in the overwhelmingly common case.
	//
	// THIS IS THE ONLY PLACE g_placed IS EVER WRITTEN. Lock-on's whole fix is that nothing else may
	// touch it -- see FollowCamera_Solve below, and g_placed's own comment for why it survives
	// every ResetSolve() call this file makes elsewhere.
	double origin[3], angles[3], fov;
	CameraBridge_GetCurrentCamera(origin, angles, fov);

	ResetSolve();
	g_placed.pos = { origin[0], origin[1], origin[2] };
	g_placed.ang = { angles[0], angles[1], angles[2] };

	if (g_s.hero < 0) g_s.hero = Deadlock::Entities_NearestHero(origin);
	g_solve.hero = g_s.hero;

	g_s.placed = true;
	g_s.on = true;
}

void FollowCamera_Reposition() {
	FollowCamera_Place();
}

void FollowCamera_Clear() {
	g_s.on = false;
	g_s.placed = false;
	g_s.hero = -1;
	ResetSolve();
	g_placed = PlacedPose();
}

void FollowCamera_SetMode(FollowMode mode) {
	if (mode == g_s.mode) return;
	g_s.mode = mode;
	// Lock-on and attach put the camera in completely different places; smoothing between them
	// would be a two-second glide through the level rather than a mode change, so the filter
	// history resets. g_placed itself is untouched (see its own comment): switching to Attach and
	// back to Lock-on resumes at the ORIGINAL placed spot, not the world origin -- only an explicit
	// Place()/Reposition() may move it.
	ResetSolve();
}

void FollowCamera_SetHero(int entityIndex) {
	if (entityIndex == g_s.hero) return;
	g_s.hero = entityIndex;
	ResetSolve();
}

void FollowCamera_SetAttach(const char* name) {
	if (name && *name) g_s.attach = name;
}

bool FollowCamera_SetValue(const char* which, double v) {
	if (!which) return false;
	if (Is(which, "offx")) { g_s.offX = Clamp(v, -1000, 1000); return true; }
	if (Is(which, "offy")) { g_s.offY = Clamp(v, -1000, 1000); return true; }
	if (Is(which, "offz")) { g_s.offZ = Clamp(v, -1000, 1000); return true; }
	if (Is(which, "rotpitch")) { g_s.rotPitch = Clamp(v, -180, 180); return true; }
	if (Is(which, "rotyaw")) { g_s.rotYaw = Clamp(v, -180, 180); return true; }
	if (Is(which, "rotroll")) { g_s.rotRoll = Clamp(v, -180, 180); return true; }
	if (Is(which, "fov")) { g_s.fov = Clamp(v, 20, 130); return true; }
	if (Is(which, "look")) { g_s.look = Clamp(v, 0, 1); return true; }
	if (Is(which, "pos")) { g_s.pos = Clamp(v, 0, 1); return true; }
	if (Is(which, "predict")) { g_s.predict = Clamp(v, 0, 0.5); return true; }
	if (Is(which, "deadzone")) { g_s.deadzone = Clamp(v, 0, 10); return true; }
	if (Is(which, "maxturn")) { g_s.maxTurn = Clamp(v, 0, 1440); return true; }
	if (Is(which, "deathhold")) { g_s.deathHold = Clamp(v, 0, 10); return true; }
	return false;
}

bool FollowCamera_SetFlag(const char* which, bool v) {
	if (!which) return false;
	if (Is(which, "releaseondeath")) { g_s.releaseOnDeath = v; return true; }
	if (Is(which, "holdlast")) { g_s.holdLast = v; return true; }
	if (Is(which, "retarget")) { g_s.retarget = v; return true; }
	return false;
}

void FollowCamera_ResetFraming() {
	g_s.offX = 0; g_s.offY = 0; g_s.offZ = 0;
	g_s.rotPitch = 0; g_s.rotYaw = 0; g_s.rotRoll = 0;
	g_s.fov = 68;
}

void FollowCamera_ResetTracking() {
	g_s.look = 0.35; g_s.pos = 0.40; g_s.predict = 0.08;
	g_s.deadzone = 1.5; g_s.maxTurn = 420; g_s.deathHold = 2;
}

// WHERE THE HERO IS, for LOCK-ON's aim point. The eye rather than the feet: aiming at the origin
// frames the floor under the hero, which reads as the camera being slightly wrong in a way nobody
// can name. Falls back to the origin when the pawn has no view offset on this build.
bool AimTargetPoint(void* pawn, FS::Vec3& out) {
	double p[3];
	if (!Deadlock::Entities_EyeOrigin(pawn, p)) return false;
	out = { p[0], p[1], p[2] };
	return true;
}

// ATTACH's mount, in the hero's OWN local frame (x=forward, y=right, z=up), added to whatever the
// user's own offX/offY/offZ sliders say. THE APPROXIMATION THIS FILE STILL CARRIES: these are fixed
// heights/reaches from the pawn's entity ORIGIN, not a read of the hero's actual animated skeleton.
//
// The MOUNT NAMES are real, measured data now (Automation/data/deadlock-hero-bones.json via
// Automation/tools/deadlock-hero-bones.py resolves every one of these to an actual bone on all 38
// heroes), which is what a live bone-transform read would key on -- but Deadlock has no known way
// yet to read a bone's ANIMATED WORLD TRANSFORM (the per-bone matrix array is not a schema field;
// see Docs/deadlock/cameras.md). So today this still returns one fixed offset per mount name,
// exactly as before P5, rather than one per (hero, mount) pair. Fixing that is future work, not
// silently claimed here.
FS::Vec3 AttachMountLocalOffset(const std::string& name) {
	if (name == "origin") return { 0.0, 0.0, 0.0 };
	if (name == "head") return { 0.0, 0.0, 72.0 };
	if (name == "hand_r") return { 12.0, 10.0, 50.0 };
	if (name == "weapon") return { 28.0, 6.0, 52.0 }; // the "muzzle" mount
	return { 0.0, 0.0, 48.0 };   // chest, and the default for a name we do not know
}

bool FollowCamera_Solve(double& x, double& y, double& z,
                        double& pitchOut, double& yawOut, double& rollOut, double& fovOut) {
	if (!g_s.on || !g_s.placed) return false;
	if (!Deadlock::Entities_Ready()) return false;

	// A hero index that no longer resolves is the ordinary case, not an error: entity indices are
	// reused across a replay and a hero who has not spawned yet simply is not there.
	void* pawn = (g_s.hero >= 0) ? Deadlock::Entities_Get(g_s.hero) : nullptr;
	// The hero pawn's entity class; see IsHeroClass in DeadlockEntities for why it is this
	// string and not the C++ class name.
	if (pawn && Deadlock::Entities_ClassName(pawn) != "player") pawn = nullptr;

	if (g_solve.hero != g_s.hero) { ResetSolve(); g_solve.hero = g_s.hero; }

	// --- the clock ------------------------------------------------------------------------------
	//
	// Driven by the DEMO tick, not by wall time. The filter has to settle the same way on the
	// second pass over a shot as on the first, or a rendered take does not match the preview.
	const int tick = Deadlock::Engine_DemoTick();
	const double interval = Deadlock::Engine_IntervalPerTick();
	double dt = FollowPlaybackDelta(g_solve.lastTick, tick, interval);
	if (FollowTickDiscontinuity(g_solve.lastTick, tick)) {
		// A seek. Everything the filter knows is about a moment that is no longer on screen.
		// g_placed is untouched by ResetSolve() (see its own comment) -- a seek forgets the
		// smoothing history, never where the camera was placed.
		ResetSolve();
		g_solve.hero = g_s.hero;
		dt = 0.0;
	}
	g_solve.lastTick = tick;

	// --- death ----------------------------------------------------------------------------------
	const int health = pawn ? Deadlock::Entities_Health(pawn) : -1;
	const bool dead = pawn && health == 0;
	if (dead != g_solve.targetDead) {
		g_solve.targetDead = dead;
		if (!dead) g_solve.released = false;
	}
	g_solve.deadFor = FS::AdvanceDeadElapsed(g_solve.deadFor, g_solve.targetDead, dt);

	const FS::DeathHoldDecision hold = FS::EvaluateDeathHold(
		g_solve.targetDead, g_solve.deadFor, g_s.deathHold, g_s.releaseOnDeath, g_s.holdLast);
	if (hold.release) {
		// Released: the camera lets the hero go and stops claiming the view entirely.
		g_solve.released = true;
		return false;
	}

	FS::Vec3 target;
	const bool haveTarget = pawn && !hold.holdLastPose && AimTargetPoint(pawn, target);

	if (!haveTarget) {
		// NO TARGET, BUT STILL A CAMERA. Hold-last-position means exactly that: keep the pose the
		// filter had rather than snapping somewhere. With no pose at all there is nothing to hold,
		// so the camera stands down and the game's own view comes back.
		if (!g_solve.valid) return false;
		x = g_solve.camPos.x; y = g_solve.camPos.y; z = g_solve.camPos.z;
		pitchOut = g_solve.outAngles.pitch; yawOut = g_solve.outAngles.yaw; rollOut = g_solve.outAngles.roll;
		fovOut = g_s.fov;
		return true;
	}

	// --- prediction -----------------------------------------------------------------------------
	//
	// Lead the hero by a fraction of a second of their own velocity, so a camera chasing a moving
	// target frames where they are going rather than where they were. Measured from the last
	// frame's position, which is why one frame of history is kept.
	const FS::Vec3 velocity = g_solve.hadTarget
		? FS::EstimateVelocity(g_solve.lastTarget, target, dt) : FS::Vec3{};
	g_solve.lastTarget = target;
	g_solve.hadTarget = true;

	const bool firstFrame = !g_solve.valid;

	if (g_s.mode == FollowMode::Attach) {
		// ATTACH: the camera RIDES the hero -- position AND rotation both move with it every frame.
		// See DeadlockFollowSolveMath.h's ATTACH section for exactly which CS2 lines this reproduces.
		double ha[3] = { 0, 0, 0 };
		Deadlock::Entities_Angles(pawn, ha);
		double origin[3] = { 0, 0, 0 };
		if (!Deadlock::Entities_Origin(pawn, origin)) return false;

		const FS::Angles baseAngles{ 0.0, ha[1], 0.0 }; // hero yaw only -- see AttachMountLocalOffset's note
		const FS::Vec3 mountBase = AttachMountLocalOffset(g_s.attach);
		const FS::Vec3 localOffset{ mountBase.x + g_s.offX, mountBase.y + g_s.offY, mountBase.z + g_s.offZ };
		const FS::Vec3 worldOffset = FS::AttachWorldOffset(localOffset, baseAngles);
		const FS::Vec3 desiredPos = FS::AttachDesiredPosition(
			{ origin[0], origin[1], origin[2] }, worldOffset, velocity, g_s.predict);
		const FS::Angles rotationOffset{ g_s.rotPitch, g_s.rotYaw, g_s.rotRoll };
		const FS::Angles desiredAng = FS::AttachDesiredAngles(baseAngles, rotationOffset);

		g_solve.camPos = FS::SmoothOrSnapPosition(!firstFrame, g_solve.camPos, desiredPos, dt, g_s.pos);
		g_solve.outAngles = FS::AttachOutputAngles(!firstFrame, g_solve.outAngles, desiredAng,
			dt, g_s.look, g_s.deadzone, g_s.maxTurn);
	} else {
		// LOCK-ON: the camera position is READ from where it was placed and NEVER recomputed here --
		// see the file header and DeadlockFollowSolveMath.h's LockOnCameraPosition. Only the aim
		// point (target + world offset + prediction) and the resulting look angles are per-frame.
		g_solve.camPos = FS::LockOnCameraPosition(g_placed.pos);

		const FS::Vec3 worldAimOffset{ g_s.offX, g_s.offY, g_s.offZ };
		const FS::Vec3 aimPoint = FS::LockOnAimPoint(target, velocity, g_s.predict, worldAimOffset);
		g_solve.smoothedAim = FS::SmoothOrSnapPosition(!firstFrame, g_solve.smoothedAim, aimPoint, dt, g_s.pos);

		const FS::Angles rawAim = FS::LockOnDesiredAngles(g_solve.camPos, g_solve.smoothedAim);
		const FS::Angles rotationTrim{ g_s.rotPitch, g_s.rotYaw, g_s.rotRoll };
		g_solve.outAngles = g_solve.lockOnAngles.Sample(rawAim, dt, g_s.look,
			g_s.deadzone, g_s.maxTurn, rotationTrim, g_placed.ang.roll);
	}
	g_solve.valid = true;

	x = g_solve.camPos.x;
	y = g_solve.camPos.y;
	z = g_solve.camPos.z;
	pitchOut = g_solve.outAngles.pitch;
	yawOut = g_solve.outAngles.yaw;
	rollOut = g_solve.outAngles.roll;
	fovOut = g_s.fov;

	if (MvmDebugLog_Active()) {
		static unsigned samples = 0;
		if ((samples++ % 64) == 0)
			MvmDebugLog_Linef("deadlock.follow",
				"tick=%d dt=%.6f hero=%d hp=%d mode=%s target=%.1f,%.1f,%.1f "
				"cam=%.1f,%.1f,%.1f aim=%.3f,%.3f trim=%.3f,%.3f,%.3f ang=%.3f,%.3f,%.3f",
				tick, dt, g_s.hero, health, g_s.mode == FollowMode::Attach ? "attach" : "lockon",
				target.x, target.y, target.z, x, y, z,
				g_solve.lockOnAngles.aim.pitch, g_solve.lockOnAngles.aim.yaw,
				g_s.rotPitch, g_s.rotYaw, g_s.rotRoll, pitchOut, yawOut, rollOut);
	}
	return true;
}

void FollowCamera_RunFrame(bool somethingElseOwnsTheView) {
	if (somethingElseOwnsTheView) {
		// Not an error and not a reset: the follow camera keeps its filter state so that handing
		// the view back after a shot does not snap. What it must NOT do is push a pose.
		return;
	}
	double x, y, z, pitch, yaw, roll, fov;
	if (!FollowCamera_Solve(x, y, z, pitch, yaw, roll, fov)) {
		g_last.valid = false;
		return;
	}
	g_last.valid = true;
	g_last.x = x; g_last.y = y; g_last.z = z;
	g_last.pitch = pitch; g_last.yaw = yaw; g_last.roll = roll; g_last.fov = fov;
	// Pushed every frame, deliberately: the bridge consumes a pose once, so a dropped frame means
	// the game's own camera shows through rather than the follow camera silently pinning.
	Deadlock::CameraRuntime_PushOwnedPose(Deadlock::ViewOwner::Follow, x, y, z, pitch, yaw, roll, fov);
}

bool FollowCamera_OwnsView() {
	// It owns the view when it is on, placed and actually producing a pose. Asking the solve is the
	// only honest answer: "on" is a switch, "owns the view" is a result.
	return g_s.on && g_s.placed && g_solve.valid && !g_solve.released;
}

void FollowCamera_AppendStateJson(std::string& out) {
	out += ",\"follow\":{\"on\":";
	out += g_s.on ? "true" : "false";
	out += ",\"placed\":";
	out += g_s.placed ? "true" : "false";
	out += ",\"mode\":";
	AppendJsonString(out, g_s.mode == FollowMode::Attach ? "attach" : "lockon");
	out += ",\"hero\":";
	if (g_s.hero < 0) out += "null";
	else AppendNum(out, g_s.hero, 0);
	out += ",\"attach\":";
	AppendJsonString(out, g_s.attach.c_str());
	out += ",\"offx\":";     AppendNum(out, g_s.offX, 1);
	out += ",\"offy\":";     AppendNum(out, g_s.offY, 1);
	out += ",\"offz\":";     AppendNum(out, g_s.offZ, 1);
	out += ",\"rotpitch\":"; AppendNum(out, g_s.rotPitch, 1);
	out += ",\"rotyaw\":";   AppendNum(out, g_s.rotYaw, 1);
	out += ",\"rotroll\":";  AppendNum(out, g_s.rotRoll, 1);
	out += ",\"fov\":";      AppendNum(out, g_s.fov, 1);
	out += ",\"look\":";     AppendNum(out, g_s.look, 3);
	out += ",\"pos\":";      AppendNum(out, g_s.pos, 3);
	out += ",\"predict\":";  AppendNum(out, g_s.predict, 3);
	out += ",\"deadzone\":"; AppendNum(out, g_s.deadzone, 2);
	out += ",\"maxturn\":";  AppendNum(out, g_s.maxTurn, 0);
	out += ",\"deathhold\":";AppendNum(out, g_s.deathHold, 2);
	out += ",\"releaseOnDeath\":";
	out += g_s.releaseOnDeath ? "true" : "false";
	out += ",\"holdLast\":";
	out += g_s.holdLast ? "true" : "false";
	out += ",\"retarget\":";
	out += g_s.retarget ? "true" : "false";
	// The panel greys the target controls on this. Reads the last scan's count -- see
	// Entities_HeroCount's own comment for why this must never trigger a fresh scan itself.
	out += ",\"targetsAvailable\":";
	out += (Deadlock::Entities_HeroCount() > 0) ? "true" : "false";
	out += "}";
}

void FollowCamera_AppendHeroesJson(std::string& out) {
	out += ",\"heroes\":";
	Deadlock::Entities_AppendHeroesJson(out);
}

void FollowCamera_Report() {
	advancedfx::Message(
		"  follow:    %s  mode=%s  hero=%d  attach=%s\n",
		g_s.on ? "on" : "off",
		g_s.mode == FollowMode::Attach ? "attach" : "lockon",
		g_s.hero, g_s.attach.c_str());
	advancedfx::Message(
		"             framing offx=%.0f offy=%.0f offz=%.0f rotpitch=%.0f rotyaw=%.0f rotroll=%.0f fov=%.0f\n",
		g_s.offX, g_s.offY, g_s.offZ, g_s.rotPitch, g_s.rotYaw, g_s.rotRoll, g_s.fov);
	advancedfx::Message(
		"             tracking look=%.2f pos=%.2f predict=%.2f deadzone=%.1f maxturn=%.0f hold=%.1f\n",
		g_s.look, g_s.pos, g_s.predict, g_s.deadzone, g_s.maxTurn, g_s.deathHold);
	if (g_s.mode == FollowMode::LockOn && g_s.placed) {
		advancedfx::Message("             placed at %.1f %.1f %.1f  (fixed -- lock-on never moves this)\n",
			g_placed.pos.x, g_placed.pos.y, g_placed.pos.z);
	}
	// Reported from the last frame's result, never by solving again -- see LastSolved.
	if (g_last.valid) {
		advancedfx::Message(
			"             SOLVING at %.1f %.1f %.1f  looking %.1f %.1f  fov %.0f\n",
			g_last.x, g_last.y, g_last.z, g_last.pitch, g_last.yaw, g_last.fov);
	} else if (!Deadlock::Entities_Ready()) {
		advancedfx::Warning(
			"             not solving: the entity list did not resolve.\n");
		Deadlock::Entities_Report();
	} else if (!g_s.placed) {
		advancedfx::Message("             not solving: no camera placed (K, or PLACE).\n");
	} else if (g_s.hero < 0) {
		advancedfx::Message("             not solving: no hero picked.\n");
	} else {
		advancedfx::Message("             not solving: hero %d is not in the replay right now.\n",
		                    g_s.hero);
	}
}

} // namespace Filmmaker
