#pragma once
namespace Filmmaker::Environment {
// Reconstructed position-only csgo_foliage sway/flutter. The material weights, noise,
// wind and game clock must come from the native inputs; wall time is not equivalent.
inline constexpr const char *FoliageDeformationShader = R"HLSL(
Texture3D<float4> foliageNoise:register(t12);
SamplerState foliageSampler:register(s0);
float3 FoliagePosition(float3 p,float3 color,float4 sway,float4 flutter,
                      float gameTime,float3 wind,float strength) {
    precise float3 weights=clamp(color,.001,1);
    precise float phase=weights.g*2-1;
    precise float3 uv=(p+phase*float3(100,0,50))*(sway.z*float3(.00075,.00075,.00005));
    uv+=sway.y*float3(-.3,0,-.03)*gameTime;
    float2 n=foliageNoise.SampleLevel(foliageSampler,uv,0).xy-.5;
    float gust=foliageNoise.SampleLevel(foliageSampler,uv*float3(.4,.4,.125),0).z;
    gust=saturate(gust*gust*1.5);
    float3 baseWind=float3(n.x,n.y,n.y*.5);
    precise float3 movement=(baseWind*gust+wind)*pow(weights.r,sway.w)*strength*(150*sway.x)*gust;
    precise float3 result=p+movement;
    result.z-=length(movement.xy*.314);
    uv=(p+phase*float3(10,5,5))*(flutter.z*float3(.0025,.0025,.0005));
    uv+=flutter.y*float3(-.35,0,.1)*gameTime;
    float3 noise=(foliageNoise.SampleLevel(foliageSampler,uv,0).xyz-.5)*2;
    movement=(noise*baseWind+noise*wind)*strength*(25*flutter.x)*pow(weights.b,flutter.w);
    result+=movement;
    result.z-=length(movement.xy)*.314;
    return result;
}
)HLSL";
}
