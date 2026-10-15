#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <windows.h>

void HookSchemaSystem(HMODULE schemaSystemDll);

// these resolved in runtime
struct ClientDllOffsets_t {
	struct C_CSGameRulesProxy {
		ptrdiff_t m_pGameRules = 0; // C_CSGameRules*
	} C_CSGameRulesProxy;

	struct C_CSGameRules {
		ptrdiff_t m_gamePhase = 0; // int32
		ptrdiff_t m_nOvertimePlaying = 0; // int32
	} C_CSGameRules;

	struct CEntityInstance {
		ptrdiff_t m_pEntity = 0; // CEntityIdentity*
	} CEntityInstance;

	struct C_BaseEntity {
		ptrdiff_t m_pGameSceneNode = 0; // CGameSceneNode*
		ptrdiff_t m_iHealth = 0; // int32
		ptrdiff_t m_iTeamNum = 0; // uint8
		ptrdiff_t m_hOwnerEntity = 0; // CHandle<C_BaseEntity>
		ptrdiff_t m_nSubclassID = 0; // CUtlStringToken (uint32 hash at +0); knife model swap UpdateSubclass()
		ptrdiff_t m_nSceneObjectOverrideFlags = 0; // DisableShadows_t: 2 = baked, 3 = realtime
		// Client interpolation gate. The renderer skips an entity whose
		// m_bHasSuccessfullyInterpolated is still false, and interpolation needs the clock to
		// advance -- so a client-only entity SPAWNED WHILE A DEMO IS PAUSED (the Customize
		// player-preview pawn) is never drawn, no matter how correctly it is posed.
		// m_bRenderEvenWhenNotSuccessfullyInterpolated is the engine's own override for exactly
		// that case. Both are OPTIONAL/non-fatal: 0 = unresolved, and the preview-visibility
		// feature disables itself instead of writing a guessed offset.
		ptrdiff_t m_bHasSuccessfullyInterpolated = 0;               // bool
		ptrdiff_t m_bRenderEvenWhenNotSuccessfullyInterpolated = 0; // bool
		// Movement inputs for the weapon-bob research probe (WeaponBobProbe.cpp). OPTIONAL:
		// unresolved => the probe reports speed/ground as unknown rather than reading garbage.
		ptrdiff_t m_vecVelocity = 0; // CNetworkVelocityVector (3 floats)
		ptrdiff_t m_fFlags = 0;      // uint32 (bit 0 = FL_ONGROUND)
		ptrdiff_t m_MoveType = 0; // MoveType_t; optional death-weapon physics override
		ptrdiff_t m_nActualMoveType = 0; // MoveType_t; optional death-weapon physics override
	} C_BaseEntity;

	// Read-only probes for physical arm recoil; no unverified physics writes.
	struct DeathWeaponRagdoll {
		ptrdiff_t m_pRagdollControl=0, m_vecForce=0, m_nForceBone=0, m_bRagdollEnabled=0;
		ptrdiff_t m_pClientsideRagdoll=0, m_RagdollPose=0;
		ptrdiff_t m_bBuiltRagdoll=0, m_bRagdollClientSide=0;
		ptrdiff_t m_bMustSyncRagdollState=0;
		ptrdiff_t m_nRagdollDamageBone=0;
		ptrdiff_t m_vRagdollDamageForce=0;
		ptrdiff_t m_vRagdollDamagePosition=0;
		ptrdiff_t m_szRagdollDamageWeaponName=0;
		ptrdiff_t m_bRagdollDamageHeadshot=0;
	} DeathWeaponRagdoll;
	struct DeathWeaponPhysics {
		ptrdiff_t m_pVPhysicsAggregate = 0, m_nIdealMotionType = 0;
		ptrdiff_t m_nParentAttachmentOrBone = 0, m_nHierarchyType = 0;
	} DeathWeaponPhysics;

