#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentCasterBounds.h"
#include <iostream>
#include <cstdlib>
#include <limits>
using namespace Filmmaker::Environment;
using Microsoft::WRL::ComPtr;
void Check(bool ok,const char* message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
int main(){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,8,nullptr,0,D3D11_SDK_VERSION,
        &device,&level,&context)),"WARP device");
    CasterBoundsReader reader;Check(reader.Initialize(device.Get()),"Bounds reader initializes");
    for(UINT stride:{12u,16u,20u}){
        std::vector<float> positions(1539*(stride/4),0);
        for(UINT i=0;i<1539;++i){positions[i*(stride/4)]=i<1536?-10.f:100.f;
            positions[i*(stride/4)+1]=float(i%3)-1;positions[i*(stride/4)+2]=.5f;}
        D3D11_BUFFER_DESC d{};d.ByteWidth=UINT(positions.size()*4);
        d.BindFlags=D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_SUBRESOURCE_DATA initial{positions.data()};ComPtr<ID3D11Buffer> buffer;
        Check(SUCCEEDED(device->CreateBuffer(&d,&initial,&buffer)),"Stream-output vertex buffer supports raw views");
        CasterVertexStream stream{buffer.Get(),1539,stride};std::vector<CasterBounds> bounds;
        ComPtr<ID3D11Buffer> sentinel;d.ByteWidth=48;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;d.MiscFlags=0;
        Check(SUCCEEDED(device->CreateBuffer(&d,nullptr,&sentinel)),"Native state sentinel");
        auto native=sentinel.Get();context->CSSetConstantBuffers(0,1,&native);
        Check(reader.Read(context.Get(),&stream,1,bounds),"Current bounds read");
        ComPtr<ID3D11Buffer> restored;context->CSGetConstantBuffers(0,1,&restored);
        Check(restored.Get()==sentinel.Get(),"Native state restored");
        Check(bounds.size()==2 && bounds[0].low.x==-10 && bounds[0].high.y==1 && bounds[1].low.x==100,
            "Whole-triangle blocks retain separated geometry and ignore alpha/UV padding");
        Matrix identity{};for(UINT i=0;i<4;++i)identity[i*5]=1;
        Check(!CasterBoundsIntersect(bounds,identity,true),"A face excludes geometry outside both of its sides");
        // Nonblocking reads report the read issued two calls earlier, grown by the late padding. Until
        // one has completed they report unavailable and every caster is kept.
        bool available=false;
        for(int frame=0;frame<6&&!available;++frame){
            available=reader.Read(context.Get(),&stream,1,bounds,false);
            Check(available||(bounds.empty()&&CasterBoundsIntersect(bounds,identity,available)),
                  "Unavailable late bounds keep every caster");
        }
        Check(available,"A stationary pose yields late bounds within a few frames");
        Check(bounds.size()==2&&bounds[0].low.x==-10-LateCasterBoundsPadding&&bounds[0].high.y==1+LateCasterBoundsPadding,
              "Late bounds are the measured blocks grown by the movement padding");
        positions[0]=0;context->UpdateSubresource(buffer.Get(),0,nullptr,positions.data(),0,0);
        Check(reader.Read(context.Get(),&stream,1,bounds)&&CasterBoundsIntersect(bounds,identity,true),
            "A moving vertex reaches the face in the same frame");
        positions[0]=std::numeric_limits<float>::quiet_NaN();
        context->UpdateSubresource(buffer.Get(),0,nullptr,positions.data(),0,0);
        const bool complete=reader.Read(context.Get(),&stream,1,bounds);
        Check(!complete&&CasterBoundsIntersect(bounds,identity,complete),"Invalid positions preserve conservative shadows");
        stream.count=3;stream.stride=buffer?UINT(positions.size()*4):0;
        Check(!reader.Read(context.Get(),&stream,1,bounds),"Out-of-buffer descriptors fail safely");
    }
    std::vector<CasterBounds> empty;Check(reader.Read(context.Get(),nullptr,0,empty),"An empty pose clears old shadows");
    Matrix identity{};for(UINT i=0;i<4;++i)identity[i*5]=1;
    Check(!CasterBoundsIntersect(empty,identity,true),"Empty current geometry affects no faces");
    std::cout<<"Current GPU caster bounds, movement, strides, native-state restoration and conservative failure PASS\n";
}
