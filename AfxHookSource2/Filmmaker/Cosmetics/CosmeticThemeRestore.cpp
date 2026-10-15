#include "CosmeticOverrideSystem.h"
#include "CosmeticEntityAccess.h"
#include "CosmeticDirectComposite.h"
#include "CosmeticModelSwap.h"
#include "../Debug/MvmDebugLog.h"
#include "../../SchemaSystem.h"
#include "../../ClientEntitySystem.h"
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstring>
#include <algorithm>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {
namespace {

bool ReadAttributes(unsigned char* item, ptrdiff_t list, CosmeticThemeAttributes* out) {
    const auto& o = g_clientDllOffsets;
    if (!list || !o.C_AttributeList.m_Attributes || !o.CEconItemAttribute.m_size
        || !o.CEconItemAttribute.m_iAttributeDefinitionIndex || !o.CEconItemAttribute.m_flValue)
        return false;
    *out = {};
    __try {
        const auto* vec = item + list + o.C_AttributeList.m_Attributes;
        const int count = *(const int*)vec;
        const auto* data = *(const unsigned char* const*)(vec + 8);
        if (count < 0 || count > 128 || (count && (uintptr_t)data <= 0x10000)) return false;
        for (int i = 0; i < count; ++i) {
            const auto* attr = data + i * o.CEconItemAttribute.m_size;
            if (!out->Add(*(const uint16_t*)(attr + o.CEconItemAttribute.m_iAttributeDefinitionIndex),
                *(const uint32_t*)(attr + o.CEconItemAttribute.m_flValue))) return false;
        }
        return true;
    } __except (1) { return false; }
}

bool RestoreAttributes(unsigned char* item, ptrdiff_t list, const CosmeticThemeAttributes& saved, bool local) {
    CosmeticThemeAttributes current;
    if (!ReadAttributes(item, list, &current) || (saved.present & ~current.present)) return false;
    // Only the local list grows through the named setter. Let the engine perform
    // destruction, vector relocation and notifications for attributes it added.
    if (local) {
        for (int s = 0; s < 4; ++s)
            if ((current.present & ~saved.present) & (1u << s))
                if (!RemoveCosmeticLocalAttribute(item, CosmeticThemeAttributes::Definition(s))) return false;
    }
    const auto& o = g_clientDllOffsets;
    __try {
        const auto* vec = item + list + o.C_AttributeList.m_Attributes;
        const int count = *(const int*)vec;
        auto* data = *(unsigned char* const*)(vec + 8);
        if (count < 0 || count > 128 || (count && (uintptr_t)data <= 0x10000)) return false;
        for (int i = 0; i < count; ++i) {
            auto* attr = data + i * o.CEconItemAttribute.m_size;
            const int slot = CosmeticThemeAttributes::Slot(*(const uint16_t*)(attr + o.CEconItemAttribute.m_iAttributeDefinitionIndex));
            if (slot >= 0 && (saved.present & (1u << slot)))
                *(uint32_t*)(attr + o.CEconItemAttribute.m_flValue) = saved.bits[slot];
        }
        return true;
    } __except (1) { return false; }
}

bool SnapshotFields(unsigned char* weapon, unsigned char* item, CosmeticRecordedWeapon* out, bool restore) {
    const auto& o = g_clientDllOffsets;
    if (!o.C_EconItemView.m_iItemIDHigh || !o.C_EconItemView.m_iItemIDLow || !o.C_EconItemView.m_iAccountID
        || !o.C_EconEntity.m_nFallbackPaintKit || !o.C_EconEntity.m_flFallbackWear
        || !o.C_EconEntity.m_nFallbackSeed || !o.C_EconEntity.m_nFallbackStatTrak) return false;
    __try {
        uint32_t* fields[] = {
            (uint32_t*)(item + o.C_EconItemView.m_iItemIDHigh),
            (uint32_t*)(item + o.C_EconItemView.m_iItemIDLow),
            (uint32_t*)(item + o.C_EconItemView.m_iAccountID),
            (uint32_t*)(weapon + o.C_EconEntity.m_nFallbackPaintKit),
            (uint32_t*)(weapon + o.C_EconEntity.m_flFallbackWear),
            (uint32_t*)(weapon + o.C_EconEntity.m_nFallbackSeed),
            (uint32_t*)(weapon + o.C_EconEntity.m_nFallbackStatTrak)
        };
        uint32_t* saved[] = {&out->itemHigh, &out->itemLow, &out->account,
            &out->fallbackPaint, &out->fallbackWear, &out->fallbackSeed, &out->fallbackStat};
        for (int i = 0; i < 7; ++i) {
            if (restore) *fields[i] = *saved[i];
            else *saved[i] = *fields[i];
        }
        return true;
    } __except (1) { return false; }
}

bool ReadIdentity(CEntityInstance* ent, WeaponEconRead* econ) {
    const auto& o = g_clientDllOffsets;
    __try {
        return ent && CosmeticLooksLikeWeaponEntity(ent) && CosmeticTryReadWeaponEconInfo((unsigned char*)ent,
            o.C_EconEntity.m_OriginalOwnerXuidLow, o.C_EconEntity.m_OriginalOwnerXuidHigh,
            o.C_EconEntity.m_AttributeManager, o.C_AttributeContainer.m_Item,
            o.C_EconItemView.m_iItemDefinitionIndex, econ);
    } __except (1) { return false; }
}

bool ReadWeaponSnapshot(CEntityInstance* ent, const WeaponEconRead& econ, CosmeticRecordedWeapon* out) {
    const auto& o = g_clientDllOffsets;
    uint32_t handle = ~0u;
    __try { handle = ent->GetHandle().ToInt(); } __except (1) { return false; }
    if (handle == ~0u || !econ.itemView) return false;
    out->entity = (uintptr_t)ent;
    out->handle = handle;
    out->owner = econ.xuid;
    out->definition = econ.liveDef;
    out->mesh = ReadEntityMeshGroupMask(ent);
    return out->mesh && SnapshotFields((unsigned char*)ent, econ.itemView, out, false)
        && ReadAttributes(econ.itemView, o.C_EconItemView.m_NetworkedDynamicAttributes, &out->networked)
        && ReadAttributes(econ.itemView, o.C_EconItemView.m_AttributeList, &out->local);
}

int ResolveRecordedWeapon(const CosmeticRecordedWeapon& saved, CEntityInstance** out, WeaponEconRead* econ) {
    const SOURCESDK::CS2::CBaseHandle handle(saved.handle);
    if (!handle.IsValid()) return 0;
    __try {
        auto* ent = CosmeticEntFromIndex(handle.GetEntryIndex());
        if (!ent || (uintptr_t)ent != saved.entity || ent->GetHandle().ToInt() != saved.handle) return 0;
        if (!CosmeticLooksLikeWeaponEntity(ent)) return -1; // metadata can be transient on an otherwise matching identity
        if (!ReadIdentity(ent, econ)) return -1;
        if (!saved.Matches((uintptr_t)ent, ent->GetHandle().ToInt(), econ->xuid, econ->liveDef)) return 0;
        *out = ent;
        return 1;
    } __except (1) { return -1; }
}

CEntityInstance* ResolveDeployedWeaponHolder(CEntityInstance* ent, uint32_t weaponHandle) {
    __try {
        const auto handle = ent->GetOwnerEntityHandle();
        auto* pawn = handle.IsValid() ? CosmeticEntFromIndex(handle.GetEntryIndex()) : nullptr;
        return pawn && pawn->GetHandle().ToInt() == handle.ToInt() && pawn->IsPlayerPawn()
            && pawn->GetActiveWeaponHandle().ToInt() == weaponHandle ? pawn : nullptr;
    } __except (1) { return nullptr; }
}

uint64_t ResolveCurrentWeaponProfileOwner(CEntityInstance* ent, uint32_t weaponHandle) {
    auto* pawn = ResolveDeployedWeaponHolder(ent, weaponHandle);
    if (!pawn) return 0;
    __try {
        const auto handle = pawn->GetPlayerControllerHandle();
        auto* controller = handle.IsValid() ? CosmeticEntFromIndex(handle.GetEntryIndex()) : nullptr;
        return controller && controller->GetHandle().ToInt() == handle.ToInt() && controller->IsPlayerController()
            && controller->GetPlayerPawnHandle().ToInt() == pawn->GetHandle().ToInt() ? controller->GetSteamId() : 0;
    } __except (1) { return 0; }
}

} // namespace

