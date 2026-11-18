#pragma once
R"HLSL(
cbuffer ShadowConstants : register(b2) {
    row_major float4x4 localShadowMatrix[24];
    row_major float4x4 sunShadowMatrix;
    float4 lightShadowInfo[16]; // x = first slice (-1 disabled), y = point light
    float4 shadowInfo; // sun available, world-space bias, local resolution, sun resolution
    row_major float4x4 shelterMatrix;
    float4 shelterInfo; // -1 unavailable, 0 off, 1 ready; resolution; world lift; soft radius in texels
    float4 shelterDirection; // xyz straight up, w world units per texel
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
    // The weather cover volume (EnvironmentWeatherCoverMath.h).
    float4 coverLow;   // xyz world corner, w = 1 when the volume is ready
    float4 coverScale; // xyz voxels per world unit
    float4 coverSize;  // xyz voxel counts
    float4 coverWind;  // xyz unit vector toward where the weather comes from, w its lean (0 straight down)
};
Texture3D<float2> coverTexture:register(t30);
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
// THE SKY MAP. One depth map looks straight down on the playable map (a few units per texel) and
// answers "is the sky open straight above this point". Twelve bilinear comparisons over a small
// fixed disc soften its edge. Fixed taps, no noise: stills and seeks are repeatable.
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
// Open sky straight above a surface, 0..1. `slope` is the surface's rise over run (0 for flat
// ground): the taps spread sideways, and on a pitched roof or a flight of steps the ones uphill
// would find the surface itself above them, so the point is lifted by what the disc can climb.
// The base lift (shelterInfo.z) is a thickness margin: a kerb, a step or a roof tile's own relief
// is not a roof. A share under a third is a seam between two meshes, not an opening.
// FAILS OPEN: with no sky map (shelterInfo.x <= 0) everything is under open sky, so weather still
// falls on a map whose geometry has not been read yet.
float SkyOpen(float3 world,float slope){
    if(shelterInfo.x<=0)return 1;
    float radius=max(shelterInfo.w,1);
    world.z+=shelterInfo.z+min(slope,2)*radius*shelterDirection.w;
    float4 clip=mul(shelterMatrix,float4(world,1));
    float3 ndc=clip.xyz/clip.w;float2 uv=ndc.xy*float2(.5,-.5)+.5;
    if(any(uv<0)||any(uv>1)||ndc.z<0)return 1;
    int resolution=(int)shelterInfo.y;
    float2 texel=uv*resolution-.5;
    float lit=0;
    [unroll]for(int i=0;i<12;++i)lit+=ShelterCompare(texel+kSoftTaps[i]*radius,ndc.z,resolution);
    return smoothstep(.3,.85,lit/12);
}
// One comparison against the sky map: is this point in the air under open sky.
float SkyTap(float3 world){
    if(shelterInfo.x<=0)return 1;
    float4 clip=mul(shelterMatrix,float4(world,1));
    float3 ndc=clip.xyz/clip.w;float2 uv=ndc.xy*float2(.5,-.5)+.5;
    if(any(uv<0)||any(uv>1)||ndc.z<0)return 1;
    int resolution=(int)shelterInfo.y;
    return ShelterCompare(uv*resolution-.5,ndc.z,resolution);
}
// The cover volume at a point: x = open air that reaches it under cover (1 beside open sky, falling
// with every step under a roof, 0 behind a wall), y = traffic (1 indoors, falling over a few steps
// out of a doorway, 0 in the open away from any way in). Eight loads and a hand-made trilinear
// blend (it is read from vertex shaders too); both are stored as eighth roots (WeatherCoverEncode).
float2 CoverReach(float3 world){
    if(coverLow.w<.5)return 0;
    float3 g=(world-coverLow.xyz)*coverScale.xyz-.5,top=coverSize.xyz-1;
    if(any(g<-1)||any(g>top+1))return 0;
    g=clamp(g,0,top);
    int3 a=min((int3)g,(int3)top),b=min(a+1,(int3)top);float3 f=g-a;
    float2 s=lerp(
        lerp(lerp(coverTexture.Load(int4(a.x,a.y,a.z,0)),coverTexture.Load(int4(b.x,a.y,a.z,0)),f.x),
             lerp(coverTexture.Load(int4(a.x,b.y,a.z,0)),coverTexture.Load(int4(b.x,b.y,a.z,0)),f.x),f.y),
        lerp(lerp(coverTexture.Load(int4(a.x,a.y,b.z,0)),coverTexture.Load(int4(b.x,a.y,b.z,0)),f.x),
             lerp(coverTexture.Load(int4(a.x,b.y,b.z,0)),coverTexture.Load(int4(b.x,b.y,b.z,0)),f.x),f.y),f.z);
    s*=s;s*=s;return s*s;
}
// WHAT REACHES A SURFACE: x = open sky above it (the sky map), y = open air reaching in under cover,
// z = traffic (the last two from the cover volume). The sky map is exact where the sky is open; the
// volume is what lets snow and wet thin out a short way past a doorway and never cross a wall. Both
// are read a little off the surface: the sky map out from a wall, the volume half a voxel out and
// up, where its nearest voxels are air.
//
// THE WIND'S ANGLE MOVES WHERE WEATHER LANDS. Weather arrives along coverWind, not straight down:
//  - in the open, a place is missed when the line back toward the weather runs under a roof. Six
//    points up that line are tested and the share that are clear is what reaches it, so the
//    sheltered side of a wall FALLS OFF: nearly bare at its foot, more with every step out, and the
//    strip widens as the wind leans further. Snow turns that share into patches that thin out, the
//    way it does at a doorway (SnowLies). (Two points and the lesser of them was a yes or a no, and
//    drew the strip's edge as a ruled line across a yard.)
//  - under cover, the volume is read a step upwind, so an opening that faces the wind takes snow
//    and wet further in and one that faces away takes less.
// With no lean both are exactly the straight-down answers.
// `lee` is how much of the first counts (1 for what falls now). Snow that lies has come down over
// hours of shifting wind, so its sheltered side is only thinned (ApplyCoatings passes a quarter):
// cleared in full, a strong wind left a doorstep bare in the middle of a snowed-in yard.
float3 WeatherCover(float3 world,float3 normal,float lee=1){
    if(shelterInfo.x<=0)return float3(1,0,0);
    float up=saturate(normal.z);
    float slope=sqrt(saturate(1-up*up))/max(up,.35);
    float3 q=world+normal*(6*(1-up));
    float sky=SkyOpen(q,up>.35?slope:0);
    // A light lean (snow drifting down) only thins the sheltered side; a strong one clears it.
    float lean=saturate(coverWind.w),blown=smoothstep(.1,.55,lean);
    [branch]if(blown>0&&sky>0){
        float3 from=q+float3(0,0,shelterInfo.z+6);
        static const float kLee[6]={30,60,95,135,180,230};
        // Each point is three taps across and along the wind line, spread wider the further up it is
        // (a penumbra), so the share falls off smoothly. Single taps stepped it in sixths, each step
        // on a straight line that is the roof edge carried along the wind, and snow cut its patches
        // off along those lines beside containers and under ledges (2026-11-18).
        float2 along=normalize(coverWind.xy+float2(1e-5,0)),across=float2(-along.y,along.x);
        float clear=0;
        [loop]for(int k=0;k<6;++k){
            float3 at=from+coverWind.xyz*kLee[k];float spread=kLee[k]*.22;
            clear+=(SkyTap(at+float3(across*-.8*spread+along*.35*spread,0))+SkyTap(at+float3(along*-.5*spread,0))
                   +SkyTap(at+float3(across*.8*spread+along*.35*spread,0)))/3;
        }
        sky*=lerp(1,clear/6,blown*lee);
    }
    float3 half=.5/max(coverScale.xyz,1e-4);
    float3 at=world+normal*half.x+float3(0,0,half.z);
    float2 here=CoverReach(at);
    float2 upwind=blown>0?CoverReach(at+float3(coverWind.xy,0)*(lean*90)):here;
    // Upwind decides how much comes in; it cannot put weather where no open air reaches at all.
    return float3(sky,lerp(here.x,min(upwind.x,here.x*4+.02),blown),here.y);
}
// Rain wets what it lands on and a little further: water runs, splashes and is walked in.
float WetReach(float3 cover){return max(cover.x,smoothstep(.03,.3,cover.y));}
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
