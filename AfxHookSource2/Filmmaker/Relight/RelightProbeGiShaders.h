#pragma once
namespace Filmmaker::Relight {
// Runtime bounce: whole-map static voxels (once per map), a camera-following region with the
// stable map occupancy, per-frame direct-light injection with brick-binned
// lights and fed-back bounce (multiple bounces), and a probe history that scrolls with the
// camera and advances recursive transport without swapping a finished volume in.
inline constexpr char kProbeGiShader[]=R"HLSL(
// Must equal GiProbeResolution (checked by RelightProbeGpuTests).
#define PROBE_RESOLUTION 64
// Fraction of a hemisphere's rays that may hit back faces before the probe band is invalid
// (filled from open neighbours). DDGI's usual threshold.
#define PROBE_BACKFACE_LIMIT .25
cbuffer Build:register(b0) {
    float4 gridLowSize,gridInfo; // region low+size | voxel resolution, cell size, record count, cursor
    float4 giLowSize,giInfo;     // trace volume low+size | enabled, cell size, resolution, bounce gain
    float4 sky,ground;           // live sky radiance | update counter (x)
    float4 staticOffset;         // region origin in static cells (xyz)
    float4 staticDims;           // static grid dimensions (xyz), reserved (w)
    float4 probeInfo;            // slices this pass, history blend, multi-bounce gain, authored edit
    float4 shift;                // probe history shift in probes (xyz)
    float4 voxelTarget;          // voxelization: low xyz, raster extent w
    float4 voxelDims;            // voxelization: dims xyz, first triangle w
    float4 reflection;           // reflection capture origin xyz, face size w
    float4 visibleInfo;          // current depth positions width, height, stride
    float4 reflectionHistory;    // retained-capture history weight (zero for cuts/edits)
};
cbuffer Object:register(b1) { row_major float4x4 model; };

// Voxel word: bit31 occupied, bit30 porous (foliage), bits 0-14 normal (5 bits per axis),
// bits 15-29 albedo as sqrt-encoded 5 bits per channel (keeps dark reflectances distinct).
uint PackVoxel(float3 n,float3 color,bool porous) {
    uint3 q=(uint3)round(saturate(n*.5+.5)*31);
    uint3 c=(uint3)round(sqrt(saturate(color))*31);
    return 0x80000000u|(porous?0x40000000u:0)|q.x|(q.y<<5)|(q.z<<10)|(c.x<<15)|(c.y<<20)|(c.z<<25);
}
float3 VoxelNormal(uint v){return normalize(float3(v&31,(v>>5)&31,(v>>10)&31)/31*2-1);}
float3 VoxelAlbedo(uint v){float3 s=float3((v>>15)&31,(v>>20)&31,(v>>25)&31)/31;return s*s;}

// ---- voxelization (dominant-axis projection, 2 raster pixels per cell) ----
RWTexture3D<uint> voxelOutput:register(u0);
StructuredBuffer<uint> triangleAlbedo:register(t10);
struct VoxelVertex { float3 world:POSITION; };
VoxelVertex VoxelVS(float3 p:POSITION) { VoxelVertex o;o.world=mul(model,float4(p,1)).xyz;return o; }
struct VoxelPixel { float4 position:SV_Position; float3 world:TEXCOORD0; nointerpolation float3 normal:TEXCOORD1; nointerpolation uint id:TEXCOORD2; };
[maxvertexcount(3)]
void VoxelGS(triangle VoxelVertex input[3],uint id:SV_PrimitiveID,inout TriangleStream<VoxelPixel> output) {
    float3 n=cross(input[1].world-input[0].world,input[2].world-input[0].world);
    if(dot(n,n)<1e-12)return;
    n=normalize(n);float3 a=abs(n);int axis=a.x>=a.y&&a.x>=a.z?0:a.y>=a.z?1:2;
    [unroll]for(int v=0;v<3;++v){
        float3 uv=(input[v].world-voxelTarget.xyz)/voxelTarget.w;
        float2 plane=axis==0?uv.yz:axis==1?uv.xz:uv.xy;
        VoxelPixel o;o.position=float4(plane*float2(2,-2)+float2(-1,1),.5,1);o.world=input[v].world;o.normal=n;o.id=id;output.Append(o);
    }
}
// Static and region grids share one cell size (gridInfo.y), aligned so region cells map to
// static cells by an integer offset.
int3 VoxelCell(float3 world){return (int3)floor((world-voxelTarget.xyz)/gridInfo.y);}
void VoxelStaticPS(VoxelPixel i) {
    int3 cell=VoxelCell(i.world);
    if(any(cell<0)||any(cell>=(int3)voxelDims.xyz))return;
    uint material=triangleAlbedo[i.id+(uint)voxelDims.w];
    float3 color=float3(material&255,(material>>8)&255,(material>>16)&255)/255;
    InterlockedMax(voxelOutput[cell],PackVoxel(i.normal,color,(material&0x01000000u)!=0));
}
// ---- region voxel lookup: stable map occupancy ----
Texture3D<uint> staticVoxels:register(t0);
uint RegionVoxel(int3 cell) {
    if(any(cell<0)||any(cell>=(int)gridInfo.x))return 0;
    int3 s=cell+(int3)staticOffset.xyz;
    if(any(s<0)||any(s>=(int3)staticDims.xyz))return 0;
    return staticVoxels[s];
}

// ---- direct + fed-back indirect injection ----
struct Light {float4 row[30];};
StructuredBuffer<Light> lights:register(t1);
Texture2D<float> shadowAtlas:register(t2);
StructuredBuffer<uint> brickOffsets:register(t7);
StructuredBuffer<uint> brickRecords:register(t8);
Texture3D<float4> previousProbes:register(t9);
Texture3D<float4> distanceHistory:register(t15);
Texture3D<float4> glassCookies:register(t14);
SamplerComparisonState shadowSampler:register(s1);
SamplerState giSampler:register(s0);
RWTexture3D<float4> radianceOutput:register(u1);
RWTexture3D<float4> directionOutput:register(u2);
Texture3D<float4> giRadiance:register(t3);
Texture3D<float4> giDirection:register(t4);
Texture3D<float4> giBackRadiance:register(t19);
RWTexture3D<float4> backRadianceOutput:register(u6);
// Gather history through the same distance visibility used by material receivers.
// Hardware trilinear filtering alone blends the far side of walls into every bounce.
float3 ProbeIrradiance(float3 p,float3 n) {
    const float resolution=PROBE_RESOLUTION;
    float3 q=clamp((p-gridLowSize.xyz)/gridLowSize.w*resolution,.5,resolution-.5);
    int3 base=(int3)floor(q-.5);float3 fraction=frac(q-.5);
    float3 result=0;float total=0;
    [unroll]for(uint corner=0;corner<8;++corner){
        int3 offset=int3(corner&1,(corner>>1)&1,(corner>>2)&1);
        int3 cell=clamp(base+offset,0,(int)resolution-1);
        float3 delta=q-(float3(cell)+.5);
        float originCode=distanceHistory.Load(int4(cell,0)).w;
        float code=abs(originCode),magnitude=frac(code)*(originCode>=0?1:-1);
        uint relocatedAxis=(uint)floor(code);
        delta-=float3(relocatedAxis==1,relocatedAxis==2,relocatedAxis==3)*magnitude;
        float rawSquared=dot(delta,delta),distanceSquared=max(rawSquared,.0001);
        float3 angular=rawSquared>1e-8?delta*delta/rawSquared:float3(1,1,1)/3;
        float visibility=0;
        [unroll]for(uint axis=0;axis<3;++axis){
            uint band=axis+(delta[axis]>=0?0:3);
            float4 moments=distanceHistory.Load(int4(cell.xy,cell.z+band*(uint)resolution,0));
            float beyond=max(sqrt(distanceSquared)-moments.x,0);
            float variance=max(moments.y-moments.x*moments.x,.0001);
            float v=variance/(variance+beyond*beyond);
            visibility+=angular[axis]*v*v*v*moments.z;
        }
        float3 trilinear=lerp(1-fraction,fraction,float3(offset));
        float facing=max(.0001,(1-dot(delta,n)*rsqrt(distanceSquared))*.5);
        float weight=trilinear.x*trilinear.y*trilinear.z*visibility*(facing*facing+.2);
        if(weight<=1e-8)continue;
        float3 color=0;float valid=0;
        [unroll]for(uint hemisphere=0;hemisphere<3;++hemisphere){
            float angular=n[hemisphere]*n[hemisphere];if(angular<1e-4)continue;
            uint face=hemisphere+(n[hemisphere]>=0?0:3);
            float4 h=previousProbes.Load(int4(cell.xy,cell.z+face*(uint)resolution,0));
            if(h.a>=0){color+=angular*(h.rgb+h.a*sky.rgb);valid+=angular;}
        }
        result+=weight*color;total+=weight*valid;
    }
    // Never normalize a cage made entirely of blocked or uninitialized probes.
    // Fade low-confidence cages continuously. A binary validity cutoff caused
    // a black flash as moving receivers crossed the normalization threshold.
    // Fully blocked cages still have zero numerator and receive no light.
    return result/max(total,1e-4)*saturate(total*1e4);
}
float CubeFaceEnergy(float3 ndc) {
    float3 axis=abs(float3(ndc.xy*1.111111111,1));axis/=max(axis.x,max(axis.y,axis.z));
    float3 weights=smoothstep(.9,1,axis);
    return weights.z/max(dot(weights,float3(1,1,1)),1e-6);
}
// A cube shadow record (Relight::ToCubeShadowRecord, row 21.w -12347): one record for the six
// faces, their rects in rows 23-28 and depth a + b / axis distance in row 22. Blends the faces as
// six face records (CubeFaceEnergy each) would.
float CubeShadowVisibility(Light light,float3 p) {
    float3 d=p-light.row[8].xyz,a=abs(d);float m=max(a.x,max(a.y,a.z));
    float3 w=smoothstep(.9,1,a/max(m,1e-6));w/=max(dot(w,float3(1,1,1)),1e-6);
    float visibility=0;
    [unroll]for(uint k=0;k<3;++k){
        if(w[k]<=0)continue;
        float s=d[k]<0?-1:1;uint face=2*k+(d[k]<0?1:0);
        float2 uv=k==0?float2(s*d.y,d.z):k==1?float2(-s*d.x,d.z):float2(s*d.x,d.y);
        float4 rect=light.row[23+face];
        float2 atlas=uv/a[k]*(1/1.111111111)*rect.zw+rect.xy; // the face record's narrowed NDC
        float depth=light.row[22].y+light.row[22].z/a[k];
        visibility+=w[k]*shadowAtlas.SampleCmpLevelZero(shadowSampler,atlas,depth-.001);
    }
    return visibility;
}
float3 InjectSurface(uint3 id,float3 p,float3 n) {
    float3 radiance=0;
    uint brick=((id.z/8)*16+(id.y/8))*16+(id.x/8);
    uint first=brickOffsets[brick],last=brickOffsets[brick+1];
    [loop]for(uint k=first;k<last;++k) {
        Light light=lights[brickRecords[k]];bool local=light.row[8].w>0;
        float3 delta=local?light.row[8].xyz-p:light.row[8].xyz;
        float d2=max(dot(delta,delta),1e-5);float3 l=delta*rsqrt(d2);
        float attenuation=local?saturate(light.row[9].x+light.row[9].y*sqrt(d2)):1;
        if(attenuation<=0)continue;
        attenuation=attenuation*attenuation*(3-2*attenuation);
        float4 position4=float4(p+n*gridInfo.y*.5,1);
        float4 clip=float4(dot(light.row[0],position4),dot(light.row[1],position4),dot(light.row[2],position4),dot(light.row[3],position4));
        // Face records share radiance: reject outside their frustum to avoid sixfold energy.
        // Stained glass beams (directional with a cookie) light only their beam, in its colours.
        bool beam=!local&&light.row[10].w>0,fixture=light.row[20].x>0;
        if((local||beam||fixture)&&(clip.w<=0||any(abs(clip.xy)>clip.w)||clip.z<0||clip.z>clip.w))continue;
        float3 ndc=clip.xyz/max(clip.w,1e-5);
        if(light.row[9].z>0)attenuation*=smoothstep(0,1,ndc.z*light.row[9].z);
        if(light.row[9].w>0)attenuation*=smoothstep(0,1,(1-ndc.z)*light.row[9].w);
        if(local&&light.row[12].y!=0){
            float4 q=light.row[11];
            float3 forward=float3(1-2*(q.y*q.y+q.z*q.z),2*(q.x*q.y+q.w*q.z),2*(q.x*q.z-q.w*q.y));
            float3 axis=float3(2*(q.x*q.y-q.w*q.z),1-2*(q.x*q.x+q.z*q.z),2*(q.y*q.z+q.w*q.x));
            float halfLength=light.row[12].z;
            float3 closest=delta-axis*clamp(dot(delta,axis),-halfLength,halfLength);
            attenuation*=saturate(light.row[12].x+light.row[12].y*dot(forward,closest*rsqrt(max(dot(closest,closest),1e-5))));
        }
        if(fixture){
            float2 edge=light.row[14].xy,a=abs(ndc.xy);float roundness=light.row[14].z;
            if(roundness<=0)attenuation*=smoothstep(0,1,saturate((1-a.x)/max(1-edge.x,1e-6)))*smoothstep(0,1,saturate((1-a.y)/max(1-edge.y,1e-6)));
            else {
                float power=2/roundness,exponent=-.5*roundness;
                float inside=edge.x*edge.y*pow(max(pow(edge.y*a.x,power)+pow(edge.x*a.y,power),1e-30),exponent);
                float outside=pow(max(pow(a.x,power)+pow(a.y,power),1e-30),exponent);
                attenuation*=inside<outside?smoothstep(0,1,saturate((1-outside)/(inside-outside))):float(outside>1);
            }
        }
        // Match native cube-face energy normalization in the overlapping cull wedges.
        if(light.row[21].w==-12346)attenuation*=CubeFaceEnergy(ndc);
        float visibility=1;
        if(light.row[21].w==-12347)visibility=CubeShadowVisibility(light,position4.xyz);
        else if(light.row[13].z>0&&clip.w>0) {
            float3 q=clip.xyz/clip.w;
            float2 uv=q.xy*light.row[13].zw+light.row[13].xy;
            visibility=shadowAtlas.SampleCmpLevelZero(shadowSampler,uv,q.z-.001);
        }
        if(local)attenuation*=light.row[8].w/max(light.row[8].w,d2);
        float3 tint=1;
        if(beam)tint=glassCookies.SampleLevel(giSampler,float3(clip.xy/clip.w*float2(.5,-.5)+.5,light.row[10].w),0).rgb;
        radiance+=light.row[10].rgb*tint*(attenuation*max(dot(n,l),0)*visibility);
    }
    // Light arriving from the previous bounce solution: surfaces lit only by bounce relight
    // their surroundings in turn (converges to multiple bounces over successive frames).
    radiance+=ProbeIrradiance(p+n*gridInfo.y,n)*probeInfo.z;
    return radiance;
}
[numthreads(8,8,1)]
void InjectCS(uint3 id:SV_DispatchThreadID) {
    uint packed=RegionVoxel(int3(id));
    if(!(packed&0x80000000u)){radianceOutput[id]=0;backRadianceOutput[id]=0;directionOutput[id]=0;return;}
    float3 n=VoxelNormal(packed),color=VoxelAlbedo(packed);
    float3 p=gridLowSize.xyz+(float3(id)+.5)*gridInfo.y;
    float coverage=(packed&0x40000000u)?.35:1;
    // A coarse wall cell represents both faces. Keep their outgoing radiance
    // separate so a lamp in a room cannot illuminate the exterior face.
    radianceOutput[id]=float4(min(InjectSurface(id,p,n)*color,64)*coverage,coverage);
    backRadianceOutput[id]=float4(min(InjectSurface(id,p,-n)*color,64)*coverage,coverage);
    directionOutput[id]=float4(n,0);
}
)HLSL"
#include "../Environment/EnvironmentGiTraceCoreShaders.h"
R"HLSL(
RWTexture3D<float4> probeOutput:register(u4);
Texture3D<float4> transportInput:register(t5);
RWTexture3D<float4> composedOutput:register(u5);
RWTexture3D<float4> distanceOutput:register(u3);
// Sixteen cosine-weighted voxel rays per ambient-cube hemisphere, anchored in world space;
// history accumulates radiance and first/second hit-distance moments for visibility weighting.
float4 SurfaceRay(float3 origin,float3 direction,float maxDistance,uint maxSteps,out float distanceInProbes,out float backface) {
    float4 result=0;float spacing=gridLowSize.w/PROBE_RESOLUTION;
    distanceInProbes=8;backface=0;
    // Traverse occupied cells, not linearly filtered opacity. Interpolating an
    // opaque wall into adjacent empty cells makes outward-facing probes see a
    // fictitious black shell around ledges and leaves a grid in shaded surfaces.
    float3 local=(origin-giLowSize.xyz)/giInfo.y;
    int3 cell=(int3)floor(local+direction*.001);
    int3 stepDirection=int3(direction.x>=0?1:-1,direction.y>=0?1:-1,direction.z>=0?1:-1);
    float3 inverse=1/max(abs(direction),1e-6);
    float3 edge=float3(cell)+float3(direction.x>=0,direction.y>=0,direction.z>=0);
    float3 next=abs(edge-local)*inverse*giInfo.y;
    float3 stepDistance=inverse*giInfo.y;
    float distance=0;
    [loop]for(uint step=0;step<maxSteps;++step){
        if(any(cell<0)||any(cell>=(int)gridInfo.x)||distance>=maxDistance)break;
        float4 value=giRadiance.Load(int4(cell,0));
        if(value.a>0){
            float3 normal=giDirection.Load(int4(cell,0)).xyz;
            if(value.a>.5&&distanceInProbes==8){
                distanceInProbes=distance/spacing;
                if(RegionVoxel(cell)&0x80000000u)backface=dot(normal,direction)>.05?1:0;
            }
            float facing=dot(normal*rsqrt(max(dot(normal,normal),1e-8)),-direction);
            if(facing<0)value.rgb=giBackRadiance.Load(int4(cell,0)).rgb;
            value.rgb*=saturate(abs(facing)*4);
            result+=(1-result.a)*value;
            if(result.a>.98)break;
        }
        float crossing=min(next.x,min(next.y,next.z));
        bool3 crossed=next<=crossing+1e-5;
        cell+=int3(crossed)*stepDirection;
        next+=float3(crossed)*stepDistance;
        distance=crossing;
    }
    return result;
}
float4 ProbeRay(float3 origin,float3 direction,out float distanceInProbes,out float backface) {
    return SurfaceRay(origin,direction,gridLowSize.w/PROBE_RESOLUTION*8,64,distanceInProbes,backface);
}
static const float3 hemisphereQuadrature[16]={
    float3(0.9842509843,0.0000000000,0.1767766953),
    float3(-0.7019542590,0.6430476018,0.3061862178),
    float3(0.0803056565,-0.9150415300,0.3952847075),
    float3(0.5377890551,0.7014505914,0.4677071733),
    float3(-0.8348314744,-0.1476699340,0.5303300859),
    float3(0.6835199104,-0.4347994159,0.5863019700),
    float3(-0.2000385528,0.7441334406,0.6373774392),
    float3(-0.3359408349,-0.6468336381,0.6846531969),
    float3(0.6431093279,0.2348624967,0.7288689869),
    float3(-0.5891570041,0.2431954451,0.7705517504),
    float3(0.2485017433,-0.5310337876,0.8100925873),
    float3(0.1587192360,0.5060219403,0.8477912479),
    float3(-0.4046654885,-0.2345119238,0.8838834765),
    float3(0.3860649978,-0.0848753053,0.9185586535),
    float3(-0.1760967056,0.2504794408,0.9519716382),
    float3(-0.0227176943,-0.1753108849,0.9842509843)
};
)HLSL"
R"HLSL(
float4 Hemisphere(float3 p,float3 n,float rotation,out float2 moments,out float backfaces) {
    float3 t=normalize(cross(n,abs(n.z)<.9?float3(0,0,1):float3(0,1,0)));
    float3 b=cross(n,t);float4 result=0;
    moments=0;backfaces=0;float sine,cosine;sincos(rotation,sine,cosine);
    [loop]for(uint i=0;i<16;++i) {
        float3 ray=hemisphereQuadrature[i];
        float3 direction=n*ray.z+t*(ray.x*cosine-ray.y*sine)+b*(ray.x*sine+ray.y*cosine);
        float distance,backface;float4 traced=ProbeRay(p,direction,distance,backface);
        moments+=float2(distance,distance*distance);backfaces+=backface;
        // Every ray contributes to the cosine integral. An opaque back face
        // blocks skylight just as its front face does, and supplies its own bounce.
        result.rgb+=traced.rgb;
        result.a+=(1-traced.a)*(direction.z*.5+.5);
    }
    moments/=16;backfaces/=16;return result/16;
}
float4 Hemisphere(float3 p,float3 n,float rotation,out float2 moments) {
    float backfaces;return Hemisphere(p,n,rotation,moments,backfaces);
}
// Traces background, nearby, or newly exposed probes and updates their history. The previous
// history is read through an SRV (typed UAV loads of half floats are not guaranteed on D3D11).
Texture3D<float4> historyInput:register(t12);
Texture2D<float4> visiblePositions:register(t16);
Texture3D<uint> visibleProbes:register(t17);
ByteAddressBuffer visibleList:register(t18);
// Reused by independent entry points: normalization row or compact visible list.
RWByteAddressBuffer reflectionNormOutput:register(u7);
// A receiver needs the corners of its NORMAL-BIASED cage and composition neighbours.
// Mark on the GPU, without readback or allocating rays for every frustum probe.
[numthreads(8,8,1)]
void MarkVisibleCS(uint3 id:SV_DispatchThreadID) {
    uint2 pixel=id.xy*(uint)visibleInfo.z;
    if(any(pixel>=(uint2)visibleInfo.xy))return;
    float4 receiver=visiblePositions.Load(int3(pixel,0));
    float3 uv=(receiver.xyz-gridLowSize.xyz)/gridLowSize.w;
    if(receiver.w<=0||any(uv<0)||any(uv>=1))return;
    // The native normal map is unavailable in the depth pass. Cover every cage
    // reachable by the 0.35-probe normal bias rather than guessing a screen normal.
    // Each dimension has two or three corners; the halo includes only face
    // neighbours used by ComposeCS, not unnecessary edge/corner neighbours.
    int3 low=(int3)floor(uv*PROBE_RESOLUTION-.5-.35);
    int3 high=(int3)floor(uv*PROBE_RESOLUTION-.5+.35)+1;
    [loop]for(int z=low.z-1;z<=high.z+1;++z)
    [loop]for(int y=low.y-1;y<=high.y+1;++y)
    [loop]for(int x=low.x-1;x<=high.x+1;++x){
            int3 cell=int3(x,y,z);
            uint outside=(x<low.x||x>high.x)+(y<low.y||y>high.y)+(z<low.z||z>high.z);
            if(outside>1)continue;
            if(all(cell>=0)&&all(cell<PROBE_RESOLUTION)){
                uint previous;InterlockedOr(voxelOutput[cell],1,previous);
                if(previous==0){
                    uint index,unused;reflectionNormOutput.InterlockedAdd(12,1,index);
                    reflectionNormOutput.Store(16+index*4,(cell.z*PROBE_RESOLUTION+cell.y)*PROBE_RESOLUTION+cell.x);
                    reflectionNormOutput.InterlockedMax(0,index/64+1,unused);
                }
            }
    }
}
[numthreads(8,8,1)]
void ProbeCS(uint3 id:SV_DispatchThreadID) {
    // Compact background dispatch: all lanes trace useful probes. Anchor its phase
    // in world space so scrolling cannot starve a retained probe. Separate compact
    // neighbourhood work refreshes visible close geometry every frame.
    // One lane traces one hemisphere, not six serial hemispheres. Keep every
    // sample and world seed; independent bands can now occupy separate GPU waves.
    uint mode=(uint)ground.y,band;
    if(mode==4){
        band=id.y/8;
        uint index=(id.x/8)*64+(id.y%8)*8+id.x%8;
        if(index>=visibleList.Load(12))return;
        uint cell=visibleList.Load(16+index*4);
        id=uint3(cell%PROBE_RESOLUTION,(cell/PROBE_RESOLUTION)%PROBE_RESOLUTION,cell/(PROBE_RESOLUTION*PROBE_RESOLUTION));
    }else{
        uint layers=mode==0?(uint)probeInfo.x:mode==1?16:PROBE_RESOLUTION;
        band=id.z/layers;id.z%=layers;
    }
    const uint resolution=PROBE_RESOLUTION;const uint stride=max(resolution/(uint)probeInfo.x,1);
    const float spacing=gridLowSize.w/resolution;
    int3 nearBase=clamp((int3)floor((reflection.xyz-gridLowSize.xyz)/spacing)-8,0,(int)resolution-16);
    if(mode==0){
        int3 base=(int3)round(gridLowSize.xyz/spacing);
        uint phase=uint((int)gridInfo.w+(base.x+(int)id.x)*3+(base.y+(int)id.y)*5-base.z)%stride;
        id.z=id.z*stride+phase;
        if(reflection.w>0&&all((int3)id>=nearBase)&&all((int3)id<nearBase+16))return;
    }else if(mode==1)id+=uint3(nearBase);
    if(mode<2&&ground.z>0&&visibleProbes.Load(int4(id,0))!=0)return;
    float age=historyInput.Load(int4(id,0)).a;
    bool exposed=age<-.5&&age>-1.5;
    // Uninitialized borders/camera cuts are handled exactly once, by mode 2.
    if(mode==3){if(visibleProbes.Load(int4(id,0))==0)return;}
    else if(mode!=4&&(mode==2)!=exposed)return;
    float3 p=gridLowSize.xyz+(float3(id)+.5)*spacing;
    int3 worldCell=(int3)round(p/spacing-.5);
    // A probe whose own voxel holds geometry sits in or against a surface: its cones start
    // behind that surface and would carry the far side's light (a lit exterior wall into a dark
    // room). Mark it invalid (alpha -2), distinct from uninitialized history (-1).
    int3 cell=(int3)floor((p-gridLowSize.xyz)/gridInfo.y);
    uint voxel=RegionVoxel(cell);
    bool inside=(voxel&0x80000000u)!=0;
    // One origin for every colour and visibility band. Different per-band origins
    // make an apparently valid visibility query read colour traced inside a wall.
    float3 origin=p;float relocation=0;bool valid=true;
    if(inside){
        valid=false;float best=-2;float3 surfaceNormal=VoxelNormal(voxel);
        [unroll]for(uint candidate=0;candidate<6;++candidate){
            uint axis=candidate%3;float sign=candidate<3?1:-1;
            float3 direction=float3(axis==0,axis==1,axis==2)*sign;
            int3 target=cell+(int3)direction;
            if(any(target<0)||any(target>=(int)gridInfo.x)||gridInfo.y>gridLowSize.w/resolution*.5)continue;
            if(RegionVoxel(target)&0x80000000u)continue;
            float score=dot(surfaceNormal,direction);
            if(score>best){best=score;valid=true;origin=p+direction*gridInfo.y;
                relocation=sign*(axis+1+gridInfo.y/(gridLowSize.w/resolution));}
        }
    }
    {
        uint3 texel=uint3(id.xy,id.z+band*resolution);
        float sign=band<3?1:-1;
        float3 n=float3(band%3==0?sign:0,band%3==1?sign:0,band%3==2?sign:0);
        if(!valid){probeOutput[texel]=float4(0,0,0,-2);distanceOutput[texel]=0;return;}
        // Stable quadrature prevents sampling noise from animating a stationary scene.
        // Hash the world probe cell: camera scrolling must keep the same ray directions.
        uint seed=(uint(worldCell.x)*73856093u)^(uint(worldCell.y)*19349663u)^(uint(worldCell.z)*83492791u)^(band*2654435761u);
        float rotation=float(seed&65535u)/65536.*6.2831853;
        float4 previous=distanceHistory[texel],old=historyInput[texel];
        bool reset=old.a<0||previous.z<=0||abs(previous.w-relocation)>.001;
        float2 moments;float backfaces;float4 traced=Hemisphere(origin,n,rotation,moments,backfaces);
        // A hemisphere that sees the back of surfaces is outside the playable space: under a
        // floor, above a one-sided ceiling, or behind a wall. Those rays escape into the void and
        // read open sky, which leaked into rooms as bright dots along floor and ceiling edges.
        // The shared origin is kept so every band still agrees on where the probe is.
        if(backfaces>=PROBE_BACKFACE_LIMIT){probeOutput[texel]=float4(0,0,0,-2);distanceOutput[texel]=float4(0,0,0,relocation);return;}
        // Authored edits replace obsolete lighting immediately; deterministic steady
        // quadrature needs only a short history for dynamic geometry and bounce.
        float blend=(mode>=3||probeInfo.w>0)?1:probeInfo.y;
        probeOutput[texel]=reset?traced:lerp(old,traced,blend);
        // Signed axis (1..3) + fractional displacement in probe cells.
        distanceOutput[texel]=float4(reset?moments:lerp(previous.xy,moments,blend),1,relocation);
    }
}
// Moves the history with the region: texel id takes what was at id+shift before the move.
// Newly exposed probes are unknown until traced; copying a border carries old-room light through walls.
Texture3D<float4> shiftInput:register(t11);
[numthreads(8,8,1)]
void ShiftCS(uint3 id:SV_DispatchThreadID) {
    const int resolution=PROBE_RESOLUTION;
    int band=id.z/resolution;int3 q=int3(id.xy,id.z%resolution)+(int3)shift.xyz;
    if(any(q<0)||any(q>=resolution)){probeOutput[id]=float4(0,0,0,-1);distanceOutput[id]=0;return;}
    probeOutput[id]=shiftInput[uint3(q.xy,q.z+band*resolution)];
    distanceOutput[id]=distanceHistory[uint3(q.xy,q.z+band*resolution)];
}
// Filter along the actual trace origins, including relocation. Nominal grid
// positions can lie inside a wall even when both probes were moved into the room.
float3 ProbeOrigin(int3 q) {
    float spacing=gridLowSize.w/PROBE_RESOLUTION;
    float code=distanceHistory.Load(int4(q,0)).w;
    uint axis=(uint)floor(abs(code));float amount=frac(abs(code))*(code>=0?1:-1);
    return gridLowSize.xyz+(float3(q)+.5+float3(axis==1,axis==2,axis==3)*amount)*spacing;
}
bool ProbeWallBetween(int3 q,int3 offset) {
    float3 start=ProbeOrigin(q),segment=ProbeOrigin(q+offset)-start;
    uint steps=max(1u,(uint)ceil(length(segment)/(gridInfo.y*.5)));
    [loop]for(uint i=1;i<steps;++i){
        float3 p=start+segment*(float(i)/steps);
        uint voxel=RegionVoxel((int3)floor((p-gridLowSize.xyz)/gridInfo.y));
        if((voxel&0x80000000u)&&!(voxel&0x40000000u))return true;
    }
    return false;
}
[numthreads(8,8,1)]
void ComposeCS(uint3 id:SV_DispatchThreadID) {
    // All six irradiance bands use the same relocated probe origins. Trace each
    // neighbour connection once, then reuse its visibility for every band.
    const uint resolution=PROBE_RESOLUTION;const int3 q=int3(id);
    const int3 offsets[6]={int3(1,0,0),int3(-1,0,0),int3(0,1,0),int3(0,-1,0),int3(0,0,1),int3(0,0,-1)};
    uint visible=0;
    [unroll]for(uint i=0;i<6;++i){int3 n=q+offsets[i];
        if(all(n>=0)&&all(n<(int)resolution)&&!ProbeWallBetween(q,offsets[i]))visible|=1u<<i;
    }
    [unroll]for(uint band=0;band<6;++band){
        uint3 address=uint3(id.xy,id.z+band*resolution);
        float4 transport=transportInput[address];
        // Valid centres keep double weight. An invalid centre is filled only
        // from open, initialized neighbours, exactly as in the per-band pass.
        float weight=transport.a>=0?2:0;
        float4 sum=transport.a>=0?transport*2:0;
        [unroll]for(uint i=0;i<6;++i){if(!(visible&(1u<<i)))continue;
            int3 n=q+offsets[i];float4 v=transportInput[uint3(n.xy,n.z+band*resolution)];
            if(v.a>=0){sum+=v;weight+=1;}
        }
        transport=weight>0?sum/weight:0;
        // Sky radiance and bounce gain are live: an edit shows next frame.
        composedOutput[address]=float4(transport.rgb*giInfo.w+transport.a*sky.rgb,1);
    }
}

