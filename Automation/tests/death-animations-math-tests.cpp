// Behaviour tests for AfxHookSource2/Filmmaker/Movie/DeathAnimationsMath.h.
#include "../../AfxHookSource2/Filmmaker/Movie/DeathAnimationsMath.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace Filmmaker::DeathAnimationsMath;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)
static bool Near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }

// A standing clip body layout facing +X (thighs at +/-Y), pelvis at the origin, floor at z = 0.
static void StandingBase(float base[kBodies * 3]) {
	for (int i = 0; i < kBodies; ++i) { base[i * 3] = 0; base[i * 3 + 1] = 0; base[i * 3 + 2] = 20; }
	auto set = [&](int b, float x, float y, float z) { base[b * 3] = x; base[b * 3 + 1] = y; base[b * 3 + 2] = z; };
	set(kThighLeft, 0, 4, 36); set(1, 0, 4, 20); set(kThighRight, 0, -4, 36); set(3, 0, -4, 20);
	set(4, 1, 0, 64); set(kPelvis, 0, 0, 40); set(6, 0, 0, 54);
	set(7, 0, 8, 56); set(8, 2, 10, 46); set(9, 0, -8, 56); set(10, 2, -10, 46);
	set(11, 8, 10, 40); set(12, 8, -10, 40); set(13, 0, 4, 4); set(14, 0, -4, 4);
}

static void TestCategories() {
	DeathFacts d;
	d.hitgroup = 1; CHECK(CategoryFor(d) == Category::Head);
	d = {}; d.hitgroup = 2; CHECK(CategoryFor(d) == Category::Torso);
	d.planarSpeed = 250; CHECK(CategoryFor(d) == Category::Torso); // speed is the stance, not the area
	d = {}; d.hitgroup = 6; d.planarSpeed = 250; CHECK(CategoryFor(d) == Category::LeftLeg); // a leg hit is a leg hit
	d = {}; d.hitgroup = 2; d.headshot = true; CHECK(CategoryFor(d) == Category::Head);
	d = {}; d.burning = true; d.hitgroup = 1; CHECK(CategoryFor(d) == Category::Fire);
	d = {}; d.explosive = true; CHECK(CategoryFor(d) == Category::Explosion);
	d = {}; d.planarSpeed = 150; CHECK(CategoryFor(d) == Category::Generic); // walking is not a sprint
	// MW2019 has no arm clips: an arm hit must fall back to torso before anything unrelated.
	Category chain[5];
	int n = FallbackChain(Category::LeftArm, chain);
	CHECK(n == 3 && chain[0] == Category::LeftArm && chain[1] == Category::Torso && chain[2] == Category::Generic);
	n = FallbackChain(Category::Neck, chain);
	CHECK(chain[1] == Category::Head);
	n = FallbackChain(Category::Torso, chain);
	CHECK(n == 2 && chain[1] == Category::Generic);
	for (int c = 0; c < (int)Category::Count; ++c) {
		n = FallbackChain((Category)c, chain);
		for (int i = 0; i < n; ++i) CHECK(chain[i] != Category::Manual || c == (int)Category::Manual);
	}
	CHECK(!Pickable(Category::Manual));
}

static void TestSides() {
	const double facing[2] = {1, 0};
	const double front[2] = {5, 0.5}, back[2] = {-5, 0}, left[2] = {0, 5}, right[2] = {0.2, -5};
	CHECK(HitSideFrom(facing, front) == HitSide::Front);
	CHECK(HitSideFrom(facing, back) == HitSide::Back);
	CHECK(HitSideFrom(facing, left) == HitSide::Left);
	CHECK(HitSideFrom(facing, right) == HitSide::Right);
	float base[kBodies * 3];
	StandingBase(base);
	double f[2];
	CHECK(FacingFromHips(base + kThighLeft * 3, base + kThighRight * 3, f) && Near(f[0], 1) && Near(f[1], 0));
}

// A crouching victim is crouched whatever the pose says; the pose only catches a missed flag.
static void TestVictimStance() {
	CHECK(VictimStance(true, true, 35.0, 0) == Stance::Crouched);
	CHECK(VictimStance(false, true, 35.0, 0) == Stance::Standing);
	CHECK(VictimStance(false, false, 20.0, 0) == Stance::Crouched);
	CHECK(VictimStance(false, true, 20.0, 0) == Stance::Crouched);
	CHECK(VictimStance(true, false, 35.0, 0) == Stance::Standing); // a stale flag is not evidence
	// Running: run speed and upright. Walking is standing; crouching outranks speed.
	CHECK(VictimStance(false, true, 35.0, 250.0) == Stance::Running);
	CHECK(VictimStance(false, true, 35.0, 130.0) == Stance::Standing); // walking
	CHECK(VictimStance(false, true, 35.0, 131.0) == Stance::Running);
	CHECK(VictimStance(true, true, 35.0, 250.0) == Stance::Crouched);
}

static ClipTags Tag(Source src, Category cat, HitSide side, Stance stance) {
	ClipTags t; t.source = src; t.category = cat; t.side = side; t.stance = stance; return t;
}

