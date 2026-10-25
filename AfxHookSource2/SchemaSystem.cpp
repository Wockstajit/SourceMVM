#include "Filmmaker/Props/ClientEntitySpawn.h"
#include "Filmmaker/Environment/EnvironmentNativeWater.h"
#include "SchemaSystem.h"
#include "Globals.h"
#include "Filmmaker/World/WorldSchema.h" // World/Effects field resolution, before the map is cleared
#include <winsock.h>
#include <fstream>
#include <string>

ClientDllOffsets_t g_clientDllOffsets;

// module name -> class name -> field name -> offset
std::unordered_map<std::string, std::unordered_map<std::string, std::unordered_map<std::string, std::ptrdiff_t>>> g_SchemaSystemOffsets;
std::unordered_map<std::string, std::unordered_map<std::string, uint32_t>> g_SchemaSystemClassSizes;

void getOffsetsFromSchemaSystem(SDK::CSchemaSystem* pSchemaSystem)
{
	void** pScopeArray = (void**)(pSchemaSystem->m_pScopeArray);

	for (uint64_t i = 0; pSchemaSystem->m_nScopeSize > i; ++i)
	{
		SDK::CSchemaSystemTypeScope* pSchemaScope = (SDK::CSchemaSystemTypeScope*)(pScopeArray[i]);

		// we don't need other modules for now
		if (!pSchemaScope || !pSchemaScope->m_pDeclaredClasses || 0 != strcmp(pSchemaScope->m_szName, "client.dll"))
		{
			continue;
		}

		std::vector<SDK::CSchemaDeclaredClassEntry> declaredClassEntries(pSchemaScope->m_nNumDeclaredClasses);
		memcpy(declaredClassEntries.data(), pSchemaScope->m_pDeclaredClasses, (pSchemaScope->m_nNumDeclaredClasses) * sizeof(SDK::CSchemaDeclaredClassEntry));

		for (uint16_t j = 0; j < pSchemaScope->m_nNumDeclaredClasses; ++j)
		{
			SDK::CSchemaDeclaredClass* pDeclaredClass = declaredClassEntries[j].m_pDeclaredClass;
			if (!pDeclaredClass) continue;

			SDK::CSchemaClass* pClass = pDeclaredClass->m_Class;
			if (!pClass) continue;

			const char* className = pClass->m_szName;
			g_SchemaSystemClassSizes[pSchemaScope->m_szName][className] = pClass->m_nSize;

			uintptr_t pClassFields = (uintptr_t)(pClass->m_pFields);
			if (pClassFields)
			{
				for (uint16_t k = 0; pClass->m_nNumFields > k; ++k)
				{
					SDK::CSchemaField* pField = (SDK::CSchemaField*)(pClassFields + sizeof(SDK::CSchemaField) * k);

					if (!pField) continue;
					if (!pField->m_pType) continue; 
					if (!pField->m_szName) continue;

					auto fieldName = pField->m_szName;
		
					size_t fieldNameSize = strlen(fieldName);
					bool isNameValid = (fieldNameSize > 0);

					for (size_t n = 0; n < fieldNameSize; ++n) {
						if (!isascii(fieldName[n])) {
							isNameValid = false;
							break;
						}
					}

					if (!isNameValid) continue;

					g_SchemaSystemOffsets[pSchemaScope->m_szName][className][fieldName] = pField->m_nOffset;
				}
			}
		
		}
	}
}

bool getOffset(ptrdiff_t* offset, std::string moduleName, std::string className, std::string fieldName)
{
	if(g_SchemaSystemOffsets.find(moduleName) == g_SchemaSystemOffsets.end()) return false;
	auto& module = g_SchemaSystemOffsets.at(moduleName);

	if(module.find(className) == module.end()) return false;
	auto& classFields = module.at(className);

	if(classFields.find(fieldName) == classFields.end()) return false;
	*offset = classFields.at(fieldName);

	return true;
}

bool getClassSize(uint32_t* size, std::string moduleName, std::string className)
{
	if(g_SchemaSystemClassSizes.find(moduleName) == g_SchemaSystemClassSizes.end()) return false;
	auto& module = g_SchemaSystemClassSizes.at(moduleName);

	if(module.find(className) == module.end()) return false;
	*size = module.at(className);

	return true;
}

