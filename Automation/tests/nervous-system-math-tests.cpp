// Behaviour tests for AfxHookSource2/Filmmaker/Movie/NervousSystemMath.h.
#include "../../AfxHookSource2/Filmmaker/Movie/NervousSystemMath.h"

#include <cmath>
#include <cstdio>

using namespace Filmmaker::NervousSystemMath;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)
static bool Near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }

// A corpse lying on its back along +X (head at +X), right side at +Y, floor at z = 0: the chest faces +Z.
static void LyingOnBack(float p[kBodies * 3]) {
	auto set = [&](int b, float x, float y, float z) { p[b * 3] = x; p[b * 3 + 1] = y; p[b * 3 + 2] = z; };
	set(kPelvis, 0, 0, 6); set(kSpine2, 14, 0, 7); set(kHeadBody, 28, 0, 6);
	set(kUpperArmLeft, 24, -8, 6); set(kForearmLeft, 13, -10, 4); set(kHandLeft, 2, -11, 3);
	set(kUpperArmRight, 24, 8, 6); set(kForearmRight, 13, 10, 4); set(kHandRight, 2, 11, 3);
	set(kThighLeft, -2, -4, 6); set(kCalfLeft, -19, -4, 5); set(kAnkleLeft, -35, -4, 4);
	set(kThighRight, -2, 4, 6); set(kCalfRight, -19, 4, 5); set(kAnkleRight, -35, 4, 4);
}

static void TestInjuries() {
	CHECK(InjuryFor(2, true, false, false, false) == Injury::Head);   // the headshot flag wins
	CHECK(InjuryFor(1, false, false, false, false) == Injury::Head);
	CHECK(InjuryFor(6, false, false, false, false) == Injury::LeftLeg);
	CHECK(InjuryFor(7, false, false, false, false) == Injury::RightLeg);
	CHECK(InjuryFor(4, false, false, false, false) == Injury::LeftArm);
	CHECK(InjuryFor(3, false, false, false, false) == Injury::Stomach);
	CHECK(InjuryFor(8, false, false, false, false) == Injury::Neck);
	CHECK(InjuryFor(0, false, true, false, false) == Injury::Explosion);
	CHECK(InjuryFor(0, false, false, true, false) == Injury::Fire);
	CHECK(InjuryFor(2, false, false, false, true) == Injury::Melee);
	CHECK(InjuryFor(0, false, false, false, false) == Injury::Chest);
}

static void TestProfiles() {
	// A headshot does nothing: no reach, no life.
	const Profile head = ProfileFor(Injury::Head, 7);
	CHECK(!head.reacts && head.reach == 0 && LifeSeconds(8, head, 7) == 0);
	for (uint32_t seed = 0; seed < 64; ++seed) {
		// A leg hit: the same-side hand always goes for the leg, the wound is on that leg, it is drawn up.
		const Profile ll = ProfileFor(Injury::LeftLeg, seed);
		CHECK(ll.reacts && (ll.reach == (kHandBitLeft | kHandBitRight)) && ll.woundBody == kThighLeft && (ll.curl & kLegBitLeft));
		const Profile rl = ProfileFor(Injury::RightLeg, seed);
		CHECK((rl.reach == (kHandBitLeft | kHandBitRight)) && rl.woundBody == kThighRight && (rl.curl & kLegBitRight));
		// An arm hit: the shot arm hangs, the OTHER hand clutches it.
		const Profile la = ProfileFor(Injury::LeftArm, seed);
		CHECK(la.reach == kHandBitRight && la.woundBody == kUpperArmLeft && (la.limp & 1));
		// Chest hits always use both hands, independent of the death seed.
		CHECK(ProfileFor(Injury::Chest, seed).reach == (kHandBitLeft | kHandBitRight));
		// Life varies per death but stays within +-25 %.
		const double life = LifeSeconds(8, ll, seed);
		CHECK(life >= 8 * ll.life * 0.75 - 1e-9 && life <= 8 * ll.life * 1.25 + 1e-9);
		CHECK(life == LifeSeconds(8, ll, seed)); // deterministic: a seek replays the same death
	}
	// A blast has no wound to hold; the legs pedal.
	const Profile blast = ProfileFor(Injury::Explosion, 3);
	CHECK(blast.reach == 0 && blast.pedal == (kLegBitLeft | kLegBitRight));
}

static void TestConsciousness() {
	const double life = 8;
	CHECK(Consciousness(0, life) == 0);
	CHECK(Consciousness(kOnset, life) == 0);                  // nothing before the reaction starts
	CHECK(Near(Consciousness(kOnset + kRampIn, life), 1.0));   // full after the ramp
	CHECK(Near(Consciousness(life * kFullShare, life), 1.0));
	CHECK(Consciousness(life, life) == 0);
	CHECK(Consciousness(life + 1, life) == 0);
	double prev = 2;
	for (double t = life * kFullShare; t < life; t += 0.1) { // fades monotonically to nothing
		const double c = Consciousness(t, life);
		CHECK(c <= prev + 1e-12);
		prev = c;
	}
	CHECK(Consciousness(3, 0) == 0);
	// A body still moving fast has little say; a resting one full.
	CHECK(SpeedGate(0) == 1.0 && SpeedGate(130) == 1.0 && Near(SpeedGate(195), 0.5) && SpeedGate(900) == 0.2);
	CHECK(NerveAccel(1, 0, 1) == 0 && NerveAccel(1, 1, 1) == kNerveAccel);
}

