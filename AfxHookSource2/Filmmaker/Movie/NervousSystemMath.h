#pragma once

// Nervous System: the engine-free half. A shot player who goes down is not a sack at once: for a few
// seconds the body still reacts -- a hand goes to the wound, a shot leg is drawn up, the chest heaves,
// the limbs writhe and twitch -- and then it fades and goes still. A headshot does none of that.
//
// References (read for the behaviour, none of their code is used):
// - Overgrowth (Wolfire, open source, Data/Scripts/aschar.as, SetActiveRagdollInjuredPose): an
//   injured ragdoll is a ragdoll whose muscles are driven toward a writhe with a strength that is
//   weaker the faster the body moves (min(1, max(0.2, 2 - 0.3 * speed))) and fades linearly to zero at
//   the time of death; after that it is limp. A blunt (non-sharp) hit goes limp at once.
// - GTA V's NaturalMotion Euphoria behaviours, as exposed by ScriptHookVDotNet's EuphoriaHelpers:
//   Shot.reachForWound (timeBeforeReachForWound 0.2 s, grabHoldTime 2 s), injuredOnGround (up to two
//   injuries, reach with one or both hands), bodyWrithe (period ~1 s, amplitude 1, softer arms than
//   back and legs), shotInGuts (the body curls), pedalLegs, and conscious-pain ramps.
//
// The corpse is CS2's fifteen-body ragdoll (DeathAnimationsMath body indices). Reactions use bounded
// pose nudges and preserve the native velocities. Repeated solver rejection reduces the drive, so
// a blocked limb relaxes instead of repeatedly pushing a joint, wall, or floor.
//
//   * Reach and leg curl are TARGETS: a wrist / elbow / knee / ankle point, solved by two-bone IK from
//     the body's current shoulder or hip. Targets never ask a limb to reach further than it can, or to
//     fold shut, so they never drag the corpse across the floor.
//   * Breathing, writhing, head sway, twitches and the last breath are OFFSETS from a base position that
//     slowly follows wherever physics leaves the body (in the body's own frame), so nothing is held in
//     a pose: the floor or a joint that stops a limb moves its base there.
//   * Limbs move as CHAINS: the upper segment turns about the shoulder or hip toward the elbow / knee
//     goal, the lower one about the elbow / knee toward the wrist / ankle goal (SwingVelocity), and the
//     chain's bodies are NUDGED there -- placed, once per physics tick, turned by that tick's share of
//     the swing about the joint pivots, starting from wherever physics left them (RotatePoseAbout). The
//     joints stay connected, and whatever the floor or a wall did last tick is where the next nudge
//     starts, so contact still wins.
//   * The chest and head are nudged along their offset (DriveToward gives the rate).
//   * Why nudges and not velocity. Measured 2026-09-27 (`nervous kick`): a velocity written to ALL
//     fifteen bodies of a landed corpse throws it 13 units up -- it is awake. But a velocity that moves a
//     limb RELATIVE to the torso is erased every tick: the ragdoll's joints carry friction (m_flFriction
//     in Valve's PHYS), and a hand commanded at 160 u/s (linear) or its whole arm at 360 deg/s (chain
//     velocities) moved 0-8 u/s. Placement is not a velocity, so joint friction cannot eat it.
//   * Every rate ramps from its previous value by at most `accel * dt` (the muscles). A body's native
//     velocity is never replaced with the pelvis velocity; reactions do not steer airborne bodies.

