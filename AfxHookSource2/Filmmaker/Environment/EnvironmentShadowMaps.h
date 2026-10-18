#pragma once
#include "EnvironmentScene.h"
#include <d3d11.h>
#include <atomic>
namespace Filmmaker::Environment {
// Diagnostic (mirv_relight facebatch): refresh a batch of cached lamp faces through one scratch
// pair per face, so the GPU is not serialized copy -> draw -> copy on a single scratch pair.
inline std::atomic<bool> ShadowFaceBatching{true};
// Diagnostic (mirv_relight slotstable): budgeted lights keep their cache slot and the sun tier is
// keyed by static geometry, so a reordered budget does not redraw every cached face.
inline std::atomic<bool> ShadowSlotStability{true};
void BeginShadowMaps(ID3D11Device *device);
void EndShadowMaps();
// Submit cached light-space depth maps using the environment's deferred context.
// No engine shadow atlases are borrowed or cleared. No camera visibility is used for casters.
void PrepareShadowMaps(ID3D11DeviceContext *context, const Frame &frame, bool viewmodel = false,
                       bool requireWorld = false);
void BindShadowMaps(ID3D11DeviceContext *context, bool compute = false);
// Draws every posed player and world weapon (the native skinned triangles the shadow casters
// captured) from the camera into a screen-sized depth map, so the world coatings can tell exactly
// which pixels are a person. False when nobody was captured this frame; then nothing is bound.
bool PreparePlayerMask(ID3D11DeviceContext *context, const Matrix &worldToClip, UINT width, UINT height, bool reversed);
void BindPlayerMask(ID3D11DeviceContext *context); // pixel shader slot t31
// Separate experimental atlas/budget. Six fixture faces and one directional tile.
// Returns borrowed resources only after all draws for this snapshot are submitted.
bool PrepareLightingProofShadows(ID3D11DeviceContext*,const Frame&,Matrix*,ID3D11ShaderResourceView**);
// Renders one cube of depth per budgeted map light into a shared atlas and hands
// back the view-projection for every face. `matrices` receives 6 entries per light.
// Returns how many lights were rendered, which may be fewer than asked for.
// positions/ranges are the budgeted lights, which get full-resolution faces with moving
// casters composited in. staticPositions/staticRanges are the cached tier: static-geometry
// faces drawn once and kept, which is what stops an unshadowed light reaching through a
// wall. staticReady reports how many of those lights have all six faces resident yet.
unsigned PrepareMapLightShadows(ID3D11DeviceContext*,const Frame&,const Vec3* positions,
                                const float* ranges,unsigned lights,const Vec3* staticPositions,
                                const float* staticRanges,unsigned staticLights,Vec3 sunDirection,
                                Matrix* matrices,unsigned* staticReady,ID3D11ShaderResourceView** atlas);
// The runtime sky-visibility depth maps (Texture2DArray, one slice per direction in
// SkyVisibilityDirectionSet) and their whole-map projections. False until every slice is drawn.
bool SkyVisibilityMaps(ID3D11ShaderResourceView** maps,Matrix* matrices);
// Redraws the close sun cascade for the first-person pass: static world, other players (not the
// viewer's own body) and the first-person pose. Once per frame, immediate render thread.
bool PrepareViewmodelSunShadow(ID3D11DeviceContext* immediate);
// Puts the world's close sun cascade back after the first-person draws (no-op otherwise). Immediate
// render thread, at the first non-first-person lit draw after PrepareViewmodelSunShadow.
void RestoreWorldSunTile(ID3D11DeviceContext* immediate);
// Diagnostic: write the sun cascade tiles (16 matrix floats + 2048^2 depths each) to a file next frame.
void RequestSunCascadeDump(const char* path);
// Visible cached light faces with a moving actor in them that refresh every frame, nearest first
// (`mirv_relight actorshadows <n>`). Each costs a redraw of that face's casters.
unsigned ActorShadowFacesPerFrame();
void SetActorShadowFacesPerFrame(unsigned faces);
// The separate 3D-skybox sun depth; borrowed until EndShadowMaps or the next map update.
ID3D11ShaderResourceView* SkyboxSunShadow(Matrix* matrix);
// The lamp tiles' depth summary (EnvironmentShadowPyramid.h) for the lamp filter at
// NativeLampPyramidSlot, when `atlas` is the map atlas it summarizes; otherwise null.
ID3D11ShaderResourceView* LampShadowPyramid(ID3D11ShaderResourceView* atlas);
// `mirv_relight lampsummary`: off leaves the summary unbound, so every lamp runs its full disk.
bool LampShadowSummary();
void SetLampShadowSummary(bool enabled);
// Depth of the static world in each stained glass beam's projection, into the atlas's beam tiles
// (Relight::GlassBeamTile + i). Cached until the beams change. Returns how many are ready.
unsigned PrepareGlassBeamShadows(ID3D11DeviceContext* immediate,const Matrix* matrices,unsigned count);

} // namespace Filmmaker::Environment