// Stance and source never relax; body area outranks hit side; sides go exact, any, other.
static void TestSelectPool() {
	const ClipTags clips[] = {
		Tag(Source::Mw2019, Category::LeftLeg, HitSide::Back, Stance::Standing),   // 0
		Tag(Source::Mw2019, Category::LeftLeg, HitSide::Right, Stance::Standing),  // 1
		Tag(Source::Mw2019, Category::LeftLeg, HitSide::Any, Stance::Standing),    // 2
		Tag(Source::Mw2019, Category::Torso, HitSide::Right, Stance::Standing),    // 3
		Tag(Source::Mw2019, Category::Head, HitSide::Front, Stance::Crouched),     // 4
		Tag(Source::Mw2019, Category::Torso, HitSide::Back, Stance::Crouched),     // 5
		Tag(Source::Classic, Category::LeftLeg, HitSide::Front, Stance::Standing), // 6
		Tag(Source::Mw2019, Category::Manual, HitSide::Front, Stance::Standing),   // 7
		Tag(Source::Mw2019, Category::Head, HitSide::Front, Stance::Prone),        // 8
	};
	const int n = sizeof(clips) / sizeof(clips[0]);
	int out[16];
	Relax relax;
	WeaponClass tier;
	// Exact: left leg from the right.
	int k = SelectPool(clips, n, Source::Mw2019, Category::LeftLeg, HitSide::Right, Stance::Standing, WeaponClass::Any, out, 16, relax, tier);
	CHECK(k == 1 && out[0] == 1 && relax == Relax::Exact);
	// No left-leg clip from the front: the side-less leg clip beats a front-hit clip of another source.
	k = SelectPool(clips, n, Source::Mw2019, Category::LeftLeg, HitSide::Front, Stance::Standing, WeaponClass::Any, out, 16, relax, tier);
	CHECK(k == 1 && out[0] == 2 && relax == Relax::AnySide);
	// Unknown side: every left-leg clip of the stance.
	k = SelectPool(clips, n, Source::Mw2019, Category::LeftLeg, HitSide::Any, Stance::Standing, WeaponClass::Any, out, 16, relax, tier);
	CHECK(k == 3 && relax == Relax::AnySide);
	// A right-arm hit has no clip: torso, keeping the side.
	k = SelectPool(clips, n, Source::Mw2019, Category::RightArm, HitSide::Right, Stance::Standing, WeaponClass::Any, out, 16, relax, tier);
	CHECK(k == 1 && out[0] == 3 && relax == Relax::FallbackArea);
	// Crouched victim, leg hit: only crouched clips, so a crouched torso clip, never a standing leg.
	k = SelectPool(clips, n, Source::Mw2019, Category::LeftLeg, HitSide::Back, Stance::Crouched, WeaponClass::Any, out, 16, relax, tier);
	CHECK(k == 1 && out[0] == 5 && relax == Relax::FallbackArea);
	// Crouched head hit from behind: the crouched head clip answers the front -- area beats side.
	k = SelectPool(clips, n, Source::Mw2019, Category::Head, HitSide::Back, Stance::Crouched, WeaponClass::Any, out, 16, relax, tier);
	CHECK(k == 1 && out[0] == 4 && relax == Relax::OtherSide);
	// No crouched clip at all in a source: nothing, the ragdoll plays.
	k = SelectPool(clips, n, Source::Classic, Category::LeftLeg, HitSide::Front, Stance::Crouched, WeaponClass::Any, out, 16, relax, tier);
	CHECK(k == 0 && relax == Relax::NoClip);
	// A running victim only gets running clips: the standing leg clips are not candidates.
	const ClipTags running[] = {
		Tag(Source::Mw2019, Category::LeftLeg, HitSide::Back, Stance::Standing),
		Tag(Source::Mw2019, Category::Torso, HitSide::Back, Stance::Running),
	};
	k = SelectPool(running, 2, Source::Mw2019, Category::LeftLeg, HitSide::Back, Stance::Running, WeaponClass::Any, out, 16, relax, tier);
	CHECK(k == 1 && out[0] == 1 && relax == Relax::FallbackArea);
	// Manual and prone clips are never picked for a standing death.
	k = SelectPool(clips, n, Source::Mw2019, Category::Head, HitSide::Front, Stance::Standing, WeaponClass::Any, out, 16, relax, tier);
	for (int i = 0; i < k; ++i) CHECK(out[i] != 7 && out[i] != 8);
}

// Weapon clips outrank universal ones for their weapon, and only for it.
static void TestWeaponPriority() {
	CHECK(WeaponClassOf("xm1014") == WeaponClass::Shotgun && WeaponClassOf("weapon_nova") == WeaponClass::Shotgun);
	CHECK(WeaponClassOf("awp") == WeaponClass::Sniper && WeaponClassOf("ssg08") == WeaponClass::Sniper);
	CHECK(WeaponClassOf("ak47") == WeaponClass::Any && WeaponClassOf(nullptr) == WeaponClass::Any);
	ClipTags clips[4];
	clips[0] = Tag(Source::Classic, Category::Torso, HitSide::Back, Stance::Standing);   // universal, exact
	clips[1] = Tag(Source::Classic, Category::Generic, HitSide::Front, Stance::Standing); // shotgun, wrong area+side
	clips[1].weapon = WeaponClass::Shotgun;
	clips[2] = Tag(Source::Classic, Category::Torso, HitSide::Back, Stance::Standing);   // heavy
	clips[2].weapon = WeaponClass::Heavy;
	clips[3] = Tag(Source::Classic, Category::Torso, HitSide::Back, Stance::Crouched);   // shotgun, crouched
	clips[3].weapon = WeaponClass::Shotgun;
	int out[8]; Relax relax; WeaponClass tier;
	// A shotgun kill takes the shotgun death even though the universal clip matches better...
	int k = SelectPool(clips, 4, Source::Classic, Category::Torso, HitSide::Back, Stance::Standing,
		WeaponClass::Shotgun, out, 8, relax, tier);
	CHECK(k == 1 && out[0] == 1 && tier == WeaponClass::Shotgun);
	// ...but never across stances: the crouched shotgun clip is not a standing candidate.
	CHECK(k == 1 && out[0] != 3);
	// A sniper kill has no sniper clip here: heavy next.
	k = SelectPool(clips, 4, Source::Classic, Category::Torso, HitSide::Back, Stance::Standing,
		WeaponClass::Sniper, out, 8, relax, tier);
	CHECK(k == 1 && out[0] == 2 && tier == WeaponClass::Heavy);
	// A rifle kill plays the universal clip, not the shotgun or heavy ones.
	k = SelectPool(clips, 4, Source::Classic, Category::Torso, HitSide::Back, Stance::Standing,
		WeaponClass::Any, out, 8, relax, tier);
	CHECK(k == 1 && out[0] == 0 && tier == WeaponClass::Any && relax == Relax::Exact);
}

// The placed clip must reproduce the clip exactly: same relative pose, only turned and moved.
static void TestPlacementIsRigid() {
	float base[kBodies * 3], death[kBodies * 3];
	StandingBase(base);
	const double yaw = 1.1; // the victim faces 63 degrees
	const float at[3] = {512, -300, 96};
	for (int i = 0; i < kBodies; ++i) {
		const double x = base[i * 3], y = base[i * 3 + 1];
		death[i * 3] = float(x * std::cos(yaw) - y * std::sin(yaw) + at[0]);
		death[i * 3 + 1] = float(x * std::sin(yaw) + y * std::cos(yaw) + at[1]);
		death[i * 3 + 2] = base[i * 3 + 2] + at[2];
	}
	const Placement pl = SolvePlacement(base, death, at[2]);
	CHECK(pl.solved);
	CHECK(Near(Yaw(pl), yaw, 1e-4));
	for (int i = 0; i < kBodies; ++i) {
		Pose clip, world;
		for (int k = 0; k < 3; ++k) clip.p[k] = base[i * 3 + k];
		Place(pl, clip, world);
		for (int k = 0; k < 3; ++k) CHECK(Near(world.p[k], death[i * 3 + k], 1e-2));
	}
	// Orientation is turned by the same yaw: a body facing +X faces the victim's way.
	Pose clip, world;
	Place(pl, clip, world);
	const double wyaw = 2 * std::atan2(world.q[2], world.q[3]);
	CHECK(Near(wyaw, yaw, 1e-4));
}

