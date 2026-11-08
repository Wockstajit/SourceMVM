#pragma once
// GROUND FOG: the low fog layer that lies on the floor and parts around players. This module owns
// its GPU half: the 2D fluid over the whole map that players push (EnvironmentGroundFogShaders.h) and the per-map floor
// map the fog is measured from (EnvironmentGroundFogMath.h). The Environment composite marches the
// layer itself (VolumeAt in EnvironmentClimateShaders.h). Render thread only.
#include "EnvironmentScene.h"
#include <d3d11.h>
#include <string>

namespace Filmmaker::Environment {
// What the composite needs to read this frame's fog; mirrors the groundFog* shader constants.
struct GroundFogFrameInfo {
    bool active = false;   // ground fog is on this frame
    bool fluid = false;    // the fluid grid is bound (otherwise the fog is at rest everywhere)
    bool floorMap = false; // the floor map is bound (otherwise the floor is the fog Base height)
    float windowX = 0, windowY = 0, windowWidth = 0, windowHeight = 0; // the grid's extent in world units
    float floorX = 0, floorY = 0, floorCell = 0, floorWidth = 0, floorHeight = 0;
    float driftX = 0, driftY = 0; // wind displacement of the billow noise, world units
};
void BeginGroundFog(ID3D11Device *device);
void EndGroundFog();
// Steps the fluid when the demo time moved since the last call (at most once per demo time, so
// the HDR and display passes of one frame share a step) and records it into `ctx`. Starts the
// floor map build for a new map and uploads it when ready.
GroundFogFrameInfo PrepareGroundFog(ID3D11DeviceContext *ctx, const Frame &frame, const Climate &climate);
// What the transparent effects did to the frame. The source returns CS2's HDR scene target of that size,
// AddRef'd, or null. Capture runs just before CS2 draws its transparent effects; Prepare, in a later
// render of that frame, compares it with the finished frame and says whether the mask is bound. Without
// a source there is no mask and the fog is measured to the surface behind every effect.
using FogSceneTargetSource = ID3D11Texture2D *(*)(unsigned width, unsigned height);
void SetFogSceneTargetSource(FogSceneTargetSource source);
void CaptureFogBeforeTransparents(ID3D11DeviceContext *immediate);
bool PrepareFogFrontMask(ID3D11DeviceContext *ctx, unsigned width, unsigned height);
// Pixel stage: t35 fluid state, t36 floor map, t37 smoothed fog amount, t38 how much fog each floor
// holds, t39 effects mask, t40 the most effects hid nearby, s6 linear clamp. Unavailable parts bind null.
void BindGroundFog(ID3D11DeviceContext *ctx);
// One line for `environment status` and the debug log.
std::string GroundFogStatus();
// `environment groundfog-dump`: on the next rendered frame, save the fluid's fog amount as a
// greyscale BMP in the debug log folder (white = heaped fog, mid grey = at rest, black = cleared)
// and log its statistics. Any thread may request; the render thread writes it.
void RequestGroundFogDump();
// Render thread, immediate context, after the composite's commands ran.
void DumpGroundFogIfRequested(ID3D11DeviceContext *immediate);
// `environment groundfog-view`: the live fluid in a corner of the frame, from above, north up. Dark is
// cleared fog, bright is heaped fog, blue is moving air; orange dots are players, green is the camera.
// `span` is how many world units across it shows around the camera: 0 fits the whole map, negative
// keeps the current setting.
inline constexpr float kGroundFogOverlaySpan = 1600.f;
void SetGroundFogOverlay(bool on, float span = -1);
bool GroundFogOverlay();
// Records the overlay over the finished frame. Does nothing unless it is on and the fluid is running.
void DrawGroundFogOverlay(ID3D11DeviceContext *ctx, ID3D11RenderTargetView *target, unsigned width, unsigned height);
} // namespace Filmmaker::Environment
