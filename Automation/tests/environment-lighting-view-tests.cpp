#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentLightingViewMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentLightingProofShaders.h"
#include "../../AfxHookSource2/Filmmaker/Relight/RelightClusterMath.h"
#include "../../AfxHookSource2/Filmmaker/Relight/RelightClusterShaders.h"
#include <d3d11sdklayers.h>
#include <iostream>
using Microsoft::WRL::ComPtr;
using namespace Filmmaker::Environment;
using namespace Filmmaker::Relight;
void Check(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
int main(int argc,char** argv){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx,work;
    const auto driver=argc>1&&std::string(argv[1])=="--hardware"?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP;
    auto hr=D3D11CreateDevice(nullptr,driver,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx);
    if(hr==DXGI_ERROR_SDK_COMPONENT_MISSING)hr=D3D11CreateDevice(nullptr,driver,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx);
    Check(SUCCEEDED(hr)&&SUCCEEDED(device->CreateDeferredContext(0,&work)),"Device and deferred context");
    ComPtr<ID3D11InfoQueue> debug;device.As(&debug);
    auto compile=[&](const char* source,size_t size,const char* entry){ComPtr<ID3DBlob> code,error;
        auto result=CompileEnvironmentShader(source,size,"LightingViewGpuTest",nullptr,entry,"cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&code,&error);
        if(error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());Check(SUCCEEDED(result),"Compile");
        ComPtr<ID3D11ComputeShader> shader;Check(SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader)),"Shader");return shader;};
    const auto rebase=compile(LightingProofRebaseShader,sizeof(LightingProofRebaseShader)-1,"Main");
    const auto bounds=compile(ClusterShader,sizeof(ClusterShader)-1,"Bounds"),fill=compile(ClusterShader,sizeof(ClusterShader)-1,"Fill");
    auto buffer=[&](UINT bytes,UINT stride,UINT flags,const void* initial=nullptr,bool raw=false){D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;d.BindFlags=flags;
        if(stride){d.StructureByteStride=stride;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;}if(raw)d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_SUBRESOURCE_DATA data{initial};ComPtr<ID3D11Buffer> b;Check(SUCCEEDED(device->CreateBuffer(&d,initial?&data:nullptr,&b)),"Buffer");return b;};
    auto srv=[&](ID3D11Buffer* b){ComPtr<ID3D11ShaderResourceView> s;Check(SUCCEEDED(device->CreateShaderResourceView(b,nullptr,&s)),"SRV");return s;};
    auto uav=[&](ID3D11Buffer* b,bool raw=false){D3D11_UNORDERED_ACCESS_VIEW_DESC d{};D3D11_BUFFER_DESC bd{};b->GetDesc(&bd);
        d.Format=DXGI_FORMAT_R32_TYPELESS;d.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;d.Buffer.NumElements=bd.ByteWidth/4;d.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
        ComPtr<ID3D11UnorderedAccessView> u;Check(SUCCEEDED(device->CreateUnorderedAccessView(b,raw?&d:nullptr,&u)),"UAV");return u;};
    auto read=[&](ID3D11Buffer* b){D3D11_BUFFER_DESC d{};b->GetDesc(&d);d.BindFlags=d.MiscFlags=d.StructureByteStride=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;Check(SUCCEEDED(device->CreateBuffer(&d,nullptr,&staging)),"Readback buffer");ctx->CopyResource(staging.Get(),b);
        D3D11_MAPPED_SUBRESOURCE map{};Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)),"Readback");
        std::vector<uint8_t> result(d.ByteWidth);memcpy(result.data(),map.pData,result.size());ctx->Unmap(staging.Get(),0);return result;};
    auto execute=[&](){ComPtr<ID3D11CommandList> commands;Check(SUCCEEDED(work->FinishCommandList(FALSE,&commands)),"Command recording");ctx->ExecuteCommandList(commands.Get(),TRUE);};
    auto b0=buffer(16,0,D3D11_BIND_CONSTANT_BUFFER),b2=buffer(256,0,D3D11_BIND_CONSTANT_BUFFER),params=buffer(64,0,D3D11_BIND_CONSTANT_BUFFER);
    struct Params{ClusterUintRow grid,list;ClusterFloatRow depth;ClusterUintRow control;};
    unsigned comparisons=0,hits=0;
    for(unsigned count:{1u,38u,872u}){
        auto grid=MakeClusterGrid(160,96,count,6400);auto fallback=ClusterSummaryFallback(grid);
        std::vector<LightRecord> records;for(unsigned i=0;i<count;++i){Light l;l.position={float(int(i%7)-3)*100,20,200+float(i)*8};l.range=20+float(i%13)*2;l.lumens=100;records.push_back(ToRecord(l,1));}
        auto authored=buffer(count*480,480,D3D11_BIND_SHADER_RESOURCE,records.data());auto authoredSrv=srv(authored.Get());
        struct Output{ComPtr<ID3D11Buffer> light,local,sun,character,masks,extents,rows;ComPtr<ID3D11UnorderedAccessView> ul,ulocal,usun,uchar,um,ue,ur;ComPtr<ID3D11ShaderResourceView> se;};
        auto make=[&](){Output o;o.light=buffer(count*480,480,D3D11_BIND_UNORDERED_ACCESS);o.local=buffer(count*480,480,D3D11_BIND_UNORDERED_ACCESS);
            o.sun=buffer(256,0,D3D11_BIND_UNORDERED_ACCESS,nullptr,true);o.character=buffer(256,0,D3D11_BIND_UNORDERED_ACCESS,nullptr,true);
            o.masks=buffer(UINT(fallback.size()*4),4,D3D11_BIND_UNORDERED_ACCESS,fallback.data());o.extents=buffer(count*32,32,D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE);o.rows=buffer(96,16,D3D11_BIND_UNORDERED_ACCESS);
            o.ul=uav(o.light.Get());o.ulocal=uav(o.local.Get());o.usun=uav(o.sun.Get(),true);o.uchar=uav(o.character.Get(),true);o.um=uav(o.masks.Get());o.ue=uav(o.extents.Get());o.ur=uav(o.rows.Get());o.se=srv(o.extents.Get());return o;};
        auto expected=make(),actual=make();LightingViewGate gate;
        std::array<float,144> view{};view[12*4]=view[13*4+1]=view[14*4+3]=1;view[15*4+3]=1;
        std::array<float,84> viewport{};viewport[82]=160;viewport[83]=96;
        std::array<float,64> normal{},character{};for(unsigned cascade=0;cascade<4;++cascade)for(unsigned row=0;row<4;++row)normal[cascade*16+row*5]=character[cascade*16+row*5]=1;
        Params p{{grid.shift,grid.tilesX,grid.tilesY,grid.sliceCount},{count,grid.lights.words,grid.lights.tiles,grid.lights.slices},{grid.sliceScale,63,160,96},{1,grid.lights.ones,0,grid.size}};
        const UINT rebaseValues[]{count,1,0,0};
        for(unsigned step=0;step<19;++step){
            bool force=false,bypass=step==15;const bool shouldRun=step!=1&&step!=2&&step!=4&&step!=8;
            switch(step){
            case 2:view[1*4+3]=.731f;break; // unused AO row
            case 3:view[35*4]=.5f;break;
            case 5:view[12*4]=.9f;break;
            case 6:view[13*4+3]=.2f;break;
            case 7:view[2*4]=1;p.control[2]=1;break;
            case 9:viewport[82]=159;break; // invalid native viewport uses fallback
            case 10:viewport[80]=1;break;
            case 11:normal[15]=.75f;break;
            case 12:records[0][40]=.123f;force=true;break;
            case 13:character[15]=.25f;break;
            case 14:p.control[0]=2;break;
            case 15:view[35*4]=.75f;gate.Invalidate();break;
            case 17:gate.Invalidate();break; // new frame, same camera
            case 18:view[12*4+2]=-0.f;break; // exact bits, including signed zero
            }
            // Different engine buffer objects contain equal relevant rows on hit cases.
            auto vb=buffer(sizeof(view),0,D3D11_BIND_CONSTANT_BUFFER,view.data()),vp=buffer(sizeof(viewport),0,D3D11_BIND_CONSTANT_BUFFER,viewport.data());
            ctx->UpdateSubresource(authored.Get(),0,nullptr,records.data(),0,0);
            std::vector<uint8_t> key;auto append=[&](const void* data,size_t size){const auto* bytes=static_cast<const uint8_t*>(data);key.insert(key.end(),bytes,bytes+size);};
            append(normal.data(),sizeof(normal));append(character.data(),sizeof(character));append(&p,sizeof(p));
            auto run=[&](Output& o,bool reuse){
                bool gated=reuse&&!bypass&&gate.Prepare(device.Get(),work.Get(),vb.Get(),vp.Get(),key,{(count+63)/64,(count+63)/64,(grid.TileCount()+grid.sliceCount+63)/64},force);
                Check(!reuse||bypass||gated,"Gate setup");
                auto dispatch=[&](unsigned pass,UINT groups){if(gated)work->DispatchIndirect(gate.Arguments(),pass*12);else work->Dispatch(groups,1,1);};
                auto b=vb.Get();work->CSSetConstantBuffers(4,1,&b);b=vp.Get();work->CSSetConstantBuffers(1,1,&b);auto s=authoredSrv.Get();work->CSSetShaderResources(0,1,&s);
                for(unsigned pass=0;pass<2;++pass){work->UpdateSubresource(b2.Get(),0,nullptr,pass?normal.data():character.data(),0,0);b=b2.Get();work->CSSetConstantBuffers(2,1,&b);
                    work->UpdateSubresource(b0.Get(),0,nullptr,rebaseValues,0,0);b=b0.Get();work->CSSetConstantBuffers(0,1,&b);
                    ID3D11UnorderedAccessView* outputs[]{o.ul.Get(),pass?o.usun.Get():o.uchar.Get(),o.ulocal.Get()};work->CSSetUnorderedAccessViews(0,3,outputs,nullptr);work->CSSetShader(rebase.Get(),nullptr,0);dispatch(0,(count+63)/64);
                    ID3D11UnorderedAccessView* empty[3]{};work->CSSetUnorderedAccessViews(0,3,empty,nullptr);}
                work->UpdateSubresource(params.Get(),0,nullptr,&p,0,0);b=params.Get();work->CSSetConstantBuffers(0,1,&b);
                auto u=o.ue.Get();work->CSSetUnorderedAccessViews(0,1,&u,nullptr);u=o.ur.Get();work->CSSetUnorderedAccessViews(2,1,&u,nullptr);work->CSSetShader(bounds.Get(),nullptr,0);dispatch(1,(count+63)/64);
                u=nullptr;work->CSSetUnorderedAccessViews(0,1,&u,nullptr);s=o.se.Get();work->CSSetShaderResources(1,1,&s);u=o.um.Get();work->CSSetUnorderedAccessViews(1,1,&u,nullptr);work->CSSetShader(fill.Get(),nullptr,0);dispatch(2,(grid.TileCount()+grid.sliceCount+63)/64);
                execute();if(gated)gate.Commit();
                if(gated){auto bytes=read(gate.Arguments());const auto* args=reinterpret_cast<const UINT*>(bytes.data());
                    if((args[0]!=0)!=shouldRun)std::cerr<<"count="<<count<<" step="<<step<<" groups="<<args[0]<<" expectedRun="<<shouldRun<<char(10);Check((args[0]!=0)==shouldRun,"Used input changes dispatch; exact duplicates skip");
                    const auto snapshot=read(gate.Counters());std::array<float,28> wanted{};
                    const unsigned selected[]{2,12,13,14,15,35};for(unsigned i=0;i<6;++i)memcpy(wanted.data()+i*4,view.data()+selected[i]*4,16);
                    memcpy(wanted.data()+24,viewport.data()+80,16);
                    Check(memcmp(snapshot.data(),wanted.data(),sizeof(wanted))==0,"Every cached shader input retains exact bits");Check(args[1]==1&&args[2]==1&&args[4]==1&&args[5]==1&&args[7]==1&&args[8]==1,"Indirect dispatch dimensions");if(!shouldRun)++hits;}
            };
            run(expected,false);run(actual,true);
            for(auto pair:{std::pair{expected.light.Get(),actual.light.Get()},std::pair{expected.local.Get(),actual.local.Get()},std::pair{expected.sun.Get(),actual.sun.Get()},std::pair{expected.character.Get(),actual.character.Get()},std::pair{expected.masks.Get(),actual.masks.Get()},std::pair{expected.extents.Get(),actual.extents.Get()},std::pair{expected.rows.Get(),actual.rows.Get()}}){Check(read(pair.first)==read(pair.second),"Gated outputs exactly match full rebase, character cascades and clusters");++comparisons;}
        }
        // Aborted command recording must not publish a CPU cache key.
        auto vb=buffer(sizeof(view),0,D3D11_BIND_CONSTANT_BUFFER,view.data()),vp=buffer(sizeof(viewport),0,D3D11_BIND_CONSTANT_BUFFER,viewport.data());
        Check(gate.Prepare(device.Get(),work.Get(),vb.Get(),vp.Get(),{}, {1,1,1},false),"Record aborted gate");
        ComPtr<ID3D11CommandList> discarded;Check(SUCCEEDED(work->FinishCommandList(FALSE,&discarded)),"Discarded recording");gate.Invalidate();
        Check(gate.Prepare(device.Get(),work.Get(),vb.Get(),vp.Get(),{}, {1,1,1},false),"Retry gate");execute();gate.Commit();auto bytes=read(gate.Arguments());Check(reinterpret_cast<const UINT*>(bytes.data())[0]==1,"Retry forces work after abort");
        Check(!gate.Prepare(device.Get(),work.Get(),nullptr,vp.Get(),{}, {1,1,1},false),"Missing native view uses fallback");
    }
    if(debug)for(UINT64 i=0;i<debug->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;debug->GetMessage(i,nullptr,&size);std::vector<char> bytes(size);auto m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());debug->GetMessage(i,m,&size);Check(m->Severity>D3D11_MESSAGE_SEVERITY_ERROR,m->pDescription);}
    std::cout<<comparisons<<" exact lighting-buffer comparisons, "<<hits<<" verified duplicate-view skips, no D3D errors\n";
}