// ---- live reflection capture ----
// The environment seen from the capture point, marched through the lit voxel radiance of the
// region (sun, fixtures and bounce, players included); what leaves the region sees the sky.
// Replaces the map's baked reflection cubes, so glossy surfaces reflect the current lighting.
RWTexture2DArray<float4> reflectionOutput:register(u6);
Texture2DArray<float4> reflectionPrevious:register(t20);
ByteAddressBuffer reflectionNormPrevious:register(t21);
float3 CubeDirection(uint face,float2 uv) {
    float2 c=uv*2-1;
    float3 d=face==0?float3(1,-c.y,-c.x):face==1?float3(-1,-c.y,c.x):face==2?float3(c.x,1,c.y):
             face==3?float3(c.x,-1,-c.y):face==4?float3(c.x,-c.y,1):float3(-c.x,-c.y,-1);
    return normalize(d);
}
float3 SkyRadiance(float3 d){return sky.rgb*lerp(.3,1,saturate(d.z*1.5+.5));}
[numthreads(8,8,1)]
void ReflectionCS(uint3 id:SV_DispatchThreadID) {
    const uint size=(uint)reflection.w;if(id.x>=size||id.y>=size)return;
    float3 d=CubeDirection(id.z,(float2(id.xy)+.5)/size);
    float distance,backface;
    float4 hit=SurfaceRay(reflection.xyz,d,giLowSize.w,gridInfo.x*3,distance,backface);
    float4 current=float4(hit.rgb+(1-saturate(hit.a))*SkyRadiance(d),1);
    if(reflectionHistory.x>0)current=lerp(current,reflectionPrevious.Load(int4(id,0)),reflectionHistory.x);
    reflectionOutput[id]=current;
}
// The ambient the capture point receives, as the material expects it for normalization
// (dp4(float4(n,1), row)): mean luminance in w, the per-axis half difference in xyz. The
// material scales each reflection by local ambient over this, so a capture taken in daylight
// does not glow on a surface in a dark room.
Texture3D<float4> composedInput:register(t13);

[numthreads(1,1,1)]
void ReflectionNormCS(uint3 id:SV_DispatchThreadID) {
    const float resolution=PROBE_RESOLUTION;
    float3 q=clamp((reflection.xyz-gridLowSize.xyz)/gridLowSize.w*resolution,.5,resolution-.5);
    float band[6];
    [unroll]for(uint b=0;b<6;++b){
        float3 uvw=float3(q.xy/resolution,(q.z+b*resolution)/(resolution*6));
        band[b]=dot(composedInput.SampleLevel(giSampler,uvw,0).rgb,float3(.2125,.7154,.0721));
    }
    float4 row=float4(band[0]-band[3],band[1]-band[4],band[2]-band[5],0)*.5;
    row.w=max((band[0]+band[1]+band[2]+band[3]+band[4]+band[5])/6,1e-3);
    // Keep the linear term inside the mean so no normal divides by zero or a negative.
    row.xyz*=min(1,.8*row.w/max(length(row.xyz),1e-6));
    if(reflectionHistory.x>0)row=lerp(row,asfloat(reflectionNormPrevious.Load4(0)),reflectionHistory.x);
    reflectionNormOutput.Store4(0,asuint(row));
}
)HLSL";
}
