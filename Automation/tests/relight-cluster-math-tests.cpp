#include "../../AfxHookSource2/Filmmaker/Relight/RelightClusterMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShadowMath.h"
#include "../../AfxHookSource2/Filmmaker/Relight/RelightStainedGlassMath.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>
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
    // ---- visible reach ----
    {
        Check(VisibleRadianceCutoff(0,.3f)==0&&VisibleRadianceCutoff(1,0)==0,"No cutoff without steps or a known exposure");
        const float cut=VisibleRadianceCutoff(1,.286f);
        Check(std::abs(std::log2(cut/.006590f))<=1.f/32,"One step at exposure 0.286 is about 0.0066 radiance (sixteenth-stop rounding)");
        Check(VisibleRadianceCutoff(1,.143f)>cut,"A lower exposure (a darker picture) hides more light: a larger cutoff");
        Light dim;dim.position={0,0,0};dim.range=1000;dim.lumens=75;dim.color={1,.69f,.40f};
        const auto record=ToRecord(dim,1);
        const float authored=-record[36]/record[37];
        Check(LightVisibleReach(record,authored,0)==authored,"Zero cutoff keeps the authored range");
        const float reach=LightVisibleReach(record,authored,cut);
        Check(reach>50&&reach<authored*.6f,"A dim 75-lumen lamp stops well inside its 1000-unit range");
        // The shipped loop's own evaluation: inside the reach the light is above the cutoff, and
        // nothing past it (to the authored range) rises above it again.
        auto contribution=[&](float d){const float peak=(std::max)(record[40],(std::max)(record[41],record[42]));
            const float t=std::clamp(record[36]+record[37]*d,0.f,1.f);
            return peak*record[35]/(std::max)(record[35],d*d)*(t*t*(3-2*t));};
        Check(contribution(reach*.995f)>cut,"Just inside the reach the light still exceeds the cutoff");
        bool tail=true;for(float d=reach;d<=authored;d+=1)tail=tail&&contribution(d)<=cut*1.0001f;
        Check(tail,"Past the reach the light never exceeds the cutoff");
        Light bright=dim;bright.lumens=4000;bright.range=480;
        const auto strong=ToRecord(bright,1);
        Check(LightVisibleReach(strong,-strong[36]/strong[37],cut)>400,"A bright lamp keeps most of its short range (its fade to the range end is what it loses)");
        // ---- shadow reach: the face depth a receiver at that face-axis distance stores ----
        for(int face=0;face<6;++face){
            const Vec3 origin{120,-40,64};
            const auto matrix=Filmmaker::Environment::LocalShadowMatrix(origin,Filmmaker::Environment::PointShadowDirection(face),90,1000,8);
            auto faceRecord=ToShadowedRecord(dim,1,matrix.data(),StaticTile);
            const Vec3 axis=Filmmaker::Environment::PointShadowDirection(face);
            for(const float distance:{20.f,240.f,900.f}){
                const Vec3 p{origin.x+axis.x*distance+3*(axis.y+axis.z),origin.y+axis.y*distance+3*(axis.z+axis.x),origin.z+axis.z*distance+3*(axis.x+axis.y)}; // off-axis, same axis distance
                const float z=faceRecord[8]*p.x+faceRecord[9]*p.y+faceRecord[10]*p.z+faceRecord[11];
                const float w=faceRecord[12]*p.x+faceRecord[13]*p.y+faceRecord[14]*p.z+faceRecord[15];
                Check(std::abs(ShadowReachDepth(faceRecord,distance)-z/w)<1e-5f,"The reach depth is what the face stores at that axis distance");
            }
            Check(ShadowReachDepth(faceRecord,4)==1e-7f,"A reach inside the near plane skips every receiver");
            Check(ShadowReachDepth(faceRecord,5000)==0,"A reach past the far plane keeps every lookup");
            Check(ShadowReachDepth(ToRecord(dim,1),100)==0,"An unshadowed record (no projection) has no reach depth");
        }
        Check(ShadowReachSlot==91&&ToShadowedRecord(dim,1,Filmmaker::Environment::LocalShadowMatrix({0,0,0},{1,0,0},90,1000,8).data(),StaticTile)[ShadowReachSlot]==0,
            "A shadowed record keeps every lookup until a reach is set");
        // ---- cube shadow records: one record for the six faces, or none ----
        {
            std::array<float,96> faces{};
            for(int f=0;f<6;++f){const auto m=Filmmaker::Environment::LocalShadowMatrix({dim.position.x,dim.position.y,dim.position.z},Filmmaker::Environment::PointShadowDirection(f),90,dim.range,8);
                std::copy(m.begin(),m.end(),faces.begin()+16*f);}
            LightRecord cube{};
            Check(ToCubeShadowRecord(dim,1,faces.data(),StaticTile,cube)&&cube[87]==CubeShadowMarker&&cube[88]==dim.range,"A point light's six faces make one cube record");
            const auto unshadowed=ToRecord(dim,1);
            Check(std::equal(cube.begin(),cube.begin()+16,unshadowed.begin()),"A cube record keeps the unshadowed range box");
            for(unsigned f=0;f<6;++f){const auto face=ToShadowedRecord(dim,1,faces.data()+16*f,StaticTile+f);
                Check(std::equal(face.begin()+52,face.begin()+56,cube.begin()+4*(CubeFaceRectRow+f)),"Each face's rect is its face record's");}
            Check(ShadowReachDepth(cube,300)==cube[89]+cube[90]/300,"A cube record's reach depth uses its faces' depth mapping");
            auto swapped=faces;std::swap_ranges(swapped.begin(),swapped.begin()+16,swapped.begin()+16);
            Check(!ToCubeShadowRecord(dim,1,swapped.data(),StaticTile,cube),"Faces out of order keep six face records");
            auto narrow=faces;for(int f=0;f<6;++f){narrow[16*f]*=1.2f;narrow[16*f+1]*=1.2f;narrow[16*f+2]*=1.2f;}
            Check(!ToCubeShadowRecord(dim,1,narrow.data(),StaticTile,cube),"A face that is not 90 degrees keeps six face records");
        }
        const auto sun=SunRecord({0,0,1},{1,1,1},nullptr,false);
        const auto g=MakeClusterGrid(1600,900,4,6400);
        ClusterView view{};view[2][0]=1;view[12][0]=1;view[13][1]=1;view[14][3]=1;
        const auto sunExtent=LightClusterExtent(sun,g,view,1600,900,cut);
        Check(sunExtent.allTiles&&sunExtent.z0==0&&sunExtent.z1==63,"The sun is never cut");
        Light far=dim;far.position={0,0,2000};
        const auto farRecord=ToRecord(far,1);
        const auto full=LightClusterExtent(farRecord,g,view,1600,900,0),cutExtent=LightClusterExtent(farRecord,g,view,1600,900,cut);
        Check(cutExtent.x1-cutExtent.x0<full.x1-full.x0&&cutExtent.z1-cutExtent.z0<full.z1-full.z0,"The cut lamp covers fewer tiles and slices");
        Check(cutExtent.x0>=full.x0&&cutExtent.x1<=full.x1&&cutExtent.z0>=full.z0&&cutExtent.z1<=full.z1,"The cut extent lies inside the authored one");
        Light nearEye=dim;nearEye.position={0,0,600};
        const auto nearRecord=ToRecord(nearEye,1);
        Check(FirstPersonLight(nearRecord,{0,0,0})&&!FirstPersonLight(nearRecord,{0,0,0},FirstPersonLightReach,cut),
              "A dim lamp 600 units away reaches the eye by range but not visibly");
    }
    // Tangent bounds retain independently sampled receivers under asymmetric, rotated and
    // orthographic projections, while removing corners of the old projected range box.
    {
        const auto grid=MakeClusterGrid(2560,1440,1,6400);bool narrowed=false;
        for(unsigned projection=0;projection<4;++projection){
            ClusterView v{};v[12][0]=.9f;v[13][0]=.2f;v[14][0]=-.4f;
            v[12][1]=-.1f;v[13][1]=1.1f;v[14][1]=.05f;
            v[15][0]=100;v[15][1]=-80;
            if(projection==0)v[14][3]=1;
            if(projection==1){v[12][3]=.5f;v[13][3]=.1f;v[14][3]=1;v[15][3]=20;}
            if(projection==2)v[15][3]=700;
            if(projection==3)v[14][3]=1;
            Light bulb;bulb.position={300,-90,projection==3?100.f:700.f};bulb.range=250;bulb.lumens=500;
            const auto r=ToRecord(bulb,1);
            const auto tight=LightClusterExtent(r,grid,v,2560,1440,0,true);
            const auto box=LightClusterExtent(r,grid,v,2560,1440,0,false);
            Check(tight.x0>=box.x0&&tight.x1<=box.x1&&tight.y0>=box.y0&&tight.y1<=box.y1,"Tangent bounds never enlarge the hull");
            narrowed|=tight.x0>box.x0||tight.x1<box.x1||tight.y0>box.y0||tight.y1<box.y1;
            for(int z=-250;z<=250;z+=10)for(int y=-250;y<=250;y+=10)for(int x=-250;x<=250;x+=10){
                if(x*x+y*y+z*z>250*250)continue;
                const auto p=bulb.position+Vec3{float(x),float(y),float(z)};
                auto clip=[&](unsigned axis){return p.x*v[12][axis]+p.y*v[13][axis]+p.z*v[14][axis]+v[15][axis];};
                const auto w=clip(3);if(w<1e-4f)continue;
                const auto tx=unsigned(std::clamp((clip(0)/w+1)*1280,0.f,2559.f))/32;
                const auto ty=unsigned(std::clamp((1-clip(1)/w)*720,0.f,1439.f))/32;
                Check(tx>=tight.x0&&tx<=tight.x1&&ty>=tight.y0&&ty<=tight.y1,"Projected sphere retains every independently sampled lit point");
            }
        }
        Check(narrowed,"Analytic tangents discard unnecessary light tiles");
    }
    // Fine depth slices: every depth a lamp reaches stays inside its slices, which cover little
    // more than the lamp's own depth instead of several 150-unit slices.
    {
        const auto coarse=MakeClusterGrid(2560,1440,1,10000),fine=MakeClusterGrid(2560,1440,1,10000,128,128,ClusterFineSliceUnits);
        Check(coarse.sliceCount==64&&fine.sliceCount==625&&fine.sliceScale==1/ClusterFineSliceUnits,"Fine slices are 16 units deep across the map");
        Check(MakeClusterGrid(2560,1440,1,100000,128,128,ClusterFineSliceUnits).sliceCount==ClusterFineSliceLimit&&
              MakeClusterGrid(2560,1440,1,200,128,128,ClusterFineSliceUnits).sliceCount==64,"The slice count is bounded at both ends");
        Check(fine.lights.slices+fine.sliceCount*fine.lights.words<=fine.probes.tiles&&fine.lights.nearby+fine.lights.words==fine.size&&
              ClusterSummaryFallback(fine).size()==size_t(fine.size)*2,"Fine slices keep the regions disjoint");
        ClusterView v{};v[12][0]=1;v[13][1]=1;v[14][3]=1; // w = z
        Light bulb;bulb.position={40,-30,900};bulb.range=220;bulb.lumens=500;
        const auto r=ToRecord(bulb,1);
        const auto a=LightClusterExtent(r,coarse,v,2560,1440),b=LightClusterExtent(r,fine,v,2560,1440);
        for(int z=-220;z<=220;z+=4){const float w=bulb.position.z+z;
            const auto slice=[&](const ClusterGrid& g){return unsigned(std::clamp(w*g.sliceScale,0.f,float(g.sliceCount-1)));};
            Check(slice(coarse)>=a.z0&&slice(coarse)<=a.z1&&slice(fine)>=b.z0&&slice(fine)<=b.z1,"Every depth the lamp reaches is inside its slices");}
        const float coarseDepth=(a.z1-a.z0+1)/coarse.sliceScale,fineDepth=(b.z1-b.z0+1)/fine.sliceScale;
        Check(fineDepth<coarseDepth*.6f&&fineDepth<=2*220+4*ClusterFineSliceUnits,"Fine slices hold the lamp for little more than its own depth");
        Check(a.x0==b.x0&&a.x1==b.x1&&a.y0==b.y0&&a.y1==b.y1,"Fine slices leave the tiles alone");
        Light distant=bulb;distant.position.z=40000;
        const auto d=LightClusterExtent(ToRecord(distant,1),MakeClusterGrid(2560,1440,1,100000,128,128,ClusterFineSliceUnits),v,2560,1440);
        Check(d.z0==ClusterFineSliceLimit-1&&d.z1==ClusterFineSliceLimit-1,"A lamp past the last slice is in the last slice");
    }
    // Lit bounds: the box around what a lamp's static shadow depth lets it reach.
    {
        using Filmmaker::Environment::Matrix;
        Light lamp;lamp.position={100,200,300};lamp.range=400;lamp.lumens=800;
        std::vector<Matrix> faces(6);
        for(int f=0;f<6;++f){faces[f]=Filmmaker::Environment::LocalShadowMatrix(lamp.position,Filmmaker::Environment::PointShadowDirection(f),90,lamp.range);
            GuardStaticFace(faces[f].data());}
        const float margin=LitDepthMargin(.0002f);
        const auto depthAt=[&](float w){return 400.f/399.f*(1-1/w);}; // LocalShadowMatrix: near 1, far 400
        std::vector<float> blocks(6*LitFaceBlocks,1.f);
        const auto open=LampLitBounds(blocks.data(),faces.data(),6,lamp.position,margin);
        Check(open.valid&&open.low.x<=lamp.position.x-lamp.range&&open.low.y<=lamp.position.y-lamp.range&&open.low.z<=lamp.position.z-lamp.range&&
              open.high.x>=lamp.position.x+lamp.range&&open.high.y>=lamp.position.y+lamp.range&&open.high.z>=lamp.position.z+lamp.range,
              "An unoccluded lamp keeps its whole range");
        std::fill(blocks.begin(),blocks.end(),depthAt(80));
        const auto room=LampLitBounds(blocks.data(),faces.data(),6,lamp.position,margin);
        Check(room.valid&&room.high.x-lamp.position.x>80&&room.high.x-lamp.position.x<140&&lamp.position.z-room.low.z>80&&lamp.position.z-room.low.z<140,
              "A lamp in a closed room keeps the room, not its range");
        // A doorway in the +x wall: the box runs to the range that way and stays narrow behind.
        for(unsigned y=3;y<=4;++y)for(unsigned x=3;x<=4;++x)blocks[y*LitBlocks+x]=1;
        const auto door=LampLitBounds(blocks.data(),faces.data(),6,lamp.position,margin);
        Check(door.valid&&door.high.x>=lamp.position.x+399&&lamp.position.x-door.low.x<140&&door.high.y-lamp.position.y<290,
              "A doorway extends the box only where its light goes");
        blocks[5]=std::numeric_limits<float>::quiet_NaN();
        Check(LampLitBounds(blocks.data(),faces.data(),6,lamp.position,margin).valid,"A NaN texel counts as the far plane");
        Matrix flat{};Check(!LampLitBounds(blocks.data(),&flat,1,lamp.position,margin).valid,"A face that cannot be inverted keeps the range sphere");
        // Texel truth: per-face depth with random 16-texel blocks and per-texel noise. A receiver can
        // pass the lamp filter only if some texel within the guard band of its own is no nearer than
        // it (less the margin); every such receiver lies inside the box.
        uint32_t seed=12345;const auto random=[&]{seed=seed*1664525u+1013904223u;return float(seed>>8)/16777216.f;};
        std::vector<float> texels(size_t(6)*StaticSize*StaticSize),farthest(6*LitFaceBlocks,0.f);
        for(unsigned f=0;f<6;++f)for(unsigned by=0;by<LitBlocks;++by)for(unsigned bx=0;bx<LitBlocks;++bx){
            const float base=random()<.25f?1.f:depthAt(40+random()*300);
            for(unsigned j=0;j<LitBlockTexels;++j)for(unsigned i=0;i<LitBlockTexels;++i){
                const float value=(std::min)(1.f,base-random()*.01f);
                texels[(size_t(f)*StaticSize+by*LitBlockTexels+j)*StaticSize+bx*LitBlockTexels+i]=value;
                auto& block=farthest[f*LitFaceBlocks+by*LitBlocks+bx];block=(std::max)(block,value);
            }
        }
        const auto box=LampLitBounds(farthest.data(),faces.data(),6,lamp.position,margin);
        Check(box.valid,"Random depth yields a box");
        unsigned inside=0;
        for(unsigned sample=0;sample<60000;++sample){
            const unsigned f=sample%6;Matrix inverse{};Check(Filmmaker::Environment::Inverse(faces[f],inverse),"face inverse");
            const int tx=int(random()*StaticSize)%int(StaticSize),ty=int(random()*StaticSize)%int(StaticSize);
            float reachable=0;
            for(int j=-int(FaceGuardTexels);j<=int(FaceGuardTexels);++j)for(int i=-int(FaceGuardTexels);i<=int(FaceGuardTexels);++i){
                const int x=tx+i,y=ty+j;if(x<0||y<0||x>=int(StaticSize)||y>=int(StaticSize))continue;
                reachable=(std::max)(reachable,texels[(size_t(f)*StaticSize+y)*StaticSize+x]);}
            const float depth=(std::min)(1.f,reachable+margin)*random();
            const Vec3 p=Filmmaker::Environment::Transform(inverse,{-1+2*(tx+random())/StaticSize,1-2*(ty+random())/StaticSize,depth});
            const bool in=p.x>=box.low.x&&p.x<=box.high.x&&p.y>=box.low.y&&p.y<=box.high.y&&p.z>=box.low.z&&p.z<=box.high.z;
            Check(in,"Every receiver the static depth can light lies inside the lit bounds");++inside;
        }
        Check(inside==60000&&(box.high.x-box.low.x)<2.6f*lamp.range,"The box is no larger than the guarded range");
        // In the light lists the box only removes tiles and slices, and keeps every point of itself.
        ClusterView v{};v[12][0]=1;v[13][1]=1;v[14][3]=1;
        const auto g=MakeClusterGrid(2560,1440,1,10000,128,128,ClusterFineSliceUnits);
        Light bulb;bulb.position={40,-30,900};bulb.range=220;bulb.lumens=500;const auto r=ToRecord(bulb,1);
        const LitBounds cut{{-20,-90,860},{120,10,1000},true};
        const auto full=LightClusterExtent(r,g,v,2560,1440),tight=LightClusterExtent(r,g,v,2560,1440,0,true,&cut);
        Check(tight.x0>=full.x0&&tight.x1<=full.x1&&tight.y0>=full.y0&&tight.y1<=full.y1&&tight.z0>=full.z0&&tight.z1<=full.z1,"Lit bounds never enlarge a light's lists");
        Check(tight.z1-tight.z0<full.z1-full.z0&&(tight.x1-tight.x0)*(tight.y1-tight.y0)<(full.x1-full.x0)*(full.y1-full.y0),"Lit bounds remove slices and tiles");
        for(int z=860;z<=1000;z+=7)for(int y=-90;y<=10;y+=5)for(int x=-20;x<=120;x+=5){
            const Vec3 p{float(x),float(y),float(z)},d=p-bulb.position;
            if(Filmmaker::Environment::Dot(d,d)>220.f*220.f)continue;
            const float w=p.z;
            const auto tx=unsigned(std::clamp((p.x/w+1)*1280,0.f,2559.f))/32,ty=unsigned(std::clamp((1-p.y/w)*720,0.f,1439.f))/32;
            const auto slice=unsigned(std::clamp(w*g.sliceScale,0.f,float(g.sliceCount-1)));
            Check(tx>=tight.x0&&tx<=tight.x1&&ty>=tight.y0&&ty<=tight.y1&&slice>=tight.z0&&slice<=tight.z1,"Every lit point inside the box keeps the light");
        }
        const LitBounds away{{5000,5000,5000},{5100,5100,5100},true};
        const auto none=LightClusterExtent(r,g,v,2560,1440,0,true,&away);
        Check(none.x0>none.x1&&none.z0>none.z1&&!none.allTiles,"A lamp whose box misses its reach is in no list");
        const LitBounds invalid{{-20,-90,860},{120,10,1000},false};
        const auto same=LightClusterExtent(r,g,v,2560,1440,0,true,&invalid);
        Check(!std::memcmp(&same,&full,sizeof(full)),"Invalid bounds change nothing");
        const auto sunExtent=LightClusterExtent(SunRecord({0,0,1},{1,1,1},nullptr,false),g,v,2560,1440,0,true,&cut);
        Check(sunExtent.allTiles&&sunExtent.z0==0&&sunExtent.z1==g.sliceCount-1,"The sun ignores lit bounds");
    }
    std::cout<<"Relight cluster addressing and conservative bounds passed\n";
}
