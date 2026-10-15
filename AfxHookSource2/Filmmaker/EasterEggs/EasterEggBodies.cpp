#include "EasterEggWorld.h"
#include "EasterEggMoonMath.h"
#include "../Platform/AtomicFile.h"

#include "../Movie/ModelPoseAccess.h"
#include "../Movie/RagdollBodies.h"
#include "../Movie/RagdollPhysics.h"
#include "../Debug/MvmDebugLog.h"
#include "../../ClientEntitySystem.h"
#include "../../hlaeFolder.h"

#include <Windows.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <string>

// BIG HEAD MODE. Every player's head_0 bone is scaled up in the PARENT-LOCAL pose (CModelState
// +0x18, and its copy at +0x48) -- the buffer the renderer actually draws from (measured for the
// preview pawn, docs: preview-pawn bone posing). A local scale on head_0 carries every child of
// it (eyes, face, headgear) with it, so the whole head grows as one piece. Records are 8 floats:
// pos xyz, SCALE, quat xyzw. Written in the late pump, after the client's own animation has
// rebuilt the pose this frame; an animated frame rebuilds it again, a paused one keeps ours, and
// both are fine because the write is a SET to the same value, never a multiply.
//
// MOON GRAVITY. ragdoll_gravity_scale, through RagdollPhysics (a convar, nothing version-coupled).
// Slightly negative: corpses drift up and away like they died on the moon and kept going.

