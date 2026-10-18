#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentCasterCull.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderCompiler.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
using namespace Filmmaker::Environment;
using Microsoft::WRL::ComPtr;
void Check(bool ok,const char* message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}

int main(){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL level;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,8,nullptr,0,D3D11_SDK_VERSION,
        &device,&level,&ctx)),"WARP device");
    CasterShadowCuller culler;Check(culler.Initialize(device.Get()),"Caster cull shaders compile");
    const char shader[]="float4 Main(float3 p:POSITION):SV_Position{return float4(p,1);}";
    ComPtr<ID3DBlob> code,error;ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11InputLayout> layout;
    Check(SUCCEEDED(CompileEnvironmentShader(shader,sizeof(shader)-1,"CasterCullTest",nullptr,"Main","vs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS,&code,&error)),"Fixture vertex shader");
    Check(SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs)),"Fixture VS");
    D3D11_INPUT_ELEMENT_DESC input{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    Check(SUCCEEDED(device->CreateInputLayout(&input,1,code->GetBufferPointer(),code->GetBufferSize(),&layout)),"Fixture layout");
    ctx->VSSetShader(vs.Get(),nullptr,0);ctx->IASetInputLayout(layout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_TEXTURE2D_DESC t{};t.Width=t.Height=32;t.MipLevels=t.ArraySize=1;t.Format=DXGI_FORMAT_D32_FLOAT;
    t.SampleDesc.Count=1;t.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth,readback;ComPtr<ID3D11DepthStencilView> dsv;
    Check(SUCCEEDED(device->CreateTexture2D(&t,nullptr,&depth))&&
        SUCCEEDED(device->CreateDepthStencilView(depth.Get(),nullptr,&dsv)),"Fixture depth target");
    t.BindFlags=0;t.Usage=D3D11_USAGE_STAGING;t.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Check(SUCCEEDED(device->CreateTexture2D(&t,nullptr,&readback)),"Fixture depth readback");
    ctx->OMSetRenderTargets(0,nullptr,dsv.Get());D3D11_VIEWPORT viewport{0,0,32,32,0,1};ctx->RSSetViewports(1,&viewport);
    D3D11_RASTERIZER_DESC r{};r.FillMode=D3D11_FILL_SOLID;r.CullMode=D3D11_CULL_NONE;r.DepthClipEnable=true;
    ComPtr<ID3D11RasterizerState> raster;Check(SUCCEEDED(device->CreateRasterizerState(&r,&raster)),"Fixture rasterizer");
    ctx->RSSetState(raster.Get());
    std::vector<Vec3> positions(3072);
    auto block=[&](UINT begin,float x,float z){
        for(UINT i=begin;i<begin+1536;i+=3){positions[i]={x-.5f,-.5f,z};positions[i+1]={x,.5f,z};positions[i+2]={x+.5f,-.5f,z};}
    };
    block(0,0,.6f);block(1536,4,.3f);
    D3D11_BUFFER_DESC b{};b.ByteWidth=UINT(positions.size()*sizeof(Vec3));
    b.BindFlags=D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_SHADER_RESOURCE;b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_SUBRESOURCE_DATA data{positions.data()};ComPtr<ID3D11Buffer> vertices;
    Check(SUCCEEDED(device->CreateBuffer(&b,&data,&vertices)),"Fixture positions");
    const UINT stride=12,offset=0;auto vb=vertices.Get();ctx->IASetVertexBuffers(0,1,&vb,&stride,&offset);
    Matrix matrix{};for(UINT i=0;i<4;++i)matrix[i*5]=1;
    CasterVertexStream stream{vertices.Get(),3072,12};
    // Existing native bindings must survive both the measurement and each compacted draw.
    b.ByteWidth=16;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;b.MiscFlags=0;ComPtr<ID3D11Buffer> sentinel;
    Check(SUCCEEDED(device->CreateBuffer(&b,nullptr,&sentinel)),"Constant state sentinel");
    auto cb=sentinel.Get();ctx->CSSetConstantBuffers(0,1,&cb);
    b.ByteWidth=12;b.BindFlags=D3D11_BIND_INDEX_BUFFER;ComPtr<ID3D11Buffer> oldIndices;
    Check(SUCCEEDED(device->CreateBuffer(&b,nullptr,&oldIndices)),"Index state sentinel");
    ctx->IASetIndexBuffer(oldIndices.Get(),DXGI_FORMAT_R16_UINT,2);
    auto render=[&](bool culled,uint64_t generation,UINT expected){
        ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);
        D3D11_QUERY_DESC q{D3D11_QUERY_PIPELINE_STATISTICS,0};ComPtr<ID3D11Query> query;
        Check(SUCCEEDED(device->CreateQuery(&q,&query)),"Pipeline statistics");
        if(culled){Matrix batch[2]{matrix,matrix};batch[0][3]-=4;
            Check(culler.Prepare(ctx.Get(),stream,batch,2,generation),"Batch preparation");}
        ctx->Begin(query.Get());
        if(culled)Check(culler.Draw(ctx.Get(),stream,matrix,generation),"Cull draw is available");
        else ctx->Draw(stream.count,0);
        ctx->End(query.Get());ctx->CopyResource(readback.Get(),depth.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};Check(SUCCEEDED(ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped)),"Depth completes");
        std::vector<float> pixels(1024);
        for(UINT y=0;y<32;++y)std::memcpy(pixels.data()+y*32,static_cast<const char*>(mapped.pData)+y*mapped.RowPitch,128);
        ctx->Unmap(readback.Get(),0);
        D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
        while(ctx->GetData(query.Get(),&stats,sizeof(stats),0)==S_FALSE)Sleep(1);
        Check(stats.IAVertices==expected,"Only potentially contributing triangles reach the vertex shader");
        ComPtr<ID3D11Buffer> restored;ctx->CSGetConstantBuffers(0,1,&restored);
        Check(restored.Get()==sentinel.Get(),"Compute constants restored");
        DXGI_FORMAT format;UINT byteOffset=0;restored.Reset();ctx->IAGetIndexBuffer(&restored,&format,&byteOffset);
        Check(restored.Get()==oldIndices.Get()&&format==DXGI_FORMAT_R16_UINT&&byteOffset==2,"Index state restored");
        return pixels;
    };
    const auto reference=render(false,0,3072);
    Check(render(true,1,1536)==reference,"Culling distant geometry preserves every depth pixel exactly");
    // A new capture can move an entire block into this face immediately, including after a seek.
    block(1536,0,.3f);ctx->UpdateSubresource(vertices.Get(),0,nullptr,positions.data(),0,0);
    Check(render(true,2,3072)==render(false,2,3072),"Current-pose movement preserves the newly nearer shadow");
    block(1536,4,.3f);ctx->UpdateSubresource(vertices.Get(),0,nullptr,positions.data(),0,0);
    render(true,3,1536);matrix[3]=-4;render(true,3,1536);matrix[3]=0;
    // The count belongs to the current GPU capture, rather than its allocation capacity.
    UINT metadata[8]{1536,1,0,0,0,0,0,1};b.ByteWidth=sizeof(metadata);
    b.BindFlags=D3D11_BIND_SHADER_RESOURCE;b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    data.pSysMem=metadata;ComPtr<ID3D11Buffer> count;ComPtr<ID3D11ShaderResourceView> countView;
    Check(SUCCEEDED(device->CreateBuffer(&b,&data,&count)),"GPU count buffer");
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R32_TYPELESS;
    sv.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;sv.BufferEx.NumElements=8;sv.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
    Check(SUCCEEDED(device->CreateShaderResourceView(count.Get(),&sv,&countView)),"GPU count view");
    stream.gpuCount=countView.Get();render(true,4,1536);
    metadata[0]=0;ctx->UpdateSubresource(count.Get(),0,nullptr,metadata,0,0);render(true,5,0);
    stream.gpuCount=nullptr;
    // Malformed bounds retain a block, and orthographic sun casters in front of the near plane remain.
    positions[1536].x=std::numeric_limits<float>::quiet_NaN();ctx->UpdateSubresource(vertices.Get(),0,nullptr,positions.data(),0,0);
    render(true,6,3072);
    block(1536,0,-10);ctx->UpdateSubresource(vertices.Get(),0,nullptr,positions.data(),0,0);render(true,7,3072);
    std::cout<<"Current-pose shadow culling: exact depth, reduced submitted geometry, movement, GPU counts and state restoration PASS\n";
}
