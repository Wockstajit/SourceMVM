#pragma once
#include "EnvironmentScene.h"
#include <d3d11.h>
namespace Filmmaker::Environment {
void BeginGi(ID3D11Device *device);
void EndGi();
void PrepareGi(ID3D11DeviceContext *context, const Frame &frame);
void BindGi(ID3D11DeviceContext *context);
} // namespace Filmmaker::Environment
