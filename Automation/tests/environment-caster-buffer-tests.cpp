#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentCasterBuffer.h"
#include <d3d11sdklayers.h>
#include <iostream>
#include <vector>
#include <cstring>
using Microsoft::WRL::ComPtr;
using namespace Filmmaker::Environment;
void Check(bool ok,const char* text){if(!ok){std::cerr<<text<<'\n';std::exit(1);}}
int main(){
    Check(CasterBufferCapacity(1,2097152)==65536,"Small captures do not reserve the limit");
    Check(CasterBufferCapacity(65537,2097152)==131072,"Growth crosses a bucket");
    Check(CasterBufferCapacity(2097152,2097152)==2097152,"Original capacity still fits");
    Check(!CasterBufferCapacity(2097153,2097152)&&!CasterBufferCapacity(UINT64_MAX,2097152),"Oversized captures cannot wrap or truncate");
    Check(CasterBufferCapacity(120000,130000)==130000,"Non-power-of-two limits remain bounded");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)),"Debug WARP device");
    ComPtr<ID3D11InfoQueue> debug;device.As(&debug);
    auto read=[&](ID3D11Buffer* source,UINT bytes){D3D11_BUFFER_DESC d{};source->GetDesc(&d);d.BindFlags=d.MiscFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;Check(SUCCEEDED(device->CreateBuffer(&d,nullptr,&staging)),"Readback buffer");ctx->CopyResource(staging.Get(),source);D3D11_MAPPED_SUBRESOURCE m{};
        Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)),"Readback map");std::vector<unsigned char> out(bytes);memcpy(out.data(),m.pData,bytes);ctx->Unmap(staging.Get(),0);return out;};
    for(UINT stride:{12u,16u,20u}){
        ComPtr<ID3D11Buffer> live,published;
        Check(EnsureCasterBuffer(device.Get(),ctx.Get(),live,1,0,stride,2097152),"Initial live allocation");
        Check(EnsureCasterBuffer(device.Get(),ctx.Get(),published,1,0,stride,2097152),"Initial published allocation");
        const UINT retained=60000;std::vector<unsigned char> pose(retained*stride),previous(30000*stride);
        for(size_t i=0;i<pose.size();++i)pose[i]=static_cast<unsigned char>((i*31+7)%251);
        for(size_t i=0;i<previous.size();++i)previous[i]=static_cast<unsigned char>((i*19+11)%251);
        D3D11_BOX box{0,0,0,UINT(pose.size()),1,1};ctx->UpdateSubresource(live.Get(),0,&box,pose.data(),0,0);
        box.right=UINT(previous.size());ctx->UpdateSubresource(published.Get(),0,&box,previous.data(),0,0);
        auto oldLive=live;Check(EnsureCasterBuffer(device.Get(),ctx.Get(),live,90000,retained,stride,2097152),"Live storage grows before the next draw");
        Check(live.Get()!=oldLive.Get()&&read(live.Get(),UINT(pose.size()))==pose,"Growth retains every current-frame byte");
        Check(EnsureCasterBuffer(device.Get(),ctx.Get(),published,90000,30000,stride,2097152),"Published destination grows independently");
        Check(read(published.Get(),UINT(previous.size()))==previous,"Growth retains the preceding complete pose");
        D3D11_BUFFER_DESC d{};live->GetDesc(&d);Check(d.ByteWidth==131072*stride,"Observed size selects bounded allocation");
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R32_TYPELESS;sv.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;sv.BufferEx.NumElements=d.ByteWidth/4;sv.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
        ComPtr<ID3D11ShaderResourceView> view;Check(SUCCEEDED(device->CreateShaderResourceView(live.Get(),&sv,&view)),"Grown buffers remain readable by bounds compute");
        auto unchanged=live;
        Check(EnsureCasterBuffer(device.Get(),ctx.Get(),live,80000,60000,stride,2097152)&&live.Get()==unchanged.Get(),"Stable frames neither allocate nor copy");
        Check(!EnsureCasterBuffer(device.Get(),ctx.Get(),live,2097153,60000,stride,2097152)&&live.Get()==unchanged.Get(),"Failed oversized growth preserves the valid pose");
        Check(!EnsureCasterBuffer(device.Get(),nullptr,live,200000,60000,stride,2097152)&&live.Get()==unchanged.Get(),"Missing copy context preserves the valid pose");
        // Publication copies exactly the captured prefix, after growth; no stale remainder is read.
        box.right=UINT(pose.size());ctx->CopySubresourceRegion(published.Get(),0,0,0,0,live.Get(),0,&box);
        Check(read(published.Get(),UINT(pose.size()))==pose,"Next-frame publication retains the entire captured pose");
    }
    if(debug)for(UINT64 i=0;i<debug->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;debug->GetMessage(i,nullptr,&bytes);std::vector<char> memory(bytes);auto m=reinterpret_cast<D3D11_MESSAGE*>(memory.data());debug->GetMessage(i,m,&bytes);Check(m->Severity>D3D11_MESSAGE_SEVERITY_ERROR,m->pDescription);}
    std::cout<<"Caster allocation limits, live/published prefix retention, raw views and failure safety PASS\n";
}
