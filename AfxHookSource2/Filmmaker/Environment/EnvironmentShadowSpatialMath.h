#pragma once
#include "EnvironmentMath.h"
#include "EnvironmentShadowMath.h"
#include <map>
#include <vector>

namespace Filmmaker::Environment {
struct ShadowDrawRange {
    uint32_t first=0,count=0;
    Vec3 low{},high{};
};
struct ShadowSpatialIndices {
    std::vector<uint32_t> indices;
    std::vector<ShadowDrawRange> ranges;
};
struct RigidShadowGeometry {
    std::vector<Vec3> positions;
    std::vector<uint32_t> indices;
};
inline RigidShadowGeometry BuildRigidShadowGeometry(const OcclusionMesh& world) {
    RigidShadowGeometry out;
    if(!world.visual||!world.visual->renderGeometry){out.positions=world.positions;out.indices=world.indices;return out;}
    if(!world.visual->rigidPositions.empty()){
        out.positions=world.visual->rigidPositions;out.indices=world.visual->rigidIndices;return out;
    }
    // Accurate silhouettes come from opaque render triangles. Physics hulls can
    // close chair legs, cover lamp openings and omit decorative wall props.
    for(const auto& batch:world.visual->batches){
        if(batch.cutoff>0||batch.animation.Active())continue;
        const auto offset=uint32_t(out.positions.size());
        for(const auto& vertex:batch.vertices)out.positions.push_back(vertex.position);
        for(auto index:batch.indices)out.indices.push_back(offset+index);
    }
    return out;
}
// The same choice without copying a whole map's geometry: only render batches
// that lack a precomputed rigid set are concatenated, into the caller's scratch.
struct RigidShadowView {const std::vector<Vec3>& positions;const std::vector<uint32_t>& indices;};
inline RigidShadowView RigidShadowSource(const OcclusionMesh& world,RigidShadowGeometry& scratch) {
    if(!world.visual||!world.visual->renderGeometry)return {world.positions,world.indices};
    if(!world.visual->rigidPositions.empty())return {world.visual->rigidPositions,world.visual->rigidIndices};
    scratch=BuildRigidShadowGeometry(world);
    return {scratch.positions,scratch.indices};
}
// Readers validate the input indices. Reorder triangles into spatial draws while
// sharing the original vertices. Bounds cover the entire triangle, including
// triangles that extend outside the cell containing their centroid.
template<class Position>
inline ShadowSpatialIndices PartitionShadowIndices(const std::vector<uint32_t>& indices,
                                                    Position position,float cellSize=512.f) {
    struct Chunk {std::vector<uint32_t> indices;Vec3 low{},high{};};
    std::map<std::array<int,3>,Chunk> chunks;
    for(size_t i=0;i+2<indices.size();i+=3){
        Vec3 p[3]{position(indices[i]),position(indices[i+1]),position(indices[i+2])};
        const Vec3 center=(p[0]+p[1]+p[2])*(1.f/3);
        const std::array<int,3> key{int(std::floor(center.x/cellSize)),
            int(std::floor(center.y/cellSize)),int(std::floor(center.z/cellSize))};
        auto& chunk=chunks[key];
        if(chunk.indices.empty())chunk.low=chunk.high=p[0];
        for(unsigned j=0;j<3;++j){
            chunk.indices.push_back(indices[i+j]);
            chunk.low={(std::min)(chunk.low.x,p[j].x),(std::min)(chunk.low.y,p[j].y),(std::min)(chunk.low.z,p[j].z)};
            chunk.high={(std::max)(chunk.high.x,p[j].x),(std::max)(chunk.high.y,p[j].y),(std::max)(chunk.high.z,p[j].z)};
        }
    }
    ShadowSpatialIndices out;out.indices.reserve(indices.size());out.ranges.reserve(chunks.size());
    for(auto& entry:chunks){const auto& chunk=entry.second;
        out.ranges.push_back({uint32_t(out.indices.size()),uint32_t(chunk.indices.size()),chunk.low,chunk.high});
        out.indices.insert(out.indices.end(),chunk.indices.begin(),chunk.indices.end());
    }
    return out;
}
struct ShadowFrustum {
    std::array<std::array<float,4>,6> planes{};
    explicit ShadowFrustum(const Matrix& m) {
        for(unsigned i=0;i<4;++i){
            planes[0][i]=m[12+i]+m[i];planes[1][i]=m[12+i]-m[i];
            planes[2][i]=m[12+i]+m[4+i];planes[3][i]=m[12+i]-m[4+i];
            planes[4][i]=m[8+i];planes[5][i]=m[12+i]-m[8+i];
        }
        // Orthographic (sun) projections pancake casters nearer than the interval onto the near
        // plane, so those casters are kept rather than culled.
        if(m[12]==0&&m[13]==0&&m[14]==0&&m[15]==1)planes[4]={0,0,0,1};
    }
    bool Intersects(Vec3 low,Vec3 high,float padding=0)const {
        low=low-Vec3{padding,padding,padding};high=high+Vec3{padding,padding,padding};
        for(const auto& p:planes){
            const Vec3 support{p[0]>=0?high.x:low.x,p[1]>=0?high.y:low.y,p[2]>=0?high.z:low.z};
            const float value=p[0]*support.x+p[1]*support.y+p[2]*support.z+p[3];
            // Conservative floating point tolerance at map coordinates.
            const float tolerance=1e-5f*(std::abs(p[0]*support.x)+std::abs(p[1]*support.y)+std::abs(p[2]*support.z)+std::abs(p[3])+1);
            if(value < -tolerance)return false;
        }
        return true;
    }
};
// Preserve cached depth while deferring animated refreshes. Reserve work for
// the oldest faces so distant shadows and indirect transport cannot starve.
struct ShadowRefreshCandidate {
    unsigned face=0;
    uint64_t last=0;
    float proximity=0;
    bool visible=false,restore=false,realtime=false;
    // A moving actor (player, weapon, ragdoll) is inside this face now. Foliage wind alone also
    // makes a face dynamic; on a planted map those faces filled the whole budget every frame and a
    // running player's shadow refreshed only when its face's turn came round -- the jittering,
    // slow-updating player shadows report (2026-10-13).
    bool actor=false;
};
inline std::vector<unsigned> SelectShadowRefresh(std::vector<ShadowRefreshCandidate> candidates,
                                                 uint64_t frame,unsigned budget=128,unsigned background=16,
                                                 unsigned actorBudget=0) {
    std::vector<unsigned> selected;
    if(!budget||candidates.empty())return selected;
    // Nearby visible receivers must see a current pose, even after a fixture
    // leaves the four high-resolution slots. Background age cannot evict them.
    const auto critical=std::stable_partition(candidates.begin(),candidates.end(),[](const auto& c){return c.realtime;});
    for(auto i=candidates.begin();i!=critical;++i)selected.push_back(i->face);
    candidates.erase(candidates.begin(),critical);
    // Then visible faces with a moving actor in them, nearest first, up to actorBudget: a person's
    // shadow is what the eye tracks, so it refreshes every frame ahead of wind-blown foliage. Like
    // the realtime set, these do not consume the background reserve below.
    if(actorBudget){
        const auto actors=std::stable_partition(candidates.begin(),candidates.end(),[](const auto& c){return c.actor&&c.visible;});
        std::stable_sort(candidates.begin(),actors,[](const auto& a,const auto& b){return a.proximity>b.proximity;});
        const size_t take=(std::min)(size_t(actorBudget),size_t(actors-candidates.begin()));
        for(size_t i=0;i<take;++i)selected.push_back(candidates[i].face);
        candidates.erase(candidates.begin(),candidates.begin()+take);
    }
    budget=(std::max)(budget,unsigned(selected.size())+background);
    const unsigned remaining=budget-unsigned(selected.size());
    const auto age=[&](const ShadowRefreshCandidate& c){return frame>=c.last?frame-c.last:0;};
    const auto priority=[&](const ShadowRefreshCandidate& c){
        return float(age(c)+1)*(c.visible?4.f:1.f)*(1+std::clamp(c.proximity,0.f,1.f))*(c.restore?2.f:1.f);
    };
    std::sort(candidates.begin(),candidates.end(),[&](const auto& a,const auto& b){
        const float pa=priority(a),pb=priority(b);return pa==pb?a.face<b.face:pa>pb;
    });
    const size_t foreground=(std::min)(candidates.size(),size_t(remaining-(std::min)(remaining,background)));
    for(size_t i=0;i<foreground;++i)selected.push_back(candidates[i].face);
    std::sort(candidates.begin()+foreground,candidates.end(),[&](const auto& a,const auto& b){
        return age(a)==age(b)?a.face<b.face:age(a)>age(b);
    });
    for(size_t i=foreground;i<candidates.size()&&selected.size()<budget;++i)selected.push_back(candidates[i].face);
    return selected;
}
} // namespace Filmmaker::Environment
