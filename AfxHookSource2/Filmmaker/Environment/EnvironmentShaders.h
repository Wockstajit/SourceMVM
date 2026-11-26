#pragma once
namespace Filmmaker::Environment {
// Embedded with the DLL so a hook-only install cannot accidentally use stale shaders.
inline constexpr char kShader[] = R"HLSL(
cbuffer FrameConstants : register(b0) {
    row_major float4x4 viewProjection;
    row_major float4x4 inverseViewProjection;
    float4 camera; // w = reversed depth
    float4 dimensions; // target xy, scene depth zw
    float4 depthRange; // world viewport min/max; CS2 reserves the remaining depth for its 3D sky
    float4 fogColorDensity;
    float4 tintExposure;
    float4 weatherCenterRadius;
    float4 weatherParams; // height, size, type, seed
    float4 weatherOffset; // wrapped absolute displacement, time
    float4 weatherColor;
    float4 weatherVelocity;
    float4 cameraRight;
    float4 cameraUp;
    float4 lighting; // light count, prop ambient, cached MSAA edge mask enabled
    float4 lightPositionRange[16];
    float4 lightColorIntensity[16];
    float4 lightDirectionCone[16];
    float4 lightRightWidth[16],lightUpHeight[16],lightStudio[16],lightPatternTime[16];
    float4 climateData[CLIMATE_VECTORS];
    float4 climateClock; // absolute demo time, wind displacement
    float4 volumeDimensions; // width, height, ray steps, HDR
    float4 stormSource,stormPoints[STORM_POINTS],stormLinks[STORM_POINTS];
    // Players the coatings stick to: xyz = pawn origin, w = radius (0 = empty slot).
    // .x of the count vector is how many are filled.
    float4 coatingActors[COATING_ACTORS];
    float4 coatingActorCount;
    // Footprints (EnvironmentFootprintMath.h): x = 1 when the image of marks is bound; then where the
    // last feet came down (xyz) and climateClock.x at that moment (w).
    float4 footInfo;
    float4 footSteps[FOOT_STEPS];
    float4 footLive[FOOT_LIVE]; // the foot view: every foot as read this frame (xyz, w = lift); footInfo.y = 1 when on
    float4 footImpact[FOOT_STEPS/4]; // how hard each step in footSteps came down (FootImpact), four to a row
    float4 smokeInfo; // x = CS2 smoke absorbance bound, yz = its size, w = debug view
    float4 smokeVolumes[8]; // xyz centre, w radius (see SmokeRayShare)
    float4 smokeVolumeCount; // x = filled slots
    float4 viewmodelInfo;
    float4 relightDebug; // x view mode, y record count, z replacement ceiling
    float4 relightSun;      // xyz direction to the sun
    float4 relightSunColor; // xyz sun radiance
    float4 relightSky;      // xyz sky ambient
    float4 shadowDebug;     // x = sun cascade false-colour view, y = rain exposure
    float4 weatherLattice;    // PrecipitationLattice: cell edge, cells across, cells up, veil optical density per unit
    float4 weatherReach;      // particle window half-extents (horizontal, vertical), veil max distance
    float4 weatherRegionLow;  // xyz map precipitation box, w = mapped
    float4 weatherRegionHigh; // xyz
    // Ground fog (EnvironmentGroundFog.h). The fluid grid: xy min corner, zw one over its size (u),
    // zero when the fluid is not bound.
    float4 groundFogWindow;
    float4 groundFogFloor;     // xy floor map origin, z texel size (u), w 1 = floor map bound
    float4 groundFogFloorSize; // xy floor map texels, zw billow drift (u)
    float4 fogAmbient;         // rgb: the sky light both fogs scatter (map sky, or the Environment sky); w: fogFrontMask bound
    float4 effectAnchors[16];  // where this frame's particle effects are: xyz, radius (see FogEffectShare)
    float4 effectAnchorCount;  // x: filled slots
    float4 windFluidWindow;    // the wind fluid's grid (EnvironmentWindFluid.h): xy min corner, zw one over its size (u), zero when it is not bound
    // Each coating actor's bones (kBodyBones of them, in BodyBone order; xyz, w = 1 when read) for its
    // shadow on the ground fog (GroundFogBodyShadow); after them, the dead bodies' bones (BonesClaim).
    // coatingActorCount.w = how many bodies' bones are filled (0 when none were read).
    float4 actorBones[COATING_ACTORS*16];
    // THE VOLUME'S TEMPORAL BLEND (VolumeTemporalPS): last frame's view and camera, and
    // x = 1 when last frame's volume can be blended in, y = this frame's jitter offset (0..1),
    // z = the share the new frame gets.
    row_major float4x4 previousViewProjection;
    float4 previousCamera;
    float4 temporalInfo;
    float4 gameTextureInfo; // x = ground texture bound, y = CS2 water beads bound, z = tile size (u), w = 1 when sand
    // WHAT WET GROUND MIRRORS OFF SCREEN (EnvironmentReflectionProbes.h): x 0 none, 1 CS2's probes, 2 the
    // live capture; y the cubes' highest mip; z how many probes reflectionIndex lists; w 1 when the
    // tone curve is measured.
    float4 reflectionInfo;
    float4 reflectionIndex[4]; // the probes' record numbers in reflectionProbeRows, smallest box first
    // The map's lamps wet ground shows (EnvironmentPuddleLampMath.h, four rows each: bulb, radiance and
    // kind, then a bulb's falloff or a shaded lamp's eye and aim). x of the info: how many.
    float4 mapLamps[32];
    float4 mapLampInfo;
    // SAND AND TUMBLEWEEDS. x = 1 when what the gusts blow along the ground is sand (the Sand weather,
    // or sand lying under Clear), y = 1 when the weather's particles are blown sand (the Sand weather,
    // drawn through ash's dust), z = how many tumbleweeds are filled in below, w = their shadow's
    // strength (0 with no key light).
    float4 sandInfo;
    float4 tumbleweeds[TUMBLEWEED_MAX]; // EnvironmentTumbleweedMath.h: xyz centre, w radius
};
cbuffer NativeViewmodel : register(b7) {
    column_major float4x4 nativeProjectionToWorld : packoffset(c4);
    float4 nativeCameraOffset : packoffset(c24);
};
cbuffer ObjectConstants : register(b1) {
    row_major float4x4 model;
    row_major float4x4 normalMatrix;
    float4 objectColor;
    float4 materialParams; // roughness, metal, emission, enabled
    float4 materialCoat; // coat amount/roughness, anisotropy, normal strength
    float4 materialTextures; // image presence, UV scale
};
Texture2D<float4> volumeTexture : register(t4);
Texture2D<float4> skyVolumeTexture : register(t32);
Texture2D<float2> volumeGeometryTexture : register(t33); // full-precision distance, sky flag
Texture2D<float4> volumeHistoryTexture : register(t43);  // last frame's blended volume (VolumeTemporalPS)
Texture2D<float2> volumeHistoryGeometry : register(t44); // last frame's distance and sky flag
// CS2's own textures from the installed game (EnvironmentGameTextures.h), bound while gameTextureInfo
// says so: the Cobblestone winter snow and the water beads of its wet surfaces.
Texture2D<float4> gameSnowColor : register(t45);
Texture2D<float4> gameSnowNormal : register(t46);  // hemi-octahedral normal in rg
Texture2D<float4> gameSnowHeight : register(t47);  // height in r
Texture2D<float4> gameDroplets : register(t48);    // normal in rg, bead height in a
SamplerState gameTextureSampler : register(s9);    // wrapping, trilinear
// The cube array the off-screen reflection reads (CS2's probes, or the live capture) and the tone curve
// that puts raw scene light into the finished frame's colours (EnvironmentReflectionProbes.h).
TextureCubeArray<float4> reflectionCubes : register(t51);
Texture1D<float4> reflectionTone : register(t52);
cbuffer ReflectionProbeRows : register(b8) { float4 reflectionProbeRows[4096]; }; // a copy of CS2's cb3
Texture2D<uint> atmosphereEdgeTexture : register(t34);
Texture2D<float> smokeAbsorbance : register(t14);
Texture2D<float4> groundFogState : register(t35);    // velocity xy, fog amount (1 = at rest); EnvironmentGroundFog
Texture2D<float4> groundFogFloorMap : register(t36); // up to four floor heights per map column, highest first
Texture2D<float> groundFogSmooth : register(t37);    // soft copy of the fog amount, which the top of the layer follows
Texture2D<float4> groundFogThickness : register(t38); // how much of the layer's depth each floor in t36 holds
Texture2D<float4> fogFrontMask : register(t39);      // what transparent effects did to each pixel (see FrontMask)
// How much snow ApplyCoatings laid on the pixel being shaded, 0..1, for Atmosphere, which lays the
// game's effects back over it.
static float snowLaid=0;
static float frontEffectLift=1;
Texture2D<float> fogFrontNearby : register(t40);     // the most they hid close to each pixel (see FrontNearby)
SamplerState groundFogSampler : register(s6);
// Depth of every posed player and what they hold (world weapons), seen from the camera. Cleared to the
// far value, so a texel is either empty or the depth of the nearest player surface there.
Texture2D<float> playerMask : register(t31);
Texture2D<float4> footTexture : register(t41);
Texture2D<float4> windFluid : register(t42);        // velocity xy (u/s), what the wind carries, 1 inside a building; EnvironmentWindFluid
SamplerState windSampler : register(s8);            // linear clamp
Texture3D<float> gustNoise : register(t49);         // the billow noise blown snow, spray and ash are torn by (GustNoiseVolume); repeats
Texture2D<float> gustFloorMap : register(t50);      // the floor a gust's drift lies on, over 100000 (GroundFogDriftFloor); EnvironmentGroundFog
Texture2D<float> waterHeightMap : register(t57);    // the water grid (BuildWaterGrid): row 0 origin and cell, then heights, none below -500000
SamplerState gustNoiseSampler : register(s10);      // trilinear, wrapping
SamplerState footSampler : register(s7);             // wrapping, filtered
#if COLOR_MS
Texture2DMS<float4> colorTexture : register(t0);
Texture2DMS<float> propDepth : register(t2);
#else
Texture2D<float4> colorTexture : register(t0);
Texture2D<float> propDepth : register(t2);
#endif
#if DEPTH_MS
Texture2DMS<float> sceneDepth : register(t1);
Texture2DMS<float> beforeViewmodelDepth : register(t17);
#else
Texture2D<float> sceneDepth : register(t1);
Texture2D<float> beforeViewmodelDepth : register(t17);
#endif
#if COLOR_MS
Texture2DMS<float4> sourceColor : register(t3);
Texture2DMS<float4> nativeMaterialColor : register(t22);
Texture2DMS<float> nativeMaterialCoverage : register(t23);
Texture2DMS<float4> nativeMaterialBaseline : register(t24);
Texture2DMS<float4> nativeMaterialWithoutFixtures : register(t25);
#else
Texture2D<float4> sourceColor : register(t3);
Texture2D<float4> nativeMaterialColor : register(t22);
Texture2D<float> nativeMaterialCoverage : register(t23);
Texture2D<float4> nativeMaterialBaseline : register(t24);
Texture2D<float4> nativeMaterialWithoutFixtures : register(t25);
#endif
)HLSL"
R"HLSL(
// Owned map-light records exactly as authored, in world space. The copy the
// material shaders read is rebased to camera-relative space on the GPU; this one
// is not, so the diagnostic views can evaluate it against reconstructed world
// positions. Thirty float4 rows per record, matching the native 480-byte stride.
StructuredBuffer<float4> relightRecords : register(t26);
StructuredBuffer<uint> relightMasks : register(t27);
StructuredBuffer<uint4> relightClusterRows : register(t28);
Texture3D<float4> relightProbeVolume : register(t29);
SamplerState relightProbeSampler : register(s5);
cbuffer RelightProbeConstants : register(b6) {float4 relightProbeRows[3600];};
float3 RelightIrradiance(float3 world,float3 normal) {
    uint width,height,depth;relightProbeVolume.GetDimensions(width,height,depth);
    if(width==0)return float3(.1,0,.1);
    float4 p=float4(world,1);
    float3 uv=float3(dot(p,relightProbeRows[0]),dot(p,relightProbeRows[1]),dot(p,relightProbeRows[2]));
    uv.z/=6;uv=clamp(uv,relightProbeRows[3].xyz,relightProbeRows[4].xyz);uv.z*=6;
    uv=uv*relightProbeRows[6].xyz+relightProbeRows[7].xyz;uv.z/=6;
    float3 bands=float3(normal.x<=0?.5:0,normal.y<=0?4./6:1./6,normal.z<=0?5./6:2./6);
    float3 result=relightProbeVolume.SampleLevel(relightProbeSampler,uv+float3(0,0,bands.x),0).rgb*normal.x*normal.x;
    result+=relightProbeVolume.SampleLevel(relightProbeSampler,uv+float3(0,0,bands.y),0).rgb*normal.y*normal.y;
    result+=relightProbeVolume.SampleLevel(relightProbeSampler,uv+float3(0,0,bands.z),0).rgb*normal.z*normal.z;
    return result*relightProbeRows[5].xyz;
}
// Re-evaluates the cull test and falloff the native shader performs per record, so
// a black surface can be told apart: no record reaches it at all, or records reach
// it and contribute nothing.
void RelightProbe(float3 world,float3 normal,float2 pixel,out float reaching,out float energy) {
    reaching=0;energy=0;
    const uint records=(uint)relightDebug.y;
    uint tileRow=0,sliceRow=0;
    if(relightDebug.w>0) {
        const float clipW=dot(float4(world,1),asfloat(relightClusterRows[5]));
        uint4 list=relightClusterRows[0],grid=relightClusterRows[3];
        float4 slices=asfloat(relightClusterRows[4]);
        uint2 tile=(uint2)pixel>>grid.x;
        tileRow=(tile.y*grid.y+tile.x)*list.z+list.y;
        sliceRow=(uint)clamp(clipW*slices.x,0,slices.y)*list.z+list.x;
    }
    [loop]for(uint i=0;i<records;++i){
        if(relightDebug.w>0&&((relightMasks[tileRow+i/32]&relightMasks[sliceRow+i/32])&(1u<<(i%32)))==0)continue;
        reaching+=1;
        const uint base=i*30;
        const float4 p=float4(world,1);
        float4 clip=float4(dot(relightRecords[base+0],p),dot(relightRecords[base+1],p),
                           dot(relightRecords[base+2],p),dot(relightRecords[base+3],p));
        if(clip.w==0)continue;
        clip.xyz/=clip.w;
        if(abs(clip.x)>1||abs(clip.y)>1||clip.z<0||clip.z>1)continue;
        const float4 position=relightRecords[base+8];
        const float4 attenuation=relightRecords[base+9];
        const float3 radiance=relightRecords[base+10].rgb;
        const float3 delta=position.xyz-world;
        const float distanceSquared=max(dot(delta,delta),1e-6);
        const float distance=sqrt(distanceSquared);
        const float falloff=position.w/max(position.w,distanceSquared);
        const float curve=saturate(attenuation.x+attenuation.y*distance);
        const float facing=saturate(dot(normal,delta/distance));
        energy+=dot(radiance,float3(.2126,.7152,.0722))*falloff*curve*facing;
    }
}
// A LAMP'S OWN BULB GLOWS. A visible bulb (a sconce's globe, a lantern's glass) wraps the small light
// authored inside it, so its outward faces turn away from that light and our lighting left them dark,
// while the game shows them white (the owner, 2026-11-23). Their brightness is a native lighting input
// (the bake lit the bulb from the light inside it), present in the native baseline of the same draw.
// How far inside some lamp's housing a surface is: 1 within three quarters of the housing radius
// (half again the light's source radius, at least 6 units), 0 at its edge, the most over the lights
// the frame lists for this pixel. A light switched off has no radiance and does not count. `glow` is
// what the lamps give a surface there seen as lit head on: each housing's light at its full radiance
// (inside the source radius the falloff is 1), by the weight. With native materials owning the frame
// the native baseline is not at hand, so this is the bulb's brightness: as bright as the shade's
// underside our light already lights beside it.
float LampHousingWeight(float3 world,float2 pixel,out float3 glow){
    glow=0;
    float weight=0;
    [loop]for(uint i=0;i<(uint)mapLampInfo.x;++i){
        float4 source=mapLamps[i*4],radiance=mapLamps[i*4+1];
        if(radiance.w!=0 || source.w<=0 || source.w>64)continue;
        float radius=sqrt(source.w);
        float inside=1-smoothstep(radius*.85,radius*1.25,length(world-source.xyz));
        weight=max(weight,inside);
        glow=max(glow,radiance.rgb*inside);
    }
    return weight;
}
// Inside a housing the surface keeps the native look where that is brighter: never more than the game.
float3 LampHousingGlow(float3 lit,float3 native,float3 world,float2 pixel){
    const float3 luminance=float3(.2126,.7152,.0722);
    if(dot(native,luminance)<=dot(lit,luminance))return lit;
    float3 glow;const float weight=LampHousingWeight(world,pixel,glow);
    return weight>0?lerp(lit,native,weight):lit;
}
float3 RelightHeat(float value,float scale) {
    const float t=saturate(value*scale);
    // black, blue, cyan, green, yellow, red: readable as a count as well as a ramp.
    float3 low=lerp(float3(0,0,.6),float3(0,.8,.8),saturate(t*4));
    float3 mid=lerp(float3(0,.8,0),float3(.9,.9,0),saturate(t*4-2));
    // These colours pass through CS2's own exposure and tone map, which drives
    // anything near 1 to white. Keep the ramp dark so the steps stay readable.
    const float3 ramp=lerp(lerp(low,mid,saturate(t*4-1)),float3(1,0,0),saturate(t*4-3));
    return value<=0?float3(0,0,0):ramp*.06;
}
float NormalizeDepth(float d) {
    float span=max(depthRange.y-depthRange.x,1e-12);
    float n=(d-depthRange.x)/span;
    // CS2 keeps the leftover depth range for the 3D skybox. Saturating that into 0/1 made
    // IsSky treat distant buildings as empty sky and the procedural sky wiped them.
    if(camera.w>0){
        if(d<1e-7)return 0;
        // Keep the relative 3D-skybox depth instead of collapsing every model onto one plane.
        // The tiny reserved band remains distinguishable from both world depth and clear sky,
        // while retaining enough perspective for screen-space normals on distant buildings.
        if(n<=0)return lerp(5e-7,5e-6,saturate(d/max(depthRange.x,1e-7)));
        return saturate(n);
    }
    if(d>0.9999999)return 1;
    if(n>=1)return lerp(.99999,.999999,saturate((d-depthRange.y)/max(1-depthRange.y,1e-7)));
    return saturate(n);
}
float ReadRawSampleDepth(float2 pixel,uint sample) {
    int2 p=clamp(int2(pixel*dimensions.zw/dimensions.xy),int2(0,0),int2(dimensions.zw)-1);
#if DEPTH_MS
    uint w,h,count;sceneDepth.GetDimensions(w,h,count);
#if COLOR_MS
    return sceneDepth.Load(p,min(sample,count-1));
#else
    // Resolved HDR color has no sample index. All passes must choose the same visible depth.
    float d=sceneDepth.Load(p,0);
    for(uint i=1;i<count;++i)d=camera.w>0?max(d,sceneDepth.Load(p,i)):min(d,sceneDepth.Load(p,i));
    return d;
#endif
#else
    return sceneDepth.Load(int3(p,0));
#endif
}
float ReadSampleDepth(float2 pixel,uint sample) { return NormalizeDepth(ReadRawSampleDepth(pixel,sample)); }
float ReadScene(float2 pixel) {
    int2 p=clamp(int2(pixel*dimensions.zw/dimensions.xy),int2(0,0),int2(dimensions.zw)-1);
#if DEPTH_MS
    uint w,h,samples; sceneDepth.GetDimensions(w,h,samples);
    float d=sceneDepth.Load(p,0);
    for(uint i=1;i<samples;++i) d=camera.w>0?max(d,sceneDepth.Load(p,i)):min(d,sceneDepth.Load(p,i));
    return NormalizeDepth(d);
#else
    return NormalizeDepth(sceneDepth.Load(int3(p,0)));
#endif
}
// HOW MUCH OF A SURFACE CS2'S SMOKE HIDES.
//
// Smoke writes no scene depth, so every environment pass used to shade the ground BEHIND a smoke:
// snow, wetness and relighting showed through it and a coated player stood out inside one.
// CS2 renders smoke (and its other transparents) with moment-based OIT; the full-resolution
// zeroth moment is the total absorbance in front of each opaque surface, so exp(-b0) is the
// share of that surface still visible. RenderSystemDX11Hooks copies it every frame the
// environment is on -- with or without ReShade -- and binds it here.
float SmokeCover(float2 pixel) {
    if(smokeInfo.x<.5) return 0;
    int2 p=clamp(int2(pixel*smokeInfo.yz/dimensions.xy),int2(0,0),int2(smokeInfo.yz)-1);
    return saturate(1-exp(-max(smokeAbsorbance.Load(int3(p,0)),0)));
}
// WHICH COVERAGE IS A SMOKE. CS2's OIT absorbance holds every transparent in front of a surface:
// glass, fire, foliage cards and particles as well as smoke. Only coverage on a ray that actually
// passes through a placed smoke belongs to that smoke. A CS2 cloud billows past the sphere we
// place for it, so the test widens each sphere and fades out over a margin instead of cutting.
// Returns that share (0..1) and where along the ray the smoke starts (-1 when the ray misses).
//
// This used to take ANY covered ray as smoke, measured from the nearest smoke on the map: with one
// smoke out anywhere, every window, flame and leaf card had its fog cut short and stood out of the
// fog crisp and bright (reported 2026-11-07). Reference: SmokeRayShare in EnvironmentClimateMath.h.
float SmokeRayShare(float3 dir,float maxT,out float entry) {
    // Smoke positions unknown: coverage is only handed over while smokes exist, so all of it is
    // taken as smoke starting at the camera, the conservative reading.
    if(smokeVolumeCount.x<.5){entry=0;return 1;}
    entry=-1;float share=0;
    int count=(int)smokeVolumeCount.x;
    for(int i=0;i<count;++i){
        float r=smokeVolumes[i].w; if(r<=0) continue;
        float3 oc=camera.xyz-smokeVolumes[i].xyz;
        float b=dot(oc,dir);
        // Closest approach of the ray to the smoke centre, within the visible stretch of the ray.
        float closest=clamp(-b,0,maxT);
        float miss=length(oc+dir*closest);
        float w=1-smoothstep(r*1.15,r*1.6,miss);
        if(w<=0) continue;
        float h=b*b-(dot(oc,oc)-r*r);
        float t=h>=0?max(-b-sqrt(h),0):closest;
        if(t>maxT) continue;
        if(entry<0||t<entry) entry=t;
        share=max(share,w);
    }
    return share;
}
// How much of something at `world` a smoke hides: only what lies BEHIND the start of a smoke the
// ray passes through. Other transparents hide nothing here; they are already in the frame.
float SmokeHides(float2 pixel,float3 world) {
    float cover=SmokeCover(pixel);
    if(cover<=0) return 0;
    if(smokeVolumeCount.x<.5) return cover; // positions unknown: whatever it covers is hidden
    float3 ray=world-camera.xyz; float dist=length(ray);
    float entry;
    cover*=SmokeRayShare(ray/max(dist,.0001),dist,entry);
    if(cover<=0||entry<0||entry>=dist)return 0;
    bool inside=entry<=0;
    float fraction=inside?saturate(dist/24):saturate((dist-entry)/80);
    float absorbance=-log(max(1-cover,1e-8));
    return 1-exp(-absorbance*fraction);
}
float ReadProp(float2 p,uint sample) {
#if COLOR_MS
    return propDepth.Load(int2(p),sample);
#else
    return propDepth.Load(int3(p,0));
#endif
}
float ReadCombinedSample(float2 p,uint sample) {float a=ReadSampleDepth(p,sample),b=ReadProp(p,sample);return camera.w>0?max(a,b):min(a,b);}
float ReadDepth(float2 p) {
    float a=ReadScene(p),b=ReadProp(p,0);
#if COLOR_MS
    uint w,h,n;propDepth.GetDimensions(w,h,n);
    for(uint i=1;i<n;++i)b=camera.w>0?max(b,ReadProp(p,i)):min(b,ReadProp(p,i));
#endif
    return camera.w>0?max(a,b):min(a,b);
}
float2 SamplePixel(float2 pixel,uint sample) {
#if COLOR_MS
    return floor(pixel)+.5+sourceColor.GetSamplePosition(sample);
#else
    return pixel;
#endif
}
float3 ReadSource(float2 p,uint sample) {
    int2 q=clamp(int2(p),int2(0,0),int2(dimensions.xy)-1);
#if COLOR_MS
    return sourceColor.Load(q,sample).rgb;
#else
    return sourceColor.Load(int3(q,0)).rgb;
#endif
}
float3 ReadColor(float2 p,uint sample) {
    int2 q=clamp(int2(p),int2(0,0),int2(dimensions.xy)-1);
#if COLOR_MS
    return colorTexture.Load(q,sample).rgb;
#else
    return colorTexture.Load(int3(q,0)).rgb;
#endif
}
bool IsSky(float depth) { return camera.w>0?depth<.0000001:depth>.9999999; }
float SkyDepth() { return camera.w>0?0:1; }
)HLSL"
R"HLSL(
// SKY EDGES OF A RESOLVED FRAME. With an MSAA depth under a resolved colour each pixel is shaded
// for its nearest sample, so a pixel along a wire or a leaf seen against the sky is taken as wire,
// while its colour holds the sky as it was drawn for the samples that missed the wire: the clouds,
// rain and fog the passes lay over the sky left that share uncovered, a bright outline of the plain
// sky around every thin thing in a dark rain sky. This is the share of the pixel's depth samples that
// are open sky (a prop in front covers the pixel); 0 with an MSAA colour, which is shaded per sample.
float EdgeSkyShare(float2 pixel) {
#if DEPTH_MS && !COLOR_MS
    if(!IsSky(ReadProp(pixel,0)))return 0;
    int2 p=clamp(int2(pixel*dimensions.zw/dimensions.xy),int2(0,0),int2(dimensions.zw)-1);
    uint w,h,count;sceneDepth.GetDimensions(w,h,count);
    uint open=0;
    [loop]for(uint i=0;i<count;++i)open+=IsSky(NormalizeDepth(sceneDepth.Load(p,i)))?1:0;
    return open/(float)max(count,1);
#else
    return 0;
#endif
}
// The sky as it was drawn next to an edge pixel: the mean of the neighbours whose samples are all
// open sky, one pixel away, else two, in the frame so far or (`source`) the frame as CS2 drew it.
// `found` is 0 when there is none.
float3 EdgeSkyColor(float2 pixel,out float found,bool source) {
    float3 sum=0;found=0;
    [loop]for(int r=1;r<=2&&found<=0;++r)
        [loop]for(int k=0;k<8;++k){
            float2 q=clamp(pixel+r*float2(k==2||k==6?0:(k<2||k>6?1:-1),k==0||k==4?0:(k<4?1:-1)),.5,dimensions.xy-.5);
            if(IsSky(ReadScene(q))&&IsSky(ReadProp(q,0))){sum+=source?ReadSource(q,0):ReadColor(q,0);found+=1;}
        }
    return found>0?sum/found:0;
}
// CS2 stores 3D skybox in the leftover depth range (viewport max 0.95, clear sky at 0 or 1).
// NormalizeDepth encodes that leftover as 1e-6 (reversed) or ~0.999999 (forward) so IsSky stays
// false. Fog/clouds must not treat those pixels as a 24km empty ray.
bool IsSkyboxGeometry(float depth) {
    return camera.w>0?(depth>5e-7&&depth<5e-6):(depth>0.99999&&!IsSky(depth));
}
bool Hidden(float value,float scene) { return camera.w>0?value+0.000001<scene:value-0.000001>scene; }
float3 World(float2 pixel,float depth) {
    float4 h=mul(inverseViewProjection,float4(pixel/dimensions.xy*float2(2,-2)+float2(-1,1),depth,1));
    return h.xyz/h.w;
}
bool IsViewmodelSample(float2 pixel,uint sample){
    if(viewmodelInfo.x<.5)return false;
    int2 p=clamp(int2(pixel*dimensions.zw/dimensions.xy),int2(0,0),int2(dimensions.zw)-1);
    float raw=ReadRawSampleDepth(pixel,sample);
    [branch] if(raw>.100001)return false;
#if DEPTH_MS
    uint w,h,count;beforeViewmodelDepth.GetDimensions(w,h,count);
    uint selected=min(sample,count-1);
#if !COLOR_MS
    // Pair the saved depth with the same sample chosen by ReadRawSampleDepth.
    // A resolved edge may have a hand in one sample and a near wall in another.
    selected=0;float nearest=sceneDepth.Load(p,0);
    for(uint i=1;i<count;++i){float candidate=sceneDepth.Load(p,i);
        if(camera.w>0?candidate>nearest:candidate<nearest){nearest=candidate;selected=i;}}
#endif
    float before=beforeViewmodelDepth.Load(p,selected);
#else
    float before=beforeViewmodelDepth.Load(int3(p,0));
#endif
    return raw<=.100001 && raw<before-.000001;
}
// Normal fitting already classifies each tap. Reuse that result instead of walking
// the MSAA samples again. Keep the depth match for placed geometry in combined depth.
float3 ReceiverPositionClassified(float2 pixel,float depth,uint sample,bool viewmodel){
    if(viewmodel && abs(depth-ReadSampleDepth(pixel,sample))<.000001){
        float4 h=mul(nativeProjectionToWorld,float4(pixel/dimensions.xy*float2(2,-2)+float2(-1,1),ReadRawSampleDepth(pixel,sample)*10,1));
        // CS2's viewmodel pass can be camera-relative even when the world pass is not.
        // Unproject the perspective eye (clip w=0, z=1), subtract it in the SAME native
        // coordinate space, then anchor the offset at the actual filmmaker camera.
        float4 eye=mul(nativeProjectionToWorld,float4(0,0,1,0));
        if(abs(eye.w)>1e-8)return camera.xyz+h.xyz/h.w-eye.xyz/eye.w;
    }
    return World(pixel,depth);
}
float3 ReceiverPosition(float2 pixel,float depth,uint sample){
    return ReceiverPositionClassified(pixel,depth,sample,IsViewmodelSample(pixel,sample));
}
// One-sided derivatives choose neighbors on the same surface instead of crossing foliage/sky edges.
float3 SurfaceNormalAt(float2 pixel,float depth,uint sample,bool combined,float3 center,bool viewmodel) {
    float3 v[4];float dist[4];
    float2 delta[4]={float2(-1,0),float2(1,0),float2(0,-1),float2(0,1)};
    [unroll]for(int j=0;j<4;++j){
        float2 q=clamp(pixel+delta[j],.5,dimensions.xy-.5);
        float d=combined?ReadCombinedSample(q,sample):ReadSampleDepth(q,sample);
        bool neighborViewmodel=IsViewmodelSample(q,sample);
        v[j]=ReceiverPositionClassified(q,d,sample,neighborViewmodel)-center;dist[j]=(IsSky(d)||all(q==pixel)||viewmodel!=neighborViewmodel)?1e20:dot(v[j],v[j]);
    }
    float3 dx=dist[0]<dist[1]?-v[0]:v[1],dy=dist[2]<dist[3]?-v[2]:v[3];
    float3 normal=cross(dx,dy);float length2=dot(normal,normal);
    if(length2<1e-10||min(dist[0],dist[1])>max(4,dot(center-camera.xyz,center-camera.xyz)*.001)||min(dist[2],dist[3])>max(4,dot(center-camera.xyz,center-camera.xyz)*.001))return normalize(camera.xyz-center);
    normal*=rsqrt(length2);return dot(normal,camera.xyz-center)<0?-normal:normal;
}
float3 SurfaceNormal(float2 pixel,float depth,uint sample,bool combined) {
    bool viewmodel=IsViewmodelSample(pixel,sample);
    float3 center=ReceiverPositionClassified(pixel,depth,sample,viewmodel);
    return SurfaceNormalAt(pixel,depth,sample,combined,center,viewmodel);
}
// Depth reconstruction naturally returns one normal per low-poly triangle. That is accurate for
// walls but ugly on rounded engine props (bags, barrels, pipes), where environment relighting made
// every source triangle visible. Average only continuous, similarly-facing nearby surfaces; the
// world-space gate keeps silhouettes, wall junctions and separate objects sharp.
)HLSL"
R"HLSL(
float3 FilterSurfaceNormalAt(float2 pixel,float depth,uint sample,bool combined,float3 base,float radius,float limit,float3 center,bool viewmodel) {
    float2 tap[8]={float2(-1,0),float2(1,0),float2(0,-1),float2(0,1),
                   float2(-.707,-.707),float2(.707,-.707),float2(-.707,.707),float2(.707,.707)};
    float3 x=0,y=0;float xx=0,xy=0,yy=0;
    // Fit one local tangent plane. Reconstructing a whole 4-neighbor normal at each
    // of these taps repeated depth/projection work and magnified quantization noise.
    [unroll]for(int k=0;k<8;++k){
        float2 q=SamplePixel(floor(pixel+tap[k]*radius)+.5,sample);
        if(any(q<.5)||any(q>dimensions.xy-.5))continue;
        float d=combined?ReadCombinedSample(q,sample):ReadSampleDepth(q,sample);
        if(IsSky(d)||IsSkyboxGeometry(d)||viewmodel!=IsViewmodelSample(q,sample))continue;
        float3 qp=ReceiverPositionClassified(q,d,sample,viewmodel),delta=qp-center;
        float distance2=dot(delta,delta),error=abs(dot(delta,base)),planeLimit=.5+sqrt(distance2)*.3;
        if(distance2>limit*limit||error>=planeLimit)continue;
        float w=saturate(1-error/planeLimit)*exp(-distance2/max(limit*limit,.001));
        float2 t=(q-pixel)/radius;
        x+=delta*(t.x*w);y+=delta*(t.y*w);xx+=t.x*t.x*w;xy+=t.x*t.y*w;yy+=t.y*t.y*w;
    }
    if(xx*yy-xy*xy<1e-5)return base;
    float3 n=cross(x*yy-y*xy,y*xx-x*xy);
    if(dot(n,n)<1e-12)return base;
    n=normalize(n);if(dot(n,camera.xyz-center)<0)n=-n;
    return dot(n,base)>.45?n:base;
}
float3 FilterSurfaceNormal(float2 pixel,float depth,uint sample,bool combined,float3 base,float radius,float limit) {
    bool viewmodel=IsViewmodelSample(pixel,sample);
    float3 center=ReceiverPositionClassified(pixel,depth,sample,viewmodel);
    return FilterSurfaceNormalAt(pixel,depth,sample,combined,base,radius,limit,center,viewmodel);
}
float3 RelightNormal(float2 pixel,float depth,uint sample,bool combined) {
    bool viewmodel=IsViewmodelSample(pixel,sample);
    float3 center=ReceiverPositionClassified(pixel,depth,sample,viewmodel);
    float3 base=SurfaceNormalAt(pixel,depth,sample,combined,center,viewmodel);
    return FilterSurfaceNormalAt(pixel,depth,sample,combined,base,viewmodel?12:6,max(1.5,length(center-camera.xyz)*.01),center,viewmodel);
}
// Small, deterministic, viewmodel-only contact occlusion. This is visible-surface AO,
// not an animated shadow caster or a substitute for authored material AO/normal maps.
float ViewmodelContactOcclusion(float2 pixel,float depth,uint sample,float3 p,float3 normal) {
    if(!IsViewmodelSample(pixel,sample))return 1;
    const float radius=1.5;
    float4 h=mul(nativeProjectionToWorld,float4(pixel/dimensions.xy*float2(2,-2)+float2(-1,1),ReadRawSampleDepth(pixel,sample)*10,1));
    float4 hx=h+mul(nativeProjectionToWorld,float4(2/dimensions.x,0,0,0));
    if(abs(h.w)<1e-8||abs(hx.w)<1e-8)return 1;
    float footprint=length(hx.xyz/hx.w-h.xyz/h.w);
    float pixels=clamp(radius/max(footprint,.001),2,max(2,dimensions.y*.08));
    float2 taps[8]={float2(1,0),float2(.707,.707),float2(0,1),float2(-.707,.707),
                    float2(-1,0),float2(-.707,-.707),float2(0,-1),float2(.707,-.707)};
    float obstruction=0;
    [unroll]for(int k=0;k<8;++k){
        float2 q=SamplePixel(floor(pixel+taps[k]*pixels*((k&1)?.8:.4))+.5,sample);
        if(any(q<.5)||any(q>dimensions.xy-.5)||!IsViewmodelSample(q,sample))continue;
        float d=ReadSampleDepth(q,sample);
        float3 delta=ReceiverPosition(q,d,sample)-p;float distance2=dot(delta,delta);
        if(distance2<1e-6||distance2>=radius*radius)continue;
        float horizon=saturate(((dot(normal,delta)-.025)*rsqrt(distance2)-.15)/.85);
        obstruction+=horizon*(1-distance2/(radius*radius));
    }
    return exp(-1.5*obstruction/8);
}
float3 SkyboxPosition(float2 pixel,uint sample) {
    // Match the separate CS2 sky projection used by afx_depth_ps_5_0. The tiny classification
    // band is NOT a projection: unprojecting it with the main camera flattens building normals.
    float raw=ReadRawSampleDepth(pixel,sample);
    float z=camera.w>0?1-saturate(raw/max(depthRange.x,1e-7)):
        saturate((raw-depthRange.y)/max(1-depthRange.y,1e-7));
    const float nearSky=32,farSky=908093.453;
    float axial=nearSky*farSky/max(nearSky,farSky-z*(farSky-nearSky));
    float farDepth=camera.w>0?.00001:.99999;
    float3 ray=normalize(World(pixel,farDepth)-camera.xyz);
    float3 forward=normalize(World(dimensions.xy*.5,farDepth)-camera.xyz);
    return ray*(axial/max(.001,dot(ray,forward)));
}
float3 SkyboxNormal(float2 pixel,float depth,uint sample) {
    float3 center=SkyboxPosition(pixel,sample),v[4];float dist[4];
    float2 delta[4]={float2(-2,0),float2(2,0),float2(0,-2),float2(0,2)};
    [unroll]for(int j=0;j<4;++j){
        float2 q=clamp(pixel+delta[j],.5,dimensions.xy-.5);float d=ReadSampleDepth(q,sample);
        bool valid=IsSkyboxGeometry(d);
        v[j]=SkyboxPosition(q,sample)-center;dist[j]=valid?dot(v[j],v[j]):1e20;
    }
    float3 dx=dist[0]<dist[1]?-v[0]:v[1],dy=dist[2]<dist[3]?-v[2]:v[3];
    float3 normal=cross(dx,dy);float length2=dot(normal,normal);
    if(length2<1e-12||min(dist[0],dist[1])>=1e19||min(dist[2],dist[3])>=1e19)
        return normalize(-center);
    normal*=rsqrt(length2);return dot(normal,-center)<0?-normal:normal;
}
)HLSL"
#include "EnvironmentCoatingShaders.h"
R"HLSL(
struct Screen { float4 position:SV_Position; };
Screen ScreenVS(uint id:SV_VertexID) { Screen o; o.position=float4(id==2?3:-1,id==1?3:-1,0,1); return o; }

