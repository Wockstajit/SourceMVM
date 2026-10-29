#define NOMINMAX
#include "../../AfxHookSource/Filmmaker/SourceLensShaders.h"
#include "../../AfxHookSource/Filmmaker/SourceLensSmoke.h"
#include <d3dcompiler.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <cstring>
#include <vector>
#include <filesystem>
#include <AfxHookSource2/Filmmaker/Lens/LensFxMath.h>
#include <AfxHookSource2/Filmmaker/Lens/LensFxConstants.h>

using Microsoft::WRL::ComPtr;
struct GpuFixture {
    HWND window = nullptr;
    ComPtr<IDirect3D9> api;
    ComPtr<IDirect3DDevice9> device;
    GpuFixture(bool enabled) {
        if (!enabled) return;
        WNDCLASSW cls{}; cls.lpfnWndProc=DefWindowProcW; cls.hInstance=GetModuleHandleW(nullptr); cls.lpszClassName=L"MvmLensFixture";
        RegisterClassW(&cls);
        window=CreateWindowExW(0,cls.lpszClassName,L"",WS_POPUP|WS_DISABLED,0,0,16,16,nullptr,nullptr,cls.hInstance,nullptr);
        // Never shown, activated or used for input. All pixels stay on private offscreen surfaces.
        api.Attach(Direct3DCreate9(D3D_SDK_VERSION));
        if (!api || !window) return;
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow=window;
        pp.BackBufferWidth=pp.BackBufferHeight=16; pp.BackBufferFormat=D3DFMT_A8R8G8B8; pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
        api->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&device);
    }
    ~GpuFixture() { device.Reset(); api.Reset(); if (window) DestroyWindow(window); UnregisterClassW(L"MvmLensFixture",GetModuleHandleW(nullptr)); }
};