	struct C_BaseModelEntity {
		ptrdiff_t m_Glow = 0; // CGlowProperty
		ptrdiff_t m_bNoInterpolate = 0; // bool (optional; death-held weapon ownership)
	} C_BaseModelEntity;

	struct CGameSceneNode {
	    ptrdiff_t m_pOwner = 0; // CEntityInstance*
        ptrdiff_t m_pParent = 0; // CGameSceneNode*
        ptrdiff_t m_vecAbsOrigin = 0; // VectorWS
        ptrdiff_t m_pChild = 0; // CGameSceneNode*    (viewmodel scene-node walk for knife model swap)
		ptrdiff_t m_pNextSibling = 0; // CGameSceneNode* (viewmodel scene-node walk for knife model swap)
		ptrdiff_t m_vecOrigin = 0; // network/interpolation origin source
		ptrdiff_t m_angRotation = 0; // QAngle       (ViewFxVm: true viewmodel rotation; optional)
        ptrdiff_t m_vecWrappedLocalOrigin = 0; // interpolated source for scene transform rebuild
        ptrdiff_t m_angWrappedLocalRotation = 0;
        ptrdiff_t m_angAbsRotation = 0;
        ptrdiff_t m_nodeToWorld = 0; // CTransformWS (ViewFxVm: cached world transform / quat; optional)
	} CGameSceneNode;

	struct C_BaseCSGrenadeProjectile {
		ptrdiff_t m_bCanCreateGrenadeTrail = 0; // bool
		ptrdiff_t m_nSnapshotTrajectoryEffectIndex = 0; // ParticleIndex_t
		ptrdiff_t m_flTrajectoryTrailEffectCreationTime = 0; // float32

	} C_BaseCSGrenadeProjectile;

	struct C_SmokeGrenadeProjectile {
		ptrdiff_t m_vSmokeColor = 0; // Vector
		// Optional (0 = unresolved): where the environment places each smoke along the view ray.
		ptrdiff_t m_vSmokeDetonationPos = 0; // Vector
		ptrdiff_t m_bDidSmokeEffect = 0; // bool
		ptrdiff_t m_nSmokeEffectTickBegin = 0; // int, optional
	} C_SmokeGrenadeProjectile;
	struct EnvironmentVfx {
		ptrdiff_t firePositions=0, fireBurning=0, fireCount=0, fireLifetime=0, firePostEffect=0;
		ptrdiff_t grenadePinPulled=0, grenadeThrowAnimating=0, molotovIncendiary=0;
	} EnvironmentVfx;

	// info_map_parameters: the map's own weather globals (Train sets them). Optional, 0 = unresolved.
	struct CMapInfo {
		ptrdiff_t m_bRainTraceToSkyEnabled = 0;    // bool
		ptrdiff_t m_flEnvRainStrength = 0;         // float
		ptrdiff_t m_flEnvPuddleRippleStrength = 0; // float
		ptrdiff_t m_flEnvPuddleRippleDirection = 0; // float
		ptrdiff_t m_flEnvWetnessCoverage = 0;      // float
		ptrdiff_t m_flEnvWetnessDryingAmount = 0;  // float
	} CMapInfo;

	struct CBasePlayerController {
		ptrdiff_t m_iszPlayerName = 0; // char[128]
		ptrdiff_t m_steamID = 0; // uint64
		ptrdiff_t m_hPawn = 0; // CHandle< C_CSPlayerPawnBase >
	} CBasePlayerController;

	struct CCSPlayerController {
		ptrdiff_t m_sSanitizedPlayerName = 0; // CUtlString
	} CCSPlayerController;

	struct C_BasePlayerPawn {
		ptrdiff_t m_hController = 0; // CHandle< CBasePlayerController >
		ptrdiff_t m_pWeaponServices = 0; // CPlayer_WeaponServices*
		ptrdiff_t m_pObserverServices = 0; // CPlayer_ObserverServices*
		ptrdiff_t m_pCameraServices = 0; // CPlayer_CameraServices*
	} C_BasePlayerPawn;