CosmeticRecordedWeapon* CosmeticOverrideSystem::ObserveRecordedWeapon(CEntityInstance* ent, uint64_t owner, int definition) {
    WeaponEconRead econ;
    CosmeticRecordedWeapon live;
    if (!CanRemoveCosmeticLocalAttributes() || !ReadIdentity(ent, &econ)
        || econ.liveDef != definition || !ReadWeaponSnapshot(ent, econ, &live)) return nullptr;
    auto& saved = m_recordedWeapons[live.handle];
    const bool fresh = !saved.Matches(live.entity, live.handle, live.owner, live.definition);
    const bool changed = saved.Observe(live);
    saved.profileOwner = owner;
    saved.pending = false;
    saved.restored = false;
    saved.meshSelected = false;
    saved.dueFrame = 0;
    // Preserve the native material cooldown for a full-identity survivor across seeks.
    if (saved.lastCompositeFrame)
        m_lastCompositeFrameByIdx[ent->GetHandle().GetEntryIndex()] = saved.lastCompositeFrame;
    if ((fresh || changed) && MvmDebugLog_Active()) {
        const auto& o = g_clientDllOffsets;
        MvmDebugLog_Linef("cosmetics.weapon.recorded", "observe handle=%u entity=%p item=%p def=%d fresh=%d changed=%d original={paint=%u seedBits=0x%08x network=0x%x local=0x%x mesh=%llu} offsets={local=0x%llx network=0x%llx value=0x%llx}",
            saved.handle, ent, econ.itemView, definition, fresh ? 1 : 0, changed ? 1 : 0,
            saved.fallbackPaint, saved.networked.bits[1], saved.networked.present, saved.local.present,
            (unsigned long long)saved.mesh, (unsigned long long)o.C_EconItemView.m_AttributeList,
            (unsigned long long)o.C_EconItemView.m_NetworkedDynamicAttributes, (unsigned long long)o.CEconItemAttribute.m_flValue);
    }
    return &saved;
}