struct MeshInput { float3 position:POSITION; float3 normal:NORMAL; float2 uv:TEXCOORD0; };
)HLSL"
#include "EnvironmentStudioShaders.h"
#include "EnvironmentShadowShaders.h"
R"HLSL(
float3 FillLights(float3 position,float3 normal,bool viewmodel=false) {
    float3 energy=0;
    for(int l=0;l<(int)lighting.x;++l) {
        [loop]for(int sample=0;sample<StudioSamples(l);++sample){
            float3 dir;float atten=StudioAttenuation(l,sample,position,dir);
            if(atten>0)atten*=WorldLightShadowSample(l,position+normal*shadowInfo.y,sample,viewmodel);
            energy+=lightColorIntensity[l].rgb*lightColorIntensity[l].w*atten*saturate(dot(normal,dir));
        }
    }
    return energy;
}
)HLSL"
#include "EnvironmentGiTraceShaders.h"
#include "EnvironmentClimateShaders.h"
#include "EnvironmentMaterialShaders.h"
R"HLSL(
float3 Grade(float3 color,float distance) {
    color*=tintExposure.rgb*tintExposure.w;
    // Fog is composited once over combined scene/prop depth in VolumeAt.
    return color;
}
float3 NativePostColor(float3 color) {
    // The native post shader expects reversible max-channel compression. Keep the
    // largest half-float channel strictly below one so its inverse stays finite.
    if(relightSky.w<.5)return color;
    color=max(color,0);
    float peak=max(color.r,max(color.g,color.b));
    return color*min(rcp(1+peak),.99951171875/max(peak,1e-6));
}
float3 NativePostLinear(float3 color) {
    return relightSky.w>.5 ? color/max(1-max(color.r,max(color.g,color.b)),.00048828125) : color;
}
)HLSL"
R"HLSL(
struct MeshOutput { float4 position:SV_Position; float3 normal:NORMAL; float3 world:TEXCOORD0; float2 uv:TEXCOORD1; };
MeshOutput MeshVS(MeshInput i) {
    MeshOutput o; o.world=mul(model,float4(i.position,1)).xyz; o.position=mul(viewProjection,float4(o.world,1));
    o.normal=normalize(mul(normalMatrix,float4(i.normal,0)).xyz);o.uv=i.uv; return o;
}
float4 ShadeMesh(MeshOutput i,uint sample,int kind) {
    if(Hidden(i.position.z,ReadSampleDepth(i.position.xy,sample))) discard;
    // A placed prop has no depth relation to the smoke either; hide it by the smoke in front of the
    // scene there, the same as every other addition.
    float smoke=SmokeHides(i.position.xy,i.world);
    if(smoke>=.999) discard;
    // The mesh pass blends opaquely: veil toward the smoke-covered scene here instead of via alpha.
    float3 scene=ReadSource(i.position.xy,sample);
    if(kind==1)return float4(lerp(Grade(ShadeMaterial(i.world,i.normal,i.uv),length(i.world-camera.xyz)),scene,smoke),1);
    float lambert=.25+.75*saturate(dot(normalize(i.normal),normalize(float3(.3,-.5,1))));
    float3 color=objectColor.rgb*lambert*lighting.y;
    return float4(lerp(Grade(Surface(color,i.world,normalize(i.normal),normalize(i.normal)),length(i.world-camera.xyz)),scene,smoke),1);
}
// The CPU already knows the mesh material. Specialize these identical branches before
// optimization instead of compiling PBR and legacy shading as one program.
float4 MeshPS(MeshOutput i,uint sample:SV_SampleIndex):SV_Target {return ShadeMesh(i,sample,0);}
float4 MaterialMeshPS(MeshOutput i,uint sample:SV_SampleIndex):SV_Target {return ShadeMesh(i,sample,1);}

