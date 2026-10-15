#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentAnimatedCasters.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentIndirectCasterMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderCompiler.h"
#include <wrl/client.h>
#include <iostream>
#include <cstdlib>
#include <cstdarg>
using namespace Filmmaker::Environment;
using Microsoft::WRL::ComPtr;
namespace Filmmaker {bool MvmDebugLog_Active(){return true;}void MvmDebugLog_LinefAlways(const char* channel,const char* format,...) {
    std::fprintf(stderr,"%s: ",channel);va_list args;va_start(args,format);std::vfprintf(stderr,format,args);va_end(args);std::fputc('\n',stderr);
}}
void Check(bool value,const char* text){if(!value){std::cerr<<text<<'\n';std::exit(1);}}
int main(){
    Check(IndirectCasterSpan(100,4,24,4,true)==92,"Padded vendor records use exact bounded span");
    Check(!IndirectCasterSpan(100,5,24,4,true)&&!IndirectCasterSpan(100,4,23,4,true)&&
        !IndirectCasterSpan(100,4,24,5,true)&&!IndirectCasterSpan(~0u,0,~3u,~0u,true),"Malformed offsets, stride and overflow rejected");
    UINT records[]{3,2,0,0,0,0,9,0,0,0,0,0};UINT counted=0;
    Check(IndirectCasterVertices(records,44,24,2,true,6,counted)&&counted==6,"Current instance count and culled vendor draws");
    Check(!IndirectCasterVertices(records,44,24,2,true,5,counted),"SO capacity protects indirect capture");
    records[0]=~0u;records[1]=~0u;Check(!IndirectCasterVertices(records,44,24,2,true,~0u,counted),"64-bit vertex product cannot overflow capture capacity");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,8,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context)),"WARP device");
    BeginAnimatedCasters(device.Get());RequestAnimatedCasters(true);
    const char* source=R"(
        cbuffer PerViewConstantBuffer_t:register(b3){column_major float4x4 g_matProjectionToWorld:packoffset(c4);
            float4 g_vWorldToCameraOffset:packoffset(c24);float4 pose:packoffset(c25);}
        struct V {float2 uv:TEXCOORD0;float4 position:SV_Position;};
        V Main(float3 position:POSITION,uint bone:BLENDINDICES) {
            V o;o.uv=position.yz*.5+.5;o.position=mul(g_matProjectionToWorld,float4(position+pose.xyz+float3(bone,0,0),1));return o;})";
    ComPtr<ID3DBlob> code,error;Check(SUCCEEDED(CompileEnvironmentShader(source,std::strlen(source),"EarlyCasterTest",nullptr,"Main","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&code,&error)),"Compile native posed fixture");
    ComPtr<ID3D11VertexShader> vs;Check(SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs)),"Native fixture VS");
    const char* name="csgo_depth_only.vfx_vs";vs->SetPrivateData(WKPDID_D3DDebugObjectName,UINT(std::strlen(name)),name);
    NoteAnimatedCasterShader(vs.Get(),code->GetBufferPointer(),code->GetBufferSize());
    const D3D11_INPUT_ELEMENT_DESC elements[]{{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"BLENDINDICES",0,DXGI_FORMAT_R32_UINT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};
    ComPtr<ID3D11InputLayout> layout;Check(SUCCEEDED(device->CreateInputLayout(elements,2,code->GetBufferPointer(),code->GetBufferSize(),&layout)),"Native skinned layout");
    struct Input {Vec3 position;UINT bone;};const Input vertices[]{{{0,-1,0},0},{{0,1,0},0},{{0,0,1},0}};
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(vertices);bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data{vertices};ComPtr<ID3D11Buffer> vb,ib,cb;
    Check(SUCCEEDED(device->CreateBuffer(&bd,&data,&vb)),"Native fixture vertices");
    const UINT indices[]{0,1,2};bd.ByteWidth=sizeof(indices);bd.BindFlags=D3D11_BIND_INDEX_BUFFER;data.pSysMem=indices;
    Check(SUCCEEDED(device->CreateBuffer(&bd,&data,&ib)),"Native fixture indices");
    bd.ByteWidth=416;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&cb)),"Native fixture constants");
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=512;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_D32_FLOAT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;ComPtr<ID3D11DepthStencilView> dsv;
    Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&depth))&&SUCCEEDED(device->CreateDepthStencilView(depth.Get(),nullptr,&dsv)),"Native depth target");
    UINT args[]{3,1,0,0,0};bd={};bd.ByteWidth=sizeof(args);bd.MiscFlags=D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;data.pSysMem=args;
    ComPtr<ID3D11Buffer> indirect;Check(SUCCEEDED(device->CreateBuffer(&bd,&data,&indirect)),"GPU indirect arguments");
    ComPtr<ID3D11PixelShader> mask;
    const char* pixelSource="float4 Main(float2 uv:TEXCOORD0):SV_Target{return float4(uv,0,1);}";
    Check(SUCCEEDED(CompileEnvironmentShader(pixelSource,std::strlen(pixelSource),"EarlyCutoutFixture",nullptr,"Main","ps_5_0",0,&code,&error))&&
        SUCCEEDED(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&mask)),"Masked fixture PS");
    const UINT valid=1;constexpr GUID trustedMaskTag={0x37530677,0x5946,0x48d8,{0xbd,0x94,0xed,0x57,0xab,0x5c,0x36,0xdd}};
    mask->SetPrivateData(trustedMaskTag,sizeof(valid),&valid); // The ABI decoder has separate native-program tests.
    D3D11_TEXTURE2D_DESC alphaDesc{};alphaDesc.Width=alphaDesc.Height=alphaDesc.MipLevels=alphaDesc.ArraySize=alphaDesc.SampleDesc.Count=1;
    alphaDesc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;alphaDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    float alpha[]{1,1,1,1};D3D11_SUBRESOURCE_DATA alphaData{alpha,16};ComPtr<ID3D11Texture2D> opacity;ComPtr<ID3D11ShaderResourceView> alphaView;
    Check(SUCCEEDED(device->CreateTexture2D(&alphaDesc,&alphaData,&opacity))&&SUCCEEDED(device->CreateShaderResourceView(opacity.Get(),nullptr,&alphaView)),"Coverage opacity");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD=D3D11_FLOAT32_MAX;ComPtr<ID3D11SamplerState> sampler;Check(SUCCEEDED(device->CreateSamplerState(&sd,&sampler)),"Coverage sampler");
    ComPtr<ID3D11DeviceContext> shadowContext;Check(SUCCEEDED(device->CreateDeferredContext(0,&shadowContext)),"Owned shadow context");
    const char* shadowSource="float4 Main(float3 p:POSITION):SV_Position{return float4(p.y*.8,p.z*1.6-.8,.5,1);}";
    Check(SUCCEEDED(CompileEnvironmentShader(shadowSource,std::strlen(shadowSource),"EarlyShadowFixture",nullptr,"Main","vs_5_0",0,&code,&error)),"Shadow fixture shader");
    ComPtr<ID3D11VertexShader> shadowVs;ComPtr<ID3D11InputLayout> shadowLayout;
    const D3D11_INPUT_ELEMENT_DESC shadowElement{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    Check(SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shadowVs))&&
        SUCCEEDED(device->CreateInputLayout(&shadowElement,1,code->GetBufferPointer(),code->GetBufferSize(),&shadowLayout)),"Shadow fixture pipeline");
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;Check(SUCCEEDED(device->CreateRasterizerState(&rd,&raster)),"Shadow raster");
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> shadowReadback;Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&shadowReadback)),"Shadow depth readback");
    ComPtr<ID3D11Buffer> retainedIndirectVertices;
    UINT retainedIndirectBytes=0;
    auto capture=[&](float x,bool gpu=false,bool masked=false){
        RequestAnimatedCasters(true);
        RequestAnimatedCasters(false); // An inactive shadow tier cannot cancel the active consumer.
        context->ClearState();context->OMSetRenderTargets(0,nullptr,dsv.Get());D3D11_VIEWPORT vp{0,0,512,512,0,.95f};context->RSSetViewports(1,&vp);
        context->IASetInputLayout(layout.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        auto buffer=vb.Get();UINT stride=sizeof(Input),offset=0;context->IASetVertexBuffers(0,1,&buffer,&stride,&offset);context->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);
        context->VSSetShader(vs.Get(),nullptr,0);float constants[104]{};for(UINT i=0;i<4;i++)constants[16+i*5]=1;constants[100]=x;
        context->UpdateSubresource(cb.Get(),0,nullptr,constants,0,0);buffer=cb.Get();context->VSSetConstantBuffers(3,1,&buffer);
        if(masked){context->PSSetShader(mask.Get(),nullptr,0);context->PSSetConstantBuffers(0,1,&buffer);context->PSSetConstantBuffers(1,1,&buffer);
            auto view=alphaView.Get();context->PSSetShaderResources(12,1,&view);auto sample=sampler.Get();context->PSSetSamplers(0,1,&sample);}
        if(gpu){
            Check(BeginAnimatedCasterIndirectDraw(context.Get(),indirect.Get(),0,20,1,true),"GPU skinned depth arguments captured before lighting");
            ComPtr<ID3D11Buffer> captured;context->SOGetTargets(1,&captured);
            Check(!!captured,"Indirect capture binds its stream-output buffer");
            D3D11_BUFFER_DESC capturedDesc{};captured->GetDesc(&capturedDesc);
            if(retainedIndirectVertices&&capturedDesc.ByteWidth<=retainedIndirectBytes)
                Check(captured.Get()==retainedIndirectVertices.Get(),"Opaque/cutout stride switches reuse sufficient GPU storage");
            retainedIndirectVertices=captured;retainedIndirectBytes=capturedDesc.ByteWidth;
            context->DrawIndexedInstancedIndirect(indirect.Get(),0);
        }
        else {Check(BeginAnimatedCasterDraw(context.Get(),3,1),"Skinned depth is captured before any color-target draw");context->DrawIndexed(3,0,0);}
        EndAnimatedCasterDraw(context.Get());
        ComPtr<ID3D11DepthStencilView> restored;context->OMGetRenderTargets(0,nullptr,&restored);Check(restored.Get()==dsv.Get(),"Native depth target preserved");
        std::vector<CasterBounds> bounds;Check(ReadAnimatedCasterBounds(context.Get(),bounds)&&bounds.size()==1,"Current early pose bounds");
        Check(bounds[0].low.x==x&&bounds[0].high.x==x&&bounds[0].low.y==-1&&bounds[0].high.z==1,"Captured early pose matches native vertices");
        shadowContext->ClearState();shadowContext->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);
        shadowContext->OMSetRenderTargets(0,nullptr,dsv.Get());vp.MaxDepth=1;shadowContext->RSSetViewports(1,&vp);shadowContext->RSSetState(raster.Get());
        shadowContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);shadowContext->IASetInputLayout(shadowLayout.Get());shadowContext->VSSetShader(shadowVs.Get(),nullptr,0);
        if(masked){Matrix projection{0,.8f,0,0,0,0,1.6f,-.8f,0,0,0,.5f,0,0,0,1};DrawAnimatedCoverageCasters(shadowContext.Get(),projection);}
        else DrawAnimatedCasters(shadowContext.Get(),AnimatedCasterSubset::Scene);
        shadowContext->OMSetRenderTargets(0,nullptr,nullptr);shadowContext->CopyResource(shadowReadback.Get(),depth.Get());
        ComPtr<ID3D11CommandList> commands;Check(SUCCEEDED(shadowContext->FinishCommandList(FALSE,&commands)),"Finish shadow fixture");context->ExecuteCommandList(commands.Get(),TRUE);
        D3D11_MAPPED_SUBRESOURCE mapped{};Check(SUCCEEDED(context->Map(shadowReadback.Get(),0,D3D11_MAP_READ,0,&mapped)),"Read GPU-counted shadow depth");
        UINT pixels=0;for(UINT y=0;y<512;++y){auto row=reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch);for(UINT x=0;x<512;++x)pixels+=row[x]<.9f;}
        context->Unmap(shadowReadback.Get(),0);
        Check(masked&&alpha[3]==0?pixels==0:pixels>80000,"GPU-counted draws cast real opaque depth and preserve transparent texels");
    };
    capture(100);NextAnimatedCasterFrame();capture(200,true);NextAnimatedCasterFrame();
    capture(300,true,true);NextAnimatedCasterFrame();
    alpha[3]=0;context->UpdateSubresource(opacity.Get(),0,nullptr,alpha,16,0);capture(400,true,true);NextAnimatedCasterFrame();
    alpha[3]=1;context->UpdateSubresource(opacity.Get(),0,nullptr,alpha,16,0);
    capture(500,true);NextAnimatedCasterFrame(); // Cutout-to-opaque reuse still casts full depth.
    for(UINT frame=0;frame<8;++frame){capture(600+float(frame),true,!!(frame%2));NextAnimatedCasterFrame();}
    const auto retiredVertices=retainedIndirectVertices;
    retainedIndirectVertices.Reset();retainedIndirectBytes=0;
    for(UINT frame=0;frame<130;++frame)NextAnimatedCasterFrame();
    capture(700,true); // Recreated storage must still capture the current pose and cast depth.
    Check(retainedIndirectVertices.Get()!=retiredVertices.Get(),"Idle indirect storage retires instead of retaining peak allocation forever");
    Check(retainedIndirectBytes<7*1024*1024,"Opaque indirect slots retain the handoff's 512K vertex limit");
    NextAnimatedCasterFrame();
    args[1]=0;context->UpdateSubresource(indirect.Get(),0,nullptr,args,0,0);
    Check(BeginAnimatedCasterIndirectDraw(context.Get(),indirect.Get(),0,20,1,true),"Zero-instance draw is counted on GPU without a CPU wait");
    context->DrawIndexedInstancedIndirect(indirect.Get(),0);EndAnimatedCasterDraw(context.Get());
    std::vector<CasterBounds> empty;Check(ReadAnimatedCasterBounds(context.Get(),empty)&&empty.empty(),"Disappearing early actor leaves no stale bounds");
    NextAnimatedCasterFrame();RequestAnimatedCasters(true);
    args[1]=~0u;context->UpdateSubresource(indirect.Get(),0,nullptr,args,0,0);
    Check(BeginAnimatedCasterIndirectDraw(context.Get(),indirect.Get(),0,20,1,true),"Counter validates an oversized native count on the GPU");
    EndAnimatedCasterDraw(context.Get()); // Deliberately do not submit billions of fixture instances.
    Check(!ReadAnimatedCasterBounds(context.Get(),empty),"GPU counter overflow cannot read stale vertices or report complete bounds");
    // Solid simple-material debris participates in the existing opaque capture path.
    NextAnimatedCasterFrame();RequestAnimatedCasters(true);
    const char* effectsName="csgo_simple.vfx_vs";vs->SetPrivateData(WKPDID_D3DDebugObjectName,UINT(std::strlen(effectsName)),effectsName);
    Check(SUCCEEDED(CompileEnvironmentShader(pixelSource,std::strlen(pixelSource),"EffectCoverageFixture",nullptr,"Main","ps_5_0",0,&code,&error)),"Compile effect coverage fixture");
    NoteAnimatedCasterPixelShader(mask.Get(),code->GetBufferPointer(),code->GetBufferSize());
    D3D11_TEXTURE2D_DESC colorDesc{};colorDesc.Width=colorDesc.Height=512;colorDesc.MipLevels=colorDesc.ArraySize=colorDesc.SampleDesc.Count=1;
    colorDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;colorDesc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> color;ComPtr<ID3D11RenderTargetView> colorView;
    Check(SUCCEEDED(device->CreateTexture2D(&colorDesc,nullptr,&color))&&SUCCEEDED(device->CreateRenderTargetView(color.Get(),nullptr,&colorView)),"Effect color target");
    auto effectRtv=colorView.Get();context->OMSetRenderTargets(1,&effectRtv,dsv.Get());context->PSSetShader(mask.Get(),nullptr,0);
    D3D11_BLEND_DESC blendDesc{};auto& blendTarget=blendDesc.RenderTarget[0];blendTarget.BlendEnable=TRUE;
    blendTarget.SrcBlend=blendTarget.DestBlend=blendTarget.SrcBlendAlpha=blendTarget.DestBlendAlpha=D3D11_BLEND_ONE;
    blendTarget.BlendOp=blendTarget.BlendOpAlpha=D3D11_BLEND_OP_ADD;blendTarget.RenderTargetWriteMask=15;
    ComPtr<ID3D11BlendState> additive;Check(SUCCEEDED(device->CreateBlendState(&blendDesc,&additive)),"Effect additive blend");context->OMSetBlendState(additive.Get(),nullptr,~0u);
    D3D11_DEPTH_STENCIL_DESC effectDesc{};effectDesc.DepthEnable=TRUE;effectDesc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;effectDesc.DepthFunc=D3D11_COMPARISON_LESS;
    ComPtr<ID3D11DepthStencilState> solidDepth,transparentDepth;Check(SUCCEEDED(device->CreateDepthStencilState(&effectDesc,&solidDepth)),"Solid effect depth");
    context->OMSetDepthStencilState(solidDepth.Get(),0);context->OMSetBlendState(nullptr,nullptr,~0u);
    Check(BeginAnimatedCasterDraw(context.Get(),3,1),"Opaque simple-material mesh casts a shadow");context->DrawIndexed(3,0,0);EndAnimatedCasterDraw(context.Get());
    effectDesc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;Check(SUCCEEDED(device->CreateDepthStencilState(&effectDesc,&transparentDepth)),"Transparent effect depth");context->OMSetDepthStencilState(transparentDepth.Get(),0);context->OMSetBlendState(additive.Get(),nullptr,~0u);
    Check(!BeginAnimatedCasterDraw(context.Get(),3,1),"Additive flash sprites without opaque depth never become solid shadow quads");
    EndAnimatedCasters();std::cout<<"Native indexed skinned depth capture, current movement and disappearance PASS\n";
}