void CosmeticOverrideSystem::MarkRecordedWeaponRestores(uint64_t owner, int definition) {
    size_t marked = 0;
    for (auto& entry : m_recordedWeapons) {
        auto& saved = entry.second;
        if ((owner && saved.profileOwner != owner) || (definition && saved.definition != definition)) continue;
        saved.pending = true;
        saved.restored = false;
        saved.meshSelected = false;
        saved.dueFrame = 0;
        ++marked;
    }
    if (marked && MvmDebugLog_Active())
        MvmDebugLog_LinefAlways("cosmetics.weapon.recorded", "rollback requested owner=%llu def=%d marked=%zu",
            (unsigned long long)owner, definition, marked);
}

void CosmeticOverrideSystem::RunRecordedWeaponRestores() {
    m_weaponMaterialBusy = false;
    if (!InDemoContext() || !g_cosmeticsOffsetsOk || m_seekSettleFrames || !m_meshSwapStable
        || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) return;
    const auto& o = g_clientDllOffsets;
    const bool paused = g_pEngineToClient && g_pEngineToClient->GetDemoFile()
        && g_pEngineToClient->GetDemoFile()->IsDemoPaused();
    for (auto it = m_recordedWeapons.begin(); it != m_recordedWeapons.end();) {
        auto& saved = it->second;
        // Pending work checks identity every frame; inactive baselines need only periodic pruning.
        if (!saved.pending && (m_frameCounter % 64) != 0) { ++it; continue; }
        CEntityInstance* ent = nullptr;
        WeaponEconRead econ;
        const int identity = ResolveRecordedWeapon(saved, &ent, &econ);
        if (identity < 0) { ++it; continue; } // unknown reads cannot authorize erasure or a write
        if (!identity) { it = m_recordedWeapons.erase(it); continue; }
        if (!saved.pending) { ++it; continue; }
        uint64_t profileOwner = econ.xuid;
        if (!profileOwner) profileOwner = ResolveCurrentWeaponProfileOwner(ent, saved.handle);
        const auto* profile = profileOwner ? m_store.Find(profileOwner) : nullptr;
        const auto* pick = profile ? profile->FindWeapon(saved.definition) : nullptr;
        if (pick && pick->set) {
            // A theme or a restored/new user choice owns apply, but Reset still needs the baseline.
            saved.profileOwner = profileOwner;
            saved.pending = saved.restored = saved.meshSelected = false;
            saved.dueFrame = 0;
            ++it; continue;
        }
        auto* holder = ResolveDeployedWeaponHolder(ent, saved.handle);
        constexpr uint32_t renderStages = DirectCompositeStage_OwnerMaterial
            | DirectCompositeStage_MaterialSet | DirectCompositeStage_UpdateSkin;
        const bool rendererOwned = saved.meshOwned.haveConfirmed || saved.meshOwned.haveAttempted
            || saved.meshScratchOwned.haveConfirmed || saved.meshScratchOwned.haveAttempted;
        const bool runnable = holder && m_autoComposite
            && (GetDirectCompositeStageMask() & renderStages) == renderStages
            && (m_modelSwap || (!rendererOwned && ReadEntityMeshGroupMask(ent) == saved.mesh));
        if (runnable) m_weaponMaterialBusy = true;
        if (m_frameCounter < saved.dueFrame) { ++it; continue; }
        CosmeticRecordedWeapon live;
        if (!ReadWeaponSnapshot(ent, econ, &live)) {
            saved.dueFrame = m_frameCounter + 32; ++it; continue;
        }
        if (saved.Observe(live)) { saved.restored = false; saved.meshSelected = false; }
        auto* weapon = (unsigned char*)ent;
        if (!saved.restored) {
            CosmeticRecordedWeapon actual;
            const bool ok = RestoreAttributes(econ.itemView, o.C_EconItemView.m_AttributeList, saved.local, true)
                && RestoreAttributes(econ.itemView, o.C_EconItemView.m_NetworkedDynamicAttributes, saved.networked, false)
                && SnapshotFields(weapon, econ.itemView, &saved, true)
                && ReadWeaponSnapshot(ent, econ, &actual) && saved.DataMatches(actual);
            if (MvmDebugLog_Active())
                MvmDebugLog_Linef("cosmetics.weapon.recorded", "restore handle=%u entity=%p item=%p ok=%d original={paint=%u network=0x%x local=0x%x}",
                    saved.handle, ent, econ.itemView, ok ? 1 : 0, saved.fallbackPaint, saved.networked.present, saved.local.present);
            if (!ok) { saved.dueFrame = m_frameCounter + 32; ++it; continue; }
            saved.restored = true;
        }
        // Data rollback is independent of deployment. Native material work waits for a drawn gun
        // and diagnostic render toggles; holstered/disabled jobs never hold an automated pause.
        if (!runnable) { ++it; continue; }
        if (!paused) { RequestOwedCompositePause(SOURCESDK::CS2::CBaseHandle(saved.handle).GetEntryIndex()); ++it; continue; }
        const int index = SOURCESDK::CS2::CBaseHandle(saved.handle).GetEntryIndex();
        const auto last = m_lastCompositeFrameByIdx.find(index);
        const uint64_t lastFrame = last != m_lastCompositeFrameByIdx.end()
            ? (std::max)(saved.lastCompositeFrame, last->second) : saved.lastCompositeFrame;
        if (lastFrame && m_frameCounter < lastFrame + 64) { ++it; continue; }
        if (!saved.meshSelected) {
            if (m_modelSwap) saved.AttemptMesh(saved.mesh); // intermediate scratch is owned on partial failure too
            if (m_modelSwap && !ApplyWeaponMeshMask(weapon, saved.mesh, (unsigned char*)holder, index)) {
                saved.dueFrame = m_frameCounter + 32; ++it; continue;
            }
            saved.meshSelected = true;
            saved.dueFrame = m_frameCounter + 8;
            ++it; continue;
        }
        // No named setter: it would recreate owned local attrs and round recorded fractional seeds.
        saved.lastCompositeFrame = m_frameCounter;
        const auto result = FireDirectCompositeRefresh(weapon, econ.itemView,
            o.C_EconItemView.m_bRestoreCustomMaterialAfterPrecache, m_compositeOwnerOffset, 0, 0, 0, false);
        if (!result.called || result.faulted || (result.completedStages & renderStages) != renderStages) {
            saved.dueFrame = m_frameCounter + 64; ++it; continue;
        }
        if (!(saved.networked.present & 1u) && !(saved.local.present & 1u) && !saved.fallbackPaint)
            ReleaseViewmodelComposites((unsigned char*)holder, weapon, index);
        m_lastCompositeFrameByIdx[index] = m_frameCounter;
        m_compositeOwedIdx.erase(index);
        m_owedPauseIdx.erase(index);
        m_meshFollowupCompositeDue.erase(index);
        if (m_nudgePhase == 2) m_pauseCompositeDoneFrame = m_frameCounter;
        if (MvmDebugLog_Active())
            MvmDebugLog_Linef("cosmetics.weapon.recorded", "complete handle=%u entity=%p mesh=%llu frame=%llu",
                saved.handle, ent, (unsigned long long)saved.mesh, (unsigned long long)m_frameCounter);
        it = m_recordedWeapons.erase(it);
    }
}

