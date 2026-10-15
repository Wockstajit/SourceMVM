#include "NervousSystem.h"
#include "NervousSystemMath.h"
#include "RagdollBodies.h"
#include "DeathAnimations.h"

#include "../Debug/MvmDebugLog.h"
#include "../Platform/JsonParser.h"
#include "../../ClientEntitySystem.h"
#include "../../MirvTime.h"
#include "../../../Shared/AfxConsole.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/igameevents.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Filmmaker {
namespace {

using namespace NervousSystemMath;
using namespace RagdollBodies;

constexpr int kMaxNerves = 32;
constexpr int kMaxUsers = 128;
// How long a death waits for its ragdoll bodies once nothing else holds them, and how long it waits
// for a Death Animations clip to let go of them.
constexpr double kAcquireWindow = 0.75;
constexpr double kClipWait = 20.0;
// An offset's velocity is its slope over this interval (seconds).
constexpr double kSlope = 1.0 / 128.0;
// The body frame's smoothing time constant (seconds).
constexpr double kFrameSmoothing = 0.4;
// The last breath starts this long before the body goes still.
constexpr double kLastGaspLead = 1.0;
// Spasms (twitches, the last breath) are involuntary: they get this share of full muscle whatever the
// consciousness, so the agonal ones near the end still move.
constexpr double kSpasmShare = 0.6;
// While the body tumbles faster than this speed gate allows, nothing is steered.
constexpr double kMinGate = 0.5;

// ---------------------------------------------------------------------------------------------
// Settings

bool g_enabled = false;
double g_strength = 1.0;   // 0.1 .. 2
double g_duration = 8.0;   // seconds
bool g_reach = true, g_breath = true, g_writhe = true, g_twitch = true;
double g_blendIn = 0.5;    // seconds the reaction eases in (ReactionBlend)
double g_blendOut = 1.5;   // seconds it eases out before the body goes still
bool g_rollOver = true; // a grounded face-down body tries to turn onto its back
Injury g_force = Injury::None; // testing: every death reacts as this injury
// Diagnostics (`nervous kick <mode>`): throw every reacting body up once and log whether it moves.
// Mode 0 velocity only, 1 motion type re-set first, 2 re-placed on its own pose first.
int g_kickMode = -1;
struct KickWatch { int userid = -1; int frames = 0; float z0[kBodies] = {}; int mode = 0; };
KickWatch g_kickWatch;

// ---------------------------------------------------------------------------------------------
// Bodies

enum class Phase { Free = 0, Waiting, Reacting };

struct Nerve {
	Phase phase = Phase::Free;
	int userid = -1;
	int pawnIndex = -1;
	CEntityInstance* pawn = nullptr;
	BodySet set;
	double deathTime = 0;
	double freeSince = -1;     // when nothing else (a death clip) held the bodies any more
	bool waitedForClip = false;
	double start = 0;
	double life = 0;
	uint32_t seed = 0;
	Injury injury = Injury::None;
	Profile prof;
	Frame frame;
	V3 restThigh[2], restShin[2];
	bool restValid = false;
	DeathAnimationsMath::Pose seen[kBodies];
	bool seenValid = false;
	double lastStep = -1;
	// Steering state per body: the last command (the muscles ramp from it) and, for offset-driven
	// bodies, the base position in the body frame relative to the pelvis.
	V3 prevCmd[kBodies], prevAng[kBodies];
	bool driven[kBodies] = {};
	V3 baseLocal[kBodies];
	bool baseValid[kBodies] = {};
	double breathPhase = 0;
	struct Twitch { int limb = -1; double t0 = 0; V3 local; };
	Twitch twitch[4];
	double nextTwitch = 0;
	int twitches = 0;
	double gaspAt = -1;
	double supportedSince = -1;
	double rollStart = -1;
	double rollDirection = 0;
	bool rolled = false;
	int steps = 0;
	double reachErr[2] = {};
	V3 reachLocal[2];
	bool reachActive[2] = {};
	bool clearedFlank[2] = {};
	ReachTrack reachTrack[2];
	bool resting[2] = {};
	double gaveUpAt[2] = {};
	bool relaxing = false;
	int layers = 0; // PickLayers: what this death plays
	V3 slideStart;
	bool slideValid = false;
	V3 placedRelative[kBodies], placedVelocity[kBodies], nudge[kBodies];
	double contactWeight[kBodies] = {};
	bool contactValid[kBodies] = {};
};
Nerve g_nerves[kMaxNerves];

struct UserState { int hitgroup = 0; double hurtTime = -1000; };
UserState g_users[kMaxUsers];
int g_lastTick = -1;
int g_reacted = 0, g_limp = 0, g_failed = 0;

const char* PhaseName(Phase p) { return p == Phase::Waiting ? "waiting" : (p == Phase::Reacting ? "reacting" : "free"); }

void End(Nerve& n, const char* why) {
	if (n.phase != Phase::Free)
		MvmDebugLog_Linef("nervous", "end user=%d injury=%s why=%s t=%.2f life=%.2f steps=%d twitches=%d",
			n.userid, InjuryName(n.injury), why, g_MirvTime.curtime_get() - n.start, n.life, n.steps, n.twitches);
	n = Nerve{};
}

// A seek, a level change or turning the feature off: let go WITHOUT touching a single body. The engine
// may have just rebuilt the world, and writing into a freed ragdoll crashed CS2 in Death Animations.
void DropAll(const char* why) {
	for (Nerve& n : g_nerves) if (n.phase != Phase::Free) End(n, why);
}

V3 P(const float* p, int b) { return BodyPos(p, b); }
V3 FromF(const float v[3]) { return Make(v[0], v[1], v[2]); }
void ToF(V3 v, float out[3]) { out[0] = float(v.x); out[1] = float(v.y); out[2] = float(v.z); }

// The bodies of a limb, by limb index (0 left arm, 1 right arm, 2 left leg, 3 right leg).
void LimbBodies(int limb, int out[3]) {
	static const int kLimbs[4][3] = {
		{ kUpperArmLeft, kForearmLeft, kHandLeft }, { kUpperArmRight, kForearmRight, kHandRight },
		{ kThighLeft, kCalfLeft, kAnkleLeft }, { kThighRight, kCalfRight, kAnkleRight } };
	for (int k = 0; k < 3; ++k) out[k] = kLimbs[limb][k];
}

void Begin(Nerve& n, double now) {
	n.injury = g_force != Injury::None ? g_force : n.injury;
	n.prof = ProfileFor(n.injury, n.seed);
	if (!n.prof.reacts) {
		++g_limp;
		MvmDebugLog_Linef("nervous", "limp user=%d injury=%s (no reaction)", n.userid, InjuryName(n.injury));
		n = Nerve{};
		return;
	}
	n.life = LifeSeconds(g_duration, n.prof, n.seed);
	n.start = now;
	n.phase = Phase::Reacting;
	n.nextTwitch = kOnset + TwitchGap(n.seed, 0, TwitchRate(1.0, n.prof.twitch));
	for (double& w : n.contactWeight) w = 1.0;
	n.layers = PickLayers((g_reach ? kLayerReach : 0) | (g_breath ? kLayerBreath : 0) | (g_writhe ? kLayerWrithe : 0)
		| (g_twitch ? kLayerTwitch : 0) | (g_rollOver ? kLayerRoll : 0), n.seed);
	++g_reacted;
	MvmDebugLog_Linef("nervous", "react user=%d injury=%s life=%.2f reach=%d curl=%d pedal=%d limp=%d writhe=%.2f afterClip=%d layers=%s%s%s%s%s",
		n.userid, InjuryName(n.injury), n.life, n.prof.reach, n.prof.curl, n.prof.pedal, n.prof.limp, n.prof.writhe,
		n.waitedForClip ? 1 : 0, (n.layers & kLayerReach) ? "reach " : "", (n.layers & kLayerRoll) ? "roll " : "",
		(n.layers & kLayerBreath) ? "breath " : "", (n.layers & kLayerWrithe) ? "writhe " : "", (n.layers & kLayerTwitch) ? "twitch" : "");
}

// The bodies a twitch of limb 0..4 (hands, feet, head) moves, with their share of it.
int TwitchBodies(int limb, int out[2], double share[2]) {
	static const int kEnd[5] = { kHandLeft, kHandRight, kAnkleLeft, kAnkleRight, kHeadBody };
	static const int kMid[5] = { kForearmLeft, kForearmRight, kCalfLeft, kCalfRight, -1 };
	out[0] = kEnd[limb]; share[0] = limb == 4 ? 0.4 : 1.0;
	out[1] = kMid[limb]; share[1] = 0.5;
	return kMid[limb] >= 0 ? 2 : 1;
}

// One physics tick of reactions. Reach and leg curl steer limbs to IK goals; writhe, breathing,
// twitches and the last breath move bodies by small pose nudges. Native velocities are preserved,
// and contact rejected by the solver reduces the next nudge's weight.
void Step(Nerve& n, double t, const DeathAnimationsMath::Pose actual[kBodies]) {
	// This death's layers (PickLayers), still switched off live by their settings.
	const bool reachOn = g_reach && (n.layers & kLayerReach), breathOn = g_breath && (n.layers & kLayerBreath);
	const bool writheOn = g_writhe && (n.layers & kLayerWrithe), twitchOn = g_twitch && (n.layers & kLayerTwitch);
	// Rolling over exposes the wound, so a death that reaches for it rolls over too.
	const bool rollOn = (n.layers & kLayerRoll) && (g_rollOver || (reachOn && n.prof.reach));
	float p[kBodies * 3];
	for (int b = 0; b < kBodies; ++b) for (int k = 0; k < 3; ++k) p[b * 3 + k] = actual[b].p[k];
	V3 physLin[kBodies], physAng[kBodies];
	for (int b = 0; b < kBodies; ++b) {
		float l[3], a[3];
		if (!ReadVelocity(n.set.bodies[b], l, a)) { l[0] = l[1] = l[2] = 0; a[0] = a[1] = a[2] = 0; }
		physLin[b] = FromF(l); physAng[b] = FromF(a);
	}
	const double dt = n.lastStep >= 0 && t > n.lastStep ? (t - n.lastStep < 0.1 ? t - n.lastStep : 0.1) : DeathAnimationsMath::kPhysicsStep;
	n.lastStep = t;
	for (int b=0;b<kBodies;++b) {
		if (!n.contactValid[b]) continue;
		const V3 relative = P(p,b)-P(p,kPelvis);
		const V3 correction = relative-n.placedRelative[b]-n.placedVelocity[b]*dt;
		n.contactWeight[b] = ContactWeight(n.contactWeight[b],n.nudge[b],correction,dt);
		n.contactValid[b] = false;
	}
	n.frame = SmoothFrame(n.frame, BodyFrame(p), dt, kFrameSmoothing);
	if (!n.frame.valid) return;
	if (!n.restValid) {
		// Each leg's own direction when the reaction began, in the body frame: where a curl starts from.
		for (int leg = 0; leg < 2; ++leg) {
			const int hip = leg ? kThighRight : kThighLeft, knee = leg ? kCalfRight : kCalfLeft, ankle = leg ? kAnkleRight : kAnkleLeft;
			V3 th = P(p, knee) - P(p, hip), sh = P(p, ankle) - P(p, knee);
			if (!Normalize(th) || !Normalize(sh)) return;
			n.restThigh[leg] = ToLocal(n.frame, th);
			n.restShin[leg] = ToLocal(n.frame, sh);
		}
		n.restValid = true;
		if (MvmDebugLog_Active()) {
			char buf[1024]; size_t used = 0;
			for (int b = 0; b < kBodies && used < sizeof(buf); ++b)
				used += (size_t)_snprintf_s(buf + used, sizeof(buf) - used, _TRUNCATE, " %d=(%.0f %.0f %.0f)", b, p[b * 3], p[b * 3 + 1], p[b * 3 + 2]);
			MvmDebugLog_Linef("nervous.pose", "user=%d front=(%.2f %.2f %.2f) up=(%.2f %.2f %.2f)%s", n.userid,
				n.frame.front.x, n.frame.front.y, n.frame.front.z, n.frame.up.x, n.frame.up.y, n.frame.up.z, buf);
		}
	}

	const bool support = GroundReactionCandidate(n.frame, physLin[kPelvis], physLin[kSpine2], physLin[kHeadBody]);
	if (!support) n.supportedSince = -1;
	else if (n.supportedSince < 0) n.supportedSince = t;
	if (t >= n.life && n.rollStart >= 0 && !n.rolled) n.rolled = true; // the relax does not keep rolling
	const bool rolling = n.rollStart >= 0 && !n.rolled;
	if (!rolling && (n.supportedSince < 0 || t - n.supportedSince < 0.25)) {
		for (int b = 0; b < kBodies; ++b) { n.driven[b] = false; n.baseValid[b] = false; }
		n.reachActive[0]=n.reachActive[1]=false;
		return; // no transform or velocity writes while falling
	}
	// Slide is measured from where steering starts; a roll moves the body on purpose, so it re-bases.
	if (!n.slideValid || rolling) { n.slideStart = P(p, kPelvis); n.slideValid = true; }
	// Use the current orientation, not the delayed reaction frame, to choose the shortest roll.
	const Frame rollFrame = BodyFrame(p);
	if (rollOn && !n.rolled && t < n.life && FaceDown(rollFrame) && n.rollStart < 0) {
		n.rollStart = t;
		n.rollDirection = RollDirection(rollFrame, n.seed);
	}
	if (n.rollStart >= 0 && !n.rolled) {
		if (!rollOn || rollFrame.front.z > 0.7 || t - n.rollStart >= 4.0) {
			n.rolled = true;
			n.frame = rollFrame;
			MvmDebugLog_Linef("nervous.roll", "finish user=%d age=%.2f frontZ=%.2f completed=%d",
				n.userid, t-n.rollStart, rollFrame.front.z, rollFrame.front.z>0.7);
		}
		else {
			const double age = t-n.rollStart;
			if (!rollFrame.valid || std::fabs(physLin[kPelvis].z)>120.0) {
				n.rollStart+=dt;
				return; // a ledge fall interrupts the roll until the body lands again
			}
			const V3 roll = RollVelocity(rollFrame, n.rollDirection, age, g_strength);
			const V3 pivot = P(p,kPelvis) + rollFrame.right*(n.rollDirection*5.0) - Make(0,0,4.0);
			const double currentRate = Dot((physAng[kPelvis]+physAng[kSpine2])*0.5,rollFrame.up);
			const V3 correction = roll-rollFrame.up*currentRate;
			// Drive a coherent turn through the connected body. Twisting only pelvis/chest
			// was cancelled by the passive limbs and could trip the airborne gate itself.
			for (int b = 0; b < kBodies; ++b) {
				V3 linear = physLin[b]+Cross(correction*(1.0/kRadToDeg),P(p,b)-pivot);
				const V3 delta = linear-physLin[b];
				if (Length(delta)>80.0) linear=physLin[b]+delta*(80.0/Length(delta));
				float l[3], a[3]; ToF(linear,l);
				ToF(physAng[b]-rollFrame.up*Dot(physAng[b],rollFrame.up)+roll,a);
				bool reasserted = false; DriveBody(n.set.bodies[b], l, a, reasserted);
			}
			// Brace the lower arm alongside the shoulder; sweep the free arm across the
			// upper torso. Both are joint-preserving nudges, with the floor solver in charge.
			for (int h=0; h<2; ++h) {
				if(n.prof.limp&(1<<h))continue;
				const int root=h?kUpperArmRight:kUpperArmLeft, mid=h?kForearmRight:kForearmLeft, end=h?kHandRight:kHandLeft;
				const double side=h?1.0:-1.0;
				const bool brace=side==n.rollDirection;
				const V3 s=P(p,root), m=P(p,mid), w=P(p,end);
				V3 goal=brace ? s+rollFrame.right*(side*6.0)-rollFrame.up*7.0 :
					P(p,kSpine2)+rollFrame.front*9.0+rollFrame.right*(n.rollDirection*7.0);
				goal.z=(std::max)(goal.z,(std::min)(w.z,s.z));
				V3 elbow,wrist;
				if (!TwoBoneIk(s,Length(m-s),Length(w-m),goal,rollFrame.right*side+Make(0,0,1),kReach,kFold,elbow,wrist)) continue;
				const V3 upper=SwingVelocity(s,m,elbow,n.prevAng[root],kNerveAngular,120.0,dt,dt);
				const V3 lower=LowerSwing(m,w,elbow,wrist,upper,n.prevAng[mid]+n.prevAng[root],kNerveAngular,120.0,dt,dt);
				n.prevAng[root]=upper;n.prevAng[mid]=lower;
				const DeathAnimationsMath::Pose in[3]={actual[root],actual[mid],actual[end]};
				DeathAnimationsMath::Pose out[3];NudgeChain(in,upper,lower,dt,out);
				const int bones[3]={root,mid,end};
				for(int j=0;j<3;++j) {
					float l[3],a[3];ReadVelocity(n.set.bodies[bones[j]],l,a);
					bool reasserted=false;HoldBody(n.set.bodies[bones[j]],out[j],l,a,reasserted);
				}
			}
			if (MvmDebugLog_Active() && n.steps++ % 16 == 0)
				MvmDebugLog_Linef("nervous.roll", "user=%d t=%.2f frontZ=%.2f side=%.0f angular=%.1f", n.userid, t, rollFrame.front.z, n.rollDirection, Length(roll));
			return;
		}
	}
	const double c = Consciousness(t, n.life);
	const double gate = SpeedGate(Length(physLin[kPelvis]));
	const double live = c * gate;
	const double accel = NerveAccel(g_strength, c, gate);
	const double spasmAccel = NerveAccel(g_strength, kSpasmShare, gate);
	const bool settled = gate >= kMinGate;
	// How much of the motion plays: the reaction's ease in / out, times the muscles' grip (a weight, so
	// a fading body lets go of a limb gradually instead of stopping it dead).
	const double blend = ReactionBlend(t, n.life, g_blendIn, g_blendOut);
	const double amount = blend * MuscleWeight(accel);
	const bool hold = settled && amount > 1e-3;
	const double step = DeathAnimationsMath::kPhysicsStep;
	const bool faceDown = FaceDown(n.frame);
	const bool keepDown = faceDown; // protect the floor until the roll exposes the chest
	const V3 pelvis = P(p, kPelvis), pelvisVel = physLin[kPelvis];

	V3 cmd[kBodies];
	bool write[kBodies] = {}, busy[kBodies] = {};
	// Which layer wrote each body this tick (nervous.drift).
	const char* tag[kBodies] = {};
	const char* layer = "";
	int twitchBusy = 0;
	for (int limb = 0; limb < 4; ++limb) {
		if (!(n.prof.limp & (1 << limb))) continue;
		int lb[3]; LimbBodies(limb, lb);
		for (int b : lb) busy[b] = true;
		twitchBusy |= 1 << limb; // TwitchLimb's bits: hands 0/1, feet 2/3
	}
	// What each written body gets this tick: a nudged pose and its own unchanged native velocities.
	DeathAnimationsMath::Pose place[kBodies];
	V3 angCmd[kBodies], velOut[kBodies], angOut[kBodies];
	for (int b = 0; b < kBodies; ++b) { angCmd[b] = V3(); velOut[b] = physLin[b]; angOut[b] = physAng[b]; }
	const double angAccel = accel * (kNerveAngular / kNerveAccel);
	const double spasmAngular = spasmAccel * (kNerveAngular / kNerveAccel);
	// One body toward a goal (the chest and head): `cmd` is the relative nudge rate.
	auto steer = [&](int b, V3 goal, V3 goalVelocity, double acc, double maxSpeed, double share) {
		share *= n.contactWeight[b];
		const V3 previous = n.driven[b] ? n.prevCmd[b] : V3();
		cmd[b] = DriveToward(P(p, b), goal, goalVelocity, previous, acc, maxSpeed, step, dt);
		place[b] = actual[b];
		const double move = dt * share;
		for (int k = 0; k < 3; ++k) place[b].p[k] = float(actual[b].p[k] + (k == 0 ? cmd[b].x : k == 1 ? cmd[b].y : cmd[b].z) * move);
		// Preserve the solver's own velocities. Broadcasting pelvis velocity to every driven body
		// fed a small contact push back into the entire ragdoll on the following tick.
		write[b] = busy[b] = true;
		tag[b] = layer;
	};
	// A limb as a chain: the upper segment turns about the root toward `midGoal`, the lower about the mid
	// joint toward `endGoal` (the end body turns with the lower segment), nudged by this tick's share.
	auto chain = [&](int root, int mid, int end, V3 midGoal, V3 endGoal, double acc, double share) {
		share *= (std::min)(n.contactWeight[mid], n.contactWeight[end]);
		const V3 r = P(p, root), m = P(p, mid), e = P(p, end);
		const V3 up = SwingVelocity(r, m, midGoal, n.driven[root] ? n.prevAng[root] : V3(), acc, kSwingSpeed, step, dt);
		const V3 previousLower = n.driven[mid] ? n.prevAng[mid] + n.prevAng[root] : V3();
		const V3 lo = LowerSwing(m, e, midGoal, endGoal, up, previousLower, acc, kSwingSpeed, step, dt);
		const DeathAnimationsMath::Pose in[3] = { actual[root], actual[mid], actual[end] };
		DeathAnimationsMath::Pose out[3];
		NudgeChain(in, up, lo, dt * share, out);
		if (end == kHandLeft || end == kHandRight) {
			const double before = ArmClearance(p, m, e), after = ArmClearance(p, FromF(out[1].p), FromF(out[2].p));
			if (after < 6.5 && after < before - 0.01) return;
		}
		place[root] = out[0]; place[mid] = out[1]; place[end] = out[2];
		angCmd[root] = up; angCmd[mid] = lo; angCmd[end] = lo;
		cmd[root] = PointVelocity(V3(), up, (m - r) * 0.5);
		cmd[mid] = PointVelocity(PointVelocity(V3(), up, m - r), lo, (e - m) * 0.5);
		cmd[end] = PointVelocity(PointVelocity(V3(), up, m - r), lo, e - m);
		write[root] = write[mid] = write[end] = busy[root] = busy[mid] = busy[end] = true;
		tag[root] = tag[mid] = tag[end] = layer;
	};
	struct LimbDef { int root, mid, end, anchor; };
	static const LimbDef kLimbDefs[4] = {
		{ kUpperArmLeft, kForearmLeft, kHandLeft, kSpine2 }, { kUpperArmRight, kForearmRight, kHandRight, kSpine2 },
		{ kThighLeft, kCalfLeft, kAnkleLeft, kPelvis }, { kThighRight, kCalfRight, kAnkleRight, kPelvis } };

	// Relax: the reaction is over. Lower the arms to the torso's sides and let the legs fall back to
	// where they started, so nothing stays frozen in its last pose. Gravity-like: the same drive at any
	// strength, and contact (the floor, the body) stops it. No reach, layers or twitches run now:
	// consciousness and the blend are 0, so `hold` is false below.
	if (t >= n.life) {
		if (!n.relaxing) {
			n.relaxing = true;
			for (double& w : n.contactWeight) w = 1.0;
			MvmDebugLog_Linef("nervous", "relax user=%d t=%.2f", n.userid, t);
		}
		const double k = RelaxWeight(t - n.life);
		for (int limb = 0; limb < 4 && k > 1e-3; ++limb) {
			if (n.prof.limp & (1 << limb)) continue; // already hanging limp
			// An arm the body lies on stays where it is: pulling it out would lever the body over.
			if (limb < 2 && (n.reachTrack[limb].why == GiveUpWhy::Pinned || ArmPinned(p, n.frame, limb))) continue;
			const LimbDef& d = kLimbDefs[limb];
			const V3 r = P(p, d.root), m = P(p, d.mid), e = P(p, d.end);
			const double a = Length(m - r), b = Length(e - m);
			V3 mid, end;
			if (limb < 2) {
				const V3 goal = RelaxArmGoal(n.frame, r, a + b, limb);
				if (!TwoBoneIk(r, a, b, goal, m - r, 0.95, kFold, mid, end)) continue;
			} else if (!LegGoal(n.frame, r, a, b, n.restThigh[limb - 2], n.restShin[limb - 2], 0.0, mid, end)) continue;
			layer = "relax";
			chain(d.root, d.mid, d.end, mid, end, kNerveAngular * 0.5, k);
		}
	}

	// Reach: the hands go to the wound, the elbow bending the way it already bends. Lying face down the
	// chest is against the floor, so the hand goes to the wound's flank instead of its front.
	// Staying face down, a wound against the floor is left alone and a limb wound is held on its upper
	// side, so no hand levers the body over.
	if (hold && reachOn && n.prof.reach && (!keepDown || FaceDownReach(n.prof))) {
		const int count = ((n.prof.reach & kHandBitLeft) ? 1 : 0) + ((n.prof.reach & kHandBitRight) ? 1 : 0);
		const V3 outward = keepDown ? n.frame.front * -1.0 : (faceDown ? V3() : n.frame.front);
		int index = 0;
		for (int h = 0; h < 2; ++h) {
			if (!(n.prof.reach & (h ? kHandBitRight : kHandBitLeft)) || (n.prof.limp & (1 << h))) continue;
			const LimbDef& d = kLimbDefs[h];
			// Reserve a reaching arm even if a collision blocks this tick. Letting the writhe
			// layer take it back made an obstructed reach change direction repeatedly.
			busy[d.root]=busy[d.mid]=busy[d.end]=true;
			// No clearance, no reach: an arm under the body or behind the back is left to physics for
			// the rest of the reaction (no relax pull either), the other hand reaching on alone.
			if (!n.reachTrack[h].gaveUp && ArmPinned(p, n.frame, h)) {
				n.reachTrack[h].gaveUp = true;
				n.reachTrack[h].why = GiveUpWhy::Pinned;
				n.gaveUpAt[h] = t - kRelaxTime;
				n.resting[h] = false; n.reachActive[h] = false;
				MvmDebugLog_Linef("nervous", "giveup user=%d hand=%d why=pinned t=%.2f", n.userid, h, t);
			}
			if (n.reachTrack[h].why == GiveUpWhy::Pinned) {
				tag[d.root] = tag[d.mid] = tag[d.end] = "pinned";
				twitchBusy |= 1 << h;
				++index;
				continue;
			}
			const V3 s = P(p, d.root), e0 = P(p, d.mid), w0 = P(p, d.end);
			V3 goal, elbow, wrist;
			double fallback = 0;
			const int i = index++;
			if (!ReachGoal(p, n.prof, s, outward, i, count, t, live,
				Length(e0 - s) + Length(w0 - e0), goal, fallback)) continue;
			// Follow a continuous hand target in the torso frame. Restarting the motion
            // from the solver's resting wrist every tick left the arm permanently short.
            const V3 previous=n.reachActive[h]?s+ToWorld(n.frame,n.reachLocal[h]):w0;
            // Never force a hand the solver keeps rejecting: that push is what slid the body. Arrival
            // is judged against the wound itself: the flank waypoint below jumps 13 units to the side
            // whenever the wrist dips behind the chest plane, and judging against it kicked a resting
            // hand back out every few ticks.
            const V3 wound=goal;
            const double armLength=Length(e0-s)+Length(w0-e0);
            const bool wasGivenUp=n.reachTrack[h].gaveUp;
            const V3 slid=pelvis-n.slideStart;
            const ReachMode mode=ArbitrateReach(n.reachTrack[h],Length(w0-wound),Length(wound-s),armLength,
                n.contactWeight[d.end],std::hypot(slid.x,slid.y),dt);
            if (mode==ReachMode::GiveUp && !wasGivenUp) n.gaveUpAt[h]=t;
            if (mode==ReachMode::GiveUp && t-n.gaveUpAt[h]<kRelaxTime) {
                // A given-up arm goes limp: lowered to the body's side like the final relax, rather
                // than left frozen wherever joint friction caught it.
                if (!wasGivenUp) {
                    MvmDebugLog_Linef("nervous", "giveup user=%d hand=%d why=%s wristToGoal=%.1f shoulderToGoal=%.1f arm=%.1f grip=%.2f",
                        n.userid, h, GiveUpName(n.reachTrack[h].why), Length(w0-wound), Length(wound-s), armLength, n.contactWeight[d.end]);
                    n.contactWeight[d.root]=n.contactWeight[d.mid]=n.contactWeight[d.end]=1.0;
                }
                n.resting[h]=false;n.reachActive[h]=false;
                twitchBusy|=1<<h;
                const double a=Length(e0-s), b=Length(w0-e0);
                if (TwoBoneIk(s,a,b,RelaxArmGoal(n.frame,s,a+b,h),e0-s,0.95,kFold,elbow,wrist)) {
                    layer="giveup";
                    chain(d.root,d.mid,d.end,elbow,wrist,kNerveAngular*0.5,RelaxWeight(t-n.gaveUpAt[h]));
                }
                continue;
            }
            goal=ReachRoute(p,n.frame,previous,wound,h,n.clearedFlank[h]);
            if (mode!=ReachMode::Drive) {
                if (mode==ReachMode::Rest && !n.resting[h])
                    MvmDebugLog_Linef("nervous.reach", "user=%d h=%d t=%.2f rest wristToWound=%.1f shoulderToWound=%.1f arm=%.1f grip=%.2f",
                        n.userid,h,t,Length(w0-wound),Length(wound-s),armLength,n.contactWeight[d.end]);
                n.resting[h]=mode==ReachMode::Rest;
                tag[d.root]=tag[d.mid]=tag[d.end]=mode==ReachMode::Rest?"rest":"giveup";
                // The hand stays where physics has it; resuming starts from there.
                n.reachLocal[h]=ToLocal(n.frame,w0-s);n.reachActive[h]=mode==ReachMode::Rest;
                twitchBusy|=1<<h;
                continue;
            }
            if (n.resting[h]) {
                // Leaving rest (the wound moved away): the old rejection says nothing about the new path.
                MvmDebugLog_Linef("nervous.reach", "user=%d h=%d t=%.2f unrest wristToWound=%.1f shoulderToWound=%.1f arm=%.1f wound=(%.1f %.1f %.1f) wrist=(%.1f %.1f %.1f) fallback=%.2f",
                    n.userid,h,t,Length(w0-wound),Length(wound-s),armLength,wound.x,wound.y,wound.z,w0.x,w0.y,w0.z,fallback);
                n.resting[h]=false;
                n.contactWeight[d.root]=n.contactWeight[d.mid]=n.contactWeight[d.end]=1.0;
            }
            const V3 next=ReachAdvance(previous,goal,dt);
            if (!ReachIk(p,n.frame,h,s,e0,w0,next,elbow,wrist)) continue;
            n.reachLocal[h]=ToLocal(n.frame,wrist-s);n.reachActive[h]=true;
            const DeathAnimationsMath::Pose in[3]={actual[d.root],actual[d.mid],actual[d.end]};
            DeathAnimationsMath::Pose out[3];
            AimChain(in,elbow,wrist,amount,out);
            {   // No pops: cap how far the hand is placed this tick.
                const double moved=Length(FromF(out[2].p)-w0);
                const double capped=CapPlaceWeight(amount,moved);
                if (capped<amount) AimChain(in,elbow,wrist,capped,out);
            }
            place[d.root]=out[0];place[d.mid]=out[1];place[d.end]=out[2];
            write[d.root]=write[d.mid]=write[d.end]=true;
            tag[d.root]=tag[d.mid]=tag[d.end]="reach";
			twitchBusy |= 1 << h;
			n.reachErr[h] = Length(wrist - w0);
			if (MvmDebugLog_Active() && (n.steps % 8) == 0)
				MvmDebugLog_Linef("nervous.reach", "user=%d h=%d t=%.2f fallback=%.2f grip=%.2f shoulder=(%.1f %.1f %.1f) wrist=(%.1f %.1f %.1f) goal=(%.1f %.1f %.1f) ik=(%.1f %.1f %.1f) err=%.1f phys=(%.0f %.0f %.0f) cmd=(%.0f %.0f %.0f) swing=%.0f/%.0f",
					n.userid, h, t, fallback, n.contactWeight[d.end], s.x, s.y, s.z, w0.x, w0.y, w0.z, goal.x, goal.y, goal.z, wrist.x, wrist.y, wrist.z, n.reachErr[h],
					physLin[d.end].x, physLin[d.end].y, physLin[d.end].z, cmd[d.end].x, cmd[d.end].y, cmd[d.end].z,
					Length(angCmd[d.root]), Length(angCmd[d.mid]));
		}
	}

	// Legs: drawn up to meet a leg or gut wound (reach), or pedalling after a blast (writhe). Lying face
	// down there is little room to draw a knee under the body.
	for (int leg = 0; leg < 2 && hold; ++leg) {
		const int bit = leg ? kLegBitRight : kLegBitLeft;
		if (n.prof.limp & (1 << (2 + leg))) continue;
		double w = -1;
		if (reachOn && (n.prof.curl & bit)) w = 0.75 + 0.25 * std::sin(2.0 * kPi * 0.35 * t + leg * 2.1);
		else if (writheOn && (n.prof.pedal & bit)) w = PedalCurl(t, leg) * (n.prof.writhe > 1 ? 1.0 : n.prof.writhe);
		if (w < 0) continue;
		w *= Clamp01(live * 1.5);
		if (keepDown) w *= (n.prof.pedal & bit) ? 0.2 : 0.0; // no knee drawn under the body
		else if (faceDown) w *= 0.3;
		if (w <= 0) continue;
		const LimbDef& d = kLimbDefs[2 + leg];
		const V3 h0 = P(p, d.root), k0 = P(p, d.mid), a0 = P(p, d.end);
		V3 knee, ankle;
		const bool injuredLeg=n.prof.woundBody==d.root;
		if (!LegGoal(n.frame, h0, Length(k0 - h0), Length(a0 - k0), n.restThigh[leg], n.restShin[leg], w, knee, ankle, injuredLeg?1.95:kCurlAngle)) continue;
		layer = (n.prof.curl & bit) ? "curl" : "pedal";
		chain(d.root, d.mid, d.end, knee, ankle, angAccel * 0.8, amount);
		twitchBusy |= 1 << (2 + leg);
	}

	// Twitches: sparse jerks of a free hand, foot or the head, more of them as the body fades.
	if (twitchOn && settled && c > 0.02 && t >= n.nextTwitch) {
		const int limb = TwitchLimb(n.seed, n.twitches, twitchBusy);
		if (limb >= 0) {
			for (Nerve::Twitch& tw : n.twitch) {
				if (tw.limb >= 0 && t - tw.t0 < kTwitchTime) continue;
				tw.limb = limb; tw.t0 = t;
				tw.local = TwitchKick(n.seed, n.twitches, kTwitchSize * (g_strength > 1.5 ? 1.5 : g_strength) * (blend > 0.3 ? 1.0 : blend / 0.3));
				if (keepDown) tw.local.x = -std::fabs(tw.local.x); // away from the floor
				MvmDebugLog_Linef("nervous.twitch", "user=%d n=%d t=%.2f limb=%d size=%.1f c=%.2f", n.userid, n.twitches, t, limb, Length(tw.local), c);
				break;
			}
		}
		++n.twitches;
		n.nextTwitch = t + TwitchGap(n.seed, n.twitches, TwitchRate(c, n.prof.twitch));
	}
	// The last breath before the body goes still.
	if (breathOn && settled && n.gaspAt < 0 && t >= n.life - kLastGaspLead) {
		n.gaspAt = t;
		MvmDebugLog_Linef("nervous", "lastgasp user=%d t=%.2f", n.userid, t);
	}

	// Offsets: writhe, breathing, twitches and the last breath, for every limb end, the chest and the
	// head not already steered. `spasm` reports a twitch or the last breath moving the body right now.
	const double breathAmp = breathOn ? n.prof.breath * live * blend : 0.0;
	const double writheAmt = writheOn ? n.prof.writhe * live * blend : 0.0;
	const double rate = BreathRate(c);
	n.breathPhase += 2.0 * kPi * rate * dt;
	auto offset = [&](int b, double at, double phase, bool& spasm) {
		V3 local = WritheOffset(n.seed, b, at, writheAmt);
		double lift = 0;
		if (b == kSpine2 || b == kHeadBody) {
			lift = (b == kSpine2 ? kBreathChest : kBreathHead) * breathAmp * BreathLift(phase);
			const double g = breathOn && n.gaspAt >= 0 ? Pulse(at - n.gaspAt, kLastGaspTime) : 0.0;
			if (g > 0) { lift += kLastGaspLift * g * (b == kSpine2 ? 1.0 : 0.5); spasm = true; }
		}
		if (keepDown) local = FaceDownOffset(local, lift * 0.5);
		else local.x += lift;
		V3 world = ToWorld(n.frame, local);
		for (const Nerve::Twitch& tw : n.twitch) {
			if (!twitchOn || tw.limb < 0) continue;
			int tb[2]; double share[2];
			const int k = TwitchBodies(tw.limb, tb, share);
			for (int j = 0; j < k; ++j) {
				if (tb[j] != b) continue;
				const double pulse = Pulse(at - tw.t0, kTwitchTime);
				if (pulse > 0) { world = world + ToWorld(n.frame, tw.local) * (pulse * share[j]); spasm = true; }
			}
		}
		return world;
	};
	const double breathBefore = n.breathPhase - 2.0 * kPi * rate * kSlope;
	// The goal of an offset body: its base (following where physics leaves it, in the body frame) plus
	// the offset now. False when nothing should move it this tick.
	auto offsetGoal = [&](int b, V3& goal, V3& goalVelocity, bool& spasm) -> bool {
		spasm = false;
		bool before = false;
		const V3 d = offset(b, t, n.breathPhase, spasm);
		const V3 dBefore = offset(b, t - kSlope, breathBefore, before);
		// A disabled layer must not hold an old base, overwrite a resting body's velocity, or
		// wake its neighbours through the joints. A live pulse crossing zero still has a slope.
		if (Length(d) < 1e-5 && Length(dBefore) < 1e-5) { n.baseValid[b] = false; return false; }
		if (!hold && !spasm) { n.baseValid[b] = false; return false; }
		const V3 a = P(p, b);
		if (!n.baseValid[b]) { n.baseLocal[b] = ToLocal(n.frame, a - d - pelvis); n.baseValid[b] = true; }
		const V3 base = FollowBase(pelvis + ToWorld(n.frame, n.baseLocal[b]), a - d, dt);
		n.baseLocal[b] = ToLocal(n.frame, base - pelvis);
		goal = base + d;
		goalVelocity = (d - dBefore) * (1.0 / kSlope);
		return true;
	};
	// Free limbs: the end body's goal moves by the offsets, and the chain follows with the joint bent the
	// way it already is.
	for (int limb = 0; limb < 4; ++limb) {
		const LimbDef& d = kLimbDefs[limb];
		if (busy[d.end] || (n.prof.limp & (1 << limb))) continue;
		V3 goal, goalVel;
		bool spasm = false;
		if (!offsetGoal(d.end, goal, goalVel, spasm)) continue;
		const V3 r = P(p, d.root), m = P(p, d.mid), e = P(p, d.end);
		V3 mid, end;
		if (!TwoBoneIk(r, Length(m - r), Length(e - m), goal, m - r, 1.0, 0.0, mid, end)) continue;
		const double acc = hold ? (spasm && spasmAngular > angAccel ? spasmAngular : angAccel) : spasmAngular;
		layer = spasm ? "spasm" : "writhe";
		chain(d.root, d.mid, d.end, mid, end, acc * (limb >= 2 ? 0.8 : 1.0), hold ? amount : blend);
	}
	// The chest and head: straight toward their offset.
	for (int b : { kSpine2, kHeadBody }) {
		if (busy[b]) continue;
		V3 goal, goalVel;
		bool spasm = false;
		if (!offsetGoal(b, goal, goalVel, spasm)) continue;
		const double acc = hold ? (spasm && spasmAccel > accel ? spasmAccel : accel) : spasmAccel;
		layer = spasm ? "spasm" : "torso";
		steer(b, goal, goalVel, acc, kLayerSpeed, hold ? amount : blend);
	}

	int written = 0;
	for (int b = 0; b < kBodies; ++b) {
		if (!write[b]) { n.driven[b] = false; n.baseValid[b] = false; }
		if (!write[b]) continue;
		float l[3], a[3];
		ToF(velOut[b], l); ToF(angOut[b], a);
		bool reasserted = false;
		if (!HoldBody(n.set.bodies[b], place[b], l, a, reasserted)) { n.driven[b] = false; continue; }
		n.nudge[b] = FromF(place[b].p)-P(p,b);
		n.placedRelative[b] = FromF(place[b].p)-pelvis;
		n.placedVelocity[b] = physLin[b]-pelvisVel;
		n.contactValid[b] = true;
		++written;
		{ n.driven[b] = true; n.prevCmd[b] = cmd[b]; n.prevAng[b] = angCmd[b]; }
	}
	// Only an actively driven, settled reaction gets extra floor grip. No position is anchored;
	// vertical velocity, the initial slide, and the roll's own motion remain native.
	const double friction = written ? ReactionFriction(std::hypot(pelvisVel.x,pelvisVel.y),dt) : 1.0;
	if (friction < 0.9999) {
		for (int b=0;b<kBodies;++b) {
			DampHorizontal(n.set.bodies[b],float(friction));
			// Contact prediction must use the velocity actually left for the next physics step.
			n.placedVelocity[b].x *= friction;
			n.placedVelocity[b].y *= friction;
		}
	}
	if (MvmDebugLog_Active() && (n.steps % 32) == 0)
		MvmDebugLog_Linef("nervous.frame", "user=%d t=%.2f c=%.2f gate=%.2f accel=%.0f blend=%.2f amount=%.2f written=%d reachErr=%.1f/%.1f front.z=%.2f keepDown=%d breathHz=%.2f pelvis=(%.2f %.2f %.2f) speed=%.2f contact=%.2f/%.2f/%.2f",
			n.userid, t, c, gate, accel, blend, amount, written, n.reachErr[0], n.reachErr[1], n.frame.front.z, keepDown ? 1 : 0,
			 rate, pelvis.x, pelvis.y, pelvis.z, Length(pelvisVel),n.contactWeight[kHandLeft],n.contactWeight[kHandRight],n.contactWeight[kSpine2]);
	// Slide and spin, and who is moving each body (every 4 physics ticks): slide = the pelvis's planar
	// distance from where the reaction started steering, yaw = the torso's spin about world up (deg/s).
	// Per body: the layer that wrote it (- none, rest/giveup a parked hand), its contact weight, its
	// height above the lowest body (the floor proxy) and its speed.
	if (MvmDebugLog_Active() && (n.steps % 4) == 0) {
		static const struct { int b; const char* name; } kNames[] = {
			{ kPelvis, "pel" }, { kSpine2, "chest" }, { kHeadBody, "head" },
			{ kUpperArmLeft, "uaL" }, { kForearmLeft, "faL" }, { kHandLeft, "hL" },
			{ kUpperArmRight, "uaR" }, { kForearmRight, "faR" }, { kHandRight, "hR" },
			{ kThighLeft, "thL" }, { kCalfLeft, "caL" }, { kAnkleLeft, "anL" },
			{ kThighRight, "thR" }, { kCalfRight, "caR" }, { kAnkleRight, "anR" } };
		double floorZ = 1e9;
		for (int b = 0; b < kBodies; ++b) floorZ = (std::min)(floorZ, double(p[b * 3 + 2]));
		double nudgeSum = 0;
		for (int b = 0; b < kBodies; ++b) if (write[b]) nudgeSum += Length(FromF(place[b].p) - P(p, b));
		char buf[1400]; size_t used = 0;
		for (const auto& k : kNames) {
			if (used >= sizeof(buf)) break;
			used += (size_t)_snprintf_s(buf + used, sizeof(buf) - used, _TRUNCATE, " %s=%s/%.2f/%.1f/%.0f", k.name,
				tag[k.b] ? tag[k.b] : "-", n.contactWeight[k.b], p[k.b * 3 + 2] - floorZ, Length(physLin[k.b]));
		}
		const V3 slide = pelvis - n.slideStart;
		MvmDebugLog_Linef("nervous.drift", "user=%d t=%.2f slide=%.1f (%.1f %.1f) planar=%.1f yaw=%.0f pelvisAng=%.0f nudge=%.2f written=%d relax=%d%s",
			n.userid, t, std::hypot(slide.x, slide.y), slide.x, slide.y, std::hypot(pelvisVel.x, pelvisVel.y),
			(physAng[kPelvis].z + physAng[kSpine2].z) * 0.5, Length(physAng[kPelvis]), nudgeSum, written, n.relaxing ? 1 : 0, buf);
	}
	++n.steps;
}

unsigned SafeHealth(CEntityInstance* e) {
	__try { return e->GetHealth(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 998; }
}

// Why a reacting body was lost, for the log: the pawn slot, the body set, the pawn's health and
// whether the bodies can be found again (and where).
void LogLost(const Nerve& n, double t) {
	if (!MvmDebugLog_Active()) return;
	CEntityInstance* now = RagdollBodies::EntityAt(n.pawnIndex);
	const unsigned health = now == n.pawn ? SafeHealth(n.pawn) : 999;
	BodySet again;
	char why[512] = {};
	const bool found = now == n.pawn && Acquire(n.pawn, again, why, sizeof(why));
	MvmDebugLog_Linef("nervous", "lost user=%d t=%.2f pawnSame=%d setCurrent=%d route=%d carrierIsPawn=%d health=%u child=%p reacquire=%d sameObject=%d routes:%s",
		n.userid, t, now == n.pawn ? 1 : 0, RagdollBodies::Current(n.set) ? 1 : 0, n.set.route, n.set.carrier == n.pawn ? 1 : 0,
		health, now == n.pawn ? ClientRagdollOf(n.pawn) : nullptr, found ? 1 : 0, found && again.object == n.set.object ? 1 : 0, why);
}

void Run(Nerve& n, double now) {
	if (RagdollBodies::EntityAt(n.pawnIndex) != n.pawn || !RagdollBodies::Current(n.set)) {
		// A practice / deathmatch demo respawns the player, and the pawn's corpse bodies go with it.
		LogLost(n, now - n.start);
		End(n, RagdollBodies::EntityAt(n.pawnIndex) == n.pawn && SafeHealth(n.pawn) > 0 && SafeHealth(n.pawn) < 998 ? "respawned" : "lost");
		return;
	}
	const double t = now - n.start;
	if (t < -0.05) { End(n, "time-reversed"); return; }
	if (t >= n.life + kRelaxTime) { End(n, "still"); return; } // Step relaxes the limbs after `life`
	DeathAnimationsMath::Pose actual[kBodies];
	for (int b = 0; b < kBodies; ++b) {
		int motion = 0;
		if (!ReadBody(n.set.bodies[b], actual[b], motion)) { End(n, "read-failed"); return; }
	}
	// Only a frame after a physics step says anything new, and a velocity written between steps is
	// what the engine hands back as the body's own (Death Animations, 2026-09-26): one write per step.
	bool stepped = !n.seenValid;
	for (int b = 0; b < kBodies && !stepped; ++b)
		stepped = std::memcmp(actual[b].p, n.seen[b].p, sizeof(actual[b].p)) != 0 || std::memcmp(actual[b].q, n.seen[b].q, sizeof(actual[b].q)) != 0;
	std::memcpy(n.seen, actual, sizeof(n.seen));
	n.seenValid = true;
	if (stepped && g_kickWatch.userid == n.userid && g_kickWatch.frames > 0) {
		--g_kickWatch.frames;
		float l[3], a[3];
		ReadVelocity(n.set.bodies[kPelvis], l, a);
		MvmDebugLog_LinefAlways("nervous.kick", "user=%d mode=%d t=%.3f dzPelvis=%.2f dzHand=%.2f dzFoot=%.2f pelvisVz=%.1f",
			n.userid, g_kickWatch.mode, t, actual[kPelvis].p[2] - g_kickWatch.z0[kPelvis], actual[kHandLeft].p[2] - g_kickWatch.z0[kHandLeft],
			actual[kAnkleLeft].p[2] - g_kickWatch.z0[kAnkleLeft], l[2]);
		return;
	}
	if (stepped && g_kickMode >= 0) {
		for (int b = 0; b < kBodies; ++b) g_kickWatch.z0[b] = actual[b].p[2];
		g_kickWatch.userid = n.userid; g_kickWatch.frames = 24; g_kickWatch.mode = g_kickMode;
		const float up[3] = {0, 0, 150}, none[3] = {0, 0, 0};
		int ok = 0;
		for (int b = 0; b < kBodies; ++b) {
			bool reasserted = false;
			if (g_kickMode == 1) ForceDynamic(n.set.bodies[b]);
			if (g_kickMode == 2) { const float zero[3] = {0, 0, 0}; ok += HoldBody(n.set.bodies[b], actual[b], zero, zero, reasserted) ? 0 : 0; }
			ok += DriveBody(n.set.bodies[b], up, none, reasserted) ? 1 : 0;
		}
		MvmDebugLog_LinefAlways("nervous.kick", "user=%d mode=%d t=%.3f kicked=%d", n.userid, g_kickMode, t, ok);
		g_kickMode = -1;
		return;
	}
	// RunFrame already permits one update per demo tick. A sleeping pose still needs the clock:
	// otherwise an isolated twitch never reaches its scheduled start and cannot wake the limb.
	Step(n, t, actual);
}

} // namespace

// ---------------------------------------------------------------------------------------------

bool NervousSystem_Enabled() { return g_enabled; }
void NervousSystem_SetEnabled(bool enabled) {
	if (g_enabled == enabled) return;
	g_enabled = enabled;
	if (!enabled) DropAll("disabled");
}
double NervousSystem_Strength() { return g_strength * 100.0; }
void NervousSystem_SetStrength(double percent) {
	if (std::isfinite(percent)) g_strength = (percent < 10 ? 10 : (percent > 200 ? 200 : percent)) / 100.0;
}
double NervousSystem_Duration() { return g_duration; }
void NervousSystem_SetDuration(double seconds) {
	if (std::isfinite(seconds)) g_duration = seconds < 2 ? 2 : (seconds > 20 ? 20 : seconds);
}
bool NervousSystem_Reach() { return g_reach; }
void NervousSystem_SetReach(bool on) {
    g_reach = on;
    if(!on)for(Nerve& n:g_nerves)for(int h=0;h<2;++h) {
        n.reachActive[h]=false;n.clearedFlank[h]=false;
    }
}
bool NervousSystem_Breath() { return g_breath; }
void NervousSystem_SetBreath(bool on) { g_breath = on; }
bool NervousSystem_Writhe() { return g_writhe; }
void NervousSystem_SetWrithe(bool on) { g_writhe = on; }
bool NervousSystem_Twitch() { return g_twitch; }
void NervousSystem_SetTwitch(bool on) { g_twitch = on; }
double NervousSystem_BlendIn() { return g_blendIn; }
void NervousSystem_SetBlendIn(double seconds) { if (std::isfinite(seconds)) g_blendIn = seconds < 0 ? 0 : (seconds > 2 ? 2 : seconds); }
double NervousSystem_BlendOut() { return g_blendOut; }
void NervousSystem_SetBlendOut(double seconds) { if (std::isfinite(seconds)) g_blendOut = seconds < 0 ? 0 : (seconds > 4 ? 4 : seconds); }
bool NervousSystem_RollOver() { return g_rollOver; }
void NervousSystem_SetRollOver(bool on) { g_rollOver = on; }

void NervousSystem_LevelInit() {
	for (Nerve& n : g_nerves) n = Nerve{};
	for (UserState& u : g_users) u = UserState{};
	g_lastTick = -1;
}

void NervousSystem_OnGameEvent(SOURCESDK::CS2::CGameEvent* event) {
	if (!g_enabled || !DeathAnimations_Enabled() || !event) return;
	const char* name = event->GetName();
	const bool hurt = 0 == _stricmp(name, "player_hurt");
	const bool death = 0 == _stricmp(name, "player_death");
	if (!hurt && !death) return;
	JsonValue r;
	if (!EventJson(event, r)) return;
	const int userid = JInt(r, "userid");
	if (userid < 0 || userid >= kMaxUsers) return;
	const double now = g_MirvTime.curtime_get();
	if (hurt) {
		g_users[userid].hitgroup = JInt(r, "hitgroup", 0);
		g_users[userid].hurtTime = now;
		return;
	}
	// Demo and live delivery can both report one death.
	for (const Nerve& n : g_nerves)
		if (n.phase != Phase::Free && n.userid == userid && std::fabs(n.deathTime - now) < 0.05) return;
	Nerve* slot = nullptr;
	for (Nerve& n : g_nerves) if (n.phase == Phase::Free) { slot = &n; break; }
	if (!slot) return;
	int pawnIndex = -1;
	CEntityInstance* pawn = PawnOfUser(userid, &pawnIndex);
	if (!pawn) return;
	const std::string weapon = JStr(r, "weapon");
	auto has = [&](const char* s) { return weapon.find(s) != std::string::npos; };
	const int hitgroup = now - g_users[userid].hurtTime < 1.0 ? g_users[userid].hitgroup : 0;
	const bool headshot = JBool(r, "headshot");
	Nerve n;
	n.phase = Phase::Waiting;
	n.userid = userid;
	n.pawn = pawn;
	n.pawnIndex = pawnIndex;
	n.deathTime = now;
	int tick = 0;
	g_MirvTime.GetCurrentDemoTick(tick);
	// Its own salt: the same tick and player as Death Animations, but not the same random stream.
	n.seed = Hash((uint32_t)tick * 0x9e3779b9u ^ (uint32_t)(userid + 1) * 0x85ebca6bu ^ 0x4e455256u);
	n.injury = InjuryFor(hitgroup, headshot, has("hegrenade") || has("c4"),
		has("inferno") || has("molotov") || has("incgrenade"), has("knife") || has("bayonet") || has("melee"));
	*slot = n;
	MvmDebugLog_Linef("nervous", "death user=%d weapon=%s hitgroup=%d headshot=%d injury=%s",
		userid, weapon.c_str(), hitgroup, headshot ? 1 : 0, InjuryName(n.injury));
}

void NervousSystem_RunFrame() {
	bool busy = false;
	for (const Nerve& n : g_nerves) busy |= n.phase != Phase::Free;
	if (!g_enabled && !busy) return;
	if (!g_enabled || !DeathAnimations_Enabled()) { DropAll("disabled"); return; }
	int tick = 0;
	if (g_MirvTime.GetCurrentDemoTick(tick)) {
		if (g_lastTick >= 0 && (tick < g_lastTick - 8 || tick > g_lastTick + 256)) DropAll("seek");
		// Our own SetTransform also changes the pose: it is not evidence of another physics step.
		if (tick == g_lastTick) return;
		g_lastTick = tick;
	}
	const double now = g_MirvTime.curtime_get();
	for (Nerve& n : g_nerves) {
		if (n.phase == Phase::Waiting) {
			if (RagdollBodies::EntityAt(n.pawnIndex) != n.pawn) { End(n, "pawn-gone"); continue; }
			if (now < n.deathTime - 0.05) { End(n, "time-reversed"); continue; }
			// A death clip is playing on these bodies: react from where it leaves them.
			if (DeathAnimations_Owns(n.pawnIndex)) {
				if (!n.waitedForClip) MvmDebugLog_Linef("nervous", "wait user=%d for=death-animation", n.userid);
				n.waitedForClip = true;
				n.freeSince = -1;
				if (now - n.deathTime > kClipWait) { ++g_failed; End(n, "clip-too-long"); }
				continue;
			}
			if (n.freeSince < 0) n.freeSince = now;
			char why[512] = {};
			if (Acquire(n.pawn, n.set, why, sizeof(why))) { Begin(n, now); continue; }
			if (now - n.freeSince > kAcquireWindow) {
				++g_failed;
				MvmDebugLog_Linef("nervous", "skip user=%d reason=no-ragdoll-bodies class=%s routes:%s", n.userid, SafeClassName(n.pawn), why);
				n = Nerve{};
			}
		} else if (n.phase == Phase::Reacting) {
			Run(n, now);
		}
	}
}

void NervousSystem_AppendStateJson(std::ostringstream& o) {
	int active = 0;
	for (const Nerve& n : g_nerves) active += n.phase == Phase::Reacting ? 1 : 0;
	o << ",\"nsOn\":" << (g_enabled ? "true" : "false")
	  << ",\"nsStrength\":" << g_strength * 100.0
	  << ",\"nsDuration\":" << g_duration
	  << ",\"nsReach\":" << (g_reach ? "true" : "false")
	  << ",\"nsBreath\":" << (g_breath ? "true" : "false")
	  << ",\"nsWrithe\":" << (g_writhe ? "true" : "false")
	  << ",\"nsTwitch\":" << (g_twitch ? "true" : "false")
	  << ",\"nsBlendIn\":" << g_blendIn
	  << ",\"nsBlendOut\":" << g_blendOut
		  << ",\"nsRollOver\":" << (g_rollOver ? "true" : "false")
	  << ",\"nsActive\":" << active;
}

void NervousSystem_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	// argv: mirv_filmmaker nervous [on|off|toggle|strength <%>|duration <s>|reach|breath|writhe|twitch
	//        [on|off|toggle]|force <injury>|off|reset|status]
	const char* sub = argc >= 3 ? args->ArgV(2) : "status";
	const bool quiet = argc >= 5 && !_stricmp(args->ArgV(4), "quiet");
	auto onOff = [&](bool& flag) {
		const char* v = argc >= 4 ? args->ArgV(3) : "toggle";
		if (!_stricmp(v, "on") || !strcmp(v, "1")) flag = true;
		else if (!_stricmp(v, "off") || !strcmp(v, "0")) flag = false;
		else flag = !flag;
	};
	if (!_stricmp(sub, "on") || !_stricmp(sub, "off") || !_stricmp(sub, "toggle")) {
		NervousSystem_SetEnabled(!_stricmp(sub, "toggle") ? !g_enabled : !_stricmp(sub, "on"));
		advancedfx::Message("mirv_filmmaker nervous: %s.\n", g_enabled ? "on" : "off");
		return;
	}
	if (!_stricmp(sub, "strength") || !_stricmp(sub, "duration") || !_stricmp(sub, "blendin") || !_stricmp(sub, "blendout")) {
		if (argc >= 4) {
			const double v = atof(args->ArgV(3));
			if (!_stricmp(sub, "strength")) NervousSystem_SetStrength(v);
			else if (!_stricmp(sub, "duration")) NervousSystem_SetDuration(v);
			else if (!_stricmp(sub, "blendin")) NervousSystem_SetBlendIn(v);
			else NervousSystem_SetBlendOut(v);
		}
		if (!quiet) advancedfx::Message("mirv_filmmaker nervous: strength %.0f%%, duration %.1f s, blend in %.2f s, blend out %.2f s\n",
			g_strength * 100.0, g_duration, g_blendIn, g_blendOut);
		return;
	}
	bool* flag = !_stricmp(sub, "reach") ? &g_reach : !_stricmp(sub, "breath") ? &g_breath
		: !_stricmp(sub, "writhe") ? &g_writhe : !_stricmp(sub, "twitch") ? &g_twitch
		: !_stricmp(sub, "rollover") ? &g_rollOver : nullptr;
	if (flag) {
		onOff(*flag);
		if (!quiet) advancedfx::Message("mirv_filmmaker nervous %s: %s\n", sub, *flag ? "on" : "off");
		return;
	}
	if (!_stricmp(sub, "kick")) {
		g_kickMode = argc >= 4 ? atoi(args->ArgV(3)) : 0;
		advancedfx::Message("mirv_filmmaker nervous kick: mode %d armed (nervous.kick in mvm_debug)\n", g_kickMode);
		return;
	}
	if (!_stricmp(sub, "force")) {
		const char* v = argc >= 4 ? args->ArgV(3) : "off";
		g_force = Injury::None;
		for (int i = 1; i < (int)Injury::Count; ++i)
			if (!_stricmp(v, InjuryName((Injury)i))) g_force = (Injury)i;
		advancedfx::Message("mirv_filmmaker nervous force: %s\n", g_force == Injury::None ? "off (hit location decides)" : InjuryName(g_force));
		return;
	}
	if (!_stricmp(sub, "reset")) {
		NervousSystem_SetEnabled(false);
		g_strength = 1.0; g_duration = 8.0;
		g_reach = g_breath = g_writhe = g_twitch = true;
		g_blendIn = 0.5; g_blendOut = 1.5; g_rollOver = true;
		g_force = Injury::None;
		advancedfx::Message("mirv_filmmaker nervous: reset (off).\n");
		return;
	}
	if (!_stricmp(sub, "status")) {
		advancedfx::Message("mirv_filmmaker nervous: %s, strength %.0f%%, duration %.1f s, blend in %.2f s, blend out %.2f s, reach %s, breath %s, writhe %s, twitch %s, roll over %s, force %s\n",
			g_enabled ? (DeathAnimations_Enabled() ? "on" : "inactive (Advanced Ragdolls off)") : "off", g_strength * 100.0, g_duration, g_blendIn, g_blendOut, g_reach ? "on" : "off",
			g_breath ? "on" : "off", g_writhe ? "on" : "off", g_twitch ? "on" : "off", g_rollOver ? "on" : "off",
			g_force == Injury::None ? "off" : InjuryName(g_force));
		advancedfx::Message("  deaths: %d reacted, %d limp (headshot), %d without bodies\n", g_reacted, g_limp, g_failed);
		const double now = g_MirvTime.curtime_get();
		for (const Nerve& n : g_nerves) {
			if (n.phase == Phase::Free) continue;
			advancedfx::Message("  user %d: %s %s t=%.1f/%.1f c=%.2f twitches=%d\n", n.userid, PhaseName(n.phase),
				InjuryName(n.injury), n.phase == Phase::Reacting ? now - n.start : 0.0, n.life,
				n.phase == Phase::Reacting ? Consciousness(now - n.start, n.life) : 0.0, n.twitches);
		}
		return;
	}
	advancedfx::Message("Usage: %s nervous on|off|toggle | strength <10-200> | duration <2-20> | blendin <0-2> | blendout <0-4>\n"
		"       | reach|breath|writhe|twitch|rollover [on|off|toggle]\n"
		"       | force head|neck|chest|stomach|leftarm|rightarm|leftleg|rightleg|explosion|fire|melee|off | reset | status\n", cmd);
}

} // namespace Filmmaker
