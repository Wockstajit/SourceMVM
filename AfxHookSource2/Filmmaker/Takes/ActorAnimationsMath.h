#pragma once

// Engine-free math, MVMACT02 binary pack loader, cue timeline evaluator (speed, looping,
// start time, duration, blending, crossfade transitions), 21-bone CS2 skeleton pose application,
// and MVMSEQ01 demo-companion sequence serialization for Actor Performance Animations.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

namespace Filmmaker::ActorAnim {

inline constexpr int kCoreBones = 51;
inline constexpr int kMaxSkeletonBones = 160;
inline constexpr int kCategoryCount = 7;

enum class Category : uint8_t {
	Locomotion = 0,
	Crouch = 1,
	Weapon = 2,
	Gesture = 3,
	Idle = 4,
	Reaction = 5,
	Cinematic = 6,
};

inline const char *CategoryName(uint8_t c) {
	static const char *const kNames[kCategoryCount] = {
	    "Locomotion", "Crouch", "Aim & Weapon", "Gestures", "Idle", "Reactions", "Cinematic"};
	return c < kCategoryCount ? kNames[c] : "Other";
}

struct Pose {
	float p[3] = {0.f, 0.f, 0.f};
	float q[4] = {0.f, 0.f, 0.f, 1.f}; // xyzw
};

// The core bone a limb bone points at, for direction retargeting. -1: not corrected (pelvis, spine,
// head, fingers, and the end bones, which follow their parent's correction).
inline constexpr int kCoreChild[kCoreBones] = {
    -1, -1, -1, -1, -1, -1, -1, 8, 9, 10, -1, 12, 13, 14, -1, 16, 17, -1, 19, 20, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
// The limb bone whose correction an end bone (hand, fingers, ankle) rides along with.
inline constexpr int kCoreEndParent[kCoreBones] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 9, -1, -1, -1, 13, -1, -1, 16, -1, -1, 19,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13};

// Selects the matching clean 8-way directional locomotion clip from the player's local forward/right velocity.
inline bool HasPrefix(const std::string &s, const char *prefix) {
	const size_t n = std::strlen(prefix);
	return s.size() >= n && s.compare(0, n, prefix) == 0;
}

inline bool IsDirectionalStopClip(const std::string &clip) {
	return clip == "hla_walk_stop_fwd" || clip == "hla_walk_stop_back" ||
	       clip == "hla_walk_stop_l" || clip == "hla_walk_stop_r";
}

inline bool IsDirectionalClip(const std::string &clip) {
	static const char *const kSuffixes[8] = {
	    "fwd", "fwd_r", "strafe_r", "back_r", "back", "back_l", "strafe_l", "fwd_l"};
	static const char *const kPrefixes[7] = {
	    "hla_walk_", "hla_run_", "hla_heavy_run_", "hla_crouch_walk_", "hla_sup_sprint_", "hla_mc_walk_", "hla_mc_run_"};
	if (IsDirectionalStopClip(clip)) return true;
	for (const char *pre : kPrefixes) {
		if (!HasPrefix(clip, pre)) continue;
		const std::string tail = clip.substr(std::strlen(pre));
		for (const char *suf : kSuffixes)
			if (tail == suf) return true;
	}
	return false;
}

// Only the primary forward locomotion clips opt into live WASD 8-way directional switching;
// specific angled clips (e.g. `hla_walk_fwd_r`) and non-locomotion idles (`hla_aim_to_ready`) play verbatim.
inline bool IsDirectionalBaseClip(const std::string &clip) {
	return clip == "hla_walk_fwd" || clip == "hla_run_fwd" || clip == "hla_crouch_walk_fwd" ||
	       clip == "hla_sup_sprint_fwd" || clip == "hla_heavy_run_fwd" ||
	       clip == "hla_mc_walk_fwd" || clip == "hla_mc_run_fwd";
}

inline std::string SelectDirectionalStopClip(float lastFwd, float lastRight) {
	if (std::fabs(lastRight) > std::fabs(lastFwd) * 1.15f)
		return lastRight > 0.f ? "hla_walk_stop_r" : "hla_walk_stop_l";
	return lastFwd < 0.f ? "hla_walk_stop_back" : "hla_walk_stop_fwd";
}

inline std::string DirectionalPrefixForClip(const std::string &currentClip, bool isWalking, bool allowIdle, bool isDucking) {
	if (isDucking) return "hla_crouch_walk";
	if (HasPrefix(currentClip, "hla_sup_sprint_") && !isWalking) return "hla_sup_sprint";
	if (HasPrefix(currentClip, "hla_heavy_run_") && !isWalking) return "hla_heavy_run";
	if (HasPrefix(currentClip, "hla_mc_")) return isWalking ? "hla_mc_walk" : "hla_mc_run";
	if (isWalking) return "hla_walk";
	return allowIdle ? "hla_run" : "hla_run";
}

inline std::string SelectDirectionalClip(const std::string &currentClip, float fwdSpeed, float rightSpeed,
                                         bool isWalking = false, bool allowIdle = false, bool isDucking = false) {
	const bool isDirSet = IsDirectionalClip(currentClip) ||
	                      (allowIdle && (currentClip == "hla_combat_idle" || currentClip == "hla_crouch_idle"));
	if (!isDirSet) return currentClip;
	const float speed = std::sqrt(fwdSpeed * fwdSpeed + rightSpeed * rightSpeed);
	if (speed < 10.f) return allowIdle ? (isDucking ? "hla_crouch_idle" : "hla_combat_idle") : currentClip;
	static const char *const kOctants[8] = {
	    "_fwd", "_fwd_r", "_strafe_r", "_back_r", "_back", "_back_l", "_strafe_l", "_fwd_l"};
	constexpr float kQuarterPi = 0.78539816339f;
	constexpr float kEighthPi = 0.3926990817f;
	const float ang = std::atan2(rightSpeed, fwdSpeed); // 0=+fwd, +pi/2=+right, pi=back, -pi/2=+left
	const int oct = ((int(std::floor((ang + kEighthPi) / kQuarterPi)) % 8) + 8) % 8;
	std::string prefix = DirectionalPrefixForClip(currentClip, isWalking, allowIdle, isDucking);
	if (!isDucking && !HasPrefix(currentClip, "hla_sup_sprint_") && !HasPrefix(currentClip, "hla_heavy_run_") &&
	    !HasPrefix(currentClip, "hla_mc_") && !isWalking && !allowIdle) {
		prefix = (speed > 120.f) ? "hla_run" : "hla_walk";
	}
	return prefix + kOctants[oct];
}

// Continuous MoveHeading angular blend (HL:A CDirectionalBlendAnimNode equivalent): returns the
// two bracketing octant clip IDs and blend weight in [0, 1] for smooth strafe/diagonal transitions.
inline bool SelectDirectionalBlendPair(const std::string &currentClip, float fwdSpeed, float rightSpeed,
                                       bool isWalking, bool isDucking,
                                       std::string &outClipA, std::string &outClipB, float &outBlendT) {
	const float speed = std::sqrt(fwdSpeed * fwdSpeed + rightSpeed * rightSpeed);
	if (speed < 10.f) return false;
	static const char *const kOctants[8] = {
	    "_fwd", "_fwd_r", "_strafe_r", "_back_r", "_back", "_back_l", "_strafe_l", "_fwd_l"};
	constexpr float kQuarterPi = 0.78539816339f;
	const float ang = std::atan2(rightSpeed, fwdSpeed);
	float f = ang / kQuarterPi;
	f = std::fmod(f, 8.f);
	if (f < 0.f) f += 8.f;
	const int idxA = int(std::floor(f)) % 8;
	const int idxB = (idxA + 1) % 8;
	outBlendT = std::clamp(f - std::floor(f), 0.f, 1.f);
	const std::string prefix = DirectionalPrefixForClip(currentClip, isWalking, /*allowIdle=*/true, isDucking);
	outClipA = prefix + kOctants[idxA];
	outClipB = prefix + kOctants[idxB];
	return true;
}
inline constexpr int kCoreArmUpperL = 8;
inline constexpr int kCoreArmLowerL = 9;
inline constexpr int kCoreHandL = 10;
inline constexpr int kCoreArmUpperR = 12;
inline constexpr int kCoreArmLowerR = 13;
inline constexpr int kCoreHandR = 14;
inline constexpr int kCoreAnkleL = 17;
inline constexpr int kCoreAnkleR = 20;
inline constexpr int kCoreFingerLStart = 21;
inline constexpr int kCoreFingerLEnd = 35;
inline constexpr int kCoreFingerRStart = 36;
inline constexpr int kCoreFingerREnd = 50;

// What the actor is posed for: the Alyx clips hold a Combine rifle in both hands.
enum class WeaponClass : uint8_t { None = 0, Rifle = 1, Pistol = 2, Grenade = 3 };

inline const char *WeaponClassName(uint8_t w) {
	switch (w) {
	case 1: return "Rifle";
	case 2: return "Pistol";
	case 3: return "Grenade";
	default: return "No weapon";
	}
}

struct ClipFrame {
	float pelvisDelta[3] = {0.f, 0.f, 0.f};
	float groundLift = 0.f;             // how far the clip's lowest foot is above the standing floor, inches
	float rotDelta[kCoreBones][4] = {}; // xyzw world-space rotation delta from the bind pose in CS2 space
	float dir[kCoreBones][3] = {};      // world direction each limb bone points (toward its child), CS2 space
};

struct Clip {
	std::string id;
	std::string displayName;
	uint8_t category = 0;
	uint8_t weaponClass = 0;
	bool loopable = false;
	bool hasRootMotion = false;
	float fps = 30.f;
	float pelvisDrop = 0.f;
	float rootSpeed = 0.f; // inches per second the clip's root travels along the ground (0: stays put)
	std::vector<ClipFrame> frames;

