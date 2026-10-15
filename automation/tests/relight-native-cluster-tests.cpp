#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Relight/RelightNativeCluster.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentLightingViewMath.h"
#include <d3d11sdklayers.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <iterator>

using namespace Filmmaker::Relight;
using Microsoft::WRL::ComPtr;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Hr(HRESULT value,const char* message){Check(SUCCEEDED(value),message);}
struct Params {ClusterUintRow grid,list;ClusterFloatRow depth;ClusterUintRow control;ClusterFloatRow nearby;ClusterUintRow nearbyList;};
static_assert(sizeof(Params)==96);
int main(int argc,char** argv){try{
    bool hardware=false;std::filesystem::path fixtures;
    for(int i=1;i<argc;++i)if(std::string(argv[i])=="--hardware")hardware=true;else fixtures=std::filesystem::u8path(argv[i]);
    if(fixtures.empty()||!std::filesystem::is_regular_file(fixtures/"native-cluster-depth.dxbc")||
        !std::filesystem::is_regular_file(fixtures/"native-cluster-tile.dxbc")){
        std::cout<<"Native engine GPU fixtures unavailable; generate with mirv_relight dumpprograms.\n";return 77;}
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    Hr(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,
        nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx),"Create device");
    ComPtr<ID3D11InfoQueue> queue;Hr(device.As(&queue),"Create info queue");
    for(const char* name:{"native-cluster-depth.dxbc","native-cluster-tile.dxbc"}){
        std::ifstream f(fixtures/name,std::ios::binary);Check(bool(f),"Missing native engine fixture");
        std::vector<uint8_t> code((std::istreambuf_iterator<char>(f)),{});ComPtr<ID3D11ComputeShader> shader;
        const unsigned kind=std::string(name).find("depth")!=std::string::npos?1:2;
        Check(NativeClusterDeclarations(code.data(),code.size(),kind),"Verify native buffer declarations");
        Check(!NativeClusterDeclarations(code.data(),code.size()-1,kind),"Reject truncated program");
        Check(!NativeClusterDeclarations(code.data(),code.size(),3-kind),"Reject mismatched culling stage");
        Hr(device->CreateComputeShader(code.data(),code.size(),nullptr,&shader),"Create native engine shader");
        NoteNativeClusterShader(shader.Get(),code.data(),code.size());
    }
    auto structured=[&](UINT stride,UINT count,const void* data,UINT flags,ComPtr<ID3D11Buffer>& b){
        D3D11_BUFFER_DESC d{};d.ByteWidth=stride*count;d.StructureByteStride=stride;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.BindFlags=flags;
        D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=data;Hr(device->CreateBuffer(&d,data?&initial:nullptr,&b),"Create structured buffer");};
    auto constants=[&](UINT bytes,const void* data,ComPtr<ID3D11Buffer>& b){
        D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=data;
        Hr(device->CreateBuffer(&d,&initial,&b),"Create constants");};
    auto read=[&](ID3D11Buffer* b){D3D11_BUFFER_DESC d{};b->GetDesc(&d);d.BindFlags=d.MiscFlags=d.StructureByteStride=0;
        d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Buffer> staging;Hr(device->CreateBuffer(&d,nullptr,&staging),"Create readback");
        ctx->CopyResource(staging.Get(),b);D3D11_MAPPED_SUBRESOURCE m{};Hr(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m),"Read results");
        std::vector<uint32_t> result(d.ByteWidth/4);memcpy(result.data(),m.pData,d.ByteWidth);ctx->Unmap(staging.Get(),0);return result;};
    NativeClusterWork native;unsigned comparisons=0;uint64_t wordsChecked=0;
    for(unsigned count:{1u,31u,32u,33u,64u,127u,872u,1025u})for(unsigned shape=0;shape<3;++shape){
        const unsigned width=shape==0?32:shape==1?257:2560,height=shape==0?32:shape==1?129:1440;
        auto grid=MakeClusterGrid(width,height,count,10000);const bool summaries=grid.lights.words<=32;
        std::vector<LightRecord> lights(count);std::vector<ClusterExtent> extents(count);
        for(unsigned i=0;i<count;++i){auto& light=lights[i];light[32]=float(i*3);light[36]=256;light[37]=-1;
            auto& e=extents[i];e.x0=i%grid.tilesX;e.x1=(std::min)(e.x0+i%7,grid.tilesX-1);
            e.y0=(i*7)%grid.tilesY;e.y1=(std::min)(e.y0+i%5,grid.tilesY-1);
            e.z0=(i*3)%64;e.z1=(std::min)(e.z0+i%13,63u);e.allTiles=i%11==0;
            if(i%17==3){e.x0=grid.tilesX+1;e.x1=0;e.allTiles=0;}
        }
        auto expected=summaries?ClusterSummaryFallback(grid):ClusterFallback(grid);
        for(unsigned cell=0;cell<grid.TileCount()+grid.sliceCount;++cell){bool tile=cell<grid.TileCount();
            unsigned x=cell%grid.tilesX,y=cell/grid.tilesX,z=cell-grid.TileCount();
            unsigned base=tile?grid.lights.tiles+cell*grid.lights.words:grid.lights.slices+z*grid.lights.words,occupied=0;
            for(unsigned word=0;word<grid.lights.words;++word){uint32_t bits=0;
                for(unsigned bit=0;bit<32&&word*32+bit<count;++bit){const auto& e=extents[word*32+bit];
                    bool inside=tile?(e.allTiles||(x>=e.x0&&x<=e.x1&&y>=e.y0&&y<=e.y1)):(z>=e.z0&&z<=e.z1);
                    if(inside)bits|=1u<<bit;}
                expected[base+word]=bits;if(bits&&word<32)occupied|=1u<<word;}
            if(summaries)for(unsigned word=0;word<grid.lights.words;++word)expected[grid.size+base+word]=occupied;
        }
        unsigned occupied=0;
        for(unsigned word=0;word<grid.lights.words;++word){uint32_t bits=0;
            for(unsigned bit=0;bit<32&&word*32+bit<count;++bit)if(FirstPersonLight(lights[word*32+bit],{100,20,8}))bits|=1u<<bit;
            expected[grid.lights.nearby+word]=bits;if(bits&&word<32)occupied|=1u<<word;}
        if(summaries)for(unsigned word=0;word<grid.lights.words;++word)expected[grid.size+grid.lights.nearby+word]=occupied;
        auto initial=summaries?ClusterSummaryFallback(grid):ClusterFallback(grid);
        ComPtr<ID3D11Buffer> authored,extentBuffer,masks,paramBuffer,view,viewport;
        structured(480,count,lights.data(),D3D11_BIND_SHADER_RESOURCE,authored);
        structured(32,count,extents.data(),D3D11_BIND_SHADER_RESOURCE,extentBuffer);
        structured(4,UINT(initial.size()),initial.data(),D3D11_BIND_UNORDERED_ACCESS,masks);
        ComPtr<ID3D11ShaderResourceView> a,e;ComPtr<ID3D11UnorderedAccessView> u;
        Hr(device->CreateShaderResourceView(authored.Get(),nullptr,&a),"Authored SRV");Hr(device->CreateShaderResourceView(extentBuffer.Get(),nullptr,&e),"Extent SRV");
        Hr(device->CreateUnorderedAccessView(masks.Get(),nullptr,&u),"Mask UAV");
        const Params params{{grid.shift,grid.tilesX,grid.tilesY,grid.sliceCount},{count,grid.lights.words,grid.lights.tiles,grid.lights.slices},
            {grid.sliceScale,63,float(width),float(height)},{1,grid.lights.ones,0,summaries?grid.size:0},{100,20,8,96},{grid.lights.nearby,0,0,0}};
        constants(sizeof(params),&params,paramBuffer);
        ClusterView v{};std::array<ClusterFloatRow,21> vp{};vp[20]={0,0,float(width),float(height)};
        constants(sizeof(v),v.data(),view);constants(sizeof(vp),vp.data(),viewport);
        Check(native.Ensure(device.Get(),count),"Recognize and prepare native kernels");
        // A discarded command list must not pretend its constant upload reached
        // the GPU. Retry through the immediate context below without committing.
        ComPtr<ID3D11DeviceContext> deferred;Hr(device->CreateDeferredContext(0,&deferred),"Create abort-test context");
        native.Record(deferred.Get(),grid,paramBuffer.Get(),a.Get(),e.Get(),u.Get(),nullptr);
        ComPtr<ID3D11CommandList> discarded;Hr(deferred->FinishCommandList(FALSE,&discarded),"Discard native recording");
        Filmmaker::Environment::LightingViewGate gate;
        for(unsigned iteration=0;iteration<3;++iteration){
            if(iteration==2){v[12][0]=1;ctx->UpdateSubresource(view.Get(),0,nullptr,v.data(),0,0);}
            const auto dims=NativeClusterDispatches(grid);
            Check(gate.Prepare(device.Get(),ctx.Get(),view.Get(),viewport.Get(),{},
                {(count+63)/64,(count+63)/64,(grid.TileCount()+64+1+63)/64},false,dims),"Prepare native dispatch gate");
            native.Record(ctx.Get(),grid,paramBuffer.Get(),a.Get(),e.Get(),u.Get(),gate.Arguments());native.Commit();gate.Commit();
            ID3D11UnorderedAccessView* empty[5]{};ctx->CSSetUnorderedAccessViews(0,5,empty,nullptr);
            const auto result=read(masks.Get());Check(result.size()==expected.size(),"Mask size");
            for(size_t i=0;i<expected.size();++i)if(result[i]!=expected[i]){
                std::cerr<<"count="<<count<<" grid="<<grid.tilesX<<'x'<<grid.tilesY<<" iteration="<<iteration<<" word="<<i<<" got="<<result[i]<<" expected="<<expected[i]<<'\n';throw std::runtime_error("Native mask differs from conservative reference");}
            const auto args=read(gate.Arguments());Check(args.size()==15,"Native argument size");
            Check(args[9]==(iteration==1?0:dims[0])&&args[10]==dims[1]&&args[11]==1,"Depth gate dimensions");
            Check(args[12]==(iteration==1?0:dims[2])&&args[13]==dims[3]&&args[14]==dims[1],"Tile gate dimensions");
            ++comparisons;wordsChecked+=result.size();
        }
    }
    for(UINT64 i=0;i<queue->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;queue->GetMessage(i,nullptr,&size);std::vector<uint8_t> storage(size);
        auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());Hr(queue->GetMessage(i,message,&size),"Read D3D message");
        if(message->Severity<=D3D11_MESSAGE_SEVERITY_ERROR)throw std::runtime_error(message->pDescription);}
    std::cout<<comparisons<<" exact native mask comparisons, "<<wordsChecked<<" words, 24 discarded recordings retried, duplicate/new-view gates, no D3D errors\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
