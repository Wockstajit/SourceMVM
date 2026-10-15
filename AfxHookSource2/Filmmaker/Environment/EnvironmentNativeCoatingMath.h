#pragma once
#include "EnvironmentShadowMath.h"
#include "EnvironmentClimateMath.h"
#include <stdexcept>
namespace Filmmaker::Environment {
// Geometry for the native material: coverage stays fixed to map coordinates.
inline float NativeCoatingNoise(Vec3 p) {
    auto hash=[](Vec3 v){
        auto fract=[](float f){return f-std::floor(f);};
        v={fract(v.x*.1031f),fract(v.y*.1031f),fract(v.z*.1031f)};
        float d=v.x*(v.y+33.33f)+v.y*(v.z+33.33f)+v.z*(v.x+33.33f);
        v=v+Vec3{d,d,d};return fract((v.x+v.y)*v.z);
    };
    Vec3 a{std::floor(p.x),std::floor(p.y),std::floor(p.z)},f=p-a;
    f={f.x*f.x*(3-2*f.x),f.y*f.y*(3-2*f.y),f.z*f.z*(3-2*f.z)};
    float value=0;
    for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<2;++x)
        value+=hash(a+Vec3{float(x),float(y),float(z)})*(x?f.x:1-f.x)*(y?f.y:1-f.y)*(z?f.z:1-f.z);
    return value;
}
// Rendered floors (cobbles, tiles) stand a few units proud of the flat collision surface the
// water is built from; this lift keeps the water visible over them without reading as floating.
inline constexpr float kNativeWaterLift = 3.0f;
// `edge` is the finest piece along a puddle outline; callers coarsen it when a large map's
// coverage would exceed the geometry budget.
inline std::vector<Vec3> NativeCoatingTriangles(const OcclusionMesh& world,const Climate& c,float edge=32) {
    std::vector<Vec3> result;
    float amount=std::clamp(c[CL_puddles],0.f,1.f)*(1-c[CL_snow]);
    if(amount<=0)return result;
    auto mask=[&](Vec3 p){
        float noise=NativeCoatingNoise(p*c[CL_maskScale])*.57f+
            NativeCoatingNoise(p*(c[CL_maskScale]*2.03f)+Vec3{17,17,17})*.29f+
            NativeCoatingNoise(p*(c[CL_maskScale]*4.11f)-Vec3{9,9,9})*.14f;
        // The bounded fBm distribution occupies about .2..8, rather than 0..1.
        // Expanding that interval makes low coverage useful and grows the SAME basins.
        float value=noise-(.8f-.62f*amount);
        if(c[CL_surfaceRadius]>0){
            Vec3 offset=p-Vec3{c[CL_surfaceX],c[CL_surfaceY],p.z};
            value=std::min(value,(c[CL_surfaceRadius]-std::sqrt(Dot(offset,offset)))/32);
            value=std::min(value,(c[CL_puddleDepth]-std::abs(p.z-c[CL_puddleHeight]))/8);
        }
        return value;
    };
    auto emit=[&](Vec3 a,Vec3 b,Vec3 d){
        std::vector<Vec3> polygon{a,b,d},clipped;
        for(size_t i=0;i<3;++i){auto p=polygon[i],q=polygon[(i+1)%3];float mp=mask(p),mq=mask(q);
            if(mp>=0)clipped.push_back(p);
            if((mp>=0)!=(mq>=0))clipped.push_back(p+(q-p)*(mp/(mp-mq)));
        }
        for(size_t i=1;i+1<clipped.size();++i){
            for(auto p:{clipped[0],clipped[i],clipped[i+1]})result.push_back(p+Vec3{0,0,kNativeWaterLift});
        }
    };
    for(size_t i=0;i+2<world.indices.size();i+=3){
        Vec3 a=world.positions[world.indices[i]],b=world.positions[world.indices[i+1]],d=world.positions[world.indices[i+2]];
        Vec3 normal=Normalize(Cross(b-a,d-a));// Reject downward faces and draining slopes. Collision winding is meaningful:
        // flipping ceiling triangles used to put standing water under ceilings.
        if(normal.z<.995f)continue;
        // ADAPTIVE SUBDIVISION. A uniform 32-unit grid split every floor triangle -- and Mirage's
        // kilometre-wide background ground planes into 4096 pieces each -- so map-wide puddles
        // always blew the geometry budget and never appeared. Pieces are split only while they are
        // larger than three outline pieces or the mask changes sign across them; a
        // piece whose corners, centre and edge midpoints agree is wholly dry or wholly wet.
        // Triangles longer than 4096 units are the map's distant backdrop, outside any play space.
        float sourceLongest=std::sqrt(std::max({Dot(b-a,b-a),Dot(d-a,d-a),Dot(d-b,d-b)}));
        if(sourceLongest>4096)continue;
        struct Piece {Vec3 a,b,d;};
        std::vector<Piece> stack{{a,b,d}};
        while(!stack.empty()){
            Piece t=stack.back();stack.pop_back();
            float longest=std::sqrt(std::max({Dot(t.b-t.a,t.b-t.a),Dot(t.d-t.a,t.d-t.a),Dot(t.d-t.b,t.d-t.b)}));
            if(longest>edge){
                Vec3 ab=(t.a+t.b)*.5f,bd=(t.b+t.d)*.5f,da=(t.d+t.a)*.5f;
                bool split=longest>edge*3;
                if(!split){
                    int wet=0;
                    for(Vec3 q:{t.a,t.b,t.d,ab,bd,da,(t.a+t.b+t.d)*(1/3.f)})wet+=mask(q)>=0;
                    if(wet==0)continue;
                    split=wet!=7;
                }
                if(split){
                    stack.push_back({t.a,ab,da});stack.push_back({ab,t.b,bd});
                    stack.push_back({da,bd,t.d});stack.push_back({ab,bd,da});
                    continue;
                }
            }
            emit(t.a,t.b,t.d);
            if(result.size()>1200000)throw std::runtime_error("Native water coverage exceeds geometry budget; reduce its area");
        }
    }
    return result;
}
}