static bool ScalarProbe(GpuFixture& gpu,IDirect3DPixelShader9* pixel,const std::vector<SourceFilmmaker::LensShaders::Binding>& bindings,bool sun,IDirect3DTexture9* smokeMap=nullptr) {
    auto* d=gpu.device.Get();
    const char vertex[] = "struct I{float4 p:POSITION0;float2 uv:TEXCOORD0;};struct O{float4 p:POSITION0;float2 uv:TEXCOORD0;};O main(I i){O o;o.p=i.p;o.uv=i.uv;return o;}";
    ComPtr<ID3DBlob> vcode;
    if (FAILED(D3DCompile(vertex,sizeof(vertex),"FixtureVertex",nullptr,nullptr,"main","vs_3_0",0,0,&vcode,nullptr))) return false;
    ComPtr<IDirect3DVertexShader9> vs;
    ComPtr<IDirect3DVertexDeclaration9> decl;
    if (FAILED(d->CreateVertexShader((const DWORD*)vcode->GetBufferPointer(),&vs))) return false;
    const D3DVERTEXELEMENT9 elements[]={{0,0,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_POSITION,0},{0,16,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},D3DDECL_END()};
    if (FAILED(d->CreateVertexDeclaration(elements,&decl))) return false;
    auto texture = [&](int size,float value) {
        ComPtr<IDirect3DTexture9> t;
        if (FAILED(d->CreateTexture(size,size,1,0,D3DFMT_R32F,D3DPOOL_MANAGED,&t,nullptr))) return t;
        D3DLOCKED_RECT lock{};
        if (FAILED(t->LockRect(0,&lock,nullptr,0))) return ComPtr<IDirect3DTexture9>{};
        for (int y=0;y<size;++y) for(int x=0;x<size;++x) *(float*)((char*)lock.pBits+y*lock.Pitch+x*4)=value;
        t->UnlockRect(0);
        return t;
    };
    auto depth=texture(8,.6f),prev=texture(1,400);
    ComPtr<IDirect3DTexture9> target;
    ComPtr<IDirect3DSurface9> surface,read;
    if (!depth || !prev || FAILED(d->CreateTexture(1,1,1,D3DUSAGE_RENDERTARGET,D3DFMT_R32F,D3DPOOL_DEFAULT,&target,nullptr))
        || FAILED(target->GetSurfaceLevel(0,&surface)) || FAILED(d->CreateOffscreenPlainSurface(1,1,D3DFMT_R32F,D3DPOOL_SYSTEMMEM,&read,nullptr))) return false;
    auto set = [&](const char* name,const float* value) {
        for (const auto& b:bindings) if (b.set==2 && b.name==name) d->SetPixelShaderConstantF(b.first,value,b.count);
    };
    const float inverse[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,.001f};
    const float dims[4]={8,8,8,8},depthInfo[4]={0,1,0,0},dof[4]={0,400,2,1},smoke[4]={0,0,8,8};
    set("inverseViewProjection",inverse); set("dims",dims); set("depthInfo",depthInfo); set("dofA",dof); set("smokeInfo",smoke);
    const float sunInfo[4]={.5f,.5f,1,0};set("sunInfo",sunInfo);
    if(smokeMap) {
        float smokeInfo[4]={1,2,8,8},sphere[32]={0,0,300,100},camera[4]={};
        set("smokeInfo",smokeInfo);set("smokeSpheres",sphere);set("cameraPos",camera);
    }
    for (const auto& b:bindings) if (b.set==3) {
        d->SetTexture(b.first,b.name=="sceneDepth" ? depth.Get() : (b.name=="focusPrev" ? prev.Get() : (b.name=="smokeAbsorb" ? smokeMap : nullptr)));
        d->SetSamplerState(b.first,D3DSAMP_MINFILTER,D3DTEXF_POINT); d->SetSamplerState(b.first,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
        d->SetSamplerState(b.first,D3DSAMP_MIPFILTER,D3DTEXF_NONE); d->SetSamplerState(b.first,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP); d->SetSamplerState(b.first,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
    }
    if (FAILED(d->SetRenderTarget(0,surface.Get()))) return false;
    d->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp{0,0,1,1,0,1}; d->SetViewport(&vp);
    d->SetRenderState(D3DRS_ZENABLE,FALSE); d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE); d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE); d->SetRenderState(D3DRS_COLORWRITEENABLE,15); d->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);
    d->SetVertexShader(vs.Get()); d->SetVertexDeclaration(decl.Get()); d->SetPixelShader(pixel);
    const float quad[4][6]={{-1,1,0,1,0,0},{1,1,0,1,1,0},{-1,-1,0,1,0,1},{1,-1,0,1,1,1}};
    float firstTarget=0;
    for (int first=0;first<2;++first) {
        if(sun) {
            depth=texture(8,first==0 ? 1.f : .6f);if(!depth) return false;
            for(const auto& b:bindings) if(b.set==3 && b.name=="sceneDepth") d->SetTexture(b.first,depth.Get());
        }
        const float frame[4]={.016f,6,0,first==0 ? 1.f : 0.f}; set("frame",frame);
        d->Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0);
        if (FAILED(d->BeginScene())) return false;
        const HRESULT drawn=d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(quad[0])); d->EndScene();
        if (FAILED(drawn) || FAILED(d->GetRenderTargetData(surface.Get(),read.Get()))) return false;
        D3DLOCKED_RECT lock{}; if (FAILED(read->LockRect(&lock,nullptr,D3DLOCK_READONLY))) return false;
        const float value=*(const float*)lock.pBits; read->UnlockRect();
        if(smokeMap && !sun && first==0) {
            if(!std::isfinite(value) || value<200 || value>350) return false;
            firstTarget=value;
        }
        const float target=smokeMap ? firstTarget : 1000.f;
        const float expected=sun ? (first==0 ? (smokeMap ? .375f : 1.f) : 0.f)
            : (first==0 ? target : 400.f+(target-400.f)*Filmmaker::Lens::AutofocusStep(6,.016f));
        std::printf("D3D9 %s: first=%d actual=%.4f expected=%.4f\n",sun ? (smokeMap ? "smoky sun visibility" : "sun visibility") : (smokeMap ? "smoke autofocus" : "autofocus"),first==0,value,expected);
        if (!std::isfinite(value) || std::fabs(value-expected)>(sun ? .001f : .2f)) return false;
    }
    return true;
}


