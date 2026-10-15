#pragma once

// Engine-free half of Death Animations (DeathAnimations.h): which clip a death gets, where the
// clip is planted, what every body's transform is at a given moment, and the velocities the
// ragdoll inherits when the clip ends. Tests: Automation/tests/death-animations-math-tests.cpp.
//
// A clip is fifteen CS2 ragdoll bodies in engine body order. Each frame stores the body's
// displacement from its frame-0 position and its absolute orientation (xyzw), in the clip's own
// space: pelvis above the origin, floor at z = 0. The clips are baked by
// FX/tools/extract-death-anims.py; category and side ids are the order of CATEGORY_IDS and
// HIT_SIDES there.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Filmmaker { namespace DeathAnimationsMath {

inline constexpr int kBodies = 15;
inline constexpr int kFloatsPerBody = 7;
inline constexpr int kPelvis = 5;
inline constexpr int kSpine2Body = 6, kHeadBody = 4;
inline constexpr int kThighLeft = 0;
inline constexpr int kThighRight = 2;

// Which donor family a clip came from, an explicit field in the baked file.
enum class Source : int { Mw2019 = 0, Classic, Extra, Count };

// Ids match CATEGORY_IDS in the pack builder. Moving keeps its id but is never baked or picked any
// more: speed is the Running stance, and a running death is filed by where the hit landed. Melee is
// knife and other melee kills.
enum class Category : int {
	Generic = 0, Head, Neck, Torso, LeftArm, RightArm, LeftLeg, RightLeg, Explosion, Moving,
	Fire, Melee, Manual,
	Count
};

// Manual clips start on the ground, in the air or never go down: they cannot follow a standing
// death, so they are never picked for one.
inline bool Pickable(Category c) { return c != Category::Manual && c != Category::Count; }

struct DeathFacts {
	int hitgroup = 0;          // CS2: 1 head, 2 chest, 3 stomach, 4/5 arms, 6/7 legs, 8 neck
	bool headshot = false;
	bool explosive = false;
	bool burning = false;
	bool melee = false;
	double planarSpeed = 0;    // victim speed at death, units/s (decides the Running stance)
};

// Faster than CS2's walk speed (130) is running. The Config panel's "Running above" overrides it.
inline constexpr double kRunSpeed = 130.0;

inline Category CategoryFor(const DeathFacts& d) {
	if (d.burning) return Category::Fire;
	if (d.explosive) return Category::Explosion;
	if (d.melee) return Category::Melee;
	if (d.headshot || d.hitgroup == 1) return Category::Head;
	if (d.hitgroup == 8) return Category::Neck;
	switch (d.hitgroup) {
	case 2: case 3: return Category::Torso;
	case 4: return Category::LeftArm;
	case 5: return Category::RightArm;
	case 6: return Category::LeftLeg;
	case 7: return Category::RightLeg;
	default: return Category::Generic;
	}
}

// The order categories are tried in when a source has no clip for the exact one (MW2019 has no
// arm, neck, melee or generic deaths). Returns the count written to `out` (at most 5).
inline int FallbackChain(Category c, Category out[5]) {
	int n = 0;
	out[n++] = c;
	switch (c) {
	case Category::Head: out[n++] = Category::Neck; break;
	case Category::Neck: out[n++] = Category::Head; break;
	case Category::Fire: out[n++] = Category::Explosion; break;
	case Category::Melee: case Category::Explosion:
	case Category::LeftArm: case Category::RightArm:
	case Category::LeftLeg: case Category::RightLeg: case Category::Torso: case Category::Generic:
	default: break;
	}
	if (c != Category::Torso) out[n++] = Category::Torso;
	if (c != Category::Generic) out[n++] = Category::Generic;
	return n;
}

enum class HitSide : int { Any = 0, Front, Back, Left, Right, Count };

// Facing in the ground plane from the two thighs, the way the bake measures clips:
// forward = (left hip - right hip) x up.
inline bool FacingFromHips(const float left[3], const float right[3], double out[2]) {
	const double lx = double(left[0]) - right[0], ly = double(left[1]) - right[1];
	const double length = std::sqrt(lx * lx + ly * ly);
	if (!(length > 1e-3)) return false;
	out[0] = ly / length;
	out[1] = -lx / length;
	return true;
}

// facing: victim forward; toSource: from the victim toward the shooter. Ground plane only.
inline HitSide HitSideFrom(const double facing[2], const double toSource[2]) {
	const double f = std::sqrt(facing[0] * facing[0] + facing[1] * facing[1]);
	const double s = std::sqrt(toSource[0] * toSource[0] + toSource[1] * toSource[1]);
	if (!(f > 1e-6) || !(s > 1e-6)) return HitSide::Any;
	const double ahead = (facing[0] * toSource[0] + facing[1] * toSource[1]) / (f * s);
	const double left = (-facing[1] * toSource[0] + facing[0] * toSource[1]) / (f * s);
	const double angle = std::atan2(left, ahead) * 57.29577951308232;
	if (angle >= -45.0 && angle <= 45.0) return HitSide::Front;
	if (angle >= 135.0 || angle <= -135.0) return HitSide::Back;
	return angle > 0 ? HitSide::Left : HitSide::Right;
}

inline bool SideCompatible(HitSide clip, HitSide hit) {
	return clip == HitSide::Any || hit == HitSide::Any || clip == hit;
}

// How the character stands when the clip starts. Order matches STANCES in the pack builder.
enum class Stance : int { Standing = 0, Crouched, Prone, Running, Count };

// Pelvis body height above the lower ankle body in a death pose: standing is ~33-37 units,
// crouched ~18-24. Below this the victim was crouching.
inline constexpr double kCrouchPelvisAboveAnkle = 27.0;

// The engine's ducking flag is the authority while the player is alive; the death pose is the
// cross-check for a death the flag sampling missed. Crouching outranks speed (a crouching player
// never reaches running speed anyway); otherwise a victim faster than walking is Running.
inline Stance VictimStance(bool duckingFlag, bool flagKnown, double pelvisAboveAnkle, double planarSpeed,
	double runSpeed = kRunSpeed) {
	if (flagKnown && duckingFlag) return Stance::Crouched;
	if (std::isfinite(pelvisAboveAnkle) && pelvisAboveAnkle < kCrouchPelvisAboveAnkle) return Stance::Crouched;
	if (planarSpeed > runSpeed) return Stance::Running;
	return Stance::Standing;
}

// Which weapon a clip is for. Order matches WEAPONS in the pack builder. Any is universal; Heavy
// (MW2019's large-calibre '_lg' hits) answers both shotgun and sniper kills.
enum class WeaponClass : int { Any = 0, Shotgun, Sniper, Heavy, Count };

// The killing weapon's class from player_death's weapon name. Every other gun is Any.
inline WeaponClass WeaponClassOf(const char* weapon) {
	if (!weapon) return WeaponClass::Any;
	static const char* const shotguns[] = { "nova", "xm1014", "mag7", "sawedoff" };
	static const char* const snipers[] = { "awp", "ssg08", "scar20", "g3sg1" };
	for (const char* w : shotguns) if (std::strstr(weapon, w)) return WeaponClass::Shotgun;
	for (const char* w : snipers) if (std::strstr(weapon, w)) return WeaponClass::Sniper;
	return WeaponClass::Any;
}

// Weapon tiers tried in order: the weapon's own clips outrank the universal ones. Other guns use
// universal clips, and heavy ones only when nothing universal fits the stance. Returns the count.
inline int WeaponTiers(WeaponClass killer, WeaponClass out[3]) {
	switch (killer) {
	case WeaponClass::Shotgun: out[0] = WeaponClass::Shotgun; out[1] = WeaponClass::Heavy; out[2] = WeaponClass::Any; return 3;
	case WeaponClass::Sniper: out[0] = WeaponClass::Sniper; out[1] = WeaponClass::Heavy; out[2] = WeaponClass::Any; return 3;
	default: out[0] = WeaponClass::Any; out[1] = WeaponClass::Heavy; return 2;
	}
}

struct ClipTags {
	Source source = Source::Extra;
	Category category = Category::Generic;
	HitSide side = HitSide::Any;
	Stance stance = Stance::Standing;
	WeaponClass weapon = WeaponClass::Any;
};

// How far a pick had to move from the exact death. Logged with every death.
enum class Relax : int {
	Exact = 0,        // body area and hit side both match
	AnySide,          // body area matches, the clip is tagged "any" side
	OtherSide,        // body area matches, the clip answers a different side
	FallbackArea,     // a fallback body area (FallbackChain), in the same side order
	NoClip            // nothing in this source for the stance: the native ragdoll plays
};

