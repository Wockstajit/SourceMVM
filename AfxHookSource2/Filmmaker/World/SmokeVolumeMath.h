#pragma once
#include "../Environment/SurfaceRayGridMath.h"
#include <queue>
#include <limits>

namespace Filmmaker::SmokeAppearance {
using Environment::Vec3;
inline constexpr int WarpSide=64, WarpStride=66, WarpWidth=1054;
struct WarpCell {float x=0,y=0,z=0,coverage=0;};
struct GrowthVolume {
    Vec3 center; float spread=1; unsigned slot=0;
};
// A view can reorder or cull the volume table without changing a grenade. Ignore
// sub-unit rounding in the uploaded world-space source, well below a growth cell.
inline bool SameGrowthVolume(const GrowthVolume& a,const GrowthVolume& b){
    constexpr float tolerance=.125f;
    return std::fabs(a.center.x-b.center.x)<=tolerance
        &&std::fabs(a.center.y-b.center.y)<=tolerance&&std::fabs(a.center.z-b.center.z)<=tolerance
        &&a.spread==b.spread&&a.slot==b.slot;
}
inline void MergeGrowthVolumes(std::vector<GrowthVolume>& known,const std::vector<GrowthVolume>& visible){
    for(const auto& v:visible){
        if(v.slot>=16||!Environment::Finite(v.center)||!std::isfinite(v.spread)||v.spread<=0)continue;
        auto item=std::find_if(known.begin(),known.end(),[&](const GrowthVolume& old){return old.slot==v.slot;});
        if(item==known.end())known.push_back(v);
        else if(!SameGrowthVolume(*item,v))*item=v;
    }
    std::sort(known.begin(),known.end(),[](const GrowthVolume& a,const GrowthVolume& b){return a.slot<b.slot;});
}
inline unsigned GrowthReadyMask(const std::vector<GrowthVolume>& built,const std::vector<GrowthVolume>& wanted){
    unsigned result=0;
    for(const auto& v:wanted)if(v.slot<16&&std::any_of(built.begin(),built.end(),[&](const GrowthVolume& old){return SameGrowthVolume(old,v);}))result|=1u<<v.slot;
    return result;
}
inline unsigned GrowthWantedMask(const std::vector<GrowthVolume>& wanted){
    unsigned result=0;for(const auto& v:wanted)if(v.slot<16)result|=1u<<v.slot;return result;
}
// Trace neighbour links, then follow shortest collision-free paths from the source.
// This operates on immutable exported physics geometry in a worker, never game memory.
inline std::vector<WarpCell> GrowVolume(const Environment::OcclusionMesh& mesh,
    const Environment::SurfaceRayGrid& grid,const GrowthVolume& volume,int side=WarpSide) {
    if(side<4||side>WarpSide||!Environment::Finite(volume.center)||!std::isfinite(volume.spread)||volume.spread<=0)return {};
    const int count=side*side*side;const float cell=640*volume.spread/side,half=320*volume.spread;
    auto index=[&](int x,int y,int z){return (z*side+y)*side+x;};
    auto position=[&](int i){return volume.center+Vec3{((i%side)+.5f)*cell-half,(((i/side)%side)+.5f)*cell-half,((i/(side*side))+.5f)*cell-half};};
    auto clear=[&](Vec3 a,Vec3 b){const auto d=b-a;const float distance=std::sqrt(Environment::Dot(d,d));Environment::SurfaceHit hit;
        return distance<.001f||!Environment::SurfaceRaycastGrid(mesh,grid,a,d*(1/distance),distance+.01f,hit);};
    const float infinity=std::numeric_limits<float>::infinity();
    std::vector<float> distances(count,infinity);std::vector<int> parents(count,-1);
    using Entry=std::pair<float,int>;std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> pending;
    // Seed all centres surrounding the source, checking each link so a source next
    // to a thin wall cannot seed the room on its other side.
    for(int z=side/2-1;z<=side/2;++z)for(int y=side/2-1;y<=side/2;++y)for(int x=side/2-1;x<=side/2;++x){
        const int i=index(x,y,z);const auto p=position(i);if(!clear(volume.center,p))continue;
        distances[i]=std::sqrt(Environment::Dot(p-volume.center,p-volume.center));pending.push({distances[i],i});
    }
    while(!pending.empty()){
        const auto [distance,i]=pending.top();pending.pop();if(distance!=distances[i])continue;
        const int x=i%side,y=(i/side)%side,z=i/(side*side);const auto from=position(i);
        for(int dz=-1;dz<=1;++dz)for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
            if(!dx&&!dy&&!dz)continue;const int xx=x+dx,yy=y+dy,zz=z+dz;
            if(xx<0||xx>=side||yy<0||yy>=side||zz<0||zz>=side)continue;
            const int j=index(xx,yy,zz);const float next=distance+cell*std::sqrt(float(dx*dx+dy*dy+dz*dz));
            if(next>=distances[j]||next>half+cell)continue;
            if(!clear(from,position(j)))continue;distances[j]=next;parents[j]=i;pending.push({next,j});
        }
    }
    std::vector<WarpCell> result(count);
    for(int i=0;i<count;++i){
        const Vec3 p=position(i);Vec3 source=(p-volume.center)*(1/volume.spread)+volume.center;
        float coverage=std::isfinite(distances[i])?1.f:0.f;
        if(coverage&&!clear(volume.center,p)){
            // Contract path length instead of a straight vector through a wall.
            const float target=distances[i]/volume.spread;int current=i;
            while(parents[current]>=0&&distances[parents[current]]>target)current=parents[current];
            const int parent=parents[current];const Vec3 before=parent>=0?position(parent):volume.center;
            const float beforeDistance=parent>=0?distances[parent]:0;
            const float f=std::clamp((target-beforeDistance)/(distances[current]-beforeDistance),0.f,1.f);
            source=before+(position(current)-before)*f;
        }
        const auto local=(source-volume.center)*(1.f/640.f)+Vec3{.5f,.5f,.5f};
        result[i]={float(volume.slot*34)/542.f+local.x*(32.f/542.f),local.y,local.z,coverage};
    }
    return result;
}
inline void IdentityWarp(std::vector<WarpCell>& atlas){
    atlas.resize(size_t(WarpWidth)*WarpSide*WarpSide);
    for(int z=0;z<WarpSide;++z)for(int y=0;y<WarpSide;++y)for(int x=0;x<WarpWidth;++x){
        const int slot=x/WarpStride,local=x%WarpStride;
        atlas[(size_t(z)*WarpSide+y)*WarpWidth+x]={float(slot*34+(std::min(local,WarpSide-1)+.5f)*.5f)/542.f,(y+.5f)/WarpSide,(z+.5f)/WarpSide,1};
    }
}
inline void StoreWarp(std::vector<WarpCell>& atlas,unsigned slot,const std::vector<WarpCell>& cells){
    if(slot>=16||cells.size()!=WarpSide*WarpSide*WarpSide||atlas.size()!=size_t(WarpWidth)*WarpSide*WarpSide)return;
    for(int z=0;z<WarpSide;++z)for(int y=0;y<WarpSide;++y)for(int x=0;x<WarpStride&&int(slot*WarpStride)+x<WarpWidth;++x)
        atlas[(size_t(z)*WarpSide+y)*WarpWidth+slot*WarpStride+x]=cells[(z*WarpSide+y)*WarpSide+std::min(x,WarpSide-1)];
}
}
