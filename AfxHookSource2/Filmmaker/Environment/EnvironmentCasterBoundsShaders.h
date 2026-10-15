#pragma once
namespace Filmmaker::Environment {
inline constexpr char CasterBoundsShader[]=R"(
ByteAddressBuffer vertices:register(t0);
ByteAddressBuffer gpuCount:register(t1);
struct Bounds {float3 low;float valid;float3 high;float reserved;};
RWStructuredBuffer<Bounds> output:register(u0);
cbuffer Settings:register(b0){uint count,stride,base,unused;}
groupshared float3 lows[128],highs[128];groupshared uint valid[128];
[numthreads(128,1,1)]
void Main(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    float3 low=3.402823466e+38,high=-3.402823466e+38;uint ok=1;
    uint currentCount=count;
    if(unused){currentCount=min(count,gpuCount.Load(0));ok=gpuCount.Load(28);}
    // 1536 vertices = 512 whole triangles, so bounds never split a triangle.
    if(unused){
        for(uint index=lane;index<currentCount;index+=128){float3 p=asfloat(vertices.Load3(index*stride));
            if(!all(isfinite(p)))ok=0;else{low=min(low,p);high=max(high,p);}}
    }else{
        [unroll]for(uint i=0;i<12;++i){uint index=group.x*1536+lane+i*128;
            if(index<currentCount){float3 p=asfloat(vertices.Load3(index*stride));
                if(!all(isfinite(p)))ok=0;else{low=min(low,p);high=max(high,p);}}}
    }
    lows[lane]=low;highs[lane]=high;valid[lane]=ok;GroupMemoryBarrierWithGroupSync();
    [unroll]for(uint width=64;width; width>>=1){
        if(lane<width){lows[lane]=min(lows[lane],lows[lane+width]);
            highs[lane]=max(highs[lane],highs[lane+width]);valid[lane]&=valid[lane+width];}
        GroupMemoryBarrierWithGroupSync();}
    if(lane==0){Bounds b;b.low=lows[0];b.high=highs[0];b.valid=valid[0]&&currentCount==0?2:valid[0];b.reserved=0;output[base+group.x]=b;}
}
)";
}