void initSchemaSystemOffsets()
{
	bool bOk = true;

	bOk = bOk && getOffset(&g_clientDllOffsets.C_CSGameRulesProxy.m_pGameRules, "client.dll", "C_CSGameRulesProxy", "m_pGameRules");
	bOk = bOk && getOffset(&g_clientDllOffsets.C_CSGameRules.m_gamePhase, "client.dll", "C_CSGameRules", "m_gamePhase");
	bOk = bOk && getOffset(&g_clientDllOffsets.C_CSGameRules.m_nOvertimePlaying, "client.dll", "C_CSGameRules", "m_nOvertimePlaying");
	bOk = bOk && getOffset(&g_clientDllOffsets.CEntityInstance.m_pEntity, "client.dll", "CEntityInstance", "m_pEntity");
	bOk = bOk && getOffset(&g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode, "client.dll", "C_BaseEntity", "m_pGameSceneNode");
	bOk = bOk && getOffset(&g_clientDllOffsets.C_BaseEntity.m_iHealth, "client.dll", "C_BaseEntity", "m_iHealth");
	bOk = bOk && getOffset(&g_clientDllOffsets.C_BaseEntity.m_hOwnerEntity, "client.dll", "C_BaseEntity", "m_hOwnerEntity");
	bOk = bOk && getOffset(&g_clientDllOffsets.C_BaseEntity.m_iTeamNum, "client.dll", "C_BaseEntity", "m_iTeamNum");
	// Optional scene-object shadow override probe. A missing field must not take down the
	// mandatory entity/schema path; the baked-shadow entity probe disables itself instead.
	getOffset(&g_clientDllOffsets.C_BaseEntity.m_nSceneObjectOverrideFlags,
		"client.dll", "C_BaseEntity", "m_nSceneObjectOverrideFlags");
	bOk = bOk && getOffset(&g_clientDllOffsets.C_BaseModelEntity.m_Glow, "client.dll", "C_BaseModelEntity", "m_Glow");
	bOk = bOk && getOffset(&g_clientDllOffsets.CGameSceneNode.m_pOwner, "client.dll", "CGameSceneNode", "m_pOwner");
	bOk = bOk && getOffset(&g_clientDllOffsets.CGameSceneNode.m_pParent, "client.dll", "CGameSceneNode", "m_pParent");
	bOk = bOk && getOffset(&g_clientDllOffsets.CGameSceneNode.m_vecAbsOrigin, "client.dll", "CGameSceneNode", "m_vecAbsOrigin");
	bOk = bOk && getOffset(&g_clientDllOffsets.CBasePlayerController.m_iszPlayerName, "client.dll", "CBasePlayerController", "m_iszPlayerName");
	bOk = bOk && getOffset(&g_clientDllOffsets.CBasePlayerController.m_steamID, "client.dll", "CBasePlayerController", "m_steamID");
	bOk = bOk && getOffset(&g_clientDllOffsets.CBasePlayerController.m_hPawn, "client.dll", "CBasePlayerController", "m_hPawn");
	bOk = bOk && getOffset(&g_clientDllOffsets.CCSPlayerController.m_sSanitizedPlayerName, "client.dll", "CCSPlayerController", "m_sSanitizedPlayerName");
	bOk = bOk && getOffset(&g_clientDllOffsets.C_BasePlayerPawn.m_hController, "client.dll", "C_BasePlayerPawn", "m_hController");
	bOk = bOk && getOffset(&g_clientDllOffsets.C_BasePlayerPawn.m_pWeaponServices, "client.dll", "C_BasePlayerPawn", "m_pWeaponServices");
	bOk = bOk && getOffset(&g_clientDllOffsets.C_BasePlayerPawn.m_pObserverServices, "client.dll", "C_BasePlayerPawn", "m_pObserverServices");
	bOk = bOk && getOffset(&g_clientDllOffsets.C_BasePlayerPawn.m_pCameraServices, "client.dll", "C_BasePlayerPawn", "m_pCameraServices");
	bOk = bOk && getOffset(&g_clientDllOffsets.CPlayer_WeaponServices.m_hActiveWeapon, "client.dll", "CPlayer_WeaponServices", "m_hActiveWeapon");
	bOk = bOk && getOffset(&g_clientDllOffsets.CPlayer_CameraServices.m_hViewEntity, "client.dll", "CPlayer_CameraServices", "m_hViewEntity");
	bOk = bOk && getOffset(&g_clientDllOffsets.CPlayer_ObserverServices.m_iObserverMode, "client.dll", "CPlayer_ObserverServices", "m_iObserverMode");
	bOk = bOk && getOffset(&g_clientDllOffsets.CPlayer_ObserverServices.m_hObserverTarget, "client.dll", "CPlayer_ObserverServices", "m_hObserverTarget");
	bOk = bOk && getOffset(&g_clientDllOffsets.C_BaseCSGrenadeProjectile.m_bCanCreateGrenadeTrail, "client.dll", "C_BaseCSGrenadeProjectile", "m_bCanCreateGrenadeTrail");
	bOk = bOk && getOffset(&g_clientDllOffsets.C_BaseCSGrenadeProjectile.m_nSnapshotTrajectoryEffectIndex, "client.dll", "C_BaseCSGrenadeProjectile", "m_nSnapshotTrajectoryEffectIndex");
	bOk = bOk && getOffset(&g_clientDllOffsets.C_BaseCSGrenadeProjectile.m_flTrajectoryTrailEffectCreationTime, "client.dll", "C_BaseCSGrenadeProjectile", "m_flTrajectoryTrailEffectCreationTime");
	// Optional, off the `ok` chain: the grenade throw tick (GrenadeThrowTick) falls back to the trail time.
	getOffset(&g_clientDllOffsets.C_BaseCSGrenadeProjectile.m_vInitialPosition, "client.dll", "C_BaseCSGrenadeProjectile", "m_vInitialPosition");
	getOffset(&g_clientDllOffsets.C_BaseCSGrenadeProjectile.m_vInitialVelocity, "client.dll", "C_BaseCSGrenadeProjectile", "m_vInitialVelocity");
	getOffset(&g_clientDllOffsets.C_BaseCSGrenadeProjectile.m_nBounces, "client.dll", "C_BaseCSGrenadeProjectile", "m_nBounces");
	bOk = bOk && getOffset(&g_clientDllOffsets.C_SmokeGrenadeProjectile.m_vSmokeColor, "client.dll", "C_SmokeGrenadeProjectile", "m_vSmokeColor");
	bOk = bOk && getOffset(&g_clientDllOffsets.C_EnvSky.m_hSkyMaterial, "client.dll", "C_EnvSky", "m_hSkyMaterial");
	bOk = bOk && getOffset(&g_clientDllOffsets.C_EnvSky.m_vTintColor, "client.dll", "C_EnvSky", "m_vTintColor");
	bOk = bOk && getOffset(&g_clientDllOffsets.C_EnvSky.m_flBrightnessScale, "client.dll", "C_EnvSky", "m_flBrightnessScale");

	// Non-fatal: the paused player-preview visibility fix (Filmmaker/Panorama/PreviewRenderFlag.cpp)
	// turns itself off if either of these is renamed, rather than taking the whole tool down.
	getOffset(&g_clientDllOffsets.C_BaseEntity.m_bHasSuccessfullyInterpolated,
		"client.dll", "C_BaseEntity", "m_bHasSuccessfullyInterpolated");
	getOffset(&g_clientDllOffsets.C_BaseEntity.m_bRenderEvenWhenNotSuccessfullyInterpolated,
		"client.dll", "C_BaseEntity", "m_bRenderEvenWhenNotSuccessfullyInterpolated");

	// Non-fatal: the environment uses these to know whether its additions are in front of or
	// behind a smoke; unresolved just falls back to hiding them wherever a smoke covers.
	getOffset(&g_clientDllOffsets.C_SmokeGrenadeProjectile.m_vSmokeDetonationPos,
		"client.dll", "C_SmokeGrenadeProjectile", "m_vSmokeDetonationPos");
	getOffset(&g_clientDllOffsets.C_SmokeGrenadeProjectile.m_bDidSmokeEffect,
		"client.dll", "C_SmokeGrenadeProjectile", "m_bDidSmokeEffect");
	getOffset(&g_clientDllOffsets.C_SmokeGrenadeProjectile.m_nSmokeEffectTickBegin,
		"client.dll", "C_SmokeGrenadeProjectile", "m_nSmokeEffectTickBegin");
	// Read-only live VFX light sources. A missing field disables that source;
	// there are no fixed offsets or writes to the game's fire simulation.
	auto& vfx=g_clientDllOffsets.EnvironmentVfx;
	getOffset(&vfx.firePositions,"client.dll","C_Inferno","m_firePositions");
	getOffset(&vfx.fireBurning,"client.dll","C_Inferno","m_bFireIsBurning");
	getOffset(&vfx.fireCount,"client.dll","C_Inferno","m_fireCount");
	getOffset(&vfx.fireLifetime,"client.dll","C_Inferno","m_nFireLifetime");
	getOffset(&vfx.firePostEffect,"client.dll","C_Inferno","m_bInPostEffectTime");
	getOffset(&vfx.grenadePinPulled,"client.dll","C_BaseCSGrenade","m_bPinPulled");
	getOffset(&vfx.grenadeThrowAnimating,"client.dll","C_BaseCSGrenade","m_bThrowAnimating");
	getOffset(&vfx.molotovIncendiary,"client.dll","C_MolotovProjectile","m_bIsIncGrenade");

	// Non-fatal: native map weather (environment mapinfo). CS2 names the client class either way.
	for (const char* cls : {"C_MapInfo", "CMapInfo"}) {
		auto& m = g_clientDllOffsets.CMapInfo;
		if (m.m_flEnvRainStrength) break;
		getOffset(&m.m_bRainTraceToSkyEnabled, "client.dll", cls, "m_bRainTraceToSkyEnabled");
		getOffset(&m.m_flEnvRainStrength, "client.dll", cls, "m_flEnvRainStrength");
		getOffset(&m.m_flEnvPuddleRippleStrength, "client.dll", cls, "m_flEnvPuddleRippleStrength");
		getOffset(&m.m_flEnvPuddleRippleDirection, "client.dll", cls, "m_flEnvPuddleRippleDirection");
		getOffset(&m.m_flEnvWetnessCoverage, "client.dll", cls, "m_flEnvWetnessCoverage");
		getOffset(&m.m_flEnvWetnessDryingAmount, "client.dll", cls, "m_flEnvWetnessDryingAmount");
	}

	if (!bOk) ErrorBox(MkErrStr(__FILE__, __LINE__));
}

