#include "../../AfxHookSource2/Filmmaker/Relight/RelightProbeMath.h"
#include <iostream>
#include <cstdlib>
#include <cstring>
using namespace Filmmaker::Relight;
void Check(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
bool Near(float a,float b){return std::abs(a-b)<.0001f;}
int main(){
    Light barn;barn.kind="light_barn";barn.position={80,80,80};barn.sizeX=8;barn.sizeY=8;barn.sizeZ=.1f;barn.range=100;
    const auto fixtureBins=BinGiLights({ToRecord(barn,1)},GiRegion{{0,0,0},GiRegionSize});
    Check(fixtureBins.offsets[1]>0&&fixtureBins.offsets.back()<100,"Projected fixture bins its expanding frustum rather than every GI brick");
    AmbientCube cube{{{1,0,0},{0,2,0},{0,0,3},{4,0,0},{0,5,0},{0,0,6}}};
    const auto diagonal=DecodeAmbientCube(cube,{.6f,0,.8f});
    Check(Near(diagonal.x,.36f)&&Near(diagonal.z,1.92f),"Native ambient cube uses squared normal weights");
    Check(DecodeAmbientCube(cube,{-1,0,0}).x==4&&DecodeAmbientCube(cube,{0,0,-1}).z==6,"Negative bands follow positive XYZ");
    const auto hemi=HemisphereCube({1,2,3},{.2f,.3f,.4f});
    Check(DecodeAmbientCube(hemi,{0,0,1}).z==3&&DecodeAmbientCube(hemi,{0,0,-1}).z==.4f,"Hemisphere axis samples round trip");
    AmbientCube neutral;neutral.fill(ProbeNeutral);
    const auto decoded=DecodeAmbientCube(neutral,{.6f,0,.8f});
    Check(Near(decoded.x,ProbeNeutral.x)&&Near(decoded.z,ProbeNeutral.z),"Uniform positive volume exactly matches constant fallback");
    for(unsigned size:{1u,16u,32u}) {
        const auto pack=PackProbe({-100,-200,-300},{100,200,300},size);
        const float world[4]{0,0,0,1};
        for(unsigned axis=0;axis<3;++axis){float uv=0;for(unsigned c=0;c<4;++c)uv+=pack.volume[axis][c]*world[c];Check(Near(uv,.5f),"Map center transforms to volume center");}
        for(unsigned band=0;band<6;++band)for(float z:{-1.f,0.f,.5f,1.f,2.f}) {
            const auto uv=ProbeSampleCoordinates({.5f,.5f,z},pack,band);
            Check(uv[2]>float(band)/6&&uv[2]<float(band+1)/6,"Native divide-clamp-scale sequence never blends adjacent bands");
            const auto texel=unsigned(uv[2]*float(size*6));
            Check(texel/size==band,"Native sample decodes the selected ambient-cube band");
        }
        uint32_t index=1;std::memcpy(&index,&pack.probe[5][3],4);Check(index==0,"Probe points at owned cb2 entry zero");
    }
    {
        const Vec3 camera{970,-116,329};
        const auto centred=CameraGiRegion(camera),ahead=ViewGiRegion(camera,{-1,0,0});
        Check(ahead.low.x<centred.low.x&&camera.x-ahead.low.x>1800,"View-directed region covers the distant street without coarsening probes");
        Check(ahead.low.x+ahead.size-camera.x>600,"Transport retains coverage behind the viewer");
        Check(ViewGiRegion(camera,{-5,0,0})==ahead,"View coverage is independent of direction magnitude");
        Check(ViewGiRegion(camera,{})==centred&&ViewGiRegion(camera,{NAN,0,0})==centred,"Invalid view direction uses camera-centred fallback");
        const auto moved=GiProbeShift(ahead,ViewGiRegion(camera+Vec3{40,0,0},{-1,0,0}));
        Check(moved[0]==1&&moved[1]==0&&moved[2]==0,"View-directed scrolling preserves whole-probe history");
    }
    {
        const Vec3 capture{100,100,100};
        Check(StableReflectionOrigin({101,102,100},capture,true).x==capture.x,"Walking retains a world capture instead of changing voxel silhouettes every step");
        Check(StableReflectionOrigin({150,100,100},capture,true).x==150,"Leaving the capture neighbourhood refreshes the environment");
        Check(StableReflectionOrigin({101,102,100},capture,false).x==101,"The first capture uses the actual camera rather than an uninitialized anchor");
        const auto moved=PlanReflectionUpdate({101,102,100},capture,true,false,true);
        Check(moved.origin.x==101&&moved.origin.y==102&&moved.historyWeight==0,
            "Even a small viewer movement refreshes the capture and rejects history from another origin");
        Check(PlanReflectionUpdate(capture,capture,true,false,true).historyWeight==.2f,
            "Stationary captures retain only a short history");
        Check(PlanReflectionUpdate(capture,capture,true,true,true).historyWeight==0&&
            PlanReflectionUpdate(capture,capture,false,false,true).historyWeight==0,
            "Lighting edits and the first capture publish without stale history");
        const auto cut=PlanReflectionUpdate({900,800,700},capture,true,false,true);
        Check(cut.origin.x==900&&cut.historyWeight==0,"A loop seek cannot blend the preceding camera's reflection");
        const auto legacy=PlanReflectionUpdate({101,102,100},capture,true,false,false);
        Check(legacy.origin.x==100&&legacy.historyWeight==.8f,"The diagnostic preserves the original delayed path");
    }
    Scene lighting;lighting.hasSun=true;lighting.sunBrightness=2;
    Light lamp;lamp.position={100,200,300};lamp.lumens=500;lighting.lights.push_back(lamp);
    const auto key=ProbeLightingKey(lighting,20);
    ++lighting.generation;lighting.bounce=4;
    Check(ProbeLightingKey(lighting,20)==key,"Displayed bounce and unrelated generation edits do not accelerate transport");
    lighting.indirect=2;
    Check(ProbeLightingKey(lighting,20)!=key,"Incident sky intensity edits accelerate recursive transport");
    const auto skyKey=ProbeLightingKey(lighting,20);lighting.skyColor={0,1,0};
    Check(ProbeLightingKey(lighting,20)!=skyKey,"Incident sky hue edits accelerate recursive transport");
    Check(ProbeLightingKey(lighting,21)!=key,"Newly resident world shadows invalidate unshadowed transport");
    const auto beforeLamp=ProbeLightingKey(lighting,20);lighting.lights[0].color={1,0,0};
    Check(ProbeLightingKey(lighting,20)!=beforeLamp,"Emitter radiance edits invalidate bounced energy");
    const auto beforeCone=ProbeLightingKey(lighting,20);lighting.lights[0].outerAngle+=10;
    Check(ProbeLightingKey(lighting,20)!=beforeCone,"Scene light cone edits invalidate bounced energy");
    const auto beforeShadows=ProbeLightingKey(lighting,20);lighting.lights[0].runtimeShadows=false;
    Check(ProbeLightingKey(lighting,20)!=beforeShadows,"Scene light shadow edits invalidate bounced energy");
    const auto edited=ProbeLightingKey(lighting,20);lighting.sunAngles.y+=30;
    Check(ProbeLightingKey(lighting,20)!=edited,"Time of day invalidates sunlight transport");
    {
        // Mirror of the shipped material's clustered reflection loop (a8163867e71fb140 lines
        // 933-966): box-space position, edge fade, parallax-corrected lookup direction.
        const Vec3 origin{100,-2000,300};
        const auto rows=ReflectionRecordRows(origin,.75f);
        auto box=[&](Vec3 p){float q[3];for(int r=0;r<3;++r)q[r]=rows[r][0]*p.x+rows[r][1]*p.y+rows[r][2]*p.z+rows[r][3];return Vec3{q[0],q[1],q[2]};};
        auto lookup=[&](Vec3 p,Vec3 d,float& weight){
            const Vec3 q=box(p);const float qa[3]{q.x,q.y,q.z},da[3]{d.x,d.y,d.z};
            weight=1;float t=1e30f;
            for(int a=0;a<3;++a){
                weight=std::min({weight,std::clamp((qa[a]-rows[3][a])*rows[5][a],0.f,1.f),std::clamp((rows[4][a]-qa[a])*rows[5][a],0.f,1.f)});
                t=std::min(t,std::max((rows[4][a]-qa[a])/da[a],(rows[3][a]-qa[a])/da[a]));
            }
            const Vec3 hit=q+d*std::abs(t);const float n=std::sqrt(hit.x*hit.x+hit.y*hit.y+hit.z*hit.z);return hit*(1/n);
        };
        for(Vec3 p:{Vec3{0,0,0},Vec3{-16000,12000,-3000},Vec3{30000,-30000,8000}})
            for(Vec3 d:{Vec3{0,0,1},Vec3{.6f,-.8f,0},Vec3{-.48f,.6f,-.64f}}){
                float weight=0;const Vec3 l=lookup(p,d,weight);
                Check(weight==1,"Every point of a map lies in the capture's full-weight interior");
                Check(l.x*d.x+l.y*d.y+l.z*d.z>.99f,"The oversized box reduces parallax correction to the reflected direction");
            }
        Check(rows[3][3]==0,"The record selects cube 0 of the bound array");
        Check(rows[7][0]==.75f&&rows[7][1]==.75f&&rows[7][2]==.75f,"Strength is the record tint");
        Check(ReflectionNormalizationCap[0]==0&&ReflectionNormalizationCap[1]==1,"Local ambient may dim, never brighten, the capture");
    }
    std::cout<<"Probe packing and ambient-cube decode passed\n";
}
