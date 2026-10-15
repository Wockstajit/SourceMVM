#pragma once
namespace Filmmaker::Relight {
// Appended to ClusterShader so first-person selection and bounds retain their
// existing definitions. The native engine kernels perform tile/bin culling.
inline constexpr char NativeClusterAdapter[]=R"HLSL(
struct NativeItem {uint4 planes;float4 rectangle;float4 interval;float4 conic0;float4 conic1;};
RWStructuredBuffer<NativeItem> nativeItems:register(u3);
RWStructuredBuffer<float4> nativePlanes:register(u4);
[numthreads(64,1,1)]void NativePrepare(uint3 id:SV_DispatchThreadID){
    if(id.x>=list.y*32)return;
    Extent e;
    if(id.x<list.x)e=extents[id.x];
    else {e.xy=uint4(grid.y+1,0,grid.z+1,0);e.z=uint4(0,0,0,0);}
    if(e.z.z)e.xy=uint4(0,grid.y-1,0,grid.z-1);
    NativeItem item;item.planes=uint4(id.x*4,0,4,0);
    item.rectangle=float4(e.xy.x,e.xy.z,e.xy.y,e.xy.w)+float4(.25,.25,.75,.75);
    item.interval=float4(e.z.xy,0,0)+float4(.25,.75,0,0);
    item.conic0=item.conic1=0;nativeItems[id.x]=item;
    nativePlanes[id.x*4]=float4(1,0,-item.rectangle.x,0);
    nativePlanes[id.x*4+1]=float4(-1,0,item.rectangle.z,0);
    nativePlanes[id.x*4+2]=float4(0,1,-item.rectangle.y,0);
    nativePlanes[id.x*4+3]=float4(0,-1,item.rectangle.w,0);
}
[numthreads(64,1,1)]void NativeFinalize(uint3 id:SV_DispatchThreadID){
    uint tiles=grid.y*grid.z;
    if(id.x==tiles+grid.w&&nearbyList.x){
        uint occupied=0;
        for(uint word=0;word<list.y;++word){
            uint bits=0;
            [loop]for(uint bit=0;bit<32;++bit){
                uint record=word*32+bit;if(record>=list.x)break;
                if(FirstPersonLight(lights[record]))bits|=1u<<bit;
            }
            masks[nearbyList.x+word]=bits;
            if(bits&&word<32)occupied|=1u<<word;
        }
        if(control.w)for(uint word=0;word<list.y;++word)masks[control.w+nearbyList.x+word]=occupied;
        return;
    }
    if(id.x>=tiles+grid.w||!control.w)return;
    uint base=id.x<tiles?list.z+id.x*list.y:list.w+(id.x-tiles)*list.y;
    uint occupied=0;
    for(uint word=0;word<list.y;++word)if(masks[base+word]&&word<32)occupied|=1u<<word;
    for(uint word=0;word<list.y;++word)masks[control.w+base+word]=occupied;
}
)HLSL";
}