// The pool a death picks from. Stance and source are never relaxed: a crouched victim only ever
// gets crouched clips, and a source without one leaves the death to the ragdoll. The killing
// weapon's own clips come first (WeaponTiers): a shotgun kill plays a shotgun death if the stance
// has one, whatever else has to give. Within a weapon tier the body area outranks the hit side --
// a leg hit should look like a leg hit even from the wrong side -- so every side level is tried
// within one body area before falling back to the next area. `tier` reports which weapon tier
// answered. Writes clip indices to `out` (capacity `cap`) and returns the count.
inline int SelectPool(const ClipTags* clips, int count, Source source, Category category, HitSide hit,
	Stance stance, WeaponClass killer, int* out, int cap, Relax& relax, WeaponClass& tier) {
	Category chain[5];
	const int links = FallbackChain(category, chain);
	WeaponClass tiers[3];
	const int tierCount = WeaponTiers(killer, tiers);
	for (int t = 0; t < tierCount; ++t) {
		for (int l = 0; l < links; ++l) {
			for (int level = 0; level < 3; ++level) {
				int n = 0;
				for (int i = 0; i < count && n < cap; ++i) {
					const ClipTags& c = clips[i];
					if (c.source != source || c.stance != stance || c.weapon != tiers[t]
						|| c.category != chain[l] || !Pickable(c.category)) continue;
					const bool exact = hit != HitSide::Any && c.side == hit;
					const bool any = c.side == HitSide::Any || hit == HitSide::Any;
					if ((level == 0 && exact) || (level == 1 && any && !exact) || (level == 2 && !exact && !any))
						out[n++] = i;
				}
				if (n) {
					relax = l > 0 ? Relax::FallbackArea : (Relax)level;
					tier = tiers[t];
					return n;
				}
			}
		}
	}
	relax = Relax::NoClip;
	tier = WeaponClass::Any;
	return 0;
}

// Deterministic per death, so a seek back replays the same clip.
inline uint32_t Scramble(uint32_t x) {
	x ^= x >> 16; x *= 0x7feb352dU;
	x ^= x >> 15; x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

// ---------------------------------------------------------------------------------------------
// Transforms

struct Pose {
	float p[3] = {0, 0, 0};
	float q[4] = {0, 0, 0, 1}; // xyzw
};

inline void QuatMul(const float a[4], const float b[4], float out[4]) {
	const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
	out[0] = x; out[1] = y; out[2] = z; out[3] = w;
}

inline bool QuatNormalize(float q[4]) {
	const double n = std::sqrt(double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2] + double(q[3]) * q[3]);
	if (!(n > 1e-8) || !std::isfinite(n)) return false;
	for (int k = 0; k < 4; ++k) q[k] = float(q[k] / n);
	return true;
}

// Shortest-path normalized lerp. Frames are 1/30 s apart, where nlerp and slerp agree.
inline void QuatBlend(const float a[4], const float b[4], double t, float out[4]) {
	const double dot = double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2] + double(a[3]) * b[3];
	const double sign = dot < 0 ? -1.0 : 1.0;
	for (int k = 0; k < 4; ++k) out[k] = float(a[k] * (1.0 - t) + b[k] * sign * t);
	if (!QuatNormalize(out)) { for (int k = 0; k < 4; ++k) out[k] = a[k]; }
}

// Spherical interpolation, for the blend from the death pose into the clip, where the two
// orientations can be far apart.
inline void QuatSlerp(const float a[4], const float b[4], double t, float out[4]) {
	double dot = double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2] + double(a[3]) * b[3];
	double bs[4] = {b[0], b[1], b[2], b[3]};
	if (dot < 0) { dot = -dot; for (double& v : bs) v = -v; }
	if (dot > 0.9995) { QuatBlend(a, b, t, out); return; }
	const double theta = std::acos(dot < 1 ? dot : 1);
	const double s = std::sin(theta);
	const double wa = std::sin((1 - t) * theta) / s, wb = std::sin(t * theta) / s;
	for (int k = 0; k < 4; ++k) out[k] = float(a[k] * wa + bs[k] * wb);
	QuatNormalize(out);
}

// Planting: one yaw about world Z plus a translation. No scale and no tilt, so the clip keeps its
// authored proportions and its floor stays level.
struct Placement {
	float cosYaw = 1.f, sinYaw = 0.f;
	float offset[3] = {0, 0, 0};
	bool solved = false;
};

inline double FitWeight(int body) {
	switch (body) {
	case 7: case 9: return 0.35;   // upper arms: the gun pose differs from the donor's
	case 8: case 10: return 0.15;  // forearms
	case 11: case 12: return 0.10; // hands
	default: return 1.0;
	}
}

// Yaw that turns the clip's facing onto the victim's, then the offset that puts the clip's pelvis
// over the corpse's pelvis and the clip's floor on `floorZ`. Facing comes from the two thighs: a
// clip that starts mid-reaction (torso already leaning) would pull a whole-body fit sideways. The
// weighted least-squares fit of every body is the fallback when the hips are degenerate.
inline Placement SolvePlacement(const float clipBase[kBodies * 3], const float death[kBodies * 3], double floorZ) {
	Placement out;
	double cc[2] = {}, pc[2] = {}, wsum = 0;
	for (int i = 0; i < kBodies; ++i) {
		const double w = FitWeight(i);
		wsum += w;
		for (int k = 0; k < 2; ++k) { cc[k] += clipBase[i * 3 + k] * w; pc[k] += death[i * 3 + k] * w; }
	}
	for (int k = 0; k < 2; ++k) { cc[k] /= wsum; pc[k] /= wsum; }
	double num = 0, den = 0;
	for (int i = 0; i < kBodies; ++i) {
		const double w = FitWeight(i);
		const double cx = clipBase[i * 3] - cc[0], cy = clipBase[i * 3 + 1] - cc[1];
		const double px = death[i * 3] - pc[0], py = death[i * 3 + 1] - pc[1];
		num += (cx * py - cy * px) * w;
		den += (cx * px + cy * py) * w;
	}
	if (!std::isfinite(num) || !std::isfinite(den) || !std::isfinite(floorZ)) return out;
	double yaw = 0;
	double clipFacing[2], deathFacing[2];
	if (FacingFromHips(clipBase + kThighLeft * 3, clipBase + kThighRight * 3, clipFacing)
		&& FacingFromHips(death + kThighLeft * 3, death + kThighRight * 3, deathFacing))
		yaw = std::atan2(deathFacing[1], deathFacing[0]) - std::atan2(clipFacing[1], clipFacing[0]);
	else if (std::sqrt(num * num + den * den) > 1e-3)
		yaw = std::atan2(num, den);
	out.cosYaw = float(std::cos(yaw));
	out.sinYaw = float(std::sin(yaw));
	const float* cp = clipBase + kPelvis * 3;
	const float* dp = death + kPelvis * 3;
	out.offset[0] = float(dp[0] - (cp[0] * out.cosYaw - cp[1] * out.sinYaw));
	out.offset[1] = float(dp[1] - (cp[0] * out.sinYaw + cp[1] * out.cosYaw));
	out.offset[2] = float(floorZ);
	out.solved = true;
	return out;
}

inline double Yaw(const Placement& pl) { return std::atan2(double(pl.sinYaw), double(pl.cosYaw)); }

inline void Place(const Placement& pl, const Pose& clip, Pose& world) {
	world.p[0] = clip.p[0] * pl.cosYaw - clip.p[1] * pl.sinYaw + pl.offset[0];
	world.p[1] = clip.p[0] * pl.sinYaw + clip.p[1] * pl.cosYaw + pl.offset[1];
	world.p[2] = clip.p[2] + pl.offset[2];
	const double half = Yaw(pl) * 0.5;
	const float yawQ[4] = {0, 0, float(std::sin(half)), float(std::cos(half))};
	QuatMul(yawQ, clip.q, world.q);
	QuatNormalize(world.q);
}

// The clip's own length. Playback holds the last frame from here on.
inline double Duration(int frameCount, double fps) {
	return frameCount > 1 && fps > 0 ? (frameCount - 1) / fps : 0.0;
}

// Clip-space pose of one body at `seconds` (clamped to the clip). `frames` is the clip's payload:
// frameCount * kBodies * kFloatsPerBody floats.
inline void Sample(const float clipBase[kBodies * 3], const float* frames, int frameCount, double fps,
	double seconds, int body, Pose& out) {
	double f = seconds * fps;
	if (!(f > 0)) f = 0;
	if (f > frameCount - 1) f = frameCount - 1;
	const int a = int(f);
	const int b = a + 1 < frameCount ? a + 1 : a;
	const double t = f - a;
	const float* fa = frames + (size_t(a) * kBodies + body) * kFloatsPerBody;
	const float* fb = frames + (size_t(b) * kBodies + body) * kFloatsPerBody;
	for (int k = 0; k < 3; ++k) out.p[k] = float(clipBase[body * 3 + k] + fa[k] * (1 - t) + fb[k] * t);
	QuatBlend(fa + 3, fb + 3, t, out.q);
}

// Blend weight from the death pose into the clip. Smoothstep, so the body neither jerks into
// motion nor snaps at the end of the blend.
inline double BlendIn(double seconds, double blendSeconds) {
	if (!(blendSeconds > 0)) return 1.0;
	double x = seconds / blendSeconds;
	if (!(x > 0)) return 0.0;
	if (x >= 1) return 1.0;
	return x * x * (3 - 2 * x);
}

// The blend in starts from the death pose CARRYING the body's death velocity, decaying over kCarryTime:
// how far that coasting pose has moved `t` seconds in. Blending from a frozen death pose made the target
// stand still at t = 0 while the ragdoll was still moving, so the clip reversed the body's motion within
// a tick -- 4 of every 7 tracked bodies did, measured at t = 0.03-0.11 s (2026-09-26).
inline constexpr double kCarryTime = 0.08;
inline constexpr double kCarryMaxSpeed = 400.0;
inline void CarriedOffset(const float velocity[3], double t, double out[3]) {
	double v2 = 0;
	for (int k = 0; k < 3; ++k) v2 += double(velocity[k]) * velocity[k];
	const double speed = std::sqrt(v2);
	const double scale = speed > kCarryMaxSpeed ? kCarryMaxSpeed / speed : 1.0;
	const double travel = t > 0 ? kCarryTime * (1.0 - std::exp(-t / kCarryTime)) : 0.0;
	for (int k = 0; k < 3; ++k) out[k] = velocity[k] * scale * travel;
}

