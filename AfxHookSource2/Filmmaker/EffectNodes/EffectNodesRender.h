#pragma once

// D3D11 multi-pass GPU executor for Effect Nodes.

#include <d3d11.h>

namespace Filmmaker::EffectNodes {

void BeginDevice(ID3D11Device* device);
void EndDevice();
// This frame's smoke grenades as spheres (x, y, z, radius; up to 8), so Player Matte hides a player
// behind a smoke. Render thread, before Render.
void SetSmokes(const float* spheres, int count);

bool Render(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target,
	ID3D11DepthStencilView* depth, ID3D11ShaderResourceView* worldDepthSrv,
	const float* refWorldToClip, float refDepthMin, float refDepthMax,
	const float* worldToClip, const float* projection,
	const float camera[3], const float forward[3],
	float depthMin, float depthMax, double demoTime,
	int historyKey = -1);
// historyKey: which render of the frame this is (a render job draws several Beauty renders a frame,
// Everything, World only, ...). Echo Trails and Stop Motion keep a history per render, so one
// render's frames never land in another's trail.

} // namespace Filmmaker::EffectNodes
