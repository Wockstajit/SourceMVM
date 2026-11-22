#pragma once

// D3D11 multi-pass GPU executor for Effect Nodes.

#include <d3d11.h>

namespace Filmmaker::EffectNodes {

void BeginDevice(ID3D11Device* device);
void EndDevice();

bool Render(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target,
	ID3D11DepthStencilView* depth, ID3D11ShaderResourceView* worldDepthSrv,
	const float* worldToClip, const float* projection,
	const float camera[3], const float forward[3],
	float depthMin, float depthMax, double demoTime);

} // namespace Filmmaker::EffectNodes
