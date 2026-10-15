#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentDepthRead.h"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <array>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace Filmmaker::Environment;
void Check(bool value,const char* message){if(!value){std::cerr<<message<<'\n';std::exit(1);}}
int main(int argc,char** argv){
    bool hardware=argc>1&&std::string(argv[1])=="--hardware";
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL level;
    auto type=hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    auto hr=D3D11CreateDevice(nullptr,type,nullptr,D3D11_CREATE_DEVICE_DEBUG,levels,2,D3D11_SDK_VERSION,&device,&level,&ctx);
    if(hr==DXGI_ERROR_SDK_COMPONENT_MISSING)hr=D3D11CreateDevice(nullptr,type,nullptr,0,levels,2,D3D11_SDK_VERSION,&device,&level,&ctx);
    Check(SUCCEEDED(hr),"Create D3D11 device");ComPtr<ID3D11InfoQueue> debug;device.As(&debug);
    auto compile=[&](const std::string& text,const char* profile){ComPtr<ID3DBlob> code,error;
        auto result=D3DCompile(text.data(),text.size(),"DepthReadTest",nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);
        if(FAILED(result)&&error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());Check(SUCCEEDED(result),"Compile GPU fixture");return code;};
    auto vertexCode=compile("float4 main(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,.5,1);}","vs_5_0");
    auto pixelCode=compile(R"(
cbuffer Values:register(b0){float4 settings;}
float main(float4 p:SV_Position,uint sample:SV_SampleIndex):SV_Depth{
    if(((uint)p.x+sample)%5==0)discard;
    float d=.08+.09*sample+.001*p.y+settings.x;
    return settings.y>0?1-d:d;
})","ps_5_0");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    Check(SUCCEEDED(device->CreateVertexShader(vertexCode->GetBufferPointer(),vertexCode->GetBufferSize(),nullptr,&vs))&&SUCCEEDED(device->CreatePixelShader(pixelCode->GetBufferPointer(),pixelCode->GetBufferSize(),nullptr,&ps)),"Create depth shaders");
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> constants;Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&constants)),"Depth constants");
    D3D11_DEPTH_STENCIL_DESC stateDesc{};stateDesc.DepthEnable=TRUE;stateDesc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;stateDesc.DepthFunc=D3D11_COMPARISON_ALWAYS;
    ComPtr<ID3D11DepthStencilState> depthState;Check(SUCCEEDED(device->CreateDepthStencilState(&stateDesc,&depthState)),"Depth state");
    struct Format{DXGI_FORMAT typeless,depth,read;};
    const Format formats[]{{DXGI_FORMAT_R16_TYPELESS,DXGI_FORMAT_D16_UNORM,DXGI_FORMAT_R16_UNORM},{DXGI_FORMAT_R24G8_TYPELESS,DXGI_FORMAT_D24_UNORM_S8_UINT,DXGI_FORMAT_R24_UNORM_X8_TYPELESS},{DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_D32_FLOAT,DXGI_FORMAT_R32_FLOAT},{DXGI_FORMAT_R32G8X24_TYPELESS,DXGI_FORMAT_D32_FLOAT_S8X24_UINT,DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS}};
    unsigned cases=0;
    for(const auto& format:formats)for(unsigned samples:{1u,4u,8u}){
        UINT quality=0;Check(SUCCEEDED(device->CheckMultisampleQualityLevels(format.depth,samples,&quality))&&quality>0,"Requested depth MSAA support");
        DepthReadTarget reader;
        // Cross source identity, dimensions, direct->copy and copy->direct transitions.
        for(unsigned input=0;input<4;++input){const bool direct=input!=1;const unsigned width=input==2?12:8,height=8;
            D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;desc.SampleDesc.Count=samples;
            desc.Format=direct?format.typeless:format.depth;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL|(direct?D3D11_BIND_SHADER_RESOURCE:0);
            ComPtr<ID3D11Texture2D> depth;ComPtr<ID3D11DepthStencilView> dsv;
            D3D11_DEPTH_STENCIL_VIEW_DESC vd{};vd.Format=format.depth;vd.ViewDimension=samples>1?D3D11_DSV_DIMENSION_TEXTURE2DMS:D3D11_DSV_DIMENSION_TEXTURE2D;
            Check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&depth))&&SUCCEEDED(device->CreateDepthStencilView(depth.Get(),&vd,&dsv)),"Native depth target");
            auto referenceDesc=desc;referenceDesc.Format=format.typeless;referenceDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> reference;ComPtr<ID3D11ShaderResourceView> referenceView;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=format.read;sd.ViewDimension=samples>1?D3D11_SRV_DIMENSION_TEXTURE2DMS:D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
            Check(SUCCEEDED(device->CreateTexture2D(&referenceDesc,nullptr,&reference))&&SUCCEEDED(device->CreateShaderResourceView(reference.Get(),&sd,&referenceView)),"Reference clone");
            auto colorDesc=desc;colorDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;colorDesc.BindFlags=D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> color;ComPtr<ID3D11RenderTargetView> rtv;
            Check(SUCCEEDED(device->CreateTexture2D(&colorDesc,nullptr,&color))&&SUCCEEDED(device->CreateRenderTargetView(color.Get(),nullptr,&rtv)),"Native color target");
            bd={};bd.ByteWidth=width*height*samples*4;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=4;
            ComPtr<ID3D11Buffer> output,staging,pixelBuffer,counter,counterStaging;ComPtr<ID3D11UnorderedAccessView> uav,pixelUav;
            Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&output))&&SUCCEEDED(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav)),"Depth readback output");
            D3D11_UNORDERED_ACCESS_VIEW_DESC pixelDesc{};pixelDesc.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
            pixelDesc.Buffer.NumElements=width*height*samples;pixelDesc.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_COUNTER;
            Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&pixelBuffer))&&SUCCEEDED(device->CreateUnorderedAccessView(pixelBuffer.Get(),&pixelDesc,&pixelUav)),"Existing pixel UAV counter");
            D3D11_BUFFER_DESC counterDesc{};counterDesc.ByteWidth=4;
            Check(SUCCEEDED(device->CreateBuffer(&counterDesc,nullptr,&counter)),"Counter copy target");
            counterDesc.Usage=D3D11_USAGE_STAGING;counterDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            Check(SUCCEEDED(device->CreateBuffer(&counterDesc,nullptr,&counterStaging)),"Counter readback");
            bd.BindFlags=bd.MiscFlags=bd.StructureByteStride=0;bd.Usage=D3D11_USAGE_STAGING;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&staging)),"Depth readback staging");
            std::string readShader=samples>1?"Texture2DMS<float,"+std::to_string(samples)+"> source:register(t0);":"Texture2D<float> source:register(t0);";
            readShader+="RWStructuredBuffer<uint> result:register(u0);[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){if(p.x>="+std::to_string(width)+"||p.y>=8)return;";
            for(unsigned s=0;s<samples;++s)readShader+="result[(p.y*"+std::to_string(width)+"+p.x)*"+std::to_string(samples)+"+"+std::to_string(s)+"]=asuint(source.Load("+(samples>1?"p.xy,"+std::to_string(s):"int3(p.xy,0)")+"));";
            readShader+="}";auto readCode=compile(readShader,"cs_5_0");ComPtr<ID3D11ComputeShader> cs;Check(SUCCEEDED(device->CreateComputeShader(readCode->GetBufferPointer(),readCode->GetBufferSize(),nullptr,&cs)),"Read shader");
            auto read=[&](ID3D11ShaderResourceView* view){auto target=uav.Get();ctx->CSSetShaderResources(0,1,&view);ctx->CSSetUnorderedAccessViews(0,1,&target,nullptr);ctx->CSSetShader(cs.Get(),nullptr,0);ctx->Dispatch((width+7)/8,1,1);
                view=nullptr;target=nullptr;ctx->CSSetShaderResources(0,1,&view);ctx->CSSetUnorderedAccessViews(0,1,&target,nullptr);
                ctx->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"Map depth values");
                std::vector<uint32_t> values(width*height*samples);std::memcpy(values.data(),mapped.pData,values.size()*4);ctx->Unmap(staging.Get(),0);return values;};
            Check(reader.Prepare(device.Get(),depth.Get()),"Prepare current depth view");Check(reader.Direct()==direct,"Readable native depth uses direct view, depth-only texture uses fallback");
            ComPtr<ID3D11Resource> viewed;reader.View()->GetResource(&viewed);Check((viewed.Get()==depth.Get())==direct,"Direct mode has no owned depth clone");
            const UINT pixelSlot=level>=D3D_FEATURE_LEVEL_11_1&&!(input&1)?63:1;
            std::vector<uint32_t> prior;
            for(unsigned update=0;update<4;++update){const bool reversed=update&1;
                ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,reversed?0.f:1.f,0);auto target=rtv.Get();auto pixel=pixelUav.Get();UINT keep=17;
                ctx->OMSetRenderTargetsAndUnorderedAccessViews(1,&target,dsv.Get(),pixelSlot,1,&pixel,&keep);ctx->OMSetDepthStencilState(depthState.Get(),71);ctx->OMSetBlendState(nullptr,nullptr,~0u);
                D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};ctx->RSSetViewports(1,&vp);ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                const float settings[]{update*.007f,reversed?1.f:0.f,0,0};ctx->UpdateSubresource(constants.Get(),0,nullptr,settings,0,0);auto cb=constants.Get();ctx->PSSetConstantBuffers(0,1,&cb);ctx->Draw(3,0);
                ctx->CopyResource(reference.Get(),depth.Get());auto expected=read(referenceView.Get());
                Check(reader.Prepare(device.Get(),depth.Get()),"Same native resource remains readable after new writes");
                std::vector<uint32_t> actual;{DepthReadScope scope(ctx.Get(),reader);actual=read(reader.View());}
                Check(expected==actual,"Every depth/MSAA sample matches the copied reference, including sky/discard and changed contents");
                if(update)Check(actual!=prior,"Intervening native depth writes are observed");prior=actual;
                ComPtr<ID3D11RenderTargetView> restoredTarget;ComPtr<ID3D11DepthStencilView> restoredDepth;ComPtr<ID3D11UnorderedAccessView> restoredUav;
                ctx->OMGetRenderTargets(1,&restoredTarget,&restoredDepth);ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,pixelSlot,1,&restoredUav);
                Check(restoredTarget.Get()==rtv.Get()&&restoredDepth.Get()==dsv.Get()&&restoredUav.Get()==pixelUav.Get(),"Output targets, native DSV and pixel UAV restored");
                ctx->CopyStructureCount(counter.Get(),0,pixelUav.Get());ctx->CopyResource(counterStaging.Get(),counter.Get());
                D3D11_MAPPED_SUBRESOURCE countRead{};Check(SUCCEEDED(ctx->Map(counterStaging.Get(),0,D3D11_MAP_READ,0,&countRead)),"Map pixel UAV counter");
                const auto count=*static_cast<const UINT*>(countRead.pData);ctx->Unmap(counterStaging.Get(),0);Check(count==17,"Pixel UAV counter preserved");
                ComPtr<ID3D11DepthStencilState> restoredState;UINT stencil=0;ctx->OMGetDepthStencilState(&restoredState,&stencil);Check(restoredState.Get()==depthState.Get()&&stencil==71,"Depth state and stencil reference preserved");++cases;
            }
            ctx->ClearState();
        }
        std::cout<<"Depth format "<<unsigned(format.depth)<<" matches clone at "<<samples<<" samples\n";
    }
    if(debug){for(UINT64 i=0;i<debug->GetNumStoredMessages();++i){SIZE_T size=0;debug->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());debug->GetMessage(i,message,&size);
        if(message->Severity==D3D11_MESSAGE_SEVERITY_ERROR||message->Severity==D3D11_MESSAGE_SEVERITY_CORRUPTION){std::cerr<<message->pDescription<<'\n';Check(false,"No D3D11 debug-layer errors");}}}
    std::cout<<cases<<" GPU depth-read comparisons and native-state restoration checks passed\n";
}
