#include "../../AfxHookSource2/Filmmaker/Relight/RelightLiteGridMath.h"
#include <cstdlib>
#include <iostream>
using namespace Filmmaker::Relight;
using Filmmaker::Environment::OcclusionMesh;
using Filmmaker::Environment::SurfaceRayGrid;
void Check(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
bool Near(float a,float b,float e=.001f){return std::abs(a-b)<e;}
float Luma(Vec3 v){return .2125f*v.x+.7154f*v.y+.0721f*v.z;}
const Vec3 grey{.55f,.55f,.55f};
// An axis-aligned quad, wound so its face normal is `normal` (one of the six axes).
void Quad(OcclusionMesh& m,Vec3 corner,Vec3 u,Vec3 v,Vec3 normal){
    const uint32_t base=uint32_t(m.positions.size());
    m.positions.insert(m.positions.end(),{corner,corner+u,corner+u+v,corner+v});
    const Vec3 n=Filmmaker::Environment::Cross(u,v);
    const bool flip=Dot(n,normal)<0;
    for(uint32_t i:flip?std::initializer_list<uint32_t>{0,2,1,0,3,2}:std::initializer_list<uint32_t>{0,1,2,0,2,3})m.indices.push_back(base+i);
}
// Small enough that every probe's rays reach a wall inside LiteGridRayRange.
// A room: floor at z=0, ceiling at z=256, walls at x,y = 0 and 384, every face looking inward.
// `door` leaves the +X wall open.
OcclusionMesh Room(bool door,bool ceiling=true){
    OcclusionMesh m;const float s=384,h=256;
    Quad(m,{0,0,0},{s,0,0},{0,s,0},{0,0,1});
    if(ceiling)Quad(m,{0,0,h},{s,0,0},{0,s,0},{0,0,-1});
    Quad(m,{0,0,0},{0,s,0},{0,0,h},{1,0,0});
    if(!door)Quad(m,{s,0,0},{0,s,0},{0,0,h},{-1,0,0});
    Quad(m,{0,0,0},{s,0,0},{0,0,h},{0,1,0});
    Quad(m,{0,s,0},{s,0,0},{0,0,h},{0,-1,0});
    m.low={0,0,0};m.high={s,s,h};
    return m;
}
struct Scene3 {
    OcclusionMesh mesh;SurfaceRayGrid rays;LiteGridBake bake;
    LiteRayMesh Mesh() const {return {&mesh.positions,&mesh.indices,&rays};}
    explicit Scene3(OcclusionMesh m,Vec3 low,Vec3 high):mesh(std::move(m)){
        Check(Filmmaker::Environment::BuildSurfaceRayGrid(mesh,rays),"The ray grid builds");
        bake.Resize(MakeLiteGridLayout(low,high));
        LiteGridTrace(Mesh(),bake,0,bake.layout.Cells());
    }
    Vec3 Band(const std::vector<Vec3>& bands,Vec3 p,unsigned band) const {return bands[bake.layout.Nearest(p)*6+band];}
};
int main(){
    {
        // Layout: probes where PackProbe's texel centres are, never more than the caps.
        const auto g=MakeLiteGridLayout({-1000,-500,0},{1000,500,300});
        Check(g.nx==32&&g.ny==16&&g.nz==5,"Probes are about 64 units apart");
        const auto pack=PackProbe(g.low,g.high,g.nx,g.ny,g.nz);
        const Vec3 c=g.Centre(3,7,2);
        const float u=c.x*pack.volume[0][0]+pack.volume[0][3],w=c.z*pack.volume[2][2]+pack.volume[2][3];
        Check(Near(u,3.5f/g.nx,1e-5f)&&Near(w,2.5f/g.nz,1e-5f),"A probe sits at its texel centre in the packed volume");
        Check(Near(pack.volume[3][0],.5f/g.nx,1e-6f)&&Near(pack.volume[3][2],.5f/g.nz/6,1e-6f),"Each axis clamps to its own half texel");
        const auto huge=MakeLiteGridLayout({-20000,-20000,-4000},{20000,20000,4000});
        Check(huge.nx==LiteGridMaxWide&&huge.nz==LiteGridMaxTall,"A huge map widens its cells instead of growing the grid");
        Check(g.Nearest(c)==g.Index(3,7,2)&&g.Nearest({-5000,0,0})==g.Index(0,8,0),"Nearest probe, clamped to the grid");
        // Directions: unit length, and a uniform surrounding gives every band exactly that value.
        const auto norms=LiteBandNorms();
        for(unsigned i=0;i<LiteGridDirections;++i){const auto d=LiteGridDirection(i);Check(Near(Dot(d,d),1),"Directions are unit length");}
        for(unsigned b=0;b<6;++b)Check(norms[b]>LiteGridDirections*.2f&&norms[b]<LiteGridDirections*.3f,"Each band sees about a quarter of the sphere's cosine");
    }
    {
        // Rays: nearest hit, facing, and the first-hit form shadows use.
        Scene3 s(Room(false),{0,0,0},{384,384,256});
        LiteRayHit hit;
        Check(LiteRay(s.Mesh(),{192,192,128},{1,0,0},4096,&hit)&&Near(hit.distance,192)&&!hit.back&&Near(hit.normal.x,-1),"A ray finds the wall it faces");
        Check(LiteRay(s.Mesh(),{192,192,400},{0,0,-1},4096,&hit)&&Near(hit.distance,144)&&hit.back,"From outside, the ceiling shows its back");
        Check(LiteRay(s.Mesh(),{192,192,128},{0,0,1},4096,nullptr)&&!LiteRay(s.Mesh(),{192,192,128},{0,0,1},100,nullptr),"A shadow ray stops at its range");
        Check(!LiteRay(s.Mesh(),{192,192,400},{0,0,1},4096,nullptr),"Nothing above the roof");
    }
    {
        // Lamp record: range window, inverse square, and facing.
        Light bulb;bulb.kind="light_omni2";bulb.position={192,192,200};bulb.lumens=1000;bulb.range=400;
        LiteLamp lamp;Check(MakeLiteLamp(bulb,1,lamp)&&Near(lamp.reach,400,.01f),"A lamp keeps its authored range");
        Vec3 toward;float distance;
        const auto under=LiteLampRadiance(lamp,{192,192,0},{0,0,1},toward,distance);
        Check(under.x>0&&Near(distance,200)&&Near(toward.z,1),"The floor under a lamp is lit");
        Check(LiteLampRadiance(lamp,{192,192,0},{0,0,-1},toward,distance).x==0,"A face turned away gets nothing");
        Check(LiteLampRadiance(lamp,{256,256+500,200},{0,-1,0},toward,distance).x==0,"Nothing past the range");
        const auto far=LiteLampRadiance(lamp,{192,192,50},{0,0,1},toward,distance);
        Check(far.x>under.x,"Closer is brighter");
        Light off=bulb;off.enabled=false;Light fire=bulb;fire.transient=true;
        Check(!MakeLiteLamp(off,1,lamp)&&!MakeLiteLamp(fire,1,lamp),"Disabled and effect lights are left out");
        // A rect fill light aimed down: it lights the floor inside its opening's projection only.
        Light fill;fill.kind="light_rect";fill.position={192,192,200};fill.angles={90,0,0};fill.lumens=400;fill.range=300;
        fill.sizeX=fill.sizeY=32;fill.sizeZ=.05f;
        LiteLamp panel;Check(MakeLiteLamp(fill,1,panel)&&panel.fixture,"A shaped fill light is kept, as a projection");
        const auto below=LiteLampRadiance(panel,{192,192,40},{0,0,1},toward,distance);
        Check(below.x>0&&toward.z>.9f&&distance>100&&distance<200,"The floor under a fill light is lit, and its shadow ray ends at the opening");
        Check(LiteLampRadiance(panel,{192,192,260},{0,0,-1},toward,distance).x==0,"Nothing behind a fill light");
        Check(LiteLampRadiance(panel,{192+2000,192,40},{0,0,1},toward,distance).x==0,"Nothing outside its projection");
        const std::vector<LiteLamp> lamps{[&]{LiteLamp l;MakeLiteLamp(bulb,1,l);return l;}()};
        const auto bins=BinLiteLamps(lamps,{0,0,0},{2048,2048,512});
        size_t count=0;bins.Begin({192,192,100},count);Check(count==1,"A lamp is listed where it reaches");
        bins.Begin({1900,1900,100},count);Check(count==0,"and not across the map");
    }
    {
        // A closed room with one lamp: the lamp's bounce fills the room, with no sky and no sun.
        Scene3 s(Room(false),{0,0,0},{384,384,256});
        Check(s.bake.sky[s.bake.layout.Nearest({192,192,128})*6+2]==0,"A closed room sees no sky");
        Light bulb;bulb.kind="light_omni2";bulb.position={192,192,200};bulb.lumens=1000;bulb.range=600;
        std::vector<LiteLamp> lamps(1);MakeLiteLamp(bulb,1,lamps[0]);
        const auto bins=BinLiteLamps(lamps,{0,0,0},{384,384,256});
        LiteGridLamps(s.Mesh(),s.bake,lamps,bins,grey,0,s.bake.layout.Cells());
        LiteGridColours colours;colours.sky={.2f,.3f,.5f};colours.sun={3,3,3};colours.albedo=grey;
        const auto lit=LiteGridCompose(s.bake,colours);
        const Vec3 corner{40,40,40};
        Check(Luma(s.Band(lit,corner,2))>0&&Luma(s.Band(lit,corner,5))>0,"A corner the lamp barely reaches is filled from every side");
        colours.lampBounce=colours.sunBounce=colours.feedback=0;
        Check(Luma(s.Band(LiteGridCompose(s.bake,colours),{192,192,128},2))==0,"Bounce strength zero leaves a closed room dark");
        colours.lampBounce=colours.sunBounce=colours.feedback=1;
        // The second bounce only adds.
        const size_t at=s.bake.layout.Nearest(corner)*6+5;
        Check(Luma(lit[at])>Luma(s.bake.lamps[at]),"Light the grid holds bounces once more");
        // A wall between the lamp and half the room: the far half gets far less.
        OcclusionMesh split=Room(false);
        Quad(split,{192,0,0},{0,384,0},{0,0,256},{1,0,0});Quad(split,{191,0,0},{0,384,0},{0,0,256},{-1,0,0});
        Scene3 d(std::move(split),{0,0,0},{384,384,256});
        Light side=bulb;side.position={300,192,200};
        std::vector<LiteLamp> sideLamps(1);MakeLiteLamp(side,1,sideLamps[0]);
        LiteGridLamps(d.Mesh(),d.bake,sideLamps,BinLiteLamps(sideLamps,{0,0,0},{384,384,256}),grey,0,d.bake.layout.Cells());
        colours.sky={}; // a corner probe is further than a ray reaches from the far walls
        const auto divided=LiteGridCompose(d.bake,colours);
        Check(Luma(d.Band(divided,{100,192,100},2))==0&&Luma(d.Band(divided,{300,192,100},5))>0,"A lamp does not fill the room behind a wall");
    }
    {
        // Outdoors: an open yard sees the sky above, and the sun on its floor lights what faces down.
        Scene3 s(Room(false,false),{0,0,0},{384,384,256});
        const Vec3 p{192,192,100};const size_t cell=s.bake.layout.Nearest(p);
        Check(s.bake.sky[cell*6+2]>.4f&&s.bake.sky[cell*6+2]<=1.001f&&s.bake.sky[cell*6+5]==0,"The open top is sky; the floor is not");
        LiteGridSun(s.Mesh(),s.bake,{0,0,1},grey,0,s.bake.layout.Cells());
        Check(s.bake.sun[cell*6+5].x>.5f*grey.x&&s.bake.sun[cell*6+2].x==0,"A sun overhead lights the floor, which lights from below");
        const Vec3 low=Filmmaker::Environment::Normalize({1,0,.2f});
        auto evening=s.bake;LiteGridSun(s.Mesh(),evening,low,grey,0,evening.layout.Cells());
        Check(evening.sun[cell*6+5].x<s.bake.sun[cell*6+5].x,"A low sun, mostly behind the wall, lights the floor less");
        // The same yard with a roof: the sun reaches nothing.
        Scene3 roofed(Room(false),{0,0,0},{384,384,256});
        LiteGridSun(roofed.Mesh(),roofed.bake,{0,0,1},grey,0,roofed.bake.layout.Cells());
        for(const auto& v:roofed.bake.sun)Check(v.x==0,"No sun under a roof");
        // Sky light on the floor comes back up: a face looking down is lit outdoors without sun.
        LiteGridColours colours;colours.sky={.2f,.3f,.5f};colours.sun={};
        std::fill(s.bake.sun.begin(),s.bake.sun.end(),Vec3{});
        const auto shade=LiteGridCompose(s.bake,colours);
        Check(Luma(shade[cell*6+5])>0&&Luma(shade[cell*6+5])<Luma(shade[cell*6+2]),"The sky-lit floor lights from below, less than the sky lights from above");
    }
    {
        // A probe inside a solid block takes its open neighbours' light instead of black.
        OcclusionMesh m=Room(false,false);
        const Vec3 a{128,128,0},b{256,256,128}; // a 128-unit block, faces outward
        Quad(m,{a.x,a.y,b.z},{128,0,0},{0,128,0},{0,0,1});Quad(m,{a.x,a.y,a.z},{0,128,0},{0,0,128},{-1,0,0});
        Quad(m,{b.x,a.y,a.z},{0,128,0},{0,0,128},{1,0,0});Quad(m,{a.x,a.y,a.z},{128,0,0},{0,0,128},{0,-1,0});
        Quad(m,{a.x,b.y,a.z},{128,0,0},{0,0,128},{0,1,0});
        Scene3 s(std::move(m),{0,0,0},{384,384,256});
        const size_t inside=s.bake.layout.Nearest({160,160,32}),open=s.bake.layout.Nearest({320,320,96});
        Check(s.bake.inside[inside]==1&&s.bake.inside[open]==0,"A probe in the block is recognised; one in the yard is not");
        LiteGridColours colours;colours.sky={.2f,.3f,.5f};
        const auto bands=LiteGridCompose(s.bake,colours);
        Check(Luma(bands[inside*6+2])>0,"The probe in the block holds its neighbours' light");
        // Texels: band b in slices [b*nz,(b+1)*nz), alpha one.
        const auto texels=LiteGridTexels(s.bake.layout,bands);
        const size_t cells=s.bake.layout.Cells();
        Check(texels.size()==cells*6*4&&texels[3]==0x3c00,"RGBA16F with alpha one");
        Check(texels[(2*cells+open)*4+2]==LiteHalf(bands[open*6+2].z)&&LiteHalf(1)==0x3c00&&LiteHalf(.5f)==0x3800&&LiteHalf(0)==0,"Bands are stacked along W as halves");
    }
    {
        // Surface colour: a red floor under an overhead sun sends red light up, and the bounce keeps it.
        OcclusionMesh m=Room(false,false);
        std::vector<uint32_t> albedo(m.indices.size()/3,0x00808080u);albedo[0]=albedo[1]=0x000000ffu; // the floor's two triangles
        SurfaceRayGrid rays;Check(Filmmaker::Environment::BuildSurfaceRayGrid(m.positions,m.indices,rays),"The ray grid builds from plain arrays");
        LiteGridBake bake;bake.Resize(MakeLiteGridLayout({0,0,0},{384,384,256}));
        const LiteRayMesh mesh{&m.positions,&m.indices,&rays,&albedo};
        LiteGridTrace(mesh,bake,0,bake.layout.Cells());
        LiteGridSun(mesh,bake,{0,0,1},grey,0,bake.layout.Cells());
        const size_t cell=bake.layout.Nearest({192,192,100});
        Check(bake.sun[cell*6+5].x>.5f&&bake.sun[cell*6+5].y==0,"A red floor in the sun lights from below in red");
        LiteGridHit hit;hit.albedo=PackLiteAlbedo(0x00ff8000u);
        const auto colour=hit.Albedo(grey);Check(colour.x==0&&Near(colour.y,.5f,.02f)&&colour.z==1,"Albedo packs as R5G6B5, red in the low byte");
        hit.albedo=LiteHitNoAlbedo;Check(hit.Albedo(grey).x==grey.x&&PackLiteAlbedo(0x00fffbffu)!=LiteHitNoAlbedo,"No colour known takes the fallback; no real colour packs to that code");
    }
    {
        // Cloth: a roof the map marked as light-transmitting is still a roof (no sky comes through),
        // but with the sun on top of it the probe under it is lit in the roof's colour. A solid
        // roof of the same colour sends nothing down.
        const auto under=[&](bool cloth,float* sky){
            OcclusionMesh m;Quad(m,{0,0,0},{384,0,0},{0,384,0},{0,0,1});
            Quad(m,{0,0,160},{384,0,0},{0,384,0},{0,0,-1});
            std::vector<uint32_t> albedo(m.indices.size()/3,0x00808080u);
            albedo[2]=albedo[3]=0x0000ff00u|(cloth?Filmmaker::MapData::kRigidTransmissive:0u);
            SurfaceRayGrid rays;Check(Filmmaker::Environment::BuildSurfaceRayGrid(m.positions,m.indices,rays),"The roofed scene's ray grid builds");
            LiteGridBake bake;bake.Resize(MakeLiteGridLayout({0,0,0},{384,384,256}));
            const LiteRayMesh mesh{&m.positions,&m.indices,&rays,&albedo};
            LiteGridTrace(mesh,bake,0,bake.layout.Cells());
            LiteGridSun(mesh,bake,{0,0,1},grey,0,bake.layout.Cells());
            const size_t cell=bake.layout.Nearest({192,192,96});
            *sky=bake.sky[cell*6+4];
            return bake.sun[cell*6+4];
        };
        float solidSky=0,clothSky=0;
        const Vec3 solid=under(false,&solidSky),cloth=under(true,&clothSky);
        Check(solid.x==0&&solid.y==0&&solid.z==0,"A solid roof in the sun sends nothing down to the probe under it");
        Check(cloth.y>.25f&&cloth.x==0&&cloth.z==0,"A cloth roof in the sun lights the probe under it in its own colour");
        Check(Near(clothSky,solidSky,.001f),"Cloth is still a roof: it lets no more sky through than a solid one");
    }
    {
        const auto c=MakeLiteGridColours({1,1,1},{2,2,2},grey,2);
        Check(Near(c.lampBounce,LiteGridLampBounce*2)&&Near(c.sunBounce,LiteGridSunBounce*2)&&Near(c.feedback,LiteGridFeedback*2),"The one dial scales every bounce");
        Check(MakeLiteGridColours({},{},grey,-1).lampBounce==0,"A negative dial is no bounce");
    }
    std::cout<<"Lite bounce grid math passed\n";
}