	float Duration() const {
		return (frames.size() > 1 && fps > 0.f) ? float(frames.size() - 1) / fps : 0.f;
	}
};

// Recommended `sv_maxspeed` for the local player while a clip plays, ported from Half-Life: Alyx
// AnimGraph MoveSpeed breakpoints (combine_soldier_new_content_procedural_rebuild & live sweep_npc_1):
//  - > 0: authentic HL:A pace per stance/family scaled by speedScale (69.6 walk, 139.2 jog,
//         180 sprint, 76 crouch walk, 105 heavy jog; or clip's measured rootSpeed)
//  - -1: leave engine `sv_maxspeed` untouched (never freezes WASD on stationary/reaction/gesture clips)
inline float RecommendedMaxSpeedForClip(const Clip &clip, bool directionalMode, bool isWalking = false,
                                        bool isDucking = false, bool upperBodyOnly = false,
                                        float speedScale = 1.0f) {
	if (upperBodyOnly) return -1.f;
	const float sc = std::clamp(speedScale, 0.1f, 3.0f);
	if (directionalMode) {
		float base = 139.2f;
		if (isDucking) base = 76.f;
		else if (isWalking) base = HasPrefix(clip.id, "hla_mc_") ? 56.f : 69.6f;
		else if (HasPrefix(clip.id, "hla_sup_sprint_")) base = 180.f;
		else if (HasPrefix(clip.id, "hla_heavy_run_")) base = 105.f;
		return std::clamp(base * sc, 25.f, 320.f);
	}
	if (clip.category == uint8_t(Category::Locomotion) && clip.rootSpeed > 10.f)
		return std::clamp(clip.rootSpeed * sc, 25.f, 320.f);
	return -1.f;
}

struct Library {
	std::array<std::string, kCoreBones> boneNames;
	Pose csBind[kCoreBones];  // the CS2 agent's bind pose (model space, CS2 axes), not whatever pose is live
	bool hasBind = false;
	std::vector<Clip> clips;
	std::string status = "not loaded";

