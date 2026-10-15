#include "RagdollWeaponArmPhysics.h"
#include "RagdollWeaponArmPhysicsMath.h"
#include "../Debug/MvmDebugLog.h"
#include "../../ClientEntitySystem.h"
#include "../../../Shared/binutils.h"
#include <cmath>
#include <cstring>

namespace Filmmaker {
namespace {
constexpr int kMaxPending = 64;
// Render skeleton index in the installed CS2 player rig. The engine function
// maps this bone through the ragdoll's own bone-to-body table before applying force.
constexpr int kRightLowerArmBone = 17;
using ApplyBoneForceFn = void(__fastcall*)(CEntityInstance*, int, const float*);
ApplyBoneForceFn g_apply = nullptr;
ptrdiff_t g_bodyOffset = 0;
bool g_resolved = false;
struct Pending {
	CEntityInstance* corpse = nullptr;
	int userId = -1;
	float force[3] = {};
};
Pending g_pending[kMaxPending];

bool Resolve() {
	if (g_resolved) return g_apply != nullptr;
	HMODULE client = GetModuleHandleA("client.dll");
	if (!client) return false;
	g_resolved = true;
	Afx::BinUtils::ImageSectionsReader sections(client);
	if (sections.Eof()) return false;
	// Verified in the installed client's C_CSPlayerPawn ragdoll-sync call chain:
	// (entity, skeletonBone, force) -> bone-to-body map -> body vfunc +0x250.
	// Resolve the function and its opaque ragdoll pointer operand together. Never
	// substitute the nearby, distinct schema m_pRagdollControl pointer.
	const auto match = Afx::BinUtils::FindPatternString(sections.GetMemRange(),
		"40 53 55 41 57 48 83 EC 50 48 8B E9 48 63 DA 48 8B 89 ?? ?? ?? ?? 4D 8B F8 48 85 C9 0F 84 ?? ?? ?? ?? 48 8B 01 FF 50 40 83 38 00");
	if (!match.IsEmpty()) {
		int32_t displacement = 0;
		std::memcpy(&displacement, reinterpret_cast<void*>(match.Start + 18), sizeof(displacement));
		if (displacement > 0 && displacement < 0x10000) {
			g_bodyOffset = displacement;
			g_apply = reinterpret_cast<ApplyBoneForceFn>(match.Start);
		}
	}
	MvmDebugLog_LinefAlways("ragdoll.weapon.arm", "phase=resolve fn=%p bodyOff=%td ready=%d",
		reinterpret_cast<void*>(g_apply), g_bodyOffset, g_apply != nullptr);
	return g_apply != nullptr;
}
} // namespace

void RagdollWeaponArmPhysics_Queue(CEntityInstance* corpse, int userId,
	const char*, const float position[3], const float rotation[4], double strength) {
	if (!corpse || !position || !rotation || !std::isfinite(strength) || strength < 0 || strength > 10000) return;
	if (!Resolve()) return;
	const auto impulse = RagdollWeaponMakeArmImpulse(
		{rotation[0], rotation[1], rotation[2], rotation[3]}, strength);
	Pending* target = nullptr;
	for (auto& p : g_pending) {
		if (p.corpse == corpse) { target = &p; break; }
		if (!p.corpse && !target) target = &p;
	}
	if (!target) return;
	target->corpse = corpse;
	target->userId = userId;
	std::memcpy(target->force, impulse.force, sizeof(target->force));
}

void RagdollWeaponArmPhysics_BeforeClientStage(int stage) {
	// Verified stage order: 6 precedes stage-7 ragdoll updates and simulation.
	// Waiting here also prevents applying impulses to a paused render-only frame.
	if (stage != 6 || !g_apply) return;
	for (auto& p : g_pending) {
		if (!p.corpse) continue;
		__try {
			void* body = *reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(p.corpse) + g_bodyOffset);
			if (body && p.corpse->GetHealth() == 0) {
				g_apply(p.corpse, kRightLowerArmBone, p.force);
				MvmDebugLog_LinefAlways("ragdoll.weapon.arm",
					"phase=physical userid=%d corpse=%p fn=%p bodyOff=%td body=%p bone=%d force=(%.3f %.3f %.3f)",
					p.userId, p.corpse, reinterpret_cast<void*>(g_apply), g_bodyOffset, body,
					kRightLowerArmBone, p.force[0], p.force[1], p.force[2]);
			} else {
				MvmDebugLog_LinefAlways("ragdoll.weapon.arm", "phase=unavailable userid=%d corpse=%p bodyOff=%td body=%p",
					p.userId, p.corpse, g_bodyOffset, body);
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			MvmDebugLog_LinefAlways("ragdoll.weapon.arm", "phase=physical userid=%d fault=1", p.userId);
		}
		p = Pending{};
	}
}

void RagdollWeaponArmPhysics_Forget(CEntityInstance* entity) {
	for (auto& p : g_pending) if (p.corpse == entity) p = Pending{};
}
void RagdollWeaponArmPhysics_Clear() {
	for (auto& p : g_pending) p = Pending{};
}
} // namespace Filmmaker
