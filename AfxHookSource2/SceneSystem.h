#pragma once

#include "Globals.h"

void HookSceneSystem(HMODULE sceneSystemDll);
void HookMaterialSystem(HMODULE materialSystemDll);

void* new_ForceUpdateSkybox(void* This);
bool SceneSystem_GetSkyColor(bool clouds,int rgba[4]);
void SceneSystem_SetSkyColor(bool clouds,bool enabled,const int rgba[4]);
bool SceneSystem_CloudsVisible();
void SceneSystem_SetCloudsVisible(bool visible);

class CMaterial2
{
public:
	virtual const char* GetName() = 0;
	virtual const char* GetShareName() = 0;
};

struct GetMaterialsArrayResult {
	uint64_t count;
	CMaterial2*** pArrMaterials;
	uint64_t unk;
};

class CResourceSystem {
public:
	CMaterial2** PreCache(const char* name);
	void GetMaterials(GetMaterialsArrayResult* out);
};

extern CResourceSystem* g_pCResourceSystem;

// Material-parameter access, resolved once by HookMaterialSystem.
//
// Exposed so feature modules can override material parameters without each re-resolving the same
// pattern-scanned functions. SceneSystem stays the home for the material-system plumbing; the
// features own their own policy and their own restore state (Filmmaker/World/CloudFx.cpp is the
// first caller). Returns nullptr when the parameter does not exist on that material, or when the
// hooks failed to resolve at all -- callers must check.
float* SceneSystem_FindMaterialParam(CMaterial2* material, const char* paramName);
void SceneSystem_UpdateMaterial(CMaterial2* material);
// SEH-guarded name read: half-constructed materials during level load fault inside the
// engine; this returns false instead. Use it in every GetMaterials() array walk.
bool SceneSystem_MaterialName(CMaterial2* material, char* buf, size_t bufLen);

// Blocking resolve of a material path ("materials/dev/gray_50.vmat") to a live CMaterial2*.
// Precaches through the resource system first (blocking load), then resolves via the material
// system, bumping the handle's strong count like the skybox swap does so the engine cannot evict
// it mid-render-job. Main thread only. Returns nullptr when the material does not exist or the
// hooks failed to resolve.
CMaterial2* SceneSystem_ResolveMaterial(const char* name);

// Dedicated Players Only / Green Screen scene policy. The stream scheduler publishes this bit
// through CRenderCommands' reliable pass queues; the live Render workspace may publish it from
// the main-thread preview arm. SceneSystem owns the draw plumbing and safe per-element material
// substitution, while RenderPlayersOnlyGreen owns classification, material preparation, and
// diagnostics.
void SceneSystem_SetPlayersOnlyGreenPolicy(bool active);
bool SceneSystem_PlayersOnlyGreenPolicyActive();
// Main-thread-only sky backdrop for the green pass. The env_sky dome material is overridden with
// the solid-green sky material (materials/skybox/sky_green.vmat) while active -- disabling the
// sky instead leaves the scene target's black clear in every sky pixel. Restored on disarm and
// map teardown; the user's own mirv_sky choice is preserved underneath.
void SceneSystem_SetPlayersOnlyGreenSkyOverride(bool active);
// Scene draws are recorded by worker command lists. During the dedicated green extra pass the
// material pointer must remain substituted until the pass has reached its reliable render
// boundary; restoring it immediately after the hook returns can make deferred commands observe
// the original world material. The pass bracket calls these two functions to flush safely.
void SceneSystem_BeginPlayersOnlyGreenPass();
void SceneSystem_RestorePlayersOnlyGreenMaterials();

// Read-only coverage report for the drawable-batch detours: every scenesystem descriptor class
// that can submit a batch, its vtable, its slot-1 Draw pointer, and whether that pointer is one we
// actually detoured. A subject filter (World Only) can only reject draws that reach a detour, so
// when something survives a filtered plate this answers "is it even passing through us?" without
// guessing. Prints through advancedfx::Message; changes nothing.
void SceneSystem_ReportDrawCoverage();


// Synchronous library/preset path; an empty name releases the sky override.
void SceneSystem_SetSkyMaterial(const char* material);

void SceneSystem_RefreshTransientSky();