static void TestFrame() {
	float p[kBodies * 3];
	LyingOnBack(p);
	const Frame f = BodyFrame(p);
	CHECK(f.valid);
	CHECK(Near(f.up.x, 1, 0.05));     // toward the head
	CHECK(Near(f.right.y, 1, 0.05));  // the right side
	CHECK(Near(f.front.z, 1, 0.05));  // on its back the chest faces the sky
	// Smoothing keeps an orthonormal frame and moves toward the new one.
	Frame g = f;
	g.up = Make(0, 0, 1); g.right = Make(0, 1, 0); g.front = Cross(g.up, g.right);
	const Frame s = SmoothFrame(f, g, 0.1, 0.4);
	CHECK(Near(Length(s.up), 1) && Near(Length(s.right), 1) && Near(Dot(s.up, s.right), 0));
	CHECK(s.up.z > 0 && s.up.z < 1);
	const V3 local = Make(1, 2, 3);
	const V3 back = ToLocal(f, ToWorld(f, local));
	CHECK(Near(back.x, 1) && Near(back.y, 2) && Near(back.z, 3));
}

static void TestReachChain() {
    using Filmmaker::DeathAnimationsMath::Pose;
    Pose in[3];in[0].p[0]=1500;in[0].p[1]=600;in[0].p[2]=130;
    in[1]=in[0];in[1].p[0]+=11;in[2]=in[1];in[2].p[0]+=10;
    const V3 root=Make(1500,600,130),goal=Make(1505,615,135);
    V3 elbow,wrist;CHECK(TwoBoneIk(root,11,10,goal,Make(0,0,1),kReach,kFold,elbow,wrist));
    for(int frame=0;frame<1000;++frame) {
        Pose out[3];AimChain(in,elbow,wrist,1,out);
        auto pos=[](const Pose& p){return Make(p.p[0],p.p[1],p.p[2]);};
        CHECK(Near(Length(pos(out[1])-pos(out[0])),11,.01));
        CHECK(Near(Length(pos(out[2])-pos(out[1])),10,.01));
        CHECK(Length(pos(out[2])-wrist)<.02);
        for(int b=0;b<3;++b)in[b]=out[b];
    }
    // A bent elbow must retain its hinge plane, not just place the wrist correctly.
    Pose bent[3];bent[1].p[0]=11;bent[2].p[0]=11;bent[2].p[1]=10;
    V3 e,w;CHECK(TwoBoneIk(Make(0,0,0),11,10,Make(8,0,12),Make(0,1,0),kReach,kFold,e,w));
    Pose posed[3];AimChain(bent,e,w,1,posed);
    float oldNormal[3]={0,0,1},newNormal[3];
    Filmmaker::DeathAnimationsMath::QuatRotate(posed[0].q,oldNormal,newNormal);
    V3 expected=Cross(e,w-e);Normalize(expected);
    CHECK(Dot(expected,Make(newNormal[0],newNormal[1],newNormal[2]))>.999);
    V3 target=Make(0,0,0);
    for(int frame=0;frame<128;++frame) {
        const V3 next=ReachAdvance(target,Make(30,10,5),1.0/64);
        CHECK(Length(next-target)<=80.0/64+1e-6);
        CHECK(Length(next-Make(30,10,5))<=Length(target-Make(30,10,5)));
        target=next;
    }
    CHECK(Length(target-Make(30,10,5))<.01);
}

static void TestIk() {
	const V3 s = Make(0, 0, 0);
	V3 mid, end;
	// Reachable goal: both segments keep their lengths and the end lands on the goal.
	CHECK(TwoBoneIk(s, 11, 11, Make(12, 5, 0), Make(0, 0, 1), kReach, kFold, mid, end));
	CHECK(Near(Length(mid - s), 11) && Near(Length(end - mid), 11));
	CHECK(Near(end.x, 12) && Near(end.y, 5));
	CHECK(mid.z > 0); // bends toward the pole
	// Out of reach: pulled in to 97 % of the arm, never stretched, still on the goal's line.
	CHECK(TwoBoneIk(s, 11, 11, Make(50, 0, 0), Make(0, 0, 1), kReach, kFold, mid, end));
	CHECK(Near(Length(end - s), 22 * kReach) && Near(end.y, 0) && Near(Length(mid - s), 11) && Near(Length(end - mid), 11));
	// Too close (a goal on the shoulder): the limb is not folded shut, it stops at half its length.
	CHECK(TwoBoneIk(s, 11, 11, Make(2, 1, 0), Make(0, 0, 1), kReach, kFold, mid, end));
	CHECK(Near(Length(end - s), 22 * kFold) && Near(Length(mid - s), 11) && Near(Length(end - mid), 11));
	// Pole along the reach line: still a valid bend.
	CHECK(TwoBoneIk(s, 11, 11, Make(15, 0, 0), Make(1, 0, 0), kReach, kFold, mid, end));
	CHECK(Near(Length(mid - s), 11));
	CHECK(!TwoBoneIk(s, 0, 11, Make(10, 0, 0), Make(0, 0, 1), kReach, kFold, mid, end));
}

