#define NOMINMAX
#include "ActorAnimations.h"

#include "../Movie/ModelPoseAccess.h"
#include "../Debug/MvmDebugLog.h"
#include "../Platform/AtomicFile.h"
#include "../Platform/JsonBuilder.h"
#include "../../ClientEntitySystem.h"
#include "../../MirvTime.h"
#include "../../SchemaSystem.h"
#include "../../hlaeFolder.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../../../Shared/binutils.h"

#include <Windows.h>
#include "../../../Deps/release/Detours/src/detours.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

extern SOURCESDK::CS2::ISource2EngineToClient *g_pEngineToClient;

namespace Filmmaker::ActorAnim {
namespace {

using namespace ModelPoseAccess;

constexpr int kClientOnlyBase = 0x4000;
constexpr int kMaxHandleIndex = 0x8000;
constexpr ptrdiff_t kLocalPose = 0x18;
constexpr ptrdiff_t kLocalPoseCopy = 0x48;
constexpr ptrdiff_t kWorldPose = 0x80;
constexpr size_t kBoneStride = 32;
constexpr double kPreviewLoopHold = 0.45;
constexpr float kWeaponDrop = 4000.f;

struct SkeletonMap {
	const void *model = nullptr;
	int count = 0;
	int wpnBone = -1;
	int parents[kMaxSkeletonBones] = {};
	int boneToCore[kMaxSkeletonBones] = {};
	bool weaponBone[kMaxSkeletonBones] = {};
	bool twistBone[kMaxSkeletonBones] = {};
	bool valid = false;
};

struct RestCache {
	bool logged = false;
	bool xformLogged = false;
	uint64_t appliedGen = 0;
	uint32_t lastWeaponHandle = 0xffffffffu;
	ULONGLONG weaponSwitchUntilMs = 0;
	ULONGLONG lastArmUpdateMs = 0;
	float nativeArmBlend = 0.f;
	WeaponHoldPose grip;
	const void *model = nullptr;
	Pose restLocal[kMaxSkeletonBones];
	Pose restWorld[kMaxSkeletonBones];
	bool ready = false;
};

struct ActiveTarget {
	CEntityInstance *ent = nullptr;
	uint64_t cacheKey = 0;
	SampledPose sample;
	bool applyRootXY = false;
	bool hideWeapon = false;
	uint64_t evalGen = 0;
};

struct PerformerEntry {
	uint64_t steamId = 0;
	int controllerSlot = -1;
	int delayTicks = 0;
	bool placedActor = false;
	std::vector<Cue> cues;
};

struct RuntimeState {
	std::recursive_mutex mutex;
	std::mutex targetMutex;
	std::mutex poseMutex;
	bool loadAttempted = false;
	Library lib;

	// Local player live playback / preview
	bool localActive = false;
	bool localDirectionalMode = false;
	bool localIsWalking = false;
	bool localIsDucking = false;
	bool localUpperBody = false;
	bool localWasMoving = false;
	bool localInStop = false;
	bool localHasMovedYet = false;
	float localLastFwd = 1.f;
	float localLastRight = 0.f;
	std::string localDirFamily = "run";
	Cue localCue;
	ULONGLONG localStartMs = 0;
	bool haveLastLocalPose = false;
	SampledPose lastLocalPose;
	bool localXfadeActive = false;
	SampledPose localXfadeFrom;
	ULONGLONG localXfadeStartMs = 0;
	float localXfadeDur = 0.18f;

	// Live multi-actor Bot Takes performers
	bool sceneRunning = false;
	double sceneElapsedSec = 0.0;
	std::map<uint64_t, PerformerEntry> performers;

	// 3D Animation Picker preview
	bool previewActive = false;
	std::string previewClip;
	float previewSpeed = 1.0f;
	bool previewLoop = true;
	float previewStartTime = 0.0f;
	float previewDuration = 0.0f;
	ULONGLONG previewStartMs = 0;

	// Demo companion playback
	bool companionLoaded = false;
	std::string companionDemo;
	CompanionSequence companion;
	std::string lastAutoDemo;

	// Active pose targets consumed by the CalcWorldSpaceBones detour
	std::vector<ActiveTarget> activeTargets;
	uint64_t evalGen = 0;

	// Skeleton & rest pose caches (guarded by poseMutex)
	std::map<const void *, SkeletonMap> skeletons;
	std::map<uint64_t, RestCache> restByKey;

