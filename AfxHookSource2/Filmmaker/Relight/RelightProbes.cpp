#define NOMINMAX
#include "RelightProbes.h"
#include "RelightStainedGlass.h"
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
}
void EndProbes(){state=State{};skyboxState=State{};EndProbeGi();EndScreenGi();EndStainedGlass();}
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
ProbeBinding AmbientProbe(ID3D11Device* device,Vec3 low,Vec3 high,Vec3 sky,Vec3 ground) {
    // The diagnostic neutral probe is immutable. Own a separate live volume so a
    // diagnostic cannot accidentally contaminate illumination in a subsequent draw.
    const auto base=NeutralProbe(device,low,high);
    if(!base.constants)return {};
    const auto cube=HemisphereCube(sky,ground);
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
ProbeBinding SkyboxAmbientProbe(ID3D11Device* device,Vec3 low,Vec3 high,Vec3 sky,Vec3 ground){
    struct Scope {Scope(){std::swap(state,skyboxState);}~Scope(){std::swap(state,skyboxState);}} scope;
    return AmbientProbe(device,low,high,sky,ground);
}
}
