#pragma once
R"HLSL(
cbuffer GiTraceConstants:register(b4) { float4 giLowSize; float4 giInfo; }; // enabled, cell size, resolution, reserved
Texture3D<float4> giRadiance:register(t7);
Texture3D<float4> giDirection:register(t8);
SamplerState giSampler:register(s3);
)HLSL"
#include "EnvironmentGiTraceCoreShaders.h"
R"HLSL(
float3 WorldBounce(float3 p,float3 normal) {
    if(CL_worldGi<.5||CL_bounce<=0||giInfo.x<.5)return 0;
    float3 tangent=normalize(cross(normal,abs(normal.z)<.9?float3(0,0,1):float3(0,1,0)));
    float3 bitangent=cross(normal,tangent),origin=p+normal*giInfo.y;
    float3 sum=0;
    [unroll]for(int c=0;c<4;++c){
        float angle=c*1.57079633;
        float3 direction=normalize(normal*.8+(tangent*cos(angle)+bitangent*sin(angle))*.6);
        sum+=TraceRadiance(origin,direction,.65,giLowSize.w*.6).rgb;
    }
    return sum*(CL_bounce*.25);
}
float3 WorldReflectionRoughness(float3 p,float3 normal,float3 reflected,float3 fallback,float roughness) {
    if(CL_worldGi<.5||CL_sceneReflections<=0||giInfo.x<.5)return fallback;
    float4 hit=TraceRadiance(p+normal*giInfo.y,reflected,max(.03,roughness*.5),giLowSize.w);
    return lerp(fallback,hit.rgb+fallback*(1-hit.a),CL_sceneReflections);
}
float3 WorldReflection(float3 p,float3 normal,float3 reflected,float3 fallback){return WorldReflectionRoughness(p,normal,reflected,fallback,CL_reflectionRoughness);}
)HLSL"
