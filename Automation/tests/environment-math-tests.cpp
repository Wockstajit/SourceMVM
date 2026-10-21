#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentWeatherMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentSurfaceMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentCoatingMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentGiMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentScene.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentStorm.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentCasterMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShadowMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShadowSpatialMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentFoliageMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentSkyVisibilityMath.h"
#include <iostream>
#include <cstdlib>
#include <limits>
#include <vector>
#include <utility>

using namespace Filmmaker::Environment;
void Check(bool value, const char *message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
bool Near(float a, float b, float epsilon = .001f) { return std::abs(a - b) < epsilon; }
int main() {
    Check(ClimateValueValid(CL_bounceSteps,1)&&ClimateValueValid(CL_bounceSteps,3)&&
          !ClimateValueValid(CL_bounceSteps,0)&&!ClimateValueValid(CL_bounceSteps,4)&&
          !ClimateValueValid(CL_bounceSteps,1.5),"Bounce step count accepts only whole values from one to three");
    {
        std::vector<ShadowRefreshCandidate> faces;
        for(unsigned i=0;i<600;++i)faces.push_back({i,0,i<150?1.f:0.f,i<150,false});
        std::vector<unsigned> visits(600);
        for(uint64_t frame=1;frame<=48;++frame){
            const auto selected=SelectShadowRefresh(faces,frame,128,16);
            Check(selected.size()==128,"Animated shadow work stays within its frame budget");
            if(frame==1)Check(selected[0]<150,"Nearby visible receivers get priority");
            std::vector<bool> seen(600);
            for(unsigned i:selected){Check(!seen[i],"A face is refreshed at most once per frame");seen[i]=true;++visits[i];faces[i].last=frame;}
        }
        for(unsigned v:visits)Check(v>0,"Background shadow refresh cannot starve distant lights");
        Check(SelectShadowRefresh(faces,49,0).empty(),"A zero work budget schedules no faces");
        for(auto& face:faces){face.last=0;face.visible=false;face.realtime=face.face<42;}
        std::fill(visits.begin(),visits.end(),0);
        for(uint64_t frame=1;frame<=48;++frame){
            const auto selected=SelectShadowRefresh(faces,frame,32,16);
            Check(selected.size()==58,"Foreground demand expands work without consuming the background reserve");
            for(unsigned i:selected){++visits[i];faces[i].last=frame;}
            for(unsigned i=0;i<42;++i)Check(std::find(selected.begin(),selected.end(),i)!=selected.end(),
                "Every nearby visible cached face receives the current animated pose, including outside-room lights");
        }
        for(unsigned v:visits)Check(v>0,"Pinned foreground faces cannot starve distant cached shadows");
        faces.resize(3);Check(SelectShadowRefresh(faces,49,128).size()==3,"Small shadow sets do not waste the budget");
        {
            // 300 dynamic faces, as on a planted map: foliage everywhere, actors in a few.
            std::vector<ShadowRefreshCandidate> planted(300);
            for(unsigned i=0;i<300;++i){planted[i].face=i;planted[i].visible=true;planted[i].proximity=float(i%7)/7;}
            for(unsigned i:{250u,251u,252u,299u}){planted[i].actor=true;planted[i].last=1000;} // refreshed last frame
            for(uint64_t frame=1001;frame<=1010;++frame){
                const auto selected=SelectShadowRefresh(planted,frame,32,16,48);
                for(unsigned i:{250u,251u,252u,299u})
                    Check(std::find(selected.begin(),selected.end(),i)!=selected.end(),
                          "Visible faces with a moving actor refresh every frame, ahead of older foliage faces");
                Check(selected.size()==32,"Actor faces do not consume the background reserve");
                for(unsigned i:selected)planted[i].last=frame;
            }
            planted[250].visible=false;
            const auto hidden=SelectShadowRefresh(planted,1011,32,16,48);
            Check(hidden.size()==32,"An off-screen actor face falls back to the shared rotation");
            std::vector<ShadowRefreshCandidate> crowd(100);
            for(unsigned i=0;i<100;++i){crowd[i].face=i;crowd[i].visible=true;crowd[i].actor=true;crowd[i].proximity=float(i)/100;}
            const auto capped=SelectShadowRefresh(crowd,1,32,16,48);
            Check(std::find(capped.begin(),capped.end(),99u)!=capped.end()&&capped.size()==64,
                  "The actor tier is capped, nearest first, and still leaves the background reserve");
        }
    }
    {
        // Runtime sky visibility: an open square sees the whole sky, a sealed room none, and
        // an arcade (roof overhead, open to one side) sits between, brighter on the open wall.
        const auto directions=SkyVisibilityDirectionSet();Vec3 total{};
        for(const auto& d:directions){Check(Near(Dot(d,d),1)&&d.z>.3f,"sky directions are unit and above the horizon");total=total+d;}
        Check(std::abs(total.x)<.01f&&std::abs(total.y)<.01f,"sky directions are balanced in azimuth");
        std::array<float,SkyVisibilityDirections> open{},sealed{},arcade{};
        for(unsigned i=0;i<SkyVisibilityDirections;++i){open[i]=1;arcade[i]=directions[i].z<.5f&&directions[i].x>.3f?1.f:0.f;}
        const Vec3 up{0,0,1},facingOpening{1,0,0},facingAway{-1,0,0};
        Check(Near(SkyVisibility(up,open),1)&&Near(SkyVisibility(facingOpening,open),1),"open ground receives the whole sky");
        Check(Near(SkyVisibility(up,sealed),0),"a sealed interior receives no sky light");
        const float floor=SkyVisibility(up,arcade),wall=SkyVisibility(facingOpening,arcade),back=SkyVisibility(facingAway,arcade);
        Check(floor>.02f&&floor<.4f,"an arcade floor sees only the low sky through its opening");
        Check(wall>floor&&wall>.3f,"a wall facing the opening sees more sky than the floor");
        Check(back<.05f,"a wall facing away from the opening stays dark");
        Check(Near(ApplySkyVisibility(.8f,0,0),.8f)&&Near(ApplySkyVisibility(.8f,.25f,1),.2f)&&Near(ApplySkyVisibility(.8f,.5f,.5f),.6f),
              "sky visibility scales the ambient by its strength");
    }
    {
        CasterAnimation foliage;foliage.sway[0]=.04f;foliage.flutter[0]=.08f;
        const Vec3 wind{.2f,-.4f,.03f};const float strength=.15f;
        const float radius=FoliageDisplacementRadius(foliage,wind,strength);
        Check(radius<2,"Ordinary foliage motion needs a tight bound rather than a 256-unit margin");
        unsigned seed=17;auto random=[&](){seed=seed*1664525u+1013904223u;return float(seed>>8)/16777215.f;};
        for(int i=0;i<5000;++i){
            foliage.sway[3]=i%3==0?-.25f:1.7f;foliage.flutter[3]=i%3==1?-.4f:2.f;
            const float r=FoliageDisplacementRadius(foliage,wind,strength);
            const Vec3 base{random()-.5f,random()-.5f,0};const Vec3 n{base.x,base.y,base.y*.5f};
            float gust=random();gust=std::clamp(gust*gust*1.5f,0.f,1.f);
            Vec3 sway=(n*gust+wind)*(std::pow(std::clamp(random(),.001f,1.f),foliage.sway[3])*strength*150*foliage.sway[0]*gust);
            Vec3 flutterNoise{random()*2-1,random()*2-1,random()*2-1};
            Vec3 flutter{flutterNoise.x*(n.x+wind.x),flutterNoise.y*(n.y+wind.y),flutterNoise.z*(n.z+wind.z)};
            flutter=flutter*(strength*25*foliage.flutter[0]*std::pow(std::clamp(random(),.001f,1.f),foliage.flutter[3]));
            Vec3 moved=sway+flutter;moved.z-=.314f*(std::sqrt(sway.x*sway.x+sway.y*sway.y)+std::sqrt(flutter.x*flutter.x+flutter.y*flutter.y));
            Check(std::abs(moved.x)<=r&&std::abs(moved.y)<=r&&std::abs(moved.z)<=r,"Native gust/flutter motion, including negative weight falloff, remains inside the proven bound");
        }
        Check(FoliageDisplacementRadius(foliage,wind,0)<.01f,"Still wind does not expand foliage by hundreds of units");
        foliage.flutter[0]=std::numeric_limits<float>::quiet_NaN();
        Check(!std::isfinite(FoliageDisplacementRadius(foliage,wind,strength)),"Malformed deformation keeps conservative shadow draws");
    }
    {
        const std::vector<Vec3> vertices{{-900,-10,0},{-880,10,1},{-880,-10,1},
            {900,-10,0},{880,10,1},{880,-10,1}, {-2000,0,0},{2000,0,0},{0,1,1}};
        const std::vector<uint32_t> triangles{0,1,2,3,4,5,6,7,8};
        const auto spatial=PartitionShadowIndices(triangles,[&](uint32_t i){return vertices[i];});
        Check(spatial.ranges.size()==3&&spatial.indices.size()==triangles.size(),"Spatial shadows preserve all separated and spanning triangles");
        auto sorted=spatial.indices;std::sort(sorted.begin(),sorted.end());
        Check(sorted==triangles,"Spatial partition loses no vertex indices");
        ShadowFrustum identity(Identity());unsigned visible=0;
        for(const auto& range:spatial.ranges)visible+=identity.Intersects(range.low,range.high);
        Check(visible==1,"Spanning triangle remains visible although its vertices lie outside the frustum");
        Check(identity.Intersects({-2,-2,-2},{2,2,2}),"Frustum enclosed by geometry remains visible");
        Check(!identity.Intersects({2,0,.2f},{3,.5f,.8f}),"Offscreen shadow geometry is rejected");
        Check(identity.Intersects({0,0,-3},{.5f,.5f,-1}),"Orthographic (sun) shadows keep casters nearer than the depth interval: they are pancaked");
        Matrix perspective=Identity();perspective[12]=0;perspective[14]=1;perspective[15]=0; // w = z
        Check(!ShadowFrustum(perspective).Intersects({0,0,-3},{.5f,.5f,-1}),"Shadow near plane rejects geometry behind a local light");
        Check(!identity.Intersects({0,0,2},{.5f,.5f,3}),"Shadow far plane rejects geometry past range");
        Check(identity.Intersects({2,0,.2f},{3,.5f,.8f},2),"Animated padding preserves geometry moving into a shadow face");
        for(int face=0;face<6;++face){
            const Vec3 light{10,20,30},p=light+PointShadowDirection(face)*100;
            ShadowFrustum cube(LocalShadowMatrix(light,PointShadowDirection(face),90,400));
            Check(cube.Intersects(p-Vec3{1,1,1},p+Vec3{1,1,1}),"All point-light shadow faces retain their center geometry");
            const Vec3 behind=light-PointShadowDirection(face)*100;
            Check(!cube.Intersects(behind-Vec3{1,1,1},behind+Vec3{1,1,1}),"All point-light shadow faces reject geometry behind the light");
        }
    }
    {
        Climate sun;sun[CL_sunPower]=8;
        for(float elevation:{-30.f,-12.f,-5.f,-.01f,0.f}){
            sun[CL_sunElevation]=elevation;
            auto radiance=ClimateSunRadiance(sun);
            Check(ClimateSunVisibility(sun)==0&&radiance.x==0&&radiance.y==0&&radiance.z==0,"Below-horizon sun contributes no direct/GI energy");
        }
        sun[CL_sunElevation]=.5f;Check(Near(ClimateSunVisibility(sun),.5f),"Sunrise direct energy fades continuously");
        sun[CL_sunElevation]=35;Check(ClimateSunVisibility(sun)==1&&ClimateSunRadiance(sun).x==8,"Daylight intensity is unchanged");
    }

    {
        for (ClimateField field : {CL_sunAzimuth, CL_sunElevation, CL_sunPower}) {
            Climate c;c[CL_enabled]=0;c[CL_sky]=0;c[CL_relighting]=0;c[CL_relight]=0;
            CoupleSunToLighting(c,field);
            Check(c[CL_enabled]==1&&c[CL_sky]==1&&c[CL_relighting]==1&&Near(c[CL_relight],0),
                "Sun controls activate connected lighting without changing zero contrast flattening");
            c[CL_relight]=.27f;CoupleSunToLighting(c,field);
            Check(Near(c[CL_relight],.27f),"Sun changes preserve an authored nonzero lighting blend");
        }
        Climate c;c[CL_enabled]=0;c[CL_sky]=0;c[CL_relighting]=0;
        CoupleSunToLighting(c,CL_ambient);
        Check(c[CL_enabled]==0&&c[CL_sky]==0&&c[CL_relighting]==0,"Unrelated controls do not activate sun coupling");
    }
    {
        Climate saved;
        saved[CL_enabled]=1;saved[CL_relighting]=1;saved[CL_sky]=1;
        saved[CL_mapLightPower]=3.5f;saved[CL_ambient]=.4f;saved[CL_bounceSteps]=3;
        saved[CL_contactShadows]=1;saved[CL_worldGi]=1;saved[CL_sunRelight]=.8f;
        saved[CL_fogDensity]=.25f;saved[CL_clouds]=0;saved[CL_bloom]=.4f;saved[CL_saturation]=1.3f;
        int weather=1;std::map<std::string,float> held;held["snow"]=.6f;
        HoldRestoredPasses(saved,weather,held);
        Check(saved[CL_relighting]==0&&saved[CL_sky]==0&&saved[CL_worldGi]==0&&saved[CL_fogDensity]==0&&
              weather==0&&saved[CL_enabled]==0,"Reopening a demo starts every pass off, weather included");
        Check(held["relighting"]==1&&held["sky"]==1&&held["worldGi"]==1&&Near(held["fogDensity"],.25f)&&
              held["weatherType"]==1,"Each pass that was on is held with its value");
        Check(!held.count("clouds"),"A pass that was already off is not held");
        Check(Near(held["snow"],.6f)&&saved[CL_snow]==0,"A value held by an earlier restore stays held");
        Check(saved[CL_mapLightPower]==3.5f&&saved[CL_ambient]==.4f&&saved[CL_bounceSteps]==3&&
              saved[CL_contactShadows]==1&&saved[CL_sunRelight]==.8f,
              "Settings that only tune a pass are kept as they were");
        Check(Near(saved[CL_bloom],.4f)&&Near(saved[CL_saturation],1.3f)&&!held.count("bloom"),
              "The grade survives a restart untouched");
        Scene stored;stored.climate=saved;stored.held=held;Scene back;std::string error;
        Check(ParseScene(SerializeScene(stored),back,error)&&back.held==held,"Held values survive the autosave");
        // Keep between demos onto a map with no saved scene: the look travels, the map's anchors do not.
        Scene carried=stored;carried.map="de_inferno";carried.objects.resize(2);carried.nextObjectId=9;
        carried.weather.follow=false;carried.climate[CL_localFogRadius]=600;carried.climate[CL_surfaceRadius]=600;
        carried.climate[CL_puddles]=.5f;carried.held["storm"]=1;
        UnbindSceneFromMap(carried);
        Check(carried.map.empty()&&carried.objects.empty()&&carried.nextObjectId==1&&carried.weather.follow,
              "Another map drops the old map's objects and fixed weather centre");
        Check(carried.climate[CL_localFogRadius]==0&&carried.climate[CL_surfaceRadius]==0&&carried.climate[CL_puddles]==0,
              "Placed volumes become global or go");
        Check(!carried.held.count("worldGi")&&!carried.held.count("storm")&&held.count("worldGi"),
              "Map-anchored passes are not held onto another map");
        Check(carried.held["relighting"]==1&&carried.held["sky"]==1&&Near(carried.held["fogDensity"],.25f)&&
              carried.held["weatherType"]==1&&Near(carried.held["snow"],.6f),"Every global pass stays held");
        Check(carried.climate[CL_mapLightPower]==3.5f&&Near(carried.climate[CL_bloom],.4f),"Tuning and grade travel");
    }
    {
        Check(Near(BakedDaylightScale(-12.f,.08f),.028f),"Night skybox keeps only the ambient floor");
        Check(Near(BakedDaylightScale(25.f,.08f),1.f),"Daylight skybox stays at its authored brightness");
        Check(UnlitOverlayScale(-12.f,.08f)<BakedDaylightScale(-12.f,.08f),"Night shafts are thinner than the skybox scale");
        Check(Near(UnlitOverlayScale(25.f,.4f),1.f),"Daylight shafts are left alone");
        Check(BakedDaylightScale(-12.f,.8f)>BakedDaylightScale(-12.f,.08f),"Raising ambient lifts the night background");
    }
    {
        Check(SmokeRayEntry({0,0,0},{1,0,0},1000,{300,0,0},100)==200,"Smoke entry is in world space");
        Check(SmokeRayEntry({0,0,0},{1,0,0},150,{300,0,0},100)<0,"Foreground rain stays before smoke");
        Check(SmokeRayEntry({300,0,0},{1,0,0},1000,{300,0,0},100)==0,"Interior smoke starts at the camera");
        Check(RainSmokeOpacity(20,0,32,true)>.999f,"Dense interior extinguishes nearby rain");
        Check(RainSmokeOpacity(20,200,100,false)==0,"Smoke does not hide foreground drops");
        Check(RainSmokeOpacity(0,0,1000,true)==0,"Cleared absorbance restores rain immediately");
        Check(RainSmokeOpacity(2,200,220,false)<RainSmokeOpacity(2,200,300,false),"Visibility changes continuously through cloud depth");
    }
    {
        // de_inferno's collision bounds from its prepared manifest.
        const Vec3 low{-3548.3f, -2872.f, -280.f}, high{3968.f, 4264.f, 1520.1f};
        const auto map = PrecipitationLattice(900, 1, true, low, high, 900);
        const auto unmapped = PrecipitationLattice(900, 1, false, {}, {}, 900);
        // Intensity is density: a light fall uses bigger cells and proportionally fewer instances,
        // over the same window, instead of drawing every cell and discarding most.
        const auto light = PrecipitationLattice(900, .06f, true, low, high, 900);
        Check(Near(1 / (light.cell * light.cell * light.cell), .06f / (map.cell * map.cell * map.cell), 1e-7f),
              "Particle density scales exactly with intensity");
        Check(light.reach == map.reach && light.reachUp == map.reachUp, "Intensity does not change the particle window");
        Check(float(light.across / 2 - 1) * light.cell >= light.reach && float(light.layers / 2 - 1) * light.cell >= light.reachUp,
              "A light fall still fills the whole window");
        Check(light.Instances() < map.Instances() / 8, "A light fall draws a fraction of the instances");
        Check(map.mapped && !unmapped.mapped, "Map bounds are used only when the map is prepared");
        Check(map.cell == unmapped.cell && Near(map.cell, 25.2f), "One cell size, from the weather radius alone: the density is the same everywhere");
        Check(map.Instances() <= kPrecipitationInstanceBudget && map.Instances() > kPrecipitationInstanceBudget * 9 / 10,
              "The particle window uses its instance budget");
        // The window starts at floor(camera / cell) - half, so it must reach one cell past the fade.
        Check(float(map.across / 2 - 1) * map.cell >= map.reach && float(map.layers / 2 - 1) * map.cell >= map.reachUp,
              "Every particle the fade keeps lies inside the drawn window");
        Check(map.reach > 800 && map.reachUp > 400, "Particles reach far enough that the veil only carries the distance");
        Check(map.high.x > high.x && map.low.y < low.y && map.low.z < low.z && map.high.z > high.z,
              "The precipitation region encloses the map and the air above its roofs");
    }
    {
        // Rain drop spectrum: mostly fine drops, a long tail of big ones.
        // The size setting is the largest drop; most are smaller, down to a quarter of it.
        Check(Near(RainDropVariation(0), .25f) && Near(RainDropVariation(1), 1) && RainDropVariation(.5f) < .55f,
              "Drop sizes spread below the size setting, mostly small");
        for (float u = 0; u < .95f; u += .05f)
            Check(RainDropVariation(u + .05f) >= RainDropVariation(u), "Drop size rises with its random draw");
        Check(RainDropFall(3.8f) / RainDropFall(.3f) > 3 && RainDropFall(3.8f) < 1.6f, "Big drops streak longer, levelling off");
        // The veil's density is the particles' coverage; it must scale the way the particles do.
        for (int type = 1; type <= 3; ++type) {
            float small = PrecipitationCrossSection(type, 1, 12), big = PrecipitationCrossSection(type, 2, 12);
            Check(small > 0 && big > small * 2, "Bigger particles add more coverage to the veil");
        }
        // Motion blur spreads one drop over its streak: a longer exposure smears, it adds no water.
        Check(Near(PrecipitationCrossSection(1, 1, 24), PrecipitationCrossSection(1, 1, 12), PrecipitationCrossSection(1, 1, 12) * .05f),
              "Motion-blurred streak length does not change rain coverage");
    }
    {
        // Settled snow and ash are sheltered straight overhead whatever the wind; rain follows it.
        const Vec3 snow=PrecipitationShelterDirection(2,300,0,65),ash=PrecipitationShelterDirection(3,-200,90,40);
        Check(Near(snow.z,1)&&Near(ash.z,1),"Snow and ash shelter from the open sky overhead, not the wind");
        Check(PrecipitationShelterDirection(1,300,0,700).x<-.3f,"Rain shelter still slants into the wind");
    }
    {
        // Gust directions: tilted about the mean, wider along the wind, always falling.
        auto angle = [](Vec3 a, Vec3 b) { return std::acos(std::clamp(Dot(a, b), -1.f, 1.f)) * 57.2958f; };
        const Vec3 mean = PrecipitationSourceDirection(40, 0, 700);
        const auto gusts = PrecipitationGustDirections(40, 0, 700);
        for (const auto &g : gusts)
            Check(Near(Dot(g, g), 1, 1e-4f) && g.z > 0, "Gust directions are unit and come from above");
        Check(Near(angle(gusts[0], mean), 20, 1.5f) && Near(angle(gusts[1], mean), 20, 1.5f), "Along-wind gusts tilt about 20 degrees in light wind");
        Check(Near(angle(gusts[2], mean), 13, 1.5f) && Near(angle(gusts[3], mean), 13, 1.5f), "Cross-wind gusts tilt less");
        // The along pair lies in the wind plane, the cross pair across it.
        Check(std::abs(gusts[0].y) < 1e-3f && std::abs(gusts[1].y) < 1e-3f && gusts[0].x < mean.x && gusts[1].x > mean.x,
              "Along-wind gusts lean further into and back out of the wind");
        Check(gusts[2].y * gusts[3].y < 0, "Cross-wind gusts lean to both sides");
        float calmAlong = 0, calmAcross = 0, stormAlong = 0, stormAcross = 0;
        PrecipitationGustAngles(0, 0, 700, calmAlong, calmAcross);
        PrecipitationGustAngles(700, 0, 700, stormAlong, stormAcross);
        Check(stormAlong > calmAlong && stormAcross > calmAcross, "A wind-slanted rain spreads further into openings");
        const auto calm = PrecipitationGustDirections(0, 0, 700);
        Check(calm[0].z > 0 && angle(calm[0], {0, 0, 1}) > 15, "Calm rain still has gusts");
        Check(Near(kPrecipitationMeanWeight + kPrecipitationGusts * PrecipitationGustWeight(), 1), "Exposure weights sum to one");
    }
    {
        Climate c; c[CL_enabled]=1; c[CL_storm]=1;
        double start=StormEventStart(c,0);
        auto lit=EvaluateStorm(c,start+.025),dark=EvaluateStorm(c,start+1);
        Check(lit.flash>0 && dark.flash==0,"Lightning has bounded pulses and dark intervals");
        EvaluateStorm(c,2000);auto replay=EvaluateStorm(c,start+.025);
        Check(lit.flash==replay.flash && lit.points[4].x==replay.points[4].x,"Lightning seek reconstructs identical channel and radiance");
        Check(EvaluateStorm(c,-1).flash==0 && EvaluateStorm(c,std::numeric_limits<double>::quiet_NaN()).flash==0,"Invalid storm time is neutral");
        c[CL_stormHold]=1;auto hold=EvaluateStorm(c,1);Check(hold.flash==EvaluateStorm(c,300).flash,"Held strike is deterministic");
        for(int seed=0;seed<32;++seed){
            c[CL_stormSeed]=float(seed);auto channel=EvaluateStorm(c,1);
            int forks=0;
            for(int i=1;i<kStormPointCount;++i){
                Check(Finite(channel.points[i]),"All fractal points are finite");
                Check(channel.parent[i]>=0&&channel.parent[i]<i,"Lightning topology always joins an existing parent");
                Check(channel.width[i]>=0&&channel.gain[i]>0&&channel.gain[i]<=1,"Taper and energy stay bounded");
                if(channel.parent[i]!=i-1)++forks;
            }
            Check(forks==5,"Channel includes primary and optional secondary forks");
            Check(channel.width[kStormPointCount-1]<channel.width[kStormTrunkPoints],"Branch tips taper");
        }
        Frame f;f.scene.climate=c;f.occlusion=std::make_shared<OcclusionMesh>();
        Check(AddStormLight(f,hold)&&f.scene.objects[0].shadows,"Lightning uses a real shadowed light");
        f.scene.objects.assign(16,Object{});for(auto &o:f.scene.objects)o.kind="point";
        Check(!AddStormLight(f,hold)&&f.scene.objects.size()==16,"Storm never evicts authored lights");
        f.scene.objects.resize(4);for(auto &o:f.scene.objects)o.shadows=true;
        Check(!AddStormLight(f,hold),"Storm preserves all four authored shadow allocations");
        Scene scene;scene.climate=c;scene.map="de_inferno";std::string error;Scene parsed;
        Check(ParseScene(SerializeScene(scene),parsed,error)&&parsed.climate[CL_storm]==1&&parsed.climate[CL_stormHold]==1,"Storm presets roundtrip");
        ResetScenePage(scene,"props");Check(scene.map=="de_inferno","Storm keeps its world anchor bound to the map");
        ResetScenePage(scene,"weather");Check(scene.climate[CL_storm]==0,"Weather reset removes lightning");
        c[CL_enabled]=0;Check(EvaluateStorm(c,start+.025).flash==0,"Master disable removes lightning");
    }
    {
        StudioLight settings;settings.cue=3;
        Check(Near(StudioCue(settings,0),1)&&Near(StudioCue(settings,.5),0),"Lightning cue has bounded on and off phases");
        Check(Near(StudioCue(settings,.5),StudioCue(settings,100.5)),"Light cue seeks are deterministic");
        settings.amount=0;Check(Near(StudioCue(settings,.5),1),"Zero cue amount preserves steady light");
        Check(ShadowPenumbra(10,50,200)>ShadowPenumbra(10,50,100),"Penumbra grows with receiver separation");
        Check(Near(ShadowPenumbra(0,50,100),0),"Zero source radius preserves hard shadows");
        Vec3 average{};for(int j=0;j<4;++j)average=average+AreaLightSample({10,20,30},{0,1,0},{0,0,1},100,50,j)*.25f;
        Check(Near(average.x,10)&&Near(average.y,20)&&Near(average.z,30),"Area quadrature is centered on the emitter");
        Scene studio;Object emitter;emitter.id=1;emitter.kind="area";emitter.studio.width=340;emitter.studio.pattern=2;emitter.studio.cue=4;studio.objects={emitter};Scene roundtrip;std::string error;
        Check(ParseScene(SerializeScene(studio),roundtrip,error)&&roundtrip.objects[0].studio.width==340&&roundtrip.objects[0].studio.pattern==2,"Studio controls survive preset serialization");
        studio.objects[0].studio.width=-1;Check(!ParseScene(SerializeScene(studio),roundtrip,error),"Invalid emitter dimensions reject the entire preset");
        studio.objects[0]=emitter;studio.objects[0].kind="point";Check(!ParseScene(SerializeScene(studio),roundtrip,error),"Point lights cannot silently accept a directional projector");
    }

    {
        Check(Near(MaterialFresnel(.04f,1),.04f)&&Near(MaterialFresnel(.04f,0),1),"Dielectric Fresnel has correct normal and grazing endpoints");
        Check(MaterialGgxDistribution(1,.1f)>MaterialGgxDistribution(1,.8f),"Smooth material concentrates the highlight");
        MaterialImage image;image.width=image.height=1;image.rgba={255,255,255,255};Check(ValidMaterialImage(image),"Bounded RGBA image accepted");image.rgba.pop_back();Check(!ValidMaterialImage(image),"Incomplete RGBA image rejected");
        Scene source;Object object;object.id=1;object.material.enabled=true;object.material.metallic=.8f;object.material.albedoTexture="Stone Color";object.cameraVisible=false;source.objects={object};Scene out;std::string error;
        Check(ParseScene(SerializeScene(source),out,error)&&out.objects[0].material.albedoTexture=="Stone Color"&&!out.objects[0].cameraVisible,"Material and visibility fields survive presets");
        source.objects[0].material.normalTexture="../escape";Check(!ParseScene(SerializeScene(source),out,error),"Texture traversal rejected before file access");
        Mesh mesh;std::istringstream obj("v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 1\nf 1/1/1 2/2/1 3/3/1\n");
        Check(LoadObj(obj,mesh,error)&&Near(mesh.vertices[1].u,1)&&Near(mesh.vertices[2].v,0)&&Near(mesh.vertices[0].normal.z,1),"OBJ keeps UVs and authored normals with image V conversion");
        std::istringstream bad("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1/9 2/9 3/9\n");Check(!LoadObj(bad,mesh,error),"Missing UV references reject malformed textured meshes");
    }
    for (int face = 0; face < 6; ++face) {
        auto dir = PointShadowDirection(face);
        auto m = LocalShadowMatrix({10, 20, 30}, dir, 90, 400);
        auto p = Transform(m, Vec3{10, 20, 30} + dir * 100);
        Check(std::abs(p.x) < 1e-5f && std::abs(p.y) < 1e-5f && p.z > 0 && p.z < 1,
              "All point-shadow cube faces project their center correctly");
        Check(PointShadowFace(dir) == face, "CPU cube face selection agrees with face directions");
        const auto finiteSource=LocalShadowMatrix({10,20,30},dir,90,400,2.714437f);
        Check(Transform(finiteSource,Vec3{10,20,30}+dir*2.27f).z<0,
              "A spherical light's own luminous bulb cannot block its center projection");
        Check(Transform(finiteSource,Vec3{10,20,30}+dir*3.60f).z>0,
              "Nearby opaque lamp housing still casts a shadow outside the emitting sphere");
    }
    auto sunMatrix = SunShadowMatrix({-400, -300, -50}, {600, 500, 700}, Normalize({1, 2, 3}));
    {
        const Vec3 camera{120, -70, 200};
        auto nearSun = NearSunShadowMatrix(sunMatrix, camera);
        auto center = Transform(nearSun, camera);
        Check(std::abs(center.x) <= 16.f/1024 && std::abs(center.y) <= 16.f/1024,
              "Near sun projection keeps camera within half a snap cell of center");
        Vec3 axis{nearSun[0], nearSun[1], nearSun[2]};
        Check(Near(2/(std::sqrt(Dot(axis,axis))*4096), .5f), "Near sun texel covers half a world unit");
        for(int i=8;i<16;++i) Check(nearSun[i]==sunMatrix[i], "Near shadow preserves full-map caster depth");
        auto closeSun=NearSunShadowMatrix(sunMatrix,camera,CloseSunHalfExtent);
        Vec3 closeAxis{closeSun[0],closeSun[1],closeSun[2]};
        Check(Near(2/(std::sqrt(Dot(closeAxis,closeAxis))*4096),.03125f),"Close sun gives 1/32-unit first-person shadow detail");
        auto closeCenter=Transform(closeSun,camera);
        Check(std::abs(closeCenter.x)<=4.f/64&&std::abs(closeCenter.y)<=4.f/64,"Close snap keeps first-person receivers inside the high-detail region");
        for(Vec3 offset:{Vec3{45,0,0},Vec3{-45,0,0},Vec3{0,45,0},Vec3{0,-45,0},Vec3{0,0,45},Vec3{0,0,-45}}){
            auto hand=Transform(closeSun,camera+offset);
            Check(std::abs(hand.x)<.8f&&std::abs(hand.y)<.8f,"Nearby hands remain before the close-cascade blend boundary");
        }
        Check(NearSunShadowMatrix(sunMatrix,{},CloseSunHalfExtent)==NearSunShadowMatrix(sunMatrix,{.01f,.01f,.01f},CloseSunHalfExtent),"Subpixel camera motion retains close cache");
        for(int i=8;i<16;++i)Check(closeSun[i]==sunMatrix[i],"Close shadow retains distant upstream blockers");
        auto detailSun=NearSunShadowMatrix(sunMatrix,camera,DetailSunHalfExtent);
        Vec3 detailAxis{detailSun[0],detailSun[1],detailSun[2]};
        Check(Near(2/(std::sqrt(Dot(detailAxis,detailAxis))*4096),.125f),"Bridge keeps ground shadow texels at one eighth unit");
        for(int i=8;i<16;++i)Check(detailSun[i]==sunMatrix[i],"Bridge retains full upstream caster depth");
        for(Vec3 offset:{Vec3{70,0,0},Vec3{-70,0,0},Vec3{0,70,0},Vec3{0,-70,0},Vec3{0,0,70},Vec3{0,0,-70}}){
            auto receiver=Transform(detailSun,camera+offset);
            Check(std::abs(receiver.x)<.75f&&std::abs(receiver.y)<.75f,"Close fade is fully inside bridge detail, including snapped centers");
        }
        // A camera-following cascade must only ever step by whole texels, or every shadow edge
        // re-rasterises at a sub-texel offset and shimmers while the camera travels.
        for(float half:{64.f,183.f,524.f,1255.f})for(Vec3 eye:{Vec3{0,0,0},Vec3{881,2012,281},Vec3{-37.3f,511.9f,-12},Vec3{3000,-2000,500}}){
            const auto m=NearSunShadowMatrix(sunMatrix,eye,half,2048);
            for(int row=0;row<2;++row){const float texels=m[row*4+3]*1024;
                Check(std::abs(texels-std::round(texels))<.01f,"Camera-following sun cascades snap by whole texels");}
        }
        auto fixed=NearSunShadowMatrix(sunMatrix, {0,0,0});
        Check(fixed==NearSunShadowMatrix(sunMatrix,{.01f,.01f,.01f}), "Tiny camera motion reuses near shadow cache");
        Check(fixed!=NearSunShadowMatrix(sunMatrix,{300,300,300}), "Camera translation updates the detail region");
        for(Vec3 direction : {Vec3{0,0,1}, Vec3{1,0,0}, Vec3{0,0,-1}}) {
            auto matrix=NearSunShadowMatrix(SunShadowMatrix({-100,-100,-100},{100,100,100},direction),{});
            for(float value:matrix) Check(std::isfinite(value), "Vertical and horizon suns have finite detail projections");
        }
    }
    for (int i = 0; i < 8; ++i) {
        auto p =
            Transform(sunMatrix, {i & 1 ? 600.f : -400.f, i & 2 ? 500.f : -300.f, i & 4 ? 700.f : -50.f});
        Check(std::abs(p.x) < 1 && std::abs(p.y) < 1 && p.z > 0 && p.z < 1,
              "Sun shadow projection encloses the complete map");
    }
    {
        std::string bytes = "MVMOCC01";
        uint32_t counts[] = {3, 3};
        Vec3 limits[] = {{0, 0, 0}, {1, 1, 0}};
        Vec3 points[] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        uint32_t indices[] = {0, 1, 2};
        bytes.append(reinterpret_cast<char *>(counts), sizeof(counts));
        bytes.append(reinterpret_cast<char *>(limits), sizeof(limits));
        bytes.append(reinterpret_cast<char *>(points), sizeof(points));
        bytes.append(reinterpret_cast<char *>(indices), sizeof(indices));
        OcclusionMesh mesh;
        std::string error;
        std::istringstream good(bytes);
        Check(ReadOcclusionMesh(good, mesh, error) && mesh.indices.size() == 3,
              "Validated occlusion cache reads source geometry");
        std::istringstream shortFile(bytes.substr(0, bytes.size() - 1));
        Check(!ReadOcclusionMesh(shortFile, mesh, error), "Truncated occlusion cache rejected");
        bytes.back() = 127;
        std::istringstream corrupt(bytes);
        Check(!ReadOcclusionMesh(corrupt, mesh, error), "Out of range shadow index rejected");
    }
    Vec3 f, r, u;
    CameraBasis({0, 0, 0}, f, r, u);
    Check(Near(f.x, 1) && Near(r.y, -1) && Near(u.z, 1), "Source camera basis");
    CameraBasis({-89, 37, 60}, f, r, u);
    Check(Near(Dot(f, u), 0) && Near(Dot(r, u), 0) && Near(Dot(r, r), 1), "Rolled camera orthonormal basis");
    Matrix projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 4, 0, 0, -1, 0};
    Vec3 camera{200, -120, 43}, angles{25, 37, 60};
    auto vp = ViewProjection(camera, angles, projection);
    Matrix inverse{};
    Check(Inverse(vp, inverse), "Projection invertible");
    CameraBasis(angles, f, r, u);
    Vec3 point = camera + f * 600 + r * 30 + u * 15;
    auto clip = Transform(vp, point);
    auto recovered = Transform(inverse, clip);
    Check(Near(clip.x, .05f) && Near(clip.y, .025f) && Near(clip.z, 4.f / 600), "World projects correctly");
    Check(Near(point.x, recovered.x) && Near(point.y, recovered.y) && Near(point.z, recovered.z),
          "World reconstructs after camera roll");
    auto eye = Transform(Multiply(inverse, projection), {0, 0, 0});
    Check(Near(eye.x, camera.x) && Near(eye.y, camera.y) && Near(eye.z, camera.z),
          "Camera extracted from completed engine matrices");
    Matrix singular{};
    Check(!Inverse(singular, inverse), "Singular projection rejected");
    auto weather = WeatherPosition(19, 7, 1053.5, {50, 40, 80}, 900, 800, {25, 0, -700});
    auto repeat = WeatherPosition(19, 7, 1053.5, {50, 40, 80}, 900, 800, {25, 0, -700});
    auto moved = WeatherPosition(19, 7, 1053.5, {51, 41, 81}, 900, 800, {25, 0, -700});
    Check(Near(weather.x, repeat.x) && Near(weather.z, repeat.z), "Seek determinism");
    Check(Near(weather.x, moved.x) && Near(weather.y, moved.y) && Near(weather.z, moved.z),
          "Camera movement does not drag weather");
    for (int i = 0; i < 6000; ++i) {
        auto p = WeatherPosition(i, 65535, -3543.123, {0, 0, 0}, 64, 64, {-1000, 1000, -1500});
        Check(Finite(p) && std::abs(p.x) <= 64 && std::abs(p.y) <= 64 && std::abs(p.z) <= 32,
              "Weather remains in volume");
    }
    Climate climate;
    Check(ClimateSteps(climate) == 32 && ClimateSteps(climate, true) == 64,
          "Bounded preview/export ray budgets");
    climate[CL_sunAzimuth] = 90;
    climate[CL_sunElevation] = 0;
    auto sun = ClimateSunDirection(climate);
    Check(Near(sun.x, 0) && Near(sun.y, 1) && Near(sun.z, 0), "Shared sun uses Source Z-up coordinates");
    Check(SnowSlope(0) == 0 && SnowSlope(1) == 1 && Near(SnowSlope(.6f), .5f), "Snow favors upward faces");
    Check(!ClimateValueValid(CL_stormSeed, 1.5) && !ClimateValueValid(CL_snow, 1.01) &&
              ClimateValueValid(CL_sunElevation, -12),
          "Climate schema rejects invalid GPU inputs");
    Check(EffectiveClimate(climate)[CL_sky] == 0 && EffectiveClimate(climate)[CL_sunRelight] == 0 &&
              EffectiveClimate(climate)[CL_sunPower] == 0,
          "Master off blocks procedural rendering");
    climate[CL_enabled] = 1;
    Check(EffectiveClimate(climate)[CL_sky] == 0 && EffectiveClimate(climate)[CL_sunRelight] == 0,
          "Enabling another pass never activates default sky or relighting");
    climate[CL_sky] = 1;
    Check(EffectiveClimate(climate)[CL_sky] == 1, "Sky requires its own explicit enable");
    climate[CL_relighting] = 1;
    Check(EffectiveClimate(climate)[CL_sunRelight] > .5f, "Relighting explicit enable activates sunlight");
    Scene original;
    original.climate[CL_clouds] = 1;
    original.climate[CL_snow] = .8f;
    original.climate[CL_sunElevation] = 8;
    original.enabled = true;
    original.map = "de_inferno";
    original.weather.type = 2;
    original.weather.follow = false;
    original.weather.center = {123, 456, 789};
    Object object;
    object.id = 1;
    object.kind = "spot";
    object.position = {900, -80, 64};
    object.intensity = 5;
    original.objects.push_back(object);
    {Scene lights;for(int i=0;i<5;++i){Object o;o.id=i+1;o.kind="point";o.shadows=true;lights.objects.push_back(o);}
        Scene decoded;std::string error;Check(!ParseScene(SerializeScene(lights),decoded,error),"Fifth shadow-casting light rejected instead of silently unshadowed");
        lights.objects.pop_back();Check(ParseScene(SerializeScene(lights),decoded,error)&&decoded.objects[0].shadows,"Per-light shadow options survive presets");}
    Scene decoded;
    std::string error;
    Check(ParseScene(SerializeScene(original), decoded, error), "Preset round trip parses");
    Check(SerializeScene(original) == SerializeScene(decoded), "Complete preset round trip");
    auto invalid = original;
    invalid.objects.push_back(object);
    Check(!ParseScene(SerializeScene(invalid), decoded, error), "Duplicate IDs rejected");
    invalid = original;
    invalid.objects[0].range = 0;
    Check(!ParseScene(SerializeScene(invalid), decoded, error), "Invalid range rejected");
    invalid = original;
    invalid.weather.count = 6001;
    Check(!ParseScene(SerializeScene(invalid), decoded, error), "Weather budget enforced");
    invalid = original;
    invalid.objects[0].kind = "obj";
    invalid.objects[0].asset = "../model";
    Check(!ParseScene(SerializeScene(invalid), decoded, error), "Mesh path traversal rejected");
    Check(!ValidName("test;quit") && !ValidName("..") && !ValidName("trailing ") &&
              ValidName("Night Shoot 01"),
          "Safe portable names");
    Check(!ParseScene(std::string(20, '[') + std::string(20, ']'), decoded, error),
          "Deep JSON rejected before recursive parser");
    std::string version = SerializeScene(original);
    version.replace(version.find("\"version\":14"), 12, "\"version\":99");
    Check(!ParseScene(version, decoded, error), "Unknown version rejected");
    for (int oldVersion=1; oldVersion<14; ++oldVersion) {
        auto legacy=SerializeScene(original);
        legacy.replace(legacy.find("\"version\":14"),12,"\"version\":"+std::to_string(oldVersion));
        Check(!ParseScene(legacy,decoded,error) && error.find("incompatible")!=std::string::npos,
              "Older presets are rejected with an incompatibility message");
    }
    original.climate[CL_fogDensity]=.6f; original.climate[CL_fogStart]=1200;
    original.fogColor={.3f,.5f,.8f};
    Check(ParseScene(SerializeScene(original),decoded,error) && decoded.climate[CL_fogStart]==1200 &&
          Near(decoded.climate[CL_fogDensity],.6f) && Near(decoded.fogColor.z,.8f), "Unified fog roundtrips");
    Check(SerializeScene(original).find("quality")==std::string::npos, "No persisted climate quality");
    Check(ClimateSteps(original.climate)==32 && ClimateSteps(original.climate,true)==64, "Fixed quality policy");
    auto retired = original;
    retired.climate[CL_ssr] = 1;
    retired.climate[CL_gi] = 2;
    Check(EffectiveClimate(retired.climate)[CL_ssr] == 0 && EffectiveClimate(retired.climate)[CL_gi] == 0,
          "Camera-dependent transport cannot render from old presets");
    auto badClimate = original;
    badClimate.climate[CL_stormSeed] = 1.5f;
    Check(!ParseScene(SerializeScene(badClimate), decoded, error), "Preset cannot inject fractional storm seed");
    Scene look = original;
    look.world.sky = "Night Sky";
    look.world.fields["skyTint"] = {1};
    look.world.fields["skyColor"] = {140, 20, 255, 180};
    look.world.fields["exposure"] = {.35};
    look.world.fields["engineLights"] = {0};
    look.world.fields["shadows"] = {0};
    look.world.fields["shadowLayers"] = {0, 1};
    look.world.fields["worldTint"] = {1, 1, 1, 1};
    Check(ParseScene(SerializeScene(look), decoded, error) && SerializeScene(look) == SerializeScene(decoded),
          "Complete world look round trip");
    Check(decoded.world.fields["engineLights"][0] == 0 &&
          decoded.world.fields["shadowLayers"] == std::vector<double>({0,1}),
          "Engine lights and shadow layers persist independently");
    auto earlier14 = SerializeScene(look);
    for (const std::string key : {std::string("engineLights"), std::string("shadowLayers")}) {
        auto start = earlier14.find(",\"" + key + "\":");
        earlier14.erase(start, earlier14.find(']', start) + 1 - start);
    }
    Check(ParseScene(earlier14, decoded, error) && decoded.world.fields["engineLights"][0] == 1 &&
          decoded.world.fields["shadowLayers"] == std::vector<double>({0,0}),
          "Format 14 without independent controls retains its original master-shadow behavior");
    ResetScenePage(look, "sky");
    Check(look.world.sky.empty() && look.world.fields["skyTint"][0] == 0 &&
              look.world.fields["exposure"][0] == .35,
          "Sky reset preserves lighting");
    // A sky reset leaves every sky switch off, clouds included. Snow (weather) and the sun elevation (lighting)
    // must still survive untouched.
    Check(look.climate[CL_clouds] == 0 && look.climate[CL_snow] == .8f && look.climate[CL_sunElevation] == original.climate[CL_sunElevation],
          "Sky reset switches the cloud layer off and preserves lighting-owned sun and surface edits");
    Check(ClimateSpecs()[CL_clouds].initial == 0 && ClimateSpecs()[CL_sky].initial == 0,
          "Clouds and sky default off: turning Weather or Fog on must not bring up the cloud sky");
    // The sun colour is a multiplier on the shipped elevation ramp, so an untouched scene has to
    // come out of a reset at exactly 1,1,1 or every preset silently changes colour.
    Check(look.climate[CL_sunColorR] == 1 && look.climate[CL_sunColorG] == 1 &&
              look.climate[CL_sunColorB] == 1,
          "Sun colour resets to the neutral tint that reproduces the shipped ramp");
    ResetScenePage(look, "weather");
    Check(look.climate[CL_snow] == 0 && look.climate[CL_sunElevation] == original.climate[CL_sunElevation],
          "Weather reset removes surface experiments only");
    Check(look.weather.type == 0 && look.world.fields["worldTint"][0] == 1,
          "Weather reset preserves atmosphere");
    ResetScenePage(look, "lighting");
    Check(look.climate[CL_sunElevation] == 35, "Lighting reset includes procedural sun");
    ResetScenePage(look, "all");
    Scene resetDefaults;
    ResetClimatePage(resetDefaults.climate, "all");
    Check(SerializeScene(look) == SerializeScene(resetDefaults), "Full reset releases all overrides and additions");
    for (ClimateField field : {CL_clouds, CL_lightShafts})
        Check(look.climate[field] == 0, "Full reset disarms subordinate feature switches too");
    for (ClimateField field : {CL_contactShadows, CL_ao, CL_skyOcclusion})
        Check(look.climate[field] == 1, "Cast shadows, AO and sky occlusion stay on after a reset");
    {
        Climate fog;
        fog[CL_enabled] = 1; fog[CL_fogDensity] = .35f; fog[CL_lightShafts] = 1;
        Check(!ClimateSamplesSunShadow(EffectiveClimate(fog), false, false),
              "Fog alone renders no sun shadow maps, light shafts included");
        Check(ClimateSamplesSunShadow(EffectiveClimate(fog), true, false),
              "Fog under the frame's owned lighting keeps shadowed shafts");
        Climate lit = fog; lit[CL_relighting] = 1;
        Check(ClimateSamplesSunShadow(EffectiveClimate(lit), false, false), "Dynamic lighting renders sun shadows");
        Climate sky = fog; sky[CL_sky] = 1;
        Check(ClimateSamplesSunShadow(EffectiveClimate(sky), false, false), "Fog shafts under the procedural sun are shadowed");
        sky[CL_lightShafts] = 0;
        Check(!ClimateSamplesSunShadow(EffectiveClimate(sky), false, false), "A bare sky with unshafted fog samples no sun shadow");
        Climate snow; snow[CL_enabled] = 1; snow[CL_snow] = .5f;
        Check(ClimateSamplesSunShadow(EffectiveClimate(snow), false, false), "Coatings keep their sun shadows");
        Check(ClimateSamplesSunShadow(EffectiveClimate(fog), false, true), "Placed props keep their sun shadows");
        lit[CL_contactShadows] = 0;
        Check(!ClimateSamplesSunShadow(EffectiveClimate(lit), true, true), "Cast shadows off renders no sun shadow maps");
        lit[CL_contactShadows] = 1; lit[CL_sunElevation] = -5;
        Check(!ClimateSamplesSunShadow(EffectiveClimate(lit), true, true), "A sun below the horizon casts no shadow maps");
        Climate off = lit; off[CL_enabled] = 0; off[CL_sunElevation] = 35;
        Check(!ClimateSamplesSunShadow(EffectiveClimate(off), true, true), "The renderer switched off renders nothing");
    }
    Climate card;
    card[CL_sky] = 1; card[CL_clouds] = 1; card[CL_storm] = 1;
    ResetClimatePage(card, "sky");
    Check(card[CL_clouds] == 0 && card[CL_storm] == 1, "Sky reset switches clouds off and preserves weather");
    Check(!look.enabled && look.climate[CL_enabled] == 0 && look.climate[CL_sky] == 0 &&
          look.climate[CL_relighting] == 0 && look.climate[CL_worldGi] == 0 &&
          look.climate[CL_fogDensity] == 0 &&
          look.climate[CL_snow] == 0 && look.climate[CL_wet] == 0 && look.climate[CL_puddles] == 0 &&
          look.climate[CL_storm] == 0 && look.climate[CL_bloom] == 0 && look.weather.type == 0,
          "Full reset leaves every environment pass off, including latent sky");
    look.map = "de_inferno";
    look.climate[CL_surfaceRadius] = 600;
    ResetScenePage(look, "lighting");
    Check(look.map == "de_inferno", "Unrelated page reset retains local surface map binding");
    ResetScenePage(look, "weather");
    Check(look.map.empty(), "Removing final spatial edit releases map binding");
    look.world.fields["skyColor"] = {256, 0, 0, 255};
    Check(!ParseScene(SerializeScene(look), decoded, error), "Invalid tint rejected transactionally");
    auto fractional = SerializeScene(original);
    fractional.replace(fractional.find("\"seed\":7"), 8, "\"seed\":7.00000001");
    Check(!ParseScene(fractional, decoded, error), "Fractional integers rejected before float rounding");
    Mesh mesh;
    std::istringstream obj("v 0 0 0\nv 4 0 0\nv 0 4 0\nvt 0 0\nvt 1 0\nvt 0 1\nf -3/1 -2/2 -1/3\n");
    Check(LoadObj(obj, mesh, error) && mesh.vertices.size() == 3, "OBJ negative indices and UV tokens");
    Check(Near(mesh.vertices[0].position.x, -1) && Near(mesh.vertices[1].position.x, 1),
          "OBJ normalized bounds");
    std::istringstream bad("v 0 0 0\nf 1 2 3\n");
    Check(!LoadObj(bad, mesh, error), "Missing OBJ vertices rejected");
    for (auto kind : {"box", "beveledbox", "sphere", "cylinder"}) {
        auto p = Primitive(kind);
        Check(p && p->vertices.size() >= 36, "Primitive triangles available");
        for (auto v : p->vertices)
            Check(Finite(v.position) && Finite(v.normal), "Finite primitive vertices");
    }
    Check(CoatingNormalWeight({0,0,1},Normalize({.1f,0,1}),{1,0,0},10)>.9f,"Coating blends shallow neighboring facets");
    Check(CoatingNormalWeight({0,0,1},{1,0,0},{1,0,0},10)==0,"Coating preserves hard corners");
    Check(CoatingNormalWeight({0,0,1},{0,0,1},{0,0,20},10)==0,"Coating rejects another depth layer");
    Check(CoatingDetailFade(.01f,2)==1&&CoatingDetailFade(2,2)==0,"Subpixel coating detail fades rather than aliasing");
    {
        // A player claims only its own body, never the floor around it.
        const Vec3 feet{100,200,50},up{0,0,1};
        const float r=kCoatingBodyRadius;
        Check(!CoatingActorClaims({110,200,50},up,feet,r),"Floor under a player stays map (no ring)");
        Check(!CoatingActorClaims({110,200,62},up,feet,r),"A low ledge beside a player stays map");
        Check(!CoatingActorClaims({140,200,50},up,feet,r),"Ground outside the body radius stays map");
        Check(CoatingActorClaims({114,200,90},{1,0,0},feet,r),"Torso side is body");
        Check(CoatingActorClaims({108,200,106},up,feet,r),"Shoulder top is body");
        Check(!CoatingActorClaims({118,200,90},{-1,0,0},feet,r),"A wall the player leans on is not body");
        Check(!CoatingActorClaims({108,200,140},{1,0,0},feet,r),"Above the head is not body");
        Check(!CoatingActorClaims({108,200,90},{1,0,0},feet,0),"An empty actor slot claims nothing");
        Check(CoatingActorClaims({112,200,70},Normalize({-.6f,0,.5f}),feet,r),"A sloped face turned inward (bent arm) stays body");
    }
    Check(Near(CoatingCoverage(1,0,.18f),1)&&Near(CoatingCoverage(1,1,.18f),1),"Full coating has no procedural holes");
    Check(CoatingCoverage(0,.5f,.18f)==0,"Disabled coating covers nothing");
    Climate gi; gi[CL_bounceSize]=1024;gi[CL_bounceX]=512;gi[CL_bounceY]=256;gi[CL_bounceZ]=128;
    auto bounds=ClimateGiBounds(gi);auto centerUv=GiCoordinate(bounds,{512,256,128});
    Check(Near(centerUv.x,.5f)&&Near(centerUv.y,.5f)&&Near(centerUv.z,.5f),"GI volume uses stored world coordinates");
    Check(EffectiveClimate(gi)[CL_worldGi]==0,"GI starts disabled");
    gi[CL_worldGi]=1;Check(EffectiveClimate(gi)[CL_worldGi]==1,"GI can light a scene independently of procedural sky");
    Scene boundGi;boundGi.map="de_inferno";boundGi.climate[CL_worldGi]=1;ResetScenePage(boundGi,"props");
    Check(boundGi.map=="de_inferno","Prop reset keeps active GI bound to its map");
    ResetClimatePage(gi,"lighting");Check(gi[CL_worldGi]==0,"Lighting reset disables world GI");
    original.climate[CL_worldGi]=1;original.climate[CL_bounceX]=1200;original.climate[CL_sceneReflections]=.7f;original.climate[CL_bounceSteps]=3;
    Check(ParseScene(SerializeScene(original),decoded,error)&&decoded.climate[CL_worldGi]==1&&Near(decoded.climate[CL_sceneReflections],.7f)&&decoded.climate[CL_bounceSteps]==3,"GI, propagation steps and reflections roundtrip in presets");
    auto rainDirection=PrecipitationSourceDirection(120,-80,600);auto shelterMatrix=PrecipitationShadowMatrix({-100,-200,-50},{100,200,80},rainDirection);
    auto receiver=Transform(shelterMatrix,{0,0,0}),upstream=Transform(shelterMatrix,rainDirection*20);
    Check(Near(receiver.x,upstream.x)&&Near(receiver.y,upstream.y)&&upstream.z<receiver.z,"Precipitation shadow coordinates follow the incoming wind direction");
    Scene collisionScene;Check(collisionScene.weather.shelter,"New precipitation defaults to world collision");
    Check(ParseScene(SerializeScene(collisionScene),decoded,error)&&decoded.weather.shelter,"Weather collision roundtrips in presets");
    {Scene waterScene;waterScene.climate[CL_puddleNative]=1;Scene loaded;Check(ParseScene(SerializeScene(waterScene),loaded,error)&&loaded.climate[CL_puddleNative]==1,"Legacy native-water flag still roundtrips");}
    Check(Climate{ }[CL_puddleNative]==0,"Coverage puddles default to the shader film");
    Check(Climate{ }[CL_bakedFill]==.6f&&Climate{ }[CL_bakedSuppressed]==0,"Baked lighting stays on by default with a usable fill ready");
    {
        // Mirage: 5920 x 5912 x 1219 units, so a radius of about 4200.
        const float mirageRadius=4200;
        const auto tuned=SunCascadeHalfExtents(3000,mirageRadius);
        Check(tuned.close==CloseSunHalfExtent,"The close cascade stays fixed; first-person receivers magnify it most");
        Check(Near(tuned.outer,1500.f),"The outer cascade follows shadowRange rather than the map");
        // Geometric spacing: every neighbouring pair differs by the same ratio, so quality steps
        // uniformly instead of falling off a 4x cliff at one boundary.
        const float a=tuned.detail/tuned.close,b=tuned.middle/tuned.detail,c=tuned.outer/tuned.middle;
        Check(Near(a,b)&&Near(b,c),"Sun cascades are spaced by one constant ratio");
        Check(a>1&&a<4,"The spacing ratio beats the fixed 4x ladder it replaces");
        // The whole point: the tier that carries everything else gets finer than the map fit.
        Check(tuned.OuterTexelWorld(4096)<1.f,"The outer cascade resolves below one unit per texel");
        Check(SunCascadeHalfExtents(9192,mirageRadius).OuterTexelWorld(4096)>
              tuned.OuterTexelWorld(4096),"A wider range trades texel density for reach");
        // A range past the map's own size only spends texels on empty space.
        Check(Near(SunCascadeHalfExtents(100000,mirageRadius).outer,mirageRadius),
              "The outer cascade clamps to the map instead of covering emptiness");
        // Degenerate inputs must not invert the ladder or divide by zero.
        const auto floored=SunCascadeHalfExtents(0,mirageRadius);
        Check(floored.outer>=floored.middle&&floored.middle>=floored.detail&&floored.detail>=floored.close,
              "Cascades stay ordered when the range is unset");
        const auto tiny=SunCascadeHalfExtents(1,mirageRadius);
        Check(tiny.outer>=tiny.close*2,"A tiny range still leaves the outer cascade wider than the close one");
    }
    {Scene unbaked;unbaked.climate[CL_bakedFill]=1.25f;Scene loaded;
     Check(ParseScene(SerializeScene(unbaked),loaded,error)&&loaded.climate[CL_bakedFill]==1.25f,"Unbaked fill roundtrips in presets");
     Check(ClimateValueValid(CL_bakedFill,2.5)&&!ClimateValueValid(CL_bakedFill,-1),"Unbaked fill accepts fractions inside its range");}
    {Climate native;native[CL_enabled]=1;native[CL_relighting]=1;native[CL_puddleNative]=1;native[CL_bakedSuppressed]=1;
     Check(EffectiveClimate(native)[CL_puddleNative]==0,"Native wet-ground meshes stay disabled at runtime");
     Check(EffectiveClimate(native)[CL_bakedSuppressed]==1,"Explicit lightmap suppression reaches the runtime");
     native[CL_relighting]=0;
     Check(EffectiveClimate(native)[CL_bakedSuppressed]==1,"Baked lighting is independent of added relighting");
     native[CL_enabled]=0;
     Check(EffectiveClimate(native)[CL_bakedSuppressed]==0,"Disabled environment releases lightmap suppression");}
    OcclusionMesh ground;ground.positions={{-256,-256,0},{256,-256,0},{256,256,0},{-256,256,0},{-256,-256,24},{256,-256,24},{256,256,24},{-256,256,24}};ground.indices={0,1,2,0,2,3,4,5,6,4,6,7};
    SurfaceHit hit;Check(SurfaceRaycast(ground,{0,0,100},{0,0,-1},200,hit)&&Near(hit.position.z,24),"Placement ray hits the nearest floor");
    // Placed puddles and snow drifts were removed (they are weather): a saved scene drops them.
    {Scene legacy;legacy.map="de_inferno";Object drift;drift.id=1;drift.kind="snowdrift";drift.scale={128,128,10};
     Object puddle;puddle.id=2;puddle.kind="puddle";puddle.scale={64,64,2};Object light;light.id=3;light.kind="point";
     legacy.objects={drift,puddle,light};
     Check(ParseScene(SerializeScene(legacy),decoded,error)&&decoded.objects.size()==1&&decoded.objects[0].kind=="point",
           "A scene saved with placed puddles or snow drifts loads without them");}
    auto bevel=Primitive("beveledbox");bool rounded=false;for(const auto &v:bevel->vertices)if(std::abs(v.normal.x)>.1f&&std::abs(v.normal.y)>.1f){rounded=true;Check(std::abs(v.position.x)<1&&std::abs(v.position.y)<1,"Beveled edges change the actual silhouette");}Check(rounded,"Beveled primitive has curved edge normals");
    {
        static_assert(sizeof(CasterVertex)==32&&sizeof(CasterAnimation)==32,"Stable caster cache layout");
        // MVMCAST6 layout: header(8) count(4) flags(4) nv(4) ni(4) | rigid positions, indices,
        // per-triangle albedo | batch: nv ni width height cutoff animation(32) addressU addressV
        // albedo vertices indices alpha | stained glass count, each: nv ni width height,
        // position+uv vertices, indices, RGBA8 colour. Offsets below follow this fixture.
        std::ostringstream bytes(std::ios::binary);bytes.write("MVMCAST6",8);
        auto u=[&](uint32_t v){bytes.write(reinterpret_cast<char*>(&v),4);};
        u(1);u(1);u(3);u(3);
        const Vec3 rigidVertices[]{{10,20,30},{40,20,30},{10,50,30}};const uint32_t rigidIndices[]{0,1,2};
        bytes.write(reinterpret_cast<const char*>(rigidVertices),sizeof(rigidVertices));
        bytes.write(reinterpret_cast<const char*>(rigidIndices),sizeof(rigidIndices));
        u(0x204060); // rigid triangle albedo: r=0x60 g=0x40 b=0x20
        u(3);u(3);u(2);u(2);float cutoff=.466f;bytes.write(reinterpret_cast<char*>(&cutoff),4);
        CasterAnimation animation;animation.sway[0]=.2f;bytes.write(reinterpret_cast<char*>(&animation),sizeof(animation));
        u(0);u(0);u(0x30a050);
        CasterVertex vertices[]={{{10,20,30},0,0},{{40,20,30},1,0},{{10,50,30},0,1}};
        bytes.write(reinterpret_cast<char*>(vertices),sizeof(vertices));u(0);u(1);u(2);
        unsigned char alpha[]={0,255,128,240};bytes.write(reinterpret_cast<char*>(alpha),4);
        u(0); // no stained glass
        auto valid=bytes.str();VisualCasters casters;std::string why;
        Check(valid.size()==256,"Version 6 fixture layout");
        std::istringstream stream(valid,std::ios::binary);
        Check(ReadVisualCasters(stream,casters,why)&&casters.batches.size()==1&&casters.renderGeometry&&casters.glass.empty(),"Version 6 pack loads");
        {
            // One stained glass pane: its triangle, UVs and a 1x2 colour image survive the pack.
            auto glassPack=valid.substr(0,252);const uint32_t header[]{1,3,3,1,2};
            glassPack.append(reinterpret_cast<const char*>(header),sizeof(header));
            const GlassVertex pane[]{{{0,0,0},0,0},{{40,0,0},1,0},{{0,0,80},0,1}};const uint32_t order[]{0,1,2};
            const unsigned char rgba[]{200,30,40,255,20,60,220,255};
            glassPack.append(reinterpret_cast<const char*>(pane),sizeof(pane));
            glassPack.append(reinterpret_cast<const char*>(order),sizeof(order));
            glassPack.append(reinterpret_cast<const char*>(rgba),sizeof(rgba));
            VisualCasters withGlass;std::istringstream glassStream(glassPack,std::ios::binary);
            Check(ReadVisualCasters(glassStream,withGlass,why)&&withGlass.glass.size()==1&&withGlass.glass[0].vertices[1].position.x==40&&
                  withGlass.glass[0].vertices[2].v==1&&withGlass.glass[0].rgba[4]==20&&withGlass.glass[0].height==2,"Stained glass geometry and colour survive the pack");
            auto oversized=glassPack;const uint32_t wide=513;std::memcpy(oversized.data()+252+12,&wide,4);
            std::istringstream badGlass(oversized,std::ios::binary);
            Check(!ReadVisualCasters(badGlass,withGlass,why),"Oversized stained glass colour images are rejected");
        }
        Check(casters.rigidAlbedo.size()==1&&casters.rigidAlbedo[0]==0x204060&&casters.batches[0].albedo==0x30a050,
              "Material albedo survives the pack for rigid triangles and cutout batches");
        {
            std::string large=valid.substr(0,76);uint32_t materialCount=513;
            std::memcpy(large.data()+8,&materialCount,4);
            for(unsigned i=0;i<materialCount;++i)large+=valid.substr(76,176);
            large+=valid.substr(252);
            VisualCasters expanded;std::istringstream many(large,std::ios::binary);
            Check(ReadVisualCasters(many,expanded,why)&&expanded.batches.size()==513,"Large competitive maps exceed the old material limit");
            materialCount=4097;std::memcpy(large.data()+8,&materialCount,4);
            std::istringstream excess(large,std::ios::binary);
            Check(!ReadVisualCasters(excess,expanded,why)&&expanded.batches.size()==513,"Material limit remains bounded and preserves prior cache");
        }
        Check(Near(casters.batches[0].cutoff,.466f)&&casters.batches[0].alpha[2]==128&&casters.high.y==50,"Cutoff, alpha and world bounds survive cache");
        Check(casters.batches[0].animation.Active(),"Native animation parameters survive cache");
        for(uint32_t address=0;address<5;++address){
            auto moded=valid;memcpy(&moded[128],&address,4);memcpy(&moded[132],&address,4);
            std::istringstream current(moded,std::ios::binary);
            Check(ReadVisualCasters(current,casters,why)&&casters.batches[0].addressU==address&&casters.batches[0].addressV==address,"Every native texture address mode survives the pack");
        }
        auto badAddress=valid;badAddress[128]=5;std::istringstream badMode(badAddress,std::ios::binary);
        Check(!ReadVisualCasters(badMode,casters,why),"Unknown texture address mode is rejected");
        auto invalidCoverage=valid;invalidCoverage[12]=2;std::istringstream badCoverage(invalidCoverage,std::ios::binary);
        Check(!ReadVisualCasters(badCoverage,casters,why),"Unknown render geometry coverage cannot disable physics fallback");
        auto invalidRigidIndex=valid;invalidRigidIndex[60]=9;std::istringstream badRigidIndex(invalidRigidIndex,std::ios::binary);
        Check(!ReadVisualCasters(badRigidIndex,casters,why),"Invalid opaque render indices cannot enter a shadow draw");
        OcclusionMesh shadowWorld;shadowWorld.positions={{99,99,99}};shadowWorld.indices={0,0,0};
        auto visual=std::make_shared<VisualCasters>();shadowWorld.visual=visual;
        Check(BuildRigidShadowGeometry(shadowWorld).positions[0].x==99,"A map without render geometry retains physics coverage");
        {RigidShadowGeometry scratch;const auto view=RigidShadowSource(shadowWorld,scratch);
         Check(&view.positions==&shadowWorld.positions&&scratch.positions.empty(),"Physics coverage is read in place, not copied");}
        visual->renderGeometry=true;visual->rigidPositions=casters.rigidPositions;visual->rigidIndices=casters.rigidIndices;
        visual->batches=casters.batches;visual->batches[0].cutoff=0;visual->batches[0].animation={};
        auto cutout=visual->batches[0];cutout.cutoff=.5f;visual->batches.push_back(cutout);
        auto moving=visual->batches[0];moving.animation.sway[0]=1;visual->batches.push_back(moving);
        const auto rigid=BuildRigidShadowGeometry(shadowWorld);
        Check(rigid.positions.size()>=3&&rigid.positions[0].x==10,
            "Rigid shadows use actual opaque render geometry without coarse physics, cutouts or animated rest poses");
        {RigidShadowGeometry scratch;const auto view=RigidShadowSource(shadowWorld,scratch);
         Check(&view.positions==&visual->rigidPositions&&scratch.positions.empty(),"Precomputed rigid casters are read in place, not copied");}
        for(int failure=0;failure<9;++failure){
            auto bad=valid;
            if(failure==0)bad.resize(bad.size()-1);
            if(failure==1)bad.push_back('x');
            if(failure==2)bad[236]=9; // First cutout index.
            if(failure==3){float nan=std::numeric_limits<float>::quiet_NaN();memcpy(&bad[92],&nan,4);} // cutoff
            if(failure==4){uint32_t huge=8193;memcpy(&bad[84],&huge,4);} // width
            if(failure==5){float nan=std::numeric_limits<float>::quiet_NaN();memcpy(&bad[96],&nan,4);} // animation
            if(failure==6){float invalid=2;memcpy(&bad[160],&invalid,4);} // vertex animation weight
            if(failure==7)bad[7]='4'; // Packs without albedo are rebuilt, never read with a guess.
            if(failure==8)bad[139]=1; // Albedo carries no fourth channel.
            std::istringstream invalid(bad,std::ios::binary);
            Check(!ReadVisualCasters(invalid,casters,why)&&casters.batches[0].alpha[2]==128,"Invalid caster cache preserves previous output");
        }
    }
    std::cout << "Environment math, document, validation and OBJ checks passed\n";
}
