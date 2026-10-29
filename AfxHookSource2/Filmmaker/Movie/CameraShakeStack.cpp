#include "CameraShakeStack.h"

#include "CameraCenterLockMath.h"
#include "CameraShakeHost.h" // what this asks the game: the watched player, the demo clock, the gun channel
#include "CameraShakePhaseClockMath.h" // phase-locked oscillator clock over the demo clock
#include "CameraShakeTrackerMath.h"    // is this sample continuous, or a cut?
#include "FollowCameraMath.h" // FollowRotateVector: camera-basis offset -> world
#include "HandheldCam.h"
#include "RecoilPivotMarker.h" // pivot marker: where the gun hinges, drawn in the world
#include "WeaponRecoil.h"
#include "WeaponBob.h"
#include "WeaponPivot.h" // the two independent hinges (recoil / bob)

#include "../Debug/MvmDebugLog.h"
#include "GunWeaponMath.h"
#include "../../../Shared/AfxConsole.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <sstream>

namespace Filmmaker {

namespace {

// ---- pose tracker -----------------------------------------------------------------------
//
// Same shape as ViewFx's tracker and for the same reasons: the view-setup pose is the only
// movement signal available here, it is sampled once per RENDER frame (so several frames can
// share one demo tick), and it can jump arbitrarily far on a seek or spectator switch. A jump
// must never be integrated as velocity -- that is what would turn a seek into a violent shake.
// Sample continuity thresholds live in CameraShakeTrackerMath.h, with their tests.
constexpr double kVelocityTau = 0.08;     // planar/vertical velocity smoothing
constexpr double kEyeBaselineTau = 1.5;   // slow eye-height baseline (crouch detection)

struct Tracker {
	bool have = false;
	double lastTime = 0.0;
	double lastX = 0.0, lastY = 0.0, lastZ = 0.0;
	double velX = 0.0, velY = 0.0, velZ = 0.0;
	double eyeBaseline = 0.0;
	// Held across same-tick render frames so a repeated demo tick reuses the previous estimate
	// instead of dividing by a zero dt.
	double lastDt = 0.0;
	// Subdivides a demo tick for oscillator phase only; see CameraShakePhaseClockMath.h.
	PhaseClockState phaseClock;
};
Tracker g_tracker;

double SmoothAlpha(double dt, double tau) {
	if (dt <= 0.0) return 0.0;
	if (tau <= 0.0) return 1.0;
	return 1.0 - std::exp(-dt / tau);
}

void ResetTracker(double now, double x, double y, double z) {
	g_tracker.have = true;
	g_tracker.lastTime = now;
	g_tracker.lastX = x; g_tracker.lastY = y; g_tracker.lastZ = z;
	g_tracker.velX = g_tracker.velY = g_tracker.velZ = 0.0;
	g_tracker.eyeBaseline = z;
	g_tracker.lastDt = 0.0;
	PhaseClockReset(g_tracker.phaseClock);
}

// Wall-clock dt between Apply calls, for the modules' paused-demo test previews only (see
// ShakeInput.wallDt). Clamped so an alt-tab or load stall never integrates as one huge step.
double WallDtSeconds() {
	static LARGE_INTEGER s_freq = {};
	static LARGE_INTEGER s_last = {};
	if (s_freq.QuadPart == 0) {
		QueryPerformanceFrequency(&s_freq);
		QueryPerformanceCounter(&s_last);
		return 0.0;
	}
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	double dt = (double)(now.QuadPart - s_last.QuadPart) / (double)s_freq.QuadPart;
	s_last = now;
	if (dt < 0.0) dt = 0.0;
	if (dt > 0.05) dt = 0.05;
	return dt;
}

// Builds this frame's ShakeInput. `fresh` means "do not integrate anything".
ShakeInput Track(double now, double x, double y, double z, double pitch, double yaw, double roll) {
	ShakeInput in;
	in.demoTime = now;
	in.wallDt = WallDtSeconds();
	in.x = x; in.y = y; in.z = z;
	in.pitch = pitch; in.yaw = yaw; in.roll = roll;

	if (!g_tracker.have) {
		ResetTracker(now, x, y, z);
		in.fresh = true;
		return in;
	}

	const double dt = now - g_tracker.lastTime;

	// Is this sample continuous with the last one? Both the backwards-jitter tolerance and the
	// teleport test live in CameraShakeTrackerMath.h, which explains why the teleport test is on
	// DISTANCE rather than on a speed derived from the demo clock's own step.
	const double moveX = x - g_tracker.lastX;
	const double moveY = y - g_tracker.lastY;
	const TrackerBreak cut = ClassifyTrackerSample(dt, moveX, moveY);

	if (cut == TrackerBreak::Gap) {
		// Backwards (seek) or a long gap (paused, alt-tabbed, loading): restart clean.
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("camera.shake.fresh", "reason=gap dt=%.4f", dt);
		ResetTracker(now, x, y, z);
		in.fresh = true;
		return in;
	}

	if (dt <= 0.0) {
		// Same demo tick, another render frame. Hold: reuse the last velocity estimate and
		// report dt = 0 so modules skip integration rather than double-stepping.
		in.dt = 0.0;
		// Oscillators are the exception. Holding their phase here is what made the viewmodel
		// repeat a frame and then jump -- at 110 fps against a 64-tick demo that is ~40% of
		// frames. They get a subdivided step instead, which still sums to this tick's own dt.
		in.phaseDt = PhaseClockStep(g_tracker.phaseClock, 0.0, in.wallDt, ShakeHost_Recording());
		in.vx = g_tracker.velX; in.vy = g_tracker.velY;
		in.vz = g_tracker.velZ;
		in.speed = std::sqrt(g_tracker.velX * g_tracker.velX + g_tracker.velY * g_tracker.velY);
		in.eyeRise = z - g_tracker.eyeBaseline;
		return in;
	}

	const double dx = moveX;
	const double dy = moveY;
	const double dz = z - g_tracker.lastZ;

	if (cut == TrackerBreak::Teleport) {
		// Further than any player could travel in this sample: a seek or a spectator switch.
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("camera.shake.fresh",
				"reason=teleport dist=%.1f limit=%.1f dt=%.4f move=(%.1f %.1f %.1f)",
				std::sqrt(dx * dx + dy * dy), TrackerTeleportDistance(dt), dt, dx, dy, dz);
		ResetTracker(now, x, y, z);
		in.fresh = true;
		return in;
	}

	const double a = SmoothAlpha(dt, kVelocityTau);
	g_tracker.velX += ((dx / dt) - g_tracker.velX) * a;
	g_tracker.velY += ((dy / dt) - g_tracker.velY) * a;
	g_tracker.velZ += ((dz / dt) - g_tracker.velZ) * a;

	// Slow eye-height baseline. Crouching drops the eye ~18 units in a few frames, which shows
	// up as a large negative eyeRise before the baseline catches up; walking down a ramp does
	// not, because the baseline tracks it.
	g_tracker.eyeBaseline += (z - g_tracker.eyeBaseline) * SmoothAlpha(dt, kEyeBaselineTau);

	g_tracker.lastTime = now;
	g_tracker.lastX = x; g_tracker.lastY = y; g_tracker.lastZ = z;
	g_tracker.lastDt = dt;

	in.dt = dt;
	// Issue only what the held frames inside this tick have not already advanced, so the phase
	// stays locked to demo time across every tick boundary.
	in.phaseDt = PhaseClockStep(g_tracker.phaseClock, dt, in.wallDt, ShakeHost_Recording());
	in.vx = g_tracker.velX; in.vy = g_tracker.velY;
	in.vz = g_tracker.velZ;
	in.speed = std::sqrt(g_tracker.velX * g_tracker.velX + g_tracker.velY * g_tracker.velY);
	in.eyeRise = z - g_tracker.eyeBaseline;
	return in;
}

// The mode the stack last ran with. A test shot needs it to tell the operator whether the kick
// can be SEEN: in free cam or on a baked path the stack bails out below and publishes zero, so
// "test shot fired" was true and useless -- the spring ran and nothing was ever displayed.
std::atomic<int> g_lastMode{ (int)ShakeCamMode::Other };
// Whether the view-setup path is reaching the stack at all. If this never moves, the problem is
// upstream of every gate below it.
std::atomic<unsigned long long> g_applyCalls{ 0 };

const char* ShakeCamModeName(ShakeCamMode m) {
	switch (m) {
	case ShakeCamMode::Pov:         return "Pov";
	case ShakeCamMode::Attach:      return "Attach";
	case ShakeCamMode::FreeCam:     return "FreeCam";
	case ShakeCamMode::CameraPath:  return "CameraPath";
	default:                        return "Other";
	}
}

bool ModeAllowed(ShakeCamMode mode) {
	// CS2's own chase and free cameras are not overrides, so they arrive here as Pov. Our free
	// cam and baked paths stay excluded -- those are hand-authored moves.
	return mode == ShakeCamMode::Pov || mode == ShakeCamMode::Attach;
}

// ---- game-event fan-in ---------------------------------------------------------------------


// Recoil is the camera's answer to a GUN going off: IsGunFire (GunWeaponMath.h) drops grenade
// throws, knife swings and the other non-gun weapon_fire events.

// The same event is delivered on both the server and the client-side path, so a single shot
// would otherwise kick twice.
//
// This used to collapse anything within a 40ms WALL-CLOCK window, which silently ate real shots:
// a fast pistol (Tec-9 / full-auto Glock, ~1100 RPM) fires every ~50ms, already borderline, and
// any demo timescale above 1x compresses real spacing until whole bursts land inside the window.
// A 600 RPM rifle at 4x speed arrives every 25ms, so every other shot vanished. GetTickCount64's
// ~15.6ms granularity made the edge fuzzy on top of that.
//
// The DEMO TICK is the exact discriminator and needs no window at all: the two deliveries of one
// shot share a tick, and two genuine shots never can -- CS2 runs 64 tick (15.6ms) and the fastest
// weapon in the game still leaves ~3 ticks between rounds. Timescale cannot affect it, because
// demo ticks are demo time.
std::atomic<unsigned long long> g_lastSignature{ 0 };

// Why events did or did not reach a module. "recoil is on but nothing happens" was previously
// unanswerable without a debugger: every weapon_fire was being dropped because the spectated
// player could not be resolved, and nothing said so.
std::atomic<unsigned long long> g_evSeen{ 0 };
std::atomic<unsigned long long> g_evNoPov{ 0 };
std::atomic<unsigned long long> g_evOtherPlayer{ 0 };
std::atomic<unsigned long long> g_evNotAGun{ 0 };   // grenade / knife / utility -- no recoil
std::atomic<unsigned long long> g_evDelivered{ 0 };
std::atomic<unsigned long long> g_evDuplicate{ 0 };  // second delivery of one shot, correctly dropped
std::atomic<unsigned long long> g_evDryFire{ 0 };    // trigger pulled on an empty magazine -- no bullet left

// LAST OBSERVED MAGAZINE STATE, for the dry-fire test below. Event-thread only, so plain ints.
int g_lastFireWeaponIndex = -1;
int g_lastFireClip = -1;
// Have we ever seen this weapon report a NON-EMPTY magazine? Only then is a zero trustworthy as
// "empty" rather than "this weapon does not use m_iClip1". Without this, a weapon whose count
// always reads 0 would have every shot after its first silently dropped -- the same class of
// failure as the free-cam gate that once ate 97% of shots.
bool g_lastFireSawAmmo = false;

bool IsDuplicate(unsigned long long signature) {
	// Exact repeat of (event kind + userid + demo tick) = the second delivery of one shot.
	// Anything else is a new shot, however fast it arrived.
	if (signature == g_lastSignature.load(std::memory_order_relaxed)) return true;
	g_lastSignature.store(signature, std::memory_order_relaxed);
	return false;
}

// FL_ONGROUND and FL_DUCKING of the body under the camera, and its vertical speed. No body (an
// attach shot, a dead player) reads as such rather than as "in the air", which would silence the bob.
void SampleBody(ShakeInput& in) {
	in.haveBody = false;
	double velocity[3] = {};
	bool onGround = false, ducking = false;
	if (!ShakeHost_BodyAt(in.x, in.y, in.z, velocity, &onGround, &ducking)) return;
	in.onGround = onGround;
	in.ducking = ducking;
	in.bodyVz = velocity[2];
	in.haveBody = true;
}

} // namespace

bool CameraShake_SpectatedWeapon(std::string& out) {
	return ShakeHost_SpectatedWeapon(out);
}

bool CameraShake_ViewedPawnMotion(double x, double y, double z, double outVel[3], bool* outOnGround) {
	return ShakeHost_BodyAt(x, y, z, outVel, outOnGround, nullptr);
}

// ============================== public API =========================================

// Can a shake actually be seen right now? False in free cam / baked paths (gated by design) and
// when no first-person POV resolves, which are the two states that make the effects look broken.
bool CameraShake_ShakeVisible(const char** outWhy) {
	const ShakeCamMode mode = (ShakeCamMode)g_lastMode.load(std::memory_order_relaxed);
	if (!ModeAllowed(mode)) {
		if (outWhy) *outWhy = (mode == ShakeCamMode::FreeCam)
			? "the camera is in FREE CAM -- shake is a first-person effect and is gated off there"
			: "the camera is on a baked path / claimed by another system -- shake is gated off there";
		return false;
	}
	// Deliberately NOT gated on a spectated player. A test shot calls TestFire() directly and
	// never goes through the weapon_fire path, so it fires with no POV resolved -- and the POV
	// reading flickers to -1 while the CONFIG panel is open, which would make this warn about the
	// wrong thing on the one screen the Test Shot button lives on. Only the camera mode actually
	// decides whether Apply publishes the pose.
	if (outWhy) *outWhy = "";
	return true;
}

bool CameraShake_Active() {
	return HandheldCamRef().Enabled() || WeaponRecoilRef().Enabled()
		|| WeaponBobRef().Enabled();
}

void CameraShake_Reset() {
	g_tracker.have = false;
	HandheldCamRef().Reset();
	WeaponRecoilRef().Reset();
	WeaponBobRef().Reset();
	ShakeHost_SetWeaponKick(0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
}

bool CameraShake_Apply(
	double curTime, ShakeCamMode mode,
	float& x, float& y, float& z,
	float& pitch, float& yaw, float& roll,
	float& fov, ShakePose* appliedHandheld)
{
	if (appliedHandheld) *appliedHandheld = {};
	g_lastMode.store((int)mode, std::memory_order_relaxed);
	g_applyCalls.fetch_add(1, std::memory_order_relaxed);
	if (!CameraShake_Active()) {
		// Nothing on: keep the tracker cold so enabling mid-demo starts from a fresh sample
		// rather than integrating across however long it was off. The weapon kick is a
		// replace-every-frame contract, so a skipped frame must also publish zero.
		g_tracker.have = false;
		ShakeHost_SetWeaponKick(0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
		RecoilPivotMarker_ClearAll();
		return false;
	}

	if (!ModeAllowed(mode)) {
		// Gated. Invalidate the tracker so returning to POV/attach is a fresh sample -- a free
		// cam fly-by must never poison the velocity estimate the modules read.
		g_tracker.have = false;
		ShakeHost_SetWeaponKick(0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
		RecoilPivotMarker_ClearAll();
		return false;
	}

	ShakeInput in = Track(curTime, x, y, z, pitch, yaw, roll);
    const bool demo=ShakeHost_DemoLoaded();
    in.aimDt=WeaponBobAimInterval(in.dt,in.wallDt,demo?ShakeHost_DemoTimescale():1.0,
        ShakeHost_Recording(),demo&&ShakeHost_DemoPaused());
	if (!in.fresh) SampleBody(in);
	if (MvmDebugLog_Active()) {
		// Every frame, not throttled: the clock jitter this diagnoses is a per-frame pattern.
		MvmDebugLog_LinefAlways("camera.shake.clock",
			"demo=%.4f dt=%.4f phase=%.4f wall=%.4f lead=%.4f rate=%.3f fresh=%d",
			in.demoTime, in.dt, in.phaseDt, in.wallDt, g_tracker.phaseClock.lead,
			g_tracker.phaseClock.rate, in.fresh ? 1 : 0);
	}

	if (MvmDebugLog_Active() && WeaponBobRef().Enabled() && !in.fresh && in.dt > 0.0) {
		// Body state beside the position-derived estimate (stairs / floor-weapon step diagnosis).
		static unsigned s_bodyThrottle = 0;
		if ((s_bodyThrottle++ % 2) == 0)
			MvmDebugLog_LinefAlways("weaponbob.truth",
				"dt=%.4f derived(speed=%.1f vz=%.1f eyeRise=%.2f z=%.2f) body(have=%d ground=%d duck=%d vz=%.1f) bob(air=%.2f crouch=%.2f frac=%.2f jump=%.3f)",
				in.dt, in.speed, in.vz, in.eyeRise, z, in.haveBody ? 1 : 0, in.onGround ? 1 : 0,
				in.ducking ? 1 : 0, in.bodyVz,
				WeaponBobRef().DebugAirborne(), WeaponBobRef().DebugCrouch(), WeaponBobRef().DebugSpeedFrac(),
				WeaponBobRef().DebugJump());
	}

	ShakePose handheld = HandheldCamRef().Advance(in);
	// Weapon Recoil: the returned pose is only the CAMERA's share of the kick. The full
	// weapon transform is published to the CalcViewModelView hook below, from the same
	// advance, so gun and camera always agree on the frame.
	ShakePose weaponRecoilCam = WeaponRecoilRef().Advance(in);
	// Weapon Bob rides the same transport: recoil is an impulse response, bob is a continuous
	// function of movement, and the two are summed HERE rather than in either module so
	// ShakeHost_SetWeaponKick keeps exactly one writer per frame.
	ShakePose weaponBobCam = WeaponBobRef().Advance(in);
	{
		double wFwd = 0.0, wRight = 0.0, wUp = 0.0;
		double wPitch = 0.0, wYaw = 0.0, wRoll = 0.0;
		WeaponRecoilRef().WeaponPose(wFwd, wRight, wUp, wPitch, wYaw, wRoll);
		{
			double bFwd = 0.0, bRight = 0.0, bUp = 0.0;
			double bPitch = 0.0, bYaw = 0.0, bRoll = 0.0;
			WeaponBobRef().WeaponPose(bFwd, bRight, bUp, bPitch, bYaw, bRoll);
			wFwd += bFwd; wRight += bRight; wUp += bUp;
			wPitch += bPitch; wYaw += bYaw; wRoll += bRoll;
		}

		// The pivot orb marks the point the weapon rotates about: `pivot` units along the
		// PRE-shake view axis (x/y/z and pitch/yaw/roll are still the unmodified args here).
		// Anchoring it pre-shake is deliberate -- the camera takes its own share of the kick, so
		// a pivot fixed in this frame visibly holds still while the view moves around it, which
		// is exactly what the operator needs to see. Published unconditionally so the orb tracks
		// the slider even before the first shot.
		// ONE ORB PER EFFECT. Weapon Recoil and Weapon Bob hinge about their own points, so the
		// loop resolves each owner's store separately and publishes to that owner's drawer slot;
		// the marker module colours them apart (amber / cyan). Publishing a single merged point
		// was the bug this replaces: whichever effect you were not tuning got an orb sitting at
		// the other one's hinge, so the slider you dragged appeared to do nothing.
		//
		// Each orb is drawn where the gun is ACTUALLY hinging -- the per-weapon pivot, not
		// recoil's own forward distance. Those were the same number only for rifles, so the
		// marker used to sit ten units in front of a knife that was hinging behind the eye.
		const FollowAngles pivotBasis{ (double)pitch, (double)yaw, (double)roll };
		for (int ownerIdx = 0; ownerIdx < kWeaponPivotOwnerCount; ++ownerIdx) {
			const WeaponPivotOwner owner = (WeaponPivotOwner)ownerIdx;
			const bool ownerOn = (owner == WeaponPivotOwner::Recoil)
				? WeaponRecoilRef().Enabled() : WeaponBobRef().Enabled();
			if (!ownerOn) { RecoilPivotMarker_Clear(owner); continue; }

			const WeaponPivotStore& store = WeaponPivotRef(owner);
			const WeaponPivotPoint hinge = store.ResolveActive();
			// The same 3D hinge WeaponPose compensates about (forward + right + down), so the
			// marker sits on the weapon rather than on the view axis. Forward drives the slide
			// along the barrel: small values sit at the receiver, large ones out at the muzzle.
			//
			// The weapon's SUMMED translation is added -- the gun moves bodily by recoil plus
			// bob together, so both orbs ride that same motion. The hinge is only what the
			// ROTATION happens about; a marker that ignored the translation would separate from
			// the weapon exactly when you are trying to watch it.
			//
			// The placement is done in CAMERA-LOCAL space FIRST, because that is the only frame
			// in which the near plane is a simple forward distance. The hinge lives inside the
			// weapon, closer to the eye than the near plane -- Weapon Bob's own default is 4.1
			// forward against a plane at about 6.5 -- so drawn where it literally is, the orb was
			// sliced in half at the default and vanished completely for any smaller Forward. The
			// marker is slid out along the eye ray until it clears, and its radius scaled to
			// match, which leaves its screen position and apparent size untouched.
			const RecoilPivotDrawPoint place = RecoilPivotPlaceMarker(
				hinge.fwd + wFwd, hinge.right + wRight, hinge.up + wUp, kRecoilPivotMarkerRadius);
			// A hinge behind the eye still goes through SetWorldPoint, with radius 0 rather than
			// through Clear: the marker module keeps sampling the edit counter that way, so an
			// edit made while an undrawable weapon (a knife) is in shot does not sit queued up and
			// flash the orb on the next weapon switch.
			const FollowVec3 hingeWorld = FollowRotateVector(
				FollowVec3{ place.fwd, place.right, place.up }, pivotBasis);
			RecoilPivotMarker_SetWorldPoint(owner,
				(double)x + hingeWorld.x,
				(double)y + hingeWorld.y,
				(double)z + hingeWorld.z,
				hinge.fwd, place.visible ? place.radius : 0.0, store.EditSerial());
		}
		if (wFwd != 0.0 || wRight != 0.0 || wUp != 0.0) {
			// Camera-basis position -> world, built from the PRE-shake angles (the args are
			// still unmodified here), matching the summed-pose translation below.
			const FollowAngles basis{ (double)pitch, (double)yaw, (double)roll };
			const FollowVec3 world = FollowRotateVector(
				FollowVec3{ wFwd, wRight, wUp }, basis);
			ShakeHost_SetWeaponKick(world.x, world.y, world.z, wPitch, wYaw, wRoll);
		} else {
			ShakeHost_SetWeaponKick(0.0, 0.0, 0.0, wPitch, wYaw, wRoll);
		}
	}

	const double rawHandheldPitch = handheld.pitch;
	const double rawHandheldYaw = handheld.yaw;
	CameraCenterLockApply(HandheldCamRef().CenterLockEnabled(),
		HandheldCamRef().CenterLockStrength(), handheld.pitch, handheld.yaw);
	if (MvmDebugLog_Active()) {
		static unsigned s_centerLockLogThrottle = 0;
		if ((s_centerLockLogThrottle++ % 15) == 0) {
			MvmDebugLog_Linef("camera.center_lock.handheld",
				"enabled=%d strength=%.1f inputPitch=%.4f inputYaw=%.4f "
				"appliedPitch=%.4f appliedYaw=%.4f roll=%.4f pos=[%.4f %.4f %.4f] "
				"recoilPitch=%.4f recoilYaw=%.4f",
				HandheldCamRef().CenterLockEnabled() ? 1 : 0,
				HandheldCamRef().CenterLockStrength(), rawHandheldPitch, rawHandheldYaw,
				handheld.pitch, handheld.yaw, handheld.roll, handheld.right, handheld.up,
				handheld.fwd);
		}
	}
	if (appliedHandheld && HandheldCamRef().ViewmodelDecoupled()) *appliedHandheld = handheld;

	ShakePose pose;
	pose.Add(handheld);
	pose.Add(weaponRecoilCam);
	pose.Add(weaponBobCam);
	if (!pose.Any()) return false;

	if (pose.right != 0.0 || pose.up != 0.0 || pose.fwd != 0.0) {
		// Offsets are in the CAMERA'S basis, so a handheld wobble stays a wobble as the camera
		// turns instead of becoming a fixed world-space drift. FollowRotateVector's local frame
		// is (x = forward, y = right, z = up). Built from the PRE-shake angles (this runs before
		// the rotation is applied below): the shake's own rotation must not feed back into its
		// own translation.
		const FollowAngles basis{ (double)pitch, (double)yaw, (double)roll };
		const FollowVec3 world = FollowRotateVector(FollowVec3{ pose.fwd, pose.right, pose.up }, basis);
		x = (float)(x + world.x);
		y = (float)(y + world.y);
		z = (float)(z + world.z);
	}

	pitch = (float)(pitch + pose.pitch);
	yaw = (float)(yaw + pose.yaw);
	roll = (float)(roll + pose.roll);
	fov = (float)(fov + pose.fov);

	return true;
}

void CameraShake_OnEvent(const char* name, const std::function<bool(ShakeEventFields&)>& read) {
	if (!name) return;

	// Footsteps drive Weapon Bob's step sync -- the walk clock is pulled onto the player's real
	// cadence so the gun dips ON the footfall rather than drifting against it. Filtered to the
	// spectated player below, same as fire.
	if (0 == _stricmp(name, "player_footstep")) {
		if (WeaponBobRef().Enabled()) {
			const int spectated = ShakeHost_SpectatedUserId();
			ShakeEventFields fs;
			if (spectated >= 0 && read(fs) && fs.userId == spectated)
				WeaponBobRef().OnFootstep();
		}
		return;
	}

	const bool isFire = 0 == _stricmp(name, "weapon_fire");
	if (!isFire) return;

	// Runtime diagnosis must still see ordinary engine recoil while every optional shake layer
	// is disabled. Otherwise an effects-off reproduction contains no marker telling us which
	// motion-field samples coincide with a real shot.
	if (MvmDebugLog_Active()) {
		ShakeEventFields debugEvent;
		const bool parsed = read(debugEvent);
		MvmDebugLog_Linef("shake.fire",
			// One specifier per argument. This carried a stale `recoil=%d` from the removed
			// RecoilShake module, so `clip=%d` read PAST the varargs and printed stack garbage
			// (measured: clip=-751760480) -- the one field a dry-fire investigation depends on.
			"parsed=%d userid=%d watched=%d weapon=%s camshake=%d weaponrecoil=%d clip=%d",
			parsed ? 1 : 0, parsed ? debugEvent.userId : -1, ShakeHost_SpectatedUserId(),
			parsed ? debugEvent.weapon.c_str() : "", CameraShake_Active() ? 1 : 0,
			WeaponRecoilRef().Enabled() ? 1 : 0,
			ShakeHost_SpectatedClip(nullptr));
	}

	if (!CameraShake_Active()) return;

	// Cheap gate first: no consumer is on, so do not pay for the KV3->JSON conversion.
	if (!WeaponRecoilRef().Enabled()) return;

	g_evSeen.fetch_add(1, std::memory_order_relaxed);

	const int watched = ShakeHost_SpectatedUserId();
	if (watched < 0) {
		// No resolvable POV: these effects are first-person by definition, so there is nothing
		// to attach them to. Counted so "camshake state" can say this instead of looking dead.
		g_evNoPov.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	ShakeEventFields fe;
	if (!read(fe)) return;
	if (fe.userId != watched) {
		g_evOtherPlayer.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	// Grenades / knives / utility raise weapon_fire too; they must not kick the camera.
	if (!IsGunFire(fe.weapon)) {
		g_evNotAGun.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	const int uid = fe.userId;

	// DID A BULLET ACTUALLY LEAVE THE GUN?
	//
	// CS2 raises weapon_fire off the trigger, so a dry click on an empty magazine raises it
	// exactly like a real shot -- which is why the camera kicked on the clicks after a fast
	// pistol burst ran the mag dry, and on every click of an already-empty gun.
	//
	// The magazine count alone cannot separate the two: MEASURED, the count read at event time
	// is the same for the last real bullet and for the dry click after it. What DOES separate
	// them is that a real shot always CHANGES the count, so two consecutive fires can never both
	// observe an empty magazine. So the rejection is deliberately narrow: only when this fire and
	// the previous one on the SAME weapon both saw an empty magazine.
	//
	// That cannot swallow a real shot -- not the last round in the mag (the fire before it saw a
	// non-empty magazine), and not the first round after a reload (the count is no longer zero).
	// It is a state test, not a time window: no cooldown, no debounce, and nothing that could
	// drop a genuinely fast burst. The shake envelope is untouched, so an accepted shot still
	// plays its full animation.
	//
	// clip < 0 means "cannot read" (optional schema field, or a weapon with no magazine such as
	// a knife) and always allows the shot, so a CS2 rename degrades to the old behaviour rather
	// than silently killing every kick.
	{
		int weaponIndex = -1;
		const int clipNow = ShakeHost_SpectatedClip(&weaponIndex);
		const bool sameWeapon = (weaponIndex >= 0 && weaponIndex == g_lastFireWeaponIndex);
		// All four must hold, and the last one is what keeps this from ever eating a real shot:
		// we only trust a zero from a weapon we have already watched count DOWN.
		const bool dryFire = (clipNow == 0) && sameWeapon && (g_lastFireClip == 0) && g_lastFireSawAmmo;
		if (!sameWeapon) g_lastFireSawAmmo = false; // new gun: learn its counting behaviour afresh
		if (clipNow > 0) g_lastFireSawAmmo = true;
		g_lastFireWeaponIndex = weaponIndex;
		g_lastFireClip = clipNow;
		if (dryFire) {
			g_evDryFire.fetch_add(1, std::memory_order_relaxed);
			return;
		}
	}

	// userid in the low 32 bits, demo tick above it. A tick of -1 (tick unavailable) still works:
	// it simply means consecutive shots in that state dedupe on userid alone, which is the old
	// behaviour and only reachable outside demo playback.
	const int fireTick = ShakeHost_DemoTick();
	const unsigned long long signature =
		((unsigned long long)(unsigned int)fireTick << 32) | (unsigned long long)(unsigned int)uid;
	if (IsDuplicate(signature)) {
		g_evDuplicate.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	g_evDelivered.fetch_add(1, std::memory_order_relaxed);

	// WHICH GUN, for the recoil scope resolver.
	//
	// The event's own token is the primary source, but it is optional in the payload and can also
	// name a gun the recoil table has never heard of. In both cases the resolver can derive no
	// slot and no type from it, so a Slot or Type override matches nothing and the shot silently
	// falls back to the All tuning -- the exact symptom of "the scopes do not gate". So when the
	// token is unrecognisable, ask the weapon entity the player is actually holding, and use that
	// instead when IT is recognisable. A knife or grenade in hand is not in the table either, so
	// this can only ever substitute a real gun.
	std::string weaponToken = fe.weapon;
	if (!RecoilFindWeapon(weaponToken)) {
		std::string live;
		if (ShakeHost_SpectatedWeapon(live) && RecoilFindWeapon(live))
			weaponToken = live;
	}
	if (WeaponRecoilRef().Enabled()) WeaponRecoilRef().OnFire(weaponToken.c_str());
}

void CameraShake_RunFrame() {
	// Demo closed / swapped: drop everything so a new demo never inherits live springs.
	static bool s_wasLoaded = false;
	const bool loaded = ShakeHost_DemoLoaded();
	if (s_wasLoaded && !loaded) CameraShake_Reset();
	s_wasLoaded = loaded;

	// WHICH WEAPON IS IN SHOT -- published to BOTH pivot stores, here, every frame.
	//
	// This is a property of the GAME (what the spectated player is holding), not of either
	// effect, so it cannot live inside one effect's Advance. It used to: WeaponBob::Advance
	// published it, and only to the BOB store. Three ways that went stale, all of them visible
	// in the panel as a pivot strip pointing at the wrong weapon:
	//
	//   * the RECOIL store was never told at all, so it sat on its Rifle default forever;
	//   * Advance returns early when Weapon Bob is switched off, so nothing updated;
	//   * Advance also returns early on `in.fresh` -- which is set by a SPECTATOR SWITCH, i.e.
	//     precisely the moment the weapon in shot changes.
	//
	// RunFrame is main-thread housekeeping that runs unconditionally, so the strip now follows a
	// weapon or player switch on the very next frame. A failed read leaves the last good value
	// standing rather than writing a guess: the spectated POV flickers while the CONFIG panel is
	// open, and blinking the strip is worse than holding it.
	//
	// GATED ON A MAP WITH A USABLE ENTITY SYSTEM. The read walks the entity system (spectated
	// pawn -> active weapon handle -> class name), so it must not run during level load or in
	// the main menu -- that is not a state it is defined for.
	//
	// The gate used to be ShakeHost_DemoLoaded() alone, and that was too narrow: the offline live-match
	// harness (mvm_test, the Insert panel) is a loaded map with a real local pawn and no demo, so
	// nothing was ever published there and BOTH pivot strips froze on whatever class they last
	// resolved. Reported as "in demos the Config tab reads the weapons fine, but in the Insert
	// tab the pivot doesn't update when you switch weapons" -- and it would have been equally
	// stuck for the hinge the effects actually use, not just the panel readout.
	//
	// Same predicate shape as ParticleFx's FxMainThreadContextActive(), deliberately rewritten
	// here rather than shared: they are unrelated features that happen to need the same question
	// answered, and the read below is the thing that defines what "usable" means for this one.
	if (ShakeHost_EntitiesLive()) {
		std::string live;
		if (CameraShake_SpectatedWeapon(live) && !live.empty()) {
			const WeaponPivotClass c = WeaponPivotClassOf(live);
			WeaponPivotRecoilRef().NoteActive(c, live);
			WeaponPivotBobRef().NoteActive(c, live);
		}
	}
}

void CameraShake_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* action = (argc >= 3) ? args->ArgV(2) : "";

	if (0 == _stricmp(action, "state")) {
		std::ostringstream o;
		o << "{\"active\":" << (CameraShake_Active() ? "true" : "false")
			<< ",\"tracking\":" << (g_tracker.have ? "true" : "false")
			<< ",\"speed\":" << (float)std::sqrt(g_tracker.velX * g_tracker.velX + g_tracker.velY * g_tracker.velY)
			<< ",\"vz\":" << (float)g_tracker.velZ
			<< ",\"lastDt\":" << (float)g_tracker.lastDt
			<< ",\"watchedUserId\":" << ShakeHost_SpectatedUserId()
			<< ",\"camMode\":\"" << ShakeCamModeName((ShakeCamMode)g_lastMode.load(std::memory_order_relaxed)) << "\""
			<< ",\"modeAllowed\":" << (ModeAllowed((ShakeCamMode)g_lastMode.load(std::memory_order_relaxed)) ? "true" : "false")
			<< ",\"applyCalls\":" << g_applyCalls.load(std::memory_order_relaxed)
			<< ",\"evSeen\":" << g_evSeen.load(std::memory_order_relaxed)
			<< ",\"evNoPov\":" << g_evNoPov.load(std::memory_order_relaxed)
			<< ",\"evNotAGun\":" << g_evNotAGun.load(std::memory_order_relaxed)
			<< ",\"evOtherPlayer\":" << g_evOtherPlayer.load(std::memory_order_relaxed)
			<< ",\"evDelivered\":" << g_evDelivered.load(std::memory_order_relaxed)
			<< ",\"evDuplicate\":" << g_evDuplicate.load(std::memory_order_relaxed)
			<< ",\"evDryFire\":" << g_evDryFire.load(std::memory_order_relaxed) << "}";
		advancedfx::Message("[camshake][state] %s\n", o.str().c_str());
		if (ShakeHost_SpectatedUserId() < 0)
			advancedfx::Warning("[camshake] no spectated player: recoil cannot fire. Spectate someone (it is a first-person effect).\n");
		advancedfx::Message("[camshake][handheld] %s\n", HandheldCamRef().DebugStateJson().c_str());
		advancedfx::Message("[camshake][weaponrecoil] %s\n", WeaponRecoilRef().DebugStateJson().c_str());
		return;
	}

	if (0 == _stricmp(action, "reset")) {
		CameraShake_Reset();
		advancedfx::Message("mirv_filmmaker: camera shake state reset.\n");
		return;
	}

	if (0 == _stricmp(action, "off")) {
		HandheldCamRef().SetEnabled(false);
		WeaponRecoilRef().SetEnabled(false);
		advancedfx::Message("mirv_filmmaker: all camera shake layers off.\n");
		return;
	}

	advancedfx::Message(
		"%s camshake state - print the shared tracker + every layer's diagnostics.\n"
		"%s camshake reset - drop all integrated shake state.\n"
		"%s camshake off - turn off handheld and recoil at once.\n"
		"  Per-layer settings live under \"%s handheld\" and \"%s recoil\".\n",
		cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker
