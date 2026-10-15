#pragma once
R"HLSL(
cbuffer ShadowConstants : register(b2) {
    row_major float4x4 localShadowMatrix[24];
    row_major float4x4 sunShadowMatrix;
    float4 lightShadowInfo[16]; // x = first slice (-1 disabled), y = point light
    float4 shadowInfo; // sun available, world-space bias, local resolution, sun resolution
    row_major float4x4 shelterMatrix;
    float4 shelterInfo; // -1 unavailable, 0 off, 1 ready; resolution; world bias
    float4 shelterDirection;
    row_major float4x4 nearSunShadowMatrix;
    float4 nearSunShadowInfo;
    row_major float4x4 closeSunShadowMatrix;
    float4 closeSunShadowInfo;
    row_major float4x4 detailSunShadowMatrix;
    float4 detailSunShadowInfo;
    float4 viewmodelShadowInfo;
    // x = sun shadow filter radius in shadow-map texels. It arrives as a constant rather than as
    // a CL_ macro on purpose: this header is compiled standalone by the shadow probe test and
    // must not depend on the climate defines.
    float4 sunFilterInfo;
    // Gust shelter maps (PrecipitationGustDirections): four directions tilted about the rain's.
    row_major float4x4 gustMatrix[4];
    float4 gustDirection[4]; // xyz toward the sky, w world bias
    float4 gustInfo;         // x ready, y resolution, z mean-direction weight, w per-gust weight
};
Texture2DArray<float> gustTexture:register(t30);
Texture2DArray<float> localShadowTexture : register(t5);
Texture2D<float> sunShadowTexture : register(t6);
Texture2D<float> shelterTexture:register(t13);
Texture2D<float> nearSunShadowTexture:register(t15);
Texture2D<float> closeSunShadowTexture:register(t16);
Texture2D<float> detailSunShadowTexture:register(t18);
// First-person receiver policy: world/editor geometry, native wind foliage and
// no synthetic self-shadow casters. Captured characters and weapons remain
// only in the normal world maps, where their complete shadows are kept.
Texture2DArray<float> viewmodelSunShadowTexture:register(t19);
Texture2DArray<float> viewmodelLocalShadowTexture:register(t20);
Texture2D<float> viewmodelSelfShadowTexture:register(t21);
float ShadowDepth(int2 q,int slice,bool sun,bool nearSun,bool closeSun,bool detailSun,bool viewmodel){
    float environment=1;
    [branch]if((sun?viewmodelShadowInfo.x:viewmodelShadowInfo.y)>.5)
        environment=sun?viewmodelSunShadowTexture.Load(int4(q,detailSun?3:closeSun?2:nearSun?1:0,0)):viewmodelLocalShadowTexture.Load(int4(q,slice,0));
    [branch]if(viewmodel){
        if(sun&&closeSun&&viewmodelShadowInfo.z>.5)environment=min(environment,viewmodelSelfShadowTexture.Load(int3(q,0)));
        return environment;
    }
    float actors=detailSun?detailSunShadowTexture.Load(int3(q,0)):closeSun?closeSunShadowTexture.Load(int3(q,0)):nearSun?nearSunShadowTexture.Load(int3(q,0)):sun?sunShadowTexture.Load(int3(q,0)):localShadowTexture.Load(int4(q,slice,0));
    // Unsplit frames keep the complete scene in the normal maps. In split
    // frames the nearest blocker from either layer is the same world shadow.
    return min(environment,actors);
}
// SOFT MAP LOOKUPS. Both world maps stretch one texture over the whole map (a few units per
// texel), so a single hard comparison drew visible stair steps wherever the projection runs at a
// slant -- slanted rain made the rain cover step across roofs. Twelve bilinear comparisons over a
// small fixed disc turn those steps into a smooth edge. Fixed taps, no noise: stills and seeks are
// repeatable.
static const float2 kSoftTaps[12]={
    float2(-.326,-.406),float2(-.840,-.074),float2(-.696,.457),float2(-.203,.621),
    float2(.962,-.195),float2(.473,-.480),float2(.519,.767),float2(.185,-.893),
    float2(.507,.064),float2(.896,.412),float2(-.322,-.933),float2(-.792,-.598)};