// The pelvis lands over the corpse's pelvis and the clip floor on the given floor.
static void TestPlacementAnchors() {
	float base[kBodies * 3], death[kBodies * 3];
	StandingBase(base);
	for (int i = 0; i < kBodies * 3; ++i) death[i] = base[i];
	death[kPelvis * 3] += 3; // a crouched, offset death pose
	for (int i = 0; i < kBodies; ++i) death[i * 3 + 2] -= 12;
	const Placement pl = SolvePlacement(base, death, -64);
	Pose clip, world;
	for (int k = 0; k < 3; ++k) clip.p[k] = base[kPelvis * 3 + k];
	Place(pl, clip, world);
	CHECK(Near(world.p[0], death[kPelvis * 3], 1e-2));
	CHECK(Near(world.p[1], death[kPelvis * 3 + 1], 1e-2));
	CHECK(Near(world.p[2], base[kPelvis * 3 + 2] - 64, 1e-3)); // authored height above the floor
}

// Sampling returns the authored frames and interpolates between them.
static void TestSample() {
	float base[kBodies * 3];
	StandingBase(base);
	const int frames = 3;
	std::vector<float> data(size_t(frames) * kBodies * kFloatsPerBody, 0.f);
	for (int f = 0; f < frames; ++f)
		for (int b = 0; b < kBodies; ++b) {
			float* e = data.data() + (size_t(f) * kBodies + b) * kFloatsPerBody;
			e[0] = float(f * 10); e[6] = 1; // displacement along x, identity rotation
		}
	// frame 2 turns the pelvis 90 degrees about Z
	float* last = data.data() + (size_t(2) * kBodies + kPelvis) * kFloatsPerBody;
	last[5] = float(std::sin(0.785398)); last[6] = float(std::cos(0.785398));
	Pose p;
	Sample(base, data.data(), frames, 30.0, 0.0, kPelvis, p);
	CHECK(Near(p.p[0], 0) && Near(p.p[2], 40));
	Sample(base, data.data(), frames, 30.0, 1.0 / 30.0, kPelvis, p);
	CHECK(Near(p.p[0], 10));
	Sample(base, data.data(), frames, 30.0, 1.5 / 30.0, kPelvis, p);
	CHECK(Near(p.p[0], 15));
	Sample(base, data.data(), frames, 30.0, 99.0, kPelvis, p); // held on the last frame
	CHECK(Near(p.p[0], 20) && Near(2 * std::atan2(p.q[2], p.q[3]), 1.570796, 1e-4));
	CHECK(Near(Duration(frames, 30.0), 2.0 / 30.0));
}

// A clip that starts leaning keeps the victim's facing: the hips decide, not the torso.
static void TestPlacementIgnoresLean() {
	float base[kBodies * 3], death[kBodies * 3];
	StandingBase(base);
	for (int i = 0; i < kBodies * 3; ++i) death[i] = base[i];
	// The clip's frame 0 throws head, chest and arms 30 units to its left (+Y); the hips face +X.
	for (int b : {4, 6, 7, 8, 9, 10, 11, 12}) base[b * 3 + 1] += 30.f;
	const Placement pl = SolvePlacement(base, death, 0.0);
	CHECK(pl.solved && Near(Yaw(pl), 0.0, 1e-4));
}

// With no blend the clip is applied as authored from the very first frame.
static void TestNoBlendIsExact() {
	CHECK(BlendIn(0.0, 0.0) == 1.0);
	CHECK(BlendIn(0.0, 0.2) == 0.0 && BlendIn(0.2, 0.2) == 1.0 && Near(BlendIn(0.1, 0.2), 0.5));
	Pose from, to, out;
	from.p[0] = 100; to.p[0] = 7;
	Mix(from, to, BlendIn(0.0, 0.0), out);
	CHECK(Near(out.p[0], 7));
}

// The release carries the clip's own motion: linear from the position change, angular in
// degrees/s about the world axis the body turned around.
static void TestReleaseVelocity() {
	Pose a, b;
	b.p[0] = 2.f; b.p[2] = -1.f;
	const double turn = 0.3; // radians about Z over dt
	b.q[2] = float(std::sin(turn / 2)); b.q[3] = float(std::cos(turn / 2));
	float lin[3], ang[3];
	CHECK(ReleaseVelocity(a, b, 0.1, lin, ang));
	CHECK(Near(lin[0], 20) && Near(lin[1], 0) && Near(lin[2], -10));
	CHECK(Near(ang[0], 0) && Near(ang[1], 0) && Near(ang[2], turn / 0.1 * 57.29578, 1e-2));
	// q and -q are one orientation: no spurious full turn.
	Pose c = a;
	for (float& v : c.q) v = -v;
	CHECK(ReleaseVelocity(a, c, 0.1, lin, ang) && Near(ang[0], 0) && Near(ang[2], 0));
	float big[3] = {3000, 0, 0}, spin[3] = {0, 0, 9000};
	ClampRelease(big, spin);
	CHECK(Near(big[0], 600) && Near(spin[2], 1440));
	CHECK(!ReleaseVelocity(a, b, 0.0, lin, ang));
}

