#include "ViewFxVmInternal.h"

#include "FollowTargetProviders.h" // EntityAt / EntityClass / Lower (external linkage helpers)
#include "../Cosmetics/CosmeticModelSwap.h" // ReadActiveViewmodelWeaponState -- proven HudModelArms
                                             // child walk (entity-list scan for "*viewmodel*" class
                                             // names finds nothing; CS2's viewmodel is the pawn's
                                             // HudModelArms child, not a standalone *ViewModel* entity)
#include "../Debug/MvmDebugLog.h" // MvmDebugLog_* -- mandatory viewmodel telemetry
#include "../../ClientEntitySystem.h"
#include "../../MirvTime.h"
#include "../../SchemaSystem.h"
#include "../../../Shared/AfxConsole.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace Filmmaker {
using namespace ViewFxVmInternal;

namespace {

// diagnostics
std::atomic<unsigned long long> g_applies{ 0 };
std::atomic<int> g_lastVmIndex{ -1 };
int g_quatOffset = -1;             // detected offset of the orientation quat inside m_nodeToWorld

// Base tracking for the non-accumulating scene-node writes (see the apply site for why). "base"
// is the engine's own pose our delta was last applied ON TOP OF; "written" is what we left in
// the field, so we can tell an engine refresh from our own leftovers.
float g_lastAngBase[2] = { 0.0f, 0.0f };
float g_lastAngWritten[2] = { 0.0f, 0.0f };
bool g_haveAngBase = false;
int g_angLastFrame = -1;
// How many scene nodes the last field-2 subtree rotation touched. 1 means the child walk found
// nothing (root only) -- which is the state that tore the viewmodel apart.
int g_dbgSubtreeNodes = 0;

// applied vs skipped duplicates. A healthy frame is 1 applied + 1 skipped; if `applied` climbs
// at roughly twice the frame rate the guard is not catching the double-apply, which IS the
// flicker. This is the signal that was missing from the probe before.
unsigned long long g_dbgApplied = 0;
unsigned long long g_dbgDupSkipped = 0;

// A world transform can be rebuilt by the engine between two calls in the same render frame.
// Caching only the root quaternion therefore cannot distinguish a duplicate call from a fresh
// child pose that happens to have the same root orientation. Keep an absolute base/output pair
// per scene node so every call is idempotent without accumulating rotation, while a genuine
// engine rewrite is rebased independently for that node.
constexpr int kMaxCachedNodes = 96;
struct NodeTransformCache {
	unsigned char* node = nullptr;
	bool valid = false;
	int lastFrame = -1;
	float basePos[3] = { 0.0f, 0.0f, 0.0f };
	float baseQuat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	float wrotePos[3] = { 0.0f, 0.0f, 0.0f };
	float wroteQuat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
};
NodeTransformCache g_nodeCaches[kMaxCachedNodes];

struct ViewmodelIdentity {
	int pawnIndex = -1;
	unsigned char* pawn = nullptr;
	int activeWeaponIndex = -1;
	void* viewmodelWeapon = nullptr;
	void* arms = nullptr;
	unsigned char* node = nullptr;
	bool povActive = false;
};
ViewmodelIdentity g_lastIdentity;
bool g_haveIdentity = false;

// Debug counters are grouped by the engine's global frame counter. The log is throttled, but
// state changes and anomalous frames are emitted immediately so a POV transition is explainable.
int g_dbgFrame = -1;
unsigned long long g_dbgFrameSerial = 0;
unsigned g_dbgFrameCalls = 0;
unsigned g_dbgFrameWorldWrites = 0;
unsigned g_dbgFrameWorldRebases = 0;
unsigned g_dbgFrameDuplicates = 0;
unsigned g_dbgFrameLargeChanges = 0;
float g_dbgLastObservedPos[3] = { 0.0f, 0.0f, 0.0f };
float g_dbgLastObservedQuat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
bool g_dbgHaveObserved = false;

// ---------------- small math ----------------
struct Quat { double x = 0, y = 0, z = 0, w = 1; };

Quat QuatFromYawPitchDeg(double yawDeg, double pitchDeg) {
	// Source convention: yaw about +Z (world up), pitch about +Y (right, after yaw). For the
	// small free-aim angles involved, composing yaw*pitch about world axes is close enough.
	const double yr = yawDeg * 3.14159265358979323846 / 360.0;   // half angles
	const double pr = pitchDeg * 3.14159265358979323846 / 360.0;
	const Quat qy{ 0.0, 0.0, std::sin(yr), std::cos(yr) };
	const Quat qp{ 0.0, std::sin(pr), 0.0, std::cos(pr) };
	// qy * qp
	Quat r;
	r.w = qy.w * qp.w - qy.x * qp.x - qy.y * qp.y - qy.z * qp.z;
	r.x = qy.w * qp.x + qy.x * qp.w + qy.y * qp.z - qy.z * qp.y;
	r.y = qy.w * qp.y - qy.x * qp.z + qy.y * qp.w + qy.z * qp.x;
	r.z = qy.w * qp.z + qy.x * qp.y - qy.y * qp.x + qy.z * qp.w;
	return r;
}

void QuatMulInto(const Quat& a, const float* b4, float* out4) { // out = a * b (b, out: x y z w)
	const double bx = b4[0], by = b4[1], bz = b4[2], bw = b4[3];
	out4[0] = (float)(a.w * bx + a.x * bw + a.y * bz - a.z * by);
	out4[1] = (float)(a.w * by - a.x * bz + a.y * bw + a.z * bx);
	out4[2] = (float)(a.w * bz + a.x * by - a.y * bx + a.z * bw);
	out4[3] = (float)(a.w * bw - a.x * bx - a.y * by - a.z * bz);
}

// ---------------- viewmodel entity + scene node ----------------
// CS2 has no standalone "*ViewModel*"-classed entity to scan for (confirmed empirically). The
// rendered first-person weapon is a child of HudModelArms. Rotate the ARMS ROOT, not only that
// weapon child: the root carries the hands and every attached weapon part together, which makes
// the complete viewmodel point toward true aim instead of twisting the gun inside static hands.
bool ResolveViewmodelTarget(ViewmodelIdentity& out) {
	out = {};
	out.povActive = g_povActive.load(std::memory_order_relaxed);
	out.pawnIndex = AfxGetSpectatedPawnIndex();
	CEntityInstance* pawn = out.pawnIndex >= 0 ? EntityAt(out.pawnIndex) : nullptr;
	if (!pawn) pawn = AfxGetLocalViewerPawn(); // demo/spectate fallback (see ClientEntitySystem.h)
	if (!pawn) return false;
	out.pawn = (unsigned char*)pawn;

	const auto weaponHandle = pawn->GetActiveWeaponHandle();
	out.activeWeaponIndex = weaponHandle.IsValid() ? weaponHandle.GetEntryIndex() : -1;

	const ptrdiff_t offNode = g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode;
	if (offNode == 0) return false;

	// Weapon class is only a HINT here -- a demo POV pawn's arms child often does not class-match
	// the world weapon, and requiring the match returns -1 (the same failure FxAlign hit, where
	// 0 of 997 samples ever resolved). ResolveViewmodelWeaponEntityIndex accepts any weapon-like
	// arms child, which IS the first-person gun.
	const char* weaponClass = nullptr;
	if (weaponHandle.IsValid()) {
		if (CEntityInstance* worldWeapon = EntityAt(weaponHandle.GetEntryIndex())) {
			const char* wc = worldWeapon->GetClassName();
			if (wc && *wc) weaponClass = wc;
		}
	}

	// The first-person viewmodel weapon is a CLIENT-ONLY entity (C_CS2HudModelWeapon, index
	// >= 0x4000). It does NOT round-trip through the entity-list index lookup, so the resolved
	// POINTER must be used -- EntityAt(vmIndex) always returns null for it. That was the bug
	// that made this whole rotation path dead: the scene node was never found, so no site or
	// field could ever have worked. See the identical note at FxAlign.cpp:265.
	void* vmRaw = nullptr;
	void* armsRaw = nullptr;
	ResolveViewmodelWeaponEntityIndex((unsigned char*)pawn, weaponClass,
		nullptr, 0, &vmRaw, &armsRaw);
	CEntityInstance* rootEnt = (CEntityInstance*)armsRaw;
	if (!rootEnt || !vmRaw) return false;
	unsigned char* node = *(unsigned char**)((unsigned char*)rootEnt + offNode);
	if (!node) return false;
	out.viewmodelWeapon = vmRaw;
	out.arms = armsRaw;
	out.node = node;
	return true;
}

unsigned char* FindViewmodelSceneNode(int* outEntityIndex) {
	if (outEntityIndex) *outEntityIndex = -1;
	ViewmodelIdentity target;
	if (!ResolveViewmodelTarget(target)) return nullptr;
	if (outEntityIndex) {
		CEntityInstance* weapon = (CEntityInstance*)target.viewmodelWeapon;
		if (weapon) {
			SOURCESDK::CS2::CBaseHandle handle = weapon->GetHandle();
			if (handle.IsValid()) *outEntityIndex = handle.GetEntryIndex();
		}
		if (*outEntityIndex < 0) *outEntityIndex = target.activeWeaponIndex;
	}
	return target.node;
}

// Auto-detect the orientation quaternion inside m_nodeToWorld: the only 4 consecutive floats
// in the transform window whose norm is ~1. (CTransformWS layout differs across dumps; the
// position triple can be validated against m_vecAbsOrigin.)
int DetectQuatOffset(unsigned char* node) {
	const ptrdiff_t offXform = g_clientDllOffsets.CGameSceneNode.m_nodeToWorld;
	if (offXform == 0) return -1;
	const float* f = (const float*)(node + offXform);
	for (int i = 0; i <= 4; ++i) { // window: 8 floats (32 bytes)
		const double n = std::sqrt((double)f[i] * f[i] + (double)f[i + 1] * f[i + 1]
			+ (double)f[i + 2] * f[i + 2] + (double)f[i + 3] * f[i + 3]);
		if (std::fabs(n - 1.0) < 0.02)
			return (int)(offXform + i * 4);
	}
	return -1;
}

// ---------------- world-transform subtree rotation ----------------
// CONFIRMED LAYOUT (mvm_debug_20260726_161635, build 10847):
//   CGameSceneNode::m_nodeToWorld = CTransformWS { VectorAligned pos (3 floats + 1.0 pad),
//                                                  Quaternion orientation }
// i.e. position at +0..11, pad at +12, quaternion at +16..31 (byte +32 from the node base here).
// Verified by matching the position triple against m_vecAbsOrigin, not by norm guessing.
//
// WHY A SUBTREE WALK IS REQUIRED. m_nodeToWorld is a WORLD transform, and each child (the arms,
// the weapon mesh, its attachments) carries its own already-baked world transform. Rotating only
// the root's quaternion leaves every child at its old world pose, so the subtree tears apart --
// observed as the viewmodel flying off screen. A world-space rotation must therefore be applied
// as a rigid body about a single pivot: the root's own origin.
//
//   child.q   = delta * child.q
//   child.pos = rootPos + delta * (child.pos - rootPos)
//
// The root itself is just the degenerate case where pos - rootPos is zero.

struct XformWS { float* pos; float* quat; };

XformWS NodeTransform(unsigned char* node) {
	XformWS t{ nullptr, nullptr };
	const ptrdiff_t off = g_clientDllOffsets.CGameSceneNode.m_nodeToWorld;
	if (off == 0 || !node) return t;
	t.pos = (float*)(node + off);
	t.quat = (float*)(node + off + 16); // confirmed: VectorAligned(16B) then Quaternion
	return t;
}

// v' = q * v * conj(q)
void QuatRotateVec(const Quat& q, const double v[3], double out[3]) {
	const double tx = 2.0 * (q.y * v[2] - q.z * v[1]);
	const double ty = 2.0 * (q.z * v[0] - q.x * v[2]);
	const double tz = 2.0 * (q.x * v[1] - q.y * v[0]);
	out[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
	out[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
	out[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
}

bool NearlyEqual(float a, float b, float epsilon = 0.0001f) {
	return std::fabs(a - b) <= epsilon;
}

bool TransformMatches(const float* aPos, const float* aQuat, const float* bPos, const float* bQuat) {
	if (!aPos || !aQuat || !bPos || !bQuat) return false;
	for (int i = 0; i < 3; ++i) if (!NearlyEqual(aPos[i], bPos[i])) return false;
	for (int i = 0; i < 4; ++i) if (!NearlyEqual(aQuat[i], bQuat[i])) return false;
	return true;
}

NodeTransformCache* FindNodeCache(unsigned char* node) {
	for (NodeTransformCache& cache : g_nodeCaches)
		if (cache.valid && cache.node == node) return &cache;
	for (NodeTransformCache& cache : g_nodeCaches) {
		if (!cache.valid) {
			cache = {};
			cache.node = node;
			cache.valid = true;
			return &cache;
		}
	}
	return nullptr;
}

void ResetTransformTracking(const char* reason) {
	for (NodeTransformCache& cache : g_nodeCaches) cache = {};
	g_haveAngBase = false;
	g_angLastFrame = -1;
	g_dbgHaveObserved = false;
	g_dbgSubtreeNodes = 0;
	if (MvmDebugLog_Active() && reason)
		MvmDebugLog_LinefAlways("viewfx.vm.reset", "reason=%s", reason);
}

// Depth-first walk of the scene-node subtree, bounded so a malformed/cyclic chain cannot hang
// the render thread. Every node is rebased independently when the engine has changed it, then
// written from that base. This is deliberately absolute, never read-modify-write accumulation.
struct CachedNodeSnapshot {
	unsigned char* node = nullptr;
	XformWS transform{ nullptr, nullptr };
	NodeTransformCache* cache = nullptr;
	bool engineRefreshed = false;
};

unsigned ApplySubtreeWorld(unsigned char* root, const Quat& delta, double outPivot[3],
	unsigned& outRebased, unsigned& outDuplicates) {
	const ptrdiff_t offChild = g_clientDllOffsets.CGameSceneNode.m_pChild;
	const ptrdiff_t offSibling = g_clientDllOffsets.CGameSceneNode.m_pNextSibling;
	if (!root) return 0;
	const int frame = g_MirvTime.framecount_get();

	CachedNodeSnapshot nodes[64];
	int count = 0;
	unsigned char* stack[64];
	int sp = 0;
	stack[sp++] = root;
	while (sp > 0 && count < 64) {
		unsigned char* node = stack[--sp];
		if (!node) continue;
		CachedNodeSnapshot& snap = nodes[count++];
		snap.node = node;
		snap.transform = NodeTransform(node);
		if (!snap.transform.pos || !snap.transform.quat) continue;
		snap.cache = FindNodeCache(node);
		if (!snap.cache) continue;
		const bool sameFrame = snap.cache->lastFrame == frame;
		if (sameFrame) {
			// The caller detour can run more than once before the engine advances its frame.
			// Never treat the intermediate pose from that second call as a new base: it may be
			// an animation overwrite of only part of the subtree. Reuse the first call's base
			// and write the same absolute result, which makes the operation idempotent.
			snap.engineRefreshed = false;
			++outDuplicates;
		} else if (snap.cache->valid && TransformMatches(snap.transform.pos, snap.transform.quat,
			snap.cache->wrotePos, snap.cache->wroteQuat)) {
			// The engine left our output in place across frames. Keep the original engine base;
			// a read-modify-write from the written value would accumulate the delta forever.
			snap.engineRefreshed = false;
			++outDuplicates;
		} else {
			snap.engineRefreshed = true;
			for (int i = 0; i < 3; ++i) snap.cache->basePos[i] = snap.transform.pos[i];
			for (int i = 0; i < 4; ++i) snap.cache->baseQuat[i] = snap.transform.quat[i];
			++outRebased;
		}
		snap.cache->lastFrame = frame;
		if (sp < 63) {
			if (offSibling != 0) {
				if (unsigned char* sibling = *(unsigned char**)(node + offSibling)) stack[sp++] = sibling;
			}
			if (offChild != 0) {
				if (unsigned char* child = *(unsigned char**)(node + offChild)) stack[sp++] = child;
			}
		}
	}

	if (count <= 0 || !nodes[0].cache) return 0;
	for (int i = 0; i < 3; ++i) outPivot[i] = nodes[0].cache->basePos[i];

	for (int i = 0; i < count; ++i) {
		CachedNodeSnapshot& snap = nodes[i];
		if (!snap.cache || !snap.transform.pos || !snap.transform.quat) continue;
		float outQuat[4];
		QuatMulInto(delta, snap.cache->baseQuat, outQuat);
		const double n2 = (double)outQuat[0] * outQuat[0] + (double)outQuat[1] * outQuat[1]
			+ (double)outQuat[2] * outQuat[2] + (double)outQuat[3] * outQuat[3];
		if (n2 > 1e-12) {
			const double inv = 1.0 / std::sqrt(n2);
			for (int q = 0; q < 4; ++q) outQuat[q] = (float)(outQuat[q] * inv);
		}
		const double rel[3] = {
			snap.cache->basePos[0] - outPivot[0],
			snap.cache->basePos[1] - outPivot[1],
			snap.cache->basePos[2] - outPivot[2] };
		double rotated[3];
		QuatRotateVec(delta, rel, rotated);
		const float outPos[3] = {
			(float)(outPivot[0] + rotated[0]),
			(float)(outPivot[1] + rotated[1]),
			(float)(outPivot[2] + rotated[2]) };
		for (int p = 0; p < 3; ++p) {
			snap.transform.pos[p] = outPos[p];
			snap.cache->wrotePos[p] = outPos[p];
		}
		for (int q = 0; q < 4; ++q) {
			snap.transform.quat[q] = outQuat[q];
			snap.cache->wroteQuat[q] = outQuat[q];
		}
	}
	return (unsigned)count;
}

bool SameIdentity(const ViewmodelIdentity& a, const ViewmodelIdentity& b) {
	return a.pawnIndex == b.pawnIndex && a.pawn == b.pawn
		&& a.activeWeaponIndex == b.activeWeaponIndex
		&& a.viewmodelWeapon == b.viewmodelWeapon && a.arms == b.arms && a.node == b.node
		&& a.povActive == b.povActive;
}

double QuatAngleDeltaDeg(const float* a, const float* b) {
	if (!a || !b) return 0.0;
	double dot = (double)a[0] * b[0] + (double)a[1] * b[1]
		+ (double)a[2] * b[2] + (double)a[3] * b[3];
	dot = std::fabs(dot);
	if (dot > 1.0) dot = 1.0;
	if (dot < -1.0) dot = -1.0;
	return 2.0 * std::acos(dot) * 180.0 / 3.14159265358979323846;
}

void BeginFrameTelemetry() {
	int frame = g_MirvTime.framecount_get();
	if (frame < 0) frame = 0;
	if (frame != g_dbgFrame) {
		if (g_dbgFrame >= 0 && MvmDebugLog_Active()
			&& (g_dbgFrameSerial % 15 == 0 || g_dbgFrameCalls > 1 || g_dbgFrameLargeChanges > 0)) {
			MvmDebugLog_LinefAlways("viewfx.vm.frame",
				"frame=%d calls=%u worldWrites=%u worldRebases=%u duplicateNodes=%u largeChanges=%u",
				g_dbgFrame, g_dbgFrameCalls, g_dbgFrameWorldWrites, g_dbgFrameWorldRebases,
				g_dbgFrameDuplicates, g_dbgFrameLargeChanges);
		}
		g_dbgFrame = frame;
		++g_dbgFrameSerial;
		g_dbgFrameCalls = 0;
		g_dbgFrameWorldWrites = 0;
		g_dbgFrameWorldRebases = 0;
		g_dbgFrameDuplicates = 0;
		g_dbgFrameLargeChanges = 0;
	}
	++g_dbgFrameCalls;
}

void UpdatePovState(bool active, int pawnIndex) {
	const bool oldActive = g_povActive.load(std::memory_order_relaxed);
	const int oldPawn = g_povPawnIndex.load(std::memory_order_relaxed);
	// A transient -1 is common while recoil/spectator interpolation updates the camera. Hold a
	// previously valid pawn through that gap; a real switch is the next valid, different index.
	const bool pawnChanged = pawnIndex >= 0 && oldPawn >= 0 && pawnIndex != oldPawn;
	const bool becameKnown = pawnIndex >= 0 && oldPawn < 0;
	if (active != oldActive || pawnChanged || becameKnown) {
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("viewfx.vm.pov",
				"changed=1 oldActive=%d newActive=%d oldPawn=%d newPawn=%d frame=%d",
				oldActive ? 1 : 0, active ? 1 : 0, oldPawn, pawnIndex, g_MirvTime.framecount_get());
		ResetTransformTracking(active ? (pawnChanged ? "spectated-pawn-change" : "pov-enter") : "pov-leave");
	}
	g_povActive.store(active, std::memory_order_relaxed);
	if (pawnIndex >= 0 || oldPawn < 0)
		g_povPawnIndex.store(pawnIndex, std::memory_order_relaxed);
}

void UpdateViewmodelIdentity(const ViewmodelIdentity& target) {
	if (!g_haveIdentity || !SameIdentity(target, g_lastIdentity)) {
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("viewfx.vm.identity",
				"changed=1 oldPawn=%d newPawn=%d oldWeapon=%d newWeapon=%d oldVm=%p newVm=%p "
				"oldArms=%p newArms=%p oldNode=%p newNode=%p pov=%d frame=%d",
				g_haveIdentity ? g_lastIdentity.pawnIndex : -1, target.pawnIndex,
				g_haveIdentity ? g_lastIdentity.activeWeaponIndex : -1, target.activeWeaponIndex,
				g_haveIdentity ? g_lastIdentity.viewmodelWeapon : nullptr, target.viewmodelWeapon,
				g_haveIdentity ? g_lastIdentity.arms : nullptr, target.arms,
				g_haveIdentity ? g_lastIdentity.node : nullptr, target.node,
				target.povActive ? 1 : 0, g_MirvTime.framecount_get());
		ResetTransformTracking(g_haveIdentity ? "viewmodel-identity-change" : "viewmodel-first-resolve");
		g_lastIdentity = target;
		g_haveIdentity = true;
	}
}

void UpdateNoViewmodelIdentity() {
	if (g_haveIdentity) {
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("viewfx.vm.identity", "changed=1 reason=unresolved frame=%d",
				g_MirvTime.framecount_get());
		ResetTransformTracking("viewmodel-unresolved");
		g_lastIdentity = {};
		g_haveIdentity = false;
	}
}

void ObserveWrittenRoot(unsigned char* node, const ViewmodelIdentity& target) {
	XformWS transform = NodeTransform(node);
	if (!transform.pos || !transform.quat) return;
	if (g_dbgHaveObserved && g_haveIdentity && SameIdentity(target, g_lastIdentity)) {
		const double dx = (double)transform.pos[0] - g_dbgLastObservedPos[0];
		const double dy = (double)transform.pos[1] - g_dbgLastObservedPos[1];
		const double dz = (double)transform.pos[2] - g_dbgLastObservedPos[2];
		const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
		const double rotation = QuatAngleDeltaDeg(transform.quat, g_dbgLastObservedQuat);
		if (distance > 8.0 || rotation > 25.0) {
			++g_dbgFrameLargeChanges;
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("viewfx.vm.anomaly",
					"frame=%d pawn=%d weapon=%d node=%p deltaPos=%.3f deltaRot=%.3f "
					"currentPos=(%.2f %.2f %.2f) currentQuat=(%.4f %.4f %.4f %.4f)",
					g_MirvTime.framecount_get(), target.pawnIndex, target.activeWeaponIndex, (void*)node,
					distance, rotation, transform.pos[0], transform.pos[1], transform.pos[2],
					transform.quat[0], transform.quat[1], transform.quat[2], transform.quat[3]);
		}
	}
	for (int i = 0; i < 3; ++i) g_dbgLastObservedPos[i] = transform.pos[i];
	for (int i = 0; i < 4; ++i) g_dbgLastObservedQuat[i] = transform.quat[i];
	g_dbgHaveObserved = true;
}

// ---------------- RE: establish the real m_nodeToWorld layout ----------------
// DetectQuatOffset guesses by norm, which cannot tell a quaternion from a rotation-matrix row.
// This resolves it from ground truth instead: m_vecAbsOrigin is a known-good world position, so
// whichever float triple inside the transform matches it IS the translation, and that fixes the
// layout. Emitted once per session (and on demand) to the mvm_debug file.
//
//   {pos,quat}   -> translation at +0,  quaternion at +16
//   {quat,pos}   -> quaternion at +0,   translation at +16
//   matrix3x4    -> translation at +3, +7, +11 (row-major, 4th column), NO quaternion anywhere
void DumpTransformLayout(unsigned char* node, const char* why) {
	if (!MvmDebugLog_Active() || !node) return;
	const ptrdiff_t offXform = g_clientDllOffsets.CGameSceneNode.m_nodeToWorld;
	const ptrdiff_t offOrigin = g_clientDllOffsets.CGameSceneNode.m_vecAbsOrigin;
	if (offXform == 0 || offOrigin == 0) {
		MvmDebugLog_Linef("viewfx.vm.layout", "%s: offsets unresolved (xform=%td origin=%td)",
			why, offXform, offOrigin);
		return;
	}

	const float* f = (const float*)(node + offXform);
	const float* o = (const float*)(node + offOrigin);

	char buf[512];
	int n = 0;
	for (int i = 0; i < 12 && n < (int)sizeof(buf) - 24; ++i)
		n += std::snprintf(buf + n, sizeof(buf) - n, "%s%.3f", i ? " " : "", f[i]);

	// Which triple equals m_vecAbsOrigin?
	int posAt = -1;
	for (int i = 0; i + 2 < 12; ++i) {
		if (std::fabs(f[i] - o[0]) < 0.05 && std::fabs(f[i + 1] - o[1]) < 0.05
			&& std::fabs(f[i + 2] - o[2]) < 0.05) { posAt = i; break; }
	}
	// Row-major matrix3x4 stores translation in the 4th column: floats 3, 7, 11.
	const bool matrix3x4 = std::fabs(f[3] - o[0]) < 0.05 && std::fabs(f[7] - o[1]) < 0.05
		&& std::fabs(f[11] - o[2]) < 0.05;

	// Norms of every 4-float window, so a matrix row (norm 1) is distinguishable from a real
	// quaternion by WHERE the unit-norm windows land.
	char norms[256]; int m = 0;
	for (int i = 0; i <= 8 && m < (int)sizeof(norms) - 16; ++i) {
		const double q = std::sqrt((double)f[i] * f[i] + (double)f[i + 1] * f[i + 1]
			+ (double)f[i + 2] * f[i + 2] + (double)f[i + 3] * f[i + 3]);
		m += std::snprintf(norms + m, sizeof(norms) - m, "%s%d:%.3f", i ? " " : "", i, q);
	}

	MvmDebugLog_Linef("viewfx.vm.layout",
		"%s xformOff=%td originOff=%td absOrigin=(%.2f %.2f %.2f) floats[0..11]=[%s] "
		"posTripleAtFloat=%d matrix3x4=%d window_norms=[%s] detectorPicked=%d",
		why, offXform, offOrigin, o[0], o[1], o[2], buf,
		posAt, matrix3x4 ? 1 : 0, norms, DetectQuatOffset(node));
}

// ---------------- the actual write ----------------
void ApplyRotation(const char* siteTag) {
	double pitch = 0.0, yaw = 0.0;
	if (g_mode.load(std::memory_order_relaxed) == 1) {
		pitch = g_freeAimPitch;
		yaw = g_freeAimYaw;
		if (pitch == 0.0 && yaw == 0.0) return;
	} else {
		yaw = g_spinDegPerApply;
		if (yaw == 0.0) return;
	}

	BeginFrameTelemetry();
	ViewmodelIdentity target;
	if (!ResolveViewmodelTarget(target)) {
		UpdateNoViewmodelIdentity();
		return;
	}
	UpdateViewmodelIdentity(target);
	unsigned char* node = target.node;
	int vmIndex = target.activeWeaponIndex;
	if (target.viewmodelWeapon) {
		CEntityInstance* vm = (CEntityInstance*)target.viewmodelWeapon;
		const SOURCESDK::CS2::CBaseHandle handle = vm->GetHandle();
		if (handle.IsValid()) vmIndex = handle.GetEntryIndex();
	}
	g_lastVmIndex.store(vmIndex, std::memory_order_relaxed);

	// One-shot layout dump, before anything is written to the transform this session.
	{
		static bool s_dumped = false;
		if (!s_dumped && MvmDebugLog_Active()) { s_dumped = true; DumpTransformLayout(node, "first-apply"); }
	}

	const int field = g_field.load(std::memory_order_relaxed);
	// The aim delta is expressed in camera/Source view-angle space. HudModelArms' LOCAL yaw
	// axis is reversed relative to that space: writing +delta makes the barrel turn away from
	// the crosshair (right-hand shot => left-facing gun). Pitch uses the same sign convention.
	const double localPitch = pitch;
	const double localYaw = -yaw;

	// NON-ACCUMULATING WRITES.
	//
	// These are read-modify-writes on PERSISTENT engine state, executed once per frame. They are
	// only correct if the engine rewrites the field from scratch every frame. It does not always
	// (the arms root keeps its node rotation between frames), so `ang = ang + delta` integrated
	// the same delta forever: the viewmodel rotated further every frame until it left the
	// frustum entirely -- the gun vanishing when the deadzone input was enabled, reappearing only
	// at a very wide FOV, and never actually tracking the input.
	//
	// Fix: remember what we wrote and what it was written on top of. If the field still holds
	// exactly our previous value, the engine did NOT refresh it, so re-apply onto the saved base
	// instead of onto our own output. If it differs, the engine produced a fresh pose and that
	// becomes the new base. Correct either way, and self-correcting if engine behaviour changes.
	bool angRefreshed = false;
	if ((field & 1) && g_clientDllOffsets.CGameSceneNode.m_angRotation != 0) {
		float* ang = (float*)(node + g_clientDllOffsets.CGameSceneNode.m_angRotation);
		double baseP = ang[0], baseY = ang[1];
		angRefreshed = true;
		const int frame = g_MirvTime.framecount_get();
		if (g_haveAngBase && g_angLastFrame == frame) {
			// The caller can be entered twice in one engine frame. Reuse the first call's base
			// even if animation rebuilt the local field between calls.
			baseP = g_lastAngBase[0];
			baseY = g_lastAngBase[1];
			angRefreshed = false;
		} else if (g_haveAngBase && NearlyEqual(ang[0], g_lastAngWritten[0])
			&& NearlyEqual(ang[1], g_lastAngWritten[1])) {
			baseP = g_lastAngBase[0];
			baseY = g_lastAngBase[1];
			angRefreshed = false; // field still holds OUR value: engine left it alone
		}
		g_lastAngBase[0] = (float)baseP; g_lastAngBase[1] = (float)baseY;
		ang[0] = (float)(baseP + localPitch);
		ang[1] = (float)(baseY + localYaw);
		g_lastAngWritten[0] = ang[0]; g_lastAngWritten[1] = ang[1];
		g_haveAngBase = true;
		g_angLastFrame = frame;
	}
	unsigned worldRebased = 0, worldDuplicates = 0, worldNodes = 0;
	if (field & 2) {
		if (g_clientDllOffsets.CGameSceneNode.m_nodeToWorld != 0) {
			if (g_quatOffset < 0)
				g_quatOffset = (int)g_clientDllOffsets.CGameSceneNode.m_nodeToWorld + 16;
			double pivot[3] = {};
			worldNodes = ApplySubtreeWorld(node, QuatFromYawPitchDeg(localYaw, localPitch),
				pivot, worldRebased, worldDuplicates);
			g_dbgSubtreeNodes = (int)worldNodes;
			g_dbgApplied += worldNodes;
			g_dbgDupSkipped += worldDuplicates;
			g_dbgFrameWorldWrites += worldNodes;
			g_dbgFrameWorldRebases += worldRebased;
			g_dbgFrameDuplicates += worldDuplicates;
		}
	}
	ObserveWrittenRoot(node, target);

	const unsigned long long n = g_applies.fetch_add(1, std::memory_order_relaxed) + 1;
	if (n <= 3 || (n % 512) == 0)
		advancedfx::Message("[viewfxvm] apply #%llu site=%s field=%d rootEnt=%d aim=(%.2f %.2f) local=(%.2f %.2f).\n",
			n, siteTag, field, vmIndex, pitch, yaw, localPitch, localYaw);

	// MANDATORY viewmodel telemetry -- see Docs/agent/mvm-debug-instrumentation.md.
	// advancedfx::Message does NOT reach a netcon client, so console output cannot be used to
	// diagnose this from outside the game. Everything needed to tell "is the gun following the
	// aim, or running away" goes to the mvm_debug FILE instead: the requested delta, the base it
	// was applied to, what was actually written, whether the engine refreshed the field between
	// frames (the runaway-accumulation tell), and where the node physically ended up.
	if (MvmDebugLog_Active()) {
		static unsigned s_vmThrottle = 0;
		if ((s_vmThrottle++ % 15) == 0) {
			float ax = 0, ay = 0, az = 0;      // node world position
			if (g_clientDllOffsets.CGameSceneNode.m_vecAbsOrigin != 0) {
				const float* o = (const float*)(node + g_clientDllOffsets.CGameSceneNode.m_vecAbsOrigin);
				ax = o[0]; ay = o[1]; az = o[2];
			}
			float ra = 0, rb = 0, rc = 0;      // node local rotation AFTER our write
			if (g_clientDllOffsets.CGameSceneNode.m_angRotation != 0) {
				const float* r = (const float*)(node + g_clientDllOffsets.CGameSceneNode.m_angRotation);
				ra = r[0]; rb = r[1]; rc = r[2];
			}
			double qn = -1.0;                  // quaternion norm: != 1 means the transform SCALES
			if (g_quatOffset >= 0) {
				const float* q = (const float*)(node + g_quatOffset);
				qn = std::sqrt((double)q[0] * q[0] + (double)q[1] * q[1]
					+ (double)q[2] * q[2] + (double)q[3] * q[3]);
			}
			MvmDebugLog_Linef("viewfx.vm",
				"frame=%d site=%s field=%d ent=%d pawn=%d weapon=%d vm=%p arms=%p node=%p "
				"offAng=%td offXform=%td quatOff=%d "
				"req=(%.3f %.3f) local=(%.3f %.3f) base=(%.3f %.3f) wrote=(%.3f %.3f) "
				"freeAim=(%.3f %.3f) "
				"angRefreshed=%d worldNodes=%u worldRebased=%u worldDuplicates=%u applied=%llu dupSkipped=%llu "
				"nodeAng=(%.3f %.3f %.3f) nodePos=(%.2f %.2f %.2f) wroteQuat=(%.4f %.4f %.4f %.4f) quatNorm=%.4f",
				g_MirvTime.framecount_get(), siteTag, field, vmIndex, target.pawnIndex, target.activeWeaponIndex,
				target.viewmodelWeapon, target.arms, (void*)node,
				g_clientDllOffsets.CGameSceneNode.m_angRotation,
				g_clientDllOffsets.CGameSceneNode.m_nodeToWorld, g_quatOffset,
				pitch, yaw, localPitch, localYaw,
				(double)g_lastAngBase[0], (double)g_lastAngBase[1],
				(double)g_lastAngWritten[0], (double)g_lastAngWritten[1],
				g_freeAimPitch, g_freeAimYaw,
				angRefreshed ? 1 : 0, worldNodes, worldRebased, worldDuplicates,
				g_dbgApplied, g_dbgDupSkipped,
				ra, rb, rc, ax, ay, az,
				(node && g_quatOffset >= 0) ? ((const float*)(node + g_quatOffset))[0] : 0.0f,
				(node && g_quatOffset >= 0) ? ((const float*)(node + g_quatOffset))[1] : 0.0f,
				(node && g_quatOffset >= 0) ? ((const float*)(node + g_quatOffset))[2] : 0.0f,
				(node && g_quatOffset >= 0) ? ((const float*)(node + g_quatOffset))[3] : 1.0f, qn);
		}
	}
}

} // namespace

namespace ViewFxVmInternal {

void SceneTransform_Apply(const char* siteTag) {
	ApplyRotation(siteTag);
}

void SceneTransform_UpdatePovState(bool active, int pawnIndex) {
	UpdatePovState(active, pawnIndex);
}

unsigned SceneTransform_ObserveCallerFrame() {
	BeginFrameTelemetry();
	ViewmodelIdentity target;
	if (ResolveViewmodelTarget(target))
		UpdateViewmodelIdentity(target);
	else
		UpdateNoViewmodelIdentity();
	return g_dbgFrameCalls;
}

void SceneTransform_DumpInfo() {
	int vmIndex = -1;
	unsigned char* node = FindViewmodelSceneNode(&vmIndex);
	if (vmIndex < 0 || !node) {
		advancedfx::Message("[viewfxvm][info] no viewmodel scene node right now.\n");
		return;
	}
	CEntityInstance* ent = EntityAt(vmIndex);
	const std::string cls = ent ? EntityClass(ent) : "?";
	advancedfx::Message("[viewfxvm][info] rootEnt=%d class=%s node=%p (HudModelArms root)\n",
		vmIndex, cls.c_str(), node);
	if (g_clientDllOffsets.CGameSceneNode.m_angRotation != 0) {
		const float* a = (const float*)(node + g_clientDllOffsets.CGameSceneNode.m_angRotation);
		advancedfx::Message("[viewfxvm][info] m_angRotation=(%.2f %.2f %.2f)\n",
			a[0], a[1], a[2]);
	}
	if (g_clientDllOffsets.CGameSceneNode.m_vecAbsOrigin != 0) {
		const float* o = (const float*)(node + g_clientDllOffsets.CGameSceneNode.m_vecAbsOrigin);
		advancedfx::Message("[viewfxvm][info] m_vecAbsOrigin=(%.2f %.2f %.2f)\n",
			o[0], o[1], o[2]);
	}
	if (g_clientDllOffsets.CGameSceneNode.m_nodeToWorld != 0) {
		const float* f = (const float*)(node + g_clientDllOffsets.CGameSceneNode.m_nodeToWorld);
		advancedfx::Message(
			"[viewfxvm][info] nodeToWorld=(%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f) quatOff=%d\n",
			f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], DetectQuatOffset(node));
	}
}

unsigned long long SceneTransform_ApplyCount() {
	return g_applies.load(std::memory_order_relaxed);
}

int SceneTransform_LastViewmodelIndex() {
	return g_lastVmIndex.load(std::memory_order_relaxed);
}

int SceneTransform_QuatOffset() {
	return g_quatOffset;
}

} // namespace ViewFxVmInternal
} // namespace Filmmaker
