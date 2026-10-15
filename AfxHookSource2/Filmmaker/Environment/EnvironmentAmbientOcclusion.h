#pragma once
#include "EnvironmentMath.h"
#include <d3d11.h>

namespace Filmmaker::Environment {
void BeginAmbientOcclusion(ID3D11Device*);
void EndAmbientOcclusion();
void NextAmbientOcclusionFrame();
// Returns an owned, borrowed RGBA visibility texture. Never returns native AO.
// The native material expects four directional visibility channels; scalar
// hemisphere visibility is replicated so its directional weighting remains valid.
// Optional runtime sky visibility (EnvironmentSkyVisibilityMath.h) folded into the same
// visibility: maps is a Texture2DArray with one slice per sky direction, matrices their
// whole-map projections. Strength 0 leaves the depth-derived AO unchanged.
// Dimensions of the texture the last AmbientOcclusion call returned (0 when neutral).
void AmbientOcclusionSize(UINT& width,UINT& height);
// Borrowed world positions, only for this frame's matching main depth target.
ID3D11ShaderResourceView* AmbientOcclusionPositions(ID3D11DeviceContext*,UINT& width,UINT& height);
struct SkyVisibilityInput {ID3D11ShaderResourceView* maps=nullptr;const Matrix* matrices=nullptr;float strength=0;};
ID3D11ShaderResourceView* AmbientOcclusion(ID3D11DeviceContext*,Vec3 camera,
    const D3D11_VIEWPORT&,ID3D11Buffer* nativeView,const Matrix& clipToWorld,float strength=1,const SkyVisibilityInput* sky=nullptr,bool prepareForGi=false);
} // namespace Filmmaker::Environment