static void TestFollowDriven() {
	// root(0) -> pelvis(1, driven by body 5) -> spine(2) -> head(3, driven by body 4) -> hat(4).
	const int parents[5] = {-1, 0, 1, 2, 3};
	const int driven[5] = {-1, kPelvis, -1, 4, -1};
	Pose old[5], out[5], bodies[kBodies];
	for (int b = 0; b < 5; ++b) { old[b].p[2] = float(b * 10); }
	// Pelvis moves +5 X and turns 90 deg about Z; head is placed explicitly.
	bodies[kPelvis].p[0] = 5; bodies[kPelvis].p[2] = 10;
	bodies[kPelvis].q[2] = float(std::sin(0.785398)); bodies[kPelvis].q[3] = float(std::cos(0.785398));
	bodies[4].p[0] = 1; bodies[4].p[1] = 2; bodies[4].p[2] = 3;
	old[2].p[0] = 4; // spine sits 4 X in front of the pelvis
	FollowDriven(parents, 5, driven, bodies, old, out);
	CHECK(Near(out[0].p[2], 0) && Near(out[0].q[3], 1));                 // root untouched
	CHECK(Near(out[1].p[0], 5) && Near(out[1].p[2], 10));                 // driven exactly
	CHECK(Near(out[2].p[0], 5) && Near(out[2].p[1], 4) && Near(out[2].p[2], 20)); // offset turned 90 deg
	CHECK(Near(out[2].q[2], std::sin(0.785398)));                         // and so is its rotation
	CHECK(Near(out[3].p[0], 1) && Near(out[3].p[1], 2) && Near(out[3].p[2], 3));
	CHECK(Near(out[4].p[0], 1) && Near(out[4].p[1], 2) && Near(out[4].p[2], 13)); // rides the head
}

static double Dist(const Pose& a, const Pose& b) {
	const double x = a.p[0] - b.p[0], y = a.p[1] - b.p[1], z = a.p[2] - b.p[2];
	return std::sqrt(x * x + y * y + z * z);
}

static void TestLimbsOutOfBody() {
	// StandingBase faces +X: arms hang at +/-Y, legs apart -- nothing may move.
	float base[kBodies * 3];
	StandingBase(base);
	Pose clear[kBodies];
	for (int b = 0; b < kBodies; ++b) for (int k = 0; k < 3; ++k) clear[b].p[k] = base[b * 3 + k];
	Pose untouched[kBodies];
	for (int b = 0; b < kBodies; ++b) untouched[b] = clear[b];
	CHECK(KeepLimbsOutOfBody(clear) == 0);
	for (int b = 0; b < kBodies; ++b) CHECK(Near(clear[b].p[0], untouched[b].p[0]) && Near(clear[b].q[3], 1));

	// Left hand driven into the middle of the chest: pushed out, lengths kept, right arm untouched.
	Pose folded[kBodies];
	for (int b = 0; b < kBodies; ++b) folded[b] = clear[b];
	folded[kHandLeft].p[0] = 1; folded[kHandLeft].p[1] = 0; folded[kHandLeft].p[2] = 48;
	const double upper = Dist(folded[kUpperArmLeft], folded[kForearmLeft]);
	const double lower = Dist(folded[kForearmLeft], folded[kHandLeft]);
	CHECK(KeepLimbsOutOfBody(folded) == 1);
	const Pose& h = folded[kHandLeft];
	CHECK((h.p[0] * h.p[0]) / (5.8 * 5.8) + (h.p[1] * h.p[1]) / (6.3 * 6.3) >= 0.99); // torso 5 x 5.5 + hand 0.8
	CHECK(Near(Dist(folded[kUpperArmLeft], folded[kForearmLeft]), upper, 1e-2));
	CHECK(Near(Dist(folded[kForearmLeft], folded[kHandLeft]), lower, 1e-2));
	CHECK(Near(folded[kHandRight].p[1], clear[kHandRight].p[1]));

	// Left ankle crossed into the right shin: the legs end up apart.
	Pose crossed[kBodies];
	for (int b = 0; b < kBodies; ++b) crossed[b] = clear[b];
	crossed[kAnkleLeft].p[1] = -4; crossed[kAnkleLeft].p[2] = 10;
	CHECK(KeepLimbsOutOfBody(crossed) >= 1);
	const double dy = crossed[kAnkleLeft].p[1] - crossed[kAnkleRight].p[1], dx = crossed[kAnkleLeft].p[0] - crossed[kAnkleRight].p[0];
	CHECK(std::sqrt(dx * dx + dy * dy) > 3.2);

	// A wrist outside the vest can still curl the fingers into it. Use the hand's orientation,
	// not an imaginary continuation of the forearm, to clear the whole glove.
	Pose wrist[kBodies]; std::memcpy(wrist, clear, sizeof(wrist));
	wrist[kHandLeft].p[0] = 8; wrist[kHandLeft].p[1] = 0; wrist[kHandLeft].p[2] = 48;
	wrist[kHandLeft].q[2] = 1; wrist[kHandLeft].q[3] = 0; // fingers toward -X, into the vest
	CHECK(KeepLimbsOutOfBody(wrist) >= 1);
	const float fingers[3] = {4, 0, 0}; float turned[3];
	QuatRotate(wrist[kHandLeft].q, fingers, turned);
	double tip[3]; for (int k = 0; k < 3; ++k) tip[k] = wrist[kHandLeft].p[k] + turned[k];
	const detail::Capsule vest = {{0,0,43}, {0,0,53}, {0,1,0}, 5.0, 5.5, 2.0};
	double outside[3] = {tip[0],tip[1],tip[2]}; detail::PushOut(vest, 0.5, outside);
	const double penetration = std::sqrt((tip[0]-outside[0])*(tip[0]-outside[0]) + (tip[1]-outside[1])*(tip[1]-outside[1]) + (tip[2]-outside[2])*(tip[2]-outside[2]));
	CHECK(penetration < 0.1);
}

// No popping: sweep a hand up through the chest and out past the collarbone in small steps; the
// corrected hand must never jump by much more than the input moved.
static void TestLimbsNoPop() {
	float base[kBodies * 3];
	StandingBase(base);
	double worst = 0, last[3] = {0, 0, 0};
	for (int i = 0; i <= 400; ++i) {
		Pose pose[kBodies];
		for (int b = 0; b < kBodies; ++b) for (int k = 0; k < 3; ++k) pose[b].p[k] = base[b * 3 + k];
		pose[kHandLeft].p[0] = 2; pose[kHandLeft].p[1] = 1;
		pose[kHandLeft].p[2] = float(40 + i * 0.1); // 40 -> 80: through the torso and out the top
		KeepLimbsOutOfBody(pose);
		if (i) {
			const double x = pose[kHandLeft].p[0] - last[0], y = pose[kHandLeft].p[1] - last[1], z = pose[kHandLeft].p[2] - last[2];
			const double d = std::sqrt(x * x + y * y + z * z);
			if (d > worst) worst = d;
		}
		for (int k = 0; k < 3; ++k) last[k] = pose[kHandLeft].p[k];
	}
	CHECK(worst < 1.0); // input steps 0.1; the old flat-ended column jumped ~6 here
}