static void TestWoundGoal() {
	float p[kBodies * 3];
	LyingOnBack(p);
	const Frame f = BodyFrame(p);
	const Profile leg = ProfileFor(Injury::LeftLeg, 0);
	const V3 shoulder = BodyPos(p, kUpperArmLeft);
	V3 wrist;
	CHECK(WoundGoal(p, leg, shoulder, f.front, 0, 1, 0, 0, wrist));
	// On the left thigh, a few units off the bone line, on its upper side (the body lies on its back).
	const V3 thigh = BodyPos(p, kThighLeft), knee = BodyPos(p, kCalfLeft);
	CHECK(wrist.x < thigh.x && wrist.x > knee.x);
	V3 axis = knee - thigh; Normalize(axis);
	V3 rel = wrist - thigh;
	const double off = Length(rel - axis * Dot(rel, axis));
	CHECK(off > leg.woundRadius * 0.5 && off < leg.woundRadius + kPalmToWrist + 1);
	CHECK(wrist.z > thigh.z);
	// Face down (no front): the flank toward the reaching shoulder instead.
	V3 flank;
	CHECK(WoundGoal(p, leg, shoulder, V3(), 0, 1, 0, 0, flank));
	CHECK(flank.y < thigh.y); // the left side is -Y
	// A chest wound sits on the chest, well away from the shoulder the hand reaches from.
	const Profile chest = ProfileFor(Injury::Chest, 0);
	V3 c;
	CHECK(WoundGoal(p, chest, BodyPos(p, kUpperArmRight), f.front, 0, 1, 0, 0, c));
	CHECK(Length(c - BodyPos(p, kUpperArmRight)) > 8.0);
	CHECK(c.z > BodyPos(p, kSpine2).z);
	// Two hands sit apart.
	V3 a, b;
	const Profile gut = ProfileFor(Injury::Stomach, 0);
	CHECK(WoundGoal(p, gut, BodyPos(p, kUpperArmLeft), f.front, 0, 2, 0, 0, a));
	CHECK(WoundGoal(p, gut, BodyPos(p, kUpperArmRight), f.front, 1, 2, 0, 0, b));
	CHECK(Length(a - b) > 2.0);
	// No wound, no goal.
	CHECK(!WoundGoal(p, ProfileFor(Injury::Explosion, 0), shoulder, f.front, 0, 1, 0, 0, wrist));
}

static void TestReachGoal() {
	float p[kBodies * 3];
	LyingOnBack(p);
	const Frame f = BodyFrame(p);
	const V3 shoulder = BodyPos(p, kUpperArmLeft);
	const double armLength = Length(BodyPos(p, kForearmLeft) - shoulder)
		+ Length(BodyPos(p, kHandLeft) - BodyPos(p, kForearmLeft));
	V3 wound, goal;
	double fallback = -1;
	const Profile leg = ProfileFor(Injury::LeftLeg, 0);
	CHECK(WoundGoal(p, leg, shoulder, f.front, 0, 1, 0, 0, wound));
	CHECK(Length(wound - shoulder) > armLength);
	CHECK(ReachGoal(p, leg, shoulder, f.front, 0, 1, 0, 0, armLength, goal, fallback));
	CHECK(fallback > 0.99);
	CHECK(Length(goal - shoulder) < Length(wound-shoulder));
	CHECK(goal.x < BodyPos(p,kThighLeft).x); // still on the injured leg, never the torso
	// A thigh brought close enough remains the actual target rather than becoming a torso reach.
	const V3 nearThigh = shoulder + Make(4, 0, -2);
	const V3 originalThigh = BodyPos(p, kThighLeft);
	for (int b : { kThighLeft, kCalfLeft }) {
		const V3 old = BodyPos(p, b), moved = nearThigh + (old - originalThigh);
		p[b * 3] = float(moved.x); p[b * 3 + 1] = float(moved.y); p[b * 3 + 2] = float(moved.z);
	}
	CHECK(ReachGoal(p, leg, shoulder, f.front, 0, 1, 0, 0, armLength, goal, fallback));
	CHECK(fallback == 0.0);
	const Profile chest = ProfileFor(Injury::Chest, 0);
	CHECK(ReachGoal(p, chest, shoulder, f.front, 0, 1, 0, 0, armLength, goal, fallback));
	CHECK(fallback == 0.0);
}

static void TestDirectionTurn() {
	const V3 from=Make(1,0,0);
	for(V3 target:{Make(0,1,0),Make(-1,0,0),Make(1,0,0),Make(.3,-.4,.5)}) {
		float q[4];CHECK(DirectionTurn(from,target,q));
		const float x[3]={1,0,0};float actual[3];
		Filmmaker::DeathAnimationsMath::QuatRotate(q,x,actual);Normalize(target);
		CHECK(Length(Make(actual[0],actual[1],actual[2])-target)<1e-5);
	}
}

static void TestLegCurl() {
	float p[kBodies * 3];
	LyingOnBack(p);
	const Frame f = BodyFrame(p);
	const V3 hip = BodyPos(p, kThighLeft), knee0 = BodyPos(p, kCalfLeft), ankle0 = BodyPos(p, kAnkleLeft);
	V3 thighDir = knee0 - hip, shinDir = ankle0 - knee0;
	const double thigh = Length(thighDir), shin = Length(shinDir);
	Normalize(thighDir); Normalize(shinDir);
	V3 knee, ankle;
	// w = 0 is the leg where it started.
	CHECK(LegGoal(f, hip, thigh, shin, ToLocal(f, thighDir), ToLocal(f, shinDir), 0, knee, ankle));
	CHECK(Length(knee - knee0) < 0.01 && Length(ankle - ankle0) < 0.01);
	// w = 1 draws it up: the knee rises off the floor (lying on the back) and the foot comes toward the hips.
	CHECK(LegGoal(f, hip, thigh, shin, ToLocal(f, thighDir), ToLocal(f, shinDir), 1, knee, ankle));
	CHECK(knee.z > knee0.z + 10);
	CHECK(Length(ankle - hip) < Length(ankle0 - hip) - 5);
	CHECK(Near(Length(knee - hip), thigh) && Near(Length(ankle - knee), shin));
	// Pedalling legs are out of phase.
	CHECK(Near(PedalCurl(0.2, 0) + PedalCurl(0.2, 1), 1.0));
}