	// Preview pawn cache (re-scanned every 30 frames like DeathAnimPreview)
	int previewPawns[16] = {};
	int previewPawnCount = 0;
	float localSpeed = 0.f;      // inches/s, from the server pawn
	float localFwd = 0.f;        // inches/s along eye forward
	float localRight = 0.f;      // inches/s along eye right
	uint8_t localEquippedWeapon = 0xFF; // WeaponClass from server pawn's active weapon
	bool localAutoUpper = false; // auto-engaged upper-body mask when walking with a stationary clip
	double localSyncTime = 0.0;  // clip time that advances with the speed the player really moves at
	ULONGLONG localSyncMs = 0;
	ULONGLONG localIdleNextMs = 0;
	int localIdleVariant = 0;
	uint64_t frame = 0;
	uint64_t writes = 0;
} g;

bool NameEquals(const char *a, const char *b) {
	for (; *a && *b; ++a, ++b) {
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
		if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
		if (x != y) return false;
	}
	return *a == *b;
}

CEntityInstance *WideResolve(int index) {
	if (index < 0 || index >= kMaxHandleIndex || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	__try { return (CEntityInstance *)g_GetEntityFromIndex(*g_pEntityList, index); }
	__except (1) { return nullptr; }
}

bool IsPreviewPawn(CEntityInstance *e) {
	if (!e) return false;
	__try {
		const char *cls = e->GetClassName();
		return cls && std::strstr(cls, "previewplayer") != nullptr;
	} __except (1) { return false; }
}

bool ReadSkeletonRaw(const void *model, SkeletonMap &sk) {
	const char *const *names = nullptr;
	const int16_t *parents = nullptr;
	if (!ReadTablePointers(model, names, parents)) return false;
	int n = 0;
	int coreFound = 0;
	sk.wpnBone = -1;
	for (; n < kMaxSkeletonBones; ++n) {
		char name[64];
		int parent = 0;
		if (!ReadBoneEntry(names, parents, n, name, sizeof(name), parent)) break;
		if (parent >= n || parent < -1) break;
		sk.parents[n] = parent;
		sk.boneToCore[n] = -1;
		if (NameEquals(name, "wpn")) sk.wpnBone = n;
		else if (sk.wpnBone < 0 && NameEquals(name, "weapon_hand_R")) sk.wpnBone = n;
		sk.weaponBone[n] = NameEquals(name, "wpn") || NameEquals(name, "wpnPivot") ||
		                   ((name[0] == 'w' || name[0] == 'W') && _strnicmp(name, "weapon", 6) == 0);
		sk.twistBone[n] = (std::strstr(name, "TWIST") != nullptr || std::strstr(name, "twist") != nullptr);
		for (int c = 0; c < kCoreBones; ++c) {
			if (!g.lib.boneNames[c].empty() && NameEquals(name, g.lib.boneNames[c].c_str())) {
				sk.boneToCore[n] = c;
				++coreFound;
				break;
			}
		}
	}
	sk.count = n;
	sk.valid = (n >= kCoreBones && coreFound >= 15);
	return sk.valid;
}

const SkeletonMap *SkeletonOf(const void *model) {
	if (g.lib.boneNames[0].empty()) return nullptr;
	auto it = g.skeletons.find(model);
	if (it != g.skeletons.end()) return it->second.valid ? &it->second : nullptr;
	SkeletonMap &sk = g.skeletons[model];
	sk.model = model;
	if (!ReadSkeletonRaw(model, sk)) { sk.valid = false; return nullptr; }
	return &sk;
}

bool ReadPoseBuffer(unsigned char *ms, ptrdiff_t off, int count, Pose *out) {
	__try {
		const unsigned char *arr = *(const unsigned char **)(ms + off);
		if ((uintptr_t)arr < 0x10000) return false;
		for (int b = 0; b < count; ++b) {
			const float *r = (const float *)(arr + b * kBoneStride);
			for (int k = 0; k < 3; ++k) out[b].p[k] = r[k];
			for (int k = 0; k < 4; ++k) out[b].q[k] = r[4 + k];
		}
		return true;
	} __except (1) { return false; }
}

// Test switches, set with `mirv_filmmaker takes anim dbg`: bit 1 skips the world-pose write, bit 2 the
// second local copy. g_dbgMod/g_dbgK pose only every Nth late-frame call.
int g_dbgMask = 0, g_dbgMod = 1, g_dbgK = 0;

bool WritePoseBuffers(unsigned char *ms, int count, const Pose *localIn, const Pose *worldIn) {
	__try {
		if (localIn) {
			for (ptrdiff_t off : {kLocalPose, kLocalPoseCopy}) {
				if (off == kLocalPoseCopy && (g_dbgMask & 2)) continue;
				unsigned char *arr = *(unsigned char **)(ms + off);
				if ((uintptr_t)arr < 0x10000) return false;
				for (int b = 0; b < count; ++b) {
					float *r = (float *)(arr + b * kBoneStride);
					for (int k = 0; k < 3; ++k) r[k] = localIn[b].p[k];
					r[3] = 1.0f;
					for (int k = 0; k < 4; ++k) r[4 + k] = localIn[b].q[k];
				}
			}
		}
		if (worldIn && !(g_dbgMask & 1)) {
			unsigned char *warr = *(unsigned char **)(ms + kWorldPose);
			if ((uintptr_t)warr < 0x10000) return false;
			for (int b = 0; b < count; ++b) {
				float *r = (float *)(warr + b * kBoneStride);
				for (int k = 0; k < 3; ++k) r[k] = worldIn[b].p[k];
				r[3] = 1.0f;
				for (int k = 0; k < 4; ++k) r[4 + k] = worldIn[b].q[k];
			}
		}
		return true;
	} __except (1) { return false; }
}

bool ExtractWorldTransform(CEntityInstance *ent, Pose &xform) {
	const auto &o = g_clientDllOffsets;
	if (!ent || !o.C_BaseEntity.m_pGameSceneNode || !o.CGameSceneNode.m_nodeToWorld) return false;
	__try {
		unsigned char *node = *(unsigned char **)((unsigned char *)ent + o.C_BaseEntity.m_pGameSceneNode);
		if ((uintptr_t)node < 0x10000) return false;
		const float *x = (const float *)(node + o.CGameSceneNode.m_nodeToWorld);
		const float qn2 = x[4] * x[4] + x[5] * x[5] + x[6] * x[6] + x[7] * x[7];
		if (!std::isfinite(x[0]) || !std::isfinite(x[1]) || !std::isfinite(x[2]) || qn2 <= 0.5f || qn2 >= 2.0f)
			return false;
		for (int k = 0; k < 3; ++k) xform.p[k] = x[k];
		for (int k = 0; k < 4; ++k) xform.q[k] = x[4 + k];
		QuatNorm(xform.q, xform.q);
		return true;
	} __except (1) { return false; }
}

static bool ReadEyeYawDeg(CEntityInstance *ent, float &outYawDeg) {
	const ptrdiff_t off = g_clientDllOffsets.C_CSPlayerPawn.m_angEyeAngles;
	if (!ent || !off) return false;
	__try {
		const float *ang = reinterpret_cast<const float *>(reinterpret_cast<const unsigned char *>(ent) + off);
		if (!std::isfinite(ang[1])) return false;
		outYawDeg = ang[1];
		return true;
	} __except (1) { return false; }
}

static uint8_t ClassifyClientWeaponName(const char *cls) {
	if (!cls || !cls[0]) return 0xFF;
	if (std::strcmp(cls, "weapon_glock") == 0 || std::strcmp(cls, "weapon_hkp2000") == 0 ||
	    std::strcmp(cls, "weapon_usp_silencer") == 0 || std::strcmp(cls, "weapon_p250") == 0 ||
	    std::strcmp(cls, "weapon_elite") == 0 || std::strcmp(cls, "weapon_fiveseven") == 0 ||
	    std::strcmp(cls, "weapon_tec9") == 0 || std::strcmp(cls, "weapon_cz75a") == 0 ||
	    std::strcmp(cls, "weapon_deagle") == 0 || std::strcmp(cls, "weapon_revolver") == 0)
		return uint8_t(WeaponClass::Pistol);
	if (std::strcmp(cls, "weapon_hegrenade") == 0 || std::strcmp(cls, "weapon_flashbang") == 0 ||
	    std::strcmp(cls, "weapon_smokegrenade") == 0 || std::strcmp(cls, "weapon_molotov") == 0 ||
	    std::strcmp(cls, "weapon_incgrenade") == 0 || std::strcmp(cls, "weapon_decoy") == 0 ||
	    std::strcmp(cls, "weapon_knife") == 0 || std::strcmp(cls, "weapon_bayonet") == 0 ||
	    std::strcmp(cls, "weapon_c4") == 0 || std::strcmp(cls, "weapon_taser") == 0)
		return uint8_t(WeaponClass::Grenade);
	if (std::strncmp(cls, "weapon_", 7) == 0) return uint8_t(WeaponClass::Rifle);
	return 0xFF;
}

static void ReadClientWeaponState(CEntityInstance *pawn, uint32_t &outHandle, uint8_t &outClass, bool &outInReload) {
	outHandle = 0xffffffffu;
	outClass = 0xFF;
	outInReload = false;
	const auto &o = g_clientDllOffsets;
	if (!pawn || !o.C_BasePlayerPawn.m_pWeaponServices || !o.CPlayer_WeaponServices.m_hActiveWeapon) return;
	__try {
		const unsigned char *ws =
		    *(const unsigned char *const *)((const unsigned char *)pawn + o.C_BasePlayerPawn.m_pWeaponServices);
		if ((uintptr_t)ws < 0x10000) return;
		const uint32_t h = *(const uint32_t *)(ws + o.CPlayer_WeaponServices.m_hActiveWeapon);
		outHandle = h;
		if (h == 0xffffffffu || h == 0) return;
		CEntityInstance *wpn = WideResolve(int(h & 0x7fffu));
		if (!wpn) return;
		outClass = ClassifyClientWeaponName(wpn->GetClassName());
		if (o.C_CSWeaponBase.m_bInReload) {
			outInReload = *(const uint8_t *)((const unsigned char *)wpn + o.C_CSWeaponBase.m_bInReload) != 0;
		}
	} __except (1) {}
}

bool ApplyPoseToSkeletonInstance(CEntityInstance *ent, const ActiveTarget &at, RestCache &rc, bool forceWriteLocal) {
	const SampledPose &sample = at.sample;
	if (!ent || !sample.active || !g.lib.hasBind) return false;
	unsigned char *ms = ModelStateOf(ent);
	const void *model = ModelOf(ms);
	if (!ms || !model) return false;
	const SkeletonMap *sk = SkeletonOf(model);
	if (!sk) return false;

	Pose liveLocal[kMaxSkeletonBones];
	if (!ReadPoseBuffer(ms, kLocalPose, sk->count, liveLocal)) return false;

	if (!rc.ready || rc.model != model) {
		rc.model = model;
		rc.grip = WeaponHoldPose{};
		rc.appliedGen = 0;
		for (int i = 0; i < sk->count; ++i) {
			rc.restLocal[i] = liveLocal[i];
			const int p = sk->parents[i];
			if (p >= 0 && p < i) ComposePose(rc.restWorld[p], rc.restLocal[i], rc.restWorld[i]);
			else rc.restWorld[i] = rc.restLocal[i];
		}
		rc.ready = true;
	}

	const ULONGLONG now = GetTickCount64();
	const float dtArm = rc.lastArmUpdateMs ? std::clamp(float(now - rc.lastArmUpdateMs) * 0.001f, 0.f, 0.1f) : 0.016f;
	rc.lastArmUpdateMs = now;
	uint32_t curWpnH = 0xffffffffu;
	uint8_t curWpnCls = 0xFF;
	bool inReload = false;
	ReadClientWeaponState(ent, curWpnH, curWpnCls, inReload);
	if (rc.lastWeaponHandle != 0xffffffffu && curWpnH != rc.lastWeaponHandle) {
		rc.weaponSwitchUntilMs = now + 950ULL;
		rc.grip.valid = false;
	}
	rc.lastWeaponHandle = curWpnH;
	const bool wantNativeArms = !at.hideWeapon && (inReload || now < rc.weaponSwitchUntilMs);
	const float armTarget = wantNativeArms ? 1.f : 0.f;
	const float armRate = wantNativeArms ? 12.f : 4.5f;
	if (rc.nativeArmBlend < armTarget) rc.nativeArmBlend = std::min(armTarget, rc.nativeArmBlend + armRate * dtArm);
	else if (rc.nativeArmBlend > armTarget) rc.nativeArmBlend = std::max(armTarget, rc.nativeArmBlend - armRate * dtArm);

	if (!at.hideWeapon && sk->wpnBone >= 0) {
		Pose freshWorld[kMaxSkeletonBones];
		int handLIdx = -1, handRIdx = -1;
		for (int i = 0; i < sk->count; ++i) {
			const int p = sk->parents[i];
			if (p >= 0 && p < i) ComposePose(freshWorld[p], liveLocal[i], freshWorld[i]);
			else freshWorld[i] = liveLocal[i];
			if (sk->boneToCore[i] == kCoreHandL) handLIdx = i;
			else if (sk->boneToCore[i] == kCoreHandR) handRIdx = i;
		}
		if (handRIdx >= 0) {
			auto dist3 = [](const float a[3], const float b[3]) {
				const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
				return std::sqrt(dx * dx + dy * dy + dz * dz);
			};
			const float dRW = dist3(freshWorld[handRIdx].p, freshWorld[sk->wpnBone].p);
			if (dRW < 24.f) {
				WeaponHoldPose gpose;
				gpose.valid = true;
				gpose.twoHanded = false;
				gpose.isMoving = (at.cacheKey == 0x10CA1ULL) ? (g.localSpeed > 8.f) : false;
				gpose.nativeArmBlend = rc.nativeArmBlend;
				gpose.equippedWeaponClass = (curWpnCls != 0xFF) ? curWpnCls
				                                                : ((at.cacheKey == 0x10CA1ULL) ? g.localEquippedWeapon : 0xFF);
				gpose.wpnBone = sk->wpnBone;
				gpose.twistBones = sk->twistBone;
				LocalFromWorld(freshWorld[handRIdx], freshWorld[sk->wpnBone], gpose.handRToWpn);
				if (handLIdx >= 0) {
					LocalFromWorld(freshWorld[sk->wpnBone], freshWorld[handLIdx], gpose.wpnToHandL);
					const float dRL = dist3(freshWorld[handRIdx].p, freshWorld[handLIdx].p);
					const float dLW = dist3(freshWorld[handLIdx].p, freshWorld[sk->wpnBone].p);
					// Require wider hand spacing (>= 6.5") so a cupped two-hand pistol hold isn't mistreated as rifle foregrip.
					gpose.twoHanded = (dRL >= 6.5f && dRL <= 24.f && dLW <= 20.f);
				}
				for (int i = 0; i < sk->count; ++i) {
					const int core = sk->boneToCore[i];
					if (core >= kCoreFingerLStart && core <= kCoreFingerREnd) {
						const int fIdx = core - kCoreFingerLStart;
						gpose.fingerLocal[fIdx] = liveLocal[i];
						gpose.hasFingerLocal[fIdx] = true;
					}
				}
				rc.grip = gpose;
			}
		}
	}

	WeaponHoldPose activeHold = rc.grip;
	activeHold.isMoving = (at.cacheKey == 0x10CA1ULL) ? (g.localSpeed > 8.f) : false;
	activeHold.nativeArmBlend = rc.nativeArmBlend;
	activeHold.equippedWeaponClass = (curWpnCls != 0xFF) ? curWpnCls
	                                                     : ((at.cacheKey == 0x10CA1ULL) ? g.localEquippedWeapon : 0xFF);
	activeHold.wpnBone = sk->wpnBone;
	activeHold.twistBones = sk->twistBone;

	Pose workLocal[kMaxSkeletonBones];
	for (int i = 0; i < sk->count; ++i) workLocal[i] = liveLocal[i];
	Pose outWorld[kMaxSkeletonBones];
	ApplyPoseToSkeleton(sk->count, sk->parents, sk->boneToCore, g.lib.csBind, rc.restLocal,
	                    workLocal, outWorld, sample, at.applyRootXY,
	                    nullptr, nullptr,
	                    (!at.hideWeapon && sk->wpnBone >= 0) ? &activeHold : nullptr);

	if (at.hideWeapon) {
		for (int i = 0; i < sk->count; ++i)
			if (sk->weaponBone[i]) { workLocal[i].p[2] -= kWeaponDrop; outWorld[i].p[2] -= kWeaponDrop; }
	}

	for (int i = 0; i < sk->count; ++i) {
		const Pose &o = outWorld[i];
		float n2 = 0.f;
		bool ok = true;
		for (int k = 0; k < 3; ++k) ok = ok && std::isfinite(o.p[k]) && std::fabs(o.p[k]) < 1.0e5f;
		for (int k = 0; k < 4; ++k) { ok = ok && std::isfinite(o.q[k]); n2 += o.q[k] * o.q[k]; }
		if (!ok || n2 < 0.5f || n2 > 2.0f) return false;
	}

	Pose xform;
	bool haveXform = ExtractWorldTransform(ent, xform);
	Pose engineWorld[kMaxSkeletonBones];
	if (!haveXform && ReadPoseBuffer(ms, kWorldPose, sk->count, engineWorld)) {
		Pose liveModel[kMaxSkeletonBones];
		for (int i = 0; i < sk->count; ++i) {
			const int p = sk->parents[i];
			if (p >= 0 && p < i) ComposePose(liveModel[p], liveLocal[i], liveModel[i]);
			else liveModel[i] = liveLocal[i];
		}
		float inv[4];
		QuatConj(liveModel[0].q, inv);
		QuatMul(engineWorld[0].q, inv, xform.q);
		QuatNorm(xform.q, xform.q);
		float rp[3];
		QuatRotate(xform.q, liveModel[0].p, rp);
		for (int k = 0; k < 3; ++k) xform.p[k] = engineWorld[0].p[k] - rp[k];
		haveXform = true;
	}

	Pose outGlobal[kMaxSkeletonBones];
	if (haveXform) {
		// Align local player world yaw to eye yaw when full-body HL:A pose drives legs so the
		// character always faces straight along movement instead of inheriting CS2 aim-turn lag.
		if (at.cacheKey == 0x10CA1ULL && !sample.upperBodyOnly) {
			float eyeYawDeg = 0.f;
			if (ReadEyeYawDeg(ent, eyeYawDeg)) {
				const float qx = xform.q[0], qy = xform.q[1], qz = xform.q[2], qw = xform.q[3];
				const float xformYaw = std::atan2(2.f * (qw * qz + qx * qy), 1.f - 2.f * (qy * qy + qz * qz));
				const float targetYaw = eyeYawDeg * 0.017453292519943295f;
				float dYaw = std::fmod(targetYaw - xformYaw, 6.283185307179586f);
				if (dYaw > 3.141592653589793f) dYaw -= 6.283185307179586f;
				if (dYaw < -3.141592653589793f) dYaw += 6.283185307179586f;
				const float qYaw[4] = {0.f, 0.f, std::sin(dYaw * 0.5f), std::cos(dYaw * 0.5f)};
				float aligned[4];
				QuatMul(qYaw, xform.q, aligned);
				QuatNorm(aligned, xform.q);
			}
		}
		for (int i = 0; i < sk->count; ++i) {
			float rp[3];
			QuatRotate(xform.q, outWorld[i].p, rp);
			for (int k = 0; k < 3; ++k) outGlobal[i].p[k] = xform.p[k] + rp[k];
			QuatMul(xform.q, outWorld[i].q, outGlobal[i].q);
			QuatNorm(outGlobal[i].q, outGlobal[i].q);
		}
	}

	const Pose *localWrite = (forceWriteLocal || !haveXform) ? workLocal : nullptr;
	if (!WritePoseBuffers(ms, sk->count, localWrite, haveXform ? outGlobal : nullptr)) return false;
	rc.appliedGen = at.evalGen;
	++g.writes;
	return true;
}

using CalcWorldSpaceBones_t = void(__fastcall *)(void *skel, uint32_t boneMask);
using InvalidateWorldBones_t = void(__fastcall *)(void *skel);

void **g_skelVtable = nullptr;
CalcWorldSpaceBones_t g_origCalcWorldSpaceBones = nullptr;
InvalidateWorldBones_t g_fnInvalidateWorldBones = nullptr;
ptrdiff_t g_worldMaskOff = 0x458;
bool g_boneHookAttempted = false;
bool g_boneHookOk = false;

uint32_t SafeReadU32(const void *base, ptrdiff_t off) {
	if (!base) return 0;
	__try { return *reinterpret_cast<const uint32_t *>(static_cast<const unsigned char *>(base) + off); }
	__except (1) { return 0; }
}

CEntityInstance *SafeReadOwner(const void *node) {
	const ptrdiff_t off = g_clientDllOffsets.CGameSceneNode.m_pOwner;
	if (!node || !off) return nullptr;
	__try { return *reinterpret_cast<CEntityInstance *const *>(static_cast<const unsigned char *>(node) + off); }
	__except (1) { return nullptr; }
}

void *SafeReadSkeletonNode(CEntityInstance *ent) {
	const auto &o = g_clientDllOffsets.ModelChain;
	if (!ent || !o.m_CBodyComponent || !o.m_skeletonInstance) return nullptr;
	__try {
		unsigned char *body = *reinterpret_cast<unsigned char *const *>(reinterpret_cast<const unsigned char *>(ent) + o.m_CBodyComponent);
		if ((uintptr_t)body < 0x10000) return nullptr;
		return body + o.m_skeletonInstance;
	} __except (1) { return nullptr; }
}

bool SafeVtableEquals(const void *obj, const void *const *vt) {
	if (!obj || !vt) return false;
	__try { return *reinterpret_cast<const void *const *const *>(obj) == vt; }
	__except (1) { return false; }
}

void SafeInvalidateSkel(void *node) {
	if (!node || !g_fnInvalidateWorldBones) return;
	__try { g_fnInvalidateWorldBones(node); }
	__except (1) {}
}

void InvalidateSkeletonAndWeaponChildren(CEntityInstance *pawn) {
	if (!g_boneHookOk || !g_fnInvalidateWorldBones || !pawn) return;
	if (void *skelNode = SafeReadSkeletonNode(pawn)) {
		if (SafeVtableEquals(skelNode, g_skelVtable)) SafeInvalidateSkel(skelNode);
	}
	const auto &o = g_clientDllOffsets;
	if (!o.C_BaseEntity.m_pGameSceneNode || !o.CGameSceneNode.m_pChild || !o.CGameSceneNode.m_pNextSibling || !o.CGameSceneNode.m_pOwner)
		return;
	__try {
		unsigned char *node = *(unsigned char **)((unsigned char *)pawn + o.C_BaseEntity.m_pGameSceneNode);
		if ((uintptr_t)node < 0x10000) return;
		unsigned char *child = *(unsigned char **)(node + o.CGameSceneNode.m_pChild);
		for (int guard = 0; (uintptr_t)child >= 0x10000 && guard < 32;
		     child = *(unsigned char **)(child + o.CGameSceneNode.m_pNextSibling), ++guard) {
			CEntityInstance *owner = *(CEntityInstance **)(child + o.CGameSceneNode.m_pOwner);
			if (!owner) continue;
			const char *cls = owner->GetClassName();
			if (cls && std::strncmp(cls, "weapon_", 7) == 0) {
				if (void *wSkel = SafeReadSkeletonNode(owner)) {
					if (SafeVtableEquals(wSkel, g_skelVtable)) SafeInvalidateSkel(wSkel);
				}
			}
		}
	} __except (1) {}
}

void __fastcall New_CalcWorldSpaceBones(void *skel, uint32_t boneMask) {
	const uint32_t beforeMask = SafeReadU32(skel, g_worldMaskOff);
	g_origCalcWorldSpaceBones(skel, boneMask);

	CEntityInstance *owner = SafeReadOwner(skel);
	if (!owner) return;

	ActiveTarget target;
	bool found = false;
	{
		std::lock_guard<std::mutex> tlk(g.targetMutex);
		for (const ActiveTarget &t : g.activeTargets) {
			if (t.ent == owner) {
				target = t;
				found = true;
				break;
			}
		}
	}
	if (!found || !target.sample.active || target.sample.weight <= 1e-4f) return;

	const uint32_t req = boneMask & 0xfffffu;
	const bool rebuilt = (beforeMask & req) != req;

	std::lock_guard<std::mutex> plk(g.poseMutex);
	RestCache &rc = g.restByKey[target.cacheKey];
	if (!rebuilt && rc.appliedGen == target.evalGen) return;
	ApplyPoseToSkeletonInstance(owner, target, rc, false);
}

bool ResolveBoneHookTargets(HMODULE clientDll, void **&outVtable, CalcWorldSpaceBones_t &outCalc,
                            InvalidateWorldBones_t &outInv, ptrdiff_t &outMaskOff) {
	outVtable = nullptr;
	outCalc = nullptr;
	outInv = nullptr;
	outMaskOff = 0x458;
	__try {
		void **vt = (void **)Afx::BinUtils::FindClassVtable(clientDll, ".?AVCSkeletonInstance@@", 0, 0x0);
		if (!vt) return false;
		const uint8_t *slot18 = reinterpret_cast<const uint8_t *>(vt[18]);
		const uint8_t *slot33 = reinterpret_cast<const uint8_t *>(vt[33]);
		if (!slot18 || !slot33) return false;

		for (int i = 0; i + 9 < 32; ++i) {
			if (slot18[i] == 0xC7 && slot18[i + 1] == 0x81) {
				uint32_t off = 0, val = 1;
				std::memcpy(&off, slot18 + i + 2, 4);
				std::memcpy(&val, slot18 + i + 6, 4);
				if (off >= 0x200 && off <= 0x800 && val == 0) {
					outMaskOff = ptrdiff_t(off);
					break;
				}
			}
		}

		uintptr_t calcAddr = 0;
		for (int i = 0; i + 8 < 0x180; ++i) {
			if (slot33[i] == 0xC1 && slot33[i + 1] == 0xE2 && slot33[i + 2] == 0x0A && slot33[i + 3] == 0xE8) {
				int32_t rel = 0;
				std::memcpy(&rel, slot33 + i + 4, 4);
				calcAddr = reinterpret_cast<uintptr_t>(slot33 + i + 8 + rel);
				break;
			}
		}
		if (!calcAddr) return false;
		outVtable = vt;
		outInv = reinterpret_cast<InvalidateWorldBones_t>(vt[18]);
		outCalc = reinterpret_cast<CalcWorldSpaceBones_t>(calcAddr);
		return true;
	} __except (1) { return false; }
}

bool EnsureBoneHook() {
	if (g_boneHookAttempted) return g_boneHookOk;
	g_boneHookAttempted = true;
	HMODULE clientDll = GetModuleHandleA("client.dll");
	if (!clientDll) return false;
	void **vt = nullptr;
	CalcWorldSpaceBones_t calcFn = nullptr;
	InvalidateWorldBones_t invFn = nullptr;
	ptrdiff_t maskOff = 0x458;
	if (!ResolveBoneHookTargets(clientDll, vt, calcFn, invFn, maskOff)) return false;

	g_skelVtable = vt;
	g_fnInvalidateWorldBones = invFn;
	g_worldMaskOff = maskOff;
	g_origCalcWorldSpaceBones = calcFn;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID &)g_origCalcWorldSpaceBones, New_CalcWorldSpaceBones);
	if (NO_ERROR == DetourTransactionCommit()) {
		g_boneHookOk = true;
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("actoranim", "CalcWorldSpaceBones hook installed (maskOff=0x%tx)", g_worldMaskOff);
	}
	return g_boneHookOk;
}

std::filesystem::path CompanionDir() {
	wchar_t appData[MAX_PATH] = {};
	if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH))
		return std::filesystem::path(appData) / L"HLAE" / L"source_mvm" / L"takes" / L"demo_companion";
	return std::filesystem::path(GetHlaeFolderW()) / L"takes" / L"demo_companion";
}