namespace Filmmaker {
namespace {

using namespace ModelPoseAccess;

constexpr float kHeadScale = 2.6f;
constexpr ptrdiff_t kLocalBuffers[] = { 0x18, 0x48 };
constexpr float kMoonGravity = -0.12f;

bool g_bigHead = false;
bool g_releasePending = false;
struct HeadRig { int head = -1; int bones[64] = {}; int count = 0; }; // head_0 + every bone below it
std::map<const void*, HeadRig> g_headRig;
unsigned long long g_writes = 0, g_faults = 0;
unsigned long long g_lastLogMs = 0;

bool g_moon = false;
float g_moonStashGravity = 1.0f;
bool g_moonCheckedLeftover = false;
const char* g_moonRecovery = "pending";
DWORD g_moonWriteError = ERROR_SUCCESS;

// Ragdoll gravity is a saved setting: like the lighting stash, the user's value is also written to
// disk while the moon is up, so a session that ends mid-moon gets it back on the next start.
std::wstring MoonStashPath() {
	std::wstring path = GetHlaeRoamingAppDataFolderW();
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
	path += L"easter_eggs_moon_stash.txt";
	return path;
}

void RestoreMoonLeftover() {
	g_moonCheckedLeftover = true;
	const auto path = MoonStashPath();
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) {
		g_moonRecovery = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES
			&& GetLastError() == ERROR_FILE_NOT_FOUND ? "none" : "read-failed";
		return;
	}
	const auto length = file.tellg();
	if (length <= 0 || length > 64) { g_moonRecovery = "invalid"; return; }
	std::string text(static_cast<size_t>(length), '\0');
	file.seekg(0);
	if (!file.read(&text[0], static_cast<std::streamsize>(text.size()))) {
		g_moonRecovery = "read-failed"; return;
	}
	file.close();
	float gravity;
	if (!EasterEggMoonMath::ReadRecovery(text, gravity)) {
		g_moonRecovery = "invalid"; return; // preserve invalid evidence and the user's current value
	}
	RagdollPhysics_Set(kRpGravity, gravity);
	g_moonRecovery = DeleteFileW(path.c_str()) ? "restored" : "restored-stash-retained";
	MvmDebugLog_LinefAlways("egg.moon", "recovery=%s gravity=%.9g", g_moonRecovery, gravity);
}

bool WriteWorldHeadScaleOne(unsigned char* ms, const HeadRig& rig) {
	__try {
		float* buf = *(float**)(ms + 0x80);
		if ((uintptr_t)buf < 0x10000) return false;
		for (int i = 0; i < rig.count; ++i) buf[rig.bones[i] * 8 + 3] = 1.0f;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

const HeadRig& RigOf(const void* model) {
	auto it = g_headRig.find(model);
	if (it != g_headRig.end()) return it->second;
	HeadRig rig;
	const char* const* names = nullptr;
	const int16_t* parents = nullptr;
	if (ReadTablePointers(model, names, parents)) {
		bool below[160] = {};
		for (int b = 0; b < 160; ++b) {
			char name[64]; int parent;
			if (!ReadBoneEntry(names, parents, b, name, sizeof(name), parent) || parent >= b || parent < -1) break;
			if (!_stricmp(name, "head_0")) rig.head = b;
			below[b] = b == rig.head || (parent >= 0 && below[parent]);
			if (below[b] && rig.count < 64) rig.bones[rig.count++] = b;
		}
	}
	g_headRig[model] = rig;
	MvmDebugLog_LinefAlways("egg.bighead", "model=%p head_0=%d subtree=%d", model, rig.head, rig.count);
	return g_headRig[model];
}

bool WriteWorldHead(unsigned char* ms, const HeadRig& rig, float scale);

// Undo of WriteWorldHead: only when the head still carries our scale (an animated frame already
// rebuilt it otherwise, and shrinking a rebuilt pose would make tiny heads).
bool WriteWorldHeadIfScaled(unsigned char* ms, const HeadRig& rig, float written) {
	float s = 0.0f;
	__try {
		float* buf = *(float**)(ms + 0x80);
		if ((uintptr_t)buf < 0x10000) return false;
		s = buf[rig.head * 8 + 3];
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	if (std::fabs(s - written) > 1e-3f) return false;
	return WriteWorldHead(ms, rig, 1.0f / written) && WriteWorldHeadScaleOne(ms, rig);
}

// World-space pose (+0x80): scale every bone of the head subtree and push it out from the head
// pivot by the same factor, so the skinning sees a bigger head even if the local scale channel is
// not composed into the world transforms (measured: a local-only scale changed nothing on screen).
bool WriteWorldHead(unsigned char* ms, const HeadRig& rig, float scale) {
	__try {
		float* buf = *(float**)(ms + 0x80);
		if ((uintptr_t)buf < 0x10000) return false;
		const float* hp = buf + rig.head * 8;
		const float px = hp[0], py = hp[1], pz = hp[2];
		if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) return false;
		for (int i = 0; i < rig.count; ++i) {
			float* rec = buf + rig.bones[i] * 8;
			if (!std::isfinite(rec[3]) || rec[3] <= 0.0f || rec[3] > 8.0f) return false;
			rec[0] = px + (rec[0] - px) * scale;
			rec[1] = py + (rec[1] - py) * scale;
			rec[2] = pz + (rec[2] - pz) * scale;
			rec[3] = scale;
		}
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { ++g_faults; return false; }
}

// Sets head_0's local scale to `scale` on one model state. `onlyIf` > 0 restricts the write to
// records currently holding that value (used to put back only what we wrote).
bool WriteHeadScale(unsigned char* ms, int head, float scale, float onlyIf) {
	__try {
		for (ptrdiff_t off : kLocalBuffers) {
			float* buf = *(float**)(ms + off);
			if ((uintptr_t)buf < 0x10000) return false;
			float* rec = buf + head * 8;
			const float s = rec[3];
			if (!std::isfinite(s) || s <= 0.0f || s > 8.0f) return false; // not a pose record
			if (onlyIf > 0.0f && std::fabs(s - onlyIf) > 1e-3f) continue;
			rec[3] = scale;
		}
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { ++g_faults; return false; }
}

void ForEachHead(float scale, float onlyIf) {
	const int highest = GetHighestEntityIndex();
	int touched = 0;
	for (int i = 0; i <= highest && i < 8192; ++i) {
		CEntityInstance* e = RagdollBodies::EntityAt(i);
		if (!e || !e->IsPlayerPawn()) continue;
		CEntityInstance* carriers[2] = { e, RagdollBodies::ClientRagdollOf(e) };
		for (CEntityInstance* c : carriers) {
			if (!c) continue;
			unsigned char* ms = ModelStateOf(c);
			const void* model = ModelOf(ms);
			if (!model) continue;
			const HeadRig& rig = RigOf(model);
			if (rig.head < 0) continue;
			if (WriteHeadScale(ms, rig.head, scale, onlyIf)) { ++g_writes; ++touched; }
			if (rig.count > 0) {
				// On release, shrink the world pose back by the inverse -- a paused frame never
				// rebuilds it, so leaving it would keep the big heads on screen after "off".
				if (onlyIf <= 0.0f) WriteWorldHead(ms, rig, scale);
				else WriteWorldHeadIfScaled(ms, rig, onlyIf);
			}
		}
	}
	if (MvmDebugLog_Active() && GetTickCount64() - g_lastLogMs > 2000) {
		g_lastLogMs = GetTickCount64();
		MvmDebugLog_Linef("egg.bighead", "scale=%.2f heads=%d writes=%llu faults=%llu", scale, touched, g_writes, g_faults);
	}
}

} // namespace

void EasterEggBodies_SetBigHead(bool on) {
	if (on == g_bigHead) return;
	g_bigHead = on;
	if (!on) g_releasePending = true; // undone on the next late pump, where the write happens
}

void EasterEggBodies_LevelInit() {
	MvmDebugLog_LinefAlways("egg.bighead", "level reset: forgetting %zu cached models", g_headRig.size());
	g_bigHead = false;
	g_releasePending = false;
	g_headRig.clear();
}

int EasterEggBodies_CachedModels() { return static_cast<int>(g_headRig.size()); }

void EasterEggBodies_RunLateFrame() {
	if (!g_moonCheckedLeftover) RestoreMoonLeftover();
	if (g_bigHead) { ForEachHead(kHeadScale, 0.0f); return; }
	if (g_releasePending) {
		// Only records still holding our value: a frame the engine already rebuilt is left alone.
		ForEachHead(1.0f, kHeadScale);
		g_releasePending = false;
	}
}

bool EasterEggBodies_SetMoon(bool on) {
	if (!g_moonCheckedLeftover) RestoreMoonLeftover();
	if (on == g_moon) return true;
	if (on) {
		const float original = RagdollPhysics_Get(kRpGravity);
		std::string text;
		g_moonWriteError = EasterEggMoonMath::WriteRecovery(original, text)
			? WriteFileAtomic(MoonStashPath(), text) : ERROR_INVALID_DATA;
		if (g_moonWriteError != ERROR_SUCCESS) {
			MvmDebugLog_LinefAlways("egg.moon", "activation refused: stash write error=%lu", g_moonWriteError);
			return false;
		}
		g_moonStashGravity = original;
		g_moon = true;
		RagdollPhysics_Set(kRpGravity, kMoonGravity);
	} else {
		RagdollPhysics_Set(kRpGravity, g_moonStashGravity);
		g_moon = false;
		DeleteFileW(MoonStashPath().c_str());
	}
	MvmDebugLog_LinefAlways("egg.moon", "%s gravity=%.9g (stash %.9g)", on ? "on" : "off",
		RagdollPhysics_Get(kRpGravity), g_moonStashGravity);
	return true;
}

const char* EasterEggBodies_MoonRecoveryStatus() {
	MvmDebugLog_LinefAlways("egg.moon", "recovery=%s active=%d gravity=%.9g stash=%.9g writeError=%lu",
		g_moonRecovery, g_moon ? 1 : 0, RagdollPhysics_Get(kRpGravity), g_moonStashGravity, g_moonWriteError);
	return g_moonRecovery;
}

} // namespace Filmmaker
