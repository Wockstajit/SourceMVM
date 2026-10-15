// Death-animation clip on the Config picker's player-model preview. Contract: DeathAnimPreview.h.

#include "DeathAnimPreview.h"
#include "../Movie/ModelPoseAccess.h"

#include "../Movie/DeathAnimations.h"
#include "../Movie/DeathAnimationsMath.h"
#include "../Debug/MvmDebugLog.h"
#include "../../ClientEntitySystem.h"
#include "../../SchemaSystem.h"
#include "../../../Shared/AfxConsole.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <string>

namespace Filmmaker {
namespace {

using namespace DeathAnimationsMath;
using namespace ModelPoseAccess;

constexpr int kMaxBones = 160;
constexpr int kClientOnlyBase = 0x4000;
constexpr int kMaxHandleIndex = 0x8000;
constexpr ptrdiff_t kLocalPose = 0x18;      // CModelState: parent-local pose the engine draws from
constexpr ptrdiff_t kLocalPoseCopy = 0x48;  // CModelState: second copy of the same pose
constexpr size_t kBoneStride = 32;          // vec3 position, float scale, quat xyzw
constexpr double kLoopHold = 0.8;
constexpr float kWeaponDrop = 4000.f;       // units the weapon bones are sunk below the preview floor           // seconds on the last frame before the clip restarts

// The fifteen ragdoll bodies, in pack order (FX/tools/extract-death-anims.py CS2_BODY_ORDER).
const char* const kBodyBone[kBodies] = {
	"leg_upper_l", "leg_lower_l", "leg_upper_r", "leg_lower_r", "head_0", "pelvis", "spine_2",
	"arm_upper_l", "arm_lower_l", "arm_upper_r", "arm_lower_r", "hand_l", "hand_r", "ankle_l", "ankle_r"};

struct Skeleton {
	const void* model = nullptr;
	int count = 0;
	int parents[kMaxBones] = {};
	int driven[kMaxBones] = {};
	bool weaponBone[kMaxBones] = {};  // weapon_hand_R / weapon_hand_L / ...: where the held gun is bone-merged
	int bodyBone[kBodies] = {};
};

struct PawnState {
	const void* model = nullptr;
	Pose rest[kMaxBones];   // model-space idle pose, captured before the first write
	bool haveRest = false;
};

std::string g_clip;
bool g_active = false;
ULONGLONG g_startMs = 0;
std::map<const void*, Skeleton> g_skeletons;
std::map<int, PawnState> g_pawns;
int g_pawnList[16] = {};
int g_pawnCount = 0;
uint64_t g_frame = 0, g_writes = 0;
const char* g_why = "idle";

CEntityInstance* WideResolve(int index) {
	if (index < 0 || index >= kMaxHandleIndex || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	__try { return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index); }
	__except (1) { return nullptr; }
}

bool IsPreviewPawn(CEntityInstance* e) {
	if (!e) return false;
	__try {
		const char* cls = e->GetClassName();
		return cls && std::strstr(cls, "previewplayer") != nullptr;
	} __except (1) { return false; }
}

bool NameEquals(const char* a, const char* b) {
	for (; *a && *b; ++a, ++b) {
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
		if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
		if (x != y) return false;
	}
	return *a == *b;
}

// One entry of the bone tables. Its own SEH frame: the name table is not terminated, so the read
// just past the last bone can fault, and that has to END the table rather than discard it.
// Bone names + parents, validated: every name a short identifier, every parent an earlier bone.
bool ReadSkeletonRaw(const void* model, Skeleton& sk) {
	const char* const* names = nullptr;
	const int16_t* parents = nullptr;
	if (!ReadTablePointers(model, names, parents)) return false;
	for (int i = 0; i < kBodies; ++i) sk.bodyBone[i] = -1;
	int n = 0;
	for (; n < kMaxBones; ++n) {
		char name[64];
		int parent = 0;
		if (!ReadBoneEntry(names, parents, n, name, sizeof(name), parent)) break;
		if (parent >= n || parent < -1) break;
		sk.parents[n] = parent;
		sk.driven[n] = -1;
		sk.weaponBone[n] = (name[0] == 'w' || name[0] == 'W') && _strnicmp(name, "weapon", 6) == 0;
		for (int b = 0; b < kBodies; ++b)
			if (sk.bodyBone[b] < 0 && NameEquals(name, kBodyBone[b])) { sk.bodyBone[b] = n; sk.driven[n] = b; }
	}
	sk.count = n;
	return n > 0;
}

const Skeleton* SkeletonOf(const void* model) {
	auto it = g_skeletons.find(model);
	if (it != g_skeletons.end()) return it->second.count ? &it->second : nullptr;
	Skeleton& sk = g_skeletons[model];
	sk.model = model;
	if (!ReadSkeletonRaw(model, sk) || sk.count < kBodies) { sk.count = 0; g_why = "bone table unreadable"; return nullptr; }
	for (int b = 0; b < kBodies; ++b)
		if (sk.bodyBone[b] < 0) { sk.count = 0; g_why = "a ragdoll bone is missing from the skeleton"; return nullptr; }
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("deathanim.view", "skeleton model=%p bones=%d pelvis=%d head=%d", model, sk.count,
			sk.bodyBone[kPelvis], sk.bodyBone[4]);
	return &sk;
}

bool ReadLocals(unsigned char* ms, int count, Pose* out) {
	__try {
		const unsigned char* arr = *(const unsigned char**)(ms + kLocalPose);
		if ((uintptr_t)arr < 0x10000) return false;
		for (int b = 0; b < count; ++b) {
			const float* r = (const float*)(arr + b * kBoneStride);
			for (int k = 0; k < 3; ++k) out[b].p[k] = r[k];
			for (int k = 0; k < 4; ++k) out[b].q[k] = r[4 + k];
		}
		return true;
	} __except (1) { return false; }
}

bool WriteLocals(unsigned char* ms, int count, const Pose* in) {
	__try {
		for (ptrdiff_t off : {kLocalPose, kLocalPoseCopy}) {
			unsigned char* arr = *(unsigned char**)(ms + off);
			if ((uintptr_t)arr < 0x10000) return false;
			for (int b = 0; b < count; ++b) {
				float* r = (float*)(arr + b * kBoneStride);
				for (int k = 0; k < 3; ++k) r[k] = in[b].p[k];
				for (int k = 0; k < 4; ++k) r[4 + k] = in[b].q[k];
			}
		}
		return true;
	} __except (1) { return false; }
}

void Compose(const Pose& parent, const Pose& local, Pose& out) {
	float rp[3];
	QuatRotate(parent.q, local.p, rp);
	for (int k = 0; k < 3; ++k) out.p[k] = parent.p[k] + rp[k];
	QuatMul(parent.q, local.q, out.q);
	QuatNormalize(out.q);
}

void Relative(const Pose& parent, const Pose& world, Pose& out) {
	float inv[4], d[3];
	QuatConj(parent.q, inv);
	for (int k = 0; k < 3; ++k) d[k] = world.p[k] - parent.p[k];
	QuatRotate(inv, d, out.p);
	QuatMul(inv, world.q, out.q);
	QuatNormalize(out.q);
}

void RefreshPawnList() {
	g_pawnCount = 0;
	for (int i = kClientOnlyBase; i < kMaxHandleIndex && g_pawnCount < 16; ++i) {
		CEntityInstance* e = WideResolve(i);
		if (e && IsPreviewPawn(e)) g_pawnList[g_pawnCount++] = i;
	}
}

void PosePawn(int index, double seconds) {
	CEntityInstance* e = WideResolve(index);
	if (!e || !IsPreviewPawn(e)) return;
	unsigned char* ms = ModelStateOf(e);
	const void* model = ModelOf(ms);
	if (!ms || !model) { g_why = "no model state"; return; }
	const Skeleton* sk = SkeletonOf(model);
	if (!sk) return;

	PawnState& ps = g_pawns[index];
	if (!ps.haveRest || ps.model != model) {
		Pose local[kMaxBones];
		if (!ReadLocals(ms, sk->count, local)) { g_why = "local pose unreadable"; return; }
		for (int b = 0; b < sk->count; ++b) {
			if (sk->parents[b] < 0) ps.rest[b] = local[b];
			else Compose(ps.rest[sk->parents[b]], local[b], ps.rest[b]);
		}
		ps.model = model;
		ps.haveRest = true;
	}

	float raw[kBodies * 7];
	const float* base = nullptr;
	double duration = 0;
	if (!DeathAnimations_ClipPose(g_clip.c_str(), seconds, raw, &base, &duration) || !base) { g_why = "clip not found"; return; }

	// Face the clip the way the idle character faces (the preview camera looks at its front), feet on
	// the model's floor (z = 0), and keep the pelvis over its start so a clip that travels stays in
	// the frame.
	float restBodies[kBodies * 3];
	for (int b = 0; b < kBodies; ++b)
		for (int k = 0; k < 3; ++k) restBodies[b * 3 + k] = ps.rest[sk->bodyBone[b]].p[k];
	const Placement pl = SolvePlacement(base, restBodies, 0.0);
	Pose bodies[kBodies];
	for (int b = 0; b < kBodies; ++b) {
		Pose clip;
		for (int k = 0; k < 3; ++k) clip.p[k] = raw[b * 7 + k];
		for (int k = 0; k < 4; ++k) clip.q[k] = raw[b * 7 + 3 + k];
		Place(pl, clip, bodies[b]);
	}
	Pose startPelvis, clipStart;
	for (int k = 0; k < 3; ++k) clipStart.p[k] = base[kPelvis * 3 + k];
	Place(pl, clipStart, startPelvis);
	const float driftX = bodies[kPelvis].p[0] - startPelvis.p[0], driftY = bodies[kPelvis].p[1] - startPelvis.p[1];
	for (int b = 0; b < kBodies; ++b) { bodies[b].p[0] -= driftX; bodies[b].p[1] -= driftY; }

	Pose world[kMaxBones], local[kMaxBones];
	FollowDriven(sk->parents, sk->count, sk->driven, bodies, ps.rest, world);
	for (int b = 0; b < sk->count; ++b) {
		if (sk->parents[b] < 0) local[b] = world[b];
		else Relative(world[sk->parents[b]], world[b], local[b]);
	}
	// No gun in a death: the held weapon is bone-merged onto the weapon_* bones, which no skin uses,
	// so dropping those bones far under the floor takes the gun out of the shot and leaves the hands.
	// (Hiding the weapon entity instead -- mesh-group mask 0 + a renderable refresh -- crashed CS2 in
	// UtlVectorMemory on the next render. Do not go back to it.)
	for (int b = 0; b < sk->count; ++b)
		if (sk->weaponBone[b] && !(sk->parents[b] >= 0 && sk->weaponBone[sk->parents[b]])) local[b].p[2] -= kWeaponDrop;
	if (WriteLocals(ms, sk->count, local)) { ++g_writes; g_why = "playing"; }
	else g_why = "local pose not writable";
}

} // namespace

void DeathAnimPreview_Set(const char* clip) {
	const bool on = clip && *clip && _stricmp(clip, "off") != 0;
	g_active = on;
	g_clip = on ? clip : "";
	g_startMs = GetTickCount64();
	g_why = on ? "waiting for the preview pawn" : "idle";
	// A fresh panel spawns a fresh (or recycled) pawn: re-capture its idle pose.
	g_pawns.clear();
	g_frame = 0;
}

bool DeathAnimPreview_Active() { return g_active; }

void DeathAnimPreview_RunLateFrame(bool configOpen) {
	if (!g_active) return;
	if (!configOpen) { DeathAnimPreview_Set(nullptr); return; }
	if ((g_frame++ % 30) == 0) RefreshPawnList();
	double duration = 0;
	if (!DeathAnimations_ClipPose(g_clip.c_str(), 0, nullptr, nullptr, &duration)) { g_why = "clip not found"; return; }
	const double cycle = duration + kLoopHold;
	double t = (GetTickCount64() - g_startMs) / 1000.0;
	if (cycle > 0) t = t - cycle * (double)(long long)(t / cycle);
	for (int i = 0; i < g_pawnCount; ++i) PosePawn(g_pawnList[i], t);
	if (MvmDebugLog_Active() && (g_frame % 120) == 1)
		MvmDebugLog_Linef("deathanim.view", "clip=%s t=%.2f pawns=%d writes=%llu state=%s",
			g_clip.c_str(), t, g_pawnCount, (unsigned long long)g_writes, g_why);
}

void DeathAnimPreview_PrintStatus() {
	advancedfx::Message("deathanim view: %s clip=%s pawns=%d writes=%llu (%s)\n", g_active ? "ON" : "off",
		g_clip.empty() ? "-" : g_clip.c_str(), g_pawnCount, (unsigned long long)g_writes, g_why);
}

} // namespace Filmmaker
