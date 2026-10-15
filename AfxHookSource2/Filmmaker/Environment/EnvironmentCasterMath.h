#pragma once
#include "EnvironmentMath.h"
#include "../MapData/MapCasterLimits.h"
#include <istream>
#include <vector>
#include <string>
#include <cstring>
namespace Filmmaker::Environment {
// Separate from collision: a cutout leaf must never become a solid rain/surface ray hit.
struct CasterVertex { Vec3 position; float u=0, v=0; Vec3 animationColor{}; };
struct CasterAnimation {
    // amount, speed, noise scale, falloff, in the native material's units.
    float sway[4]{0,0,1,1}, flutter[4]{0,0,1,1};
    bool Active() const { return sway[0]>0 || flutter[0]>0; }
};
struct CutoutCaster {
    std::vector<CasterVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<unsigned char> alpha;
    uint32_t width=0, height=0;
    float cutoff=.5f;
    CasterAnimation animation;
    uint32_t addressU=0,addressV=0;
    uint32_t albedo=0x808080; // linear material reflectance, R8G8B8 (bounce lighting)
};
// Stained glass: an opaque caster that also transmits coloured sunlight. Its triangles and
// colour image (sRGB RGBA8) are projected along the sun into the rooms behind it.
struct GlassVertex { Vec3 position; float u=0, v=0; };
static_assert(sizeof(GlassVertex)==20,"stained glass vertex layout");
struct StainedGlass {
    std::vector<GlassVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<unsigned char> rgba;
    uint32_t width=0, height=0;
};
struct VisualCasters {
    std::vector<CutoutCaster> batches;
    std::vector<StainedGlass> glass;
    bool renderGeometry=false;
    std::vector<Vec3> rigidPositions;
    std::vector<uint32_t> rigidIndices;
    std::vector<uint32_t> rigidAlbedo; // one linear R8G8B8 material albedo per rigid triangle
    Vec3 low{1e20f,1e20f,1e20f}, high{-1e20f,-1e20f,-1e20f};
};
inline bool ReadVisualCasters(std::istream &in, VisualCasters &out, std::string &error) {
    VisualCasters result;
    auto fail=[&](const char *message){error=message;return false;};
    auto read=[&](void *p,size_t n){in.read(static_cast<char*>(p),n);return bool(in);};
    char magic[8];uint32_t count=0;
    // Only version 6 (render geometry, material albedo, stained glass). Older packs are rebuilt
    // by map preparation, never read with a guessed reflectance or missing glass.
    if(!read(magic,8)||memcmp(magic,"MVMCAST6",8)||!read(&count,4)||count>4096)
        return fail("Invalid visual caster header");
    uint64_t bytes=0;
    {
        uint32_t flags=0,nv=0,ni=0;
        if(!read(&flags,4)||flags!=1||!read(&nv,4)||!read(&ni,4)||
           nv<3||nv>MapData::kVisualCasterMaxVertices||ni<3||ni>MapData::kVisualCasterMaxIndices||ni%3)return fail("Invalid render geometry coverage");
        result.renderGeometry=true;
        bytes=uint64_t(nv)*sizeof(Vec3)+uint64_t(ni)*4+uint64_t(ni/3)*4;
        result.rigidPositions.resize(nv);result.rigidIndices.resize(ni);result.rigidAlbedo.resize(ni/3);
        if(!read(result.rigidPositions.data(),size_t(nv)*sizeof(Vec3))||!read(result.rigidIndices.data(),size_t(ni)*4)||
           !read(result.rigidAlbedo.data(),size_t(ni/3)*4))
            return fail("Truncated rigid render geometry");
        for(auto p:result.rigidPositions){
            if(!Finite(p)||std::abs(p.x)>100000||std::abs(p.y)>100000||std::abs(p.z)>100000)
                return fail("Invalid rigid render vertex");
            result.low={(std::min)(result.low.x,p.x),(std::min)(result.low.y,p.y),(std::min)(result.low.z,p.z)};
            result.high={(std::max)(result.high.x,p.x),(std::max)(result.high.y,p.y),(std::max)(result.high.z,p.z)};
        }
        for(auto i:result.rigidIndices)if(i>=nv)return fail("Invalid rigid render index");
    }
    for(uint32_t b=0;b<count;++b){
        CutoutCaster c;uint32_t nv=0,ni=0;
        if(!read(&nv,4)||!read(&ni,4)||!read(&c.width,4)||!read(&c.height,4)||!read(&c.cutoff,4)||
           nv<3||nv>2000000||ni<3||ni>6000000||ni%3||!c.width||c.width>8192||!c.height||c.height>8192||
           !std::isfinite(c.cutoff)||c.cutoff<0||c.cutoff>1)
            return fail("Invalid visual caster batch");
        if(!read(&c.animation,sizeof(c.animation)))return fail("Truncated caster animation");
        if(!read(&c.addressU,4)||!read(&c.addressV,4)||c.addressU>4||c.addressV>4)
            return fail("Invalid caster texture addressing");
        if(!read(&c.albedo,4)||(c.albedo>>24))return fail("Invalid caster albedo");
        for(auto values:{c.animation.sway,c.animation.flutter})
            for(int i=0;i<4;++i)if(!std::isfinite(values[i])||values[i]<0||values[i]>100)
                return fail("Invalid caster animation");
        bytes+=uint64_t(nv)*sizeof(CasterVertex)+uint64_t(ni)*4+uint64_t(c.width)*c.height;
        if(bytes>MapData::kVisualCasterMemoryLimit)return fail("Visual caster cache exceeds memory limit");
        c.vertices.resize(nv);c.indices.resize(ni);c.alpha.resize(size_t(c.width)*c.height);
        if(!read(c.vertices.data(),nv*sizeof(CasterVertex))||!read(c.indices.data(),ni*4)||!read(c.alpha.data(),c.alpha.size()))
            return fail("Truncated visual caster cache");
        for(const auto &v:c.vertices){
            const auto p=v.position;
            if(!Finite(p)||std::abs(p.x)>100000||std::abs(p.y)>100000||std::abs(p.z)>100000||
               !std::isfinite(v.u)||!std::isfinite(v.v)||std::abs(v.u)>10000||std::abs(v.v)>10000||
               !Finite(v.animationColor)||v.animationColor.x<0||v.animationColor.x>1||
               v.animationColor.y<0||v.animationColor.y>1||v.animationColor.z<0||v.animationColor.z>1)
                return fail("Invalid visual caster vertex");
            result.low={(std::min)(result.low.x,p.x),(std::min)(result.low.y,p.y),(std::min)(result.low.z,p.z)};
            result.high={(std::max)(result.high.x,p.x),(std::max)(result.high.y,p.y),(std::max)(result.high.z,p.z)};
        }
        for(auto i:c.indices)if(i>=nv)return fail("Invalid visual caster index");
        result.batches.push_back(std::move(c));
    }
    uint32_t glassCount=0;
    if(!read(&glassCount,4)||glassCount>256)return fail("Invalid stained glass count");
    for(uint32_t g=0;g<glassCount;++g){
        StainedGlass glass;uint32_t nv=0,ni=0;
        if(!read(&nv,4)||!read(&ni,4)||!read(&glass.width,4)||!read(&glass.height,4)||nv<3||nv>2000000||ni<3||ni>6000000||ni%3||
           !glass.width||glass.width>512||!glass.height||glass.height>512)return fail("Invalid stained glass batch");
        bytes+=uint64_t(nv)*sizeof(GlassVertex)+uint64_t(ni)*4+uint64_t(glass.width)*glass.height*4;
        if(bytes>MapData::kVisualCasterMemoryLimit)return fail("Visual caster cache exceeds memory limit");
        glass.vertices.resize(nv);glass.indices.resize(ni);glass.rgba.resize(size_t(glass.width)*glass.height*4);
        if(!read(glass.vertices.data(),nv*sizeof(GlassVertex))||!read(glass.indices.data(),ni*4)||!read(glass.rgba.data(),glass.rgba.size()))
            return fail("Truncated stained glass");
        for(const auto &v:glass.vertices)
            if(!Finite(v.position)||std::abs(v.position.x)>100000||std::abs(v.position.y)>100000||std::abs(v.position.z)>100000||
               !std::isfinite(v.u)||!std::isfinite(v.v)||std::abs(v.u)>10000||std::abs(v.v)>10000)return fail("Invalid stained glass vertex");
        for(auto i:glass.indices)if(i>=nv)return fail("Invalid stained glass index");
        result.glass.push_back(std::move(glass));
    }
    if(in.peek()!=std::char_traits<char>::eof())return fail("Unexpected visual caster data");
    out=std::move(result);error.clear();return true;
}
} // namespace Filmmaker::Environment
