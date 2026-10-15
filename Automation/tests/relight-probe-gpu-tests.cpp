#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Relight/RelightProbeGiShaders.h"
#include "../../AfxHookSource2/Filmmaker/Relight/RelightScreenGiShaders.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentSunVisibilityShaders.h"
#include "../../AfxHookSource2/Filmmaker/Relight/RelightProbeMath.h"
#include "../../AfxHookSource2/Filmmaker/Relight/RelightProbes.h"
#include "../../AfxHookSource2/Filmmaker/Relight/RelightStainedGlass.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentCasterMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShadowMaps.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderCompiler.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShadowMath.h"
#include <fstream>
#include <wrl/client.h>
#include <d3d11.h>
#include <iostream>
#include <string>
#include <vector>
using namespace Filmmaker::Relight;
using Microsoft::WRL::ComPtr;
namespace Filmmaker {bool MvmDebugLog_Active(){return false;}void MvmDebugLog_LinefAlways(const char*,const char*,...){} }
void Check(bool b,const char* message){if(!b){std::cerr<<message<<'\n';std::exit(1);}}
void CheckAmbientLifecycle(ID3D11Device* device,ID3D11DeviceContext* ctx){
    const Vec3 low{-32,-32,-32},high{32,32,32};
    const auto initial=AmbientProbe(device,low,high,{1,0,0},{});
    Check(initial.irradiance&&initial.sampler,"ambient binding exists");
    ComPtr<ID3D11ShaderResourceView> previous=initial.irradiance;
    auto rgb=[&](ID3D11ShaderResourceView* view){
        ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
        ComPtr<ID3D11Texture3D> texture;resource.As(&texture);D3D11_TEXTURE3D_DESC d{};texture->GetDesc(&d);
        d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture3D> read;Check(SUCCEEDED(device->CreateTexture3D(&d,nullptr,&read)),"ambient readback texture");
        ctx->CopyResource(read.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE m{};
        Check(SUCCEEDED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&m)),"ambient readback");
        Vec3 result{};std::memcpy(&result,m.pData,sizeof(result));ctx->Unmap(read.Get(),0);return result;
    };
    auto red=rgb(initial.irradiance);Check(red.x>0&&red.y==0&&red.z==0,"live red sky enters ambient field");
    const auto changed=AmbientProbe(device,low,high,{0,0,1},{});
    auto blue=rgb(changed.irradiance);Check(blue.x==0&&blue.y==0&&blue.z>0,"sky edit replaces ambient radiance immediately");
    const auto skybox=SkyboxAmbientProbe(device,low*16,high*16,{0,1,0},{});
    auto green=rgb(skybox.irradiance);Check(green.x==0&&green.y>0&&green.z==0,"skybox has its own ambient radiance");
    auto retained=rgb(changed.irradiance);
    Check(retained.x==0&&retained.y==0&&retained.z>0&&skybox.constants!=changed.constants,
        "skybox edits and miniature packing cannot overwrite foreground probe inputs");
    auto neutral=rgb(NeutralProbe(device,low,high).irradiance);
    Check(neutral.x==ProbeNeutral.x&&neutral.y==ProbeNeutral.y&&neutral.z==ProbeNeutral.z,"ambient edits preserve diagnostic probe isolation");
    EndProbes();
    const auto fresh=AmbientProbe(device,low,high,{},{});
    Check(fresh.irradiance&&fresh.irradiance!=previous.Get(),"ending probes releases cached live volume before recreation");
    auto black=rgb(fresh.irradiance);Check(black.x==0&&black.y==0&&black.z==0,"recreated ambient field cannot retain prior illumination");
    EndProbes();
}
float Half(uint16_t value){
    const unsigned exponent=(value>>10)&31,mantissa=value&1023;
    return (value&0x8000?-1.f:1.f)*std::ldexp(exponent?1.f+mantissa/1024.f:mantissa/1024.f,exponent?int(exponent)-15:-14);
}
ProbeRow ReadHalf4(ID3D11Device* device,ID3D11DeviceContext* ctx,ID3D11ShaderResourceView* srv,unsigned x,unsigned y,unsigned z){
    ComPtr<ID3D11Resource> resource;srv->GetResource(&resource);ComPtr<ID3D11Texture3D> texture;resource.As(&texture);
    D3D11_TEXTURE3D_DESC d{};texture->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;d.MipLevels=1;
    ComPtr<ID3D11Texture3D> staging;Check(SUCCEEDED(device->CreateTexture3D(&d,nullptr,&staging)),"driver readback texture");
    ctx->CopySubresourceRegion(staging.Get(),0,0,0,0,texture.Get(),0,nullptr);D3D11_MAPPED_SUBRESOURCE m{};
    Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)),"driver readback");
    uint16_t value[4];std::memcpy(value,static_cast<const char*>(m.pData)+z*m.DepthPitch+y*m.RowPitch+x*8,8);ctx->Unmap(staging.Get(),0);
    return {Half(value[0]),Half(value[1]),Half(value[2]),Half(value[3])};
}
void CheckScreenCache(ID3D11Device* device,ID3D11DeviceContext* ctx){
    const unsigned p=GiProbeResolution;
    std::vector<ProbeRow> colors(size_t(p)*p*p*6,ProbeRow{2,4,8,1});
    auto volume=[&](const std::vector<ProbeRow>& data){
        D3D11_TEXTURE3D_DESC d{};d.Width=d.Height=p;d.Depth=p*6;d.MipLevels=1;
        d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{data.data(),p*16,p*p*16};ComPtr<ID3D11Texture3D> t;ComPtr<ID3D11ShaderResourceView> srv;
        Check(SUCCEEDED(device->CreateTexture3D(&d,&initial,&t))&&SUCCEEDED(device->CreateShaderResourceView(t.Get(),nullptr,&srv)),"screen cache input volume");return srv;};
    auto irradiance=volume(colors);for(auto& c:colors)c={8,64,1,0};auto distances=volume(colors);
    std::array<ProbeRow,4> points{{{32,32,32,1},{33,32,32,1},{32,33,32,1},{33,33,32,1}}};
    D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=2;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{points.data(),32,0};ComPtr<ID3D11Texture2D> position;ComPtr<ID3D11ShaderResourceView> positions;
    Check(SUCCEEDED(device->CreateTexture2D(&d,&initial,&position))&&SUCCEEDED(device->CreateShaderResourceView(position.Get(),nullptr,&positions)),"screen positions");
    ProbeBinding binding;binding.irradiance=irradiance.Get();binding.distance=distances.Get();binding.packing=PackProbe({0,0,0},{64,64,64},p);
    const auto matrix=Filmmaker::Environment::Identity();
    auto output=PrepareScreenGi(device,ctx,binding,positions.Get(),2,2,{32,32,64},matrix,1);
    Check(output,"screen cache prepared");auto lit=ReadHalf4(device,ctx,output,0,0,0);
    Check(std::abs(lit[0]-2)<.001f&&std::abs(lit[1]-4)<.001f&&std::abs(lit[2]-8)<.001f&&lit[3]==1,"uniform world irradiance and linear depth survive downsampling");
    for(auto& c:colors)c={8,64,1e-5f,0};auto weakDistances=volume(colors);binding.distance=weakDistances.Get();
    output=PrepareScreenGi(device,ctx,binding,positions.Get(),2,2,{32,32,64},matrix,2);
    const auto weak=ReadHalf4(device,ctx,output,0,0,0);
    Check(weak[0]>0&&weak[0]<1,"Screen cache fades low-confidence cages continuously");
    // An exact probe-row plane with only zero-distance bands in that row still
    // receives uniform light from the visible neighbouring row (Office base slashes).
    for(auto& point:points)point[2]=31.15f;
    ctx->UpdateSubresource(position.Get(),0,nullptr,points.data(),32,0);
    for(unsigned z=0;z<p*6;++z)for(unsigned y=0;y<p;++y)for(unsigned x=0;x<p;++x)
        colors[(size_t(z)*p+y)*p+x]=(z%p==31)?ProbeRow{0,0,1,0}:ProbeRow{8,64,1,0};
    auto deadRow=volume(colors);binding.distance=deadRow.Get();
    output=PrepareScreenGi(device,ctx,binding,positions.Get(),2,2,{32,32,64},matrix,3);
    auto supported=ReadHalf4(device,ctx,output,0,0,0);
    Check(std::abs(supported[0]-2)<.01f,"A dead probe row cannot erase visible neighbouring receiver support");
    // Intersecting dead rows leave just one visible corner. A one-percent
    // per-axis floor still collapses its product below the normalization floor.
    points={{{31.5f,31.5f,31.15f,1},{32.5f,31.5f,31.15f,1},{31.5f,32.5f,31.15f,1},{32.5f,32.5f,31.15f,1}}};
    ctx->UpdateSubresource(position.Get(),0,nullptr,points.data(),32,0);
    for(unsigned z=0;z<p*6;++z)for(unsigned y=0;y<p;++y)for(unsigned x=0;x<p;++x)
        colors[(size_t(z)*p+y)*p+x]=(x==31||y==31||z%p==31)?ProbeRow{0,0,1,1.5f}:ProbeRow{8,64,1,0};
    auto deadIntersection=volume(colors);binding.distance=deadIntersection.Get();
    output=PrepareScreenGi(device,ctx,binding,positions.Get(),2,2,{32,32,64},matrix,4);
    Check(std::abs(ReadHalf4(device,ctx,output,0,0,0)[0]-2)<.01f,
          "Intersecting dead probe rows retain the remaining visible corner");
    for(auto& c:colors)c={0,0,1,1.5f};auto blockedDistances=volume(colors);binding.distance=blockedDistances.Get();
    output=PrepareScreenGi(device,ctx,binding,positions.Get(),2,2,{32,32,64},matrix,5);
    Check(ReadHalf4(device,ctx,output,0,0,0)[0]<.001f,"Neighbour support cannot light a completely blocked screen cage");
    points={{{32,32,32,1},{33,32,32,1},{32,33,32,1},{33,33,32,1}}};
    ctx->UpdateSubresource(position.Get(),0,nullptr,points.data(),32,0);
    binding.distance=distances.Get();
    // Equal-distance probes on opposite sides: visible-side red must dominate
    // back-side blue even though both broad distance fields report visibility.
    for(unsigned z=0;z<p*6;++z)for(unsigned y=0;y<p;++y)for(unsigned x=0;x<p;++x)
        colors[(size_t(z)*p+y)*p+x]=(z%p>=32)?ProbeRow{1,0,0,1}:ProbeRow{0,0,1,1};
    auto sided=volume(colors);binding.irradiance=sided.Get();
    output=PrepareScreenGi(device,ctx,binding,positions.Get(),2,2,{32,32,64},matrix,6);
    auto facing=ReadHalf4(device,ctx,output,0,0,0);
    Check(facing[0]>.72f&&facing[2]<.28f,"surface-facing weight suppresses back-side probe colour");
    binding.irradiance=irradiance.Get();
    points[0][3]=0;ctx->UpdateSubresource(position.Get(),0,nullptr,points.data(),32,0);
    output=PrepareScreenGi(device,ctx,binding,positions.Get(),2,2,{32,32,64},matrix,7);
    auto invalid=ReadHalf4(device,ctx,output,0,0,0);Check(invalid[3]<0,"new frame clears invalid depth instead of retaining light");
    Check(!PrepareScreenGi(device,ctx,binding,nullptr,2,2,{32,32,64},matrix,3),"missing depth has a world-query fallback");
    // Nonuniform light exposes a cage-boundary jump that uniform fixtures hide.
    // Check both old trilinear row planes and the new nearest-probe boundaries.
    binding.distance=distances.Get();
    for(unsigned z=0;z<p*6;++z)for(unsigned y=0;y<p;++y)for(unsigned x=0;x<p;++x)
        colors[(size_t(z)*p+y)*p+x]={float(x%3+1),1,1,1};
    auto varying=volume(colors);binding.irradiance=varying.Get();
    unsigned frame=8;
    for(float plane:{31.5f,32.f}){
        float samples[2]{};
        for(unsigned side=0;side<2;++side){
            const float x=plane+(side?1e-4f:-1e-4f);
            points={{{x,32,32,1},{x+1,32,32,1},{x,33,32,1},{x+1,33,32,1}}};
            ctx->UpdateSubresource(position.Get(),0,nullptr,points.data(),32,0);
            output=PrepareScreenGi(device,ctx,binding,positions.Get(),2,2,{x,32,64},matrix,frame++);
            samples[side]=ReadHalf4(device,ctx,output,0,0,0)[0];
        }
        Check(std::abs(samples[0]-samples[1])<.002f,"Nonuniform screen irradiance is continuous across cage boundaries");
    }
    EndScreenGi();ctx->ClearState();
}
void CheckMath(){
    const auto a=CameraGiRegion({100,200,300}),b=CameraGiRegion({110,195,305});
    Check(a==b,"sub-probe camera motion keeps the region");
    const Vec3 centre=a.low+Vec3{a.size*.5f,a.size*.5f,a.size*.5f};
    Check(std::abs(centre.x-100)<=GiProbeSpacing*.5f&&std::abs(centre.z-300)<=GiProbeSpacing*.5f,"region stays centred on the camera");
    const auto c=CameraGiRegion({100+GiProbeSpacing,200,300-2*GiProbeSpacing});
    const auto moved=GiProbeShift(a,c);
    Check(moved[0]==1&&moved[1]==0&&moved[2]==-2,"region moves in whole probes and reports the history shift");
    Check(GiCell<=20&&GiProbeSpacing<=40,"bounce voxels and probes are room scale");
    // Brick binning: a small local light reaches only nearby bricks; the sun reaches all.
    GiRegion region{{0,0,0},GiRegionSize};
    LightRecord sun{};sun[35]=0;sun[36]=1;sun[37]=0;
    LightRecord lamp{};lamp[32]=80;lamp[33]=80;lamp[34]=80;lamp[35]=4;lamp[36]=1;lamp[37]=-1.f/50;
    LightRecord remote{};remote[32]=-5000;remote[33]=0;remote[34]=0;remote[35]=4;remote[36]=1;remote[37]=-1.f/50;
    const auto bricks=BinGiLights({sun,lamp,remote},region);
    const unsigned count=GiBrickResolution*GiBrickResolution*GiBrickResolution;
    Check(bricks.offsets.size()==count+1,"one list per brick");
    auto list=[&](unsigned brick){return std::vector<uint32_t>(bricks.records.begin()+bricks.offsets[brick],bricks.records.begin()+bricks.offsets[brick+1]);};
    Check(list(0)==std::vector<uint32_t>{0,1},"the lamp's brick lists the sun and the lamp");
    Check(list(count-1)==std::vector<uint32_t>{0},"a distant brick lists only the sun");
    for(unsigned i=0;i<count;++i)for(auto r:list(i))Check(r!=2,"a light outside the region is never binned");
    // Voxel word round trip.
    const auto word=PackGiVoxel({0,0,1},{1,0,.25f},true);
    Check((word&0x80000000u)&&(word&0x40000000u)&&((word>>15)&31)==31&&((word>>20)&31)==0&&((word>>25)&31)==16,"voxel word packs occupancy, porosity and sqrt albedo");
}
void CheckShaders(ID3D11Device* device,ID3D11DeviceContext* ctx){
    Check(std::string(kProbeGiShader).find("#define PROBE_RESOLUTION "+std::to_string(GiProbeResolution)+"\n")!=std::string::npos,
          "shader probe resolution matches GiProbeResolution");
    ComPtr<ID3D11ComputeShader> inject,trace,shift,compose,reflect,reflectNorm,markVisible;
    const char* entries[]{"VoxelVS","VoxelGS","VoxelStaticPS","InjectCS","ProbeCS","ShiftCS","ComposeCS","ReflectionCS","ReflectionNormCS","MarkVisibleCS"};
    const char* profiles[]{"vs_5_0","gs_5_0","ps_5_0","cs_5_0","cs_5_0","cs_5_0","cs_5_0","cs_5_0","cs_5_0","cs_5_0"};
    ComPtr<ID3D11ComputeShader>* targets[]{nullptr,nullptr,nullptr,&inject,&trace,&shift,&compose,&reflect,&reflectNorm,&markVisible};
    for(unsigned i=0;i<std::size(entries);++i){ComPtr<ID3DBlob> code,error;
        auto hr=Filmmaker::Environment::CompileEnvironmentShader(kProbeGiShader,strlen(kProbeGiShader),"RelightProbeGi",nullptr,entries[i],profiles[i],D3DCOMPILE_ENABLE_STRICTNESS,&code,&error);
        if(error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());Check(SUCCEEDED(hr),"GI shader compilation");
        if(targets[i])Check(SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,targets[i]->GetAddressOf())),"compute shader");
    }
    auto buffer=[&](UINT bytes,UINT stride,UINT bind,const void* p){
        D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;d.BindFlags=bind;d.StructureByteStride=stride;d.MiscFlags=stride?D3D11_RESOURCE_MISC_BUFFER_STRUCTURED:0;
        D3D11_SUBRESOURCE_DATA data{};data.pSysMem=p;ComPtr<ID3D11Buffer> out;
        Check(SUCCEEDED(device->CreateBuffer(&d,p?&data:nullptr,&out)),"buffer");return out;};
    auto srvOf=[&](ID3D11Resource* r){ComPtr<ID3D11ShaderResourceView> v;Check(SUCCEEDED(device->CreateShaderResourceView(r,nullptr,&v)),"srv");return v;};
    struct Volume{ComPtr<ID3D11Texture3D> texture;ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;};
    auto texture=[&](UINT width,UINT depth,DXGI_FORMAT format,const void* initial,UINT pixel){
        D3D11_TEXTURE3D_DESC d{};d.Width=d.Height=width;d.Depth=depth;d.MipLevels=1;d.Format=format;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        D3D11_SUBRESOURCE_DATA data{initial,width*pixel,width*width*pixel};Volume out;
        Check(SUCCEEDED(device->CreateTexture3D(&d,initial?&data:nullptr,&out.texture))&&
              SUCCEEDED(device->CreateShaderResourceView(out.texture.Get(),nullptr,&out.srv))&&
              SUCCEEDED(device->CreateUnorderedAccessView(out.texture.Get(),nullptr,&out.uav)),"volume");return out;};
    auto read=[&](const Volume& volume,unsigned x,unsigned y,unsigned z){
        D3D11_TEXTURE3D_DESC d{};volume.texture->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.BindFlags=0;
        ComPtr<ID3D11Texture3D> staging;Check(SUCCEEDED(device->CreateTexture3D(&d,nullptr,&staging)),"staging");
        ctx->CopyResource(staging.Get(),volume.texture.Get());D3D11_MAPPED_SUBRESOURCE m{};Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)),"map");
        ProbeRow out{};std::memcpy(out.data(),static_cast<char*>(m.pData)+z*m.DepthPitch+y*m.RowPitch+x*16,16);ctx->Unmap(staging.Get(),0);return out;};
    // Constants: region at origin, 8 voxels of 1 unit, static grid identical and aligned.
    ProbeRow values[14]{{0,0,0,8},{8,1,1,0},{0,0,0,8},{1,1,8,1},{0,0,0,0},{0,0,0,0},{0,0,0,0},{8,8,8,0},{1,.5f,1,.3f},{0,0,0,0},{0,0,0,8},{8,8,8,0}};
    auto constants=buffer(sizeof(values),0,D3D11_BIND_CONSTANT_BUFFER,values);auto cb=constants.Get();
    auto upload=[&](){ctx->UpdateSubresource(constants.Get(),0,nullptr,values,0,0);ctx->CSSetConstantBuffers(0,1,&cb);};
    // Native direct and injected bounce must assign identical energy to overlapping
    // cube wedges; otherwise walking between atlas tiers changes bounced brightness.
    const std::string faceShader=std::string(kProbeGiShader)+
        "\n[numthreads(1,1,1)] void FaceEnergyTestCS(){radianceOutput[uint3(0,0,0)]=float4(CubeFaceEnergy(float3(0,0,.5)),CubeFaceEnergy(float3(.9,0,.5)),CubeFaceEnergy(float3(.9,.9,.5)),1);}";
    ComPtr<ID3DBlob> faceCode,faceError;ComPtr<ID3D11ComputeShader> faceTest;
    Check(SUCCEEDED(Filmmaker::Environment::CompileEnvironmentShader(faceShader.c_str(),faceShader.size(),"FaceEnergyTest",nullptr,"FaceEnergyTestCS","cs_5_0",0,&faceCode,&faceError))&&
          SUCCEEDED(device->CreateComputeShader(faceCode->GetBufferPointer(),faceCode->GetBufferSize(),nullptr,&faceTest)),"Cube bounce-energy shader");
    auto faceOutput=texture(8,8,DXGI_FORMAT_R32G32B32A32_FLOAT,nullptr,16);auto faceUav=faceOutput.uav.Get();
    ctx->CSSetUnorderedAccessViews(1,1,&faceUav,nullptr);ctx->CSSetShader(faceTest.Get(),nullptr,0);ctx->Dispatch(1,1,1);ctx->ClearState();
    const auto faceWeights=read(faceOutput,0,0,0);
    Check(std::abs(faceWeights[0]-1)<.001f&&std::abs(faceWeights[1]*2-1)<.001f&&std::abs(faceWeights[2]*3-1)<.001f,
          "GI conserves source energy at cube centres, shared edges and three-face corners");
    // Static red plane and green surface. A stale actor SRV at t6 must be ignored.
    std::vector<uint32_t> cells(8*8*8,0),dynamicCells(8*8*8,0);
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)cells[(4*8+y)*8+x]=PackGiVoxel({0,0,1},{1,0,0},false);
    cells[(6*8+6)*8+6]=PackGiVoxel({0,0,1},{0,1,0},false);
    dynamicCells[(5*8+5)*8+5]=PackGiVoxel({0,0,1},{0,1,0},false);
    auto voxels=texture(8,8,DXGI_FORMAT_R32_UINT,cells.data(),4),dynamic=texture(8,8,DXGI_FORMAT_R32_UINT,dynamicCells.data(),4);
    auto radiance=texture(8,8,DXGI_FORMAT_R32G32B32A32_FLOAT,nullptr,16),direction=texture(8,8,DXGI_FORMAT_R32G32B32A32_FLOAT,nullptr,16);
    auto backRadiance=texture(8,8,DXGI_FORMAT_R32G32B32A32_FLOAT,nullptr,16);
    const auto sun=SunRecord({0,0,1},{2,3,4},nullptr,false);
    auto records=buffer(sizeof(sun),sizeof(sun),D3D11_BIND_SHADER_RESOURCE,sun.data());auto lightSrv=srvOf(records.Get());
    std::vector<uint32_t> offsets(GiBrickResolution*GiBrickResolution*GiBrickResolution+1,1);offsets[0]=0;const uint32_t list[1]{0};
    auto brickOffsets=buffer(UINT(offsets.size()*4),4,D3D11_BIND_SHADER_RESOURCE,offsets.data());auto brickRecords=buffer(4,4,D3D11_BIND_SHADER_RESOURCE,list);
    auto offsetsSrv=srvOf(brickOffsets.Get()),recordsSrv=srvOf(brickRecords.Get());
    const unsigned P=GiProbeResolution;
    std::vector<ProbeRow> bounce(size_t(P)*P*P*6,ProbeRow{0,0,0,0});
    auto previous=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,bounce.data(),16);
    std::vector<ProbeRow> feedbackMoments(bounce.size(),ProbeRow{8,64,1,0});
    auto feedbackDistance=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,feedbackMoments.data(),16);
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    ComPtr<ID3D11SamplerState> sampler;Check(SUCCEEDED(device->CreateSamplerState(&sd,&sampler)),"sampler");
    auto injectRun=[&](ID3D11ShaderResourceView* history){upload();auto s=sampler.Get();ctx->CSSetSamplers(0,1,&s);
        ID3D11ShaderResourceView* inputs[10]{voxels.srv.Get(),lightSrv.Get(),nullptr,nullptr,nullptr,nullptr,dynamic.srv.Get(),offsetsSrv.Get(),recordsSrv.Get(),history};
        ctx->CSSetShaderResources(0,10,inputs);
        auto distances=feedbackDistance.srv.Get();ctx->CSSetShaderResources(15,1,&distances);
        ID3D11UnorderedAccessView* outputs[]{radiance.uav.Get(),direction.uav.Get()};ctx->CSSetUnorderedAccessViews(1,2,outputs,nullptr);
        auto backOutput=backRadiance.uav.Get();ctx->CSSetUnorderedAccessViews(6,1,&backOutput,nullptr);
        ctx->CSSetShader(inject.Get(),nullptr,0);ctx->Dispatch(1,1,8);ctx->ClearState();};
    injectRun(previous.srv.Get());
    auto red=read(radiance,3,3,4),player=read(radiance,5,5,5),empty=read(radiance,3,3,3);
    Check(std::abs(red[0]-2)<.01f&&red[1]==0&&red[2]==0&&red[3]==1,"injection reflects the sun by the surface's material albedo");
    Check(read(backRadiance,3,3,4)==ProbeRow{0,0,0,1},"A sunlit wall does not copy its front-face radiance into the shaded back face");
    Check(player==ProbeRow{},"Animated actor occupancy cannot change indirect radiance");
    const auto green=read(radiance,6,6,6);
    Check(green[0]==0&&std::abs(green[1]-3)<.01f&&green[2]==0,"Static green surfaces retain material albedo");
    cells[(6*8+6)*8+6]=0;
    voxels=texture(8,8,DXGI_FORMAT_R32_UINT,cells.data(),4);
    Check(empty==ProbeRow{},"empty voxels carry no radiance or opacity");
    const auto interiorSun=SunRecord({0,0,-1},{4,3,2},nullptr,false);
    ctx->UpdateSubresource(records.Get(),0,nullptr,interiorSun.data(),0,0);injectRun(previous.srv.Get());
    Check(read(radiance,3,3,4)[0]==0&&std::abs(read(backRadiance,3,3,4)[0]-4)<.01f,
          "Light on the opposite side of a surface produces its own outgoing bounce");
    ctx->UpdateSubresource(records.Get(),0,nullptr,sun.data(),0,0);
    // Fed-back bounce: a surface also reflects the previous probe solution (multiple bounces).
    for(auto& v:bounce)v={.5f,.5f,.5f,0};
    auto lit=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,bounce.data(),16);
    injectRun(lit.srv.Get());
    const auto again=read(radiance,3,3,4);
    Check(std::abs(again[0]-2.5f)<.02f&&again[1]==0,"previous bounce relights surfaces in turn, still through their albedo");
    values[3][3]=4;injectRun(lit.srv.Get());const auto boosted=read(radiance,3,3,4);
    Check(std::abs(boosted[0]-again[0])<.001f,"Display bounce boost never compounds in recursive transport");
    values[3][3]=0;injectRun(lit.srv.Get());
    Check(std::abs(read(radiance,3,3,4)[0]-again[0])<.001f,"Zero display bounce preserves the internal transport solution");
    values[3][3]=1;
    // A small entry point calls the production gather at a symmetric eight-probe cage.
    const std::string feedbackShader=std::string(kProbeGiShader)+
        "\n[numthreads(1,1,1)] void FeedbackTestCS(){radianceOutput[uint3(0,0,0)]=float4(ProbeIrradiance(float3(4,4,4),float3(0,0,1)),1);}";
    ComPtr<ID3DBlob> feedbackCode,feedbackError;ComPtr<ID3D11ComputeShader> feedbackTest;
    const auto feedbackCompile=Filmmaker::Environment::CompileEnvironmentShader(feedbackShader.c_str(),feedbackShader.size(),"FeedbackTest",nullptr,"FeedbackTestCS","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&feedbackCode,&feedbackError);
    if(feedbackError)std::cerr<<static_cast<const char*>(feedbackError->GetBufferPointer());
    Check(SUCCEEDED(feedbackCompile)&&SUCCEEDED(device->CreateComputeShader(feedbackCode->GetBufferPointer(),feedbackCode->GetBufferSize(),nullptr,&feedbackTest)),"Production recursive gather test shader");
    auto feedbackRun=[&](){upload();auto history=lit.srv.Get(),distances=feedbackDistance.srv.Get();
        ctx->CSSetShaderResources(9,1,&history);ctx->CSSetShaderResources(15,1,&distances);
        auto output=radiance.uav.Get();ctx->CSSetUnorderedAccessViews(1,1,&output,nullptr);
        ctx->CSSetShader(feedbackTest.Get(),nullptr,0);ctx->Dispatch(1,1,1);ctx->ClearState();return read(radiance,0,0,0);};
    // Bright history across a sealed wall must not enter the recursive solution.
    // The direct red sunlight remains intact when every history probe is blocked.
    for(auto& v:feedbackMoments)v={.01f,.0001f,1,0};
    feedbackDistance=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,feedbackMoments.data(),16);
    injectRun(lit.srv.Get());
    Check(std::abs(read(radiance,3,3,4)[0]-red[0])<.001f,"Blocked history cannot feed colour through a wall on subsequent bounces");
    for(auto& v:feedbackMoments)v={8,64,0,0};
    feedbackDistance=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,feedbackMoments.data(),16);
    injectRun(lit.srv.Get());
    Check(std::abs(read(radiance,3,3,4)[0]-red[0])<.001f,"Uninitialized visibility cannot admit history while probes converge");
    // All eight cage corners have the same distance. Relocation toward the receiver
    // makes the close visibility field valid; ignoring that offset rejects it.
    const float relocatedDistance=std::sqrt(.5f);
    for(unsigned z=0;z<P*6;++z)for(unsigned y=0;y<P;++y)for(unsigned x=0;x<P;++x){
        const float code=x<P/2?1.5f:-1.5f;
        feedbackMoments[(size_t(z)*P+y)*P+x]={relocatedDistance,relocatedDistance*relocatedDistance,1,code};
    }
    feedbackDistance=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,feedbackMoments.data(),16);
    Check(std::abs(feedbackRun()[0]-.5f)<.002f,"Recursive visibility measures from the shared relocated probe origin");
    for(auto& v:feedbackMoments)v={relocatedDistance,relocatedDistance*relocatedDistance,1,0};
    feedbackDistance=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,feedbackMoments.data(),16);
    Check(feedbackRun()[0]<.002f,"The same unrelocated probe behind its blocker is rejected");
    for(auto& v:feedbackMoments)v={8,64,1,0};
    feedbackDistance=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,feedbackMoments.data(),16);
    // Porous voxels pass part of the light through a cone.
    cells[(4*8+3)*8+3]=PackGiVoxel({0,0,1},{1,0,0},true);
    voxels=texture(8,8,DXGI_FORMAT_R32_UINT,cells.data(),4);injectRun(previous.srv.Get());
    Check(read(radiance,3,3,4)[3]<.5f,"foliage voxels are partially transparent to traced light");
    // Probe history: traced slices blend into the previous value; others carry over.
    std::vector<ProbeRow> old(size_t(P)*P*P*6,ProbeRow{1,1,1,1});
    auto historyIn=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,old.data(),16),historyOut=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,old.data(),16);
    std::vector<ProbeRow> momentData(old.size(),ProbeRow{8,64,1,0});
    auto distanceIn=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,momentData.data(),16),distanceOut=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,nullptr,16);
    const float glowing[4]{0,0,0,0},face[4]{0,0,1,1};
    ctx->ClearUnorderedAccessViewFloat(radiance.uav.Get(),glowing);ctx->ClearUnorderedAccessViewFloat(direction.uav.Get(),face);
    values[0]={0,0,0,128};values[1]={128,1,1,float(P/2)};values[2]=values[0];values[3]={1,1,128,1};values[8]={1,.25f,1,1};
    // The traced volume must match its declared dimensions. Point traversal does
    // not clamp out-of-range loads to a glowing edge, unlike filtered sampling.
    radiance=texture(128,128,DXGI_FORMAT_R32G32B32A32_FLOAT,nullptr,16);
    direction=texture(128,128,DXGI_FORMAT_R32G32B32A32_FLOAT,nullptr,16);
    ctx->ClearUnorderedAccessViewFloat(radiance.uav.Get(),glowing);
    ctx->ClearUnorderedAccessViewFloat(direction.uav.Get(),face);
    std::vector<uint32_t> none(8*8*8,0);auto noVoxels=texture(8,8,DXGI_FORMAT_R32_UINT,none.data(),4);
    auto mask=texture(P,P,DXGI_FORMAT_R32_UINT,nullptr,4);
    D3D11_BUFFER_DESC ld{};ld.ByteWidth=16+P*P*P*4;ld.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
    ld.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;ComPtr<ID3D11Buffer> visibleList,dispatchArgs;
    Check(SUCCEEDED(device->CreateBuffer(&ld,nullptr,&visibleList)),"visible list");
    D3D11_SHADER_RESOURCE_VIEW_DESC ls{};ls.Format=DXGI_FORMAT_R32_TYPELESS;ls.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;
    ls.BufferEx.NumElements=ld.ByteWidth/4;ls.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;ComPtr<ID3D11ShaderResourceView> listSrv;
    Check(SUCCEEDED(device->CreateShaderResourceView(visibleList.Get(),&ls,&listSrv)),"visible list srv");
    D3D11_UNORDERED_ACCESS_VIEW_DESC lu{};lu.Format=ls.Format;lu.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
    lu.Buffer.NumElements=ld.ByteWidth/4;lu.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;ComPtr<ID3D11UnorderedAccessView> listUav;
    Check(SUCCEEDED(device->CreateUnorderedAccessView(visibleList.Get(),&lu,&listUav)),"visible list uav");
    ld={};ld.ByteWidth=12;ld.MiscFlags=D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    Check(SUCCEEDED(device->CreateBuffer(&ld,nullptr,&dispatchArgs)),"visible dispatch args");
    auto traceRun=[&](){upload();ID3D11ShaderResourceView* srvs[13]{noVoxels.srv.Get(),nullptr,nullptr,radiance.srv.Get(),direction.srv.Get(),nullptr,noVoxels.srv.Get(),nullptr,nullptr,nullptr,nullptr,nullptr,historyIn.srv.Get()};
        ctx->CSSetShaderResources(0,13,srvs);auto s=sampler.Get();ctx->CSSetSamplers(0,1,&s);auto out=historyOut.uav.Get();ctx->CSSetUnorderedAccessViews(4,1,&out,nullptr);
        auto moments=distanceIn.srv.Get();ctx->CSSetShaderResources(15,1,&moments);auto distances=distanceOut.uav.Get();ctx->CSSetUnorderedAccessViews(3,1,&distances,nullptr);
        auto selected=mask.srv.Get(),compact=listSrv.Get();ctx->CSSetShaderResources(17,1,&selected);ctx->CSSetShaderResources(18,1,&compact);
        ctx->CSSetShader(trace.Get(),nullptr,0);
        if(values[5][1]==4)ctx->DispatchIndirect(dispatchArgs.Get(),0);
        else if(values[5][1]==1)ctx->Dispatch(2,2,16*6);
        else ctx->Dispatch(P/8,P/8,(values[5][1]>=2?P:unsigned(values[8][0]))*6);ctx->ClearState();};
    traceRun();
    const auto blended=read(historyOut,P/2,P/2,P/2+2*P);
    // Authored edits replace the previous sample immediately; stable lighting retains history.
    Check(std::abs(blended[0])<.01f&&blended[3]<1,"a traced probe immediately replaces obsolete authored lighting");
    Check(read(historyOut,P/2,P/2,P/2+1)==ProbeRow{1,1,1,1},"untraced slices keep their history");
    const auto tracedDistance=read(distanceOut,P/2,P/2,P/2+2*P);
    Check(std::abs(tracedDistance[0]-8)<.001f&&std::abs(tracedDistance[1]-64)<.001f&&tracedDistance[2]==1,"open probes publish directional distance moments for visibility-weighted gathering");
    const auto stableSky=read(historyOut,P/2,P/2,P/2+2*P);
    // Analytic cosine integral of the linear sky (.5 + .5*direction.z):
    // +Z receives 5/6, -Z receives 1/6. The old eight-ray quadrature
    // misses this tolerance even in an empty scene, before wall discontinuities.
    Check(std::abs(stableSky[3]-5.f/6)<.001f,
          "Probe sky integration resolves the unoccluded cosine hemisphere accurately");
    // A diagonal scroll changes texture coordinates, not the update phase or rays
    // of a retained world probe. This failed with the old camera-relative schedule.
    values[0][0]=values[0][2]=2;traceRun();
    Check(read(historyOut,P/2-1,P/2,P/2-1+2*P)==stableSky,
          "A world probe keeps its update phase and quadrature through diagonal scrolling");
    values[0][0]=values[0][2]=0;
    // Newly exposed cells cannot wait for the background sweep to turn valid.
    const size_t exposed=(size_t(9)*P+8)*P+7;
    for(unsigned band=0;band<6;++band)old[exposed+size_t(band)*P*P*P]={0,0,0,-1};
    historyIn=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,old.data(),16);values[5][1]=2;traceRun();
    values[5][1]=0;
    Check(read(historyOut,7,8,9)[3]>=0&&read(distanceOut,7,8,9)[2]==1,
          "Newly exposed probes publish light and visibility in the same frame");
    // The neighbourhood refresh is independent of the far-field phase.
    values[12]={65,65,67,128};values[5][1]=1;traceRun();
    Check(read(historyOut,P/2,P/2,P/2+1)[0]==0,
          "Nearby light edits appear on the first frame even outside the scheduled phase");
    values[12]={};values[5][1]=0;
    // Current visible cages refresh off-schedule, including composition neighbours.
    // Invalid and out-of-region positions must not stamp clamped border probes.
    std::array<ProbeRow,7> receivers{{{65,65,67,1},{-100,20,20,1},{80,80,80,0},{400,20,20,1},{85,85,87,1},{101,101,103,1},{65,65,67,1}}};
    D3D11_TEXTURE2D_DESC pd{};pd.Width=7;pd.Height=1;pd.MipLevels=pd.ArraySize=pd.SampleDesc.Count=1;
    pd.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;pd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA pointData{receivers.data(),112,0};ComPtr<ID3D11Texture2D> points;
    Check(SUCCEEDED(device->CreateTexture2D(&pd,&pointData,&points)),"priority positions texture");auto positionSrv=srvOf(points.Get());
    values[13]={7,1,1,0};upload();const UINT clear[4]{};ctx->ClearUnorderedAccessViewUint(mask.uav.Get(),clear);
    ctx->ClearUnorderedAccessViewUint(listUav.Get(),clear);
    const UINT header[4]{0,6,1,0};const D3D11_BOX hb{0,0,0,16,1,1};ctx->UpdateSubresource(visibleList.Get(),0,&hb,header,0,0);
    auto listOutput=listUav.Get();ctx->CSSetUnorderedAccessViews(7,1,&listOutput,nullptr);
    auto maskUav=mask.uav.Get();auto position=positionSrv.Get();ctx->CSSetUnorderedAccessViews(0,1,&maskUav,nullptr);
    ctx->CSSetShaderResources(16,1,&position);ctx->CSSetShader(markVisible.Get(),nullptr,0);ctx->Dispatch(1,1,1);ctx->ClearState();
    // Three disjoint receivers lie on cage boundaries. Each needs 27 corners
    // covering all normal biases and 54 face neighbours for composition.
    // A repeated receiver must not allocate duplicate trace work. More than 64
    // probes exercises the second compact dispatch group and tail-lane guard.
    D3D11_BUFFER_DESC hd{};hd.ByteWidth=16;hd.Usage=D3D11_USAGE_STAGING;hd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> headerRead;Check(SUCCEEDED(device->CreateBuffer(&hd,nullptr,&headerRead)),"list header staging");
    ctx->CopySubresourceRegion(headerRead.Get(),0,0,0,0,visibleList.Get(),0,&hb);D3D11_MAPPED_SUBRESOURCE headerMap{};
    Check(SUCCEEDED(ctx->Map(headerRead.Get(),0,D3D11_MAP_READ,0,&headerMap)),"visible list count readback");
    UINT headerValue[4]{};std::memcpy(headerValue,headerMap.pData,16);ctx->Unmap(headerRead.Get(),0);
    Check(headerValue[0]==4&&headerValue[1]==6&&headerValue[2]==1&&headerValue[3]==243,"GPU compaction deduplicates biased receiver cages and dispatches every hemisphere exactly");
    ctx->CopyResource(historyOut.texture.Get(),historyIn.texture.Get());
    const D3D11_BOX argsBox{0,0,0,12,1,1};ctx->CopySubresourceRegion(dispatchArgs.Get(),0,0,0,0,visibleList.Get(),0,&argsBox);
    values[5][1]=4;values[8][3]=0;traceRun();
    Check(read(historyOut,32,32,33)[0]==0,"A visible distant receiver refreshes in the current frame outside the background phase");
    Check(read(historyOut,32,32,34)[0]==0,"Visible composition neighbours are refreshed too");
    Check(read(historyOut,31,31,33)[0]==0&&read(historyOut,30,31,33)[0]==0,
          "Normal-biased diagonal cages and their composition neighbours refresh immediately");
    Check(read(historyOut,42,42,43)[0]==0&&read(historyOut,50,50,51)[0]==0,"Every compact group refreshes its selected probes");
    Check(read(historyOut,0,10,10)==ProbeRow{1,1,1,1},"Outside volume positions cannot contaminate border probes");
    Check(read(historyOut,40,40,40)==ProbeRow{1,1,1,1},"Missing current depth leaves unrelated history untouched");
    for(unsigned band=0;band<6;++band){
        const auto visible=read(historyOut,32,32,33+band*P),moments=read(distanceOut,32,32,33+band*P);
        Check(visible[0]==0&&visible[1]==0&&visible[2]==0&&visible[3]>=0&&visible[3]<=1&&moments[2]==1,
              "Compact visible dispatch refreshes all six independent hemispheres and their visibility");
    }
    // Retained visible probes are excluded from background duplication, while
    // unselected background probes still advance. Scrolling disables this flag.
    ctx->CopyResource(historyOut.texture.Get(),historyIn.texture.Get());
    values[5][1]=0;values[5][2]=1;values[8][0]=P;traceRun();
    Check(read(historyOut,32,32,33)==ProbeRow{1,1,1,1},"Background work preserves probes reserved for current visible refresh");
    Check(std::abs(read(historyOut,2,2,2)[0]-.75f)<.001f,"Off-screen background transport continues independently");
    values[5][2]=0;values[8][0]=1;values[8][3]=1;
    values[5][0]=123;traceRun();
    Check(read(historyOut,P/2,P/2,P/2+2*P)==stableSky,"unchanged probe geometry and light do not animate ray quadrature");
    values[8][3]=0;traceRun();
    Check(std::abs(read(historyOut,P/2,P/2,P/2+2*P)[0]-.75f)<.01f,
          "A different ray sample uses the slow history rate without an authored light edit");
    values[8][3]=1;
    // An equal-luminance authored hue swap still takes the fast history response.
    const float blue[4]{0,0,1,1};ctx->ClearUnorderedAccessViewFloat(radiance.uav.Get(),blue);
    for(auto& v:old)v={.0721f/.2125f,0,0,0};
    historyIn=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,old.data(),16);traceRun();
    const auto hue=read(historyOut,P/2,P/2,P/2+5*P);
    Check(hue[0]<old[0][0]*.6f&&hue[2]>.5f,"A colour edit cannot hide behind unchanged luminance");
    // An opaque wall must not expand into its empty neighbouring cell through
    // trilinear opacity. Trace along the wall and then directly into it.
    const std::string rayShader=std::string(kProbeGiShader)+
        "\n[numthreads(2,1,1)] void WallRayTestCS(uint3 id:SV_DispatchThreadID){float d,backface;float4 v=ProbeRay(float3(65.1,64,64),id.x==0?float3(0,1,0):float3(-1,0,0),d,backface);probeOutput[id]=float4(v.a,d,0,0);}";
    ComPtr<ID3DBlob> rayCode,rayError;ComPtr<ID3D11ComputeShader> rayTest;
    Check(SUCCEEDED(Filmmaker::Environment::CompileEnvironmentShader(rayShader.c_str(),rayShader.size(),"WallRayTest",nullptr,"WallRayTestCS","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&rayCode,&rayError))&&
          SUCCEEDED(device->CreateComputeShader(rayCode->GetBufferPointer(),rayCode->GetBufferSize(),nullptr,&rayTest)),"wall ray regression shader");
    std::vector<ProbeRow> wallRadiance(size_t(128)*128*128);
    for(unsigned z=0;z<128;++z)for(unsigned y=0;y<128;++y)wallRadiance[(size_t(z)*128+y)*128+64]={0,0,0,1};
    auto wallSamples=texture(128,128,DXGI_FORMAT_R32G32B32A32_FLOAT,wallRadiance.data(),16);
    upload();auto wallSrv=wallSamples.srv.Get(),normalSrv=direction.srv.Get();auto raySampler=sampler.Get();
    ctx->CSSetShaderResources(3,1,&wallSrv);ctx->CSSetShaderResources(4,1,&normalSrv);ctx->CSSetSamplers(0,1,&raySampler);
    auto rayOutput=historyOut.uav.Get();ctx->CSSetUnorderedAccessViews(4,1,&rayOutput,nullptr);
    ctx->CSSetShader(rayTest.Get(),nullptr,0);ctx->Dispatch(1,1,1);ctx->ClearState();
    Check(read(historyOut,0,0,0)[0]==0,"a ray parallel to a wall stays unoccluded in the adjacent empty cell");
    const auto blockedRay=read(historyOut,1,0,0);
    Check(blockedRay[0]==1&&blockedRay[1]>.04f&&blockedRay[1]<.06f,"a ray entering the wall stops at its actual cell boundary");
    // Looking up at the back of a sealed ceiling must retain its blocking weight
    // in the sky integral, while reading the radiance from the interior face.
    const std::string ceilingShader=std::string(kProbeGiShader)+
        "\n[numthreads(1,1,1)] void CeilingTestCS(){float2 m;probeOutput[uint3(0,0,0)]=Hemisphere(float3(64,64,63.5),float3(0,0,1),0,m);}";
    ComPtr<ID3DBlob> ceilingCode,ceilingError;ComPtr<ID3D11ComputeShader> ceilingTest;
    Check(SUCCEEDED(Filmmaker::Environment::CompileEnvironmentShader(ceilingShader.c_str(),ceilingShader.size(),"CeilingTest",nullptr,"CeilingTestCS","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&ceilingCode,&ceilingError))&&
          SUCCEEDED(device->CreateComputeShader(ceilingCode->GetBufferPointer(),ceilingCode->GetBufferSize(),nullptr,&ceilingTest)),"ceiling regression shader");
    std::fill(wallRadiance.begin(),wallRadiance.end(),ProbeRow{});
    for(unsigned y=0;y<128;++y)for(unsigned x=0;x<128;++x)wallRadiance[(size_t(64)*128+y)*128+x]={8,0,0,1};
    auto ceilingSamples=texture(128,128,DXGI_FORMAT_R32G32B32A32_FLOAT,wallRadiance.data(),16);
    for(auto& v:wallRadiance)if(v[3]>0)v={0,2,0,1};
    auto interiorSamples=texture(128,128,DXGI_FORMAT_R32G32B32A32_FLOAT,wallRadiance.data(),16);
    std::vector<uint32_t> ceilingCells(size_t(128)*128*128);
    for(unsigned y=0;y<128;++y)for(unsigned x=0;x<128;++x)ceilingCells[(size_t(64)*128+y)*128+x]=PackGiVoxel({0,0,1},{1,1,1},false);
    auto ceilingVoxels=texture(128,128,DXGI_FORMAT_R32_UINT,ceilingCells.data(),4);
    upload();auto ceilingSrv=ceilingSamples.srv.Get(),interiorSrv=interiorSamples.srv.Get(),ceilingOccupancy=ceilingVoxels.srv.Get();
    ctx->CSSetShaderResources(0,1,&ceilingOccupancy);ctx->CSSetShaderResources(3,1,&ceilingSrv);ctx->CSSetShaderResources(4,1,&normalSrv);ctx->CSSetShaderResources(19,1,&interiorSrv);
    ctx->CSSetUnorderedAccessViews(4,1,&rayOutput,nullptr);ctx->CSSetShader(ceilingTest.Get(),nullptr,0);ctx->Dispatch(1,1,1);ctx->ClearState();
    const auto ceiling=read(historyOut,0,0,0);
    Check(ceiling[0]==0&&ceiling[1]>1.7f&&ceiling[2]==0&&ceiling[3]==0,
          "A back-facing opaque ceiling reflects interior light and admits no blue skylight");
    // Boundary probes relocate symmetrically, and publish the origin used by receiver visibility.
    const unsigned V=128,c=P+1;
    values[7]={float(V),float(V),float(V),0};
    std::vector<uint32_t> occupied(size_t(V)*V*V,0);
    occupied[(size_t(c)*V+c)*V+c]=PackGiVoxel({0,0,1},{1,1,1},false);
    noVoxels=texture(V,V,DXGI_FORMAT_R32_UINT,occupied.data(),4);traceRun();
    for(unsigned band=0;band<6;++band){
        const auto relocated=read(distanceOut,P/2,P/2,P/2+band*P);
        Check(relocated[2]==1&&std::abs(relocated[3]-3.5f)<.001f,
              "Every visibility and colour band shares the same open origin near a boundary");
        const auto fresh=read(historyOut,P/2,P/2,P/2+band*P);
        Check(fresh[0]<.001f&&std::isfinite(fresh[2])&&fresh[2]>=0&&fresh[3]>=0,
              "Moving a probe discards radiance from its former trace origin");
    }
    occupied[(size_t(c+1)*V+c)*V+c]=PackGiVoxel({0,0,1},{1,1,1},false);
    noVoxels=texture(V,V,DXGI_FORMAT_R32_UINT,occupied.data(),4);traceRun();
    const auto alternate=read(distanceOut,P/2,P/2,P/2);
    Check(alternate[2]==1&&alternate[3]!=3.5f,"Blocking the preferred origin selects another open shared position");
    for(unsigned band=1;band<6;++band)Check(read(distanceOut,P/2,P/2,P/2+band*P)[3]==alternate[3],
          "Relocation changes all bands together rather than creating partial probe holes");
    for(int axis=0;axis<3;++axis)for(int sign:{-1,1}){int q[3]{int(c),int(c),int(c)};q[axis]+=sign;
        occupied[(size_t(q[2])*V+q[1])*V+q[0]]=PackGiVoxel({0,0,1},{1,1,1},false);}
    noVoxels=texture(V,V,DXGI_FORMAT_R32_UINT,occupied.data(),4);traceRun();
    for(unsigned band=0;band<6;++band)Check(read(historyOut,P/2,P/2,P/2+band*P)[3]<0&&read(distanceOut,P/2,P/2,P/2+band*P)[2]==0,
          "A completely enclosed probe rejects every band instead of tracing through geometry");
    noVoxels=texture(8,8,DXGI_FORMAT_R32_UINT,none.data(),4);
    values[7]={8,8,8,0};
    // Shift: history follows the region; exposed probes stay unknown until traced.
    std::vector<ProbeRow> ramp(size_t(P)*P*P*6);
    for(unsigned z=0;z<P*6;++z)for(unsigned y=0;y<P;++y)for(unsigned x=0;x<P;++x)ramp[(size_t(z)*P+y)*P+x]={float(x),float(y),float(z%P),float(z/P)};
    auto rampIn=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,ramp.data(),16),rampOut=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,nullptr,16);
    values[9]={1,0,-1,0};upload();auto rin=rampIn.srv.Get();ctx->CSSetShaderResources(11,1,&rin);auto rout=rampOut.uav.Get();ctx->CSSetUnorderedAccessViews(4,1,&rout,nullptr);
    ctx->CSSetShader(shift.Get(),nullptr,0);ctx->Dispatch(P/8,P/8,P*6);ctx->ClearState();
    const auto shifted=read(rampOut,10,20,30+3*P),edge=read(rampOut,P-1,0,0);
    Check(shifted[0]==11&&shifted[1]==20&&shifted[2]==29&&shifted[3]==3,"history moves by whole probes within each band");
    Check(edge[3]<0&&edge[0]==0,"newly exposed probes cannot inherit illumination from a different room");
    // Composition: probes inside geometry take their open neighbours; sky and gain are live.
    std::vector<ProbeRow> marked(size_t(P)*P*P*6,ProbeRow{.2f,.1f,.05f,.5f});
    marked[((size_t(P/2)+P)*P+P/2)*P+P/2]={9,9,9,-1};
    auto transport=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,marked.data(),16),composed=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,nullptr,16);
    values[3][3]=2;values[4]={0,0,1,0};upload();auto input=transport.srv.Get();ctx->CSSetShaderResources(5,1,&input);
    auto output=composed.uav.Get();ctx->CSSetUnorderedAccessViews(5,1,&output,nullptr);
    ctx->CSSetShader(compose.Get(),nullptr,0);ctx->Dispatch(P/8,P/8,P);ctx->ClearState();
    const auto filled=read(composed,P/2,P/2,P/2+P);
    Check(std::abs(filled[0]-.4f)<.001f&&std::abs(filled[1]-.2f)<.001f&&std::abs(filled[2]-.6f)<.001f,
          "a probe inside geometry is filled from open neighbours; bounce gain and sky apply live");
    // Every band and boundary must retain the old six-neighbour stencil. Distinct
    // values per axis/band catch missing dispatch slices and cross-band indexing.
    for(unsigned z=0;z<P*6;++z)for(unsigned y=0;y<P;++y)for(unsigned x=0;x<P;++x)
        marked[(size_t(z)*P+y)*P+x]={float(x)*.01f,float(y)*.02f,float(z%P)*.03f+float(z/P),((x+y+z)%7)==0?-1.f:.25f};
    transport=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,marked.data(),16);
    upload();input=transport.srv.Get();ctx->CSSetShaderResources(5,1,&input);
    output=composed.uav.Get();ctx->CSSetUnorderedAccessViews(5,1,&output,nullptr);
    ctx->CSSetShader(compose.Get(),nullptr,0);ctx->Dispatch(P/8,P/8,P);ctx->ClearState();
    const int stencilOffsets[7][3]{{0,0,0},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for(unsigned band=0;band<6;++band)for(unsigned c:{0u,P/2,P-1}){
        ProbeRow expected{};float weight=0;
        for(unsigned i=0;i<7;++i){int x=int(c)+stencilOffsets[i][0],y=int(c)+stencilOffsets[i][1],z=int(c)+stencilOffsets[i][2];
            if(x<0||y<0||z<0||x>=int(P)||y>=int(P)||z>=int(P))continue;
            const auto v=marked[(size_t(z+band*P)*P+y)*P+x];if(v[3]<0)continue;
            const float w=i==0?2.f:1.f;for(unsigned k=0;k<4;++k)expected[k]+=v[k]*w;weight+=w;
        }
        for(auto& v:expected)v=weight>0?v/weight:0;
        const auto actual=read(composed,c,c,c+band*P);
        for(unsigned k=0;k<3;++k)Check(std::abs(actual[k]-(expected[k]*2+expected[3]*values[4][k]))<.0001f,
            "shared visibility composition preserves every band, invalid centre and volume boundary");
    }
    // Relocation-aware filter: nominal centres lie on a wall, while both trace
    // origins lie just above it. Their radiance must mix; crossing the wall must not.
    std::fill(occupied.begin(),occupied.end(),0);
    for(unsigned y=0;y<V;++y)for(unsigned x=0;x<V;++x)occupied[(size_t(c)*V+y)*V+x]=PackGiVoxel({0,0,1},{1,1,1},false);
    auto filterWall=texture(V,V,DXGI_FORMAT_R32_UINT,occupied.data(),4);
    for(auto& v:marked)v={0,0,0,0};marked[(size_t(P/2)*P+P/2)*P+P/2+1]={8,0,0,0};
    transport=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,marked.data(),16);
    for(auto& v:momentData)v={8,64,1,3.5f};
    auto filterOrigins=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,momentData.data(),16);
    values[3][3]=1;values[4]={0,0,0,0};values[7]={float(V),float(V),float(V),0};
    auto filterRun=[&](){upload();auto t=transport.srv.Get();ctx->CSSetShaderResources(5,1,&t);
        auto wall=filterWall.srv.Get();ctx->CSSetShaderResources(0,1,&wall);ctx->CSSetShaderResources(6,1,&wall);
        auto origins=filterOrigins.srv.Get();ctx->CSSetShaderResources(15,1,&origins);
        auto target=composed.uav.Get();ctx->CSSetUnorderedAccessViews(5,1,&target,nullptr);
        ctx->CSSetShader(compose.Get(),nullptr,0);ctx->Dispatch(P/8,P/8,P);ctx->ClearState();return read(composed,P/2,P/2,P/2);};
    Check(filterRun()[0]>.9f,"filter connects relocated probes on the same side of a wall");
    momentData[(size_t(P/2)*P+P/2)*P+P/2+1][3]=-3.5f;
    filterOrigins=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,momentData.data(),16);
    Check(filterRun()[0]<.001f,"filter rejects relocated neighbours across a wall");

    // ---- CPU receiver replica for dip diagnostics -------------------------------------
    // Mirrors the screen Build / material WorldQuery math over downloaded probe volumes so
    // receiver fields can be scanned densely without recompiling the shader per point.
    struct VolData{std::vector<ProbeRow> px;unsigned w,h,d;};
    auto download=[&](Volume& v)->VolData{
        D3D11_TEXTURE3D_DESC desc;v.texture->GetDesc(&desc);
        desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        desc.MiscFlags=0;ComPtr<ID3D11Texture3D> staging;
        if(FAILED(device->CreateTexture3D(&desc,nullptr,&staging)))return{};
        ctx->CopyResource(staging.Get(),v.texture.Get());
        D3D11_MAPPED_SUBRESOURCE m{};if(FAILED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)))return{};
        VolData o;o.w=desc.Width;o.h=desc.Height;o.d=desc.Depth;o.px.resize(size_t(o.w)*o.h*o.d);
        for(unsigned z=0;z<o.d;++z)for(unsigned y=0;y<o.h;++y)
            memcpy(o.px.data()+(size_t(z)*o.h+y)*o.w,(const char*)m.pData+z*m.DepthPitch+y*m.RowPitch,o.w*16);
        ctx->Unmap(staging.Get(),0);return o;
    };
    struct Dump{float band[6];float total,validTotal,confidence;
        struct C{int cell[3];float code,delta[3],d2,vis,valid,facing,trilinear,weight,m[3][4];}c[27];};
    auto cpuBuildAt=[&](const VolData& distV,const VolData& compV,const ProbeRow& region,
                        float px,float py,float pz,float nx,float ny,float nz,Dump*out)->float{
        const float q[3]={std::min(std::max((px-region[0])/region[3],.5f/64),63.5f/64)*64+nx*.35f,
                          std::min(std::max((py-region[1])/region[3],.5f/64),63.5f/64)*64+ny*.35f,
                          std::min(std::max((pz-region[2])/region[3],.5f/64),63.5f/64)*64+nz*.35f};
        int base[3];float fraction[3];
        for(int a=0;a<3;++a){base[a]=(int)std::floor(q[a]);fraction[a]=q[a]-.5f-base[a];}
        float total=0,validTotal=0;float colors[6]{};
        const float nn[3]={nx,ny,nz};
        for(unsigned corner=0;corner<27;++corner){
            const int off[3]={int(corner%3)-1,int((corner/3)%3)-1,int(corner/9)-1};
            int cell[3];float delta[3];
            for(int a=0;a<3;++a){cell[a]=std::min(std::max(base[a]+off[a],0),63);delta[a]=q[a]-(cell[a]+.5f);}
            const auto& d0=distV.px[size_t(cell[2])*P*P+size_t(cell[1])*P+cell[0]];
            const float code=d0[3];const float axis=std::floor(std::fabs(code));
            const float amount=(std::fabs(code)-axis)*(code>=0?1:-1);
            for(int a=0;a<3;++a)if(axis==a+1)delta[a]-=amount;
            const float rawSquared=delta[0]*delta[0]+delta[1]*delta[1]+delta[2]*delta[2];
            const float d2=std::max(rawSquared,.0001f);const float rdist=1.f/std::sqrt(d2);
            const float angular[3]={rawSquared>1e-8f?delta[0]*delta[0]/rawSquared:1.f/3,
                                    rawSquared>1e-8f?delta[1]*delta[1]/rawSquared:1.f/3,
                                    rawSquared>1e-8f?delta[2]*delta[2]/rawSquared:1.f/3};
            float vis=0,valid=0;float mm[3][4]{};
            for(int a=0;a<3;++a){
                const unsigned band=a+(delta[a]>=0?0:3);
                const auto& mv=distV.px[size_t(cell[2]+band*P)*P*P+size_t(cell[1])*P+cell[0]];
                memcpy(mm[a],mv.data(),sizeof(mm[a]));
                const float beyond=std::max(1.f/rdist-mv[0],0.f),variance=std::max(mv[1]-mv[0]*mv[0],.0001f);
                const float v=variance/(variance+beyond*beyond);
                vis+=angular[a]*v*v*v*mv[2];valid=std::max(valid,mv[2]);
            }
            const float facing=std::max(.0001f,(1.f-(delta[0]*nx+delta[1]*ny+delta[2]*nz)*rdist)*.5f);
            const auto kernel=[&](int a){const float edge=.5f+off[a]*fraction[a];return off[a] == 0?.75f-fraction[a]*fraction[a]:.5f*edge*edge;};
            const float trilinear=kernel(0)*kernel(1)*kernel(2);
            const float weight=trilinear*vis*(facing*facing+.2f);
            total+=weight;validTotal+=trilinear*valid;
            for(unsigned b=0;b<6;++b){
                const auto& iv=compV.px[size_t(cell[2]+b*P)*P*P+size_t(cell[1])*P+cell[0]];
                colors[b]+=weight*iv[0];
            }
            if(out){auto&c=out->c[corner];memcpy(c.cell,cell,sizeof(cell));c.code=code;
                memcpy(c.delta,delta,sizeof(delta));c.d2=d2;c.vis=vis;c.valid=valid;
                c.facing=facing;c.trilinear=trilinear;c.weight=weight;memcpy(c.m,mm,sizeof(mm));}
        }
        const float confidence=std::min(std::max(total/std::max(validTotal,1e-4f)*1e4f,0.f),1.f);
        float band[6];for(unsigned b=0;b<6;++b)band[b]=colors[b]/std::max(total,1e-4f)*confidence;
        float mix=0;for(int a=0;a<3;++a)mix+=nn[a]*nn[a]*band[a+(nn[a]>=0?0:3)];
        if(out){out->total=total;out->validTotal=validTotal;out->confidence=confidence;memcpy(out->band,band,sizeof(band));}
        return mix;
    };
    auto printDumpAt=[&](const VolData& distV,const VolData& compV,const ProbeRow& region,
                         float px,float py,float pz,float nx,float ny,float nz){
        Dump d;const float v=cpuBuildAt(distV,compV,region,px,py,pz,nx,ny,nz,&d);
        std::cout<<"  dump p=("<<px<<","<<py<<","<<pz<<") n=("<<nx<<","<<ny<<","<<nz<<") v="<<v
                 <<" total="<<d.total<<" validTotal="<<d.validTotal<<" conf="<<d.confidence<<"\n";
        for(unsigned c=0;c<27;++c){const auto&r=d.c[c];
            std::cout<<"    corner"<<c<<" cell="<<r.cell[0]<<","<<r.cell[1]<<","<<r.cell[2]
                <<" code="<<r.code<<" delta="<<r.delta[0]<<","<<r.delta[1]<<","<<r.delta[2]
                <<" vis="<<r.vis<<" valid="<<r.valid<<" face="<<r.facing<<" tri="<<r.trilinear<<" w="<<r.weight
                <<" | m0="<<r.m[0][0]<<","<<r.m[0][1]<<","<<r.m[0][2]
                <<" m1="<<r.m[1][0]<<","<<r.m[1][1]<<","<<r.m[1][2]
                <<" m2="<<r.m[2][0]<<","<<r.m[2][1]<<","<<r.m[2][2]<<"\n";}
    };
    // ---- surface-adjacent receiver line: periodic dips along a wall above a floor ----
    {
        const unsigned V2=128;
        // Region [0,2560]: 20-unit voxels, 40-unit probes. Floor slab fills cells z{0,1}
        // (a floor surface near 27), wall slab fills cells x{1,2} above it (a wall face
        // near 33, room at x<33). Probe columns at x/z=20 land inside the slabs.
        values[0]={0,0,0,2560};values[1]={128,20,1,float(P/2)};values[2]=values[0];values[3]={1,20,128,1};
        values[4]={.4f,.4f,.4f,0};values[5]={0,2,0,0};values[6]={0,0,0,0};
        values[7]={128,128,128,0};values[8]={float(P),1,1,0};values[12]={};values[13]={};
        std::vector<uint32_t> room(size_t(V2)*V2*V2,0);
        auto roomAt=[&](unsigned x,unsigned y,unsigned z)->uint32_t&{return room[(size_t(z)*V2+y)*V2+x];};
        for(unsigned y=0;y<V2;++y)for(unsigned x=0;x<V2;++x)for(unsigned z:{0u,1u})
            roomAt(x,y,z)=PackGiVoxel({0,0,1},{.45f,.45f,.45f},false);
        // Sloped wall x = 40 + 0.5*y (room below): a two-cell staircase advancing one
        // voxel per two rows. Probes inside sit on two alternating diagonals; one can
        // only relocate -x into the room, the other only +x behind the wall.
        for(unsigned y=0;y<V2;++y)for(unsigned z=2;z<V2;++z){
            const int cx0=(40+10*int(y))/20;
            for(int x:{cx0,cx0+1})if(x>=0&&x<int(V2))roomAt(x,y,z)=PackGiVoxel({-.8944272f,.4472136f,0},{.5f,.5f,.5f},false);
        }
        auto roomVoxels=texture(V2,V2,DXGI_FORMAT_R32_UINT,room.data(),4);
        // Voxel radiance the ray marcher reads: floor top lit, wall room-side lit, both
        // modest so dips are visible against sky (.a = opacity, .rgb = outgoing radiance).
        std::vector<ProbeRow> rad(size_t(V2)*V2*V2,ProbeRow{0,0,0,0}),nrm(rad.size(),ProbeRow{0,0,0,0});
        for(unsigned y=0;y<V2;++y)for(unsigned x=0;x<V2;++x){
            for(unsigned z:{0u,1u}){auto i=(size_t(z)*V2+y)*V2+x;rad[i]={.3f,.3f,.3f,1};nrm[i]={0,0,1,0};}
            for(unsigned z=2;z<V2;++z){
                const int cx0=(40+10*int(y))/20;
                for(int x:{cx0,cx0+1})if(x>=0&&x<int(V2)){auto i=(size_t(z)*V2+y)*V2+x;rad[i]={.2f,.2f,.2f,1};nrm[i]={-.8944272f,.4472136f,0,0};}
            }
        }
        auto litRadiance=texture(V2,V2,DXGI_FORMAT_R32G32B32A32_FLOAT,rad.data(),16);
        auto litDir=texture(V2,V2,DXGI_FORMAT_R32G32B32A32_FLOAT,nrm.data(),16);
        auto litBack=texture(V2,V2,DXGI_FORMAT_R32G32B32A32_FLOAT,rad.data(),16);
        // All probes unknown -> mode 2 traces every band of every probe.
        std::vector<ProbeRow> unknown(size_t(P)*P*P*6,ProbeRow{0,0,0,-1});
        auto probeIn=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,unknown.data(),16);
        std::vector<ProbeRow> zeroMoments(unknown.size(),ProbeRow{0,0,0,0});
        auto distIn=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,zeroMoments.data(),16);
        upload();
        {
            auto cb2=constants.Get();ctx->CSSetConstantBuffers(0,1,&cb2);
            auto s=sampler.Get();ctx->CSSetSamplers(0,1,&s);
            ID3D11ShaderResourceView* srvs[13]{roomVoxels.srv.Get(),nullptr,nullptr,litRadiance.srv.Get(),litDir.srv.Get(),nullptr,
                nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,probeIn.srv.Get()};
            ctx->CSSetShaderResources(0,13,srvs);
            auto back=litBack.srv.Get();ctx->CSSetShaderResources(19,1,&back);
            auto moments=distIn.srv.Get();ctx->CSSetShaderResources(15,1,&moments);
            auto distances=distanceOut.uav.Get();ctx->CSSetUnorderedAccessViews(3,1,&distances,nullptr);
            auto out=historyOut.uav.Get();ctx->CSSetUnorderedAccessViews(4,1,&out,nullptr);
            ctx->CSSetShader(trace.Get(),nullptr,0);ctx->Dispatch(P/8,P/8,P*6);ctx->ClearState();
        }
        // Report relocation along the wall's inside-probe diagonals.
        for(unsigned i=1;i<=7;++i){
            const unsigned x=i,y=2*i-1; // diagonal A: nominal (40i+20, 40(2i-1)+20)
            const auto m=read(distanceOut,x,y,2+3*P);
            std::cout<<"inwallA ("<<x<<","<<y<<") reloc="<<m[3]<<" mz="<<m[2]<<" m="<<m[0]<<","<<m[1]<<std::endl;
        }
        for(unsigned i=1;i<=7;++i){
            const unsigned x=i,y=2*i-2; // diagonal B
            const auto m=read(distanceOut,x,y,2+3*P);
            std::cout<<"inwallB ("<<x<<","<<y<<") reloc="<<m[3]<<" mz="<<m[2]<<" m="<<m[0]<<","<<m[1]<<std::endl;
        }
        // Compose: fill invalid bands from open neighbours, apply sky+gain.
        auto roomComposed=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,nullptr,16);
        {
            upload();auto cb2=constants.Get();ctx->CSSetConstantBuffers(0,1,&cb2);
            auto input=historyOut.srv.Get();ctx->CSSetShaderResources(5,1,&input);
            auto wall=roomVoxels.srv.Get();ctx->CSSetShaderResources(0,1,&wall);
            auto origins=distanceOut.srv.Get();ctx->CSSetShaderResources(15,1,&origins);
            auto target=roomComposed.uav.Get();ctx->CSSetUnorderedAccessViews(5,1,&target,nullptr);
            ctx->CSSetShader(compose.Get(),nullptr,0);ctx->Dispatch(P/8,P/8,P);ctx->ClearState();
        }
        // Receiver line along the wall face (x+y=240), 6 units into the room,
        // tangent (1,-1,0)/sqrt2, z rows 30..44.
        const unsigned LW=100,LH=8;
        std::vector<ProbeRow> line(size_t(LW)*LH);
        for(unsigned v=0;v<LH;++v)for(unsigned u=0;u<LW;++u){
            const float t=float(u)*2;
            line[(size_t(v)*LW+u)]={117.f+t*.70710678f,117.f-t*.70710678f,30.f+v*2,1};
        }
        // CPU replica of the screen Build receiver over the same volumes: scans a dense
        // field of receivers and prints every corner term at the worst dip found.
        const VolData distV=download(distanceOut),compV=download(roomComposed);
        auto cpuBuild=[&](float px,float py,float pz,float nx,float ny,float nz,Dump*out)->float{
            return cpuBuildAt(distV,compV,values[0],px,py,pz,nx,ny,nz,out);
        };
        // Scan wall receivers (normal facing the diagonal wall) and floor receivers near it.
        auto printDump=[&](float px,float py,float pz,float nx,float ny,float nz){
            printDumpAt(distV,compV,values[0],px,py,pz,nx,ny,nz);
        };
        auto scan=[&](const char*tag,float nx,float ny,float nz,float x,float z){
            float worstRatio=1,worstT=-1,worstV=0;
            std::vector<float> vs;
            for(float t=0;t<=200;t+=1)vs.push_back(cpuBuild(x,t,z,nx,ny,nz,nullptr));
            for(size_t i=6;i+6<vs.size();++i){
                float local=0;for(int k=-6;k<=6;++k)local+=vs[i+k];local/=13;
                if(vs[i]/local<worstRatio){worstRatio=vs[i]/local;worstT=float(i);worstV=vs[i];}
            }
            std::cout<<tag<<" x="<<x<<" z="<<z<<" worst dip ratio="<<worstRatio
                     <<" at t="<<worstT<<" v="<<worstV<<"\n";
            if(worstT>=0)printDump(x,worstT,z,nx,ny,nz);
        };
        // Receivers run parallel to the sloped wall (dir (0.447,0.894)), at standoff s
        // along the wall normal (-0.894,0.447,0).
        auto scanWall=[&](float s,float z){
            float worst=1,wt=-1;std::vector<float> vs;
            for(float t=0;t<=200;t+=1)
                vs.push_back(cpuBuild(40+.4472136f*t-.8944272f*s,.8944272f*t-.4472136f*s,z,-.8944272f,.4472136f,0,nullptr));
            for(size_t i=6;i+6<vs.size();++i){float l=0;for(int k=-6;k<=6;++k)l+=vs[i+k];l/=13;
                if(vs[i]/l<worst){worst=vs[i]/l;wt=float(i);}}
            std::cout<<"slope wall s="<<s<<" z="<<z<<" worst="<<worst<<" at t="<<wt<<"\n";
            if(wt>=0)printDump(40+.4472136f*wt-.8944272f*s,.8944272f*wt-.4472136f*s,z,-.8944272f,.4472136f,0);
        };
        for(float s:{4.f,8.f,14.f,20.f}){scanWall(s,34);scanWall(s,46);}
        // Floor receivers just inside the room edge of the sloped wall.
        auto scanFloor=[&](float s){
            float worst=1,wt=-1;std::vector<float> vs;
            for(float t=0;t<=200;t+=1)
                vs.push_back(cpuBuild(40+.4472136f*t-.8944272f*s,.8944272f*t-.4472136f*s,33.f,0,0,1,nullptr));
            for(size_t i=6;i+6<vs.size();++i){float l=0;for(int k=-6;k<=6;++k)l+=vs[i+k];l/=13;
                if(vs[i]/l<worst){worst=vs[i]/l;wt=float(i);}}
            std::cout<<"slope floor s="<<s<<" worst="<<worst<<" at t="<<wt<<"\n";
        };
        for(float s:{4.f,10.f,20.f,30.f})scanFloor(s);
        // InjectCS feeds voxel radiance with ProbeIrradiance(p + n*cell): consecutive wall
        // cells sample the probe field at alternating y-phases (on-row vs between-rows).
        // If the field varies per probe, voxel radiance stripes at the 20-unit cell period.
        const VolData histV=download(historyOut);
        auto cpuBounce=[&](float px,float py,float pz,float nx,float ny,float nz)->float{
            const float q[3]={std::min(std::max(px/2560.f*64,.5f),63.5f),
                              std::min(std::max(py/2560.f*64,.5f),63.5f),
                              std::min(std::max(pz/2560.f*64,.5f),63.5f)};
            int base[3];float fraction[3];
            for(int a=0;a<3;++a){const float f=std::floor(q[a]-.5f);base[a]=(int)f;fraction[a]=q[a]-.5f-f;}
            float result=0,total=0;const float nn[3]={nx,ny,nz};const float skyRgb=.4f;
            for(unsigned corner=0;corner<8;++corner){
                const int off[3]={int(corner&1),int((corner>>1)&1),int((corner>>2)&1)};
                int cell[3];float delta[3];
                for(int a=0;a<3;++a){cell[a]=std::min(std::max(base[a]+off[a],0),63);delta[a]=q[a]-(cell[a]+.5f);}
                const auto& d0=distV.px[size_t(cell[2])*P*P+size_t(cell[1])*P+cell[0]];
                const float code=d0[3];const float axis=std::floor(std::fabs(code));
                const float amount=(std::fabs(code)-axis)*(code>=0?1:-1);
                for(int a=0;a<3;++a)if(axis==a+1)delta[a]-=amount;
                const float rawSquared=delta[0]*delta[0]+delta[1]*delta[1]+delta[2]*delta[2];
                const float d2=std::max(rawSquared,.0001f);
                const float angular[3]={rawSquared>1e-8f?delta[0]*delta[0]/rawSquared:1.f/3,
                                        rawSquared>1e-8f?delta[1]*delta[1]/rawSquared:1.f/3,
                                        rawSquared>1e-8f?delta[2]*delta[2]/rawSquared:1.f/3};
                float vis=0;
                for(int a=0;a<3;++a){
                    const unsigned band=a+(delta[a]>=0?0:3);
                    const auto& mv=distV.px[size_t(cell[2]+band*P)*P*P+size_t(cell[1])*P+cell[0]];
                    const float beyond=std::max(std::sqrt(d2)-mv[0],0.f),variance=std::max(mv[1]-mv[0]*mv[0],.0001f);
                    const float v=variance/(variance+beyond*beyond);vis+=angular[a]*v*v*v*mv[2];
                }
                const float facing=std::max(.0001f,(1.f-(delta[0]*nx+delta[1]*ny+delta[2]*nz)/std::sqrt(d2))*.5f);
                const float trilinear=(off[0]?fraction[0]:1-fraction[0])*(off[1]?fraction[1]:1-fraction[1])*(off[2]?fraction[2]:1-fraction[2]);
                const float weight=trilinear*vis*(facing*facing+.2f);
                if(weight<=1e-8f)continue;
                float color=0,valid=0;
                for(int a=0;a<3;++a){
                    const float ang=nn[a]*nn[a];if(ang<1e-4f)continue;
                    const unsigned face=a+(nn[a]>=0?0:3);
                    const auto& h=histV.px[size_t(cell[2]+face*P)*P*P+size_t(cell[1])*P+cell[0]];
                    if(h[3]>=0){color+=ang*(h[0]+h[3]*skyRgb);valid+=ang;}
                }
                result+=weight*color;total+=weight*valid;
            }
            return result/std::max(total,1e-4f)*std::min(std::max(total*1e4f,0.f),1.f);
        };
        std::cout<<"wall-cell bounce (p=cell centre + n*cell, n=-x), cells (1,j,4):\n";
        for(unsigned j=0;j<30;++j){
            const float v=cpuBounce(30-20.f,20.f*j+10,90.f,-1,0,0);
            std::cout<<"  j="<<j<<" y="<<20.f*j+10<<" bounce="<<v<<"\n";
        }
        D3D11_TEXTURE2D_DESC ld{};ld.Width=LW;ld.Height=LH;ld.MipLevels=ld.ArraySize=ld.SampleDesc.Count=1;
        ld.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;ld.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA lineData{line.data(),LW*16,0};ComPtr<ID3D11Texture2D> lineTex;
        Check(SUCCEEDED(device->CreateTexture2D(&ld,&lineData,&lineTex)),"receiver line texture");
        auto lineSrv=srvOf(lineTex.Get());
        // Screen cbuffer: uv=position/2560 clamped to texel centres, camera in the room.
        std::array<ProbeRow,10> screen{};
        screen[0]={1.f/2560,1.f/2560,1.f/2560,0};screen[1]={0,0,0,0};
        screen[2]={.5f/64,.5f/64,.5f/64,0};screen[3]={63.5f/64,63.5f/64,63.5f/64,0};
        screen[4]={-500,150,60,0};screen[5]={float(LW),float(LH),1,0};
        screen[9]={0,0,0,1}; // worldToClip row 3 -> output.w = 1
        auto screenCb=buffer(UINT(screen.size()*16),0,D3D11_BIND_CONSTANT_BUFFER,screen.data());
        D3D11_TEXTURE3D_DESC od{};od.Width=LW;od.Height=LH;od.Depth=6;od.MipLevels=1;
        od.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;od.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        Volume buildOut;
        Check(SUCCEEDED(device->CreateTexture3D(&od,nullptr,&buildOut.texture))&&
              SUCCEEDED(device->CreateShaderResourceView(buildOut.texture.Get(),nullptr,&buildOut.srv))&&
              SUCCEEDED(device->CreateUnorderedAccessView(buildOut.texture.Get(),nullptr,&buildOut.uav)),"receiver output volume");
        ComPtr<ID3DBlob> screenCode,screenErr;ComPtr<ID3D11ComputeShader> buildCs;
        {
            auto hr=Filmmaker::Environment::CompileEnvironmentShader(ScreenGiShader,strlen(ScreenGiShader),"ScreenGi",nullptr,"Build","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&screenCode,&screenErr);
            if(screenErr)std::cerr<<static_cast<const char*>(screenErr->GetBufferPointer());
            Check(SUCCEEDED(hr)&&SUCCEEDED(device->CreateComputeShader(screenCode->GetBufferPointer(),screenCode->GetBufferSize(),nullptr,&buildCs)),"receiver Build shader");
        }
        {
            auto cb2=screenCb.Get();ctx->CSSetConstantBuffers(0,1,&cb2);
            ID3D11ShaderResourceView* srvs[3]{lineSrv.Get(),roomComposed.srv.Get(),distanceOut.srv.Get()};
            ctx->CSSetShaderResources(0,3,srvs);
            auto out=buildOut.uav.Get();ctx->CSSetUnorderedAccessViews(0,1,&out,nullptr);
            ctx->CSSetShader(buildCs.Get(),nullptr,0);ctx->Dispatch((LW+7)/8,(LH+7)/8,1);ctx->ClearState();
        }
        std::cout<<"wall line t (receiver -x band value) at z=38:\n";
        for(unsigned u=0;u<LW;++u){
            const auto c=read(buildOut,u,4,3);
            std::cout<<"  t="<<u*2<<" rgb="<<c[0]<<","<<c[1]<<","<<c[2]<<" a="<<c[3]<<"\n";
        }
    }

    // ---- real-geometry window: de_inferno angled wall, CPU-voxelized occlusion ----
    {
        std::ifstream mf("Automation/runs/native-extract/de_inferno.mvmocclusion",std::ios::binary);
        if(mf){
            Filmmaker::Environment::OcclusionMesh mesh;std::string meshError;
            Check(ReadOcclusionMesh(mf,mesh,meshError),"load de_inferno occlusion cache");
            const float wx=-1633.f,wy=-1258.f,wz=181.f; // 20-degree wall centroid
            const float low[3]={std::floor((wx-1300)/20)*20,std::floor((wy-1300)/20)*20,std::floor((wz-1300)/20)*20};
            const unsigned V2=128;
            std::vector<uint32_t> vox(size_t(V2)*V2*V2,0);
            std::vector<ProbeRow> rad(size_t(V2)*V2*V2,ProbeRow{0,0,0,0}),nr(rad.size(),ProbeRow{0,0,0,0});
            auto mark=[&](float x,float y,float z,float nx,float ny,float nz){
                const int cx=(int)std::floor((x-low[0])/20),cy=(int)std::floor((y-low[1])/20),cz=(int)std::floor((z-low[2])/20);
                if(cx<0||cy<0||cz<0||cx>=(int)V2||cy>=(int)V2||cz>=(int)V2)return;
                const size_t i=(size_t(cz)*V2+cy)*V2+cx;
                vox[i]=PackGiVoxel({nx,ny,nz},{.5f,.5f,.5f},false);rad[i]={.2f,.2f,.2f,1};nr[i]={nx,ny,nz,0};
            };
            // Dominant-axis rasterization at 5-unit pitch, matching the runtime voxelizer.
            const size_t tris=mesh.indices.size()/3;
            for(size_t t=0;t<tris;++t){
                float v[3][3];
                for(int k=0;k<3;++k){const auto&p=mesh.positions[mesh.indices[3*t+k]];v[k][0]=p.x;v[k][1]=p.y;v[k][2]=p.z;}
                float mn[3]{std::min(v[0][0],std::min(v[1][0],v[2][0])),std::min(v[0][1],std::min(v[1][1],v[2][1])),std::min(v[0][2],std::min(v[1][2],v[2][2]))};
                float mx[3]{std::max(v[0][0],std::max(v[1][0],v[2][0])),std::max(v[0][1],std::max(v[1][1],v[2][1])),std::max(v[0][2],std::max(v[1][2],v[2][2]))};
                if(mx[0]<low[0]||mn[0]>low[0]+2560||mx[1]<low[1]||mn[1]>low[1]+2560||mx[2]<low[2]||mn[2]>low[2]+2560)continue;
                float nx=(v[1][1]-v[0][1])*(v[2][2]-v[0][2])-(v[1][2]-v[0][2])*(v[2][1]-v[0][1]);
                float ny=(v[1][2]-v[0][2])*(v[2][0]-v[0][0])-(v[1][0]-v[0][0])*(v[2][2]-v[0][2]);
                float nz=(v[1][0]-v[0][0])*(v[2][1]-v[0][1])-(v[1][1]-v[0][1])*(v[2][0]-v[0][0]);
                const float nl=std::sqrt(nx*nx+ny*ny+nz*nz);if(nl<1e-5f)continue;nx/=nl;ny/=nl;nz/=nl;
                const float an[3]={std::fabs(nx),std::fabs(ny),std::fabs(nz)};
                const int A=an[0]>=an[1]&&an[0]>=an[2]?0:(an[1]>=an[2]?1:2),U=(A+1)%3,W=(A+2)%3;
                const float lo_u=std::min(v[0][U],std::min(v[1][U],v[2][U])),hi_u=std::max(v[0][U],std::max(v[1][U],v[2][U]));
                const float lo_w=std::min(v[0][W],std::min(v[1][W],v[2][W])),hi_w=std::max(v[0][W],std::max(v[1][W],v[2][W]));
                for(float su=lo_u;su<=hi_u;su+=5)for(float sw=lo_w;sw<=hi_w;sw+=5){
                    const float e0=(su-v[0][U])*(v[1][W]-v[0][W])-(sw-v[0][W])*(v[1][U]-v[0][U]);
                    const float e1=(su-v[1][U])*(v[2][W]-v[1][W])-(sw-v[1][W])*(v[2][U]-v[1][U]);
                    const float e2=(su-v[2][U])*(v[0][W]-v[2][W])-(sw-v[2][W])*(v[0][U]-v[2][U]);
                    if((e0<0||e1<0||e2<0)&&(e0>0||e1>0||e2>0))continue;
                    float p[3];p[U]=su;p[W]=sw;
                    float nA=A==0?nx:(A==1?ny:nz);float nU=U==0?nx:(U==1?ny:nz);float nW=W==0?nx:(W==1?ny:nz);
                    if(std::fabs(nA)<1e-6f)continue;
                    p[A]=v[0][A]-(nU*(su-v[0][U])+nW*(sw-v[0][W]))/nA;
                    mark(p[0],p[1],p[2],nx,ny,nz);
                }
            }
            auto mapVoxels=texture(V2,V2,DXGI_FORMAT_R32_UINT,vox.data(),4);
            auto mapRad=texture(V2,V2,DXGI_FORMAT_R32G32B32A32_FLOAT,rad.data(),16);
            auto mapNrm=texture(V2,V2,DXGI_FORMAT_R32G32B32A32_FLOAT,nr.data(),16);
            values[0]={low[0],low[1],low[2],2560};values[1]={128,20,1,float(P/2)};values[2]=values[0];values[3]={1,20,128,1};
            values[4]={.4f,.4f,.4f,0};values[5]={0,2,0,0};values[6]={0,0,0,0};
            values[7]={128,128,128,0};values[8]={float(P),1,1,0};values[12]={};values[13]={};
            std::vector<ProbeRow> unknown(size_t(P)*P*P*6,ProbeRow{0,0,0,-1});
            auto probeIn2=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,unknown.data(),16);
            std::vector<ProbeRow> zeroMoments(unknown.size(),ProbeRow{0,0,0,0});
            auto distIn2=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,zeroMoments.data(),16);
            upload();
            {
                auto cb2=constants.Get();ctx->CSSetConstantBuffers(0,1,&cb2);
                auto s=sampler.Get();ctx->CSSetSamplers(0,1,&s);
                ID3D11ShaderResourceView* srvs[13]{mapVoxels.srv.Get(),nullptr,nullptr,mapRad.srv.Get(),mapNrm.srv.Get(),nullptr,
                    nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,probeIn2.srv.Get()};
                ctx->CSSetShaderResources(0,13,srvs);
                auto back=mapRad.srv.Get();ctx->CSSetShaderResources(19,1,&back);
                auto moments=distIn2.srv.Get();ctx->CSSetShaderResources(15,1,&moments);
                auto distances=distanceOut.uav.Get();ctx->CSSetUnorderedAccessViews(3,1,&distances,nullptr);
                auto out=historyOut.uav.Get();ctx->CSSetUnorderedAccessViews(4,1,&out,nullptr);
                ctx->CSSetShader(trace.Get(),nullptr,0);ctx->Dispatch(P/8,P/8,P*6);ctx->ClearState();
            }
            auto mapComposed=texture(P,P*6,DXGI_FORMAT_R32G32B32A32_FLOAT,nullptr,16);
            {
                upload();auto cb2=constants.Get();ctx->CSSetConstantBuffers(0,1,&cb2);
                auto input=historyOut.srv.Get();ctx->CSSetShaderResources(5,1,&input);
                auto wall=mapVoxels.srv.Get();ctx->CSSetShaderResources(0,1,&wall);
                auto origins=distanceOut.srv.Get();ctx->CSSetShaderResources(15,1,&origins);
                auto target=mapComposed.uav.Get();ctx->CSSetUnorderedAccessViews(5,1,&target,nullptr);
                ctx->CSSetShader(compose.Get(),nullptr,0);ctx->Dispatch(P/8,P/8,P);ctx->ClearState();
            }
            const VolData distM=download(distanceOut),compM=download(mapComposed);
            // Scan receivers along the wall tangent on both sides at several heights.
            const float wnx=.94f,wny=.35f,wtx=-.35f,wty=.94f; // wall normal / tangent
            auto scanLine=[&](const char*tag,float ox,float oy,float oz,float nx,float ny,float nz){
                std::vector<float> vs;
                for(float t=-150;t<=150;t+=1)vs.push_back(cpuBuildAt(distM,compM,values[0],ox+wtx*t,oy+wty*t,oz,nx,ny,nz,nullptr));
                float worst=1;int wi=-1;
                for(size_t i=6;i+6<vs.size();++i){float l=0;for(int k=-6;k<=6;++k)l+=vs[i+k];l/=13;
                    if(vs[i]/l<worst){worst=vs[i]/l;wi=int(i);}}
                const float wt=wi>=0?float(wi)-150.f:0;
                std::cout<<"map "<<tag<<" o=("<<ox<<","<<oy<<","<<oz<<") worst="<<worst<<" at t="<<wt<<"\n";
                Check(worst>=.85f,"Real angled-wall receivers have no periodic dip below 85% of their local mean");
                if(wi>=0)printDumpAt(distM,compM,values[0],ox+wtx*wt,oy+wty*wt,oz,nx,ny,nz);
            };
            for(float z:{171.f,181.f,191.f,211.f})
                for(float s:{4.f,10.f,20.f})
                    scanLine("wallA",wx+wnx*s,wy+wny*s,z,-wnx,-wny,0),
                    scanLine("wallB",wx-wnx*s,wy-wny*s,z,wnx,wny,0);
            for(float s:{4.f,10.f,20.f,40.f})
                scanLine("floor",wx-wnx*s,wy-wny*s,171.f,0,0,1);
        }
    }

}
// Exercise the production glass-cookie shader: incident visibility, not receiver shadows.
void CheckGlassIncidentLight(ID3D11Device* device,ID3D11DeviceContext* ctx) {
    auto visual=std::make_shared<Filmmaker::Environment::VisualCasters>();
    Filmmaker::Environment::StainedGlass pane;
    pane.width=pane.height=1;pane.rgba={255,0,0,255};
    pane.vertices={{{-8,-8,0},0,0},{{8,-8,0},1,0},{{8,8,0},1,1},{{-8,8,0},0,1}};
    pane.indices={0,1,2,0,2,3};visual->glass.push_back(pane);
    D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=8;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> atlas;ComPtr<ID3D11ShaderResourceView> atlasView;
    Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&atlas))&&
          SUCCEEDED(device->CreateShaderResourceView(atlas.Get(),nullptr,&atlasView)),"glass visibility atlas");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.ComparisonFunc=D3D11_COMPARISON_LESS_EQUAL;
    ComPtr<ID3D11SamplerState> comparison;Check(SUCCEEDED(device->CreateSamplerState(&sd,&comparison)),"glass comparison sampler");
    float matrix[16]{.01f,0,0,0,0,.01f,0,0,0,0,.01f,.5f,0,0,0,1};
    auto sun=SunRecord({0,0,1},{1,1,1},matrix,true);sun[52]=sun[53]=sun[54]=sun[55]=.5f;
    uint64_t frame=1;
    auto prepare=[&](Vec3 radiance){return PrepareStainedGlass(device,ctx,visual,&sun,atlasView.Get(),comparison.Get(),{0,0,1},radiance,frame++);};
    auto depth=[&](float value){float pixels[64];std::fill(std::begin(pixels),std::end(pixels),value);ctx->UpdateSubresource(atlas.Get(),0,nullptr,pixels,8*sizeof(float),0);};
    auto cookie=[&](ID3D11ShaderResourceView* srv){
        Check(srv!=nullptr,"glass cookie exists");ComPtr<ID3D11Resource> resource;srv->GetResource(&resource);
        ComPtr<ID3D11Texture3D> texture;resource.As(&texture);D3D11_TEXTURE3D_DESC t{};texture->GetDesc(&t);
        t.Usage=D3D11_USAGE_STAGING;t.BindFlags=0;t.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture3D> staging;Check(SUCCEEDED(device->CreateTexture3D(&t,nullptr,&staging)),"glass cookie readback texture");
        ctx->CopyResource(staging.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE m{};
        Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)),"glass cookie readback");
        const auto* pixel=static_cast<const unsigned char*>(m.pData)+(GlassCookieSize/2)*m.RowPitch+(GlassCookieSize/2)*4;
        const unsigned red=pixel[0];ctx->Unmap(staging.Get(),0);return red;
    };
    depth(1);auto lit=prepare({1,1,1});
    Check(lit.records.size()==1&&cookie(lit.cookies)>250,"sun reaching the pane produces its coloured cookie");
    depth(.25f);auto blocked=prepare({1,1,1});
    Check(cookie(blocked.cookies)==0,"a wall between the sun and pane extinguishes transmission");
    depth(1);auto relit=prepare({1,1,1});Check(cookie(relit.cookies)>250,"removing the blocker restores transmission");
    auto dark=prepare({});Check(dark.records.empty()&&!dark.cookies,"switching the sun off cannot retain a previous beam");
    Scene night;night.sunAngles.x=-10;night.sunBrightness=3;night.skyColor={1,1,1};night.indirect=1;
    auto below=prepare(SunRadiance(night));Check(below.records.empty()&&!below.cookies,"bright sky below the sun's horizon cannot create a directional beam");
    auto missing=PrepareStainedGlass(device,ctx,visual,&sun,nullptr,comparison.Get(),{0,0,1},{1,1,1},frame++);
    Check(missing.records.empty()&&!missing.cookies,"missing incident-light visibility fails dark");
    matrix[3]=5;sun=SunRecord({0,0,1},{1,1,1},matrix,true);sun[52]=sun[53]=sun[54]=sun[55]=.5f;
    Check(cookie(prepare({1,1,1}).cookies)==0,"a pane outside sun-shadow coverage cannot inherit a lit border texel");
    EndStainedGlass();ctx->ClearState();
}
void CheckStaticGeometryUploads(ID3D11Device* device,ID3D11DeviceContext* ctx) {
    using namespace Filmmaker::Environment;
    // Equivalent collision and render geometry, split into rigid data and two batches.
    // The first batch crosses both conversion slice boundaries and uses vertices in
    // the last slice. The second batch needs another nonzero vertex/albedo offset.
    auto collision=std::make_shared<OcclusionMesh>();
    collision->low={0,0,0};collision->high={128,128,128};
    collision->positions={{0,0,0},{128,0,0},{0,128,0},{128,0,0},{128,128,0},{0,128,0},
                          {0,0,128},{0,128,128},{128,0,128}};
    collision->indices={0,1,2,3,4,5,6,7,8};
    auto rendered=std::make_shared<OcclusionMesh>(*collision);
    auto visual=std::make_shared<VisualCasters>();visual->renderGeometry=true;
    visual->low=collision->low;visual->high=collision->high;
    visual->rigidPositions.assign(collision->positions.begin(),collision->positions.begin()+3);
    visual->rigidIndices={0,1,2};visual->rigidAlbedo={0x808080};
    CutoutCaster batch;batch.cutoff=0;batch.vertices.resize(16387);
    for(unsigned i=0;i<3;++i)batch.vertices[16384+i].position=collision->positions[3+i];
    for(unsigned i=0;i<5500;++i)batch.indices.insert(batch.indices.end(),{16384,16385,16386});
    visual->batches.push_back(std::move(batch));
    batch=CutoutCaster{};batch.cutoff=0;batch.vertices.resize(3);batch.indices={0,1,2};
    for(unsigned i=0;i<3;++i)batch.vertices[i].position=collision->positions[6+i];
    visual->batches.push_back(std::move(batch));rendered->visual=visual;
    Filmmaker::Relight::Scene scene;scene.indirect=.5f;scene.hasSun=true;scene.sunBrightness=1;
    std::vector<LightRecord> records{SunRecord({0,0,1},{1,1,1},nullptr,false)};
    const std::array<std::array<unsigned,3>,6> points{{{30,30,30},{31,30,30},{30,31,30},{31,31,30},{32,32,31},{40,40,40}}};
    auto sample=[&](const std::shared_ptr<OcclusionMesh>& world){
        EndProbes();ctx->ClearState();
        PrepareProbeGi(device,ctx,world,scene,records,nullptr,nullptr,0,0,{64,64,64});
        const auto binding=PrepareProbeGi(device,ctx,world,scene,records,nullptr,nullptr,1,0,{64,64,64});
        Check(binding.irradiance&&binding.distance,"geometry upload publishes a completed probe field");
        std::array<ProbeRow,12> values{};
        for(size_t i=0;i<points.size();++i){
            const auto p=points[i];
            values[i]=ReadHalf4(device,ctx,binding.irradiance,p[0],p[1],p[2]+5*GiProbeResolution);
            values[points.size()+i]=ReadHalf4(device,ctx,binding.distance,p[0],p[1],p[2]+5*GiProbeResolution);
        }
        return values;
    };
    const auto expected=sample(collision),actual=sample(rendered);
    for(size_t i=0;i<expected.size();++i)for(unsigned c=0;c<4;++c)
        Check(std::abs(expected[i][c]-actual[i][c])<.002f,"sliced render-geometry upload preserves collision-equivalent bounce and visibility");
    auto invalid=std::make_shared<OcclusionMesh>(*collision);invalid->indices.pop_back();
    EndProbes();
    Check(!PrepareProbeGi(device,ctx,invalid,scene,records,nullptr,nullptr,0).constants,"incomplete triangle data is rejected");
    Check(!PrepareProbeGi(device,ctx,invalid,scene,records,nullptr,nullptr,1).constants,"a failed map does not publish partial upload resources");
    EndProbes();ctx->ClearState();ctx->Flush();
}
void CheckDriver(ID3D11Device* device,ID3D11DeviceContext* ctx) {
    auto world=std::make_shared<Filmmaker::Environment::OcclusionMesh>();
    world->low={0,0,0};world->high={128,128,128};
    world->positions={{0,0,0},{128,0,0},{0,128,0}};world->indices={0,1,2};
    Scene scene;scene.indirect=.5f;scene.hasSun=true;scene.sunBrightness=1;
    std::vector<LightRecord> records{SunRecord({0,0,1},{1,1,1},nullptr,false)};
    Vec3 camera{64,64,64};
    ID3D11ShaderResourceView* shadow=nullptr;ID3D11SamplerState* comparison=nullptr;
    auto update=[&](uint64_t frame){return PrepareProbeGi(device,ctx,world,scene,records,shadow,comparison,frame,0,camera);};
    const unsigned warmup=2;
    for(uint64_t frame=0;frame+1<warmup;++frame){
        Check(!update(frame).constants,"an unfinished first sweep is never bound");
        Check(!update(frame).constants,"repeated draws in one frame do not advance the sweep");
    }
    const auto first=update(warmup-1);Check(first.constants&&first.irradiance,"the first complete sweep publishes");
    Check(first.reflection&&first.reflectionNorm,"A production live reflection capture is published");
    D3D11_SAMPLER_DESC receiverSampler{};first.sampler->GetDesc(&receiverSampler);
    Check(receiverSampler.Filter==D3D11_FILTER_MIN_MAG_MIP_POINT,"production receiver binding uses exact corner reads instead of filtering the cage twice");
    const auto sky=ReadHalf4(device,ctx,first.irradiance,40,40,40+2*GiProbeResolution);Check(sky[2]>0,"open probes receive live sky");
    // Depth priority collection occurs between material draws. It must leave the
    // published field intact, even when its contents differ from probe history.
    ComPtr<ID3D11Resource> field;first.irradiance->GetResource(&field);
    ComPtr<ID3D11UnorderedAccessView> fieldUav;
    Check(SUCCEEDED(device->CreateUnorderedAccessView(field.Get(),nullptr,&fieldUav)),"published irradiance fixture");
    const float sentinel[4]{7,6,5,4};ctx->ClearUnorderedAccessViewFloat(fieldUav.Get(),sentinel);
    D3D11_TEXTURE2D_DESC pd{};pd.Width=pd.Height=pd.MipLevels=pd.ArraySize=pd.SampleDesc.Count=1;
    pd.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;pd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    const ProbeRow point{camera.x,camera.y,camera.z,1};D3D11_SUBRESOURCE_DATA pointData{point.data(),16,0};
    ComPtr<ID3D11Texture2D> positions;ComPtr<ID3D11ShaderResourceView> positionView;
    Check(SUCCEEDED(device->CreateTexture2D(&pd,&pointData,&positions))&&
          SUCCEEDED(device->CreateShaderResourceView(positions.Get(),nullptr,&positionView)),"visible receiver fixture");
    MarkVisibleProbeGi(ctx,positionView.Get(),1,1,warmup-1);
    Check(ReadHalf4(device,ctx,first.irradiance,40,40,40+2*GiProbeResolution)==ProbeRow{7,6,5,4},
          "Collecting visible receivers cannot mutate the field consumed by material draws");
    scene.skyColor={0,1,0};const auto green=update(warmup);
    const auto greenSky=ReadHalf4(device,ctx,green.irradiance,40,40,40+2*GiProbeResolution);
    Check(greenSky[1]>0&&greenSky[0]<sky[0],"a sky colour edit shows on the next frame, no rebuild");
    // A budget remaps the same light from a blocked atlas tile to a lit tile.
    // Neither authored settings nor the record count changes during this transition.
    D3D11_TEXTURE2D_DESC td{};td.Width=2;td.Height=td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    const float depths[]{0,1};D3D11_SUBRESOURCE_DATA initial{depths,sizeof(depths),0};
    ComPtr<ID3D11Texture2D> atlas;ComPtr<ID3D11ShaderResourceView> atlasView;
    Check(SUCCEEDED(device->CreateTexture2D(&td,&initial,&atlas))&&SUCCEEDED(device->CreateShaderResourceView(atlas.Get(),nullptr,&atlasView)),"remapped GI shadow atlas");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.ComparisonFunc=D3D11_COMPARISON_LESS_EQUAL;
    ComPtr<ID3D11SamplerState> shadowSampler;Check(SUCCEEDED(device->CreateSamplerState(&sd,&shadowSampler)),"GI comparison sampler");
    shadow=atlasView.Get();comparison=shadowSampler.Get();scene.indirect=0;
    float matrix[16]{};matrix[0]=matrix[5]=1.f/4096;matrix[11]=.5f;matrix[15]=1;
    records[0]=SunRecord({0,0,1},{4,0,0},matrix,true);
    records[0][52]=.25f;records[0][53]=.5f;records[0][54]=.01f;records[0][55]=-.01f;
    uint64_t next=warmup+1;ProbeBinding binding;
    for(unsigned i=0;i<24;++i)binding=update(next++);
    const auto blocked=ReadHalf4(device,ctx,binding.irradiance,30,30,30+5*GiProbeResolution);
    const auto unchangedKey=ProbeLightingKey(scene);
    records[0][52]=.75f;
    Check(ProbeLightingKey(scene)==unchangedKey&&records.size()==1,"Atlas reassignment leaves authored key and record count unchanged");
    for(unsigned i=0;i<4;++i)binding=update(next++);
    const auto relit=ReadHalf4(device,ctx,binding.irradiance,30,30,30+5*GiProbeResolution);
    std::cout<<"GI atlas reassignment blocked="<<blocked[0]<<" relit="<<relit[0]<<std::endl;
    Check(relit[0]>.02f&&relit[0]>blocked[0]*3,"Nearby GI follows a same-count shadow tile reassignment within four frames");
    Check(relit[1]<relit[0]*.1f&&relit[2]<relit[0]*.1f,"Static map surfaces transport the incident light colour into probe bounce");
    for(unsigned i=0;i<64;++i)binding=update(next++);
    binding.irradiance->GetResource(&field);fieldUav.Reset();
    Check(SUCCEEDED(device->CreateUnorderedAccessView(field.Get(),nullptr,&fieldUav)),"settled irradiance fixture");
    ctx->ClearUnorderedAccessViewFloat(fieldUav.Get(),sentinel);
    binding=update(next++);
    Check(ReadHalf4(device,ctx,binding.irradiance,40,40,40+2*GiProbeResolution)==ProbeRow{7,6,5,4},
          "An unchanged static scene reuses settled transport rather than retracing animated shadow inputs");
    scene.bounce=0;binding=update(next++);
    Check(ReadHalf4(device,ctx,binding.irradiance,40,40,40+2*GiProbeResolution)!=ProbeRow{7,6,5,4},
          "Display bounce edits invalidate cached composition even though the transport lighting key is unchanged");
    scene.bounce=1;
    for(unsigned i=0;i<64;++i)binding=update(next++);
    ctx->ClearUnorderedAccessViewFloat(fieldUav.Get(),sentinel);
    scene.sunBrightness+=1;binding=update(next++);
    Check(ReadHalf4(device,ctx,binding.irradiance,40,40,40+2*GiProbeResolution)!=ProbeRow{7,6,5,4},
          "An authored lighting edit immediately invalidates settled transport");
    camera.x+=1;const auto walked=update(next++);
    Check(walked.reflectionOrigin.x==binding.reflectionOrigin.x,"Small live camera movement preserves the capture and its normalization origin");
    camera.x-=1;
    camera.x+=GiProbeSpacing*3;const auto moved=update(next++);
    Check(moved.constants&&moved.irradiance,"moving the camera never unbinds the bounce volume");
    Check(moved.packing.probe[0][3]!=first.packing.probe[0][3],"the bound volume follows the camera");
    camera.x+=GiRegionSize*2;const auto cut=update(next++);
    Check(cut.constants&&cut.distance,"a camera cut retains a complete GI binding");
    const auto newVisibility=ReadHalf4(device,ctx,cut.distance,1,1,1);
    Check(newVisibility[2]==1,"a camera cut traces exposed visibility before publishing the field");
    world=std::make_shared<Filmmaker::Environment::OcclusionMesh>(*world);
    Check(!update(next++).constants,"a map geometry change invalidates the volume");
    EndProbes();ctx->ClearState();ctx->Flush();
}
void CheckSunMsaaEdges(ID3D11Device* device,ID3D11DeviceContext* ctx){
    auto compile=[&](const std::string& source,const char* entry,const char* profile,const D3D_SHADER_MACRO* macros=nullptr){
        ComPtr<ID3DBlob> code,error;auto hr=Filmmaker::Environment::CompileEnvironmentShader(source.data(),source.size(),"SunMsaaEdge",macros,entry,profile,0,&code,&error);
        if(FAILED(hr)&&error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());
        Check(SUCCEEDED(hr),"MSAA edge shader compiles");return code;};
    auto vc=compile("float4 Main(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}","Main","vs_5_0");
    auto pc=compile("cbuffer Mode:register(b0){uint mode;float sky;} float Main(sample float4 p:SV_Position,uint s:SV_SampleIndex):SV_Target{if(mode==5)return .2+.015*(p.x+p.y);if(mode==3)return .2+.015*(p.x+p.y)+s*.003;if(mode==4)return uint(p.x)==4&&uint(p.y)==4?.6:.3;return mode==0||s==0?.6:mode==1?.3:sky;}","Main","ps_5_0");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    Check(SUCCEEDED(device->CreateVertexShader(vc->GetBufferPointer(),vc->GetBufferSize(),nullptr,&vs))&&SUCCEEDED(device->CreatePixelShader(pc->GetBufferPointer(),pc->GetBufferSize(),nullptr,&ps)),"MSAA depth fixture shaders");
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=8;td.MipLevels=td.ArraySize=1;td.SampleDesc.Count=4;td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> depth;ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11ShaderResourceView> srv;
    Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&depth))&&SUCCEEDED(device->CreateRenderTargetView(depth.Get(),nullptr,&rtv))&&SUCCEEDED(device->CreateShaderResourceView(depth.Get(),nullptr,&srv)),"MSAA depth fixture");
    td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> output,read;ComPtr<ID3D11UnorderedAccessView> uav;
    Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&output))&&SUCCEEDED(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav)),"MSAA edge output");
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&read)),"MSAA edge readback");
    const D3D_SHADER_MACRO macros[]{{"MSAA_SAMPLES","4"},{nullptr,nullptr}};
    auto cc=compile(std::string(Filmmaker::Environment::SunVisibilityShader)+"[numthreads(8,8,1)]void EdgeTest(uint3 id:SV_DispatchThreadID){float3 p;bool valid=Position(id.xy,p);float f;float z=ReadDepth(id.xy,f);float unused;result[id.xy]=depthInfo.w==5?float4(p,valid?1:0):float4(valid?1:0,z,f,ReadDepth(int2(id.xy)+int2(-1,0),unused));}","EdgeTest","cs_5_0",macros);
    ComPtr<ID3D11ComputeShader> cs;Check(SUCCEEDED(device->CreateComputeShader(cc->GetBufferPointer(),cc->GetBufferSize(),nullptr,&cs)),"MSAA edge compute");
    std::array<ProbeRow,86> rows{};rows[0][0]=rows[1][1]=rows[3][3]=1;rows[2][2]=100;rows[25][2]=-10;rows[26]={0,0,8,8};rows[27]={0,1,0,0};
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(rows);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> parameters,mode;Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&parameters)),"MSAA edge constants");bd.ByteWidth=16;Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&mode)),"MSAA fixture mode");
    for(unsigned reversed=0;reversed<2;++reversed)for(unsigned scenario=0;scenario<6;++scenario){
        ctx->ClearState();struct Mode{unsigned mode;float sky,pad[2];} data{scenario,reversed?0.f:1.f,{}};ctx->UpdateSubresource(mode.Get(),0,nullptr,&data,0,0);
        D3D11_VIEWPORT vp{0,0,8,8,0,1};ctx->RSSetViewports(1,&vp);auto target=rtv.Get();ctx->OMSetRenderTargets(1,&target,nullptr);auto b=mode.Get();ctx->PSSetConstantBuffers(0,1,&b);
        // One covered foreground sample surrounded by background, as on a thin railing.
        if(scenario==4){const float background[4]{.3f,.3f,.3f,.3f};ctx->ClearRenderTargetView(rtv.Get(),background);ctx->OMSetBlendState(nullptr,nullptr,1);}
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);ctx->Draw(3,0);ctx->ClearState();
        rows[27][2]=float(reversed);rows[27][3]=float(scenario);ctx->UpdateSubresource(parameters.Get(),0,nullptr,rows.data(),0,0);b=parameters.Get();ctx->CSSetConstantBuffers(0,1,&b);auto v=srv.Get();ctx->CSSetShaderResources(0,1,&v);auto u=uav.Get();ctx->CSSetUnorderedAccessViews(0,1,&u,nullptr);ctx->CSSetShader(cs.Get(),nullptr,0);ctx->Dispatch(1,1,1);ctx->ClearState();
        ctx->CopyResource(read.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE m{};Check(SUCCEEDED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&m)),"MSAA edge map");auto value=reinterpret_cast<const float*>(static_cast<const char*>(m.pData)+4*m.RowPitch+4*16);float valid=scenario==5?value[3]:value[0];if(scenario==5)Check(std::abs(value[2]-32-6*(value[0]-value[1]))<.0001f,"MSAA reconstructed receiver stays on the actual sloped plane");if(scenario==4)std::cout<<"isolated reversed="<<reversed<<" valid="<<valid<<" depths="<<value[1]<<","<<value[2]<<","<<value[3]<<std::endl;ctx->Unmap(read.Get(),0);
        Check(valid==((scenario==0||scenario==3||scenario==5)?1.f:0.f),scenario==4?"isolated thin foreground MSAA samples retain per-material shadows":scenario==3?"sloped distant MSAA surfaces retain soft shadows":scenario==0?"uniform MSAA surface retains cached soft shadows":"mixed foreground/background or sky samples require material shadow fallback");
    }
    // An ambiguous opaque MSAA edge can use a shared zero only when every
    // covered sample is independently shadowed. Clear or differently lit samples
    // must keep the native per-material fallback.
    auto productionCode=compile(Filmmaker::Environment::SunVisibilityShader,"Visibility","cs_5_0",macros);
    ComPtr<ID3D11ComputeShader> production;
    Check(SUCCEEDED(device->CreateComputeShader(productionCode->GetBufferPointer(),productionCode->GetBufferSize(),nullptr,&production)),"MSAA production sun compute");
    std::array<float,16*16> shadowDepth{};
    td={};td.Width=td.Height=16;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> shadow;ComPtr<ID3D11ShaderResourceView> shadowView;
    D3D11_SUBRESOURCE_DATA shadowData{shadowDepth.data(),16*4,0};
    Check(SUCCEEDED(device->CreateTexture2D(&td,&shadowData,&shadow))&&SUCCEEDED(device->CreateShaderResourceView(shadow.Get(),nullptr,&shadowView)),"MSAA opaque edge shadow atlas");
    rows[4]={1,0,0,0};rows[5]={0,1,0,0};rows[6]={0,0,-.01f,.9f};rows[7]={0,0,0,1};
    rows[20]={.5f,.5f,.5f,-.5f};rows[24]={1,1,1,1};rows[25]={0,0,100,0};
    rows[28]={0,0,16,1000};rows[29]={16,16,1.f/16,1.f/16};
    auto visibility=[&](unsigned scenario,unsigned reversed,float blocker){
        ctx->ClearState();struct Mode{unsigned mode;float sky,pad[2];} data{scenario,reversed?0.f:1.f,{}};
        ctx->UpdateSubresource(mode.Get(),0,nullptr,&data,0,0);
        D3D11_VIEWPORT vp{0,0,8,8,0,1};ctx->RSSetViewports(1,&vp);auto target=rtv.Get();ctx->OMSetRenderTargets(1,&target,nullptr);auto b=mode.Get();ctx->PSSetConstantBuffers(0,1,&b);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);ctx->Draw(3,0);ctx->ClearState();
        shadowDepth.fill(blocker);ctx->UpdateSubresource(shadow.Get(),0,nullptr,shadowDepth.data(),16*4,0);
        rows[27]={0,1,float(reversed),0};ctx->UpdateSubresource(parameters.Get(),0,nullptr,rows.data(),0,0);
        b=parameters.Get();ctx->CSSetConstantBuffers(0,1,&b);ID3D11ShaderResourceView* views[]{srv.Get(),shadowView.Get()};ctx->CSSetShaderResources(0,2,views);auto u=uav.Get();ctx->CSSetUnorderedAccessViews(0,1,&u,nullptr);ctx->CSSetShader(production.Get(),nullptr,0);ctx->Dispatch(1,1,1);ctx->ClearState();
        ctx->CopyResource(read.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE m{};Check(SUCCEEDED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&m)),"MSAA opaque edge visibility readback");
        ProbeRow value;memcpy(value.data(),static_cast<const char*>(m.pData)+4*m.RowPitch+4*16,16);ctx->Unmap(read.Get(),0);return value;
    };
    for(unsigned reversed=0;reversed<2;++reversed){
        const auto covered=visibility(1,reversed,0);
        Check(covered[0]<.001f&&covered[3]>.999f,"Every shadowed opaque MSAA sample retains covered trim edges without native sunlight leaks");
        const auto mixed=visibility(1,reversed,.45f);
        Check(mixed[3]==0,"Differently lit MSAA surfaces retain their per-material shadow fallback");
        const auto clear=visibility(2,reversed,0);
        Check(clear[3]==0,"An opaque edge against clear sky retains its per-material shadow fallback");
    }

}
void CheckSunContact(ID3D11Device* device,ID3D11DeviceContext* ctx){
    // The production PCSS search must not erase a narrow centre blocker.
    const std::string shader=std::string(Filmmaker::Environment::SunVisibilityShader)+
        "[numthreads(1,1,1)]void ContactTest(uint3 id:SV_DispatchThreadID){result[id.xy]=CascadeVisibility(0,camera.xyz,depthInfo.xyz).xxxx;}";
    ComPtr<ID3DBlob> code,error;Check(SUCCEEDED(Filmmaker::Environment::CompileEnvironmentShader(shader.data(),shader.size(),"SunContact",nullptr,"ContactTest","cs_5_0",0,&code,&error)),"sun contact shader compiles");
    ComPtr<ID3D11ComputeShader> cs;Check(SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&cs)),"sun contact shader");
    std::array<ProbeRow,30+Filmmaker::Environment::SunSearchTaps+Filmmaker::Environment::SunFilterTaps> rows{};
    const auto search=Filmmaker::Environment::SunDiskTaps<Filmmaker::Environment::SunSearchTaps,true>();
    const auto filter=Filmmaker::Environment::SunDiskTaps<Filmmaker::Environment::SunFilterTaps,false>();
    for(unsigned i=0;i<search.size();++i)rows[30+i]={search[i].x,search[i].y,search[i].radius,0};
    for(unsigned i=0;i<filter.size();++i)rows[30+search.size()+i]={filter[i].x,filter[i].y,filter[i].radius,0};
    rows[4]={1.f/32,0,0,0};rows[5]={0,1.f/32,0,0};rows[6]={0,0,.00001f,.5f};rows[7]={0,0,0,1};
    rows[20]={.5f,.5f,.5f,-.5f};rows[28]={.05f,0,64,100000};rows[29]={64,64,1.f/64,1.f/64};
    rows[27]={0,0,-1,0};
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(rows);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA bi{rows.data(),0,0};ComPtr<ID3D11Buffer> cb;Check(SUCCEEDED(device->CreateBuffer(&bd,&bi,&cb)),"sun test constants");
    std::vector<float> depths(64*64,1);for(unsigned y=31;y<=32;++y)for(unsigned x=31;x<=32;++x)depths[y*64+x]=.4999125f;
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=64;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{depths.data(),64*4,0};ComPtr<ID3D11Texture2D> atlas;ComPtr<ID3D11ShaderResourceView> srv;
    Check(SUCCEEDED(device->CreateTexture2D(&td,&data,&atlas))&&SUCCEEDED(device->CreateShaderResourceView(atlas.Get(),nullptr,&srv)),"sun test atlas");
    td.Width=td.Height=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> output,read;ComPtr<ID3D11UnorderedAccessView> uav;
    Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&output))&&SUCCEEDED(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav)),"sun test output");
    td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&read)),"sun test readback");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.ComparisonFunc=D3D11_COMPARISON_LESS_EQUAL;
    ComPtr<ID3D11SamplerState> sampler;Check(SUCCEEDED(device->CreateSamplerState(&sd,&sampler)),"sun test sampler");
    auto run=[&](){auto c=cb.Get();auto s=srv.Get();auto u=uav.Get();auto m=sampler.Get();ctx->CSSetConstantBuffers(0,1,&c);ctx->CSSetShaderResources(1,1,&s);ctx->CSSetSamplers(0,1,&m);ctx->CSSetUnorderedAccessViews(0,1,&u,nullptr);ctx->CSSetShader(cs.Get(),nullptr,0);ctx->Dispatch(1,1,1);ctx->ClearState();ctx->CopyResource(read.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};Check(SUCCEEDED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped)),"sun test map");float value=*static_cast<const float*>(mapped.pData);ctx->Unmap(read.Get(),0);return value;};
    Check(run()<.9f,"a thin centre blocker cannot disappear when the wide PCSS search misses it");
    std::fill(depths.begin(),depths.end(),1.f);ctx->UpdateSubresource(atlas.Get(),0,nullptr,depths.data(),64*4,0);
    Check(run()>.999f,"open sky remains fully lit after the centre-blocker safeguard");
    rows[27]={0,0,1,0};ctx->UpdateSubresource(cb.Get(),0,nullptr,rows.data(),0,0);
    Check(run()==0,"An opaque receiver blocks sunlight behind its geometric plane even with an empty atlas");

    // A receiver inside an opaque edge must not be moved into the opening by
    // normal bias. This models the bright rims reported at Inferno wall bases.
    rows[6]={0,0,-.00001f,.5f};rows[25]={-1.25f,0,0,0};
    rows[27]={.70710678f,0,.70710678f,0};rows[28][0]=.004625f;
    ctx->UpdateSubresource(cb.Get(),0,nullptr,rows.data(),0,0);
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)
        depths[y*64+x]=x<32?.4999f:1.f;
    ctx->UpdateSubresource(atlas.Get(),0,nullptr,depths.data(),64*4,0);
    const float contact=run();std::cout<<"sun wall contact visibility="<<contact<<std::endl;
    Check(contact<.05f,"an opaque contact corner stays shaded instead of sampling through the adjacent opening");
    rows[25][0]=-.09f;ctx->UpdateSubresource(cb.Get(),0,nullptr,rows.data(),0,0);
    Check(run()<.001f,"A subtexel opaque contact corner cannot borrow sunlight from its adjacent opening");

    // Just outside the same trim: one bilinear neighbour is blocked, but the
    // receiver's own texel is open. The former any-tap rule blackened both sides
    // of every ring and drew sharp teeth on the Nuke silo and glossy valves.
    rows[25][0]=.09f;ctx->UpdateSubresource(cb.Get(),0,nullptr,rows.data(),0,0);
    const float outsideContact=run();
    Check(outsideContact>.5f&&outsideContact<.65f,"An open centre retains partial neighbour contact coverage");
    rows[25][0]=-.09f;ctx->UpdateSubresource(cb.Get(),0,nullptr,rows.data(),0,0);

    // A nearby contact blocker still wins when the wide cone also sees distant
    // blockers. Averaging those distances must not soften an opaque trim join.
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)
        depths[y*64+x]=x<32?.49f:1.f;
    // Keep the close blocker beyond the shipped 1-2 texel receiver tolerance.
    for(unsigned y=31;y<=32;++y)depths[y*64+31]=.5f-.00002f;
    ctx->UpdateSubresource(atlas.Get(),0,nullptr,depths.data(),64*4,0);
    // Put one search tap on the distant blocker and the others outside its cone.
    // This isolates the averaging defect from the particular quadrature phase.
    for(unsigned j=0;j<search.size();++j)rows[30+j]={1,0,1,0};
    rows[30]={-2.f/231.25f,0,2.f/231.25f,0};
    ctx->UpdateSubresource(cb.Get(),0,nullptr,rows.data(),0,0);
    const float mixedContact=run();std::cout<<"sun mixed contact visibility="<<mixedContact<<std::endl;
    Check(mixedContact<.001f,"A near contact blocker cannot be diluted by distant blockers in the sun cone");
    for(unsigned j=0;j<search.size();++j)rows[30+j]={search[j].x,search[j].y,search[j].radius,0};

    // A convex receiver's neighbouring shadow texels lie in front of its tangent
    // plane. These are its own curved facets, not a separate blocker. Test the
    // actual production shader with the runtime bias, rather than bias=0 only.
    rows[6][2]=-.001f; // .0002 normalized bias is .2 world units here
    rows[25]={0,0,0,0};rows[27]={0,0,1,0};rows[28][1]=.0002f;
    ctx->UpdateSubresource(cb.Get(),0,nullptr,rows.data(),0,0);
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){
        const float dx=x+.5f-32,dy=y+.5f-32;
        depths[y*64+x]=.5f-.001f*.25f*(dx*dx+dy*dy);
    }
    ctx->UpdateSubresource(atlas.Get(),0,nullptr,depths.data(),64*4,0);
    const float curvedLit=run();
    Check(curvedLit>.999f,"An exposed curved ground receiver does not shadow its own subtexel facets");
    // Reproduce the former five-percent cap against the same curved geometry.
    std::string oldShader=shader;const auto oldCap=oldShader.find("max(sun.y,texelWorld*(.75+.25/max(abs(facing),.2))*depthPerUnit)");
    Check(oldCap!=std::string::npos,"curved receiver regression locates the production bound");
    oldShader.replace(oldCap,strlen("max(sun.y,texelWorld*(.75+.25/max(abs(facing),.2))*depthPerUnit)"),"min(sun.y,texelWorld*.05*depthPerUnit)");
    ComPtr<ID3DBlob> oldCode;ComPtr<ID3D11ComputeShader> oldCs;
    Check(SUCCEEDED(Filmmaker::Environment::CompileEnvironmentShader(oldShader.data(),oldShader.size(),"SunContactOldCap",nullptr,"ContactTest","cs_5_0",0,&oldCode,&error))&&
        SUCCEEDED(device->CreateComputeShader(oldCode->GetBufferPointer(),oldCode->GetBufferSize(),nullptr,&oldCs)),"old tolerance regression shader");
    auto fixedCs=cs;cs=oldCs;Check(run()<.001f,"The former cap reproduces the curved receiver self-shadow");cs=fixedCs;
    // A genuinely separate blocker two units above that same receiver must
    // survive the bounded tolerance, including a narrow contact footprint.
    for(unsigned y=31;y<=32;++y)for(unsigned x=31;x<=32;++x)depths[y*64+x]=.5f-.002f;
    ctx->UpdateSubresource(atlas.Get(),0,nullptr,depths.data(),64*4,0);
    Check(run()<.001f,"A separate narrow blocker survives curved-receiver tolerance");
    rows[6][2]=-.00001f;rows[28][1]=0;

    // Exact planar self-depth at several slopes must stay lit without the old
    // normal/slope offsets; otherwise fixing contact would introduce shadow acne.
    for(const auto normal:std::array<ProbeRow,5>{{{0,0,1,0},{.70710678f,0,.70710678f,0},{.8f,.5f,.33166248f,0},{.995f,0,.09987492f,0},{.99995f,0,.01f,0}}}){
        const float sx=normal[0]/normal[2],sy=normal[1]/normal[2];
        rows[25]={.37f,-.21f,-sx*.37f+sy*.21f,0};rows[27]=normal;
        ctx->UpdateSubresource(cb.Get(),0,nullptr,rows.data(),0,0);
        for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)
            depths[y*64+x]=.5f+.00001f*(sx*(x+.5f-32)-sy*(y+.5f-32));
        ctx->UpdateSubresource(atlas.Get(),0,nullptr,depths.data(),64*4,0);
        const float lit=run();std::cout<<"sun planar visibility="<<lit<<std::endl;
        Check(lit>.999f,"an unoccluded sloped receiver stays lit without contact-detaching bias");
    }

}
int main(){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)),"WARP device");
    CheckSunMsaaEdges(device.Get(),ctx.Get());
    CheckSunContact(device.Get(),ctx.Get());
    CheckAmbientLifecycle(device.Get(),ctx.Get());
    CheckScreenCache(device.Get(),ctx.Get());
    CheckMath();
    std::cout<<"Probe math: region stepping, history shift, brick binning and voxel words passed\n";
    CheckShaders(device.Get(),ctx.Get());
    std::cout<<"Probe shaders: albedo, stable occupancy, multi-bounce, porosity, blending, shifting and composition passed\n";
    CheckGlassIncidentLight(device.Get(),ctx.Get());
    std::cout<<"Glass cookies: lit, blocked, relit, sun off, night and missing visibility passed\n";
    CheckStaticGeometryUploads(device.Get(),ctx.Get());
    CheckDriver(device.Get(),ctx.Get());
    std::cout<<"Probe driver: warm-up, live sky, camera-following volume and map invalidation passed\n";
}