std::string NormalizeDemoStem(const std::string &name) {
	std::filesystem::path p(name);
	std::string stem = p.stem().string();
	std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	return stem;
}

bool ReadSteamAndPawn(CEntityInstance *ctrl, uint64_t &steamId, CEntityInstance *&pawn) {
	steamId = 0;
	pawn = nullptr;
	const auto &o = g_clientDllOffsets;
	if (!ctrl || !o.CBasePlayerController.m_steamID) return false;
	__try {
		const char *cls = ctrl->GetClassName();
		if (!cls || std::strstr(cls, "PlayerController") == nullptr) return false;
		steamId = *(const uint64_t *)((const unsigned char *)ctrl + o.CBasePlayerController.m_steamID);
		uint32_t handle = 0xffffffffu;
		if (o.CCSPlayerController.m_hPlayerPawn)
			handle = *(const uint32_t *)((const unsigned char *)ctrl + o.CCSPlayerController.m_hPlayerPawn);
		if ((handle == 0xffffffffu || handle == 0) && o.CBasePlayerController.m_hPawn)
			handle = *(const uint32_t *)((const unsigned char *)ctrl + o.CBasePlayerController.m_hPawn);
		if (handle != 0xffffffffu && handle != 0)
			pawn = WideResolve(int(handle & 0x7fffu));
		return pawn != nullptr;
	} __except (1) { return false; }
}