// Distinct coverage samples catch the old resolve-and-overwrite path. After the two
// Lens draws, replace one sample and resolve again: flattened samples give a wrong mean.
static bool MultisampleCompositeProbe(GpuFixture& gpu,const std::string& source) {
    using namespace Filmmaker::Lens;
    auto* d=gpu.device.Get();
    DWORD quality=0;
    if(FAILED(gpu.api->CheckDeviceMultiSampleType(0,D3DDEVTYPE_HAL,D3DFMT_A16B16G16R16F,TRUE,D3DMULTISAMPLE_4_SAMPLES,&quality))) return false;
    const char vertex[]="struct I{float4 p:POSITION0;float2 uv:TEXCOORD0;};struct O{float4 p:POSITION0;float2 uv:TEXCOORD0;};O main(I i){O o;o.p=i.p;o.uv=i.uv;return o;}";
    const char solid[]="float4 color:register(c0);float4 main():COLOR0{return color;}";
    ComPtr<ID3DBlob> vcode,plain;
    if(FAILED(D3DCompile(vertex,sizeof(vertex),nullptr,nullptr,nullptr,"main","vs_3_0",0,0,&vcode,nullptr))
        || FAILED(D3DCompile(solid,sizeof(solid),nullptr,nullptr,nullptr,"main","ps_3_0",0,0,&plain,nullptr))) return false;
    ComPtr<IDirect3DVertexShader9> vs;ComPtr<IDirect3DVertexDeclaration9> decl;
    ComPtr<IDirect3DPixelShader9> flat,factor,add;
    if(FAILED(d->CreateVertexShader((const DWORD*)vcode->GetBufferPointer(),&vs))
        || FAILED(d->CreatePixelShader((const DWORD*)plain->GetBufferPointer(),&flat))) return false;
    const D3DVERTEXELEMENT9 elements[]={{0,0,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_POSITION,0},{0,16,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},D3DDECL_END()};
    if(FAILED(d->CreateVertexDeclaration(elements,&decl))) return false;
    std::vector<SourceFilmmaker::LensShaders::Binding> bindings[2];
    for(int n=0;n<2;++n) {
        ComPtr<ID3DBlob> code,errors;
        if(FAILED(D3DCompile(source.data(),source.size(),nullptr,nullptr,nullptr,n ? "CompositeAddPS" : "CompositeFactorPS","ps_3_0",
            D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors))) return false;
        if(!SourceFilmmaker::LensShaders::ReadBindings(code->GetBufferPointer(),code->GetBufferSize(),bindings[n])) return false;
        if(FAILED(d->CreatePixelShader((const DWORD*)code->GetBufferPointer(),n ? &add : &factor))) return false;
    }
    ComPtr<IDirect3DSurface9> msaa,resolved,read;
    ComPtr<IDirect3DTexture9> gather;
    if(FAILED(d->CreateRenderTarget(16,16,D3DFMT_A16B16G16R16F,D3DMULTISAMPLE_4_SAMPLES,0,FALSE,&msaa,nullptr))
        || FAILED(d->CreateRenderTarget(16,16,D3DFMT_A16B16G16R16F,D3DMULTISAMPLE_NONE,0,FALSE,&resolved,nullptr))
        || FAILED(d->CreateOffscreenPlainSurface(16,16,D3DFMT_A16B16G16R16F,D3DPOOL_SYSTEMMEM,&read,nullptr))
        || FAILED(d->CreateTexture(1,1,1,0,D3DFMT_A32B32G32R32F,D3DPOOL_MANAGED,&gather,nullptr))) return false;
    D3DLOCKED_RECT lock{};
    if(FAILED(gather->LockRect(0,&lock,nullptr,0))) return false;
    float* value=(float*)lock.pBits;value[0]=value[1]=value[2]=std::pow(.2f,2.2f);value[3]=.5f;gather->UnlockRect(0);
    if(FAILED(d->SetRenderTarget(0,msaa.Get()))) return false;
    d->SetDepthStencilSurface(nullptr);D3DVIEWPORT9 vp{0,0,16,16,0,1};d->SetViewport(&vp);
    d->SetRenderState(D3DRS_ZENABLE,FALSE);d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);d->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE);
    d->SetVertexShader(vs.Get());d->SetVertexDeclaration(decl.Get());
    const float quad[4][6]={{-1,1,0,1,0,0},{1,1,0,1,1,0},{-1,-1,0,1,0,1},{1,-1,0,1,1,1}};
    auto draw=[&] {return SUCCEEDED(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(quad[0])));};
    if(FAILED(d->Clear(0,nullptr,D3DCLEAR_TARGET,D3DCOLOR_ARGB(77,153,153,153),1,0)) || FAILED(d->BeginScene())) return false;
    bool ok=true;
    d->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS,TRUE);d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);d->SetRenderState(D3DRS_COLORWRITEENABLE,15);
    d->SetPixelShader(flat.Get());
    const float color[4]={0,0,0,.3f};d->SetPixelShaderConstantF(0,color,1);
    d->SetRenderState(D3DRS_MULTISAMPLEMASK,1);ok=draw() && ok;
    d->EndScene();
    if(FAILED(d->StretchRect(msaa.Get(),nullptr,resolved.Get(),nullptr,D3DTEXF_NONE))
        || FAILED(d->GetRenderTargetData(resolved.Get(),read.Get())) || FAILED(read->LockRect(&lock,nullptr,D3DLOCK_READONLY))) return false;
    auto half=[](unsigned short v) {
        const unsigned e=(v>>10)&31,m=v&1023;
        const float n=e ? std::ldexp(1.f+m/1024.f,(int)e-15) : std::ldexp((float)m,-24);
        return v&0x8000 ? -n : n;
    };
    const auto* initial=(const unsigned short*)((const char*)lock.pBits+5*lock.Pitch)+6*4;
    const float baseline=half(initial[0]),baselineAlpha=half(initial[3]);read->UnlockRect();
    std::printf("D3D9 coverage fixture initial: mean=%.5f alpha=%.5f\n",baseline,baselineAlpha);
    if(std::fabs(baseline-.45f)>.002f || std::fabs(baselineAlpha-.3f)>.003f) return false;
    if(FAILED(d->BeginScene())) return false;
    GpuConstants k{};k.dims={16,16,16,16};k.modes={1,0,1,0};
    d->SetPixelShaderConstantF(0,(const float*)&k,sizeof(k)/16);
    d->SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffffu);d->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS,FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);d->SetRenderState(D3DRS_COLORWRITEENABLE,7);d->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);
    for(unsigned n=0;n<2;++n) {
        d->SetPixelShader(n ? add.Get() : factor.Get());
        d->SetRenderState(D3DRS_SRCBLEND,n ? D3DBLEND_ONE : D3DBLEND_ZERO);d->SetRenderState(D3DRS_DESTBLEND,n ? D3DBLEND_ONE : D3DBLEND_SRCCOLOR);
        for(const auto& b:bindings[n]) if(b.set==3) {
            d->SetTexture(b.first,b.name=="gatherTex" ? gather.Get() : nullptr);
            d->SetSamplerState(b.first,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(b.first,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
        }
        ok=draw() && ok;
    }
    d->SetPixelShader(flat.Get());d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);d->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS,TRUE);
    d->SetRenderState(D3DRS_MULTISAMPLEMASK,1);const float black[4]={0,0,0,.3f};d->SetPixelShaderConstantF(0,black,1);ok=draw() && ok;
    d->EndScene();d->SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffffu);
    if(!ok || FAILED(d->StretchRect(msaa.Get(),nullptr,resolved.Get(),nullptr,D3DTEXF_NONE))
        || FAILED(d->GetRenderTargetData(resolved.Get(),read.Get())) || FAILED(read->LockRect(&lock,nullptr,D3DLOCK_READONLY))) return false;
    const auto* pixel=(const unsigned short*)((const char*)lock.pBits+5*lock.Pitch)+6*4;
    const float actual=half(pixel[0]),alpha=half(pixel[3]);read->UnlockRect();
    // Samples are now 0, .4, .4, .4. A flattened composite would give .24375.
    std::printf("D3D9 4x MSAA composite: mean=%.5f expected=.3 alpha=%.5f expected=.3\n",actual,alpha);
    return std::fabs(actual-.3f)<.002f && std::fabs(alpha-.3f)<.003f;
}