bool CosmeticOverrideSystem::CaptureThemePawn(CEntityInstance* controller, CEntityInstance* pawn, const char* requested) {
    CosmeticThemePawnIdentity identity;
    int health = 0;
    if (CosmeticReadThemePawnIdentity(controller, pawn, &identity, &health) != 1 || health <= 0) return false;
    char live[256] = {};
    CosmeticReadEntityModelPath(pawn, live, sizeof(live));
    if (std::strlen(live) >= sizeof(live) - 1 || !IsValidAgentModelPath(live)) return false;
    auto& saved = m_themeOriginalModels[identity.pawnHandle];
    const auto observed = saved.Observe(identity, live);
    if (observed == CosmeticThemePawn::Refused) return false;
    saved.pending = false;
    saved.restoring = false;
    saved.dueFrame = 0;
    // A false SetModel return can follow a partial write. Keep the observed owned path as well
    // as this attempt until the next valid read resolves which one the engine retained.
    saved.attempted = requested;
    if (MvmDebugLog_Active())
        MvmDebugLog_Linef("cosmetics.theme.pawn",
            "capture controller=%u pawnHandle=%u steam=%llu team=%d pawn=%p modelNameOffset=0x%llx observe=%d original='%s' live='%s' confirmed='%s' attempt='%s'",
            identity.controllerHandle, identity.pawnHandle, (unsigned long long)identity.steamId,
            identity.team, pawn, (unsigned long long)g_clientDllOffsets.ModelChain.m_ModelName,
            (int)observed, saved.original.c_str(), live, saved.confirmed.c_str(), requested);
    return true;
}

