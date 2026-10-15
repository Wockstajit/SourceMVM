#pragma once
namespace Filmmaker::Environment {
inline constexpr char AmbientOcclusionShader[]=R"HLSL(
// Must equal SkyVisibilityDirections (checked by EnvironmentAmbientOcclusionTests).
#define SKY_DIRECTIONS 14
cbuffer Parameters:register(b0) {
    float4 cameraAndRadius;
    float4 viewport; // origin and depth dimensions
    float4 outputAndDepth; // output dimensions, viewport depth minimum/span
    float4 tuning; // strength, reversed depth, reserved
    // Inverse of the captured world-to-clip. Not CS2's cb4: its rows moved with a game
    // update and reconstructing through them rejected every pixel.
    row_major float4x4 clipToWorld;
}
#if MSAA_SAMPLES > 1
Texture2DMS<float,MSAA_SAMPLES> sourceDepth:register(t0);
#else
Texture2D<float> sourceDepth:register(t0);
#endif
Texture2D<float4> rawVisibility:register(t1);
Texture2D<float4> preparedPositions:register(t3);
cbuffer Sky:register(b1) {
    row_major float4x4 skyMatrix[SKY_DIRECTIONS];
    float4 skyParams; // enabled, strength, normal offset (units), depth bias (units)
}
Texture2DArray<float> skyMaps:register(t2);
SamplerComparisonState skySampler:register(s0);
// Fraction of the sky reaching p, cosine-weighted about n. Mirrors SkyVisibility() in
// EnvironmentSkyVisibilityMath.h; row 2 of each projection points from the sky into the map.
float SkyVisibilityAt(float3 p,float3 n) {
    if(skyParams.x<.5)return 1;
    float sum=0,weight=0;
    [loop]for(uint i=0;i<SKY_DIRECTIONS;++i){
        float3 depthAxis=skyMatrix[i][2].xyz;float3 toSky=-normalize(depthAxis);
        float w=max(0,dot(n,toSky))+.02;
        float4 c=mul(skyMatrix[i],float4(p+n*skyParams.z,1));
        float2 uv=c.xy*float2(.5,-.5)+.5;float visible=1;
        if(all(uv>0)&&all(uv<1)&&c.z>0&&c.z<1)
            visible=skyMaps.SampleCmpLevelZero(skySampler,float3(uv,i),c.z-length(depthAxis)*skyParams.w);
        sum+=w*visible;weight+=w;
    }
    return weight>0?sum/weight:1;
}
RWTexture2D<float4> result:register(u0);
float ReadDepth(int2 pixel) {
    pixel=clamp(pixel,int2(0,0),int2(viewport.zw)-1);
#if MSAA_SAMPLES > 1
    float z=sourceDepth.Load(pixel,0);
    [unroll]for(uint s=1;s<MSAA_SAMPLES;++s){
        float v=sourceDepth.Load(pixel,s);z=tuning.y>0?max(z,v):min(z,v);
    }
    return z;
#else
    return sourceDepth.Load(int3(pixel,0));
#endif
}
bool DepthPosition(int2 pixel,out float3 p) {
    float z=ReadDepth(pixel);
    bool valid=tuning.y>0?z>outputAndDepth.z+.000001:z<outputAndDepth.z+outputAndDepth.w-.000001;
    float2 ndc=(float2(pixel)+.5-viewport.xy)/viewport.zw*float2(2,-2)+float2(-1,1);
    float4 h=mul(clipToWorld,float4(ndc,(z-outputAndDepth.z)/outputAndDepth.w,1));
    p=h.xyz/h.w;
    return valid&&abs(h.w)>1e-8&&all(isfinite(p));
}
[numthreads(8,8,1)] void PreparePositions(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=uint2(outputAndDepth.xy)))return;
    int2 pixel=int2((float2(id.xy)+.5)*viewport.zw/outputAndDepth.xy);
    float3 p;bool valid=DepthPosition(pixel,p);
    result[id.xy]=valid?float4(p,1):float4(0,0,0,0);
}
bool Position(int2 pixel,out float3 p) {
    float4 value=preparedPositions.Load(int3(clamp(pixel,0,int2(outputAndDepth.xy)-1),0));
    p=value.xyz;return value.w>0&&all(pixel>=0)&&all(pixel<int2(outputAndDepth.xy));
}
[numthreads(8,8,1)] void Horizon(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=uint2(outputAndDepth.xy)))return;
    int2 pixel=int2(id.xy);
    float3 p;if(!Position(pixel,p)){result[id.xy]=float4(1,0,0,0);return;}
    float3 l,r,u,d;bool vl=Position(pixel+int2(-1,0),l),vr=Position(pixel+int2(1,0),r);
    bool vu=Position(pixel+int2(0,-1),u),vd=Position(pixel+int2(0,1),d);
    if((!vl&&!vr)||(!vu&&!vd)){result[id.xy]=float4(1,p);return;}
    float3 dx=vr&&(!vl||length(r-p)<length(p-l))?r-p:p-l;
    float3 dy=vd&&(!vu||length(d-p)<length(p-u))?d-p:p-u;
    float3 n=cross(dx,dy);float n2=dot(n,n);
    if(n2<1e-12||!all(isfinite(n))){result[id.xy]=float4(1,p);return;}
    n*=rsqrt(n2);if(dot(n,cameraAndRadius.xyz-p)<0)n=-n;
    float radius=cameraAndRadius.w,r2=radius*radius;
    float pixelRadius=clamp(radius/max(.01,(length(dx)+length(dy))*.5),1,96);
    // MSAA does not accumulate a stochastic AO pattern. A pixel hash changes the
    // quadrature whenever a receiver moves on screen, even with a static scene.
    // Use the same symmetric directions everywhere; bilateral filtering remains
    // spatial and lighting edits still take effect in the current frame.
    static const float2 axes[8]={float2(1,0),float2(.70710678,.70710678),
        float2(0,1),float2(-.70710678,.70710678),float2(-1,0),
        float2(-.70710678,-.70710678),float2(0,-1),float2(.70710678,-.70710678)};
    float sum=0;
    [branch]if(tuning.x>0)[unroll]for(uint direction=0;direction<8;++direction){
        float2 axis=axes[direction];float horizon=0,occlusion=0;
        [unroll]for(uint step=1;step<=6;++step){
            float distance=1+(pixelRadius-1)*(step*step/36.0);
            int2 q=pixel+int2(round(axis*distance));
            if(any(q<0)||any(q>=int2(outputAndDepth.xy)))continue;
            float3 v;if(!Position(q,v))continue;
            v-=p;float distance2=dot(v,v);
            if(distance2>1e-8&&distance2<r2){
                float sinS=saturate(dot(n,v)*rsqrt(distance2)-.03);
                // Integrate each newly blocked interval, as in Wicked's HBAO.
                // A distant sample cannot replace and erase a nearer horizon.
                if(sinS>horizon){occlusion+=(sinS-horizon)*exp(-2*distance2/r2);horizon=sinS;}
            }
        }
        sum+=occlusion;
    }
    float sky=SkyVisibilityAt(p,n);
    float ao=tuning.x>0?pow(saturate(1-sum/8),tuning.x*2):1;
    result[id.xy]=float4(ao*(1-skyParams.y+skyParams.y*saturate(sky)),p);
}
[numthreads(8,8,1)] void Filter(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=uint2(outputAndDepth.xy)))return;
    float4 center=rawVisibility.Load(int3(id.xy,0));float sum=0,weight=0;
    [unroll]for(int y=-2;y<=2;++y)[unroll]for(int x=-2;x<=2;++x){
        int2 q=clamp(int2(id.xy)+int2(x,y),0,int2(outputAndDepth.xy)-1);
        float4 sample=rawVisibility.Load(int3(q,0));
        float w=exp(-.35*(x*x+y*y))*saturate(1-length(sample.yzw-center.yzw)/max(.01,cameraAndRadius.w*.15));
        sum+=sample.x*w;weight+=w;
    }
    float ao=weight>0?sum/weight:center.x;
    result[id.xy]=ao.xxxx;
}
)HLSL";
}
