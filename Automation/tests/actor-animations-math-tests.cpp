#include "../../AfxHookSource2/Filmmaker/Takes/ActorAnimationsMath.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace Filmmaker::ActorAnim;

namespace {
int g_fails = 0;
void Check(bool cond, const char *msg) {
	if (!cond) { std::fprintf(stderr, "FAIL: %s\n", msg); ++g_fails; }
}

std::vector<uint8_t> BuildTestPack() {
	std::vector<uint8_t> b;
	const char magic[8] = {'M','V','M','A','C','T','0','2'};
	b.insert(b.end(), magic, magic + 8);
	WriteU32(b, 2);
	WriteU32(b, kCoreBones);
	WriteU32(b, 2);
	for (int i = 0; i < kCoreBones; ++i) {
		char nm[24] = {};
		std::snprintf(nm, sizeof(nm), "bone_%d", i);
		b.insert(b.end(), nm, nm + 24);
	}
	for (int i = 0; i < kCoreBones; ++i) {
		WriteF32(b, 0.f); WriteF32(b, 0.f); WriteF32(b, float(i)); // bind position
		WriteF32(b, 0.f); WriteF32(b, 0.f); WriteF32(b, 0.f); WriteF32(b, 1.f); // bind rotation
	}
	auto pushClip = [&](const char *id, const char *disp, uint8_t cat, uint8_t flags, float zEnd) {
		char idb[64] = {}, db[64] = {};
		std::strncpy(idb, id, 63); std::strncpy(db, disp, 63);
		b.insert(b.end(), idb, idb + 64);
		b.insert(b.end(), db, db + 64);
		b.push_back(cat); b.push_back(flags); b.push_back(0); b.push_back(0);
		WriteF32(b, 30.f);
		WriteU32(b, 31); // 1.0s clip
		WriteF32(b, 0.f);
		WriteF32(b, 0.f); // root speed
		for (int f = 0; f < 31; ++f) {
			const float t = float(f) / 30.f;
			WriteF32(b, 0.f); WriteF32(b, 0.f); WriteF32(b, zEnd * t);
			WriteF32(b, 0.f); // ground lift
			const float ang = t * 1.5707963f; // 90 deg about Z
			for (int bi = 0; bi < kCoreBones; ++bi) {
				WriteF32(b, 0.f); WriteF32(b, 0.f);
				WriteF32(b, std::sin(ang * 0.5f)); WriteF32(b, std::cos(ang * 0.5f));
			}
			for (int bi = 0; bi < kCoreBones; ++bi) { WriteF32(b, 0.f); WriteF32(b, 0.f); WriteF32(b, 1.f); }
		}
	};
	pushClip("clip_a", "Clip A", 3, 1, -10.f);
	pushClip("clip_b", "Clip B", 4, 0, +10.f);
	return b;
}
} // namespace


// Leg IK: the ankle reaches a reachable target, bone lengths stay put, rotations stay unit length.
static void TestLegIK() {
	Pose hip, knee, ankle;
	hip.p[0] = 0; hip.p[1] = 0; hip.p[2] = 40;
	knee.p[0] = 2; knee.p[1] = 0; knee.p[2] = 21;    // slightly bent forward
	ankle.p[0] = 0; ankle.p[1] = 0; ankle.p[2] = 4;
	const float l1 = std::sqrt(2.f * 2.f + 19.f * 19.f), l2 = std::sqrt(2.f * 2.f + 17.f * 17.f);
	const float target[3] = {6.f, 0.f, 12.f};
	SolveLegIK(hip, knee, ankle, target);
	const float dx = ankle.p[0] - target[0], dy = ankle.p[1] - target[1], dz = ankle.p[2] - target[2];
	Check(std::sqrt(dx * dx + dy * dy + dz * dz) < 0.05f, "leg IK ankle reaches a reachable target");
	const float a1 = std::sqrt(std::pow(knee.p[0] - hip.p[0], 2.f) + std::pow(knee.p[1] - hip.p[1], 2.f) + std::pow(knee.p[2] - hip.p[2], 2.f));
	const float a2 = std::sqrt(std::pow(ankle.p[0] - knee.p[0], 2.f) + std::pow(ankle.p[1] - knee.p[1], 2.f) + std::pow(ankle.p[2] - knee.p[2], 2.f));
	Check(std::fabs(a1 - l1) < 0.05f && std::fabs(a2 - l2) < 0.05f, "leg IK keeps thigh and shin lengths");
	const float qn = hip.q[0] * hip.q[0] + hip.q[1] * hip.q[1] + hip.q[2] * hip.q[2] + hip.q[3] * hip.q[3];
	Check(std::fabs(qn - 1.f) < 1e-3f, "leg IK keeps unit rotations");
	// Out of reach: the leg stretches toward the target without breaking.
	Pose h2 = hip, k2 = knee, a2p = ankle;
	const float far[3] = {0.f, 0.f, -60.f};
	SolveLegIK(h2, k2, a2p, far);
	Check(std::isfinite(a2p.p[0]) && std::isfinite(a2p.p[2]), "leg IK survives an unreachable target");
}

// Root motion: a clip that walks 10 inches a cycle moves 30 inches in three loops.
static void TestRootMotion() {
	Clip c;
	c.fps = 10.f;
	c.frames.resize(11); // 1 second
	for (int f = 0; f < 11; ++f) c.frames[f].pelvisDelta[0] = float(f);
	Cue cue;
	cue.loop = true;
	float o[2];
	RootMotionOffset(c, cue, 3.0, o);
	Check(std::fabs(o[0] - 30.f) < 0.01f, "root motion adds a cycle of travel per loop");
	cue.speed = 2.f;
	RootMotionOffset(c, cue, 1.0, o);
	Check(std::fabs(o[0] - 20.f) < 0.01f, "root motion follows the cue speed");
}

