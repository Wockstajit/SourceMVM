#pragma once
#include "EnvironmentScene.h"
#include <d3d11.h>
namespace Filmmaker::Environment {
void RequestHardwareRayProbe(const std::string &map);
void PumpHardwareRayProbe(ID3D11Device *device, const Frame &frame);
} // namespace Filmmaker::Environment
