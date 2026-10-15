#define NOMINMAX
#include "RelightStainedGlass.h"
#include "../Environment/EnvironmentCasterMath.h"
#include "../Environment/EnvironmentShaderCompiler.h"
#include "../Debug/MvmDebugLog.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>
#include <tuple>
namespace Filmmaker::Relight {
using Microsoft::WRL::ComPtr;
namespace {
// Draws one window's glass in its beam projection. The colour is the glass texture; it is kept
// only where the sun reaches the outside of the glass (a point just on the sun's side of it,
// tested against the whole-map sun shadow), so a window in the shade of another building, or
// one the sun meets from the room side, projects nothing.
constexpr char kGlassShader[]=R"HLSL(
cbuffer Window:register(b0) {
    row_major float4x4 beam;
    float4 sunRows[4];   // whole-map sun shadow projection (record rows 0-3)
    float4 sunRect;      // atlas offset xy, scale zw (record row 13); zw 0 = unshadowed
    float4 towardSun;    // xyz, w: offset toward the sun for the visibility test
};
Texture2D<float4> glass:register(t0);
Texture2D<float> shadowAtlas:register(t1);
SamplerState linearSampler:register(s0);
SamplerComparisonState shadowSampler:register(s1);
struct Vertex {float3 p:POSITION;float2 uv:TEXCOORD0;};
struct Pixel {float4 position:SV_Position;float3 world:TEXCOORD0;float2 uv:TEXCOORD1;};
Pixel GlassVS(Vertex v){Pixel o;o.position=mul(beam,float4(v.p,1));o.position.z=.5;o.world=v.p;o.uv=v.uv;return o;}
float4 GlassPS(Pixel i):SV_Target {
    float visibility=0;
    if(sunRect.z>0){
        float4 p=float4(i.world+towardSun.xyz*towardSun.w,1);
        float4 clip=float4(dot(sunRows[0],p),dot(sunRows[1],p),dot(sunRows[2],p),dot(sunRows[3],p));
        float3 q=clip.xyz/clip.w;
        if(clip.w>0 && all(abs(q.xy)<=1) && q.z>=0 && q.z<=1)
            visibility=shadowAtlas.SampleCmpLevelZero(shadowSampler,q.xy*sunRect.zw+sunRect.xy,q.z-.001);
    }
    return float4(glass.Sample(linearSampler,i.uv).rgb*visibility,1);
}
)HLSL";
struct Range {UINT first=0,count=0,texture=0;};
struct Window {std::vector<Vec3> points;std::vector<Range> ranges;};
struct Constants {float beam[16];float sunRows[16];float sunRect[4];float towardSun[4];};
struct State {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> work;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vertices,constants;
    std::vector<ComPtr<ID3D11ShaderResourceView>> textures;
    ComPtr<ID3D11Texture3D> cookies;ComPtr<ID3D11ShaderResourceView> cookieSrv;
    std::vector<ComPtr<ID3D11RenderTargetView>> cookieRtv;
    ComPtr<ID3D11SamplerState> linear;ComPtr<ID3D11RasterizerState> raster;ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    std::weak_ptr<const Environment::VisualCasters> visual;
    std::vector<Window> windows;
    GlassBeams last;uint64_t frame=~0ull;bool failed=false,logged=false;
} state;
bool Build(const std::shared_ptr<const Environment::VisualCasters>& visual){
    auto d=state.device.Get();
    state.windows.clear();state.textures.clear();state.cookieRtv.clear();state.vertices.Reset();state.cookies.Reset();state.cookieSrv.Reset();
    state.visual=visual;
    if(!visual||visual->glass.empty())return true;
    // Flatten every glass triangle, group them into windows, then lay each window's triangles
    // out contiguously per glass texture so a window draws in a few ranges.
    std::vector<GlassTriangle> triangles;std::vector<std::pair<uint32_t,uint32_t>> source;
    for(uint32_t b=0;b<visual->glass.size();++b){const auto& g=visual->glass[b];
        for(size_t t=0;t+2<g.indices.size();t+=3){
            triangles.push_back({g.vertices[g.indices[t]].position,g.vertices[g.indices[t+1]].position,g.vertices[g.indices[t+2]].position});
            source.push_back({b,uint32_t(t)});}}
    auto grouped=GroupGlassWindows(triangles);
    if(grouped.size()>GlassWindowLimit)grouped.resize(GlassWindowLimit);
    std::vector<Environment::GlassVertex> stream;
    for(const auto& w:grouped){
        Window window;
        std::vector<uint32_t> order=w.triangles;
        std::sort(order.begin(),order.end(),[&](uint32_t a,uint32_t b){return source[a].first<source[b].first;});
        for(auto t:order){
            const auto& g=visual->glass[source[t].first];
            if(window.ranges.empty()||window.ranges.back().texture!=source[t].first)
                window.ranges.push_back({UINT(stream.size()),0,source[t].first});
            for(int k=0;k<3;++k){const auto& v=g.vertices[g.indices[source[t].second+k]];stream.push_back(v);window.points.push_back(v.position);}
            window.ranges.back().count+=3;
        }
        state.windows.push_back(std::move(window));
    }
    D3D11_BUFFER_DESC b{};b.ByteWidth=UINT(stream.size()*sizeof(Environment::GlassVertex));b.BindFlags=D3D11_BIND_VERTEX_BUFFER;b.Usage=D3D11_USAGE_IMMUTABLE;
    D3D11_SUBRESOURCE_DATA initial{stream.data(),0,0};
    if(FAILED(d->CreateBuffer(&b,&initial,&state.vertices)))return false;
    for(const auto& g:visual->glass){
        D3D11_TEXTURE2D_DESC t{};t.Width=g.width;t.Height=g.height;t.MipLevels=0;t.ArraySize=1;t.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        t.SampleDesc.Count=1;t.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;t.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;
        ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> srv;
        if(FAILED(d->CreateTexture2D(&t,nullptr,&texture))||FAILED(d->CreateShaderResourceView(texture.Get(),nullptr,&srv)))return false;
        state.work->UpdateSubresource(texture.Get(),0,nullptr,g.rgba.data(),g.width*4,0);
        state.work->GenerateMips(srv.Get());
        state.textures.push_back(srv);
    }
    D3D11_TEXTURE3D_DESC c{};c.Width=c.Height=GlassCookieSize;c.Depth=UINT(state.windows.size());c.MipLevels=1;
    c.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;c.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    if(FAILED(d->CreateTexture3D(&c,nullptr,&state.cookies))||FAILED(d->CreateShaderResourceView(state.cookies.Get(),nullptr,&state.cookieSrv)))return false;
    for(UINT i=0;i<c.Depth;++i){
        D3D11_RENDER_TARGET_VIEW_DESC r{};r.Format=c.Format;r.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE3D;r.Texture3D.FirstWSlice=i;r.Texture3D.WSize=1;
        ComPtr<ID3D11RenderTargetView> rtv;if(FAILED(d->CreateRenderTargetView(state.cookies.Get(),&r,&rtv)))return false;
        state.cookieRtv.push_back(rtv);
    }
    return true;
}
bool Initialize(){
    if(state.vs)return true;if(state.failed)return false;state.failed=true;
    auto d=state.device.Get();ComPtr<ID3DBlob> vs,ps,errors;
    for(auto [entry,profile,blob]:{std::tuple<const char*,const char*,ID3DBlob**>{"GlassVS","vs_5_0",vs.GetAddressOf()},{"GlassPS","ps_5_0",ps.GetAddressOf()}})
        if(FAILED(Environment::CompileEnvironmentShader(kGlassShader,sizeof(kGlassShader)-1,"RelightStainedGlass",nullptr,entry,profile,
            D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,blob,errors.ReleaseAndGetAddressOf()))){
            MvmDebugLog_LinefAlways("relight.glass","shader=%s failure=%s",entry,errors?static_cast<const char*>(errors->GetBufferPointer()):"compiler unavailable");
            return false;}
    const D3D11_INPUT_ELEMENT_DESC elements[]{{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(Constants);cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC s{};s.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;s.MaxLOD=D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC r{};r.FillMode=D3D11_FILL_SOLID;r.CullMode=D3D11_CULL_NONE;r.DepthClipEnable=FALSE;
    // Overlapping glass layers (the lit and unlit variants share panes) keep the brighter colour.
    D3D11_BLEND_DESC blend{};blend.RenderTarget[0].BlendEnable=TRUE;blend.RenderTarget[0].SrcBlend=blend.RenderTarget[0].DestBlend=D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOp=D3D11_BLEND_OP_MAX;blend.RenderTarget[0].SrcBlendAlpha=blend.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_MAX;blend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_DEPTH_STENCIL_DESC depth{};
    if(FAILED(d->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&state.vs))||
       FAILED(d->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&state.ps))||
       FAILED(d->CreateInputLayout(elements,2,vs->GetBufferPointer(),vs->GetBufferSize(),&state.layout))||
       FAILED(d->CreateBuffer(&cb,nullptr,&state.constants))||FAILED(d->CreateSamplerState(&s,&state.linear))||
       FAILED(d->CreateRasterizerState(&r,&state.raster))||FAILED(d->CreateBlendState(&blend,&state.blend))||
       FAILED(d->CreateDepthStencilState(&depth,&state.depth))||FAILED(d->CreateDeferredContext(0,&state.work))){state.vs.Reset();return false;}
    state.failed=false;return true;
}
}
void EndStainedGlass(){state=State{};}
GlassBeams PrepareStainedGlass(ID3D11Device* device,ID3D11DeviceContext* immediate,
    const std::shared_ptr<const Environment::VisualCasters>& visual,const LightRecord* sun,
    ID3D11ShaderResourceView* atlas,ID3D11SamplerState* comparison,Vec3 towardSun,Vec3 radiance,uint64_t frame){
    if(state.device.Get()!=device){EndStainedGlass();state.device=device;}
    if(!visual||visual->glass.empty()||!HasRadiance(radiance)||!Initialize())return {};
    if(state.frame==frame)return state.last;
    state.frame=frame;state.last={};
    auto work=state.work.Get();
    if(state.visual.lock()!=visual&&!Build(visual)){state.windows.clear();return {};}
    if(state.windows.empty())return {};
    const bool shadowed=sun&&atlas&&comparison&&(*sun)[54]>0;
    // Transmission requires known incident sunlight, even if receiver shadows are disabled.
    if(!shadowed)return {};
    Constants constants{};
    if(shadowed){for(int i=0;i<16;++i)constants.sunRows[i]=(*sun)[i];for(int i=0;i<4;++i)constants.sunRect[i]=(*sun)[52+i];}
    constants.towardSun[0]=towardSun.x;constants.towardSun[1]=towardSun.y;constants.towardSun[2]=towardSun.z;constants.towardSun[3]=4;
    const UINT stride=sizeof(Environment::GlassVertex),offset=0;auto vb=state.vertices.Get();
    work->IASetVertexBuffers(0,1,&vb,&stride,&offset);work->IASetInputLayout(state.layout.Get());
    work->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    work->VSSetShader(state.vs.Get(),nullptr,0);work->PSSetShader(state.ps.Get(),nullptr,0);
    auto cb=state.constants.Get();work->VSSetConstantBuffers(0,1,&cb);work->PSSetConstantBuffers(0,1,&cb);
    ID3D11SamplerState* samplers[2]{state.linear.Get(),comparison};work->PSSetSamplers(0,2,samplers);
    work->RSSetState(state.raster.Get());work->OMSetDepthStencilState(state.depth.Get(),0);
    const float factor[4]{};work->OMSetBlendState(state.blend.Get(),factor,~0u);
    const D3D11_VIEWPORT vp{0,0,float(GlassCookieSize),float(GlassCookieSize),0,1};work->RSSetViewports(1,&vp);
    const float black[4]{};unsigned beams=0;
    for(UINT w=0;w<state.windows.size();++w){
        const auto& window=state.windows[w];
        const auto beam=GlassBeamProjection(window.points,towardSun);
        auto rtv=state.cookieRtv[w].Get();work->ClearRenderTargetView(rtv,black);
        if(!beam.valid)continue;
        if(MvmDebugLog_Active()&&!state.logged){
            float low=1e30f,high=-1e30f,oldLow=1e30f,oldHigh=-1e30f;
            for(const auto& p:window.points){const float z=BeamClip(beam,p).z;
                low=(std::min)(low,z);high=(std::max)(high,z);
                const float old=-(p.x*towardSun.x+p.y*towardSun.y+p.z*towardSun.z);
                oldLow=(std::min)(oldLow,old);oldHigh=(std::max)(oldHigh,old);}
            MvmDebugLog_LinefAlways("relight.glass.plane",
                "window=%u points=%zu depth=%.6f..%.6f skippedSlabBefore=%.2f oblique=1",w,window.points.size(),low,high,oldHigh-oldLow);
        }
        std::memcpy(constants.beam,beam.matrix,sizeof(beam.matrix));
        work->UpdateSubresource(cb,0,nullptr,&constants,0,0);
        work->OMSetRenderTargets(1,&rtv,nullptr);
        ID3D11ShaderResourceView* inputs[2]{nullptr,shadowed?atlas:nullptr};
        for(const auto& range:window.ranges){
            inputs[0]=state.textures[range.texture].Get();work->PSSetShaderResources(0,2,inputs);
            work->Draw(range.count,range.first);
        }
        state.last.records.push_back(GlassBeamRecord(beam,towardSun,radiance,GlassCookieSlice(w,UINT(state.windows.size()))));
        ++beams;
    }
    ID3D11ShaderResourceView* none[2]{};work->PSSetShaderResources(0,2,none);
    ID3D11RenderTargetView* noTarget=nullptr;work->OMSetRenderTargets(1,&noTarget,nullptr);
    ComPtr<ID3D11CommandList> commands;if(FAILED(work->FinishCommandList(FALSE,&commands)))return {};
    immediate->ExecuteCommandList(commands.Get(),TRUE);
    state.last.cookies=state.cookieSrv.Get();
    if(MvmDebugLog_Active()&&!state.logged){state.logged=true;
        MvmDebugLog_LinefAlways("relight.glass","windows=%zu beams=%u batches=%zu shadowTested=%u cookie=%ux%ux%zu",
            state.windows.size(),beams,visual->glass.size(),unsigned(shadowed),GlassCookieSize,GlassCookieSize,state.windows.size());}
    return state.last;
}
}