static void TestWeaponHoldAndFingers() {
	// Synthetic 11-bone skeleton (topological order: parent < index):
	// 0:pelvis(core 0), 1:spine_3(core 4),
	// 2:arm_upper_L(8), 3:arm_lower_L(9), 4:hand_L(10), 5:finger_index_0_L(24),
	// 6:arm_upper_R(12), 7:arm_lower_R(13), 8:hand_R(14), 9:finger_index_0_R(39),
	// 10:wpn(non-core, parent spine_3)
	constexpr int N = 11;
	const int parents[N] = {-1, 0, 1, 2, 3, 4, 1, 6, 7, 8, 1};
	const int boneToCore[N] = {
	    0, 4,
	    kCoreArmUpperL, kCoreArmLowerL, kCoreHandL, kCoreFingerLStart + 3,
	    kCoreArmUpperR, kCoreArmLowerR, kCoreHandR, kCoreFingerRStart + 3,
	    -1};

	Pose bind[kCoreBones];
	auto setBind = [&](int core, float x, float y, float z) {
		bind[core].p[0] = x; bind[core].p[1] = y; bind[core].p[2] = z;
		QuatIdentity(bind[core].q);
	};
	setBind(0, 0.f, 0.f, 36.f);
	setBind(4, 0.f, 0.f, 50.f);
	setBind(kCoreArmUpperL, 0.f, 8.f, 52.f);
	setBind(kCoreArmLowerL, 10.f, 8.f, 52.f);
	setBind(kCoreHandL, 18.f, 8.f, 52.f);
	setBind(kCoreFingerLStart + 3, 21.f, 8.f, 52.f);
	setBind(kCoreArmUpperR, 0.f, -8.f, 52.f);
	setBind(kCoreArmLowerR, 8.f, -8.f, 52.f);
	setBind(kCoreHandR, 14.f, -8.f, 52.f);
	setBind(kCoreFingerRStart + 3, 17.f, -8.f, 52.f);

	Pose restLocal[N];
	for (int i = 0; i < N; ++i) {
		QuatIdentity(restLocal[i].q);
		if (parents[i] < 0) {
			restLocal[i] = bind[boneToCore[i]];
		} else if (boneToCore[i] >= 0) {
			const Pose &bp = bind[boneToCore[parents[i]]];
			const Pose &bc = bind[boneToCore[i]];
			for (int k = 0; k < 3; ++k) restLocal[i].p[k] = bc.p[k] - bp.p[k];
		} else {
			restLocal[i].p[0] = 6.f; restLocal[i].p[1] = 0.f; restLocal[i].p[2] = 6.f;
		}
	}

	WeaponHoldPose hold;
	hold.valid = true;
	hold.twoHanded = true;
	hold.wpnBone = 10;
	hold.handRToWpn.p[0] = 0.f; hold.handRToWpn.p[1] = 2.f; hold.handRToWpn.p[2] = 2.f;
	QuatIdentity(hold.handRToWpn.q);
	hold.wpnToHandL.p[0] = 0.f; hold.wpnToHandL.p[1] = 8.f; hold.wpnToHandL.p[2] = 0.f;
	QuatIdentity(hold.wpnToHandL.q);
	// Curled grip rotations for index fingers (L: 60 deg Z, R: 45 deg Z)
	hold.fingerLocal[3].p[0] = 3.f; hold.fingerLocal[3].p[1] = 0.f; hold.fingerLocal[3].p[2] = 0.f;
	hold.fingerLocal[3].q[2] = std::sin(0.5235988f); hold.fingerLocal[3].q[3] = std::cos(0.5235988f);
	hold.hasFingerLocal[3] = true;
	hold.fingerLocal[18].p[0] = 3.f; hold.fingerLocal[18].p[1] = 0.f; hold.fingerLocal[18].p[2] = 0.f;
	hold.fingerLocal[18].q[2] = std::sin(0.3926991f); hold.fingerLocal[18].q[3] = std::cos(0.3926991f);
	hold.hasFingerLocal[18] = true;

	auto dist = [](const Pose &a, const Pose &b) {
		const float dx = a.p[0] - b.p[0], dy = a.p[1] - b.p[1], dz = a.p[2] - b.p[2];
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	};
	auto quatDotAbs = [](const float a[4], const float b[4]) {
		return std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
	};

	// 1. Two-handed rifle clip: wpn attaches to hand_R, hand_L reaches foregrip, both fingers grip
	{
		Pose liveLocal[N], outWorld[N];
		for (int i = 0; i < N; ++i) liveLocal[i] = restLocal[i];
		SampledPose sp;
		sp.active = true;
		sp.weight = 1.0f;
		sp.weaponClass = uint8_t(WeaponClass::Rifle);
		sp.category = uint8_t(Category::Locomotion);
		for (int b = 0; b < kCoreBones; ++b) QuatIdentity(sp.rotDelta[b]);
		ApplyPoseToSkeleton(N, parents, boneToCore, bind, restLocal, liveLocal, outWorld, sp, false, nullptr, nullptr, &hold);

		Pose expWpn, expHandL;
		ComposePose(outWorld[8], hold.handRToWpn, expWpn);
		ComposePose(outWorld[10], hold.wpnToHandL, expHandL);
		Check(dist(outWorld[10], expWpn) < 0.05f, "rifle: wpn attaches cleanly in hand_R");
		Check(dist(outWorld[4], expHandL) < 0.1f, "rifle: hand_L IK reaches foregrip");
		Check(quatDotAbs(liveLocal[5].q, hold.fingerLocal[3].q) > 0.999f, "rifle: left finger blends to CS2 grip");
		Check(quatDotAbs(liveLocal[9].q, hold.fingerLocal[18].q) > 0.999f, "rifle: right finger blends to CS2 grip");
	}

	// 2. Gesture clip with left hand raised >25 in from foregrip: right hand holds weapon, left stays raised
	{
		Pose liveLocal[N], outWorld[N];
		for (int i = 0; i < N; ++i) liveLocal[i] = restLocal[i];
		SampledPose sp;
		sp.active = true;
		sp.weight = 1.0f;
		sp.weaponClass = uint8_t(WeaponClass::Rifle);
		sp.category = uint8_t(Category::Gesture);
		for (int b = 0; b < kCoreBones; ++b) QuatIdentity(sp.rotDelta[b]);
		// Swing left arm -90 deg about Y so +X points straight up (+Z) -> hand_L rises 18+ inches up
		const float s45 = std::sin(-0.78539816f), c45 = std::cos(-0.78539816f);
		for (int c : {kCoreArmUpperL, kCoreArmLowerL, kCoreHandL, kCoreFingerLStart + 3}) {
			sp.rotDelta[c][0] = 0.f; sp.rotDelta[c][1] = s45; sp.rotDelta[c][2] = 0.f; sp.rotDelta[c][3] = c45;
		}
		ApplyPoseToSkeleton(N, parents, boneToCore, bind, restLocal, liveLocal, outWorld, sp, false, nullptr, nullptr, &hold);

		Pose expWpn;
		ComposePose(outWorld[8], hold.handRToWpn, expWpn);
		Check(dist(outWorld[10], expWpn) < 0.05f, "gesture: wpn stays in hand_R");
		Check(quatDotAbs(liveLocal[9].q, hold.fingerLocal[18].q) > 0.999f, "gesture: right finger keeps weapon grip");
		Check(outWorld[4].p[2] > 65.f, "gesture: raised hand_L releases foregrip");
		Check(quatDotAbs(liveLocal[5].q, hold.fingerLocal[3].q) < 0.95f, "gesture: left finger keeps Alyx gesture pose");
	}

	// 3. Hands-free clip (WeaponClass::None): weapon hold and finger grip stay disabled
	{
		Pose liveLocal[N], outWorld[N];
		for (int i = 0; i < N; ++i) liveLocal[i] = restLocal[i];
		SampledPose sp;
		sp.active = true;
		sp.weight = 1.0f;
		sp.weaponClass = uint8_t(WeaponClass::None);
		sp.category = uint8_t(Category::Gesture);
		for (int b = 0; b < kCoreBones; ++b) QuatIdentity(sp.rotDelta[b]);
		ApplyPoseToSkeleton(N, parents, boneToCore, bind, restLocal, liveLocal, outWorld, sp, false, nullptr, nullptr, &hold);

		Pose expFreeWpn;
		ComposePose(outWorld[1], restLocal[10], expFreeWpn);
		Check(dist(outWorld[10], expFreeWpn) < 0.05f, "none weapon: wpn not pulled into hand_R");
		Check(std::fabs(outWorld[4].p[1] - 8.f) < 0.05f, "none weapon: hand_L not pulled to foregrip");
		Check(quatDotAbs(liveLocal[9].q, hold.fingerLocal[18].q) < 0.95f, "none weapon: right finger stays in Alyx pose");
	}
	// 4. Pistol clip (WeaponClass::Pistol): weapon attaches cleanly in hand_R WITHOUT rifle foregrip IK or barrel swing
	{
		Pose liveLocal[N], outWorld[N];
		for (int i = 0; i < N; ++i) liveLocal[i] = restLocal[i];
		SampledPose sp;
		sp.active = true;
		sp.weight = 1.0f;
		sp.weaponClass = uint8_t(WeaponClass::Pistol);
		sp.category = uint8_t(Category::Weapon);
		for (int b = 0; b < kCoreBones; ++b) QuatIdentity(sp.rotDelta[b]);
		// Raise left hand near right hand as if supporting grip — pistol must NOT swing hand_R toward hand_L!
		const float s30 = std::sin(0.5235987f * 0.5f), c30 = std::cos(0.5235987f * 0.5f);
		sp.rotDelta[kCoreHandR][0] = 0.f; sp.rotDelta[kCoreHandR][1] = 0.f; sp.rotDelta[kCoreHandR][2] = s30; sp.rotDelta[kCoreHandR][3] = c30;
		ApplyPoseToSkeleton(N, parents, boneToCore, bind, restLocal, liveLocal, outWorld, sp, false, nullptr, nullptr, &hold);

		Pose expWpn;
		ComposePose(outWorld[8], hold.handRToWpn, expWpn);
		Check(dist(outWorld[10], expWpn) < 0.05f, "pistol: wpn attaches cleanly in hand_R");
		Check(quatDotAbs(outWorld[8].q, sp.rotDelta[kCoreHandR]) > 0.999f, "pistol: hand_R orientation kept intact without rifle barrel-swing");
	}
}