float ShelterCompare(float2 texel,float z,int resolution){
    int2 base=(int2)floor(texel);float2 blend=frac(texel);float lit=0;
    [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x){
        float d=shelterTexture.Load(int3(clamp(base+int2(x,y),0,resolution-1),0));
        lit+=(z<=d+.000001?1:0)*(x?blend.x:1-blend.x)*(y?blend.y:1-blend.y);
    }
    return lit;
}
float WorldPrecipitationVisible(float3 world){
    // FAILS OPEN. shelterInfo.x < 0 means "shelter was asked for but there is no shelter map to
    // test against" -- the map geometry has not been prepared, which is the state every map is in
    // until someone runs Prepare map. This used to `return 0`, i.e. discard EVERY particle, so the
    // whole precipitation feature rendered nothing at all on an unprepared map: no rain, no snow,
    // no ash. From the outside that is indistinguishable from the feature being broken, and it is
    // what "there is no snow falling, nothing is working" was.
    //
    // Not knowing whether a flake is under a roof is not a reason to delete the weather. Draw it
    // and let the operator prepare the map if they want flakes to stop at rooflines -- the same
    // fail-open trade the rest of this codebase makes for optional data.
    if(shelterInfo.x<0)return 1;
    if(shelterInfo.x==0)return 1;
    float4 clip=mul(shelterMatrix,float4(world+shelterDirection.xyz*shelterInfo.z,1));
    float3 ndc=clip.xyz/clip.w;float2 uv=ndc.xy*float2(.5,-.5)+.5;
    if(any(uv<0)||any(uv>1)||ndc.z<0)return 1;
    int resolution=(int)shelterInfo.y;
    float2 texel=uv*resolution-.5;
    float radius=max(shelterInfo.w,1);
    float lit=0;
    [unroll]for(int i=0;i<12;++i)lit+=ShelterCompare(texel+kSoftTaps[i]*radius,ndc.z,resolution);
    return lit/12;
}
// Open sky along gust direction g, one bilinear comparison (1 = the rain reaches this point).
float GustOpen(int g,float3 world){
    float4 clip=mul(gustMatrix[g],float4(world+gustDirection[g].xyz*gustDirection[g].w,1));
    float3 ndc=clip.xyz/clip.w;float2 uv=ndc.xy*float2(.5,-.5)+.5;
    if(any(uv<0)||any(uv>1)||ndc.z<0)return 1;
    int resolution=(int)gustInfo.y;
    float2 texel=uv*resolution-.5;int2 base=(int2)floor(texel);float2 blend=frac(texel);float lit=0;
    [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x){
        float d=gustTexture.Load(int4(clamp(base+int2(x,y),0,resolution-1),g,0));
        lit+=(ndc.z<=d+.000001?1:0)*(x?blend.x:1-blend.x)*(y?blend.y:1-blend.y);
    }
    return lit;
}
// HOW MUCH RAIN REACHES A SURFACE, 0..1: the weighted share of the mean and gust directions with
// open sky. Open ground is 1, a doorway or arch facing the wind a fraction that falls off a short
// way inside, a deep interior 0. Fails open like WorldPrecipitationVisible when there is no map.
float PrecipitationExposure(float3 world){
    float mean=WorldPrecipitationVisible(world);
    if(shelterInfo.x<=0||gustInfo.x<.5)return mean;
    float gusts=0;
    [unroll]for(int g=0;g<4;++g)gusts+=GustOpen(g,world);
    return gustInfo.z*mean+gustInfo.w*gusts;
}
)HLSL"
R"HLSL(
float CompareMapBias(float4 clip,int slice,bool sun,float bias,bool nearSun=false,bool closeSun=false,bool detailSun=false,bool viewmodel=false) {
    if(viewmodel&&(sun?viewmodelShadowInfo.x:viewmodelShadowInfo.y)<.5)return 1;
    if(clip.w<=0)return 1;
    float3 ndc=clip.xyz/clip.w;
    float2 uv=ndc.xy*float2(.5,-.5)+.5;
    if(any(uv<0)||any(uv>1)||ndc.z<0||ndc.z>1)return 1;
    int resolution=(int)(sun?shadowInfo.w:shadowInfo.z);
    float2 texel=uv*resolution-.5;int2 base=(int2)floor(texel);float2 blend=frac(texel);float result=0;
    [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x){
        int2 q=clamp(base+int2(x,y),0,resolution-1);
        float d=ShadowDepth(q,slice,sun,nearSun,closeSun,detailSun,viewmodel);
        result+=(ndc.z<=d+bias?1:0)*(x?blend.x:1-blend.x)*(y?blend.y:1-blend.y);
    }
    return result;
}
float CompareMap(float4 clip,int slice,bool sun,bool viewmodel=false) { return CompareMapBias(clip,slice,sun,.000005,false,false,false,viewmodel); }
float WorldSunShadow(float3 p) {
    if(shadowInfo.x<.5)return 1;
    return CompareMap(mul(sunShadowMatrix,float4(p,1)),0,true);
}
// Surface receivers use a compact filtered comparison with a slope-scaled depth allowance.
// Fog and GI deliberately keep the full-map lookup so their cached radiance is camera-independent.
float SurfaceSunShadow(float3 p,float3 n,float4x4 lightProjection,bool nearSun,bool closeSun=false,bool detailSun=false,bool viewmodel=false) {
    float resolution=max(shadowInfo.w,1);
    float texelWorld=2/max(length(lightProjection[0].xyz)*resolution,1e-6);
    float depthPerUnit=length(lightProjection[2].xyz);
    float3 away=normalize(lightProjection[2].xyz); // from the sun into the scene
    float facing=saturate(dot(n,-away));
    float slope=min(sqrt(max(0,1-facing*facing))/max(facing,.15),6);
    // One bilinear comparison preserves coverage at the edge without a broad blur disc.
    // The near map has a half-unit footprint; bias scales with that footprint too.
    p+=n*texelWorld*.5;
    float bias=(texelWorld*slope+texelWorld*.25)*depthPerUnit+.000005;
    float4 clip=mul(lightProjection,float4(p,1));
    // A single bilinear comparison is a hard edge one texel wide, which is what made a wide
    // cascade look stair-stepped rather than soft. Spread the comparison over a disc measured in
    // TEXELS: the same setting is a tight penumbra in the 0.03-unit close cascade and a broad one
    // in the coarse full-map cascade, so the filter widens exactly where the resolution drops.
    float radius=sunFilterInfo.x;
    if(radius<=0)return CompareMapBias(clip,0,true,bias,nearSun,closeSun,detailSun,viewmodel);
    float lit=0;
    // [loop], not [unroll]: each tap expands to CompareMapBias's own unrolled 2x2, and
    // WorldSunShadowSoft can call this for two cascades, so unrolling twelve taps multiplies into
    // hundreds of inlined fetches and the compiler gives up on whole entry points.
    [loop]for(int i=0;i<12;++i){
        float4 shifted=clip;
        shifted.xy+=kSoftTaps[i]*(radius/resolution)*float2(2,-2)*clip.w;
        lit+=CompareMapBias(shifted,0,true,bias,nearSun,closeSun,detailSun,viewmodel);
    }
    return lit/12;
}
float SunRegionWeight(float3 p,float4x4 projection,float available,float start) {
    if(available<.5)return 0;
    float3 clip=mul(projection,float4(p,1)).xyz;
    if(clip.z<0||clip.z>1)return 0;
    return 1-smoothstep(start,.98,max(abs(clip.x),abs(clip.y)));
}
// Which cascade a point's shadow lookup lands in, and whether one covers it at all. Returns the
// finest tier that claims the point: 1 close, 2 detail, 3 middle, 4 outer, 0 none. A 0 is the
// interesting answer -- it means no cascade projects this surface, so it can never be shadowed
// however good the caster geometry is.
int SunCascadeAt(float3 p) {
    if(shadowInfo.x<.5)return 0;
    if(SunRegionWeight(p,closeSunShadowMatrix,closeSunShadowInfo.x,.8)>0)return 1;
    if(SunRegionWeight(p,detailSunShadowMatrix,detailSunShadowInfo.x,.75)>0)return 2;
    if(SunRegionWeight(p,nearSunShadowMatrix,nearSunShadowInfo.x,.85)>0)return 3;
    // The outer tier has no region weight of its own: it is the fallback the others blend to,
    // so it covers a point exactly when the point projects inside its clip volume.
    float3 clip=mul(sunShadowMatrix,float4(p,1)).xyz;
    if(clip.z<0||clip.z>1)return 0;
    return max(abs(clip.x),abs(clip.y))<=1?4:0;
}
float WorldSunShadowSoft(float3 p,float3 n,bool viewmodel=false) {
    if(shadowInfo.x<.5)return 1;
    // Select finest first. Only blend two adjacent levels at a boundary; don't sample
    // every wider map for the common interior case. The bridge avoids a 16:1 detail cliff.
    float closeWeight=SunRegionWeight(p,closeSunShadowMatrix,closeSunShadowInfo.x,.8);
    float closeVisibility=1;
    if(closeWeight>0)closeVisibility=SurfaceSunShadow(p,n,closeSunShadowMatrix,true,true,false,viewmodel);
    if(closeWeight>=1)return closeVisibility;
    float detailWeight=SunRegionWeight(p,detailSunShadowMatrix,detailSunShadowInfo.x,.75);
    float detailVisibility=1;
    if(detailWeight>0)detailVisibility=SurfaceSunShadow(p,n,detailSunShadowMatrix,true,false,true,viewmodel);
    if(detailWeight>=1)return lerp(detailVisibility,closeVisibility,closeWeight);
    float weight=SunRegionWeight(p,nearSunShadowMatrix,nearSunShadowInfo.x,.85);
    float visibility=1;
    if(weight>0)visibility=SurfaceSunShadow(p,n,nearSunShadowMatrix,true,false,false,viewmodel);
    if(weight<1)visibility=lerp(SurfaceSunShadow(p,n,sunShadowMatrix,false,false,false,viewmodel),visibility,weight);
    visibility=lerp(visibility,detailVisibility,detailWeight);
    visibility=lerp(visibility,closeVisibility,closeWeight);
    return visibility;
}
// THE CHEAP SUN LOOKUP for coatings: the finest cascade that covers the point, one bilinear
// comparison, no disc filter and no cross-cascade blend. The soft version is up to ~96 loads and the
// coating pass runs per MSAA sample on every wet or snowy pixel, which made wetness cost more than
// the rest of the environment together. A wet highlight or a snow tint does not show a penumbra.
float WorldSunShadowQuick(float3 p,float3 n,bool viewmodel=false) {
    if(shadowInfo.x<.5)return 1;
    bool close=SunRegionWeight(p,closeSunShadowMatrix,closeSunShadowInfo.x,.8)>0;
    bool detail=!close&&SunRegionWeight(p,detailSunShadowMatrix,detailSunShadowInfo.x,.75)>0;
    bool nearSun=!close&&!detail&&SunRegionWeight(p,nearSunShadowMatrix,nearSunShadowInfo.x,.85)>0;
    float4x4 m=close?closeSunShadowMatrix:detail?detailSunShadowMatrix:nearSun?nearSunShadowMatrix:sunShadowMatrix;
    float resolution=max(shadowInfo.w,1);
    float texelWorld=2/max(length(m[0].xyz)*resolution,1e-6);
    float3 away=normalize(m[2].xyz);
    float facing=saturate(dot(n,-away));
    float slope=min(sqrt(max(0,1-facing*facing))/max(facing,.15),6);
    p+=n*texelWorld*.5;
    float bias=(texelWorld*slope+texelWorld*.25)*length(m[2].xyz)+.000005;
    return CompareMapBias(mul(m,float4(p,1)),0,true,bias,close||detail||nearSun,close,detail,viewmodel);
}
// Fixed light-space disk taps: no screen-space noise or camera-dependent history.
float SoftLocalShadow(float4 clip,int slice,float sourceRadius,float range,bool viewmodel=false) {
    if(viewmodel&&viewmodelShadowInfo.y<.5)return 1;
    if(sourceRadius<=0)return CompareMap(clip,slice,false,viewmodel);
    if(clip.w<=0)return 1;
    float3 ndc=clip.xyz/clip.w;float2 uv=ndc.xy*float2(.5,-.5)+.5;
    if(any(uv<0)||any(uv>1)||ndc.z<0||ndc.z>1)return 1;
    float resolution=shadowInfo.z;
    float4x4 m=localShadowMatrix[slice];
    float projectionScale=length(m[0].xyz);
    float searchRadius=clamp(sourceRadius*projectionScale*.5/max(clip.w,1),1/resolution,32/resolution);
    float blockers=0,blockerDistance=0;
    [loop]for(int j=0;j<16;++j){
        float angle=j*2.39996323;float2 tap=float2(cos(angle),sin(angle))*sqrt((j+.5)/16.);
        int2 q=clamp((int2)((uv+tap*searchRadius)*resolution),0,(int)resolution-1);
        float d=ShadowDepth(q,slice,false,false,false,false,viewmodel);
        if(d<ndc.z-.000005){blockerDistance+=range/max(.0001,range-d*(range-1));blockers+=1;}
    }
    if(blockers==0)return 1;
    blockerDistance/=blockers;
    float penumbra=sourceRadius*max(0,clip.w-blockerDistance)/max(blockerDistance,1);
    float radius=clamp(penumbra*projectionScale*.5/max(clip.w,1),.5/resolution,32/resolution);
    float visibility=0;
    [loop]for(int k=0;k<16;++k){
        float angle=k*2.39996323;float2 tap=float2(cos(angle),sin(angle))*sqrt((k+.5)/16.);
        float4 shifted=clip;shifted.xy+=tap*radius*float2(2,-2)*clip.w;
        visibility+=CompareMap(shifted,slice,false,viewmodel);
    }
    return visibility/16;
}
float WorldLightShadowSample(int light,float3 p,int sample,bool viewmodel=false) {
    if(shadowInfo.z<1)return 1;
    int slice=(int)lightShadowInfo[light].x;if(slice<0)return 1;
    float3 delta=p-StudioPosition(light,sample);
    p-=normalize(delta+float3(1e-8,0,0))*shadowInfo.y;
    if(lightShadowInfo[light].y>1.5)slice+=sample;
    else if(lightShadowInfo[light].y>.5){
        float3 a=abs(delta);
        slice+=a.x>=a.y&&a.x>=a.z?(delta.x>=0?0:1):a.y>=a.z?(delta.y>=0?2:3):(delta.z>=0?4:5);
    }
    return SoftLocalShadow(mul(localShadowMatrix[slice],float4(p,1)),slice,lightShadowInfo[light].z,lightShadowInfo[light].w,viewmodel);
}
float WorldLightShadow(int light,float3 p,bool viewmodel=false) {
    float visibility=0;int count=StudioSamples(light);
    [loop]for(int sample=0;sample<count;++sample)visibility+=WorldLightShadowSample(light,p,sample,viewmodel);
    return visibility/count;
}
)HLSL"
