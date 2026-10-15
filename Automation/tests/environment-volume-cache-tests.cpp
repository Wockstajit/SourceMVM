#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderPrograms.h"
#include <wrl/client.h>
#include <d3d11.h>
#include <iostream>
#include <cstring>
using Microsoft::WRL::ComPtr;
using namespace Filmmaker::Environment;
void Check(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
struct Image {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> view;
};
int main(){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)),"WARP device");
    const std::string fixture=R"(
float DepthFixture(Screen i,uint sample:SV_SampleIndex):SV_Target {
    float d=i.position.x<6?.2+sample*.04:(i.position.x<10?.975:1);
    if(i.position.y<4&&sample==0)d=1;
    return camera.w>0?1-d:d;
}
float4 VolumeFixture(Screen i):SV_Target {return float4(i.position.xy*.1,.2,.5+i.position.x*.05);}
float4 SkyFixture(Screen i):SV_Target {return float4(.1,i.position.xy*.03,i.position.x<2?-1:.7);}
)";
    const auto source=EnvironmentShaderSource()+fixture;
    auto reference=source;
    const std::string cached="return volumeGeometryTexture.Load(int3(q,0));";
    auto location=reference.find(cached);Check(location!=std::string::npos,"Find cache accessor");
    reference.replace(location,cached.size(),"float2 p=(q+.5)*dimensions.xy/volumeDimensions.xy;float d=ReadDepth(p);return float2(FogDistance(p,d),IsSky(d));");
    auto compile=[&](const std::string& text,const char* entry,const char* profile,unsigned samples){
        D3D_SHADER_MACRO macros[]={{"DEPTH_MS",samples>1?"1":"0"},{"COLOR_MS",samples>1?"1":"0"},{nullptr,nullptr}};
        ComPtr<ID3DBlob> code,error;auto hr=CompileEnvironmentShader(text.data(),text.size(),"VolumeCacheTest",macros,entry,profile,EnvironmentShaderFlags,&code,&error);
        if(FAILED(hr)&&error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());Check(SUCCEEDED(hr),"Compile volume test");return code;
    };
    auto pixel=[&](const std::string& text,const char* entry,unsigned samples){auto code=compile(text,entry,"ps_5_0",samples);
        ComPtr<ID3D11PixelShader> shader;Check(SUCCEEDED(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader)),"Pixel shader");return shader;};
    auto vsCode=compile(source,"ScreenVS","vs_5_0",1);ComPtr<ID3D11VertexShader> vertex;
    Check(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vertex)),"Screen vertex shader");
    auto layoutCode=compile(source,"VolumePS","ps_5_0",1);
    auto reflect=reinterpret_cast<HRESULT(WINAPI*)(LPCVOID,SIZE_T,REFIID,void**)>(GetProcAddress(EnvironmentShaderCompiler(),"D3DReflect"));
    ComPtr<ID3D11ShaderReflection> reflection;Check(reflect&&SUCCEEDED(reflect(layoutCode->GetBufferPointer(),layoutCode->GetBufferSize(),__uuidof(ID3D11ShaderReflection),&reflection)),"Reflect frame layout");
    auto layout=reflection->GetConstantBufferByName("FrameConstants");D3D11_SHADER_BUFFER_DESC bd{};Check(SUCCEEDED(layout->GetDesc(&bd)),"Frame layout");
    std::vector<float> frame(bd.Size/4);
    auto field=[&](const char* name){D3D11_SHADER_VARIABLE_DESC d{};Check(SUCCEEDED(layout->GetVariableByName(name)->GetDesc(&d)),name);return frame.data()+d.StartOffset/4;};
    for(const char* name:{"viewProjection","inverseViewProjection"})for(unsigned i=0;i<4;++i)field(name)[i*5]=1;
    for(unsigned i=0;i<4;++i)field("dimensions")[i]=16;
    field("volumeDimensions")[0]=field("volumeDimensions")[1]=4;field("volumeDimensions")[2]=1;
    field("depthRange")[1]=.95f;
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=bd.Size;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constants;Check(SUCCEEDED(device->CreateBuffer(&cb,nullptr,&constants)),"Frame buffer");
    auto image=[&](unsigned size,DXGI_FORMAT format,unsigned samples){
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=size;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=samples;
        d.Format=format;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;Image out;
        Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&out.texture))&&SUCCEEDED(device->CreateRenderTargetView(out.texture.Get(),nullptr,&out.target))&&
            SUCCEEDED(device->CreateShaderResourceView(out.texture.Get(),nullptr,&out.view)),"Fixture image");return out;
    };
    auto draw=[&](ID3D11PixelShader* ps,unsigned size,std::initializer_list<ID3D11RenderTargetView*> targets){
        ctx->OMSetRenderTargets(UINT(targets.size()),targets.begin(),nullptr);D3D11_VIEWPORT vp{0,0,float(size),float(size),0,1};ctx->RSSetViewports(1,&vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vertex.Get(),nullptr,0);ctx->PSSetShader(ps,nullptr,0);
        auto b=constants.Get();ctx->PSSetConstantBuffers(0,1,&b);ctx->Draw(3,0);ctx->OMSetRenderTargets(0,nullptr,nullptr);
    };
    auto read=[&](const Image& input){
        D3D11_TEXTURE2D_DESC d{};input.texture->GetDesc(&d);ComPtr<ID3D11Texture2D> single=input.texture;
        if(d.SampleDesc.Count>1){d.SampleDesc={1,0};single.Reset();Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&single)),"Resolve target");ctx->ResolveSubresource(single.Get(),0,input.texture.Get(),0,d.Format);}
        d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;
        Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&staging)),"Readback image");ctx->CopyResource(staging.Get(),single.Get());
        D3D11_MAPPED_SUBRESOURCE m{};Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)),"Readback map");
        std::vector<uint16_t> values(d.Width*d.Height*4);for(unsigned y=0;y<d.Height;++y)std::memcpy(values.data()+y*d.Width*4,static_cast<const char*>(m.pData)+y*m.RowPitch,d.Width*8);
        ctx->Unmap(staging.Get(),0);return values;
    };
    auto bind=[&](unsigned slot,const Image& input){auto v=input.view.Get();ctx->PSSetShaderResources(slot,1,&v);};
    for(unsigned samples:{1u,4u,8u}){
        auto depth=image(16,DXGI_FORMAT_R32_FLOAT,samples),color=image(16,DXGI_FORMAT_R16G16B16A16_FLOAT,samples);
        auto volume=image(4,DXGI_FORMAT_R16G16B16A16_FLOAT,1),sky=image(4,DXGI_FORMAT_R16G16B16A16_FLOAT,1),geometry=image(4,DXGI_FORMAT_R32G32_FLOAT,1);
        auto blur=image(4,DXGI_FORMAT_R16G16B16A16_FLOAT,1),skyBlur=image(4,DXGI_FORMAT_R16G16B16A16_FLOAT,1),composite=image(16,DXGI_FORMAT_R16G16B16A16_FLOAT,samples);
        auto depthPS=pixel(source,"DepthFixture",samples),marchPS=pixel(source,"VolumePS",samples),fill=pixel(source,"VolumeFixture",samples),skyFill=pixel(source,"SkyFixture",samples);
        auto blurPS=pixel(source,"VolumeBlurPS",samples),oldBlur=pixel(reference,"VolumeBlurPS",samples);
        auto compositePS=pixel(source,"CompositePS",samples),oldComposite=pixel(reference,"CompositePS",samples);
        for(bool reversed:{false,true}){
            ctx->ClearState();field("camera")[3]=reversed?1.f:0.f;ctx->UpdateSubresource(constants.Get(),0,nullptr,frame.data(),0,0);
            draw(depthPS.Get(),16,{depth.target.Get()});const float base[]{.2f,.3f,.4f,1};ctx->ClearRenderTargetView(color.target.Get(),base);
            bind(0,color);bind(1,depth);bind(2,depth);bind(3,color);
            draw(marchPS.Get(),4,{volume.target.Get(),geometry.target.Get()});
            draw(fill.Get(),4,{volume.target.Get()});draw(skyFill.Get(),4,{sky.target.Get()});
            bind(4,volume);bind(32,sky);bind(33,geometry);
            draw(oldBlur.Get(),4,{blur.target.Get(),skyBlur.target.Get()});auto expected=read(blur),expectedSky=read(skyBlur);
            draw(blurPS.Get(),4,{blur.target.Get(),skyBlur.target.Get()});
            Check(read(blur)==expected&&read(skyBlur)==expectedSky,"Cached blur matches repeated depth reads including sky/geometry boundaries");
            bind(4,blur);bind(32,skyBlur);
            draw(oldComposite.Get(),16,{composite.target.Get()});expected=read(composite);
            draw(compositePS.Get(),16,{composite.target.Get()});
            Check(read(composite)==expected,"Cached composite matches sample-frequency reference including minority sky samples");
        }
        std::cout<<"Volume cache matches reference at "<<samples<<" samples, normal and reversed depth\n";
    }
}