inline void Mix(const Pose& from, const Pose& to, double w, Pose& out) {
	for (int k = 0; k < 3; ++k) out.p[k] = float(from.p[k] + (to.p[k] - from.p[k]) * w);
	QuatSlerp(from.q, to.q, w, out.q);
}

// Velocity a body carries into physics when the clip releases it, from two world poses `dt`
// apart: linear in units/s, angular in DEGREES/s about world axes (vphysics2's angular setter
// takes degrees). CoD4's ragdoll does the same: it snapshots the animated pose on two frames and
// starts the simulation with that motion (Ragdoll_UpdateVelocityCapture).
inline bool ReleaseVelocity(const Pose& before, const Pose& after, double dt, float linear[3], float angularDeg[3]) {
	if (!(dt > 1e-5)) return false;
	for (int k = 0; k < 3; ++k) linear[k] = float((after.p[k] - before.p[k]) / dt);
	// delta = after * conj(before), in world frame.
	const float inv[4] = {-before.q[0], -before.q[1], -before.q[2], before.q[3]};
	float d[4];
	QuatMul(after.q, inv, d);
	if (d[3] < 0) for (float& v : d) v = -v;
	const double s = std::sqrt(double(d[0]) * d[0] + double(d[1]) * d[1] + double(d[2]) * d[2]);
	const double angle = 2.0 * std::atan2(s, double(d[3]));
	const double scale = s > 1e-9 ? angle / s / dt * 57.29577951308232 : 0.0;
	for (int k = 0; k < 3; ++k) angularDeg[k] = float(d[k] * scale);
	for (int k = 0; k < 3; ++k)
		if (!std::isfinite(linear[k]) || !std::isfinite(angularDeg[k])) return false;
	return true;
}

// Real impacts cap what a limb can carry into the ragdoll; a clip ending mid-swing must not
// launch a hand across the map. Linear in units/s, angular in degrees/s.
inline void ClampRelease(float linear[3], float angularDeg[3], double maxLinear = 600.0, double maxAngular = 1440.0) {
	const double l = std::sqrt(double(linear[0]) * linear[0] + double(linear[1]) * linear[1] + double(linear[2]) * linear[2]);
	if (l > maxLinear) for (int k = 0; k < 3; ++k) linear[k] = float(linear[k] * maxLinear / l);
	const double a = std::sqrt(double(angularDeg[0]) * angularDeg[0] + double(angularDeg[1]) * angularDeg[1] + double(angularDeg[2]) * angularDeg[2]);
	if (a > maxAngular) for (int k = 0; k < 3; ++k) angularDeg[k] = float(angularDeg[k] * maxAngular / a);
}

// ---- whole-skeleton posing (the picker's player-model preview) ---------------------------------
// The clip drives fifteen bones. Every other bone of the agent skeleton (spine_0/1, neck, clavicles,
// fingers, jiggle bones, ...) keeps its current offset from its nearest DRIVEN ancestor, so it rides
// along rigidly. Bones with no driven ancestor (root_motion) are left as they are.

inline void QuatConj(const float q[4], float out[4]) { out[0] = -q[0]; out[1] = -q[1]; out[2] = -q[2]; out[3] = q[3]; }

inline void QuatRotate(const float q[4], const float v[3], float out[3]) {
	const float p[4] = {v[0], v[1], v[2], 0}, qc[4] = {-q[0], -q[1], -q[2], q[3]};
	float t[4], r[4];
	QuatMul(q, p, t);
	QuatMul(t, qc, r);
	out[0] = r[0]; out[1] = r[1]; out[2] = r[2];
}

// oldWorld: the skeleton's current world pose (parents before children, as model skeletons store
// them). driven[b] >= 0 names the clip body that drives bone b, else -1. bodies: the fifteen target
// world poses. newWorld receives the whole posed skeleton.
inline void FollowDriven(const int* parents, int count, const int* driven, const Pose* bodies,
	const Pose* oldWorld, Pose* newWorld) {
	for (int b = 0; b < count; ++b) {
		if (driven[b] >= 0 && driven[b] < kBodies) { newWorld[b] = bodies[driven[b]]; continue; }
		// Nearest driven ancestor.
		int a = (b > 0 && parents[b] >= 0 && parents[b] < b) ? parents[b] : -1;
		while (a >= 0 && !(driven[a] >= 0 && driven[a] < kBodies))
			a = (parents[a] >= 0 && parents[a] < a) ? parents[a] : -1;
		if (a < 0) { newWorld[b] = oldWorld[b]; continue; }
		// delta = new(a) * inverse(old(a)), applied to b's old world pose.
		float inv[4], delta[4], rel[3], rot[3];
		QuatConj(oldWorld[a].q, inv);
		QuatMul(newWorld[a].q, inv, delta);
		QuatNormalize(delta);
		for (int k = 0; k < 3; ++k) rel[k] = oldWorld[b].p[k] - oldWorld[a].p[k];
		QuatRotate(delta, rel, rot);
		for (int k = 0; k < 3; ++k) newWorld[b].p[k] = newWorld[a].p[k] + rot[k];
		QuatMul(delta, oldWorld[b].q, newWorld[b].q);
		QuatNormalize(newWorld[b].q);
	}
}

// ---- keeping the limbs out of the body ---------------------------------------------------------
// The clips come from other games' skeletons and bodies: an arm folded across the chest on the donor
// passes through a CS2 agent's vest, pouches and pack, a hand sweeps through a thigh, a knee through
// the other leg. Build a clearance target by pushing limb probes toward the volume surfaces while
// keeping bone lengths (the limb swings, it never stretches) and turning the limb's bodies to match.
// Smooth those targets in clip space, so the in-game corpse and picker preview get the same result.
//
// Every volume is a capsule -- rounded at both ends, in the torso's case with a rounded-box
// cross-section -- so crossing the surface does not switch a full push on or off. Projection can
// still switch exit sides near the centre; BuildClearanceTrack smooths those corrections in clip
// time. These are approximate clearance volumes, not collision against the agent's gear mesh.
//
// Volumes: the torso (pelvis -> shoulder midpoint), and each leg's thigh and shin.
// Arms stay out of all of them; each leg stays out of the other leg.

// CLEARANCE IS DELIBERATELY TIGHT (reduced 2026-09-25 on report: the first values -- a 7 x 10.5
// torso, 1.2-3.0 joint padding -- shoved hands off the body in almost every clip and made them
// wander). These only catch real pass-through; a hand resting on the vest is left alone.
struct BodyVolumes {
	double torsoSide = 5.0;   // torso half-width, side to side (ribcage + kit)
	double torsoDepth = 5.5;  // torso half-depth, front to back (chest, vest, pouches, pack)
	double torsoBelow = 1.5;  // rounded bottom reaches this far below the pelvis (belt and pouches)
	double shoulderInset = 3.0; // upper axis end below the shoulder midpoint; cap covers the collar
	double thigh = 3.2;       // thigh radius
	double shin = 2.4;        // shin radius
};

inline constexpr int kUpperArmLeft = 7, kForearmLeft = 8, kUpperArmRight = 9, kForearmRight = 10;
inline constexpr int kHandLeft = 11, kHandRight = 12, kSpine2 = 6;
inline constexpr int kCalfLeft = 1, kCalfRight = 3, kAnkleLeft = 13, kAnkleRight = 14;