// SOFT POROUS SHADOW OF ROLLING TUMBLEWEEDS (sandInfo.z count, sandInfo.w sun-shadow strength).
// Placed meshes do not cast into CS2's baked lighting, so each tumbleweed casts:
//  1. soft contact occlusion directly beneath/beside it where twigs meet the floor or a wall;
//  2. a porous directional sun shadow along -SunDirection() letting filtered light through the brush.
float TumbleweedShadow(float3 p,float3 normal) {
    int count=(int)sandInfo.z;
    float3 sun=SunDirection();
    float sunStrength=sandInfo.w*saturate(sun.z*4);
    float keep=1;
    [loop]for(int i=0;i<count;++i){
        float4 tw=tumbleweeds[i];
        float r=max(tw.w,.001),on=step(.001,tw.w);
        float3 d=tw.xyz-p;
        float horiz=length(d.xy),below=d.z;
        float radial=1-smoothstep(r*.15,r*1.35,horiz);
        float vert=smoothstep(-r*.25,r*.2,below)*(1-smoothstep(r*.7,r*1.85,below));
        keep*=1-.44*on*radial*vert*saturate(normal.z*.7+.3);
        float along=dot(d,sun);
        float perp=length(d-sun*along);
        float soft=r*(.28+max(along,0)*.045);
        float mask=1-smoothstep(r*.35,r*.98+soft,perp);
        float reach=step(0,along)*(1-smoothstep(r*8,r*14,along));
        keep*=1-.46*on*sunStrength*mask*reach*saturate(dot(normal,sun)*2+.2);
    }
    return keep;
}
float3 ShadeSample(float2 pixel,float depth,float3 color,uint sample) {
    bool sky=IsSky(depth);
    float3 position=sky?camera.xyz:ReceiverPosition(pixel,depth,sample);
    float distance=sky?8000:length(position-camera.xyz);
    // A true clear-depth sample is the map's sky MATERIAL and must be replaced completely.
    // Trying to preserve colour detail here kept the stock sky's painted clouds, producing two
    // skies at once. Real 3D-skybox buildings use the reserved depth range encoded above and are
    // already protected by !IsSky / IsSkyboxGeometry.
    // Native sky geometry already supplied these samples before HDR resolved.
    // The display target broadcasts resolved color, despite remaining MSAA.
    // Replacing only its clear-depth samples would weight the foreground twice.
    if(sky && CL_sky>0 && relightSunColor.w<.5)
        color=lerp(color,SkyColor(normalize(World(pixel,camera.w>0?.00001:.99999)-camera.xyz),1,false),CL_sky);
    // Skip surface reconstruction when none of its consumers contributes. Each term is the
    // exact complement of that consumer's own zero case: coatings (ApplyCoatings early-out),
    // relight/sun relight (multiplied through), FillLights (loops lighting.x) and WorldBounce
    // (returns 0 when worldGi<.5 or bounce<=0 -- hence >=.5 here, not >.5).
    // 3D skybox leftover depth is not a reconstructable world surface.
    [branch] if(!sky && !IsSkyboxGeometry(depth) && (viewmodelInfo.w>.5 || (viewmodelInfo.z>.5 && mapLampInfo.x>0) || CL_snow>0 || CL_sand>0 || sandInfo.z>.5 || CL_wet>0 || CL_puddles>0 || CL_relight>0 || CL_sunRelight>0 ||
                       lighting.x>0 || weatherParams.z==1 || (CL_worldGi>=.5 && CL_bounce>0))) {
        float3 normal=RelightNormal(pixel,depth,sample,false);
        float3 coating=normal;
        [branch] if(CL_snow>0||CL_sand>0||CL_wet>0||CL_puddles>0)coating=CoatingNormal(pixel,depth,sample,normal);
        bool viewmodel=IsViewmodelSample(pixel,sample);
        float contactOcclusion=1; // Preserve native hand/weapon contact shading; no synthetic self occlusion.
        float4 material=0;
        [branch]if(viewmodelInfo.y>.5){
#if COLOR_MS
            material=nativeMaterialColor.Load((int2)pixel,sample);
#else
            material=nativeMaterialColor.Load(int3(pixel,0));
#endif
        }
        float nativeCoverage=0;float3 nativeBase=0,nativeWithoutFixtures=0;
        [branch]if(viewmodelInfo.y>.5){
#if COLOR_MS
            nativeCoverage=nativeMaterialCoverage.Load((int2)pixel,sample);
            nativeBase=nativeMaterialBaseline.Load((int2)pixel,sample).rgb;
            nativeWithoutFixtures=nativeMaterialWithoutFixtures.Load((int2)pixel,sample).rgb;
#else
            nativeCoverage=nativeMaterialCoverage.Load(int3(pixel,0));
            nativeBase=nativeMaterialBaseline.Load(int3(pixel,0)).rgb;
            nativeWithoutFixtures=nativeMaterialWithoutFixtures.Load(int3(pixel,0)).rgb;
#endif
        }
        // Sun cascade false-colour. Ramp colours stay dark on purpose: these still pass through
        // CS2's exposure and tone map, which drives anything near one to white.
        // Weather cover view: grey under open sky, dark blue under cover, green where open air still
        // reaches in, red where people walk in and out (traffic).
        // Dark values on purpose: they still pass through CS2's exposure and tone map.
        [branch]if(shadowDebug.y>0){float3 cover=WeatherCover(position,normal);return lerp(float3(.01,.015,.07),float3(.18,.18,.18),cover.x)+float3(0,.14,0)*(1-cover.x)*smoothstep(.03,.45,cover.y)+float3(.2,.02,0)*smoothstep(.06,.5,cover.z)+FootMarks(position).xyx*.5;}
        [branch]if(shadowDebug.x>0){
            int cascade=SunCascadeAt(position);
            float visibility=CL_contactShadows>0?WorldSunShadowSoft(position+SunDirection()*shadowInfo.y,normal,IsViewmodelSample(pixel,sample)):1;
            // Shadowed pixels of a tier read at half its colour, so the map each shadow was
            // resolved from is legible rather than a flat region wash.
            float3 tint=cascade==1?float3(1,.15,.15)   // close
                       :cascade==2?float3(.15,1,.15)   // detail
                       :cascade==3?float3(.25,.45,1)   // middle
                       :cascade==4?float3(1,.85,.2)    // outer
                       :float3(1,0,1);                 // none: this surface can never be shadowed
            return tint*(cascade==0?.18:lerp(.05,.18,visibility));
        }
        [branch]if(relightDebug.x>3.5)return saturate(RelightIrradiance(position,normal));
        [branch]if(relightDebug.x>0){
            float reaching,energy;RelightProbe(position,normal,pixel,reaching,energy);
            // 1: how many records reach this surface. 2: what they deliver there.
            // 3: whether the material was replaced at all (magenta = it was not).
            if(relightDebug.x<1.5)return RelightHeat(reaching,1./128);
            if(relightDebug.x<2.5)return RelightHeat(log2(1+energy),1./8);
            return (nativeCoverage>0?float3(0,nativeCoverage,0):float3(1,0,1))*.06;
        }
        // WATER. Water writes no depth, so a pixel of a pool is the pool's bed. Under the map's own
        // lighting the game's water is left as drawn (relit, the bed replaced its reflections with a
        // flat floor). Under ours the engine draws the water unlit, so the bed is lit by us and the
        // water shaded over it (WaterSurfaceShade). The owner, 2026-11-23.
        float waterLevel=viewmodel?-1e6:WaterLevelAt(position.xy);
        bool underWater=waterLevel>-5e5&&position.z<waterLevel+3;
        bool ourLighting=CL_relight>0||viewmodelInfo.z>.5;
        [branch]if(viewmodelInfo.w>.5&&nativeCoverage>0&&(!underWater||ourLighting)){
            // Optional diagnostic clamp precedes native HDR input encoding.
            float3 owned=relightDebug.z>0?min(material.rgb,relightDebug.z):material.rgb;
            [branch]if(underWater)owned=WaterSurfaceShade(owned,position,waterLevel);
            [branch]if(!viewmodel&&!underWater&&viewmodelInfo.y>.5)owned=LampHousingGlow(owned,nativeBase,position,pixel);
            return Grade(owned,distance);
        }
        float3 nativeDirect=nativeCoverage>0&&volumeDimensions.w>.5?max(material.rgb-nativeBase,0):float3(-1,-1,-1);
        // One call to the coatings for both paths (see SurfaceLight): same result as coating each.
        float mapLuminance=dot(max(color,0),float3(.2126,.7152,.0722));
        if(viewmodelInfo.z<=.5&&!(underWater&&!ourLighting))color=SurfaceLight(color,position,normal,viewmodel,contactOcclusion,nativeDirect,max(nativeBase-nativeWithoutFixtures,0));
        // Native materials own the frame: a lamp's bulb glows with its own light (LampHousingWeight).
        [branch]if(viewmodelInfo.z>.5&&!viewmodel&&!underWater){float3 glow;if(LampHousingWeight(position,pixel,glow)>0)color=max(color,glow);}
        [branch]if(underWater&&ourLighting)color=WaterSurfaceShade(color,position,waterLevel);
        color=ApplyCoatings(color,position,normal,coating,mapLuminance,viewmodel);
        // Impacts and tumbleweed shadows belong to map surfaces, never the first-person gun or
        // player bodies. Share one CoatingActor check so fxc does not inline it twice in ShadeSample.
        [branch]if(!viewmodel&&(sandInfo.z>.5||(weatherParams.z==1&&weatherColor.w>0))&&!CoatingActor(position,normal)){
            [branch]if(sandInfo.z>.5)color*=TumbleweedShadow(position,normal);
            [branch]if(weatherParams.z==1&&weatherColor.w>0)color=ActiveRainImpacts(color,position,normal,pixel,sample);
        }
        // PLAYER FILL. Under dynamic lighting a player in shade is far darker than the wall beside
        // them. Their darkest shade is lifted by CL_playerFill, less the brighter the pixel already
        // is, so a player in the sun is left as lit.
        if(CL_playerFill>1&&(CL_relight>0||viewmodelInfo.z>.5)&&!viewmodel&&PlayerMaskClaims(position))
            color*=lerp(CL_playerFill,1,saturate(dot(max(color,0),float3(.2126,.7152,.0722))/.3));
    }
    // On the composite-lit path 3D-skybox buildings and the white light shafts never take the
    // material lighting: their albedo already contains daytime light, or they are additive cards
    // with no light slot. Scale that baked brightness by the same sun height the world uses. Clear
    // sky (stars) stays on the sky path. The composite's CL_relight is cleared once native materials
    // own the frame, so the native-lighting flag is the switch that is actually on.
    // Edges match BakedDaylightScale / UnlitOverlayScale.
    if(CL_relight>0 || viewmodelInfo.z>.5){
        float day=smoothstep(-10,8,CL_sunElevation);
        float nightFloor=max(saturate(CL_ambient)*.35,.02);
        float scale=lerp(nightFloor,1,day);
        // With native materials the skybox's lit draws are relit like the map and its unlit ones
        // (haze cards, painted skylines) are dimmed as they are drawn (SkyboxUnlitDim in
        // EnvironmentNativeMaterialDraw.cpp). Scaling it here as well left Mirage's skyline far
        // darker than the map at night.
        if(IsSkyboxGeometry(depth)){if(viewmodelInfo.z<.5)color*=scale;}
        // Native materials dim the additive shafts at draw time (UnlitOverlayScale), and every
        // other bright pixel here is a surface the native path already lit -- a placed lamp's
        // pool at night. Darkening those again by a luminance threshold cut the pool into a
        // crust wherever the albedo crossed it (issue 41), so only the composite-lit path keeps it.
        else if(viewmodelInfo.z<.5 && !sky && !IsViewmodelSample(pixel,sample)){
            float lum=dot(max(color,0),float3(.2126,.7152,.0722));
            if(lum>.22) color*=scale*lerp(.55,1,day);
        }
    }
    return Grade(color,distance);
}
// MSAA cost split. Shading every sample of every pixel multiplied the whole surface pass by the
// sample count (8x MSAA made it the frame's largest cost). Only pixels whose samples lie on
// different surfaces need per-sample shading: an edge is any sample whose depth differs from
// sample 0 by more than 1 %. Interior pixels are shaded once by AtmospherePixelPS, which writes
// every sample; edge pixels keep per-sample shading in AtmospherePS. Each pass discards the
// other's pixels, so together they cover the frame exactly once.
bool AtmosphereEdge(float2 pixel) {
#if COLOR_MS && DEPTH_MS
    int2 p=clamp(int2(pixel*dimensions.zw/dimensions.xy),int2(0,0),int2(dimensions.zw)-1);
    uint w,h,count;sceneDepth.GetDimensions(w,h,count);
    float d0=sceneDepth.Load(p,0);
    [loop]for(uint s=1;s<count;++s){
        float d=sceneDepth.Load(p,s);
        if(abs(d-d0)>.01*max(max(d,d0),1e-7))return true;
    }
#endif
    return false;
}
// Classify each output pixel once. SamplePixel uses the source color sample-0 offset,
// preserving the original depth mapping even when source and target dimensions differ.
uint AtmosphereEdgePS(Screen i):SV_Target {
    return AtmosphereEdge(SamplePixel(i.position.xy,0))?1:0;
}
bool CachedAtmosphereEdge(float2 pixel) {
    [branch]if(lighting.z>.5)return atmosphereEdgeTexture.Load(int3(int2(pixel),0))!=0;
    return AtmosphereEdge(pixel); // allocation failure and single-sample depth fallback
}
float4 Atmosphere(float2 pixel,uint sample);
float4 AtmospherePS(Screen i,uint sample:SV_SampleIndex):SV_Target {
#if COLOR_MS
    [branch]if(!CachedAtmosphereEdge(SamplePixel(i.position.xy,0)))discard;
#endif
    return Atmosphere(SamplePixel(i.position.xy,sample),sample);
}
float4 AtmospherePixelPS(Screen i):SV_Target {
#if COLOR_MS
    float2 pixel=SamplePixel(i.position.xy,0);
    [branch]if(CachedAtmosphereEdge(pixel))discard;
    return Atmosphere(pixel,0);
#else
    discard;
    return 0;
#endif
}
)HLSL"
R"HLSL(
float4 Atmosphere(float2 pixel,uint sample) {
#if COLOR_MS
    float depth=ReadSampleDepth(pixel,sample); float3 source=NativePostLinear(sourceColor.Load(int2(pixel),sample).rgb);
#else
    float depth=ReadScene(pixel); float3 source=NativePostLinear(sourceColor.Load(int3(pixel,0)).rgb);
    // The sky's share of an edge pixel (EdgeSkyShare) is the map's own sky as CS2 drew it: swapped
    // for the Environment sky, as ShadeSample swaps the pixels that are all sky.
    [branch]if(CL_sky>0&&relightSunColor.w<.5&&!IsSky(depth)){
        float share=EdgeSkyShare(pixel);
        [branch]if(share>0){
            float found;float3 drawn=NativePostLinear(EdgeSkyColor(pixel,found,true));
            if(found>0)source=max(source+share*CL_sky*(SkyColor(normalize(World(pixel,camera.w>0?.00001:.99999)-camera.xyz),1,false)-drawn),0);
        }
    }
#endif
    // EFFECTS LIE OVER THE SNOW AND THE WET, NOT IN A HOLE IN THEM. The frame comes with the game's
    // flames, smoke, sparks and dust already in it, and they have no depth. Painted over, they were
    // hidden; kept as drawn wherever an effect was, each one stood in a patch of the bare, dry ground
    // it had been drawn on, like a cut-out. So the effect is taken off first (the frame is brought
    // back to what it was before the effect, by how much the effect changed the pixel's brightness),
    // the ground is shaded and coated as if nothing were in front of it, and the effect is laid back
    // over that: light added to it, smoke partly hiding it.
    float lift=1,glow=0;snowLaid=0;frontEffectLift=1;
    bool frontKnown=fogAmbient.w>.5&&(CL_snow>0||CL_sand>0||CL_wet>0||CL_puddles>0);
    [branch]if(frontKnown){
        float4 effect=fogFrontMask.Load(int3(int2(pixel),0));
        // Light of its own is strongly coloured, well brighter than the ground, AND bright in
        // absolute linear terms (flames, sparks, muzzle flash). Bullet impact dust or gun smoke in
        // dark shade can have a large relative lift over dim stone while remaining low luminance:
        // gating on linear brightness keeps dust puffs from routing through the flame branch and
        // cutting bare-ground holes through sand or snow in shade.
        float srcLuma=dot(max(source,0),float3(.2126,.7152,.0722));
        lift=clamp(effect.b,.02,64);
        glow=smoothstep(.45,.8,effect.a)*smoothstep(1.4,2.6,lift)*smoothstep(.25,.75,srcLuma);
        frontEffectLift=lift;
    }
#if COLOR_MS
    float3 shaded=ShadeSample(pixel,depth,source/lift,sample);
#else
    float3 shaded=ShadeSample(pixel,depth,source/lift,0);
#endif
    // Smoke keeps CS2's own volume, but it still has to follow Environment lighting: otherwise
    // flatten/sunlight relight the world and leave grenade clouds sitting on the baked probe.
    // Only the coverage of a placed smoke counts; glass and particles keep the surface shading.
    float smoke=SmokeCover(pixel);
    [branch] if(smoke>0){
        float3 surface=IsSky(depth)?World(pixel,camera.w>0?.00001:.99999):World(pixel,depth);
        float3 toward=surface-camera.xyz;float reach=length(toward),entry;
        smoke*=SmokeRayShare(toward/max(reach,.0001),IsSky(depth)?24000:reach,entry);
    }
    float3 veiled=source;
    if(smoke>0&&(CL_relight>0||CL_sunRelight>0)){
        float lum=max(dot(source,float3(.2126,.7152,.0722)),.02);
        float ao=saturate(pow(lum,.55));
        float3 albedo=source/pow(lum,.82);
        float3 hemi=CL_ambient*(.55+.45*saturate(SunDirection().z*.5+.5));
        float3 direct=SunColor()*(.35+.65*saturate(SunDirection().z))*CL_sunRelight;
        if(CL_relight>0) veiled=lerp(source,albedo*(hemi*ao+direct),saturate(CL_relight));
        else veiled=source+source*direct;
    }
    veiled=Grade(veiled,IsSky(depth)?8000:length(World(pixel,depth)-camera.xyz));
    float3 ground=shaded;
    // EFFECTS STAND IN FRONT OF THE SNOW AND THE WET. Those are painted on the ground in this pass,
    // over a frame that already has the game's flames, smoke puffs, sparks and debris in it, and
    // particles have no depth to say they are in front. So where an effect changed the frame (the
    // mask of what transparent effects did to it), the frame is kept as the game drew it: a fire
    // burns on the snow, a grenade's cloud and a muzzle's smoke are seen against it.
    [branch]if(abs(lift-1)>.004){
        const float3 kLuma=float3(.2126,.7152,.0722);
        float3 drawn=Grade(source,IsSky(depth)?8000:length(World(pixel,depth)-camera.xyz));
        // NOTHING OF THE BARE GROUND MAY COME BACK WITH THE EFFECT. Laid back as "the frame less the
        // ground", an effect carried whatever of the ground the guess of its thickness got wrong: the
        // cobbles showed in every puff of smoke, as if it wore a hole in the snow.
        // `coated`: how much brighter or darker this pass left the ground than the game drew it
        // (1 where it changed nothing: there the frame comes back exactly as it was).
        float coated=(dot(shaded,kLuma)+1e-4)/(dot(drawn,kLuma)/lift+1e-4);
        [flatten]if(lift>=1){
            // Smoke and dust: a grey of its own over the ground as it now is, never brighter than
            // snow in the same light. A pale puff over dark ground is thin, and on the snow lying
            // there it is a faint grey; one as bright as the snow hides it.
            float most=max(max(lift,2.5),.6*coated);
            float thick=saturate((lift-1)/(most-1));
            // Its brightness is set against the ground as it now is: a little under the snow's, a
            // little over bare or wet ground's. Only where this pass changed nothing is it worked
            // back from the frame. (Worked back everywhere, it came out as bright as the game had
            // drawn it, with the ground's pattern in it: under dim light every puff was a lit window
            // onto the cobbles.)
            float changed=max(snowLaid,saturate(abs(log2(coated))*2));
            float3 grey=shaded*(1+thick*(lerp(most/coated,lerp(1.3,.7,snowLaid),changed)-1));
            // Coloured light (a flame, a flash, sparks): added to the ground as it now is; only a
            // core many times brighter than the ground hides it.
            float core=saturate((lift-4)/(lift+2));
            float3 lit=shaded*(1-core)+max(drawn-drawn/lift*(1-core),0);
            shaded=lerp(grey,lit,glow);
        }else{
            // Darker than before: a bullet hole, a scorch mark, blood. They lie on the ground, so
            // under snow they are covered; on bare or wet ground they stay.
            shaded=lerp(shaded*lift,shaded,snowLaid);
        }
    }
    float3 color=lerp(shaded,veiled,smoke);
    // WHAT CS2'S SMOKE LETS THROUGH IS KNOWN, so a smoke lies OVER the snow instead of cutting it out.
    // CS2 blends smoke and its other order-independent transparents onto the ground as
    //   frame = ground * T + their own light,  T = exp(-b0)
    // (b0 is the absorbance the game itself worked out for each pixel and hands us; SmokeCover is 1-T).
    // Their own light is therefore the frame less the ground as the game drew it (the frame brought back
    // by `lift`) times T, and the coated ground goes in its place: ground' * T + that light. This is
    // the ordinary "over" composite with the game's own transmittance (Porter and Duff 1984; the OIT
    // resolve of Muenstermann et al. 2018, "Moment-Based Order-Independent Transparency"), so nothing
    // has to be guessed. The guesses above (a smoke's grey from how much brighter it made the pixel,
    // and a placed smoke shown as the game drew it) let the bare ground through: snow vanished under a
    // smoke and the cobbles showed in it. Where nothing transparent covers a pixel this changes nothing.
    // Only the share of that absorbance that belongs to a placed smoke (`smoke`: SmokeCover times
    // SmokeRayShare) is taken. The same pass draws the paint on the floor (a bombsite's marking),
    // glass and leaf cards; paint lies ON the ground and goes under the snow, and taken as a veil it
    // showed through the snow in a pale patch.
    float transmit=frontKnown?1-smoke:1;
    [branch]if(transmit<.995){
        float3 drawn=Grade(source,IsSky(depth)?8000:length(World(pixel,depth)-camera.xyz));
        float3 own=max(drawn-drawn/lift*transmit,0);
        // A placed smoke's own light follows the Environment's light, as `veiled` relights it.
        own=lerp(own,own*(veiled+1e-4)/(drawn+1e-4),smoke);
        color=ground*transmit+own;
    }
    color=lerp(color,smoke.xxx,step(.5,smokeInfo.w));
    return float4(NativePostColor(color),1);
}
)HLSL"
R"HLSL(
float Random01(uint n) { n^=n>>16;n*=0x7feb352dU;n^=n>>15;n*=0x846ca68bU;n^=n>>16;return (n&0xffffff)/16777216.0; }
float Wrap(float x,float size) { return x-floor(x/size)*size; }
// Mirror of RainDropVariation in EnvironmentWeatherMath.h: the size setting is the LARGEST drop;
// most are smaller, down to a quarter of it.
float RainDropVariation(float u) { return .25+.75*pow(u,1.6); }
struct ParticleOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float alpha:TEXCOORD1; float3 world:TEXCOORD2; float4 rnd:TEXCOORD3; float3 light:TEXCOORD4; };
// The shelter test at one bilinear tap. Particles ask it once each in the vertex shader and the veil
// asks it per ray step; the 12-tap SkyOpen is for surfaces, where a roof edge is
// seen up close.
float PrecipitationOpenSky(float3 world){
    if(shelterInfo.x<=0)return 1;
    float4 clip=mul(shelterMatrix,float4(world+float3(0,0,2),1));
    float3 ndc=clip.xyz/clip.w;float2 uv=ndc.xy*float2(.5,-.5)+.5;
    if(any(uv<0)||any(uv>1)||ndc.z<0)return 1;
    int resolution=(int)shelterInfo.y;
    return ShelterCompare(uv*resolution-.5,ndc.z,resolution);
}
// RainLighting at a fraction of the cost: the full-map sun shadow at one bilinear tap instead of the
// filtered cascades. Evaluated once per particle (vertex shader) and once per veil pixel; a drop is
// too small and too fast for a soft shadow edge across it to show.
float3 PrecipitationLighting(float3 p,float2 pixel){
    float3 scene=RainSceneRadiance(pixel,0);
    // In front of the sky the frame still holds the game's own clear blue: our sky and its clouds
    // are drawn later. A drop there takes our sky instead, kept in the frame's brightness scale, so
    // rain under storm clouds is grey and dim rather than sky blue (the owner, 2026-11-23).
    [branch]if(CL_sky>0&&IsSky(ReadCombinedSample(pixel,0))){
        float3 drawn=NativePostLinear(ReadSource(pixel,0));
        float3 ours=SkyColor(normalize(World(pixel,camera.w>0?.00001:.99999)-camera.xyz),1,true);
        float3 lum=float3(.2126,.7152,.0722);
        scene=lerp(scene,ours*(dot(scene,lum)/max(dot(drawn,lum),1e-4)),saturate(CL_sky));
    }
    float3 illumination=scene*CL_ambient+SunColor()*WorldSunShadow(p)*CL_sunRelight*.3;
    float3 view=normalize(camera.xyz-p);
    [loop]for(int l=0;l<(int)lighting.x;++l){
        float3 dir;float atten=StudioAttenuation(l,0,p,dir);
        if(atten>0)atten*=WorldLightShadowSample(l,p,0);
        illumination+=lightColorIntensity[l].rgb*lightColorIntensity[l].w*atten*(.25+.75*pow(saturate(dot(-view,dir)),8))*CL_particleLight;
    }
    return illumination;
}
// WIND GUSTS (Gusts, CL_gusts): the air is read through WindAt and shown with GustFlow
// (EnvironmentClimateShaders.h). Here the weather reads it: where the air moves faster than the
// steady wind the rain leans over and snow is carried sideways and drawn out into streaks. Without
// the fluid (no map data yet) there are no gusts.
// A place as the strands see it: along and across the wind, carried downwind.
float2 GustPlace(float3 p,float pace){
    float2 d=GustAlong();
    return float2(dot(p.xy,d)-climateClock.x*(220+1.5*length(weatherVelocity.xy))*pace,dot(p.xy,float2(-d.y,d.x)));
}
float GustFront(float3 p){return GustAmount(WindAt(p));}
// CURL NOISE ON THE WEATHER. Rain, snow and ash are pushed about by a curl-noise force (strong for
// rain and snow, a fifth of it for ash, as for blown sand) up to about 394 units/s, so the weather
// turns in eddies instead of falling in ruled lines. A particle here is a function of its cell and the
// clock, with nothing to integrate, so the eddy is an orbit about its place: two sizes of it, the
// phase from a smooth noise over where the particle rests so neighbours turn together and the eddies
// read as swirling sheets, tilted out of level by the same noise. `strength` scales the orbit (1 =
// snow's); `velocity` is how fast it moves on the orbit, for the streak.
float3 WeatherSwirl(float3 rest,float strength,out float3 velocity){
    velocity=0;
    if(strength<=0)return 0;
    float t=climateClock.x;
    float3 d=0;
    [unroll]for(int k=0;k<2;++k){
        float size=k==0?380:130,radius=(k==0?62:20)*strength,spin=k==0?2.5:4.9;
        float3 q=rest/size+k*7.31;
        float phase=Noise3(q)*12.566,tilt=Noise3(q*.7+3.1)-.5;
        float a=t*spin*(Noise3(q+11.7)>.5?1:-1)+phase;
        float3 u=normalize(float3(1,0,tilt*1.6)),v=normalize(float3(0,1,-tilt*1.2));
        d+=(u*cos(a)+v*sin(a))*radius;
        velocity+=(-u*sin(a)+v*cos(a))*radius*spin*(Noise3(q+11.7)>.5?1:-1);
    }
    return d;
}
// The share of the swirl each weather takes: snow all of it, rain nine tenths, ash (blown like sand,
// gently) two fifths, all scaled by Turbulence.
float WeatherSwirlStrength(){return CL_turbulence*(weatherParams.z==3?.4:weatherParams.z==2?1:.85);}
// The strands the wind tears what it carries into, 0..1: long along the wind, thin across it and in
// height, and running ahead of the front they are in.
// `coarse` widens them (the air between the camera and a far wall is sampled a few times a ray, and
// strands finer than the samples would be drawn as grain).
float GustStrands(float3 p,float coarse=1){
    float2 q=GustPlace(p,1.6);
    return Noise3(float3(q.x/(520*coarse),q.y/(46*coarse),p.z/(38*coarse)+climateClock.x*.35))*.6
          +Noise3(float3(q.x/(210*coarse),q.y/(19*coarse),p.z/(16*coarse)-climateClock.x*.5))*.4;
}
)HLSL"
R"HLSL(
// Where rain, snow and ash may exist: inside the map's precipitation box when the map is prepared
// (none beyond its edges, below its floor or high above its roofs), and inside the fixed-height layer
// around the weather centre when it does not follow the camera.
float PrecipitationRegion(float3 p){
    float mask=1;
    if(weatherRegionLow.w>.5){
        float2 inside=min(p.xy-weatherRegionLow.xy,weatherRegionHigh.xy-p.xy);
        mask=saturate(min(inside.x,inside.y)/192)*saturate((p.z-weatherRegionLow.z)/64)*saturate((weatherRegionHigh.z-p.z)/128);
    }
    if(weatherVelocity.w<.5)mask*=1-smoothstep(weatherParams.x*.4,weatherParams.x*.5,abs(p.z-weatherCenterRadius.z));
    return mask;
}
// 0 inside the particle window, 1 outside; the particles take 1 - this and the veil takes this, so
// together they are one density. The window is an ellipsoid half as tall as it is wide.
float PrecipitationVeilWeight(float3 p){
    float3 d=(p-weatherCenterRadius.xyz)*float3(1,1,weatherReach.x/max(weatherReach.y,1));
    return smoothstep(.8,.97,length(d)/max(weatherReach.x,1));
}
)HLSL"
R"HLSL(
// WHAT THE FLOOR VOLUMES HIDE OF A PARTICLE, 0..1 of it showing. Rain, snow, their veil and the
// splashes are drawn after the fog composite (they have no depth for it to measure to), so the
// ground fog and the gusts' drift in front of them are taken here, from the eye to `world`. Drawn over
// the composite without it, a flake or splash behind a dense bank was painted on top of the bank.
// The drift is stepped as GustVolumeMarch steps it, without its light; `maxSteps` bounds the walk.
// One return: see GustVolumeAt.
float FloorVolumeThrough(float3 world,int maxSteps){
    float3 ray=world-camera.xyz;float reach=length(ray);
    float tau=0;
    [branch]if(reach>1&&(CL_groundFog>0||CL_gusts>0)){
        ray/=reach;
        tau=GroundFogDepthAlong(ray,reach);
        [branch]if(CL_gusts>0&&GustLookType()>0&&windFluidWindow.z>0){
            GustLook g=GustLookFor(GustLookType());
            float show=GustShown(),range=min(reach,3000),layer=g.top+g.lift,climb=max(abs(ray.z),.01),t=0;
            [loop]for(int i=0;i<maxSteps&&t<range;++i){
                float3 p=camera.xyz+ray*t;
                float floorHere=GustFloor(p);
                float over=p.z-floorHere,above=over-layer;
                float dt=clamp(4+t*.045,4,180);
                if(over<layer*2)dt=min(dt,max((max(above,0)+layer/6)/climb,2));
                if(above>0)dt=max(dt,min(above/max(-ray.z,.04)*.6,300));
                if(over<-40&&ray.z<0)break;
                dt=min(dt,range-t);
                float3 q=camera.xyz+ray*(t+dt*.5);
                t+=dt;
                float region=PrecipitationRegion(q);
                if(region<=0)continue;
                float d;float sigma=GustVolumeAt(q,GustFloor(q),g,true,d);
                if(sigma<=0)continue;
                tau+=sigma*region*max(PrecipitationOpenSky(q),CoverReach(q).x)*show*(1-smoothstep(2400,3000,t))*dt;
            }
        }
    }
    return exp(-tau);
}
ParticleOutput WeatherVS(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    uint n=instance*3+(uint)weatherParams.w*7919;
    float radius=weatherCenterRadius.w,height=weatherParams.x;
    float3 p;float alpha=1;
    float3 velocity=weatherVelocity.xyz;
    float3 fallAxis=normalize(-velocity+float3(0,0,.0001)); // streak axis
    const bool flake=weatherParams.z==2||weatherParams.z==3;
    float gust=0;
    if(weatherParams.z==1||flake){
        // One lattice, one density (PrecipitationLattice in EnvironmentWeatherMath.h), identical for
        // rain, snow and ash. A particle's position is a function of its cell and absolute time only;
        // the camera chooses which cells are drawn, never where a particle is, and the cell edge comes
        // from the weather radius, so it does not change as the camera moves either.
        float cell=weatherLattice.x;
        uint across=(uint)weatherLattice.y,layers=(uint)weatherLattice.z;
        float3 motion=velocity*climateClock.x;
        int3 base=(int3)floor((weatherCenterRadius.xyz-motion)/cell)-int3((int)(across/2),(int)(across/2),(int)(layers/2));
        int3 id=base+int3((int)(instance%across),(int)((instance/across)%across),(int)(instance/(across*across)));
        n=(uint)id.x*73856093U^(uint)id.y*19349663U^(uint)id.z*83492791U^(uint)weatherParams.w*7919U;
        float3 jitter=float3(Random01(n),Random01(n+1),Random01(n+2));
        p=(float3(id)+jitter)*cell+motion;
        // The swirl (WeatherSwirl), round the place the particle would be: rain, snow and ash
        // turn in eddies. From its cell and absolute time only, so it stays camera-independent.
        {
            float3 swirl;
            p+=WeatherSwirl((float3(id)+jitter)*cell,WeatherSwirlStrength(),swirl);
            velocity+=swirl;
            fallAxis=normalize(-velocity+float3(0,0,.0001));
        }
        // Every cell holds a particle; count sets the cell size. The window edge fades out exactly as
        // WeatherVeilPS fades the same density in, so distance never changes how thick it is. A
        // particle under a roof is dropped whole, once, here rather than tested per pixel.
        alpha=(1-PrecipitationVeilWeight(p))*PrecipitationRegion(p);
        // Each particle is tested against the sky map once. With the wind leaning, one in the open is
        // dropped when the line it came down ran under a roof (the sheltered side of a wall).
        // UNDER COVER it drifts in through the opening and thins out the further in it is: the cover
        // volume's open air, which halves every 48 units in and never crosses a wall, a floor or a
        // roof, so nothing comes down through a catwalk or into a room with no way in. Each particle
        // has its own depth, so the edge is a thinning, not a line across an archway. Snow and ash
        // drift in about three paces; rain only a pace in still air and further as the wind leans it,
        // and an opening that faces the wind lets it straight in. (Before 2026-11-18 only the
        // windward line counted, and the weather stopped dead at every roof's edge.)
        if(alpha>0){
            float open=PrecipitationOpenSky(p);
            float blown=smoothstep(.1,.55,saturate(coverWind.w));
            [branch]if(shelterInfo.x>0&&(open<.999||blown>0)){
                float came=blown>0?SkyTap(p+coverWind.xyz*72):open;
                if(open>.5)open=lerp(open,min(open,came),blown);
                else{
                    float reach=CoverReach(p).x;
                    float depth=weatherParams.z==1?lerp(.2,.85,blown):1;
                    float t=lerp(.98,.06,depth*Random01(n+40));
                    open=max(smoothstep(t-.04,t+.04,reach),came*step(.3,reach)*blown);
                }
            }
            alpha*=smoothstep(.3,.7,open);
        }
        // A gust front passing: the particle is carried downwind and the strands it is torn into
        // are thicker and thinner; rain leans over with it (snow is drawn out below).
        [branch]if(CL_gusts>0&&alpha>0){
            float4 air=WindAt(p);
            float open=1-saturate(air.w*2);
            gust=CL_gusts*saturate(air.z)*open;
            // How much faster than the steady wind the air here moves: that is what carries it.
            float2 excess=(air.xy-weatherVelocity.xy)*open*CL_gusts;
            // Still air in the lee of a house does not stand the rain up straight: what falls there
            // came down through the wind above the roofs.
            float2 along=GustAlong();
            excess-=along*min(dot(excess,along)+.25*length(weatherVelocity.xy),0);
            float strand=GustStrands(p);
            p.xy+=excess*(flake?.8:.35)*(.6+.8*strand);
            p.z+=gust*(flake?26:0)*(strand-.5);
            alpha*=lerp(1,.4+(flake?5.:4.)*strand,saturate(gust));
            velocity.xy+=excess*(flake?1.3:.9);
            fallAxis=normalize(-velocity+float3(0,0,.0001));
        }
    }else{
        float3 low=weatherCenterRadius.xyz-float3(radius,radius,height*.5);
        float3 span=float3(radius*2,radius*2,height);
        float3 raw=float3(Random01(n),Random01(n+1),Random01(n+2))*span+weatherOffset.xyz-low;
        p=low+float3(Wrap(raw.x,span.x),Wrap(raw.y,span.y),Wrap(raw.z,span.z));
        p+=float3(sin(climateClock.x+Random01(n)*6.28),cos(climateClock.x*.7+Random01(n+1)*6.28),0)*5*CL_turbulence;
        alpha=saturate((radius-max(abs(p.x-weatherCenterRadius.x),abs(p.y-weatherCenterRadius.y)))/max(64,radius*.15));
    }
    float2 corners[6]={float2(-1,-1),float2(-1,1),float2(1,1),float2(-1,-1),float2(1,1),float2(1,-1)};
    float2 uv=corners[vertex];
    float variation=lerp(.65,1.4,Random01(n+7));
    float4 rnd=float4(Random01(n+11),Random01(n+12),Random01(n+13),Random01(n+14));
    // Mostly small drops, a few up to the full size setting (RainDropVariation).
    if(weatherParams.z==1)variation=RainDropVariation(Random01(n+7));
    // Snow is many small flakes and a few larger ones; ash is a spread of small flecks and some
    // larger torn pieces.
    if(weatherParams.z==2)variation=lerp(.3,1.1,pow(Random01(n+7),2.2));
    if(weatherParams.z==3)variation=lerp(.4,2.3,pow(Random01(n+7),1.8));
    float3 right=cameraRight.xyz*weatherParams.y*variation,up=cameraUp.xyz*weatherParams.y*variation;
    if(weatherParams.z==1){
        float3 streak=fallAxis;
        float3 eye=normalize(camera.xyz-p),side=cross(streak,eye);
        float projected=length(side);side=projected>.01?side/projected:cameraRight.xyz;
        float pixel=2*length(camera.xyz-p)/max(1,dimensions.y*length(viewProjection[1].xyz));
        float physical=weatherParams.y*.18*variation; // a real drop is a few millimetres across
        // Most drops are thinner than a pixel. A fixed raster floor drew every one of them the same
        // width, so the floor grows with the drop: big drops read as thicker streaks. Opacity is still
        // scaled by physical/width, so a drop adds the coverage its real size gives it.
        // A streak is never drawn wider than two pixels: a drop right at the lens is a thin line like
        // the rest, not a white wedge across the frame.
        float width=min(max(physical,pixel*(.35+.65*variation)),pixel*2);
        alpha*=min(1.,physical/width); // conserve opacity as the raster footprint widens
        right=side*width;
        // Big drops fall faster (terminal velocity rises with size and levels off), so their
        // exposure-time streak is longer: about 3.5x between the smallest and the largest.
        float fall=(1-exp(-1.1*variation))/(1-exp(-1.1));
        float streakLength=max(physical*2,CL_streak*weatherParams.y*fall);
        up=streak*streakLength;
        // MOTION BLUR. A streak is one drop smeared over the exposure, so its opacity is the drop's
        // share of that length: long, fast streaks are faint and soft, as a camera records rain.
        // Drawing every streak as opaque as a still drop is what read as fibreglass against the sky.
        alpha*=min(1.,RAIN_MOTION_BLUR*physical/streakLength);
        up+=cameraUp.xyz*width*(1-smoothstep(.0,.12,projected));
        // Rain fades IN with distance (a third at the lens, all of it 787 units out): the streaks near
        // the eye are faint and the storm thickens away from it. (Fading to nothing at the lens hid
        // the rain in daylight.)
        alpha*=lerp(.35,1,saturate((length(camera.xyz-p)-24)/760));
    }
    // ASH IS THREE THINGS, as a sand storm and a fire are: the torn flakes, fine dust blown through
    // them (dim streaking motes, 4 in 10 particles) and a few embers still glowing (1 in 40). Passed
    // to the pixel stage in rnd.x's whole part.
    float ashKind=0;
    if(weatherParams.z==3){
        if(sandInfo.y>.5){
            // Blown sand weather: fine wind-streaked sand grains and subtle dust motes
            // (no burnt-paper flakes, embers or oversized round blobs).
            ashKind=1;
            variation*=lerp(.22,.52,Random01(n+31));
        }else{
            float pick=Random01(n+31);
            ashKind=pick<.025?2:pick<.425?1:0;
            if(ashKind>0)variation*=ashKind==2?.32:.4;
        }
    }
    // ASH DUST AND EMBERS ARE DRAWN OUT ALONG THEIR FLIGHT: they face their velocity and grow with
    // speed (2 times wider and 10 times longer at 787 units/s). Snow flakes stay round: drawn out,
    // they read as smears rather than flakes (the owner, 2026-11-18). The eddies still carry them.
    if(weatherParams.z==2||ashKind>0){
        float speed=length(velocity);
        float grow=weatherParams.z==2?0:saturate(speed/787);
        float3 streak=speed>1?velocity/speed:float3(0,0,-1);
        float dist=length(camera.xyz-p);
        float pixel=2*dist/max(1,dimensions.y*length(viewProjection[1].xyz));
        float radius=weatherParams.y*variation;
        float widthR=max(radius,pixel*.8);
        alpha*=saturate(radius*radius/(widthR*widthR));
        alpha*=smoothstep(10,48,dist); // nothing fills the lens
        float3 eye=normalize(camera.xyz-p),side=cross(streak,eye);
        float projected=length(side);side=projected>.01?side/projected:cameraRight.xyz;
        float w=widthR*(1+(sandInfo.y>.5?.25:1)*grow),l=widthR*(1+(sandInfo.y>.5?11:9)*grow);
        right=side*w;
        up=streak*l+cameraUp.xyz*w*(1-smoothstep(.0,.12,projected));
        // A flake smeared over its streak spreads the same snow over more of the frame.
        alpha*=lerp(1,.55,grow);
    }else if(weatherParams.z==3){
        // Ash flakes tumble: each has its own spin, and they are torn, elongated fragments. The
        // footprint never drops under a pixel (it would shimmer); when it is widened the opacity is
        // reduced in step so the coverage a distant flake adds to the frame is unchanged.
        float spin=(rnd.y-.5)*3;
        float angle=rnd.x*6.2831853+climateClock.x*spin;
        float aspect=1;
        if(weatherParams.z==3){
            // Ash is flat, torn paper: it flips end over end as it falls, so its outline narrows to an
            // edge and opens again. That flicker, not the shape, is what reads as ash.
            float flip=cos(climateClock.x*lerp(1.5,4.,rnd.w)+rnd.z*6.2831853);
            aspect=lerp(1.1,1.8,rnd.z)*(.12+.88*abs(flip));
        }
        float dist=length(camera.xyz-p);
        float pixel=2*dist/max(1,dimensions.y*length(viewProjection[1].xyz));
        float radius=weatherParams.y*variation;
        float widthR=max(radius,pixel*.8);
        alpha*=saturate(radius*radius/(widthR*widthR));
        alpha*=smoothstep(10,48,dist); // nothing fills the lens
        float sn=sin(angle),cs=cos(angle);
        // An edge-on flake keeps a pixel of footprint and gives up opacity instead (no shimmer).
        float wide=max(widthR*aspect,pixel*.8);
        alpha*=saturate(widthR*aspect/wide);
        right=(cameraRight.xyz*cs+cameraUp.xyz*sn)*wide;
        up=(-cameraRight.xyz*sn+cameraUp.xyz*cs)*widthR;
    }
    rnd.x+=ashKind*2;
    ParticleOutput o;o.world=p+right*uv.x+up*uv.y;o.position=mul(viewProjection,float4(o.world,1));o.uv=uv;o.rnd=rnd;
    o.alpha=alpha*CL_particleOpacity;
    o.light=1;
    if(alpha<=0){o.position=float4(2,2,2,1);return o;}
    // The same for every corner: measured to the particle's centre.
    o.alpha*=FloorVolumeThrough(p,80);
    // Rain, snow and ash take the map's light at the particle centre, once per particle.
    if(weatherParams.z>=1&&weatherParams.z<=3){
        float4 centre=mul(viewProjection,float4(p,1));
        float2 pixel=(centre.xy/max(centre.w,1e-4)*float2(.5,-.5)+.5)*dimensions.xy;
        o.light=PrecipitationLighting(p,pixel);
        // Snow takes the scene's brightness and hardly any of its colour: against the sky every flake
        // was sky blue.
        if(weatherParams.z==2)o.light=lerp(dot(o.light,float3(.2126,.7152,.0722)).xxx,o.light,.15);
    }
    return o;
}
)HLSL"
R"HLSL(
float4 WeatherPS(ParticleOutput i,uint sample:SV_SampleIndex):SV_Target {
    if(Hidden(i.position.z,ReadCombinedSample(i.position.xy,sample))) discard;
    float smoke=SmokeHides(i.position.xy,i.world);
    if(smoke>=.999) discard;
    float a=weatherParams.z==1?pow(saturate(1-abs(i.uv.x)),2)*saturate(1-abs(i.uv.y)):saturate(1-dot(i.uv,i.uv));
    float3 tone=float3(1,1,1);
    if(weatherParams.z==2) {
        // SNOW: a small round soft flake with a brighter middle. Each has its own softness, and the
        // soft ones read as out of focus. (Lumpy outlines and crystal glints drew clover and star
        // shapes at any size a flake is seen at; an even soft dot read as a blurred blob up close.)
        float r=length(i.uv);
        float soft=lerp(.5,1.,i.rnd.w);
        float body=smoothstep(1,1-soft,r);
        float core=exp(-r*r*6);
        a=saturate(body*.4+core*.75);
        a*=.88+.12*sin(climateClock.x*(2+i.rnd.y*5)+i.rnd.z*40);
        tone=lerp(float3(.96,.98,1.01),float3(1.02,1.02,1),i.rnd.z);
    }
    float ashKind=weatherParams.z==3?floor(i.rnd.x*.5):0;
    if(ashKind>0)i.rnd.x-=ashKind*2;
    bool ember=ashKind==2;
    if(ashKind==1){
        // Ash dust or blown sand motes: subtle wind-streaked grains.
        float r=length(i.uv);
        a=pow(saturate(1-r),sandInfo.y>.5?1.75:1.6)*(sandInfo.y>.5?.58:.75);
        float lum=lerp(.68,.98,i.rnd.z);
        float3 sc=float3(CL_sandColorR,CL_sandColorG,CL_sandColorB);
        float3 sandTint=lerp(float3(1.02,.98,.9),lerp(float3(1,1,1),clamp(sc/max(dot(sc,float3(.2126,.7152,.0722)),.05),.55,1.35),.45),saturate(CL_sandHarmonize));
        tone=sandInfo.y>.5?sandTint*lum:float3(lum,lum*.98,lum*.95);
    }else if(ember){
        // An ember: a hot point that flickers, drawn out along its flight like a spark.
        float r=length(i.uv);
        a=exp(-r*r*3.5);
        float flicker=.65+.35*sin(climateClock.x*(9+i.rnd.y*14)+i.rnd.z*40)*sin(climateClock.x*(5.3+i.rnd.w*7)+i.rnd.x*17);
        tone=float3(2.6,.85,.2)*(1.4*flicker)*lerp(.6,1.4,i.rnd.w);
    }else if(weatherParams.z==3) {
        // ASH: flat flakes of burnt paper -- pale grey, thin and translucent, with a crisp torn outline
        // and a darker charred rim. The earlier soft, dense, dark round blobs lit by nothing but their
        // own colour read as dirt clumps. Most flakes are light; one in five is a charcoal fleck.
        // (The glowing ones are embers of their own, above.)
        float2 q=i.uv;float r=length(q),ang=atan2(q.y,q.x);
        float edge=.78+.12*sin(ang*3+i.rnd.x*6.28)+.08*sin(ang*5+i.rnd.y*6.28)+.05*sin(ang*11+i.rnd.z*6.28);
        float d=r/max(edge,.3);
        float body=smoothstep(1,.82,d);
        float rim=smoothstep(.45,.95,d);
        float fibre=.85+.15*sin(q.x*13.+i.rnd.x*20)*sin(q.y*9.+i.rnd.y*20);
        bool charcoal=i.rnd.w<.2;
        a=body*(charcoal?.85:.62)*fibre;
        float lum=charcoal?lerp(.25,.4,i.rnd.z):lerp(.85,1.15,i.rnd.z);
        lum*=lerp(1,.55,rim); // the burnt edge is darker than the ash face
        tone=float3(lum,lum,lum*1.03);
    }
    if(weatherParams.z==6)a=pow(a,2)*.3;
    // GROUND CONTACT. A flake or a drop ends when it reaches a surface -- it does not sink through
    // the floor and keep going. This fades a particle out over the last stretch before whatever is
    // behind it, so precipitation dies AT the ground instead of intersecting it as a hard sprite.
    //
    // The distance is generous (and generous for rain, which arrives fast and should not pop): it
    // reads as the drop breaking up on impact rather than as a sprite being clipped. CL_softness
    // still scales it, so the old control means the same thing, just over a longer run-out.
    float d=ReadCombinedSample(i.position.xy,sample);
    if(!IsSky(d)) {
        float contact=max(CL_softness,1)*(weatherParams.z==1?4:2.5);
        a*=saturate((length(World(i.position.xy,d)-camera.xyz)-length(i.world-camera.xyz))/contact);
    }
    float3 energy=0;
    if(weatherParams.z>=4)energy=FillLights(i.world,normalize(camera.xyz-i.world));
    float3 color=weatherColor.rgb*tone*(1+energy*CL_particleLight);
    // Ash takes the map's light like rain does: pale flakes in sun, dim ones in shade, and the
    // map's colour cast, instead of a fixed grey that floats over every scene at the same value.
    // Both were lit per particle in WeatherVS.
    if(weatherParams.z==3)color=weatherColor.rgb*tone*i.light*2.4*tintExposure.rgb*tintExposure.w;
    // An ember gives its own light: not the ash colour, and not dimmed by the shade it flies through.
    if(ember)color=tone*tintExposure.rgb*tintExposure.w;
    // So does snow (issue 135). In its own colour, unlit and unexposed, it glowed white under a night
    // sky and brightened the whole frame. Lit daylight saturates at that same white, so day snow is
    // unchanged; under a dark sky it dims with the scene, and lamps still light it.
    if(weatherParams.z==2)color=weatherColor.rgb*tone*min(1,i.light*SNOW_BRIGHTNESS)*tintExposure.rgb*tintExposure.w;
    if(weatherParams.z==1){
        color=weatherColor.rgb*i.light*RAIN_BRIGHTNESS*tintExposure.rgb*tintExposure.w;
        a=saturate(a*RAIN_SHAPE_GAIN);
        float distance=length(i.world-camera.xyz);
        float optical=max(0,distance-CL_fogStart)*CL_fogDensity*.001
            *exp(-max(0,i.world.z-CL_fogBase)/CL_fogHeight);
        a*=exp(-optical);
        color=lerp(color,fogColorDensity.rgb,1-exp(-optical));
    }
    if(weatherParams.z==4)color*=1.7;
    // Snow and ash sink into the height fog with distance, as rain does above: the weather is drawn
    // after the fog volumes, so the fog is not laid over them. (Embers glow through it.)
    [branch]if((weatherParams.z==2||weatherParams.z==3)&&!ember){
        float optical=max(0,length(i.world-camera.xyz)-CL_fogStart)*CL_fogDensity*.001*exp(-max(0,i.world.z-CL_fogBase)/CL_fogHeight);
        color=lerp(color,fogColorDensity.rgb,1-exp(-optical));
    }
    float opacity=a*i.alpha*(1-smoke);
    if(weatherParams.z==1)opacity=min(opacity,RAIN_MAX_OPACITY); // no solid white streaks at the lens
    return float4(color,opacity);
}
// PRECIPITATION BEYOND THE PARTICLES. The particle lattice is drawn only in a window around the
// camera; past it the same density is integrated analytically along each pixel's view ray. Only
// stretches of the ray inside the map's precipitation region and open to the sky count, so a view
// out of a doorway sees the same snowfall outside as a camera standing in it, and none indoors. The
// optical density is the particles' own (occupied cells per volume x the coverage each adds), and
// world-anchored noise that falls with the particles keeps it from reading as flat fog.
)HLSL"
R"HLSL(
float4 WeatherVeilPS(Screen i):SV_Target {
    if(weatherLattice.w<=0)return 0;
    float2 pixel=i.position.xy;
    float depth=ReadCombinedSample(pixel,0);
    float3 dir=normalize(World(pixel,.5)-camera.xyz);
    // An edge pixel against the sky (EdgeSkyShare): the march runs on to the sky, and the stretch up
    // to the thing in front is kept apart, so the sky's share of the pixel gets the sky's veil.
    float skyShare=IsSky(depth)?0:EdgeSkyShare(pixel);
    float tSurface=IsSky(depth)?1e9:FogDistance(pixel,depth);
    float t1=min(skyShare>0?1e9:tSurface,weatherReach.z),t0=0;
    if(weatherRegionLow.w>.5){
        // Clip the ray to the precipitation box.
        float3 inv=1/(abs(dir)>1e-6?dir:1e-6);
        float3 a=(weatherRegionLow.xyz-camera.xyz)*inv,b=(weatherRegionHigh.xyz-camera.xyz)*inv;
        float3 lo=min(a,b),hi=max(a,b);
        t0=max(max(lo.x,lo.y),max(lo.z,0));t1=min(t1,min(hi.x,min(hi.y,hi.z)));
    }
    // Inside the particle window the particles carry the density; skip to where the veil starts.
    if(weatherVelocity.w>.5)t0=max(t0,.8*weatherReach.x/length(dir*float3(1,1,weatherReach.x/max(weatherReach.y,1))));
    if(t1<=t0)return 0;
    float density=weatherLattice.w;
    float3 velocity=weatherVelocity.xyz;
    bool rain=weatherParams.z==1;
    // A streak seen end-on covers less of the frame than one seen broadside.
    if(rain)density*=max(.15,sqrt(saturate(1-pow(dot(dir,normalize(-velocity+float3(0,0,.0001))),2))));
    float3 scale=rain?float3(1./240,1./240,1./900):float3(1./260,1./260,1./260);
    // Few steps: the integrand is smooth (a density times open-sky and a slow noise), and the
    // per-pixel jitter turns the remaining sampling error into fine grain rather than bands.
    const int steps=14;
    float jitter=InterleavedGradient(pixel); // an even dither: pure chance per pixel showed as speckle wherever a gust thickened the veil
    float tau=0,weighted=0,tauNear=0;
    [loop]for(int k=0;k<steps&&t1>t0;++k){
        float b0=pow(k/(float)steps,1.6),b1=pow((k+1)/(float)steps,1.6);
        float t=t0+(t1-t0)*lerp(b0,b1,jitter),dt=(t1-t0)*(b1-b0);
        float3 p=camera.xyz+dir*t;
        float w=PrecipitationVeilWeight(p)*PrecipitationRegion(p);
        if(w<=0)continue;
        w*=PrecipitationOpenSky(p);
        if(w<=0)continue;
        float clump=.55+.9*Noise3((p-velocity*climateClock.x)*scale);
        // Far off, a gust front is a curtain of thicker weather crossing the view.
        [branch]if(CL_gusts>0)clump*=lerp(1,.6+1.2*GustFront(p),saturate(CL_gusts));
        float d=density*dt*w*clump;
        tau+=d;weighted+=d*t;
        if(t<tSurface)tauNear+=d;
    }
    tau*=CL_particleOpacity;tauNear*=CL_particleOpacity;weighted*=CL_particleOpacity;
    float coverage=skyShare>0?1-((1-skyShare)*exp(-tauNear)+skyShare*exp(-tau)):1-exp(-tau);
    float smokeCover=SmokeCover(pixel);
    [branch] if(smokeCover>0&&coverage>0){float entry;coverage*=1-smokeCover*SmokeRayShare(dir,t1,entry);}
    if(coverage<=0)return 0;
    float3 p=camera.xyz+dir*(weighted/max(tau,1e-9));
    coverage*=FloorVolumeThrough(p,24); // the ground fog and drift in front of it (see FloorVolumeThrough)
    if(coverage<=0)return 0;
    // Falling rain and snow take the light of the scene behind them, read at the pixel and at four
    // points a little way off it. That suits thin streaks. (What a gust blows along the ground is a
    // layer of the fog volume, GustVolumeMarch.)
    float3 light=PrecipitationLighting(p,pixel);
    if(weatherParams.z==2)light=lerp(dot(light,float3(.2126,.7152,.0722)).xxx,light,.15);
    float3 color=weatherColor.rgb;
    if(rain){
        color=weatherColor.rgb*light*RAIN_BRIGHTNESS*tintExposure.rgb*tintExposure.w;
        float distance=length(p-camera.xyz);
        float optical=max(0,distance-CL_fogStart)*CL_fogDensity*.001*exp(-max(0,p.z-CL_fogBase)/CL_fogHeight);
        coverage*=exp(-optical);
        color=lerp(color,fogColorDensity.rgb,1-exp(-optical));
    }
    if(weatherParams.z==3)color=weatherColor.rgb*.7*light*2.4*tintExposure.rgb*tintExposure.w;
    // The far snow is lit like the flakes: an unlit veil hazed a night sky grey.
    if(weatherParams.z==2)color=weatherColor.rgb*min(1,light*SNOW_BRIGHTNESS)*tintExposure.rgb*tintExposure.w;
    // Far snow and ash sink into the height fog as rain does: the weather is drawn after the fog
    // volumes, so the fog is not laid over it.
    [branch]if(!rain){
        float optical=max(0,length(p-camera.xyz)-CL_fogStart)*CL_fogDensity*.001*exp(-max(0,p.z-CL_fogBase)/CL_fogHeight);
        color=lerp(color,fogColorDensity.rgb,1-exp(-optical));
    }
    // HALF A STEP OF NOISE ON WHAT IS DRAWN. A gust's veil changes slowly over the frame, and laid on
    // an 8-bit image its soft edge came out as bands. Dithering the samples along the ray hid them
    // but drew grain; half a display step on the output does not.
    float dither=(InterleavedGradient(pixel+float2(41,7))-.5)/255;
    return float4(color+dither,saturate(coverage+dither));
}
)HLSL"
#include "EnvironmentSplashShaders.h"
;
}
