#pragma once

// Which "make the renderable stale" field writes an apply is allowed to perform.
//
// Split out of CosmeticOverrideSystem.h so the low-level write layer (CosmeticEntityAccess) can
// take it by value without depending on the policy layer that drives it -- the same
// shared-contract split as ViewFxTypes.h / CameraShakeTypes.h.
//
// These were added experimentally to try to force a demo weapon to re-composite its skin. Live
// testing (2026-06-28) showed NONE of them trigger the 3D rebuild AND at least one
// (clearUgc / initialized) makes the weapon VANISH from the HUD weapon-switch bar by blanking its
// econ identity. So every flag DEFAULTS OFF -- a normal apply writes only the real skin data
// (networked attrs + fallback fields), which is HUD-safe -- and each is independently toggleable
// ("cosmetics rebuildflags <name> 0|1") so the harmful-vs-useful one can be bisected without
// writing all of them at once. POD (all bool), so it is safe to pass by value into the
// SEH-guarded write. See Docs/archive/cosmetics-recompose-research.md.
struct CosmeticRebuildFlags {
	bool visualsDataSet = false; // C_CSWeaponBase::m_bVisualsDataSet = false (most plausible gate)
	bool clearUgc = false;       // C_CSWeaponBase::m_bClearWeaponIdentifyingUGC = true (HUD-vanish suspect)
	bool reloadEvent = false;    // C_CSWeaponBase::m_nCustomEconReloadEventId = -1 (networked reload token)
	bool initialized = false;    // C_EconItemView::m_bInitialized/m_bInitializedTags = false (desc cache)
	bool attrInit = false;       // C_EconEntity::m_bAttributesInitialized = false (entity attr re-init)
	bool imageCache = false;     // m_bRestoreCustomMaterialAfterPrecache + inventory-image cache hints
	bool attrParity = false;     // CAttributeManager::m_iReapplyProvisionParity ++ (attr cache bump)
};
