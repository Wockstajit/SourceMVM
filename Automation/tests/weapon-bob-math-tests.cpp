#include "../../AfxHookSource2/Filmmaker/Movie/WeaponBobMath.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace Filmmaker;

namespace {
const WeaponBobClassProfile kRifle = WeaponBobProfileFor(WeaponBobClass::Rifle);
// The hinge is shared state now, so the tests name the two measured points directly.
const WeaponPivotPoint kRifleHinge = WeaponPivotDefaultFor(WeaponPivotClass::Rifle);
const WeaponPivotPoint kKnifeHinge = WeaponPivotDefaultFor(WeaponPivotClass::Knife);
// Multipliers neutralised. The walk-vs-sprint SHAPE assertions are about ARC9's figure, and
// running them through a profile whose sprintMult is deliberately 0.55 would test the class
// table instead -- which is what the per-class block below is for.
const WeaponBobClassProfile kNeutral = [] {
	WeaponBobClassProfile p = WeaponBobProfileFor(WeaponBobClass::Rifle);
	p.walkMult = 1.0; p.sprintMult = 1.0; return p;
}();
const WeaponBobClassProfile kKnife = WeaponBobProfileFor(WeaponBobClass::Knife);

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << "\n";
		std::exit(1);
	}
}
bool Near(double a, double b, double epsilon = 1e-9) { return std::fabs(a - b) <= epsilon; }
double Mag(const WeaponBobOffset& o) { return std::sqrt(o.right * o.right + o.up * o.up + o.fwd * o.fwd); }
double RotMag(const WeaponBobOffset& o) { return std::sqrt(o.pitch * o.pitch + o.yaw * o.yaw + o.roll * o.roll); }

// Peak over one full cycle so a single phase cannot land on a node and understate a channel.
double PeakPos(double speed, double sprint, double sway = 100, double tilt = 100, double amount = 100) {
	double peak = 0.0;
	for (double ph = 0.0; ph < 6.3; ph += 0.01)
		peak = std::fmax(peak, Mag(WeaponBobEvaluate(ph, ph * 9.25, speed, sprint, 0, 0, 0, 0, 0, sway, tilt, amount, kNeutral)));
	return peak;
}
double PeakRot(double speed, double sprint, double sway = 100, double tilt = 100, double amount = 100) {
	double peak = 0.0;
	for (double ph = 0.0; ph < 6.3; ph += 0.01)
		peak = std::fmax(peak, RotMag(WeaponBobEvaluate(ph, ph * 9.25, speed, sprint, 0, 0, 0, 0, 0, sway, tilt, amount, kNeutral)));
	return peak;
}
} // namespace

