#pragma once

// D3D11 GPU renderer for World Effects (Little Planet 360 cubemap stereographic projection +
// 3D depth-reprojected mesh deformation for Inception, Rolling Horizon, World Wave, World Fold).

#include "WorldWarpMath.h"
#include <d3d11.h>

namespace Filmmaker::WorldWarp {

void BeginDevice(ID3D11Device* device);
void EndDevice();

// Called during Little Planet extra-pass capture for each face (0..5: Down, Fwd, Left, Back, Right, Up).
// Copies the rendered face texture and records its exact projection half-tangents for seamless stitching.
void CaptureCubemapFace(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target,
	int faceIndex, const float* projectionMatrix);

// Applies the active WorldWarp effect over `target` in place (BeforeUi2, before Panorama UI).
bool Render(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target,
	ID3D11DepthStencilView* depth, const float* worldToClip, const float* projection,
	const float camera[3], float depthMin, float depthMax, double demoTime);

} // namespace Filmmaker::WorldWarp
