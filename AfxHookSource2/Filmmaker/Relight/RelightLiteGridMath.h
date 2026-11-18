#pragma once
// Lite lighting's bounce grid.
//
// Lite keeps no traced bounce volume. One map-wide fill stood in for it, and a room lit by a lamp
// went black wherever the lamp did not reach directly. This grid is the bounce worked out once,
// on worker threads, from the map's own triangles: a coarse grid of ambient cubes over the
// playable map that the lit programs read through their plain trilinear probe query. Nothing is traced
// per frame, so a frame costs what the map-wide fill cost. (The rays go through the map's render
// triangles where it has them: the collision also holds invisible ceilings and clips, which shut
// the sky out of open ground, and it carries no surface colour.)
//
// Each probe looks along the same fixed directions. A direction that leaves the map sees the sky.
// One that hits a surface sees what that surface sends back: the lamps and the sun that reach it
// (one shadow ray each), and then the light the grid itself holds there (a second bounce, read
// from the grid, no rays). The three sources are kept apart so that each is redone only when its
// own input changes:
//   hits and sky   the map
//   lamps          the map's lamps (static once loaded)
//   sun            the sun's direction
// Colours and strengths are applied when the layers are put together (LiteGridCompose).
//
// Engine-free; Automation/tests/relight-lite-grid-math-tests.cpp checks it on small scenes.
#include "RelightProbeMath.h"
#include "../Environment/SurfaceRayGridMath.h"
#include "../MapData/MapCasterLimits.h"
#include <cstdint>
#include <cstring>

