#pragma once
namespace Filmmaker::Environment {
inline constexpr char CasterCullShader[]=R"HLSL(
struct Bounds {float3 low;float valid;float3 high;float reserved;};
StructuredBuffer<Bounds> bounds:register(t0);
ByteAddressBuffer gpuCount:register(t1);
RWByteAddressBuffer indices:register(u0);
RWByteAddressBuffer arguments:register(u1);
cbuffer Settings:register(b0){uint count,stride,base,indirect;float4 planes[96];}
groupshared uint first,counted;
[numthreads(128,1,1)]
void Cull(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    uint begin=group.x*1536;
    uint vertices=indirect?min(count,gpuCount.Load(0)):count;
    if(lane==0){
        Bounds b=bounds[group.x];
        // Invalid or unproven bounds retain geometry. Empty blocks do not.
        bool inside=b.valid!=2;
        if(b.valid==1){
            [unroll]for(uint p=0;p<6;++p){
                float4 plane=planes[group.y*6+p];
                float3 support=float3(plane.x>=0?b.high.x:b.low.x,
                    plane.y>=0?b.high.y:b.low.y,plane.z>=0?b.high.z:b.low.z);
                float3 product=plane.xyz*support;
                float tolerance=1e-5*(dot(abs(product),1)+abs(plane.w)+1);
                if(dot(product,1)+plane.w < -tolerance)inside=false;
            }
        }
        counted=inside&&begin<vertices?min(1536,vertices-begin):0;
        first=0;
        if(counted)arguments.InterlockedAdd(group.y*20,counted,first);
    }
    GroupMemoryBarrierWithGroupSync();
    for(uint i=lane;i<counted;i+=128)indices.Store((group.y*count+first+i)*4,begin+i);
}
)HLSL";
}