static void TestBlendSampledPoses() {
	SampledPose a, b;
	a.active = true; a.weight = 1.0f; a.groundLift = 1.0f;
	a.pelvisDelta[0] = 0.f; a.pelvisDelta[1] = 2.f; a.pelvisDelta[2] = -4.f;
	b.active = true; b.weight = 0.8f; b.groundLift = 3.0f;
	b.pelvisDelta[0] = 4.f; b.pelvisDelta[1] = -2.f; b.pelvisDelta[2] = 0.f;
	for (int i = 0; i < kCoreBones; ++i) {
		QuatIdentity(a.rotDelta[i]);
		QuatIdentity(b.rotDelta[i]);
		a.dir[i][0] = 1.f; a.dir[i][1] = 0.f; a.dir[i][2] = 0.f;
		b.dir[i][0] = 0.f; b.dir[i][1] = 1.f; b.dir[i][2] = 0.f;
	}
	b.rotDelta[0][2] = std::sin(0.78539816f); b.rotDelta[0][3] = std::cos(0.78539816f); // 90 deg Z
	SampledPose m = BlendSampledPoses(a, b, 0.5f);
	Check(m.active && std::fabs(m.weight - 1.0f) < 1e-4f, "blend keeps max weight");
	Check(std::fabs(m.pelvisDelta[0] - 2.f) < 0.01f && std::fabs(m.pelvisDelta[1] - 0.f) < 0.01f &&
	      std::fabs(m.pelvisDelta[2] - (-2.f)) < 0.01f, "blend interpolates pelvisDelta");
	Check(std::fabs(m.groundLift - 2.0f) < 0.01f, "blend interpolates groundLift");
	const float qn = m.rotDelta[0][0] * m.rotDelta[0][0] + m.rotDelta[0][1] * m.rotDelta[0][1] +
	                 m.rotDelta[0][2] * m.rotDelta[0][2] + m.rotDelta[0][3] * m.rotDelta[0][3];
	Check(std::fabs(qn - 1.f) < 1e-3f, "blend keeps unit quaternion");
	Check(std::fabs(m.dir[0][0] - 0.7071f) < 0.01f && std::fabs(m.dir[0][1] - 0.7071f) < 0.01f, "blend renormalizes dir");
}

static void TestDirectionalWithIdleAndWalk() {
	Check(SelectDirectionalClip("hla_walk_fwd", 0.f, 0.f, false, true) == "hla_combat_idle", "standing transitions to idle");
	Check(SelectDirectionalClip("hla_walk_fwd", 0.f, 0.f, false, true, true) == "hla_crouch_idle", "ducking idle transitions to crouch_idle");
	Check(SelectDirectionalClip("hla_combat_idle", 90.f, 0.f, false, true) == "hla_run_fwd", "idle + W transitions to run_fwd");
	Check(SelectDirectionalClip("hla_combat_idle", 90.f, 0.f, true, true) == "hla_walk_fwd", "idle + Shift+W transitions to walk_fwd");
	Check(SelectDirectionalClip("hla_run_fwd", -90.f, 0.f, false, true) == "hla_run_back", "S run transitions to run_back");
	Check(SelectDirectionalClip("hla_walk_back", -90.f, 0.f, true, true) == "hla_walk_back", "Shift+S transitions to walk_back");
	Check(SelectDirectionalClip("hla_run_fwd", 0.f, -90.f, false, true) == "hla_run_strafe_l", "A run transitions to run_strafe_l");
	Check(SelectDirectionalClip("hla_run_strafe_l", 0.f, 90.f, false, true) == "hla_run_strafe_r", "D run transitions to run_strafe_r");
	Check(SelectDirectionalClip("hla_walk_fwd", 0.f, -90.f, true, true) == "hla_walk_strafe_l", "Shift+A transitions to walk_strafe_l");
	Check(SelectDirectionalClip("hla_walk_fwd", 0.f, 90.f, true, true) == "hla_walk_strafe_r", "Shift+D transitions to walk_strafe_r");
	Check(SelectDirectionalClip("hla_run_fwd", 90.f, 90.f, false, true) == "hla_run_fwd_r", "W+D transitions to run_fwd_r");
	Check(SelectDirectionalClip("hla_run_fwd", -90.f, -90.f, false, true) == "hla_run_back_l", "S+A transitions to run_back_l");
	Check(SelectDirectionalClip("hla_run_fwd", 90.f, 0.f, false, true, true) == "hla_crouch_walk_fwd", "Ctrl+W transitions to crouch_walk_fwd");
	Check(SelectDirectionalClip("hla_crouch_walk_fwd", 0.f, -80.f, false, true, true) == "hla_crouch_walk_strafe_l", "Ctrl+A transitions to crouch_walk_strafe_l");
	Check(SelectDirectionalStopClip(100.f, 0.f) == "hla_walk_stop_fwd", "stop fwd");
	Check(SelectDirectionalStopClip(-100.f, 0.f) == "hla_walk_stop_back", "stop back");
	Check(SelectDirectionalStopClip(0.f, -100.f) == "hla_walk_stop_l", "stop left");
	Check(SelectDirectionalStopClip(0.f, 100.f) == "hla_walk_stop_r", "stop right");
}

static void TestZeroWeightDoesNotCollapse() {
	constexpr int N = 2;
	const int parents[N] = {-1, 0};
	const int boneToCore[N] = {0, 4};
	Pose bind[kCoreBones], restLocal[N], liveLocal[N], outWorld[N];
	for (int i = 0; i < kCoreBones; ++i) QuatIdentity(bind[i].q);
	restLocal[0].p[0] = 1.f; restLocal[0].p[1] = 2.f; restLocal[0].p[2] = 36.f; QuatIdentity(restLocal[0].q);
	restLocal[1].p[0] = 0.f; restLocal[1].p[1] = 0.f; restLocal[1].p[2] = 14.f; QuatIdentity(restLocal[1].q);
	liveLocal[0] = restLocal[0]; liveLocal[1] = restLocal[1];
	SampledPose sp;
	sp.active = true;
	sp.weight = 0.0f;
	for (int b = 0; b < kCoreBones; ++b) QuatIdentity(sp.rotDelta[b]);
	ApplyPoseToSkeleton(N, parents, boneToCore, bind, restLocal, liveLocal, outWorld, sp, false);
	Check(std::fabs(outWorld[0].p[2] - 36.f) < 1e-4f && std::fabs(outWorld[1].p[2] - 50.f) < 1e-4f,
	      "zero weight writes liveWorld cleanly without collapsing outWorld to zero");
}

