#pragma once
namespace Filmmaker::Environment {
inline constexpr char NativeSmokeLightingShader[]=R"HLSL(
struct Light {float4 rows[30];};
StructuredBuffer<Light> lights:register(t120);
Texture2D<float> shadowAtlas:register(t121);
Texture3D<float4> irradiance:register(t122);
SamplerState volumeSampler:register(s14);
SamplerComparisonState shadowSampler:register(s15);
cbuffer SmokeLighting:register(b12){uint lightCount;float shadowBias;float2 reserved;}
cbuffer SmokeProbes:register(b13){float4 probe[8];}
float3 main(float3 world:TEXCOORD0,float3 normal:TEXCOORD1):SV_Target {
    float3 n=normalize(normal+float3(0,0,.00001));
    float3 uv=world*probe[0].xyz+probe[1].xyz;
    uv=clamp(uv,probe[2].xyz,probe[3].xyz);
    uv=uv*probe[6].xyz+probe[7].xyz;uv.z/=6;
    // Multiple scattering is less directional than an opaque surface. Blend the
    // six directional bands so voxel-gradient changes cannot split a puff.
    float3 weights=.25+.25*n*n;
    float3 ambient=irradiance.SampleLevel(volumeSampler,uv+float3(0,0,n.x<0?.5:0),0).rgb*weights.x;
    ambient+=irradiance.SampleLevel(volumeSampler,uv+float3(0,0,n.y<0?4./6:1./6),0).rgb*weights.y;
    ambient+=irradiance.SampleLevel(volumeSampler,uv+float3(0,0,n.z<0?5./6:2./6),0).rgb*weights.z;
    float3 result=ambient*probe[4].xyz;
    [loop]for(uint i=0;i<lightCount;++i){
        Light light=lights[i];float4 p=float4(world,1);
        float4 clip=float4(dot(light.rows[0],p),dot(light.rows[1],p),dot(light.rows[2],p),dot(light.rows[3],p));
        if(clip.w<=0)continue;clip.xyz/=clip.w;
        if(any(abs(clip.xy)>1)||clip.z<0||clip.z>1)continue;
        float4 source=light.rows[8];float3 delta=source.w>0?source.xyz-world:source.xyz;
        float distanceSquared=max(dot(delta,delta),.0001),distance=sqrt(distanceSquared);
        float falloff=source.w>0?source.w/max(source.w,distanceSquared):1;
        float curve=saturate(light.rows[9].x+light.rows[9].y*distance);curve=curve*curve*(3-2*curve);
        float visibility=1;
        if(light.rows[16].y>0){
            float2 atlas=clip.xy*light.rows[13].zw+light.rows[13].xy;
            uint width,height;shadowAtlas.GetDimensions(width,height);
            float2 texel=1./float2(width,height);
            visibility=0;
            [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x)
                visibility+=shadowAtlas.SampleCmpLevelZero(shadowSampler,atlas+float2(x,y)*texel,clip.z-shadowBias)/9;
        }
        // Smoke scatters into the camera on both sides of its density gradient.
        float phase=.35+.65*saturate(dot(n,delta/distance));
        result+=light.rows[10].rgb*(falloff*curve*visibility*phase);
    }
    return max(result,0);
}
)HLSL";
}