static void TestLayers() {
	// Writhe is bounded by its amplitude, deterministic, zero for the pelvis, and actually moves.
	double lo = 1e9, hi = -1e9;
	for (double t = 0; t < 5; t += 0.01) {
		const V3 w = WritheOffset(42, kHandLeft, t, 1.0);
		const V3 amp = WritheAmplitude(kHandLeft);
		CHECK(std::fabs(w.x) <= amp.x + 1e-9 && std::fabs(w.y) <= amp.y + 1e-9 && std::fabs(w.z) <= amp.z + 1e-9);
		lo = w.x < lo ? w.x : lo; hi = w.x > hi ? w.x : hi;
		const V3 w2 = WritheOffset(42, kHandLeft, t, 1.0);
		CHECK(w.x == w2.x && w.y == w2.y && w.z == w2.z);
		const V3 pel = WritheOffset(42, kPelvis, t, 1.0);
		CHECK(pel.x == 0 && pel.y == 0 && pel.z == 0);
	}
	CHECK(hi - lo > 3.0);
	CHECK(WritheOffset(1, kHandLeft, 0.37, 1.0).x != WritheOffset(2, kHandLeft, 0.37, 1.0).x); // per death
	// Breathing: the lift stays in [0, 1], and the body breathes faster when more conscious.
	for (double ph = 0; ph < 7; ph += 0.05) { const double l = BreathLift(ph); CHECK(l >= 0 && l <= 1.0 + 1e-12); }
	CHECK(BreathRate(1) > BreathRate(0.2));
}

static void TestTwitches() {
	// More twitches as the body fades; the schedule is deterministic.
	CHECK(TwitchRate(0.1, 1) > TwitchRate(1.0, 1));
	CHECK(TwitchGap(9, 3, 1.0) == TwitchGap(9, 3, 1.0));
	CHECK(TwitchGap(9, 3, 0) > 1e8);
	double sum = 0;
	for (int n = 0; n < 2000; ++n) { const double g = TwitchGap(9, n, 2.0); CHECK(g > 0); sum += g; }
	CHECK(sum / 2000 > 0.35 && sum / 2000 < 0.65); // mean gap ~ 1 / rate
	// Busy limbs are skipped; all busy -> none.
	for (int n = 0; n < 50; ++n) { const int l = TwitchLimb(5, n, 0x3); CHECK(l >= 2 && l <= 4); }
	CHECK(TwitchLimb(5, 0, 0x1f) == -1);
	const V3 k = TwitchKick(5, 1, kTwitchSize);
	CHECK(k.x > 0 && Length(k) <= kTwitchSize + 1e-9);
	// A twitch is an out-and-back pulse.
	CHECK(Pulse(0, kTwitchTime) == 0 && Pulse(kTwitchTime, kTwitchTime) == 0 && Near(Pulse(kTwitchTime / 2, kTwitchTime), 1.0));
	CHECK(Pulse(-0.1, kTwitchTime) == 0 && Pulse(1.0, kTwitchTime) == 0);
}

// The muscle drive on a limb the solver fights: every physics tick the joints and floor friction take most
// of the velocity away (measured in game: a hand's read-back was ~0 while 45 u/s was commanded). Ramping
// from the previous COMMAND still gets it to the goal, without ringing and under the reach speed.
static void TestDrive() {
	const double step = 1.0 / 64.0, g = 386.0;
	V3 pos = Make(0, 0, 0), cmd = Make(0, 0, 0);
	const V3 goal = Make(12, 0, 8), still = Make(0, 0, 0);
	int reversals = 0;
	double prevVx = 0, maxSpeed = 0;
	for (int i = 0; i < 192; ++i) {
		cmd = DriveToward(pos, goal, still, cmd, kNerveAccel, kReachSpeed, step, step);
		maxSpeed = Length(cmd) > maxSpeed ? Length(cmd) : maxSpeed;
		V3 moved = cmd * 0.35;          // the solver lets through a third of it
		moved.z -= g * step;            // and gravity pulls
		pos = pos + moved * step;
		if (i > 4 && ((cmd.x > 1 && prevVx < -1) || (cmd.x < -1 && prevVx > 1))) ++reversals;
		prevVx = cmd.x;
	}
	CHECK(Length(goal - pos) < 1.5);
	CHECK(reversals == 0);
	CHECK(maxSpeed <= kReachSpeed + 1e-6);
	// Unhindered it converges too, without overshoot.
	pos = Make(0, 0, 0); cmd = Make(0, 0, 0);
	double over = 0;
	for (int i = 0; i < 192; ++i) {
		cmd = DriveToward(pos, goal, still, cmd, kNerveAccel, kReachSpeed, step, step);
		pos = pos + cmd * step;
		if (pos.x - goal.x > over) over = pos.x - goal.x;
	}
	CHECK(Length(goal - pos) < 0.2 && over < 0.5);
	// The feedforward carries the anchor's motion: a body falling at 200 u/s keeps falling.
	const V3 falling = Make(0, 0, -200);
	const V3 c = DriveToward(Make(0, 0, 0), Make(0, 0, 0), falling, falling, kNerveAccel, kReachSpeed, step, step);
	CHECK(Near(c.z, -200));
	// The muscles ramp: one tick changes the command by at most accel * dt.
	const V3 r = DriveToward(Make(0, 0, 0), Make(100, 0, 0), still, still, kNerveAccel, kReachSpeed, step, step);
	CHECK(Length(r) <= kNerveAccel * step + 1e-9);
	// Weak muscles let go.
	CHECK(!MusclesHold(NerveAccel(1.0, 0.1, 1.0)) && MusclesHold(NerveAccel(1.0, 0.5, 1.0)));
	// A base follows where physics leaves the body, slowly.
	const V3 base = FollowBase(Make(0, 0, 0), Make(10, 0, 0), 0.1);
	CHECK(base.x > 0.5 && base.x < 1.5);
}