static void TestFootGroundSanitizerAndGrounding() {
	// 1. Both feet reporting -14" under server hull (e.g. solid manhole prop missing from collision mesh) -> clamped to 0"
	{
		const bool rawHit[2] = {true, true};
		const float rawGz[2] = {-14.0f, -13.5f};
		bool sanHit[2] = {};
		float sanGz[2] = {};
		SanitizeFootGroundProbes(rawHit, rawGz, sanHit, sanGz);
		Check(sanHit[0] && sanHit[1] && std::fabs(sanGz[0]) < 1e-4f && std::fabs(sanGz[1]) < 1e-4f,
		      "both-feet deep uncached drop rejects sinking");
	}
	// 2. Real stair step (upper foot at 0", lower foot at -7.5") -> preserves lower step for leg IK
	{
		const bool rawHit[2] = {true, true};
		const float rawGz[2] = {0.2f, -7.5f};
		bool sanHit[2] = {};
		float sanGz[2] = {};
		SanitizeFootGroundProbes(rawHit, rawGz, sanHit, sanGz);
		Check(sanHit[0] && sanHit[1] && std::fabs(sanGz[0]) < 1e-4f && std::fabs(sanGz[1] - (-7.5f)) < 1e-4f,
		      "stair step keeps lower foot offset while zeroing upper floor noise");
	}
	// 3. Single-foot hole (-15") with other foot invalid -> rejected
	{
		const bool rawHit[2] = {true, false};
		const float rawGz[2] = {-15.0f, 0.0f};
		bool sanHit[2] = {};
		float sanGz[2] = {};
		SanitizeFootGroundProbes(rawHit, rawGz, sanHit, sanGz);
		Check(sanHit[0] && sanHit[1] && std::fabs(sanGz[0]) < 1e-4f && std::fabs(sanGz[1]) < 1e-4f,
		      "single-foot deep drop rejects sinking");
	}
	// 4. Grounded locomotion vs airborne cinematic groundLift handling in ApplyPoseToSkeleton
	constexpr int N = 7; // pelvis(0), L leg(15,16,17), R leg(18,19,20)
	const int parents[N] = {-1, 0, 1, 2, 0, 4, 5};
	const int boneToCore[N] = {0, 15, 16, 17, 18, 19, 20};
	Pose bind[kCoreBones], restLocal[N], liveLocal[N], outWorld[N];
	for (int i = 0; i < kCoreBones; ++i) QuatIdentity(bind[i].q);
	auto setB = [&](int c, float x, float y, float z) { bind[c].p[0] = x; bind[c].p[1] = y; bind[c].p[2] = z; };
	setB(0, 0, 0, 42);
	setB(15, 0, 4, 38); setB(16, 0, 4, 21); setB(17, 0, 4, 4.3f);
	setB(18, 0, -4, 38); setB(19, 0, -4, 21); setB(20, 0, -4, 4.3f);
	for (int i = 0; i < N; ++i) {
		QuatIdentity(restLocal[i].q);
		if (parents[i] < 0) restLocal[i] = bind[boneToCore[i]];
		else {
			const Pose &bp = bind[boneToCore[parents[i]]];
			const Pose &bc = bind[boneToCore[i]];
			for (int k = 0; k < 3; ++k) restLocal[i].p[k] = bc.p[k] - bp.p[k];
		}
		liveLocal[i] = restLocal[i];
	}
	SampledPose loco;
	loco.active = true; loco.weight = 1.0f; loco.category = uint8_t(Category::Locomotion);
	loco.groundLift = 3.5f;
	for (int b = 0; b < kCoreBones; ++b) QuatIdentity(loco.rotDelta[b]);
	ApplyPoseToSkeleton(N, parents, boneToCore, bind, restLocal, liveLocal, outWorld, loco, false);
	const float lowLoco = std::min(outWorld[3].p[2], outWorld[6].p[2]);
	Check(std::fabs(lowLoco - 7.8f) < 0.05f, "locomotion preserves airborne push-off groundLift (no stride snap)");

	SampledPose dip = loco;
	dip.groundLift = -1.0f;
	for (int i = 0; i < N; ++i) liveLocal[i] = restLocal[i];
	ApplyPoseToSkeleton(N, parents, boneToCore, bind, restLocal, liveLocal, outWorld, dip, false);
	const float lowDip = std::min(outWorld[3].p[2], outWorld[6].p[2]);
	Check(std::fabs(lowDip - 3.3f) < 0.05f, "mild negative ankle-roll groundLift (-1.0) is preserved");

	SampledPose drop = loco;
	drop.groundLift = -20.0f;
	for (int i = 0; i < N; ++i) liveLocal[i] = restLocal[i];
	ApplyPoseToSkeleton(N, parents, boneToCore, bind, restLocal, liveLocal, outWorld, drop, false);
	const float lowDrop = std::min(outWorld[3].p[2], outWorld[6].p[2]);
	Check(std::fabs(lowDrop - 2.8f) < 0.05f, "deep negative ledge-drop groundLift clamps at -1.5");

	SampledPose cine = loco;
	cine.category = uint8_t(Category::Cinematic);
	cine.groundLift = 12.0f;
	for (int i = 0; i < N; ++i) liveLocal[i] = restLocal[i];
	ApplyPoseToSkeleton(N, parents, boneToCore, bind, restLocal, liveLocal, outWorld, cine, false);
	const float lowCine = std::min(outWorld[3].p[2], outWorld[6].p[2]);
	Check(std::fabs(lowCine - 16.3f) < 0.05f, "airborne cinematic clip preserves positive groundLift");
}

static void TestRecommendedMaxSpeeds() {
	Clip c;
	c.id = "hla_explosion_wince_s"; c.category = uint8_t(Category::Reaction); c.rootSpeed = 27.8f;
	Check(RecommendedMaxSpeedForClip(c, false, false) < -0.5f, "explosion wince unlocks movement (-1)");

	c.id = "hla_flinch_01"; c.category = uint8_t(Category::Reaction); c.rootSpeed = 17.7f;
	Check(RecommendedMaxSpeedForClip(c, false, false) < -0.5f, "flinch unlocks movement (-1)");

	c.id = "hla_signal_halt"; c.category = uint8_t(Category::Gesture); c.rootSpeed = 6.7f;
	Check(RecommendedMaxSpeedForClip(c, false, false) < -0.5f, "gesture unlocks movement (-1)");

	c.id = "hla_reload_standing"; c.category = uint8_t(Category::Weapon); c.rootSpeed = 3.9f;
	Check(RecommendedMaxSpeedForClip(c, false, false) < -0.5f, "reload unlocks movement (-1)");

	c.id = "hla_crouch_idle"; c.category = uint8_t(Category::Crouch); c.rootSpeed = 0.5f;
	Check(RecommendedMaxSpeedForClip(c, false, false) < -0.5f, "crouch unlocks movement (-1)");

	c.id = "hla_rubble_slide"; c.category = uint8_t(Category::Cinematic); c.rootSpeed = 98.7f;
	Check(RecommendedMaxSpeedForClip(c, false, false) < -0.5f, "cinematic unlocks movement (-1)");

	c.id = "hla_combat_idle"; c.category = uint8_t(Category::Idle); c.rootSpeed = 0.2f;
	Check(RecommendedMaxSpeedForClip(c, false, false) < -0.5f, "non-directional idle unlocks movement (-1)");
	Check(std::fabs(RecommendedMaxSpeedForClip(c, true, false) - 139.2f) < 0.2f, "directional idle allows HL:A 139.2 u/s jog speed");
	Check(std::fabs(RecommendedMaxSpeedForClip(c, true, true) - 69.6f) < 0.2f, "directional idle with shift allows HL:A 69.6 u/s walk speed");
	Check(std::fabs(RecommendedMaxSpeedForClip(c, true, false, true) - 76.f) < 0.2f, "directional crouch allows HL:A 76 u/s crouch-walk speed");

	c.id = "hla_walk_01"; c.category = uint8_t(Category::Locomotion); c.rootSpeed = 39.0f;
	Check(std::fabs(RecommendedMaxSpeedForClip(c, false, false) - 39.f) < 1e-4f, "tactical walk matches authored 39 u/s");

	c.id = "hla_patrol_pacing"; c.category = uint8_t(Category::Locomotion); c.rootSpeed = 12.9f;
	Check(std::fabs(RecommendedMaxSpeedForClip(c, false, false) - 25.f) < 1e-4f, "patrol pacing clamps at min 25 u/s");

	c.id = "hla_run_strafe_r"; c.category = uint8_t(Category::Locomotion); c.rootSpeed = 155.0f;
	Check(std::fabs(RecommendedMaxSpeedForClip(c, true, false) - 139.2f) < 0.2f, "run strafe right matches HL:A 139.2 u/s");

	c.id = "hla_run_fwd"; c.category = uint8_t(Category::Locomotion); c.rootSpeed = 184.6f;
	Check(std::fabs(RecommendedMaxSpeedForClip(c, true, false) - 139.2f) < 0.2f, "run forward uses uniform HL:A 139.2 u/s family speed");
	Check(std::fabs(RecommendedMaxSpeedForClip(c, true, false, false, false, 0.5f) - 69.6f) < 0.2f, "speed scale halves HL:A run speed");

	c.id = "hla_heavy_run_fwd"; c.category = uint8_t(Category::Locomotion); c.rootSpeed = 96.0f;
	Check(std::fabs(RecommendedMaxSpeedForClip(c, true, false) - 105.f) < 0.2f, "heavy shotgun run matches HL:A 105 u/s");

	c.id = "hla_walk_strafe_l"; c.category = uint8_t(Category::Locomotion); c.rootSpeed = 66.0f;
	Check(std::fabs(RecommendedMaxSpeedForClip(c, true, true) - 69.6f) < 0.2f, "walk strafe left matches HL:A 69.6 u/s");

	c.id = "hla_mc_walk_fwd"; c.category = uint8_t(Category::Locomotion); c.rootSpeed = 57.7f;
	Check(std::fabs(RecommendedMaxSpeedForClip(c, true, true) - 56.f) < 0.2f, "metrocop walk matches HL:A 56 u/s");

	c.id = "hla_sup_sprint_fwd"; c.category = uint8_t(Category::Locomotion); c.rootSpeed = 125.0f;
	Check(std::fabs(RecommendedMaxSpeedForClip(c, true, false) - 180.f) < 0.2f, "suppressor sprint matches HL:A 180 u/s");

	c.id = "hla_signal_halt"; c.category = uint8_t(Category::Gesture); c.rootSpeed = 6.7f;
	Check(RecommendedMaxSpeedForClip(c, false, false, false, true) < -0.5f, "upper-body blend unlocks engine sv_maxspeed (-1)");
}

