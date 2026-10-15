#pragma once

// Pure, engine-free maths for METHOD ACTING (see MethodActing.h): the blink schedule and the
// fire-squint envelope that together drive a player's single `eye_blink` flex weight.
//
// Header-only and free of any CS2 type on purpose, per the repository's "testable logic lives
// in *Math.h" rule -- the hook cannot be unit tested, so every formula here is exercised by
// Automation/tests/method-acting-math-tests.cpp instead.
//
// TWO PROPERTIES MATTER MORE THAN THE EXACT CURVES:
//
//   1. The blink is a PURE FUNCTION of demo time and a per-player seed. Nothing is integrated,
//      so scrubbing to a tick always produces the same face, and a paused demo holds a still
//      frame instead of blinking on the wall clock. Anything stateful here would make the same
//      frame render differently on a second visit, which is fatal for a movie tool.
//
//   2. The squint IS integrated (it has to be -- "how long since the last shot" cannot express
//      a rise that survives the next shot resetting the clock), but its state is a single float
//      that re-converges within a few frames, so a seek costs at most one blink-length of wrong
//      squint rather than a permanently wrong pose.

#include <cstdint>
#include <cmath>
#include <algorithm>

namespace Filmmaker {
namespace MethodActingMath {

struct BlinkParams {
	// Mean seconds between blinks. A human at rest blinks every 3-5 s; the placement below
	// spreads real gaps across roughly 0.4x-1.6x this, which reads as irregular without ever
	// producing an ACCIDENTAL double-blink -- the deliberate one below is a separate decision.
	double intervalSec = 4.0;
	double closeSec = 0.06;   // lid down
	double openSec = 0.09;    // lid back up; slower than the close, as a real blink is

