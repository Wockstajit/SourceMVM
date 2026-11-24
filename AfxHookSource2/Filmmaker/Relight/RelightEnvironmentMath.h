#pragma once
#include "RelightMath.h"
#include "../Environment/EnvironmentClimateMath.h"
#include "../Environment/EnvironmentSkyMath.h"
#include "../Environment/EnvironmentWorld.h"
#include "../Environment/EnvironmentScene.h"

namespace Filmmaker::Relight {
// Mirage authors 3, Vertigo 3.25, Overpass 2.8.
inline constexpr float UnlitMapSunBrightness = 3;
// A scene can request replacement lighting before its background map load finishes.
// Never disable native lighting or publish an unshadowed default sun in that interval.
inline bool MapLightingReady(const Scene& authored,const std::string& map,bool geometryReady) {
    return geometryReady && !map.empty() && authored.map==map;
}
// Global world controls also apply when Relight runs without an Environment scene.
inline void ApplyGlobalLighting(Scene& out,const Environment::WorldLook& world) {
    auto value=[&](const char* key,unsigned component,float fallback){
        const auto i=world.fields.find(key);
        return i!=world.fields.end()&&i->second.size()>component?float(i->second[component]):fallback;
    };
    const Vec3 tint{value("lightColor",0,255)/255,value("lightColor",1,255)/255,value("lightColor",2,255)/255};
    const float amount=(std::max)(0.f,value("lightAmount",0,1));
    out.intensity*=amount;
    for(auto& light:out.lights)light.color={light.color.x*tint.x,light.color.y*tint.y,light.color.z*tint.z};
    out.sunColor={out.sunColor.x*tint.x,out.sunColor.y*tint.y,out.sunColor.z*tint.z};
    out.sun*=amount*(std::max)(0.f,value("sunBrightness",0,1));
    if(value("skyTint",0,0)!=0)out.skyColor={out.skyColor.x*value("skyColor",0,255)/255,
        out.skyColor.y*value("skyColor",1,255)/255,out.skyColor.z*value("skyColor",2,255)/255};
    out.skyColor={out.skyColor.x*tint.x,out.skyColor.y*tint.y,out.skyColor.z*tint.z};
    uint64_t hash=out.generation;
    auto add=[&](const void* p,size_t bytes){auto b=static_cast<const unsigned char*>(p);for(size_t i=0;i<bytes;++i)hash=(hash^b[i])*1099511628211ull;};
    // Include the control itself: lamp-only scenes can have a black sun and sky.
    add(&tint,sizeof(tint));add(&out.intensity,sizeof(out.intensity));
    add(&out.sun,sizeof(out.sun));add(&out.skyColor,sizeof(out.skyColor));
    out.generation=hash;
}
// Environment edits change incident radiance, never the material's base colour.
inline void ApplyEnvironmentLighting(Scene& out,const Environment::Climate& authored,
                                     const Environment::WorldLook& world) {
    using namespace Environment;
    const auto c=EffectiveClimate(authored);
    auto value=[&](const char* key,unsigned component,double fallback){
        const auto i=world.fields.find(key);
        return i!=world.fields.end()&&i->second.size()>component?float(i->second[component]):float(fallback);
    };
    out.enabled=true;
    out.intensity*=c[CL_mapLightPower];
    out.shadows=out.shadows&&c[CL_contactShadows]>.5f;
    out.hasSun=true;
    // The key light: the sun, or the moon once the sun has set (ClimateMoonKey).
    const bool moonKey=ClimateMoonKey(c);
    out.sunAngles=moonKey?Vec3{c[CL_moonElevation],c[CL_moonAzimuth],0}:Vec3{c[CL_sunElevation],c[CL_sunAzimuth],0};
    if(value("sunPositionOverride",0,0)!=0)
        out.sunAngles={value("sunPosition",1,out.sunAngles.x),value("sunPosition",0,out.sunAngles.y),0};
    if(moonKey)out.sunColor={out.sunColor.x*c[CL_moonColorR],out.sunColor.y*c[CL_moonColorG],out.sunColor.z*c[CL_moonColorB]};
    else out.sunColor={out.sunColor.x*c[CL_sunColorR],out.sunColor.y*c[CL_sunColorG],out.sunColor.z*c[CL_sunColorB]};
    // Overcast maps (Office, Train) author a zero-brightness sun. The Environment owns the sun's
    // position and power, so a zero would make the Sun brightness slider and sun position do
    // nothing: start from a typical authored daylight value instead.
    if(out.sunBrightness<=0)out.sunBrightness=UnlitMapSunBrightness;
    // The moon's power already carries its phase and its fade in through twilight; SunRadiance fades
    // it at the moon's own horizon from sunAngles.
    out.sun*=moonKey?(std::max)(c[CL_moon],0.f)*MoonIllumination(c[CL_moonPhase])*kMoonLightScale*SkySmooth(-c[CL_sunElevation]/6.f)
                    :c[CL_sunPower];
    out.indirect=c[CL_ambient];
    out.sunAngle=c[CL_sunAngle];
    out.lampAngle=c[CL_lampAngle];
    out.shadowSoftness=c[CL_shadowSoftness];
    // Fill lights (Light::fill) off: no direct light and no bounce from them.
    const bool fillsOff=c[CL_fillLights]<.5f;
    if(fillsOff)for(auto& light:out.lights)if(light.fill)light.enabled=false;
    // The probe field also supplies occluded skylight. Zero bounce removes reflected
    // transport at composition, while retaining that visibility solution.
    out.bounce=c[CL_bounce];
    out.bounceSteps=unsigned(std::clamp(c[CL_bounceSteps],1.f,3.f));
    out.ao*=c[CL_ao];
    out.skyOcclusion*=c[CL_skyOcclusion];
    // The same atmosphere as the procedural backdrop (EnvironmentSkyMath.h), integrated over the
    // upper hemisphere instead of inventing a ground bounce. Moonlit at night.
    if(c[CL_sky]>0) {
        out.skyColor=ClimateSkyIrradiance(c);out.skyIntensity=1;
    }
    ApplyGlobalLighting(out,world);
    // Stable for stationary inputs, including animated dials. GI must never publish
    // illumination from a previous time of day after a live edit.
    uint64_t hash=out.generation;
    auto add=[&](const void* p,size_t bytes){auto b=static_cast<const unsigned char*>(p);for(size_t i=0;i<bytes;++i)hash=(hash^b[i])*1099511628211ull;};
    add(&out.sunAngles,sizeof(out.sunAngles));add(&out.sunColor,sizeof(out.sunColor));add(&out.sun,sizeof(out.sun));
    add(&out.skyColor,sizeof(out.skyColor));add(&out.indirect,sizeof(out.indirect));add(&out.intensity,sizeof(out.intensity));
    add(&out.shadows,sizeof(out.shadows));add(&out.sunAngle,sizeof(out.sunAngle));
    add(&out.lampAngle,sizeof(out.lampAngle));add(&out.shadowSoftness,sizeof(out.shadowSoftness));
    add(&fillsOff,sizeof(fillsOff));
    out.generation=hash;
}
// Scene Objects must enter the same material/occlusion path as map lights. The
// display composite deliberately skips illumination after native relighting.
// Called on a fresh captured scene, after map-light choices and before VFX lights.
inline unsigned ApplySceneObjectLights(Scene& out,const std::vector<Environment::Object>& objects,double time) {
    if(!out.enabled || !std::isfinite(time))return 0;
    unsigned added=0,emitters=0;
    for(const auto& object:objects) {
        if(!object.enabled||!object.IsLight())continue;
        if(emitters++>=16)break;
        const unsigned samples=object.kind=="area"?4u:1u;
        Vec3 forward,right,up;LightBasis(object.angles,forward,right,up);
        for(unsigned sample=0;sample<samples;++sample) {
            Light light;
            light.identity="scene:"+std::to_string(object.id)+":"+std::to_string(sample);
            light.kind=object.kind=="point"?"light_omni":"light_spot";
            light.sceneObject=true;light.runtimeShadows=object.shadows;
            light.position=samples==1?object.position:Environment::AreaLightSample(
                object.position,right,up,object.studio.width,object.studio.height,int(sample));
            light.angles=object.angles;light.color=object.color;light.range=object.range;
            light.sourceRadius=(std::max)(16.f,object.studio.sourceRadius);
            light.outerAngle=object.cone*.5f;
            light.innerAngle=std::acos((std::min)(.9999f,std::cos(Environment::Radians(light.outerAngle))+.12f))/Environment::Radians(1);
            // Match the old intensity's diffuse energy halfway through its range;
            // native materials then supply their actual normals and BRDF.
            light.lumens=object.intensity*object.range*object.range/(80.f*samples);
            light.brightnessScale=Environment::StudioCue(object.studio,time);
            out.lights.push_back(std::move(light));++added;
        }
    }
    return added;
}
// How much of the map's authored daylight the relit world still receives: the luminance of
// sun plus sky irradiance after Environment edits, over the same before them. Draws whose
// albedo has daylight painted in (the 3D skybox) are scaled by it so they dim exactly as far
// as the relit world does. A sun-height curve alone left Vertigo's skyline about 12x brighter
// than the foreground at night, because a night look also turns the ambient right down.
inline float DaylightEnergyRatio(const Scene& authored,const Scene& lit) {
    auto incident=[](const Scene& s){
        const auto sun=SunRadiance(s);
        const float height=(std::max)(0.f,SunDirection(s.sunAngles).z);
        const auto sky=SkyAmbient(s,s.indirect);
        auto luminance=[](Vec3 v){return .2126f*v.x+.7152f*v.y+.0722f*v.z;};
        return luminance(sun)*height+luminance(sky);
    };
    const float reference=incident(authored);
    if(!(reference>1e-6f))return 1.f;
    return std::clamp(incident(lit)/reference,0.f,1.f);
}
}
