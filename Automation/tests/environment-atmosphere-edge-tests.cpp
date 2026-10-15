#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderPrograms.h"
#include <wrl/client.h>
#include <d3d11sdklayers.h>
#include <cstring>
#include <iostream>
using Microsoft::WRL::ComPtr;
using namespace Filmmaker::Environment;
void Check(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
struct Image {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> view;
};
int main(int argc,char** argv){
    bool hardware=argc>1&&std::string(argv[1])=="--hardware";
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    auto type=hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP;
    auto hr=D3D11CreateDevice(nullptr,type,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx);
    if(hr==DXGI_ERROR_SDK_COMPONENT_MISSING)hr=D3D11CreateDevice(nullptr,type,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx);
    Check(SUCCEEDED(hr),"D3D11 device");ComPtr<ID3D11InfoQueue> debug;device.As(&debug);
    const std::string fixture=R"(
float DepthFixture(Screen i,uint sample:SV_SampleIndex):SV_Target {
    uint cell=(uint)i.position.x%8;
    float d=.2;
    if(cell==1)d+=sample*.0002;
    if(cell==2)d+=sample*.008;
    if(cell==3&&sample==7)d=1;
    if(cell==4&&sample==0)d=1;
    if(cell==5)d=.975;
    if(cell==6)d=1;
    if(cell==7)d=.05+sample*.11;
    return camera.w>0?1-d:d;
}
float4 ColorFixture(Screen i,uint sample:SV_SampleIndex):SV_Target {
    return float4(.12+i.position.x*.01+sample*.015,.2+i.position.y*.007,.3+sample*.02,1);
}
float4 ExtractSamples(Screen i):SV_Target {
    uint x=(uint)i.position.x, count=(uint)lighting.w;
#if COLOR_MS
    return colorTexture.Load(int2(x/count,(uint)i.position.y),x%count);
#else
    return colorTexture.Load(int3(x,(uint)i.position.y,0));
#endif
}
)";
    const auto source=EnvironmentShaderSource()+fixture;
    auto reference=source;
    const std::string cache="[branch]if(lighting.z>.5)return atmosphereEdgeTexture.Load(int3(int2(pixel),0))!=0;";
    auto at=reference.find(cache);Check(at!=std::string::npos,"Reference edge accessor");reference.erase(at,cache.size());
    auto compile=[&](const std::string& text,const char* entry,const char* profile,unsigned depthSamples,unsigned colorSamples){
        D3D_SHADER_MACRO macros[]={{"DEPTH_MS",depthSamples>1?"1":"0"},{"COLOR_MS",colorSamples>1?"1":"0"},{nullptr,nullptr}};
        ComPtr<ID3DBlob> code,error;auto result=CompileEnvironmentShader(text.data(),text.size(),"AtmosphereEdgeTest",macros,entry,profile,EnvironmentShaderFlags,&code,&error);
        if(FAILED(result)&&error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());Check(SUCCEEDED(result),entry);return code;
    };
    auto pixel=[&](const std::string& text,const char* entry,unsigned ds,unsigned cs){auto code=compile(text,entry,"ps_5_0",ds,cs);
        ComPtr<ID3D11PixelShader> shader;Check(SUCCEEDED(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader)),"Pixel shader");return shader;};
    auto vertexCode=compile(source,"ScreenVS","vs_5_0",1,1);ComPtr<ID3D11VertexShader> vertex;
    Check(SUCCEEDED(device->CreateVertexShader(vertexCode->GetBufferPointer(),vertexCode->GetBufferSize(),nullptr,&vertex)),"Screen vertex");
    auto layoutCode=compile(source,"AtmospherePS","ps_5_0",1,1);
    auto reflect=reinterpret_cast<HRESULT(WINAPI*)(LPCVOID,SIZE_T,REFIID,void**)>(GetProcAddress(EnvironmentShaderCompiler(),"D3DReflect"));
    ComPtr<ID3D11ShaderReflection> reflection;Check(reflect&&SUCCEEDED(reflect(layoutCode->GetBufferPointer(),layoutCode->GetBufferSize(),__uuidof(ID3D11ShaderReflection),&reflection)),"Reflect frame");
    auto layout=reflection->GetConstantBufferByName("FrameConstants");D3D11_SHADER_BUFFER_DESC bd{};Check(SUCCEEDED(layout->GetDesc(&bd)),"Frame layout");
    std::vector<float> frame(bd.Size/4);
    auto field=[&](const char* name){D3D11_SHADER_VARIABLE_DESC d{};Check(SUCCEEDED(layout->GetVariableByName(name)->GetDesc(&d)),name);return frame.data()+d.StartOffset/4;};
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=bd.Size;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constants;Check(SUCCEEDED(device->CreateBuffer(&cb,nullptr,&constants)),"Frame constants");
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=rd.MultisampleEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;Check(SUCCEEDED(device->CreateRasterizerState(&rd,&raster)),"Raster state");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;Check(SUCCEEDED(device->CreateSamplerState(&sd,&sampler)),"Sampler");
    auto image=[&](unsigned width,unsigned height,DXGI_FORMAT format,unsigned samples){
        D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=samples;
        d.Format=format;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;Image out;
        Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&out.texture))&&SUCCEEDED(device->CreateRenderTargetView(out.texture.Get(),nullptr,&out.target))&&
            SUCCEEDED(device->CreateShaderResourceView(out.texture.Get(),nullptr,&out.view)),"Image");return out;
    };
    auto draw=[&](ID3D11PixelShader* ps,Image& target){D3D11_TEXTURE2D_DESC d{};target.texture->GetDesc(&d);
        auto r=target.target.Get();ctx->OMSetRenderTargets(1,&r,nullptr);D3D11_VIEWPORT vp{0,0,float(d.Width),float(d.Height),0,1};ctx->RSSetViewports(1,&vp);ctx->RSSetState(raster.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vertex.Get(),nullptr,0);ctx->PSSetShader(ps,nullptr,0);
        auto b=constants.Get();ctx->PSSetConstantBuffers(0,1,&b);ctx->Draw(3,0);ctx->OMSetRenderTargets(0,nullptr,nullptr);
    };
    auto read=[&](const Image& input){D3D11_TEXTURE2D_DESC d{};input.texture->GetDesc(&d);Check(d.SampleDesc.Count==1,"Read single samples");
        d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;
        Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&staging)),"Readback");ctx->CopyResource(staging.Get(),input.texture.Get());D3D11_MAPPED_SUBRESOURCE m{};
        Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)),"Map");std::vector<uint16_t> values(d.Width*d.Height*4);
        for(unsigned y=0;y<d.Height;++y)std::memcpy(values.data()+y*d.Width*4,static_cast<const char*>(m.pData)+y*m.RowPitch,d.Width*8);ctx->Unmap(staging.Get(),0);return values;
    };
    auto bind=[&](unsigned slot,const Image& input){auto v=input.view.Get();ctx->PSSetShaderResources(slot,1,&v);};
    unsigned comparisons=0;
    for(unsigned samples:{1u,4u,8u})for(unsigned depthSamples:{1u,samples}){
        if(samples==1&&depthSamples==1&&comparisons)continue;
        auto depthPS=pixel(source,"DepthFixture",depthSamples,samples),colorPS=pixel(source,"ColorFixture",depthSamples,samples);
        auto edgePS=pixel(source,"AtmosphereEdgePS",depthSamples,samples),cachedPS=pixel(source,"AtmospherePS",depthSamples,samples),cachedPixel=pixel(source,"AtmospherePixelPS",depthSamples,samples);
        auto oldPS=pixel(reference,"AtmospherePS",depthSamples,samples),oldPixel=pixel(reference,"AtmospherePixelPS",depthSamples,samples),extract=pixel(source,"ExtractSamples",depthSamples,samples);
        for(unsigned depthSize:{10u,16u})for(bool reversed:{false,true})for(unsigned effect=0;effect<5;++effect){
            ctx->ClearState();std::fill(frame.begin(),frame.end(),0.f);
            for(const char* name:{"viewProjection","inverseViewProjection"})for(unsigned i=0;i<4;++i)field(name)[i*5]=1;
            field("dimensions")[0]=field("dimensions")[1]=16;field("dimensions")[2]=field("dimensions")[3]=float(depthSize);
            field("camera")[3]=reversed?1.f:0.f;field("depthRange")[1]=.95f;
            field("lighting")[2]=samples>1&&depthSamples>1?1.f:0.f;field("lighting")[3]=float(samples);
            for(unsigned i=0;i<4;++i)field("tintExposure")[i]=1;
            if(effect==1){field("climateData")[CL_wet]=.7f;field("climateData")[CL_snow]=.5f;field("climateData")[CL_puddles]=.4f;field("viewmodelInfo")[2]=1;}
            if(effect==2){field("weatherParams")[2]=1;field("weatherColor")[3]=.6f;field("weatherCenterRadius")[3]=200;field("viewmodelInfo")[2]=1;}
            if(effect==3){field("relightDebug")[0]=1;field("shadowDebug")[0]=1;field("viewmodelInfo")[2]=1;field("viewmodelInfo")[3]=1;}
            if(effect==4){field("relightSky")[3]=1;field("climateData")[CL_sky]=.5f;field("climateData")[CL_sunRelight]=.7f;field("viewmodelInfo")[2]=1;}
            ctx->UpdateSubresource(constants.Get(),0,nullptr,frame.data(),0,0);
            auto depth=image(depthSize,depthSize,DXGI_FORMAT_R32_FLOAT,depthSamples),color=image(16,16,DXGI_FORMAT_R16G16B16A16_FLOAT,samples);
            auto mask=image(16,16,DXGI_FORMAT_R8_UINT,1),output=image(16,16,DXGI_FORMAT_R16G16B16A16_FLOAT,samples),flat=image(16*samples,16,DXGI_FORMAT_R16G16B16A16_FLOAT,1);
            draw(depthPS.Get(),depth);draw(colorPS.Get(),color);bind(1,depth);bind(3,color);
            for(unsigned i=0;i<5;++i){auto s=sampler.Get();ctx->PSSetSamplers(i,1,&s);}
            const float sentinel[]{-12,-13,-14,-15};ctx->ClearRenderTargetView(output.target.Get(),sentinel);
            if(samples>1)draw(oldPixel.Get(),output);draw(oldPS.Get(),output);bind(0,output);draw(extract.Get(),flat);auto expected=read(flat);
            for(size_t i=0;i<expected.size();++i){
                Check((expected[i]&0x7c00)!=0x7c00,"Reference samples remain finite");
                if(i%4==3)Check(expected[i]==0x3c00,"Both passes cover every MSAA sample");
            }
            if(effect==0&&depthSize==16&&samples>1&&depthSamples>1)
                Check(expected[(7*samples)*4]!=expected[(7*samples+1)*4],"Edge samples keep distinct source colors");
            ID3D11ShaderResourceView* none=nullptr;ctx->PSSetShaderResources(0,1,&none);
            if(field("lighting")[2]>0){draw(edgePS.Get(),mask);bind(34,mask);}
            ctx->ClearRenderTargetView(output.target.Get(),sentinel);if(samples>1)draw(cachedPixel.Get(),output);draw(cachedPS.Get(),output);
            bind(0,output);draw(extract.Get(),flat);Check(read(flat)==expected,"Every cached MSAA sample matches original shading bitwise");++comparisons;
        }
    }
    if(debug)for(UINT64 i=0;i<debug->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;debug->GetMessage(i,nullptr,&bytes);std::vector<char> memory(bytes);
        auto m=reinterpret_cast<D3D11_MESSAGE*>(memory.data());debug->GetMessage(i,m,&bytes);Check(m->Severity>D3D11_MESSAGE_SEVERITY_ERROR,m->pDescription);}
    std::cout<<comparisons<<" bitwise GPU comparisons passed on "<<(hardware?"hardware":"WARP")<<"; 1x/4x/8x MSAA, normal/reversed depth, different dimensions, sky, coatings, rain, diagnostics and compressed HDR\n";
}