static double RotationDistance(const float a[4], const float b[4]) {
	double dot = 0;
	for (int k = 0; k < 4; ++k) dot += double(a[k]) * b[k];
	return 2 * std::acos((std::min)(1.0, std::fabs(dot)));
}

static void TestClearanceTrack() {
	// Opposite exit sides used to flip a limb in one frame. Bound the correction's angular
	// speed through a deliberately abrupt reversal, including equivalent quaternion signs.
	std::vector<ClearanceFrame> abrupt(121);
	for (int f = 0; f < 121; ++f) {
		const double angle = f < 60 ? 2.8 : -2.8;
		abrupt[f].swing[1][2] = float(std::sin(angle / 2));
		abrupt[f].swing[1][3] = float(std::cos(angle / 2));
		if (f % 2) for (float& v : abrupt[f].swing[1]) v = -v;
	}
	SmoothClearanceTrack(abrupt);
	for (size_t f = 1; f < abrupt.size(); ++f)
		CHECK(RotationDistance(abrupt[f - 1].swing[1], abrupt[f].swing[1]) <= kClearanceAngularSpeed / kClearanceFps + 1e-4);

	// Sweep an authored hand through the torso axis. Interpolation must preserve lengths,
	// rotate the hand with the forearm, and return the same pose after an unrelated seek.
	float base[kBodies * 3]; StandingBase(base);
	constexpr int count = 31;
	std::vector<float> frames(count * kBodies * kFloatsPerBody, 0.f);
	for (int f = 0; f < count; ++f) for (int b = 0; b < kBodies; ++b) {
		float* e = frames.data() + (f * kBodies + b) * kFloatsPerBody;
		e[6] = 1;
		if (b == kHandLeft) {
			e[0] = float(-3 + f * 0.2) - base[b * 3];
			e[1] = -base[b * 3 + 1];
			e[2] = 48 - base[b * 3 + 2];
		}
	}
	const auto track = BuildClearanceTrack(base, frames.data(), count, 30);
	CHECK(track.size() == 121);
	for (int f = 0; f < 240; ++f) {
		const double t = f / 240.0;
		Pose raw[kBodies], out[kBodies], again[kBodies];
		for (int b = 0; b < kBodies; ++b) {
			Sample(base, frames.data(), count, 30, t, b, raw[b]); out[b] = again[b] = raw[b];
		}
		ApplyClearanceTrack(out, track, t);
		Pose other[kBodies]; std::memcpy(other, raw, sizeof(raw));
		ApplyClearanceTrack(other, track, 0.9);
		ApplyClearanceTrack(again, track, t);
		CHECK(std::memcmp(out, again, sizeof(out)) == 0);
		for (int limb = 0; limb < 4; ++limb) {
			const int root = kClearanceBones[limb * 2], mid = kClearanceBones[limb * 2 + 1], end = kClearanceEnds[limb];
			CHECK(Near(Dist(raw[root], raw[mid]), Dist(out[root], out[mid]), 1e-4));
			CHECK(Near(Dist(raw[mid], raw[end]), Dist(out[mid], out[end]), 1e-4));
			CHECK(RotationDistance(out[mid].q, out[end].q) < 1e-3);
		}
	}
	// The same pipeline leaves an already-clear static clip alone and clamps time at either end.
	std::vector<float> clear(kBodies * kFloatsPerBody, 0.f);
	for (int b = 0; b < kBodies; ++b) clear[b * kFloatsPerBody + 6] = 1;
	const auto staticTrack = BuildClearanceTrack(base, clear.data(), 1, 30);
	CHECK(staticTrack.size() == 1);
	for (double t : {-10.0, 0.0, 10.0}) {
		Pose out[kBodies], raw[kBodies];
		for (int b = 0; b < kBodies; ++b) { Sample(base, clear.data(), 1, 30, 0, b, raw[b]); out[b] = raw[b]; }
		ApplyClearanceTrack(out, staticTrack, t);
		for (int b = 0; b < kBodies; ++b) CHECK(Dist(out[b], raw[b]) < 1e-5 && RotationDistance(out[b].q, raw[b].q) < 1e-3);
	}
	// A vertical limb reversing direction needs a perpendicular half-turn axis.
	const double down[3] = {0, 0, -1}, up[3] = {0, 0, 1};
	float q[4], moved[3]; const float v[3] = {0, 0, -1};
	detail::Swing(down, up, q); QuatRotate(q, v, moved);
	CHECK(Near(moved[0], 0) && Near(moved[1], 0) && Near(moved[2], 1));
}

static void TestPlaybackFeel() {
	// Blend out: full weight until the last blendOut seconds, zero at the end, smooth in between.
	CHECK(Near(BlendOutWeight(0.5, 2.0, 0.4), 1.0));
	CHECK(Near(BlendOutWeight(2.0, 2.0, 0.4), 0.0));
	CHECK(Near(BlendOutWeight(1.8, 2.0, 0.4), 0.5));
	CHECK(Near(BlendOutWeight(1.99, 2.0, 0.0), 1.0) && Near(BlendOutWeight(2.0, 2.0, 0.0), 0.0));

	PlaybackTuning tune;
	ContactState contact;
	CHECK(Near(BodyFollow(kPelvis, 0.5, 2.0, tune, contact), 1.0));
	tune.strength = 0.3;
	CHECK(Near(BodyFollow(kHandLeft, 0.5, 2.0, tune, contact), 0.3));

	// Contact: nothing counts during the settle frames or for a single noisy frame.
	ContactRules rules;
	double push[kBodies] = {};
	for (int f = 0; f < rules.settle; ++f) { push[kPelvis] = 50; UpdateContact(contact, push, rules); }
	CHECK(!contact.stop);
	push[kPelvis] = 50; UpdateContact(contact, push, rules);
	push[kPelvis] = 0;  UpdateContact(contact, push, rules);
	CHECK(!contact.stop);

	// A limb pushed a little gives (bends); the rest of the body is untouched.
	push[kHandLeft] = 3.75; // halfway between soft (1.5) and hard (6)
	UpdateContact(contact, push, rules); UpdateContact(contact, push, rules);
	CHECK(contact.limbGive[0] > 0.45 && contact.limbGive[0] < 0.55 && !contact.limp[0]);
	tune.strength = 1.0;
	CHECK(Near(BodyFollow(kHandLeft, 0.5, 2.0, tune, contact), 1.0 - contact.limbGive[0]));
	CHECK(Near(BodyFollow(kHandRight, 0.5, 2.0, tune, contact), 1.0));
	// Pushed hard: limp for good, even after the push goes away.
	push[kHandLeft] = 9; UpdateContact(contact, push, rules); UpdateContact(contact, push, rules);
	push[kHandLeft] = 0; UpdateContact(contact, push, rules);
	CHECK(contact.limp[0] && Near(BodyFollow(kForearmLeft, 0.5, 2.0, tune, contact), 0.0));
	CHECK(!contact.stop);
	// Walls off: the limb follows the clip regardless.
	tune.reactToWalls = false;
	CHECK(Near(BodyFollow(kHandLeft, 0.5, 2.0, tune, contact), 1.0));

	// The torso pushed for two frames stops the clip.
	push[kSpine2] = 6; UpdateContact(contact, push, rules); UpdateContact(contact, push, rules);
	CHECK(contact.stop && contact.stopBody == kSpine2);
}

