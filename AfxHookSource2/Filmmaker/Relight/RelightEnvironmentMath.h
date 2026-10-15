#pragma once
#include "RelightMath.h"
#include "../Environment/EnvironmentClimateMath.h"
#include "../Environment/EnvironmentWorld.h"

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
    out.sunAngles={c[CL_sunElevation],c[CL_sunAzimuth],0};
    if(value("sunPositionOverride",0,0)!=0)
        out.sunAngles={value("sunPosition",1,out.sunAngles.x),value("sunPosition",0,out.sunAngles.y),0};
    out.sunColor={out.sunColor.x*c[CL_sunColorR],out.sunColor.y*c[CL_sunColorG],out.sunColor.z*c[CL_sunColorB]};
    // Overcast maps (Office, Train) author a zero-brightness sun. The Environment owns the sun's
    // position and power, so a zero would make the Sun brightness slider and sun position do
    // nothing: start from a typical authored daylight value instead.
    if(out.sunBrightness<=0)out.sunBrightness=UnlitMapSunBrightness;
    out.sun*=c[CL_sunPower];
    out.indirect=c[CL_ambient];
    out.sunAngle=c[CL_sunAngle];
    out.lampAngle=c[CL_lampAngle];
    out.shadowSoftness=c[CL_shadowSoftness];
    // The probe field also supplies occluded skylight. Zero bounce removes reflected
    // transport at composition, while retaining that visibility solution.
    out.bounce=c[CL_bounce];
    out.bounceSteps=unsigned(std::clamp(c[CL_bounceSteps],1.f,3.f));
    out.ao*=c[CL_ao];
    out.skyOcclusion*=c[CL_skyOcclusion];
    // Identical horizon/zenith radiance to the procedural backdrop. Integrate the
    // emitted sky over the upper hemisphere instead of inventing a ground bounce.
    if(c[CL_sky]>0) {
        auto smooth=[](float t){t=std::clamp(t,0.f,1.f);return t*t*(3-2*t);};
        const float day=smooth((c[CL_sunElevation]+14)/19);
        const float elevation=std::clamp(c[CL_sunElevation]/25,0.f,1.f);
        auto mix=[](Vec3 a,Vec3 b,float t){return a*(1-t)+b*t;};
        const Vec3 horizon=mix({.018f,.022f,.05f},mix({.9f,.45f,.2f},{.56f,.7f,.85f},elevation),day);
        const Vec3 zenith=mix({.002f,.004f,.018f},{.075f,.26f,.57f},day);
        Vec3 integrated{};
        for(unsigned i=0;i<32;++i){
            const float z=std::sqrt((i+.5f)/32);
            integrated=integrated+mix(horizon,zenith,std::pow(z,.4f)/(1+c[CL_haze]*.25f));
        }
        out.skyColor=integrated*(1.f/32);out.skyIntensity=1;
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
    out.generation=hash;
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