// Rotate v about unit axis k by `deg` degrees (Rodrigues).
static V3 Rotate(V3 v, V3 k, double deg) {
	const double r = deg / kRadToDeg, c = std::cos(r), sn = std::sin(r);
	return v * c + Cross(k, v) * sn + k * (Dot(k, v) * (1 - c));
}

// A limb as a chain: an arm hanging straight down swings its hand onto a goal in front of the chest,
// the segments keep their lengths, and the swing never exceeds its speed cap.
static void TestChain() {
	const double step = 1.0 / 64.0;
	const V3 shoulder = Make(0, 0, 0);
	V3 upper = Make(0, 0, -11), lower = Make(0, 0, -11); // shoulder->elbow, elbow->wrist
	V3 upPrev, loPrev;
	const V3 goal = Make(10, -6, -8);
	V3 elbowGoal, wristGoal;
	double fastest = 0;
	for (int i = 0; i < 256; ++i) {
		const V3 elbow = shoulder + upper, wrist = elbow + lower;
		CHECK(TwoBoneIk(shoulder, 11, 11, goal, Make(-1, 0, -1), kReach, kFold, elbowGoal, wristGoal));
		upPrev = SwingVelocity(shoulder, elbow, elbowGoal, upPrev, kNerveAngular, kSwingSpeed, step, step);
		loPrev = SwingVelocity(elbow, wrist, wristGoal, loPrev, kNerveAngular, kSwingSpeed, step, step);
		fastest = Length(upPrev) > fastest ? Length(upPrev) : fastest;
		V3 ku = upPrev, kl = loPrev;
		const double du = Length(ku), dl = Length(kl);
		if (Normalize(ku)) upper = Rotate(upper, ku, du * step);
		if (Normalize(kl)) lower = Rotate(lower, kl, dl * step);
	}
	const V3 wrist = shoulder + upper + lower;
	CHECK(Length(wrist - wristGoal) < 0.5);
	CHECK(Near(Length(upper), 11, 1e-6) && Near(Length(lower), 11, 1e-6));
	CHECK(fastest <= kSwingSpeed + 1e-6);
	// The linear velocity a turn gives a point: 90 deg/s about +Z moves a point 10 units out along +X at
	// ~15.7 u/s along +Y, on top of the base's own motion.
	const V3 v = PointVelocity(Make(0, 0, -5), Make(0, 0, 90), Make(10, 0, 0));
	CHECK(Near(v.y, 10 * 90 / kRadToDeg, 1e-6) && Near(v.z, -5) && Near(v.x, 0));
	// Already on the goal: no swing; the command ramps down from the previous one.
	const V3 still = SwingVelocity(shoulder, Make(0, 0, -11), Make(0, 0, -11), Make(0, 100, 0), kNerveAngular, kSwingSpeed, step, step);
	CHECK(Length(still) < 100 && Length(still) >= 100 - kNerveAngular * step - 1e-6);
}

// A nudge turns the chain about its pivots: the root stays put, the joints stay the same distance apart
// (the ragdoll's joints stay connected), and a 90 deg/s upper-arm turn over 1 s swings the elbow 90 deg.
static void TestNudge() {
	using Filmmaker::DeathAnimationsMath::Pose;
	Pose in[3], out[3];
	auto set = [](Pose& p, float x, float y, float z) { p.p[0] = x; p.p[1] = y; p.p[2] = z; p.q[0] = p.q[1] = p.q[2] = 0; p.q[3] = 1; };
	set(in[0], 0, 0, 0); set(in[1], 0, 0, -11); set(in[2], 0, 0, -22);
	NudgeChain(in, Make(0, 90, 0), V3(), 1.0, out);
	CHECK(Near(out[0].p[0], 0) && Near(out[0].p[2], 0));
	CHECK(Near(out[1].p[0], -11, 1e-3) && Near(out[1].p[2], 0, 1e-3));  // +Y turn takes -Z to -X
	CHECK(Near(out[2].p[0], -22, 1e-3));
	// The bodies turned with it (the quaternion is a 90 deg turn about +Y).
	CHECK(Near(out[1].q[1], std::sin(kPi / 4), 1e-4) && Near(out[1].q[3], std::cos(kPi / 4), 1e-4));
	// Both turns: the segment lengths are unchanged.
	NudgeChain(in, Make(30, 10, 0), Make(0, 0, 45), 0.5, out);
	auto dist = [](const Pose& a, const Pose& b) { return std::sqrt(double(a.p[0] - b.p[0]) * (a.p[0] - b.p[0]) + double(a.p[1] - b.p[1]) * (a.p[1] - b.p[1]) + double(a.p[2] - b.p[2]) * (a.p[2] - b.p[2])); };
	CHECK(Near(dist(out[0], out[1]), 11, 1e-3) && Near(dist(out[1], out[2]), 11, 1e-3));
	// No turn: nothing moves.
	NudgeChain(in, V3(), V3(), 0.1, out);
	CHECK(Near(out[2].p[2], -22) && Near(out[2].q[3], 1));
}