namespace detail {
inline double Dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void Cross(const double a[3], const double b[3], double o[3]) {
	o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
inline bool Unit(double v[3]) {
	const double n = std::sqrt(Dot(v, v));
	if (!(n > 1e-6)) return false;
	for (int k = 0; k < 3; ++k) v[k] /= n;
	return true;
}
inline void Load(const Pose& p, double o[3]) { for (int k = 0; k < 3; ++k) o[k] = p.p[k]; }
// Shortest rotation taking unit direction u onto unit direction v.
inline void Swing(const double u[3], const double v[3], float q[4]) {
	double c[3];
	Cross(u, v, c);
	const double w = 1.0 + Dot(u, v);
	if (w < 1e-6) {
		// A half-turn needs an axis perpendicular to u; world Z fails for vertical limbs.
		double ref[3] = {1, 0, 0};
		if (std::fabs(u[0]) > 0.8) { ref[0] = 0; ref[1] = 1; }
		Cross(u, ref, c); Unit(c);
		q[0] = float(c[0]); q[1] = float(c[1]); q[2] = float(c[2]); q[3] = 0; return;
	}
	q[0] = float(c[0]); q[1] = float(c[1]); q[2] = float(c[2]); q[3] = float(w);
	QuatNormalize(q);
}

// Axis a -> b, half-widths rx along `side` and ry along the axis x side direction. The torso uses
// a squircle cross-section: an ellipse cuts through the front pouches near either side of the vest.
// Round capsules pass rx == ry, power == 2 and any side perpendicular to the axis.
struct Capsule {
	double a[3], b[3], side[3], rx, ry;
	double power = 2.0;
};

// Pushes p out of the capsule inflated by `radius`. The push is zero on the surface, but its
// direction can switch at the axis; the clip track handles that discontinuity. True if p moved.
inline bool PushOut(const Capsule& c, double radius, double p[3]) {
	double axis[3] = {c.b[0] - c.a[0], c.b[1] - c.a[1], c.b[2] - c.a[2]};
	const double len2 = Dot(axis, axis);
	if (!(len2 > 1e-8)) return false;
	double d[3] = {p[0] - c.a[0], p[1] - c.a[1], p[2] - c.a[2]};
	double t = Dot(d, axis) / len2;
	t = t < 0 ? 0 : (t > 1 ? 1 : t);
	double up[3] = {axis[0], axis[1], axis[2]};
	Unit(up);
	double fwd[3];
	Cross(c.side, up, fwd);
	if (!Unit(fwd)) return false;
	const double rx = c.rx + radius, ry = c.ry + radius;
	// Offset from the nearest axis point, in capsule coordinates scaled so the surface is radius 1.
	double o[3];
	for (int k = 0; k < 3; ++k) o[k] = p[k] - (c.a[k] + axis[k] * t);
	const double x = Dot(o, c.side) / rx, y = Dot(o, fwd) / ry, z = Dot(o, up) / rx;
	const double planar = c.power == 2.0 ? x * x + y * y
		: std::pow(std::pow(std::fabs(x), c.power) + std::pow(std::fabs(y), c.power), 2.0 / c.power);
	const double r2 = planar + z * z;
	if (r2 >= 1.0) return false;
	double nx = x, ny = y, nz = z;
	if (r2 < 1e-10) { nx = 0; ny = 1; nz = 0; }   // exactly on the axis: out through the front
	else { const double s = 1.0 / std::sqrt(r2); nx *= s; ny *= s; nz *= s; }
	for (int k = 0; k < 3; ++k)
		p[k] = c.a[k] + axis[k] * t + c.side[k] * nx * rx + fwd[k] * ny * ry + up[k] * nz * rx;
	return true;
}

inline Capsule Round(const double a[3], const double b[3], double r) {
	Capsule c;
	for (int k = 0; k < 3; ++k) { c.a[k] = a[k]; c.b[k] = b[k]; }
	double axis[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
	// Any unit vector perpendicular to the axis.
	double ref[3] = {0, 0, 1};
	if (std::fabs(axis[2]) > std::fabs(axis[0]) && std::fabs(axis[2]) > std::fabs(axis[1])) { ref[0] = 1; ref[2] = 0; }
	Cross(axis, ref, c.side);
	if (!Unit(c.side)) { c.side[0] = 1; c.side[1] = 0; c.side[2] = 0; }
	c.rx = c.ry = r;
	return c;
}

// One limb: root (fixed), middle joint, end joint, and a tip past the end (fingers / toes).
struct Limb { int root, mid, end; double tip; double endRadius = 0.7; };

// Moves one limb out of `count` capsules. Returns true if it moved; rotates its three bodies.
inline bool ClearLimb(Pose bodies[kBodies], const Limb& limb, const Capsule* caps, int count) {
	double S[3], E[3], H[3], E0[3], H0[3];
	Load(bodies[limb.root], S); Load(bodies[limb.mid], E); Load(bodies[limb.end], H);
	for (int k = 0; k < 3; ++k) { E0[k] = E[k]; H0[k] = H[k]; }
	double se[3] = {E[0] - S[0], E[1] - S[1], E[2] - S[2]}, eh[3] = {H[0] - E[0], H[1] - E[1], H[2] - E[2]};
	const double upperLen = std::sqrt(Dot(se, se)), lowerLen = std::sqrt(Dot(eh, eh));
	if (!(upperLen > 1e-3) || !(lowerLen > 1e-3)) return false;
	double originalLower[3] = {eh[0], eh[1], eh[2]}; Unit(originalLower);
	// CS2's hand/ankle X axis points down the digits on the left and is mirrored on the right.
	// A bent wrist's fingers do not continue the forearm line. Test their actual orientation.
	const bool right = limb.end == kHandRight || limb.end == kAnkleRight;
	const float tipLocal[3] = {float(right ? -limb.tip : limb.tip), 0, 0};
	float originalTip[3]; QuatRotate(bodies[limb.end].q, tipLocal, originalTip);
	bool any = false;
	for (int iter = 0; iter < 8; ++iter) {
		bool step = false;
		for (int c = 0; c < count; ++c) {
			// Middle joint, then back onto the upper segment's length from the fixed root.
			if (PushOut(caps[c], 0.8, E)) {
				step = true;
				double d[3] = {E[0] - S[0], E[1] - S[1], E[2] - S[2]};
				if (Unit(d)) for (int k = 0; k < 3; ++k) E[k] = S[k] + d[k] * upperLen;
			}
			// Mid-segment, end joint and tip all carry the end joint with them.
			const double probes[3][2] = {{0.5, 0.7}, {1.0, limb.endRadius}, {1.0, 0.5}};
			for (int probe = 0; probe < 3; ++probe) {
				const auto& pr = probes[probe];
				double dir[3] = {H[0] - E[0], H[1] - E[1], H[2] - E[2]};
				if (!Unit(dir)) break;
				double p[3], q[3];
				float tip[3] = {};
				if (probe == 2) {
					float swing[4]; Swing(originalLower, dir, swing); QuatRotate(swing, originalTip, tip);
				}
				for (int k = 0; k < 3; ++k) q[k] = p[k] = E[k] + dir[k] * lowerLen * pr[0] + tip[k];
				if (!PushOut(caps[c], pr[1], p)) continue;
				step = true;
				const double t = pr[0] > 1.0 ? 1.0 : pr[0];
				for (int k = 0; k < 3; ++k) H[k] += (p[k] - q[k]) / t;
			}
			double d2[3] = {H[0] - E[0], H[1] - E[1], H[2] - E[2]};
			if (Unit(d2)) for (int k = 0; k < 3; ++k) H[k] = E[k] + d2[k] * lowerLen;
		}
		if (!step) break;
		any = true;
	}
	if (!any) return false;
	double u0[3] = {E0[0] - S[0], E0[1] - S[1], E0[2] - S[2]}, u1[3] = {E[0] - S[0], E[1] - S[1], E[2] - S[2]};
	double f0[3] = {H0[0] - E0[0], H0[1] - E0[1], H0[2] - E0[2]}, f1[3] = {H[0] - E[0], H[1] - E[1], H[2] - E[2]};
	if (!Unit(u0) || !Unit(u1) || !Unit(f0) || !Unit(f1)) return false;
	float su[4], sf[4], out[4];
	Swing(u0, u1, su);
	Swing(f0, f1, sf);
	QuatMul(su, bodies[limb.root].q, out); QuatNormalize(out); for (int k = 0; k < 4; ++k) bodies[limb.root].q[k] = out[k];
	QuatMul(sf, bodies[limb.mid].q, out); QuatNormalize(out); for (int k = 0; k < 4; ++k) bodies[limb.mid].q[k] = out[k];
	QuatMul(sf, bodies[limb.end].q, out); QuatNormalize(out); for (int k = 0; k < 4; ++k) bodies[limb.end].q[k] = out[k];
	for (int k = 0; k < 3; ++k) { bodies[limb.mid].p[k] = float(E[k]); bodies[limb.end].p[k] = float(H[k]); }
	return true;
}

inline int LegCapsules(const Pose bodies[kBodies], int thigh, int calf, int ankle, const BodyVolumes& v, Capsule out[2]) {
	double hip[3], knee[3], foot[3];
	Load(bodies[thigh], hip); Load(bodies[calf], knee); Load(bodies[ankle], foot);
	out[0] = Round(hip, knee, v.thigh);
	out[1] = Round(knee, foot, v.shin);
	return 2;
}
} // namespace detail

// Returns how many limbs were moved (0 = the pose was already clear and is untouched).
inline int KeepLimbsOutOfBody(Pose bodies[kBodies], const BodyVolumes& vol = BodyVolumes()) {
	using namespace detail;
	double P[3], up[3], side[3], l[3], r[3];
	Load(bodies[kPelvis], P);
	Load(bodies[kUpperArmLeft], l); Load(bodies[kUpperArmRight], r);
	// spine_2 sits below the chest and can lean differently from the shoulders. Extending its
	// pelvis axis missed the vest in hunched clips. Fit the full torso to the shoulder midpoint.
	for (int k = 0; k < 3; ++k) up[k] = (l[k] + r[k]) * 0.5 - P[k];
	const double height = std::sqrt(Dot(up, up));
	if (!Unit(up)) return 0;
	for (int k = 0; k < 3; ++k) side[k] = l[k] - r[k];
	const double along = Dot(side, up);
	for (int k = 0; k < 3; ++k) side[k] -= up[k] * along;
	if (!Unit(side)) return 0;
	// Keep the waist wide enough for belt pouches while rounding both ends.
	Capsule torso;
	for (int k = 0; k < 3; ++k) {
		torso.a[k] = P[k] - up[k] * (vol.torsoBelow - vol.torsoSide);
		torso.b[k] = P[k] + up[k] * (height - vol.shoulderInset);
		torso.side[k] = side[k];
	}
	torso.rx = vol.torsoSide;
	torso.ry = vol.torsoDepth;
	torso.power = 2.0; // a plain ellipse: the boxy power-4 section filled the space in front of the arms

	int moved = 0;
	// Legs first, each out of the other one, so the arms then see where the legs really are.
	const Limb legL = {kThighLeft, kCalfLeft, kAnkleLeft, 5.0}, legR = {kThighRight, kCalfRight, kAnkleRight, 5.0};
	Capsule caps[5];
	LegCapsules(bodies, kThighRight, kCalfRight, kAnkleRight, vol, caps);
	moved += ClearLimb(bodies, legL, caps, 2) ? 1 : 0;
	LegCapsules(bodies, kThighLeft, kCalfLeft, kAnkleLeft, vol, caps);
	moved += ClearLimb(bodies, legR, caps, 2) ? 1 : 0;

	caps[0] = torso;
	LegCapsules(bodies, kThighLeft, kCalfLeft, kAnkleLeft, vol, caps + 1);
	LegCapsules(bodies, kThighRight, kCalfRight, kAnkleRight, vol, caps + 3);
	// The wrist bone is not the outside of the glove: reserve palm thickness around it as well.
	const Limb armL = {kUpperArmLeft, kForearmLeft, kHandLeft, 4.0, 0.8}, armR = {kUpperArmRight, kForearmRight, kHandRight, 4.0, 0.8};
	moved += ClearLimb(bodies, armL, caps, 5) ? 1 : 0;
	moved += ClearLimb(bodies, armR, caps, 5) ? 1 : 0;
	return moved;
}

// A projection has no continuous choice of exit at a capsule's centre. Baking only its raw
// result would still pop when that choice changes sides. Smooth the additional limb SWINGS in
// clip time, then cap their angular speed in both directions. This anticipates contact without
// depending on playback history (seeking and the preview must return the same pose).
inline constexpr double kClearanceFps = 120.0;
inline constexpr double kClearanceAngularSpeed = 8.0; // radians/s of correction, not authored motion
inline constexpr int kClearanceBones[8] = {7, 8, 9, 10, 0, 1, 2, 3};
inline constexpr int kClearanceEnds[4] = {11, 12, 13, 14};
struct ClearanceFrame {
	float swing[8][4] = {{0,0,0,1}, {0,0,0,1}, {0,0,0,1}, {0,0,0,1},
		{0,0,0,1}, {0,0,0,1}, {0,0,0,1}, {0,0,0,1}};
};

inline void LimitClearanceStep(const float previous[4], float next[4], double maxAngle) {
	double dot = 0;
	for (int k = 0; k < 4; ++k) dot += double(previous[k]) * next[k];
	dot = std::fabs(dot);
	const double angle = 2 * std::acos(dot < 1 ? dot : 1);
	if (angle > maxAngle) QuatSlerp(previous, next, maxAngle / angle, next);
}

inline void SmoothClearanceTrack(std::vector<ClearanceFrame>& track) {
	if (track.empty()) return;
	const auto targets = track;
	const int last = int(track.size()) - 1;
	// Centred 133 ms window; authored positions/rotations are never filtered or delayed.
	constexpr int radius = 8;
	for (int f = 0; f <= last; ++f) for (int b = 0; b < 8; ++b) {
		float sum[4] = {};
		for (int j = -radius; j <= radius; ++j) {
			const int index = f + j < 0 ? 0 : (f + j > last ? last : f + j);
			const float* q = targets[index].swing[b];
			// All target swings use the identity hemisphere, including near 180-degree exits.
			const double weight = (radius + 1 - std::abs(j)) * (q[3] < 0 ? -1.0 : 1.0);
			for (int k = 0; k < 4; ++k) sum[k] += float(weight * q[k]);
		}
		if (QuatNormalize(sum)) std::memcpy(track[f].swing[b], sum, sizeof(sum));
	}
	const double step = kClearanceAngularSpeed / kClearanceFps;
	for (int f = 1; f <= last; ++f) for (int b = 0; b < 8; ++b)
		LimitClearanceStep(track[f - 1].swing[b], track[f].swing[b], step);
	for (int f = last - 1; f >= 0; --f) for (int b = 0; b < 8; ++b)
		LimitClearanceStep(track[f + 1].swing[b], track[f].swing[b], step);
}

inline std::vector<ClearanceFrame> BuildClearanceTrack(const float base[kBodies * 3], const float* frames,
	int frameCount, double fps, const BodyVolumes& volumes = BodyVolumes()) {
	if (!frames || frameCount < 1 || !(fps > 0)) return {};
	const int count = int(std::ceil(Duration(frameCount, fps) * kClearanceFps)) + 1;
	std::vector<ClearanceFrame> result(count);
	for (int f = 0; f < count; ++f) {
		Pose raw[kBodies], corrected[kBodies];
		for (int b = 0; b < kBodies; ++b) {
			Sample(base, frames, frameCount, fps, f / kClearanceFps, b, raw[b]);
			corrected[b] = raw[b];
		}
		if (!KeepLimbsOutOfBody(corrected, volumes)) continue;
		for (int b = 0; b < 8; ++b) {
			float inverse[4];
			QuatConj(raw[kClearanceBones[b]].q, inverse);
			QuatMul(corrected[kClearanceBones[b]].q, inverse, result[f].swing[b]);
			QuatNormalize(result[f].swing[b]);
		}
	}
	SmoothClearanceTrack(result);
	return result;
}

// Apply interpolated rotations to the sampled bone vectors, not interpolated joint positions:
// both segment lengths stay exactly those of the authored pose even between track samples.
inline void ApplyClearanceTrack(Pose bodies[kBodies], const std::vector<ClearanceFrame>& track, double seconds) {
	if (track.empty()) return;
	double f = seconds * kClearanceFps;
	if (!(f > 0)) f = 0;
	if (f > track.size() - 1) f = double(track.size() - 1);
	const size_t a = size_t(f), b = a + 1 < track.size() ? a + 1 : a;
	for (int limb = 0; limb < 4; ++limb) {
		const int root = kClearanceBones[limb * 2], mid = kClearanceBones[limb * 2 + 1], end = kClearanceEnds[limb];
		float upper[3], lower[3], movedUpper[3], movedLower[3], su[4], sf[4];
		for (int k = 0; k < 3; ++k) {
			upper[k] = bodies[mid].p[k] - bodies[root].p[k];
			lower[k] = bodies[end].p[k] - bodies[mid].p[k];
		}
		QuatSlerp(track[a].swing[limb * 2], track[b].swing[limb * 2], f - a, su);
		QuatSlerp(track[a].swing[limb * 2 + 1], track[b].swing[limb * 2 + 1], f - a, sf);
		QuatRotate(su, upper, movedUpper); QuatRotate(sf, lower, movedLower);
		for (int k = 0; k < 3; ++k) {
			bodies[mid].p[k] = bodies[root].p[k] + movedUpper[k];
			bodies[end].p[k] = bodies[mid].p[k] + movedLower[k];
		}
		QuatMul(su, bodies[root].q, bodies[root].q); QuatNormalize(bodies[root].q);
		QuatMul(sf, bodies[mid].q, bodies[mid].q); QuatNormalize(bodies[mid].q);
		QuatMul(sf, bodies[end].q, bodies[end].q); QuatNormalize(bodies[end].q);
	}
}

// ---- playback feel: strength, blend out, contact ------------------------------------------------
// The clip no longer has to own the body outright. Each frame a body is placed part-way from where
// physics actually left it toward the clip pose -- `strength` of the way. 1 is the old 1:1 playback;
// below 1, gravity and collisions keep the rest. The last `blendOut` seconds ramp that down to zero,
// so the ragdoll is already carrying the body when the clip ends instead of dropping it from the
// clip's last pose (the "floats, then falls" at the end).
//
// Contact is read from the physics itself: every frame the body is placed with a velocity, so after
// the physics step it should sit at (placed + velocity * dt). A body the world pushed back from that
// is touching something. A LIMB that is pushed follows the clip more loosely as the push grows, so it
// bends against the wall; pushed hard, it goes limp for the rest of the clip while everything else
// keeps playing. The TORSO being pushed means the whole body ran into something: the animation stops
// and physics takes over.

struct PlaybackTuning {
	double blendIn = 0.1;     // seconds from the death pose into the clip
	double strength = 1.0;    // 0..1 of the way from the physical pose to the clip pose, every frame
	double blendOut = 0.5;    // seconds over which the clip crossfades the body into physics, before every hand-off
	bool reactToWalls = true; // limbs bend / go limp and the torso stops the clip on contact
	bool solid = false;       // drive bodies by velocity so the world collides (false = place them exactly)
	double impact = 1.0;      // scale of the push a death gives the body (shot direction, weapon, blast)
};

// A crossfade that STARTS at `start` (contact, balance loss -- moments nobody can see coming): 1 before
// it, a smoothstep down to 0 over `length`. start < 0 = no fade.
inline double FadeOutWeight(double seconds, double start, double length) {
	if (start < 0 || seconds <= start) return 1.0;
	if (!(length > 0)) return 0.0;
	const double x = (seconds - start) / length;
	if (x >= 1) return 0.0;
	return 1.0 - x * x * (3.0 - 2.0 * x);
}

// Clip weight over time for the blend OUT: 1 until `blendOut` before the end, then a smoothstep to 0.
inline double BlendOutWeight(double seconds, double duration, double blendOut) {
	if (!(blendOut > 0)) return seconds >= duration ? 0.0 : 1.0;
	const double x = (duration - seconds) / blendOut;
	if (!(x > 0)) return 0.0;
	if (x >= 1) return 1.0;
	return x * x * (3 - 2 * x);
}

// Which limb a body belongs to: 0 left arm, 1 right arm, 2 left leg, 3 right leg; -1 for the torso
// (pelvis, spine, head).
inline int LimbOf(int body) {
	switch (body) {
	case 7: case 8: case 11: return 0;
	case 9: case 10: case 12: return 1;
	case 0: case 1: case 13: return 2;
	case 2: case 3: case 14: return 3;
	default: return -1;
	}
}

struct ContactRules {
	double limbSoft = 1.5;   // units of push where a limb starts to give
	double limbHard = 6.0;   // push that makes the limb go limp
	double torso = 4.0;      // push on pelvis / spine / head that stops the clip
	int frames = 2;          // consecutive frames over a threshold before it counts (noise)
	int settle = 4;          // frames ignored after the clip starts (the body is still being caught)
	bool limbStops = false;  // a limb hit hard stops the whole clip (instead of just going limp)
};

struct ContactState {
	int frame = 0;
	int limbHits[4] = {};
	int torsoHits = 0;
	double limbGive[4] = {};   // 0 = follows the clip, 1 = fully physical
	bool limp[4] = {};
	bool stop = false;
	int stopBody = -1;
	double stopPush = 0;
};

// Feed one frame of per-body push distances (actual vs predicted, units). Updates which limbs are
// giving or limp and whether the clip must stop.
// Solid drive: a LIMB that hits the floor or a wall goes limp (physics) while the rest of the clip plays
// on; the TORSO hitting something ends the clip and CS2's own ragdoll physics carries on from there. The CS2 ragdoll cannot reach every clip pose
// exactly (its bone lengths are not the donor's) and physics steps land between render frames:
// measured 2026-09-25 in open space, pushes reach ~4 on the torso and ~6 on limbs, so the stop
// thresholds sit above that. No partial give: a limb either follows the clip or goes limp.
inline ContactRules SolidContactRules() {
	ContactRules r;
	r.limbSoft = 10.0; r.limbHard = 10.0; r.torso = 6.0; r.frames = 2;
	return r;
}

inline void UpdateContact(ContactState& s, const double push[kBodies], const ContactRules& r = ContactRules()) {
	const bool live = s.frame++ >= r.settle;
	double limbPush[4] = {};
	int limbBody[4] = {-1, -1, -1, -1};
	double torsoPush = 0;
	int torsoBody = -1;
	for (int b = 0; b < kBodies; ++b) {
		const double p = std::isfinite(push[b]) ? push[b] : 0.0;
		const int limb = LimbOf(b);
		if (limb >= 0) { if (p > limbPush[limb]) { limbPush[limb] = p; limbBody[limb] = b; } }
		else if (p > torsoPush) { torsoPush = p; torsoBody = b; }
	}
	if (!live) return;
	s.torsoHits = torsoPush > r.torso ? s.torsoHits + 1 : 0;
	if (s.torsoHits >= r.frames && !s.stop) { s.stop = true; s.stopBody = torsoBody; s.stopPush = torsoPush; }
	for (int l = 0; l < 4; ++l) {
		if (s.limp[l]) continue;
		s.limbHits[l] = limbPush[l] > r.limbSoft ? s.limbHits[l] + 1 : 0;
		if (s.limbHits[l] < r.frames) { s.limbGive[l] *= 0.8; continue; } // eases back once clear
		if (limbPush[l] >= r.limbHard) {
			s.limp[l] = true; s.limbGive[l] = 1.0;
			if (r.limbStops && !s.stop) { s.stop = true; s.stopBody = limbBody[l]; s.stopPush = limbPush[l]; }
			continue;
		}
		const double give = (limbPush[l] - r.limbSoft) / (r.limbHard - r.limbSoft);
		if (give > s.limbGive[l]) s.limbGive[l] = give;
	}
}

// Fraction of the way toward the clip pose a body is placed this frame. 0 = leave it to physics.
inline double BodyFollow(int body, double seconds, double duration, const PlaybackTuning& tune, const ContactState& contact) {
	double w = tune.strength < 0 ? 0 : (tune.strength > 1 ? 1 : tune.strength);
	w *= BlendOutWeight(seconds, duration, tune.blendOut);
	const int limb = LimbOf(body);
	if (tune.reactToWalls && limb >= 0) w *= contact.limp[limb] ? 0.0 : 1.0 - contact.limbGive[limb];
	return w;
}

// ---- solid playback: velocity drive ---------------------------------------------------------------
// Placing a body on the clip pose every frame (SetTransform) ignores the world: the solver pushes a
// knee back out of the floor during its step, and the next frame places it straight back in. The
// solid drive never places anything. It hands each body the velocity that carries it from where
// physics has it now onto the clip pose one frame ahead (the kinematic-target velocity Jolt's
// Body::MoveKinematic and active-ragdoll velocity matching use), and the solver integrates that
// through its contacts: a limb aimed into the floor stops on the floor, a body aimed into a wall
// stops at the wall, and the error is re-measured and re-aimed the next frame. `follow` (0..1) is
// the share of the gap closed per frame: 1 tracks the clip, lower lets gravity and contacts keep more.
inline constexpr double kDriveMaxLinear = 2500.0;   // units/s; a clip never legitimately exceeds it
inline constexpr double kDriveMaxAngular = 3600.0;  // degrees/s

// Braking-curve tracking. The velocity a body is asked for = the clip's own velocity (feedforward,
// `now` -> `ahead` over `step`) plus a correction toward the pose it should be in NOW. The correction
// is capped at the speed the muscles can still stop from in time, sqrt(2 * accel * |error|): close to
// the pose that is the error over one physics step (tight tracking), far from it it is slower. Asking
// for "the whole gap in one tick" while the muscles limit acceleration overshot and oscillated around
// the pose -- a limit cycle that read as jitter, worse at low strength (2026-09-26).
// accel <= 0 means no correction at all (the body just follows the feedforward, if anything).
// The correction closes at most kTrackGain of the error per physics tick. The pose it reads is one tick
// old when the correction lands, and "close all of it next tick" with that delay rings (x' = x - x_prev
// oscillates every ~6 ticks: the 8-13 reversals/s measured at 100 % strength, 2026-09-26). 0.25 is the
// largest gain whose response to a one-tick delay has no oscillation (z^2 - z + g = 0, real roots for
// g <= 0.25). The feedforward -- the clip's own velocity -- carries the motion; this only corrects drift.
inline constexpr double kTrackGain = 0.25;
inline double BrakingGain(double error, double accel, double step) {
	const double tight = step > 0 ? kTrackGain / step : 0.0;
	if (!(accel > 0)) return 0.0;
	if (!(error > 1e-6)) return tight;
	const double g = std::sqrt(2.0 * accel / error);
	return g < tight ? g : tight;
}
inline bool TrackVelocity(const Pose& actual, const Pose& now, const Pose& ahead, double step,
	double linearAccel, double angularAccel, float linear[3], float angularDeg[3]) {
	float ffL[3], ffA[3], eL[3], eA[3];
	if (!ReleaseVelocity(now, ahead, step, ffL, ffA)) return false;
	if (!ReleaseVelocity(actual, now, 1.0, eL, eA)) return false; // the error: units, and degrees about world axes
	const double nl = std::sqrt(double(eL[0]) * eL[0] + double(eL[1]) * eL[1] + double(eL[2]) * eL[2]);
	const double na = std::sqrt(double(eA[0]) * eA[0] + double(eA[1]) * eA[1] + double(eA[2]) * eA[2]);
	const double gl = BrakingGain(nl, linearAccel, step), ga = BrakingGain(na, angularAccel, step);
	for (int k = 0; k < 3; ++k) {
		linear[k] = float(ffL[k] + eL[k] * gl);
		angularDeg[k] = float(ffA[k] + eA[k] * ga);
	}
	ClampRelease(linear, angularDeg, kDriveMaxLinear, kDriveMaxAngular);
	return true;
}

// The step the drive aims across is CS2's physics tick, not the render frame. Measured 2026-09-25:
// aiming across a 4 ms render frame asked for 4x the velocity and every body overshot by metres, and
// aiming across the interval at which the bodies are SEEN to move (~7.5 ms) still rotated the pelvis
// ~2x faster than commanded and blew the ragdoll up -- the engine integrates a set velocity over its
// whole 1/64 s tick.
inline constexpr double kPhysicsStep = 1.0 / 64.0;

// ---- physics that plays a part: muscles, root drift, impact ------------------------------------
// Muscles. Each body keeps the motion physics gave it (gravity, momentum, the push of the shot) and the
// clip may only change that velocity as fast as its "muscles" can accelerate: MuscleLimit, which falls
// with the SQUARE of strength. At 100 % a limb can reach 6000 u/s^2 and tracks the clip; at 30 % the
// cap is 540, barely more than gravity (386), so the body sags and the clip only leads; at 10 % it is
// 60 and the body is a ragdoll the clip nudges. A share-of-the-gap blend could not do this: any
// per-frame fraction of "go to the clip" still converges on the clip within a few frames, so 10 %
// looked like 100 % with extra jitter.
//
// The pelvis's own muscle is weaker (kRootAccel): it holds the body up against gravity but a push from
// a shot or a blast takes a few tenths of a second to be absorbed, so the body travels with it. The
// animation itself is carried with the pelvis (root drift, applied by the caller), so the knock-back
// is never pulled back to where the victim stood.
inline constexpr double kMuscleAccel = 6000.0;     // u/s^2 at strength 1, every body but the pelvis
// Rotation muscles are deliberately weak: at 60000 they forced joints into poses the CS2 skeleton does
// not fit (its bone lengths are not the donor's) -- hyperextended, stiff, "rigor mortis". Positions
// lead and the joints settle the orientation.
inline constexpr double kMuscleAngular = 15000.0;  // deg/s^2 at strength 1
inline constexpr double kRootAccel = 1500.0;       // u/s^2 at strength 1, the pelvis

inline double MuscleLimit(double strength, double atFull) {
	const double s = strength < 0 ? 0 : (strength > 1 ? 1 : strength);
	return atFull * s * s;
}

// Physics' velocity moved toward the drive velocity by at most `linearAccel * dt` / `angularAccel * dt`.
inline void MuscleVelocity(const float physLinear[3], const float physAngular[3], const float driveLinear[3],
	const float driveAngular[3], double linearAccel, double angularAccel, double dt, float linear[3], float angular[3]) {
	double dl[3], da[3], nl = 0, na = 0;
	for (int k = 0; k < 3; ++k) {
		dl[k] = double(driveLinear[k]) - physLinear[k]; nl += dl[k] * dl[k];
		da[k] = double(driveAngular[k]) - physAngular[k]; na += da[k] * da[k];
	}
	nl = std::sqrt(nl); na = std::sqrt(na);
	const double ml = linearAccel * dt, ma = angularAccel * dt;
	const double sl = nl > ml && nl > 0 ? ml / nl : 1.0, sa = na > ma && na > 0 ? ma / na : 1.0;
	for (int k = 0; k < 3; ++k) {
		linear[k] = float(physLinear[k] + dl[k] * sl);
		angular[k] = float(physAngular[k] + da[k] * sa);
	}
	ClampRelease(linear, angular, kDriveMaxLinear, kDriveMaxAngular);
}

// ---- exact playback on the real floor: the ground track ----------------------------------------
// Exact playback places the clip, so the world cannot stop it -- a knee aimed into the floor sinks,
// and a lying pose authored above the floor floats. The ground track fixes both WITHOUT physics: a
// vertical offset for the whole pose, computed once per clip from the clip itself and smoothed with a
// zero-phase filter, so the animation stays exactly as smooth as it was authored.
//   lift:   whenever a body would go below the floor (its origin closer to the floor than
//           kGroundRadius -- how far that body's origin sits above the floor when it rests on it),
//           the pose rises by that much;
//   settle: once the clip is down (pelvis below kDownPelvis), the pose also lowers until its lowest
//           body rests on the floor, so the corpse is on the ground when the clip lets go.
// Clip space: floor at z = 0.
inline constexpr float kGroundRadius[kBodies] = {
	6.0f, 4.5f, 6.0f, 4.5f,   // thighs, calves
	6.0f, 7.0f, 7.0f,         // head, pelvis, spine
	3.5f, 3.0f, 3.5f, 3.0f,   // upper arms, forearms
	3.0f, 3.0f,               // hands
	4.0f, 4.0f };             // feet (a standing clip's feet sit at 4.3)
inline constexpr double kDownPelvis = 18.0;
inline constexpr double kGroundRate = 60.0;
inline constexpr double kGroundSmooth = 0.08; // s, time constant of the zero-phase smoothing
inline constexpr double kGroundSink = 1.0;    // units a smoothed lift may lag the raw one
// No rebound: the clip's hips may never rise more than kReboundAllowance above their lowest point
// so far. The CoD explosion clips animate a crouch and then the body HURLED UP (hips 43 -> 21 -> 44
// -> 11 units in cod_explosion_death_r1): played on the ground the corpse started falling, was pulled
// back up to standing, then fell over (2026-09-26). Limbs, twist and the fall all stay; only the fake
// launch goes -- a blast's real lift is the grenade flight. The allowance keeps small natural bounces
// (gun clips rebound 1-4 units).
inline constexpr double kReboundAllowance = 4.0;

// Raw offset for one pose: > 0 lifts, < 0 lowers (only when the clip is down).
inline double GroundOffsetRaw(const Pose pose[kBodies]) {
	double gap = 1e9;
	for (int b = 0; b < kBodies; ++b) { const double g = pose[b].p[2] - kGroundRadius[b]; if (g < gap) gap = g; }
	if (pose[kPelvis].p[2] < kDownPelvis) return -gap;   // down: rest on the floor
	return gap < 0 ? -gap : 0.0;                          // up: only never go through it
}

// Smooth minimum of a and b: equals the smaller one away from the crossing, rounds the corner over
// about k units there (no velocity step where the cap takes over or lets go).
inline double SoftMin(double a, double b, double k) {
	const double m = a < b ? a : b;
	return m - k * std::log(1.0 + std::exp(-std::fabs(a - b) / k));
}
inline constexpr double kReboundSoftness = 1.5;

template<class SampleFn>
inline std::vector<float> BuildGroundTrack(double end, SampleFn sample) {
	std::vector<float> track;
	if (!(end > 0)) return track;
	const int n = int(end * kGroundRate) + 2;
	std::vector<double> ground(n), rebound(n), s(n);
	Pose pose[kBodies];
	double lowest = 1e9;
	for (int i = 0; i < n; ++i) {
		const double t = i / kGroundRate < end ? i / kGroundRate : end;
		sample(t, pose);
		// No rebound (kReboundAllowance), applied to the hip height directly as a soft minimum -- not
		// smoothed, so it cannot lag behind the climb it removes. The floor lift below is computed on
		// the capped pose.
		const double z = pose[kPelvis].p[2];
		if (z < lowest) lowest = z;
		rebound[i] = SoftMin(z, lowest + kReboundAllowance, kReboundSoftness) - z;
		if (rebound[i] > 0) rebound[i] = 0;
		for (int b = 0; b < kBodies; ++b) pose[b].p[2] = float(pose[b].p[2] + rebound[i]);
		ground[i] = GroundOffsetRaw(pose);
	}
	// A floor lift starts before the dip and ends after it (a moving maximum over ~3 time constants),
	// so the smoothed curve already covers it instead of being snapped up by the clamp below -- a pop.
	std::vector<double> in(n);
	const int w = int(3.0 * kGroundSmooth * kGroundRate);
	for (int i = 0; i < n; ++i) {
		double lift = 0;
		for (int j = i - w < 0 ? 0 : i - w; j <= i + w && j < n; ++j) if (ground[j] > lift) lift = ground[j];
		in[i] = lift > 0 ? lift : ground[i];
	}
	// Zero-phase: exponential smoothing forward, then backward.
	const double a = 1.0 - std::exp(-1.0 / (kGroundRate * kGroundSmooth));
	s[0] = in[0];
	for (int i = 1; i < n; ++i) s[i] = s[i - 1] + (in[i] - s[i - 1]) * a;
	for (int i = n - 2; i >= 0; --i) s[i] = s[i + 1] + (s[i] - s[i + 1]) * a;
	track.resize(n);
	for (int i = 0; i < n; ++i) {
		double v = s[i];
		if (ground[i] > 0 && v < ground[i] - kGroundSink) v = ground[i] - kGroundSink; // never sink more than that
		track[i] = float(v + rebound[i]);
	}
	// Starts at 0: the clip's first frame stands where the victim stood.
	const int ramp = int(0.15 * kGroundRate);
	for (int i = 0; i < n && i < ramp; ++i) track[i] = float(track[i] * (double(i) / ramp));
	return track;
}

inline double GroundOffsetAt(const std::vector<float>& track, double t) {
	if (track.empty()) return 0.0;
	double x = t * kGroundRate;
	if (x <= 0) return track.front();
	const size_t i = size_t(x);
	if (i + 1 >= track.size()) return track.back();
	const double f = x - double(i);
	return track[i] * (1.0 - f) + track[i + 1] * f;
}

// ---- fitting a clip to the map before it is picked --------------------------------------------
// The ground track handles a flat floor. Stairs, curbs, slopes and walls are the map's, so each
// candidate clip is probed against the map before it is chosen (DeathAnimations.cpp EnvironmentFit):
// kFitSamples moments of the clip, the floor under its hips, head, hands, calves and feet, and a
// sideways ray along the path its hips and head travel. The score is the deepest penetration left
// after the ground track (units); the first candidate within kFitTolerance plays, else the best one.
// Only the first few candidates are probed (kFitMaxTries), so picking never hitches.
inline constexpr int kFitSamples = 6;
inline constexpr double kFitTolerance = 3.0;
inline constexpr int kFitMaxTries = 8;
// A body at clip height `clipZ` over a local floor `localStep` units above the clip's floor: how
// deep it goes in, after the ground track has kept it at least its radius above the clip floor.
inline double FloorPenetration(double clipZ, double radius, double localStep) {
	const double above = clipZ - radius;
	const double pen = localStep - (above > 0 ? above : 0.0);
	return pen > 0 ? pen : 0.0;
}

// ---- grenade flight (exact playback) ----------------------------------------------------------
// A blast throws the body: an instant kick out and up, then air drag bleeds off the sideways speed
// (exponential, kBlastDrag per second) while gravity pulls it back down. The clip plays on top of
// that flight (smooth, no physics) and physics takes over when it comes back to the ground
// (BlastLandTime). A constant sideways speed read as a straight-line slide that went far too far.
inline constexpr double kGravity = 386.0;          // units/s^2 (CS2's sv_gravity 800 is players; ragdolls fall at ~386)
// Modest on purpose: many explosion clips animate their own launch, and the flight stacks on top.
// 260 out / 220 up read as flying far too fast (2026-09-26); this peaks ~29 units up, ~0.8 s in the
// air, ~55 units out.
inline constexpr double kBlastOut = 140.0, kBlastUp = 150.0; // u/s kick at Grenade blast 100 %
inline constexpr double kBlastDrag = 2.0;          // 1/s: sideways speed falls to 14 % in one second
// Grenade blast percent -> kick scale. Square root: 200 % is ~1.4x the kick (about twice the
// height), not 2x the speed and 4x the distance.
inline double BlastScale(double impact) { return impact > 0 ? std::sqrt(impact) : 0.0; }

// Solid playback, thrown by a blast: sideways speed decays like the exact-mode flight's air drag.
// CS2 launches an HE corpse at up to ~330 u/s sideways and nothing slowed it while the clip held
// the body in its pose -- it landed like a sled and slid across the map (2026-09-26). 2.5/s keeps a
// few hundred milliseconds of throw and leaves ~2 % of it after 1.5 s.
inline constexpr double kFlightDrag = 2.5;
// On the ground the throw has to stop, not coast: once the pelvis is within kGroundContactHeight of
// the floor the sideways speed decays at kGroundFriction instead (landed bodies still slid, 2026-09-26),
// and whatever is left when the clip lets go is cut to kHandoffSlide.
inline constexpr double kGroundFriction = 12.0;
inline constexpr double kGroundContactHeight = 14.0;
inline constexpr double kHandoffSlide = 0.2;
inline double FlightDragRate(double pelvisAboveFloor) {
	return pelvisAboveFloor < kGroundContactHeight ? kGroundFriction : kFlightDrag;
}
inline void FlightDrag(float linear[3], double dt, double rate = kFlightDrag) {
	const double k = std::exp(-rate * (dt > 0 ? dt : 0.0));
	linear[0] = float(linear[0] * k);
	linear[1] = float(linear[1] * k);
}
inline void BlastOffset(const double dir[2], double out, double up, double t, double offset[3]) {
	const double travel = out / kBlastDrag * (1.0 - std::exp(-kBlastDrag * t));
	offset[0] = dir[0] * travel;
	offset[1] = dir[1] * travel;
	const double z = up * t - 0.5 * kGravity * t * t;
	offset[2] = z > 0 ? z : 0.0;
}
inline double BlastLandTime(double up) { return up > 0 ? 2.0 * up / kGravity : 0.0; }

// ---- jitter measurement -----------------------------------------------------------------------
// Jitter is a body going back and forth. Counted as direction REVERSALS of its velocity between
// samples, only while it moves faster than kJitterMinSpeed (a body at rest wobbling by a fraction of a
// unit is not visible jitter). Sampled on the physics tick, so the 64 Hz stair-step of a body that
// only moves on physics steps never counts -- a staircase never reverses. Compared against the same
// count on the clip's own motion (a real flail reverses too).
inline constexpr double kJitterMinSpeed = 20.0;
struct ReversalCounter {
	float prev[3] = {}, prevV[3] = {};
	bool havePos = false, haveVel = false;
	int reversals = 0;
	void Add(const float p[3], double dt) {
		if (havePos && dt > 1e-5) {
			float v[3];
			double speed2 = 0, prevSpeed2 = 0, dot = 0;
			for (int k = 0; k < 3; ++k) {
				v[k] = float((p[k] - prev[k]) / dt);
				speed2 += double(v[k]) * v[k]; prevSpeed2 += double(prevV[k]) * prevV[k]; dot += double(v[k]) * prevV[k];
			}
			const double m2 = kJitterMinSpeed * kJitterMinSpeed;
			if (haveVel && speed2 > m2 && prevSpeed2 > m2 && dot < 0) ++reversals;
			std::memcpy(prevV, v, sizeof(prevV));
			haveVel = true;
		}
		std::memcpy(prev, p, sizeof(prev));
		havePos = true;
	}
};

// Balance. The angle (degrees) between the body's actual torso line (pelvis -> head) and the clip's.
// Past kBalanceLossDegrees physics has tipped the body where the clip does not go, and the clip lets
// go: an active ragdoll steering it back looks like a body fighting to stand up.
inline constexpr double kBalanceLossDegrees = 35.0;
inline double TorsoTilt(const float pelvis[3], const float head[3], const float clipPelvis[3], const float clipHead[3]) {
	double a[3], b[3], na = 0, nb = 0, dot = 0;
	for (int k = 0; k < 3; ++k) {
		a[k] = double(head[k]) - pelvis[k]; b[k] = double(clipHead[k]) - clipPelvis[k];
		na += a[k] * a[k]; nb += b[k] * b[k]; dot += a[k] * b[k];
	}
	if (na < 1e-6 || nb < 1e-6) return 0.0;
	double c = dot / std::sqrt(na * nb);
	c = c > 1 ? 1 : (c < -1 ? -1 : c);
	return std::acos(c) * 57.29577951308232;
}

// Grenade blast: the only death that throws the body, with the same modest kick as the exact-mode
// flight (kBlastOut / kBlastUp), scaled by BlastScale. Solid playback REPLACES CS2's own launch
// (measured 2026-09-26: 73-299 u/s up, 180-320 sideways) with it. Bullet deaths get no push.
inline double ImpactSpeed(const DeathFacts& f) { return f.explosive ? kBlastOut : 0.0; }
inline double ImpactLift(const DeathFacts& f) { return f.explosive ? kBlastUp : 0.0; }

// Spin a blast launch keeps (and any flying body is written back with), degrees/s.
inline constexpr double kLaunchSpin = 720.0;
// Below this engine launch speed (u/s, sideways) its direction is noise; use the attacker instead.
inline constexpr double kBlastDirMinSpeed = 30.0;

// The limb a shot landed on goes limp for the whole clip (the rest of the animation plays): 0 left arm,
// 1 right arm, 2 left leg, 3 right leg, -1 for head / torso hits.
inline int ShotLimb(int hitgroup) {
	switch (hitgroup) { case 4: return 0; case 5: return 1; case 6: return 2; case 7: return 3; default: return -1; }
}

// The body part a CS2 hitgroup lands on: 1 head, 2 chest, 3 stomach, 4/5 arms, 6/7 legs, 8 neck.
inline int HitBody(int hitgroup) {
	switch (hitgroup) {
	case 1: case 8: return kHeadBody;
	case 2: return kSpine2Body;
	case 3: return kPelvis;
	case 4: return 8;   // left forearm
	case 5: return 10;  // right forearm
	case 6: return 1;   // left calf
	case 7: return 3;   // right calf
	default: return -1;
	}
}

// ---- where a clip's motion ends ----------------------------------------------------------------
// Many clips finish with the body lying still for half a second or more. Playing that tail holds the
// body in the clip's final pose -- often above where the CS2 ragdoll would rest -- and then drops it
// when the clip ends. The clip is treated as ending where its motion does: the last moment any body
// still moves faster than `speed` (units/s), sampled at `rate` Hz, never earlier than `minimum` s.
// The blend out then fades the clip over its last real motion instead of over the still tail.
inline constexpr double kRestSpeed = 20.0;

// When the clip's body lands: the first moment (after `earliest` s) the torso -- pelvis, spine, head
// -- is within `margin` units of the height it ends at, for a clip that ends LYING (pelvis below
// `lying` units, clip floor = 0). -1 for a clip that ends kneeling or standing. The clip's lying pose
// often sits a few units above the CS2 floor; playing on after the landing holds the corpse floating
// there. Handing off at the landing lets the ragdoll drop that last bit while it is still moving.
template<class SampleFn>
inline double LandTime(double end, SampleFn sample, double margin = 3.0, double lying = 18.0, double earliest = 0.2, double rate = 60.0) {
	if (!(end > 0)) return -1;
	static constexpr int kTorso[3] = {kPelvis, kSpine2Body, kHeadBody};
	Pose last[kBodies], cur[kBodies];
	sample(end, last);
	if (!(last[kPelvis].p[2] < lying)) return -1;
	for (int f = int(earliest * rate); f / rate < end; ++f) {
		const double t = f / rate;
		sample(t, cur);
		bool down = true;
		for (int b : kTorso) if (cur[b].p[2] - last[b].p[2] > margin) { down = false; break; }
		if (down) return t;
	}
	return end;
}
template<class SampleFn>
inline double MotionEndTime(double duration, SampleFn sample, double speed = kRestSpeed, double rate = 60.0, double minimum = 0.4) {
	if (!(duration > 0)) return duration;
	const int steps = int(duration * rate);
	Pose prev[kBodies], cur[kBodies];
	sample(0.0, prev);
	double last = 0;
	for (int f = 1; f <= steps + 1; ++f) {
		const double t = f <= steps ? f / rate : duration;
		sample(t, cur);
		const double dt = t - (f - 1) / rate;
		if (dt <= 1e-6) break;
		for (int b = 0; b < kBodies; ++b) {
			double d2 = 0;
			for (int k = 0; k < 3; ++k) d2 += double(cur[b].p[k] - prev[b].p[k]) * (cur[b].p[k] - prev[b].p[k]);
			if (std::sqrt(d2) / dt > speed) { last = t; break; }
		}
		std::memcpy(prev, cur, sizeof(prev));
	}
	if (last < minimum) last = minimum;
	return last < duration ? last : duration;
}

}} // namespace Filmmaker::DeathAnimationsMath
