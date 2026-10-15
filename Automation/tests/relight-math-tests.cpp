#include "../../AfxHookSource2/Filmmaker/Relight/RelightMath.h"
#include "../../AfxHookSource2/Filmmaker/Relight/RelightLightEditorMath.h"
#include "../../AfxHookSource2/Filmmaker/Relight/RelightStainedGlassMath.h"
#include "../../AfxHookSource2/Filmmaker/Relight/RelightEnvironmentMath.h"
#include "../../AfxHookSource2/Filmmaker/Relight/RelightConsoleMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShadowMath.h"
#include <iostream>
#include <cstdlib>
using namespace Filmmaker::Environment;
void Check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
bool Near(float a, float b, float epsilon = .001f) { return std::abs(a-b) < epsilon; }
int main() {
    {
        using namespace Filmmaker::Relight;
        Scene pendingLighting;
        Check(!MapLightingReady(pendingLighting,"de_boulder",false),"Pending map data preserves native lighting and shadows");
        pendingLighting.map="de_overpass";
        Check(!MapLightingReady(pendingLighting,"de_boulder",true),"Previous-map metadata cannot replace current-map shadows");
        pendingLighting.map="de_boulder";
        Check(!MapLightingReady(pendingLighting,"de_boulder",false),"Authored lights without geometry cannot render shadowless replacement lighting");
        Check(MapLightingReady(pendingLighting,"de_boulder",true),"Verified current-map geometry and metadata permit lighting takeover");
        {
            // Vertigo authored daylight, then the Environment night look applied over it.
            Scene day;day.sunAngles={62,234,0};day.sunBrightness=3.25f;day.sun=1;day.sunColor={1,.95f,.85f};
            day.skyColor={.57f,.68f,.8f};day.skyIntensity=1.65f;day.indirect=.3f;
            Check(Near(DaylightEnergyRatio(day,day),1),"Unchanged lighting keeps the skybox at its painted brightness");
            Climate night;night[CL_enabled]=1;night[CL_relighting]=1;night[CL_sky]=1;night[CL_sunElevation]=-12;night[CL_sunPower]=1;night[CL_ambient]=.08f;
            Scene lit=day;ApplyEnvironmentLighting(lit,night,{});
            const float ratio=DaylightEnergyRatio(day,lit);
            Check(ratio<BakedDaylightScale(-12.f,.08f)*.25f,"A night look dims the skybox as far as the relit world, past the sun-height floor");
            Climate brighter=night;brighter[CL_ambient]=1;Scene lifted=day;ApplyEnvironmentLighting(lifted,brighter,{});
            Check(DaylightEnergyRatio(day,lifted)>ratio,"Raising ambient lifts the skybox with the world");
            Climate dusk=night;dusk[CL_sunElevation]=20;Scene evening=day;ApplyEnvironmentLighting(evening,dusk,{});
            Check(DaylightEnergyRatio(day,evening)>ratio,"A risen sun relights the skybox");
            Scene dark;dark.sunBrightness=0;dark.skyIntensity=0;
            Check(Near(DaylightEnergyRatio(dark,lit),1),"A map without authored daylight leaves its skybox alone");
        }
        Check(!MapLightingReady(pendingLighting,"",true),"No map cannot own native lighting");
        for(Vec3 angles:{Vec3{0,0,0},Vec3{90,180,0},Vec3{25,138,65}}){
            Vec3 f,r,u;LightBasis(angles,f,r,u);float q[4];LightOrientationQuat(f,r,u,q);
            const Vec3 aim{1-2*(q[1]*q[1]+q[2]*q[2]),2*(q[0]*q[1]+q[3]*q[2]),2*(q[0]*q[2]-q[3]*q[1])};
            const Vec3 axis{2*(q[0]*q[1]-q[3]*q[2]),1-2*(q[0]*q[0]+q[2]*q[2]),2*(q[1]*q[2]+q[3]*q[0])};
            Check(Near(Dot(aim,f),1)&&Near(Dot(axis,r),-1),"Native quaternion aims +X and extends tubes along authored +Y");
            Check(Near(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3],1),"Authored orientation is a unit quaternion");
        }
        float bias,scale;ConeFade(30,80,bias,scale);
        const auto fade=[&](float angle){return std::clamp(bias-scale*std::cos(angle*3.14159265f/180),0.f,1.f);};
        Check(Near(fade(30),1)&&Near(fade(80),0)&&fade(55)>0&&fade(55)<1,"Cone fades continuously from inner to outer half-angle");
        Check(fade(120)==0,"Directed fixture cannot illuminate behind its cone");
        Light globe;globe.kind="light_omni2";globe.shape=0;globe.angles={90,0,0};
        globe.innerAngle=72.4f;globe.outerAngle=100;
        const auto globeRecord=ToRecord(globe,1);
        Check(Near(globeRecord[48]+globeRecord[49],1)&&Near(globeRecord[48]-globeRecord[49],1),
            "Omni globe illuminates both hemispheres despite retained spotlight properties");
        globe.kind="light_spot";const auto spotRecord=ToRecord(globe,1);
        Check(std::clamp(spotRecord[48]+spotRecord[49],0.f,1.f)==0&&
              std::clamp(spotRecord[48]-spotRecord[49],0.f,1.f)==1,
            "Spotlight retains directed emission instead of becoming an omni");
        Light barn;barn.kind="light_barn";barn.position={40,-20,60};barn.sizeX=8;barn.sizeY=16;barn.sizeZ=.1f;barn.range=100;
        auto record=ToRecord(barn,1);
        const auto ndc=[&](Vec3 p){auto m=FixtureProjection(barn);const float w=m[12]*p.x+m[13]*p.y+m[14]*p.z+m[15];return Vec3{(m[0]*p.x+m[1]*p.y+m[2]*p.z+m[3])/w,(m[4]*p.x+m[5]*p.y+m[6]*p.z+m[7])/w,(m[8]*p.x+m[9]*p.y+m[10]*p.z+m[11])/w};};
        Check(Near(ndc({40,-12,60}).x,1)&&Near(ndc({40,-20,76}).y,1),"Barn size parameters are authored half extents");
        Check(Near(ndc({50,-12,60}).x,.5f)&&Near(ndc({140,-20,60}).z,1),"Perspective fixture expands from the aperture toward authored range");
        Check(record[56]==.75f&&record[57]==.75f&&FixtureFaces(barn)==1,"Barn uses authored soft edges and one projection");
        barn.softX=barn.softY=0;record=ToRecord(barn,1);
        const auto edgeFade=[&](float x){float t=std::clamp((1-std::abs(x))/(1-record[56]),0.f,1.f);return t*t*(3-2*t);};
        Check(edgeFade(.9f)>0&&edgeFade(.9f)<1&&edgeFade(1)==0&&edgeFade(.8f)==1,
            "Zero-softness fill lights retain a continuous finite feather instead of a hard projected box");
        barn.softX=.6f;Check(Near(ToRecord(barn,1)[56],.4f),"A softer authored fill remains softer than the runtime minimum");
        barn.sourceRadius=AuthoredSourceRadius(barn.sizeX,barn.sizeY);
        const auto projectedShadow=ToShadowedRecord(barn,1,FixtureProjection(barn).data(),0);
        // Depth is zero FixtureShadowClearance in front of the emitter (near 10) and one at range.
        const float start=FixtureDepthStart(barn);
        const auto projectedDepth=[&](float z){return (1/start-1/z)/(1/start-1/110.f);};
        Check(Near(start,10+FixtureShadowClearance)&&Near(ndc({40+FixtureShadowClearance,-20,60}).z,0)&&ndc({41,-20,60}).z<0,
            "Fixture shadow depth starts a few units in front of the emitter, so a recessed ceiling tile cannot occlude the light");
        Check(Near((projectedDepth(80)-projectedDepth(20))*projectedShadow[84],8.f*(1.f/20-1.f/80)),
            "Projector penumbra uses the decoded finite source radius instead of doubling the window aperture");
        barn.roundness=.878f;
        Check(Near(ToRecord(barn,1)[58],.878f),"Fixture preserves fractional authored edge roundness");
        Light tube;tube.kind="light_omni2";tube.shape=1;tube.sizeY=14;tube.range=400;tube.sourceRadius=1;
        const auto shadow=ToShadowedRecord(tube,1,Identity().data(),0);
        const auto depth=[](float z){return (400.f/399)*(1-1/z);};
        const float angular=(depth(80)-depth(20))*shadow[84]*ShadowFaceBlendWidth;
        Check(Near(angular,14.f*(1.f/20-1.f/80)),"Perspective blocker depth produces the physical source-to-receiver penumbra");
        Check(Near((depth(20)-depth(20))*shadow[84],0),"Fixture shadow stays sharp at caster contact");
        std::vector<Light> lights(5);for(unsigned i=0;i<5;++i){lights[i].lumens=10;lights[i].position={float(i*10+10),0,0};}
        auto slots=StableShadowBudget(lights,{0,0,0},4,{});
        Check(StableShadowBudget(lights,{25,0,0},4,slots)==slots,"Crossing distance rankings preserves resident shadow atlas slots");
        auto promoted=StableShadowBudget(lights,{51,0,0},4,slots);unsigned changed=0;for(unsigned i=0;i<slots.size();++i)changed+=slots[i]!=promoted[i];
        Check(changed==1&&std::find(promoted.begin(),promoted.end(),4)!=promoted.end(),"Only one clearly closer fixture promotes per frame");
    }
    {
        using namespace Filmmaker::Relight;
        Scene source;
        const auto defaultSky=SkyAmbient(source,source.indirect);
        Check(defaultSky.x>0&&defaultSky.z>defaultSky.x,"Standalone relighting starts with authored skylight contributing to GI");
        Check(SkyAmbient(source,0).x==0,"An explicit zero sky contribution remains dark");
        source.enabled=true;source.sunAngles={45,0,0};source.sunBrightness=1;
        source.sunColor={1,.8f,.6f};source.skyColor={.4f,.6f,1};source.skyIntensity=1;
        source.lights.resize(1);source.lights[0].color={1,.7f,.3f};
        WorldLook world;world.fields["lightColor"]={255,0,0};
        Scene red=source;ApplyGlobalLighting(red,world);
        Check(red.enabled&&red.sunColor.x==source.sunColor.x&&red.sunColor.y==0&&red.sunColor.z==0,
              "Standalone relighting receives global light colour without changing enablement");
        Check(red.lights[0].color.x==1&&red.lights[0].color.y==0&&red.skyColor.z==0,
              "Global colour reaches lamp and sky inputs as well as the sun");
        const auto redSun=SunRadiance(red),redSky=SkyAmbient(red,1);
        Check(redSun.x>0&&redSun.y==0&&redSky.x>0&&redSky.z==0,"Published sun and sky radiance retain global hue");
        world.fields["lightColor"]={0,0,255};Scene blue=source;ApplyGlobalLighting(blue,world);
        Check(blue.generation!=red.generation&&blue.lights[0].color.z>0&&blue.sunColor.x==0,
              "Hue edits invalidate GI and reach the current light records");
        source.sunColor=source.skyColor={0,0,0};Scene lampsBlue=source;ApplyGlobalLighting(lampsBlue,world);
        world.fields["lightColor"]={255,0,0};Scene lampsRed=source;ApplyGlobalLighting(lampsRed,world);
        Check(lampsBlue.generation!=lampsRed.generation,"Lamp-only tint edits invalidate the probe field");
        source.enabled=false;Scene disabled=source;ApplyGlobalLighting(disabled,world);
        Check(!disabled.enabled,"Global colour does not enable an inactive relight scene");
        source=Scene{};source.sunBrightness=1;source.lights.resize(1);
        world.fields["lightColor"]={128,64,32};world.fields["lightAmount"]={.5};
        Scene standalone=source;ApplyGlobalLighting(standalone,world);
        Scene environment=source;ApplyEnvironmentLighting(environment,Climate{},world);
        Check(Near(standalone.lights[0].color.x,environment.lights[0].color.x)&&
              Near(standalone.intensity,environment.intensity)&&Near(environment.lights[0].color.x,128.f/255),
              "Environment applies global tint and intensity exactly once");
        Scene overcast=Scene{};overcast.sunBrightness=0;overcast.source="maps/cs_office";overcast.lights.resize(1);
        Climate sunny;sunny[CL_enabled]=1;sunny[CL_sunPower]=2;ApplyEnvironmentLighting(overcast,sunny,WorldLook{});
        Check(overcast.sunBrightness>0&&SunRadiance(overcast).x>0,
              "A map that authors a zero-brightness sun still gets the Environment's sun");
    }
    {
        using namespace Filmmaker::Relight;
        Scene source;source.intensity=2;source.sun=3;source.shadows=true;
        source.sunBrightness=2;source.lights.resize(1);source.lights[0].lumens=250;
        Climate climate;climate[CL_enabled]=1;climate[CL_ambient]=.3f;
        WorldLook world;
        Scene lit=source;ApplyEnvironmentLighting(lit,climate,world);
        Check(Near(lit.indirect,.3f)&&Near(lit.intensity,2)&&Near(lit.sun,3)&&lit.shadows,
              "Environment lamp, sky and sun controls remain independent");
        climate[CL_contactShadows]=0;climate[CL_mapLightPower]=2;climate[CL_ambient]=0;
        Scene edited=source;ApplyEnvironmentLighting(edited,climate,world);
        Check(!edited.shadows&&Near(edited.intensity,4)&&edited.indirect==0&&Near(edited.sun,3),
              "Shadow switch reaches the owned path; turning off ambient keeps map lamps and sun");
        Check(edited.generation!=lit.generation,"Lighting edits invalidate the current lighting field");
        climate[CL_bounce]=0;Scene noBounce=source;ApplyEnvironmentLighting(noBounce,climate,world);
        Check(noBounce.gi&&noBounce.bounce==0,"Zero bounce retains the probe field that occludes skylight");
        climate[CL_bounceSteps]=3;Scene fastBounce=source;ApplyEnvironmentLighting(fastBounce,climate,world);
        Check(fastBounce.bounceSteps==3&&fastBounce.bounce==0,"Three bounce updates reach probe transport independently of display intensity");
        Scene disabledGi=source;disabledGi.gi=false;ApplyEnvironmentLighting(disabledGi,climate,world);
        Check(!disabledGi.gi,"A deliberate GI diagnostic override remains off");
        climate[CL_bounce]=1;
        climate[CL_sunPower]=0;climate[CL_bakedFill]=4;climate[CL_relight]=1;
        Scene night=source;ApplyEnvironmentLighting(night,climate,world);
        Check(night.sun==0&&night.indirect==0&&Near(night.intensity,4),
              "Sun off retains lamps and never introduces baked or flattened fill");
        Check(HasLocalRadiance(night)&&!HasRadiance(SunRadiance(night)),
              "At night only emitting map fixtures require direct shadow work");
        night.intensity=0;
        Check(!HasLocalRadiance(night),"The map-light blackout skips direct lighting and its shadow work");
        night.intensity=1;night.lights[0].color={0,0,0};
        Check(!HasLocalRadiance(night),"A black light tint emits no energy even with positive lumens");
        night.lights[0].color={1,1,1};night.lights[0].enabled=false;
        Check(!HasLocalRadiance(night),"Disabled fixtures do not keep the direct shadow pass alive");
        night.lights[0].enabled=true;night.localLights=false;
        Check(!HasLocalRadiance(night),"The local-light switch suppresses fixture shadow work");
    }
    {
        float value=5;
        for(const char* text:{"","garbage","1suffix","nan","inf","-inf","1e999"})
            Check(!Filmmaker::Relight::ParseFiniteNumber(text,value)&&value==5,"invalid controls cannot corrupt renderer state");
        Check(Filmmaker::Relight::ParseFiniteNumber("-1.25e2",value)&&value==-125,"finite scientific numeric control");
    }
    {
        using namespace Filmmaker::Relight;
        const auto east = SunDirection({0, 0, 0}), north = SunDirection({0, 90, 0});
        const auto overhead = SunDirection({90, 237, 0});
        Check(Near(east.x, 1) && Near(east.y, 0) && Near(east.z, 0), "Zero angles point east");
        Check(Near(north.x, 0) && Near(north.y, 1), "Azimuth rotates around world Z");
        Check(Near(overhead.z, 1) && Near(overhead.x, 0), "Zenith is independent of azimuth");
        const auto mirage=SunDirection(AuthoredSunAngles({60,318,0}));
        Check(Near(mirage.x,-.371572f)&&Near(mirage.y,.334565f)&&Near(mirage.z,.866025f),
              "Mirage authored sun matches live native cb3[1945], including ray-direction inversion");
        Scene scene; scene.sunBrightness = 3; scene.sun = 2; scene.sunColor = {1, .5f, .25f};
        for (float elevation : {-90.f, -1.f, 0.f}) {
            scene.sunAngles.x = elevation;
            Check(SunRadiance(scene).x == 0, "Sun below the horizon contributes no direct light");
        }
        scene.sunAngles.x = 30;
        const auto day = SunRadiance(scene);
        Check(Near(day.x, 6) && day.x > day.y && day.y > day.z, "Sun uses linear authored color and brightness");
        scene.sunAngles.x = .5f;
        Check(Near(SunRadiance(scene).x, day.x * .5f), "Current sun fade spans the first degree");
        float diagA[4]{}, diagB[4]{};
        LightmapDiagTexel(0, 0, LightmapDiagSize, diagA);
        LightmapDiagTexel(LightmapDiagSize - 1, LightmapDiagSize - 1, LightmapDiagSize, diagB);
        Check(diagA[0] < diagB[0] && diagA[1] < diagB[1], "Lightmap diag encodes UV spatially");
        Check(diagA[3] == 1 && diagB[3] == 1, "Lightmap diag keeps opaque alpha");
        Check(Near(AuthoredSourceRadius(24.f, 8.35f), 12.f), "Rect size_params use the larger half-extent");
        Check(Near(AuthoredSourceRadius(5.f, 5.f), 2.5f), "Square barn size_params bound the near field");
        Check(Near(AuthoredSourceRadius(0.f, 0.f), 16.f), "Missing size_params keep the fixture default");
        Light bulb;bulb.kind="light_omni2";bulb.shape=0;bulb.sizeX=2.714437f;bulb.sizeY=24;bulb.range=450;
        Check(Near(AuthoredShadowNearClip(bulb),2.714437f),"Spherical emitter starts outside its bulb, not at half the inactive planar size");
        bulb.shape=-1;Check(AuthoredShadowNearClip(bulb)==1,"Unknown emitter shape retains conservative geometry coverage");
        bulb.shape=0;bulb.kind="light_rect";Check(AuthoredShadowNearClip(bulb)==1,"A planar emitter cannot discard its housing using a sphere radius");
        {
            Vec3 forward{}, right{}, up{};
            LightBasis({0, 0, 0}, forward, right, up);
            Check(Near(forward.x, 1) && Near(forward.y, 0) && Near(forward.z, 0),
                  "Zero angles aim along +X");
            Light chamber;
            chamber.kind = "light_rect";
            chamber.position = {-632, 1352, 350};
            chamber.angles = {44.86f, 117.67f, 13.39f};
            chamber.sizeX = 24.f;
            chamber.sizeY = 8.35f;
            chamber.range = 1024;
            chamber.lumens = 1600;
            chamber.color = {1, .84f, .71f};
            const auto shaped = ToRecord(chamber, 1);
            Check(std::abs(shaped[0]) + std::abs(shaped[1]) + std::abs(shaped[2]) > .01f,
                  "Shaped rect writes an oriented clip row");
            Check(shaped[47] != 0 || shaped[44] != 0 || shaped[45] != 0 || shaped[46] != 0,
                  "Shaped rect stores a non-zero orientation quat");
            Light omni;
            omni.kind = "light_omni2";
            omni.position = chamber.position;
            omni.range = 1024;
            omni.lumens = 100;
            const auto sphere = ToRecord(omni, 1);
            Check(Near(sphere[0], sphere[5]) && Near(sphere[47], 1.f),
                  "Omni keeps axis-aligned identity orientation");
        }
        const auto plain = SunRecord(east, day, nullptr, false);
        Check(plain[35] == 0 && plain[37] == 0, "Sun is directional and has no finite cull radius");
        Check(plain[54] == 0 && plain[65] == 0, "Sun without an atlas never samples a shadow");
        Matrix transform{}; transform[0] = transform[5] = transform[10] = transform[15] = 1;
        const auto shadowed = SunRecord(east, day, transform.data(), true);
        Check(shadowed[54] > 0 && shadowed[65] == 1 && shadowed[40] == plain[40],
              "Adding the sun atlas preserves radiance and enables shadow sampling");
    }
    {
        // Authored map lights convert to the native clustered-light record the
        // shipped forward shaders read. Check the record through the operations
        // that bytecode performs, rather than restating its constants.
        Filmmaker::Relight::Light lamp;
        lamp.kind = "light_omni2";
        lamp.position = {-1701, -554, -21};
        lamp.color = {1, 215 / 255.f, 183 / 255.f};
        lamp.lumens = 300;
        lamp.range = 900;
        const auto record = Filmmaker::Relight::ToRecord(lamp, 4);
        // The shader transforms world position by rows 0-3, divides by w, then keeps
        // x and y inside (-1,1) and z inside (0,1). The light must land dead centre
        // of its own volume and the authored range must fall outside it.
        const auto clip = [&](Vec3 p) {
            return Vec3{record[0] * p.x + record[1] * p.y + record[2] * p.z + record[3],
                        record[4] * p.x + record[5] * p.y + record[6] * p.z + record[7],
                        record[8] * p.x + record[9] * p.y + record[10] * p.z + record[11]};
        };
        const auto centre = clip(lamp.position);
        Check(Near(centre.x, 0) && Near(centre.y, 0) && Near(centre.z, .5f),
              "Authored light centres its own cull volume");
        Check(clip(lamp.position + Vec3{901, 0, 0}).x > 1, "Authored range bounds the cull volume");
        Check(clip(lamp.position + Vec3{899, 0, 0}).x < 1, "Surfaces inside the authored range stay lit");
        // Row 9 is saturate(y * distance + x), so full strength at the light and
        // zero exactly at the authored range.
        const auto attenuation = [&](float distance) { return record[36] + record[37] * distance; };
        Check(Near(attenuation(0), 1), "Attenuation is full strength at the light");
        Check(Near(attenuation(lamp.range), 0), "Attenuation reaches zero at the authored range");
        Check(attenuation(lamp.range * .5f) > 0 && attenuation(lamp.range * .5f) < 1,
              "Attenuation falls off across the range");
        Check(record[40] > record[41] && record[41] > record[42],
              "A warm authored colour stays warm after sRGB decoding");
        const auto brighter = Filmmaker::Relight::ToRecord(lamp, 8);
        Check(Near(brighter[40], record[40] * 2), "Intensity scales radiance linearly");
        lamp.enabled = false;
        const auto off = Filmmaker::Relight::ToRecord(lamp, 4);
        Check(off[40] == 0 && off[41] == 0 && off[42] == 0, "A disabled light emits nothing");
        Check(off[32] == record[32], "A disabled light keeps its authored position");
        lamp.enabled = true;
        // Shaping must stay on the analytic branch. A non-zero selector routes the
        // shader into cookie and IES paths whose textures we do not supply.
        Check(record[43] == 0, "Light shaping stays on the branch that samples no cookie");
        Check(record[56] < 1 && record[56] > .99f, "Edge fade endpoints avoid a zero denominator");
        // Nothing is baked once the lightmap inputs are neutralised, so the shader
        // must not scale an owned light down by a baked share of it.
        for (unsigned i = 60; i < 64; ++i)
            Check(record[i] == 0, "No baked contribution is subtracted from an owned light");
        Check(record[35] != 0, "Position carries the marker that requests camera-relative rebasing");
        Check(record[47] == 1 && record[48] == 1 && record[49] == 0,
              "An unshaped light passes every direction");
        Check(record[52] == 0 && record[54] == 0, "A light with no atlas tile requests no shadow");
        // The shader attenuates by radiusSquared / max(radiusSquared, distanceSquared).
        // Past the emitter those cancel, so changing the modelled emitter size must
        // not change how bright a distant surface is -- it may only bound the peak.
        const auto illumination = [](const Filmmaker::Relight::LightRecord &r, float distance) {
            const float radiusSquared = r[35];
            const float falloff = radiusSquared / (std::max)(radiusSquared, distance * distance);
            const float curve = (std::max)(0.f, (std::min)(1.f, r[36] + r[37] * distance));
            return r[40] * falloff * curve;
        };
        Filmmaker::Relight::Light small = lamp, large = lamp;
        small.sourceRadius = 2;
        large.sourceRadius = 32;
        const auto smallRecord = Filmmaker::Relight::ToRecord(small, 4);
        const auto largeRecord = Filmmaker::Relight::ToRecord(large, 4);
        for (float distance : {100.f, 300.f, 600.f})
            Check(Near(illumination(smallRecord, distance), illumination(largeRecord, distance), .01f),
                  "Emitter size does not change how brightly a distant surface is lit");
        Check(illumination(largeRecord, 1) < illumination(smallRecord, 1),
              "A larger emitter bounds the near field more tightly");
        // A point emitter drives radiance towards the half-float ceiling of the HDR
        // target, and a saturated target tone maps to black rather than to white.
        // Mirage's brightest authored light is 1500 lumens.
        Filmmaker::Relight::Light brightest = lamp;
        brightest.lumens = 1500;
        const auto peak = Filmmaker::Relight::ToRecord(brightest, 4)[40];
        Check(peak * 72 < 65504.f, "The whole authored light set cannot saturate a half-float target");
    }
    {
        // Clustered capacity is thirty-two lights per mask word, and every record
        // has to be reachable: a light no mask bit names is silently unlit.
        Check(Filmmaker::Relight::MaskWords(0) == 1, "An empty light set still addresses one word");
        Check(Filmmaker::Relight::MaskWords(1) == 1 && Filmmaker::Relight::MaskWords(32) == 1, "One word carries thirty-two lights");
        Check(Filmmaker::Relight::MaskWords(33) == 2, "A thirty-third light needs a second word");
        Check(Filmmaker::Relight::MaskWords(73) == 3, "Mirage's authored light set needs three words");
        Check(Filmmaker::Relight::MaskWords(238) == 8, "Train's authored light set needs eight words");
        const size_t records = 73;
        const auto mask = Filmmaker::Relight::FullMask(records, 1);
        Check(mask.size() == Filmmaker::Relight::MaskWords(records), "The mask covers exactly the words the loop reads");
        unsigned reachable = 0;
        for (size_t light = 0; light < records; ++light)
            if (mask[light / 32] & (1u << (light % 32)))
                ++reachable;
        Check(reachable == records, "Every authored light is reachable through the cluster mask");
        Check((mask[2] & ~((1u << (records % 32)) - 1)) == 0, "No bit names a light that does not exist");
    }
    {
        // Only a handful of lights can own shadow-atlas tiles, so the choice has to
        // follow the view and respect what the map author asked for.
        std::vector<Filmmaker::Relight::Light> lights;
        auto add = [&](float x, int castShadows, float lumens) {
            Filmmaker::Relight::Light light;
            light.kind = "light_omni2";
            light.position = {x, 0, 0};
            light.range = 100;
            light.lumens = lumens;
            light.castShadows = castShadows;
            lights.push_back(light);
            return unsigned(lights.size() - 1);
        };
        const unsigned near = add(200, 2, 500);
        const unsigned far = add(4000, 2, 500);
        const unsigned middle = add(900, 2, 500);
        const unsigned sunlightOnly = add(210, 1, 500);
        const unsigned never = add(220, 0, 500);
        const unsigned unlit = add(230, 2, 0);
        const Vec3 camera{0, 0, 0};
        const auto chosen = Filmmaker::Relight::ShadowBudget(lights, camera, 4);
        Check(chosen.size() == 4, "Compiler shadow modes do not suppress runtime fixture shadows");
        for (auto index : chosen) {
            Check(index != unlit, "A light emitting nothing is not worth a shadow tile");
        }
        Check(chosen[0] == near && chosen[1] == sunlightOnly && chosen[2] == never && chosen[3] == middle,
              "Room fixtures receive high-resolution shadows regardless of compiler mode");
        Check(Filmmaker::Relight::ShadowBudget(lights, camera, 2).size() == 2, "The budget is a hard cap");
        Check(Filmmaker::Relight::ShadowBudget(lights, camera, 0).empty(), "A zero budget casts no shadows at all");
        lights[near].enabled = false;
        const auto withoutNear = Filmmaker::Relight::ShadowBudget(lights, camera, 4);
        Check(withoutNear.size() == 4 && withoutNear[0] == sunlightOnly && withoutNear.back()==far,
              "A disabled light gives up its tile to the next emitting fixture");
        lights[far].range=100000;
        Check(Filmmaker::Relight::ShadowBudget(lights,camera,1)[0]==sunlightOnly,
              "A remote large-range fixture cannot displace the nearest room light");
    }
    {
        // A face samples the region it was rendered into. If the rect and the region ever
        // disagree, a light samples another light's depth and shadows appear unrelated to
        // their caster, so check the mapping rather than the arithmetic.
        using namespace Filmmaker::Relight;
        auto uv = [&](const float *rect, float clipX, float clipY) {
            return std::pair<float, float>{clipX * rect[2] + rect[0], clipY * rect[3] + rect[1]};
        };
        float sun[4];
        SunRect(sun);
        const auto sunLow = uv(sun, -1, -1), sunHigh = uv(sun, 1, 1);
        Check(Near((std::min)(sunLow.first, sunHigh.first), 0, .001f) &&
              Near((std::max)(sunLow.first, sunHigh.first), float(SunCascadeSize) / AtlasWidth, .001f),
              "The full-map sun fallback occupies its own quadrant");
        // Every face of either tier must stay inside its own tile, clear of the sun's
        // square, and clear of every other tile. Overlapping tiles is the failure that
        // would show up as one light casting another light's shadows.
        for (unsigned tile = 0; tile < ShadowTiles; ++tile) {
            if (tile == SunTile) continue;
            float rect[4];
            TileRect(tile, rect);
            const auto lo = uv(rect, -1, -1), hi = uv(rect, 1, 1);
            unsigned x = 0, y = 0, size = 0;
            TileOrigin(tile, x, y, size);
            Check(size == (tile < SunTile ? LocalSize : tile >= SunMiddleTile ? SunCascadeSize : StaticSize),
                  "A tile is sized by the tier it belongs to");
            Check(x + size <= AtlasWidth && y + size <= AtlasHeight,
                  "A tile stays inside the atlas");
            Check((std::min)(lo.first, hi.first) >= float(x) / AtlasWidth - .001f &&
                  (std::max)(lo.first, hi.first) <= float(x + size) / AtlasWidth + .001f,
                  "A face stays inside its own tile horizontally");
            Check((std::min)(lo.second, hi.second) >= float(y) / AtlasHeight - .001f &&
                  (std::max)(lo.second, hi.second) <= float(y + size) / AtlasHeight + .001f,
                  "A face stays inside its own tile vertically");
            if(tile>=SunMiddleTile){
                Check(x+size<=SunSize&&y+size<=SunSize,"Detail cascades stay in the existing sun footprint");
                continue;
            }
            Check((std::min)(lo.first, hi.first) >= float(SunSize) / AtlasWidth - .001f,
                  "A local face never samples the sun's square");
            // The two tiers occupy disjoint columns, so neither can overwrite the other.
            if (tile < SunTile)
                Check(x + size <= StaticOriginX, "A budgeted face stays out of the cached tier");
            else
                Check(x >= StaticOriginX, "A cached face stays out of the budgeted tier");
        }
        // The whole point of the cached tier is that it holds enough lights for a real map
        // to have no shadowless ones left. Mirage carries about seventy.
        Check(StaticShadowLimit == 245, "The cached tier holds a map's worth of lights (the grid's last 64 tiles are stained glass beams)");
        Check(GlassBeamTile >= StaticTile + StaticShadowLimit * ShadowFaces && GlassBeamTile + GlassBeamTiles == SunMiddleTile && !IsStaticFaceTile(GlassBeamTile),"Beam shadow tiles sit after the lamp faces, inside the grid, without the face guard");
        float first[4], second[4];
        TileRect(0, first);
        TileRect(1, second);
        Check(first[0] != second[0], "Different faces occupy different regions");
        Check(first[3] < 0 && sun[3] < 0, "The vertical axis flips between clip and texture space");
        // The sun is why shadows looked blocky: it covers a whole map in one projection.
        Check(SunSize >= 4 * LocalSize, "The sun is given far more resolution than one cube face");
        // The nine-tap filter weights must sum to one, or a surface in full sun comes out
        // darker than lit and a surface in full shadow lighter than shadowed.
        float rows[16];
        ShadowFilterRows(2, rows);
        const float total = 4 * rows[3] + 4 * rows[4] + rows[5];
        Check(Near(total, 1), "The shadow filter weights sum to one");
        Check(rows[9] > 0 && Near(rows[10], -rows[9]), "Horizontal taps straddle the sample");
        Check(rows[13] > 0 && Near(rows[14], -rows[13]), "Vertical taps straddle the sample");
        // A hard shadow is the zero-radius case, not a different kernel.
        float hard[16];
        ShadowFilterRows(0, hard);
        Check(hard[9] == 0 && hard[13] == 0, "Zero softness collapses the taps onto one texel");
        Check(Near(4 * hard[3] + 4 * hard[4] + hard[5], 1), "A hard filter still sums to one");
        // Widening the radius has to widen the kernel, which is what softens the penumbra.
        float wide[16];
        ShadowFilterRows(6, wide);
        Check(wide[9] > rows[9] && wide[13] > rows[13], "More softness spreads the taps further");
        // The atlas is wider than it is tall, so a texel is not square; the offsets have to
        // follow that or the penumbra comes out stretched along one axis. Both are tiny, so
        // compare their ratio rather than their difference.
        Check(Near(rows[13] / rows[9], float(AtlasWidth) / AtlasHeight, .01f),
              "Tap offsets follow the atlas texel shape rather than being square");
        // The shader treats a zero atlas scale as "this light has no shadow map" and
        // skips the lookup, which is what keeps unbudgeted lights working.
        Filmmaker::Relight::Light light;
        light.kind = "light_omni2";
        light.position = {10, 20, 30};
        light.lumens = 500;
        light.range = 400;
        const auto plain = Filmmaker::Relight::ToRecord(light, 1);
        Check(plain[54] == 0, "An unbudgeted light asks for no shadow lookup");
        Check(plain[65] == 0, "An unbudgeted light has no shadow strength");
        Matrix face{};
        for (unsigned i = 0; i < 16; ++i) face[i] = float(i) + 1;
        const auto shadowed = Filmmaker::Relight::ToShadowedRecord(light, 1, face.data(), 9);
        for (unsigned i = 0; i < 16; ++i)
            Check(Near(shadowed[i],face[i]*(i<8?ShadowFaceBlendStart:1)), "Cube culling overlaps its neighbours while preserving shadow depth");
        Check(shadowed[54] > 0, "A shadowed face asks for the shadow lookup");
        Check(shadowed[65] == 1, "A shadowed face applies its shadow at full strength");
        Check(shadowed[40] == plain[40] && shadowed[37] == plain[37],
              "Adding a shadow changes neither radiance nor falloff");
        float expected[4];
        Filmmaker::Relight::TileRect(9, expected);
        expected[2]*=ShadowFaceBlendWidth;expected[3]*=ShadowFaceBlendWidth;
        for (unsigned i = 0; i < 4; ++i)
            Check(shadowed[52 + i] == expected[i], "A shadowed face carries its own atlas rect");
        // A cached face addresses a tile in the far grid, and its rect has to be the
        // smaller one -- reading a 1024-sized rect out of a 128 tile would sample six
        // neighbouring lights' depth instead of its own.
        const auto cachedFace =
            Filmmaker::Relight::ToShadowedRecord(light, 1, face.data(), StaticTile);
        Check(cachedFace[54] > 0 && cachedFace[54] < shadowed[54],
              "A cached face samples a smaller region of the atlas than a budgeted one");
        Check(Near(cachedFace[54], StaticSize * .5f / StaticFaceGuard / AtlasWidth * ShadowFaceBlendWidth, 1e-6f),
              "A cached face's overlapping projection stays within its own guarded tile");
    }
    {
        // Which lights the cached tier takes. The selection must not depend on the camera:
        // these faces are drawn once and kept, so a set that reshuffled as the camera moved
        // would redraw them forever.
        using Filmmaker::Relight::Light;
        std::vector<Light> lights(6);
        for (unsigned i = 0; i < lights.size(); ++i) {
            lights[i].kind = "light_omni2";
            lights[i].lumens = float(100 * (i + 1));
            lights[i].position = {float(i) * 100, 0, 0};
        }
        const std::vector<unsigned> budgeted{5};
        const auto near = Filmmaker::Relight::StaticShadowSet(lights, budgeted, 3);
        const auto far = Filmmaker::Relight::StaticShadowSet(lights, {0,1}, 3);
        Check(near == far, "The cached set does not depend on where the camera stands");
        Check(near.size() == 3, "The cached set fills its limit");
        Check(std::find(near.begin(), near.end(), 5u) != near.end(),
              "Promoted lights retain cached residency when the camera moves away");
        // Brightest first, so the lights that lose out on a crowded map are the dimmest.
        Check(std::find(near.begin(), near.end(), 4u) != near.end() &&
                  std::find(near.begin(), near.end(), 3u) != near.end() &&
                  std::find(near.begin(), near.end(), 5u) != near.end(),
              "The brightest remaining lights are the ones cached");
        Check(std::is_sorted(near.begin(), near.end()),
              "The set is returned in authored order so a light keeps its tile");
        // An unlit or disabled light would render an empty face and waste a tile.
        lights[4].enabled = false;
        lights[3].lumens = 0;
        const auto filtered = Filmmaker::Relight::StaticShadowSet(lights, budgeted, 3);
        Check(std::find(filtered.begin(), filtered.end(), 4u) != filtered.end() &&
                  std::find(filtered.begin(), filtered.end(), 3u) == filtered.end(),
              "Disabling a fixture preserves its tile for re-enablement");
    }
    {
        // The fallback ambient rows are evaluated against the surface normal, so a flat
        // constant lights a ceiling and a floor identically. Check the hemisphere through
        // the same dot product the native shader performs.
        const Vec3 sky{.2f, .4f, .8f}, ground{.3f, .2f, .1f};
        float rows[12];
        Filmmaker::Relight::AmbientRows(sky, ground, rows);
        const auto evaluate = [&](Vec3 normal) {
            Vec3 out{};
            float *channel = &out.x;
            for (unsigned c = 0; c < 3; ++c)
                channel[c] = rows[c * 4 + 0] * normal.x + rows[c * 4 + 1] * normal.y +
                             rows[c * 4 + 2] * normal.z + rows[c * 4 + 3];
            return out;
        };
        const auto up = evaluate({0, 0, 1}), down = evaluate({0, 0, -1}), side = evaluate({1, 0, 0});
        Check(Near(up.x, sky.x) && Near(up.y, sky.y) && Near(up.z, sky.z),
              "An upward face sees the sky");
        Check(Near(down.x, ground.x) && Near(down.y, ground.y) && Near(down.z, ground.z),
              "A downward face sees the bounce");
        Check(Near(side.x, (sky.x + ground.x) * .5f), "A vertical face sees half of each");
        Check(up.z > side.z && side.z > down.z, "Ambient varies with how the face is turned");
        // Zero indirect has to mean zero, or "lights off" stops being a dark baseline.
        Filmmaker::Relight::Scene set;
        set.skyColor = {.5f, .7f, 1};
        set.skyIntensity = 1;
        const auto none = Filmmaker::Relight::SkyAmbient(set, 0);
        Check(none.x == 0 && none.y == 0 && none.z == 0, "No indirect means no skylight");
        const auto some = Filmmaker::Relight::SkyAmbient(set, .5f);
        Check(some.z > some.x, "Skylight keeps the authored sky hue");
        const auto bounce = Filmmaker::Relight::GroundAmbient({1, .8f, .6f}, .55f, 1);
        Check(bounce.x > bounce.z && bounce.x < 1, "The ground bounce is a dimmed, warm share of the sun");
        Check(Filmmaker::Relight::GroundAmbient({1, .8f, .6f}, .55f, 0).x == 0, "No indirect means no bounce");
    }
    {
        using namespace Filmmaker::Relight;
        auto light=[](const char* kind,Vec3 p){Light l;l.kind=kind;l.position=p;return l;};
        // Light editor keys and aiming.
        Light named=light("light_omni2",{1.4f,-2.6f,3});
        Check(LightEditorKey(named)=="light_omni2@1,-3,3","An unnamed light is keyed by class and rounded position");
        named.identity="lamp_7";Check(LightEditorKey(named)=="lamp_7","A light with an authored identity keeps it");
        const float identity[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};float sx=0,sy=0;
        Check(ProjectToScreen(identity,{0,0,0},200,100,sx,sy)&&Near(sx,100)&&Near(sy,50),"Clip origin lands at the screen centre");
        const float behind[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,-1,0};Check(!ProjectToScreen(behind,{0,0,1},200,100,sx,sy),"Points behind the camera are not drawn");
        std::vector<LightMarker> markers(3);
        markers[0]={"far",103,52,500,true};markers[1]={"near",106,49,100,true};markers[2]={"off",100,50,50,false};
        Check(AimedLightMarker(markers,200,100)==1,"Among lights under the crosshair the nearer one is aimed");
        markers[1].x=170;Check(AimedLightMarker(markers,200,100)==0,"A light far from the crosshair is not aimed");
        markers[0].x=180;Check(AimedLightMarker(markers,200,100)==-1,"Nothing is aimed when no light is near the crosshair");
        markers[0].x=101;markers[0].distance=2500;Check(AimedLightMarker(markers,200,100)==-1,"A light across the map cannot be picked, however well aimed");
        Check(LightMarkerOpacity(100)==1&&Near(LightMarkerOpacity(5000),LightMarkerFaint)&&LightMarkerOpacity(1200)<1&&LightMarkerOpacity(1200)>LightMarkerFaint,
              "Markers are clear nearby and fade to faint (not invisible) far away");
    }
    {
        using namespace Filmmaker::Relight;
        // Two windows 200 units apart    {
        using namespace Filmmaker::Relight;
        // Two windows 200 units apart; the first is two panes 4 units apart (lead came).
        auto quad=[](Vec3 o,float w,float h){return std::vector<GlassTriangle>{{o,o+Vec3{w,0,0},o+Vec3{w,0,h}},{o,o+Vec3{w,0,h},o+Vec3{0,0,h}}};};
        std::vector<GlassTriangle> glass;
        for(auto part:{quad({0,0,0},40,80),quad({44,0,0},40,80),quad({300,0,0},60,60)})glass.insert(glass.end(),part.begin(),part.end());
        const auto windows=GroupGlassWindows(glass);
        Check(windows.size()==2&&windows[0].triangles.size()==4&&windows[1].triangles.size()==2,"Panes joined by lead came form one window; separate windows stay apart");
        // Sun above and in front (-y side): light travels +y into the room behind the glass.
        const Vec3 towardSun=Normalize(Vec3{0,-1,1});
        std::vector<Vec3> points;for(auto i:windows[0].triangles)for(Vec3 p:{glass[i].a,glass[i].b,glass[i].c})points.push_back(p);
        const auto beam=GlassBeamProjection(points,towardSun);
        Check(beam.valid,"A window has a beam");
        for(Vec3 p:points){const auto c=BeamClip(beam,p);
            Check(std::abs(c.x)<=1.0001f&&std::abs(c.y)<=1.0001f&&c.z<=0,"The glass itself spans the cookie and sits at or before the beam start");}
        // Follow the light from the window centre 300 units: inside the beam, behind the glass.
        const Vec3 centre{42,0,40},inside=centre-towardSun*300,outside=centre+towardSun*300;
        const auto in=BeamClip(beam,inside),out=BeamClip(beam,outside);
        Check(std::abs(in.x)<1&&std::abs(in.y)<1&&in.z>0&&in.z<1,"Receivers behind the glass along the light are in the beam");
        Check(out.z<0,"Receivers on the sun's side of the glass are culled by the beam frustum");
        // A wall only four units behind the upper pane used to fall before the
        // depth slab beginning at the window's bottom. It must remain a caster.
        const Vec3 upper{20,0,76},wall=upper-towardSun*4;
        Check(BeamClip(beam,wall).z>0,"A wall immediately behind the upper pane is not clipped out of its beam shadow");
        Check(BeamClip(beam,upper+towardSun*4).z<0,"A point in front of the upper pane cannot receive the beam");
        Check(Near(BeamClip(beam,upper-towardSun*300).z,BeamClip(beam,centre-towardSun*300).z),
              "Beam depth measures travel from each point of the window plane");
        Check(!GlassBeamProjection(points,{1,0,0}).valid,"Sun parallel to the pane produces no transmission");
        Check(!GlassBeamProjection({{0,0,0},{1,0,0},{2,0,0}},towardSun).valid,"Degenerate glass produces no beam");

        const auto side=BeamClip(beam,inside+Vec3{400,0,0});
        Check(std::abs(side.x)>1,"Receivers beside the window's shadow footprint are outside the beam");
        const auto record=GlassBeamRecord(beam,towardSun,{2,2,2},GlassCookieSlice(1,4));
        Check(IsGlassBeamRecord(record)&&record[35]==0&&Near(record[43],.375f)&&record[58]==1&&record[59]==1,
              "Beam records are directional, select their cookie slice and use a unit cookie rect");
        Check(record[54]==0,"Beams carry no shadow map (the glass would shadow its own beam)");
        Check(!IsGlassBeamRecord(SunRecord(towardSun,{1,1,1},beam.matrix,false)),"The plain sun record is not a beam");
    }
    {
        using namespace Filmmaker::Relight;
        // A static face samples only inside its own tile: the exact face edge (clip +-1) maps
        // StaticFaceGuardTexels inside the tile, so a filter of that radius never reads a neighbour.
        float rect[4];TileRect(StaticTile+5,rect);unsigned x=0,y=0,size=0;TileOrigin(StaticTile+5,x,y,size);
        for(float c:{-1.f,1.f}){
            const float u=(c*rect[2]+rect[0])*AtlasWidth,v=(c*rect[3]+rect[1])*AtlasHeight;
            Check(u>=x+StaticFaceGuardTexels-.01f&&u<=x+size-StaticFaceGuardTexels+.01f&&
                  v>=y+StaticFaceGuardTexels-.01f&&v<=y+size-StaticFaceGuardTexels+.01f,
                  "Static face edges sample inside the tile by the guard band");}
        // Lamp source size: the opening radius is the bulb's angular radius in texels, capped.
        Check(Near(ShadowLinearDepth(0,4,400),4,1e-3f)&&Near(ShadowLinearDepth(1,4,400),400,1e-2f),"stored depth maps back to distance");
        const float cord=LampOpeningTexels(5,StaticSize,StaticFaceGuard),leg=LampOpeningTexels(50,StaticSize,StaticFaceGuard);
        // A 0.3-unit cord 5 units from the lamp spans ~3.6 texels: thinner than the opening (2r).
        const float cordTexels=.3f/5/(2*StaticFaceGuard/StaticSize),legTexels=1.5f/50/(2*StaticFaceGuard/StaticSize);
        Check(cordTexels<2*cord&&legTexels>2*leg,"a cord near the lamp opens away; a chair leg across the room keeps its shadow");
        Check(LampOpeningTexels(.01f,StaticSize,StaticFaceGuard)==LampOpeningMaxTexels,"the radius is capped");
        Check(LampOpeningRadius(16)==3&&LampOpeningRadius(.1f)==LampSourceRadius&&LampOpeningRadius(2)==2,"Lamps open by their own source size, between a bulb and a globe");
        float m[16]{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};GuardStaticFace(m);GuardStaticFace(m,1/StaticFaceGuard);
        Check(Near(m[0],1)&&Near(m[7],8)&&m[8]==9,"The guard is undone exactly for the face record");
        // Every cube-face tier carries the guard, sized from its own tile, including the budgeted faces.
        for(unsigned tile=0;tile<SunMiddleTile;++tile){
            if(tile==SunTile||tile>=StaticTile+StaticShadowLimit*ShadowFaces)continue; // past the lamp faces: spare and beam tiles
            float r[4];TileRect(tile,r);unsigned tx=0,ty=0,ts=0;TileOrigin(tile,tx,ty,ts);
            Check(IsCubeFaceTile(tile)&&Near(TileGuard(tile),FaceGuardFor(ts),1e-6f),"Every cube face tile has a size-derived guard");
            for(float c:{-1.f,1.f}){
                const float u=(c*r[2]+r[0])*AtlasWidth,v=(c*r[3]+r[1])*AtlasHeight;
                Check(u>=tx+FaceGuardTexels-.01f&&u<=tx+ts-FaceGuardTexels+.01f&&
                      v>=ty+FaceGuardTexels-.01f&&v<=ty+ts-FaceGuardTexels+.01f,"Cube face edges sample inside the tile by the guard band");}
        }
        for(unsigned tile=SunMiddleTile;tile<ShadowTiles;++tile)Check(!IsCubeFaceTile(tile)&&TileGuard(tile)==1.f,"Sun cascades are not guarded");
        Check(TileGuard(0)==LocalFaceGuard&&LocalFaceGuard<=StaticFaceGuard,"Nearby area sources have enough guard reach without exceeding the static tier's angular margin");
        TileRect(SunTile,rect);float plain[4];AtlasRectPixels(0,0,SunCascadeSize,plain);
        Check(rect[2]==plain[2],"Sun tiles carry no guard");
    }
    {
        using namespace Filmmaker::Relight;
        Light seamLamp;seamLamp.kind="light_omni2";seamLamp.range=300;
        for(unsigned tile:{0u,StaticTile}){
            const auto exact=LocalShadowMatrix({0,0,0},{1,0,0},90,300);
            const auto blended=ToShadowedRecord(seamLamp,1,exact.data(),tile);
            float originalRect[4];TileRect(tile,originalRect);unsigned tx,ty,ts;TileOrigin(tile,tx,ty,ts);
            for(float ratio:{-.999f,0.f,.999f,ShadowFaceBlendWidth}){
                const float originalClip=ratio,blendClip=ratio*ShadowFaceBlendStart;
                Check(Near(originalClip*originalRect[2]+originalRect[0],blendClip*blended[54]+blended[52],1e-6f),"Cube overlap preserves the stored shadow projection");
                const float texel=(blendClip*blended[54]+blended[52])*AtlasWidth;
                Check(texel-blended[86]>=tx-.01f&&texel+blended[86]<=tx+ts+.01f,"Overlapping face and its full filter stay inside their own atlas tile");
            }
        }
        const auto edge=ShadowFaceWeights({1,1,0}),corner=ShadowFaceWeights({1,1,1});
        Check(Near(edge.x,.5f)&&Near(edge.y,.5f)&&Near(edge.z,0),"Cube edges blend both shadow faces with conserved energy");
        Check(Near(corner.x,1.f/3)&&Near(corner.y,1.f/3)&&Near(corner.z,1.f/3),"Cube corners normalize all three shadow faces");
        const auto before=ShadowFaceWeights({1,.9999f,.2f}),after=ShadowFaceWeights({.9999f,1,.2f});
        Check(Near(before.x,after.x,.001f)&&Near(before.y,after.y,.001f),"Shadow contribution stays continuous across dominant-face switches");
        Check(LampFilterTexels(0,1)==0&&LampFilterTexels(2,0)==0,"Point sources and zero softness have sharp filters");
        Check(LampFilterTexels(2,1)>LampFilterTexels(1,1),"A wider lamp source softens its shadow");
        Check(LampFilterTexels(1,2)>LampFilterTexels(1,1),"Softness scales the lamp filter");
        Check(LampFilterTexels(5,4)<=FaceGuardTexels,"Lamp filtering never reads another atlas tile");
        float rows[16];ShadowFilterRows(LampFilterTexels(2,1),rows);
        Check(Near(4*rows[3]+4*rows[4]+rows[5],1),"Softness preserves light and shadow energy");
        auto beam=GlassBeamRecord(GlassBeam{}, {0,0,1}, {1,1,1}, .5f);ShadowGlassBeam(beam,0);
        Check(beam[54]>0&&beam[65]==1,"Glass transmission has a dedicated receiver blocker map");
    }
    {
        using namespace Filmmaker::Relight;
        std::vector<Light> lights(6);
        for(unsigned i=0;i<lights.size();++i){lights[i].lumens=100;lights[i].position={float(i+1)*10,0,0};}
        lights[5].transient=true;lights[5].lumens=100000;lights[5].position={300,0,0};
        const auto resident=StableShadowBudget(lights,{0,0,0},4,{0,1,2,3});
        Check(std::find(resident.begin(),resident.end(),5)!=resident.end(),"A short VFX flash receives shadows immediately despite fixture residency");
        const auto cached=StaticShadowSet(lights,resident,20);
        Check(std::find(cached.begin(),cached.end(),5)==cached.end(),"Transient emitters cannot invalidate or occupy the static atlas");
        lights[5].enabled=false;
        const auto expired=StableShadowBudget(lights,{0,0,0},4,resident);
        Check(expired.size()==4&&std::find(expired.begin(),expired.end(),5)==expired.end(),"Expired effects return their moving-shadow slot to fixtures");
    }
    std::cout << "Relight math checks passed\n";
}