static void TestSolidDrive() {
	// On the pose, moving with the clip: the feedforward alone (the clip's velocity).
	Pose now, ahead;
	ahead.p[0] = 2.f;
	const double step = 1.0 / 64;
	float lin[3], ang[3];
	CHECK(TrackVelocity(now, now, ahead, step, 6000, 15000, lin, ang) && Near(lin[0], 128.0) && Near(lin[2], 0.0));
	// Off the pose by a little: kTrackGain of it corrected per tick (no ringing with a one-tick delay).
	Pose actual;
	actual.p[2] = -0.5f;
	CHECK(TrackVelocity(actual, now, now, step, 6000, 15000, lin, ang) && Near(lin[2], kTrackGain * 0.5 / step));
	// Far off: the correction is what the muscles can stop from -- sqrt(2 a e), not e / step.
	actual.p[2] = -200.f;
	CHECK(TrackVelocity(actual, now, now, step, 6000, 15000, lin, ang) && Near(lin[2], std::sqrt(2.0 * 6000 * 200), 0.5));
	CHECK(lin[2] < 200.0 * kTrackGain / step);
	// With the pose read one tick late, the correction settles without ringing (no overshoot).
	double xs[3] = {-10, -10, -10};
	double over = 0;
	for (int k = 0; k < 64; ++k) {
		Pose late; late.p[0] = float(xs[1]); // the reading lags one tick
		TrackVelocity(late, now, now, step, 1e9, 1e9, lin, ang);
		xs[0] = xs[1]; xs[1] = xs[2]; xs[2] = xs[1] + lin[0] * step;
		if (xs[2] > over) over = xs[2];
	}
	CHECK(std::fabs(xs[2]) < 0.1 && over < 0.05);
	// No muscles, no correction.
	CHECK(TrackVelocity(actual, now, now, step, 0, 0, lin, ang) && Near(lin[2], 0.0));
	// Rotation is braked the same way, in degrees.
	Pose turned = now;
	const double half = 60.0 * 0.5 * 3.14159265358979 / 180.0;
	turned.q[2] = float(std::sin(half)); turned.q[3] = float(std::cos(half));
	CHECK(TrackVelocity(now, turned, turned, step, 6000, 1500, lin, ang) && Near(ang[2], std::sqrt(2.0 * 1500 * 60), 1.0));
	// Simulated: an accel-limited body chasing a still pose with braking settles without overshooting.
	double x = -30, v = 0;
	const double a = 1500, dt = 1.0 / 240;
	double worstOver = 0;
	for (int f = 0; f < 480; ++f) {
		Pose ap; ap.p[0] = float(x);
		TrackVelocity(ap, now, now, step, a, a, lin, ang);
		const double dv = lin[0] - v, lim = a * dt;
		v += dv > lim ? lim : (dv < -lim ? -lim : dv);
		x += v * dt;
		if (x > worstOver) worstOver = x;
	}
	CHECK(std::fabs(x) < 0.5 && worstOver < 1.0);
}

