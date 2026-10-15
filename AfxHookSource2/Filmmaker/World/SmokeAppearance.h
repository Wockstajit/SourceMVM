#pragma once
#include "SmokeAppearanceMath.h"
#include <d3d11.h>
#include <string>
namespace advancedfx {class ICommandArgs;}
namespace Filmmaker::SmokeAppearance {
struct State { bool enabled=false; Parameters parameters; unsigned pixelPrograms=0,computePrograms=0; unsigned long long draws=0,dispatches=0; };
State Snapshot();
void Configure(bool enabled,const Parameters& parameters);
void Reset();
// Keep all opacity/colour passes on one settings and collision-atlas snapshot.
void NextFrame();
void RunCommand(int argc,advancedfx::ICommandArgs* args);
void AppendStateJson(std::string& json);
bool CreatingShader();
void NotePixelShader(ID3D11PixelShader* shader,const void* code,size_t bytes);
void NoteComputeShader(ID3D11ComputeShader* shader,const void* code,size_t bytes);
// Capture only verified-size GPU constant buffers; never write native volume memory.
void NoteMap(ID3D11Resource* resource,D3D11_MAP mode,void* data);
void NoteUnmap(ID3D11Resource* resource);
void NoteUpdate(ID3D11Resource* resource,const D3D11_BOX* box,const void* data);
void NoteCopy(ID3D11Resource* destination,ID3D11Resource* source,UINT offset=0,const D3D11_BOX* sourceBox=nullptr);
void NoteVertexShader(ID3D11VertexShader* shader,const void* code,size_t bytes);
class Scope {
public:
    explicit Scope(ID3D11DeviceContext* ctx,bool enabled=true,bool compute=false);
    ~Scope();
    Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
private:
    ID3D11DeviceContext* context=nullptr;ID3D11DeviceChild* original=nullptr;
    ID3D11Buffer* originalBuffer=nullptr;bool computeStage=false;
    ID3D11VertexShader* originalVertex=nullptr;ID3D11Buffer* originalVertexBuffer=nullptr;
    ID3D11ShaderResourceView* originalWarp=nullptr;ID3D11SamplerState* originalWarpSampler=nullptr;bool warpBound=false;
    ID3D11DepthStencilState* originalDepth=nullptr;UINT originalStencilReference=0;bool depthChanged=false;
    D3D11_RECT originalScissors[16]{};UINT originalScissorCount=0;bool scissorsChanged=false;
};
}
