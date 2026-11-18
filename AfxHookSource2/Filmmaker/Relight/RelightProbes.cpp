#define NOMINMAX
#include "RelightProbes.h"
#include "RelightStainedGlass.h"
#include "RelightLiteGrid.h"
#include <wrl/client.h>
#include <cstring>
namespace Filmmaker::Relight {
using Microsoft::WRL::ComPtr;
namespace {
struct State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11ShaderResourceView> irradiance,black;
    ProbePacking packing{};
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Texture3D> ambientTexture;
    ComPtr<ID3D11ShaderResourceView> ambientView;
    AmbientCube ambientUploaded{};
} state;
State skyboxState;
struct LiteReflection {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Texture2D> cube;ComPtr<ID3D11ShaderResourceView> view;ComPtr<ID3D11Buffer> norm;
    std::array<Vec3,7> uploaded{};bool valid=false;
} liteReflection;
}
void EndProbes(){state=State{};skyboxState=State{};liteReflection=LiteReflection{};EndLiteGrid();EndProbeGi();EndScreenGi();EndStainedGlass();}
void AddLiteReflection(ID3D11Device* device,const Scene& scene,Vec3 camera,ProbeBinding& binding) {
    auto& r=liteReflection;constexpr unsigned size=LiteReflectionSize;
    if(r.device.Get()!=device){r=LiteReflection{};r.device=device;}
    if(!r.cube){
        D3D11_TEXTURE2D_DESC t{};t.Width=t.Height=size;t.MipLevels=1;t.ArraySize=6;t.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        t.SampleDesc.Count=1;t.BindFlags=D3D11_BIND_SHADER_RESOURCE;t.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};v.Format=t.Format;v.ViewDimension=D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
        v.TextureCubeArray.MipLevels=1;v.TextureCubeArray.NumCubes=1;
        D3D11_BUFFER_DESC b{};b.ByteWidth=16;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(device->CreateTexture2D(&t,nullptr,&r.cube))||FAILED(device->CreateShaderResourceView(r.cube.Get(),&v,&r.view))||
           FAILED(device->CreateBuffer(&b,nullptr,&r.norm))){r=LiteReflection{};return;}
    }
    // The cube is a function of the sky, the bounce stand-in and the fill it is normalized by.
    const auto fill=AmbientFill(scene);
    const std::array<Vec3,7> key{SkyAmbient(scene,scene.indirect),LiteGround(scene),fill[0],fill[2],fill[5],{},{}};
    if(!r.valid||std::memcmp(&r.uploaded,&key,sizeof(key))){
        ComPtr<ID3D11DeviceContext> ctx;device->GetImmediateContext(&ctx);
        std::array<uint16_t,size*size*4> texels;
        auto half=[](float value){ // float to half, for finite non-negative radiance
            uint32_t bits;const float clamped=(std::min)((std::max)(value,0.f),65000.f);std::memcpy(&bits,&clamped,4);
            const int exponent=int((bits>>23)&255)-127+15;
            return exponent<=0?uint16_t(0):uint16_t((exponent<<10)|((bits>>13)&1023));
        };
        // The material scales the cube by the receiver's ambient over the capture point's. At
        // night both the cube and that ambient are near black, and a lamp-lit room's metal stayed
        // black. Lift the two together (LiteReflectionLift): the ratio, and so the picture by day,
        // is unchanged, and a dark sky no longer zeroes the cube.
        auto row=AmbientNormalizationRow(fill);
        const float lift=LiteReflectionLift(row[3]);
        for(auto& value:row)value*=lift;
        for(unsigned face=0;face<6;++face){
            for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){
                const auto c=LiteReflectionLifted(LiteReflectionRadiance(scene,CubeFaceDirection(face,(x+.5f)/size,(y+.5f)/size)),lift);
                auto* texel=&texels[(y*size+x)*4];texel[0]=half(c.x);texel[1]=half(c.y);texel[2]=half(c.z);texel[3]=half(1);
            }
            ctx->UpdateSubresource(r.cube.Get(),face,nullptr,texels.data(),size*8,size*size*8);
        }
        ctx->UpdateSubresource(r.norm.Get(),0,nullptr,row.data(),0,0);
        r.uploaded=key;r.valid=true;
    }
    binding.reflection=r.view.Get();binding.reflectionNorm=r.norm.Get();binding.reflectionMips=1;binding.reflectionOrigin=camera;
}
ProbeBinding NeutralProbe(ID3D11Device* device,Vec3 low,Vec3 high) {
    if(state.device.Get()!=device){state=State{};state.device=device;}
    const auto packing=PackProbe(low,high,1);
    if(!state.constants||state.packing.probe!=packing.probe) {
        std::array<ProbeRow,3600> rows{};std::copy(packing.volume.begin(),packing.volume.end(),rows.begin());
        D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(rows);d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;d.Usage=D3D11_USAGE_IMMUTABLE;
        D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=rows.data();
        if(FAILED(device->CreateBuffer(&d,&initial,state.constants.ReleaseAndGetAddressOf())))return {};
        state.packing=packing;
    }
    if(!state.irradiance||!state.black) {
        D3D11_TEXTURE3D_DESC d{};d.Width=d.Height=d.MipLevels=1;d.Depth=6;
        d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        std::array<ProbeRow,6> cube{};for(auto& band:cube)band={ProbeNeutral.x,ProbeNeutral.y,ProbeNeutral.z,0};
        D3D11_SUBRESOURCE_DATA data{cube.data(),16,16};ComPtr<ID3D11Texture3D> volume;
        if(FAILED(device->CreateTexture3D(&d,&data,&volume))||
           FAILED(device->CreateShaderResourceView(volume.Get(),nullptr,state.irradiance.ReleaseAndGetAddressOf())))return {};
        const ProbeRow black{};data.pSysMem=black.data();d.Depth=1;volume.Reset();
        if(FAILED(device->CreateTexture3D(&d,&data,&volume))||
           FAILED(device->CreateShaderResourceView(volume.Get(),nullptr,state.black.ReleaseAndGetAddressOf())))return {};
    }
    if(!state.sampler) {
        D3D11_SAMPLER_DESC d{};d.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        d.AddressU=d.AddressV=d.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;d.MaxLOD=D3D11_FLOAT32_MAX;
        if(FAILED(device->CreateSamplerState(&d,&state.sampler)))return {};
    }
    return {state.constants.Get(),state.irradiance.Get(),state.black.Get(),state.packing,state.sampler.Get()};
}
ProbeBinding AmbientProbe(ID3D11Device* device,Vec3 low,Vec3 high,const AmbientCube& cube) {
    // The diagnostic neutral probe is immutable. Own a separate live volume so a
    // diagnostic cannot accidentally contaminate illumination in a subsequent draw.
    const auto base=NeutralProbe(device,low,high);
    if(!base.constants)return {};
    if(!state.ambientTexture){
        D3D11_TEXTURE3D_DESC d{};d.Width=d.Height=d.MipLevels=1;d.Depth=6;
        d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        if(FAILED(device->CreateTexture3D(&d,nullptr,&state.ambientTexture))||FAILED(device->CreateShaderResourceView(state.ambientTexture.Get(),nullptr,&state.ambientView)))return {};
        state.ambientUploaded={};state.ambientUploaded[0]={-1,-1,-1};
    }
    if(std::memcmp(&state.ambientUploaded,&cube,sizeof(cube))){
        std::array<ProbeRow,6> texels;
        for(unsigned i=0;i<6;++i)texels[i]={cube[i].x,cube[i].y,cube[i].z,1};
        ComPtr<ID3D11DeviceContext> ctx;device->GetImmediateContext(&ctx);
        ctx->UpdateSubresource(state.ambientTexture.Get(),0,nullptr,texels.data(),16,16);state.ambientUploaded=cube;
    }
    auto result=base;result.irradiance=state.ambientView.Get();return result;
}
ProbeBinding SkyboxAmbientProbe(ID3D11Device* device,Vec3 low,Vec3 high,const AmbientCube& cube){
    struct Scope {Scope(){std::swap(state,skyboxState);}~Scope(){std::swap(state,skyboxState);}} scope;
    return AmbientProbe(device,low,high,cube);
}
}