bool g_cosmeticsOffsetsOk = false;

// Econ/cosmetics offsets for the offline skin-changer. Resolved on a SEPARATE non-fatal path:
// a renamed schema field just disables skin overrides (g_cosmeticsOffsetsOk=false) instead of
// tripping the mandatory ErrorBox above and breaking the whole tool.
void initCosmeticsOffsets()
{
	bool ok = true;
	ok = ok && getOffset(&g_clientDllOffsets.C_EconEntity.m_OriginalOwnerXuidLow, "client.dll", "C_EconEntity", "m_OriginalOwnerXuidLow");
	ok = ok && getOffset(&g_clientDllOffsets.C_EconEntity.m_OriginalOwnerXuidHigh, "client.dll", "C_EconEntity", "m_OriginalOwnerXuidHigh");
	ok = ok && getOffset(&g_clientDllOffsets.C_EconEntity.m_nFallbackPaintKit, "client.dll", "C_EconEntity", "m_nFallbackPaintKit");
	ok = ok && getOffset(&g_clientDllOffsets.C_EconEntity.m_nFallbackSeed, "client.dll", "C_EconEntity", "m_nFallbackSeed");
	ok = ok && getOffset(&g_clientDllOffsets.C_EconEntity.m_flFallbackWear, "client.dll", "C_EconEntity", "m_flFallbackWear");
	ok = ok && getOffset(&g_clientDllOffsets.C_EconEntity.m_nFallbackStatTrak, "client.dll", "C_EconEntity", "m_nFallbackStatTrak");
	ok = ok && getOffset(&g_clientDllOffsets.C_EconEntity.m_AttributeManager, "client.dll", "C_EconEntity", "m_AttributeManager");
	ok = ok && getOffset(&g_clientDllOffsets.C_AttributeContainer.m_Item, "client.dll", "C_AttributeContainer", "m_Item");
	ok = ok && getOffset(&g_clientDllOffsets.C_EconItemView.m_iItemDefinitionIndex, "client.dll", "C_EconItemView", "m_iItemDefinitionIndex");
	getOffset(&g_clientDllOffsets.C_EconItemView.m_iItemID, "client.dll", "C_EconItemView", "m_iItemID");
	ok = ok && getOffset(&g_clientDllOffsets.C_EconItemView.m_iItemIDHigh, "client.dll", "C_EconItemView", "m_iItemIDHigh");
	ok = ok && getOffset(&g_clientDllOffsets.C_EconItemView.m_iItemIDLow, "client.dll", "C_EconItemView", "m_iItemIDLow");
	ok = ok && getOffset(&g_clientDllOffsets.C_EconItemView.m_iAccountID, "client.dll", "C_EconItemView", "m_iAccountID");
	getOffset(&g_clientDllOffsets.C_EconItemView.m_bRestoreCustomMaterialAfterPrecache, "client.dll", "C_EconItemView", "m_bRestoreCustomMaterialAfterPrecache");
	getOffset(&g_clientDllOffsets.C_EconItemView.m_bDisallowSOC, "client.dll", "C_EconItemView", "m_bDisallowSOC");
	getOffset(&g_clientDllOffsets.C_EconItemView.m_bInventoryImageRgbaRequested, "client.dll", "C_EconItemView", "m_bInventoryImageRgbaRequested");
	getOffset(&g_clientDllOffsets.C_EconItemView.m_bInventoryImageTriedCache, "client.dll", "C_EconItemView", "m_bInventoryImageTriedCache");
	getOffset(&g_clientDllOffsets.C_EconItemView.m_szCurrentLoadCachedFileName, "client.dll", "C_EconItemView", "m_szCurrentLoadCachedFileName");
	getOffset(&g_clientDllOffsets.C_EconItemView.m_AttributeList, "client.dll", "C_EconItemView", "m_AttributeList");
	getOffset(&g_clientDllOffsets.C_EconItemView.m_NetworkedDynamicAttributes, "client.dll", "C_EconItemView", "m_NetworkedDynamicAttributes");
	getOffset(&g_clientDllOffsets.CAttributeManager.m_CachedResults, "client.dll", "CAttributeManager", "m_CachedResults");
	getOffset(&g_clientDllOffsets.CAttributeManager.m_iReapplyProvisionParity, "client.dll", "CAttributeManager", "m_iReapplyProvisionParity");
	// Non-fatal: a renamed field must disable the "did a bullet actually leave" check, never
	// ErrorBox the hook. See CameraShakeStack.cpp for the fallback.
	getOffset(&g_clientDllOffsets.C_BasePlayerWeapon.m_iClip1, "client.dll", "C_BasePlayerWeapon", "m_iClip1");
	getOffset(&g_clientDllOffsets.C_CSWeaponBase.m_bVisualsDataSet, "client.dll", "C_CSWeaponBase", "m_bVisualsDataSet");
	getOffset(&g_clientDllOffsets.C_CSWeaponBase.m_bClearWeaponIdentifyingUGC, "client.dll", "C_CSWeaponBase", "m_bClearWeaponIdentifyingUGC");
	getOffset(&g_clientDllOffsets.C_CSWeaponBase.m_nCustomEconReloadEventId, "client.dll", "C_CSWeaponBase", "m_nCustomEconReloadEventId");
	// Weapon-bob / reload-impact research probe inputs. All optional and OFF the `ok` chain: a
	// renamed field must disable one probe line, never ErrorBox the hook. See WeaponBobProbe.cpp.
	getOffset(&g_clientDllOffsets.C_CSWeaponBase.m_bInReload, "client.dll", "C_CSWeaponBase", "m_bInReload");
	getOffset(&g_clientDllOffsets.C_BaseEntity.m_vecVelocity, "client.dll", "C_BaseEntity", "m_vecVelocity");
	getOffset(&g_clientDllOffsets.C_BaseEntity.m_fFlags, "client.dll", "C_BaseEntity", "m_fFlags");
	getOffset(&g_clientDllOffsets.C_BaseEntity.m_MoveType, "client.dll", "C_BaseEntity", "m_MoveType");
	getOffset(&g_clientDllOffsets.C_BaseEntity.m_nActualMoveType, "client.dll", "C_BaseEntity", "m_nActualMoveType");
	getOffset(&g_clientDllOffsets.DeathWeaponRagdoll.m_pClientsideRagdoll,"client.dll","CBaseAnimGraph","m_pClientsideRagdoll");
	getOffset(&g_clientDllOffsets.DeathWeaponRagdoll.m_RagdollPose,"client.dll","CBaseAnimGraph","m_RagdollPose");
	getOffset(&g_clientDllOffsets.DeathWeaponRagdoll.m_pRagdollControl,"client.dll","CBaseAnimGraph","m_pRagdollControl");
	getOffset(&g_clientDllOffsets.DeathWeaponRagdoll.m_vecForce,"client.dll","CBaseAnimGraph","m_vecForce");
	getOffset(&g_clientDllOffsets.DeathWeaponRagdoll.m_nForceBone,"client.dll","CBaseAnimGraph","m_nForceBone");
	getOffset(&g_clientDllOffsets.DeathWeaponRagdoll.m_bRagdollEnabled,"client.dll","CBaseAnimGraph","m_bRagdollEnabled");
	getOffset(&g_clientDllOffsets.DeathWeaponRagdoll.m_bBuiltRagdoll,"client.dll","CBaseAnimGraph","m_bBuiltRagdoll");
	getOffset(&g_clientDllOffsets.DeathWeaponRagdoll.m_bRagdollClientSide,"client.dll","CBaseAnimGraph","m_bRagdollClientSide");
	getOffset(&g_clientDllOffsets.DeathWeaponRagdoll.m_bMustSyncRagdollState,"client.dll","C_CSPlayerPawn","m_bMustSyncRagdollState");
	getOffset(&g_clientDllOffsets.DeathWeaponRagdoll.m_nRagdollDamageBone,"client.dll","C_CSPlayerPawn","m_nRagdollDamageBone");
	getOffset(&g_clientDllOffsets.DeathWeaponRagdoll.m_vRagdollDamageForce,"client.dll","C_CSPlayerPawn","m_vRagdollDamageForce");
	getOffset(&g_clientDllOffsets.DeathWeaponRagdoll.m_vRagdollDamagePosition,"client.dll","C_CSPlayerPawn","m_vRagdollDamagePosition");
	getOffset(&g_clientDllOffsets.DeathWeaponRagdoll.m_szRagdollDamageWeaponName,"client.dll","C_CSPlayerPawn","m_szRagdollDamageWeaponName");
	getOffset(&g_clientDllOffsets.DeathWeaponRagdoll.m_bRagdollDamageHeadshot,"client.dll","C_CSPlayerPawn","m_bRagdollDamageHeadshot");
	// NOTE: the schema class is "CAttributeList" (no underscore) -- the old "C_AttributeList" name
	// never matched, so m_Attributes stayed 0 and EVERY econ attribute read (paint kit/wear/seed)
	// silently failed, falling back to the m_nFallback* fields (which are 0 for networked demo items).
	getOffset(&g_clientDllOffsets.C_AttributeList.m_Attributes, "client.dll", "CAttributeList", "m_Attributes");
	getOffset(&g_clientDllOffsets.CEconItemAttribute.m_iAttributeDefinitionIndex, "client.dll", "CEconItemAttribute", "m_iAttributeDefinitionIndex");
	getOffset(&g_clientDllOffsets.CEconItemAttribute.m_flValue, "client.dll", "CEconItemAttribute", "m_flValue");
	getClassSize(&g_clientDllOffsets.CEconItemAttribute.m_size, "client.dll", "CEconItemAttribute");
	getOffset(&g_clientDllOffsets.C_EconItemView.m_bInitialized, "client.dll", "C_EconItemView", "m_bInitialized");
	getOffset(&g_clientDllOffsets.C_EconItemView.m_bInitializedTags, "client.dll", "C_EconItemView", "m_bInitializedTags");
	// Experimental composite-refresh lever on C_EconEntity (the weapon), distinct from
	// C_EconItemView::m_bInitialized. Optional/non-fatal -- guarded by != 0 at the write site, so a
	// missing field just skips this lever rather than disabling cosmetics. See Phase 2 research doc.
	getOffset(&g_clientDllOffsets.C_EconEntity.m_bAttributesInitialized, "client.dll", "C_EconEntity", "m_bAttributesInitialized");
	// Equipped gloves on the pawn -- optional (kept OFF the `ok` chain so a missing field disables
	// only the "current gloves" read, not the whole skin-changer). Guarded by != 0 at the read site.
	getOffset(&g_clientDllOffsets.C_CSPlayerPawn.m_EconGloves, "client.dll", "C_CSPlayerPawn", "m_EconGloves");
	// Model-swap (knife/glove/agent) APPLY offsets -- all optional/non-fatal (guarded by != 0 at the
	// write site). See Docs/cosmetics/cosmetics-cs2-methodology-notes.md for the Andromeda/nerv recipes.
	getOffset(&g_clientDllOffsets.C_BaseEntity.m_nSubclassID, "client.dll", "C_BaseEntity", "m_nSubclassID");
	getOffset(&g_clientDllOffsets.C_EconItemView.m_iEntityQuality, "client.dll", "C_EconItemView", "m_iEntityQuality");
	getOffset(&g_clientDllOffsets.CGameSceneNode.m_pChild, "client.dll", "CGameSceneNode", "m_pChild");
	getOffset(&g_clientDllOffsets.CGameSceneNode.m_pNextSibling, "client.dll", "CGameSceneNode", "m_pNextSibling");
	// ViewFxVm (true viewmodel rotation) -- optional/non-fatal, guarded by != 0 at the write site.
	getOffset(&g_clientDllOffsets.CGameSceneNode.m_vecOrigin, "client.dll", "CGameSceneNode", "m_vecOrigin");
	getOffset(&g_clientDllOffsets.CGameSceneNode.m_angRotation, "client.dll", "CGameSceneNode", "m_angRotation");
	getOffset(&g_clientDllOffsets.CGameSceneNode.m_vecWrappedLocalOrigin, "client.dll", "CGameSceneNode", "m_vecWrappedLocalOrigin");
	getOffset(&g_clientDllOffsets.CGameSceneNode.m_angWrappedLocalRotation, "client.dll", "CGameSceneNode", "m_angWrappedLocalRotation");
	getOffset(&g_clientDllOffsets.CGameSceneNode.m_angAbsRotation, "client.dll", "CGameSceneNode", "m_angAbsRotation");
	getOffset(&g_clientDllOffsets.CGameSceneNode.m_nodeToWorld, "client.dll", "CGameSceneNode", "m_nodeToWorld");
	getOffset(&g_clientDllOffsets.C_BaseModelEntity.m_bNoInterpolate, "client.dll", "C_BaseModelEntity", "m_bNoInterpolate");
	getOffset(&g_clientDllOffsets.C_CSPlayerPawn.m_bNeedToReApplyGloves, "client.dll", "C_CSPlayerPawn", "m_bNeedToReApplyGloves");
	getOffset(&g_clientDllOffsets.C_CSPlayerPawn.m_hHudModelArms, "client.dll", "C_CSPlayerPawn", "m_hHudModelArms");
	getOffset(&g_clientDllOffsets.C_CSPlayerPawn.m_nEconGlovesChanged, "client.dll", "C_CSPlayerPawn", "m_nEconGlovesChanged");
	getOffset(&g_clientDllOffsets.C_CSPlayerPawn.m_flLastSpawnTimeIndex, "client.dll", "C_CSPlayerPawnBase", "m_flLastSpawnTimeIndex");
	// Flashbang whiteout fields for the Customize-modal flash suppression. Optional/non-fatal;
	// try the base class first, then the derived pawn class (schema moved fields before).
	if (!getOffset(&g_clientDllOffsets.C_CSPlayerPawn.m_flFlashMaxAlpha, "client.dll", "C_CSPlayerPawnBase", "m_flFlashMaxAlpha"))
		getOffset(&g_clientDllOffsets.C_CSPlayerPawn.m_flFlashMaxAlpha, "client.dll", "C_CSPlayerPawn", "m_flFlashMaxAlpha");
	if (!getOffset(&g_clientDllOffsets.C_CSPlayerPawn.m_flFlashDuration, "client.dll", "C_CSPlayerPawnBase", "m_flFlashDuration"))
		getOffset(&g_clientDllOffsets.C_CSPlayerPawn.m_flFlashDuration, "client.dll", "C_CSPlayerPawn", "m_flFlashDuration");

	// Method Acting shot detector. Optional: a rename disables the fire squint (blinking still
	// works) rather than taking down the cosmetics path this function shares.
	if (!getOffset(&g_clientDllOffsets.C_CSPlayerPawn.m_iShotsFired, "client.dll", "C_CSPlayerPawn", "m_iShotsFired"))
		getOffset(&g_clientDllOffsets.C_CSPlayerPawn.m_iShotsFired, "client.dll", "C_CSPlayerPawnBase", "m_iShotsFired");
	// Customize Player: a dead player's controller moves to its observer pawn, and the body it left
	// behind drops its controller handle. These two links still name the body. Optional: unresolved
	// just means a dead player's body cannot be found (no first-person view on it).
	getOffset(&g_clientDllOffsets.CCSPlayerController.m_hPlayerPawn, "client.dll", "CCSPlayerController", "m_hPlayerPawn");
	if (!getOffset(&g_clientDllOffsets.C_CSPlayerPawn.m_hOriginalController, "client.dll", "C_CSPlayerPawn", "m_hOriginalController"))
		getOffset(&g_clientDllOffsets.C_CSPlayerPawn.m_hOriginalController, "client.dll", "C_CSPlayerPawnBase", "m_hOriginalController");
	// Read-only model-state chain (agent/player-model display). Off the `ok` chain: a missing field
	// just disables the agent read, not the whole skin-changer. Guarded by != 0 at the read site.
	getOffset(&g_clientDllOffsets.ModelChain.m_CBodyComponent, "client.dll", "C_BaseEntity", "m_CBodyComponent");
	getOffset(&g_clientDllOffsets.ModelChain.m_skeletonInstance, "client.dll", "CBodyComponentSkeletonInstance", "m_skeletonInstance");
	getOffset(&g_clientDllOffsets.ModelChain.m_modelState, "client.dll", "CSkeletonInstance", "m_modelState");
	getOffset(&g_clientDllOffsets.ModelChain.m_ModelName, "client.dll", "CModelState", "m_ModelName");
	getOffset(&g_clientDllOffsets.DeathWeaponPhysics.m_pVPhysicsAggregate, "client.dll", "CModelState", "m_pVPhysicsAggregate");
	getOffset(&g_clientDllOffsets.DeathWeaponPhysics.m_nIdealMotionType, "client.dll", "CModelState", "m_nIdealMotionType");
	getOffset(&g_clientDllOffsets.DeathWeaponPhysics.m_nParentAttachmentOrBone, "client.dll", "CGameSceneNode", "m_nParentAttachmentOrBone");
	getOffset(&g_clientDllOffsets.DeathWeaponPhysics.m_nHierarchyType, "client.dll", "CGameSceneNode", "m_nHierarchyType");
	getOffset(&g_clientDllOffsets.ModelChain.m_MeshGroupMask, "client.dll", "CModelState", "m_MeshGroupMask");
	g_cosmeticsOffsetsOk = ok;
}