static void TestNativeFeetPistolHoldRootVelAndBlendPair() {
	// 1. SampleClipRootVelocity extracts backward travel velocity on staggers/reactions.
	Clip backClip;
	backClip.fps = 30.f;
	backClip.frames.resize(31);
	for (int f = 0; f < 31; ++f) backClip.frames[f].pelvisDelta[0] = -2.0f * float(f); // -60 in/s
	float rvel[2] = {};
	SampleClipRootVelocity(backClip, 0.4, 1.0f, false, rvel);
	Check(std::fabs(rvel[0] - (-60.f)) < 1.0f, "SampleClipRootVelocity returns -60 in/s backward velocity");

	// 2. Continuous MoveHeading blend pair brackets diagonal motion between octants.
	std::string cA, cB;
	float bt = -1.f;
	const bool okPair = SelectDirectionalBlendPair("hla_run_fwd", 100.f, 41.421356f, false, false, cA, cB, bt);
	Check(okPair && cA == "hla_run_fwd" && cB == "hla_run_fwd_r" && std::fabs(bt - 0.5f) < 0.05f,
	      "SelectDirectionalBlendPair smoothly blends fwd and fwd_r at 22.5 deg");

	// 3. Pistol equipped while playing a rifle clip blends arms toward live pistol pose (no rifle foregrip IK).
	constexpr int N = 9;
	// 0:pelvis(0), 1:spine_3(4), 2:arm_upper_L(8), 3:arm_lower_L(9), 4:hand_L(10), 5:arm_upper_R(12), 6:arm_lower_R(13), 7:hand_R(14), 8:wpn(-1)
	const int parents[N] = {-1, 0, 1, 2, 3, 1, 5, 6, 1};
	const int boneToCore[N] = {0, 4, kCoreArmUpperL, kCoreArmLowerL, kCoreHandL, kCoreArmUpperR, kCoreArmLowerR, kCoreHandR, -1};
	Pose bind[kCoreBones], restLocal[N], liveLocal[N], outWorld[N];
	for (int i = 0; i < kCoreBones; ++i) QuatIdentity(bind[i].q);
	auto setB = [&](int c, float x, float y, float z) { bind[c].p[0] = x; bind[c].p[1] = y; bind[c].p[2] = z; };
	setB(0, 0, 0, 36); setB(4, 0, 0, 52);
	setB(kCoreArmUpperL, 0, 8, 52); setB(kCoreArmLowerL, 8, 8, 52); setB(kCoreHandL, 16, 4, 52);
	setB(kCoreArmUpperR, 0, -8, 52); setB(kCoreArmLowerR, 8, -8, 52); setB(kCoreHandR, 16, -4, 52);
	for (int i = 0; i < N; ++i) {
		QuatIdentity(restLocal[i].q);
		if (parents[i] < 0) restLocal[i] = bind[boneToCore[i]];
		else if (boneToCore[i] >= 0) {
			const Pose &bp = bind[boneToCore[parents[i]]], &bc = bind[boneToCore[i]];
			for (int k = 0; k < 3; ++k) restLocal[i].p[k] = bc.p[k] - bp.p[k];
		} else {
			restLocal[i].p[0] = 20.f; restLocal[i].p[1] = 0.f; restLocal[i].p[2] = 0.f;
		}
		liveLocal[i] = restLocal[i];
	}
	// Live CS2 pistol aim puts hand_L forward near wpn
	Pose liveWorldBefore[N];
	for (int i = 0; i < N; ++i) {
		if (parents[i] >= 0) ComposePose(liveWorldBefore[parents[i]], liveLocal[i], liveWorldBefore[i]);
		else liveWorldBefore[i] = liveLocal[i];
	}
	WeaponHoldPose hold;
	hold.valid = true; hold.twoHanded = true; hold.wpnBone = 8;
	hold.equippedWeaponClass = uint8_t(WeaponClass::Pistol);
	hold.handRToWpn.p[0] = 4.f; QuatIdentity(hold.handRToWpn.q);
	SampledPose sp;
	sp.active = true; sp.weight = 1.0f; sp.weaponClass = uint8_t(WeaponClass::Rifle);
	for (int b = 0; b < kCoreBones; ++b) QuatIdentity(sp.rotDelta[b]);
	// HL:A rifle clip twists left arm 90 deg right
	sp.rotDelta[kCoreArmUpperL][2] = std::sin(0.785398f); sp.rotDelta[kCoreArmUpperL][3] = std::cos(0.785398f);
	ApplyPoseToSkeleton(N, parents, boneToCore, bind, restLocal, liveLocal, outWorld, sp, false, nullptr, nullptr, &hold);
	auto dist = [](const Pose &a, const Pose &b) {
		return std::sqrt(std::pow(a.p[0] - b.p[0], 2.f) + std::pow(a.p[1] - b.p[1], 2.f) + std::pow(a.p[2] - b.p[2], 2.f));
	};
	Check(dist(outWorld[8], liveWorldBefore[8]) < 1.5f, "pistol on rifle clip keeps CS2 pistol wpn pose without rifle distortion");

	// 4. Reload / weapon-switch hierarchical nativeArmBlend moves full arms while preserving bone lengths.
	WeaponHoldPose relHold = hold;
	relHold.equippedWeaponClass = uint8_t(WeaponClass::Rifle);
	relHold.nativeArmBlend = 1.0f;
	Pose workLocal[N];
	for (int i = 0; i < N; ++i) workLocal[i] = restLocal[i];
	// Rotate live arm_upper_R +90 deg as in CS2 reload
	workLocal[5].q[2] = std::sin(0.785398f); workLocal[5].q[3] = std::cos(0.785398f);
	Pose liveRel[N];
	for (int i = 0; i < N; ++i) {
		if (parents[i] >= 0) ComposePose(liveRel[parents[i]], workLocal[i], liveRel[i]);
		else liveRel[i] = workLocal[i];
	}
	ApplyPoseToSkeleton(N, parents, boneToCore, bind, restLocal, workLocal, outWorld, sp, false, nullptr, nullptr, &relHold);
	Check(dist(outWorld[7], liveRel[7]) < 0.1f, "reload/switch blend moves full right arm cleanly to CS2 live pose");
	Check(std::fabs(dist(outWorld[5], outWorld[6]) - dist(liveRel[5], liveRel[6])) < 0.05f,
	      "reload/switch hierarchical arm blend preserves exact upper-arm bone length");
}

