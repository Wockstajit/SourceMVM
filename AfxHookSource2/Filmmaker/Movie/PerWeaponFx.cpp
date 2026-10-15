#include "stdafx.h"

#include "PerWeaponFx.h"

#include "FollowTargetProviders.h"      // Filmmaker::EntityAt, Filmmaker::DesignerClass
#include "../../ClientEntitySystem.h"   // AfxGetSpectatedPawnIndex, AfxGetLocalViewerPawn

#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>

using Filmmaker::DesignerClass;
using Filmmaker::EntityAt;

namespace {

// baseSystem  the .vpcf file name the rules table resolved to (no directory)
// weaponClass lowercase substring matched against the firing weapon's DESIGNER class
//             ("weapon_scar20"), never the client class -- see PerWeaponFx_SampleMainThread
// forkSuffix  appended to the base stem to name the per-weapon asset
//
// The fork is addressed by rewriting the resolved path IN PLACE, so one entry automatically
// follows whichever variant directory the swap picked (regular / less-impacts / less-smoke /
// modern) rather than needing an entry per variant.
struct PerWeaponRule {
	const char* baseSystem;
	const char* weaponClass;
	const char* forkSuffix;
};

const PerWeaponRule kRules[] = {
	// SCAR-20 and G3SG1 both resolve to weapon_muzzle_flash_awp. Only the SCAR-20 was reported
	// with its flash sitting in front of the barrel, so it takes the clamped-spark fork and the
	// AWP/G3SG1 keep the authored spread.
	{ "weapon_muzzle_flash_awp.vpcf", "weapon_scar20", "_scar20" },

	// M249 and Negev both resolve to weapon_muzzle_flash_para. No entry yet because neither has
	// been reported individually -- add one the moment one of them is, e.g.
	//     { "weapon_muzzle_flash_para.vpcf", "weapon_m249", "_m249" },
	// and fork the asset in postprocess_insurgency.py (apply_insurgency_tuning).
};

std::mutex g_mx;
char g_weaponClass[64] = {};
std::atomic<bool> g_have{ false };

void LowerInto(const std::string& in, char* out, size_t cap) {
	size_t n = 0;
	for (char c : in) {
		if (n + 1 >= cap)
			break;
		out[n++] = (char)std::tolower((unsigned char)c);
	}
	out[n] = 0;
}

} // namespace

void PerWeaponFx_SampleMainThread() {
	CEntityInstance* pawn = nullptr;
	const int idx = AfxGetSpectatedPawnIndex();
	if (idx >= 0)
		pawn = EntityAt(idx);
	if (!pawn || !pawn->IsPlayerPawn())
		pawn = AfxGetLocalViewerPawn();

	std::string cls;
	if (pawn && pawn->IsPlayerPawn()) {
		const auto handle = pawn->GetActiveWeaponHandle();
		if (handle.IsValid()) {
			if (CEntityInstance* weapon = EntityAt(handle.GetEntryIndex())) {
				// DesignerClass (GetClassName) gives "weapon_scar20". EntityClass
				// (GetClientClassName) is useless here: nearly every CS2 gun reports the
				// SAME client class, C_CSWeaponBaseGun, so it cannot separate a SCAR-20
				// from a G3SG1 -- which is the entire purpose of this module.
				cls = DesignerClass(weapon);
			}
		}
	}

	std::lock_guard<std::mutex> lock(g_mx);
	LowerInto(cls, g_weaponClass, sizeof(g_weaponClass));
	g_have.store(g_weaponClass[0] != 0, std::memory_order_relaxed);
}

const char* PerWeaponFx_Override(const char* baseTargetLower) {
	if (!baseTargetLower || !g_have.load(std::memory_order_relaxed))
		return nullptr;

	char cls[sizeof(g_weaponClass)];
	{
		std::lock_guard<std::mutex> lock(g_mx);
		std::memcpy(cls, g_weaponClass, sizeof(cls));
	}

	for (const PerWeaponRule& rule : kRules) {
		if (!std::strstr(cls, rule.weaponClass))
			continue;
		const char* at = std::strstr(baseTargetLower, rule.baseSystem);
		if (!at)
			continue;
		// Only a whole path segment counts. Without this, weapon_muzzle_flash_awp.vpcf would
		// also match inside an already-forked weapon_muzzle_flash_awp_scar20.vpcf.
		if (at != baseTargetLower && at[-1] != '/')
			continue;

		const size_t baseLen = std::strlen(rule.baseSystem);
		const size_t stemLen = baseLen - 5; // drop ".vpcf"
		thread_local std::string out;
		out.assign(baseTargetLower, (size_t)(at - baseTargetLower));
		out.append(rule.baseSystem, stemLen);
		out.append(rule.forkSuffix);
		out.append(".vpcf");
		out.append(at + baseLen);
		return out.c_str();
	}
	return nullptr;
}

const char* PerWeaponFx_ActiveWeaponClass() {
	static thread_local char copy[sizeof(g_weaponClass)];
	std::lock_guard<std::mutex> lock(g_mx);
	std::memcpy(copy, g_weaponClass, sizeof(copy));
	return copy[0] ? copy : "-";
}
