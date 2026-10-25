#include "DeathAnimations.h"
#include "DeathAnimationsMath.h"
#include "RagdollBodies.h"
#include "RagdollWeaponGround.h"
#include "../Environment/MapCollisionGrid.h"
#include "../Panorama/DeathAnimPreview.h"
#include "../Cosmetics/CosmeticEntityAccess.h"

#include "../Debug/MvmDebugLog.h"
#include "../Platform/JsonParser.h"
#include "../Platform/TextEncoding.h"
#include "../../ClientEntitySystem.h"
#include "../../MirvTime.h"
#include "../../SchemaSystem.h"
#include "../../hlaeFolder.h"
#include "../../../Shared/AfxConsole.h"
#include "../../../Shared/binutils.h"
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/entityhandle.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/igameevents.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/tier1/utlstring.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <functional>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace Filmmaker {
namespace {

using namespace DeathAnimationsMath;
// Engine access to the corpse's bodies is shared with the Nervous System (RagdollBodies.h).
using namespace RagdollBodies;

constexpr int kMaxCorpses = 32;
constexpr int kMaxUsers = 128;
constexpr int kRecent = 8;
// How long a death waits for its ragdoll bodies to exist before it is left to native physics.
constexpr double kAcquireWindow = 0.75;
// Blend in (setting, default 0.1 s): the clip blends in from the pose the player died in, as Call
// of Duty blends a death animation in from the live pose. Several donor deaths start mid-reaction
// (CoD shotgun deaths lean 28-37 degrees at frame 0); applying that frame at once snapped the whole
// body sideways. Strength, blend out and wall reactions: DeathAnimationsMath::PlaybackTuning.

const char* const kSourceKeys[] = { "mw2019", "classic", "extra" };
const char* const kSourceNames[] = { "Modern Warfare 2019", "Classic CoD", "Extra (GMod)" };
const char* const kCategoryNames[] = { "generic", "head", "neck", "torso", "leftarm", "rightarm",
	"leftleg", "rightleg", "explosion", "moving", "fire", "melee", "manual" };
const char* const kWeaponNames[] = { "any", "shotgun", "sniper", "heavy" };
const char* const kSideNames[] = { "any", "front", "back", "left", "right" };
const char* const kStanceNames[] = { "standing", "crouched", "prone", "running" };
const char* const kRelaxNames[] = { "exact", "any-side", "other-side", "fallback-area", "no-clip" };

// ---------------------------------------------------------------------------------------------
// Clip library

struct Clip {
	std::string name;
	ClipTags tags;
	int frames = 0;
	float base[kBodies * 3] = {};
	size_t payload = 0; // first float of this clip in the library payload
	std::vector<ClearanceFrame> clearance; // deterministic correction track, built on first use
	double motionEnd = -1;                 // where the clip's motion stops (MotionEndTime), on first use
	double land = -2;                      // when the clip's body lands (LandTime; -1 = never), on first use
};

struct Library {
	bool attempted = false;
	std::string status = "not loaded";
	double fps = 30.0;
	std::vector<float> payload;
	std::vector<Clip> clips;
	std::vector<ClipTags> tags; // parallel to clips, for DeathAnimationsMath::SelectPool
} g_lib;

// v9 entry: name[32], source, category, frames, payload offset, hit side, stance, weapon,
// frame-0 pose. The source field (DeathAnimationsMath::Source) replaced name-prefix guessing.
constexpr char kMagic[8] = {'M', 'V', 'M', 'D', 'A', 'N', 'I', 'M'};
constexpr uint32_t kVersion = 9;
constexpr size_t kHeaderBytes = 8 + 20;
constexpr size_t kEntryBytes = 32 + 28 + kBodies * 3 * sizeof(float);

bool ParseLibrary(const std::vector<unsigned char>& blob, std::string& error) {
	if (blob.size() < kHeaderBytes || std::memcmp(blob.data(), kMagic, 8)) { error = "not a death animation file"; return false; }
	uint32_t version, count, bodies; float fps;
	std::memcpy(&version, blob.data() + 8, 4);
	std::memcpy(&count, blob.data() + 12, 4);
	std::memcpy(&bodies, blob.data() + 16, 4);
	std::memcpy(&fps, blob.data() + 20, 4);
	if (version != kVersion) { error = "unsupported ragdoll animation version"; return false; }
	if (bodies != (uint32_t)kBodies) { error = "file is not for the 15-body player ragdoll"; return false; }
	if (count > 4096 || !(fps > 1.f && fps < 240.f)) { error = "file header out of range"; return false; }
	const size_t table = kHeaderBytes + count * kEntryBytes;
	if (blob.size() < table || (blob.size() - table) % sizeof(float)) { error = "file is truncated"; return false; }
	std::vector<float>& payload = g_lib.payload;
	payload.resize((blob.size() - table) / sizeof(float));
	if (!payload.empty()) std::memcpy(payload.data(), blob.data() + table, blob.size() - table);
	g_lib.fps = fps;
	for (uint32_t i = 0; i < count; ++i) {
		const unsigned char* e = blob.data() + kHeaderBytes + i * kEntryBytes;
		uint32_t source, cat, frames, offset, side, stance, weapon;
		std::memcpy(&source, e + 32, 4); std::memcpy(&cat, e + 36, 4);
		std::memcpy(&frames, e + 40, 4); std::memcpy(&offset, e + 44, 4);
		std::memcpy(&side, e + 48, 4); std::memcpy(&stance, e + 52, 4);
		std::memcpy(&weapon, e + 56, 4);
		const size_t need = size_t(frames) * kBodies * kFloatsPerBody;
		if (source >= (uint32_t)Source::Count || cat >= (uint32_t)Category::Count
			|| side >= (uint32_t)HitSide::Count || stance >= (uint32_t)Stance::Count
			|| weapon >= (uint32_t)WeaponClass::Count || frames < 2
			|| offset % 4 || offset / 4 > payload.size() || payload.size() - offset / 4 < need) {
			error = "clip table is inconsistent"; return false;
		}
		Clip c;
		char name[33] = {};
		std::memcpy(name, e, 32);
		c.name = name;
		c.tags.source = (Source)source;
		c.tags.category = (Category)cat;
		c.tags.side = (HitSide)side;
		c.tags.stance = (Stance)stance;
		c.tags.weapon = (WeaponClass)weapon;
		c.frames = (int)frames;
		std::memcpy(c.base, e + 60, sizeof(c.base));
		c.payload = offset / 4;
		g_lib.clips.push_back(c);
		g_lib.tags.push_back(c.tags);
	}
	return true;
}

void LoadLibrary() {
	if (g_lib.attempted) return;
	g_lib.attempted = true;
	std::wstring path(GetHlaeFolderW());
	path.append(L"resources\\ragdoll\\ragdoll_animations.mvmanim");
	std::ifstream in(path, std::ios::binary);
	if (!in) { g_lib.status = "ragdoll animation file missing"; }
	else {
		std::vector<unsigned char> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		std::string error;
		if (ParseLibrary(blob, error)) g_lib.status = "ready";
		else g_lib.status = error;
	}
	MvmDebugLog_LinefAlways("deathanim.pack", "loaded file=%s clips=%d fps=%.1f status=%s",
		WideToUtf8(path).c_str(), (int)g_lib.clips.size(), g_lib.fps, g_lib.status.c_str());
}

const float* Frames(const Clip& c) { return g_lib.payload.data() + c.payload; }

int ClipCount(Source s) {
	int n = 0;
	for (const Clip& c : g_lib.clips) if (c.tags.source == s) ++n;
	return n;
}

int FindClip(const char* name) {
	for (size_t i = 0; i < g_lib.clips.size(); ++i)
		if (0 == _stricmp(g_lib.clips[i].name.c_str(), name)) return (int)i;
	return -1;
}

// ---------------------------------------------------------------------------------------------
// Settings

bool g_enabled = false;
Source g_source = Source::Mw2019;
PlaybackTuning g_tune;
// Diagnostics: `deathanim trace <body|-1>` logs one body of every playing corpse each frame
// (deathanim.trace): stepped, actual, target, physics velocity, commanded velocity.
int g_traceBody = -1;
// Diagnostics: `deathanim bodyprobe <frames>` logs every dead player's ragdoll bodies (physics) next
// to the drawn right-hand attachment each frame, playing or paused, death animations on or off
// (deathanim.bodyprobe). Built to answer "does the ragdoll simulate while the demo is paused?".
int g_bodyProbeFrames = 0;
std::string g_forcedClip;        // every death plays this clip (testing)
std::map<std::string, std::string> g_playerClips; // player key -> clip (per-player animations)
std::string g_pendingScript;     // for the Config panel: catalog / preview data
std::vector<std::string> g_queue; // the next deaths play these, in order (testing)
int g_recent[kRecent] = {-1, -1, -1, -1, -1, -1, -1, -1};
int g_recentAt = 0;

bool Recent(int clip) {
	for (int r : g_recent) if (r == clip) return true;
	return false;
}

// Strict pick: stance and source never relax, body area outranks hit side (SelectPool). Among the
// equally good clips, ones not used in the last eight deaths win; the seed makes a seek replay it.
int g_fitTried = 0;       // last pick: candidates probed against the map
double g_fitScore = 0;    // last pick: its penetration (units), see EnvironmentFit

int Pick(Category category, HitSide side, Stance stance, WeaponClass killer, uint32_t seed, Relax& relax,
	WeaponClass& tier, int& poolSize, const std::function<double(int)>& fit) {
	std::vector<int> pool(g_lib.tags.size() + 1);
	const int n = SelectPool(g_lib.tags.data(), (int)g_lib.tags.size(), g_source, category, side, stance,
		killer, pool.data(), (int)pool.size(), relax, tier);
	poolSize = n;
	if (!n) return -1;
	pool.resize(n);
	std::vector<int> fresh, stale;
	for (int i : pool) (Recent(i) ? stale : fresh).push_back(i);
	// Candidates in pick order: the scrambled fresh clips first, then recently played ones.
	std::vector<int> order;
	for (const std::vector<int>* list : {&fresh, &stale}) {
		if (list->empty()) continue;
		const size_t start = Scramble(seed) % list->size();
		for (size_t k = 0; k < list->size(); ++k) order.push_back((*list)[(start + k) % list->size()]);
	}
	g_fitTried = 0; g_fitScore = 0;
	if (!fit) return order.front();
	int best = order.front();
	double bestScore = 1e9;
	for (int ci : order) {
		const double score = fit(ci);
		++g_fitTried;
		if (score < bestScore) { bestScore = score; best = ci; }
		if (score <= kFitTolerance || g_fitTried >= kFitMaxTries) break;
	}
	g_fitScore = bestScore;
	return best;
}

// ---------------------------------------------------------------------------------------------
// Engine access (the body reads and writes themselves live in RagdollBodies.cpp).

bool SafeAttachment(CEntityInstance* entity, const char* name, float outPos[3], float outQ[4]) {
	SOURCESDK::Vector p;
	SOURCESDK::Quaternion q;
	bool ok = false;
	__try {
		const uint8_t index = entity->LookupAttachment(name);
		if (index) ok = entity->GetAttachment(index, p, q);
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	if (!ok) return false;
	outPos[0] = p.x; outPos[1] = p.y; outPos[2] = p.z;
	outQ[0] = q.x; outQ[1] = q.y; outQ[2] = q.z; outQ[3] = q.w;
	return true;
}

// A stable key for a player: the steam id, or the name for bots (steam id 0). Only characters the
// console passes through unchanged, so the picker can send it back as one command argument.
// SEH-guarded reads of a controller; kept free of C++ objects so __try is allowed.
bool ControllerInfo(CEntityInstance* e, int& team, uint64_t& steam, const char*& name) {
	team = 0; steam = 0; name = nullptr;
	if (!e) return false;
	__try {
		if (!e->IsPlayerController()) return false;
		team = e->GetTeam(); steam = e->GetSteamId(); name = e->GetSanitizedPlayerName();
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::string PlayerKey(CEntityInstance* controller) {
	int team = 0;
	uint64_t steam = 0;
	const char* name = nullptr;
	if (!ControllerInfo(controller, team, steam, name)) return std::string();
	char buf[80];
	if (steam) { _snprintf_s(buf, sizeof(buf), _TRUNCATE, "s%llu", (unsigned long long)steam); return buf; }
	std::string key = "n";
	for (const char* p = name ? name : ""; *p && key.size() < 60; ++p)
		key += ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-') ? *p : '_';
	return key.size() > 1 ? key : std::string();
}

// ---------------------------------------------------------------------------------------------
// Events

struct UserState {
	int hitgroup = 0;
	double hurtTime = -1000;
	float origin[3] = {};
	double originTime = -1000;
	double speed = 0;
	bool grounded = true;
	bool ducking = false;       // FL_DUCKING while alive, sampled every frame
	double duckTime = -1000;
};
UserState g_users[kMaxUsers];

bool Contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

// ---------------------------------------------------------------------------------------------
// Corpses

enum class Phase { Free = 0, Waiting, Playing };

struct Corpse {
	Phase phase = Phase::Free;
	int userid = -1;
	int pawnIndex = -1;
	CEntityInstance* pawn = nullptr;
	CEntityInstance* carrier = nullptr;
	int carrierIndex = -1; // BodySet::carrierIndex
	// Where the bodies came from: 0 = the carrier's model-state aggregate, 1 = the pawn's ragdoll
	// control. `aggregate` is that object.
	int route = 0;
	void* aggregate = nullptr;
	void* bodies[kBodies] = {};
	double deathTime = 0;
	double start = 0;
	uint32_t seed = 0;
	DeathFacts facts;
	bool attackerKnown = false;
	float attacker[3] = {};
	float victimOrigin[3] = {};
	WeaponClass killer = WeaponClass::Any;
	char weapon[48] = {};
	char playerKey[80] = {};
	int clip = -1;
	Placement placement;
	Pose from[kBodies];
	float fromVel[kBodies][3] = {}; // each body's velocity at death: the blend in coasts it (CarriedOffset)
	int reasserts = 0;
	int frames = 0;
	// Where each body was placed last frame and with what velocity: after the physics step it should
	// be at placed + velocity * dt, and a body the world pushed away from that is touching something.
	Pose placed[kBodies];
	float placedVel[kBodies][3] = {};
	bool placedValid[kBodies] = {};
	double placedTime = -1;
	ContactState contact;
	double maxPush = 0;         // largest push seen, for the log
	Pose seen[kBodies];         // solid drive: body poses read last frame, to tell when physics stepped
	bool seenValid = false;
	float physLin[kBodies][3] = {}, physAng[kBodies][3] = {}; // solid drive: motion physics produced last step
	double end = 0;             // clip time playback ends at: where the clip's motion stops
	double land = -1;           // clip time the body lands (hand-off to physics), -1 = never
	double peakLinear = 0, peakAngular = 0; // solid drive: largest velocities commanded since the last log line
	int offBalance = 0;         // solid drive: consecutive physics steps the torso tilted away from the clip
	bool flying = false;        // grenade death: thrown by the blast, the clip carried with the body
	// Motion telemetry (handoff log): actual hips/head path against the clip's, travel and height.
	float deathPelvis[3] = {};
	float prevActual[2][3] = {}, prevClip[2][3] = {};
	bool motionValid = false;
	double pathActual[2] = {}, pathClip[2] = {}, peakAbove = 0;
	// Jitter (ReversalCounter): hips, head, hands, forearms, feet -- actual against the clip, sampled on
	// the physics tick.
	ReversalCounter jitterActual[7], jitterClip[7];
	double jitterSample = -1, jitterTime = 0;
	// Rebound: the most the hips rose again after their lowest point so far -- actual body, clip target
	// (with the floor correction), the floor correction's own largest lift, and the blend-in weight and
	// clip time when the actual rebound peaked (is it the blend-in pulling a collapsing body back up?).
	double minActualZ = 1e9, reboundActual = 0, minClipZ = 1e9, reboundClip = 0, maxGroundLift = 0;
	double reboundAtT = 0, reboundAtBlend = 0;
	double fadeStart = -1;      // contact / balance loss: when the crossfade into physics began
	const char* fadeWhy = "";
	std::vector<float> ground;  // exact playback: the clip's vertical floor correction (BuildGroundTrack)
	double blastDir[2] = {};    // exact playback, grenade death: the arc the clip plays along
	double blastOut = 0, blastUp = 0, blastLand = -1;
	double clipPelvisZ0 = 0;    // the clip's frame-0 hip height (grenade flight, see Target)
};
Corpse g_corpses[kMaxCorpses];
int g_lastTick = -1;
int g_played = 0, g_failed = 0;

BodySet SetOf(CEntityInstance* carrier, int route, void* object, void* const bodies[kBodies]) {
	BodySet set;
	set.carrier = carrier; set.route = route; set.object = object;
	std::memcpy(set.bodies, bodies, sizeof(set.bodies));
	return set;
}

// Whether the entity that carries a corpse's bodies still exists, decided WITHOUT touching it: the
// pawn and the carrier (the pawn itself, or its client-side ragdoll) are both still in the entity
// slots they were acquired from. RagdollBodies::Current alone reads the carrier and then calls into
// its aggregate; once the game has removed that ragdoll its freed memory can still read like the
// old one, and the call runs whatever now lives there. That is how the settle watch below
// corrupted CS2's heap (crash at localize.dll from RagdollBodies::ReadBody, 2026-10-24).
bool CarrierAlive(int pawnIndex, CEntityInstance* pawn, CEntityInstance* carrier, int carrierIndex) {
	if (EntityAt(pawnIndex) != pawn) return false;
	return carrier == pawn || (carrierIndex >= 0 && EntityInSlot(carrierIndex) == carrier);
}

bool Current(Corpse& c) {
	return CarrierAlive(c.pawnIndex, c.pawn, c.carrier, c.carrierIndex)
		&& RagdollBodies::Current(SetOf(c.carrier, c.route, c.aggregate, c.bodies));
}

// The whole clip-space pose at `t`, with the limbs kept out of the body (the donor skeletons fold
// arms through a CS2 agent's vest and kit, and legs through each other -- KeepLimbsOutOfBody). The one
// sampler for the corpse and the picker preview. Callers ask for the same (clip, t) body by body, so
// the last pose is kept.
const Pose* SampleClip(int clip, double t) {
	static int s_clip = -1;
	static double s_t = -1;
	static Pose s_pose[kBodies];
	if (clip == s_clip && t == s_t) return s_pose;
	Clip& c = g_lib.clips[clip];
	if (c.clearance.empty()) {
		c.clearance = BuildClearanceTrack(c.base, Frames(c), c.frames, g_lib.fps);
		MvmDebugLog_Linef("deathanim.clearance", "clip=%s samples=%zu fps=%.0f maxSwingSpeed=%.1f",
			c.name.c_str(), c.clearance.size(), kClearanceFps, kClearanceAngularSpeed);
	}
	for (int b = 0; b < kBodies; ++b) Sample(c.base, Frames(c), c.frames, g_lib.fps, t, b, s_pose[b]);
	ApplyClearanceTrack(s_pose, c.clearance, t);
	s_clip = clip;
	s_t = t;
	return s_pose;
}

// The clip's world pose for one body at `t` seconds, blended in from the death pose.
double ClipMotionEnd(int clip) {
	Clip& c = g_lib.clips[clip];
	if (c.motionEnd < 0) {
		const double duration = Duration(c.frames, g_lib.fps);
		c.motionEnd = MotionEndTime(duration, [clip](double t, Pose out[kBodies]) { std::memcpy(out, SampleClip(clip, t), sizeof(Pose) * kBodies); });
		MvmDebugLog_Linef("deathanim.end", "clip=%s duration=%.2f motionEnds=%.2f trimmed=%.2f", c.name.c_str(), duration, c.motionEnd, duration - c.motionEnd);
	}
	return c.motionEnd;
}

double ClipLandTime(int clip, double end) {
	Clip& c = g_lib.clips[clip];
	if (c.land < -1.5) {
		c.land = LandTime(end, [clip](double t, Pose out[kBodies]) { std::memcpy(out, SampleClip(clip, t), sizeof(Pose) * kBodies); });
		MvmDebugLog_Linef("deathanim.end", "clip=%s lands=%.2f end=%.2f", c.name.c_str(), c.land, end);
	}
	return c.land;
}

// Probe one candidate clip against the map before it is picked (see kFitSamples in the math header):
// the floor under key bodies at a few moments, and a sideways ray along where the hips and head go.
// Returns the deepest penetration (units) the ground track would not fix. Raw clip samples, no
// clearance track -- this runs for several candidates in one frame and must stay cheap.
double EnvironmentFit(int clip, const Placement& pl, double floorZ, const float startPelvis[3]) {
	const Clip& c = g_lib.clips[clip];
	const double end = Duration(c.frames, g_lib.fps);
	static constexpr int kProbe[] = {kPelvis, kHeadBody, 11, 12, 1, 3, 13, 14};
	const float down[3] = {0, 0, -1};
	double worst = 0;
	for (int k = 1; k <= kFitSamples; ++k) {
		const double t = end * k / kFitSamples;
		for (int b : kProbe) {
			Pose clipPose, w;
			Sample(c.base, Frames(c), c.frames, g_lib.fps, t, b, clipPose);
			Place(pl, clipPose, w);
			const float from[3] = {w.p[0], w.p[1], float(floorZ + 48.0)};
			float hit[3], normal[3];
			if (RagdollWeaponGround_Ray(from, down, 96.f, hit, normal)) {
				const double pen = FloorPenetration(clipPose.p[2], kGroundRadius[b], hit[2] - floorZ);
				if (pen > worst) worst = pen;
			}
			if (b != kPelvis && b != kHeadBody) continue;
			// Walls: sideways from where the hips were at death toward where this body is now.
			double d[2] = {double(w.p[0]) - startPelvis[0], double(w.p[1]) - startPelvis[1]};
			const double dist = std::sqrt(d[0] * d[0] + d[1] * d[1]);
			if (dist < 4.0) continue;
			const float dir[3] = {float(d[0] / dist), float(d[1] / dist), 0.f};
			const float origin[3] = {startPelvis[0], startPelvis[1], startPelvis[2]};
			const float reach = float(dist + kGroundRadius[b]);
			if (RagdollWeaponGround_Ray(origin, dir, reach, hit, normal)) {
				const double hx = hit[0] - origin[0], hy = hit[1] - origin[1];
				const double pen = reach - std::sqrt(hx * hx + hy * hy);
				if (pen > worst) worst = pen;
			}
		}
	}
	return worst;
}

void Target(const Corpse& c, double t, int body, Pose& out) {
	Pose placed;
	Place(c.placement, SampleClip(c.clip, t)[body], placed);
	// During a grenade flight the floor lift fades in by touchdown: it was computed for the clip's crouch,
	// which the flight replaces with the standing height (below), and added on top it overshot the arc.
	const double flight = c.blastLand > 0 && t < c.blastLand ? t / c.blastLand : 1.0;
	placed.p[2] += float(GroundOffsetAt(c.ground, t) * flight);
	// The blend in leaves from the death pose still moving the way the body was moving (CarriedOffset);
	// a grenade death has its own launch instead.
	Pose source = c.from[body];
	if (!(c.blastLand > 0)) {
		double carry[3];
		CarriedOffset(c.fromVel[body], t, carry);
		for (int k = 0; k < 3; ++k) source.p[k] = float(source.p[k] + carry[k]);
	}
	Mix(source, placed, BlendIn(t, g_tune.blendIn), out);
	// The blast flight moves the WHOLE blended body, so it leaves from frame 0: added before the
	// blend in, the body lagged behind the flying pose and then snapped along it.
	if (c.blastLand > 0) {
		double off[3];
		BlastOffset(c.blastDir, c.blastOut, c.blastUp, t, off);
		// Early in the flight the hips ride the arc from their STANDING height, not the clip's crouch:
		// the explosion clips drop the hips ~22 units in 0.2 s, faster than the arc lifts, so the body
		// sank and was then thrown up -- "falling, then brought back up" (2026-09-26). The clip's own
		// height is handed back linearly by touchdown, so the corpse lands on the clip's lying pose.
		const double u = t < c.blastLand ? t / c.blastLand : 1.0;
		const float* clip = &SampleClip(c.clip, t)[kPelvis].p[0];
		off[2] += (c.clipPelvisZ0 - clip[2]) * (1.0 - u) * BlendIn(t, g_tune.blendIn);
		for (int k = 0; k < 3; ++k) out.p[k] += float(off[k]);
	}
}

// Stop driving without touching the bodies: physics already owns them (the clip faded out, or the
// body ran into something and the solver's response is the motion to keep).
void WatchSettle(const Corpse& c, const char* why);
void LogMotion(const Corpse& c, const char* why) {
	if (c.clip < 0 || !c.motionValid) return;
	const double dx = c.prevActual[0][0] - c.deathPelvis[0], dy = c.prevActual[0][1] - c.deathPelvis[1];
	int ja = 0, jc = 0;
	char per[64]; int n = 0;
	for (int i = 0; i < 7; ++i) {
		ja += c.jitterActual[i].reversals; jc += c.jitterClip[i].reversals;
		n += snprintf(per + n, sizeof(per) - n, "%s%d", i ? "," : "", c.jitterActual[i].reversals);
	}
	const double secs = c.jitterTime > 1e-3 ? c.jitterTime : 1e-3;
	MvmDebugLog_Linef("deathanim.motion",
		"user=%d clip=%s why=%s t=%.3f flying=%d travel=%.1f peakAbove=%.1f hipsPath=%.1f/%.1f headPath=%.1f/%.1f jitter=%.1f/%.1f perBody=%s",
		c.userid, g_lib.clips[c.clip].name.c_str(), why, g_MirvTime.curtime_get() - c.start, c.flying ? 1 : 0,
		std::sqrt(dx * dx + dy * dy), c.peakAbove, c.pathActual[0], c.pathClip[0], c.pathActual[1], c.pathClip[1],
		ja / secs, jc / secs, per);
	MvmDebugLog_Linef("deathanim.rebound", "user=%d clip=%s rebound=%.1f/%.1f groundLift=%.1f atT=%.2f blendIn=%.2f solid=%d",
		c.userid, g_lib.clips[c.clip].name.c_str(), c.reboundActual, c.reboundClip, c.maxGroundLift, c.reboundAtT,
		c.reboundAtBlend, g_tune.solid ? 1 : 0);
}

void Handoff(Corpse& c, const char* why) {
	LogMotion(c, why);
	// A body thrown by a blast lets go without its leftover sideways speed, or the ragdoll glides on.
	if (c.flying && Current(c))
		for (int i = 0; i < kBodies; ++i) DampHorizontal(c.bodies[i], float(kHandoffSlide));
	WatchSettle(c, why);
	if (c.clip >= 0)
		MvmDebugLog_Linef("deathanim", "handoff user=%d clip=%s why=%s t=%.3f frames=%d maxPush=%.1f limp=%d%d%d%d stopBody=%d stopPush=%.1f",
			c.userid, g_lib.clips[c.clip].name.c_str(), why, g_MirvTime.curtime_get() - c.start, c.frames, c.maxPush,
			c.contact.limp[0] ? 1 : 0, c.contact.limp[1] ? 1 : 0, c.contact.limp[2] ? 1 : 0, c.contact.limp[3] ? 1 : 0,
			c.contact.stopBody, c.contact.stopPush);
	c = Corpse{};
}

// ---------------------------------------------------------------------------------------------
// Settle watch (diagnostics, mvm_debug only): once a clip hands its body to physics, follow it for
// kSettleSeconds and log how far each body dropped and how high it rests above the floor. A clip
// whose final pose floats shows up as a drop right after the hand-off.
constexpr double kSettleSeconds = 2.5;
struct Settle {
	bool active = false;
	int pawnIndex = -1;
	CEntityInstance* pawn = nullptr;
	CEntityInstance* carrier = nullptr;
	int carrierIndex = -1;
	int route = 0;
	void* aggregate = nullptr;
	void* bodies[kBodies] = {};
	int clip = -1;
	double start = 0;
	float z0[kBodies] = {};
	float pelvisXY[2] = {};  // where the hips were at the hand-off, for the slide distance
	char why[24] = {};
	// Followed every frame: how much the corpse still moves after the clip let go.
	float last[3] = {};
	double lastTime = -1;
	double path = 0;         // hips path length since the hand-off
	double lateSpeed = 0;    // fastest the hips moved more than 0.5 s after the hand-off
};
Settle g_settles[kMaxCorpses];

void WatchSettle(const Corpse& c, const char* why) {
	if (!MvmDebugLog_Active() || c.clip < 0) return;
	for (Settle& s : g_settles) {
		if (s.active) continue;
		s = Settle{};
		s.pawnIndex = c.pawnIndex; s.pawn = c.pawn; s.carrier = c.carrier; s.carrierIndex = c.carrierIndex;
		s.route = c.route; s.aggregate = c.aggregate;
		std::memcpy(s.bodies, c.bodies, sizeof(s.bodies));
		s.clip = c.clip;
		s.start = g_MirvTime.curtime_get();
		_snprintf_s(s.why, sizeof(s.why), _TRUNCATE, "%s", why);
		for (int i = 0; i < kBodies; ++i) { Pose p; int m = 0; if (!ReadBody(s.bodies[i], p, m)) return; s.z0[i] = p.p[2]; if (i == kPelvis) { s.pelvisXY[0] = p.p[0]; s.pelvisXY[1] = p.p[1]; std::memcpy(s.last, p.p, sizeof(s.last)); } }
		s.lastTime = s.start;
		s.active = true;
		return;
	}
}

void PumpSettles(double now) {
	for (Settle& s : g_settles) {
		if (!s.active) continue;
		if (now < s.start - 0.05) { s.active = false; continue; }
		// Every frame, before any body is read: the watch outlives the clip, and the game may remove
		// the ragdoll at any point of it (see CarrierAlive).
		if (!CarrierAlive(s.pawnIndex, s.pawn, s.carrier, s.carrierIndex)
			|| !RagdollBodies::Current(SetOf(s.carrier, s.route, s.aggregate, s.bodies))) {
			s.active = false;
			MvmDebugLog_LinefAlways("deathanim.settle", "clip=%s why=%s lost=corpse after=%.2f path=%.1f lateSpeed=%.0f",
				g_lib.clips[s.clip].name.c_str(), s.why, now - s.start, s.path, s.lateSpeed);
			continue;
		}
		if (now - s.start < kSettleSeconds) {
			Pose p; int m = 0;
			if (now > s.lastTime + 1e-4 && ReadBody(s.bodies[kPelvis], p, m)) {
				double d2 = 0;
				for (int k = 0; k < 3; ++k) d2 += (p.p[k] - s.last[k]) * (p.p[k] - s.last[k]);
				const double d = std::sqrt(d2), dt = now - s.lastTime;
				s.path += d;
				if (now - s.start > 0.5 && d / dt > s.lateSpeed) s.lateSpeed = d / dt;
				std::memcpy(s.last, p.p, sizeof(s.last));
				s.lastTime = now;
			}
			continue;
		}
		s.active = false;
		Pose rest[kBodies];
		bool ok = true;
		for (int i = 0; i < kBodies && ok; ++i) { int m = 0; ok = ReadBody(s.bodies[i], rest[i], m); }
		if (!ok) {
			MvmDebugLog_LinefAlways("deathanim.settle", "clip=%s why=%s lost=bodies path=%.1f lateSpeed=%.0f",
				g_lib.clips[s.clip].name.c_str(), s.why, s.path, s.lateSpeed);
			continue;
		}
		float from[3] = {rest[kPelvis].p[0], rest[kPelvis].p[1], rest[kPelvis].p[2] + 20.f};
		const float down[3] = {0, 0, -1};
		float hit[3] = {}, normal[3] = {};
		const bool floor = RagdollWeaponGround_Ray(from, down, 80.f, hit, normal);
		char heights[256] = {};
		size_t used = 0;
		for (int i = 0; i < kBodies; ++i)
			used += (size_t)_snprintf_s(heights + used, sizeof(heights) - used, _TRUNCATE, "%s%.1f", i ? "," : "", floor ? rest[i].p[2] - hit[2] : -99.f);
		double drop = 0;
		for (int i = 0; i < kBodies; ++i) drop = (std::max)(drop, double(s.z0[i] - rest[i].p[2]));
		MvmDebugLog_LinefAlways("deathanim.settle", "clip=%s why=%s drop=%.1f pelvisDrop=%.1f path=%.1f lateSpeed=%.0f slide=%.1f floor=%d restHeights=[%s]",
			g_lib.clips[s.clip].name.c_str(), s.why, drop, s.z0[kPelvis] - rest[kPelvis].p[2], s.path, s.lateSpeed,
			std::sqrt((rest[kPelvis].p[0] - s.pelvisXY[0]) * (rest[kPelvis].p[0] - s.pelvisXY[0]) + (rest[kPelvis].p[1] - s.pelvisXY[1]) * (rest[kPelvis].p[1] - s.pelvisXY[1])),
			floor ? 1 : 0, heights);
	}
}

void Release(Corpse& c, const char* why) {
	LogMotion(c, why);
	if (c.phase == Phase::Playing && c.clip >= 0 && Current(c)) {
		const Clip& clip = g_lib.clips[c.clip];
		const double end = c.end > 0 ? c.end : Duration(clip.frames, g_lib.fps);
		const double now = g_MirvTime.curtime_get();
		const double t = now - c.start < end ? now - c.start : end;
		const double h = 1.0 / g_lib.fps;
		int ok = 0;
		for (int i = 0; i < kBodies; ++i) {
			Pose before, after;
			Target(c, t - h > 0 ? t - h : 0, i, before);
			Target(c, t, i, after);
			float linear[3], angular[3];
			if (!ReleaseVelocity(before, after, t > h ? h : (t > 1e-4 ? t : h), linear, angular)) {
				linear[0] = linear[1] = linear[2] = 0; angular[0] = angular[1] = angular[2] = 0;
			}
			ClampRelease(linear, angular);
			ok += ReleaseBody(c.bodies[i], after, linear, angular) ? 1 : 0;
		}
		WatchSettle(c, why);
		MvmDebugLog_Linef("deathanim", "release user=%d clip=%s why=%s t=%.3f end=%.3f bodies=%d frames=%d reasserts=%d",
			c.userid, clip.name.c_str(), why, t, end, ok, c.frames, c.reasserts);
	}
	c = Corpse{};
}

void ReleaseAll(const char* why) {
	for (Corpse& c : g_corpses) if (c.phase != Phase::Free) Release(c, why);
}

// A seek or a time reversal: let go WITHOUT touching a single body. The engine has just rebuilt the
// world, so a corpse's ragdoll may be freed or replaced -- writing a release pose into it corrupted the
// client's physics memory and crashed CS2 a moment later (client.dll+0xafe192, read of a garbage
// pointer, twice at a seek-back, 2026-09-26). The settle watches follow the same bodies; drop them too.
void DropAll(const char* why) {
	for (Corpse& c : g_corpses) {
		if (c.phase == Phase::Free) continue;
		MvmDebugLog_Linef("deathanim", "drop user=%d clip=%s why=%s", c.userid,
			c.clip >= 0 ? g_lib.clips[c.clip].name.c_str() : "-", why);
		c = Corpse{};
	}
	for (Settle& st : g_settles) st.active = false;
}

// Find the victim's fifteen ragdoll bodies: on the pawn's own model-state aggregate, on its
// client-side ragdoll child's, or through the pawn's ragdoll control. `why` describes every route
// tried, for the log when none works.
bool Acquire(Corpse& c, char* why, size_t whySize) {
	BodySet set;
	if (!RagdollBodies::Acquire(c.pawn, set, why, whySize)) return false;
	c.carrier = set.carrier;
	c.carrierIndex = set.carrierIndex;
	c.route = set.route;
	c.aggregate = set.object;
	std::memcpy(c.bodies, set.bodies, sizeof(c.bodies));
	return true;
}

void Begin(Corpse& c, double now) {
	float death[kBodies * 3];
	for (int i = 0; i < kBodies; ++i) {
		int motion = 0;
		if (!ReadBody(c.bodies[i], c.from[i], motion)) { ++g_failed; c = Corpse{}; return; }
		float ang[3];
		if (!ReadVelocity(c.bodies[i], c.fromVel[i], ang)) for (int k = 0; k < 3; ++k) c.fromVel[i][k] = 0.f;
		for (int k = 0; k < 3; ++k) death[i * 3 + k] = c.from[i].p[k];
	}
	const float* pelvis = death + kPelvis * 3;
	// Where the victim's feet stand, taken from the death pose itself: the lowest ankle. The clip's
	// floor goes wherever puts the clip's own frame-0 ankle there. The map ray is only a
	// cross-check -- it can pass through a floor it has no geometry for and land on the level
	// below, which planted bodies 25-35 units into the ground.
	const double deathAnkle = death[13 * 3 + 2] < death[14 * 3 + 2] ? death[13 * 3 + 2] : death[14 * 3 + 2];
	double rayZ = 0;
	bool rayHit = false;
	{
		const float down[3] = {0, 0, -1};
		float hit[3], normal[3];
		if (RagdollWeaponGround_Ray(pelvis, down, 120.f, hit, normal)) { rayZ = hit[2]; rayHit = true; }
	}
	HitSide side = HitSide::Any;
	double facing[2];
	if (c.attackerKnown && FacingFromHips(death + kThighLeft * 3, death + kThighRight * 3, facing)) {
		const double toSource[2] = {double(c.attacker[0]) - pelvis[0], double(c.attacker[1]) - pelvis[1]};
		side = HitSideFrom(facing, toSource);
	}
	const Category category = CategoryFor(c.facts);
	const UserState& u = g_users[c.userid];
	const bool duckKnown = now - u.duckTime < 0.5 && now >= u.duckTime - 0.05;
	// A grounded clip must never establish an imaginary floor at an airborne victim's feet.
	// Native physics owns these deaths, including exact playback and the jump apex.
	if (duckKnown && !u.grounded && !c.facts.explosive) {
		MvmDebugLog_Linef("deathanim", "skip user=%d reason=airborne flagsOffset=0x%llx pawn=%p pelvisZ=%.1f ankleZ=%.1f",
			c.userid, (unsigned long long)g_clientDllOffsets.C_BaseEntity.m_fFlags, c.pawn, pelvis[2], deathAnkle);
		c = Corpse{};
		return;
	}
	const double pelvisAboveAnkle = pelvis[2] - deathAnkle;
	const Stance stance = VictimStance(u.ducking, duckKnown, pelvisAboveAnkle, c.facts.planarSpeed, kRunSpeed);
	int clip = -1, pool = 0;
	Relax relax = Relax::Exact;
	WeaponClass tier = WeaponClass::Any;
	const char* chosenBy = "pick";
	auto assigned = g_playerClips.find(c.playerKey);
	if (!g_queue.empty()) { clip = FindClip(g_queue.front().c_str()); g_queue.erase(g_queue.begin()); chosenBy = "queue"; }
	else if (assigned != g_playerClips.end()) { clip = FindClip(assigned->second.c_str()); chosenBy = "player"; }
	else if (!g_forcedClip.empty()) { clip = FindClip(g_forcedClip.c_str()); chosenBy = "forced"; }
	if (clip < 0) {
		// Each candidate is fitted to the map first (EnvironmentFit): the first that neither sinks
		// into a step nor swings through a wall plays.
		auto fit = [&](int ci) -> double {
			const Clip& cl = g_lib.clips[ci];
			const double ca = cl.base[13 * 3 + 2] < cl.base[14 * 3 + 2] ? cl.base[13 * 3 + 2] : cl.base[14 * 3 + 2];
			double fz = deathAnkle - ca;
			if (rayHit && std::fabs(rayZ - fz) <= 6.0) fz = rayZ;
			const Placement pl = SolvePlacement(cl.base, death, fz);
			return pl.solved ? EnvironmentFit(ci, pl, fz, pelvis) : 1e9;
		};
		clip = Pick(category, side, stance, c.killer, c.seed, relax, tier, pool, fit);
		chosenBy = "pick";
	}
	if (clip < 0) {
		MvmDebugLog_Linef("deathanim", "skip user=%d reason=no-clip source=%s stance=%s(duck %d/%d pelvisAboveAnkle %.1f) category=%s side=%s status=%s",
			c.userid, kSourceKeys[(int)g_source], kStanceNames[(int)stance], u.ducking ? 1 : 0, duckKnown ? 1 : 0,
			pelvisAboveAnkle, kCategoryNames[(int)category], kSideNames[(int)side], g_lib.status.c_str());
		c = Corpse{};
		return;
	}
	const Clip& chosen = g_lib.clips[clip];
	const double clipAnkle = chosen.base[13 * 3 + 2] < chosen.base[14 * 3 + 2] ? chosen.base[13 * 3 + 2] : chosen.base[14 * 3 + 2];
	double floorZ = deathAnkle - clipAnkle;
	if (rayHit && std::fabs(rayZ - floorZ) <= 6.0) floorZ = rayZ;
	c.placement = SolvePlacement(chosen.base, death, floorZ);
	if (!c.placement.solved) { ++g_failed; c = Corpse{}; return; }
	c.clip = clip;
	c.start = now;
	c.end = ClipMotionEnd(clip);
	c.land = ClipLandTime(clip, c.end);
	// The landing is known ahead: playback ends there, so the blend out crossfades the body into the
	// ragdoll over the last moments of the fall instead of cutting at the touchdown. Not for a body
	// thrown by a blast in solid playback: the clip's landing is not where the BODY lands -- physics
	// flies it, and touching the ground (contact) is what ends the clip.
	if (g_tune.solid && g_tune.impact > 0 && c.facts.explosive) c.land = -1; // c.flying, set just below
	if (c.land > 0 && c.land < c.end) c.end = c.land;
	c.phase = Phase::Playing;
	std::memcpy(c.deathPelvis, pelvis, sizeof(c.deathPelvis));
	// A shot arm or leg is limp from the start: the rest of the animation plays, that limb is physics.
	if (const int limb = ShotLimb(c.facts.hitgroup); limb >= 0) { c.contact.limp[limb] = true; c.contact.limbGive[limb] = 1.0; }
	// Grenade blast (solid playback only -- exact playback would place the push away next frame): the
	// body is thrown outward and up, and the clip plays while it flies (root drift, see Drive).
	c.flying = g_tune.solid && g_tune.impact > 0 && c.facts.explosive;
	const bool setLaunch = g_tune.solid && c.facts.explosive;
	// Exact playback: the floor correction track, and for a grenade the arc the clip plays along.
	c.ground = BuildGroundTrack(c.end, [clipIndex = clip](double t, Pose out[kBodies]) { std::memcpy(out, SampleClip(clipIndex, t), sizeof(Pose) * kBodies); });
	if (!g_tune.solid && g_tune.impact > 0 && c.facts.explosive) {
		double d[2] = {0, 0};
		if (c.attackerKnown) { d[0] = pelvis[0] - c.attacker[0]; d[1] = pelvis[1] - c.attacker[1]; }
		double dn = std::sqrt(d[0] * d[0] + d[1] * d[1]), hips[2];
		if (dn < 1e-3 && FacingFromHips(death + kThighLeft * 3, death + kThighRight * 3, hips)) { d[0] = -hips[0]; d[1] = -hips[1]; dn = 1; }
		if (dn > 1e-3) {
			c.blastDir[0] = d[0] / dn; c.blastDir[1] = d[1] / dn;
			c.blastOut = kBlastOut * BlastScale(g_tune.impact); c.blastUp = kBlastUp * BlastScale(g_tune.impact);
			c.blastLand = BlastLandTime(c.blastUp);
			c.clipPelvisZ0 = SampleClip(clip, 0.0)[kPelvis].p[2];
			MvmDebugLog_Linef("deathanim", "blast user=%d out=%.0f up=%.0f lands=%.2f", c.userid, c.blastOut, c.blastUp, c.blastLand);
		}
	}
	// The Grenade blast slider SETS the launch rather than adding to CS2's own (~300 u/s): topped up,
	// every value above 0 still flew the full engine launch -- 25 % travelled 105-192 units -- and at 0
	// the muscles fought that launch, so the body moved ~40 % more than the clip (read as sped up).
	// 0 now plays the clip in place. Angular spin is kept but capped (kLaunchSpin).
	if (setLaunch) {
		// Direction: away from the blast. CS2's own launch already points that way (the explosion's
		// physics push), and grenade deaths carry no usable attacker position (attacker=0): the old
		// "backwards from the victim's facing" fallback threw three victims of one grenade in three
		// unrelated directions. Only its DIRECTION is kept; the strength is the slider's.
		float before[3] = {}, spin[3] = {};
		ReadVelocity(c.bodies[kPelvis], before, spin);
		double dir[2] = {before[0], before[1]};
		double n = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
		if (n < kBlastDirMinSpeed && c.attackerKnown) { dir[0] = pelvis[0] - c.attacker[0]; dir[1] = pelvis[1] - c.attacker[1]; n = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]); }
		double hips[2];
		if (n < 1e-3 && FacingFromHips(death + kThighLeft * 3, death + kThighRight * 3, hips)) { dir[0] = -hips[0]; dir[1] = -hips[1]; n = 1; }
		if (n > 1e-3) {
			const double speed = ImpactSpeed(c.facts) * BlastScale(g_tune.impact);
			const double lift = ImpactLift(c.facts) * BlastScale(g_tune.impact);
			for (int i = 0; i < kBodies; ++i) {
				float launch[3] = {float(dir[0] / n * speed), float(dir[1] / n * speed), float(lift)}, ang[3] = {}, ignored[3];
				ReadVelocity(c.bodies[i], ignored, ang);
				ClampRelease(launch, ang, kDriveMaxLinear, kLaunchSpin);
				bool reasserted = false;
				DriveBody(c.bodies[i], launch, ang, reasserted);
			}
			MvmDebugLog_Linef("deathanim", "impact user=%d launch=%.0f lift=%.0f dir=(%.2f %.2f) engineVelocity=(%.0f %.0f %.0f) replaced",
				c.userid, speed, lift, dir[0] / n, dir[1] / n, before[0], before[1], before[2]);
		}
	}
	g_recent[g_recentAt++ % kRecent] = clip;
	++g_played;
	MvmDebugLog_Linef("deathanim",
		"play user=%d clip=%s by=%s source=%s weapon=%s(%s, clip %s tier %s) category=%s(clip %s) side=%s(clip %s) stance=%s(clip %s, duck %d/%d, pelvisAboveAnkle %.1f) match=%s pool=%d fit=%.1f(tried %d) hitgroup=%d headshot=%d speed=%.0f "
		"yaw=%.1f floor=%.1f(ankle %.1f ray %s%.1f) pelvis=(%.1f %.1f %.1f) duration=%.2f carrier=%s aggregate=%p",
		c.userid, chosen.name.c_str(), chosenBy, kSourceKeys[(int)g_source], c.weapon, kWeaponNames[(int)c.killer],
		kWeaponNames[(int)chosen.tags.weapon], kWeaponNames[(int)tier], kCategoryNames[(int)category],
		kCategoryNames[(int)chosen.tags.category], kSideNames[(int)side], kSideNames[(int)chosen.tags.side],
		kStanceNames[(int)stance], kStanceNames[(int)chosen.tags.stance], u.ducking ? 1 : 0, duckKnown ? 1 : 0,
		pelvisAboveAnkle, kRelaxNames[(int)relax], pool, g_fitScore, g_fitTried, c.facts.hitgroup,
		c.facts.headshot ? 1 : 0, c.facts.planarSpeed, Yaw(c.placement) * 57.29578, floorZ, deathAnkle, rayHit ? "" : "miss ", rayZ,
		pelvis[0], pelvis[1], pelvis[2], Duration(chosen.frames, g_lib.fps),
		c.carrier == c.pawn ? (c.route ? "pawn-control" : "pawn") : (c.route ? "child-control" : "child"), c.aggregate);
}

void Drive(Corpse& c, double now) {
	if (!Current(c)) {
		MvmDebugLog_Linef("deathanim", "lost user=%d clip=%s frames=%d", c.userid,
			c.clip >= 0 ? g_lib.clips[c.clip].name.c_str() : "-", c.frames);
		c = Corpse{};
		return;
	}
	const double t = now - c.start;
	if (t < -0.05) { MvmDebugLog_Linef("deathanim", "drop user=%d why=time-reversed", c.userid); c = Corpse{}; return; }
	const Clip& clip = g_lib.clips[c.clip];
	// Playback ends where the clip's motion does (MotionEndTime), not after its still tail.
	const double duration = c.end;
	// With a blend out the bodies are already physics' by the last frame: just stop. Without one,
	// the old hand-off: the final clip pose, carrying the clip's motion.
	const char* endWhy = c.land > 0 && std::fabs(duration - c.land) < 1e-6 ? "landed" : "clip-end";
	if (t >= duration) { if (g_tune.blendOut > 0) Handoff(c, endWhy); else Release(c, endWhy); return; }
	// A crossfade started by contact or balance loss has finished.
	if (c.fadeStart >= 0 && t >= c.fadeStart + g_tune.blendOut) { Handoff(c, c.fadeWhy); return; }
	// A grenade arc back on the ground: physics takes the landing (the bodies carry the arc's motion).
	if (c.blastLand > 0 && t >= c.blastLand) { Release(c, "blast-landed"); return; }

	// 1. Where physics left each body against where it was placed (see Corpse::placed).
	Pose actual[kBodies];
	bool haveActual = true;
	for (int i = 0; i < kBodies && haveActual; ++i) { int motion = 0; haveActual = ReadBody(c.bodies[i], actual[i], motion); }
	const double dt = now - c.placedTime;
	// Solid drive: did physics step since last frame? Render frames outnumber physics steps and a body
	// only moves on a step, so only a frame after a step says anything about contact.
	bool stepped = true;
	if (g_tune.solid && haveActual) {
		stepped = !c.seenValid;
		for (int i = 0; i < kBodies && !stepped; ++i)
			stepped = std::memcmp(actual[i].p, c.seen[i].p, sizeof(actual[i].p)) != 0 || std::memcmp(actual[i].q, c.seen[i].q, sizeof(actual[i].q)) != 0;
		// The motion physics gave each body, for the muscles (MuscleVelocity). Read from the
		// engine: a pose difference is fooled when poses update between ticks.
		for (int i = 0; i < kBodies; ++i)
			if (!ReadVelocity(c.bodies[i], c.physLin[i], c.physAng[i]))
				for (int k = 0; k < 3; ++k) { c.physLin[i][k] = 0.f; c.physAng[i][k] = 0.f; }
		std::memcpy(c.seen, actual, sizeof(c.seen));
		c.seenValid = true;
	}
	double push[kBodies] = {};
	// Measured (2026-09-25, seven open-space deaths): the whole body drifts 1-2.5 units from the
	// prediction together -- frame timing against the physics step, not contact. Limbs are therefore
	// judged RELATIVE to the pelvis, and so is the head (that shared drift cancels); the torso keeps
	// its absolute push against a higher threshold. Contact is not judged during the blend in or the
	// blend out, where the body is moving between physics and the clip and drifts from it by design.
	const bool judge = t > g_tune.blendIn && BlendOutWeight(t, duration, g_tune.blendOut) >= 0.999 && c.fadeStart < 0;
	if (haveActual && stepped && c.placedTime >= 0 && dt > 1e-4 && dt < 0.25) {
		double vec[kBodies][3] = {};
		for (int i = 0; i < kBodies; ++i) {
			if (!c.placedValid[i]) continue;
			for (int k = 0; k < 3; ++k) vec[i][k] = actual[i].p[k] - (c.placed[i].p[k] + c.placedVel[i][k] * dt);
		}
		for (int i = 0; i < kBodies; ++i) {
			if (!c.placedValid[i]) continue;
			// The head rides the neck joint like a limb: judged relative to the pelvis too.
			const bool limb = (LimbOf(i) >= 0 || i == 4) && c.placedValid[kPelvis];
			double d2 = 0;
			for (int k = 0; k < 3; ++k) {
				const double v = limb ? vec[i][k] - vec[kPelvis][k] : vec[i][k];
				d2 += v * v;
			}
			push[i] = std::sqrt(d2);
			if (judge && push[i] > c.maxPush) c.maxPush = push[i];
		}
		if (g_tune.reactToWalls && judge) UpdateContact(c.contact, push, g_tune.solid ? SolidContactRules() : ContactRules());
	}
	// Contact (and balance loss below) start the same crossfade the blend out gives the end of a clip,
	// from this moment: an instant cut is what made every hand-off look abrupt.
	auto beginFade = [&](const char* why) -> bool {
		if (!(g_tune.blendOut > 0)) { Handoff(c, why); return true; }
		if (c.fadeStart < 0) {
			c.fadeStart = t; c.fadeWhy = why;
			MvmDebugLog_Linef("deathanim", "fade user=%d clip=%s why=%s t=%.3f over=%.2f", c.userid, clip.name.c_str(), why, t, g_tune.blendOut);
		}
		return false;
	};
	if (c.contact.stop && beginFade("contact")) return;
	// Thrown by a blast: the clip drives the body only in the AIR. Once the hips come down to the
	// floor the crossfade starts and the ragdoll takes the landing -- clips with leg motion kept
	// pushing the legs against the floor and walked the landed body around (sweep 2026-09-26: MW2019
	// hips moved 2-4x as far as the clip did).
	if (c.flying && haveActual && t > kPhysicsStep * 4 && actual[kPelvis].p[2] - c.placement.offset[2] < kGroundContactHeight
		&& c.physLin[kPelvis][2] <= 0.f && beginFade("landed")) return;
	// Lost balance: physics has tipped the torso well away from where the clip holds it (a push, a
	// blast, a fall the clip does not have). Steering it back is what made bodies pop back up and
	// fight to stand; the clip lets go instead (DeathAnimationsMath::TorsoTilt).
	if (g_tune.solid && haveActual && stepped && judge) {
		Pose pelvisTarget, headTarget;
		Target(c, t > 0 ? t : 0, kPelvis, pelvisTarget);
		Target(c, t > 0 ? t : 0, kHeadBody, headTarget);
		const double tilt = TorsoTilt(actual[kPelvis].p, actual[kHeadBody].p, pelvisTarget.p, headTarget.p);
		c.offBalance = tilt > kBalanceLossDegrees ? c.offBalance + 1 : 0;
		if (c.offBalance >= 2) {
			MvmDebugLog_Linef("deathanim", "balance user=%d clip=%s t=%.3f tilt=%.1f", c.userid, clip.name.c_str(), t, tilt);
			if (beginFade("balance")) return;
		}
	}

	// 2. Move each body toward the clip, BodyFollow of the way (strength, blend out, limbs giving way
	//    against a wall; 0 = leave that body to physics). Solid: by velocity only, so the solver keeps
	//    it out of the floor and walls. Otherwise: placed on the pose, through whatever is there.
	const bool log = MvmDebugLog_Active() && (c.frames % 15) == 0;
	const double h = 1.0 / 60.0;
	const double at = t > 0 ? t : 0;
	// Solid: the render frame the muscles act over, and for a grenade death the root drift -- the whole
	// animation is carried with wherever the blast has thrown the pelvis, not pulled back.
	const double frameDt = c.placedTime >= 0 && dt > 0 && dt < 0.05 ? dt : kPhysicsStep;
	// Thrown by a blast, the clip only POSES the body around its pelvis: the pelvis is pure physics
	// (skipped below) and every other body aims at its clip offset from where the pelvis will be after
	// the next physics step. The clip's own root motion never reaches the drive -- the CoD explosion
	// clips animate their own jump, and tracking it held the body up in the air.
	float drift[3] = {};
	if (g_tune.solid && haveActual && c.flying) {
		Pose pelvisNext;
		Target(c, at + kPhysicsStep, kPelvis, pelvisNext);
		for (int k = 0; k < 3; ++k)
			drift[k] = float(actual[kPelvis].p[k] + c.physLin[kPelvis][k] * kPhysicsStep - pelvisNext.p[k]);
	}
	for (int i = 0; i < kBodies; ++i) {
		const double w = BodyFollow(i, at, duration, g_tune, c.contact) * FadeOutWeight(at, c.fadeStart, g_tune.blendOut);
		// Thrown by a blast: every body's sideways speed decays (FlightDrag). The pelvis, and any body
		// the clip has let go, get just that -- pure physics otherwise.
		if (g_tune.solid && haveActual && c.flying) {
			FlightDrag(c.physLin[i], frameDt, FlightDragRate(actual[kPelvis].p[2] - c.placement.offset[2]));
			if (w < 0.001 || i == kPelvis) {
				// Everything written back is capped: the angular read comes straight from engine memory.
				float lin[3], ang[3];
				std::memcpy(lin, c.physLin[i], sizeof(lin)); std::memcpy(ang, c.physAng[i], sizeof(ang));
				ClampRelease(lin, ang, kDriveMaxLinear, kLaunchSpin);
				bool reasserted = false;
				if (!DriveBody(c.bodies[i], lin, ang, reasserted)) { Release(c, "write-fault"); return; }
				c.placedValid[i] = false;
				continue;
			}
		}
		if (w < 0.001) { c.placedValid[i] = false; continue; }
		// Velocity drive: always in solid playback, and in exact playback whenever the clip only holds
		// the body part-way (strength below 100 %, the blend out). Placing a body part-way every render
		// frame (~240 Hz) while physics moved it every tick (64 Hz) was a sawtooth -- jitter.
		const bool velocityDrive = haveActual && (g_tune.solid || w < 0.999);
		if (velocityDrive) {
			// Solid: one command per physics tick. Between ticks the engine hands back our own last write
			// as the body's velocity, so re-running the muscles every render frame ramped the command 3-4
			// times per tick off itself -- and the joints threw it back each step (trace 2026-09-26: a hand's
			// z command +3 -> +15 -> +21, then -24 after the step, a reversal every tick). The written
			// velocity simply stands until the solver consumes it.
			if (g_tune.solid && !stepped && c.placedValid[i]) continue;
			if (!g_tune.solid && !ReadVelocity(c.bodies[i], c.physLin[i], c.physAng[i]))
				for (int k = 0; k < 3; ++k) { c.physLin[i][k] = 0.f; c.physAng[i][k] = 0.f; }
			Pose now, next;
			Target(c, at, i, now);
			Target(c, at + kPhysicsStep, i, next);
			for (int k = 0; k < 3; ++k) { now.p[k] += drift[k]; next.p[k] += drift[k]; }
			float linear[3], angular[3];
			float drive[3], driveAngular[3];
			const double linAccel = MuscleLimit(w, i == kPelvis && c.flying ? kRootAccel : kMuscleAccel);
			const double angAccel = MuscleLimit(w, kMuscleAngular);
			// A body far from its pose is NOT placed back: at low strength that is the point, and the
			// muscles cannot fling it (measured: placing lagging limbs blew the ragdoll up at 10 %).
			if (TrackVelocity(actual[i], now, next, kPhysicsStep, linAccel, angAccel, drive, driveAngular)) {
				MuscleVelocity(c.physLin[i], c.physAng[i], drive, driveAngular, linAccel, angAccel, g_tune.solid ? kPhysicsStep : frameDt, linear, angular);
				const double l = std::sqrt(double(linear[0]) * linear[0] + double(linear[1]) * linear[1] + double(linear[2]) * linear[2]);
				const double a = std::sqrt(double(angular[0]) * angular[0] + double(angular[1]) * angular[1] + double(angular[2]) * angular[2]);
				if (l > c.peakLinear) c.peakLinear = l;
				if (a > c.peakAngular) c.peakAngular = a;
				bool reasserted = false;
				if (i == g_traceBody && MvmDebugLog_Active())
					MvmDebugLog_Linef("deathanim.trace", "u=%d t=%.4f st=%d w=%.2f act=%.2f,%.2f,%.2f tgt=%.2f,%.2f,%.2f phys=%.1f,%.1f,%.1f cmd=%.1f,%.1f,%.1f ang=%.0f,%.0f,%.0f/%.0f,%.0f,%.0f",
						c.userid, at, stepped ? 1 : 0, w, actual[i].p[0], actual[i].p[1], actual[i].p[2], now.p[0], now.p[1], now.p[2],
						c.physLin[i][0], c.physLin[i][1], c.physLin[i][2], linear[0], linear[1], linear[2],
						c.physAng[i][0], c.physAng[i][1], c.physAng[i][2], angular[0], angular[1], angular[2]);
				if (!DriveBody(c.bodies[i], linear, angular, reasserted)) { Release(c, "write-fault"); return; }
				if (reasserted && c.frames > 0) ++c.reasserts;
				// Where the next physics step should leave it: that, not the frame, is the prediction
				// the contact check compares against.
				c.placed[i] = actual[i];
				for (int k = 0; k < 3; ++k) { c.placed[i].p[k] = float(actual[i].p[k] + linear[k] * kPhysicsStep); c.placedVel[i][k] = 0.f; }
				c.placedValid[i] = true;
				continue;
			}
			c.placedValid[i] = false; // bad pose: leave that body to physics this frame
			continue;
		}
		Pose target, ahead;
		Target(c, at, i, target);
		Target(c, at + h, i, ahead);
		float linear[3] = {0, 0, 0}, angular[3] = {0, 0, 0};
		if (!ReleaseVelocity(target, ahead, h, linear, angular)) { linear[0] = linear[1] = linear[2] = 0; angular[0] = angular[1] = angular[2] = 0; }
		const Pose& command = target;
		bool reasserted = false;
		if (i == g_traceBody && MvmDebugLog_Active() && haveActual)
			MvmDebugLog_Linef("deathanim.trace", "u=%d t=%.4f st=%d w=%.2f act=%.2f,%.2f,%.2f tgt=%.2f,%.2f,%.2f hold vel=%.1f,%.1f,%.1f",
				c.userid, at, stepped ? 1 : 0, w, actual[i].p[0], actual[i].p[1], actual[i].p[2], target.p[0], target.p[1], target.p[2],
				linear[0], linear[1], linear[2]);
		if (!HoldBody(c.bodies[i], command, linear, angular, reasserted)) { Release(c, "write-fault"); return; }
		if (reasserted && c.frames > 0) ++c.reasserts;
		c.placed[i] = command;
		for (int k = 0; k < 3; ++k) c.placedVel[i][k] = linear[k];
		c.placedValid[i] = true;
	}
	// Motion telemetry: how far the hips and head really moved against how far the clip moved them.
	if (haveActual) {
		const int watch[2] = {kPelvis, kHeadBody};
		for (int w = 0; w < 2; ++w) {
			Pose clipPose;
			Target(c, at, watch[w], clipPose);
			if (c.motionValid) {
				double da = 0, dc = 0;
				for (int k = 0; k < 3; ++k) {
					da += (actual[watch[w]].p[k] - c.prevActual[w][k]) * (actual[watch[w]].p[k] - c.prevActual[w][k]);
					dc += (clipPose.p[k] - c.prevClip[w][k]) * (clipPose.p[k] - c.prevClip[w][k]);
				}
				c.pathActual[w] += std::sqrt(da);
				c.pathClip[w] += std::sqrt(dc);
			}
			std::memcpy(c.prevActual[w], actual[watch[w]].p, sizeof(c.prevActual[w]));
			std::memcpy(c.prevClip[w], clipPose.p, sizeof(c.prevClip[w]));
		}
		c.motionValid = true;
		// Jitter samples on the physics tick (reversals per second, see ReversalCounter).
		if (c.jitterSample < 0 || now - c.jitterSample >= kPhysicsStep) {
			const double dtj = c.jitterSample < 0 ? 0.0 : now - c.jitterSample;
			static constexpr int kJitterBodies[7] = {kPelvis, kHeadBody, 11, 12, 8, 10, 13};
			for (int j = 0; j < 7; ++j) {
				Pose clipPose;
				Target(c, at, kJitterBodies[j], clipPose);
				const int before = c.jitterActual[j].reversals;
				c.jitterActual[j].Add(actual[kJitterBodies[j]].p, dtj);
				if (c.jitterActual[j].reversals != before)
					MvmDebugLog_Linef("deathanim.reversal", "user=%d body=%d t=%.3f w=%.2f fade=%d stepped=%d contact=%d",
						c.userid, kJitterBodies[j], at, BodyFollow(kJitterBodies[j], at, duration, g_tune, c.contact),
						c.fadeStart >= 0 ? 1 : 0, stepped ? 1 : 0, c.contact.stop ? 1 : 0);
				c.jitterClip[j].Add(clipPose.p, dtj);
			}
			c.jitterTime += dtj;
			c.jitterSample = now;
		}
		const double above = actual[kPelvis].p[2] - c.placement.offset[2];
		if (above > c.peakAbove) c.peakAbove = above;
		{
			Pose pt;
			Target(c, at, kPelvis, pt);
			const double za = actual[kPelvis].p[2], zc = pt.p[2];
			if (za < c.minActualZ) c.minActualZ = za;
			if (zc < c.minClipZ) c.minClipZ = zc;
			if (za - c.minActualZ > c.reboundActual) { c.reboundActual = za - c.minActualZ; c.reboundAtT = at; c.reboundAtBlend = BlendIn(at, g_tune.blendIn); }
			if (zc - c.minClipZ > c.reboundClip) c.reboundClip = zc - c.minClipZ;
			const double lift = GroundOffsetAt(c.ground, at);
			if (lift > c.maxGroundLift) c.maxGroundLift = lift;
		}
	}
	c.placedTime = now;
	if (log) {
		double torso = 0, limbs[4] = {};
		for (int i = 0; i < kBodies; ++i) {
			const int limb = LimbOf(i);
			if (limb < 0) { if (push[i] > torso) torso = push[i]; }
			else if (push[i] > limbs[limb]) limbs[limb] = push[i];
		}
		MvmDebugLog_Linef("deathanim.frame",
			"user=%d clip=%s t=%.3f/%.3f %s push torso=%.2f armL=%.2f armR=%.2f legL=%.2f legR=%.2f give=%.2f/%.2f/%.2f/%.2f limp=%d%d%d%d strength=%.2f reasserts=%d peak=%.0fu/s,%.0fdeg/s",
			c.userid, clip.name.c_str(), t, duration, g_tune.solid ? "solid" : "exact", torso, limbs[0], limbs[1], limbs[2], limbs[3],
			c.contact.limbGive[0], c.contact.limbGive[1], c.contact.limbGive[2], c.contact.limbGive[3],
			c.contact.limp[0] ? 1 : 0, c.contact.limp[1] ? 1 : 0, c.contact.limp[2] ? 1 : 0, c.contact.limp[3] ? 1 : 0,
			g_tune.strength, c.reasserts, c.peakLinear, c.peakAngular);
		c.peakLinear = c.peakAngular = 0;
	}
	++c.frames;
}

bool SafeFlags(CEntityInstance* e, uint32_t& flags) {
	const ptrdiff_t off = g_clientDllOffsets.C_BaseEntity.m_fFlags;
	if (!e || !off) return false;
	__try { flags = *(uint32_t*)((unsigned char*)e + off); return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

constexpr uint32_t kFlDucking = 1u << 1;

bool ProbeHealth(CEntityInstance* pawn, unsigned& health) {
	__try { health = pawn->GetHealth(); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void BodyProbe() {
	if (g_bodyProbeFrames <= 0 || !MvmDebugLog_Active()) return;
	--g_bodyProbeFrames;
	int tick = -1;
	g_MirvTime.GetCurrentDemoTick(tick);
	double demoTime = 0.0;
	g_MirvTime.GetCurrentDemoTime(demoTime);
	for (int userid = 0; userid < 64; ++userid) {
		int index = -1;
		CEntityInstance* pawn = PawnOfUser(userid, &index);
		if (!pawn) continue;
		unsigned health = 1;
		if (!ProbeHealth(pawn, health) || health > 0) continue;
		Corpse probe;
		probe.pawn = pawn;
		char why[512] = {};
		if (!Acquire(probe, why, sizeof(why))) continue;
		Pose pelvis, hand;
		int motion = 0;
		if (!ReadBody(probe.bodies[kPelvis], pelvis, motion) || !ReadBody(probe.bodies[12], hand, motion)) continue;
		float att[3] = {}, attQ[4] = {};
		const bool haveAtt = SafeAttachment(pawn, "weapon_hand_R", att, attQ);
		MvmDebugLog_LinefAlways("deathanim.bodyprobe",
			"user=%d tick=%d demoTime=%.4f real=%.4f pelvis=(%.2f %.2f %.2f) handBody=(%.2f %.2f %.2f) handDrawn=%d(%.2f %.2f %.2f) motion=%d",
			userid, tick, demoTime, (double)GetTickCount64() / 1000.0, pelvis.p[0], pelvis.p[1], pelvis.p[2],
			hand.p[0], hand.p[1], hand.p[2], haveAtt ? 1 : 0, att[0], att[1], att[2], motion);
	}
}

void SampleLiving(double now) {
	const int highest = GetHighestEntityIndex();
	for (int userid = 0; userid < 64 && userid + 1 <= highest; ++userid) {
		CEntityInstance* pawn = PawnOfUser(userid, nullptr);
		float o[3];
		if (!pawn || !SafeOrigin(pawn, o)) continue;
		UserState& u = g_users[userid];
		uint32_t flags = 0;
		unsigned health = 0;
		bool alive = false;
		__try { health = pawn->GetHealth(); alive = health > 0; } __except (EXCEPTION_EXECUTE_HANDLER) { alive = false; }
		if (alive && SafeFlags(pawn, flags)) { u.grounded = (flags & 1u) != 0; u.ducking = (flags & kFlDucking) != 0; u.duckTime = now; }
		const double dt = now - u.originTime;
		if (dt > 0.02 && dt < 0.5) {
			const double dx = o[0] - u.origin[0], dy = o[1] - u.origin[1];
			u.speed = std::sqrt(dx * dx + dy * dy) / dt;
		}
		if (dt > 0.02 || dt < 0) { std::memcpy(u.origin, o, sizeof(o)); u.originTime = now; }
	}
}

} // namespace

// ---------------------------------------------------------------------------------------------

bool DeathAnimations_Enabled() { return g_enabled; }

bool DeathAnimations_Owns(int pawnIndex) {
	for (const Corpse& c : g_corpses)
		if (c.phase != Phase::Free && c.pawnIndex == pawnIndex) return true;
	return false;
}

void DeathAnimations_SetEnabled(bool enabled) {
	if (g_enabled == enabled) return;
	g_enabled = enabled;
	if (enabled) LoadLibrary();
	else ReleaseAll("disabled");
}

int DeathAnimations_Source() { return (int)g_source; }

void DeathAnimations_SetSource(int source) {
	if (source < 0 || source >= (int)Source::Count) return;
	g_source = (Source)source;
}

double DeathAnimations_BlendIn() { return g_tune.blendIn; }
void DeathAnimations_SetBlendIn(double seconds) { if (std::isfinite(seconds)) g_tune.blendIn = seconds < 0 ? 0 : (seconds > 1 ? 1 : seconds); }
double DeathAnimations_Impact() { return g_tune.impact * 100.0; }
void DeathAnimations_SetImpact(double percent) {
	if (std::isfinite(percent)) g_tune.impact = (percent < 0 ? 0 : (percent > 300 ? 300 : percent)) / 100.0;
}
double DeathAnimations_Strength() { return g_tune.strength * 100.0; }
void DeathAnimations_SetStrength(double percent) {
	if (std::isfinite(percent)) g_tune.strength = (percent < 10 ? 10 : (percent > 100 ? 100 : percent)) / 100.0;
}
double DeathAnimations_BlendOut() { return g_tune.blendOut; }
void DeathAnimations_SetBlendOut(double seconds) { if (std::isfinite(seconds)) g_tune.blendOut = seconds < 0 ? 0 : (seconds > 1 ? 1 : seconds); }
bool DeathAnimations_Walls() { return g_tune.reactToWalls; }
void DeathAnimations_SetWalls(bool on) { g_tune.reactToWalls = on; }
bool DeathAnimations_Solid() { return g_tune.solid; }
void DeathAnimations_SetSolid(bool on) { g_tune.solid = on; }

void DeathAnimations_LevelInit() {
	for (Corpse& c : g_corpses) c = Corpse{};
	for (UserState& u : g_users) u = UserState{};
	g_lastTick = -1;
}

void DeathAnimations_OnGameEvent(SOURCESDK::CS2::CGameEvent* event) {
	if (!g_enabled || !event) return;
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
	for (const Corpse& c : g_corpses)
		if (c.phase != Phase::Free && c.userid == userid && std::fabs(c.deathTime - now) < 0.05) return;
	Corpse* slot = nullptr;
	for (Corpse& c : g_corpses) if (c.phase == Phase::Free) { slot = &c; break; }
	if (!slot) return;
	int pawnIndex = -1;
	CEntityInstance* pawn = PawnOfUser(userid, &pawnIndex);
	if (!pawn) return;
	Corpse c;
	c.phase = Phase::Waiting;
	c.userid = userid;
	c.pawn = pawn;
	c.pawnIndex = pawnIndex;
	c.deathTime = now;
	int tick = 0;
	g_MirvTime.GetCurrentDemoTick(tick);
	c.seed = (uint32_t)tick * 0x9e3779b9u ^ (uint32_t)(userid + 1) * 0x85ebca6bu;
	const std::string weapon = JStr(r, "weapon");
	const UserState& u = g_users[userid];
	c.facts.hitgroup = now - u.hurtTime < 1.0 ? u.hitgroup : 0;
	c.facts.headshot = JBool(r, "headshot");
	c.facts.burning = Contains(weapon, "inferno") || Contains(weapon, "molotov") || Contains(weapon, "incgrenade");
	c.facts.explosive = Contains(weapon, "hegrenade") || Contains(weapon, "c4");
	c.facts.melee = Contains(weapon, "knife") || Contains(weapon, "bayonet") || Contains(weapon, "melee");
	c.killer = WeaponClassOf(weapon.c_str());
	std::snprintf(c.playerKey, sizeof(c.playerKey), "%s", PlayerKey(EntityAt(userid + 1)).c_str());
	std::snprintf(c.weapon, sizeof(c.weapon), "%s", weapon.c_str());
	c.facts.planarSpeed = now - u.originTime < 0.5 ? u.speed : 0;
	SafeOrigin(pawn, c.victimOrigin);
	const int attacker = JInt(r, "attacker");
	if (attacker >= 0 && attacker != userid)
		c.attackerKnown = SafeOrigin(PawnOfUser(attacker, nullptr), c.attacker);
	*slot = c;
	MvmDebugLog_Linef("deathanim", "death user=%d weapon=%s hitgroup=%d headshot=%d speed=%.0f attacker=%d",
		userid, weapon.c_str(), c.facts.hitgroup, c.facts.headshot ? 1 : 0, c.facts.planarSpeed, attacker);
}

void DeathAnimations_RunFrame() {
	BodyProbe();
	bool busy = false;
	for (const Corpse& c : g_corpses) busy |= c.phase != Phase::Free;
	if (!g_enabled && !busy) return;
	const double now = g_MirvTime.curtime_get();
	int tick = 0;
	if (g_MirvTime.GetCurrentDemoTick(tick)) {
		// A seek rebuilds the world; bodies still alive after it go back to physics.
		if (g_lastTick >= 0 && (tick < g_lastTick - 8 || tick > g_lastTick + 256)) DropAll("seek");
		g_lastTick = tick;
	}
	if (!g_enabled) { ReleaseAll("disabled"); return; }
	Environment::MapCollisionGrid_Pump();
	SampleLiving(now);
	PumpSettles(now);
	for (Corpse& c : g_corpses) {
		if (c.phase == Phase::Waiting) {
			char why[512] = {};
			const bool found = EntityAt(c.pawnIndex) == c.pawn && Acquire(c, why, sizeof(why));
			if (!found && (now - c.deathTime > kAcquireWindow || now < c.deathTime - 0.05)) {
				MvmDebugLog_Linef("deathanim", "skip user=%d reason=no-ragdoll-bodies waited=%.2f pawn=%p class=%s child=%p routes:%s",
					c.userid, now - c.deathTime, c.pawn, EntityAt(c.pawnIndex) == c.pawn ? SafeClassName(c.pawn) : "gone",
					EntityAt(c.pawnIndex) == c.pawn ? ClientRagdollOf(c.pawn) : nullptr, why);
				++g_failed;
				c = Corpse{};
				continue;
			}
			if (EntityAt(c.pawnIndex) != c.pawn) { c = Corpse{}; continue; }
			if (found) Begin(c, now);
			if (c.phase == Phase::Playing) Drive(c, now);
		} else if (c.phase == Phase::Playing) {
			Drive(c, now);
		}
	}
}

void DeathAnimations_RunLateFrame() {
	if (!g_enabled || !MvmDebugLog_Active()) return;
	for (Corpse& c : g_corpses) {
		// Current(), not just the pawn: this runs after the engine's own frame handler, which may
		// have removed the ragdoll since Drive last checked it.
		if (c.phase != Phase::Playing || (c.frames > 45 && g_traceBody != 12) || !Current(c)) continue;
		Pose hand; int motion = 0;
		if (!ReadBody(c.bodies[12], hand, motion)) continue;
		float att[3] = {}, attQ[4] = {}, origin[3] = {};
		const bool haveAtt = SafeAttachment(c.pawn, "weapon_hand_R", att, attQ);
		SafeOrigin(c.pawn, origin);
		const double dx = att[0] - hand.p[0], dy = att[1] - hand.p[1], dz = att[2] - hand.p[2];
		if (g_traceBody == 12) {
			MvmDebugLog_Linef("deathanim.drawn", "u=%d t=%.4f att=%.2f,%.2f,%.2f held=%.2f,%.2f,%.2f body=%.2f,%.2f,%.2f",
				c.userid, g_MirvTime.curtime_get() - c.start, att[0], att[1], att[2], c.placed[12].p[0], c.placed[12].p[1], c.placed[12].p[2],
				hand.p[0], hand.p[1], hand.p[2]);
			continue;
		}
		MvmDebugLog_Linef("deathanim.render",
			"user=%d frame=%d handBody=(%.1f %.1f %.1f) handAttach=%d(%.1f %.1f %.1f) gap=%.1f origin=(%.1f %.1f %.1f) motion=%d",
			c.userid, c.frames, hand.p[0], hand.p[1], hand.p[2], haveAtt ? 1 : 0, att[0], att[1], att[2],
			std::sqrt(dx * dx + dy * dy + dz * dz), origin[0], origin[1], origin[2], motion);
	}
}

const std::map<std::string, std::string>& DeathAnimations_PlayerClips() { return g_playerClips; }

void DeathAnimations_SetPlayerClip(const std::string& playerKey, const std::string& clip) {
	if (playerKey.empty() || playerKey.size() > 79) return;
	if (clip.empty() || !_stricmp(clip.c_str(), "auto")) { g_playerClips.erase(playerKey); return; }
	if (g_playerClips.size() >= 256 && !g_playerClips.count(playerKey)) return;
	g_playerClips[playerKey] = clip;
}

bool DeathAnimations_ClipPose(const char* name, double seconds, float* poses7x15, const float** base, double* duration) {
	LoadLibrary();
	const int index = name ? FindClip(name) : -1;
	if (index < 0) return false;
	const Clip& c = g_lib.clips[index];
	if (base) *base = c.base;
	if (duration) *duration = Duration(c.frames, g_lib.fps);
	if (poses7x15) {
		const Pose* all = SampleClip(index, seconds);
		for (int b = 0; b < kBodies; ++b) {
			const Pose& pose = all[b];
			float* out = poses7x15 + b * 7;
			for (int k = 0; k < 3; ++k) out[k] = pose.p[k];
			for (int k = 0; k < 4; ++k) out[3 + k] = pose.q[k];
		}
	}
	return true;
}

bool DeathAnimations_TakeScript(std::string& script) {
	if (g_pendingScript.empty()) return false;
	script.swap(g_pendingScript);
	g_pendingScript.clear();
	return true;
}

namespace {

void JsonString(std::ostringstream& o, const char* s) {
	o << '"';
	for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; ++p) {
		if (*p == '"' || *p == '\\') o << '\\' << *p;
		else if (*p < 0x20) o << ' ';
		else o << *p;
	}
	o << '"';
}

const char* SourceLabel(Source s) {
	return s == Source::Mw2019 ? "MW2019" : s == Source::Classic ? "Classic CoD" : "Extra";
}

// Every clip, compact, for the picker's list: [name, source, stance, area, side, weapon, frames].
std::string CatalogScript() {
	std::ostringstream o;
	o << "$.ConfigHudDap && $.ConfigHudDap.setCatalog([";
	for (size_t i = 0; i < g_lib.clips.size(); ++i) {
		const Clip& c = g_lib.clips[i];
		if (i) o << ',';
		o << '[';
		JsonString(o, c.name.c_str());
		o << ",\"" << SourceLabel(c.tags.source) << "\",\"" << kStanceNames[(int)c.tags.stance]
		  << "\",\"" << kCategoryNames[(int)c.tags.category] << "\",\"" << kSideNames[(int)c.tags.side]
		  << "\",\"" << kWeaponNames[(int)c.tags.weapon] << "\"," << c.frames << ']';
	}
	o << "]);";
	return o.str();
}

CEntityInstance* ControllerPawn(CEntityInstance* controller) {
	if (!controller) return nullptr;
	__try {
		const auto handle = controller->GetPlayerPawnHandle();
		return handle.IsValid() ? EntityAt(handle.GetEntryIndex()) : nullptr;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void AppendPlayersJson(std::ostringstream& o) {
	o << ",\"daPlayers\":[";
	bool first = true;
	const int highest = GetHighestEntityIndex();
	for (int i = 1; i <= 64 && i <= highest; ++i) {
		CEntityInstance* e = EntityAt(i);
		int team = 0;
		uint64_t steam = 0;
		const char* name = nullptr;
		if (!ControllerInfo(e, team, steam, name) || team < 2) continue;
		const std::string key = PlayerKey(e);
		if (key.empty()) continue;
		auto it = g_playerClips.find(key);
		o << (first ? "" : ",") << "{\"k\":";
		JsonString(o, key.c_str());
		o << ",\"n\":";
		JsonString(o, name ? name : "?");
		o << ",\"t\":" << team << ",\"c\":";
		JsonString(o, it == g_playerClips.end() ? "" : it->second.c_str());
		// The player's agent model, for the picker's player-model preview.
		char model[260] = {};
		if (CEntityInstance* pawn = ControllerPawn(e)) CosmeticReadEntityModelPath(pawn, model, sizeof(model));
		o << ",\"m\":";
		JsonString(o, model);
		o << '}';
		first = false;
	}
	o << ']';
}

} // namespace

void DeathAnimations_AppendStateJson(std::ostringstream& o) {
	int active = 0;
	for (const Corpse& c : g_corpses) active += c.phase == Phase::Playing ? 1 : 0;
	o << ",\"daOn\":" << (g_enabled ? "true" : "false")
	  << ",\"daSource\":" << (int)g_source
	  << ",\"daClips\":[" << ClipCount(Source::Mw2019) << "," << ClipCount(Source::Classic) << "," << ClipCount(Source::Extra) << "]"
	  << ",\"daLoaded\":" << (g_lib.attempted ? "true" : "false")
	  << ",\"daActive\":" << active
	  << ",\"daBlendIn\":" << g_tune.blendIn
	  << ",\"daStrength\":" << g_tune.strength * 100.0
	  << ",\"daImpact\":" << g_tune.impact * 100.0
	  << ",\"daBlendOut\":" << g_tune.blendOut
	  << ",\"daWalls\":" << (g_tune.reactToWalls ? "true" : "false")
	  << ",\"daSolid\":" << (g_tune.solid ? "true" : "false")
	  << ",\"daAssigned\":" << g_playerClips.size();
	AppendPlayersJson(o);
	std::string status = g_lib.attempted ? g_lib.status : "loads when enabled";
	for (char& ch : status) if (ch == '"' || ch == '\\') ch = '\'';
	o << ",\"daStatus\":\"" << status << "\"";
}

void DeathAnimations_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	// argv: mirv_filmmaker deathanim [on|off|toggle|source <mw2019|classic|extra>|clip <name|off>|
	//        next <name>...|list [source]|status|reset]
	const char* sub = argc >= 3 ? args->ArgV(2) : "status";
	if (!_stricmp(sub, "on") || !_stricmp(sub, "off") || !_stricmp(sub, "toggle")) {
		DeathAnimations_SetEnabled(!_stricmp(sub, "toggle") ? !g_enabled : !_stricmp(sub, "on"));
		advancedfx::Message("mirv_filmmaker deathanim: %s (%s).\n", g_enabled ? "on" : "off", kSourceNames[(int)g_source]);
		return;
	}
	if (!_stricmp(sub, "player")) {
		// deathanim player <key> <clip|auto>
		if (argc >= 5) {
			LoadLibrary();
			const char* clip = args->ArgV(4);
			if (_stricmp(clip, "auto") && FindClip(clip) < 0) { advancedfx::Message("deathanim: no clip named %s.\n", clip); return; }
			DeathAnimations_SetPlayerClip(args->ArgV(3), clip);
		}
		for (const auto& kv : g_playerClips) advancedfx::Message("  %s -> %s\n", kv.first.c_str(), kv.second.c_str());
		advancedfx::Message("deathanim: %d player(s) with their own animation.\n", (int)g_playerClips.size());
		return;
	}
	if (!_stricmp(sub, "armcheck")) {
		// deathanim armcheck [top]: how many frames of each clip push a limb into the body (and so get
		// corrected by KeepLimbsOutOfBody). Lists the worst clips -- the ones to eyeball in the picker.
		LoadLibrary();
		const int top = argc >= 4 ? atoi(args->ArgV(3)) : 15;
		std::vector<std::pair<int, int>> hits;
		int total = 0, frames = 0;
		for (int i = 0; i < (int)g_lib.clips.size(); ++i) {
			const Clip& c = g_lib.clips[i];
			int n = 0;
			for (int f = 0; f < c.frames; ++f) {
				Pose pose[kBodies];
				for (int b = 0; b < kBodies; ++b) Sample(c.base, Frames(c), c.frames, g_lib.fps, f / g_lib.fps, b, pose[b]);
				if (KeepLimbsOutOfBody(pose)) ++n;
			}
			frames += c.frames;
			total += n;
			if (n) hits.push_back({n, i});
		}
		std::sort(hits.begin(), hits.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) { return a.first > b.first; });
		for (int k = 0; k < (int)hits.size() && k < top; ++k)
			advancedfx::Message("  %-40s %4d / %4d frames corrected\n", g_lib.clips[hits[k].second].name.c_str(),
				hits[k].first, g_lib.clips[hits[k].second].frames);
		advancedfx::Message("deathanim armcheck: %d of %d clips, %d of %d frames have a limb inside the body.\n",
			(int)hits.size(), (int)g_lib.clips.size(), total, frames);
		return;
	}
	if (!_stricmp(sub, "view")) {
		// deathanim view <clip> | off | status: the clip on the picker's player-model preview.
		if (argc >= 4 && _stricmp(args->ArgV(3), "status")) DeathAnimPreview_Set(args->ArgV(3));
		DeathAnimPreview_PrintStatus();
		return;
	}
	if (!_stricmp(sub, "catalog")) {
		LoadLibrary();
		g_pendingScript = CatalogScript();
		return;
	}
	{
		// Playback feel: deathanim blendin <s> | strength <percent> | impact <percent> | blendout <s> | walls|solid on|off|toggle
		const bool quiet = argc >= 5 && !_stricmp(args->ArgV(4), "quiet");
		if (!_stricmp(sub, "impact")) {
			if (argc >= 4) DeathAnimations_SetImpact(atof(args->ArgV(3)));
			if (!quiet) advancedfx::Message("mirv_filmmaker deathanim impact: %.0f%%\n", g_tune.impact * 100.0);
			return;
		}
		if (!_stricmp(sub, "blendin") || !_stricmp(sub, "strength") || !_stricmp(sub, "blendout")) {
			if (argc >= 4) {
				const double v = atof(args->ArgV(3));
				if (!_stricmp(sub, "blendin")) DeathAnimations_SetBlendIn(v);
				else if (!_stricmp(sub, "strength")) DeathAnimations_SetStrength(v);
				else DeathAnimations_SetBlendOut(v);
			}
			if (!quiet) advancedfx::Message("mirv_filmmaker deathanim: blend in %.2f s, strength %.0f%%, blend out %.2f s, walls %s, solid %s\n",
				g_tune.blendIn, g_tune.strength * 100.0, g_tune.blendOut, g_tune.reactToWalls ? "on" : "off", g_tune.solid ? "on" : "off");
			return;
		}
		if (!_stricmp(sub, "walls")) {
			const char* v = argc >= 4 ? args->ArgV(3) : "toggle";
			if (!_stricmp(v, "on") || !strcmp(v, "1")) g_tune.reactToWalls = true;
			else if (!_stricmp(v, "off") || !strcmp(v, "0")) g_tune.reactToWalls = false;
			else g_tune.reactToWalls = !g_tune.reactToWalls;
			if (!quiet) advancedfx::Message("mirv_filmmaker deathanim walls: %s\n", g_tune.reactToWalls ? "on" : "off");
			return;
		}
		if (!_stricmp(sub, "bodyprobe")) {
			g_bodyProbeFrames = argc >= 4 ? atoi(args->ArgV(3)) : 600;
			advancedfx::Message("mirv_filmmaker deathanim bodyprobe: %d frame(s) to deathanim.bodyprobe (needs mvm_debug)\n", g_bodyProbeFrames);
			return;
		}
		if (!_stricmp(sub, "trace")) {
			g_traceBody = argc >= 4 ? atoi(args->ArgV(3)) : -1;
			advancedfx::Message("mirv_filmmaker deathanim trace: body %d\n", g_traceBody);
			return;
		}
		if (!_stricmp(sub, "solid")) {
			const char* v = argc >= 4 ? args->ArgV(3) : "toggle";
			if (!_stricmp(v, "on") || !strcmp(v, "1")) g_tune.solid = true;
			else if (!_stricmp(v, "off") || !strcmp(v, "0")) g_tune.solid = false;
			else g_tune.solid = !g_tune.solid;
			if (!quiet) advancedfx::Message("mirv_filmmaker deathanim solid: %s\n", g_tune.solid ? "on" : "off");
			return;
		}
	}
	if (!_stricmp(sub, "source")) {
		if (argc >= 4) {
			for (int i = 0; i < (int)Source::Count; ++i)
				if (!_stricmp(args->ArgV(3), kSourceKeys[i]) || atoi(args->ArgV(3)) == i && args->ArgV(3)[0] == '0' + i)
					DeathAnimations_SetSource(i);
		}
		advancedfx::Message("mirv_filmmaker deathanim source: %s\n", kSourceNames[(int)g_source]);
		return;
	}
	if (!_stricmp(sub, "reset")) {
		DeathAnimations_SetEnabled(false);
		g_source = Source::Mw2019;
		g_tune = PlaybackTuning{};
		g_forcedClip.clear();
		g_queue.clear();
		advancedfx::Message("mirv_filmmaker deathanim: reset (off, Modern Warfare 2019).\n");
		return;
	}
	LoadLibrary();
	if (!_stricmp(sub, "clip")) {
		if (argc >= 4 && _stricmp(args->ArgV(3), "off")) {
			if (FindClip(args->ArgV(3)) < 0) { advancedfx::Message("deathanim: no clip named %s.\n", args->ArgV(3)); return; }
			g_forcedClip = args->ArgV(3);
		} else g_forcedClip.clear();
		advancedfx::Message("deathanim: every death plays %s.\n", g_forcedClip.empty() ? "a picked clip" : g_forcedClip.c_str());
		return;
	}
	if (!_stricmp(sub, "next")) {
		for (int i = 3; i < argc; ++i) {
			if (FindClip(args->ArgV(i)) < 0) { advancedfx::Message("deathanim: no clip named %s.\n", args->ArgV(i)); continue; }
			g_queue.push_back(args->ArgV(i));
		}
		advancedfx::Message("deathanim: %d queued clip(s).\n", (int)g_queue.size());
		return;
	}
	if (!_stricmp(sub, "list")) {
		int want = (int)g_source;
		if (argc >= 4) for (int i = 0; i < (int)Source::Count; ++i) if (!_stricmp(args->ArgV(3), kSourceKeys[i])) want = i;
		for (const Clip& c : g_lib.clips)
			if ((int)c.tags.source == want)
				advancedfx::Message("%-32s %-9s %-6s %-8s %-7s frames=%d\n", c.name.c_str(), kCategoryNames[(int)c.tags.category],
					kSideNames[(int)c.tags.side], kStanceNames[(int)c.tags.stance], kWeaponNames[(int)c.tags.weapon], c.frames);
		return;
	}
	int active = 0;
	for (const Corpse& c : g_corpses) active += c.phase == Phase::Playing ? 1 : 0;
	advancedfx::Message("deathanim: %s, source %s, file: %s, clips mw2019=%d classic=%d extra=%d, playing=%d played=%d failed=%d%s%s\n",
		g_enabled ? "on" : "off", kSourceNames[(int)g_source], g_lib.status.c_str(),
		ClipCount(Source::Mw2019), ClipCount(Source::Classic), ClipCount(Source::Extra), active, g_played, g_failed,
		g_forcedClip.empty() ? "" : ", forced ", g_forcedClip.c_str());
	(void)cmd;
}

} // namespace Filmmaker