void CosmeticOverrideSystem::PruneThemePawns() {
    if (!m_theme || m_seekSettleFrames || (m_frameCounter % 64) != 0
        || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) return;
    size_t removed = 0;
    for (auto it = m_themeOriginalModels.begin(); it != m_themeOriginalModels.end();) {
        CEntityInstance* pawn = nullptr;
        CosmeticThemePawnIdentity current;
        int health = 0;
        const int result = CosmeticResolveThemePawn(it->second.identity.controllerHandle, &pawn, &current, &health);
        if (result == 0 || (result == 1 && !it->second.identity.Matches(current))) {
            it = m_themeOriginalModels.erase(it); ++removed;
        } else ++it; // an unknown/read failure cannot authorize erasure or a write
    }
    if (removed && MvmDebugLog_Active())
        MvmDebugLog_Linef("cosmetics.theme.pawn", "prune removed=%zu remaining=%zu", removed, m_themeOriginalModels.size());
}

void CosmeticOverrideSystem::RunThemePawnRestores() {
    if (m_theme || m_themeOriginalModels.empty() || !InDemoContext() || !g_cosmeticsOffsetsOk
        || m_seekSettleFrames || !m_meshSwapStable || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) return;
    for (auto it = m_themeOriginalModels.begin(); it != m_themeOriginalModels.end();) {
        auto& saved = it->second;
        if (!saved.pending || m_frameCounter < saved.dueFrame) { ++it; continue; }
        CEntityInstance* pawn = nullptr;
        CosmeticThemePawnIdentity current;
        int health = 0;
        const int result = CosmeticResolveThemePawn(saved.identity.controllerHandle, &pawn, &current, &health);
        if (result < 0) { saved.dueFrame = m_frameCounter + 32; ++it; continue; }
        if (result == 0 || !saved.identity.Matches(current)) { it = m_themeOriginalModels.erase(it); continue; }
        const auto* profile = current.steamId ? m_store.Find(current.steamId) : nullptr;
        const bool userAgent = (profile && profile->agent.set)
            || (!current.steamId && m_botAgents.count(current.controllerHandle));
        // SetModel on a dead pawn restarts ragdoll physics. A saved user choice owns normal apply.
        if (health <= 0) { saved.dueFrame = m_frameCounter + 32; ++it; continue; }
        if (userAgent) { it = m_themeOriginalModels.erase(it); continue; }
        char live[256] = {};
        CosmeticReadEntityModelPath(pawn, live, sizeof(live));
        if (std::strlen(live) >= sizeof(live) - 1 || !IsValidAgentModelPath(live)) {
            saved.dueFrame = m_frameCounter + 32; ++it; continue;
        }
        if (!saved.NeedsRestore(live)) {
            // Recorded data already restored or superseded our model; do not overwrite it.
            if (MvmDebugLog_Active())
                MvmDebugLog_Linef("cosmetics.theme.pawn", "restore unnecessary controller=%u pawnHandle=%u pawn=%p live='%s' original='%s'",
                    current.controllerHandle, current.pawnHandle, pawn, live, saved.original.c_str());
            it = m_themeOriginalModels.erase(it); continue;
        }
        saved.restoring = true;
        bool refreshed = false;
        const bool ok = ApplyAgentModel((unsigned char*)pawn, saved.original.c_str(), true, &refreshed);
        char actual[256] = {};
        CosmeticReadEntityModelPath(pawn, actual, sizeof(actual));
        const bool complete = ok && refreshed && CosmeticThemeModelEqual(saved.original, actual);
        if (MvmDebugLog_Active())
            MvmDebugLog_Linef("cosmetics.theme.pawn",
                "restore controller=%u pawnHandle=%u steam=%llu team=%d pawn=%p modelNameOffset=0x%llx ok=%d refreshed=%d complete=%d live='%s' want='%s' actual='%s'",
                current.controllerHandle, current.pawnHandle, (unsigned long long)current.steamId,
                current.team, pawn, (unsigned long long)g_clientDllOffsets.ModelChain.m_ModelName,
                ok ? 1 : 0, refreshed ? 1 : 0, complete ? 1 : 0, live, saved.original.c_str(), actual);
        if (complete) it = m_themeOriginalModels.erase(it);
        else { saved.dueFrame = m_frameCounter + 32; ++it; }
    }
}

} // namespace Filmmaker