static ComPtr<IDirect3DTexture9> SmokeCaptureProbe(GpuFixture& gpu) {
    using namespace SourceFilmmaker;
    auto* d=gpu.device.Get();
    const char vertex[]="float4 main(float4 p:POSITION0):POSITION0{return p;}";
    const char pixel[]="float4 color:register(c0);float4 main():COLOR0{return color;}";
    ComPtr<ID3DBlob> vc,pc;ComPtr<IDirect3DVertexShader9> vs;ComPtr<IDirect3DPixelShader9> ps;
    ComPtr<IDirect3DVertexDeclaration9> decl;
    if(FAILED(D3DCompile(vertex,sizeof(vertex),nullptr,nullptr,nullptr,"main","vs_3_0",0,0,&vc,nullptr))
        || FAILED(D3DCompile(pixel,sizeof(pixel),nullptr,nullptr,nullptr,"main","ps_3_0",0,0,&pc,nullptr))
        || FAILED(d->CreateVertexShader((const DWORD*)vc->GetBufferPointer(),&vs))
        || FAILED(d->CreatePixelShader((const DWORD*)pc->GetBufferPointer(),&ps))) return {};
    const D3DVERTEXELEMENT9 elements[]={{0,0,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_POSITION,0},D3DDECL_END()};
    if(FAILED(d->CreateVertexDeclaration(elements,&decl))) return {};
    ComPtr<IDirect3DTexture9> texture;ComPtr<IDirect3DSurface9> coverage,discard,depth,read;
    if(FAILED(d->CreateTexture(8,8,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,D3DPOOL_DEFAULT,&texture,nullptr))
        || FAILED(texture->GetSurfaceLevel(0,&coverage))
        || FAILED(d->CreateRenderTarget(8,8,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&discard,nullptr))
        || FAILED(d->CreateDepthStencilSurface(8,8,D3DFMT_D24S8,D3DMULTISAMPLE_NONE,0,TRUE,&depth,nullptr))
        || FAILED(d->CreateOffscreenPlainSurface(8,8,D3DFMT_A16B16G16R16F,D3DPOOL_SYSTEMMEM,&read,nullptr))) return {};
    d->SetDepthStencilSurface(nullptr);d->SetRenderTarget(0,discard.Get());d->SetDepthStencilSurface(depth.Get());
    D3DVIEWPORT9 vp{0,0,8,8,0,1};d->SetViewport(&vp);
    d->ColorFill(coverage.Get(),nullptr,0xffffffff);d->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xff336699,.5f,0);
    d->SetVertexShader(vs.Get());d->SetPixelShader(ps.Get());d->SetVertexDeclaration(decl.Get());
    d->SetRenderState(D3DRS_ZENABLE,TRUE);d->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);d->SetRenderState(D3DRS_COLORWRITEENABLE,15);
    d->SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffffu);d->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS,FALSE);
    LensSmoke_Begin(d,coverage.Get());LensSmoke_Material(true);
    if(FAILED(d->BeginScene())) return {};
    bool ok=true;
    for(int layer=0;layer<3;++layer) {
        const float alpha=layer==0 ? .5f : layer==1 ? .25f : 1.f,z=layer==2 ? .75f : .25f;
        const float color[4]={1,0,0,alpha},quad[4][4]={{-1,1,z,1},{1,1,z,1},{-1,-1,z,1},{1,-1,z,1}};
        d->SetPixelShaderConstantF(0,color,1);
        const bool capturing=LensSmoke_BeforeDraw(d);ok=ok && capturing;
        const HRESULT result=d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(quad[0]));
        if(capturing) LensSmoke_AfterDraw();ok=ok && SUCCEEDED(result);
    }
    d->EndScene();const unsigned draws=LensSmoke_End();
    ComPtr<IDirect3DSurface9> restored;d->GetRenderTarget(0,&restored);DWORD blend=TRUE;d->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend);
    if(!ok || draws!=3 || restored.Get()!=discard.Get() || blend!=FALSE || LensSmoke_BeforeDraw(d)
        || FAILED(d->GetRenderTargetData(coverage.Get(),read.Get()))) return {};
    D3DLOCKED_RECT lock{};if(FAILED(read->LockRect(&lock,nullptr,D3DLOCK_READONLY))) return {};
    const auto* value=(const unsigned short*)((const char*)lock.pBits+3*lock.Pitch)+3*4;
    // .375 is represented exactly by IEEE half (0x3600). The rear opaque particle must contribute nothing.
    const unsigned alpha=value[3];read->UnlockRect();
    std::printf("D3D9 smoke capture: 3 draws, transmittance half=%04x expected=3600, targets/blend restored\n",alpha);
    return alpha==0x3600 ? texture : ComPtr<IDirect3DTexture9>{};
}

