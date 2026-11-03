// Render the real flare shaders against a narrow lamp. CPU mirrors alone cannot
// catch gaps between GPU taps, separated streak rails, or speckled ghost interiors.
#define NOMINMAX
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../AfxHookSource2/Filmmaker/Lens/LensFxShaders.h"
#include "../../AfxHookSource2/Filmmaker/Lens/LensFxConstants.h"
using Microsoft::WRL::ComPtr;
using namespace Filmmaker::Lens;

static void Require(bool ok,const char* label){if(!ok)throw std::runtime_error(label);}
static void Hr(HRESULT hr,const char* label){Require(SUCCEEDED(hr),label);}
struct Image {
    UINT w,h;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
};
int main(){try {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx),"D3D11 device");
    const std::string source=std::string(kShaderCommon)+kShaderDof+kShaderFlare+kShaderComposite;
    auto compile=[&](const char* entry,const char* profile){ComPtr<ID3DBlob> code,error;
        HRESULT result=D3DCompile(source.data(),source.size(),"LensFlareGpuTests",nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);
        if(FAILED(result)&&error)std::printf("%s\n",static_cast<const char*>(error->GetBufferPointer()));
        Hr(result,entry);return code;};
    auto vertexCode=compile("VS","vs_5_0");ComPtr<ID3D11VertexShader> vs;
    Hr(device->CreateVertexShader(vertexCode->GetBufferPointer(),vertexCode->GetBufferSize(),nullptr,&vs),"Vertex shader");
    auto pixel=[&](const char* entry){auto code=compile(entry,"ps_5_0");ComPtr<ID3D11PixelShader> ps;
        Hr(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps),entry);return ps;};
    auto sourceH=pixel("SourceHPS"),streak=pixel("StreakPS"),aperture=pixel("ApertureBlurPS");
    // Compile the remaining affected pipeline as well, including composite bindings.
    for(const char* entry:{"BrightPS","Down13PS","SourcePS","GhostPS","GlowHPS","GlowVPS","CompositePS"})pixel(entry);
    // The depth-of-field passes share the constant buffer and the smoke lookup: they must compile too.
    for(const char* entry:{"FocusPS","CocPS","SunPS","MipPS","TilePS","DilateHPS","DilateVPS","GatherPS","DenoisePS"})pixel(entry);
    auto image=[&](UINT w,UINT h,bool mips=false){Image im{w,h};D3D11_TEXTURE2D_DESC td{};
        td.Width=w;td.Height=h;td.MipLevels=mips?0:1;td.ArraySize=td.SampleDesc.Count=1;
        td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags=mips?D3D11_RESOURCE_MISC_GENERATE_MIPS:0;
        Hr(device->CreateTexture2D(&td,nullptr,&im.tex),"Texture");Hr(device->CreateShaderResourceView(im.tex.Get(),nullptr,&im.srv),"SRV");
        Hr(device->CreateRenderTargetView(im.tex.Get(),nullptr,&im.rtv),"RTV");return im;};
    auto lamp=[&](Image& im){std::vector<float> data(im.w*im.h*4,0);const auto at=(im.h/2*im.w+im.w/2)*4;
        data[at]=data[at+1]=data[at+2]=1;ctx->UpdateSubresource(im.tex.Get(),0,nullptr,data.data(),im.w*16,0);};
    // Same packing as LensConstants: matrix followed by 17 float4 records.
    float constants[21][4]{};
    constants[4][0]=1920;constants[4][1]=1080;
    constants[12][0]=1;constants[12][2]=2;
    constants[13][0]=1;constants[13][1]=.9f;constants[13][3]=6;
    constants[16][0]=constants[16][1]=constants[16][2]=1;
    constants[17][0]=480;constants[17][1]=270;constants[17][2]=240;constants[17][3]=135;
    constants[19][0]=1.f/480;constants[19][1]=1.f/270;
    constants[20][2]=1920.f/1080;
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(constants);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{constants,0,0};ComPtr<ID3D11Buffer> cb;Hr(device->CreateBuffer(&bd,&initial,&cb),"Constants");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;Hr(device->CreateSamplerState(&sd,&sampler),"Sampler");
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=true;
    ComPtr<ID3D11RasterizerState> raster;Hr(device->CreateRasterizerState(&rd,&raster),"Rasterizer");
    ctx->RSSetState(raster.Get());ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vs.Get(),nullptr,0);
    auto buffer=cb.Get();ctx->PSSetConstantBuffers(0,1,&buffer);auto sam=sampler.Get();ctx->PSSetSamplers(0,1,&sam);
    auto draw=[&](Image& out,ID3D11PixelShader* ps,Image& input,UINT slot){ID3D11ShaderResourceView* empty[20]{};ctx->PSSetShaderResources(0,20,empty);
        auto rtv=out.rtv.Get();ctx->OMSetRenderTargets(1,&rtv,nullptr);D3D11_VIEWPORT vp{0,0,float(out.w),float(out.h),0,1};ctx->RSSetViewports(1,&vp);
        auto srv=input.srv.Get();ctx->PSSetShaderResources(slot,1,&srv);ctx->PSSetShader(ps,nullptr,0);ctx->Draw(3,0);
        ctx->PSSetShaderResources(0,20,empty);ctx->OMSetRenderTargets(0,nullptr,nullptr);};
    auto read=[&](Image& im){D3D11_TEXTURE2D_DESC td{};im.tex->GetDesc(&td);td.MipLevels=1;td.BindFlags=td.MiscFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;Hr(device->CreateTexture2D(&td,nullptr,&staging),"Readback");ctx->CopySubresourceRegion(staging.Get(),0,0,0,0,im.tex.Get(),0,nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};Hr(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Map");std::vector<float> result(im.w*im.h);
        for(UINT y=0;y<im.h;++y){auto row=reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch);for(UINT x=0;x<im.w;++x)result[y*im.w+x]=row[x*4];}
        ctx->Unmap(staging.Get(),0);return result;};

    // Exercise the real CS2 sample-frequency branch, including a lifted highlight that
    // clamps some samples but not others. Resolving before the composite would fail it.
    {
        const D3D_SHADER_MACRO defs[]={{"DEPTH_MS","1"},{"COLOR_MS","1"},{nullptr,nullptr}};
        ComPtr<ID3DBlob> msCode,errors;
        for(const char* entry:{"FocusPS","CocPS","SunPS","CompositePS"}) {
            const HRESULT hr=D3DCompile(source.data(),source.size(),"LensMsaa",defs,nullptr,entry,"ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&msCode,&errors);
            if(FAILED(hr)&&errors)std::printf("%s\n",(const char*)errors->GetBufferPointer());
            Hr(hr,entry);
        }
        ComPtr<ID3D11PixelShader> composite,seed;
        Hr(device->CreatePixelShader(msCode->GetBufferPointer(),msCode->GetBufferSize(),nullptr,&composite),"MSAA composite");
        const char seedSource[]="float4 main(uint sample:SV_SampleIndex):SV_Target{return float4((sample/3.0).xxx,.3);}";
        Hr(D3DCompile(seedSource,sizeof(seedSource),"CoverageSeed",nullptr,nullptr,"main","ps_5_0",0,0,&msCode,nullptr),"Coverage seed compile");
        Hr(device->CreatePixelShader(msCode->GetBufferPointer(),msCode->GetBufferSize(),nullptr,&seed),"Coverage seed shader");
        auto msImage=[&]{Image im{8,8};D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=8;td.MipLevels=td.ArraySize=1;
            td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.SampleDesc.Count=4;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            Hr(device->CreateTexture2D(&td,nullptr,&im.tex),"MSAA texture");Hr(device->CreateRenderTargetView(im.tex.Get(),nullptr,&im.rtv),"MSAA RTV");
            Hr(device->CreateShaderResourceView(im.tex.Get(),nullptr,&im.srv),"MSAA SRV");return im;};
        auto original=msImage(),target=msImage(),resolved=image(8,8),gather=image(8,8),coc=image(8,8),ghost=image(8,8);
        D3D11_VIEWPORT vp{0,0,8,8,0,1};ctx->RSSetViewports(1,&vp);
        auto originalRtv=original.rtv.Get();ctx->OMSetRenderTargets(1,&originalRtv,nullptr);ctx->PSSetShader(seed.Get(),nullptr,0);ctx->Draw(3,0);
        ctx->OMSetRenderTargets(0,nullptr,nullptr);
        const float black[4]={},gray[4]={.5f,.5f,.5f,1};ctx->ClearRenderTargetView(coc.rtv.Get(),black);ctx->ClearRenderTargetView(ghost.rtv.Get(),gray);
        GpuConstants k{};k.dims={8,8,8,8};k.lowRes={8,8,8,8};k.modes={1,0,1,0};k.flareA.x=1;k.flareTint={1,1,1,0};
        D3D11_BUFFER_DESC kd{};kd.ByteWidth=sizeof(k);kd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> fullConstants;Hr(device->CreateBuffer(&kd,nullptr,&fullConstants),"Composite constants");
        auto constant=fullConstants.Get();ctx->PSSetConstantBuffers(0,1,&constant);
        D3D11_TEXTURE2D_DESC stage{};resolved.tex->GetDesc(&stage);stage.BindFlags=0;stage.Usage=D3D11_USAGE_STAGING;stage.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;Hr(device->CreateTexture2D(&stage,nullptr,&staging),"Composite readback");
        for(int test=0;test<4;++test) {
            const float linear=std::pow(test==2 ? 1.6f : .2f,2.2f),g[4]={linear,linear,linear,test==0 ? 0.f : .5f};
            ctx->ClearRenderTargetView(gather.rtv.Get(),g);k.modes.y=test==3 ? 1.f : 0.f;ctx->UpdateSubresource(fullConstants.Get(),0,nullptr,&k,0,0);
            auto rtv=target.rtv.Get();ctx->OMSetRenderTargets(1,&rtv,nullptr);
            ID3D11ShaderResourceView* inputs[23]{};inputs[4]=coc.srv.Get();inputs[6]=gather.srv.Get();inputs[10]=ghost.srv.Get();inputs[13]=original.srv.Get();
            ctx->PSSetShaderResources(0,23,inputs);ctx->PSSetShader(composite.Get(),nullptr,0);ctx->Draw(3,0);
            ID3D11ShaderResourceView* empty[23]{};ctx->PSSetShaderResources(0,23,empty);ctx->OMSetRenderTargets(0,nullptr,nullptr);
            ctx->ResolveSubresource(resolved.tex.Get(),0,target.tex.Get(),0,DXGI_FORMAT_R32G32B32A32_FLOAT);ctx->CopyResource(staging.Get(),resolved.tex.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};Hr(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Composite sample map");
            const float* pixel=(const float*)mapped.pData;const float value=pixel[0],alpha=pixel[3];ctx->Unmap(staging.Get(),0);
            const float expected=test==0 ? .5f : test==1 ? .35f : test==2 ? (float)((.8+(.8+.5/3)+1+1)/4) : 1-.65f*std::exp(-.5f);
            std::printf("D3D11 4x MSAA composite case=%d actual=%.6f expected=%.6f alpha=%.6f\n",test,value,expected,alpha);
            Require(std::fabs(value-expected)<.0001f && std::fabs(alpha-.3f)<.0001f,"Per-sample DOF/flare, highlight clamp and alpha");
        }
        ctx->PSSetConstantBuffers(0,1,&buffer);
    }

    auto q=image(480,270),h=image(240,540),line=image(960,540);lamp(q);
    draw(h,sourceH.Get(),q,17);draw(line,streak.Get(),h,19);auto pixels=read(line);
    const UINT cy=271,cx=481;float worstRipple=0;
    for(UINT x=100;x<850;++x){if(std::abs(int(x)-int(cx))<24)continue;
        float a=pixels[cy*line.w+x-1],b=pixels[cy*line.w+x],c=pixels[cy*line.w+x+1];
        if(b>.01f)worstRipple=std::max(worstRipple,std::abs(a-2*b+c)/b);}
    std::printf("streak relative ripple=%.5f\n",worstRipple);
    Require(worstRipple<.055f,"Narrow lamp streak has no sample bars");
    // Cross-width must have one peak, with no secondary rails six pixels away.
    UINT peak=250;for(UINT y=251;y<290;++y)if(pixels[y*line.w+700]>pixels[peak*line.w+700])peak=y;
    Require(pixels[peak*line.w+700]>.01f,"Streak is visible");
    for(UINT y=peak+1;y<peak+12;++y)Require(pixels[y*line.w+700]<=pixels[(y-1)*line.w+700]+.0005f,"Smooth upper streak edge");
    for(UINT y=peak-1;y>peak-12;--y)Require(pixels[y*line.w+700]<=pixels[(y+1)*line.w+700]+.0005f,"Smooth lower streak edge");
    auto e=image(240,135,true),ghost=image(480,270);lamp(e);ctx->GenerateMips(e.srv.Get());draw(ghost,aperture.Get(),e,18);pixels=read(ghost);
    float low=1,high=0;
    for(UINT y=129;y<139;++y)for(UINT x=235;x<245;++x){float value=pixels[y*ghost.w+x];low=std::min(low,value);high=std::max(high,value);}
    std::printf("ghost interior min=%.5f max=%.5f\n",low,high);
    Require(low>.015f&&low/high>.60f,"Point-source ghost has a solid interior");
    std::puts("Lens flare GPU checks passed");return 0;
}catch(const std::exception& e){std::printf("FAIL: %s\n",e.what());return 1;}}
