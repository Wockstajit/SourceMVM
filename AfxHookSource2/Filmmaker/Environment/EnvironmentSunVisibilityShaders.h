#pragma once
namespace Filmmaker::Environment {
// Per-pixel sun visibility with a physically sized penumbra (PCSS).
//
// The engine's own cascade loop filters the sun shadow with one fixed 9-tap kernel, so its
// penumbra has the same width at a contact point and under a ten-metre eave. This pass replaces
// that term (the material samples the result where its screen-space shadow hook would read) with:
//
//   1. a blocker search over the cone the sun disc subtends from the receiver, giving the average
//      distance d from the receiver to whatever shades it;
//   2. a filter whose radius is d * tan(A / 2) for a sun of angular diameter A.
//
// Contact points are therefore sharp and a shadow softens with distance from its caster, as in
// life. Cascade selection and the 12% blend between cascades follow the native loop exactly.
// Everything is deterministic (no temporal noise): a still frame is repeatable.
inline constexpr char SunVisibilityShader[]=R"HLSL(
cbuffer Parameters:register(b0) {
    row_major float4x4 clipToWorld;
    row_major float4x4 cascade[4]; // world -> sun clip (orthographic); z is normalized depth
    float4 rect[4];                // atlas uv = clip.xy * rect.zw + rect.xy (rect.w is negative)
    float4 thresholds;             // per cascade: max(|clip.x|,|clip.y|) below this selects it
    float4 camera;                 // xyz
    float4 viewport;               // origin xy, size zw
    float4 depthInfo;              // viewport depth minimum, span, reversed (1 or 0), unused
    float4 sun;                    // x tan(half angle), y normalized depth bias, z tile size in texels, w max search distance
    float4 atlas;                  // width, height, 1/width, 1/height
    float4 searchDisk[24];          // xy fixed unit-disc offset, z radial distance
    float4 filterDisk[32];
}
#if MSAA_SAMPLES > 1
Texture2DMS<float,MSAA_SAMPLES> sourceDepth:register(t0);
#else
Texture2D<float> sourceDepth:register(t0);
#endif
Texture2D<float> atlasDepth:register(t1);
SamplerComparisonState atlasCompare:register(s0);
RWTexture2D<float4> result:register(u0);
static const uint kSearchTaps=24;
static const uint kFilterTaps=32;
float ReadSurfaceDepth(int2 pixel,out float farthest,out float2 sampleOffset,out float2 farOffset) {
    sampleOffset=float2(0,0);farOffset=float2(0,0);
    pixel=clamp(pixel,int2(0,0),int2(viewport.zw)-1);
#if MSAA_SAMPLES > 1
    float z=sourceDepth.Load(pixel,0);farthest=z;uint nearestSample=0,farthestSample=0;
    [unroll]for(uint s=1;s<MSAA_SAMPLES;++s){
        float v=sourceDepth.Load(pixel,s);
        if(depthInfo.z>0?v>z:v<z){z=v;nearestSample=s;}
        if(depthInfo.z>0?v<farthest:v>farthest){farthest=v;farthestSample=s;}
    }
    sampleOffset=sourceDepth.GetSamplePosition(nearestSample);
    farOffset=sourceDepth.GetSamplePosition(farthestSample);return z;
#else
    farthest=sourceDepth.Load(int3(pixel,0));return farthest;
#endif
}
float ReadDepth(int2 pixel,out float farthest) {
    float2 sampleOffset,farOffset;return ReadSurfaceDepth(pixel,farthest,sampleOffset,farOffset);
}
bool SurfacePosition(int2 pixel,out float3 p,out float z,out float farthest) {
    float2 sampleOffset,farOffset;z=ReadSurfaceDepth(pixel,farthest,sampleOffset,farOffset);
    bool valid=depthInfo.z>0?z>depthInfo.x+.000001:z<depthInfo.x+depthInfo.y-.000001;
    // Match the selected depth sample's location; the pixel centre can lie across a thin edge.
    float2 ndc=(float2(pixel)+.5+sampleOffset-viewport.xy)/viewport.zw*float2(2,-2)+float2(-1,1);
    float4 h=mul(clipToWorld,float4(ndc,(z-depthInfo.x)/depthInfo.y,1));
    p=h.xyz/h.w;
    if(!valid||abs(h.w)<=1e-8||!all(isfinite(p)))return false;
    return true;
}
bool Position(int2 pixel,out float3 p) {
    float z,farthest;
    if(!SurfacePosition(pixel,p,z,farthest))return false;
    // Clear samples and genuinely separate surfaces still need native per-sample
    // shadows. A sloped plane, however, naturally spans depth within one pixel:
    // rejecting that span made distant ground revert to a sharp fixed filter.
    bool farValid=depthInfo.z>0?farthest>depthInfo.x+.000001:farthest<depthInfo.x+depthInfo.y-.000001;
    if(!farValid)return false;
    float2 sampleOffset,farOffset;ReadSurfaceDepth(pixel,farthest,sampleOffset,farOffset);
    float2 ndc=(float2(pixel)+.5+farOffset-viewport.xy)/viewport.zw*float2(2,-2)+float2(-1,1);
    float4 farH=mul(clipToWorld,float4(ndc,(farthest-depthInfo.x)/depthInfo.y,1));
    if(abs(farH.w)<=1e-8)return false;
    float3 farP=farH.xyz/farH.w;
    if(!all(isfinite(farP)))return false;
    if(length(farP-p)<=max(.5,length(p-camera.xyz)*.001))return true;
    float unused;
    float zl=ReadDepth(pixel+int2(-1,0),unused),zr=ReadDepth(pixel+int2(1,0),unused);
    float zu=ReadDepth(pixel+int2(0,-1),unused),zd=ReadDepth(pixel+int2(0,1),unused);
    // Use the smaller gradient on each side, so a silhouette cannot inflate the
    // tolerance with the unrelated background depth.
    // An isolated thin foreground sample has background on both sides. Its
    // opposing slopes must not be mistaken for a continuous receiver plane.
    if((z-zl)*(zr-z)<0||(z-zu)*(zd-z)<0)return false;
    float span=min(abs(z-zl),abs(zr-z))+min(abs(z-zu),abs(zd-z));
    return abs(farthest-z)<=span*1.1+1e-7;
}
// Atlas uv of a cascade's clip position, kept one texel inside the cascade's own tile so a wide
// filter never reads a neighbouring cascade.
float2 AtlasUv(uint i,float2 clipXy) {
    float2 uv=clipXy*rect[i].zw+rect[i].xy;
    float2 reach=abs(rect[i].zw);
    return clamp(uv,rect[i].xy-reach+atlas.zw,rect[i].xy+reach-atlas.zw);
}
// Compare each bilinear texel against the receiver plane at that texel's centre.
// One reference for all four texels requires a slope bias; that bias and the old
// normal offset moved contact receivers through adjacent walls and ledges.
float ReceiverDepth(uint i,float2 uv,int2 texel,float z,float2 gradient,float2 clipPerWorld) {
    float2 offset=((float2(texel)+.5)*atlas.zw-uv)/(rect[i].zw*clipPerWorld);
    return z+dot(offset,gradient);
}
float BlockerDistance(uint i,float2 clipXy,float z,float2 gradient,float2 clipPerWorld,float depthPerUnit) {
    float2 uv=AtlasUv(i,clipXy);int2 texel=int2(uv*atlas.xy);
    float receiver=ReceiverDepth(i,uv,texel,z,gradient,clipPerWorld);
    return (receiver-atlasDepth.Load(int3(texel,0)))/depthPerUnit;
}
float FilterVisibility(uint i,float2 clipXy,float z,float2 gradient,float2 clipPerWorld) {
    float2 uv=AtlasUv(i,clipXy),pixel=uv*atlas.xy-.5;
    int2 base=int2(floor(pixel));float2 fraction=frac(pixel);float4 visibility;
    [unroll]for(uint tap=0;tap<4;++tap){
        int2 texel=base+int2(tap&1,tap>>1);
        float receiver=ReceiverDepth(i,uv,texel,z,gradient,clipPerWorld);
        visibility[tap]=receiver<=atlasDepth.Load(int3(texel,0))?1:0;
    }
    return lerp(lerp(visibility.x,visibility.y,fraction.x),lerp(visibility.z,visibility.w,fraction.x),fraction.y);
}
// The closest covered texel in the contact footprint must retain its narrow
// penumbra even if the wider cone also sees a distant roof or another wall.
float ContactVisibility(uint i,float2 clipXy,float z,float2 gradient,float2 clipPerWorld,float depthPerUnit,out float distanceWorld) {
    float2 uv=AtlasUv(i,clipXy);int2 base=int2(floor(uv*atlas.xy-.5));
    float closest=1e30;float4 covered;
    [unroll]for(uint tap=0;tap<4;++tap){
        int2 texel=base+int2(tap&1,tap>>1);
        float delta=ReceiverDepth(i,uv,texel,z,gradient,clipPerWorld)-atlasDepth.Load(int3(texel,0));
        covered[tap]=delta>0?1:0;
        if(delta>0)closest=min(closest,delta);
    }
    distanceWorld=closest/depthPerUnit;
    // Retain opaque contact at the receiver's own texel. Neighbouring blockers
    // contribute their coverage, rather than blackening the entire footprint.
    float2 fraction=frac(uv*atlas.xy-.5);
    uint centre=(fraction.x>=.5?1:0)+(fraction.y>=.5?2:0);
    if(covered[centre]>0)return 0;
    return 1-lerp(lerp(covered.x,covered.y,fraction.x),lerp(covered.z,covered.w,fraction.x),fraction.y);
}
float CascadeVisibility(uint i,float3 p,float3 n) {
    float4x4 m=cascade[i];
    float3 u=m[0].xyz,v=m[1].xyz,w=m[2].xyz;
    float lu=length(u),lv=length(v),depthPerUnit=length(w);
    float3 ud=u/lu,vd=v/lv,wd=w/depthPerUnit; // wd points from the sun into the scene
    float texelWorld=2/(lu*sun.z);
    // The opaque receiver itself blocks sun arriving behind its geometric plane.
    // A positive slope floor on a back face reverses the plane and can look through contact edges.
    // Only clearly back-facing geometry: the normal is rebuilt from depth, and on a surface the sun
    // grazes its sign flips pixel to pixel, which returned black speckle across sunlit walls and
    // steps (report 2026-10-13). The material's own N.L already darkens a surface turning away.
    if(dot(n,wd)>.2)return 0;
    // Preserve the receiver plane at grazing angles. Clamping its cosine to .2
    // flattens curved tanks and valves, making their own depth texels block them.
    // Keep the sign for slightly back-facing reconstructed normals; only bound
    // the singular tangent. Bias below remains limited independently.
    float cosine=-dot(n,wd);
    float facing=(cosine<0?-1:1)*max(abs(cosine),.001);
    float3 c=mul(m,float4(p,1)).xyz; // keep the receiver on its actual surface
    // Receiver-plane depth: a tap offset (a,b) across the shadow map sees the receiver surface at
    // this depth, so a wide filter does not shadow slanted ground against itself.
    float2 gradient=float2(dot(n,ud),dot(n,vd))/facing*depthPerUnit;
    // The plane removes first-order slope, but a curved receiver can depart from
    // that plane within the bilinear footprint. Bound the remaining comparison
    // tolerance to 1-2 shadow texels, independent of the grazing slope.
    // Five percent of a texel made rough ground shadow its own curved facets.
    float zr=c.z-max(sun.y,texelWorld*(.75+.25/max(abs(facing),.2))*depthPerUnit); // ~1 texel, 2 at grazing: half a texel still speckled bumpy stone (2026-10-13)
    float tanHalf=sun.x;
    float2 clipPerWorld=float2(lu,lv);
    float distanceToNear=max(c.z/depthPerUnit,0);
    float radiusWorld=0;
    if(tanHalf>1e-5){
        // Blocker search. Offsets are spaced quadratically so contact blockers, which can only
        // lie a few texels away, are found as reliably as distant ones.
        float searchRadius=min(distanceToNear,sun.w)*tanHalf;
        // Include the centre: a wide sparse search can miss a thin blocker directly
        // over the receiver and incorrectly erase its entire shadow.
        float centreDistance=BlockerDistance(i,c.xy,zr,gradient,clipPerWorld,depthPerUnit);
        float distanceSum=max(centreDistance,0),found=centreDistance>0?1:0;
        [loop]for(uint j=0;j<kSearchTaps;++j){
            float r=searchRadius*searchDisk[j].z;
            float2 offset=searchDisk[j].xy*searchRadius;
            float receiver=zr+dot(offset,gradient);
            float distanceWorld=BlockerDistance(i,c.xy+offset*clipPerWorld,receiver,gradient,clipPerWorld,depthPerUnit);
            // A blocker hides part of the sun only inside the cone the disc subtends from here.
            if(distanceWorld>0&&r<=distanceWorld*tanHalf){distanceSum+=distanceWorld;found+=1;}
        }
        // No sampled blocker is not proof of open sky: retain the small PCF footprint.
        if(found>0)radiusWorld=distanceSum/found*tanHalf;
    }
    // A subtexel contact penumbra must not borrow light from the opening beside
    // its blocker. Preserve covered contact texels, then fade into the soft disk
    // as the physical penumbra grows beyond the map's resolving power.
    float contactDistance;
    float contact=ContactVisibility(i,c.xy,zr,gradient,clipPerWorld,depthPerUnit,contactDistance);
    radiusWorld=min(radiusWorld,contactDistance*tanHalf);
    float contactWeight=1-smoothstep(.25,.75,radiusWorld/texelWorld);
    if(contactWeight>=1)return contact;
    radiusWorld=max(radiusWorld,texelWorld*.75);
    float lit=0;
    [loop]for(uint k=0;k<kFilterTaps;++k){
        float2 offset=filterDisk[k].xy*radiusWorld;
        float reference=zr+dot(offset,gradient);
        lit+=FilterVisibility(i,c.xy+offset*clipPerWorld,reference,gradient,clipPerWorld);
    }
    return lerp(lit/kFilterTaps,contact,contactWeight);
}
float SurfaceSunVisibility(float3 p,float3 n) {
    // Finest cascade that contains the point, as the native loop picks it.
    uint pick=4;float2 edge=float2(0,0);
    [unroll]for(uint i=0;i<4;++i){
        float4 clip=mul(cascade[i],float4(p,1));
        float extent=max(abs(clip.x),abs(clip.y));
        if(pick==4&&extent<thresholds[i]){pick=i;edge=abs(clip.xy);}
    }
    if(pick==4)return 1;
    float visibility=CascadeVisibility(pick,p,n);
    // Fade into the next, coarser cascade over the outer 12% of this one (the native blend), so a
    // moving camera never shows a seam between cascades; the last cascade fades to lit.
    float2 fade=saturate(edge*8.3333333-7.3333333);
    float weight=(1-fade.x)*(1-fade.y);
    if(weight<1){
        float next=pick<3?CascadeVisibility(pick+1,p,n):1;
        visibility=lerp(next,visibility,weight);
    }
    return visibility;
}
#if MSAA_SAMPLES > 1
bool SamplePosition(int2 pixel,uint sample,out float3 p) {
    pixel=clamp(pixel,int2(0,0),int2(viewport.zw)-1);
    float z=sourceDepth.Load(pixel,sample);
    bool valid=depthInfo.z>0?z>depthInfo.x+.000001:z<depthInfo.x+depthInfo.y-.000001;
    float2 ndc=(float2(pixel)+.5+sourceDepth.GetSamplePosition(sample)-viewport.xy)/viewport.zw*float2(2,-2)+float2(-1,1);
    float4 h=mul(clipToWorld,float4(ndc,(z-depthInfo.x)/depthInfo.y,1));p=h.xyz/h.w;
    return valid&&abs(h.w)>1e-8&&all(isfinite(p));
}
bool FullyOccludedFootprint(int2 pixel) {
    // An ambiguous edge can share zero visibility only when every covered MSAA
    // sample is independently shadowed. Sky and differently lit surfaces keep
    // their native per-material fallback; this never spreads foreground shade
    // onto an exposed background silhouette.
    [loop]for(uint sample=0;sample<MSAA_SAMPLES;++sample){
        float3 p,l,r,u,d;
        if(!SamplePosition(pixel,sample,p))return false;
        bool vl=SamplePosition(pixel+int2(-1,0),sample,l),vr=SamplePosition(pixel+int2(1,0),sample,r);
        bool vu=SamplePosition(pixel+int2(0,-1),sample,u),vd=SamplePosition(pixel+int2(0,1),sample,d);
        if(!(vl||vr)||!(vu||vd))return false;
        float3 dx=vr&&(!vl||length(r-p)<length(p-l))?r-p:p-l;
        float3 dy=vd&&(!vu||length(d-p)<length(p-u))?d-p:p-u;
        float3 n=cross(dx,dy);float n2=dot(n,n);
        if(n2<1e-12||!all(isfinite(n)))return false;
        n*=rsqrt(n2);if(dot(n,camera.xyz-p)<0)n=-n;
        if(SurfaceSunVisibility(p,n)>.001)return false;
    }
    return true;
}
#endif
[numthreads(8,8,1)] void Visibility(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=uint2(viewport.zw)))return;
    int2 pixel=int2(id.xy);
    float3 p;
    if(!Position(pixel,p)){
#if MSAA_SAMPLES > 1
        if(FullyOccludedFootprint(pixel)){result[id.xy]=float4(0,0,0,1);return;}
#endif
        result[id.xy]=float4(1,1,1,0);return;
    }
    float3 l,r,u,d;float unusedZ,unusedFar;
    bool vl=SurfacePosition(pixel+int2(-1,0),l,unusedZ,unusedFar),vr=SurfacePosition(pixel+int2(1,0),r,unusedZ,unusedFar);
    bool vu=SurfacePosition(pixel+int2(0,-1),u,unusedZ,unusedFar),vd=SurfacePosition(pixel+int2(0,1),d,unusedZ,unusedFar);
    float3 dx=vr&&(!vl||length(r-p)<length(p-l))?r-p:p-l;
    float3 dy=vd&&(!vu||length(d-p)<length(p-u))?d-p:p-u;
    float3 n=cross(dx,dy);float n2=dot(n,n);
    if(n2<1e-12||!all(isfinite(n)))n=normalize(camera.xyz-p);
    else{n*=rsqrt(n2);if(dot(n,camera.xyz-p)<0)n=-n;}
    // A fixed light-space disc follows the world under camera motion. Pixel-hashed
    // rotation resamples a receiver whenever it moves on screen, causing shimmer.
    float visibility=SurfaceSunVisibility(p,n);
    result[id.xy]=float4(visibility.xxx,1);
}
)HLSL";
}
