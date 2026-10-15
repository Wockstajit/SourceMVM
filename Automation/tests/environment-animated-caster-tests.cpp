#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentAnimatedCasters.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderCompiler.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderCoverageMath.h"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <array>
#include <cstring>
#include <iostream>
#include <cmath>
#include <thread>
#include <vector>
#include <fstream>
using Microsoft::WRL::ComPtr;
using namespace Filmmaker::Environment;
namespace Filmmaker {bool MvmDebugLog_Active(){return false;}void MvmDebugLog_LinefAlways(const char*,const char*,...){} }
void Check(bool condition,const char* message){if(!condition){std::cerr<<message<<'\n';std::exit(1);}}
int main(int argc,char** argv){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)),"device");
    auto compile=[&](const char* source,const char* target){ComPtr<ID3DBlob> code,error;
        auto hr=CompileEnvironmentShader(source,strlen(source),"AnimatedCasterTest",nullptr,"Main",target,D3DCOMPILE_ENABLE_STRICTNESS,&code,&error);
        if(error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());Check(SUCCEEDED(hr),"compile");return code;};
    auto code=compile(R"(
        cbuffer PerViewConstantBuffer_t:register(b3){
            column_major float4x4 projection:packoffset(c0);
            column_major float4x4 g_matProjectionToWorld:packoffset(c4);
            float4 g_vWorldToCameraOffset:packoffset(c24);
            float4 pose:packoffset(c25);
        }
        // CS2's character path keeps near-camera leg/occlusion controls in its
        // top-level $Globals buffer. The capture replay must neutralize these
        // only in its private constant-buffer copy; the native draw's buffer
        // and all caller state must be restored afterwards.
        float g_flPlayerOcclusionAmount;
        float g_flWeaponOcclusionAmount;
        float g_flFirstpersonLegsFade;
        bool g_bDisableFirstpersonLegsDistortion;
        struct NativeOutput {
            float3 a:TEXCOORD13;float3 b:TEXCOORD0;float3 c:TEXCOORD1;
            float4 d:TEXCOORD2;float4 e:TEXCOORD4;float3 f:TEXCOORD5;
            float4 g:TEXCOORD6;uint h:TEXCOORD12;float4 position:SV_Position;
        };
        NativeOutput Main(uint id:SV_VertexID) {
            float2 v[3]={float2(-.1,-.1),float2(0,.1),float2(.1,-.1)};
            NativeOutput o=(NativeOutput)0;
            // Model the native forward path: posed world coordinates are made
            // camera-relative before a perspective projection. The capture GS
            // receives the native inverse matrix, which already restores the
            // world origin and therefore must not apply the offset a second time.
            float3 world=float3(v[id]+pose.xy,.5);
            float3 camera=world+g_vWorldToCameraOffset.xyz;
            float4 clip=mul(projection,float4(camera,1));
            if(!g_bDisableFirstpersonLegsDistortion){
                // Model the native near-camera path's post-projection warp:
                // the replay must disable these globals before the VS emits
                // SV_Position, otherwise inverse reconstruction is not a
                // world-space pose anymore.
                clip.x+=(g_flPlayerOcclusionAmount+g_flWeaponOcclusionAmount)*.01;
                clip.y+=g_flFirstpersonLegsFade*clip.x*.01;
                clip.z+=g_flPlayerOcclusionAmount*clip.y*.01;
            }
            o.e.w=pose.z;
            o.position=clip;
            return o;
        })","vs_5_0");
    ComPtr<ID3D11VertexShader> native;
    Check(SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&native)),"native VS");
    const char* name="csgo_character.vfx_vs";native->SetPrivateData(WKPDID_D3DDebugObjectName,UINT(strlen(name)),name);
    NoteAnimatedCasterShader(native.Get(),code->GetBufferPointer(),code->GetBufferSize());
    ComPtr<ID3D11VertexShader> nativeWeapon;
    Check(SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&nativeWeapon)),"native weapon VS");
    const char* weaponName="csgo_weapon.vfx_vs";
    nativeWeapon->SetPrivateData(WKPDID_D3DDebugObjectName,UINT(strlen(weaponName)),weaponName);
    NoteAnimatedCasterShader(nativeWeapon.Get(),code->GetBufferPointer(),code->GetBufferSize());
    // Native shader creation runs on multiple workers and can revisit cached
    // signatures after a level change. Stress that cache without sharing any
    // live compiler blob or reflection object between calls.
    std::vector<std::thread> creationWorkers;
    for(unsigned worker=0;worker<4;++worker)creationWorkers.emplace_back([&]{
        for(unsigned n=0;n<64;++n){ComPtr<ID3D11VertexShader> repeated;
            Check(SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&repeated)),"concurrent native VS");
            NoteAnimatedCasterShader(repeated.Get(),code->GetBufferPointer(),code->GetBufferSize());
        }
    });
    for(auto& worker:creationWorkers)worker.join();
    std::array<float,108> constants{};
    std::array<float,4> globals{{3.0f,4.0f,2.0f,0.0f}};
    auto setCamera=[&](float x,float y,float z){
        constants.fill(0);
        // Perspective projection: clip.z=.5*camera.z+2 and clip.w=camera.z.
        constants[0]=constants[5]=1;constants[10]=.5f;constants[11]=1;constants[14]=2;
        // Inverse projection plus T(-cameraOffset). This is the same kind of
        // camera-relative inverse supplied by the native PerView buffer.
        constants[16]=constants[21]=1;
        constants[24]=-.5f*x;constants[25]=-.5f*y;constants[26]=-.5f*z;constants[27]=.5f;
        constants[28]=.25f*x;constants[29]=.25f*y;constants[30]=1+.25f*z;constants[31]=-.25f;
        constants[96]=x;constants[97]=y;constants[98]=z;
    };
    setCamera(10.0f,-20.0f,30.0f);
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(constants);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cb;Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&cb)),"constants");
    D3D11_BUFFER_DESC globalsDesc{};globalsDesc.ByteWidth=sizeof(globals);globalsDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> globalsCb;Check(SUCCEEDED(device->CreateBuffer(&globalsDesc,nullptr,&globalsCb)),"native globals");
    D3D11_BUFFER_DESC globalsReadDesc=globalsDesc;globalsReadDesc.BindFlags=0;globalsReadDesc.Usage=D3D11_USAGE_STAGING;globalsReadDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> globalsReadback;Check(SUCCEEDED(device->CreateBuffer(&globalsReadDesc,nullptr,&globalsReadback)),"native globals readback");
    auto csCode=compile("[numthreads(1,1,1)] void Main(uint3 id:SV_DispatchThreadID){}","cs_5_0");
    ComPtr<ID3D11ComputeShader> sentinelCs;Check(SUCCEEDED(device->CreateComputeShader(csCode->GetBufferPointer(),csCode->GetBufferSize(),nullptr,&sentinelCs)),"sentinel CS");
    D3D11_BUFFER_DESC uavBufferDesc{};uavBufferDesc.ByteWidth=16;uavBufferDesc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;uavBufferDesc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;uavBufferDesc.StructureByteStride=4;
    ComPtr<ID3D11Buffer> sentinelUavBuffer;Check(SUCCEEDED(device->CreateBuffer(&uavBufferDesc,nullptr,&sentinelUavBuffer)),"sentinel UAV buffer");
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};uavDesc.Format=DXGI_FORMAT_UNKNOWN;uavDesc.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;uavDesc.Buffer.NumElements=4;
    ComPtr<ID3D11UnorderedAccessView> sentinelUav;Check(SUCCEEDED(device->CreateUnorderedAccessView(sentinelUavBuffer.Get(),&uavDesc,&sentinelUav)),"sentinel UAV");
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=512;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target,depth;ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11DepthStencilView> dsv;
    Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&target))&&SUCCEEDED(device->CreateRenderTargetView(target.Get(),nullptr,&rtv)),"target");
    td.Format=DXGI_FORMAT_D32_FLOAT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&depth))&&SUCCEEDED(device->CreateDepthStencilView(depth.Get(),nullptr,&dsv)),"depth");
    BeginAnimatedCasters(device.Get());RequestAnimatedCasters(true);
    auto bindNative=[&](float depthMax,ID3D11VertexShader* shader=nullptr){ctx->ClearState();auto r=rtv.Get();ctx->OMSetRenderTargets(1,&r,dsv.Get());
        D3D11_VIEWPORT vp{0,0,512,512,0,depthMax};ctx->RSSetViewports(1,&vp);ctx->VSSetShader(shader?shader:native.Get(),nullptr,0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);auto b=cb.Get();ctx->VSSetConstantBuffers(3,1,&b);
        // A pre-existing GS constant binding must survive the capture bracket;
        // the top-level native globals likewise must be restored after replay.
        ctx->GSSetConstantBuffers(0,1,&b);ctx->UpdateSubresource(b,0,nullptr,constants.data(),0,0);
        auto g=globalsCb.Get();ctx->VSSetConstantBuffers(0,1,&g);ctx->UpdateSubresource(globalsCb.Get(),0,nullptr,globals.data(),0,0);};
    bindNative(.95f);Check(!BeginAnimatedCasterDraw(ctx.Get(),2,1),"reject incomplete triangle");
    auto sentinelCsRaw=sentinelCs.Get();ctx->CSSetShader(sentinelCsRaw,nullptr,0);
    UINT initialCounts=0;auto sentinelUavRaw=sentinelUav.Get();ctx->CSSetUnorderedAccessViews(0,1,&sentinelUavRaw,&initialCounts);
    Check(!BeginAnimatedCasterDraw(ctx.Get(),3,0xffffffffu),"bound capacity without integer overflow");
    Check(BeginAnimatedCasterDraw(ctx.Get(),3,1),"capture native pose");ctx->Draw(3,0);EndAnimatedCasterDraw(ctx.Get());
    Check(AnimatedCasterVertexCount()==3,"one triangle captured");
    ComPtr<ID3D11Buffer> restoredGlobals;ctx->VSGetConstantBuffers(0,1,&restoredGlobals);
    Check(restoredGlobals.Get()==globalsCb.Get(),"restore native globals after private replay");
    ComPtr<ID3D11ComputeShader> restoredCs;ctx->CSGetShader(&restoredCs,nullptr,nullptr);
    Check(restoredCs.Get()==sentinelCs.Get(),"restore CS shader after private replay");
    ID3D11UnorderedAccessView* restoredUavRaw=nullptr;
    ctx->CSGetUnorderedAccessViews(0,1,&restoredUavRaw);
    Check(restoredUavRaw==sentinelUav.Get(),"restore CS UAV0 after private replay");
    if(restoredUavRaw)restoredUavRaw->Release();
    ctx->CopyResource(globalsReadback.Get(),globalsCb.Get());D3D11_MAPPED_SUBRESOURCE globalsMapped{};
    Check(SUCCEEDED(ctx->Map(globalsReadback.Get(),0,D3D11_MAP_READ,0,&globalsMapped)),"map native globals readback");
    Check(std::memcmp(globalsMapped.pData,globals.data(),sizeof(globals))==0,"preserve native globals contents after private replay");
    ctx->Unmap(globalsReadback.Get(),0);
    ComPtr<ID3D11GeometryShader> gs;ctx->GSGetShader(&gs,nullptr,nullptr);Check(!gs,"restore GS");
    ComPtr<ID3D11Buffer> restored;ctx->GSGetConstantBuffers(0,1,&restored);Check(restored.Get()==cb.Get(),"restore GS constants");
    // Stream the captured positions through a second, identity SO pass to read
    // them back. This verifies the production capture math, not a CPU imitation.
    auto passthrough=compile("float4 Main(float3 p:POSITION):SV_Position{return float4(p,1);}","vs_5_0");
    ComPtr<ID3D11VertexShader> readVs;ComPtr<ID3D11InputLayout> layout;
    D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    Check(SUCCEEDED(device->CreateVertexShader(passthrough->GetBufferPointer(),passthrough->GetBufferSize(),nullptr,&readVs))&&
          SUCCEEDED(device->CreateInputLayout(&element,1,passthrough->GetBufferPointer(),passthrough->GetBufferSize(),&layout)),"read VS");
    D3D11_SO_DECLARATION_ENTRY so{0,"SV_Position",0,0,4,0};UINT stride=16;
    ComPtr<ID3D11GeometryShader> readGs;
    Check(SUCCEEDED(device->CreateGeometryShaderWithStreamOutput(passthrough->GetBufferPointer(),passthrough->GetBufferSize(),&so,1,&stride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&readGs)),"read stream");
    bd={};bd.ByteWidth=48;bd.BindFlags=D3D11_BIND_STREAM_OUTPUT;ComPtr<ID3D11Buffer> output,staging;
    Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&output)),"output");
    bd.BindFlags=0;bd.Usage=D3D11_USAGE_STAGING;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&staging)),"readback");
    auto inspect=[&](float expected,AnimatedCasterSubset subset=AnimatedCasterSubset::All,float expectedY=-.1f,float expectedZ=.5f){ctx->ClearState();ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(layout.Get());ctx->VSSetShader(readVs.Get(),nullptr,0);ctx->GSSetShader(readGs.Get(),nullptr,0);
        auto out=output.Get();UINT zero=0;ctx->SOSetTargets(1,&out,&zero);DrawAnimatedCasters(ctx.Get(),subset);ctx->SOSetTargets(0,nullptr,nullptr);
        ctx->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"map");auto p=static_cast<const float*>(mapped.pData);
        Check(std::abs(p[0]-expected)<1e-5f&&std::abs(p[1]-expectedY)<1e-5f&&std::abs(p[2]-expectedZ)<1e-5f,"posed world position removes camera-relative offset");ctx->Unmap(staging.Get(),0);};
    inspect(-.1f);
    constants[100]=.5f;bindNative(.95f,nativeWeapon.Get());
    Check(BeginAnimatedCasterDraw(ctx.Get(),3,1),"capture weapon pose separately");ctx->Draw(3,0);EndAnimatedCasterDraw(ctx.Get());
    Check(AnimatedCasterVertexCount()==6&&
          AnimatedCasterVertexCount(AnimatedCasterSubset::Characters)==3&&
          AnimatedCasterVertexCount(AnimatedCasterSubset::Weapons)==3,
          "character and weapon capture counts remain independently selectable");
    inspect(-.1f,AnimatedCasterSubset::Characters);
    inspect(.4f,AnimatedCasterSubset::Weapons);
    auto generation=AnimatedCasterGeneration();RequestAnimatedCasters(true);NextAnimatedCasterFrame();
    // Passes that run before this frame's character draws read the last complete frame instead of
    // nothing; one frame later, with nothing captured, the stale pose is gone.
    Check(AnimatedCasterVertexCount(AnimatedCasterSubset::Actors)==6&&AnimatedCasterGeneration()!=generation,"last complete actor pose stays available for one frame");
    RequestAnimatedCasters(true);NextAnimatedCasterFrame();Check(AnimatedCasterVertexCount()==0,"an empty frame after that removes the stale pose");
    // A different camera translation must reconstruct the same world-space
    // vertices. This catches reapplying g_vWorldToCameraOffset in the capture GS.
    setCamera(-40.0f,12.0f,50.0f);bindNative(.95f);
    Check(BeginAnimatedCasterDraw(ctx.Get(),3,1),"capture translated camera pose");ctx->Draw(3,0);EndAnimatedCasterDraw(ctx.Get());inspect(-.1f);
    NextAnimatedCasterFrame();RequestAnimatedCasters(true);
    constants[100]=.5f;bindNative(.95f);
    Check(BeginAnimatedCasterDraw(ctx.Get(),3,1),"capture changed animation");ctx->Draw(3,0);EndAnimatedCasterDraw(ctx.Get());inspect(.4f);
    NextAnimatedCasterFrame();
    // The dedicated first-person stream uses the rendered projection, anchors
    // relative to its own eye, and cannot enter world caster counts/draws.
    const UINT worldBefore=AnimatedCasterVertexCount();
    setCamera(10,-20,30);globals={0,0,0,1};bindNative(.1f,nativeWeapon.Get());
    Check(BeginAnimatedCasterDraw(ctx.Get(),3,1),"capture actual first-person weapon");ctx->Draw(3,0);EndAnimatedCasterDraw(ctx.Get());
    Check(AnimatedCasterVertexCount()==worldBefore&&AnimatedCasterVertexCount(AnimatedCasterSubset::Viewmodel)==3,"first-person stream isolated from world");
    inspect(9.9f,AnimatedCasterSubset::Viewmodel,-20.1f,30.5f);
    // Like actors, the last first-person pose stays for one frame: the first-person shadow pass runs
    // before that frame's viewmodel draws. An empty frame after that discards it.
    NextAnimatedCasterFrame();Check(AnimatedCasterVertexCount(AnimatedCasterSubset::Viewmodel)==3,"last first-person pose stays for the next shadow pass");
    NextAnimatedCasterFrame();Check(AnimatedCasterVertexCount(AnimatedCasterSubset::Viewmodel)==0,"discard previous viewmodel pose");
    // Static and physics props/cables use their native VS pose but remain outside
    // the actor layer, so they can occlude first-person hands too.
    ComPtr<ID3D11VertexShader> nativeProp;
    Check(SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&nativeProp)),"prop VS");
    const char* propName="csgo_vertexlitgeneric.vfx_vs";
    nativeProp->SetPrivateData(WKPDID_D3DDebugObjectName,UINT(strlen(propName)),propName);
    NoteAnimatedCasterShader(nativeProp.Get(),code->GetBufferPointer(),code->GetBufferSize());
    auto opaqueCode=compile("float4 Main():SV_Target{return 1;}","ps_5_0");
    auto cutoutCode=compile("float4 Main(float4 p:SV_Position):SV_Target{clip(p.x-20);return 1;}","ps_5_0");
    Check(OpaquePixelShaderCoverage(opaqueCode->GetBufferPointer(),opaqueCode->GetBufferSize()),"decode opaque DXBC");
    Check(!OpaquePixelShaderCoverage(cutoutCode->GetBufferPointer(),cutoutCode->GetBufferSize()),"decode discard DXBC");
    bool alphaOne=false;
    Check(OpaquePixelShaderCoverage(opaqueCode->GetBufferPointer(),opaqueCode->GetBufferSize(),nullptr,&alphaOne)&&alphaOne,"prove literal alpha one");
    auto translucentCode=compile("float4 Main():SV_Target{return float4(1,1,1,.5);}","ps_5_0");
    Check(OpaquePixelShaderCoverage(translucentCode->GetBufferPointer(),translucentCode->GetBufferSize(),nullptr,&alphaOne)&&!alphaOne,"half alpha is not opaque under blending");
    auto variableCode=compile("float4 Main(float4 p:SV_Position):SV_Target{return float4(1,1,1,saturate(p.x/100));}","ps_5_0");
    Check(OpaquePixelShaderCoverage(variableCode->GetBufferPointer(),variableCode->GetBufferSize(),nullptr,&alphaOne)&&!alphaOne,"variable alpha is not proven opaque");
    auto branchCode=compile("float4 Main(float4 p:SV_Position):SV_Target{[branch]if(p.x<20)return float4(1,1,1,.5);return 1;}","ps_5_0");
    Check(OpaquePixelShaderCoverage(branchCode->GetBufferPointer(),branchCode->GetBufferSize(),nullptr,&alphaOne)&&!alphaOne,"final alpha one cannot hide an earlier transparent return");

    for(size_t n=0;n<opaqueCode->GetBufferSize();++n)
        Check(!OpaquePixelShaderCoverage(opaqueCode->GetBufferPointer(),n),"reject every truncated DXBC prefix");
    auto coverageCode=compile("uint Main():SV_Coverage{return 1;}","ps_5_0");
    Check(!OpaquePixelShaderCoverage(coverageCode->GetBufferPointer(),coverageCode->GetBufferSize()),"reject coverage-mask output");
    ComPtr<ID3D11PixelShader> opaquePs,cutoutPs;
    Check(SUCCEEDED(device->CreatePixelShader(opaqueCode->GetBufferPointer(),opaqueCode->GetBufferSize(),nullptr,&opaquePs)),"opaque PS");
    Check(SUCCEEDED(device->CreatePixelShader(cutoutCode->GetBufferPointer(),cutoutCode->GetBufferSize(),nullptr,&cutoutPs)),"cutout PS");
    NoteAnimatedCasterPixelShader(opaquePs.Get(),opaqueCode->GetBufferPointer(),opaqueCode->GetBufferSize());
    NoteAnimatedCasterPixelShader(cutoutPs.Get(),cutoutCode->GetBufferPointer(),cutoutCode->GetBufferSize());
    RequestAnimatedCasters(true);setCamera(0,0,0);bindNative(.95f,nativeProp.Get());
    Check(!BeginAnimatedCasterDraw(ctx.Get(),3,1),"unknown pixel coverage cannot become solid shadow");
    ctx->PSSetShader(cutoutPs.Get(),nullptr,0);
    Check(!BeginAnimatedCasterDraw(ctx.Get(),3,1),"cutout pixel shader cannot become solid shadow");
    ctx->PSSetShader(opaquePs.Get(),nullptr,0);
    Check(BeginAnimatedCasterDraw(ctx.Get(),3,1),"capture opaque prop");ctx->Draw(3,0);EndAnimatedCasterDraw(ctx.Get());
    Check(AnimatedCasterVertexCount()==3&&AnimatedCasterVertexCount(AnimatedCasterSubset::Scene)==3&&AnimatedCasterVertexCount(AnimatedCasterSubset::Characters)==0,"scene caster independent of actors");
    inspect(-.1f,AnimatedCasterSubset::Scene);
    NextAnimatedCasterFrame();RequestAnimatedCasters(true);
    Check(AnimatedCasterVertexCount(AnimatedCasterSubset::Scene)==3,"completed prop pose survives until early shadow pass");
    inspect(-.1f,AnimatedCasterSubset::Scene);
    constants[100]=.5f;bindNative(.95f,nativeProp.Get());ctx->PSSetShader(opaquePs.Get(),nullptr,0);
    Check(BeginAnimatedCasterDraw(ctx.Get(),3,1),"capture next prop animation pose");
    ctx->Draw(3,0);EndAnimatedCasterDraw(ctx.Get());
    inspect(.4f,AnimatedCasterSubset::Scene);
    NextAnimatedCasterFrame();RequestAnimatedCasters(true);
    inspect(.4f,AnimatedCasterSubset::Scene);
    NextAnimatedCasterFrame();RequestAnimatedCasters(true);
    Check(AnimatedCasterVertexCount(AnimatedCasterSubset::Scene)==0,"empty frame expires prop pose instead of leaving a ghost shadow");
    constants[100]=0;
    // CS2 scene assets can use the same static-material programs as map geometry.
    // They are absent from the extracted map and must enter live shadow capture.
    for(const char* name:{"csgo_environment.vfx_vs","csgo_environment_blend.vfx_vs","csgo_simple_3layer_parallax.vfx_vs"}) {
        nativeProp->SetPrivateData(WKPDID_D3DDebugObjectName,UINT(std::strlen(name)),name);
        bindNative(.95f,nativeProp.Get());ctx->PSSetShader(cutoutPs.Get(),nullptr,0);
        Check(!BeginAnimatedCasterDraw(ctx.Get(),3,1),"Static-material cutout still needs verified opacity");
        ctx->PSSetShader(opaquePs.Get(),nullptr,0);
        Check(BeginAnimatedCasterDraw(ctx.Get(),3,1),"Capture placed static-material asset");
        ctx->Draw(3,0);EndAnimatedCasterDraw(ctx.Get());inspect(-.1f,AnimatedCasterSubset::Scene);
        NextAnimatedCasterFrame();RequestAnimatedCasters(true);
        NextAnimatedCasterFrame();RequestAnimatedCasters(true);
        Check(AnimatedCasterVertexCount(AnimatedCasterSubset::Scene)==0,"Removed static-material prop leaves no shadow");
    }
    // An early light pass must not replace yesterday's complete scene with the
    // first opaque draw of this frame. The overlay still needs the current pose.
    constants[100]=0;bindNative(.95f,nativeProp.Get());ctx->PSSetShader(opaquePs.Get(),nullptr,0);
    Check(BeginAnimatedCasterDraw(ctx.Get(),3,1),"Capture complete prior scene");
    ctx->Draw(3,0);EndAnimatedCasterDraw(ctx.Get());NextAnimatedCasterFrame();RequestAnimatedCasters(true);
    constants[100]=.5f;bindNative(.95f,nativeProp.Get());ctx->PSSetShader(opaquePs.Get(),nullptr,0);
    Check(BeginAnimatedCasterDraw(ctx.Get(),3,1),"Capture first draw of next scene");
    ctx->Draw(3,0);EndAnimatedCasterDraw(ctx.Get());
    { CompletedSceneCasterScope completed;inspect(-.1f,AnimatedCasterSubset::Scene); }
    inspect(.4f,AnimatedCasterSubset::Scene);
    NextAnimatedCasterFrame();RequestAnimatedCasters(true);
    { CompletedSceneCasterScope completed;inspect(.4f,AnimatedCasterSubset::Scene); }
    NextAnimatedCasterFrame();RequestAnimatedCasters(true);
    { CompletedSceneCasterScope completed;Check(!AnimatedCasterVertexCount(AnimatedCasterSubset::Scene),"Removed prop expires from the completed scene"); }
    constants[100]=0;
    // Complex materials also appear on moving physics props. Their shader name
    // alone must never bypass the pixel coverage proof.
    NextAnimatedCasterFrame();RequestAnimatedCasters(true);
    const char* complexName="csgo_complex.vfx_vs";
    nativeProp->SetPrivateData(WKPDID_D3DDebugObjectName,UINT(strlen(complexName)),complexName);
    bindNative(.95f,nativeProp.Get());ctx->PSSetShader(cutoutPs.Get(),nullptr,0);
    Check(!BeginAnimatedCasterDraw(ctx.Get(),3,1),"complex cutout cannot become solid shadow");
    ctx->PSSetShader(opaquePs.Get(),nullptr,0);
    Check(BeginAnimatedCasterDraw(ctx.Get(),3,1),"capture opaque complex physics prop");
    ctx->Draw(3,0);EndAnimatedCasterDraw(ctx.Get());
    Check(AnimatedCasterVertexCount(AnimatedCasterSubset::Scene)==3&&AnimatedCasterVertexCount(AnimatedCasterSubset::Characters)==0,"complex prop remains in scene layer");
    inspect(-.1f,AnimatedCasterSubset::Scene);
    NextAnimatedCasterFrame();RequestAnimatedCasters(true);constants[100]=.5f;
    const char* cableName="cables.vfx_vs";nativeProp->SetPrivateData(WKPDID_D3DDebugObjectName,UINT(strlen(cableName)),cableName);
    bindNative(.95f,nativeProp.Get());ctx->PSSetShader(opaquePs.Get(),nullptr,0);
    Check(BeginAnimatedCasterDraw(ctx.Get(),3,1),"capture native cable pose");ctx->Draw(3,0);EndAnimatedCasterDraw(ctx.Get());
    inspect(.4f,AnimatedCasterSubset::Scene);

    ComPtr<ID3D11PixelShader> translucentPs;
    Check(SUCCEEDED(device->CreatePixelShader(translucentCode->GetBufferPointer(),translucentCode->GetBufferSize(),nullptr,&translucentPs)),"translucent PS");
    NoteAnimatedCasterPixelShader(translucentPs.Get(),translucentCode->GetBufferPointer(),translucentCode->GetBufferSize());
    D3D11_BLEND_DESC blendDesc{};auto &blendTarget=blendDesc.RenderTarget[0];
    blendTarget.BlendEnable=true;blendTarget.SrcBlend=D3D11_BLEND_SRC_ALPHA;blendTarget.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;
    blendTarget.BlendOp=blendTarget.BlendOpAlpha=D3D11_BLEND_OP_ADD;blendTarget.SrcBlendAlpha=D3D11_BLEND_ONE;
    blendTarget.DestBlendAlpha=D3D11_BLEND_ZERO;blendTarget.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> alphaBlend,additiveBlend,coverageBlend;
    Check(SUCCEEDED(device->CreateBlendState(&blendDesc,&alphaBlend)),"native cable blend state");
    blendTarget.SrcBlend=blendTarget.DestBlend=D3D11_BLEND_ONE;
    Check(SUCCEEDED(device->CreateBlendState(&blendDesc,&additiveBlend)),"additive fixture state");
    blendTarget.BlendEnable=false;blendDesc.AlphaToCoverageEnable=true;
    Check(SUCCEEDED(device->CreateBlendState(&blendDesc,&coverageBlend)),"alpha-to-coverage fixture state");
    nativeProp->SetPrivateData(WKPDID_D3DDebugObjectName,UINT(strlen(complexName)),complexName);
    bindNative(.95f,nativeProp.Get());ctx->PSSetShader(opaquePs.Get(),nullptr,0);
    ctx->OMSetBlendState(coverageBlend.Get(),nullptr,~0u);
    Check(!BeginAnimatedCasterDraw(ctx.Get(),3,1),"complex color alpha one does not prove depth-prepass cutout coverage");
    nativeProp->SetPrivateData(WKPDID_D3DDebugObjectName,UINT(strlen(cableName)),cableName);
    for(auto blendState:{alphaBlend.Get(),coverageBlend.Get()}){
        NextAnimatedCasterFrame();RequestAnimatedCasters(true);bindNative(.95f,nativeProp.Get());
        ctx->OMSetBlendState(blendState,nullptr,0xffffffffu);ctx->PSSetShader(translucentPs.Get(),nullptr,0);
        Check(!BeginAnimatedCasterDraw(ctx.Get(),3,1),"transparent cable/coverage draw cannot become a solid blocker");
        ctx->PSSetShader(opaquePs.Get(),nullptr,0);
        Check(BeginAnimatedCasterDraw(ctx.Get(),3,1),"constant-alpha cable/coverage draw contributes geometry");
        ctx->Draw(3,0);EndAnimatedCasterDraw(ctx.Get());
        Check(AnimatedCasterVertexCount(AnimatedCasterSubset::Scene)==3,"proven opaque blended caster recorded");
    }
    ctx->OMSetBlendState(additiveBlend.Get(),nullptr,0xffffffffu);
    Check(!BeginAnimatedCasterDraw(ctx.Get(),3,1),"constant alpha does not make additive material an opaque blocker");
    ctx->OMSetBlendState(nullptr,nullptr,0xffffffffu);
    bindNative(.1f,nativeProp.Get());ctx->PSSetShader(opaquePs.Get(),nullptr,0);
    Check(!BeginAnimatedCasterDraw(ctx.Get(),3,1),"scene geometry cannot enter the viewmodel layer");
    // A cable PS that forwards reflected vertex alpha uses a dedicated stream
    // and coverage shader. Confirm actual GPU depth coverage, including fades,
    // rather than only asserting that a draw passed the gate.
    auto fadeCode=compile("float4 Main(float4 fade:TEXCOORD4):SV_Target{return float4(.2,.3,.4,fade.w);}","ps_5_0");
    UINT alphaInput=~0u;
    Check(OpaquePixelShaderCoverage(fadeCode->GetBufferPointer(),fadeCode->GetBufferSize(),nullptr,nullptr,&alphaInput)&&alphaInput!=~0u,"prove complete vertex opacity");
    auto mixedCode=compile("float4 Main(float4 fade:TEXCOORD4):SV_Target{return float4(.2,.3,.4,fade.w*.5);}","ps_5_0");
    Check(OpaquePixelShaderCoverage(mixedCode->GetBufferPointer(),mixedCode->GetBufferSize(),nullptr,nullptr,&alphaInput)&&alphaInput==~0u,"scaled alpha cannot masquerade as unchanged vertex opacity");
    auto earlyCode=compile("float4 Main(float4 fade:TEXCOORD4):SV_Target{[branch]if(fade.x<.2)return 0;return fade;}","ps_5_0");
    Check(OpaquePixelShaderCoverage(earlyCode->GetBufferPointer(),earlyCode->GetBufferSize(),nullptr,nullptr,&alphaInput)&&alphaInput==~0u,"conditional opacity cannot masquerade as terminal vertex opacity");
    ComPtr<ID3D11PixelShader> fadePs;
    Check(SUCCEEDED(device->CreatePixelShader(fadeCode->GetBufferPointer(),fadeCode->GetBufferSize(),nullptr,&fadePs)),"fade PS");
    NoteAnimatedCasterPixelShader(fadePs.Get(),fadeCode->GetBufferPointer(),fadeCode->GetBufferSize());
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> depthReadback;Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&depthReadback)),"coverage depth readback");
    D3D11_DEPTH_STENCIL_DESC depthStateDesc{};depthStateDesc.DepthEnable=true;depthStateDesc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;depthStateDesc.DepthFunc=D3D11_COMPARISON_LESS;
    ComPtr<ID3D11DepthStencilState> depthState;Check(SUCCEEDED(device->CreateDepthStencilState(&depthStateDesc,&depthState)),"coverage depth state");
    D3D11_RASTERIZER_DESC rasterDesc{};rasterDesc.FillMode=D3D11_FILL_SOLID;rasterDesc.CullMode=D3D11_CULL_NONE;rasterDesc.DepthClipEnable=true;
    ComPtr<ID3D11RasterizerState> raster;Check(SUCCEEDED(device->CreateRasterizerState(&rasterDesc,&raster)),"coverage raster state");
    auto countCoverage=[&](float alpha){
        NextAnimatedCasterFrame();RequestAnimatedCasters(true);setCamera(0,0,0);constants[102]=alpha;
        bindNative(.95f,nativeProp.Get());ctx->PSSetShader(fadePs.Get(),nullptr,0);ctx->OMSetBlendState(alphaBlend.Get(),nullptr,~0u);
        Check(BeginAnimatedCasterDraw(ctx.Get(),3,1),"capture cable with native vertex fade");ctx->Draw(3,0);EndAnimatedCasterDraw(ctx.Get());
        Check(AnimatedCasterVertexCount(AnimatedCasterSubset::Scene)==3,"coverage caster participates in scene shadow invalidation");
        ctx->ClearState();ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);ctx->OMSetRenderTargets(0,nullptr,dsv.Get());
        ctx->OMSetDepthStencilState(depthState.Get(),0);ctx->RSSetState(raster.Get());D3D11_VIEWPORT vp{0,0,512,512,0,1};ctx->RSSetViewports(1,&vp);
        Matrix identity{};identity[0]=identity[5]=identity[10]=identity[15]=1;
        DrawAnimatedCoverageCasters(ctx.Get(),identity);ctx->OMSetRenderTargets(0,nullptr,nullptr);ctx->CopyResource(depthReadback.Get(),depth.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};Check(SUCCEEDED(ctx->Map(depthReadback.Get(),0,D3D11_MAP_READ,0,&mapped)),"read cable shadow coverage");
        unsigned covered=0;for(UINT y=0;y<512;++y){const auto row=reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch);
            for(UINT x=0;x<512;++x)if(row[x]<.9f)++covered;}
        ctx->Unmap(depthReadback.Get(),0);return covered;
    };
    const auto solidCoverage=countCoverage(1),halfCoverage=countCoverage(.5f),zeroCoverage=countCoverage(0);
    Check(solidCoverage>1000,"opaque cable writes real depth");
    Check(zeroCoverage==0,"fully faded cable casts no shadow");
    Check(halfCoverage>solidCoverage*.45&&halfCoverage<solidCoverage*.55,"partial fade retains proportional coverage");
    NextAnimatedCasterFrame();RequestAnimatedCasters(true);ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);
    Check(AnimatedCasterVertexCount(AnimatedCasterSubset::Scene)==0,"old cable coverage cleared each frame");
    // The default fixture isolates resource/pose capture from shader discovery.
    // Passing the shipped depth PS additionally exercises the real ABI decoder.
    std::vector<unsigned char> depthCode;
    ComPtr<ID3D11PixelShader> depthPs;
    if(argc>1){
        std::ifstream input(argv[1],std::ios::binary);Check(bool(input),"open native depth shader fixture");
        depthCode.assign(std::istreambuf_iterator<char>(input),{});
        Check(NativeDepthCutoutCoverage(depthCode.data(),depthCode.size()),"recognize actual native depth opacity program");
        auto changedDepthCode=depthCode;
        UINT chunkCount=0;std::memcpy(&chunkCount,changedDepthCode.data()+28,4);
        for(UINT i=0;i<chunkCount;++i){UINT offset=0;std::memcpy(&offset,changedDepthCode.data()+32+i*4,4);
            if(!std::memcmp(changedDepthCode.data()+offset,"SHEX",4)||!std::memcmp(changedDepthCode.data()+offset,"SHDR",4)){
                changedDepthCode[offset+16]^=1;break;}}
        Check(!NativeDepthCutoutCoverage(changedDepthCode.data(),changedDepthCode.size()),"changed native opacity instructions fail closed");
        for(size_t n=0;n<depthCode.size();++n)Check(!NativeDepthCutoutCoverage(depthCode.data(),n),"reject truncated native depth shader");
        Check(SUCCEEDED(device->CreatePixelShader(depthCode.data(),depthCode.size(),nullptr,&depthPs)),"native depth PS");
        NoteAnimatedCasterPixelShader(depthPs.Get(),depthCode.data(),depthCode.size());
    }else{
        Check(!NativeDepthCutoutCoverage(opaqueCode->GetBufferPointer(),opaqueCode->GetBufferSize()),"opaque program is not native cutout ABI");
        Check(SUCCEEDED(device->CreatePixelShader(opaqueCode->GetBufferPointer(),opaqueCode->GetBufferSize(),nullptr,&depthPs)),"isolated depth capture fixture PS");
        constexpr GUID trustedCutoutTag={0x37530677,0x5946,0x48d8,{0xbd,0x94,0xed,0x57,0xab,0x5c,0x36,0xdd}};
        const UINT valid=1;depthPs->SetPrivateData(trustedCutoutTag,sizeof(valid),&valid);
    }
    auto depthVsCode=compile(R"(
        cbuffer PerViewConstantBuffer_t:register(b3){
            column_major float4x4 projection:packoffset(c0);
            column_major float4x4 g_matProjectionToWorld:packoffset(c4);
            float4 g_vWorldToCameraOffset:packoffset(c24);
            float4 pose:packoffset(c25);
        }
        struct V {float2 uv:TEXCOORD0;float4 position:SV_Position;};
        V Main(uint id:SV_VertexID){
            float2 p[3]={float2(-.8,-.8),float2(0,.8),float2(.8,-.8)};
            V o;o.uv=p[id]*.5+.5;
            o.position=mul(projection,float4(float3(p[id]+pose.xy,.5)+g_vWorldToCameraOffset.xyz,1));return o;
        })","vs_5_0");
    ComPtr<ID3D11VertexShader> depthVs;Check(SUCCEEDED(device->CreateVertexShader(depthVsCode->GetBufferPointer(),depthVsCode->GetBufferSize(),nullptr,&depthVs)),"native depth fixture VS");
    const char* depthName="csgo_depth_only.vfx_vs";depthVs->SetPrivateData(WKPDID_D3DDebugObjectName,UINT(strlen(depthName)),depthName);
    NoteAnimatedCasterShader(depthVs.Get(),depthVsCode->GetBufferPointer(),depthVsCode->GetBufferSize());
    D3D11_BUFFER_DESC materialDesc{};materialDesc.ByteWidth=32;materialDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> material,mode;
    Check(SUCCEEDED(device->CreateBuffer(&materialDesc,nullptr,&material))&&SUCCEEDED(device->CreateBuffer(&materialDesc,nullptr,&mode)),"native cutout constants");
    D3D11_SAMPLER_DESC sampleDesc{};sampleDesc.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sampleDesc.AddressU=sampleDesc.AddressV=sampleDesc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampleDesc.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sample;Check(SUCCEEDED(device->CreateSamplerState(&sampleDesc,&sample)),"native opacity sampler");
    auto opacityTexture=[&](float left,float right){
        float pixels[]={1,1,1,left,1,1,1,right};D3D11_TEXTURE2D_DESC desc{};desc.Width=2;desc.Height=desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.Usage=D3D11_USAGE_IMMUTABLE;
        D3D11_SUBRESOURCE_DATA data{pixels,sizeof(pixels),0};ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> srv;
        Check(SUCCEEDED(device->CreateTexture2D(&desc,&data,&texture))&&SUCCEEDED(device->CreateShaderResourceView(texture.Get(),nullptr,&srv)),"native opacity texture");return srv;
    };
    auto maskTexture=opacityTexture(0,1),solidTexture=opacityTexture(1,1),halfTexture=opacityTexture(.5,.5),emptyTexture=opacityTexture(0,0);
    auto cutoutCoverage=[&](ID3D11ShaderResourceView* opacity,float cutoff,float translate=0.f){
        NextAnimatedCasterFrame();RequestAnimatedCasters(true);setCamera(0,0,0);constants[100]=translate;
        bindNative(.95f,depthVs.Get());ctx->OMSetRenderTargets(0,nullptr,dsv.Get());ctx->PSSetShader(depthPs.Get(),nullptr,0);
        ctx->PSSetShaderResources(12,1,&opacity);auto s=sample.Get();ctx->PSSetSamplers(0,1,&s);
        float values[8]={0,0,0,cutoff,1,0,0,0};UINT modes[8]={0,0,0,0,0,0,1,0};
        ctx->UpdateSubresource(material.Get(),0,nullptr,values,0,0);ctx->UpdateSubresource(mode.Get(),0,nullptr,modes,0,0);
        ID3D11Buffer* buffers[]={material.Get(),mode.Get()};ctx->PSSetConstantBuffers(0,2,buffers);ctx->OMSetBlendState(coverageBlend.Get(),nullptr,~0u);
        Check(BeginAnimatedCasterDraw(ctx.Get(),3,1),"capture native depth cutout without a color target");ctx->Draw(3,0);EndAnimatedCasterDraw(ctx.Get());
        Check(AnimatedCasterVertexCount(AnimatedCasterSubset::Scene)==3&&AnimatedCasterVertexCount(AnimatedCasterSubset::Viewmodel)==0,"depth cutout belongs to world scene only");
        // The engine reuses these buffers immediately. The shadow must retain
        // the draw's old values, not observe this later update or texture bind.
        values[3]=2;ctx->UpdateSubresource(material.Get(),0,nullptr,values,0,0);
        ctx->ClearState();auto sentinel=emptyTexture.Get();ctx->PSSetShaderResources(12,1,&sentinel);ctx->PSSetSamplers(0,1,&s);ctx->PSSetConstantBuffers(0,2,buffers);
        ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);ctx->OMSetRenderTargets(0,nullptr,dsv.Get());ctx->OMSetDepthStencilState(depthState.Get(),0);ctx->RSSetState(raster.Get());
        D3D11_VIEWPORT vp{0,0,512,512,0,1};ctx->RSSetViewports(1,&vp);Matrix identity{};identity[0]=identity[5]=identity[10]=identity[15]=1;
        DrawAnimatedCoverageCasters(ctx.Get(),identity);
        ComPtr<ID3D11ShaderResourceView> restoredTexture;ctx->PSGetShaderResources(12,1,&restoredTexture);Check(restoredTexture.Get()==sentinel,"restore opacity SRV after shadow draw");
        ComPtr<ID3D11Buffer> restoredMaterial;ctx->PSGetConstantBuffers(0,1,&restoredMaterial);Check(restoredMaterial.Get()==material.Get(),"restore material CB after shadow draw");
        ctx->OMSetRenderTargets(0,nullptr,nullptr);ctx->CopyResource(depthReadback.Get(),depth.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(SUCCEEDED(ctx->Map(depthReadback.Get(),0,D3D11_MAP_READ,0,&mapped)),"read cutout shadow depth");
        unsigned covered=0,left=0;for(UINT y=0;y<512;++y){auto row=reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch);
            for(UINT x=0;x<512;++x)if(row[x]<.9f){++covered;if(x<250)++left;}}
        ctx->Unmap(depthReadback.Get(),0);return std::array<unsigned,2>{covered,left};
    };
    const auto solidCutout=cutoutCoverage(solidTexture.Get(),.5f),maskedCutout=cutoutCoverage(maskTexture.Get(),.5f),halfCutout=cutoutCoverage(halfTexture.Get(),.5f),emptyCutout=cutoutCoverage(emptyTexture.Get(),.5f);
    Check(solidCutout[0]>80000,"opaque texels cast full posed depth");
    Check(maskedCutout[0]>solidCutout[0]*.47&&maskedCutout[0]<solidCutout[0]*.53&&maskedCutout[1]==0,"transparent UV half stays a hole in the shadow");
    Check(halfCutout[0]>solidCutout[0]*.47&&halfCutout[0]<solidCutout[0]*.53,"native half coverage remains proportional");
    Check(emptyCutout[0]==0,"transparent texture casts no shadow");
    // Native edge smoothing blends a small amount of raw alpha into the ramp;
    // it does not become a binary cutoff merely because the texture is constant.
    Check(cutoutCoverage(halfTexture.Get(),.75f)[0]<solidCutout[0]*.02&&cutoutCoverage(halfTexture.Get(),.25f)[0]>solidCutout[0]*.97,"each draw keeps its native alpha cutoff");
    Check(cutoutCoverage(maskTexture.Get(),.5f,.25f)[0]>10000,"moving cutout keeps posed geometry");
    NextAnimatedCasterFrame();Check(AnimatedCasterVertexCount(AnimatedCasterSubset::Scene)==0,"cutout geometry expires every frame");
    RequestAnimatedCasters(false);NextAnimatedCasterFrame();NextAnimatedCasterFrame();
    bindNative(.95f);Check(!BeginAnimatedCasterDraw(ctx.Get(),3,1),"No requesting consumer stops capture on the following frame");
    EndAnimatedCasters();Check(AnimatedCasterVertexCount()==0,"reset clears poses");
    std::cout<<"Animated caster GPU capture tests passed\n";
}