	// People often blink twice in quick succession. `doubleChance` is the probability that any
	// given blink is a pair rather than a single; the draw is deterministic per player and per
	// window, so a double blink reproduces on a re-render like everything else here.
	double doubleChance = 0.35;
	double doubleGapSec = 0.10;  // eyes-open time between the two blinks of a pair
};

struct SquintParams {
	double level = 0.5;       // flex weight held while firing; 1.0 would be eyes fully shut
	double attackSec = 0.06;  // 0 -> level once the trigger goes
	double holdSec = 0.90;    // carry one squint across deliberate pistol taps
	double releaseSec = 0.45; // level -> 0 once the hold expires
};

// --- deterministic noise ---------------------------------------------------------------------
// A seeded integer hash rather than rand(): the blink time for (player, cycle) has to be the
// same value on every visit to that frame, including after a seek, a reload or a restart.

inline uint32_t Hash32(uint32_t a, uint32_t b) {
	uint32_t h = a * 0x9E3779B1u ^ (b + 0x85EBCA6Bu + (a << 6) + (a >> 2));
	h ^= h >> 16;
	h *= 0x7FEB352Du;
	h ^= h >> 15;
	h *= 0x846CA68Bu;
	h ^= h >> 16;
	return h;
}

// Hash -> [0,1).
inline double Unit01(uint32_t h) {
	return (double)(h & 0x00FFFFFFu) / (double)0x01000000u;
}

inline double Clamp01(double v) {
	if (!(v > 0.0)) return 0.0;   // also catches NaN
	if (v > 1.0) return 1.0;
	return v;
}

// --- blink -----------------------------------------------------------------------------------

// One blink: lid down over `close`, back up over `open`. `u` is time since the blink began, so a
// negative or past-the-end `u` is simply an open eye.
inline double BlinkPulse(double u, double close, double open) {
	if (u < 0.0) return 0.0;
	if (u < close) return Clamp01(u / close);
	if (u < close + open) return Clamp01(1.0 - (u - close) / open);
	return 0.0;
}

// One blink event per `intervalSec` window, placed inside the middle 60% of its window so
// consecutive blinks can neither collide nor drift into a metronome. `seed` is the player's
// identity, so two players standing side by side never blink in lockstep.
//
// The event is a PAIR with probability `doubleChance`, drawn from a second hash of the same
// window so the decision is stable across a seek. A pair reserves enough of the window that both
// blinks still finish inside it, which is what lets a single-window lookup stay correct.
//
// Returns the eye-closed weight in [0,1] at demo time `t`.
inline double BlinkWeight(double t, uint32_t seed, const BlinkParams& p) {
	const double interval = p.intervalSec;
	const double close = p.closeSec;
	const double open = p.openSec;
	if (!(interval > 0.0) || !(close > 0.0) || !(open > 0.0))
		return 0.0;
	if (!(t > 0.0))
		t = 0.0;

	const double cycleF = t / interval;
	const int64_t cycle = (int64_t)cycleF;
	const double windowStart = (double)cycle * interval;

	const double single = close + open;
	double gap = p.doubleGapSec;
	if (!(gap >= 0.0)) gap = 0.0;

	// The pair is dropped rather than truncated when the window is too short for it: a clipped
	// second blink would read as a stutter, and the single always fits wherever the pair does not.
	const double pairLength = single * 2.0 + gap;
	const bool wantDouble = Unit01(Hash32(seed ^ 0x5BD1E995u, (uint32_t)cycle)) < p.doubleChance;
	const bool isDouble = wantDouble && (pairLength <= interval);

	const double length = isDouble ? pairLength : single;
	double span = interval - length;
	if (span < 0.0) span = 0.0;

	const double offset = (0.2 + 0.6 * Unit01(Hash32(seed, (uint32_t)cycle))) * span;
	const double u = t - (windowStart + offset);

	double w = BlinkPulse(u, close, open);
	if (isDouble) {
		const double w2 = BlinkPulse(u - (single + gap), close, open);
		if (w2 > w) w = w2;
	}
	return w;
}

// --- fire squint -------------------------------------------------------------------------------

// Where the squint wants to be right now. Negative `secSinceLastShot` means "this player has not
// fired at all", which is how a fresh state and a genuinely idle player both read.
inline double SquintTarget(double secSinceLastShot, const SquintParams& p) {
	if (secSinceLastShot < 0.0) return 0.0;
	if (secSinceLastShot > p.holdSec) return 0.0;
	return Clamp01(p.level);
}
inline double ThreatSquintTarget(double secSinceNearShot, const SquintParams& p) {
    return secSinceNearShot >= 0.0 && secSinceNearShot <= 1.10 ? Clamp01(p.level) * .8 : 0.0;
}

// Direct injury is stronger and shorter than the near-miss defensive squint.
inline double HurtSquintTarget(double age,const SquintParams& p) {
    return std::isfinite(age)&&age>=0&&age<=.35?(std::min)(.85,Clamp01(p.level)*1.35):0;
}
// Flash duration is the victim's exposure, not the flashbang's position: only the
// blinded player reacts. Combat squint Off also disables both new reactions.
inline double FlashSquintTarget(double remaining,const SquintParams& p) {
    return std::isfinite(remaining)&&remaining>0?(std::min)(.85,Clamp01(p.level)*1.7):0;
}

// Distance to the actual bullet segment, rather than to its impact alone: a shot that crosses
// close to a face should read as incoming even when it hits a wall farther away.
inline bool NearBullet(const float start[3], const float impact[3], const float face[3], double radius) {
    if (!(radius > 0.0)) return false;
    double along=0.0,length2=0.0;
    for(int k=0;k<3;++k) {
        if(!std::isfinite(start[k]) || !std::isfinite(impact[k]) || !std::isfinite(face[k])) return false;
        const double d=(double)impact[k]-start[k];
        along+=((double)face[k]-start[k])*d;
        length2+=d*d;
    }
    if(length2<1.0) return false;
    double t=along/length2;
    if(t<0)t=0; else if(t>1)t=1;
    double distance2=0.0;
    for(int k=0;k<3;++k) {
        const double d=(double)face[k]-(start[k]+t*((double)impact[k]-start[k]));
        distance2+=d*d;
    }
    return distance2<=radius*radius;
}

// Move `current` one frame toward `target`. Attack and release are expressed as the time for a
// FULL 0 <-> level travel, so raising the squint level does not also make it slower.
inline double SquintStep(double current, double target, double dtSec, const SquintParams& p) {
	if (!(dtSec > 0.0)) return Clamp01(current);
	const double level = Clamp01(p.level);
	if (!(level > 0.0)) return 0.0;

	const bool rising = target > current;
	const double duration = rising ? p.attackSec : p.releaseSec;
	if (!(duration > 0.0)) return Clamp01(target);

	const double step = (level / duration) * dtSec;
	double next = current;
	if (rising) {
		next += step;
		if (next > target) next = target;
	} else {
		next -= step;
		if (next < target) next = target;
	}
	return Clamp01(next);
}

// Eye direction in the agent head's authored axes: +Y forward, +X up, +Z left.
struct Gaze { double yaw = 0, up = 0; };
struct IdleLook { Gaze gaze; double headYaw = 0; };
inline double Ease(double t) { t = Clamp01(t); return t*t*(3.0-2.0*t); }
inline double WanderPeriod(uint32_t seed) { return 1.6 + Unit01(Hash32(seed,91))*.7; }
inline IdleLook WanderPose(double time, uint32_t seed) {
    time = std::isfinite(time) && time > 0 ? time : 0;
    const double period = WanderPeriod(seed);
    const auto cycle = (uint32_t)(time / period);
    const double phase=time-(double)cycle*period;
    // Each glance returns to center. Ease in, visibly hold for 0.65 s, ease out, then rest.
    // Directly switching between two random side targets caused quick reversals on film.
    const double amount=Ease(phase/.34)*(1.0-Ease((phase-.99)/.38));
    const double choice=Unit01(Hash32(seed,cycle*3+11));
    const double side=choice<.08 ? 0.0 : choice<.54 ? -1.0 : 1.0;
    const bool wide=side!=0 && Unit01(Hash32(seed,cycle*3+14))<.45;
    const double spread=Unit01(Hash32(seed,cycle*3+12));
    const double yaw=side*(wide ? 34.0+14.0*spread : 14.0+8.0*spread);
    const double up=side==0 ? 0.0 : (Unit01(Hash32(seed,cycle*3+13))-.5)*5.0;
    const double head=wide ? side*(28.0+17.0*Unit01(Hash32(seed,cycle*3+15)))
                           : side*(8.0+6.0*spread);
    return {{yaw*amount,up*amount},head*amount};
}
// During combat only some glance cycles run, and their head/eye movement stays small.
inline IdleLook CombatWanderPose(double time,uint32_t seed) {
    time=std::isfinite(time)&&time>0?time:0;
    const auto cycle=(uint32_t)(time/WanderPeriod(seed));
    if(Unit01(Hash32(seed,cycle*3+19))>=.35) return {};
    const IdleLook idle=WanderPose(time,seed);
    return {{idle.gaze.yaw*.22,idle.gaze.up*.22},idle.headYaw*.20};
}
inline Gaze Wander(double time, uint32_t seed) { return WanderPose(time,seed).gaze; }
inline double HeadFollowYaw(double gazeYaw) {
    if (!std::isfinite(gazeYaw)) return 0.0;
    const double follow = gazeYaw * .28;
    return follow < -4.0 ? -4.0 : follow > 4.0 ? 4.0 : follow;
}
inline double HeadStep(double current, double target, double dt) {
    const double a=dt>0 ? 1-std::exp(-dt/0.22) : 0;
    return current+(target-current)*a;
}
inline bool NearbyGaze(const float headLocalDelta[3], double distance, Gaze& out) {
    if (!std::isfinite(distance) || distance < 24 || distance > 220) return false;
    const double x=headLocalDelta[0], y=headLocalDelta[1], z=headLocalDelta[2];
    if (!std::isfinite(x+y+z) || y <= 0) return false;
    const double yaw=std::atan2(z,y)*57.295779513;
    const double up=std::atan2(x,std::sqrt(y*y+z*z))*57.295779513;
    if (std::abs(yaw)>25 || std::abs(up)>18) return false;
    out={yaw,up}; return true;
}
inline Gaze GazeStep(Gaze current, Gaze target, double dt) {
    const double a=dt>0 ? 1-std::exp(-dt/0.12) : 0;
    return {current.yaw+(target.yaw-current.yaw)*a, current.up+(target.up-current.up)*a};
}
struct DeathEyes { double yaw, up, closed; };
inline DeathEyes DeathEyePose(double age, uint32_t seed = 0) {
    if (!(age>=0)) return {0,0,0};
    // Full blink weights pinch the corpse's eyelid mesh on shipped agents. Keep the
    // iris visible: a readable upward roll followed by a lightly lowered resting lid.
    const double settle=Ease((age-.3)/.75), roll=Ease(age/.50);
    const double side=(Hash32(seed,0xDEADu)&1) ? 1.0 : -1.0;
    return {side*7.0*roll, 25.0*roll*(1-.12*settle), .12*settle};
}
// Absolute local rotation, never added to last frame's eye pose (paused frames cannot drift).
inline void EyeRotation(Gaze gaze, float out[4]) {
    const double yaw=gaze.yaw*0.00872664626, pitch=-gaze.up*0.00872664626;
    const double sx=std::sin(yaw), cx=std::cos(yaw), sz=std::sin(pitch), cz=std::cos(pitch);
    // q = yaw(+X) * pitch(-Z) * authored eye rest quaternion (.5,.5,.5,.5).
    const double x=sx*cz, y=-sx*sz, z=cx*sz, w=cx*cz;
    out[0]=(float)((w+x+y-z)*.5); out[1]=(float)((w-x+y+z)*.5);
    out[2]=(float)((w+x-y+z)*.5); out[3]=(float)((w-x-y-z)*.5);
}

// --- combine -----------------------------------------------------------------------------------

// MAX, not sum: a blink during a burst has to still read as a blink (the lid reaches fully shut)
// rather than saturating at the squint level, and a squint must never be cancelled by the blink
// being at 0. Adding them would clip a mid-burst blink to a barely visible flicker.
inline double Combine(double squint, double blink) {
	const double s = Clamp01(squint);
	const double b = Clamp01(blink);
	return s > b ? s : b;
}

} // namespace MethodActingMath
} // namespace Filmmaker