CEntityInstance *FindLocalPlayerPawn() {
	return AfxGetLocalViewerPawn();
}

std::vector<std::filesystem::path> CandidatePackPaths() {
	std::vector<std::filesystem::path> out;
	const std::wstring hlae = GetHlaeFolderW();
	if (!hlae.empty())
		out.push_back(std::filesystem::path(hlae) / L"resources" / L"animations" / L"actor_animations.mvmanim");
	HMODULE selfMod = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       (LPCWSTR)&CandidatePackPaths, &selfMod)) {
		wchar_t buf[MAX_PATH] = {};
		if (GetModuleFileNameW(selfMod, buf, MAX_PATH)) {
			const auto dir = std::filesystem::path(buf).parent_path();
			out.push_back(dir / L"resources" / L"animations" / L"actor_animations.mvmanim");
			out.push_back(dir.parent_path() / L"resources" / L"animations" / L"actor_animations.mvmanim");
		}
	}
	return out;
}

} // namespace

bool EnsureLoaded() {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	if (g.loadAttempted) return !g.lib.clips.empty();
	g.loadAttempted = true;
	for (const auto &p : CandidatePackPaths()) {
		std::ifstream f(p, std::ios::binary);
		if (!f) continue;
		std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		if (LoadPack(bytes.data(), bytes.size(), g.lib)) {
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("actoranim", "loaded %zu clips from %s", g.lib.clips.size(), p.string().c_str());
			return true;
		}
	}
	g.lib.status = "pack not found (run FX/tools/extract-alyx-anims.py)";
	return false;
}

const Library &GetLibrary() {
	EnsureLoaded();
	return g.lib;
}

namespace {
std::string g_pendingScript;
}
void QueueScript(const std::string &js) { g_pendingScript = js; }
bool TakeScript(std::string &js) {
	if (g_pendingScript.empty()) return false;
	js.swap(g_pendingScript);
	g_pendingScript.clear();
	return true;
}