// Both segments following one rigid swing need no extra elbow rotation. Then exercise the actual
// composed chain against an IK target, rather than integrating the two segments independently.
static void TestCoupledChain() {
	using Filmmaker::DeathAnimationsMath::Pose;
	const double dt = 1.0 / 64;
	const V3 r = Make(0,0,0), m = Make(0,0,-11), e = Make(0,0,-22);
	const V3 mg = Make(11,0,0), eg = Make(22,0,0);
	const V3 up = SwingVelocity(r,m,mg,V3(),kNerveAngular,kSwingSpeed,dt,dt);
	CHECK(Length(LowerSwing(m,e,mg,eg,up,V3(),kNerveAngular,kSwingSpeed,dt,dt)) < 1e-6);
	Pose pose[3] = {};
	for (int i=0;i<3;++i) { pose[i].p[2] = float(-11*i); pose[i].q[3] = 1; }
	V3 prevUp, prevLowerWorld;
	const V3 goal = Make(10,-6,-8);
	double lastError = 100, lateMax = 0;
	for (int i=0;i<384;++i) {
		const V3 root=Make(pose[0].p[0],pose[0].p[1],pose[0].p[2]);
		const V3 mid=Make(pose[1].p[0],pose[1].p[1],pose[1].p[2]);
		const V3 end=Make(pose[2].p[0],pose[2].p[1],pose[2].p[2]);
		V3 elbow,wrist;
		CHECK(TwoBoneIk(root,11,11,goal,Make(-1,0,-1),kReach,kFold,elbow,wrist));
		const V3 upper=SwingVelocity(root,mid,elbow,prevUp,kNerveAngular,kSwingSpeed,dt,dt);
		const V3 lower=LowerSwing(mid,end,elbow,wrist,upper,prevLowerWorld,kNerveAngular,kSwingSpeed,dt,dt);
		Pose out[3]; NudgeChain(pose,upper,lower,dt,out);
		for (int j=0;j<3;++j) pose[j]=out[j];
		prevUp=upper; prevLowerWorld=lower+upper;
		lastError=Length(Make(pose[2].p[0],pose[2].p[1],pose[2].p[2])-wrist);
		if(i>256 && lastError>lateMax) lateMax=lastError;
	}
	CHECK(lastError < 0.1 && lateMax < 0.1);
}

// The reaction eases in and out instead of snapping; the muscles let go gradually.
static void TestBlend() {
	const double life = 8;
	CHECK(ReactionBlend(kOnset, life, 0.5, 1.5) == 0);
	CHECK(ReactionBlend(kOnset + 0.25, life, 0.5, 1.5) > 0.2 && ReactionBlend(kOnset + 0.25, life, 0.5, 1.5) < 0.8);
	CHECK(Near(ReactionBlend(4, life, 0.5, 1.5), 1.0));
	CHECK(ReactionBlend(life - 0.75, life, 0.5, 1.5) < 0.8 && ReactionBlend(life - 0.75, life, 0.5, 1.5) > 0.2);
	CHECK(ReactionBlend(life, life, 0.5, 1.5) == 0);
	// No sudden steps: consecutive physics ticks never differ by more than a few percent.
	double prev = 0, jump = 0;
	for (double t = 0; t <= life; t += 1.0 / 64.0) {
		const double b = ReactionBlend(t, life, 0.5, 1.5);
		jump = std::fabs(b - prev) > jump ? std::fabs(b - prev) : jump;
		prev = b;
	}
	CHECK(jump < 0.06);
	CHECK(Near(ReactionBlend(4, life, 0, 0), 1.0)); // 0 = no easing
	// The grip fades between half and one and a half times the hold level.
	CHECK(MuscleWeight(0.4 * kHoldAccel) == 0 && Near(MuscleWeight(1.6 * kHoldAccel), 1.0));
	CHECK(MuscleWeight(kHoldAccel) > 0.3 && MuscleWeight(kHoldAccel) < 0.7);
}

// Face down: nothing pushes into the floor, the breath lifts the back, and no hand goes under the chest.
static void TestFaceDown() {
	Frame f;
	f.valid = true; f.front = Make(0, 0, -1); f.up = Make(1, 0, 0); f.right = Make(0, -1, 0);
	CHECK(FaceDown(f));
	f.front = Make(0, 0, 1);
	CHECK(!FaceDown(f));
	const V3 o = FaceDownOffset(Make(3, 1, 2), 0.5);
	CHECK(o.x <= -1.0 + 1e-9 && o.y == 1 && o.z == 2);     // the push into the floor is gone; the back rises
	CHECK(FaceDownOffset(Make(-2, 0, 0), 0).x == -2);       // moving away from the floor is kept
	CHECK(!FaceDownReach(ProfileFor(Injury::Chest, 0)));
	CHECK(!FaceDownReach(ProfileFor(Injury::Stomach, 0)));
	CHECK(FaceDownReach(ProfileFor(Injury::LeftLeg, 0)));
	CHECK(FaceDownReach(ProfileFor(Injury::RightArm, 0)));
}

// Airborne apices cannot count as a settled body; a grounded prone body can.
static void TestGroundAndRoll() {
    float p[kBodies * 3]; LyingOnBack(p);
    Frame f = BodyFrame(p);
    CHECK(GroundReactionCandidate(f, V3(), V3(), V3()));
    CHECK(!GroundReactionCandidate(f, Make(0,0,-30), V3(), V3()));
    CHECK(!GroundReactionCandidate(f, V3(), Make(0,0,20), V3()));
    f.up = Make(0,0,1);
    CHECK(!GroundReactionCandidate(f, V3(), V3(), V3()));
    f = BodyFrame(p); f.front = f.front * -1;
    CHECK(Length(RollVelocity(f, 1, 0, 1)) == 0);
    CHECK(Length(RollVelocity(f, 1, 0.5, 1)) > 0);
    CHECK(Length(RollVelocity(f, 1, 4, 1)) == 0);
    CHECK(Dot(RollVelocity(f, 1, 0.5, 1), RollVelocity(f, -1, 0.5, 1)) < 0);
    // Both lean directions must immediately raise the chest-facing vector toward the sky.
    for(double side:{-1.0,1.0}) {
        Frame lean;lean.valid=true;lean.up=Make(1,0,0);
        lean.right=Make(0,-0.8,side*0.6);lean.front=Cross(lean.up,lean.right);
        const double direction=RollDirection(lean,1);
        CHECK(direction==-side);
        CHECK(RollDirection(lean,2)==direction); // seed cannot reverse an existing lean
        CHECK(Cross(RollVelocity(lean,direction,0.5,1),lean.front).z>0);
    }
    f.front = Make(0,0,1);
    CHECK(Length(RollVelocity(f, 1, 0.5, 1)) == 0);
}
static void TestReachRoute() {
    float p[kBodies * 3]; LyingOnBack(p);
    Frame f = BodyFrame(p);
    const V3 goal = BodyPos(p,kSpine2) + f.front * 9;
    const V3 under = BodyPos(p,kSpine2) - f.front * 5;
    bool leftClear=false, rightClear=false;
    const V3 left = ReachRoute(p,f,under,goal,0,leftClear), right = ReachRoute(p,f,under,goal,1,rightClear);
    CHECK(Dot(left - goal,f.right) < -10);
    CHECK(Dot(right - goal,f.right) > 10);
    CHECK(Length(ReachRoute(p,f,goal,goal,0,leftClear)-goal) < 1e-6);
    CHECK(leftClear);
    const V3 pressed = BodyPos(p,kSpine2) + f.front * 6.8;
    CHECK(Length(ReachRoute(p,f,pressed,pressed,0,leftClear)-pressed) < 1e-6);
    CHECK(leftClear); // crossing the old threshold while holding the wound must not restart routing
    ReachRoute(p,f,under,goal,0,leftClear);
    CHECK(!leftClear); // a real return behind the torso needs clearance again
    CHECK(ArmClearance(p,Make(7,-12,6),Make(7,12,6)) < 1);
    CHECK(ArmClearance(p,Make(7,-12,17),Make(7,12,17)) > 9);
}