// TEMP DIAGNOSTIC: dump every client.dll schema class whose name mentions Econ/Attribute (plus the
// player pawn) to %TEMP%\hlae_schema_econ.txt, so we can read the EXACT live field/class names the
// attribute-list reader needs. The names hardcoded in initCosmeticsOffsets ("C_AttributeList",
// "CEconItemAttribute", "m_Attributes"...) are NOT matching the current schema, leaving every skin
// read at 0. Runs once at init, before the schema map is cleared. Remove once the offsets are fixed.
void dumpEconSchemaToFile()
{
	char tmp[MAX_PATH] = {};
	GetEnvironmentVariableA("TEMP", tmp, MAX_PATH);
	std::string path = (tmp[0] ? std::string(tmp) : std::string(".")) + "\\hlae_schema_econ.txt";
	std::ofstream f(path, std::ios::trunc);
	if (!f) return;

	auto modIt = g_SchemaSystemOffsets.find("client.dll");
	if (modIt == g_SchemaSystemOffsets.end()) { f << "no client.dll schema scope\n"; return; }

	for (const auto& classEntry : modIt->second) {
		const std::string& className = classEntry.first;
		// Econ/attribute classes (skin read) + the model-state chain (agent/player-model read:
		// C_BaseEntity -> body component -> skeleton instance -> CModelState::m_ModelName).
		if (className.find("Econ") == std::string::npos
			&& className.find("Attribute") == std::string::npos
			&& className.find("Skeleton") == std::string::npos
			&& className.find("ModelState") == std::string::npos
			&& className.find("BodyComponent") == std::string::npos
			&& className.find("SceneNode") == std::string::npos
			&& className.find("Physics") == std::string::npos
			&& className.find("Keychain") == std::string::npos
			&& className.find("KeyChain") == std::string::npos
			&& className != "C_CSPlayerPawn"
			&& className != "C_BaseEntity"
			&& className != "C_BaseModelEntity")
			continue;

		uint32_t sz = 0;
		if (g_SchemaSystemClassSizes.count("client.dll")) {
			auto& sizes = g_SchemaSystemClassSizes.at("client.dll");
			auto szIt = sizes.find(className);
			if (szIt != sizes.end()) sz = szIt->second;
		}

		f << "=== " << className << " (size=" << sz << ") ===\n";
		for (const auto& fieldEntry : classEntry.second)
			f << "  " << fieldEntry.first << " @ 0x" << std::hex << fieldEntry.second << std::dec << "\n";
	}
}