	struct CPlayer_CameraServices {
		ptrdiff_t m_hViewEntity = 0; // CHandle< CBaseEntity >
	} CPlayer_CameraServices;

	struct CPlayer_WeaponServices {
		ptrdiff_t m_hActiveWeapon = 0; // CHandle< CBasePlayerWeapon >
	} CPlayer_WeaponServices;

	struct CPlayer_ObserverServices {
		ptrdiff_t m_iObserverMode = 0; // uint8                                   
		ptrdiff_t m_hObserverTarget  = 0; // CHandle< CBaseEntity >
	} CPlayer_ObserverServices;

	struct C_EnvSky {
		ptrdiff_t m_hSkyMaterial = 0; // CStrongHandle<InfoForResourceTypeIMaterial2>
		ptrdiff_t m_vTintColor = 0; // Color
		ptrdiff_t m_flBrightnessScale = 0; // float32
	} C_EnvSky;

	// Econ/cosmetics fields for the offline demo skin-changer (Phase 3). Resolved separately
	// and NON-FATALLY (see g_cosmeticsOffsetsOk) so a renamed field disables skins instead of
	// aborting startup. Weapons (C_CSWeaponBase) derive from C_EconEntity; setting m_iItemIDHigh
	// to -1 makes the client composite the material from the m_nFallback* fields below.
	struct C_EconEntity {
		ptrdiff_t m_OriginalOwnerXuidLow = 0;  // uint32
		ptrdiff_t m_OriginalOwnerXuidHigh = 0; // uint32
		ptrdiff_t m_nFallbackPaintKit = 0;     // int32 (paint kit / skin id)
		ptrdiff_t m_nFallbackSeed = 0;         // int32 (pattern seed)
		ptrdiff_t m_flFallbackWear = 0;        // float32 (0..1 wear)
		ptrdiff_t m_nFallbackStatTrak = 0;     // int32 (-1 = none)
		ptrdiff_t m_AttributeManager = 0;      // C_AttributeContainer (value member)
		// Experimental composite-refresh lever (Phase 2 research): distinct from
		// C_EconItemView::m_bInitialized (an inventory description-text cache flag that does NOT touch
		// the render path). Clearing this on the weapon is hypothesized to force a re-init of its econ
		// attributes / skin composite. Optional / non-fatal -- 0 = field unavailable, lever skipped.
		ptrdiff_t m_bAttributesInitialized = 0; // bool
	} C_EconEntity;

	struct C_AttributeContainer {
		ptrdiff_t m_Item = 0; // C_EconItemView (value member)
	} C_AttributeContainer;

	struct C_EconItemView {
		ptrdiff_t m_bRestoreCustomMaterialAfterPrecache = 0; // bool, optional refresh hint
		ptrdiff_t m_bDisallowSOC = 0;                        // bool, Andromeda/nerv set false for glove/skin apply
		ptrdiff_t m_bInventoryImageRgbaRequested = 0; // bool, optional cache invalidation hint
		ptrdiff_t m_bInventoryImageTriedCache = 0;    // bool, optional cache invalidation hint
		ptrdiff_t m_szCurrentLoadCachedFileName = 0;  // char[], optional cache invalidation hint
		ptrdiff_t m_iItemDefinitionIndex = 0; // uint16
		ptrdiff_t m_iEntityQuality = 0;       // int32 (set to 3 = unusual on knife/glove swap; optional)
		ptrdiff_t m_iItemID = 0;              // uint64 (full item id; Andromeda/nerv copy for glove resolve)
		ptrdiff_t m_iItemIDHigh = 0;          // uint32 (-1 forces fallback fields)
		ptrdiff_t m_iItemIDLow = 0;           // uint32
		ptrdiff_t m_iAccountID = 0;           // uint32
		ptrdiff_t m_AttributeList = 0;        // C_AttributeList (value member)
		ptrdiff_t m_NetworkedDynamicAttributes = 0; // C_AttributeList (value member); where a
		                                            // networked/spectated item's paint kit, wear,
		                                            // seed actually live (m_AttributeList is the
		                                            // local cooked list, empty for demo players).
		ptrdiff_t m_bInitialized = 0;         // bool, optional refresh hint
		ptrdiff_t m_bInitializedTags = 0;     // bool, optional refresh hint
	} C_EconItemView;

