#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Relight/RelightClusterMath.h"
#include "../../AfxHookSource2/Filmmaker/Relight/RelightClusterShaders.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShadowMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderCompiler.h"
#include "../../AfxHookSource2/Filmmaker/Render/RenderPrivateTextureScope.h"
#include "../../AfxHookSource2/Filmmaker/Relight/RelightStainedGlassMath.h"
#include <wrl/client.h>
#include <d3d11.h>
#include <iostream>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
using namespace Filmmaker::Relight;
void Check(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
void CheckRasterW(ID3D11Device* device,ID3D11DeviceContext* ctx){
    const char* vs="float4 Main(uint id:SV_VertexID):SV_Position{float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(p[id]*1000,500,1000);}";
    const char* ps="float Main(float4 p:SV_Position):SV_Target{return p.w;}";
    auto compile=[](const char* text,const char* profile){ComPtr<ID3DBlob> code,error;
        Check(SUCCEEDED(Filmmaker::Environment::CompileEnvironmentShader(text,strlen(text),"RelightRasterW",nullptr,"Main",profile,
            D3DCOMPILE_ENABLE_STRICTNESS,&code,&error)),"Raster W probe compile");return code;};
    const auto vc=compile(vs,"vs_5_0"),pc=compile(ps,"ps_5_0");
    ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;
    Check(SUCCEEDED(device->CreateVertexShader(vc->GetBufferPointer(),vc->GetBufferSize(),nullptr,&vertex)),"Probe vertex shader");
    Check(SUCCEEDED(device->CreatePixelShader(pc->GetBufferPointer(),pc->GetBufferSize(),nullptr,&pixel)),"Probe pixel shader");
    Filmmaker::RenderPrivateTextureScope privateTextures;
    D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=4;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R32_FLOAT;d.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target,staging;ComPtr<ID3D11RenderTargetView> rtv;
    Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&target))&&SUCCEEDED(device->CreateRenderTargetView(target.Get(),nullptr,&rtv)),"Probe target");
    auto raw=rtv.Get();ctx->OMSetRenderTargets(1,&raw,nullptr);
    D3D11_VIEWPORT viewport{0,0,4,4,0,1};ctx->RSSetViewports(1,&viewport);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vertex.Get(),nullptr,0);ctx->PSSetShader(pixel.Get(),nullptr,0);ctx->Draw(3,0);
    ctx->OMSetRenderTargets(0,nullptr,nullptr);
    d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&staging)),"Probe readback");ctx->CopyResource(staging.Get(),target.Get());
    D3D11_MAPPED_SUBRESOURCE map{};Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)),"Probe map");
    const float w=static_cast<const float*>(map.pData)[0];ctx->Unmap(staging.Get(),0);
    Check(std::abs(w-1000)<.01f,"D3D11 raster SV_Position.w retains clip W, not reciprocal W");
    ctx->ClearState();
}
int main(){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx,work;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)),"WARP device");
    Check(SUCCEEDED(device->CreateDeferredContext(0,&work)),"deferred context");
    CheckRasterW(device.Get(),ctx.Get());
    ComPtr<ID3D11ComputeShader> shaders[2];
    for(unsigned i=0;i<2;++i){
        ComPtr<ID3DBlob> code,error;
        auto hr=Filmmaker::Environment::CompileEnvironmentShader(ClusterShader,sizeof(ClusterShader)-1,
            "RelightCluster",nullptr,i?"Fill":"Bounds","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&code,&error);
        if(error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());
        Check(SUCCEEDED(hr)&&SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shaders[i])),"cluster shader compile");
    }
    auto buffer=[&](UINT bytes,UINT stride,UINT flags,const void* initial){
        D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;d.BindFlags=flags;
        if(stride){d.StructureByteStride=stride;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;}
        D3D11_SUBRESOURCE_DATA data{};data.pSysMem=initial;ComPtr<ID3D11Buffer> b;
        Check(SUCCEEDED(device->CreateBuffer(&d,initial?&data:nullptr,&b)),"buffer creation");return b;
    };
    auto srv=[&](ID3D11Buffer* b){ComPtr<ID3D11ShaderResourceView> s;Check(SUCCEEDED(device->CreateShaderResourceView(b,nullptr,&s)),"SRV creation");return s;};
    auto uav=[&](ID3D11Buffer* b){ComPtr<ID3D11UnorderedAccessView> u;Check(SUCCEEDED(device->CreateUnorderedAccessView(b,nullptr,&u)),"UAV creation");return u;};
    std::vector<LightRecord> lights;
    for(unsigned i=0;i<38;++i){Light l;l.position={float(int(i%7)-3)*100,0,200+float(i)*80};l.range=20+float(i)*2;l.lumens=100;lights.push_back(ToRecord(l,1));}
    for(Vec3 position:{Vec3{100,0,10},Vec3{0,0,-100},Vec3{0,0,10}}){
        Light l;l.position=position;l.range=20;l.lumens=100;lights.push_back(ToRecord(l,1));
    }
    lights.push_back(SunRecord({0,0,1},{1,1,1},nullptr,false));
    for(unsigned face=0;face<6;++face){Light l;l.position={80,60,1200};l.range=300;l.lumens=100;
        const auto matrix=Filmmaker::Environment::LocalShadowMatrix(l.position,Filmmaker::Environment::PointShadowDirection(face),90,l.range);
        lights.push_back(ToShadowedRecord(l,1,matrix.data(),face));}
    for(Vec3 sun:{Vec3{0,0,-1},Vec3{.4f,.2f,-1}}){
        const auto beam=GlassBeamProjection({{-60,-40,900},{60,-40,900},{60,40,900},{-60,40,900}},sun,500);
        lights.push_back(GlassBeamRecord(beam,sun,{1,1,1},.5f));
    }
    const auto crossing=GlassBeamProjection({{-60,-40,-10},{60,-40,-10},{60,40,-10}}, {0,0,-1},500);
    lights.push_back(GlassBeamRecord(crossing,{0,0,-1},{1,1,1},.5f));
    const auto grid=MakeClusterGrid(160,96,unsigned(lights.size()),6400);
    const auto fallback=ClusterSummaryFallback(grid);
    ClusterView view{};view[2][0]=1;view[12][0]=1;view[13][1]=1;view[14][3]=1;
    ClusterFloatRow viewport[21]{};viewport[20]={0,0,160,96};
    auto records=buffer(UINT(lights.size()*sizeof(LightRecord)),sizeof(LightRecord),D3D11_BIND_SHADER_RESOURCE,lights.data());
    auto extent=buffer(UINT(lights.size()*sizeof(ClusterExtent)),sizeof(ClusterExtent),D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,nullptr);
    auto masks=buffer(UINT(fallback.size()*4),4,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,fallback.data());
    auto rows=buffer(96,16,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,nullptr);
    auto b4=buffer(sizeof(view),0,D3D11_BIND_CONSTANT_BUFFER,view.data());
    auto b1=buffer(sizeof(viewport),0,D3D11_BIND_CONSTANT_BUFFER,viewport);
    struct Params{ClusterUintRow grid,list;ClusterFloatRow depth;ClusterUintRow control;ClusterFloatRow nearby;ClusterUintRow nearbyList;ClusterFloatRow reach;};
    Params p{{grid.shift,grid.tilesX,grid.tilesY,grid.sliceCount},
        {UINT(lights.size()),grid.lights.words,grid.lights.tiles,grid.lights.slices},
        {grid.sliceScale,63,160,96},{1,grid.lights.ones,0,grid.size},{0,0,0,FirstPersonLightReach},{0,0,0,0},{0,0,0,0}};
    auto b0=buffer(sizeof(p),0,D3D11_BIND_CONSTANT_BUFFER,&p);
    auto r0=srv(records.Get()),r1=srv(extent.Get());auto u0=uav(extent.Get()),u1=uav(masks.Get()),u2=uav(rows.Get());
    auto read=[&](ID3D11Buffer* from){
        D3D11_BUFFER_DESC d{};from->GetDesc(&d);d.BindFlags=d.MiscFlags=d.StructureByteStride=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;Check(SUCCEEDED(device->CreateBuffer(&d,nullptr,&staging)),"readback buffer");
        ctx->CopyResource(staging.Get(),from);D3D11_MAPPED_SUBRESOURCE map{};
        Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)),"readback map");
        std::vector<uint32_t> result(d.ByteWidth/4);std::memcpy(result.data(),map.pData,d.ByteWidth);ctx->Unmap(staging.Get(),0);return result;
    };
    // Pass 7 repeats mode 4 (tiles, slices and the first-person list) with a visible-reach cutoff.
    for(unsigned pass:{0u,1u,2u,3u,4u,5u,6u,7u}){
        const unsigned mode=pass==7?4u:pass;const float cutoff=pass==7?.5f:0.f;p.reach[0]=cutoff;
        const bool fallbackMode=mode==3||mode>=5,firstPerson=mode==6;
        p.control[0]=mode>=3?1:mode;viewport[20][0]=mode==3?12.f:0.f;
        p.control[2]=mode>=4?1:0;
        p.nearbyList[0]=mode>=4?grid.lights.nearby:0;
        view[2][0]=mode>=5?0.f:1.f;
        if(firstPerson)p.control[2]=1;else if(mode==5)p.nearbyList[0]=0;
        work->UpdateSubresource(b4.Get(),0,nullptr,view.data(),0,0);
        work->UpdateSubresource(b0.Get(),0,nullptr,&p,0,0);work->UpdateSubresource(b1.Get(),0,nullptr,viewport,0,0);
        ID3D11Buffer* cb=b0.Get();work->CSSetConstantBuffers(0,1,&cb);cb=b1.Get();work->CSSetConstantBuffers(1,1,&cb);cb=b4.Get();work->CSSetConstantBuffers(4,1,&cb);
        auto resource=r0.Get();work->CSSetShaderResources(0,1,&resource);UINT keep=~0u;
        auto output=u0.Get();work->CSSetUnorderedAccessViews(0,1,&output,&keep);output=u2.Get();work->CSSetUnorderedAccessViews(2,1,&output,&keep);
        work->CSSetShader(shaders[0].Get(),nullptr,0);work->Dispatch(1,1,1);
        output=nullptr;work->CSSetUnorderedAccessViews(0,1,&output,&keep);resource=r1.Get();work->CSSetShaderResources(1,1,&resource);
        output=u1.Get();work->CSSetUnorderedAccessViews(1,1,&output,&keep);work->CSSetShader(shaders[1].Get(),nullptr,0);work->Dispatch((grid.TileCount()+64+grid.sliceCount)/64,1,1);
        ComPtr<ID3D11CommandList> commands;Check(SUCCEEDED(work->FinishCommandList(FALSE,&commands)),"finish commands");ctx->ExecuteCommandList(commands.Get(),TRUE);
        const auto actual=read(masks.Get()),constants=read(rows.Get());
        auto expected=grid.ListRow(grid.lights,fallbackMode?0:p.control[0]);
        if(firstPerson)expected={grid.lights.nearby,grid.lights.nearby,grid.lights.words,0};
        Check(std::equal(expected.begin(),expected.end(),constants.begin()),"GPU publishes exact list constants or unsupported-viewport fallback");
        if(p.nearbyList[0]){
            // First-person list: exactly the lights that can reach the eye's sphere, plus
            // every light without a proven range (sun, glass beams).
            unsigned listed=0,occupied=0;
            for(unsigned i=0;i<lights.size();++i){
                const bool wanted=FirstPersonLight(lights[i],{0,0,0},FirstPersonLightReach,cutoff);listed+=wanted;
                Check(((actual[grid.lights.nearby+i/32]>>(i%32))&1)==unsigned(wanted),"GPU first-person list matches the CPU reach test");
            }
            Check(listed>3&&listed<lights.size(),"first-person list drops distant lights and keeps near ones");
            for(unsigned w=0;w<grid.lights.words;++w)if(actual[grid.lights.nearby+w])occupied|=1u<<w;
            for(unsigned w=0;w<grid.lights.words;++w)Check(actual[grid.size+grid.lights.nearby+w]==occupied,"first-person occupancy mirror matches its words");
        }
        for(unsigned i=0;i<lights.size();++i){
            auto e=LightClusterExtent(lights[i],grid,view,160,96,cutoff);
            for(unsigned t=0;t<grid.TileCount();++t){
                const bool wanted=fallbackMode||e.allTiles||(t%grid.tilesX>=e.x0&&t%grid.tilesX<=e.x1&&t/grid.tilesX>=e.y0&&t/grid.tilesX<=e.y1);
                const bool bit=(actual[grid.lights.tiles+t*grid.lights.words+i/32]&(1u<<(i%32)))!=0;
                Check(bit==wanted,"GPU tile mask matches independent conservative bounds");
            }
            for(unsigned z=0;z<64;++z){const bool bit=(actual[grid.lights.slices+z*grid.lights.words+i/32]&(1u<<(i%32)))!=0;
                Check(bit==(fallbackMode||(z>=e.z0&&z<=e.z1)),"GPU slice mask matches conservative bounds");}
        }
        for(unsigned w=0;w<grid.lights.words;++w)Check(actual[w]==fallback[w],"Compute preserves fallback mask");
        for(unsigned row=0;row<grid.TileCount()+grid.sliceCount;++row){
            const unsigned base=row<grid.TileCount()?grid.lights.tiles+row*grid.lights.words:
                grid.lights.slices+(row-grid.TileCount())*grid.lights.words;
            unsigned occupied=0;for(unsigned w=0;w<grid.lights.words;++w)if(actual[base+w])occupied|=1u<<w;
            for(unsigned w=0;w<grid.lights.words;++w)Check(actual[grid.size+base+w]==occupied,"Every mirrored occupancy row matches generated mask words");
        }

    }
    std::cout<<"Relight cluster GPU packing and viewport fallback passed\n";
}