namespace Filmmaker::Relight {
// Probe spacing. A doorway is about 60 units wide and a storey about 130 tall, so 64 keeps rooms
// and floors apart; the caps keep a very large map to a few megabytes by widening its cells.
inline constexpr float LiteGridSpacing=64.f;
inline constexpr unsigned LiteGridMaxWide=96,LiteGridMaxTall=24;
inline constexpr unsigned LiteGridDirections=24;
// A ray that finds nothing this far away sees the sky, as a traced probe's ray does (eight probe
// spacings, 320 units, in ProbeRay). Counting surfaces further off made shade greyer and darker
// than the traced picture, which a street's blue shade is judged against.
inline constexpr float LiteGridRayRange=320.f;
// How far a sun shadow ray looks for something in the way.
inline constexpr float LiteGridSunRange=16384.f;
// How far along its normal a surface reads the grid, in cells. A surface between two probes reads
// both, and one of them can be behind the wall it is on: lamp light from inside a church came
// through to its front. A cell out, the probes around the read are on the surface's own side.
inline constexpr float LiteGridNormalBias=.9f;

struct LiteGridLayout {
    Vec3 low{},high{};
    unsigned nx=1,ny=1,nz=1;
    size_t Cells() const {return size_t(nx)*ny*nz;}
    size_t Index(unsigned x,unsigned y,unsigned z) const {return (size_t(z)*ny+y)*nx+x;}
    Vec3 Cell() const {return {(high.x-low.x)/nx,(high.y-low.y)/ny,(high.z-low.z)/nz};}
    Vec3 Centre(unsigned x,unsigned y,unsigned z) const {
        const auto c=Cell();return {low.x+(x+.5f)*c.x,low.y+(y+.5f)*c.y,low.z+(z+.5f)*c.z};
    }
    // The cell whose probe is nearest to a point (clamped to the grid).
    size_t Nearest(Vec3 p) const {
        const auto c=Cell();
        auto at=[](float v,float lo,float size,unsigned n){return unsigned(std::clamp(std::floor((v-lo)/size),0.f,float(n-1)));};
        return Index(at(p.x,low.x,c.x,nx),at(p.y,low.y,c.y,ny),at(p.z,low.z,c.z,nz));
    }
    bool operator==(const LiteGridLayout& o) const {
        return nx==o.nx&&ny==o.ny&&nz==o.nz&&low.x==o.low.x&&low.y==o.low.y&&low.z==o.low.z&&
            high.x==o.high.x&&high.y==o.high.y&&high.z==o.high.z;
    }
};
inline LiteGridLayout MakeLiteGridLayout(Vec3 low,Vec3 high) {
    LiteGridLayout g;g.low=low;
    // PackProbe gives every axis at least one unit of extent; the probes must sit where it puts them.
    g.high={(std::max)(high.x,low.x+1),(std::max)(high.y,low.y+1),(std::max)(high.z,low.z+1)};
    auto steps=[](float extent,unsigned limit){
        return unsigned(std::clamp(std::ceil(extent/LiteGridSpacing),1.f,float(limit)));};
    g.nx=steps(g.high.x-g.low.x,LiteGridMaxWide);g.ny=steps(g.high.y-g.low.y,LiteGridMaxWide);
    g.nz=steps(g.high.z-g.low.z,LiteGridMaxTall);
    return g;
}

// The directions every probe looks along: an even spiral over the sphere. Every probe uses the
// same set, so neighbouring probes differ only by what they see and the grid has no noise.
inline Vec3 LiteGridDirection(unsigned i) {
    const float z=1-(2*i+1.f)/LiteGridDirections,r=std::sqrt((std::max)(0.f,1-z*z)),turn=i*2.39996323f;
    return {r*std::cos(turn),r*std::sin(turn),z};
}
// An ambient-cube band (+X +Y +Z -X -Y -Z) holds the cosine-weighted mean of the radiance arriving
// on its side: what a Lambert surface facing that axis receives, over pi.
inline float LiteBandWeight(unsigned band,Vec3 d) {
    const float c=band%3==0?d.x:band%3==1?d.y:d.z;
    return (std::max)(0.f,band<3?c:-c);
}
inline std::array<float,6> LiteBandNorms() {
    std::array<float,6> norms{};
    for(unsigned i=0;i<LiteGridDirections;++i)for(unsigned b=0;b<6;++b)norms[b]+=LiteBandWeight(b,LiteGridDirection(i));
    return norms;
}
// The sky a direction sees: the traced volume's own horizon falloff (SkyRadiance, RelightProbeGiShaders.h).
inline float LiteSkyFalloff(Vec3 d) {return .3f+.7f*std::clamp(d.z*1.5f+.5f,0.f,1.f);}

// Rays against the collision triangles through the shared uniform grid (SurfaceRayGridMath.h).
// Unlike SurfaceRaycastGrid this keeps the triangle's own facing, which is how a probe inside a
// wall is recognised, and it can stop at the first hit, which is all a shadow ray needs.
struct LiteRayMesh {
    const std::vector<Vec3>* positions=nullptr;
    const std::vector<uint32_t>* indices=nullptr;
    const Environment::SurfaceRayGrid* grid=nullptr;
    // One linear R8G8B8 albedo per triangle (VisualCasters::rigidAlbedo), or null for none.
    const std::vector<uint32_t>* albedo=nullptr;
};
using LiteRayHit=Environment::SurfaceGridRayHit;
// `direction` must be unit length. With `hit` null the first triangle found ends the ray.
inline bool LiteRay(const LiteRayMesh& mesh,Vec3 origin,Vec3 direction,float range,LiteRayHit* hit) {
    return Environment::SurfaceGridRay(*mesh.positions,*mesh.indices,*mesh.grid,origin,direction,range,hit);
}

// A lamp as the bounce needs it: where it is, how bright, how it falls off and where it aims.
// Taken from the record the lit programs shade with (ToRecord), so the two cannot disagree.
struct LiteLamp {
    Vec3 position{},radiance{},forward{};
    float soft=1,windowBias=1,windowScale=0,coneBias=1,coneScale=0;
    float reach=0; // nothing further than this from `position` is lit: the bins' radius
    // Shaped fixtures (light_rect, light_barn: most of a map's fill lights) light only inside
    // their projection, from an eye behind the opening or, with no eye, along one direction.
    bool fixture=false;
    std::array<float,16> projection{};
    Vec3 origin{},aim{};
    float nearFade=0,farFade=0,edgeX=1,edgeY=1;
};
inline bool MakeLiteLamp(const Light& light,float intensity,LiteLamp& out) {
    // Fire, muzzle flashes and other effect lights come and go; their bounce is not worth a redo.
    if(!light.enabled||light.transient)return false;
    const auto r=ToRecord(light,intensity);
    out=LiteLamp{};
    out.position=light.position;out.soft=r[35];out.windowBias=r[36];out.windowScale=r[37];
    out.radiance={r[40],r[41],r[42]};
    const float x=r[44],y=r[45],z=r[46],w=r[47];
    out.forward={1-2*(y*y+z*z),2*(x*y+w*z),2*(x*z-w*y)};
    out.coneBias=r[48];out.coneScale=r[49];
    if(r[80]>0){
        out.fixture=true;std::copy(r.begin(),r.begin()+16,out.projection.begin());
        out.origin={r[32],r[33],r[34]};
        Vec3 right,up;LightBasis(light.angles,out.aim,right,up);
        out.nearFade=r[38];out.farFade=r[39];out.edgeX=r[56];out.edgeY=r[57];
        // The projection widens with distance; three ranges holds any opening used in a map.
        out.reach=3*(std::max)(1.f,light.range)+light.sizeX+light.sizeY;
    }else{
        out.position={r[32],r[33],r[34]};
        out.reach=out.windowScale<0?-out.windowBias/out.windowScale:0;
        if(!(out.soft>0))return false;
    }
    return out.reach>0&&std::isfinite(out.reach)&&
        (out.radiance.x>0||out.radiance.y>0||out.radiance.z>0)&&
        std::isfinite(out.radiance.x+out.radiance.y+out.radiance.z);
}
inline float LiteSmooth(float t) {t=std::clamp(t,0.f,1.f);return t*t*(3-2*t);}
// Unshadowed light a surface at `point` facing `normal` sends back from this lamp, per unit
// albedo. The falloff, range window, cone and projection are the lit programs' own (InjectSurface
// mirrors the same rows); rounded openings and cookies are not applied. `toward` and `distance`
// aim the shadow ray at the lamp, or at a fixture's opening.
inline Vec3 LiteLampRadiance(const LiteLamp& lamp,Vec3 point,Vec3 normal,Vec3& toward,float& distance) {
    float fade=1;
    if(lamp.fixture){
        const auto& m=lamp.projection;
        auto row=[&](unsigned i){return m[i*4]*point.x+m[i*4+1]*point.y+m[i*4+2]*point.z+m[i*4+3];};
        const float cx=row(0),cy=row(1),cz=row(2),cw=row(3);
        if(!(cw>0)||std::abs(cx)>cw||std::abs(cy)>cw||cz<0||cz>cw)return {};
        const float nx=cx/cw,ny=cy/cw,nz=cz/cw;
        if(lamp.nearFade>0)fade*=LiteSmooth(nz*lamp.nearFade);
        if(lamp.farFade>0)fade*=LiteSmooth((1-nz)*lamp.farFade);
        fade*=LiteSmooth((1-std::abs(nx))/(std::max)(1-lamp.edgeX,1e-6f))*LiteSmooth((1-std::abs(ny))/(std::max)(1-lamp.edgeY,1e-6f));
        if(lamp.soft>0){
            const Vec3 delta=lamp.origin-point;const float d2=(std::max)(Dot(delta,delta),1e-5f),d=std::sqrt(d2);
            toward=delta*(1/d);fade*=lamp.soft/(std::max)(lamp.soft,d2);
            // The eye is behind the opening by the near distance; the ray ends at the opening.
            const float opening=std::sqrt(lamp.soft);
            distance=d*(1-opening/(std::max)(cw,opening));
        }else{
            toward=lamp.origin;distance=(std::max)(0.f,Dot(point-lamp.position,lamp.aim));
        }
    }else{
        const Vec3 delta=lamp.position-point;
        const float d2=(std::max)(Dot(delta,delta),1e-5f);
        distance=std::sqrt(d2);toward=delta*(1/distance);
        fade=std::clamp(lamp.windowBias+lamp.windowScale*distance,0.f,1.f);
        if(!(fade>0))return {};
        fade=fade*fade*(3-2*fade)*lamp.soft/(std::max)(lamp.soft,d2);
        if(lamp.coneScale!=0)fade*=std::clamp(lamp.coneBias+lamp.coneScale*Dot(lamp.forward,toward),0.f,1.f);
    }
    return lamp.radiance*(fade*(std::max)(0.f,Dot(normal,toward)));
}
// Lamps by the box their range can reach, so a surface tests only the ones near it.
struct LiteLampBins {
    Vec3 low{};float cell=256;unsigned nx=1,ny=1,nz=1;
    std::vector<uint32_t> start,items;
    const uint32_t* Begin(Vec3 p,size_t& count) const {
        auto at=[&](float v,float lo,unsigned n){return unsigned(std::clamp(std::floor((v-lo)/cell),0.f,float(n-1)));};
        const size_t i=(size_t(at(p.z,low.z,nz))*ny+at(p.y,low.y,ny))*nx+at(p.x,low.x,nx);
        count=start[i+1]-start[i];return items.data()+start[i];
    }
};
inline LiteLampBins BinLiteLamps(const std::vector<LiteLamp>& lamps,Vec3 low,Vec3 high) {
    LiteLampBins bins;bins.low=low;
    auto steps=[&](float extent){return unsigned(std::clamp(std::ceil(extent/bins.cell),1.f,64.f));};
    bins.nx=steps(high.x-low.x);bins.ny=steps(high.y-low.y);bins.nz=steps(high.z-low.z);
    // Past the capped grid the last bin holds everything further, so nothing is lost on a huge map.
    auto range=[&](float lo,float hi,float origin,unsigned n,unsigned& a,unsigned& b){
        a=unsigned(std::clamp(std::floor((lo-origin)/bins.cell),0.f,float(n-1)));
        b=unsigned(std::clamp(std::floor((hi-origin)/bins.cell),0.f,float(n-1)));};
    const size_t cells=size_t(bins.nx)*bins.ny*bins.nz;
    std::vector<uint32_t> counts(cells+1,0);
    for(int pass=0;pass<2;++pass){
        for(uint32_t i=0;i<lamps.size();++i){
            const auto& l=lamps[i];unsigned x0,x1,y0,y1,z0,z1;
            range(l.position.x-l.reach,l.position.x+l.reach,low.x,bins.nx,x0,x1);
            range(l.position.y-l.reach,l.position.y+l.reach,low.y,bins.ny,y0,y1);
            range(l.position.z-l.reach,l.position.z+l.reach,low.z,bins.nz,z0,z1);
            for(unsigned z=z0;z<=z1;++z)for(unsigned y=y0;y<=y1;++y)for(unsigned x=x0;x<=x1;++x){
                const size_t c=(size_t(z)*bins.ny+y)*bins.nx+x;
                if(pass==0)++counts[c];else bins.items[counts[c]++]=i;
            }
        }
        if(pass==0){
            bins.start.assign(cells+1,0);uint32_t running=0;
            for(size_t c=0;c<cells;++c){bins.start[c]=running;running+=counts[c];}
            bins.start[cells]=running;bins.items.assign(running,0);
            std::copy(bins.start.begin(),bins.start.end(),counts.begin());
        }
    }
    return bins;
}

// What one probe direction found. Kept so the lamps and the sun are redone without the first ray.
// Eight bytes: a 96 by 96 by 24 grid keeps 5.3 million of them.
struct LiteGridHit {
    uint16_t distance=0; // eighths of a unit
    int8_t normal[3]{};
    uint8_t kind=0;      // 0 sky, 1 a surface facing the probe, 2 the back of a surface, 3 cloth
    uint16_t albedo=0;   // linear R5G6B5, or LiteHitNoAlbedo
    float Distance() const {return distance*(1/8.f);}
    Vec3 Normal() const {return Vec3{float(normal[0]),float(normal[1]),float(normal[2])}*(1/127.f);}
    Vec3 Albedo(Vec3 fallback) const;
};
static_assert(sizeof(LiteGridHit)==8,"probe hit record size");
// Cloth (a triangle the map data marked MapData::kRigidTransmissive: a tarp, a fabric awning) is a
// surface facing the probe from either side (a hit's normal is always toward the probe), and it
// also sends on the light that falls on its far face, through its own colour (GiClothPass). The
// traced probes hold cloth the same way (VOXEL_CLOTH in RelightProbeGiShaders.h).
inline constexpr uint8_t LiteHitSky=0,LiteHitFront=1,LiteHitBack=2,LiteHitCloth=3;
inline bool LiteHitLit(uint8_t kind) {return kind==LiteHitFront||kind==LiteHitCloth;}
// Pure white with the lowest green bit clear is kept as "no colour known": the fallback albedo.
inline constexpr uint16_t LiteHitNoAlbedo=0xffdf;
inline uint16_t PackLiteAlbedo(uint32_t rgb8) {
    const uint16_t packed=uint16_t(((rgb8&255)>>3)|((((rgb8>>8)&255)>>2)<<5)|((((rgb8>>16)&255)>>3)<<11));
    return packed==LiteHitNoAlbedo?uint16_t(0xffff):packed;
}
inline Vec3 LiteGridHit::Albedo(Vec3 fallback) const {
    if(albedo==LiteHitNoAlbedo)return fallback;
    return {float(albedo&31)/31,float((albedo>>5)&63)/63,float(albedo>>11)/31};
}
// How far off a surface its shadow rays and its grid lookup start, so they leave the surface.
inline constexpr float LiteGridSurfaceOffset=2.f;
// A lamp's shadow rays end this far from it, clear of the fixture it sits in.
inline constexpr float LiteLampClearance=40.f;

struct LiteGridBake {
    LiteGridLayout layout;
    std::vector<LiteGridHit> hits; // cells * LiteGridDirections
    std::vector<float> sky;        // cells * 6: per-band share of the sky
    std::vector<Vec3> sun;         // cells * 6: per-band albedo of what is seen, by how squarely the sun is on it
    std::vector<Vec3> lamps;       // cells * 6: per-band lamp light the seen surfaces send back
    std::vector<uint8_t> inside;   // 1: the probe mostly sees the backs of surfaces (it is in a wall)
    void Resize(const LiteGridLayout& l) {
        layout=l;const size_t n=l.Cells();
        hits.assign(n*LiteGridDirections,{});sky.assign(n*6,0);sun.assign(n*6,{});lamps.assign(n*6,{});inside.assign(n,0);
    }
};
inline Vec3 LiteGridProbe(const LiteGridLayout& layout,size_t cell) {
    const unsigned x=unsigned(cell%layout.nx),y=unsigned(cell/layout.nx%layout.ny),z=unsigned(cell/(size_t(layout.nx)*layout.ny));
    return layout.Centre(x,y,z);
}
// First rays: what each probe direction sees, and the sky's share of each band.
inline void LiteGridTrace(const LiteRayMesh& mesh,LiteGridBake& bake,size_t first,size_t last) {
    const auto norms=LiteBandNorms();
    for(size_t cell=first;cell<last;++cell){
        const Vec3 p=LiteGridProbe(bake.layout,cell);unsigned backs=0;
        float sky[6]{};
        for(unsigned i=0;i<LiteGridDirections;++i){
            const Vec3 d=LiteGridDirection(i);LiteRayHit hit;auto& out=bake.hits[cell*LiteGridDirections+i];
            out={};
            if(!LiteRay(mesh,p,d,LiteGridRayRange,&hit)){
                const float falloff=LiteSkyFalloff(d);
                for(unsigned b=0;b<6;++b)sky[b]+=LiteBandWeight(b,d)*falloff;
                continue;
            }
            const uint32_t word=mesh.albedo&&hit.triangle<mesh.albedo->size()?(*mesh.albedo)[hit.triangle]:0u;
            const bool cloth=(word&MapData::kRigidTransmissive)!=0;
            out.distance=uint16_t(std::lround(std::clamp(hit.distance*8,0.f,65535.f)));
            // Cloth seen from behind is no back: it is lit from there too (LiteHitCloth).
            out.kind=cloth?LiteHitCloth:hit.back?LiteHitBack:LiteHitFront;backs+=hit.back&&!cloth;
            out.albedo=mesh.albedo&&hit.triangle<mesh.albedo->size()?PackLiteAlbedo(word&0xffffffu):LiteHitNoAlbedo;
            const float n[3]{hit.normal.x,hit.normal.y,hit.normal.z};
            for(unsigned k=0;k<3;++k)out.normal[k]=int8_t(std::lround(std::clamp(n[k],-1.f,1.f)*127));
        }
        for(unsigned b=0;b<6;++b)bake.sky[cell*6+b]=sky[b]/norms[b];
        // A probe in the open sees a back now and then (a thin prop, a one-sided brush); one inside
        // a wall sees little else.
        bake.inside[cell]=backs*4>LiteGridDirections;
    }
}
inline Vec3 Tint(Vec3 light,Vec3 albedo) {return {light.x*albedo.x,light.y*albedo.y,light.z*albedo.z};}
// Sun: per band, how much of what the probe sees is in the sun, how squarely, and its colour.
// `fallback` is the albedo of a surface whose colour is not known.
inline void LiteGridSun(const LiteRayMesh& mesh,LiteGridBake& bake,Vec3 towardSun,Vec3 fallback,size_t first,size_t last) {
    const auto norms=LiteBandNorms();
    for(size_t cell=first;cell<last;++cell){
        const Vec3 p=LiteGridProbe(bake.layout,cell);Vec3 sun[6]{};
        for(unsigned i=0;i<LiteGridDirections;++i){
            const auto& hit=bake.hits[cell*LiteGridDirections+i];
            if(!LiteHitLit(hit.kind))continue;
            const Vec3 d=LiteGridDirection(i);Vec3 n=hit.Normal();
            float facing=Dot(n,towardSun),share=1;
            // The sun on the far face of cloth: the shadow ray leaves from that face.
            if(hit.kind==LiteHitCloth&&facing<0){facing=-facing;n=n*-1.f;share=GiClothPass;}
            if(!(facing>0))continue;
            const Vec3 q=p+d*hit.Distance()+n*LiteGridSurfaceOffset;
            if(LiteRay(mesh,q,towardSun,LiteGridSunRange,nullptr))continue;
            const Vec3 sent=hit.Albedo(fallback)*(facing*share);
            for(unsigned b=0;b<6;++b)sun[b]=sun[b]+sent*LiteBandWeight(b,d);
        }
        for(unsigned b=0;b<6;++b)bake.sun[cell*6+b]=sun[b]*(1/norms[b]);
    }
}
// Lamps: per band, the lamp light the seen surfaces send back, one shadow ray per lamp in reach.
inline void LiteGridLamps(const LiteRayMesh& mesh,LiteGridBake& bake,const std::vector<LiteLamp>& lamps,
                          const LiteLampBins& bins,Vec3 fallback,size_t first,size_t last) {
    const auto norms=LiteBandNorms();
    for(size_t cell=first;cell<last;++cell){
        const Vec3 p=LiteGridProbe(bake.layout,cell);Vec3 sum[6]{};
        for(unsigned i=0;i<LiteGridDirections&&!lamps.empty();++i){
            const auto& hit=bake.hits[cell*LiteGridDirections+i];
            if(!LiteHitLit(hit.kind))continue;
            const Vec3 d=LiteGridDirection(i);Vec3 light{};
            // Cloth takes the lamps on its far face as well.
            for(unsigned face=0;face<(hit.kind==LiteHitCloth?2u:1u);++face){
                const Vec3 n=hit.Normal()*(face?-1.f:1.f);const float share=face?GiClothPass:1.f;
                const Vec3 q=p+d*hit.Distance()+n*LiteGridSurfaceOffset;
                size_t count=0;const auto* listed=bins.Begin(q,count);
                for(size_t k=0;k<count;++k){
                    const auto& lamp=lamps[listed[k]];Vec3 toward;float distance;
                    const Vec3 sent=LiteLampRadiance(lamp,q,n,toward,distance);
                    if(!(sent.x>0||sent.y>0||sent.z>0))continue;
                    // Stop short of the lamp: its own fixture (a lantern's cage and glass) is in the
                    // render triangles and would shadow every surface it lights.
                    if(distance>LiteLampClearance&&LiteRay(mesh,q,toward,distance-LiteLampClearance,nullptr))continue;
                    light=light+sent*share;
                }
            }
            light=Tint(light,hit.Albedo(fallback));
            for(unsigned b=0;b<6;++b)sum[b]=sum[b]+light*LiteBandWeight(b,d);
        }
        for(unsigned b=0;b<6;++b)bake.lamps[cell*6+b]=sum[b]*(1/norms[b]);
    }
}

// A probe inside a wall holds nothing true, and the lit programs blend it into the surfaces
// beside it. Give it the mean of its open neighbours, working inward from the open probes.
inline void LiteGridFillInside(const LiteGridLayout& layout,std::vector<Vec3>& bands,const std::vector<uint8_t>& inside) {
    std::vector<uint8_t> open(inside.size());
    for(size_t i=0;i<inside.size();++i)open[i]=!inside[i];
    const int dims[3]{int(layout.nx),int(layout.ny),int(layout.nz)};
    static constexpr int offsets[6][3]{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for(unsigned pass=0;pass<8;++pass){
        std::vector<size_t> filled;std::vector<Vec3> values;
        for(int z=0;z<dims[2];++z)for(int y=0;y<dims[1];++y)for(int x=0;x<dims[0];++x){
            const size_t cell=layout.Index(x,y,z);if(open[cell])continue;
            Vec3 sum[6]{};unsigned count=0;
            for(const auto& o:offsets){
                const int nx=x+o[0],ny=y+o[1],nz=z+o[2];
                if(nx<0||ny<0||nz<0||nx>=dims[0]||ny>=dims[1]||nz>=dims[2])continue;
                const size_t other=layout.Index(nx,ny,nz);if(!open[other])continue;
                for(unsigned b=0;b<6;++b)sum[b]=sum[b]+bands[other*6+b];
                ++count;
            }
            if(!count)continue;
            filled.push_back(cell);
            for(unsigned b=0;b<6;++b)values.push_back(sum[b]*(1.f/count));
        }
        if(filled.empty())break;
        for(size_t i=0;i<filled.size();++i){
            open[filled[i]]=1;
            for(unsigned b=0;b<6;++b)bands[filled[i]*6+b]=values[i*6+b];
        }
    }
}
struct LiteGridColours {
    Vec3 sky{},sun{};
    Vec3 albedo{.55f,.55f,.55f}; // of a surface whose colour is not known
    // Strength of what surfaces send back: from the lamps, from the sun, and from the grid itself.
    float lampBounce=1,sunBounce=1,feedback=1;
};
// The strengths that put the grid's picture nearest the traced one: at six Inferno views by day
// (sun at 35 degrees) and four at night its mean level was within about 15% of it (2026-11-13).
// A lamp-lit room wanted its lamps' bounce at full strength and a little over; open shade came out
// too bright unless the sun's bounce and the second bounce were cut. The traced volume loses the
// same light in other ways (coarse voxels, rays that stop at 320 units), so these are a match to
// its picture, not a statement about the light. `bounce` is the user's dial (litebounce).
inline constexpr float LiteGridLampBounce=1.3f,LiteGridSunBounce=.45f,LiteGridFeedback=.5f;
inline LiteGridColours MakeLiteGridColours(Vec3 sky,Vec3 sun,Vec3 albedo,float bounce) {
    bounce=(std::max)(0.f,bounce);
    return {sky,sun,albedo,LiteGridLampBounce*bounce,LiteGridSunBounce*bounce,LiteGridFeedback*bounce};
}
// Light the grid already holds comes back off the surfaces each probe sees: the second bounce and,
// outdoors, the sky-lit ground. Read from the nearest probe on the surface's own side.
inline constexpr unsigned LiteGridFeedbackPasses=2;
// Puts the layers together into the six bands of every probe: sky, then what surfaces send back
// from the lamps and the sun, then that light bounced again.
inline std::vector<Vec3> LiteGridCompose(const LiteGridBake& bake,const LiteGridColours& c) {
    const size_t cells=bake.layout.Cells();const auto norms=LiteBandNorms();
    std::vector<Vec3> first(cells*6);
    for(size_t i=0;i<cells*6;++i)first[i]=c.sky*bake.sky[i]+bake.lamps[i]*c.lampBounce+Tint(bake.sun[i],c.sun)*c.sunBounce;
    LiteGridFillInside(bake.layout,first,bake.inside);
    auto bands=first;
    const float reach=(std::min)({bake.layout.Cell().x,bake.layout.Cell().y,bake.layout.Cell().z})*.5f;
    for(unsigned pass=0;pass<LiteGridFeedbackPasses;++pass){
        std::vector<Vec3> next(cells*6);
        for(size_t cell=0;cell<cells;++cell){
            if(bake.inside[cell])continue;
            const Vec3 p=LiteGridProbe(bake.layout,cell);Vec3 sum[6]{};
            for(unsigned i=0;i<LiteGridDirections;++i){
                const auto& hit=bake.hits[cell*LiteGridDirections+i];
                if(!LiteHitLit(hit.kind))continue;
                const Vec3 d=LiteGridDirection(i),n=hit.Normal();
                const size_t at=bake.layout.Nearest(p+d*hit.Distance()+n*reach);
                AmbientCube cube;for(unsigned b=0;b<6;++b)cube[b]=bands[at*6+b];
                const Vec3 sent=Tint(DecodeAmbientCube(cube,n),hit.Albedo(c.albedo))*c.feedback;
                for(unsigned b=0;b<6;++b)sum[b]=sum[b]+sent*LiteBandWeight(b,d);
            }
            for(unsigned b=0;b<6;++b)next[cell*6+b]=first[cell*6+b]+sum[b]*(1/norms[b]);
        }
        LiteGridFillInside(bake.layout,next,bake.inside);
        bands.swap(next);
    }
    return bands;
}
inline uint16_t LiteHalf(float value) {
    const float clamped=(std::min)((std::max)(value,0.f),65000.f);
    uint32_t bits;std::memcpy(&bits,&clamped,4);
    const int exponent=int((bits>>23)&255)-127+15;
    if(exponent<=0)return 0; // below the smallest normal half: black
    return uint16_t((exponent<<10)|((bits>>13)&1023));
}
// The volume the lit programs read: nx by ny by nz*6, band b in slices [b*nz, (b+1)*nz), RGBA16F.
inline std::vector<uint16_t> LiteGridTexels(const LiteGridLayout& layout,const std::vector<Vec3>& bands) {
    const size_t cells=layout.Cells();std::vector<uint16_t> texels(cells*6*4);
    for(unsigned b=0;b<6;++b)for(size_t cell=0;cell<cells;++cell){
        const auto& v=bands[cell*6+b];auto* out=&texels[(b*cells+cell)*4];
        out[0]=LiteHalf(v.x);out[1]=LiteHalf(v.y);out[2]=LiteHalf(v.z);out[3]=LiteHalf(1);
    }
    return texels;
}
}