int main(int argc,char**) {
    const bool hardware=argc>1;
    GpuFixture gpu(hardware);
    if (hardware && !gpu.device) { std::puts("No D3D9 HAL device available"); return 77; }
    const std::string source = SourceFilmmaker::LensShaders::Source();
    int failures = 0;
    auto smokeMap=hardware ? SmokeCaptureProbe(gpu) : ComPtr<IDirect3DTexture9>{};
    if(hardware && !smokeMap) {++failures;std::puts("D3D9 smoke coverage capture failed");}
    if(hardware && !MultisampleCompositeProbe(gpu,source)) { ++failures;std::puts("D3D9 per-sample Lens composite failed"); }
    Filmmaker::Lens::Values tuning;
    tuning[Filmmaker::Lens::DofQuality]=2;
    if (Filmmaker::Lens::TapCount(tuning)>256) { std::puts("SM3 iris reversal must cover the new tap count"); return 1; }
    for (const char* entry : SourceFilmmaker::LensShaders::Entries()) {
        ID3DBlob *code=nullptr,*error=nullptr;
        const HRESULT hr = D3DCompile(source.data(),source.size(),"SourceLens",nullptr,nullptr,entry,"ps_3_0",
            D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
        if (FAILED(hr)) {
            ++failures;
            std::printf("%s FAILED: %s\n",entry,error ? (const char*)error->GetBufferPointer() : "no compiler diagnostics");
        } else {
            std::vector<SourceFilmmaker::LensShaders::Binding> bindings;
            if (!SourceFilmmaker::LensShaders::ReadBindings(code->GetBufferPointer(),code->GetBufferSize(),bindings)) {
                ++failures; std::printf("%s has invalid shader bindings\n",entry);
            }
            std::printf("%s: %zu bytes; samplers:",entry,code->GetBufferSize());
            for (const auto& b : bindings) if (b.set==3) std::printf(" %s=s%u",b.name.c_str(),b.first);
            std::puts("");
            if (hardware) {
                ComPtr<IDirect3DPixelShader9> shader;
                const HRESULT made=gpu.device->CreatePixelShader((const DWORD*)code->GetBufferPointer(),&shader);
                if (FAILED(made)) { ++failures; std::printf("%s: CreatePixelShader failed %08x\n",entry,(unsigned)made); }
                else if ((std::string(entry)=="FocusPS" || std::string(entry)=="SunPS")
                    && (!ScalarProbe(gpu,shader.Get(),bindings,std::string(entry)=="SunPS")
                        || (smokeMap && !ScalarProbe(gpu,shader.Get(),bindings,std::string(entry)=="SunPS",smokeMap.Get())))) {
                    ++failures;std::puts("D3D9 scalar depth/smoke fixture failed");
                }
            }
            // Reject truncated comments and out-of-range strings without accepting partial output.
            if (SourceFilmmaker::LensShaders::ReadBindings(code->GetBufferPointer(),4,bindings) || !bindings.empty()) ++failures;
            std::vector<unsigned char> bad((unsigned char*)code->GetBufferPointer(),(unsigned char*)code->GetBufferPointer()+code->GetBufferSize());
            const uint32_t tag=0x42415443u;
            for (size_t n=4;n+32<bad.size();n+=4) {
                if (std::memcmp(bad.data()+n,&tag,4)) continue;
                uint32_t info=0;
                std::memcpy(&info,bad.data()+n+4+16,4);
                if (n+4+info+4<=bad.size()) {
                    const uint32_t outside=0xffffffffu;
                    std::memcpy(bad.data()+n+4+info,&outside,4);
                    if (SourceFilmmaker::LensShaders::ReadBindings(bad.data(),bad.size(),bindings) || !bindings.empty()) ++failures;
                }
                break;
            }
        }
        if (code) code->Release();
        if (error) error->Release();
    }
    // Both wrappers include exactly the same radial mapping. Check their real compiler inputs.
    const auto root=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    for(bool source : {true,false}) {
        const auto file=root/"Shaders"/(source ? "afx_source_fisheye_ps30.fxc" : "afx_fisheye_ps_5_0.fxc");
        ComPtr<ID3DBlob> code,errors;
        const HRESULT hr=D3DCompileFromFile(file.c_str(),nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,"main",source ? "ps_3_0" : "ps_5_0",
            D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
        if(FAILED(hr)) {++failures;std::printf("fisheye %s: %s\n",source ? "D3D9" : "D3D11",errors ? (const char*)errors->GetBufferPointer() : "no compiler output");}
        else if(source && hardware) {
            ComPtr<IDirect3DPixelShader9> shader;
            if(FAILED(gpu.device->CreatePixelShader((const DWORD*)code->GetBufferPointer(),&shader))) ++failures;
        }
    }
    return failures ? 1 : 0;
}
