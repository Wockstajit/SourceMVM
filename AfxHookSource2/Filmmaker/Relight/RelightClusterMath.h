#pragma once
#include "RelightMath.h"
#include <limits>

namespace Filmmaker::Relight {
// Native cb3[19..22] are integer bit patterns; cb3[23] contains real floats.
using ClusterUintRow = std::array<uint32_t, 4>;
using ClusterFloatRow = std::array<float, 4>;
struct ClusterList {
    uint32_t ones=0, tiles=0, slices=0, words=1, count=0, nearby=0;
};

struct ClusterGrid {
    uint32_t shift=5, tilesX=1, tilesY=1, sliceCount=64, size=0;
    float sliceScale=0;
    ClusterList lights, probes, reflections;
    uint32_t TileCount() const { return tilesX*tilesY; }
    ClusterUintRow ListRow(const ClusterList& list, unsigned mode) const {
        return {mode?list.slices:list.ones, mode==1?list.tiles:list.ones, list.words, 0};
    }
    ClusterUintRow TileConstants(unsigned mode) const {
        return mode==1?ClusterUintRow{shift,tilesX,0,0}:ClusterUintRow{31,0,0,0};
    }
    ClusterFloatRow SliceConstants(unsigned mode) const {
        return mode?ClusterFloatRow{sliceScale,float(sliceCount-1),0,0}:ClusterFloatRow{};
    }
};
inline ClusterGrid MakeClusterGrid(uint32_t width,uint32_t height,uint32_t lights,float farDistance,
                                   uint32_t probes=128,uint32_t reflections=128) {
    ClusterGrid g;
    g.tilesX=(std::clamp(width,1u,16384u)+31)/32;
    g.tilesY=(std::clamp(height,1u,16384u)+31)/32;
    g.sliceScale=float(g.sliceCount)/(std::isfinite(farDistance)?(std::max)(1.f,farDistance):1.f);
    g.lights.count=lights;g.probes.count=probes;g.reflections.count=reflections;
    for(auto* list:{&g.lights,&g.probes,&g.reflections}) {
        list->words=MaskWords(list->count);list->ones=g.size;g.size+=list->words;
    }
    for(auto* list:{&g.lights,&g.probes,&g.reflections}) {
        list->tiles=g.size;g.size+=g.TileCount()*list->words;
        list->slices=g.size;g.size+=g.sliceCount*list->words;
    }
    g.lights.nearby=g.size;g.size+=g.lights.words;
    return g;
}
inline std::vector<uint32_t> ClusterFallback(const ClusterGrid& g) {
    std::vector<uint32_t> out(g.size,0);
    for(const auto* list:{&g.lights,&g.probes,&g.reflections}) {
        const auto bits=FullMask(list->count,1);
        std::copy(bits.begin(),bits.end(),out.begin()+list->ones);
    }
    const auto everyLight=FullMask(g.lights.count,1);
    std::copy(everyLight.begin(),everyLight.end(),out.begin()+g.lights.nearby);
    std::fill(out.begin()+g.probes.tiles,out.begin()+g.probes.slices+g.sliceCount*g.probes.words,1u);
    // One map-wide live reflection capture: record 0 is live in every tile and slice.
    std::fill(out.begin()+g.reflections.tiles,out.begin()+g.reflections.slices+g.sliceCount*g.reflections.words,1u);
    return out;
}
// Mirror one occupancy value across each light mask row. Native addressing
// can then load its tile summary without division or a second resource binding.
inline std::vector<uint32_t> ClusterSummaryFallback(const ClusterGrid& g) {
    auto out=ClusterFallback(g);if(g.lights.words>32)return out;
    out.resize(size_t(g.size)*2,0);
    auto row=[&](uint32_t base){uint32_t occupied=0;
        for(uint32_t word=0;word<g.lights.words;++word)if(out[base+word])occupied|=1u<<word;
        for(uint32_t word=0;word<g.lights.words;++word)out[g.size+base+word]=occupied;
    };
    row(g.lights.ones);row(g.lights.nearby);
    for(uint32_t tile=0;tile<g.TileCount();++tile)row(g.lights.tiles+tile*g.lights.words);
    for(uint32_t slice=0;slice<g.sliceCount;++slice)row(g.lights.slices+slice*g.lights.words);
    return out;
}
// Exact integer addressing used by imad/ushr/ftou in the shipped material loop.
inline uint32_t ClusterTileRow(uint32_t pixelX,uint32_t pixelY,const ClusterUintRow& tiles,
                               const ClusterUintRow& list) {
    return ((pixelY>>tiles[0])*tiles[1]+(pixelX>>tiles[0]))*list[2]+list[1];
}
inline uint32_t ClusterSliceRow(float depth,const ClusterFloatRow& slices,const ClusterUintRow& list) {
    return uint32_t(std::clamp(depth*slices[0],0.f,slices[1]))*list[2]+list[0];
}
struct ClusterExtent { uint32_t x0=0,x1=0,y0=0,y1=0,z0=0,z1=0,allTiles=1,pad=0; };
static_assert(sizeof(ClusterExtent)==32,"compute extent stride");
using ClusterView = std::array<ClusterFloatRow,36>;
struct ClusterHull {std::array<Vec3,8> points{};unsigned count=8;};
// A directional cookie is finite when its verified orthographic projection bounds it.
// Ordinary sunlight has no cookie and must remain map-wide.
inline bool GlassClusterHull(const LightRecord& light,ClusterHull& hull) {
    if(light[35]!=0||!(light[43]>0)||light[12]!=0||light[13]!=0||light[14]!=0||light[15]!=1)return false;
    const Vec3 x{light[0],light[1],light[2]},y{light[4],light[5],light[6]},z{light[8],light[9],light[10]};
    const float xx=Dot(x,x),yy=Dot(y,y),zz=Dot(z,z);
    auto cross=[](Vec3 a,Vec3 b){return Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
    const auto yz=cross(y,z),zx=cross(z,x),xy=cross(x,y);
    const float determinant=Dot(x,yz);
    if(!std::isfinite(xx+yy+zz)||xx<1e-14f||yy<1e-14f||zz<1e-14f||
       !std::isfinite(light[3]+light[7]+light[11])||
       std::abs(determinant)<1e-4f*std::sqrt(xx*yy*zz))return false;
    const auto ix=yz*(1/determinant),iy=zx*(1/determinant),iz=xy*(1/determinant);
    hull.count=8;
    for(unsigned i=0;i<8;++i){
        const float u=((i&1)?1.f:-1.f)*(1+std::sqrt(xx));
        const float v=((i&2)?1.f:-1.f)*(1+std::sqrt(yy));
        const float w=(i&4)?1+std::sqrt(zz):-std::sqrt(zz);
        hull.points[i]=ix*(u-light[3])+iy*(v-light[7])+iz*(w-light[11]);
        if(!std::isfinite(hull.points[i].x)||!std::isfinite(hull.points[i].y)||!std::isfinite(hull.points[i].z))return false;
    }
    return true;
}
inline ClusterHull LightClusterHull(const LightRecord& light,float radius) {
    const Vec3 c{light[32],light[33],light[34]};ClusterHull hull;
    for(unsigned i=0;i<8;++i)hull.points[i]=c+Vec3{(i&1)?radius:-radius,(i&2)?radius:-radius,(i&4)?radius:-radius};
    const Vec3 x{light[0],light[1],light[2]},y{light[4],light[5],light[6]},f{light[12],light[13],light[14]};
    const float xx=Dot(x,x),yy=Dot(y,y),ff=Dot(f,f);
    // A verified perspective face contributes only inside its projection cone.
    // Bound that cone rather than repeating the full bulb sphere for all six faces.
    // Unknown/orthographic matrices keep the conservative sphere box.
    if(!(light[35]>0)||!std::isfinite(xx+yy+ff)||xx<1e-4f||yy<1e-4f||std::abs(ff-1)>1e-4f||
       std::abs(Dot(x,y))>1e-4f*std::sqrt(xx*yy)||std::abs(Dot(x,f))>1e-4f*std::sqrt(xx)||
       std::abs(Dot(y,f))>1e-4f*std::sqrt(yy)||std::abs(Dot(x,c)+light[3])>.25f||
       std::abs(Dot(y,c)+light[7])>.25f||std::abs(Dot(f,c)+light[15])>.25f)return hull;
    const float pad=1+radius*.001f;
    const Vec3 side=x*(1/xx),up=y*(1/yy),farCenter=c+f*(radius+pad);
    hull.count=5;hull.points[0]=c-f*pad;
    for(unsigned i=0;i<4;++i)hull.points[i+1]=farCenter+side*((i&1)?radius+3*pad:-radius-3*pad)+up*((i&2)?radius+3*pad:-radius-3*pad);
    return hull;
}
// Visible reach. A light's authored range is where its smooth window reaches zero, but long
// before that its inverse-square falloff has made it too dim to change the picture. Clusters
// beyond that point would only loop over it (and run its shadow filter) for nothing: at a busy
// view the authored spheres covered about seven times the volume the lights can visibly light
// (de_ancient, 2026-10-18). The cutoff is a radiance: one display step near black (the first
// sRGB code, linear 0.0003) on a mid-grey surface at the applied exposure, per `steps`.
inline constexpr float VisibleStepLinear=.0003f;
inline constexpr float VisibleReferenceAlbedo=.5f;
inline float VisibleRadianceCutoff(float steps,float exposure){
    if(!(steps>0)||!(exposure>0)||!std::isfinite(steps*exposure))return 0;
    const float cutoff=steps*VisibleStepLinear*3.14159265f/(VisibleReferenceAlbedo*exposure);
    // Sixteenth-stop steps: an easing exposure must not rebuild the clusters every frame.
    return std::exp2(std::round(std::log2(cutoff)*16)/16);
}
// The distance at which this record's light, at its brightest (no cone, cookie or shadow),
// falls to `cutoff`: radiance * soft / max(soft, d^2) * smoothstep(saturate(x + y d)), rows
// 8.w, 9 and 10 exactly as the shipped loop evaluates them. Never beyond `radius`; a zero
// cutoff, an unproven record or a directional one keeps `radius`. Mirrors VisibleReach (HLSL).
// A shadowed record's face depth (its projection, rows 0-3) at `distance` along the face axis:
// the lamp kernel skips its shadow lookup for receivers deeper than this (EnvironmentNativeShadow
// Math.h). Record slot ShadowReachSlot (row 22.w, which no shipped material program reads); 0
// (every record's default) keeps every lookup, as does a projection that is not perspective.
inline constexpr unsigned ShadowReachSlot=91;
inline float ShadowReachDepth(const LightRecord& record,float distance){
    // Perspective: depth = (row2.p)/(row3.p) = a + b/w with w the face-axis distance. A cube
    // record (ToCubeShadowRecord) carries its faces' a and b in row 22.
    unsigned axis=0;for(unsigned j=1;j<3;++j)if(std::abs(record[12+j])>std::abs(record[12+axis]))axis=j;
    const bool cube=record[87]==CubeShadowMarker;
    if(!(distance>0)||(!cube&&!(std::abs(record[12+axis])>1e-6f)))return 0;
    const float a=cube?record[89]:record[8+axis]/record[12+axis],b=cube?record[90]:record[11]-a*record[15];
    if(!std::isfinite(a)||!std::isfinite(b))return 0;
    const float depth=a+b/distance;
    // Reach inside the near plane (a light too dim to matter anywhere): skip every receiver.
    return depth<1?(std::max)(depth,1e-7f):0;
}
inline float LightVisibleReach(const LightRecord& light,float radius,float cutoff){
    if(!(cutoff>0)||!(radius>0)||!std::isfinite(radius)||!(light[35]>0))return radius;
    const float peak=(std::max)(light[40],(std::max)(light[41],light[42]));
    const float soft=light[35];
    if(!(peak>0)||!std::isfinite(peak*soft))return radius;
    float low=0,high=radius;
    for(unsigned i=0;i<24;++i){
        const float d=(low+high)*.5f;
        const float t=std::clamp(light[36]+light[37]*d,0.f,1.f);
        const float c=peak*soft/(std::max)(soft,d*d)*(t*t*(3-2*t));
        if(c>cutoff)low=d;else high=d;
    }
    return high;
}
// First-person raster coordinates use their own projection, so their pixels cannot
// index screen tiles. They read one list of the lights that can reach a sphere
// around the eye instead of every record. The reach covers hands, weapons and
// held items; beams, cookies and lights without a proven finite range are always listed.
inline constexpr float FirstPersonLightReach=96.f;
inline bool FirstPersonLight(const LightRecord& light,const Vec3& camera,float reach=FirstPersonLightReach,float cutoff=0) {
    ClusterHull cookie;
    if(light[80]>0||GlassClusterHull(light,cookie))return true;
    if(!(light[37]<0)||!std::isfinite(light[36])||!std::isfinite(light[37]))return true;
    float radius=-light[36]/light[37];
    if(!(radius>=0)||!std::isfinite(radius))return true;
    radius=LightVisibleReach(light,radius,cutoff);
    const Vec3 delta=Vec3{light[32],light[33],light[34]}-camera;
    const float distance=Dot(delta,delta);
    if(!std::isfinite(distance))return true;
    return distance<=(radius+reach)*(radius+reach);
}
// Exact projected sphere tangents, intersected with the existing conservative cone/box.
// The eye-plane crossing case retains the clipped hull; dividing by a nearly zero tangent
// denominator there would magnify roundoff. One extra tile remains on every edge.
inline void TightenSphereExtent(Vec3 c,float radius,const ClusterGrid& g,const ClusterView& view,
                                float width,float height,ClusterExtent& e){
    const Vec3 b{view[12][3],view[13][3],view[14][3]};
    const float w=Dot(b,c)+view[15][3],bb=Dot(b,b),rr=radius*radius;
    if(!(w>radius*std::sqrt(bb)+(std::max)(1e-4f,std::abs(w)*1e-4f)))return;
    const float denominator=w*w-rr*bb;if(!(denominator>0)||!std::isfinite(denominator))return;
    float lo[2]{},hi[2]{};
    for(unsigned axis=0;axis<2;++axis){
        const Vec3 a{view[12][axis],view[13][axis],view[14][axis]};
        const float x=Dot(a,c)+view[15][axis],aa=Dot(a,a),ab=Dot(a,b);
        const Vec3 tangent=a*w-b*x;
        const float discriminant=rr*(Dot(tangent,tangent)-rr*(aa*bb-ab*ab));
        if(!std::isfinite(discriminant)||discriminant<0)return;
        const float centre=(x*w-rr*ab)/denominator,half=std::sqrt(discriminant)/denominator;
        lo[axis]=centre-half;hi[axis]=centre+half;
        if(!std::isfinite(lo[axis])||!std::isfinite(hi[axis]))return;
        const float pad=1e-4f*(1+(std::max)(std::abs(lo[axis]),std::abs(hi[axis])));
        lo[axis]-=pad;hi[axis]+=pad;
    }
    auto low=[&](float pixel){const auto tile=uint32_t(pixel)>>g.shift;return tile?tile-1:0;};
    auto high=[&](float pixel,uint32_t limit){return (std::min)((uint32_t(pixel)>>g.shift)+1,limit-1);};
    e.x0=(std::max)(e.x0,low(std::clamp((lo[0]+1)*width*.5f,0.f,width-1)));
    e.x1=(std::min)(e.x1,high(std::clamp((hi[0]+1)*width*.5f,0.f,width-1),g.tilesX));
    e.y0=(std::max)(e.y0,low(std::clamp((1-hi[1])*height*.5f,0.f,height-1)));
    e.y1=(std::min)(e.y1,high(std::clamp((1-lo[1])*height*.5f,0.f,height-1),g.tilesY));
}
inline ClusterExtent LightClusterExtent(const LightRecord& light,const ClusterGrid& g,
                                       const ClusterView& view,float width,float height,float cutoff=0,bool tightSphere=true) {
    ClusterExtent e{0,g.tilesX-1,0,g.tilesY-1,0,g.sliceCount-1,1,0};
    ClusterHull hull;const bool beam=GlassClusterHull(light,hull);float radius=0;
    if(!beam){
        // Row 8.w softens inverse-square falloff; row 9 proves a finite cull sphere.
        if(!(light[37]<0)||!std::isfinite(light[36])||!std::isfinite(light[37]))return e;
        radius=-light[36]/light[37];if(!(radius>=0)||!std::isfinite(radius))return e;
        radius=LightVisibleReach(light,radius,cutoff);
        hull=LightClusterHull(light,radius);
    }
    const Vec3 c{light[32],light[33],light[34]};
    auto project=[&](Vec3 p,unsigned component) {
        return p.x*view[12][component]+p.y*view[13][component]+p.z*view[14][component]+view[15][component];
    };
    // Both reconstructed-position and D3D11 raster SV_Position.w use clip W.
    // RelightClusterGpuTests renders a known W to verify the raster convention.
    {
        const float w=project(c,3);
        const float extent=radius*std::sqrt(view[12][3]*view[12][3]+view[13][3]*view[13][3]+view[14][3]*view[14][3]);
        float nearW=w-extent,farW=w+extent;
        if(beam||hull.count==5){nearW=std::numeric_limits<float>::infinity();farW=-nearW;
            for(unsigned i=0;i<hull.count;++i){const float v=project(hull.points[i],3);nearW=(std::min)(nearW,v);farW=(std::max)(farW,v);}}
        const float lo=nearW*g.sliceScale,hi=farW*g.sliceScale;
        if(std::isfinite(lo)&&std::isfinite(hi)) {
            // One slice absorbs CPU/GPU arithmetic differences at exact boundaries.
            e.z0=uint32_t(std::clamp(std::floor(lo)-1,0.f,float(g.sliceCount-1)));
            e.z1=uint32_t(std::clamp(std::floor(hi)+1,0.f,float(g.sliceCount-1)));
        }
    }
    float lowX=width,lowY=height,highX=0,highY=0;
    std::array<Vec3,8> clips{};bool visible=false;
    auto include=[&](Vec3 q){
        const float sx=std::clamp((q.x/q.z+1)*width*.5f,0.f,width-1);
        const float sy=std::clamp((1-q.y/q.z)*height*.5f,0.f,height-1);
        lowX=(std::min)(lowX,sx);highX=(std::max)(highX,sx);
        lowY=(std::min)(lowY,sy);highY=(std::max)(highY,sy);visible=true;
    };
    constexpr float nearW=1e-4f;
    for(unsigned i=0;i<hull.count;++i){
        const auto p=hull.points[i];clips[i]={project(p,0),project(p,1),project(p,3)};
        const auto q=clips[i];
        if(!std::isfinite(q.x)||!std::isfinite(q.y)||!std::isfinite(q.z))return e;
        if(q.z>=nearW)include(q);
    }
    // Clip every vertex pair, including diagonals. This contains every clipped
    // hull edge without relying on the box/cone's topology and is conservative.
    // A face crossing the eye no longer forces unrelated tiles to shade it.
    for(unsigned i=0;i<hull.count;++i)for(unsigned j=i+1;j<hull.count;++j){
        const auto a=clips[i],b=clips[j];
        if((a.z>=nearW)==(b.z>=nearW))continue;
        const float t=(nearW-a.z)/(b.z-a.z);
        include({a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,nearW});
    }
    if(!visible){e.x0=g.tilesX;e.x1=0;e.allTiles=0;return e;}
    auto low=[&](float pixel) {const auto tile=uint32_t(pixel)>>g.shift;return tile?tile-1:0;};
    e.x0=low(lowX);e.y0=low(lowY);
    e.x1=(std::min)((uint32_t(highX)>>g.shift)+1,g.tilesX-1);
    e.y1=(std::min)((uint32_t(highY)>>g.shift)+1,g.tilesY-1);e.allTiles=0;
    if(!beam&&tightSphere)TightenSphereExtent(c,radius,g,view,width,height,e);
    return e;
}
}