std::string CatalogJson() {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	EnsureLoaded();
	JsonBuilder j;
	j.BeginArray();
	for (const auto &c : g.lib.clips) {
		j.BeginArray();
		j.ValueString(c.id);
		j.ValueString(c.displayName);
		j.ValueInt(int(c.category));
		j.ValueDouble(c.Duration());
		j.ValueInt(c.loopable ? 1 : 0);
		j.ValueInt(c.hasRootMotion ? 1 : 0);
		j.ValueInt(int(c.weaponClass));
		j.ValueDouble(c.rootSpeed);
		j.EndArray();
	}
	j.EndArray();
	return j.Str();
}

bool PlayLocal(const Cue &cue) {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	if (!EnsureLoaded()) return false;
	const Clip *c = g.lib.Find(cue.clip);
	if (!c) return false;
	const ULONGLONG now = GetTickCount64();
	const bool wasActive = g.localActive && g.haveLastLocalPose;
	if (wasActive) {
		g.localXfadeFrom = g.lastLocalPose;
		g.localXfadeDur = 0.18f;
		g.localXfadeStartMs = now;
		g.localXfadeActive = true;
	} else {
		g.localXfadeActive = false;
	}
	g.localCue = cue;
	g.localCue.startTick = 0;
	if (cue.upperBodyOnly) g.localUpperBody = true;
	g.localCue.upperBodyOnly = cue.upperBodyOnly || g.localUpperBody;
	if (wasActive) {
		g.localCue.blendIn = 0.f;
	} else if (g.localCue.blendIn <= 0.f && !g.localCue.upperBodyOnly) {
		g.localCue.blendIn = std::min(0.14f, c->Duration() * 0.25f);
	}
	if (!g.localCue.loop && g.localCue.blendOut <= 0.f) {
		g.localCue.blendOut = std::min(0.22f, c->Duration() * 0.35f);
	}
	g.localDirectionalMode = !g.localCue.upperBodyOnly && IsDirectionalBaseClip(cue.clip);
	if (cue.clip == "hla_sup_sprint_fwd") g.localDirFamily = "sup";
	else if (cue.clip == "hla_heavy_run_fwd") g.localDirFamily = "heavy";
	else if (cue.clip == "hla_mc_walk_fwd") g.localDirFamily = "mc_walk";
	else if (cue.clip == "hla_mc_run_fwd") g.localDirFamily = "mc_run";
	else if (cue.clip == "hla_walk_fwd") g.localDirFamily = "walk";
	else if (cue.clip == "hla_crouch_walk_fwd") g.localDirFamily = "crouch";
	else g.localDirFamily = "run";
	g.localWasMoving = false;
	g.localInStop = false;
	g.localHasMovedYet = false;
	g.localAutoUpper = false;
	g.localIdleVariant = 0;
	g.localIdleNextMs = now + 5200ULL;
	g.localStartMs = now;
	g.localSyncTime = 0.0;
	g.localSyncMs = now;
	g.localActive = true;
	return true;
}

void StopLocal() {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	g.localActive = false;
	g.localDirectionalMode = false;
	g.localWasMoving = false;
	g.localInStop = false;
	g.localHasMovedYet = false;
	g.localXfadeActive = false;
	g.haveLastLocalPose = false;
	{
		std::lock_guard<std::mutex> tlk(g.targetMutex);
		g.activeTargets.erase(
		    std::remove_if(g.activeTargets.begin(), g.activeTargets.end(),
		                   [](const ActiveTarget &t) { return t.cacheKey == 0x10CA1ULL; }),
		    g.activeTargets.end());
	}
	if (CEntityInstance *localPawn = FindLocalPlayerPawn())
		InvalidateSkeletonAndWeaponChildren(localPawn);
}

bool LocalActive() {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	return g.localActive;
}

std::string LocalClip() {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	return g.localActive ? g.localCue.clip : std::string();
}

void SetLocalUpperBody(bool upperBody) {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	g.localUpperBody = upperBody;
	if (g.localActive) {
		const ULONGLONG now = GetTickCount64();
		if (g.haveLastLocalPose) {
			g.localXfadeFrom = g.lastLocalPose;
			g.localXfadeDur = 0.18f;
			g.localXfadeStartMs = now;
			g.localXfadeActive = true;
		}
		g.localCue.upperBodyOnly = upperBody;
		g.localDirectionalMode = !upperBody && IsDirectionalBaseClip(g.localCue.clip);
		g.localWasMoving = false;
		g.localInStop = false;
		g.localHasMovedYet = false;
	}
}

bool LocalUpperBody() {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	return g.localUpperBody;
}

void ClearPerformers() {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	g.performers.clear();
	g.sceneRunning = false;
}

void SetPerformer(uint64_t steamId, int delayTicks, bool placedActor, const std::vector<Cue> &cues, int controllerSlot) {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	if (cues.empty()) { g.performers.erase(steamId); return; }
	PerformerEntry e;
	e.steamId = steamId;
	e.controllerSlot = controllerSlot;
	e.delayTicks = delayTicks;
	e.placedActor = placedActor;
	e.cues = cues;
	g.performers[steamId] = std::move(e);
}

void SetSceneElapsed(double elapsedSec, bool running) {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	g.sceneElapsedSec = elapsedSec;
	g.sceneRunning = running;
}

void SetPreview(const std::string &clipId, float speed, bool loop, float startTime, float duration) {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	EnsureLoaded();
	g.previewClip = clipId;
	g.previewSpeed = std::clamp(speed, 0.1f, 3.0f);
	g.previewLoop = loop;
	g.previewStartTime = std::max(0.f, startTime);
	g.previewDuration = std::max(0.f, duration);
	g.previewStartMs = GetTickCount64();
	g.previewActive = !clipId.empty() && g.lib.Find(clipId) != nullptr;
}

void StopPreview() {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	g.previewActive = false;
	g.previewClip.clear();
}

bool PreviewActive() {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	return g.previewActive;
}

std::string PreviewClip() {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	return g.previewActive ? g.previewClip : std::string();
}

