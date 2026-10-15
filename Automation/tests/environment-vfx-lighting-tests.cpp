#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeSmokeMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeSmokeShaders.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentVfxLightingMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderCompiler.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <iostream>
#include <fstream>
#include <iterator>
using namespace Filmmaker::Environment;
using Microsoft::WRL::ComPtr;
void Check(bool ok,const char* message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
int main(int argc,char** argv){
    VfxLightPulse shot;shot.light.identity="vfx:muzzle";shot.start=10;shot.duration=VfxMuzzleDuration;
    Check(VfxLightEnergy(shot,10)==1,"A muzzle flash retains its peak energy");
    Check(std::abs(VfxLightEnergy(shot,10+VfxMuzzleDuration*.5)-.5f)<.001f,"Muzzle illumination decays during the shot instead of holding a lamp-like plateau");
    Check(VfxLightEnergy(shot,10+VfxMuzzleDuration*.9)<.03f,"Late muzzle smoke is not lit by a lingering flash");
    Check(VfxLightEnergy(shot,10+VfxMuzzleDuration)==0&&VfxLightEnergy(shot,9)==0,"Expired or future shots emit no muzzle light");
    Check(std::abs(VfxLightEnergy(shot,10.02)-20.f/27.f)<.001f,"Muzzle energy depends on demo time rather than render frame count");
    VfxLightPulse blast=shot;blast.light.identity="vfx:explosion";
    Check(VfxLightEnergy(blast,10+VfxMuzzleDuration*.5)==1,"Muzzle fade does not change the separate explosion envelope");
    Check(VfxMuzzleEnergy(INFINITY,10,VfxMuzzleDuration)==0,"Invalid muzzle time cannot illuminate the scene");
    Check(VfxPulseEnergy(10,10,.1)==1,"A flash starts at full energy");
    Check(VfxPulseEnergy(10.08,10,.1)>.89&&VfxPulseEnergy(10.08,10,.1)<.90,"Flash smoothly fades at the end of its lifetime");
    Check(VfxPulseEnergy(9,10,.1)==0&&VfxPulseEnergy(10.1,10,.1)==0,"A seek or expiration removes stale flashes");
    Check(VfxPulseEnergy(10.05,10,.1)==1,"A paused flash retains full energy before its fade interval");
    Check(VfxPulseEnergy(INFINITY,10,.1)==0,"Invalid time cannot produce a light");
    VfxLightPulse fire;fire.start=10;fire.duration=7.5;
    EndVfxPulse(fire,15);
    Check(VfxLightEnergy(fire,15)==1&&!VfxLightExpired(fire,15),"Fire expiration retains its current brightness");
    Check(std::abs(VfxLightEnergy(fire,15.09)-.5f)<.001f,"An extinguished fire fades gradually");
    EndVfxPulse(fire,15.5);
    Check(VfxLightEnergy(fire,15.18)==0&&VfxLightExpired(fire,15.18),"Repeated end events cannot restart a fading fire");
    VfxLightPulse held;held.start=10;held.duration=32;held.sustained=true;
    Check(VfxLightEnergy(held,40)==1,"A still-burning held wick does not dim merely because it is old");
    EndVfxPulse(held,40);Check(std::abs(VfxLightEnergy(held,40.09)-.5f)<.001f,"A sustained wick fades when its source disappears");
    Check(VfxLightEnergy(held,40.3)==0,"Fire leaves no prolonged light after its particles disappear");
    Check(VfxIgnitionEnergy(0)==0&&std::abs(VfxIgnitionEnergy(.09)-.5f)<.001f&&VfxIgnitionEnergy(.18)==1,"Fire ignition ramps up smoothly");
    float dim=2,bright=0;for(unsigned i=0;i<120;++i){const auto energy=VfxFireFlicker(i/60.,7);dim=std::min(dim,energy);bright=std::max(bright,energy);}
    Check(bright-dim>.15f&&dim>=.69f&&bright<=1.11f,"Fire has visible, bounded variation without randomized frame noise");
    Check(VfxFireFlicker(12.4,7)==VfxFireFlicker(12.4,7)&&VfxFireFlicker(12.4,7)!=VfxFireFlicker(12.4,8),"Paused fire retains its state and separated flames vary independently");
    float nativeMin=2,nativeMax=0,previous=VfxNativeFireFlicker(0,7);
    for(unsigned i=0;i<2400;++i){const float value=VfxNativeFireFlicker(i/240.,7);
        nativeMin=std::min(nativeMin,value);nativeMax=std::max(nativeMax,value);
        Check(std::abs(value-previous)<.05f,"Native fire changes smoothly between frames");previous=value;}
    Check(nativeMin>.33f&&nativeMax<1.43f&&nativeMax-nativeMin>.45f,"Native fire flickers without going dark");
    Check(VfxNativeFireFlicker(12.4,7)==VfxNativeFireFlicker(12.4,7)&&VfxNativeFireFlicker(12.4,7)!=VfxNativeFireFlicker(12.4,8),"Native fire freezes on pause and varies by source");
    float combinedMin=2,combinedMax=0;
    for(unsigned i=0;i<2400;++i){float combined=0;
        for(unsigned slot=0;slot<4;++slot)combined+=VfxNativePatchFlicker(i/240.,7,slot)*.25f;
        combinedMin=std::min(combinedMin,combined);combinedMax=std::max(combinedMax,combined);
    }
    Check(combinedMin>.33f&&combinedMax<1.43f&&combinedMax-combinedMin>.4f,"Overlapping patches keep visible fire variation without blackouts");
    std::vector<Vec3> patches;
    for(int i=0;i<12;++i){patches.push_back({float(i*8),0,0});patches.push_back({0,float(i*8),0});}
    patches.push_back({300,300,40});patches.push_back({304,300,40});
    const auto anchors=VfxFireAnchors(patches);
    Check(anchors.size()==4,"Spreading fire fits the four moving shadow sources");
    for(const auto& a:anchors){bool actual=false;for(const auto& p:patches){const auto d=a-p;if(Dot(d,d)<.001f)actual=true;}
        Check(actual,"A fire emitter stays on a real flame instead of moving into a corner wall");}
    for(const auto& p:patches){float nearest=INFINITY;for(const auto& a:anchors){const auto d=a-p;nearest=std::min(nearest,Dot(d,d));}
        Check(nearest<65*65,"Each arm of the fire spread and a remote burning patch receives a nearby emitter");}
    Check(VfxFireAnchors({}).empty(),"An extinguished inferno does not emit light");
    std::vector<Filmmaker::Relight::Light> simultaneous;
    for(int group=0;group<2;++group)for(int patch=0;patch<4;++patch){
        auto light=VfxEmitter("vfx:fire",{float(group*100+patch),0,0},{1,1,1},1000,340,48);
        light.transientGroup=group;simultaneous.push_back(light);
    }
    auto budget=Filmmaker::Relight::StableShadowBudget(simultaneous,{},4,{0,1,2,3});
    unsigned first=0,second=0;for(auto i:budget)(simultaneous[i].transientGroup==0?first:second)++;
    Check(first==2&&second==2,"Two simultaneous fires share shadow slots instead of starving the second fire");
    simultaneous.push_back(VfxEmitter("vfx:muzzle",{500,0,0},{1,1,1},1200,280,24));
    budget=Filmmaker::Relight::StableShadowBudget(simultaneous,{},4,budget);
    Check(std::find(budget.begin(),budget.end(),8)!=budget.end(),"A brief shot gets a slot even while fires occupy every previous slot");
    first=second=0;for(auto i:budget){first+=simultaneous[i].transientGroup==0;second+=simultaneous[i].transientGroup==1;}
    Check(first&&second,"A shot does not extinguish either active fire");
    simultaneous.resize(1);budget=Filmmaker::Relight::StableShadowBudget(simultaneous,{},4,budget);
    Check(budget.size()==1&&budget[0]==0,"Expired effects and stale resident indices are removed safely");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"Create D3D11 device");
    ComPtr<ID3DBlob> code,error;
    auto hr=CompileEnvironmentShader(NativeSmokeLightingShader,sizeof(NativeSmokeLightingShader)-1,"EnvironmentNativeSmoke",nullptr,"main","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&code,&error);
    if(error)std::cout<<static_cast<const char*>(error->GetBufferPointer());Check(SUCCEEDED(hr),"Compile live volume lighting");
    ComPtr<ID3D11PixelShader> pixel;Check(SUCCEEDED(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&pixel)),"Create live volume shader");
    for(int i=1;i<argc;++i){std::ifstream file(argv[i],std::ios::binary);std::vector<uint8_t> source((std::istreambuf_iterator<char>(file)),{}),patched;unsigned queries=0;
        Check(!source.empty(),"Read native shader fixture");
        Check(NativeSmokeProgram(source.data(),source.size(),code->GetBufferPointer(),code->GetBufferSize(),patched,&queries),"Link native smoke ray-march lighting");
        ComPtr<ID3D11PixelShader> linked;hr=device->CreatePixelShader(patched.data(),patched.size(),nullptr,&linked);
        std::cout<<argv[i]<<" queries="<<queries<<" create="<<std::hex<<hr<<std::dec<<'\n';
        Check(SUCCEEDED(hr)&&queries==2,"D3D11 validates both native smoke branches");
        Check(!NativeSmokeProgram(source.data(),source.size()-4,code->GetBufferPointer(),code->GetBufferSize(),patched),"Truncated shaders fail closed");
    }
    constexpr char vertexSource[]=R"HLSL(float4 main(uint id:SV_VertexID,out float3 world:TEXCOORD0,out float3 normal:TEXCOORD1):SV_Position{world=0;normal=float3(0,0,1);return float4(id==1?3:-1,id==2?3:-1,0,1);})HLSL";
    ComPtr<ID3DBlob> vertexCode;Check(SUCCEEDED(CompileEnvironmentShader(vertexSource,sizeof(vertexSource)-1,"VfxLightingTest",nullptr,"main","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&vertexCode,&error)),"Compile test vertex shader");
    ComPtr<ID3D11VertexShader> vertex;device->CreateVertexShader(vertexCode->GetBufferPointer(),vertexCode->GetBufferSize(),nullptr,&vertex);
    auto light=VfxEmitter("test",{0,0,100},{1,0,0},1000,500,16);auto record=Filmmaker::Relight::ToRecord(light,1);
    Check(record[64]==.5f,"VFX sources broaden specular highlights without editing material roughness");
    light.identity="vfx:muzzle";
    Check(!VfxOwnMuzzle(light)&&VfxViewmodelEnergy(light)==1,"Other players flashes retain world occlusion and energy");
    light.viewmodelEmitter=true;
    Check(VfxOwnMuzzle(light)&&VfxViewmodelEnergy(light)>.1f&&VfxViewmodelEnergy(light)<.3f,"Local muzzle has visible bounded first-person response");
    light.identity="vfx:fire";
    Check(!VfxOwnMuzzle(light),"Other effects cannot bypass wall occlusion as a local muzzle");
    Check(VfxViewmodelEnergy(light)==1,"Fire illumination retains its energy on weapons and hands");
    record[52]=record[53]=.5f;record[65]=1;
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(record);bd.StructureByteStride=sizeof(record);bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SUBRESOURCE_DATA initial{record.data(),0,0};ComPtr<ID3D11Buffer> records;Check(SUCCEEDED(device->CreateBuffer(&bd,&initial,&records)),"Create lighting records");ComPtr<ID3D11ShaderResourceView> recordsView;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;srv.Buffer.NumElements=1;Check(SUCCEEDED(device->CreateShaderResourceView(records.Get(),&srv,&recordsView)),"Create structured lighting view");
    struct Params{UINT count;float bias,pad[2];}params{1,.0002f,{}};
    bd={};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;initial={&params,0,0};ComPtr<ID3D11Buffer> constants;device->CreateBuffer(&bd,&initial,&constants);
    float probe[32]{};probe[4]=probe[5]=probe[6]=.5f;probe[12]=probe[13]=probe[14]=1;probe[16]=probe[17]=probe[18]=1;probe[24]=probe[25]=probe[26]=1;
    bd.ByteWidth=sizeof(probe);initial={probe,0,0};ComPtr<ID3D11Buffer> probes;device->CreateBuffer(&bd,&initial,&probes);
    float volumeData[24]{};for(unsigned i=0;i<6;++i)volumeData[i*4+2]=.1f;
    D3D11_TEXTURE3D_DESC vd{};vd.Width=vd.Height=vd.MipLevels=1;vd.Depth=6;vd.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;vd.BindFlags=D3D11_BIND_SHADER_RESOURCE;initial={volumeData,16,16};ComPtr<ID3D11Texture3D> volume;device->CreateTexture3D(&vd,&initial,&volume);ComPtr<ID3D11ShaderResourceView> volumeView;device->CreateShaderResourceView(volume.Get(),nullptr,&volumeView);
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    float shadowDepth=1;initial={&shadowDepth,4,0};ComPtr<ID3D11Texture2D> shadow;device->CreateTexture2D(&td,&initial,&shadow);ComPtr<ID3D11ShaderResourceView> shadowView;device->CreateShaderResourceView(shadow.Get(),nullptr,&shadowView);
    td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET;ComPtr<ID3D11Texture2D> output;device->CreateTexture2D(&td,nullptr,&output);ComPtr<ID3D11RenderTargetView> target;device->CreateRenderTargetView(output.Get(),nullptr,&target);
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;device->CreateTexture2D(&td,nullptr,&staging);
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;ComPtr<ID3D11SamplerState> sampler,comparison;device->CreateSamplerState(&sd,&sampler);sd.Filter=D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;sd.ComparisonFunc=D3D11_COMPARISON_LESS_EQUAL;device->CreateSamplerState(&sd,&comparison);
    ID3D11ShaderResourceView* views[]{recordsView.Get(),shadowView.Get(),volumeView.Get()};context->PSSetShaderResources(120,3,views);ID3D11Buffer* buffers[]{constants.Get(),probes.Get()};context->PSSetConstantBuffers(12,2,buffers);ID3D11SamplerState* samplers[]{sampler.Get(),comparison.Get()};context->PSSetSamplers(14,2,samplers);
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=true;ComPtr<ID3D11RasterizerState> rasterizer;device->CreateRasterizerState(&rd,&rasterizer);context->RSSetState(rasterizer.Get());
    auto rtv=target.Get();context->OMSetRenderTargets(1,&rtv,nullptr);D3D11_VIEWPORT viewport{0,0,1,1,0,1};context->RSSetViewports(1,&viewport);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(pixel.Get(),nullptr,0);
    auto draw=[&](){context->Draw(3,0);context->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};Check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"Read GPU lighting");std::array<float,4> result{};std::memcpy(result.data(),mapped.pData,16);context->Unmap(staging.Get(),0);return result;};
    const auto lit=draw();std::cout<<"Lit smoke="<<lit[0]<<','<<lit[1]<<','<<lit[2]<<'\n';Check(lit[0]>.5f&&lit[2]>.09f,"Smoke receives colored dynamic light and live blue bounce");
    shadowDepth=0;context->UpdateSubresource(shadow.Get(),0,nullptr,&shadowDepth,4,0);const auto blocked=draw();Check(blocked[0]<.001f&&blocked[2]>.09f,"A shadow blocks the flash while retaining indirect lighting");
    params.count=0;context->UpdateSubresource(constants.Get(),0,nullptr,&params,0,0);Check(draw()[0]<.001f,"Expired effects stop illuminating smoke");
    std::cout<<"Environment VFX lighting checks passed\n";
}
