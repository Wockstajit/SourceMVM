#pragma once
namespace Filmmaker::Environment {
inline constexpr char NativeSkyShader[] = R"HLSL(
cbuffer SkyFrame:register(b13) {
    row_major float4x4 inverseViewProjection;
    float4 camera,viewport,sun,sunlight,sky;
}
)HLSL"
#include "EnvironmentSkyRadianceShaders.h"
R"HLSL(
float4 NativeSkyPS(float4 pixel:SV_Position):SV_Target {
    float2 ndc=(pixel.xy-viewport.xy)/viewport.zw*float2(2,-2)+float2(-1,1);
    float4 p=mul(inverseViewProjection,float4(ndc,.5,1));
    float3 ray=normalize(p.xyz/p.w-camera.xyz);
    float3 color=SkyRadiance(ray,sun.xyz,sunlight.xyz,sun.w,sunlight.w,sky.x,1);
    // Match the scene encoding consumed by CS2's native post-process shader.
    // Sky geometry shares the material HDR target and its MSAA resolve.
    if(sky.y>.5){
        float peak=max(color.r,max(color.g,color.b));
        color*=min(rcp(1+peak),.99951171875/max(peak,1e-6));
    }
    return float4(color,1);
}
)HLSL";
}
