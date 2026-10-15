#pragma once
namespace Filmmaker::Environment {
inline constexpr char LightingProofRebaseShader[]=R"HLSL(
struct Light {float4 rows[30];};
StructuredBuffer<Light> authored : register(t0);
cbuffer NativeView : register(b4) {float4 nativeView[36];}
cbuffer Rebase : register(b0) {uint4 rebase;}
cbuffer SunCascades : register(b2) {float4 sunRows[16];}
RWStructuredBuffer<Light> outputLight : register(u0);
RWByteAddressBuffer outputSun : register(u1);
RWStructuredBuffer<Light> outputLocal : register(u2);
[numthreads(64,1,1)] void Main(uint3 id:SV_DispatchThreadID) {
    if(id.x>=rebase.x)return;
    Light result=authored[id.x];
    // The tested PS variants add cb4[35].xyz to their position varying for
    // world-space operations, but evaluate dynamic lights using that varying directly.
    [unroll]for(uint row=0;row<4;++row)
        result.rows[row].w+=dot(result.rows[row].xyz,nativeView[35].xyz);
    if(result.rows[8].w!=0)result.rows[8].xyz-=nativeView[35].xyz;
    outputLight[id.x]=result;
    if(rebase.y){
        // The sun is the native cascade loop here, so its record goes dark in the local list.
        // Stained glass beams are directional too but carry a cookie (radiance.w > 0) and stay.
        Light local=result;if(local.rows[8].w==0&&local.rows[10].w==0)local.rows[10]=0;
        outputLocal[id.x]=local;
        if(id.x==0)[unroll]for(uint cascade=0;cascade<4;++cascade){
            uint base=cascade*4;
            float4 x=float4(sunRows[base].x,sunRows[base+1].x,sunRows[base+2].x,sunRows[base+3].x);
            float4 y=float4(sunRows[base].y,sunRows[base+1].y,sunRows[base+2].y,sunRows[base+3].y);
            float4 z=float4(sunRows[base].z,sunRows[base+1].z,sunRows[base+2].z,sunRows[base+3].z);
            float4 w=float4(sunRows[base].w,sunRows[base+1].w,sunRows[base+2].w,sunRows[base+3].w);
            w+=x*nativeView[35].x+y*nativeView[35].y+z*nativeView[35].z;
            outputSun.Store4((base+0)*16,asuint(x));outputSun.Store4((base+1)*16,asuint(y));
            outputSun.Store4((base+2)*16,asuint(z));outputSun.Store4((base+3)*16,asuint(w));
        }
    }
}
)HLSL";
}