static void TestNativeStairLegsAndFlatMoveStability() {
	// 8-bone skeleton: pelvis(0), spine(4), thighL(15), kneeL(16), ankleL(17), thighR(18), kneeR(19), ankleR(20)
	constexpr int N = 8;
	const int parents[N] = {-1, 0, 0, 2, 3, 0, 5, 6};
	const int boneToCore[N] = {0, 4, 15, 16, 17, 18, 19, 20};
	Pose bind[kCoreBones], restLocal[N], outWorld[N];
	for (int i = 0; i < kCoreBones; ++i) QuatIdentity(bind[i].q);
	auto setB = [&](int c, float x, float y, float z) { bind[c].p[0] = x; bind[c].p[1] = y; bind[c].p[2] = z; };
	setB(0, 0, 0, 40); setB(4, 0, 0, 54);
	setB(15, 0, 4, 36); setB(16, 0, 4, 20); setB(17, 0, 4, 3);
	setB(18, 0, -4, 36); setB(19, 0, -4, 20); setB(20, 0, -4, 3);
	for (int i = 0; i < N; ++i) {
		QuatIdentity(restLocal[i].q);
		if (parents[i] < 0) restLocal[i] = bind[boneToCore[i]];
		else {
			const Pose &bp = bind[boneToCore[parents[i]]], &bc = bind[boneToCore[i]];
			for (int k = 0; k < 3; ++k) restLocal[i].p[k] = bc.p[k] - bp.p[k];
		}
	}
	SampledPose sp;
	sp.active = true; sp.weight = 1.0f;
	for (int b = 0; b < kCoreBones; ++b) QuatIdentity(sp.rotDelta[b]);

	// Stationary on stairs: native CS2 AnimGraph lifts ankleL up +8" while ankleR stays on lower step.
	Pose liveStair[N];
	for (int i = 0; i < N; ++i) liveStair[i] = restLocal[i];
	liveStair[2].q[1] = -std::sin(0.35f); liveStair[2].q[3] = std::cos(0.35f);
	liveStair[3].q[1] = std::sin(0.45f); liveStair[3].q[3] = std::cos(0.45f);
	Pose liveStairWorld[N];
	for (int i = 0; i < N; ++i) {
		if (parents[i] >= 0) ComposePose(liveStairWorld[parents[i]], liveStair[i], liveStairWorld[i]);
		else liveStairWorld[i] = liveStair[i];
	}
	WeaponHoldPose stHold; stHold.isMoving = false;
	ApplyPoseToSkeleton(N, parents, boneToCore, bind, restLocal, liveStair, outWorld, sp, false, nullptr, nullptr, &stHold);
	Check(std::fabs(outWorld[4].p[2] - liveStairWorld[4].p[2]) < 0.25f,
	      "stair native leg blend follows CS2 left ankle cleanly without flipping knee");

	// Moving on flat ground with small live ankle oscillation (1.5"): body must stay locked to steady bindFloorZ.
	Pose liveWalkA[N], liveWalkB[N], outA[N], outB[N];
	for (int i = 0; i < N; ++i) { liveWalkA[i] = restLocal[i]; liveWalkB[i] = restLocal[i]; }
	liveWalkA[0].p[2] = 40.0f;
	liveWalkB[0].p[2] = 38.5f; // -1.5" walk bounce on live CS2 pose
	WeaponHoldPose mvHold; mvHold.isMoving = true;
	ApplyPoseToSkeleton(N, parents, boneToCore, bind, restLocal, liveWalkA, outA, sp, false, nullptr, nullptr, &mvHold);
	ApplyPoseToSkeleton(N, parents, boneToCore, bind, restLocal, liveWalkB, outB, sp, false, nullptr, nullptr, &mvHold);
	Check(std::fabs(outA[0].p[2] - outB[0].p[2]) < 0.05f,
	      "moving on flat ground ignores CS2 pelvis/ankle vertical oscillation (no leg jitter)");
}

