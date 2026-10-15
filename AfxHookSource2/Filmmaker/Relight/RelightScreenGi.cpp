#define NOMINMAX
#include "RelightProbes.h"
#include "RelightScreenGiShaders.h"
#include "../Environment/EnvironmentShaderCompiler.h"
#include "../Render/RenderPrivateTextureScope.h"
#include "../Debug/MvmDebugLog.h"
#include <wrl/client.h>
#include <d3dcompiler.h>
namespace Filmmaker::Relight {
using Microsoft::WRL::ComPtr;
namespace {
struct State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> work;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11Texture3D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11UnorderedAccessView> output;
    UINT width=0,height=0;
    uint64_t frame=~uint64_t(0);
    ID3D11ShaderResourceView* positions=nullptr;
    ID3D11ShaderResourceView* irradiance=nullptr;
    Environment::Matrix matrix{};
} state;
struct Constants {ProbeRow scale,bias,lower,upper,camera,size;Environment::Matrix matrix;};
}
void EndScreenGi(){state=State{};}
ID3D11ShaderResourceView* PrepareScreenGi(ID3D11Device* device,ID3D11DeviceContext* immediate,
    const ProbeBinding& probe,ID3D11ShaderResourceView* positions,UINT width,UINT height,
    Vec3 camera,const Environment::Matrix& matrix,uint64_t frame){
    if(!device||!immediate||!positions||!width||!height||!probe.distance||!probe.irradiance)return nullptr;
    if(state.device.Get()!=device){EndScreenGi();state.device=device;}
    if(state.frame==frame&&state.positions==positions&&state.irradiance==probe.irradiance&&state.matrix==matrix&&
       state.width==width&&state.height==height)return state.view.Get();
    RenderPrivateTextureScope privateTextures;
    if(!state.shader){
        ComPtr<ID3DBlob> code,error;
        if(FAILED(Environment::CompileEnvironmentShader(ScreenGiShader,sizeof(ScreenGiShader)-1,
            "RelightScreenGi",nullptr,"Build","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,&code,&error))||
           FAILED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&state.shader))){
            MvmDebugLog_LinefAlways("relight.gi.screen","compile failed: %s",error?static_cast<const char*>(error->GetBufferPointer()):"unavailable");return nullptr;}
    }
    if(!state.work&&FAILED(device->CreateDeferredContext(0,&state.work)))return nullptr;
    if(!state.constants){D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(Constants);d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(device->CreateBuffer(&d,nullptr,&state.constants)))return nullptr;}
    if(!state.texture||state.width!=width||state.height!=height){
        state.texture.Reset();state.view.Reset();state.output.Reset();
        D3D11_TEXTURE3D_DESC d{};d.Width=width;d.Height=height;d.Depth=6;d.MipLevels=1;
        d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        if(FAILED(device->CreateTexture3D(&d,nullptr,&state.texture))||
           FAILED(device->CreateShaderResourceView(state.texture.Get(),nullptr,&state.view))||
           FAILED(device->CreateUnorderedAccessView(state.texture.Get(),nullptr,&state.output)))return nullptr;
        state.width=width;state.height=height;
    }
    Constants constants{};
    for(unsigned a=0;a<3;++a){constants.scale[a]=probe.packing.probe[a][a];constants.bias[a]=probe.packing.probe[a][3];
        constants.lower[a]=probe.packing.volume[3][a]*(a==2?6.f:1.f);
        constants.upper[a]=probe.packing.volume[4][a]*(a==2?6.f:1.f);}
    constants.camera={camera.x,camera.y,camera.z,0};constants.size={float(width),float(height),0,0};constants.matrix=matrix;
    auto work=state.work.Get();work->ClearState();work->UpdateSubresource(state.constants.Get(),0,nullptr,&constants,0,0);
    auto cb=state.constants.Get();work->CSSetConstantBuffers(0,1,&cb);
    ID3D11ShaderResourceView* inputs[]{positions,probe.irradiance,probe.distance};work->CSSetShaderResources(0,3,inputs);
    auto output=state.output.Get();work->CSSetUnorderedAccessViews(0,1,&output,nullptr);
    work->CSSetShader(state.shader.Get(),nullptr,0);work->Dispatch((width+7)/8,(height+7)/8,1);work->ClearState();
    ComPtr<ID3D11CommandList> list;if(FAILED(work->FinishCommandList(FALSE,&list)))return nullptr;
    immediate->ExecuteCommandList(list.Get(),TRUE);
    state.frame=frame;state.positions=positions;state.irradiance=probe.irradiance;state.matrix=matrix;
    if(MvmDebugLog_Active()){static ULONGLONG last=0;auto now=GetTickCount64();if(now-last>=3000){last=now;
        MvmDebugLog_LinefAlways("relight.gi.screen","prepared=%ux%u bands=6 frame=%llu",width,height,frame);}}
    return state.view.Get();
}
}
