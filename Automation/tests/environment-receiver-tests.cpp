#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentViewmodel.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderCompiler.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentClimateMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentStormMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentWeatherMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderPrograms.h"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <iostream>
#include <cstring>
using Microsoft::WRL::ComPtr;
using namespace Filmmaker::Environment;
namespace Filmmaker { bool MvmDebugLog_Active(){return false;} void MvmDebugLog_LinefAlways(const char*,const char*,...){} }
void Check(bool ok,const char *what){if(!ok){std::cerr<<what<<'\n';std::exit(1);}}
int main(){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)),"WARP device");
    auto compile=[&](const std::string &source,const char *entry,const char *profile,bool ms=false,bool colorMs=false){
        D3D_SHADER_MACRO macros[]={{"DEPTH_MS",ms?"1":"0"},{"COLOR_MS",colorMs?"1":"0"},{nullptr,nullptr}};
        ComPtr<ID3DBlob> code,error;
        auto hr=CompileEnvironmentShader(source.data(),source.size(),"ReceiverTest",macros,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,&code,&error);
        if(FAILED(hr)&&error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());
        Check(SUCCEEDED(hr),"Compile receiver probe");return code;
    };
    // The DLL's own defines, so a new shader constant cannot leave the probe uncompilable.
    const std::string source=EnvironmentShaderSource()+R"(
        float4 ReceiverProbe(float4 p:SV_Position):SV_Target {
            return float4(ReceiverPosition(p.xy,ReadSampleDepth(p.xy,0),0),IsViewmodelSample(p.xy,0));
        }
        float4 NormalProbe(float4 p:SV_Position):SV_Target {
            return float4(RelightNormal(p.xy,ReadSampleDepth(p.xy,0),0,false),1);
        }
        float4 ContactProbe(float4 p:SV_Position):SV_Target {
            float d=ReadSampleDepth(p.xy,0);float3 position=ReceiverPosition(p.xy,d,0);
            return ViewmodelContactOcclusion(p.xy,d,0,position,float3(0,0,1));
        }
        float4 MaterialProbe(float4 p:SV_Position):SV_Target {
            float3 color=((int)p.x&1)?float3(.4,.2,.1):float3(.04,.02,.01);
            return float4(Surface(color,float3(0,0,0),float3(0,0,1),float3(0,0,1),true,.5),1);
        }
        float4 WetProbe(float4 p:SV_Position):SV_Target {
            // Adjacent triangles have slightly different geometric normals but the same
            // filtered surface. Wet material response must not draw their shared edge.
            float3 n=normalize(float3(((int)p.x&1)?.24:-.12,0,1));
            return float4(ApplyCoatings(float3(.3,.25,.2),float3(0,100,-40),n,float3(0,0,1),.25),1);
        }
        float4 ShadowWarpProbe(float4 p:SV_Position):SV_Target {
            // Same receiver and sun incidence, opposite tangent normals. A large normal
            // bias incorrectly moves one lookup across the blocker silhouette.
            float3 n=float3(((int)p.x&1)?.6:-.6,0,.8);
            return float4(Surface(float3(.3,.3,.3),float3(.25,0,0),n,n),1);
        }
    )";
    auto vsCode=compile(source,"ScreenVS","vs_5_0");ComPtr<ID3D11VertexShader> screen;
    Check(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&screen)),"Screen VS");
    const char *nativeSource=R"(
        cbuffer PerViewConstantBuffer_t:register(b3){
            column_major float4x4 unusedMatrix:packoffset(c0);
            column_major float4x4 g_matProjectionToWorld:packoffset(c4);
            float4 g_vWorldToCameraOffset:packoffset(c24);
        }
        float4 Main(uint id:SV_VertexID):SV_Position {
            return mul(g_matProjectionToWorld,float4(id,1,1,1))+g_vWorldToCameraOffset;
        }
    )";
    auto nativeCode=compile(nativeSource,"Main","vs_5_0");ComPtr<ID3D11VertexShader> native;
    Check(SUCCEEDED(device->CreateVertexShader(nativeCode->GetBufferPointer(),nativeCode->GetBufferSize(),nullptr,&native)),"Native fixture VS");
    NoteViewmodelShader(native.Get(),nativeCode->GetBufferPointer(),nativeCode->GetBufferSize());
    std::array<float,100> nativeData{};
    // Inverse perspective, native eye at (10,20,30), deliberately different from
    // both the camera-relative origin used live and the actual world camera.
    nativeData[16]=4;nativeData[21]=5;nativeData[24]=-9.9f;nativeData[25]=-19.8f;
    nativeData[26]=-29.7f;nativeData[27]=-.99f;nativeData[31]=1;
    nativeData[28]=10;nativeData[29]=20;nativeData[30]=29;
    nativeData[96]=2;nativeData[97]=3;nativeData[98]=4;
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(nativeData);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{nativeData.data(),0,0};ComPtr<ID3D11Buffer> nativeBuffer;
    Check(SUCCEEDED(device->CreateBuffer(&bd,&initial,&nativeBuffer)),"Native fixture constants");
    auto reflect=reinterpret_cast<HRESULT(WINAPI*)(LPCVOID,SIZE_T,REFIID,void**)>(GetProcAddress(EnvironmentShaderCompiler(),"D3DReflect"));
    Check(reflect!=nullptr,"Reflect shader");
    auto receiverCode=compile(source,"ReceiverProbe","ps_5_0");ComPtr<ID3D11ShaderReflection> reflection;
    Check(SUCCEEDED(reflect(receiverCode->GetBufferPointer(),receiverCode->GetBufferSize(),__uuidof(ID3D11ShaderReflection),&reflection)),"Receiver reflection");
    auto frameLayout=reflection->GetConstantBufferByName("FrameConstants");D3D11_SHADER_BUFFER_DESC desc{};
    Check(SUCCEEDED(frameLayout->GetDesc(&desc)),"Frame constants layout");
    std::vector<float> frameData(desc.Size/4);
    auto field=[&](const char *name){D3D11_SHADER_VARIABLE_DESC v{};Check(SUCCEEDED(frameLayout->GetVariableByName(name)->GetDesc(&v)),name);return frameData.data()+v.StartOffset/4;};
    float *matrix=field("inverseViewProjection");for(int i=0;i<4;++i)matrix[i*5]=1;
    matrix=field("viewProjection");for(int i=0;i<4;++i)matrix[i*5]=1;
    auto dim=field("dimensions");for(int i=0;i<4;++i)dim[i]=256;
    field("depthRange")[1]=.95f;field("viewmodelInfo")[0]=1;
    field("camera")[0]=100;field("camera")[1]=-200;field("camera")[2]=300;
    bd.ByteWidth=desc.Size;initial.pSysMem=frameData.data();ComPtr<ID3D11Buffer> frameBuffer;
    Check(SUCCEEDED(device->CreateBuffer(&bd,&initial,&frameBuffer)),"Frame buffer");
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=256;td.ArraySize=td.MipLevels=1;td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target,readback;ComPtr<ID3D11RenderTargetView> rtv;
    Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&target))&&SUCCEEDED(device->CreateRenderTargetView(target.Get(),nullptr,&rtv)),"Probe target");
    td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&readback)),"Probe readback");
    auto render=[&](ID3DBlob *code,int pixelX=128,int pixelY=128){
        ComPtr<ID3D11PixelShader> ps;Check(SUCCEEDED(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps)),"Probe PS");
        ctx->OMSetRenderTargets(1,rtv.GetAddressOf(),nullptr);D3D11_VIEWPORT vp{0,0,256,256,0,1};ctx->RSSetViewports(1,&vp);
        ctx->VSSetShader(screen.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->UpdateSubresource(frameBuffer.Get(),0,nullptr,frameData.data(),0,0);auto b=frameBuffer.Get();ctx->PSSetConstantBuffers(0,1,&b);ctx->Draw(3,0);
        ctx->CopyResource(readback.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE map{};Check(SUCCEEDED(ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&map)),"Read probe");
        std::array<float,8> result{};memcpy(result.data(),static_cast<char*>(map.pData)+pixelY*map.RowPitch+pixelX*16,sizeof(result));ctx->Unmap(readback.Get(),0);return result;
    };
    for(bool ms:{false,true}){
        ResetViewmodel();ctx->ClearState();td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;td.SampleDesc.Count=ms?4:1;
        td.Format=DXGI_FORMAT_R32_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> depth;ComPtr<ID3D11DepthStencilView> dsv;ComPtr<ID3D11ShaderResourceView> srv;
        Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&depth)),"Native depth fixture");
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};dd.Format=DXGI_FORMAT_D32_FLOAT;dd.ViewDimension=ms?D3D11_DSV_DIMENSION_TEXTURE2DMS:D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32_FLOAT;sd.ViewDimension=ms?D3D11_SRV_DIMENSION_TEXTURE2DMS:D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
        Check(SUCCEEDED(device->CreateDepthStencilView(depth.Get(),&dd,&dsv))&&SUCCEEDED(device->CreateShaderResourceView(depth.Get(),&sd,&srv)),"Depth views");
        Check(!BindViewmodel(ctx.Get(),depth.Get()),"No fabricated snapshot before native pass");
        ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,.8f,0);ctx->OMSetRenderTargets(0,nullptr,dsv.Get());
        auto cb=nativeBuffer.Get();ctx->UpdateSubresource(cb,0,nullptr,nativeData.data(),0,0);ctx->VSSetConstantBuffers(3,1,&cb);ctx->VSSetShader(native.Get(),nullptr,0);
        D3D11_VIEWPORT vp{0,0,256,256,0,.95f};ctx->RSSetViewports(1,&vp);ObserveViewmodel(ctx.Get());
        Check(!BindViewmodel(ctx.Get(),depth.Get()),"World viewport is not a viewmodel pass");
        vp.MaxDepth=.1f;ctx->RSSetViewports(1,&vp);ObserveViewmodel(ctx.Get());
        ctx->OMSetRenderTargets(0,nullptr,nullptr);Check(BindViewmodel(ctx.Get(),depth.Get()),"Capture exact native view constants and before-depth");
        auto changed=nativeData;changed[28]=999;ctx->UpdateSubresource(nativeBuffer.Get(),0,nullptr,changed.data(),0,0);
        ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,.05f,0);auto resource=srv.Get();ctx->PSSetShaderResources(1,1,&resource);
        auto code=compile(source,"ReceiverProbe","ps_5_0",ms);auto hand=render(code.Get());
        Check(std::abs(hand[0]-(100+.015625f/.505f))<.0001f&&std::abs(hand[1]-(-200-.01953125f/.505f))<.0001f&&std::abs(hand[2]-(300-1/.505f))<.0001f&&hand[3]==1,"Native receiver reanchors the snapshotted perspective eye at the world camera");
        auto normalCode=compile(source,"NormalProbe","ps_5_0",ms);auto normal=render(normalCode.Get());
        Check(std::abs(normal[0])<1e-5&&std::abs(normal[1])<1e-5&&std::abs(normal[2]-1)<1e-5,"Fitted viewmodel normals preserve a planar receiver");
        auto contactCode=compile(source,"ContactProbe","ps_5_0",ms);auto contact=render(contactCode.Get());
        Check(std::abs(contact[0]-1)<1e-5,"Flat viewmodel surface does not self-occlude, including MSAA");
        if(!ms){
            // Visible neighboring surfaces protrude toward the viewer, leaving the sampled
            // center pixel recessed. Same native projection/snapshot as the production pass.
            std::vector<float> cavity(256*256,.02f);for(int y=127;y<=130;++y)for(int x=127;x<=130;++x)cavity[y*256+x]=.05f;
            D3D11_TEXTURE2D_DESC aoDesc{};aoDesc.Width=aoDesc.Height=256;aoDesc.MipLevels=aoDesc.ArraySize=aoDesc.SampleDesc.Count=1;
            aoDesc.Format=DXGI_FORMAT_R32_FLOAT;aoDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA cavityData{cavity.data(),256*sizeof(float),0};
            ComPtr<ID3D11Texture2D> cavityTexture;ComPtr<ID3D11ShaderResourceView> cavityView;
            Check(SUCCEEDED(device->CreateTexture2D(&aoDesc,&cavityData,&cavityTexture))&&SUCCEEDED(device->CreateShaderResourceView(cavityTexture.Get(),nullptr,&cavityView)),"Contact cavity fixture");
            auto cavitySrv=cavityView.Get();ctx->PSSetShaderResources(1,1,&cavitySrv);
            auto recessed=render(contactCode.Get());Check(recessed[0]<.95f&&recessed[0]>.4f,"Nearby glove/weapon crease reduces ambient visibility without crushing it");
            std::fill(cavity.begin(),cavity.end(),.8f);for(int y=127;y<=130;++y)for(int x=127;x<=130;++x)cavity[y*256+x]=.05f;
            ctx->UpdateSubresource(cavityTexture.Get(),0,nullptr,cavity.data(),256*sizeof(float),0);
            auto silhouette=render(contactCode.Get());Check(silhouette[0]>.999f,"World pixels behind a hand cannot create contact halos");
            ctx->PSSetShaderResources(1,1,&resource);
        }
        ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,.8f,0);auto wall=render(code.Get());
        Check(wall[3]==0&&std::abs(wall[2]-.8f/.95f)<.0001f,"Unchanged world pixel uses world projection");
        normal=render(normalCode.Get());
        Check(std::abs(normal[0])<1e-5&&std::abs(normal[1])<1e-5&&std::abs(normal[2]-1)<1e-5,"Fitted world normals preserve planar walls without faceting");
        Check(!BindViewmodel(ctx.Get(),target.Get()),"Unrelated depth fails closed");
        NextViewmodelFrame();Check(!BindViewmodel(ctx.Get(),depth.Get()),"Next frame cannot reuse stale hand pose");
    }
    ResetViewmodel();ctx->ClearState();
    auto c=field("climateData");c[CL_wet]=1;c[CL_sky]=1;c[CL_sunElevation]=35;c[CL_maskScale]=.02f;
    auto wet=compile(source,"WetProbe","ps_5_0");auto coated=render(wet.Get());
    for(int i=0;i<3;++i)Check(std::abs(coated[i]-coated[i+4])<1e-6f,"Wetness does not trace geometric triangle edges into material roughness");
    c[CL_wet]=0;c[CL_puddles]=1;coated=render(wet.Get());
    for(int i=0;i<3;++i)Check(std::abs(coated[i]-coated[i+4])<1e-6f,"Puddle-only coverage uses the filtered normal too");
    ctx->ClearState();std::fill(c,c+ClimateCount,0.f);
    c[CL_sunColorR]=c[CL_sunColorG]=c[CL_sunColorB]=1;
    c[CL_relight]=1;c[CL_ambient]=1;
    auto materialCode=compile(source,"MaterialProbe","ps_5_0");auto material=render(materialCode.Get());
    Check(std::abs(material[0]/material[4]-.1f)<1e-5f,"Viewmodel relight preserves native ten-to-one material contrast");
    Check(std::abs(material[4]-.4f)<1e-5f,"Viewmodel contact occlusion affects ambient energy");
    c[CL_ambient]=0;c[CL_sunRelight]=1;c[CL_sunElevation]=90;c[CL_sunPower]=1;
    auto directMaterial=render(materialCode.Get());Check(std::abs(directMaterial[4]-.8f)<1e-5f,"Contact AO must not darken unobstructed direct sunlight");
    std::fill(c,c+ClimateCount,0.f);
    c[CL_sunColorR]=c[CL_sunColorG]=c[CL_sunColorB]=1;
    c[CL_relight]=1;c[CL_sunRelight]=1;c[CL_contactShadows]=1;c[CL_sunElevation]=90;c[CL_sunPower]=1;
    auto shadowProbe=compile(source,"ShadowWarpProbe","ps_5_0");ComPtr<ID3D11ShaderReflection> shadowReflection;
    Check(SUCCEEDED(reflect(shadowProbe->GetBufferPointer(),shadowProbe->GetBufferSize(),__uuidof(ID3D11ShaderReflection),&shadowReflection)),"Shadow probe reflection");
    auto shadowLayout=shadowReflection->GetConstantBufferByName("ShadowConstants");D3D11_SHADER_BUFFER_DESC shadowDesc{};
    Check(SUCCEEDED(shadowLayout->GetDesc(&shadowDesc)),"Shadow constants layout");
    std::vector<float> shadowData(shadowDesc.Size/4);
    auto shadowField=[&](const char *name){D3D11_SHADER_VARIABLE_DESC v{};Check(SUCCEEDED(shadowLayout->GetVariableByName(name)->GetDesc(&v)),name);return shadowData.data()+v.StartOffset/4;};
    auto sunMatrix=shadowField("sunShadowMatrix");sunMatrix[0]=sunMatrix[5]=.05f;sunMatrix[10]=-.01f;sunMatrix[11]=.5f;sunMatrix[15]=1;
    auto info=shadowField("shadowInfo");info[0]=1;info[1]=2;info[3]=256;
    bd.ByteWidth=shadowDesc.Size;initial.pSysMem=shadowData.data();ComPtr<ID3D11Buffer> shadowBuffer;
    Check(SUCCEEDED(device->CreateBuffer(&bd,&initial,&shadowBuffer)),"Shadow fixture buffer");
    auto shadowCb=shadowBuffer.Get();ctx->PSSetConstantBuffers(2,1,&shadowCb);
    std::vector<float> shadowPixels(256*256,1.f);for(int y=0;y<256;++y)for(int x=0;x<128;++x)shadowPixels[y*256+x]=.1f;
    td={};td.Width=td.Height=256;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA shadowInitial{shadowPixels.data(),256*sizeof(float),0};
    ComPtr<ID3D11Texture2D> shadowTexture;ComPtr<ID3D11ShaderResourceView> shadowView;
    Check(SUCCEEDED(device->CreateTexture2D(&td,&shadowInitial,&shadowTexture))&&SUCCEEDED(device->CreateShaderResourceView(shadowTexture.Get(),nullptr,&shadowView)),"Sharp shadow silhouette fixture");
    auto shadowSrv=shadowView.Get();ctx->PSSetShaderResources(6,1,&shadowSrv);
    auto shadowed=render(shadowProbe.Get());
    Check(shadowed[0]>.01f&&std::abs(shadowed[0]-shadowed[4])<1e-4f,"Rounded receiver normals must not drag world shadow silhouettes sideways");
    auto legacySource=source;const std::string fixedLookup="WorldSunShadowSoft(p+sun*(viewmodel?min(shadowInfo.y,.02):shadowInfo.y),normal,viewmodel)";
    auto lookup=legacySource.find(fixedLookup);Check(lookup!=std::string::npos,"Production sun receiver lookup found");
    legacySource.replace(lookup,fixedLookup.size(),"WorldSunShadowSoft(p+normal*shadowInfo.y+sun*shadowInfo.y,normal,viewmodel)");
    auto legacyProbe=compile(legacySource,"ShadowWarpProbe","ps_5_0");auto warped=render(legacyProbe.Get());
    Check(std::abs(warped[0]-warped[4])>.01f,"Regression fixture detects the former normal-bias warp");
    // Drive Surface itself, not just the lookup helper: identical world/viewmodel
    // receivers must choose different depth domains while preserving map shadows.
    auto layerSource=source+R"(
        float4 ReceiverLayerProbe(float4 p:SV_Position):SV_Target {
            return float4(Surface(float3(.3,.3,.3),float3(-2,0,0),float3(0,0,1),float3(0,0,1),((int)p.x&1)!=0),1);
        })";
    auto layerCode=compile(layerSource,"ReceiverLayerProbe","ps_5_0");
    auto layerDesc=td;layerDesc.ArraySize=4;
    std::vector<float> receiverClear(256*256,1.f);
    D3D11_SUBRESOURCE_DATA receiverInitial[4];
    for(auto &entry:receiverInitial)entry={receiverClear.data(),256*sizeof(float),0};
    ComPtr<ID3D11Texture2D> receiverLayers;ComPtr<ID3D11ShaderResourceView> receiverLayerView;
    Check(SUCCEEDED(device->CreateTexture2D(&layerDesc,receiverInitial,&receiverLayers))&&
          SUCCEEDED(device->CreateShaderResourceView(receiverLayers.Get(),nullptr,&receiverLayerView)),"Viewmodel cascade array fixture");
    auto receiverLayerSrv=receiverLayerView.Get();ctx->PSSetShaderResources(19,1,&receiverLayerSrv);
    shadowField("viewmodelShadowInfo")[0]=1;ctx->UpdateSubresource(shadowBuffer.Get(),0,nullptr,shadowData.data(),0,0);
    auto split=render(layerCode.Get());
    Check(split[0]<.001f&&split[4]>.1f,"Surface excludes world actor silhouette only for the first-person receiver");
    ID3D11ShaderResourceView *emptyLayer=nullptr;ctx->PSSetShaderResources(19,1,&emptyLayer);
    ctx->CopySubresourceRegion(receiverLayers.Get(),0,0,0,0,shadowTexture.Get(),0,nullptr);
    ctx->PSSetShaderResources(19,1,&receiverLayerSrv);auto mapShadow=render(layerCode.Get());
    Check(mapShadow[0]<.001f&&mapShadow[4]<.001f,"World/foliage shadow in receiver layer still darkens the viewmodel");
    shadowField("viewmodelShadowInfo")[0]=0;ctx->UpdateSubresource(shadowBuffer.Get(),0,nullptr,shadowData.data(),0,0);
    auto missingLayer=render(layerCode.Get());
    Check(missingLayer[0]<.001f&&missingLayer[4]>.1f,"Missing first-person layer never falls back to the duplicate actor shadow");
    ctx->PSSetShaderResources(19,1,&emptyLayer);
    // A blocked bridge and close map over a clear near/far map expose accidental skipping
    // of t18: the former path turns fully bright immediately outside the close region.
    auto cascadeSource=source+std::string("\nfloat4 CascadeProbe(float4 position:SV_Position):SV_Target { float x=((int)position.x&1)?224:64; return WorldSunShadowSoft(float3(x,0,0),float3(0,0,1)); }");
    auto cascadeProbe=compile(cascadeSource,"CascadeProbe","ps_5_0");
    auto setRegion=[&](const char* name,float extent){auto m=shadowField(name);std::fill(m,m+16,0.f);m[0]=m[5]=1/extent;m[10]=-.01f;m[11]=.5f;m[15]=1;};
    setRegion("nearSunShadowMatrix",1024);setRegion("detailSunShadowMatrix",256);setRegion("closeSunShadowMatrix",64);
    shadowField("nearSunShadowInfo")[0]=shadowField("detailSunShadowInfo")[0]=shadowField("closeSunShadowInfo")[0]=1;
    ctx->UpdateSubresource(shadowBuffer.Get(),0,nullptr,shadowData.data(),0,0);
    std::fill(shadowPixels.begin(),shadowPixels.end(),.1f);
    ctx->UpdateSubresource(shadowTexture.Get(),0,nullptr,shadowPixels.data(),256*sizeof(float),0);
    ctx->PSSetShaderResources(16,1,&shadowSrv);ctx->PSSetShaderResources(18,1,&shadowSrv);
    ComPtr<ID3D11Texture2D> clearTexture;ComPtr<ID3D11ShaderResourceView> clearView;
    std::fill(shadowPixels.begin(),shadowPixels.end(),1.f);
    Check(SUCCEEDED(device->CreateTexture2D(&td,&shadowInitial,&clearTexture))&&SUCCEEDED(device->CreateShaderResourceView(clearTexture.Get(),nullptr,&clearView)),"Clear wider cascade fixture");
    auto clearSrv=clearView.Get();ctx->PSSetShaderResources(6,1,&clearSrv);ctx->PSSetShaderResources(15,1,&clearSrv);
    // A separate first-person self map must darken the hand/weapon receiver,
    // while the same texel in the world remains completely clear.
    auto selfProbe=compile(source+R"(
        float4 SelfLayerProbe(float4 p:SV_Position):SV_Target {
            return ShadowDepth(int2(64,64),0,true,false,true,false,((int)p.x&1)!=0);
        })","SelfLayerProbe","ps_5_0");
    ctx->PSSetShaderResources(16,1,&clearSrv);ctx->PSSetShaderResources(19,1,&receiverLayerSrv);
    ctx->PSSetShaderResources(21,1,&shadowSrv);
    shadowField("viewmodelShadowInfo")[0]=shadowField("viewmodelShadowInfo")[2]=1;
    ctx->UpdateSubresource(shadowBuffer.Get(),0,nullptr,shadowData.data(),0,0);
    auto selfLayer=render(selfProbe.Get());
    Check(selfLayer[0]>.99f&&std::abs(selfLayer[4]-.1f)<.001f,"Viewmodel self map casts only into its own receiver layer");
    auto nativeSurfaceProbe=compile(source+R"(
        float4 NativeSurfaceProbe(float4 p:SV_Position):SV_Target {
            return float4(Surface(float3(.2,.3,.4),float3(0,0,0),float3(0,0,1),float3(0,0,1),true,1,float3(.8,.4,.2)),1);
        })","NativeSurfaceProbe","ps_5_0");
    c[CL_ambient]=.35f;
    auto nativeShadowed=render(nativeSurfaceProbe.Get());
    Check(std::abs(nativeShadowed[0]-.07f)<.002f&&std::abs(nativeShadowed[1]-.105f)<.002f,
          "Weapon self shadow removes native direct light while preserving ambient material detail");
    shadowField("viewmodelShadowInfo")[2]=0;ctx->UpdateSubresource(shadowBuffer.Get(),0,nullptr,shadowData.data(),0,0);
    auto nativeUnshadowed=render(nativeSurfaceProbe.Get());
    Check(std::abs(nativeUnshadowed[0]-.87f)<.002f&&std::abs(nativeUnshadowed[1]-.505f)<.002f,
          "Native direct highlight survives exactly when the self caster is removed");
    auto nativeWorldProbe=compile(source+R"(
        float4 NativeWorldProbe(float4 p:SV_Position):SV_Target {
            return float4(Surface(float3(.2,.3,.4),float3(0,0,0),float3(0,0,1),float3(0,0,1),false,1,float3(.8,.4,.2)),1);
        })","NativeWorldProbe","ps_5_0");
    ctx->PSSetShaderResources(18,1,&clearSrv);
    auto nativeWorldClear=render(nativeWorldProbe.Get());
    Check(std::abs(nativeWorldClear[0]-.87f)<.002f&&std::abs(nativeWorldClear[1]-.505f)<.002f,
          "Native world material retains ambient color and direct response without albedo reconstruction");
    ctx->PSSetShaderResources(16,1,&shadowSrv);ctx->PSSetShaderResources(18,1,&shadowSrv);
    auto nativeWorldShadow=render(nativeWorldProbe.Get());
    Check(std::abs(nativeWorldShadow[0]-.07f)<.002f&&std::abs(nativeWorldShadow[1]-.105f)<.002f,
          "World shadow removes native direct light while preserving material ambient detail");
    ctx->PSSetShaderResources(16,1,&clearSrv);
    c[CL_ambient]=0;
    auto selfDisabled=render(selfProbe.Get());
    Check(selfDisabled[0]>.99f&&selfDisabled[4]>.99f,"Unavailable self map cannot retain the prior weapon silhouette");
    shadowField("viewmodelShadowInfo")[0]=0;ctx->UpdateSubresource(shadowBuffer.Get(),0,nullptr,shadowData.data(),0,0);
    ctx->PSSetShaderResources(21,1,&emptyLayer);ctx->PSSetShaderResources(19,1,&emptyLayer);
    ctx->PSSetShaderResources(16,1,&shadowSrv);
    auto transition=render(cascadeProbe.Get());
    Check(transition[0]<.001f,"Leaving close region preserves bridge silhouette instead of fading to coarse near map");
    float t=(224.f/256-.75f)/(.98f-.75f), expected=t*t*(3-2*t);
    Check(std::abs(transition[4]-expected)<.001f,"Bridge blends continuously into near region at its own outer boundary");
    shadowField("detailSunShadowInfo")[0]=0;ctx->UpdateSubresource(shadowBuffer.Get(),0,nullptr,shadowData.data(),0,0);
    auto unavailable=render(cascadeProbe.Get());
    Check(unavailable[0]>.999f,"Unavailable bridge safely falls back and fixture detects the old detail cliff");

    // A perspective receiver must reconstruct each depth layer independently.  At one screen
    // coordinate the raised body is closer than the floor, so a directional shadow-map depth
    // step must move with the receiver in light space.  A screen/floor-UV overlay would return
    // the same shadow for both samples even though their world-space z differs.
    auto perspectiveFrame=frameData;
    auto vp=field("viewProjection");std::fill(vp,vp+16,0.f);
    constexpr float perspectiveA=1.01f,perspectiveB=-.1f;
    vp[0]=vp[5]=1.f;vp[10]=perspectiveA;vp[11]=perspectiveB;vp[14]=1.f;
    auto inv=field("inverseViewProjection");std::fill(inv,inv+16,0.f);
    inv[0]=inv[5]=perspectiveB;inv[11]=perspectiveB;inv[14]=1.f;inv[15]=-perspectiveA;
    field("camera")[0]=field("camera")[1]=field("camera")[2]=0.f;
    field("viewmodelInfo")[0]=0.f;
    auto baseSun=shadowField("sunShadowMatrix");std::fill(baseSun,baseSun+16,0.f);
    baseSun[0]=baseSun[5]=.1f;baseSun[10]=.1f;baseSun[15]=1.f;
    shadowField("shadowInfo")[0]=1.f;shadowField("shadowInfo")[1]=0.f;shadowField("shadowInfo")[3]=256.f;
    shadowField("nearSunShadowInfo")[0]=shadowField("detailSunShadowInfo")[0]=shadowField("closeSunShadowInfo")[0]=0.f;
    ctx->UpdateSubresource(shadowBuffer.Get(),0,nullptr,shadowData.data(),0,0);
    std::fill(shadowPixels.begin(),shadowPixels.end(),.4f);
    ctx->UpdateSubresource(shadowTexture.Get(),0,nullptr,shadowPixels.data(),256*sizeof(float),0);
    // The cascade fixture above intentionally leaves its all-lit clear SRV bound at t6.
    // Restore the directional blocker texture before checking the perspective receiver.
    ctx->PSSetShaderResources(6,1,&shadowSrv);
    auto perspectiveSource=source+R"(
        float4 RaisedReceiverProbe(float4 p:SV_Position):SV_Target {
            float raw=ReadRawSampleDepth(p.xy,0),depth=ReadSampleDepth(p.xy,0);
            float3 position=ReceiverPosition(p.xy,depth,0);
            return float4(position.z,WorldSunShadowSoft(position,float3(0,0,1)),position.x,raw);
        }
    )";
    auto perspectiveCode=compile(perspectiveSource,"RaisedReceiverProbe","ps_5_0");
    D3D11_TEXTURE2D_DESC receiverDepthDesc{};receiverDepthDesc.Width=receiverDepthDesc.Height=256;
    receiverDepthDesc.MipLevels=receiverDepthDesc.ArraySize=1;receiverDepthDesc.SampleDesc.Count=1;
    receiverDepthDesc.Format=DXGI_FORMAT_R32_TYPELESS;receiverDepthDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> receiverDepth;ComPtr<ID3D11DepthStencilView> receiverDsv;ComPtr<ID3D11ShaderResourceView> receiverSrv;
    Check(SUCCEEDED(device->CreateTexture2D(&receiverDepthDesc,nullptr,&receiverDepth)),"Perspective receiver depth");
    D3D11_DEPTH_STENCIL_VIEW_DESC receiverDsvDesc{};receiverDsvDesc.Format=DXGI_FORMAT_D32_FLOAT;receiverDsvDesc.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC receiverSrvDesc{};receiverSrvDesc.Format=DXGI_FORMAT_R32_FLOAT;receiverSrvDesc.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;receiverSrvDesc.Texture2D.MipLevels=1;
    Check(SUCCEEDED(device->CreateDepthStencilView(receiverDepth.Get(),&receiverDsvDesc,&receiverDsv))&&
          SUCCEEDED(device->CreateShaderResourceView(receiverDepth.Get(),&receiverSrvDesc,&receiverSrv)),"Perspective receiver depth views");
    auto receiverDepthSample=[&](float worldZ){return .95f*(perspectiveA+perspectiveB/worldZ);};
    auto receiverSrvRaw=receiverSrv.Get();ID3D11ShaderResourceView *noReceiverDepth=nullptr;
    ctx->PSSetShaderResources(1,1,&noReceiverDepth);
    ctx->OMSetRenderTargets(0,nullptr,receiverDsv.Get());ctx->ClearDepthStencilView(receiverDsv.Get(),D3D11_CLEAR_DEPTH,receiverDepthSample(3.f),0);
    ctx->OMSetRenderTargets(0,nullptr,nullptr);ctx->PSSetShaderResources(1,1,&receiverSrvRaw);auto body=render(perspectiveCode.Get(),140,128);
    ctx->PSSetShaderResources(1,1,&noReceiverDepth);
    ctx->OMSetRenderTargets(0,nullptr,receiverDsv.Get());ctx->ClearDepthStencilView(receiverDsv.Get(),D3D11_CLEAR_DEPTH,receiverDepthSample(6.f),0);
    ctx->OMSetRenderTargets(0,nullptr,nullptr);ctx->PSSetShaderResources(1,1,&receiverSrvRaw);auto floor=render(perspectiveCode.Get(),140,128);
    Check(std::abs(body[0]-3.f)<.02f&&std::abs(floor[0]-6.f)<.02f&&body[0]<floor[0]-.5f,
          "Perspective receiver reconstructs raised body and floor at distinct world depths");
    Check(body[1]>.95f&&floor[1]<.05f,
          "Directional shadow cross-section follows raised receiver instead of reusing floor screen coordinates");
    frameData=perspectiveFrame;
    std::cout<<"Native receiver snapshots, world/hand separation, MSAA, wet triangle invariance, shadow warp and cascade transitions passed\n";
}