	const Clip *Find(const std::string &id) const {
		for (const auto &c : clips)
			if (c.id == id) return &c;
		return nullptr;
	}
};

struct Cue {
	int startTick = 0;      // 64-tick server/demo tick relative to take start
	std::string clip;       // Clip::id
	float speed = 1.0f;     // 0.1 .. 3.0
	bool loop = false;
	bool upperBodyOnly = false; // blend onto upper body (spine feathered) while legs follow CS2 AnimGraph2
	float startTime = 0.0f; // seconds offset into clip
	float duration = 0.0f;  // 0 = full clip length (scaled by speed); >0 = explicit play duration
	float blendIn = 0.2f;   // seconds to blend in
	float blendOut = 0.25f; // seconds to blend out
};

struct SampledPose {
	bool active = false;
	float weight = 0.f; // 0..1 blend weight over CS2 AnimGraph2 pose
	uint8_t weaponClass = 1; // WeaponClass: 0=None, 1=Rifle, 2=Pistol, 3=Grenade
	uint8_t category = 0;    // Category
	bool isReload = false;
	bool upperBodyOnly = false;
	float pelvisDelta[3] = {0.f, 0.f, 0.f};
	float groundLift = 0.f;
	float rotDelta[kCoreBones][4] = {};
	float dir[kCoreBones][3] = {};
};

// -- Quaternion & vector helpers -------------------------------------------------------------------

inline void QuatIdentity(float out[4]) {
	out[0] = 0.f; out[1] = 0.f; out[2] = 0.f; out[3] = 1.f;
}

inline void QuatNorm(const float q[4], float out[4]) {
	const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (n < 1e-8f) { QuatIdentity(out); return; }
	const float inv = 1.f / n;
	out[0] = q[0] * inv; out[1] = q[1] * inv; out[2] = q[2] * inv; out[3] = q[3] * inv;
}

inline void QuatConj(const float q[4], float out[4]) {
	out[0] = -q[0]; out[1] = -q[1]; out[2] = -q[2]; out[3] = q[3];
}

inline void QuatMul(const float a[4], const float b[4], float out[4]) {
	const float r[4] = {
	    a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
	    a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
	    a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
	    a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
	for (int i = 0; i < 4; ++i) out[i] = r[i];
}

inline void QuatRotate(const float q[4], const float v[3], float out[3]) {
	const float tx = 2.f * (q[1] * v[2] - q[2] * v[1]);
	const float ty = 2.f * (q[2] * v[0] - q[0] * v[2]);
	const float tz = 2.f * (q[0] * v[1] - q[1] * v[0]);
	out[0] = v[0] + q[3] * tx + q[1] * tz - q[2] * ty;
	out[1] = v[1] + q[3] * ty + q[2] * tx - q[0] * tz;
	out[2] = v[2] + q[3] * tz + q[0] * ty - q[1] * tx;
}

inline void QuatSlerp(const float a[4], const float bIn[4], float t, float out[4]) {
	t = std::clamp(t, 0.f, 1.f);
	float b[4] = {bIn[0], bIn[1], bIn[2], bIn[3]};
	float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	if (dot < 0.f) {
		for (int i = 0; i < 4; ++i) b[i] = -b[i];
		dot = -dot;
	}
	if (dot > 0.9995f) {
		float r[4];
		for (int i = 0; i < 4; ++i) r[i] = a[i] + t * (b[i] - a[i]);
		QuatNorm(r, out);
		return;
	}
	const float th0 = std::acos(std::clamp(dot, -1.f, 1.f));
	const float th = th0 * t;
	const float sin0 = std::sin(th0);
	const float s0 = std::cos(th) - dot * std::sin(th) / sin0;
	const float s1 = std::sin(th) / sin0;
	float r[4];
	for (int i = 0; i < 4; ++i) r[i] = s0 * a[i] + s1 * b[i];
	QuatNorm(r, out);
}

inline float SmoothStep01(float x) {
	x = std::clamp(x, 0.f, 1.f);
	return x * x * (3.f - 2.f * x);
}

// -- MVMACT01 pack loader --------------------------------------------------------------------------

inline bool LoadPack(const uint8_t *data, size_t size, Library &out) {
	out = Library{};
	if (!data || size < 20) { out.status = "file too small"; return false; }
	if (std::memcmp(data, "MVMACT02", 8) != 0) { out.status = "bad magic"; return false; }
	uint32_t version = 0, boneCount = 0, clipCount = 0;
	std::memcpy(&version, data + 8, 4);
	std::memcpy(&boneCount, data + 12, 4);
	std::memcpy(&clipCount, data + 16, 4);
	if (version != 2 || boneCount != kCoreBones || clipCount > 4096) {
		out.status = "unsupported version or bone count";
		return false;
	}
	size_t off = 20;
	if (off + size_t(kCoreBones) * 24 > size) { out.status = "truncated bone table"; return false; }
	for (int b = 0; b < kCoreBones; ++b) {
		char name[25] = {};
		std::memcpy(name, data + off, 24);
		out.boneNames[b] = name;
		off += 24;
	}
	if (off + size_t(kCoreBones) * 28 > size) { out.status = "truncated bind pose"; return false; }
	for (int b = 0; b < kCoreBones; ++b) {
		std::memcpy(out.csBind[b].p, data + off, 12); off += 12;
		std::memcpy(out.csBind[b].q, data + off, 16); off += 16;
		QuatNorm(out.csBind[b].q, out.csBind[b].q);
	}
	out.hasBind = true;
	out.clips.reserve(clipCount);
	constexpr size_t kFrameBytes = (4 + kCoreBones * 4 + kCoreBones * 3) * sizeof(float);
	for (uint32_t i = 0; i < clipCount; ++i) {
		if (off + 148 > size) { out.status = "truncated clip header"; return false; }
		Clip c;
		char idBuf[65] = {}, dispBuf[65] = {};
		std::memcpy(idBuf, data + off, 64); off += 64;
		std::memcpy(dispBuf, data + off, 64); off += 64;
		c.id = idBuf;
		c.displayName = dispBuf;
		c.category = data[off];
		c.weaponClass = data[off + 2];
		const uint8_t flags = data[off + 1];
		c.loopable = (flags & 1) != 0;
		c.hasRootMotion = (flags & 2) != 0;
		off += 4;
		uint32_t frameCount = 0;
		std::memcpy(&c.fps, data + off, 4); off += 4;
		std::memcpy(&frameCount, data + off, 4); off += 4;
		std::memcpy(&c.pelvisDrop, data + off, 4); off += 4;
		std::memcpy(&c.rootSpeed, data + off, 4); off += 4;
		if (frameCount < 2 || frameCount > 36000 || off + size_t(frameCount) * kFrameBytes > size) {
			out.status = "truncated clip frames";
			return false;
		}
		c.frames.resize(frameCount);
		for (uint32_t f = 0; f < frameCount; ++f) {
			std::memcpy(c.frames[f].pelvisDelta, data + off, 12); off += 12;
			std::memcpy(&c.frames[f].groundLift, data + off, 4); off += 4;
			std::memcpy(c.frames[f].rotDelta, data + off, kCoreBones * 16); off += kCoreBones * 16;
			std::memcpy(c.frames[f].dir, data + off, kCoreBones * 12); off += kCoreBones * 12;
		}
		out.clips.push_back(std::move(c));
	}
	out.status = "loaded " + std::to_string(out.clips.size()) + " clips";
	return true;
}

// -- Clip sampling & cue timeline evaluation -------------------------------------------------------

inline void SampleClip(const Clip &clip, float clipTimeSec, bool loop, float pelvisOut[3],
                       float rotOut[kCoreBones][4], float dirOut[kCoreBones][3] = nullptr,
                       float *liftOut = nullptr) {
	const float dur = clip.Duration();
	if (clip.frames.empty() || dur <= 1e-6f) {
		for (int k = 0; k < 3; ++k) pelvisOut[k] = 0.f;
		for (int b = 0; b < kCoreBones; ++b) QuatIdentity(rotOut[b]);
		if (dirOut) std::memset(dirOut, 0, sizeof(float) * kCoreBones * 3);
		if (liftOut) *liftOut = 0.f;
		return;
	}
	float t = clipTimeSec;
	if (loop) {
		t = std::fmod(t, dur);
		if (t < 0.f) t += dur;
	} else {
		t = std::clamp(t, 0.f, dur);
	}
	const float fPos = t * clip.fps;
	const size_t f0 = std::min<size_t>(size_t(std::floor(fPos)), clip.frames.size() - 1);
	const size_t f1 = std::min<size_t>(f0 + 1, clip.frames.size() - 1);
	const float alpha = std::clamp(fPos - float(f0), 0.f, 1.f);
	const auto &a = clip.frames[f0];
	const auto &b = clip.frames[f1];
	for (int k = 0; k < 3; ++k)
		pelvisOut[k] = a.pelvisDelta[k] + alpha * (b.pelvisDelta[k] - a.pelvisDelta[k]);
	for (int bi = 0; bi < kCoreBones; ++bi)
		QuatSlerp(a.rotDelta[bi], b.rotDelta[bi], alpha, rotOut[bi]);
	if (dirOut) {
		for (int bi = 0; bi < kCoreBones; ++bi) {
			float d[3];
			for (int k = 0; k < 3; ++k) d[k] = a.dir[bi][k] + alpha * (b.dir[bi][k] - a.dir[bi][k]);
			const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			for (int k = 0; k < 3; ++k) dirOut[bi][k] = n > 1e-5f ? d[k] / n : 0.f;
		}
	}
	if (liftOut) *liftOut = a.groundLift + alpha * (b.groundLift - a.groundLift);
}

// Where the clip's root has travelled to after `tSec` of the cue, relative to its first frame, in the
// clip's own frame (x is the way the actor faces). Loops add one cycle's travel each time round.
inline void RootMotionOffset(const Clip &clip, const Cue &cue, double tSec, float out[2]) {
	out[0] = out[1] = 0.f;
	const float dur = clip.Duration();
	if (clip.frames.size() < 2 || dur <= 1e-6f) return;
	const float spd = std::clamp(cue.speed, 0.1f, 3.0f);
	double clipT = double(std::max(0.f, cue.startTime)) + tSec * double(spd);
	const auto at = [&](double t, float xy[2]) {
		t = std::clamp(t, 0.0, double(dur));
		const double fPos = t * double(clip.fps);
		const size_t f0 = std::min<size_t>(size_t(std::floor(fPos)), clip.frames.size() - 1);
		const size_t f1 = std::min<size_t>(f0 + 1, clip.frames.size() - 1);
		const float a = float(std::clamp(fPos - double(f0), 0.0, 1.0));
		for (int k = 0; k < 2; ++k)
			xy[k] = clip.frames[f0].pelvisDelta[k] + a * (clip.frames[f1].pelvisDelta[k] - clip.frames[f0].pelvisDelta[k]);
	};
	float first[2], last[2], cur[2];
	at(0.0, first);
	at(double(dur), last);
	double cycles = 0.0;
	if (cue.loop) {
		cycles = std::floor(clipT / double(dur));
		clipT -= cycles * double(dur);
	}
	at(clipT, cur);
	for (int k = 0; k < 2; ++k)
		out[k] = float(cycles * double(last[k] - first[k]) + double(cur[k] - first[k]));
}

// Instantaneous clip-local horizontal root velocity (in/s, +X forward, +Y left) at clipTime.
inline void SampleClipRootVelocity(const Clip &clip, double tSec, float speed, bool loop, float outVelXY[2]) {
	outVelXY[0] = outVelXY[1] = 0.f;
	const float dur = clip.Duration();
	if (clip.frames.size() < 2 || clip.fps <= 0.f || dur <= 1e-5f) return;
	const float spd = std::clamp(speed, 0.1f, 3.0f);
	double clipT = tSec * double(spd);
	if (!loop && (clipT < 0.0 || clipT >= double(dur))) return;
	const double dt = 1.0 / 60.0;
	Cue one;
	one.speed = spd;
	one.loop = loop;
	float p0[2] = {}, p1[2] = {};
	const double tA = std::max(0.0, tSec);
	const double tB = tA + dt;
	RootMotionOffset(clip, one, tA, p0);
	RootMotionOffset(clip, one, tB, p1);
	outVelXY[0] = float((double(p1[0]) - double(p0[0])) / dt);
	outVelXY[1] = float((double(p1[1]) - double(p0[1])) / dt);
}

inline float CueEffectiveDuration(const Cue &cue, const Clip &clip) {
	const float spd = std::clamp(cue.speed, 0.1f, 3.0f);
	if (cue.duration > 0.f) return cue.duration;
	if (cue.loop) return 1e9f; // infinite until next cue or stop
	const float remaining = std::max(0.05f, clip.Duration() - std::max(0.f, cue.startTime));
	return remaining / spd;
}

inline SampledPose BlendSampledPoses(const SampledPose &a, const SampledPose &b, float alpha) {
	alpha = std::clamp(alpha, 0.f, 1.f);
	if (alpha <= 1e-4f || !b.active) return a;
	if (alpha >= 1.f - 1e-4f || !a.active) return b;
	SampledPose out;
	out.active = a.active || b.active;
	out.weight = (std::max)(a.weight, b.weight);
	const SampledPose &meta = alpha < 0.5f ? a : b;
	out.weaponClass = meta.weaponClass;
	out.category = meta.category;
	out.isReload = meta.isReload;
	out.upperBodyOnly = a.upperBodyOnly && b.upperBodyOnly ? true : meta.upperBodyOnly;
	for (int k = 0; k < 3; ++k)
		out.pelvisDelta[k] = a.pelvisDelta[k] * (1.f - alpha) + b.pelvisDelta[k] * alpha;
	for (int i = 0; i < kCoreBones; ++i) {
		QuatSlerp(a.rotDelta[i], b.rotDelta[i], alpha, out.rotDelta[i]);
		float d[3];
		for (int k = 0; k < 3; ++k) d[k] = a.dir[i][k] * (1.f - alpha) + b.dir[i][k] * alpha;
		const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		for (int k = 0; k < 3; ++k) out.dir[i][k] = n > 1e-5f ? d[k] / n : 0.f;
	}
	out.groundLift = a.groundLift * (1.f - alpha) + b.groundLift * alpha;
	return out;
}

inline SampledPose EvaluateCues(const Library &lib, const std::vector<Cue> &cues, double elapsedSec,
                                double tickRate = 64.0) {
	SampledPose out;
	for (int b = 0; b < kCoreBones; ++b) QuatIdentity(out.rotDelta[b]);
	if (cues.empty() || lib.clips.empty()) return out;

	// Evaluate up to two overlapping cues for smooth crossfading: the latest started cue and
	// the immediately preceding cue if it is still within its blend-out window.
	int activeIdx = -1;
	double bestStart = -1e18;
	for (int i = 0; i < (int)cues.size(); ++i) {
		const double st = double(cues[i].startTick) / tickRate;
		if (st <= elapsedSec && st >= bestStart) {
			bestStart = st;
			activeIdx = i;
		}
	}
	if (activeIdx < 0) return out;

	auto evalOne = [&](int idx, SampledPose &res) -> bool {
		if (idx < 0 || idx >= (int)cues.size()) return false;
		const Cue &cue = cues[idx];
		const Clip *clip = lib.Find(cue.clip);
		if (!clip) return false;
		const double localT = elapsedSec - double(cue.startTick) / tickRate;
		if (localT < 0.0) return false;
		const float effDur = CueEffectiveDuration(cue, *clip);
		if (localT > double(effDur)) return false;
		const float spd = std::clamp(cue.speed, 0.1f, 3.0f);
		const float clipT = std::max(0.f, cue.startTime) + float(localT) * spd;
		SampleClip(*clip, clipT, cue.loop, res.pelvisDelta, res.rotDelta, res.dir, &res.groundLift);
		res.weaponClass = clip->weaponClass;
		res.category = clip->category;
		res.isReload = clip->id.find("reload") != std::string::npos;
		res.upperBodyOnly = cue.upperBodyOnly;
		float wIn = cue.blendIn > 1e-4f ? SmoothStep01(float(localT) / cue.blendIn) : 1.f;
		float wOut = 1.f;
		if (!cue.loop || cue.duration > 0.f) {
			const float left = effDur - float(localT);
			wOut = cue.blendOut > 1e-4f ? SmoothStep01(left / cue.blendOut) : 1.f;
		}
		res.weight = std::clamp(std::min(wIn, wOut), 0.f, 1.f);
		// Active for the whole cue, even where the blend weight is 0 (the first frame of a blend-in):
		// the caller ends the cue when it runs out of time, not when it fades.
		res.active = true;
		return true;
	};

	SampledPose cur;
	if (!evalOne(activeIdx, cur)) return out;

	// Check for crossfade from the previous cue if current cue is still blending in
	const Cue &curCue = cues[activeIdx];
	const double curLocalT = elapsedSec - double(curCue.startTick) / tickRate;
	if (activeIdx > 0 && curCue.blendIn > 1e-4f && curLocalT < double(curCue.blendIn)) {
		SampledPose prev;
		if (evalOne(activeIdx - 1, prev)) {
			const float alpha = SmoothStep01(float(curLocalT) / curCue.blendIn);
			out = BlendSampledPoses(prev, cur, alpha);
			out.weaponClass = cur.weaponClass;
			out.category = cur.category;
			out.isReload = cur.isReload;
			out.upperBodyOnly = cur.upperBodyOnly;
			return out;
		}
	}
	return cur;
}

// -- Apply sampled pose onto a CS2 skeleton's parent-local pose ------------------------------------

inline void ComposePose(const Pose &parent, const Pose &local, Pose &out) {
	float rp[3];
	QuatRotate(parent.q, local.p, rp);
	for (int k = 0; k < 3; ++k) out.p[k] = parent.p[k] + rp[k];
	QuatMul(parent.q, local.q, out.q);
	QuatNorm(out.q, out.q);
}

inline void LocalFromWorld(const Pose &parent, const Pose &world, Pose &local) {
	float invQ[4], dp[3];
	QuatConj(parent.q, invQ);
	for (int k = 0; k < 3; ++k) dp[k] = world.p[k] - parent.p[k];
	QuatRotate(invQ, dp, local.p);
	QuatMul(invQ, world.q, local.q);
	QuatNorm(local.q, local.q);
}

// Applies SampledPose (21-bone world rotation deltas + pelvis offset) onto a live CS2 skeleton.
// - `restWorld[count]`: model-space rest pose captured for this CS2 model
// - `liveLocal[count]`: current AnimGraph2 parent-local pose (in/out: overwritten with blended pose)
// - `parents[count]`: parent bone index (-1 for root)
// - `boneToCore[count]`: maps skeleton bone index -> [0..kCoreBones-1] or -1
// - `applyRootXY`: false strips horizontal pelvis translation (for moving bots), true keeps it
// Shortest-arc rotation taking unit vector `a` to unit vector `b`.
inline void QuatFromTo(const float a[3], const float b[3], float out[4]) {
	const float dotv = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	if (dotv < -0.9999f) {
		float ax[3] = {a[1], -a[0], 0.f};
		if (ax[0] * ax[0] + ax[1] * ax[1] < 1e-6f) { ax[0] = 0.f; ax[1] = a[2]; ax[2] = -a[1]; }
		const float al = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
		out[0] = ax[0] / al; out[1] = ax[1] / al; out[2] = ax[2] / al; out[3] = 0.f;
		return;
	}
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
	out[3] = 1.f + dotv;
	QuatNorm(out, out);
}

// Height of the ground under a model-space point, in model space (the entity's floor is z = 0).
// footIndex: 0 = left ankle, 1 = right ankle.
using GroundProbeFn = bool (*)(void *user, int footIndex, const float modelPos[3], float *groundZ);

// Filters raw MapCollisionGrid ground deltas (relative to the entity's physics origin Z) so
// uncached physics props/manholes or deep floor drops below the server hull never pull the body underground.
inline void SanitizeFootGroundProbes(const bool rawHit[2], const float rawGz[2],
                                     bool outHit[2], float outGz[2]) {
	bool ok[2] = {false, false};
	float gz[2] = {0.f, 0.f};
	for (int l = 0; l < 2; ++l) {
		if (rawHit[l] && std::isfinite(rawGz[l]) && rawGz[l] >= -11.f && rawGz[l] <= 6.f) {
			float v = std::min(rawGz[l], 2.5f);
			if (std::fabs(v) < 1.25f) v = 0.f;
			ok[l] = true;
			gz[l] = v;
		}
	}
	if (!ok[0] && !ok[1]) {
		outHit[0] = outHit[1] = true;
		outGz[0] = outGz[1] = 0.f;
		return;
	}
	if (ok[0] && ok[1]) {
		if (std::max(gz[0], gz[1]) < -2.5f) {
			outHit[0] = outHit[1] = true;
			outGz[0] = outGz[1] = 0.f;
			return;
		}
		outHit[0] = outHit[1] = true;
		outGz[0] = gz[0];
		outGz[1] = gz[1];
		return;
	}
	const int validIdx = ok[0] ? 0 : 1;
	if (gz[validIdx] < -3.5f) {
		outHit[0] = outHit[1] = true;
		outGz[0] = outGz[1] = 0.f;
		return;
	}
	outHit[0] = outHit[1] = true;
	outGz[0] = ok[0] ? gz[0] : 0.f;
	outGz[1] = ok[1] ? gz[1] : 0.f;
}

struct WeaponHoldPose {
	bool valid = false;
	bool twoHanded = false; // CS2 live AnimGraph puts hand_L on the weapon foregrip/support grip
	bool isMoving = false;  // CS2 pawn is actively moving (>8 u/s) so floor clamp stays steady
	float nativeArmBlend = 0.f; // 0..1 smooth hierarchical blend to CS2 live arms during reload / weapon switch
	uint8_t equippedWeaponClass = 0xFF; // 0xFF = follow sample.weaponClass; else WeaponClass
	int wpnBone = -1;       // skeleton bone index of "wpn" (or "weapon_hand_R")
	const bool *twistBones = nullptr; // optional [count]: true for *_TWIST bones that follow parent cleanly
	Pose handRToWpn;        // wpn in hand_R local space from CS2 live AnimGraph2 pose
	Pose wpnToHandL;        // hand_L in wpn local space from CS2 live AnimGraph2 pose
	Pose fingerLocal[30];   // parent-local CS2 weapon-grip finger poses: [0..14]=L (core 21..35), [15..29]=R (core 36..50)
	bool hasFingerLocal[30] = {};
};

// Two-bone leg IK: bends hip -> knee -> ankle so the ankle reaches `target`, keeping the knee on the side
// it already points to. Rotates the three bones' world rotations to match and moves knee and ankle.
inline void SolveLegIK(Pose &hip, Pose &knee, Pose &ankle, const float target[3]) {
	float h[3] = {hip.p[0], hip.p[1], hip.p[2]};
	float k[3] = {knee.p[0], knee.p[1], knee.p[2]};
	float a[3] = {ankle.p[0], ankle.p[1], ankle.p[2]};
	float hk[3] = {k[0] - h[0], k[1] - h[1], k[2] - h[2]};
	float ka[3] = {a[0] - k[0], a[1] - k[1], a[2] - k[2]};
	const float l1 = std::sqrt(hk[0] * hk[0] + hk[1] * hk[1] + hk[2] * hk[2]);
	const float l2 = std::sqrt(ka[0] * ka[0] + ka[1] * ka[1] + ka[2] * ka[2]);
	if (l1 < 1e-3f || l2 < 1e-3f) return;
	float ht[3] = {target[0] - h[0], target[1] - h[1], target[2] - h[2]};
	float d = std::sqrt(ht[0] * ht[0] + ht[1] * ht[1] + ht[2] * ht[2]);
	if (d < 1e-3f) return;
	const float dmax = (l1 + l2) * 0.999f, dmin = std::fabs(l1 - l2) * 1.001f + 0.01f;
	const float dc = std::clamp(d, dmin, dmax);
	float dir[3] = {ht[0] / d, ht[1] / d, ht[2] / d};
	// Pole: the current knee direction with the hip->target part removed.
	const float kd = hk[0] * dir[0] + hk[1] * dir[1] + hk[2] * dir[2];
	float pole[3] = {hk[0] - dir[0] * kd, hk[1] - dir[1] * kd, hk[2] - dir[2] * kd};
	float pl = std::sqrt(pole[0] * pole[0] + pole[1] * pole[1] + pole[2] * pole[2]);
	if (pl < 1e-4f) { pole[0] = 1.f; pole[1] = 0.f; pole[2] = 0.f; pl = 1.f; }
	for (int i = 0; i < 3; ++i) pole[i] /= pl;
	const float along = (l1 * l1 - l2 * l2 + dc * dc) / (2.f * dc);
	const float up = std::sqrt(std::max(0.f, l1 * l1 - along * along));
	float nk[3], na[3];
	for (int i = 0; i < 3; ++i) {
		nk[i] = h[i] + dir[i] * along + pole[i] * up;
		na[i] = h[i] + dir[i] * dc;
	}
	float oldHK[3] = {hk[0] / l1, hk[1] / l1, hk[2] / l1};
	float newHK[3] = {(nk[0] - h[0]) / l1, (nk[1] - h[1]) / l1, (nk[2] - h[2]) / l1};
	float s1[4];
	QuatFromTo(oldHK, newHK, s1);
	// The shin after the thigh's swing, then swung onto the final knee->ankle direction.
	float shin[3];
	QuatRotate(s1, ka, shin);
	const float sl = std::sqrt(shin[0] * shin[0] + shin[1] * shin[1] + shin[2] * shin[2]);
	float nka[3] = {na[0] - nk[0], na[1] - nk[1], na[2] - nk[2]};
	const float nl = std::sqrt(nka[0] * nka[0] + nka[1] * nka[1] + nka[2] * nka[2]);
	float s2[4] = {0.f, 0.f, 0.f, 1.f};
	if (sl > 1e-4f && nl > 1e-4f) {
		float f[3] = {shin[0] / sl, shin[1] / sl, shin[2] / sl};
		float t2[3] = {nka[0] / nl, nka[1] / nl, nka[2] / nl};
		QuatFromTo(f, t2, s2);
	}
	float s21[4];
	QuatMul(s2, s1, s21);
	float q[4];
	QuatMul(s1, hip.q, q); QuatNorm(q, hip.q);
	QuatMul(s21, knee.q, q); QuatNorm(q, knee.q);
	QuatMul(s21, ankle.q, q); QuatNorm(q, ankle.q);
	for (int i = 0; i < 3; ++i) { knee.p[i] = nk[i]; ankle.p[i] = na[i]; }
}

inline void SolveTwoBoneIK(Pose &upper, Pose &lower, Pose &end, const float target[3]) {
	SolveLegIK(upper, lower, end, target);
}

// Half-Life: Alyx BlendSpace_Parent-style spine-feathered bone mask: keeps pelvis and lower
// limbs on CS2's live AnimGraph2 locomotion while feathering across spine_0..spine_2 to full
// upper-body control on chest, neck, head, arms, hands, and all 30 fingers.
inline float CoreUpperBodyWeight(int core) {
	if (core == 0) return 0.f;
	if (core == 1) return 0.35f;
	if (core == 2) return 0.65f;
	if (core == 3) return 0.85f;
	if ((core >= 4 && core <= 14) || (core >= kCoreFingerLStart && core <= kCoreFingerREnd)) return 1.f;
	return 0.f;
}

inline void ApplyPoseToSkeleton(int count, const int *parents, const int *boneToCore,
                                const Pose *bind, const Pose *restLocal,
                                Pose *liveLocal, Pose *outWorld,
                                const SampledPose &sample, bool applyRootXY,
                                GroundProbeFn probe = nullptr, void *probeUser = nullptr,
                                const WeaponHoldPose *weaponHold = nullptr) {
	if (count <= 0 || count > kMaxSkeletonBones) return;

	// 1. Current live world pose from the AnimGraph2 parent-local pose (kept for blending, zero-weight
	//    passthrough, and the bones the clip does not drive).
	Pose liveWorld[kMaxSkeletonBones];
	for (int i = 0; i < count; ++i) {
		const int p = parents[i];
		if (p >= 0 && p < i) ComposePose(liveWorld[p], liveLocal[i], liveWorld[i]);
		else liveWorld[i] = liveLocal[i];
	}
	if (!sample.active || sample.weight <= 1e-4f) {
		for (int i = 0; i < count; ++i) outWorld[i] = liveWorld[i];
		return;
	}
	const float w = std::clamp(sample.weight, 0.f, 1.f);
	const bool upperOnly = sample.upperBodyOnly;
	float boneMask[kMaxSkeletonBones];
	for (int i = 0; i < count; ++i) {
		if (!upperOnly) {
			boneMask[i] = 1.f;
		} else {
			const int core = boneToCore[i];
			if (core >= 0 && core < kCoreBones) boneMask[i] = CoreUpperBodyWeight(core);
			else if (weaponHold && weaponHold->valid && i == weaponHold->wpnBone) boneMask[i] = 1.f;
			else {
				const int p = parents[i];
				boneMask[i] = (p >= 0 && p < i) ? boneMask[p] : 0.f;
			}
		}
	}

	// 2. Target world pose. Every driven bone starts from the CS2 BIND pose, never from the live pose, so
	//    a clip looks the same whatever the character was doing when it started. In upper-body blend mode,
	//    pelvis anchors directly to CS2's live walking/crouching pelvis so upper-body motion rides it cleanly.
	Pose targetWorld[kMaxSkeletonBones];
	int coreIdx[kCoreBones];
	for (int c = 0; c < kCoreBones; ++c) coreIdx[c] = -1;
	float corr[kCoreBones][4];
	for (int c = 0; c < kCoreBones; ++c) QuatIdentity(corr[c]);
	for (int i = 0; i < count; ++i) {
		const int p = parents[i];
		const int core = boneToCore[i];
		if (core >= 0 && core < kCoreBones) coreIdx[core] = i;
		if (core == 0) {
			if (upperOnly) {
				targetWorld[i] = liveWorld[i];
			} else {
				targetWorld[i].p[0] = bind[0].p[0] + (applyRootXY ? sample.pelvisDelta[0] : 0.f);
				targetWorld[i].p[1] = bind[0].p[1] + (applyRootXY ? sample.pelvisDelta[1] : 0.f);
				targetWorld[i].p[2] = bind[0].p[2] + sample.pelvisDelta[2];
				QuatMul(sample.rotDelta[0], bind[0].q, targetWorld[i].q);
				QuatNorm(targetWorld[i].q, targetWorld[i].q);
			}
		} else if (core > 0 && core < kCoreBones) {
			if (p >= 0 && p < i) {
				float localP[3];
				if (boneToCore[p] >= 0) {
					Pose bindLoc;
					LocalFromWorld(bind[boneToCore[p]], bind[core], bindLoc);
					for (int k = 0; k < 3; ++k) localP[k] = bindLoc.p[k];
				} else {
					for (int k = 0; k < 3; ++k) localP[k] = restLocal[i].p[k];
				}
				float rp[3];
				QuatRotate(targetWorld[p].q, localP, rp);
				for (int k = 0; k < 3; ++k) targetWorld[i].p[k] = targetWorld[p].p[k] + rp[k];
			} else {
				for (int k = 0; k < 3; ++k) targetWorld[i].p[k] = bind[core].p[k];
			}
			float q[4];
			QuatMul(sample.rotDelta[core], bind[core].q, q);
			const int child = kCoreChild[core];
			const float *d = sample.dir[core];
			if (child >= 0 && d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 0.25f) {
				float bd[3] = {bind[child].p[0] - bind[core].p[0], bind[child].p[1] - bind[core].p[1],
				               bind[child].p[2] - bind[core].p[2]};
				const float bl = std::sqrt(bd[0] * bd[0] + bd[1] * bd[1] + bd[2] * bd[2]);
				if (bl > 1e-4f) {
					for (int k = 0; k < 3; ++k) bd[k] /= bl;
					float cur[3];
					QuatRotate(sample.rotDelta[core], bd, cur);
					// Shortest-arc rotation from `cur` to the wanted direction `d`.
					const float dotv = cur[0] * d[0] + cur[1] * d[1] + cur[2] * d[2];
					float s[4];
					if (dotv < -0.9999f) {
						float ax[3] = {cur[1], -cur[0], 0.f};
						if (ax[0] * ax[0] + ax[1] * ax[1] < 1e-6f) { ax[0] = 0.f; ax[1] = cur[2]; ax[2] = -cur[1]; }
						const float al = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
						s[0] = ax[0] / al; s[1] = ax[1] / al; s[2] = ax[2] / al; s[3] = 0.f;
					} else {
						s[0] = cur[1] * d[2] - cur[2] * d[1];
						s[1] = cur[2] * d[0] - cur[0] * d[2];
						s[2] = cur[0] * d[1] - cur[1] * d[0];
						s[3] = 1.f + dotv;
						QuatNorm(s, s);
					}
					for (int k = 0; k < 4; ++k) corr[core][k] = s[k];
				}
			} else if (kCoreEndParent[core] >= 0) {
				for (int k = 0; k < 4; ++k) corr[core][k] = corr[kCoreEndParent[core]][k];
			}
			QuatMul(corr[core], q, targetWorld[i].q);
			QuatNorm(targetWorld[i].q, targetWorld[i].q);
		} else {
			// Non-core bone: neutral twist for *_TWIST bones so CS2 leg counter-twist never warps HL:A thighs.
			if (p >= 0 && p < i) {
				const bool isTwist = weaponHold && weaponHold->twistBones && weaponHold->twistBones[i];
				if (isTwist) {
					Pose neutral = liveLocal[i];
					QuatIdentity(neutral.q);
					ComposePose(targetWorld[p], neutral, targetWorld[i]);
				} else {
					ComposePose(targetWorld[p], liveLocal[i], targetWorld[i]);
				}
			} else {
				targetWorld[i] = liveWorld[i];
			}
		}
	}

	// 2a. Weapon hold, left-arm foregrip IK, hierarchical reload/switch/pistol arm blend, and finger grip:
	//     Respect the CS2 pawn's actual equipped weapon class and active reload/weapon-switch window,
	//     blending parent-local arm joints hierarchically so bone lengths never distort and full arms
	//     move smoothly instead of bending only the wrist.
	const uint8_t effWeapon = (weaponHold && weaponHold->equippedWeaponClass != 0xFF)
	                              ? weaponHold->equippedWeaponClass
	                              : sample.weaponClass;
	const bool pistolOnRifleClip = (effWeapon == uint8_t(WeaponClass::Pistol)) &&
	                               (sample.weaponClass == uint8_t(WeaponClass::Rifle));
	const float armLiveBlend = weaponHold
	                               ? std::max(weaponHold->nativeArmBlend, pistolOnRifleClip ? 0.92f : 0.f)
	                               : 0.f;
	const int hrPre = coreIdx[kCoreHandR];
	const int wbPre = weaponHold ? weaponHold->wpnBone : -1;
	Pose liveHandRToWpn;
	bool haveLiveHandRToWpn = false;
	if (wbPre >= 0 && wbPre < count && hrPre >= 0) {
		LocalFromWorld(liveWorld[hrPre], liveWorld[wbPre], liveHandRToWpn);
		haveLiveHandRToWpn = true;
	}
	if (weaponHold && wbPre >= 0 && wbPre < count) {
		if (armLiveBlend > 0.01f) {
			Pose hlLocal[kMaxSkeletonBones];
			bool isArmRooted[kMaxSkeletonBones] = {};
			for (int i = 0; i < count; ++i) {
				const int p = parents[i];
				if (p >= 0 && p < i) LocalFromWorld(targetWorld[p], targetWorld[i], hlLocal[i]);
				else hlLocal[i] = targetWorld[i];
			}
			for (int i = 0; i < count; ++i) {
				if (i == wbPre) continue;
				const int core = boneToCore[i];
				const int p = parents[i];
				const bool isArmBone = (core >= 7 && core <= 14) ||
				                       (core >= kCoreFingerLStart && core <= kCoreFingerREnd);
				const bool isArmChild = (p >= 0 && p < i && p != wbPre && isArmRooted[p]);
				if (!isArmBone && !isArmChild) continue;
				isArmRooted[i] = true;
				Pose loc;
				for (int k = 0; k < 3; ++k)
					loc.p[k] = hlLocal[i].p[k] + armLiveBlend * (liveLocal[i].p[k] - hlLocal[i].p[k]);
				QuatSlerp(hlLocal[i].q, liveLocal[i].q, armLiveBlend, loc.q);
				if (p >= 0 && p < i) ComposePose(targetWorld[p], loc, targetWorld[i]);
				else {
					for (int k = 0; k < 3; ++k)
						targetWorld[i].p[k] += armLiveBlend * (liveWorld[i].p[k] - targetWorld[i].p[k]);
					QuatSlerp(targetWorld[i].q, liveWorld[i].q, armLiveBlend, targetWorld[i].q);
				}
			}
			// Anchor wpn directly to hand_R (never via pelvis-parented wpnPivot!) so pistol stays in hand.
			if (haveLiveHandRToWpn && effWeapon != uint8_t(WeaponClass::None)) {
				Pose wpnLive;
				ComposePose(targetWorld[hrPre], liveHandRToWpn, wpnLive);
				if (weaponHold->valid && armLiveBlend < 0.99f) {
					Pose wpnGrip;
					ComposePose(targetWorld[hrPre], weaponHold->handRToWpn, wpnGrip);
					for (int k = 0; k < 3; ++k)
						targetWorld[wbPre].p[k] = wpnGrip.p[k] + armLiveBlend * (wpnLive.p[k] - wpnGrip.p[k]);
					QuatSlerp(wpnGrip.q, wpnLive.q, armLiveBlend, targetWorld[wbPre].q);
				} else {
					targetWorld[wbPre] = wpnLive;
				}
				for (int i = wbPre + 1; i < count; ++i) {
					if (parents[i] == wbPre) ComposePose(targetWorld[wbPre], liveLocal[i], targetWorld[i]);
				}
			}
		} else if (weaponHold->valid) {
			float holdR = 0.f, holdL = 0.f;
			if (effWeapon == uint8_t(WeaponClass::None)) {
				holdR = 0.f;
				holdL = 0.f;
			} else if (effWeapon == uint8_t(WeaponClass::Rifle) &&
			           sample.weaponClass == uint8_t(WeaponClass::Rifle)) {
				holdR = 1.f;
				holdL = weaponHold->twoHanded ? 1.f : 0.f;
			} else {
				// Pistols and grenades sit in hand_R without pulling hand_L onto a rifle foregrip
				// or twisting the gun axis toward hand_L.
				holdR = 1.f;
				holdL = 0.f;
			}
			const int hr = coreIdx[kCoreHandR];
			const int hl = coreIdx[kCoreHandL];
			const int al = coreIdx[kCoreArmUpperL];
			const int fl = coreIdx[kCoreArmLowerL];
			const int wb = weaponHold->wpnBone;
			if (holdR > 1e-4f && hr >= 0) {
				Pose alyxFingerLocal[30];
				bool haveAlyxFinger[30] = {};
				for (int i = 0; i < count; ++i) {
					const int core = boneToCore[i];
					if (core >= kCoreFingerLStart && core <= kCoreFingerREnd) {
						const int p = parents[i];
						if (p >= 0 && p < i) {
							LocalFromWorld(targetWorld[p], targetWorld[i], alyxFingerLocal[core - kCoreFingerLStart]);
							haveAlyxFinger[core - kCoreFingerLStart] = true;
						}
					}
				}

				Pose targetWpn;
				ComposePose(targetWorld[hr], weaponHold->handRToWpn, targetWpn);

				Pose rawForegrip;
				if (holdL > 0.f && hl >= 0) {
					ComposePose(targetWpn, weaponHold->wpnToHandL, rawForegrip);
					const float dx = targetWorld[hl].p[0] - rawForegrip.p[0];
					const float dy = targetWorld[hl].p[1] - rawForegrip.p[1];
					const float dz = targetWorld[hl].p[2] - rawForegrip.p[2];
					const float dHand = std::sqrt(dx * dx + dy * dy + dz * dz);
					if (sample.isReload || sample.category == uint8_t(Category::Gesture))
						holdL *= (1.f - SmoothStep01((dHand - 6.f) / 8.f));
					else
						holdL *= (1.f - SmoothStep01((dHand - 18.f) / 10.f));
				}

				if (effWeapon == uint8_t(WeaponClass::Rifle) && holdL > 0.01f && hl >= 0) {
					float cdir[3] = {rawForegrip.p[0] - targetWorld[hr].p[0],
					                 rawForegrip.p[1] - targetWorld[hr].p[1],
					                 rawForegrip.p[2] - targetWorld[hr].p[2]};
					float adir[3] = {targetWorld[hl].p[0] - targetWorld[hr].p[0],
					                 targetWorld[hl].p[1] - targetWorld[hr].p[1],
					                 targetWorld[hl].p[2] - targetWorld[hr].p[2]};
					const float clen = std::sqrt(cdir[0] * cdir[0] + cdir[1] * cdir[1] + cdir[2] * cdir[2]);
					const float alen = std::sqrt(adir[0] * adir[0] + adir[1] * adir[1] + adir[2] * adir[2]);
					if (clen > 6.f && alen > 6.f) {
						for (int k = 0; k < 3; ++k) { cdir[k] /= clen; adir[k] /= alen; }
						float swingQ[4];
						QuatFromTo(cdir, adir, swingQ);
						const float ang = 2.f * std::acos(std::clamp(std::fabs(swingQ[3]), 0.f, 1.f));
						float idQ[4] = {0.f, 0.f, 0.f, 1.f};
						if (ang > 1.22f) QuatSlerp(idQ, swingQ, 1.22f / ang, swingQ);
						float wristSwing[4], swungQ[4];
						QuatSlerp(idQ, swingQ, holdL, wristSwing);
						QuatMul(wristSwing, targetWorld[hr].q, swungQ);
						QuatNorm(swungQ, targetWorld[hr].q);
						ComposePose(targetWorld[hr], weaponHold->handRToWpn, targetWpn);
					}
				}

				for (int k = 0; k < 3; ++k)
					targetWorld[wb].p[k] = targetWorld[wb].p[k] + holdR * (targetWpn.p[k] - targetWorld[wb].p[k]);
				QuatSlerp(targetWorld[wb].q, targetWpn.q, holdR, targetWorld[wb].q);

				if (holdL > 0.01f && al >= 0 && fl >= 0 && hl >= 0) {
					Pose foregripWorld;
					ComposePose(targetWpn, weaponHold->wpnToHandL, foregripWorld);
					float ikTarget[3];
					for (int k = 0; k < 3; ++k)
						ikTarget[k] = targetWorld[hl].p[k] + holdL * (foregripWorld.p[k] - targetWorld[hl].p[k]);
					SolveLegIK(targetWorld[al], targetWorld[fl], targetWorld[hl], ikTarget);
					QuatSlerp(targetWorld[hl].q, foregripWorld.q, holdL, targetWorld[hl].q);
				}

				bool armDirty[kMaxSkeletonBones] = {};
				armDirty[hr] = true;
				armDirty[wb] = true;
				if (holdL > 0.01f && al >= 0 && fl >= 0 && hl >= 0) {
					armDirty[al] = true;
					armDirty[fl] = true;
					armDirty[hl] = true;
				}
				for (int i = 0; i < count; ++i) {
					const int core = boneToCore[i];
					const int p = parents[i];
					if (core >= kCoreFingerLStart && core <= kCoreFingerREnd) {
						const int fIdx = core - kCoreFingerLStart;
						if (haveAlyxFinger[fIdx] && p >= 0 && p < i) {
							const float fBlend = (core <= kCoreFingerLEnd) ? holdL : holdR;
							Pose flocal = alyxFingerLocal[fIdx];
							if (weaponHold->hasFingerLocal[fIdx] && fBlend > 1e-4f) {
								const Pose &glocal = weaponHold->fingerLocal[fIdx];
								for (int k = 0; k < 3; ++k)
									flocal.p[k] = flocal.p[k] + fBlend * (glocal.p[k] - flocal.p[k]);
								QuatSlerp(flocal.q, glocal.q, fBlend, flocal.q);
							}
							ComposePose(targetWorld[p], flocal, targetWorld[i]);
							armDirty[i] = true;
						}
					} else if (core < 0 && p >= 0 && p < i && armDirty[p] && i != wb) {
						ComposePose(targetWorld[p], liveLocal[i], targetWorld[i]);
						armDirty[i] = true;
					}
				}
			}
		}
	}

	// 2b. Grounding & CS2 native foot IK on stairs/curbs/slopes:
	//     While moving on flat ground, anchor to steady bindFloorZ so walking stride never bounces.
	//     On stairs/curbs/uneven ground (`nativeDiff` > threshold), feather cleanly to CS2's live
	//     AnimGraph2 pelvis height + legs instead of running custom two-bone IK that can flip knees.
	if (!upperOnly) {
		const float bindFloorZ = std::min(bind[kCoreAnkleL].p[2], bind[kCoreAnkleR].p[2]);
		const int pelvisIdx = coreIdx[0];
		const int alIdx = coreIdx[kCoreAnkleL], arIdx = coreIdx[kCoreAnkleR];
		const bool moving = weaponHold && weaponHold->isMoving;
		const bool haveLiveFeet = alIdx >= 0 && arIdx >= 0 &&
		                          std::isfinite(liveWorld[alIdx].p[2]) && std::isfinite(liveWorld[arIdx].p[2]);
		float liveFloorZ = bindFloorZ;
		if (haveLiveFeet) liveFloorZ = std::min(liveWorld[alIdx].p[2], liveWorld[arIdx].p[2]);
		const float floorZ = moving
		                         ? bindFloorZ
		                         : (haveLiveFeet ? std::clamp(liveFloorZ, bindFloorZ - 2.5f, bindFloorZ + 0.5f)
		                                         : bindFloorZ);
		{
			float lowest = 1e9f;
			for (int i = 0; i < count; ++i)
				if (boneToCore[i] == kCoreAnkleL || boneToCore[i] == kCoreAnkleR) lowest = std::min(lowest, targetWorld[i].p[2]);
			if (lowest < 1e8f) {
				// Preserve natural airborne stride/push-off lift and slight ankle-roll dips across all
				// categories while clamping deep ledge-drop animations so feet never sink underground.
				const float effectiveLift = std::max(-1.5f, sample.groundLift);
				const float dz = floorZ + effectiveLift - lowest;
				for (int i = 0; i < count; ++i)
					if (parents[i] >= 0 || boneToCore[i] >= 0) targetWorld[i].p[2] += dz;
			}
		}

		// 2c. Terrain step IK: explicit `probe` (unit tests) OR native CS2 AnimGraph2 stair/slope legs.
		if (probe) {
			const int legs[2][3] = {{15, 16, 17}, {18, 19, 20}};
			const float bindAnkleZ = floorZ;
			float desired[2] = {0.f, 0.f}, dzFoot[2] = {0.f, 0.f};
			bool have[2] = {false, false};
			for (int l = 0; l < 2; ++l) {
				const int ai = coreIdx[legs[l][2]];
				if (coreIdx[legs[l][0]] < 0 || coreIdx[legs[l][1]] < 0 || ai < 0) continue;
				const Pose &a = targetWorld[ai];
				float gz = 0.f;
				if (!probe(probeUser, l, a.p, &gz) || !std::isfinite(gz)) continue;
				const float height = a.p[2] - bindAnkleZ;
				const float plant = 1.f - SmoothStep01((height - 1.5f) / 6.f);
				desired[l] = std::max(a.p[2] + gz * plant, gz + bindAnkleZ * 0.85f);
				dzFoot[l] = desired[l] - a.p[2];
				have[l] = true;
			}
			float dzBody = 0.f;
			if (have[0] && have[1]) dzBody = std::min(dzFoot[0], dzFoot[1]);
			else if (have[0]) dzBody = dzFoot[0];
			else if (have[1]) dzBody = dzFoot[1];
			dzBody = std::clamp(dzBody, -10.f, 2.5f);
			if (have[0] || have[1]) {
				for (int i = 0; i < count; ++i)
					if (parents[i] >= 0 || boneToCore[i] >= 0) targetWorld[i].p[2] += dzBody;
				bool dirty[kMaxSkeletonBones] = {};
				for (int l = 0; l < 2; ++l) {
					if (!have[l]) continue;
					const int hi = coreIdx[legs[l][0]], ki = coreIdx[legs[l][1]], ai = coreIdx[legs[l][2]];
					const float target[3] = {targetWorld[ai].p[0], targetWorld[ai].p[1], desired[l]};
					SolveLegIK(targetWorld[hi], targetWorld[ki], targetWorld[ai], target);
					dirty[hi] = dirty[ki] = dirty[ai] = true;
				}
				for (int i = 0; i < count; ++i) {
					const int p = parents[i];
					if (boneToCore[i] < 0 && p >= 0 && p < i && dirty[p]) {
						ComposePose(targetWorld[p], liveLocal[i], targetWorld[i]);
						dirty[i] = true;
					}
				}
			}
		} else if (haveLiveFeet && pelvisIdx >= 0) {
			const float nativeDiff = std::fabs(liveWorld[alIdx].p[2] - liveWorld[arIdx].p[2]);
			const float nativeDrop = bindFloorZ - liveFloorZ;
			const float stairMetric = std::max(nativeDiff, moving ? 0.f : std::fabs(nativeDrop));
			const float nativeLegsW = moving
			                              ? SmoothStep01((nativeDiff - 2.2f) / 2.2f)
			                              : SmoothStep01((stairMetric - 0.6f) / 1.4f);
			if (nativeLegsW > 0.01f) {
				const float pelvisDz = nativeLegsW * (liveWorld[pelvisIdx].p[2] - targetWorld[pelvisIdx].p[2]);
				bool isLeg[kMaxSkeletonBones] = {};
				for (int i = 0; i < count; ++i) {
					const int core = boneToCore[i];
					const int p = parents[i];
					if (core >= 15 && core <= 20) isLeg[i] = true;
					else if (core < 0 && p >= 0 && p < i && isLeg[p]) isLeg[i] = true;
				}
				for (int i = 0; i < count; ++i) {
					if (i == pelvisIdx) {
						targetWorld[i].p[2] += pelvisDz;
						QuatSlerp(targetWorld[i].q, liveWorld[i].q, nativeLegsW, targetWorld[i].q);
					} else if (isLeg[i]) {
						for (int k = 0; k < 3; ++k)
							targetWorld[i].p[k] += nativeLegsW * (liveWorld[i].p[k] - targetWorld[i].p[k]);
						QuatSlerp(targetWorld[i].q, liveWorld[i].q, nativeLegsW, targetWorld[i].q);
					} else if (parents[i] >= 0 || boneToCore[i] >= 0) {
						targetWorld[i].p[2] += pelvisDz;
					}
				}
			}
		}
	}

	// 3. Blend liveWorld -> targetWorld by `w * boneMask[i]` and write back parent-local + world transforms.
	//    In upper-body mode, blend local bone translations relative to outWorld[parent] so feathered spine
	//    weights preserve exact bone lengths; afterwards anchor wpn directly to outWorld[hand_R] regardless
	//    of skeleton bone index order (CS2 wpn=24 precedes hand_R!).
	for (int i = 0; i < count; ++i) {
		const float wi = w * boneMask[i];
		const int p = parents[i];
		Pose blended;
		if (!upperOnly) {
			for (int k = 0; k < 3; ++k)
				blended.p[k] = liveWorld[i].p[k] + wi * (targetWorld[i].p[k] - liveWorld[i].p[k]);
			QuatSlerp(liveWorld[i].q, targetWorld[i].q, wi, blended.q);
		} else if (wi <= 1e-4f) {
			blended = liveWorld[i];
		} else if (p >= 0 && p < i) {
			QuatSlerp(liveWorld[i].q, targetWorld[i].q, wi, blended.q);
			Pose tLoc;
			LocalFromWorld(targetWorld[p], targetWorld[i], tLoc);
			float locP[3], rotP[3];
			for (int k = 0; k < 3; ++k)
				locP[k] = liveLocal[i].p[k] + wi * (tLoc.p[k] - liveLocal[i].p[k]);
			QuatRotate(outWorld[p].q, locP, rotP);
			for (int k = 0; k < 3; ++k) blended.p[k] = outWorld[p].p[k] + rotP[k];
		} else {
			for (int k = 0; k < 3; ++k)
				blended.p[k] = liveWorld[i].p[k] + wi * (targetWorld[i].p[k] - liveWorld[i].p[k]);
			QuatSlerp(liveWorld[i].q, targetWorld[i].q, wi, blended.q);
		}
		outWorld[i] = blended;
		if (p >= 0 && p < i) LocalFromWorld(outWorld[p], outWorld[i], liveLocal[i]);
		else liveLocal[i] = outWorld[i];
	}

	const int hrIdx = coreIdx[kCoreHandR];
	const int wb = weaponHold ? weaponHold->wpnBone : -1;
	if (weaponHold && wb >= 0 && wb < count && hrIdx >= 0 && effWeapon != uint8_t(WeaponClass::None) &&
	    (weaponHold->valid || armLiveBlend > 0.01f) && haveLiveHandRToWpn) {
		Pose activeOffset = (armLiveBlend > 0.01f || !weaponHold->valid) ? liveHandRToWpn : weaponHold->handRToWpn;
		if (armLiveBlend > 0.01f && armLiveBlend < 0.99f && weaponHold->valid) {
			for (int k = 0; k < 3; ++k)
				activeOffset.p[k] = weaponHold->handRToWpn.p[k] + armLiveBlend * (liveHandRToWpn.p[k] - weaponHold->handRToWpn.p[k]);
			QuatSlerp(weaponHold->handRToWpn.q, liveHandRToWpn.q, armLiveBlend, activeOffset.q);
		}
		Pose wpnOnOut;
		ComposePose(outWorld[hrIdx], activeOffset, wpnOnOut);
		const float ww = w * boneMask[wb];
		if (ww > 0.01f) {
			outWorld[wb] = wpnOnOut;
		} else {
			for (int k = 0; k < 3; ++k)
				outWorld[wb].p[k] = liveWorld[wb].p[k] + ww * (wpnOnOut.p[k] - liveWorld[wb].p[k]);
			QuatSlerp(liveWorld[wb].q, wpnOnOut.q, ww, outWorld[wb].q);
		}
		const int wp = parents[wb];
		if (wp >= 0 && wp < wb) LocalFromWorld(outWorld[wp], outWorld[wb], liveLocal[wb]);
		else liveLocal[wb] = outWorld[wb];
	}
}

// -- Companion demo sequence serialization (MVMSEQ01) ----------------------------------------------

struct CompanionActor {
	uint64_t steamId = 0;
	std::string name;
	int delayTicks = 0;
	bool placedActor = false;
	std::vector<Cue> cues;
};

struct CompanionSequence {
	int demoStartTick = 0;
	std::vector<CompanionActor> actors;
};

inline void WriteU32(std::vector<uint8_t> &b, uint32_t v) {
	const size_t at = b.size(); b.resize(at + 4); std::memcpy(b.data() + at, &v, 4);
}
inline void WriteI32(std::vector<uint8_t> &b, int32_t v) {
	const size_t at = b.size(); b.resize(at + 4); std::memcpy(b.data() + at, &v, 4);
}
inline void WriteU64(std::vector<uint8_t> &b, uint64_t v) {
	const size_t at = b.size(); b.resize(at + 8); std::memcpy(b.data() + at, &v, 8);
}
inline void WriteF32(std::vector<uint8_t> &b, float v) {
	const size_t at = b.size(); b.resize(at + 4); std::memcpy(b.data() + at, &v, 4);
}
inline void WriteStr(std::vector<uint8_t> &b, const std::string &s) {
	WriteU32(b, uint32_t(s.size()));
	b.insert(b.end(), s.begin(), s.end());
}

inline bool ReadU32(const uint8_t *&p, const uint8_t *e, uint32_t &v) {
	if (size_t(e - p) < 4) return false; std::memcpy(&v, p, 4); p += 4; return true;
}
inline bool ReadI32(const uint8_t *&p, const uint8_t *e, int32_t &v) {
	if (size_t(e - p) < 4) return false; std::memcpy(&v, p, 4); p += 4; return true;
}
inline bool ReadU64(const uint8_t *&p, const uint8_t *e, uint64_t &v) {
	if (size_t(e - p) < 8) return false; std::memcpy(&v, p, 8); p += 8; return true;
}
inline bool ReadF32(const uint8_t *&p, const uint8_t *e, float &v) {
	if (size_t(e - p) < 4) return false; std::memcpy(&v, p, 4); p += 4; return true;
}
inline bool ReadStr(const uint8_t *&p, const uint8_t *e, std::string &s) {
	uint32_t n = 0;
	if (!ReadU32(p, e, n) || n > 4096 || size_t(e - p) < n) return false;
	s.assign(reinterpret_cast<const char *>(p), n);
	p += n;
	return true;
}

inline void SerializeCue(std::vector<uint8_t> &b, const Cue &c) {
	WriteI32(b, c.startTick);
	WriteStr(b, c.clip);
	WriteF32(b, c.speed);
	b.push_back(uint8_t((c.loop ? 1 : 0) | (c.upperBodyOnly ? 2 : 0)));
	WriteF32(b, c.startTime);
	WriteF32(b, c.duration);
	WriteF32(b, c.blendIn);
	WriteF32(b, c.blendOut);
}

inline bool DeserializeCue(const uint8_t *&p, const uint8_t *e, Cue &c) {
	if (!ReadI32(p, e, c.startTick) || !ReadStr(p, e, c.clip) || !ReadF32(p, e, c.speed)) return false;
	if (p >= e) return false;
	const uint8_t flags = *p++;
	c.loop = (flags & 1) != 0;
	c.upperBodyOnly = (flags & 2) != 0;
	return ReadF32(p, e, c.startTime) && ReadF32(p, e, c.duration) &&
	       ReadF32(p, e, c.blendIn) && ReadF32(p, e, c.blendOut);
}

inline std::vector<uint8_t> SerializeCompanion(const CompanionSequence &seq) {
	std::vector<uint8_t> b;
	const char magic[8] = {'M','V','M','S','E','Q','0','1'};
	b.insert(b.end(), magic, magic + 8);
	WriteU32(b, 1);
	WriteI32(b, seq.demoStartTick);
	WriteU32(b, uint32_t(seq.actors.size()));
	for (const auto &a : seq.actors) {
		WriteU64(b, a.steamId);
		WriteStr(b, a.name);
		WriteI32(b, a.delayTicks);
		b.push_back(a.placedActor ? 1 : 0);
		WriteU32(b, uint32_t(a.cues.size()));
		for (const auto &c : a.cues) SerializeCue(b, c);
	}
	return b;
}

inline bool DeserializeCompanion(const uint8_t *data, size_t size, CompanionSequence &out) {
	out = CompanionSequence{};
	if (!data || size < 20 || std::memcmp(data, "MVMSEQ01", 8) != 0) return false;
	const uint8_t *p = data + 8, *e = data + size;
	uint32_t ver = 0, count = 0;
	if (!ReadU32(p, e, ver) || ver != 1 || !ReadI32(p, e, out.demoStartTick) || !ReadU32(p, e, count) || count > 256)
		return false;
	out.actors.resize(count);
	for (uint32_t i = 0; i < count; ++i) {
		auto &a = out.actors[i];
		uint32_t nCues = 0;
		if (!ReadU64(p, e, a.steamId) || !ReadStr(p, e, a.name) || !ReadI32(p, e, a.delayTicks)) return false;
		if (p >= e) return false;
		a.placedActor = (*p++) != 0;
		if (!ReadU32(p, e, nCues) || nCues > 4096) return false;
		a.cues.resize(nCues);
		for (uint32_t j = 0; j < nCues; ++j)
			if (!DeserializeCue(p, e, a.cues[j])) return false;
	}
	return true;
}

} // namespace Filmmaker::ActorAnim

#pragma pop_macro("max")
#pragma pop_macro("min")