static void TestUpperBodyBoneMaskAndWeaponHold() {
	// 12-bone skeleton: 0:pelvis(0), 1:spine_0(1), 2:spine_1(2), 3:spine_2(3), 4:spine_3(4),
	// 5:arm_upper_R(12), 6:arm_lower_R(13), 7:hand_R(14), 8:leg_upper_L(15), 9:leg_lower_L(16), 10:ankle_L(17), 11:wpn(-1)
	constexpr int N = 12;
	const int parents[N] = {-1, 0, 1, 2, 3, 4, 5, 6, 0, 8, 9, 4};
	const int boneToCore[N] = {0, 1, 2, 3, 4, kCoreArmUpperR, kCoreArmLowerR, kCoreHandR, 15, 16, 17, -1};
	Pose bind[kCoreBones], restLocal[N], liveLocal[N], outWorld[N];
	for (int i = 0; i < kCoreBones; ++i) QuatIdentity(bind[i].q);
	auto setB = [&](int c, float x, float y, float z) { bind[c].p[0] = x; bind[c].p[1] = y; bind[c].p[2] = z; };
	setB(0, 0, 0, 40); setB(1, 0, 0, 44); setB(2, 0, 0, 48); setB(3, 0, 0, 52); setB(4, 0, 0, 56);
	setB(kCoreArmUpperR, 0, -6, 56); setB(kCoreArmLowerR, 8, -6, 56); setB(kCoreHandR, 16, -6, 56);
	setB(15, 0, 4, 36); setB(16, 0, 4, 20); setB(17, 0, 4, 4);
	for (int i = 0; i < N; ++i) {
		QuatIdentity(restLocal[i].q);
		if (parents[i] < 0) restLocal[i] = bind[boneToCore[i]];
		else if (boneToCore[i] >= 0) {
			const Pose &bp = bind[boneToCore[parents[i]]];
			const Pose &bc = bind[boneToCore[i]];
			for (int k = 0; k < 3; ++k) restLocal[i].p[k] = bc.p[k] - bp.p[k];
		} else {
			restLocal[i].p[0] = 6; restLocal[i].p[1] = 0; restLocal[i].p[2] = 0;
		}
		liveLocal[i] = restLocal[i];
	}
	// Simulate CS2 live AnimGraph2 crouch + leg stride on liveLocal
	liveLocal[0].p[2] = 28.0f; // crouched pelvis 12" lower than bind
	liveLocal[8].q[1] = std::sin(0.3f); liveLocal[8].q[3] = std::cos(0.3f); // swinging left thigh

	Pose beforeLiveWorld[N];
	for (int i = 0; i < N; ++i) {
		if (parents[i] >= 0) ComposePose(beforeLiveWorld[parents[i]], liveLocal[i], beforeLiveWorld[i]);
		else beforeLiveWorld[i] = liveLocal[i];
	}

	WeaponHoldPose hold;
	hold.valid = true; hold.twoHanded = false; hold.wpnBone = 11;
	hold.handRToWpn.p[0] = 1.f; hold.handRToWpn.p[1] = 0.5f; hold.handRToWpn.p[2] = 2.f;
	QuatIdentity(hold.handRToWpn.q);

	SampledPose sp;
	sp.active = true; sp.weight = 1.0f; sp.upperBodyOnly = true;
	sp.weaponClass = uint8_t(WeaponClass::Rifle); sp.category = uint8_t(Category::Gesture);
	for (int b = 0; b < kCoreBones; ++b) QuatIdentity(sp.rotDelta[b]);
	// Raise right arm & twist spine_3 60 deg
	const float s30 = std::sin(0.5235988f), c30 = std::cos(0.5235988f);
	sp.rotDelta[4][2] = s30; sp.rotDelta[4][3] = c30;
	sp.rotDelta[kCoreArmUpperR][2] = s30; sp.rotDelta[kCoreArmUpperR][3] = c30;
	sp.rotDelta[kCoreArmLowerR][2] = s30; sp.rotDelta[kCoreArmLowerR][3] = c30;
	sp.rotDelta[kCoreHandR][2] = s30; sp.rotDelta[kCoreHandR][3] = c30;

	bool probeCalled = false;
	auto fakeProbe = [](void *u, int, const float[3], float *gz) -> bool {
		*static_cast<bool *>(u) = true;
		*gz = 8.f;
		return true;
	};
	ApplyPoseToSkeleton(N, parents, boneToCore, bind, restLocal, liveLocal, outWorld, sp, false, fakeProbe, &probeCalled, &hold);

	auto dist = [](const Pose &a, const Pose &b) {
		const float dx = a.p[0] - b.p[0], dy = a.p[1] - b.p[1], dz = a.p[2] - b.p[2];
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	};
	Check(!probeCalled, "upper-body blend skips leg GroundProbe IK");
	Check(dist(outWorld[0], beforeLiveWorld[0]) < 1e-4f, "upper-body keeps live CS2 pelvis position");
	Check(dist(outWorld[8], beforeLiveWorld[8]) < 1e-4f && dist(outWorld[10], beforeLiveWorld[10]) < 1e-4f,
	      "upper-body keeps live CS2 legs & ankles exact");
	Check(std::fabs(dist(outWorld[1], outWorld[0]) - 4.0f) < 1e-3f &&
	      std::fabs(dist(outWorld[2], outWorld[1]) - 4.0f) < 1e-3f &&
	      std::fabs(dist(outWorld[4], outWorld[3]) - 4.0f) < 1e-3f,
	      "upper-body preserves spine bone lengths across feather weights");
	Check(std::fabs(outWorld[4].q[2] - s30) < 0.02f, "upper-body chest applies full HL:A pose");
	Pose expWpn;
	ComposePose(outWorld[7], hold.handRToWpn, expWpn);
	Check(dist(outWorld[11], expWpn) < 0.05f, "upper-body keeps wpn glued to hand_R");
}

static void TestCs2WpnBeforeHandRTwistIsolationAndRootXY() {
	auto dist = [](const Pose &a, const Pose &b) {
		return std::sqrt(std::pow(a.p[0] - b.p[0], 2.f) + std::pow(a.p[1] - b.p[1], 2.f) + std::pow(a.p[2] - b.p[2], 2.f));
	};
	// Real CS2 order: pelvis(0) -> wpnPivot(1) -> wpn(2), BEFORE spine_3(3) -> hand_R(6), plus leg_upper_L(7) -> twist(8) -> ankle_L(9)
	constexpr int N = 10;
	const int parents[N] = {-1, 0, 1, 0, 3, 4, 5, 0, 7, 7};
	const int boneToCore[N] = {0, -1, -1, 4, kCoreArmUpperR, kCoreArmLowerR, kCoreHandR, 15, -1, kCoreAnkleL};
	const bool twistBones[N] = {false, false, false, false, false, false, false, false, true, false};

	Pose bind[kCoreBones], restLocal[N];
	for (int i = 0; i < kCoreBones; ++i) QuatIdentity(bind[i].q);
	auto setB = [&](int c, float x, float y, float z) { bind[c].p[0] = x; bind[c].p[1] = y; bind[c].p[2] = z; };
	setB(0, 0, 0, 42); setB(4, 0, 0, 56);
	setB(kCoreArmUpperR, 0, -6, 56); setB(kCoreArmLowerR, 8, -6, 56); setB(kCoreHandR, 16, -6, 56);
	setB(15, 0, 4, 38); setB(kCoreAnkleL, 0, 4, 4);
	for (int i = 0; i < N; ++i) {
		QuatIdentity(restLocal[i].q);
		if (parents[i] < 0) restLocal[i] = bind[boneToCore[i]];
		else if (boneToCore[i] >= 0 && boneToCore[parents[i]] >= 0) {
			for (int k = 0; k < 3; ++k) restLocal[i].p[k] = bind[boneToCore[i]].p[k] - bind[boneToCore[parents[i]]].p[k];
		} else if (i == 1) {
			restLocal[i].p[0] = 2.f; restLocal[i].p[1] = 0.f; restLocal[i].p[2] = 10.f;
		} else if (i == 2) {
			restLocal[i].p[0] = 14.f; restLocal[i].p[1] = -6.f; restLocal[i].p[2] = 4.f;
		} else if (i == 8) {
			restLocal[i].p[0] = 0.f; restLocal[i].p[1] = 0.f; restLocal[i].p[2] = -8.f;
		}
	}

	Pose liveLocal[N], liveWorld[N], outWorld[N];
	for (int i = 0; i < N; ++i) liveLocal[i] = restLocal[i];
	// Simulate live CS2 pistol arm stance (+30 deg pitch on arm_upper_R) & bogus 60 deg live leg twist
	liveLocal[4].q[1] = std::sin(-0.261799f); liveLocal[4].q[3] = std::cos(-0.261799f);
	liveLocal[8].q[2] = std::sin(0.523599f); liveLocal[8].q[3] = std::cos(0.523599f);
	for (int i = 0; i < N; ++i) {
		if (parents[i] >= 0) ComposePose(liveWorld[parents[i]], liveLocal[i], liveWorld[i]);
		else liveWorld[i] = liveLocal[i];
	}
	Pose expectedHandToWpn;
	LocalFromWorld(liveWorld[6], liveWorld[2], expectedHandToWpn);

	WeaponHoldPose hold;
	hold.valid = true; hold.wpnBone = 2; hold.twistBones = twistBones;
	hold.equippedWeaponClass = uint8_t(WeaponClass::Pistol);
	hold.nativeArmBlend = 1.0f;
	hold.handRToWpn = expectedHandToWpn;

	SampledPose sp;
	sp.active = true; sp.weight = 1.0f; sp.weaponClass = uint8_t(WeaponClass::Rifle);
	for (int b = 0; b < kCoreBones; ++b) QuatIdentity(sp.rotDelta[b]);
	// Rotate HL:A pelvis & spine_3 +45 deg Z; twist thigh would ruin hips unless twist bone is neutralized
	sp.rotDelta[0][2] = std::sin(0.392699f); sp.rotDelta[0][3] = std::cos(0.392699f);
	sp.rotDelta[4][2] = std::sin(0.392699f); sp.rotDelta[4][3] = std::cos(0.392699f);

	for (bool upper : {false, true}) {
		Pose wl[N];
		for (int i = 0; i < N; ++i) wl[i] = liveLocal[i];
		sp.upperBodyOnly = upper;
		ApplyPoseToSkeleton(N, parents, boneToCore, bind, restLocal, wl, outWorld, sp, false, nullptr, nullptr, &hold);
		Pose expWpn;
		ComposePose(outWorld[6], expectedHandToWpn, expWpn);
		Check(dist(outWorld[2], expWpn) < 0.05f,
		      upper ? "cs2 order upperBody: wpn (index < hand_R) locks to hand_R"
		            : "cs2 order fullBody: wpn (index < hand_R) locks to hand_R during switch/pistol");
		if (!upper) {
			Check(std::fabs(outWorld[8].q[2]) < 0.05f, "leg_upper_L_TWIST ignores unrelated live AnimGraph twist");
		}
	}

	// Stationary applyRootXY=true cancels ankle slide when pelvis compensates thigh tilt
	Pose wlRoot[N];
	for (int i = 0; i < N; ++i) wlRoot[i] = restLocal[i];
	sp.upperBodyOnly = false;
	for (int b = 0; b < kCoreBones; ++b) QuatIdentity(sp.rotDelta[b]);
	sp.pelvisDelta[0] = 4.0f; sp.pelvisDelta[1] = -2.0f; sp.pelvisDelta[2] = 0.f;
	// Pitch leg_upper_L slightly backward so ankle shifts -4" in local X relative to pelvis -> compensated by +4" pelvisDelta!
	const float sinA = std::sin(0.059f), cosA = std::cos(0.059f);
	sp.rotDelta[15][1] = sinA; sp.rotDelta[15][3] = cosA;
	ApplyPoseToSkeleton(N, parents, boneToCore, bind, restLocal, wlRoot, outWorld, sp, true, nullptr, nullptr, nullptr);
	Check(std::fabs(outWorld[9].p[0] - 0.f) < 0.6f, "applyRootXY=true keeps planted ankle near X=0 instead of sliding");
}

