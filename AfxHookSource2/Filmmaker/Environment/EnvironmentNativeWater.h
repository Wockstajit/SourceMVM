#pragma once
#include "EnvironmentScene.h"
namespace Filmmaker::Environment {
void NativeWater_InitSchema();
// Coverage puddles use the shader film. This only tears down leftover native water entities.
void UpdateNativeWater(const Scene&,const std::string& map);
void ResetNativeWater();
}