static void TestEndOfMotion() {
	// A clip that moves for its first second and then lies still until 2.5 s ends at 1 s.
	auto sample = [](double t, Pose out[kBodies]) {
		for (int b = 0; b < kBodies; ++b) {
			out[b] = Pose();
			out[b].p[0] = float((t < 1.0 ? t : 1.0) * 100.0 + b);
		}
	};
	CHECK(Near(MotionEndTime(2.5, sample), 1.0, 1.0 / 60 + 1e-6));
	// Still from the start: never shorter than the minimum.
	auto still = [](double, Pose out[kBodies]) { for (int b = 0; b < kBodies; ++b) out[b] = Pose(); };
	CHECK(Near(MotionEndTime(2.0, still), 0.4));
	// Moving to the last frame: the whole clip.
	auto moving = [](double t, Pose out[kBodies]) { for (int b = 0; b < kBodies; ++b) { out[b] = Pose(); out[b].p[2] = float(t * 60); } };
	CHECK(Near(MotionEndTime(1.5, moving), 1.5));

	// Landing: a clip that falls from 36 to 10 units by t = 1 and lies there lands at ~1 s (within 3
	// units of the end height); one that ends kneeling (pelvis 24) never "lands".
	auto fall = [](double t, Pose out[kBodies]) {
		const double h = t < 1.0 ? 36.0 - 26.0 * t : 10.0;
		for (int b = 0; b < kBodies; ++b) { out[b] = Pose(); out[b].p[2] = float(h); }
	};
	const double land = LandTime(2.0, fall);
	CHECK(land > 0.85 && land < 1.0);
	auto kneel = [](double t, Pose out[kBodies]) { for (int b = 0; b < kBodies; ++b) { out[b] = Pose(); out[b].p[2] = float(36.0 - 12.0 * (t < 1 ? t : 1)); } };
	CHECK(LandTime(2.0, kneel) < 0);

	// Solid contact: the torso pushed (by the floor or a wall) for two frames stops the clip, and so
	// does a limb hit hard -- no partial give.
	ContactState cs;
	const ContactRules sr = SolidContactRules();
	double push[kBodies] = {};
	for (int f = 0; f < sr.settle; ++f) UpdateContact(cs, push, sr);
	push[13] = 12.0; // left foot
	UpdateContact(cs, push, sr);
	CHECK(!cs.stop);
	UpdateContact(cs, push, sr);
	CHECK(!cs.stop && cs.limp[2]); // the leg goes limp, the clip plays on
	push[5] = 8.0; // the torso hits something
	UpdateContact(cs, push, sr);
	UpdateContact(cs, push, sr);
	CHECK(cs.stop && cs.stopBody == 5);
	ContactState quiet;
	double small[kBodies] = {};
	for (int b = 0; b < kBodies; ++b) small[b] = 4.0; // open-space noise
	for (int f = 0; f < 20; ++f) UpdateContact(quiet, small, sr);
	CHECK(!quiet.stop && quiet.limbGive[0] == 0.0);

	// Muscles: strength sets how fast the clip may change physics' velocity, falling with its square.
	CHECK(Near(MuscleLimit(1.0, kMuscleAccel), 6000) && Near(MuscleLimit(0.3, kMuscleAccel), 540) && Near(MuscleLimit(0.1, kMuscleAccel), 60));
	CHECK(MuscleLimit(0.1, kMuscleAccel) < 386.0); // at 10 % the body cannot even hold itself up
	const float phys[3] = {0, 0, -200}, physAng[3] = {0, 0, 0}, drive[3] = {100, 0, 0}, driveAng[3] = {0, 0, 90};
	float lin[3], ang[3];
	// Strong enough: the drive velocity exactly.
	MuscleVelocity(phys, physAng, drive, driveAng, 1e6, 1e6, 1.0 / 64, lin, ang);
	CHECK(Near(lin[0], 100) && Near(lin[2], 0) && Near(ang[2], 90));
	// Weak: physics' motion changes by at most accel * dt toward it (here 60 u/s^2 over 1/60 s = 1 u/s).
	MuscleVelocity(phys, physAng, drive, driveAng, 60, 60, 1.0 / 60, lin, ang);
	const double moved = std::sqrt((lin[0] - phys[0]) * (lin[0] - phys[0]) + (lin[2] - phys[2]) * (lin[2] - phys[2]));
	CHECK(Near(moved, 1.0) && lin[2] < -199.0 && Near(ang[2], 1.0));
	// None: physics untouched.
	MuscleVelocity(phys, physAng, drive, driveAng, 0, 0, 1.0 / 60, lin, ang);
	CHECK(Near(lin[0], 0) && Near(lin[2], -200));

	// Balance: the torso line's angle from the clip's.
	const float p0[3] = {0, 0, 0}, up[3] = {0, 0, 30}, side[3] = {30, 0, 0}, lean[3] = {10, 0, 30};
	CHECK(Near(TorsoTilt(p0, up, p0, up), 0.0) && Near(TorsoTilt(p0, side, p0, up), 90.0, 1e-3));
	CHECK(TorsoTilt(p0, lean, p0, up) < kBalanceLossDegrees && TorsoTilt(p0, side, p0, up) > kBalanceLossDegrees);

	// Ground track: a clip whose knee dips 5 units into the floor is lifted out of it; one that lies
	// 3 units above the floor is lowered onto it; the track starts at 0 and is smooth.
	auto dip = [](double t, Pose out[kBodies]) {
		for (int b = 0; b < kBodies; ++b) { out[b] = Pose(); out[b].p[2] = 30.f; }
		out[kPelvis].p[2] = 36.f;
		out[1].p[2] = float(kGroundRadius[1] - (t > 0.5 && t < 1.0 ? 5.0 : 0.0)); // left calf
	};
	const auto lift = BuildGroundTrack(1.5, dip);
	CHECK(GroundOffsetAt(lift, 0.75) > 4.0 - 1e-3 && GroundOffsetAt(lift, 0.75) < 5.0 + 1e-3);
	CHECK(Near(GroundOffsetAt(lift, 0.0), 0.0) && GroundOffsetAt(lift, 1.45) < 0.5);
	auto floating = [](double, Pose out[kBodies]) {
		for (int b = 0; b < kBodies; ++b) { out[b] = Pose(); out[b].p[2] = float(kGroundRadius[b] + 3.0 + b); }
		out[kPelvis].p[2] = float(kGroundRadius[kPelvis] + 3.0 + 10.0 < kDownPelvis ? kGroundRadius[kPelvis] + 3.0 + 10.0 : 10.0);
	};
	const auto settle = BuildGroundTrack(2.0, floating);
	CHECK(Near(GroundOffsetAt(settle, 1.5), -3.0, 0.05));
	double worstStep = 0;
	for (size_t i = 1; i < lift.size(); ++i) worstStep = std::max(worstStep, std::fabs(double(lift[i]) - lift[i - 1]));
	CHECK(worstStep < 1.5); // no pops frame to frame

	// Fade from a moment: untouched before it, half at the middle, gone at the end; no fade without a start.
	CHECK(Near(FadeOutWeight(0.9, 1.0, 0.5), 1.0) && Near(FadeOutWeight(1.25, 1.0, 0.5), 0.5) && Near(FadeOutWeight(1.5, 1.0, 0.5), 0.0));
	CHECK(Near(FadeOutWeight(5.0, -1.0, 0.5), 1.0) && Near(FadeOutWeight(1.1, 1.0, 0.0), 0.0));

	// Map fit: a body resting on the clip floor next to a 6-unit step is 6 in; one 10 above it is clear.
	CHECK(Near(FloorPenetration(kGroundRadius[kPelvis], kGroundRadius[kPelvis], 6.0), 6.0));
	CHECK(Near(FloorPenetration(kGroundRadius[kPelvis] + 10.0, kGroundRadius[kPelvis], 6.0), 0.0));
	CHECK(Near(FloorPenetration(kGroundRadius[kPelvis] - 2.0, kGroundRadius[kPelvis], 0.0), 0.0)); // the ground track lifts it

	// No rebound: a clip whose hips dip from 42 to 20 and climb back to 44 before falling to 10 (the CoD
	// explosion shape) never shows the hips more than kReboundAllowance above their lowest point.
	auto hurl = [](double t, Pose out[kBodies]) {
		double z = t < 0.2 ? 42 - 110 * t : (t < 0.5 ? 20 + 80 * (t - 0.2) : (t < 0.8 ? 44 - 113 * (t - 0.5) : 10.0));
		for (int b = 0; b < kBodies; ++b) { out[b] = Pose(); out[b].p[2] = float(z - (b == kPelvis ? 0 : 5)); }
		if (z < 20) for (int b = 0; b < kBodies; ++b) out[b].p[2] = float(std::max(out[b].p[2], kGroundRadius[b] + 1.0f));
	};
	const auto capped = BuildGroundTrack(1.5, hurl);
	double lowest = 1e9, worstRise = 0;
	for (int i = 0; i < 90; ++i) {
		const double t = i / 60.0;
		Pose p[kBodies]; hurl(t, p);
		const double z = p[kPelvis].p[2] + GroundOffsetAt(capped, t);
		lowest = std::min(lowest, z);
		worstRise = std::max(worstRise, z - lowest);
	}
	CHECK(worstRise < kReboundAllowance + 2.5); // smoothing lag allowed, the 24-unit hurl is gone

	// Grenade arc: out and up, back on the ground at BlastLandTime, never below it.
	const double gdir[2] = {1, 0};
	double off[3];
	const double arcLand = BlastLandTime(kBlastUp);
	BlastOffset(gdir, kBlastOut, kBlastUp, arcLand / 2, off);
	CHECK(off[0] > 0 && off[2] > 20.0 && off[2] < 40.0);
	CHECK(Near(BlastScale(1.0), 1.0) && Near(BlastScale(4.0), 2.0) && BlastScale(2.0) < 1.5 && BlastScale(0) == 0);
	// Flight drag: sideways speed bleeds off, vertical is gravity's; 330 u/s is nearly gone in 1.5 s.
	float fly[3] = {330.f, 0.f, 120.f};
	for (int f = 0; f < 90; ++f) FlightDrag(fly, 1.0 / 60);
	CHECK(fly[0] < 10.f && fly[0] > 0.f && Near(fly[2], 120.0));
	// On the ground it stops within a fraction of a second.
	float slide[3] = {200.f, 0.f, 0.f};
	for (int f = 0; f < 15; ++f) FlightDrag(slide, 1.0 / 60, FlightDragRate(8.0));
	CHECK(slide[0] < 10.f && FlightDragRate(40.0) == kFlightDrag);
	BlastOffset(gdir, kBlastOut, kBlastUp, arcLand * 1.2, off);
	CHECK(off[2] == 0.0);
	// It leaves smoothly (no jump at t = 0) and drag slows it: the second half of the flight covers
	// less ground than the first, and it never goes past out / drag.
	double a0[3], a1[3], a2[3];
	BlastOffset(gdir, kBlastOut, kBlastUp, 0.0, a0);
	BlastOffset(gdir, kBlastOut, kBlastUp, arcLand / 2, a1);
	BlastOffset(gdir, kBlastOut, kBlastUp, arcLand, a2);
	CHECK(Near(a0[0], 0) && Near(a0[2], 0));
	CHECK(a2[0] - a1[0] < a1[0] - a0[0] && a2[0] < kBlastOut / kBlastDrag);

	// Only a grenade blast throws the body; bullets never push.
	DeathFacts rifle, blast;
	blast.explosive = true;
	CHECK(ImpactSpeed(rifle) == 0 && ImpactLift(rifle) == 0);
	CHECK(ImpactSpeed(blast) > 0 && ImpactLift(blast) > 0);
	CHECK(HitBody(1) == kHeadBody && HitBody(6) == 1 && HitBody(0) == -1);
	// A shot arm or leg goes limp; head and torso hits leave every limb on the clip.
	CHECK(ShotLimb(4) == 0 && ShotLimb(7) == 3 && ShotLimb(1) == -1 && ShotLimb(2) == -1);
	CHECK(LimbOf(HitBody(6)) == ShotLimb(6) && LimbOf(HitBody(5)) == ShotLimb(5));
}

