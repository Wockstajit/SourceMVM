#pragma once

// Players Only / Green Screen.
//
// The stream scheduler owns the extra-pass bracket; this module owns the pass-local subject
// vocabulary, the prepared chroma material, diagnostics, and the live-preview arm. SceneSystem
// calls the small render-thread queries below while it is already inside a safe drawable-batch
// detour. The module never disables world, particles, or smoke.

class CMaterial2;
namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

enum class PlayersOnlyGreenDomain : int {
	World = 0,
	Players,
	PlayerEquipment,
	Viewmodel,
	Effect,
	Unknown
};

// Main-thread material preparation. The blocking resolve is intentionally done before a render
// job can submit its first extra pass, so a scene draw never causes a synchronous resource load.
bool RenderPlayersOnlyGreen_PrepareMaterial();

// Live workspace state. A live preview paints the normal scene in-place; a recorder pass uses the
// render-pass state below instead, so the beauty/main pass is never recoloured by a job.
void RenderPlayersOnlyGreen_RunMainThreadFrame();
void RenderPlayersOnlyGreen_OnLevelInit();
void RenderPlayersOnlyGreen_SetArmed(bool armed, const char* reason);
bool RenderPlayersOnlyGreen_IsArmed();
bool RenderPlayersOnlyGreen_Active();

// Reliable render-command bracket used by the dedicated stream. `passOrdinal` is the in-stream
// pass number, not an engine-thread guess. `includeSmoke` is diagnostic metadata and deliberately
// stays true for this pass.
void RenderPlayersOnlyGreen_SetRenderPassActive(bool active, int passOrdinal, bool includeSmoke);
bool RenderPlayersOnlyGreen_RenderActive();
// Gates the D3D indirect-draw skip (GPU-driven instanced scatter: grass tufts, ground
// litter). Console: render playersonlygreen indirect 0|1.
bool RenderPlayersOnlyGreen_IndirectSkipEnabled();
// Shader-name vocabulary for the D3D-level draw skip (builtins + the console-extendable list;
// `render playersonlygreen skipshader add <substr>`). Called on classification cache misses
// only. The generation counter bumps whenever the dynamic list changes, so the caller can
// invalidate its pointer caches.
bool RenderPlayersOnlyGreen_ShaderNameIsSkipped(const char* name);
unsigned RenderPlayersOnlyGreen_SkipShaderGeneration();
// Render-thread report: this material classified skip-class in a scene batch. The main-thread
// pump then pokes the MATERIAL ITSELF (alpha-test reference 2.0, green colour tint) -- the only
// mechanism that reaches draws replayed from level-lifetime cached command lists, which ignore
// every draw-time treatment. Restored on disarm; dropped without restoring on level teardown.
void RenderPlayersOnlyGreen_NoteSkipClassMaterial(CMaterial2* material);
// True once the pump has inspected the material and found it OPAQUE (no alpha-test reference):
// those skip-class elements go back onto the ordinary chroma repaint path, which is safe for
// opaque pipelines and the only thing that makes a dark rock chunk flat green.
bool RenderPlayersOnlyGreen_SkipClassWantsChroma(CMaterial2* material);

// Non-POV cameras may legitimately draw shared third-person arms. POV draws must reject the same
// material path because it is the first-person viewmodel. The job driver sets this once per job.
void RenderPlayersOnlyGreen_SetViewmodelPlayerSide(bool playerSide);
bool RenderPlayersOnlyGreen_ViewmodelIsPlayerSide();

CMaterial2* RenderPlayersOnlyGreen_Material();
// Swap target for SKIP-CLASS elements: green albedo + white tint, so pipelines without a
// self-illum combo render green instead of black. May be null (falls back to the world chroma).
CMaterial2* RenderPlayersOnlyGreen_SkipMaterial();
// tools/toolsinvisible, prepared alongside the chroma material. Skip-class elements inside MIXED
// batches are pointed at this instead of the chroma; may be null (falls back to the chroma).
CMaterial2* RenderPlayersOnlyGreen_InvisibleMaterial();
PlayersOnlyGreenDomain RenderPlayersOnlyGreen_Classify(const char* materialName);
bool RenderPlayersOnlyGreen_Admits(PlayersOnlyGreenDomain domain);
bool RenderPlayersOnlyGreen_IsEffectMaterial(const char* materialName);
// Surface decals (including blood/gore decal materials) must be rejected as whole batches.
// Painting their translucent atlas with the opaque chroma material produces black splats/halos.
bool RenderPlayersOnlyGreen_IsDecalMaterial(const char* materialName);
// Detail overlays (grass tufts, ground debris kits) leak with their ORIGINAL textures even
// though the per-element material pointer is measurably swapped (replaced=1 in the draw log,
// de_cache 2026-08-20): their alpha-tested csgo_foliage/debris pipelines do not consume the
// batch-element material our hook rewrites. The only reliable treatment is the decal one --
// skip the whole batch. The ground they decorate is repainted green underneath, so skipping
// costs no coverage and no depth.
bool RenderPlayersOnlyGreen_IsDetailOverlayMaterial(const char* materialName);
// Batch-skip vocabulary: decals plus detail overlays.
bool RenderPlayersOnlyGreen_IsSkippedSceneMaterial(const char* materialName);
void RenderPlayersOnlyGreen_NoteDraw(PlayersOnlyGreenDomain domain, bool replaced);
void RenderPlayersOnlyGreen_NoteUnknown(const char* materialName);
void RenderPlayersOnlyGreen_NoteBatchSkipped(const char* source, int materialCount,
	const char* firstMaterialName);

// The scene-target clear hook asks for the exact self-illuminated key colour, rather than
// hard-coding a colour in D3D code that also handles unrelated UI/backbuffer clears.
void RenderPlayersOnlyGreen_KeyColor(float outRgba[4]);

// Console aliases: `mirv_filmmaker render playersonlygreen on|off`, `playersgreen`, and
// `greenscreen`. The render-pass command surface calls the same arm state for live preview.
void RenderPlayersOnlyGreen_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