	struct CAttributeManager {
		ptrdiff_t m_CachedResults = 0;          // optional attribute-cache invalidation hint
		ptrdiff_t m_iReapplyProvisionParity = 0; // optional attribute-cache invalidation hint
	} CAttributeManager;

	struct C_BasePlayerWeapon {
		// Rounds in the magazine. OPTIONAL (non-fatal): used to prove a bullet actually left the
		// gun before a recoil shake is played, so a dry click on an empty magazine cannot kick
		// the camera. Unresolved => the shake falls back to firing on the event alone.
		ptrdiff_t m_iClip1 = 0; // int32
	} C_BasePlayerWeapon;

	struct C_CSWeaponBase {
		ptrdiff_t m_bVisualsDataSet = 0;             // optional render/econ visuals cache hint
		ptrdiff_t m_bClearWeaponIdentifyingUGC = 0;  // optional render/econ visuals cache hint
		ptrdiff_t m_nCustomEconReloadEventId = 0;    // optional render/econ visuals cache hint
		// Is this weapon mid-reload? OPTIONAL (non-fatal): the weapon-bob research probe uses it
		// to bracket the reload window in demo playback. Unresolved => the probe logs
		// inReload=-1 ("unknown") instead of a guessed false, which would look identical to
		// "reload state is not replicated in demos" -- the exact question being asked.
		ptrdiff_t m_bInReload = 0;                   // bool
	} C_CSWeaponBase;

	struct C_AttributeList {
		ptrdiff_t m_Attributes = 0; // C_UtlVectorEmbeddedNetworkVar<CEconItemAttribute>
	} C_AttributeList;

	struct CEconItemAttribute {
		ptrdiff_t m_iAttributeDefinitionIndex = 0; // uint16
		ptrdiff_t m_flValue = 0;                   // float32
		uint32_t m_size = 0;                       // schema class size / vector stride
	} CEconItemAttribute;

	// Equipped gloves item view lives directly on the player pawn (value member). Used read-only to
	// tell whether the spectated player wears custom gloves so the Customize modal can show them or
	// fall back to the team default. Optional/non-fatal: a 0 offset just means "treat as default".
	struct C_CSPlayerPawn {
		ptrdiff_t m_EconGloves = 0; // C_EconItemView (value member)
		// Glove APPLY path (Andromeda/nerv): after writing m_EconGloves, set m_bNeedToReApplyGloves and
		// fire SetBodyGroup()/UpdateBodyGroupChoice() for a few frames. m_hHudModelArms is the viewmodel
		// arms entity whose scene-node children include the first-person weapon/knife viewmodel.
		// m_flLastSpawnTimeIndex (on C_CSPlayerPawnBase) gates re-apply on spawn/round/team change.
		ptrdiff_t m_bNeedToReApplyGloves = 0; // bool
		ptrdiff_t m_hHudModelArms = 0;        // CHandle< C_CS2HudModelArms >
		ptrdiff_t m_nEconGlovesChanged = 0;   // uint8 (OnEconGlovesChanged); bump to signal visual refresh
		ptrdiff_t m_flLastSpawnTimeIndex = 0; // float (GameTime_t), resolved on C_CSPlayerPawnBase
		// Flashbang whiteout fields (resolved on C_CSPlayerPawnBase). The Customize modal zeroes
		// these each frame while open so the flash overlay never covers the customizer UI/preview.
		// Optional/non-fatal: 0 = unresolved, suppression silently skipped.
		ptrdiff_t m_flFlashMaxAlpha = 0; // float32
		ptrdiff_t m_flFlashDuration = 0; // float32
		// Method Acting's shot detector (Filmmaker/Movie/MethodActing.cpp). Counts rounds in the
		// current burst and resets to 0 when the player releases the trigger, so a CHANGE to a
		// non-zero value is one shot -- readable for every player in the demo, not just the
		// spectated one, which no weapon_fire tap can offer without a userid->pawn map.
		// Optional/non-fatal: 0 = unresolved, and the facial squint disables itself.
		ptrdiff_t m_iShotsFired = 0; // int32
	} C_CSPlayerPawn;