static void TestContactCompliance() {
	const double dt=1.0/64;
	const V3 nudge=Make(0.2,0,0);
	double blocked=1, free=1, lateral=1;
	for(int i=0;i<32;++i) {
		blocked=ContactWeight(blocked,nudge,Make(-0.2,0,0),dt);
		free=ContactWeight(free,nudge,V3(),dt);
		lateral=ContactWeight(lateral,nudge,Make(0,4,0),dt);
	}
	CHECK(blocked < 0.01); // repeated rejected motion relaxes instead of shoving the corpse
	CHECK(Near(free,1) && Near(lateral,1));
	CHECK(Near(ContactWeight(1,Make(0,0,0.05),Make(0,0,-0.05),dt),1)); // ordinary settling is not obstruction
	const double released=ContactWeight(blocked,nudge,V3(),dt);
	CHECK(released > blocked && released-blocked < 0.02); // no abrupt restart against the same contact
	for(int i=0;i<320;++i) blocked=ContactWeight(blocked,nudge,V3(),dt);
	CHECK(blocked > 0.95);
	CHECK(Near(ReactionFriction(30,dt),1)); // the initial slide is preserved
	CHECK(Near(ReactionFriction(0,0),1));
	CHECK(ReactionFriction(2,dt) < 1 && ReactionFriction(2,dt) > 0);
	CHECK(Near(std::pow(ReactionFriction(2,1.0/30),30),std::pow(ReactionFriction(2,1.0/128),128),1e-8));
}

static void TestReachArbitration() {
	const double dt = 1.0 / 64;
	// A hand far from its goal drives; one that arrives rests, blocked or not, and never gives up.
	ReachTrack r;
	CHECK(ArbitrateReach(r, 12, 20, 28, 1.0, 0, dt) == ReachMode::Drive);
	CHECK(ArbitrateReach(r, 4, 20, 28, 1.0, 0, dt) == ReachMode::Rest);
	for (int i = 0; i < 640; ++i) CHECK(ArbitrateReach(r, 3, 20, 28, 0.0, 0, dt) == ReachMode::Rest);
	CHECK(!r.gaveUp);
	// Hysteresis: a wrist springing back to 9-11 units off keeps the hand resting (the flicker that
	// re-placed the arm every few ticks); only past kRestExit does it drive again.
	for (int i = 0; i < 64; ++i) CHECK(ArbitrateReach(r, 9.0 + 2.0 * (i % 2), 20, 28, 1.0, 0, dt) == ReachMode::Rest);
	CHECK(ArbitrateReach(r, kRestExit + 0.5, 20, 28, 1.0, 0, dt) == ReachMode::Drive);
	CHECK(ArbitrateReach(r, 12, 20, 28, 1.0, 0, dt) == ReachMode::Drive); // must arrive again first
	// Blocked away from its goal: pushing on for longer than kGiveUpBlocked gives the hand up, for good.
	ReachTrack b;
	int steps = 0;
	while (ArbitrateReach(b, 16, 20, 28, 0.0, 0, dt) == ReachMode::Drive) ++steps;
	CHECK(b.gaveUp && b.why == GiveUpWhy::Blocked);
	CHECK(steps * dt > kGiveUpBlocked - 2 * dt && steps * dt < kGiveUpBlocked + 2 * dt);
	CHECK(ArbitrateReach(b, 1, 5, 28, 1.0, 0, dt) == ReachMode::GiveUp);
	// A brief bump does not accumulate into a give-up.
	ReachTrack bump;
	for (int i = 0; i < 400; ++i) CHECK(ArbitrateReach(bump, 16, 20, 28, (i % 8) < 2 ? 0.0 : 1.0, 0, dt) == ReachMode::Drive);
	// A goal past arm's length is given up even when nothing blocks the hand, and never rests.
	ReachTrack far;
	steps = 0;
	while (ArbitrateReach(far, 3, 40, 28, 1.0, 0, dt) == ReachMode::Drive) ++steps;
	CHECK(far.gaveUp && far.why == GiveUpWhy::OutOfReach && steps * dt > kGiveUpFar - 2 * dt);
	// Once the body has slid past the budget, a driving hand stops at once; a resting one stays put.
	ReachTrack slid;
	CHECK(ArbitrateReach(slid, 16, 20, 28, 1.0, kSlideBudget + 0.1, dt) == ReachMode::GiveUp && slid.why == GiveUpWhy::Slide);
	ReachTrack parked;
	CHECK(ArbitrateReach(parked, 3, 20, 28, 1.0, kSlideBudget + 5, dt) == ReachMode::Rest);
}

