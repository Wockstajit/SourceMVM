#pragma once
// WIND GUSTS: the air the Gusts control (CL_gusts) moves through rain, snow and ash, as a 2D fluid
// over the whole map (EnvironmentWindFluidShaders.h). This module steps it and hands the weather
// shaders its state: how fast the air moves at a place and how much blown mist or snow it carries.
// The buildings it flows round come from the weather cover volume. Render thread only.
#include "EnvironmentScene.h"
#include <d3d11.h>
#include <string>

namespace Filmmaker::Environment {
// Mirrors the windFluidWindow shader constant: the grid's min corner, and one over its size (zero
// when the fluid is not bound, which the shaders read as no gusts).
struct WindFluidFrameInfo {
    float originX = 0, originY = 0, perUnitX = 0, perUnitY = 0;
};
void BeginWindFluid(ID3D11Device *device);
void EndWindFluid();
// Steps the fluid when the demo's clock moved since the last call and records it into `ctx`.
// `precipitating` says rain, snow or ash is falling; without it, or with Gusts at 0, nothing runs.
WindFluidFrameInfo PrepareWindFluid(ID3D11DeviceContext *ctx, const Frame &frame, const Climate &climate, bool precipitating);
// Vertex and pixel stage: t42 the fluid state, s8 linear clamp. Binds null when there is none.
void BindWindFluid(ID3D11DeviceContext *ctx);
std::string WindFluidStatus();
// `environment wind-view`: the live fluid in a corner of the frame, from above, north up, `span`
// world units across round the camera (0 fits the whole map, negative keeps the setting).
void SetWindFluidOverlay(bool on, float span = -1);
bool WindFluidOverlay();
void DrawWindFluidOverlay(ID3D11DeviceContext *ctx, ID3D11RenderTargetView *target, unsigned width, unsigned height);
} // namespace Filmmaker::Environment