	// READ-ONLY model-state chain for showing which agent/player-model a spectated player wears
	// (this is a READ for the modal display; it is NOT a model swap -- the actual client-side
	// swap goes through C_BaseModelEntity::SetModel in Filmmaker/Cosmetics/, not these fields).
	// Chain: pawn + m_CBodyComponent(ptr) -> + m_skeletonInstance -> + m_modelState -> + m_ModelName.
	struct ModelChain {
		ptrdiff_t m_CBodyComponent = 0;   // C_BaseEntity::m_CBodyComponent (CBodyComponent*, a POINTER)
		ptrdiff_t m_skeletonInstance = 0; // CBodyComponentSkeletonInstance::m_skeletonInstance (embedded)
		ptrdiff_t m_modelState = 0;       // CSkeletonInstance::m_modelState (embedded CModelState)
		ptrdiff_t m_ModelName = 0;        // CModelState::m_ModelName (CUtlSymbolLarge == const char*)
		ptrdiff_t m_MeshGroupMask = 0;    // CModelState::m_MeshGroupMask (uint64) -- the LIVE rendered mesh
		                                  // group selection (legacy CS:GO vs modern CS2 mesh). Read-only
		                                  // diagnostic: shows whether SetMeshGroupMask stuck + the natural
		                                  // mask of a correctly-rendering weapon (the right bit values).
	} ModelChain;
};

extern struct ClientDllOffsets_t g_clientDllOffsets;

// True only if ALL econ/cosmetics offsets above resolved; skin overrides are gated on this.
extern bool g_cosmeticsOffsetsOk;

// https://github.com/sneakyevil/CS2-SchemaDumper/blob/main/CSchemaSystem.hpp

#define S2_PAD_INSERT(x, y) x ## y
#define S2_PAD_DEFINE(x, y) S2_PAD_INSERT(x, y)
#define S2_PAD(size) char S2_PAD_DEFINE(padding_, __LINE__)[size]

namespace SDK
{
	class CSchemaField
	{
	public:
		const char* m_szName;
		void* m_pType;
		uint32_t m_nOffset;
		uint32_t m_nMetadataSize;
		void* m_nMetadata;
	};

	class CSchemaClass
	{
	public:
		void* vfptr;
		const char* m_szName;
		const char* m_szModuleName;
		const char* m_szName2;
		uint32_t m_nSize;
		uint16_t m_nNumFields;

		S2_PAD(0x2);

		uint16_t m_nStaticSize;
		uint16_t m_nMetadataSize;

		S2_PAD(0x4);

		CSchemaField* m_pFields;
	};

	class CSchemaDeclaredClass
	{
	public:
		void* vfptr;
		const char* m_szName;
		const char* m_szModuleName;
		const char* m_szUnknownStr;
		CSchemaClass* m_Class;
	};

	class CSchemaDeclaredClassEntry
	{
	public:
		uint64_t m_nHash[2];
		CSchemaDeclaredClass* m_pDeclaredClass;
	};

	class CSchemaSystemTypeScope
	{
	public:
		void* vfptr;
		char m_szName[256];

		S2_PAD(0x368);

		uint16_t m_nNumDeclaredClasses;

		S2_PAD(0x6);

		CSchemaDeclaredClassEntry* m_pDeclaredClasses;
	};

	class CSchemaSystem
	{
	public:
		S2_PAD(0x190);

		uint64_t m_nScopeSize;
		CSchemaSystemTypeScope** m_pScopeArray;
	};
}
