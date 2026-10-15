#pragma once

// THE single schema-offset resolver for every World/Effects feature (Lighting, Atmosphere,
// Weather & Sky, ragdoll physics). See Docs/effects/update-resilience.md.
//
// WHY THIS FILE EXISTS: the reference research these features are ported from
// (reference/coding help/{fog-night-mode,world-modulation,smoke,gravity-physics,...}) is
// almost entirely HARDCODED OFFSETS, which rot on every CS2 update -- visibly so in those
// threads. Everything here resolves BY FIELD NAME at runtime instead, which survives Valve
// moving fields around. A rename still breaks us, but that is rarer, and `mirv_filmmaker
// world health` names the exact field so the fix is one string in the table below.
//
// Misc/cs2-offsets/CS2-OFFSETS-main/ is a FIELD-NAME REFERENCE ONLY. No offset from it is ever
// compiled in; it goes stale on update day and the names are the durable part.
//
// CONTRACT:
//   - Resolution is NON-FATAL. A miss leaves the offset at 0 and records a reason.
//   - EVERY read/write site must guard on `offset != 0`.
//   - A feature whose required fields are missing must refuse to enable, WITH A NAMED REASON.
//     It must never enable and silently do nothing -- that is the exact trap
//     Docs/agent/cs2-update-recovery.md 2.4 warns about for optional fields.
//
// Fields for features that are not built yet are still listed and still resolved, so
// `world health` gives a complete picture after an update rather than only covering whatever
// happened to ship first.

#include <cstddef>

namespace Filmmaker {

// Stable ids for every world/effects schema field. Order is the report order in `world health`;
// grouping comments mark which feature consumes each block.
enum WorldField {
	// --- Night Mode / exposure (NightMode.cpp) --------------------------------------
	kWF_Ppv_MinExposure = 0,
	kWF_Ppv_MaxExposure,
	kWF_Ppv_ExposureControl,   // ignored clamp if false -- the field the UC ports omit
	kWF_Ppv_Master,            // diagnostic: which volume the map considers dominant
	kWF_Ppv_ExposureCompensation,
	kWF_Ppv_FadeDuration,
	kWF_CamSvc_ActivePpv,      // diagnostic: which volume the camera is inside

	// --- Fog (FogFx.cpp) -------------------------------------------------------------
	kWF_Fog_Color,
	kWF_Fog_StartDistance,
	kWF_Fog_EndDistance,
	kWF_Fog_MaxOpacity,
	kWF_Fog_FalloffExponent,
	kWF_Fog_HeightEnabled,
	kWF_Fog_StartHeight,
	kWF_Fog_EndHeight,
	kWF_Fog_VerticalExponent,
	kWF_Fog_Strength,
	kWF_Fog_IsEnabled,

	// --- Smoke (SmokeFx.cpp) ---------------------------------------------------------
	kWF_Smoke_Color,
	kWF_Smoke_EffectTickBegin, // when the volume spawned -- distinguishes in-flight from deployed
	kWF_Smoke_DidSmokeEffect,  // set once the volume has been created
	kWF_Smoke_VolumeDataReceived,
	kWF_Smoke_DetonationPos,   // anchor used to locate the client-side voxel volume (SmokeFx.cpp)
	// The NETWORKED voxel data. These are real schema fields (from Misc/cs2-offsets), so they are
	// name-resolved and update-proof -- unlike the hardcoded wrapper chain the forum research
	// uses, which measured INVALID on this build.
	kWF_Smoke_VoxelFrameData,
	kWF_Smoke_VoxelFrameDataSize,
	kWF_Smoke_VoxelUpdate,     // bumps whenever the volume is regenerated

	// --- Ragdoll physics (Movie/RagdollPhysics.cpp) ----------------------------------
	kWF_Ent_GravityScale,
	kWF_Ent_GravityDisabled,
	kWF_Ent_Friction,
	kWF_Ent_Elasticity,
	kWF_Ent_TimeScale,

	// --- Sun / lights (SunLight.cpp) -------------------------------------------------
	// The light's DIRECTION is its entity scene-node rotation, not a light-component field --
	// which is why this does not need the dead CLightBinnerGpu RE from
	// reference/coding help/world-modulation/734508-light-rotation.md.
	kWF_Light_Component,       // C_LightEntity::m_CLightComponent (a POINTER)
	kWF_LightC_Color,
	kWF_LightC_Brightness,
	kWF_LightC_BrightnessScale,
	kWF_LightC_CastShadows,
	kWF_LightC_SkyColor,
	kWF_LightC_SkyIntensity,
	kWF_LightC_Enabled,
	kWF_LightC_UsesBakedShadowing,
	kWF_LightC_BakedShadowIndex,
	kWF_LightC_MixedShadows,

	// --- Weather (WeatherFx.cpp) -----------------------------------------------------
	kWF_Precip_Density,
	kWF_Precip_InnerDist,
	kWF_Wind_Shared,           // C_EnvWind::m_EnvWindShared (embedded)
	kWF_WindS_InitialWindDir,
	kWF_WindS_InitialWindSpeed,
	kWF_WindS_MinWind,
	kWF_WindS_MaxWind,

	// --- Sky fog params (SkyboxUi.cpp / FogFx.cpp) -----------------------------------
	kWF_Sky_FogType,
	kWF_Sky_FogMinStart,
	kWF_Sky_FogMinEnd,
	kWF_Sky_FogMaxStart,
	kWF_Sky_FogMaxEnd,
	kWF_Sky_Enabled,

	// --- Sky orientation (SkyOrient.cpp) ---------------------------------------------
	// env_sky draws its cubemap through the entity's own scene-node transform, so yaw/roll are
	// a transform write rather than a material parameter -- sky.vfx has no rotation parameter
	// at all. Reached as entity -> m_CBodyComponent -> m_pSceneNode -> m_angRotation.
	kWF_Ent_BodyComponent,     // C_BaseEntity::m_CBodyComponent (a POINTER)
	kWF_Body_SceneNode,        // CBodyComponent::m_pSceneNode  (a POINTER)
	kWF_Node_Rotation,         // CGameSceneNode::m_angRotation
	kWF_Node_AbsRotation,      // CGameSceneNode::m_angAbsRotation
	kWF_Node_Scale,            // CGameSceneNode::m_flScale -- negative mirrors the sky
	kWF_Node_NodeToWorld,      // CGameSceneNode::m_nodeToWorld -- the matrix the RENDERER samples

	kWF_Count
};

// Resolves every field above. Idempotent and cheap to re-call; the first call does the work.
// Safe before the schema system is up (it simply resolves nothing and reports not-ready).
void WorldSchema_Init();

// True once Init has run against a live schema system and at least one field resolved.
bool WorldSchema_Ready();

// 0 == unresolved. EVERY caller must check this before using it as an offset.
std::ptrdiff_t WorldSchema_Offset(WorldField field);

// Table metadata, for `world health` and for error messages that name the broken field.
const char* WorldSchema_ClassName(WorldField field);
const char* WorldSchema_FieldName(WorldField field);

// True only if every field in `fields` resolved. Features call this with their own required
// set to decide whether they may enable at all.
bool WorldSchema_HasAll(const WorldField* fields, std::size_t count);

// Name of the first unresolved field in `fields`, or nullptr if all resolved. Used to build
// the "refusing to enable because X is missing" message.
const char* WorldSchema_FirstMissing(const WorldField* fields, std::size_t count);

} // namespace Filmmaker