int main() {
	// 1. Pack load & lookup
	const auto pack = BuildTestPack();
	Library lib;
	Check(LoadPack(pack.data(), pack.size(), lib), "LoadPack succeeds");
	Check(lib.clips.size() == 2, "2 clips loaded");
	Check(lib.Find("clip_a") != nullptr && lib.Find("clip_a")->loopable, "clip_a loopable");
	Check(std::fabs(lib.Find("clip_a")->Duration() - 1.0f) < 1e-4f, "clip_a duration 1.0s");

	// 2. Cue playback with speed, startTime, duration, blendIn/Out, looping
	Cue c1;
	c1.startTick = 0; c1.clip = "clip_a"; c1.speed = 2.0f; c1.loop = true;
	c1.startTime = 0.0f; c1.duration = 2.0f; c1.blendIn = 0.2f; c1.blendOut = 0.2f;
	std::vector<Cue> cues = {c1};

	auto sMid = EvaluateCues(lib, cues, 0.25); // 0.25s * 2x = 0.5s into clip -> z = -5
	Check(sMid.active && std::fabs(sMid.weight - 1.0f) < 1e-4f, "mid weight is 1");
	Check(std::fabs(sMid.pelvisDelta[2] - (-5.0f)) < 0.05f, "speed 2x samples z=-5 at 0.25s");

	auto sWrap = EvaluateCues(lib, cues, 0.75); // 0.75s * 2x = 1.5s -> wraps to 0.5s -> z = -5
	Check(sWrap.active && std::fabs(sWrap.pelvisDelta[2] - (-5.0f)) < 0.05f, "loop wraps cleanly");

	auto sFadeOut = EvaluateCues(lib, cues, 1.9); // 0.1s left of 0.2s blendOut -> weight 0.5
	Check(sFadeOut.active && std::fabs(sFadeOut.weight - 0.5f) < 0.05f, "blendOut weight ~0.5");

	auto sDone = EvaluateCues(lib, cues, 2.05);
	Check(!sDone.active, "inactive after duration");

	// 3. Crossfade transition between two cues
	Cue c2;
	c2.startTick = 64; // 1.0s
	c2.clip = "clip_b"; c2.speed = 1.0f; c2.loop = false; c2.blendIn = 0.2f; c2.blendOut = 0.2f;
	cues.push_back(c2);
	auto sCross = EvaluateCues(lib, cues, 1.1); // halfway through c2 blendIn
	Check(sCross.active && sCross.weight > 0.9f, "crossfade keeps full blended weight");

	// 4. Companion sequence round-trip
	CompanionSequence seq;
	seq.demoStartTick = 1234;
	CompanionActor act;
	act.steamId = 76561198000000001ULL;
	act.name = "take1";
	act.delayTicks = 32;
	act.placedActor = true;
	cues[0].upperBodyOnly = true;
	act.cues = cues;
	seq.actors.push_back(act);
	auto bytes = SerializeCompanion(seq);
	CompanionSequence seq2;
	Check(DeserializeCompanion(bytes.data(), bytes.size(), seq2), "companion round-trip succeeds");
	Check(seq2.demoStartTick == 1234 && seq2.actors.size() == 1, "companion header matches");
	Check(seq2.actors[0].steamId == act.steamId && seq2.actors[0].placedActor, "companion actor matches");
	Check(seq2.actors[0].cues.size() == 2 && seq2.actors[0].cues[1].clip == "clip_b", "companion cues match");
	Check(seq2.actors[0].cues[0].upperBodyOnly && !seq2.actors[0].cues[1].upperBodyOnly, "companion preserves upperBodyOnly flag");

	TestLegIK();
	TestRootMotion();
	TestWeaponHoldAndFingers();
	TestBlendSampledPoses();
	TestDirectionalWithIdleAndWalk();
	TestZeroWeightDoesNotCollapse();
	TestFootGroundSanitizerAndGrounding();
	TestRecommendedMaxSpeeds();
	TestUpperBodyBoneMaskAndWeaponHold();
	TestNativeFeetPistolHoldRootVelAndBlendPair();
	TestNativeStairLegsAndFlatMoveStability();
	TestCs2WpnBeforeHandRTwistIsolationAndRootXY();

	// 5. Directional locomotion selection (walk/run/back/strafe L/R)
	Check(SelectDirectionalClip("hla_walk_fwd", 90.f, 0.f) == "hla_walk_fwd", "walk forward");
	Check(SelectDirectionalClip("hla_walk_fwd", 200.f, 0.f) == "hla_run_fwd", "run forward above 145");
	Check(SelectDirectionalClip("hla_walk_fwd", -180.f, 0.f) == "hla_run_back", "run backward above 145");
	Check(SelectDirectionalClip("hla_walk_fwd", -80.f, 0.f) == "hla_walk_back", "walk backward");
	Check(SelectDirectionalClip("hla_walk_fwd", 0.f, 100.f) == "hla_walk_strafe_r", "strafe right");
	Check(SelectDirectionalClip("hla_walk_fwd", 0.f, -100.f) == "hla_walk_strafe_l", "strafe left");
	Check(SelectDirectionalClip("hla_combat_idle", 200.f, 0.f) == "hla_combat_idle", "non-directional clip unchanged");
	Check(IsDirectionalBaseClip("hla_walk_fwd"), "walk forward is a directional base clip");
	Check(IsDirectionalBaseClip("hla_run_fwd"), "run forward is a directional base clip");
	Check(!IsDirectionalBaseClip("hla_walk_fwd_r"), "walk forward-right plays verbatim without directional hijacking");
	Check(!IsDirectionalBaseClip("hla_aim_to_ready"), "aim-to-ready idle one-shot plays verbatim without directional hijacking");

	if (g_fails == 0) std::printf("ActorAnimationsMathTests: ALL PASSED\n");
	return g_fails ? 1 : 0;
}