void HookSchemaSystem(HMODULE schemaSystemDll)
{

   // 18000d8a7 48  89  05       MOV        qword ptr [DAT_180076730 ],RAX
   //           82  8e  06  00
   // 18000d8ae 4c  8d  0d       LEA        R9,[s_schema_list_bindings_<substring>_1800548   = "schema_list_bindings <substri
   //           0b  70  04  00
   // 18000d8b5 33  c0           XOR        EAX ,EAX
   // 18000d8b7 48  c7  05       MOV        qword ptr [DAT_18007675c ],0xc80000
   //           9a  8e  06 
   //           00  00  00 
	size_t instructionAddr = getAddress(schemaSystemDll, "48 89 05 ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 33 C0 48 C7 05");
	if (0 == instructionAddr) {
		ErrorBox(MkErrStr(__FILE__, __LINE__));	
		return;
	}

	uintptr_t _SchemaSystemInterface = instructionAddr + *(int32_t*)(instructionAddr + 3) + 7;
	SDK::CSchemaSystem* schemaSystem = (SDK::CSchemaSystem*)(_SchemaSystemInterface);

	if (!schemaSystem)
	{
		ErrorBox(MkErrStr(__FILE__, __LINE__));
		return;
	}

	getOffsetsFromSchemaSystem(schemaSystem);

	initSchemaSystemOffsets();
	initCosmeticsOffsets(); // non-fatal; must run before g_SchemaSystemOffsets is cleared
	// World/Effects features (Lighting, Atmosphere, Weather & Sky, ragdoll physics) resolve
	// their own fields by name too. Non-fatal, and like the two above it MUST run before the
	// map is cleared -- resolving later finds an empty map and every feature reports dead.
	// See Filmmaker/World/WorldSchema.h and Docs/effects/update-resilience.md.
	Filmmaker::WorldSchema_Init();
	Filmmaker::Environment::NativeWater_InitSchema();
	Filmmaker::Props::ClientEntitySpawn_InitSchema();
	dumpEconSchemaToFile(); // TEMP diagnostic; must run before the schema map is cleared below

	g_SchemaSystemOffsets.clear();
	g_SchemaSystemClassSizes.clear();
}
