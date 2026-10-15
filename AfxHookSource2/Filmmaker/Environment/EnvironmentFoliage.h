#pragma once
#include <d3d11.h>
#include <cstddef>
#include "EnvironmentMath.h"
namespace Filmmaker::Environment {
// Native D3D draw observation only; never changes the engine's bindings or constants.
void NoteFoliageShader(ID3D11VertexShader*,const void*,size_t);
void ObserveFoliageInputs(ID3D11DeviceContext*);
void NextFoliageFrame();
void RequestFoliageInputs(bool);
void ResetFoliageInputs();
// Observe native CPU uploads without mapping a GPU resource. The values are used
// only after shader reflection proves the source buffer is the wind ABI.
void NoteFoliageConstantMap(ID3D11Resource*,D3D11_MAP,const void*);
void NoteFoliageConstantUnmap(ID3D11Resource*);
void NoteFoliageConstantUpdate(ID3D11Resource*,const D3D11_BOX*,const void*);
void InvalidateFoliageConstantCopy(ID3D11Resource*);
// Bind snapshots on the environment's own context. False means no verified native inputs.
bool BindFoliageInputs(ID3D11DeviceContext*,unsigned long long &generation);
// Current verified wind for conservative culling. Only normalized noise formats
// have a proven sample range; unknown formats retain all potentially moving draws.
bool ReadFoliageWind(ID3D11DeviceContext*,Vec3&,float& strength);
}
