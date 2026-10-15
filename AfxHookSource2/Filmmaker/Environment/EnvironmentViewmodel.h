#pragma once
#include <d3d11.h>
#include <cstddef>
namespace Filmmaker::Environment {
void NoteViewmodelShader(ID3D11VertexShader*,const void*,size_t);
void ObserveViewmodel(ID3D11DeviceContext*);
void NextViewmodelFrame();
void ResetViewmodel();
bool BindViewmodel(ID3D11DeviceContext*,ID3D11Texture2D*);
void ProbeViewmodel(ID3D11DeviceContext*,ID3D11Texture2D*,float,float,float);
}