#include "DeathAnimationsMath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Filmmaker {
namespace NervousSystemMath {

using DeathAnimationsMath::kBodies;
using DeathAnimationsMath::kPelvis;
using DeathAnimationsMath::kHeadBody;
using DeathAnimationsMath::kSpine2;
using DeathAnimationsMath::kThighLeft;
using DeathAnimationsMath::kThighRight;
using DeathAnimationsMath::kCalfLeft;
using DeathAnimationsMath::kCalfRight;
using DeathAnimationsMath::kAnkleLeft;
using DeathAnimationsMath::kAnkleRight;
using DeathAnimationsMath::kUpperArmLeft;
using DeathAnimationsMath::kUpperArmRight;
using DeathAnimationsMath::kForearmLeft;
using DeathAnimationsMath::kForearmRight;
using DeathAnimationsMath::kHandLeft;
using DeathAnimationsMath::kHandRight;

inline constexpr double kPi = 3.14159265358979323846;

// ---- small vector helpers ------------------------------------------------------------------------
struct V3 { double x = 0, y = 0, z = 0; };
inline V3 Make(double x, double y, double z) { V3 v; v.x = x; v.y = y; v.z = z; return v; }
inline V3 operator+(V3 a, V3 b) { return Make(a.x + b.x, a.y + b.y, a.z + b.z); }
inline V3 operator-(V3 a, V3 b) { return Make(a.x - b.x, a.y - b.y, a.z - b.z); }
inline V3 operator*(V3 a, double s) { return Make(a.x * s, a.y * s, a.z * s); }
inline double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return Make(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline double Length(V3 a) { return std::sqrt(Dot(a, a)); }
inline bool Normalize(V3& a) {
	const double n = Length(a);
	if (!(n > 1e-9)) return false;
	a = a * (1.0 / n);
	return true;
}
inline V3 Lerp(V3 a, V3 b, double t) { return a + (b - a) * t; }
inline double Clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline double Smooth(double v) { v = Clamp01(v); return v * v * (3.0 - 2.0 * v); }

// ---- the injury ----------------------------------------------------------------------------------
enum class Injury : int {
	None = 0, Head, Neck, Chest, Stomach, LeftArm, RightArm, LeftLeg, RightLeg, Explosion, Fire, Melee, Count
};
inline const char* InjuryName(Injury i) {
	static const char* const names[] = { "none", "head", "neck", "chest", "stomach", "leftarm", "rightarm",
		"leftleg", "rightleg", "explosion", "fire", "melee" };
	const int k = (int)i;
	return k >= 0 && k < (int)Injury::Count ? names[k] : "?";
}

// CS2 hitgroups: 1 head, 2 chest, 3 stomach, 4/5 left/right arm, 6/7 left/right leg, 8 neck, 0 generic.
// A headshot flag wins over everything (a grenade does not report one).
inline Injury InjuryFor(int hitgroup, bool headshot, bool explosive, bool burning, bool melee) {
	if (headshot || hitgroup == 1) return Injury::Head;
	if (explosive) return Injury::Explosion;
	if (burning) return Injury::Fire;
	if (melee) return Injury::Melee;
	switch (hitgroup) {
	case 8: return Injury::Neck;
	case 2: return Injury::Chest;
	case 3: return Injury::Stomach;
	case 4: return Injury::LeftArm;
	case 5: return Injury::RightArm;
	case 6: return Injury::LeftLeg;
	case 7: return Injury::RightLeg;
	default: return Injury::Chest;
	}
}

// Deterministic per death (seeded by demo tick and player), so a seek replays the same reaction.
inline uint32_t Hash(uint32_t x) {
	x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
	return x;
}
inline double Random01(uint32_t seed, uint32_t salt) {
	return double(Hash(seed ^ Hash(salt + 0x9e3779b9u)) & 0xffffff) / double(0x1000000);
}

inline constexpr int kHandBitLeft = 1, kHandBitRight = 2;
inline constexpr int kLegBitLeft = 1, kLegBitRight = 2;
// Limb index for limp masks: 0 left arm, 1 right arm, 2 left leg, 3 right leg.
inline int LimbBodyMask(int limb) { return 1 << limb; }

// What the nervous system does for one death.
struct Profile {
	bool reacts = true;      // false: limp at once (headshot)
	int reach = 0;           // kHandBit*: hands that go to the wound
	int woundBody = -1;      // the wound lies on this body, `along` of the way to `woundChild`
	int woundChild = -1;
	double along = 0.5;
	double woundRadius = 5;  // how far the skin is from that body line (units)
	int curl = 0;            // kLegBit*: legs drawn up
	int pedal = 0;           // kLegBit*: legs that pedal (blast)
	int limp = 0;            // 1 << limb: limbs left to physics (the shot arm)
	double writhe = 0.5;     // writhe amplitude, 1 = Euphoria bodyWrithe's default
	double breath = 1.0;     // breathing amplitude
	double twitch = 1.0;     // twitch rate scale
	double life = 1.0;       // how long they last, relative to the Duration setting
};

inline Profile ProfileFor(Injury injury, uint32_t seed) {
	Profile p;
	switch (injury) {
	case Injury::None:
	case Injury::Head:
	case Injury::Count:
		p.reacts = false;
		p.writhe = p.breath = p.twitch = 0;
		p.life = 0;
		break;
	case Injury::Neck:
		p.reach = kHandBitLeft | kHandBitRight;
		p.woundBody = kSpine2; p.woundChild = kHeadBody; p.along = 0.8; p.woundRadius = 2.5;
		p.writhe = 0.45; p.breath = 1.4; p.twitch = 1.2; p.life = 0.7;
		break;
	case Injury::Chest:
		// Both hands clutch the chest.
		p.reach = kHandBitLeft | kHandBitRight;
		p.woundBody = kSpine2; p.woundChild = kHeadBody; p.along = 0.35; p.woundRadius = 5.0;
		p.curl = Random01(seed, 2) < 0.5 ? (Random01(seed, 3) < 0.5 ? kLegBitLeft : kLegBitRight) : 0;
		p.writhe = 0.5; p.breath = 1.2; p.twitch = 1.0; p.life = 1.0;
		break;
	case Injury::Stomach:
	case Injury::Melee:
		// Gut shot: both hands on the stomach and the body curls (Euphoria shotInGuts).
		p.reach = kHandBitLeft | kHandBitRight;
		p.woundBody = kPelvis; p.woundChild = kSpine2; p.along = 0.45; p.woundRadius = 6.0;
		p.curl = kLegBitLeft | kLegBitRight;
		p.writhe = 0.6; p.breath = 1.0; p.twitch = 0.8; p.life = 1.2;
		break;
	case Injury::LeftArm:
	case Injury::RightArm: {
		// The shot arm hangs; the other hand clutches it.
		const bool left = injury == Injury::LeftArm;
		p.reach = left ? kHandBitRight : kHandBitLeft;
		p.woundBody = left ? kUpperArmLeft : kUpperArmRight;
		p.woundChild = left ? kForearmLeft : kForearmRight;
		p.along = 0.75; p.woundRadius = 2.5;
		p.limp = LimbBodyMask(left ? 0 : 1);
		p.writhe = 0.4; p.breath = 0.9; p.twitch = 0.8; p.life = 1.3;
		break;
	}
	case Injury::LeftLeg:
	case Injury::RightLeg: {
		// Both hands reach for the injured leg, which draws up to meet them.
		const bool left = injury == Injury::LeftLeg;
		p.reach = kHandBitLeft | kHandBitRight;
		p.woundBody = left ? kThighLeft : kThighRight;
		p.woundChild = left ? kCalfLeft : kCalfRight;
		p.along = 0.6; p.woundRadius = 3.5;
		p.curl = left ? kLegBitLeft : kLegBitRight;
		p.writhe = 0.5; p.breath = 0.9; p.twitch = 0.9; p.life = 1.3;
		break;
	}
	case Injury::Explosion:
		// No wound to hold: the legs pedal, the body thrashes and twitches.
		p.pedal = kLegBitLeft | kLegBitRight;
		p.writhe = 1.0; p.breath = 1.2; p.twitch = 1.5; p.life = 0.8;
		break;
	case Injury::Fire:
		p.pedal = kLegBitLeft | kLegBitRight;
		p.writhe = 1.6; p.breath = 1.4; p.twitch = 1.2; p.life = 0.9;
		break;
	}
	return p;
}

// Seconds this body reacts for: the Duration setting times the profile's share, +-25 % per death.
inline double LifeSeconds(double duration, const Profile& p, uint32_t seed) {
	if (!p.reacts || !(duration > 0)) return 0;
	return duration * p.life * (0.75 + 0.5 * Random01(seed, 4));
}

// Which layers one death plays, from the layers the settings enable. Everything at once read as a body
// thrashing, so reaching for the wound (with rolling over, which exposes the wound) always plays when
// enabled, and of breathing, writhing and twitches at most kMaxAmbientLayers do, chosen per death.
inline constexpr int kLayerReach = 1, kLayerBreath = 2, kLayerWrithe = 4, kLayerTwitch = 8, kLayerRoll = 16;
inline constexpr int kMaxAmbientLayers = 2;
inline int PickLayers(int enabled, uint32_t seed) {
	int out = enabled & (kLayerReach | kLayerRoll);
	if (enabled & kLayerReach) out |= kLayerRoll;
	int pool[3], count = 0;
	for (int bit : { kLayerBreath, kLayerWrithe, kLayerTwitch }) if (enabled & bit) pool[count++] = bit;
	for (int i = count - 1; i > 0; --i) { // seeded Fisher-Yates
		int j = int(Random01(seed, 0x4c41u + i) * (i + 1));
		if (j > i) j = i;
		const int swap = pool[i]; pool[i] = pool[j]; pool[j] = swap;
	}
	for (int i = 0; i < count && i < kMaxAmbientLayers; ++i) out |= pool[i];
	return out;
}

// ---- consciousness -------------------------------------------------------------------------------
// 0 until the reaction starts (Euphoria's timeBeforeReachForWound), a short ramp in, full for the first
// share of the life, then a linear fade to 0 at `life` (Overgrowth fades the injured ragdoll's strength
// linearly to its time of death).
inline constexpr double kOnset = 0.2;
inline constexpr double kRampIn = 0.4;
inline constexpr double kFullShare = 0.3;
inline double Consciousness(double t, double life) {
	if (!(life > 0) || t <= kOnset || t >= life) return 0;
	const double in = Smooth((t - kOnset) / kRampIn);
	const double fadeStart = life * kFullShare;
	const double out = t <= fadeStart ? 1.0 : Clamp01((life - t) / (life - fadeStart));
	return in * out;
}

// How much of the reaction plays (0..1): it eases in over `blendIn` seconds once it starts (after the
// onset) and eases out over the last `blendOut` seconds, so the body never snaps into or out of it.
// Consciousness fades the strength; this fades the MOTION itself, and it is what makes a Death Animations
// hand-off or the final "goes still" read as one movement.
inline double ReactionBlend(double t, double life, double blendIn, double blendOut) {
	if (!(life > 0) || t <= kOnset || t >= life) return 0;
	const double in = blendIn > 1e-3 ? Smooth((t - kOnset) / blendIn) : 1.0;
	const double out = blendOut > 1e-3 ? Smooth((life - t) / blendOut) : 1.0;
	return in * out;
}

// A body still flying or tumbling has no say (Overgrowth: min(1, max(0.2, 2 - 0.3 * speed)) in m/s;
// 1 m/s is ~39 units/s, so 0.3 per m/s is 1/130 per unit/s). `speed` is the pelvis speed, units/s.
inline double SpeedGate(double speed) {
	const double g = 2.0 - speed / 130.0;
	return g < 0.2 ? 0.2 : (g > 1.0 ? 1.0 : g);
}

// ---- the body's own frame ------------------------------------------------------------------------
// up: pelvis toward the shoulders; right: left shoulder/hip toward the right one; front: up x right
// (the chest's facing). Lying on the back, `front` points at the sky.
struct Frame { V3 front, right, up; bool valid = false; };

inline V3 BodyPos(const float* bodies15x3, int b) {
	return Make(bodies15x3[b * 3], bodies15x3[b * 3 + 1], bodies15x3[b * 3 + 2]);
}

inline bool Orthonormal(V3 up, V3 right, Frame& out) {
	if (!Normalize(up)) return false;
	right = right - up * Dot(right, up);
	if (!Normalize(right)) return false;
	out.up = up; out.right = right; out.front = Cross(up, right); out.valid = true;
	return true;
}

inline Frame BodyFrame(const float* p) {
	Frame f;
	const V3 shoulders = (BodyPos(p, kUpperArmLeft) + BodyPos(p, kUpperArmRight)) * 0.5;
	const V3 right = (BodyPos(p, kUpperArmRight) - BodyPos(p, kUpperArmLeft))
		+ (BodyPos(p, kThighRight) - BodyPos(p, kThighLeft));
	Orthonormal(shoulders - BodyPos(p, kPelvis), right, f);
	return f;
}

// Low-pass a frame (time constant `tau` seconds) so the layers built on it do not jump when the body
// rolls; re-orthonormalised afterwards.
inline Frame SmoothFrame(const Frame& prev, const Frame& cur, double dt, double tau) {
	if (!prev.valid) return cur;
	if (!cur.valid) return prev;
	const double a = tau > 0 ? 1.0 - std::exp(-dt / tau) : 1.0;
	Frame out;
	if (!Orthonormal(Lerp(prev.up, cur.up, a), Lerp(prev.right, cur.right, a), out)) return cur;
	return out;
}
inline V3 ToWorld(const Frame& f, V3 local) { return f.front * local.x + f.right * local.y + f.up * local.z; }
inline V3 ToLocal(const Frame& f, V3 world) { return Make(Dot(world, f.front), Dot(world, f.right), Dot(world, f.up)); }

// ---- two-bone IK ---------------------------------------------------------------------------------
// Root (shoulder / hip) `s`, segment lengths `a` (upper) and `b` (lower), goal `t` for the end (wrist /
// ankle), `pole` the side the middle joint (elbow / knee) bends toward. The goal is kept between
// `fold` and `reach` of the full length, so the limb is never asked to stretch or to fold further than a ragdoll
// joint allows. Returns false for degenerate input.
inline constexpr double kReach = 0.97, kFold = 0.5;
inline bool TwoBoneIk(V3 s, double a, double b, V3 t, V3 pole, double reach, double fold, V3& mid, V3& end) {
	if (!(a > 1e-3) || !(b > 1e-3)) return false;
	V3 n = t - s;
	double d = Length(n);
	if (!Normalize(n)) return false;
	const double maxD = (a + b) * reach;
	double minD = (a + b) * fold;
	if (minD < std::fabs(a - b) + 1e-3) minD = std::fabs(a - b) + 1e-3;
	if (d > maxD) d = maxD;
	if (d < minD) d = minD;
	V3 p = pole - n * Dot(pole, n);
	if (!Normalize(p)) {
		p = Cross(n, std::fabs(n.z) < 0.9 ? Make(0, 0, 1) : Make(1, 0, 0));
		if (!Normalize(p)) return false;
	}
	double c = (a * a + d * d - b * b) / (2.0 * a * d);
	c = c < -1 ? -1 : (c > 1 ? 1 : c);
	const double sn = std::sqrt(1.0 - c * c);
	mid = s + n * (a * c) + p * (a * sn);
	end = s + n * d;
	return true;
}

// Reach with a hand: the wrist goal on the wound. The skin point is on the wound's body line, pushed out
// toward `outward` (the chest's front for a body on its back or side; the caller picks the flank when
// it lies face down) and a little toward the reaching shoulder. `index`/`count` spread two hands along
// the wound so they do not sit inside each other.
inline constexpr double kPalmToWrist = 5.0;   // units the wrist stays back from the skin (2.5 put the hand inside the chest's collision, which popped it back out)
inline constexpr double kHandSpread = 2.5;    // units two hands sit apart along the wound
inline constexpr double kPressDepth = 0.8;    // units of the pressing pulse
inline constexpr double kPressHz = 1.3;
inline bool WoundGoal(const float* p, const Profile& prof, V3 shoulder, V3 outward, int index, int count, double t, double weight, V3& wrist) {
	if (prof.woundBody < 0 || prof.woundChild < 0) return false;
	const V3 a = BodyPos(p, prof.woundBody), b = BodyPos(p, prof.woundChild);
	V3 axis = b - a;
	if (!Normalize(axis)) return false;
	V3 point = Lerp(a, b, prof.along);
	if (count > 1) point = point + axis * (kHandSpread * (index == 0 ? -0.5 : 0.5));
	V3 side = shoulder - point;
	side = side - axis * Dot(side, axis);
	if (!Normalize(side)) return false;
	V3 front = outward - axis * Dot(outward, axis);
	const bool limb = prof.woundBody==kThighLeft || prof.woundBody==kThighRight ||
		prof.woundBody==kUpperArmLeft || prof.woundBody==kUpperArmRight;
	V3 out = Normalize(front) ? (limb ? side+front*0.75 : front+side*0.5) : side;
	if (!Normalize(out)) return false;
	const V3 skin = point + out * prof.woundRadius;
	const double press = kPressDepth * std::sin(2.0 * kPi * kPressHz * t + index * 1.7) * weight;
	wrist = skin + out * (kPalmToWrist - press);
	return true;
}

// Keep a reach on the injured limb. If its nominal wound point is too far away, choose the
// closest exposed point on that same segment while the leg curl brings it within arm's reach.
inline bool ReachGoal(const float* p, const Profile& prof, V3 shoulder, V3 outward,
	int index, int count, double t, double weight, double armLength, V3& wrist, double& fallback) {
	fallback = 0.0;
	if (!WoundGoal(p, prof, shoulder, outward, index, count, t, weight, wrist)) return false;
	const bool leg = prof.woundBody == kThighLeft || prof.woundBody == kThighRight;
	const bool arm = prof.woundBody == kUpperArmLeft || prof.woundBody == kUpperArmRight;
	if (!leg && !arm) return true;
	if (!(armLength > 1.0)) return true;
	const double reach = armLength * kReach;
	double best = Length(wrist - shoulder);
	if (best <= reach) return true;
	for (int sample = 1; sample <= 18; ++sample) {
		Profile candidate = prof;
		candidate.along = sample * 0.05;
		V3 surface;
		if (!WoundGoal(p, candidate, shoulder, outward, index, count, t, weight, surface)) continue;
		const double distance = Length(surface - shoulder);
		if (distance < best) { best = distance; wrist = surface; fallback = 1.0; }
	}
	return true;
}

// Stage a reach around the flank before crossing the front of the torso.
inline V3 ReachRoute(const float* p, const Frame& f, V3 wrist, V3 goal, int hand, bool& clearedFlank) {
	const V3 chest = BodyPos(p, kSpine2);
	const double front = Dot(wrist - chest, f.front);
	if (front >= 7.5) clearedFlank = true;
	else if (front < 0) clearedFlank = false;
	// Once around the flank, keep reaching the wound. The pressing pulse can sit below 7.5;
	// choosing the flank again there made the hand shuttle between two goals every few ticks.
	if (!clearedFlank && Dot(goal - chest, f.front) > 0) {
		const double side = hand == 0 ? -1.0 : 1.0;
		const V3 local = ToLocal(f, goal - chest);
		return chest + f.front * 10.0 + f.right * (side * 13.0) + f.up * local.z;
	}
	return goal;
}
inline V3 ElbowPole(V3 shoulder, V3 elbow, V3 wrist, const Frame& f, int hand) {
	return f.right * (hand == 0 ? -1.0 : 1.0) + f.front * 0.7 - f.up * 0.35;
}
inline double ArmClearance(const float* p, V3 elbow, V3 wrist) {
	const V3 a = BodyPos(p, kPelvis), axis = BodyPos(p, kSpine2) - a;
	const double len2 = Dot(axis, axis);
	double clearance = 1e9;
	for (int i = 0; i <= 8; ++i) {
		const V3 q = Lerp(elbow, wrist, i / 8.0);
		const V3 nearest = a + axis * (len2 > 1e-6 ? Clamp01(Dot(q - a, axis) / len2) : 0);
		const double d = Length(q - nearest);
		if (d < clearance) clearance = d;
	}
	return clearance;
}

inline bool ReachIk(const float* p, const Frame& f, int hand, V3 shoulder, V3 currentElbow,
	V3 currentWrist, V3 goal, V3& elbow, V3& wrist) {
	const double side=hand?1.0:-1.0;
	const V3 poles[]={currentElbow-shoulder, f.right*side+f.front*0.7-f.up*0.35,
		f.front*2.0+f.right*(side*0.5), f.front+f.up, f.front-f.up};
	double best=1e30;bool found=false;
	for(const V3 pole:poles) {
		V3 e,w;
		if(!TwoBoneIk(shoulder,Length(currentElbow-shoulder),Length(currentWrist-currentElbow),
			goal,pole,kReach,kFold,e,w))continue;
		const double clearance=ArmClearance(p,e,w);
		const double penalty=clearance<6.5 ? (6.5-clearance)*1000.0 : 0.0;
		const double floor=(std::min)(currentElbow.z,currentWrist.z)-0.5;
		const double below=e.z<floor ? (floor-e.z)*100.0 : 0.0;
		const double score=Length(e-currentElbow)+penalty+below;
		if(score<best){best=score;elbow=e;wrist=w;found=true;}
	}
	return found;
}

// ---- legs ----------------------------------------------------------------------------------------
// A leg drawn up: the thigh raised `curlAngle` toward the front, the shin hanging back down toward the
// feet (knee up, foot near the seat). `w` 0..1 blends from the leg's own starting direction (captured
// in the body frame when the reaction began) to the curled one.
inline constexpr double kCurlAngle = 1.2;  // radians, ~70 degrees of hip flexion
inline bool LegGoal(const Frame& f, V3 hip, double thigh, double shin, V3 restThighLocal, V3 restShinLocal,
	double w, V3& knee, V3& ankle, double curlAngle = kCurlAngle) {
	V3 curledThigh = Make(std::sin(curlAngle), 0, -std::cos(curlAngle));
	V3 curledShin = Make(-0.25, 0, -1.0);
	Normalize(curledShin);
	V3 dThigh = Lerp(restThighLocal, curledThigh, Clamp01(w));
	V3 dShin = Lerp(restShinLocal, curledShin, Clamp01(w));
	if (!Normalize(dThigh) || !Normalize(dShin)) return false;
	knee = hip + ToWorld(f, dThigh) * thigh;
	ankle = knee + ToWorld(f, dShin) * shin;
	return true;
}

// ---- layers --------------------------------------------------------------------------------------
// Smooth noise: three incommensurate sines per axis with seeded phases, sum of weights 1, so the value
// stays in [-1, 1]. `period` ~1 s is Euphoria bodyWrithe's default.
inline double Wave(uint32_t seed, uint32_t salt, double t, double period) {
	static const double kRatio[3] = { 1.0, 1.73, 2.61 };
	static const double kWeight[3] = { 0.55, 0.3, 0.15 };
	double v = 0;
	for (int k = 0; k < 3; ++k) {
		const double phase = 2.0 * kPi * Random01(seed, salt * 7u + k);
		v += kWeight[k] * std::sin(2.0 * kPi * kRatio[k] * t / period + phase);
	}
	return v;
}

// Writhe amplitude per body (units), front/right/up in the body frame. Arms softer than legs and back
// (Euphoria: arm stiffness and amplitude below back and legs); the pelvis carries the corpse and does
// not writhe.
inline constexpr double kWrithePeriod = 1.1;
inline V3 WritheAmplitude(int b) {
	switch (b) {
	case 11: case 12: return Make(4.0, 3.0, 2.5);  // hands
	case 8: case 10: return Make(2.0, 1.5, 1.0);   // forearms
	case 13: case 14: return Make(3.5, 2.5, 3.0);  // ankles
	case 1: case 3: return Make(2.5, 1.5, 1.5);    // calves (knees)
	case 6: return Make(0.8, 1.2, 0.0);            // spine: a twist and a heave
	case 4: return Make(1.0, 1.5, 0.0);            // head
	default: return Make(0, 0, 0);
	}
}
inline V3 WritheOffset(uint32_t seed, int b, double t, double amount) {
	const V3 amp = WritheAmplitude(b) * amount;
	return Make(amp.x * Wave(seed, 100 + b * 3, t, kWrithePeriod),
		amp.y * Wave(seed, 101 + b * 3, t, kWrithePeriod),
		amp.z * Wave(seed, 102 + b * 3, t, kWrithePeriod));
}

// Breathing: the chest (and a little of the head) lifts along the body's front and settles back. The
// rate is panicked early and slows as consciousness goes (1.1 Hz -> 0.3 Hz); the phase is integrated by
// the caller with BreathRate. Inhales are quick and exhales slow (a gasp).
inline double BreathRate(double consciousness) { return 0.3 + 0.8 * Clamp01(consciousness); }
inline double BreathLift(double phase) {
	const double c = 0.5 - 0.5 * std::cos(phase);
	return std::sqrt(c);
}
inline constexpr double kBreathChest = 1.2;  // units of chest lift at full breath
inline constexpr double kBreathHead = 0.5;

// Pedalling legs (blast / fire): each leg cycles its curl, the two out of phase.
inline constexpr double kPedalHz = 0.9;
inline double PedalCurl(double t, int leg) {
	return 0.5 + 0.5 * std::sin(2.0 * kPi * kPedalHz * t + (leg ? kPi : 0.0));
}

// ---- twitches ------------------------------------------------------------------------------------
// Sparse kicks, more of them as the body fades (agonal spasms), none once it is gone. The gap to the
// next is seeded by the twitch count, so a death always twitches the same way.
inline double TwitchRate(double consciousness, double scale) {
	return scale * (0.3 + 1.4 * (1.0 - Clamp01(consciousness)));
}
inline double TwitchGap(uint32_t seed, int count, double rate) {
	if (!(rate > 1e-6)) return 1e9;
	const double u = Random01(seed, 1000u + (uint32_t)count);
	return -std::log(1.0 - 0.98 * u) / rate;
}
// Which limb a twitch kicks: 0 left hand, 1 right hand, 2 left foot, 3 right foot, 4 head; skipping
// limbs in `busyMask` (1 << limb). -1 when every limb is busy.
inline int TwitchLimb(uint32_t seed, int count, int busyMask) {
	int start = (int)(Random01(seed, 2000u + (uint32_t)count) * 5.0) % 5;
	for (int k = 0; k < 5; ++k) {
		const int limb = (start + k) % 5;
		if (!(busyMask & (1 << limb))) return limb;
	}
	return -1;
}
// The jerk, in the body frame: mostly off the floor (front when lying on the back) with a random
// sideways part, `size` units at its peak. It plays as a quick out-and-back pulse (Pulse): a one-tick
// velocity kick moved a hand ~1 unit before the next tick's steering took it back.
inline V3 TwitchKick(uint32_t seed, int count, double size) {
	V3 d = Make(0.8, Random01(seed, 3000u + (uint32_t)count) * 2.0 - 1.0, (Random01(seed, 4000u + (uint32_t)count) * 2.0 - 1.0) * 0.5);
	Normalize(d);
	return d * (size * (0.6 + 0.4 * Random01(seed, 5000u + (uint32_t)count)));
}
inline constexpr double kTwitchSize = 6.0;   // units
inline constexpr double kTwitchTime = 0.22;  // seconds out and back
inline double Pulse(double age, double length) {
	return age > 0 && age < length ? std::sin(kPi * age / length) : 0.0;
}
// The last breath: the chest heaves up and settles, this big and this long, just before the end.
inline constexpr double kLastGaspLift = 2.5, kLastGaspTime = 0.9;

// ---- on the floor ------------------------------------------------------------------------------
// Lying face down: the chest faces the floor.
inline constexpr double kFaceDownZ = -0.3;
inline bool FaceDown(const Frame& f) { return f.valid && f.front.z < kFaceDownZ; }

// Face down, the floor is under the chest, and anything aimed into it levers the body up and rolls it
// onto its back. Until the roll exposes the chest, a hand never goes to a chest or belly wound (it is
// against the floor), a limb wound is held on its upper side (the back of the thigh or arm), knees are
// not drawn under the body, and writhe and breathing only move AWAY from the floor (the back rises).
inline bool FaceDownReach(const Profile& p) {
	return p.woundBody != kPelvis && p.woundBody != kSpine2 && p.woundBody != kHeadBody;
}
// Writhe / breath offset in the body frame (x = front), with the part that pushes into the floor removed
// and the breath turned to lift the back.
inline V3 FaceDownOffset(V3 local, double breathLift) {
	if (local.x > 0) local.x = 0;         // front is down: never push into the floor
	local.x -= 2.0 * breathLift;          // the breath's lift, which was along the front, raises the back
	return local;
}

// Require a lying torso and low vertical speed in all core bodies for a sustained interval.
inline bool GroundReactionCandidate(const Frame& f, V3 pelvis, V3 chest, V3 head) {
	return f.valid && std::fabs(f.up.z) < 0.65 && std::fabs(pelvis.z) < 12
		&& std::fabs(chest.z) < 12 && std::fabs(head.z) < 12;
}
inline double RollDirection(const Frame& f, uint32_t seed) {
	// d(front.z)/d(angle) = -right.z. Continue the existing lean toward face up.
	return std::fabs(f.right.z) > 0.04 ? (f.right.z > 0 ? -1.0 : 1.0) : (seed & 1 ? 1.0 : -1.0);
}
inline V3 RollVelocity(const Frame& f, double direction, double age, double strength) {
	if (!f.valid || age < 0 || age >= 4 || f.front.z > 0.7) return V3();
	const double remaining = std::acos((std::max)(-1.0, (std::min)(1.0, f.front.z)));
	const double rate = (std::min)(155.0, remaining * 100.0);
	const double ease = Smooth(age / 0.22) * Smooth((4.0 - age) / 0.4);
	return f.up * (direction * rate * ease * (std::min)(1.0, (std::max)(0.6, strength)));
}

// ---- the muscles ---------------------------------------------------------------------------------
// The velocity to command a body steered to `goal`: `feedforward` (the anchor body's motion plus the
// goal's own) and a correction on the braking curve (DeathAnimationsMath::BrakingGain: it can still
// stop in time, and closes at most a quarter of the error per physics tick), the correction capped at
// `maxSpeed`, reached from the previous command by at most `accel * dt`.
inline constexpr double kNerveAccel = 3000.0;  // units/s^2 at full strength and consciousness
inline constexpr double kReachSpeed = 160.0;   // units/s, a hand never moves faster than this
inline constexpr double kLayerSpeed = 35.0;    // small offsets must not snap a resting joint
// Muscles this weak (about gravity) let go of the body: the drive stops writing and physics has it.
inline constexpr double kHoldAccel = 400.0;
// Base positions follow where physics leaves a body with this time constant (seconds).
inline constexpr double kBaseFollow = 1.2;
inline V3 DriveToward(V3 actual, V3 goal, V3 feedforward, V3 previous, double accel, double maxSpeed, double step, double dt) {
	V3 err = goal - actual;
	V3 want = err * DeathAnimationsMath::BrakingGain(Length(err), accel, step);
	const double ws = Length(want);
	if (ws > maxSpeed && ws > 0) want = want * (maxSpeed / ws);
	want = want + feedforward;
	V3 dv = want - previous;
	const double dvs = Length(dv), cap = accel * dt;
	if (dvs > cap && dvs > 0) dv = dv * (cap / dvs);
	return previous + dv;
}
inline bool MusclesHold(double accel) { return accel >= kHoldAccel; }
// The muscles' grip as a weight (0..1) instead of on/off: none at half of kHoldAccel, full at 1.5x.
// Cutting the nudges the tick the muscles dropped below the hold level stopped a moving limb dead.
inline double MuscleWeight(double accel) { return Smooth((accel - 0.5 * kHoldAccel) / kHoldAccel); }

// Chains. Angular velocities are world axes in degrees/s (vphysics2's setter; Death Animations).
inline constexpr double kRadToDeg = 57.29577951308232;
inline constexpr double kNerveAngular = 1200.0;  // degrees/s^2 at full strength and consciousness
inline constexpr double kSwingSpeed = 120.0;     // degrees/s, a limb never swings faster
// The angular velocity that turns the segment `root -> tip` toward `root -> goal`: braking curve on the
// angle, capped at `maxSpeed`, reached from `previous` by at most `accel * dt`.
inline V3 SwingVelocity(V3 root, V3 tip, V3 goal, V3 previous, double accel, double maxSpeed, double step, double dt) {
	V3 u = tip - root, w = goal - root, want;
	if (Normalize(u) && Normalize(w)) {
		V3 axis = Cross(u, w);
		const double sn = Length(axis);
		if (sn > 1e-6) {
			const double angle = std::atan2(sn, Dot(u, w)) * kRadToDeg;
			double speed = angle * DeathAnimationsMath::BrakingGain(angle, accel, step);
			if (speed > maxSpeed) speed = maxSpeed;
			want = axis * (speed / sn);
		}
	}
	V3 dv = want - previous;
	const double dvs = Length(dv), cap = accel * dt;
	if (dvs > cap && dvs > 0) dv = dv * (cap / dvs);
	return previous + dv;
}
// The lower segment is already carried by the upper segment's turn. Solve its WORLD turn from
// the desired elbow-to-wrist vector, then subtract the parent's turn before applying NudgeChain.
// Aiming at the wrist from the old elbow applied the upper turn twice and made a held reach hunt.
inline V3 LowerSwing(V3 mid, V3 end, V3 midGoal, V3 endGoal, V3 upper,
	V3 previousWorld, double accel, double maxSpeed, double step, double dt) {
	return SwingVelocity(mid, end, mid + (endGoal - midGoal), previousWorld, accel, maxSpeed, step, dt) - upper;
}
// The rotation `angularDeg` (degrees/s, world axes) makes over `dt` seconds, as an xyzw quaternion.
inline void TurnQuat(V3 angularDeg, double dt, float q[4]) {
	V3 axis = angularDeg;
	const double angle = Length(axis) * dt / kRadToDeg;
	if (!Normalize(axis) || !(angle > 1e-9)) { q[0] = q[1] = q[2] = 0; q[3] = 1; return; }
	const double h = std::sin(angle * 0.5);
	q[0] = float(axis.x * h); q[1] = float(axis.y * h); q[2] = float(axis.z * h); q[3] = float(std::cos(angle * 0.5));
}
// Shortest rotation between two bone directions, including an exactly reversed segment.
inline bool DirectionTurn(V3 from,V3 to,float q[4]) {
	q[0]=q[1]=q[2]=0;q[3]=1;
	if(!Normalize(from)||!Normalize(to))return false;
	const double dot=(std::max)(-1.0,(std::min)(1.0,Dot(from,to)));
	V3 axis=Cross(from,to);
	if(dot< -0.999999) {
		axis=Cross(from,std::fabs(from.z)<.9?Make(0,0,1):Make(0,1,0));Normalize(axis);
		q[0]=float(axis.x);q[1]=float(axis.y);q[2]=float(axis.z);q[3]=0;
	}else {q[0]=float(axis.x);q[1]=float(axis.y);q[2]=float(axis.z);q[3]=float(1+dot);DeathAnimationsMath::QuatNormalize(q);}
	return true;
}
// A body pose turned by `dq` about `pivot`: its origin swings around the pivot and it turns with it.
inline DeathAnimationsMath::Pose RotatePoseAbout(const DeathAnimationsMath::Pose& in, V3 pivot, const float dq[4]) {
	DeathAnimationsMath::Pose out;
	const float rel[3] = { float(in.p[0] - pivot.x), float(in.p[1] - pivot.y), float(in.p[2] - pivot.z) };
	float r[3];
	DeathAnimationsMath::QuatRotate(dq, rel, r);
	out.p[0] = float(pivot.x + r[0]); out.p[1] = float(pivot.y + r[1]); out.p[2] = float(pivot.z + r[2]);
	DeathAnimationsMath::QuatMul(dq, in.q, out.q);
	DeathAnimationsMath::QuatNormalize(out.q);
	return out;
}
// Advance a hand target with a bounded, smooth approach independent of solver friction.
inline V3 ReachAdvance(V3 previous,V3 goal,double dt) {
    if(!(dt>0))return previous;
    const V3 delta=goal-previous;
    const double distance=Length(delta);
    const double fraction=(std::min)(1.0-std::exp(-dt/0.18),distance>1e-9?80.0*dt/distance:1.0);
    return previous+delta*fraction;
}
// Reach arbitration (measured 2026-09-28): a hand re-placed on a target the solver keeps rejecting
// (skin contact, a joint limit, a goal past arm's length) was pushed back ~1.8 units every tick,
// and that correction went through the shoulder into the torso. The goal follows the torso, so the
// body chased its own hand across the floor (10-23 units per reaction; 0.9 with reach off). A hand
// that has ARRIVED rests (no writes; joint friction holds it on the wound) until the wound moves
// kRestExit away: a single radius made the breathing goal flip it between rest and reach every few
// ticks, and each re-placement shoved the body again. A hand that stays blocked or out of reach, or
// any driving hand once the body has slid kSlideBudget, GIVES UP for the rest of the reaction: that
// arm goes limp while the other reaches on.
// Measured: released within 5 units of the wound, the wrist sprang back to 9-11 units within 3 ticks.
// Closer than that is a pose the joints and the hand-chest contact reject, so holding it is the fight.
inline constexpr double kRestRadius = 10.0;    // units: a wrist this close to its goal has arrived
inline constexpr double kRestExit = 14.0;      // units: a resting wrist drives again only this far off
inline constexpr double kBlockedGrip = 0.3;    // contact weight below which the solver is rejecting the hand
inline constexpr double kGiveUpBlocked = 0.6;  // seconds a hand may stay blocked away from its goal
inline constexpr double kGiveUpFar = 1.2;      // seconds a goal may stay out of arm's reach (a curling leg may bring it in)
inline constexpr double kSlideBudget = 6.0;    // units the pelvis may slide before driving hands give up
enum class ReachMode { Drive, Rest, GiveUp };
enum class GiveUpWhy { None, Blocked, OutOfReach, Slide, Pinned };
struct ReachTrack { double blockedFor = 0, farFor = 0; bool resting = false, gaveUp = false; GiveUpWhy why = GiveUpWhy::None; };
inline ReachMode ArbitrateReach(ReachTrack& r, double wristToGoal, double shoulderToGoal, double armLength, double grip,
	double slide, double dt) {
	if (r.gaveUp) return ReachMode::GiveUp;
	const bool far = shoulderToGoal > armLength * kReach + 1.5;
	r.farFor = far ? r.farFor + dt : 0.0;
	if (!far && wristToGoal < (r.resting ? kRestExit : kRestRadius)) { r.resting = true; r.blockedFor = 0; return ReachMode::Rest; }
	r.resting = false;
	const bool blocked = grip < kBlockedGrip;
	r.blockedFor = blocked ? r.blockedFor + dt : (std::max)(0.0, r.blockedFor - dt);
	r.why = slide > kSlideBudget ? GiveUpWhy::Slide : r.farFor > kGiveUpFar ? GiveUpWhy::OutOfReach
		: r.blockedFor > kGiveUpBlocked ? GiveUpWhy::Blocked : GiveUpWhy::None;
	if (r.why != GiveUpWhy::None) { r.gaveUp = true; return ReachMode::GiveUp; }
	return ReachMode::Drive;
}
inline const char* GiveUpName(GiveUpWhy w) {
	return w == GiveUpWhy::Blocked ? "blocked" : w == GiveUpWhy::OutOfReach ? "out-of-reach" : w == GiveUpWhy::Slide ? "slide"
		: w == GiveUpWhy::Pinned ? "pinned" : "none";
}

// An arm the body lies on, or one behind the back, has no clearance: pulling it out levers the whole
// body over it, which spun the corpse. Such an arm never reaches and is never relaxed; physics has it.
// The forearm or hand is pinned when it is behind the back plane AND within the torso's width (an arm
// on the floor beside a body lying on its back is free), or below the torso's centre line within
// its footprint (under the chest or lower back, whichever way the body lies).
inline constexpr double kBehindBack = 3.0;       // units behind the back plane
inline constexpr double kTorsoHalfWidth = 8.0;   // units either side of the spine
inline constexpr double kUnderTorso = 7.0;       // horizontal units from the spine counted as under it
inline bool ArmPinned(const float* p, const Frame& f, int hand) {
	if (!f.valid) return false;
	const V3 a = BodyPos(p, kPelvis), axis = BodyPos(p, kSpine2) - a;
	const double len2 = Dot(axis, axis);
	const int bodies[2] = { hand ? kForearmRight : kForearmLeft, hand ? kHandRight : kHandLeft };
	for (int b : bodies) {
		const V3 q = BodyPos(p, b);
		const V3 nearest = a + axis * (len2 > 1e-6 ? Clamp01(Dot(q - a, axis) / len2) : 0.0);
		const V3 d = q - nearest;
		const V3 local = ToLocal(f, d);
		if (local.x < -kBehindBack && std::fabs(local.y) < kTorsoHalfWidth) return true;
		if (d.z < -1.0 && std::hypot(d.x, d.y) < kUnderTorso) return true;
	}
	return false;
}

// ---- relax: after the reaction the body goes limp -------------------------------------------------
// Joint friction holds a limb wherever the last nudge left it, so a body that simply stops being
// driven keeps its hands frozen on the wound. For kRelaxTime after its life the arms are lowered to
// the torso's sides (and toward the floor) and the legs back to their starting direction, contact
// weighted, so the floor and the body stop them. Then physics has everything.
inline constexpr double kRelaxTime = 1.6;
inline double RelaxWeight(double age) {
	if (!(age >= 0) || age >= kRelaxTime) return 0;
	return Smooth(age / 0.3) * Smooth((kRelaxTime - age) / 0.5);
}
inline V3 RelaxArmGoal(const Frame& f, V3 shoulder, double armLength, int hand) {
	V3 dir = f.up * -0.75 + f.right * ((hand ? 1.0 : -1.0) * 0.4) + Make(0, 0, -0.45);
	if (!Normalize(dir)) dir = Make(0, 0, -1);
	return shoulder + dir * (armLength * 0.95);
}

// The most a reaching hand is placed per physics tick. Re-aiming the whole arm at its IK pose after
// the solver had shoved it back re-placed the hand by several units in one tick: a visible pop.
inline constexpr double kMaxHandPlace = 0.6;   // units per tick (~38 units/s at 64 Hz)
inline double CapPlaceWeight(double weight, double handMove) {
	return handMove > kMaxHandPlace ? weight * (kMaxHandPlace / handMove) : weight;
}
// Pose only the connected rigid bodies: rotations about shoulder and elbow preserve bone lengths.
// Never rebuild the renderer's local bone translations from the physics skeleton.
inline void AimChain(const DeathAnimationsMath::Pose in[3],V3 elbow,V3 wrist,double weight,DeathAnimationsMath::Pose out[3]) {
    float turn[4],blend[4];const float identity[4]={0,0,0,1};
    const V3 root=Make(in[0].p[0],in[0].p[1],in[0].p[2]);
    const V3 mid=Make(in[1].p[0],in[1].p[1],in[1].p[2]);
    DirectionTurn(mid-root,elbow-root,turn);
    // Match the elbow's bend plane as well as the upper-arm direction. A swing alone
    // can leave a sideways elbow hinge, which the ragdoll solver violently rejects.
    const V3 end=Make(in[2].p[0],in[2].p[1],in[2].p[2]);
    V3 normal=Cross(mid-root,end-mid),wanted=Cross(elbow-root,wrist-elbow),axis=elbow-root;
    if(Normalize(normal)&&Normalize(wanted)&&Normalize(axis)) {
        float v[3]={float(normal.x),float(normal.y),float(normal.z)},rotated[3];
        DeathAnimationsMath::QuatRotate(turn,v,rotated);
        normal=Make(rotated[0],rotated[1],rotated[2]);
        const double angle=std::atan2(Dot(Cross(normal,wanted),axis),Dot(normal,wanted));
        float twist[4],combined[4];TurnQuat(axis*(angle*kRadToDeg),1,twist);
        DeathAnimationsMath::QuatMul(twist,turn,combined);
        for(int k=0;k<4;++k)turn[k]=combined[k];
    }
    DeathAnimationsMath::QuatBlend(identity,turn,Clamp01(weight),blend);
    out[0]=RotatePoseAbout(in[0],root,blend);
    out[1]=RotatePoseAbout(in[1],root,blend);
    out[2]=RotatePoseAbout(in[2],root,blend);
    const V3 movedMid=Make(out[1].p[0],out[1].p[1],out[1].p[2]);
    const V3 movedEnd=Make(out[2].p[0],out[2].p[1],out[2].p[2]);
    DirectionTurn(movedEnd-movedMid,wrist-elbow,turn);
    DeathAnimationsMath::QuatBlend(identity,turn,Clamp01(weight),blend);
    out[1]=RotatePoseAbout(out[1],movedMid,blend);
    out[2]=RotatePoseAbout(out[2],movedMid,blend);
}
// A limb chain nudged: the upper segment's turn `up` about the root, then the lower segment's turn `lo`
// about the (moved) mid joint, both over `dt`. Poses in and out: root, mid, end body.
inline void NudgeChain(const DeathAnimationsMath::Pose in[3], V3 up, V3 lo, double dt, DeathAnimationsMath::Pose out[3]) {
	float qu[4], ql[4];
	TurnQuat(up, dt, qu);
	TurnQuat(lo, dt, ql);
	const V3 root = Make(in[0].p[0], in[0].p[1], in[0].p[2]);
	out[0] = RotatePoseAbout(in[0], root, qu);
	const DeathAnimationsMath::Pose mid = RotatePoseAbout(in[1], root, qu);
	const DeathAnimationsMath::Pose end = RotatePoseAbout(in[2], root, qu);
	const V3 joint = Make(mid.p[0], mid.p[1], mid.p[2]);
	out[1] = RotatePoseAbout(mid, joint, ql);
	out[2] = RotatePoseAbout(end, joint, ql);
}

// The velocity of a point `offset` from a point moving at `baseVelocity` on a body turning at `angularDeg`.
inline V3 PointVelocity(V3 baseVelocity, V3 angularDeg, V3 offset) {
	return baseVelocity + Cross(angularDeg * (1.0 / kRadToDeg), offset);
}

// Base follow: move `base` toward where physics has the body with the offset taken out.
inline V3 FollowBase(V3 base, V3 actualMinusOffset, double dt) {
	return Lerp(base, actualMinusOffset, 1.0 - std::exp(-dt / kBaseFollow));
}

// Give way when the solver undoes a nudge (floor, wall, or joint limit). Compare relative to the
// anchor and its native velocity so a freely translating/falling corpse is not mistaken for contact.
// Yield quickly, recover slowly: continually reapplying a rejected pose makes the whole body creep.
inline double ContactWeight(double weight, V3 nudge, V3 correction, double dt) {
	const double size2 = Dot(nudge,nudge);
	// Small integration/solver corrections are not a wall. Without this allowance, gravity and
	// resting-joint slop suppressed breathing and a gentle reach before either could move visibly.
	const double slop = 0.08 + 0.5 * 386.0 * dt * dt;
	const bool blocked = size2 > 1e-6 && Dot(correction,nudge) < -0.35 * size2 - slop * std::sqrt(size2);
	const double goal = blocked ? 0.0 : 1.0;
	return Clamp01(weight + (goal-weight) * (1.0-std::exp(-dt/(blocked ? 0.08 : 1.5))));
}

// Grounded reactions have no locomotion goal. Absorb low-speed sideways energy from nudged limbs
// through floor friction, while leaving the initial fall/slide and all vertical motion to physics.
inline double ReactionFriction(double planarSpeed, double dt) {
	const double grip = Smooth((20.0-planarSpeed)/10.0);
	return std::exp(-12.0*grip*(dt > 0 ? dt : 0));
}

// Muscle acceleration available now: strength setting (1 = 100 %), consciousness and the speed gate.
// Linear in consciousness so the hold fades evenly; below kHoldAccel the body is let go.
inline double NerveAccel(double strength, double consciousness, double gate) {
	const double s = strength < 0 ? 0 : strength;
	return kNerveAccel * s * s * Clamp01(consciousness) * gate;
}

} // namespace NervousSystemMath
} // namespace Filmmaker
