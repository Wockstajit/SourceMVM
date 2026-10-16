#pragma once
namespace Filmmaker::Environment {
inline constexpr const char* AnimatedCasterCaptureShader=R"(
cbuffer NativeView:register(b0) {
    column_major float4x4 projectionToWorld:packoffset(c4);
    float4 worldToCameraOffset:packoffset(c24);
}
struct Input {float4 position:SV_Position;};
struct Output {float3 position:POSITION;};
[maxvertexcount(3)]
void Main(triangle Input input[3],inout TriangleStream<Output> output) {
    [unroll]for(uint i=0;i<3;++i) {
        float4 p=mul(projectionToWorld,input[i].position);
        // Native g_matProjectionToWorld already includes the camera-relative
        // origin used before forward projection. Subtracting it again places
        // the caster roughly one camera translation away from its visible mesh.
        Output v;v.position=p.xyz/p.w;
        output.Append(v);
    }
    output.RestartStrip();
}
)";

// Static-material programs draw the map AND placed CS2 assets. Keep only
// triangles overlapping a placed asset's world box. The CPU counts three
// vertices per input triangle, so a rejected triangle stays in the stream as a
// zero-area point inside the first box: it rasterizes nothing and its bounds
// stay inside a region that already holds a caster.
inline constexpr const char* AnimatedCasterPlacedCaptureShader=R"(
cbuffer NativeView:register(b0) {
    column_major float4x4 projectionToWorld:packoffset(c4);
    float4 worldToCameraOffset:packoffset(c24);
}
cbuffer PlacedBoxes:register(b1) {uint4 placedCount;float4 placedLow[64];float4 placedHigh[64];}
struct Input {float4 position:SV_Position;};
struct Output {float3 position:POSITION;};
[maxvertexcount(3)]
void Main(triangle Input input[3],inout TriangleStream<Output> output) {
    float3 p[3];
    [unroll]for(uint i=0;i<3;++i){float4 w=mul(projectionToWorld,input[i].position);p[i]=w.xyz/w.w;}
    const float3 low=min(p[0],min(p[1],p[2])),high=max(p[0],max(p[1],p[2]));
    bool inside=false;
    for(uint b=0;b<placedCount.x&&!inside;++b)inside=all(low<=placedHigh[b].xyz)&&all(high>=placedLow[b].xyz);
    const float3 anchor=(placedLow[0].xyz+placedHigh[0].xyz)*.5;
    [unroll]for(uint j=0;j<3;++j){Output v;v.position=inside?p[j]:anchor;output.Append(v);}
    output.RestartStrip();
}
)";

// Dither the actual vertex opacity into depth coverage. Endpoints zero and one
// stay exact; partially faded cables do not become opaque blockers. PCF filters
// this stable shadow-texel pattern along with the geometric shadow edge.
inline constexpr const char* AnimatedCasterCoverageShader=R"(
cbuffer Shadow:register(b3){row_major float4x4 worldToShadow;}
struct V {float4 position:SV_Position;float alpha:ALPHA;};
V VS(float3 p:POSITION,float alpha:ALPHA){V o;o.position=mul(worldToShadow,float4(p,1));o.alpha=alpha;return o;}
void PS(V i){
    float threshold=max(frac(52.9829189*frac(dot(floor(i.position.xy),float2(.06711056,.00583715)))),1.0/65536.0);
    clip(saturate(i.alpha)-threshold);
}
)";

// Native depth-prepass opacity, evaluated with retained per-draw constants and
// the original texture/sampler. Single-sample shadow maps represent the native
// alpha-to-coverage result with stable texel dithering before their PCF filter.
inline constexpr const char* AnimatedCasterCutoutShader=R"(
cbuffer Material:register(b0){float4 material0;float4 material1;}
cbuffer NativeMode:register(b1){uint4 mode0;uint4 mode1;}
cbuffer Shadow:register(b3){row_major float4x4 worldToShadow;}
Texture2D<float4> opacity:register(t12);SamplerState nativeSampler:register(s0);
struct V {float4 position:SV_Position;float2 uv:TEXCOORD0;};
V VS(float3 p:POSITION,float2 uv:TEXCOORD0){V o;o.position=mul(worldToShadow,float4(p,1));o.uv=uv;return o;}
void PS(V i){
    float2 uvWidth=abs(ddx_coarse(i.uv))+abs(ddy_coarse(i.uv));
    float blend=1+material1.x*(min(length(uvWidth)*4,1)-1);
    float a=opacity.Sample(nativeSampler,i.uv).a;
    float width=max(abs(ddx_coarse(a))+abs(ddy_coarse(a)),.000001);
    float delta=a-material0.w;
    float ramp=saturate(delta/width+.5);
    float coverage=lerp(ramp,a,blend);
    clip((mode1.z?coverage:delta)-.001);
    if(mode1.z){
        float threshold=max(frac(52.9829189*frac(dot(floor(i.position.xy),float2(.06711056,.00583715)))),1.0/65536.0);
        clip(saturate(coverage)-threshold);
    }
}
)";

}
