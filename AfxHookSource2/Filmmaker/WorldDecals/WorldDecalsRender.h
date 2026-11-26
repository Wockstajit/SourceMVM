#pragma once

// D3D11 deferred world-space decal & wall-media projector.
// Renders Text, Logos/Images, Videos on walls, Arrows, and Animated Painted Routes onto CS2's
// walls and floors by unprojecting the scene depth buffer.

#include <d3d11.h>

namespace Filmmaker::WorldDecals {

void BeginDevice(ID3D11Device* device);
void EndDevice();

bool Render(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target,
	ID3D11DepthStencilView* depth, const float* worldToClip, const float* projection,
	const float camera[3], float depthMin, float depthMax,
	const float (*actors)[4], int actorCount, double demoTime, bool recording);

} // namespace Filmmaker::WorldDecals