int main() {
	{
		WeaponBobAimState aim;
		double p = 0, y = 0;
		WeaponBobAimAdvance(aim, 0.0163, 10, 179, p, y);
		Check(p == 0 && y == 0, "first aim sample does not kick the gun");
		// Actual Inferno timing: raw dt=0.0004, interpolated playback dt=0.0163.
		WeaponBobAimAdvance(aim, 0.0163, 11, -179, p, y);
		Check(Near(p, 1.0 / 0.0163) && Near(y, 2.0 / 0.0163),
			"interpolated turn uses its render interval and wraps yaw through 180");
		WeaponBobSwayState sway;
		WeaponBobSwayAdvance(sway, 0.0163, p, y);
		Check(std::fabs(sway.pitch) < 0.3, "clock jitter cannot manufacture a giant sway impulse");
		// A render frame whose raw demo tick was held must still supply its own aim delta.
		WeaponBobAimAdvance(aim, 0.01, 11.6, -178, p, y);
		Check(Near(p, 60) && Near(y, 100), "same-tick aim is sampled instead of accumulated");
		WeaponBobAimAdvance(aim, 0, 20, -170, p, y);
		Check(p == 0 && y == 0, "paused aim produces no velocity");
		WeaponBobAimAdvance(aim, 0.01, 20, -170, p, y);
		Check(p == 0 && y == 0, "resuming cannot turn a paused aim change into a kick");
		Check(WeaponBobAimAdvance(aim, 0.01, 80, -170, p, y) && p == 0,
			"spectator cuts reset sway instead of producing angular velocity");
		Check(WeaponBobAimAdvance(aim, 0.01, std::nan(""), 0, p, y) && !aim.have,
			"invalid aim drops the baseline and cannot poison sway");
	}
	{
		// Equal steady turns produce the same carried-gun pose at capture rates from 60 to 240.
		double reference = 0;
		for (int fps : {60, 100, 144, 240}) {
			WeaponBobAimState aim; WeaponBobSwayState sway; double p = 0, y = 0;
			WeaponBobAimAdvance(aim, 1.0 / fps, 0, 0, p, y);
			for (int i = 1; i <= fps; ++i) {
				WeaponBobAimAdvance(aim, 1.0 / fps, 90.0 * i / fps, 0, p, y);
				WeaponBobSwayAdvance(sway, 1.0 / fps, p, y);
			}
			const auto pose = WeaponBobSwayEvaluate(sway, 150, 150, 0, kRifleHinge);
			if (fps == 60) reference = pose.pitch;
			Check(Near(pose.pitch, reference, 1e-8), "steady aim sway is independent of capture rate");
		}
	}
    {
        WeaponBobAimState aim; double p=0,y=0;
        WeaponBobAimAdvance(aim,.01,0,0,p,y);
        // Identical rendered turns, but the raw clock alternates holds, tiny
        // advances and catch-up. None may change the measured turn velocity.
        const double raw[] = {0,.0004,.0196,0,.016};
        for(int i=0;i<5;++i){
            const double sample=WeaponBobAimInterval(raw[i],.01,1,false,false);
            WeaponBobAimAdvance(aim,sample,0,(i+1)*.9,p,y);
            Check(Near(y,90),"Look sway ignores demo phase-clock corrections during steady turns");
        }
        Check(Near(WeaponBobAimInterval(0,.02,.25,false,false),.005),"Slow playback measures aim in demo seconds");
        Check(WeaponBobAimInterval(.02,.01,1,false,true)==0,"Explicit pause freezes aim without waiting for oscillator stall detection");
        Check(Near(WeaponBobAimInterval(1./60,.001,4,true,true),1./60),"Export uses exact demo intervals even when stepping paused frames");
        Check(WeaponBobAimInterval(.01,std::nan(""),1,false,false)==0,"Invalid render timing cannot poison aim velocity");
    }
    {
        // A demo turn at 90 deg/s rendered at 144 fps: the angle moves once per 64 tps tick, so
        // most frames repeat it. Every frame must read the steady turn, not zero / spike.
        WeaponBobAimState aim; double p = 0, y = 0;
        const double frame = 1.0 / 144.0, tick = 1.0 / 64.0;
        WeaponBobAimAdvance(aim, frame, 0, 0, p, y);
        double minY = 1e9, maxY = -1e9;
        for (int i = 1; i <= 288; ++i) {
            const double t = i * frame;
            const double angle = 90.0 * std::floor(t / tick) * tick; // the angle of the last tick
            WeaponBobAimAdvance(aim, frame, 0, angle, p, y);
            if (t > 0.1) { minY = (std::min)(minY, y); maxY = (std::max)(maxY, y); }
        }
        Check(minY > 60 && maxY < 120, "a tick-stepped steady turn reads steady, never zero or a spike");
        // The player stops: past the hold the turn reads zero.
        for (int i = 0; i < 12; ++i) WeaponBobAimAdvance(aim, frame, 0, 90.0 * std::floor(2.0 / tick) * tick, p, y);
        Check(y == 0 && p == 0, "a turn that stopped reads zero once the hold runs out");
    }
	// ============================== walk bob ==============================================
	{
		Check(!WeaponBobEvaluate(1.0, 1.0 * 9.25, 1.0, 0, 0, 0, 0, 0, 0, 100, 100, 0, kRifle).Any(),
			"amount 0 produces no bob at all");
		auto still = WeaponBobEvaluate(1.0, 1.0 * 9.25, 0.0, 0, 0, 0, 0, 0, 0, 100, 100, 100, kRifle);
		Check(Near(Mag(still), 0.0) && Near(RotMag(still), 0.0),
			"a stationary player's weapon is perfectly still");
	}
	{
		double prev = -1.0;
		for (double d = 0.0; d <= 1.0001; d += 0.25) {
			const double peak = PeakPos(d, 0.0);
			Check(peak >= prev, "bob amplitude never decreases as speed rises");
			prev = peak;
		}
		// ARC9 style 0 is LINEAR in speed (mag = d * 2). v1's quartic easing is gone with it.
		Check(PeakPos(0.5, 0.0) > PeakPos(1.0, 0.0) * 0.40, "walk bob scales roughly linearly with speed");
	}
	{
		// The sprint branch is the bigger figure -- this is the MW run feel.
		Check(PeakPos(1.0, 1.0) > PeakPos(1.0, 0.0) * 1.5, "sprint branch travels much further than walk");
		Check(PeakRot(1.0, 1.0) > PeakRot(1.0, 0.0), "sprint branch rotates more than walk");
		// Sprint adds a yaw channel the walk branch does not have.
		double walkYaw = 0.0, sprintYaw = 0.0;
		for (double ph = 0.0; ph < 6.3; ph += 0.01) {
			walkYaw = std::fmax(walkYaw, std::fabs(WeaponBobEvaluate(ph, ph * 9.25, 1, 0, 0, 0, 0, 0, 0, 100, 100, 100, kNeutral).yaw));
			sprintYaw = std::fmax(sprintYaw, std::fabs(WeaponBobEvaluate(ph, ph * 9.25, 1, 1, 0, 0, 0, 0, 0, 100, 100, 100, kNeutral).yaw));
		}
		Check(Near(walkYaw, 0.0) && sprintYaw > 1.0, "only the sprint branch yaws the weapon");
	}
	{
		auto noTilt = WeaponBobEvaluate(1.0, 1.0 * 9.25, 1.0, 0, 0, 0, 0, 0, 0, 100.0, 0.0, 100.0, kRifle);
		Check(Mag(noTilt) > 0.0 && Near(RotMag(noTilt), 0.0), "tilt 0 removes rotation, keeps position");
		auto noSway = WeaponBobEvaluate(1.0, 1.0 * 9.25, 1.0, 0, 0, 0, 0, 0, 0, 0.0, 100.0, 100.0, kRifle);
		Check(Near(Mag(noSway), 0.0) && RotMag(noSway) > 0.0, "sway 0 removes position, keeps rotation");
	}
	{
		// Crouch: shorter stride (step 9.25 -> 6) and a much harder vertical.
		double standUp = 0.0, crouchUp = 0.0;
		for (double ph = 0.0; ph < 6.3; ph += 0.01) {
			standUp = std::fmax(standUp, std::fabs(WeaponBobEvaluate(ph, ph * 9.25, 1, 0, 0, 0.0, 0, 0, 0, 100, 100, 100, kRifle).up));
			crouchUp = std::fmax(crouchUp, std::fabs(WeaponBobEvaluate(ph, ph * 9.25, 1, 0, 0, 1.0, 0, 0, 0, 100, 100, 100, kRifle).up));
		}
		Check(crouchUp > standUp * 2.0, "crouch-walking rocks the weapon vertically much harder");
	}
	{
		// Strafe (ARC9 sidemove) feeds roll directly and survives at zero speed: a pose, not a cycle.
		auto left = WeaponBobEvaluate(1.0, 1.0 * 9.25, 0.0, 0, 0, 0, 0, -3.0, 0, 100, 100, 100, kRifle);
		auto right = WeaponBobEvaluate(1.0, 1.0 * 9.25, 0.0, 0, 0, 0, 0, 3.0, 0, 100, 100, 100, kRifle);
		Check(left.roll < 0.0 && right.roll > 0.0 && Near(left.roll, -right.roll),
			"strafing rolls the weapon, symmetrically");
	}
	{
		// Jump/land: landing dominates.
		Check(WeaponBobJumpMove(-150.0) > 0.0, "falling produces a positive jumpmove (the dip)");
		Check(WeaponBobJumpMove(350.0) < 0.0, "rising produces a negative jumpmove");
		Check(std::fabs(WeaponBobJumpMove(-150.0)) < std::fabs(WeaponBobJumpMove(350.0)),
			"ARC9's rise term is the larger raw value; the caller's clamp is what makes landing win");
		auto land = WeaponBobEvaluate(1.0, 1.0 * 9.25, 0.0, 0, 0, 0, 0, 0, -0.3, 100, 100, 100, kRifle);
		Check(land.pitch < 0.0 && land.up > 0.0, "a landing pitches the muzzle down and lifts the gun");
	}
	{
		Check(Near(WeaponBobAdvancePhase(5.0, 0.0, 1.0, 0.0), 5.0), "zero dt holds the phase");
		Check(Near(WeaponBobAdvancePhase(5.0, -1.0, 1.0, 0.0), 5.0), "negative dt is rejected");
		Check(WeaponBobAdvancePhase(0, 1, 1, 0) > WeaponBobAdvancePhase(0, 1, 0, 0), "faster = quicker cadence");
		const double air = WeaponBobAdvancePhase(0, 1, 1, 1);
		Check(air < WeaponBobAdvancePhase(0, 1, 1, 0) && air > 0.0, "airborne slows the clock but never stops it");
		double whole = WeaponBobAdvancePhase(0.0, 0.5, 0.7, 0.0), split = 0.0;
		for (int i = 0; i < 50; ++i) split = WeaponBobAdvancePhase(split, 0.01, 0.7, 0.0);
		Check(Near(whole, split, 1e-9), "phase advance is frame-rate independent");
		Check(std::isfinite(WeaponBobAdvancePhase(0.0, 1.0, std::nan(""), std::nan(""))),
			"a NaN speed cannot poison the phase accumulator");
	}

	// ============================== look sway =============================================
	{
		WeaponBobSwayState st;
		auto zero = WeaponBobSwayEvaluate(st, 100, 100, 0, kRifleHinge);
		Check(!zero.Any(), "no aim motion = no look sway");

		// Turn LEFT (+yaw in engine convention) at a steady rate: the gun must lag -> rotate RIGHT
		// (negative yaw) and shift toward the right side of the screen.
		for (int i = 0; i < 60; ++i) WeaponBobSwayAdvance(st, 1.0 / 100.0, 0.0, 180.0);
		auto lag = WeaponBobSwayEvaluate(st, 100, 0, 0, kRifleHinge);
		Check(lag.yaw < 0.0, "turning left makes the gun lag by rotating right");
		Check(lag.right < 0.0 || lag.fwd != 0.0, "the lag carries a positional component from the hinge");

		// Pitch: looking DOWN (+pitch) -> gun lags by pitching UP (negative).
		WeaponBobSwayState sp;
		for (int i = 0; i < 60; ++i) WeaponBobSwayAdvance(sp, 1.0 / 100.0, 180.0, 0.0);
		auto lagP = WeaponBobSwayEvaluate(sp, 100, 0, 0, kRifleHinge);
		Check(lagP.pitch < 0.0, "looking down makes the gun lag by pitching up");
	}
	{
		// Clamps: a violent flick saturates rather than sending the gun off screen.
		WeaponBobSwayState st;
		for (int i = 0; i < 200; ++i) WeaponBobSwayAdvance(st, 1.0 / 100.0, 0.0, 3600.0);
		auto sat = WeaponBobSwayEvaluate(st, 100, 0, 0, kRifleHinge);
		// yaw clamp is 4 deg * 0.9 pre-clamp -> 4, times sightmult 1.5 -> 6.
		Check(std::fabs(sat.yaw) <= 6.0 + 1e-6, "yaw sway saturates at the ARC9 clamp");
		Check(std::isfinite(sat.fwd) && std::isfinite(sat.right) && std::isfinite(sat.up),
			"saturated sway keeps a finite hinge compensation");
	}
	{
		// Determinism: the same turn at two frame rates lands the same smoothed state.
		WeaponBobSwayState a, b;
		for (int i = 0; i < 100; ++i) WeaponBobSwayAdvance(a, 0.01, 90.0, 120.0);
		for (int i = 0; i < 400; ++i) WeaponBobSwayAdvance(b, 0.0025, 90.0, 120.0);
		auto ra = WeaponBobSwayEvaluate(a, 100, 100, 0, kRifleHinge), rb = WeaponBobSwayEvaluate(b, 100, 100, 0, kRifleHinge);
		Check(Near(ra.yaw, rb.yaw, 1e-3) && Near(ra.pitch, rb.pitch, 1e-3),
			"look sway is frame-rate independent");
	}
	{
		// Inertia is the slower term: after a short turn it still holds when the fast sway has
		// mostly decayed. Feed a turn, then stop, and compare the two knobs in isolation.
		WeaponBobSwayState st;
		for (int i = 0; i < 30; ++i) WeaponBobSwayAdvance(st, 0.01, 0.0, 400.0);
		for (int i = 0; i < 40; ++i) WeaponBobSwayAdvance(st, 0.01, 0.0, 0.0);
		auto fast = WeaponBobSwayEvaluate(st, 100, 0, 0, kRifleHinge);
		auto slow = WeaponBobSwayEvaluate(st, 0, 100, 0, kRifleHinge);
		Check(std::fabs(slow.yaw) > std::fabs(fast.yaw), "inertia outlasts the fast sway after a turn stops");
	}
	{
		// Hinge compensation: zero rotation = zero correction; a pure yaw moves the gun sideways.
		double f = 0, r = 0, u = 0;
		WeaponBobHingeCompensate(0.0, 0.0, kRifleHinge, f, r, u);
		Check(Near(f, 0) && Near(r, 0) && Near(u, 0), "no rotation = no hinge correction");
		WeaponBobHingeCompensate(0.0, 10.0, kRifleHinge, f, r, u);
		Check(r != 0.0 && Near(u, 0.0, 1e-9), "a pure yaw about the hinge corrects sideways, not vertically");
	}
	{
		// NaN hygiene through the sway path.
		WeaponBobSwayState st;
		WeaponBobSwayAdvance(st, 0.01, std::nan(""), std::nan(""));
		auto o = WeaponBobSwayEvaluate(st, 100, 100, 0, kRifleHinge);
		Check(std::isfinite(o.yaw) && std::isfinite(o.pitch) && std::isfinite(o.right),
			"a NaN aim velocity resolves to a finite sway");
	}

	// ============================== idle breath ===========================================
	{
		Check(!WeaponBobIdleEvaluate(3.0, 0, 0).Any(), "idle 0 produces nothing");
		auto a = WeaponBobIdleEvaluate(1.0, 0, 100), b = WeaponBobIdleEvaluate(2.0, 0, 100);
		Check(a.Any() && (a.roll != b.roll), "idle breath drifts over time");
		Check(Mag(a) < 0.1 && RotMag(a) < 0.5, "idle breath is small by design");
		auto ads = WeaponBobIdleEvaluate(1.0, 1.0, 100);
		Check(std::fabs(ads.roll) < std::fabs(a.roll), "aiming damps idle breath");
	}

	// ============================== footstep sync =========================================
	{
		Check(Near(WeaponBobSyncToFootstep(1.234, 0.0), 1.234), "strength 0 leaves the carrier untouched");
		const double period = 2.0 * kWeaponBobPi;
		const double boundary = period * 0.25;          // where -sin(stepArg) bottoms out
		const double offBy = boundary + period * 0.4;
		const double pulled = WeaponBobSyncToFootstep(offBy, 1.0);
		Check(std::fabs(pulled - boundary) < std::fabs(offBy - boundary), "a footstep pulls the carrier toward the footfall");
		Check(std::fabs(pulled - boundary) > std::fabs(offBy - boundary) * 0.5, "the pull is partial, never a snap");
		Check(Near(WeaponBobSyncToFootstep(boundary, 1.0), boundary), "a carrier already on the footfall is left alone");
	}

	// ============================== crouch continuity (the live jitter) ===================
	{
		// Reproduce the reported bug: a smoothed crouch sliding the stride frequency 9.25 -> 6 over
		// ~0.3 s while the accumulators are large. With the old sin(phase * step) form the carrier
		// argument swept ~300 radians and the gun thrashed. The carrier accumulator must keep every
		// consecutive vertical sample within what one frame of motion can produce.
		double phase = 120.0, arg = 120.0 * 9.25; // a couple of minutes into a demo
		const double dt = 1.0 / 64.0;
		double prevUp = 0.0; bool have = false; double maxJump = 0.0;
		for (int i = 0; i < 60; ++i) {
			const double crouch = WeaponBobClamp(i / 20.0, 0.0, 1.0); // ramps 0 -> 1 over 20 frames
			phase = WeaponBobAdvancePhase(phase, dt, 1.0, 0.0);
			arg = WeaponBobAdvanceStepArg(arg, dt, 1.0, 0.0, crouch);
			const double up = WeaponBobEvaluate(phase, arg, 1.0, 0.0, 0.0, crouch, 0, 0, 0, 100, 100, 100, kRifle).up;
			if (have) maxJump = std::fmax(maxJump, std::fabs(up - prevUp));
			prevUp = up; have = true;
		}
		// The carrier advances at most ~2.75 * 9.25 * dt = 0.4 rad per frame at full speed, and the
		// vertical amplitude tops out near 0.1 * 1.7 * 2 * 3.2 = ~1.1 units crouched, so a single
		// frame can legitimately move it ~0.4 units. Anything past that is a discontinuity.
		Check(maxJump < 0.45, "crouching never jumps the vertical channel between frames");
		Check(std::isfinite(WeaponBobAdvanceStepArg(1.0, 0.01, std::nan(""), std::nan(""), std::nan(""))),
			"a NaN input cannot poison the carrier accumulator");
	}


	// ============================== per-weapon-class profiles =============================
	{
		// The knife hinge must sit BEHIND the eye and the rifle's in front -- that sign flip is
		// the whole fix for "the knife jumps around a lot", so assert the sign, not just a value.
		Check(kRifleHinge.fwd > 0.0, "the rifle hinges in front of the camera");
		Check(kKnifeHinge.fwd < 0.0, "the knife hinges BEHIND the camera");
		Check(kKnife.walkMult < kRifle.walkMult, "the knife bobs less than a rifle");
		for (int i = 0; i < 7; ++i) {
			const WeaponBobClassProfile pr = WeaponBobProfileFor((WeaponBobClass)i);
			Check(pr.walkMult > 0.0, "no class is silently switched off");
			// CS2 has no tactical sprint, so a run must NOT be suppressed the way MW2019
			// suppresses its sprint. An earlier table shipped sprintMult 0.55 on rifles and the
			// live result was "barely moving even with maxed out settings" -- p95 travel 1.76u
			// -> 0.87u with the sprint blend active ~70% of the time. Guard that regression.
			Check(pr.sprintMult >= pr.walkMult * 0.99,
				"a CS2 run is not a tactical sprint and must not be scaled down");
		}
		// A rifle at default must reach the FULL figure: the class table may retune other weapons,
		// but it must never quietly cap the reference weapon below the shape it was built from.
		double sprintFigure = 0.0, sprintReal = 0.0;
		for (double ph = 0.0; ph < 6.3; ph += 0.01) {
			sprintFigure = std::fmax(sprintFigure,
				Mag(WeaponBobEvaluate(ph, ph * 9.25, 1, 1, 0, 0, 0, 0, 0, 100, 100, 100, kNeutral)));
			sprintReal = std::fmax(sprintReal,
				Mag(WeaponBobEvaluate(ph, ph * 9.25, 1, 1, 0, 0, 0, 0, 0, 100, 100, 100, kRifle)));
		}
		Check(Near(sprintReal, sprintFigure, 1e-9), "a default rifle runs at the full figure");
		// The quieter classes still have to be quieter -- that is the pistol/SMG/knife fix.
		const WeaponBobClassProfile pistol = WeaponBobProfileFor(WeaponBobClass::Pistol);
		const WeaponBobClassProfile smg = WeaponBobProfileFor(WeaponBobClass::Smg);
		Check(pistol.walkMult < kRifle.walkMult && smg.walkMult < kRifle.walkMult,
			"pistols and SMGs stay calmer than a rifle");
		Check(kKnife.walkMult < pistol.walkMult, "the knife is the calmest of all");
	}
	{
		// The same rotation about two different hinges must produce DIFFERENT translation, or the
		// per-class hinge is not actually reaching the maths.
		double rf = 0, rr = 0, ru = 0, kf = 0, kr = 0, ku = 0;
		WeaponBobHingeCompensate(0.0, 8.0, kRifleHinge, rf, rr, ru);
		WeaponBobHingeCompensate(0.0, 8.0, kKnifeHinge, kf, kr, ku);
		Check(std::fabs(rr - kr) > 1e-6, "the hinge choice changes the compensation");
		// And they must push OPPOSITE ways, because the hinges are on opposite sides of the eye.
		Check(rr * kr < 0.0, "hinges in front of and behind the eye compensate in opposite directions");
	}
	{
		// A knife's look sway must move the model less than a rifle's for the same turn.
		WeaponBobSwayState st;
		for (int i = 0; i < 60; ++i) WeaponBobSwayAdvance(st, 1.0 / 100.0, 0.0, 240.0);
		auto r = WeaponBobSwayEvaluate(st, 100, 100, 0, kRifleHinge);
		auto k = WeaponBobSwayEvaluate(st, 100, 100, 0, kKnifeHinge);
		Check(Near(r.yaw, k.yaw, 1e-9), "the rotation itself is unchanged; only the hinge differs");

		// Compare the HINGE displacement, not the whole offset: the position-lag terms are
		// hinge-independent and identical for both, so including them dilutes the very thing
		// under test (and can flip its sign). What the player sees swing is the model, which
		// sits at the hinge, so the hinge travel through a given rotation IS "how much it jumps".
		double rf = 0, rr = 0, ru = 0, kf = 0, kr = 0, ku = 0;
		WeaponBobHingeCompensate(r.pitch, r.yaw, kRifleHinge, rf, rr, ru);
		WeaponBobHingeCompensate(k.pitch, k.yaw, kKnifeHinge, kf, kr, ku);
		const double rMag = std::sqrt(rf * rf + rr * rr + ru * ru);
		const double kMag = std::sqrt(kf * kf + kr * kr + ku * ku);
		Check(kMag < rMag, "a knife's hinge travels less than a rifle's through the same turn");
	}
	{
		// Class multipliers actually scale the figure.
		WeaponBobClassProfile quiet = kRifle; quiet.walkMult = 0.0;
		auto off = WeaponBobEvaluate(1.0, 1.0 * 9.25, 1.0, 0, 0, 0, 0, 0, 0, 100, 100, 100, quiet);
		Check(Near(Mag(off), 0.0) && Near(RotMag(off), 0.0), "walkMult 0 silences the walk figure");
		WeaponBobClassProfile loud = kRifle; loud.walkMult = 2.0;
		auto big = WeaponBobEvaluate(1.0, 1.0 * 9.25, 1.0, 0, 0, 0, 0, 0, 0, 100, 100, 100, loud);
		auto ref = WeaponBobEvaluate(1.0, 1.0 * 9.25, 1.0, 0, 0, 0, 0, 0, 0, 100, 100, 100, kRifle);
		Check(Mag(big) > Mag(ref) * 1.5, "a higher walkMult visibly enlarges the figure");
	}

	{
		// STAIRS AND FLOOR WEAPONS. The eye climbs a staircase at a sustained ~130 u/s while running
		// (a step up over a dropped weapon is a short spike). That is not a jump: the gait must keep
		// its full strength and the jump term must not tip the gun.
		const double stairVz = 130.0, runSpeed = 250.0;
		Check(WeaponBobAirborneEstimate(stairVz, runSpeed) == 0.0 && WeaponBobFreeVz(stairVz, runSpeed) == 0.0,
			"climbing stairs at run speed is not airborne without the ground flag");
		Check(WeaponBobFreeVz(-stairVz, runSpeed) == 0.0, "walking down the same stairs is not a fall either");
		Check(WeaponBobJumpMove(WeaponBobFreeVz(stairVz, runSpeed)) == 0.0,
			"a staircase produces no jump lift or landing dip");
		// A real jump (initial vertical speed 302) and a real fall still register.
		Check(WeaponBobAirborneEstimate(302.0, 0.0) > 0.9, "a standing jump reads as airborne");
		Check(WeaponBobAirborneEstimate(302.0, runSpeed) > 0.5, "a running jump still reads as airborne");
		Check(WeaponBobAirborneEstimate(-500.0, runSpeed) > 0.9, "a long fall reads as airborne");
		Check(WeaponBobFreeVz(302.0, runSpeed) > 0.0 && WeaponBobFreeVz(-500.0, runSpeed) < 0.0,
			"free vertical speed keeps its sign");
		Check(WeaponBobJumpMove(WeaponBobFreeVz(302.0, 0.0)) < 0.0, "a real jump still lifts the gun");
		Check(std::isfinite(WeaponBobAirborneEstimate(std::nan(""), std::nan(""))),
			"a bad velocity read cannot poison the airborne estimate");
	}
	{
		// GROUND FLAG PATH. Losing contact for a tick or two (stairs down, a lip) must not dent the bob;
		// a jump or fall that lasts must.
		double air = 0.0;
		const double tick = 1.0 / 64.0;
		double peak = 0.0;
		for (int i = 0; i < 2; ++i) { air = WeaponBobAirTimeAdvance(air, tick, false); peak = std::fmax(peak, WeaponBobAirborneTarget(air)); }
		Check(peak == 0.0, "two ticks off the ground never count as airborne");
		air = WeaponBobAirTimeAdvance(air, tick, true);
		Check(air == 0.0, "touching the ground clears the timer at once");
		for (int i = 0; i < 4; ++i) air = WeaponBobAirTimeAdvance(air, tick, false);
		Check(WeaponBobAirborneTarget(air) < 0.5, "a quarter second on the way is still ramping in");
		for (int i = 0; i < 12; ++i) air = WeaponBobAirTimeAdvance(air, tick, false);
		Check(WeaponBobAirborneTarget(air) == 1.0, "a sustained jump is fully airborne");
		Check(Near(WeaponBobAirTimeAdvance(0.3, 0.0, false), 0.3) && Near(WeaponBobAirTimeAdvance(0.3, -1.0, false), 0.3),
			"a held or negative dt does not advance the timer");
		Check(WeaponBobAirTimeAdvance(9.99, 5.0, false) <= 10.0, "the timer is bounded");
	}

	std::cout << "WeaponBobMathTests: all checks passed\n";
	return 0;
}