bool SaveCompanionForDemo(const std::string &demoName, const CompanionSequence &seq) {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	const std::string stem = NormalizeDemoStem(demoName);
	if (stem.empty()) return false;
	std::error_code ec;
	const auto dir = CompanionDir();
	std::filesystem::create_directories(dir, ec);
	const auto path = dir / (stem + ".mvmanimseq");
	const auto bytes = SerializeCompanion(seq);
	const std::string payload(reinterpret_cast<const char *>(bytes.data()), bytes.size());
	const bool saved = WriteFileAtomic(path.wstring(), payload) == ERROR_SUCCESS;
	if (saved && MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("actoranim", "saved demo companion %s (%zu actors, startTick=%d)",
		                        path.string().c_str(), seq.actors.size(), seq.demoStartTick);
	return saved;
}

bool LoadCompanionForDemo(const std::string &demoName) {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	const std::string stem = NormalizeDemoStem(demoName);
	if (stem.empty()) return false;
	const auto path = CompanionDir() / (stem + ".mvmanimseq");
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	CompanionSequence seq;
	if (!DeserializeCompanion(bytes.data(), bytes.size(), seq)) return false;
	g.companion = std::move(seq);
	g.companionDemo = stem;
	g.companionLoaded = true;
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("actoranim", "loaded demo companion %s (%zu actors)", path.string().c_str(),
		                        g.companion.actors.size());
	return true;
}

void ClearCompanion() {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	g.companionLoaded = false;
	g.companionDemo.clear();
	g.companion = CompanionSequence{};
}

std::string CompanionStatus() {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	if (!g.companionLoaded) return "none";
	return g.companionDemo + " (" + std::to_string(g.companion.actors.size()) + " actors)";
}

void RunLateFrame(bool panelOpen) {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	++g.frame;

	const bool needPreview = panelOpen && g.previewActive;
	const bool needLocal = g.localActive;
	const bool needLivePerformers = g.sceneRunning && !g.performers.empty();
	const bool needDemoCompanion = g.companionLoaded;

	if (!needPreview && !needLocal && !needLivePerformers && !needDemoCompanion) {
		std::lock_guard<std::mutex> tlk(g.targetMutex);
		g.activeTargets.clear();
		return;
	}

	if (!EnsureLoaded()) return;
	EnsureBoneHook();

	if (g_dbgMod > 1 && int(g.frame % uint64_t(g_dbgMod)) != g_dbgK) return;

	std::vector<ActiveTarget> nextTargets;

	// 1. 3D Animation Picker Preview (MapPlayerPreviewPanel)
	if (needPreview) {
		if (g.previewPawnCount == 0 || (g.frame % 30) == 0) {
			g.previewPawnCount = 0;
			for (int idx = kClientOnlyBase; idx < kMaxHandleIndex && g.previewPawnCount < 16; ++idx) {
				CEntityInstance *e = WideResolve(idx);
				if (IsPreviewPawn(e)) g.previewPawns[g.previewPawnCount++] = idx;
			}
		}
		if (const Clip *pc = g.lib.Find(g.previewClip)) {
			Cue pcue;
			pcue.clip = g.previewClip;
			pcue.speed = g.previewSpeed;
			pcue.loop = g.previewLoop;
			pcue.startTime = g.previewStartTime;
			pcue.duration = g.previewDuration;

			double pElapsed = double(GetTickCount64() - g.previewStartMs) * 0.001;
			if (pcue.loop && !pc->loopable) {
				const double effDur = double(CueEffectiveDuration(pcue, *pc));
				if (effDur > 1e-4) {
					const double cycle = effDur + kPreviewLoopHold;
					pElapsed = std::min(std::fmod(pElapsed, cycle), effDur);
				}
			}
			const SampledPose psp = EvaluateCues(g.lib, {pcue}, pElapsed);
			if (psp.active) {
				for (int i = 0; i < g.previewPawnCount; ++i) {
					const int idx = g.previewPawns[i];
					CEntityInstance *e = WideResolve(idx);
					if (!IsPreviewPawn(e)) continue;
					ActiveTarget at;
					at.ent = e;
					at.cacheKey = 0x5072657600000000ULL | uint64_t(idx);
					at.sample = psp;
					at.applyRootXY = (!pc->hasRootMotion && pc->category != uint8_t(Category::Locomotion) && !psp.upperBodyOnly);
					at.hideWeapon = false;
					nextTargets.push_back(std::move(at));
				}
			}
		}
	}

	// 2. Local Player Live Preview / Manual Playback while recording
	if (needLocal) {
		const ULONGLONG now = GetTickCount64();
		const bool preferWalk = g.localIsWalking || g.localDirFamily == "walk" || g.localDirFamily == "mc_walk";
		if (g.localDirectionalMode) {
			const float speed = g.localSpeed;
			std::string desiredClip = g.localCue.clip;
			bool desiredLoop = g.localCue.loop;
			float desiredDur = 0.f;
			float xfadeDur = 0.18f;
			bool nextWasMoving = g.localWasMoving;
			bool nextInStop = g.localInStop;

			if (speed >= 12.f) {
				nextWasMoving = true;
				nextInStop = false;
				g.localHasMovedYet = true;
				g.localLastFwd = g.localFwd;
				g.localLastRight = g.localRight;
				std::string seed = g.localCue.clip;
				if (!IsDirectionalClip(seed) || IsDirectionalStopClip(seed)) {
					if (g.localDirFamily == "sup") seed = "hla_sup_sprint_fwd";
					else if (g.localDirFamily == "heavy") seed = "hla_heavy_run_fwd";
					else if (g.localDirFamily == "mc_walk") seed = "hla_mc_walk_fwd";
					else if (g.localDirFamily == "mc_run") seed = "hla_mc_run_fwd";
					else if (g.localDirFamily == "walk") seed = "hla_walk_fwd";
					else if (g.localDirFamily == "crouch") seed = "hla_crouch_walk_fwd";
					else seed = "hla_run_fwd";
				}
				desiredClip = SelectDirectionalClip(
				    seed, g.localFwd, g.localRight, preferWalk, /*allowIdle=*/true, g.localIsDucking);
				if (!g.lib.Find(desiredClip)) {
					desiredClip = SelectDirectionalClip(
					    "hla_walk_fwd", g.localFwd, g.localRight, preferWalk, /*allowIdle=*/true, g.localIsDucking);
				}
				desiredLoop = true;
			} else if (!g.localWasMoving && !g.localInStop && !g.localHasMovedYet) {
				// Keep showing and playing the base clip the user chose until they start moving.
				desiredClip = g.localCue.clip;
				desiredLoop = g.localCue.loop;
			} else if (g.localIsDucking) {
				nextWasMoving = false;
				nextInStop = false;
				desiredClip = "hla_crouch_idle";
				desiredLoop = true;
			} else if (g.localWasMoving || g.localInStop) {
				// Return straight to idle when stopping instead of playing post-stop walking frames in place.
				desiredClip = g.localIsDucking ? "hla_crouch_idle" : "hla_combat_idle";
				desiredLoop = true;
				xfadeDur = 0.14f;
				nextWasMoving = false;
				nextInStop = false;
			} else {
				// Subtle CChoiceAnimNode-style idle variation between compatible idle clips when standing still.
				static const char *const kIdleChoices[] = {"hla_combat_idle", "hla_idle_alert", "hla_idle"};
				if (now >= g.localIdleNextMs) {
					g.localIdleNextMs = now + 5200ULL;
					g.localIdleVariant = (g.localIdleVariant + 1) % 3;
				}
				std::string chosenIdle = "hla_combat_idle";
				if (const Clip *alt = g.lib.Find(kIdleChoices[g.localIdleVariant]);
				    alt && alt->category == uint8_t(Category::Idle)) {
					chosenIdle = alt->id;
				}
				if (g.localCue.clip != chosenIdle) {
					desiredClip = chosenIdle;
					desiredLoop = true;
					xfadeDur = 0.35f;
				}
			}

			g.localWasMoving = nextWasMoving;
			g.localInStop = nextInStop;

			if (desiredClip != g.localCue.clip) {
				if (const Clip *nc = g.lib.Find(desiredClip)) {
					if (g.haveLastLocalPose) {
						g.localXfadeFrom = g.lastLocalPose;
						g.localXfadeDur = xfadeDur;
						g.localXfadeStartMs = now;
						g.localXfadeActive = true;
					}
					double newSyncTime = 0.0;
					if (!nextInStop && nc->rootSpeed > 10.f && nc->Duration() > 1e-4f) {
						if (const Clip *oc = g.lib.Find(g.localCue.clip)) {
							const double oldDur = oc->Duration();
							if (oldDur > 1e-4 && oc->rootSpeed > 10.f && !IsDirectionalStopClip(oc->id)) {
								double phase = std::fmod(g.localSyncTime / oldDur, 1.0);
								if (phase < 0.0) phase += 1.0;
								newSyncTime = phase * nc->Duration();
							}
						}
					}
					g.localCue.clip = desiredClip;
					g.localCue.loop = nc->loopable ? desiredLoop : false;
					g.localCue.duration = desiredDur;
					g.localCue.blendIn = 0.f;
					g.localCue.blendOut = 0.f;
					g.localSyncTime = newSyncTime;
					g.localStartMs = g.localSyncMs = now;
				}
			}
		}

		// Auto-blend upper body over CS2's native legs if the player presses WASD during an in-place clip.
		if (g.localSpeed >= 8.f) g.localHasMovedYet = true;
		const Clip *lcMain = g.lib.Find(g.localCue.clip);
		const bool wantAutoUpper = !g.localCue.upperBodyOnly && !g.localDirectionalMode &&
		                           lcMain && (!lcMain->hasRootMotion || lcMain->rootSpeed < 12.f) &&
		                           g.localSpeed > 8.f;
		if (wantAutoUpper != g.localAutoUpper) {
			if (g.haveLastLocalPose) {
				g.localXfadeFrom = g.lastLocalPose;
				g.localXfadeDur = 0.16f;
				g.localXfadeStartMs = now;
				g.localXfadeActive = true;
			}
			g.localAutoUpper = wantAutoUpper;
		}

		Cue evalCue = g.localCue;
		if (g.localAutoUpper) evalCue.upperBodyOnly = true;
		double elapsed = double(now - g.localStartMs) * 0.001;
		if (lcMain) {
			const bool isLoco = (lcMain->category == uint8_t(Category::Locomotion) || IsDirectionalClip(lcMain->id));
			if (isLoco && !IsDirectionalStopClip(lcMain->id)) {
				const double dt = double(now - g.localSyncMs) * 0.001;
				g.localSyncMs = now;
				const double userSpd = double(std::clamp(g.localCue.speed, 0.1f, 3.0f));
				double rate = 1.0;
				if (g.localSpeed < 8.f) {
					rate = (g.localDirectionalMode || g.localHasMovedYet) ? 0.0 : 1.0;
				} else if (lcMain->rootSpeed > 10.f) {
					rate = std::clamp(double(g.localSpeed) / double(lcMain->rootSpeed), 0.45, 1.35);
				}
				if (dt > 0.0 && dt < 0.5) g.localSyncTime += dt * rate * userSpd;
				elapsed = g.localSyncTime;
				evalCue.speed = 1.0f;
			}
		}

		SampledPose sp;
		std::string blendA, blendB;
		float blendT = 0.f;
		const Clip *cA = nullptr, *cB = nullptr;
		if (g.localDirectionalMode && g.localSpeed >= 12.f &&
		    SelectDirectionalBlendPair(g.localCue.clip, g.localFwd, g.localRight, preferWalk, g.localIsDucking,
		                               blendA, blendB, blendT) &&
		    (cA = g.lib.Find(blendA)) != nullptr && (cB = g.lib.Find(blendB)) != nullptr &&
		    cA != cB && blendT > 0.02f && blendT < 0.98f) {
			double basePhase = (lcMain && lcMain->Duration() > 1e-4f) ? std::fmod(elapsed / lcMain->Duration(), 1.0) : 0.0;
			if (basePhase < 0.0) basePhase += 1.0;
			Cue cueA = evalCue, cueB = evalCue;
			cueA.clip = blendA; cueA.loop = true; cueA.duration = 0.f; cueA.speed = 1.0f;
			cueB.clip = blendB; cueB.loop = true; cueB.duration = 0.f; cueB.speed = 1.0f;
			const SampledPose spA = EvaluateCues(g.lib, {cueA}, basePhase * cA->Duration());
			const SampledPose spB = EvaluateCues(g.lib, {cueB}, basePhase * cB->Duration());
			sp = BlendSampledPoses(spA, spB, SmoothStep01(blendT));
		} else {
			std::vector<Cue> one = {evalCue};
			sp = EvaluateCues(g.lib, one, elapsed);
		}
		if (!sp.active) {
			if (g.localDirectionalMode) {
				// Defensive recovery: directional mode stays active and returns to idle rather than ending.
				g.localCue.clip = g.localIsDucking ? "hla_crouch_idle" : "hla_combat_idle";
				g.localCue.loop = true;
				g.localCue.duration = 0.f;
				g.localInStop = false;
				g.localAutoUpper = false;
				g.localStartMs = g.localSyncMs = now;
				Cue recCue = g.localCue;
				sp = EvaluateCues(g.lib, {recCue}, 0.0);
			} else {
				g.localActive = false;
				g.localDirectionalMode = false;
				g.localXfadeActive = false;
				g.haveLastLocalPose = false;
				g.localAutoUpper = false;
			}
		}
		if (sp.active) {
			if (g.localXfadeActive) {
				const double xElapsed = double(now - g.localXfadeStartMs) * 0.001;
				const float alpha = SmoothStep01(float(xElapsed / std::max(0.05f, g.localXfadeDur)));
				if (alpha >= 1.f - 1e-4f) {
					g.localXfadeActive = false;
				} else {
					sp = BlendSampledPoses(g.localXfadeFrom, sp, alpha);
				}
			}
			g.lastLocalPose = sp;
			g.haveLastLocalPose = sp.active;
			if (CEntityInstance *localPawn = FindLocalPlayerPawn()) {
				ActiveTarget at;
				at.ent = localPawn;
				at.cacheKey = 0x10CA1ULL;
				at.sample = sp;
				const Clip *lcRoot = g.lib.Find(evalCue.clip);
				at.applyRootXY = (lcRoot != nullptr && !lcRoot->hasRootMotion &&
				                  lcRoot->category != uint8_t(Category::Locomotion) &&
				                  !sp.upperBodyOnly && g.localSpeed < 8.f);
				at.hideWeapon = false;
				nextTargets.push_back(std::move(at));
			}
		}
	}

	// 3. Multi-Actor Live Performers (private match) OR Synchronized Companion Actors (GOTV demo)
	if (needLivePerformers || needDemoCompanion) {
		int demoTick = 0;
		const bool haveDemoTick = needDemoCompanion && g_MirvTime.GetCurrentDemoTick(demoTick);
		for (int slot = 1; slot <= 64; ++slot) {
			CEntityInstance *ctrl = WideResolve(slot);
			uint64_t sid = 0;
			CEntityInstance *pawn = nullptr;
			if (!ReadSteamAndPawn(ctrl, sid, pawn) || !pawn) continue;

			if (needLivePerformers) {
				auto it = (sid != 0) ? g.performers.find(sid) : g.performers.end();
				if (it == g.performers.end()) {
					for (auto pIt = g.performers.begin(); pIt != g.performers.end(); ++pIt) {
						if (pIt->second.controllerSlot == slot) { it = pIt; break; }
					}
				}
				if (it != g.performers.end()) {
					const double trackElapsed = g.sceneElapsedSec - double(it->second.delayTicks) / 64.0;
					if (trackElapsed >= 0.0) {
						const SampledPose sp = EvaluateCues(g.lib, it->second.cues, trackElapsed);
						if (sp.active) {
							const Clip *bc = g.lib.Find(it->second.cues.empty() ? std::string() : it->second.cues[0].clip);
							ActiveTarget at;
							at.ent = pawn;
							at.cacheKey = (sid != 0) ? sid : (0xB070000000000000ULL | uint64_t(slot));
							at.sample = sp;
							at.applyRootXY = (bc && !bc->hasRootMotion && bc->category != uint8_t(Category::Locomotion) && !sp.upperBodyOnly);
							at.hideWeapon = false;
							nextTargets.push_back(std::move(at));
						}
					}
				}
			} else if (sid != 0 && haveDemoTick) {
				for (const auto &a : g.companion.actors) {
					if (a.steamId != sid) continue;
					const double trackElapsed =
					    double(demoTick - g.companion.demoStartTick - a.delayTicks) / 64.0;
					if (trackElapsed >= 0.0) {
						const SampledPose sp = EvaluateCues(g.lib, a.cues, trackElapsed);
						if (sp.active) {
							const Clip *bc = g.lib.Find(a.cues.empty() ? std::string() : a.cues[0].clip);
							ActiveTarget at;
							at.ent = pawn;
							at.cacheKey = sid;
							at.sample = sp;
							at.applyRootXY = (bc && !bc->hasRootMotion && bc->category != uint8_t(Category::Locomotion) && !sp.upperBodyOnly);
							at.hideWeapon = false;
							nextTargets.push_back(std::move(at));
						}
					}
					break;
				}
			}
		}
	}

	{
		std::lock_guard<std::mutex> tlk(g.targetMutex);
		++g.evalGen;
		for (ActiveTarget &at : nextTargets) at.evalGen = g.evalGen;
		g.activeTargets = nextTargets;
	}

	for (const ActiveTarget &at : nextTargets) {
		InvalidateSkeletonAndWeaponChildren(at.ent);
		if (!g_boneHookOk || at.hideWeapon) {
			std::lock_guard<std::mutex> plk(g.poseMutex);
			RestCache &rc = g.restByKey[at.cacheKey];
			ApplyPoseToSkeletonInstance(at.ent, at, rc, /*forceWriteLocal=*/true);
		}
	}
}

// SEH-guarded walk of the pawn's scene-node children, one line per child. No objects with destructors
// here, so __try is allowed.
static void WalkPawnChildren(CEntityInstance *pawn, const Pose *engineWorld, bool haveWorld, int boneCount,
                             const char *const *names, const int16_t *parents, char *out, size_t outSize) {
	const auto &o = g_clientDllOffsets;
	size_t used = 0;
	out[0] = 0;
	unsigned char *node = nullptr;
	__try {
		if (o.C_BaseEntity.m_pGameSceneNode) node = *(unsigned char **)((unsigned char *)pawn + o.C_BaseEntity.m_pGameSceneNode);
	} __except (1) { node = nullptr; }
	if (!node || !o.CGameSceneNode.m_pChild || !o.CGameSceneNode.m_pNextSibling || !o.CGameSceneNode.m_nodeToWorld) {
		std::snprintf(out, outSize, "no scene node\n");
		return;
	}
	int shown = 0;
	__try {
		unsigned char *child = *(unsigned char **)(node + o.CGameSceneNode.m_pChild);
		for (; child && shown < 24; child = *(unsigned char **)(child + o.CGameSceneNode.m_pNextSibling), ++shown) {
			const float *x = (const float *)(child + o.CGameSceneNode.m_nodeToWorld);
			CEntityInstance *owner = o.CGameSceneNode.m_pOwner ? *(CEntityInstance **)(child + o.CGameSceneNode.m_pOwner) : nullptr;
			const char *cls = "?";
			__try { if (owner) cls = owner->GetClassName(); } __except (1) {}
			int hier = -1, attach = -1;
			if (o.DeathWeaponPhysics.m_nHierarchyType) hier = *(unsigned char *)(child + o.DeathWeaponPhysics.m_nHierarchyType);
			if (o.DeathWeaponPhysics.m_nParentAttachmentOrBone) attach = *(unsigned short *)(child + o.DeathWeaponPhysics.m_nParentAttachmentOrBone);
			int nearest = -1;
			float nd = 1e9f;
			if (haveWorld) for (int i = 0; i < boneCount; ++i) {
				const float dx = engineWorld[i].p[0] - x[0], dy = engineWorld[i].p[1] - x[1], dz = engineWorld[i].p[2] - x[2];
				const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (d < nd) { nd = d; nearest = i; }
			}
			char bn[64] = {};
			int bp = 0;
			if (nearest >= 0) ReadBoneEntry(names, parents, nearest, bn, sizeof(bn), bp);
			// The weapon's own skeleton: its bone names and where its bones sit in the world, to see whether it
			// is bone-merged onto the player's bones.
			char wb[420] = {};
			if (owner) {
				unsigned char *wms = ModelStateOf(owner);
				const void *wmodel = wms ? ModelOf(wms) : nullptr;
				const char *const *wn = nullptr;
				const int16_t *wp = nullptr;
				if (wmodel && ReadTablePointers(wmodel, wn, wp)) {
					Pose ww[48];
					int wcount = 0;
					for (; wcount < 48; ++wcount) {
						char nm[64]; int par = 0;
						if (!ReadBoneEntry(wn, wp, wcount, nm, sizeof(nm), par)) break;
					}
					const bool haveW = wcount > 0 && ReadPoseBuffer(wms, kWorldPose, wcount, ww);
					size_t wu = 0;
					wu += size_t(std::snprintf(wb + wu, sizeof(wb) - wu, "    weapon skeleton %d bones:", wcount));
					// Which pawn bones sit closest to the weapon's root bone (names and distances).
					if (haveW && haveWorld) {
						int best[3] = {-1, -1, -1};
						float bd[3] = {1e9f, 1e9f, 1e9f};
						for (int i = 0; i < boneCount; ++i) {
							const float dx = engineWorld[i].p[0] - ww[0].p[0], dy = engineWorld[i].p[1] - ww[0].p[1], dz = engineWorld[i].p[2] - ww[0].p[2];
							const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
							for (int k = 0; k < 3; ++k) if (d < bd[k]) {
								for (int m = 2; m > k; --m) { bd[m] = bd[m - 1]; best[m] = best[m - 1]; }
								bd[k] = d; best[k] = i; break;
							}
						}
						wu += size_t(std::snprintf(wb + wu, sizeof(wb) - wu, " [root nearest:"));
						for (int k = 0; k < 3 && best[k] >= 0; ++k) {
							char nm[64] = {}; int par = 0;
							ReadBoneEntry(names, parents, best[k], nm, sizeof(nm), par);
							wu += size_t(std::snprintf(wb + wu, sizeof(wb) - wu, " %s=%.1f", nm, bd[k]));
						}
						wu += size_t(std::snprintf(wb + wu, sizeof(wb) - wu, "]"));
					}
					for (int i = 0; i < wcount && i < 8 && wu + 60 < sizeof(wb); ++i) {
						char nm[64]; int par = 0;
						ReadBoneEntry(wn, wp, i, nm, sizeof(nm), par);
						if (haveW) wu += size_t(std::snprintf(wb + wu, sizeof(wb) - wu, " %s(%.0f %.0f %.0f)", nm, ww[i].p[0], ww[i].p[1], ww[i].p[2]));
						else wu += size_t(std::snprintf(wb + wu, sizeof(wb) - wu, " %s", nm));
					}
				}
			}
			if (used + 700 >= outSize) break;
			used += size_t(std::snprintf(out + used, outSize - used,
			    "child %d: class=%s hier=%d attach=%d world=(%.1f %.1f %.1f) nearest bone %d=%s dist=%.1f\n",
			    shown, cls ? cls : "?", hier, attach, x[0], x[1], x[2], nearest, bn, nd));
			if (wb[0]) used += size_t(std::snprintf(out + used, outSize - used, "%s\n", wb));
		}
	} __except (1) {
		std::snprintf(out + used, outSize - used, "(scene walk faulted)\n");
	}
}

std::string DumpAttachments() {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	if (!EnsureLoaded()) return "pack not loaded";
	CEntityInstance *pawn = FindLocalPlayerPawn();
	if (!pawn) return "no local pawn";
	unsigned char *ms = ModelStateOf(pawn);
	const void *model = ModelOf(ms);
	SkeletonMap skCopy;
	const SkeletonMap *sk = nullptr;
	if (ms && model) {
		std::lock_guard<std::mutex> plk(g.poseMutex);
		if (const SkeletonMap *s = SkeletonOf(model)) { skCopy = *s; sk = &skCopy; }
	}
	if (!ms || !sk) return "no skeleton";
	const char *const *names = nullptr;
	const int16_t *parents = nullptr;
	ReadTablePointers(model, names, parents);
	Pose engineWorld[kMaxSkeletonBones];
	const bool haveWorld = ReadPoseBuffer(ms, kWorldPose, sk->count, engineWorld);
	std::string out = "bones (non-core):";
	char line[400];
	for (int i = 0; i < sk->count; ++i) {
		if (sk->boneToCore[i] >= 0) continue;
		char name[64] = {};
		int parent = 0;
		if (!ReadBoneEntry(names, parents, i, name, sizeof(name), parent)) continue;
		if (std::strstr(name, "finger") || std::strstr(name, "TWIST") || std::strstr(name, "eye") || std::strstr(name, "jiggle")) continue;
		std::snprintf(line, sizeof(line), " %d=%s", i, name);
		out += line;
	}
	out += "\n";
	Pose xform;
	if (haveWorld && ExtractWorldTransform(pawn, xform)) {
		float azL = 0.f, azR = 0.f, bzL = 0.f, bzR = 0.f;
		bool hAL = false, hAR = false, hBL = false, hBR = false;
		for (int i = 0; i < sk->count; ++i) {
			if (sk->boneToCore[i] == kCoreAnkleL) { azL = engineWorld[i].p[2] - xform.p[2]; hAL = true; }
			else if (sk->boneToCore[i] == kCoreAnkleR) { azR = engineWorld[i].p[2] - xform.p[2]; hAR = true; }
			else {
				char nm[64] = {};
				int par = 0;
				if (ReadBoneEntry(names, parents, i, nm, sizeof(nm), par)) {
					if (NameEquals(nm, "ball_L")) { bzL = engineWorld[i].p[2] - xform.p[2]; hBL = true; }
					else if (NameEquals(nm, "ball_R")) { bzR = engineWorld[i].p[2] - xform.p[2]; hBR = true; }
				}
			}
		}
		if (hAL || hAR) {
			std::snprintf(line, sizeof(line),
			              "feet_z(originZ=%.2f): ankleL=%.2f ankleR=%.2f ballL=%.2f ballR=%.2f\n",
			              xform.p[2], hAL ? azL : -99.f, hAR ? azR : -99.f, hBL ? bzL : -99.f, hBR ? bzR : -99.f);
			out += line;
		}
	}
	static char children[6000];
	WalkPawnChildren(pawn, engineWorld, haveWorld, sk->count, names, parents, children, sizeof(children));
	out += children;
	return out;
}

void SetLocalSpeed(float speed) {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	g.localSpeed = speed;
}

void SetLocalSpeedScale(float speed) {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	const float s = std::clamp(speed, 0.1f, 3.0f);
	g.localCue.speed = s;
	g.previewSpeed = s;
}

float LocalSpeedScale() {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	return g.localCue.speed;
}

void SetLocalEquippedWeaponClass(uint8_t weaponClass) {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	g.localEquippedWeapon = weaponClass;
}

void SetLocalMotion(float fwdSpeed, float rightSpeed, bool isWalking, bool isDucking) {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	g.localFwd = fwdSpeed;
	g.localRight = rightSpeed;
	g.localSpeed = std::sqrt(fwdSpeed * fwdSpeed + rightSpeed * rightSpeed);
	g.localIsWalking = isWalking;
	g.localIsDucking = isDucking;
}

bool SampleLocalRootVelocity(float outLocalVelXY[2]) {
	outLocalVelXY[0] = outLocalVelXY[1] = 0.f;
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	if (!g.localActive || g.localCue.upperBodyOnly || g.localAutoUpper || g.localDirectionalMode)
		return false;
	const Clip *lc = g.lib.Find(g.localCue.clip);
	if (!lc || !lc->hasRootMotion || lc->rootSpeed < 5.f || lc->category == uint8_t(Category::Locomotion))
		return false;
	const double elapsed = double(GetTickCount64() - g.localStartMs) * 0.001;
	const float effDur = CueEffectiveDuration(g.localCue, *lc);
	if (elapsed < 0.0 || elapsed > double(effDur)) return false;
	const double t = double(std::max(0.f, g.localCue.startTime)) + elapsed;
	SampleClipRootVelocity(*lc, t, g.localCue.speed, g.localCue.loop, outLocalVelXY);
	return (outLocalVelXY[0] * outLocalVelXY[0] + outLocalVelXY[1] * outLocalVelXY[1]) > 4.f;
}

float DesiredLocalMaxSpeed() {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	if (!g.localActive) return -1.f;
	const Clip *lc = g.lib.Find(g.localCue.clip);
	const bool walkCap = g.localIsWalking || g.localDirFamily == "walk" || g.localDirFamily == "mc_walk";
	return lc ? RecommendedMaxSpeedForClip(*lc, g.localDirectionalMode, walkCap, g.localIsDucking,
	                                       g.localCue.upperBodyOnly || g.localAutoUpper, g.localCue.speed)
	          : -1.f;
}

void SetDebug(int mask, int mod, int k) {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	g_dbgMask = mask;
	g_dbgMod = std::max(1, mod);
	g_dbgK = std::max(0, k);
}

std::string StatusLine() {
	std::lock_guard<std::recursive_mutex> lk(g.mutex);
	EnsureLoaded();
	char buf[280];
	std::snprintf(buf, sizeof(buf),
	              "actoranim: %s | local=%s | upper=%s | dir=%s | performers=%zu | companion=%s | hook=%s | writes=%llu",
	              g.lib.status.c_str(), g.localActive ? g.localCue.clip.c_str() : "off",
	              g.localUpperBody ? "on" : "off", g.localDirectionalMode ? "on" : "off",
	              g.performers.size(), CompanionStatus().c_str(), g_boneHookOk ? "ok" : "off",
	              (unsigned long long)g.writes);
	return buf;
}

} // namespace Filmmaker::ActorAnim
