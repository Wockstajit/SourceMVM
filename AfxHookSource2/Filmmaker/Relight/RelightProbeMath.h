#pragma once
#include "RelightMath.h"
#include <limits>
#include <vector>
#include <cmath>
namespace Filmmaker::Relight {
using ProbeRow=std::array<float,4>;
using AmbientCube=std::array<Vec3,6>; // +X,+Y,+Z,-X,-Y,-Z, packed along texture W
struct ProbePacking {
    std::array<ProbeRow,6> probe{}; // cb3[1177..1182]
    std::array<ProbeRow,9> volume{}; // cb2[0..8], remaining buffer rows are owned zeros
};
inline AmbientCube HemisphereCube(Vec3 sky,Vec3 ground) {
    const auto side=(sky+ground)*.5f;return {side,side,sky,side,side,ground};
}
inline Vec3 DecodeAmbientCube(const AmbientCube& cube,Vec3 n) {
    return cube[n.x<=0?3:0]*(n.x*n.x)+cube[n.y<=0?4:1]*(n.y*n.y)+cube[n.z<=0?5:2]*(n.z*n.z);
}
inline ProbePacking PackProbe(Vec3 low,Vec3 high,unsigned resolution) {
    ProbePacking out;
    const float lo[3]{low.x,low.y,low.z},hi[3]{high.x,high.y,high.z};
    const float half=.5f/(std::max)(1u,resolution);
    for(unsigned axis=0;axis<3;++axis) {
        const float extent=(std::max)(1.f,hi[axis]-lo[axis]);
        out.probe[axis][axis]=1/extent;out.probe[axis][3]=-lo[axis]/extent;
        out.volume[axis]=out.probe[axis];
        out.probe[3][axis]=0;out.probe[4][axis]=1;out.probe[5][axis]=10000;
        out.volume[3][axis]=axis==2?half/6:half;
        out.volume[4][axis]=axis==2?(1-half)/6:1-half;
        out.volume[5][axis]=out.volume[6][axis]=1;
    }
    // cb3[1182].w is a raw integer volume index, zero for our single owned volume.
    out.probe[5][3]=0;
    return out;
}
inline ProbeRow ProbeSampleCoordinates(Vec3 uvw,const ProbePacking& p,unsigned band) {
    float q[3]{uvw.x,uvw.y,uvw.z/6};
    for(unsigned i=0;i<3;++i)q[i]=std::clamp(q[i],p.volume[3][i],p.volume[4][i]);
    q[2]*=6;
    for(unsigned i=0;i<3;++i)q[i]=q[i]*p.volume[6][i]+p.volume[7][i];
    return {q[0],q[1],q[2]/6+float(band)/6,0};
}
inline constexpr Vec3 ProbeNeutral{.125f,.25f,.375f};

// Runtime bounce volume. It follows the camera instead of stretching over the whole map: fitted
// to Inferno's extent, 32 probes were ~250 units apart and 128 voxels ~64 units wide, far coarser
// than a room or an alley, so bounce smeared through walls. 2560 units at 128 voxels and 64
// probes is 20-unit voxels and 40-unit probes. The shader clamps positions outside the volume to
// its border probes, so distant surfaces keep the edge value rather than losing ambient.
inline constexpr unsigned GiVoxelResolution=128,GiProbeResolution=64,GiBrickResolution=16;
// Live reflection cube face size (texels), marched from the camera through the region.
inline constexpr unsigned GiReflectionSize=128;
inline constexpr float GiRegionSize=2560,GiCell=GiRegionSize/GiVoxelResolution,GiProbeSpacing=GiRegionSize/GiProbeResolution;
struct GiRegion {Vec3 low{};float size=0;
    bool operator==(const GiRegion& o)const{return low.x==o.low.x&&low.y==o.low.y&&low.z==o.low.z&&size==o.size;}};
// Moves in single probe steps, so the history shifts by whole probes and keeps everything it
// already knows; a coarser snap made each move discard the volume and "flip" a new one in.
inline GiRegion CameraGiRegion(Vec3 camera,float size=GiRegionSize) {
    const float step=size/GiProbeResolution;
    const Vec3 centre{std::round(camera.x/step)*step,std::round(camera.y/step)*step,std::round(camera.z/step)*step};
    return {centre-Vec3{size*.5f,size*.5f,size*.5f},size};
}
// Reserve three quarters of the region ahead of the viewer, keeping a quarter
// behind for recursive transport. Snap the final origin, preserving world history.
inline GiRegion ViewGiRegion(Vec3 camera,Vec3 forward) {
    const float length=std::sqrt(forward.x*forward.x+forward.y*forward.y+forward.z*forward.z);
    if(!std::isfinite(length)||length<1e-6f)return CameraGiRegion(camera);
    return CameraGiRegion(camera+forward*(GiRegionSize*.25f/length));
}
// Whole probes the history moves when the region moves (new texel = old texel + shift).
inline std::array<int,3> GiProbeShift(const GiRegion& from,const GiRegion& to) {
    const float step=to.size/GiProbeResolution;
    return {int(std::lround((to.low.x-from.low.x)/step)),int(std::lround((to.low.y-from.low.y)/step)),int(std::lround((to.low.z-from.low.z)/step))};
}
// Lights reaching each 8-voxel brick of the region, so injection tests a handful of records
// per voxel instead of every record in the map. Directional records reach every brick; a
// local record reaches the bricks its attenuation sphere touches (unbounded falloff: all).
struct GiBricks {std::vector<uint32_t> offsets,records;};
inline float GiRecordRange(const LightRecord& r) {
    if(!(r[35]>0))return std::numeric_limits<float>::infinity();
    return r[37]<0?(std::max)(0.f,-r[36]/r[37]):std::numeric_limits<float>::infinity();
}
inline GiBricks BinGiLights(const std::vector<LightRecord>& records,const GiRegion& region) {
    constexpr unsigned n=GiBrickResolution;const float brick=region.size/n;
    std::vector<std::vector<uint32_t>> lists(n*n*n);
    for(uint32_t i=0;i<records.size();++i){
        const auto& r=records[i];const float range=GiRecordRange(r);
        unsigned lo[3]{0,0,0},hi[3]{n-1,n-1,n-1};
        if(r[80]>0){
            Environment::Matrix projection{},inverse{};std::copy_n(r.begin(),16,projection.begin());
            if(Environment::Inverse(projection,inverse)){
                Vec3 a{INFINITY,INFINITY,INFINITY},b{-INFINITY,-INFINITY,-INFINITY};
                for(unsigned v=0;v<8;++v){
                    const float x=(v&1)?1.f:-1.f,y=(v&2)?1.f:-1.f,z=(v&4)?1.f:0.f;
                    const float w=inverse[12]*x+inverse[13]*y+inverse[14]*z+inverse[15];
                    const Vec3 p{(inverse[0]*x+inverse[1]*y+inverse[2]*z+inverse[3])/w,
                        (inverse[4]*x+inverse[5]*y+inverse[6]*z+inverse[7])/w,
                        (inverse[8]*x+inverse[9]*y+inverse[10]*z+inverse[11])/w};
                    a={(std::min)(a.x,p.x),(std::min)(a.y,p.y),(std::min)(a.z,p.z)};
                    b={(std::max)(b.x,p.x),(std::max)(b.y,p.y),(std::max)(b.z,p.z)};
                }
                // Injection biases a voxel receiver toward its normal.
                const float pad=region.size/GiVoxelResolution;
                const float low[3]{region.low.x,region.low.y,region.low.z},min[3]{a.x-pad,a.y-pad,a.z-pad},max[3]{b.x+pad,b.y+pad,b.z+pad};
                bool outside=false;
                for(unsigned axis=0;axis<3;++axis){
                    const float x=std::floor((min[axis]-low[axis])/brick),y=std::floor((max[axis]-low[axis])/brick);
                    if(y<0||x>=float(n)){outside=true;break;}
                    lo[axis]=unsigned((std::max)(0.f,x));hi[axis]=unsigned((std::min)(float(n-1),y));
                }
                if(outside)continue;
            }
        }else if(std::isfinite(range)){
            const float centre[3]{r[32],r[33],r[34]},low[3]{region.low.x,region.low.y,region.low.z};
            bool outside=false;
            for(unsigned axis=0;axis<3;++axis){
                const float a=std::floor((centre[axis]-range-low[axis])/brick),b=std::floor((centre[axis]+range-low[axis])/brick);
                if(b<0||a>=float(n)){outside=true;break;}
                lo[axis]=unsigned((std::max)(0.f,a));hi[axis]=unsigned((std::min)(float(n-1),b));
            }
            if(outside)continue;
        }
        for(unsigned z=lo[2];z<=hi[2];++z)for(unsigned y=lo[1];y<=hi[1];++y)for(unsigned x=lo[0];x<=hi[0];++x)
            lists[(z*n+y)*n+x].push_back(i);
    }
    GiBricks out;out.offsets.reserve(lists.size()+1);out.offsets.push_back(0);
    for(const auto& list:lists){out.records.insert(out.records.end(),list.begin(),list.end());out.offsets.push_back(uint32_t(out.records.size()));}
    if(out.records.empty())out.records.push_back(0); // a structured buffer cannot be empty
    return out;
}
// Native clustered reflection record, cb3[25+9n .. 33+9n] (decoded from the shipped material
// loop): rows 0-2 world->box 3x4 (the capture sits at the box origin), 3 box min + raw cube index
// in w, 4 box max, 5 inverse edge feather, 6 unread, 7 tint, 8 ambient normalization (written
// on the GPU). The ray is parallax-corrected against the box; a box far larger than the map
// makes that a plain direction lookup, and the whole map lies inside the full-weight interior.
// Retain a valid capture point during small movements instead of marching
// camera-dependent voxel silhouettes and changing the normalization every step.
inline constexpr float ReflectionCaptureTravel=GiCell*2;
inline Vec3 StableReflectionOrigin(Vec3 camera,Vec3 previous,bool ready) {
    const auto d=camera-previous;
    return ready&&Dot(d,d)<ReflectionCaptureTravel*ReflectionCaptureTravel?previous:camera;
}
struct ReflectionUpdate {Vec3 origin;float historyWeight;};
inline ReflectionUpdate PlanReflectionUpdate(Vec3 camera,Vec3 previous,bool ready,bool edited,bool responsive) {
    const auto d=camera-previous;
    if(!responsive)return {StableReflectionOrigin(camera,previous,ready),
        ready&&!edited&&Dot(d,d)<GiProbeSpacing*GiProbeSpacing*16?.8f:0.f};
    // Cube texels encode directions from their capture origin. Blending another origin
    // without reprojection leaves stale silhouettes in puddles after movement/cuts.
    // A small stationary history damps transport changes without the old long trail.
    return {camera,ready&&!edited&&Dot(d,d)<1e-6f?.2f:0.f};
}
inline constexpr float ReflectionBoxHalfExtent=1e7f;
inline std::array<ProbeRow,8> ReflectionRecordRows(Vec3 origin,float strength) {
    std::array<ProbeRow,8> rows{};
    const float o[3]{origin.x,origin.y,origin.z},b=ReflectionBoxHalfExtent;
    for(unsigned axis=0;axis<3;++axis){
        rows[axis][axis]=1;rows[axis][3]=-o[axis];
        rows[3][axis]=-b;rows[4][axis]=b;rows[5][axis]=1.f/64;
        rows[7][axis]=strength;
    }
    rows[3][3]=0; // cube 0 of the bound array (read as a raw integer)
    return rows;
}
// cb3[6].xy scale the cap on local/captured ambient: cap = max(roughness*x + y, 1). (0, 1)
// caps it at 1, so a reflection is only ever dimmed where the surface is darker than the
// capture point, never brightened past what was captured.
inline constexpr std::array<float,2> ReflectionNormalizationCap{0.f,1.f};
// The viewer's own body, excluded from the weapon's direct sun shadow: a capsule around the camera (eye) this
// wide, reaching this far above and below it. A standing player is 72 units tall with the eye at 64.
inline constexpr float ViewerShadowRadius=24,ViewerShadowAbove=16,ViewerShadowBelow=80;
// Recursive transport loses a small amount of energy on each bounce. The user's
// display boost is applied only at composition, never fed back into this solution.
inline constexpr float GiFeedbackGain=.95f;
// CPU mirror of the shader voxel word (PackVoxel in RelightProbeGiShaders.h).
inline uint32_t PackGiVoxel(Vec3 n,Vec3 color,bool porous) {
    auto q=[](float v){return uint32_t(std::lround(std::clamp(v*.5f+.5f,0.f,1.f)*31));};
    auto c=[](float v){return uint32_t(std::lround(std::sqrt(std::clamp(v,0.f,1.f))*31));};
    return 0x80000000u|(porous?0x40000000u:0u)|q(n.x)|(q(n.y)<<5)|(q(n.z)<<10)|(c(color.x)<<15)|(c(color.y)<<20)|(c(color.z)<<25);
}

// Transport depends on world-space emitters, never camera-relative GPU records.
// Sky edits also affect recursive transport. Display-only bounce strength is excluded.
inline uint64_t ProbeLightingKey(const Scene& scene,unsigned residentStaticLights=0) {
    uint64_t hash=1469598103934665603ull;
    auto add=[&](const auto& value){const auto* bytes=reinterpret_cast<const unsigned char*>(&value);
        for(size_t i=0;i<sizeof(value);++i)hash=(hash^bytes[i])*1099511628211ull;};
    add(scene.intensity);add(scene.localLights);add(scene.sun);add(scene.hasSun);
    add(scene.indirect);add(scene.skyColor);add(scene.skyIntensity);
    add(scene.sunAngles);add(scene.sunColor);add(scene.sunBrightness);
    add(scene.shadows);add(scene.sunShadows);add(scene.staticShadows);
    add(scene.albedo);add(scene.probeRebuild);add(residentStaticLights);
    add(scene.stainedGlass);
    for(const auto& light:scene.lights){
        add(light.position);add(light.angles);add(light.color);add(light.lumens);
        add(light.brightnessScale);add(light.range);add(light.sizeX);add(light.sizeY);
        add(light.sourceRadius);add(light.enabled);add(light.castShadows);
        add(light.innerAngle);add(light.outerAngle);add(light.sceneObject);add(light.runtimeShadows);
    }
    return hash;
}
}
