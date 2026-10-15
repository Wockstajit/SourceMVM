#include "../../AfxHookSource2/Filmmaker/Relight/RelightClusterMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShadowMath.h"
#include "../../AfxHookSource2/Filmmaker/Relight/RelightStainedGlassMath.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
using namespace Filmmaker::Relight;
void Check(bool value,const char* why) {if(!value){std::cerr<<why<<'\n';std::exit(1);}}
int main() {
    for(unsigned count:{1u,32u,33u,73u,238u,413u}) {
        const auto g=MakeClusterGrid(1600,900,count,6400);
        Check(g.tilesX==50&&g.tilesY==29,"Partial bottom row is allocated");
        const auto mask=ClusterFallback(g);
        for(unsigned mode:{0u,1u,2u}) {
            const auto list=g.ListRow(g.lights,mode),tiles=g.TileConstants(mode);
            const auto depth=g.SliceConstants(mode);
            for(unsigned y:{0u,31u,32u,899u})for(unsigned x:{0u,31u,32u,1599u}) {
                const auto row=ClusterTileRow(x,y,tiles,list);
                const auto expected=mode==1?g.lights.tiles+((y/32)*50+x/32)*list[2]:g.lights.ones;
                Check(row==expected&&row+list[2]<=mask.size(),"Native tile addressing stays in its own region");
            }
            for(float w:{-100.f,0.f,99.9f,100.f,6400.f,100000.f}) {
                const auto row=ClusterSliceRow(w,depth,list);
                const auto expected=mode?g.lights.slices+unsigned(std::clamp(w*.01f,0.f,63.f))*list[2]:g.lights.ones;
                Check(row==expected&&row+list[2]<=mask.size(),"Native depth addressing clamps at both ends");
            }
        }
        for(const auto* list:{&g.lights,&g.probes,&g.reflections}) {
            unsigned reachable=0;
            for(unsigned i=0;i<list->words*32;++i)if(mask[list->ones+i/32]&(1u<<(i%32)))++reachable;
            Check(reachable==list->count,"Fallback names every valid record and no padded record");
            Check(list->tiles+g.TileCount()*list->words==list->slices,"Tile and slice regions are disjoint");
        }
        Check(g.lights.slices+64*g.lights.words<=g.probes.tiles,"Probes do not overlap lights");
        Check(g.probes.slices+64*g.probes.words<=g.reflections.tiles,"Reflections do not overlap probes");
        Check(g.reflections.slices+64*g.reflections.words<=g.lights.nearby&&g.lights.nearby+g.lights.words==g.size,"First-person list has its own region");
        unsigned nearbyDefault=0;
        for(unsigned i=0;i<g.lights.words*32;++i)if(mask[g.lights.nearby+i/32]&(1u<<(i%32)))++nearbyDefault;
        Check(nearbyDefault==count,"Before the first fill, first-person pixels keep every light");
        const auto row=g.ListRow(g.lights,1);float wrong=0;std::memcpy(&wrong,&row[2],4);
        Check(wrong!=float(row[2]),"Integer row is not float encoded");
    }
    const auto g=MakeClusterGrid(1600,900,73,6400);
    ClusterView view{};view[2][0]=1;view[12][0]=1;view[13][1]=1;view[14][3]=1;
    Light lamp;lamp.position={0,0,1000};lamp.range=20;lamp.lumens=500;
    auto record=ToRecord(lamp,1);
    const auto extent=LightClusterExtent(record,g,view,1600,900);
    Check(!extent.allTiles&&extent.x0>0&&extent.x1<49,"Distant small light occupies only central tiles");
    // Independently project a dense collection of surface points in the falloff sphere.
    for(int z=-20;z<=20;z+=2)for(int y=-20;y<=20;y+=2)for(int x=-20;x<=20;x+=2) {
        if(x*x+y*y+z*z>400)continue;
        const float w=1000.f+z;
        const unsigned tx=unsigned((float(x)/w+1)*800)/32;
        const unsigned ty=unsigned((1-float(y)/w)*450)/32;
        const unsigned slice=unsigned(w*.01f);
        Check(tx>=extent.x0&&tx<=extent.x1&&ty>=extent.y0&&ty<=extent.y1,
              "Every contributing sample is retained by its tile extent");
        Check(slice>=extent.z0&&slice<=extent.z1,"Every contributing sample is retained by its depth extent");
    }
    record[35]=1e8f;
    const auto softened=LightClusterExtent(record,g,view,1600,900);
    Check(softened.x0==extent.x0&&softened.x1==extent.x1,"Emitter softness is not mistaken for a cull radius");
    lamp.position.z=10;record=ToRecord(lamp,1);
    const auto near=LightClusterExtent(record,g,view,1600,900);
    Check(near.x0==0&&near.x1==49&&near.y0==0&&near.y1==28,"Eye-enclosing light retains every screen tile");
    lamp.position={100,0,10};record=ToRecord(lamp,1);
    const auto side=LightClusterExtent(record,g,view,1600,900);
    Check(!side.allTiles&&side.x0>24,"Eye-plane crossing to the right excludes the left half");
    lamp.position={0,0,-100};record=ToRecord(lamp,1);
    const auto behind=LightClusterExtent(record,g,view,1600,900);
    Check(!behind.allTiles&&behind.x0>behind.x1,"Wholly behind-eye light has an empty tile range");
    record=SunRecord({0,0,1},{1,1,1},nullptr,false);
    const auto sun=LightClusterExtent(record,g,view,1600,900);
    Check(sun.allTiles&&sun.z0==0&&sun.z1==63,"Unbounded sun remains in every cluster");
    lamp.position={100000,0,1000};record=ToRecord(lamp,1);
    const auto edge=LightClusterExtent(record,g,view,1600,900);
    Check(edge.x1==49&&edge.x0==48,"Offscreen projection is folded into the edge and dilated");
    view[2][0]=0;
    const auto raster=LightClusterExtent(record,g,view,1600,900);
    Check(raster.z0==edge.z0&&raster.z1==edge.z1,"Raster and reconstructed W use identical slice bounds");
    lamp.position={180,60,1000};lamp.range=300;
    for(float eyeDepth:{1000.f,100.f,-100.f})for(Vec3 direction:{Vec3{1,0,0},Vec3{-1,0,0},Vec3{0,1,0},Vec3{0,-1,0},Vec3{0,0,1},Vec3{0,0,-1},Vec3{.6f,.8f,0}}){
        lamp.position.z=eyeDepth;
        const auto matrix=Filmmaker::Environment::LocalShadowMatrix(lamp.position,direction,90,lamp.range);
        record=ToShadowedRecord(lamp,1,matrix.data(),0);
        Check(LightClusterHull(record,lamp.range).count==5,"Perspective shadow faces use a finite cone hull");
        const auto face=LightClusterExtent(record,g,view,1600,900);
        for(int z=-300;z<=300;z+=15)for(int y=-300;y<=300;y+=15)for(int x=-300;x<=300;x+=15){
            if(x*x+y*y+z*z>90000)continue;
            const Vec3 p=lamp.position+Vec3{float(x),float(y),float(z)};
            if(p.z<=1e-4f)continue;
            float clip[4]{};for(unsigned row=0;row<4;++row)clip[row]=matrix[row*4]*p.x+matrix[row*4+1]*p.y+matrix[row*4+2]*p.z+matrix[row*4+3];
            if(clip[3]<=0||std::abs(clip[0])>clip[3]||std::abs(clip[1])>clip[3]||clip[2]<0||clip[2]>clip[3])continue;
            const unsigned tx=unsigned(std::clamp((p.x/p.z+1)*800,0.f,1599.f))/32;
            const unsigned ty=unsigned(std::clamp((1-p.y/p.z)*450,0.f,899.f))/32;
            const unsigned slice=unsigned(std::clamp(p.z*.01f,0.f,63.f));
            Check(face.allTiles||(tx>=face.x0&&tx<=face.x1&&ty>=face.y0&&ty<=face.y1),"Cone culling retains every independently projected contributing receiver");
            Check(slice>=face.z0&&slice<=face.z1,"Cone depth culling retains every contributing receiver");
        }
    }
    lamp.position.z=1000;
    record=ToRecord(lamp,1);const auto sphere=LightClusterExtent(record,g,view,1600,900);
    const auto facing=Filmmaker::Environment::LocalShadowMatrix(lamp.position,{0,0,1},90,lamp.range);
    const auto front=LightClusterExtent(ToShadowedRecord(lamp,1,facing.data(),0),g,view,1600,900);
    Check(front.z0>sphere.z0,"Forward face excludes the bulb sphere's rear depth slices");
    record=ToShadowedRecord(lamp,1,facing.data(),0);record[15]+=5;
    Check(LightClusterHull(record,lamp.range).count==8,"An unrelated projection centre retains the sphere box");
    for(Vec3 towardSun:{Vec3{0,0,-1},Vec3{.4f,.2f,-1}}){
        const auto beam=GlassBeamProjection({{-80,-60,900},{80,-60,900},{80,60,900},{-80,60,900}},towardSun,500);
        auto cookie=GlassBeamRecord(beam,towardSun,{1,1,1},.5f);
        ClusterHull hull;Check(GlassClusterHull(cookie,hull),"Orthographic glass cookie has a finite hull");
        const auto bounds=LightClusterExtent(cookie,g,view,1600,900);
        Check(!bounds.allTiles&&bounds.x0>0&&bounds.x1<49&&bounds.z1<63,"Glass beam excludes unrelated screen tiles and distant slices");
        unsigned retained=0;
        for(int z=700;z<=1500;z+=20)for(int y=-300;y<=300;y+=20)for(int x=-400;x<=400;x+=20){
            const Vec3 p{float(x),float(y),float(z)};const auto clip=BeamClip(beam,p);
            if(std::abs(clip.x)>1||std::abs(clip.y)>1||clip.z<0||clip.z>1)continue;
            ++retained;
            const unsigned tx=unsigned(std::clamp((p.x/p.z+1)*800,0.f,1599.f))/32;
            const unsigned ty=unsigned(std::clamp((1-p.y/p.z)*450,0.f,899.f))/32;
            const unsigned slice=unsigned(std::clamp(p.z*.01f,0.f,63.f));
            Check(tx>=bounds.x0&&tx<=bounds.x1&&ty>=bounds.y0&&ty<=bounds.y1&&slice>=bounds.z0&&slice<=bounds.z1,
                "Every independently clipped glass receiver survives cluster culling");
        }
        Check(retained>100,"Beam coverage fixture samples its interior");
        cookie[0]=cookie[1]=cookie[2]=0;
        const auto malformed=LightClusterExtent(cookie,g,view,1600,900);
        Check(malformed.allTiles&&malformed.z0==0&&malformed.z1==63,"Unknown cookie projection retains the conservative fallback");
    }
    const auto crossing=GlassBeamProjection({{-80,-60,-10},{80,-60,-10},{80,60,-10}}, {0,0,-1},500);
    const auto crossingExtent=LightClusterExtent(GlassBeamRecord(crossing,{0,0,-1},{1,1,1},.5f),g,view,1600,900);
    Check(crossingExtent.x0==0&&crossingExtent.x1==49&&crossingExtent.y0==0&&crossingExtent.y1==28,
        "A glass beam crossing the eye retains all screen tiles");
    // First-person list: a light is listed when its range can reach the eye's sphere.
    Light hand;hand.range=40;hand.lumens=100;
    hand.position={0,0,FirstPersonLightReach+39};Check(FirstPersonLight(ToRecord(hand,1),{0,0,0}),"Light whose range reaches the held weapon is listed");
    hand.position={0,0,FirstPersonLightReach+41};Check(!FirstPersonLight(ToRecord(hand,1),{0,0,0}),"Light that cannot reach the eye sphere is dropped");
    Check(FirstPersonLight(ToRecord(hand,1),{0,0,FirstPersonLightReach}),"The test follows the eye, not the origin");
    Check(FirstPersonLight(SunRecord({0,0,1},{1,1,1},nullptr,false),{0,0,0}),"The sun has no range and is always listed");
    const auto cookieBeam=GlassBeamProjection({{-80,-60,9000},{80,-60,9000},{80,60,9000},{-80,60,9000}},{0,0,-1},500);
    Check(FirstPersonLight(GlassBeamRecord(cookieBeam,{0,0,-1},{1,1,1},.5f),{0,0,0}),"Glass beams are always listed");
    record=ToRecord(hand,1);record[37]=0;Check(FirstPersonLight(record,{0,0,0}),"A record without a proven range is listed");
    std::cout<<"Relight cluster addressing and conservative bounds passed\n";
}