static void TestLayerPicking() {
	const int all = kLayerReach | kLayerBreath | kLayerWrithe | kLayerTwitch | kLayerRoll;
	int seen[3] = {};
	for (uint32_t seed = 1; seed < 400; ++seed) {
		const int l = PickLayers(all, Hash(seed));
		CHECK((l & kLayerReach) && (l & kLayerRoll));
		int ambient = 0;
		for (int k = 0, bit = kLayerBreath; bit <= kLayerTwitch; ++k, bit <<= 1) if (l & bit) { ++ambient; ++seen[k]; }
		CHECK(ambient == kMaxAmbientLayers);
		CHECK(PickLayers(all, Hash(seed)) == l); // the same death plays the same layers
	}
	for (int k = 0; k < 3; ++k) CHECK(seen[k] > 150); // every ambient layer is used
	// Reach brings roll over with it even when the roll setting is off; disabled layers never play.
	CHECK(PickLayers(kLayerReach, 5) == (kLayerReach | kLayerRoll));
	CHECK(PickLayers(kLayerBreath, 5) == kLayerBreath);
	CHECK(PickLayers(kLayerBreath | kLayerTwitch | kLayerRoll, 5) == (kLayerBreath | kLayerTwitch | kLayerRoll));
	CHECK(PickLayers(0, 5) == 0);
}

static void TestPinnedArm() {
	float p[kBodies * 3]; LyingOnBack(p);
	Frame f = BodyFrame(p);
	// Arms on the floor beside a body lying on its back are free, even though they are below the chest.
	CHECK(!ArmPinned(p, f, 0) && !ArmPinned(p, f, 1));
	// The left hand tucked under the lower back (floor level, inside the torso's footprint) is pinned.
	float under[kBodies * 3]; for (int i = 0; i < kBodies * 3; ++i) under[i] = p[i];
	under[kHandLeft * 3] = 6; under[kHandLeft * 3 + 1] = -2; under[kHandLeft * 3 + 2] = 1;
	CHECK(ArmPinned(under, f, 0) && !ArmPinned(under, f, 1));
	// Lying on its side (chest facing +Y): a forearm behind the back within the torso's width is pinned;
	// one out to the side past the torso's width is not.
	float side[kBodies * 3]; for (int i = 0; i < kBodies * 3; ++i) side[i] = p[i];
	auto set = [&](float* q, int b, float x, float y, float z) { q[b * 3] = x; q[b * 3 + 1] = y; q[b * 3 + 2] = z; };
	set(side, kUpperArmLeft, 24, 0, 14); set(side, kUpperArmRight, 24, 0, 1);
	set(side, kThighLeft, -2, 0, 10); set(side, kThighRight, -2, 0, 2);
	const Frame sf = BodyFrame(side);
	CHECK(sf.valid && sf.front.y > 0.9);
	set(side, kForearmLeft, 16, -7, 9); set(side, kHandLeft, 8, -8, 8);
	CHECK(ArmPinned(side, sf, 0));
	set(side, kForearmLeft, 16, 4, 20); set(side, kHandLeft, 8, 6, 26);
	CHECK(!ArmPinned(side, sf, 0));
}

static void TestPlaceCap() {
	CHECK(Near(CapPlaceWeight(1.0, 0.3), 1.0));                    // a small placement is untouched
	CHECK(Near(CapPlaceWeight(1.0, 3.0), kMaxHandPlace / 3.0));   // a 3-unit snap becomes a 0.6-unit step
	CHECK(Near(CapPlaceWeight(0.5, 1.2), 0.5 * kMaxHandPlace / 1.2));
}

static void TestRelax() {
	CHECK(RelaxWeight(-0.1) == 0 && RelaxWeight(kRelaxTime) == 0 && Near(RelaxWeight(0.8), 1));
	CHECK(RelaxWeight(0.1) > 0 && RelaxWeight(0.1) < 1 && RelaxWeight(kRelaxTime - 0.1) < 1);
	// Lying on the back: a relaxed hand ends beside the torso, below the shoulder and toward the floor.
	float p[kBodies * 3]; LyingOnBack(p);
	const Frame f = BodyFrame(p);
	for (int h = 0; h < 2; ++h) {
		const V3 s = BodyPos(p, h ? kUpperArmRight : kUpperArmLeft);
		const V3 g = RelaxArmGoal(f, s, 22, h);
		CHECK(Near(Length(g - s), 22 * 0.95, 1e-6));
		CHECK(g.z < s.z && Dot(g - s, f.up) < -10);
		CHECK(Dot(g - s, f.right) * (h ? 1 : -1) > 4); // out to its own side, not across the chest
	}
}

int main() {
	TestReachArbitration();
	TestLayerPicking();
	TestRelax();
	TestPlaceCap();
	TestPinnedArm();
	TestInjuries();
	TestProfiles();
	TestConsciousness();
	TestFrame();
	TestIk(); TestReachChain();
	TestWoundGoal();
	TestReachGoal();
	TestDirectionTurn();
	TestLegCurl();
	TestLayers();
	TestTwitches();
	TestDrive();
	TestChain();
	TestNudge();
	TestCoupledChain();
	TestBlend();
	TestFaceDown();
	TestGroundAndRoll();
	TestReachRoute();
	TestContactCompliance();
	if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
	std::printf("NervousSystemMathTests passed\n");
	return 0;
}