static void TestCarriedOffset() {
	// Starts at the death pose moving at the death velocity, coasts to a stop, never over the speed cap.
	const float v[3] = {200.f, 0.f, -100.f};
	double a[3], b[3], far[3];
	CarriedOffset(v, 0.0, a);
	CHECK(std::fabs(a[0]) < 1e-9 && std::fabs(a[2]) < 1e-9);
	CarriedOffset(v, 0.001, b);
	CHECK(std::fabs(b[0] / 0.001 - 200.0) < 3.0);
	CarriedOffset(v, 5.0, far);
	CHECK(std::fabs(far[0] - 200.0 * kCarryTime) < 1e-3);
	const float fast[3] = {4000.f, 0.f, 0.f};
	CarriedOffset(fast, 0.001, b);
	CHECK(b[0] / 0.001 <= kCarryMaxSpeed + 1.0);
}

static void TestJitterCounter() {
	// A smooth sweep and a 64 Hz staircase never reverse; a back-and-forth wobble reverses every sample.
	ReversalCounter smooth, stairs, wobble;
	for (int i = 0; i < 64; ++i) {
		const float a[3] = {float(i * 2.0), 0, 0};
		smooth.Add(a, 1.0 / 64);
		const float b[3] = {float((i / 4) * 8.0), 0, 0};
		stairs.Add(b, 1.0 / 64);
		const float c[3] = {float((i & 1) ? 3.0 : -3.0), 0, 0};
		wobble.Add(c, 1.0 / 64);
	}
	CHECK(smooth.reversals == 0 && stairs.reversals == 0 && wobble.reversals > 50);
	// Slow creep below the speed floor does not count.
	ReversalCounter creep;
	for (int i = 0; i < 64; ++i) { const float c[3] = {float((i & 1) ? 0.1 : -0.1), 0, 0}; creep.Add(c, 1.0 / 64); }
	CHECK(creep.reversals == 0);
}

static void TestSourceField() {
	// The v9 clip table stores the source as an explicit u32; the enum order is the file format.
	CHECK((int)Source::Mw2019 == 0 && (int)Source::Classic == 1 && (int)Source::Extra == 2);
	CHECK((int)Source::Count == 3);
	ClipTags t; t.source = Source::Classic;
	CHECK(t.source == Source::Classic);
}

int main() {
	TestSourceField();
	TestCategories();
	TestSides();
	TestVictimStance();
	TestSelectPool();
	TestWeaponPriority();
	TestPlacementIsRigid();
	TestPlacementAnchors();
	TestPlacementIgnoresLean();
	TestSample();
	TestNoBlendIsExact();
	TestReleaseVelocity();
	TestFollowDriven();
	TestLimbsOutOfBody();
	TestLimbsNoPop();
	TestPlaybackFeel();
	TestSolidDrive();
	TestEndOfMotion();
	TestJitterCounter();
	TestCarriedOffset();
	TestClearanceTrack();
	if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
	std::printf("DeathAnimationsMathTests passed\n");
	return 0;
}
