// Native CS2 Molotov and detonation lights, independent of the environment renderer.
#define NOMINMAX
#include "ParticleFxInternal.h"
#include "../Environment/EnvironmentVfxLighting.h"
#include "../Environment/EnvironmentVfxLightingMath.h"
#include "../Debug/MvmDebugLog.h"
#include "../../MirvTime.h"
#include <Windows.h>
#include <algorithm>
#include <cstring>

struct ParticleData;
extern unsigned char* (__fastcall *g_Original_getParticleManager)();
extern int* (__fastcall *g_Original_createParticle)(unsigned char*,int*,const char*,int,int64_t,int64_t,int64_t,int64_t);
extern void (__fastcall *g_Original_updateParticle)(unsigned char*,int,int,ParticleData*,int);
extern void (__fastcall *g_Original_destroyParticle)(unsigned char*,int,bool,bool);

namespace Filmmaker::fx {
namespace {
constexpr const char* kMollyLight="particles/filmmaker/molly_light.vpcf";
constexpr const char* kMollyArea="particles/filmmaker/molly_area.vpcf";
constexpr const char* kFlashLight="particles/filmmaker/flash_light.vpcf";
struct NativeLight {
    Environment::MollyLightSource source;
    Environment::VfxLightPulse pulse;
    int particle=-1;bool seen=false,burst=false,flash=false;double lastSeen=0;float brightness=0;
};
struct Detonation {
    Environment::Vec3 position;
    double time;int tick,entity;unsigned epoch;ULONGLONG wall;bool flash;
};
std::mutex detonationMx;
std::vector<Detonation> pendingDetonations,recentDetonations;
std::vector<NativeLight> lights;
std::atomic<bool> ready{false};
unsigned epoch=0;int lastTick=-1;
ULONGLONG retryAt=0,lastLog=0;
bool Available(){return g_Original_getParticleManager&&g_Original_createParticle&&g_Original_updateParticle&&g_Original_destroyParticle;}
bool Create(NativeLight& light) {
    __try {
        auto manager=g_Original_getParticleManager();if(!manager)return false;
        const char* asset=light.flash?kFlashLight:(!strcmp(light.source.kind,"vfx:fire")?kMollyArea:kMollyLight);
        g_Original_createParticle(manager,&light.particle,asset,8,0,0,0,0);
        return light.particle>=0;
    } __except(EXCEPTION_EXECUTE_HANDLER){light.particle=-1;return false;}
}
bool Update(NativeLight& light,float energy) {
    // CP0 owns world position; CP1 carries lumens, local range, and emitter radius.
    float position[3]{light.source.position.x,light.source.position.y,light.source.position.z};
    float brightness=energy;
    if(!strcmp(light.source.kind,"vfx:fire")){
        unsigned patches=0;
        for(const auto& other:lights)if(other.source.handle==light.source.handle&&!strcmp(other.source.kind,"vfx:fire"))++patches;
        brightness*=light.source.lumens/std::max(1u,patches);
    }else brightness*=light.source.lumens;
    light.brightness=brightness;
    float settings[3]{brightness,light.source.range,light.source.radius};
    float normal[3]{0,0,(light.source.slot&1)?1.f:-1.f};
    __try {
        auto manager=g_Original_getParticleManager();if(!manager)return false;
        g_Original_updateParticle(manager,light.particle,0,reinterpret_cast<ParticleData*>(position),0);
        g_Original_updateParticle(manager,light.particle,1,reinterpret_cast<ParticleData*>(settings),0);
        if(!strcmp(light.source.kind,"vfx:fire"))g_Original_updateParticle(manager,light.particle,2,reinterpret_cast<ParticleData*>(normal),0);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void Destroy(NativeLight& light) {
    if(light.particle<0)return;
    // Native light renderers can retain their last control-point energy when a
    // system is stopped. Extinguish before releasing it, including toggle/seek cleanup.
    Update(light,0);
    MvmDebugLog_Linef("fx.molly.destroy","entity=%d particle=%d kind=%s brightness=%.3f",
        light.source.entity,light.particle,light.source.kind,light.brightness);
    __try {if(auto manager=g_Original_getParticleManager())g_Original_destroyParticle(manager,light.particle,true,false);}
    __except(EXCEPTION_EXECUTE_HANDLER){}
    light.particle=-1;
}
void Clear(){if(Available())for(auto& light:lights)Destroy(light);lights.clear();lastTick=-1;}
}
bool MollyFx_Ready(){return ready.load();}
void NativeLightFx_OnDetonate(bool flash,int entity,float x,float y,float z){
    if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z))return;
    {std::lock_guard<std::mutex> lock(g_mx);if(!g_enabled||!(flash?g_flashLights:g_heLights).load())return;}
    double time=0;if(!g_MirvTime.GetCurrentDemoTime(time))time=g_MirvTime.curtime_get();
    if(!std::isfinite(time))return;
    const Detonation event{{x,y,z},time,g_demoTickNow.load(),entity,g_levelEpoch.load(),GetTickCount64(),flash};
    std::lock_guard<std::mutex> lock(detonationMx);
    for(const auto& previous:recentDetonations){
        const auto delta=previous.position-event.position;
        const bool echo=event.tick>previous.tick&&event.tick-previous.tick<=64;
        if(previous.epoch==event.epoch&&previous.flash==flash&&previous.entity==entity&&Environment::Dot(delta,delta)<1&&
            (event.wall-previous.wall<=250||echo))return;
    }
    if(recentDetonations.size()>=32)recentDetonations.erase(recentDetonations.begin());
    recentDetonations.push_back(event);
    if(pendingDetonations.size()>=32)pendingDetonations.erase(pendingDetonations.begin());
    pendingDetonations.push_back(event);
}
void MollyFx_OnLevelInit(){
    // The engine destroys the old manager's systems. Never destroy old indices in a new level.
    Environment::ResetVfxLightingSources();
    lights.clear();ready.store(false);epoch=0;lastTick=-1;retryAt=0;
    std::lock_guard<std::mutex> lock(detonationMx);pendingDetonations.clear();recentDetonations.clear();
}
void MollyFx_PumpMainThread(){
    const unsigned currentEpoch=g_levelEpoch.load();
    if(epoch!=currentEpoch){lights.clear();ready.store(false);lastTick=-1;retryAt=0;epoch=currentEpoch;}
    bool enabled=false,molly=false,he=false,flash=false;
    {std::lock_guard<std::mutex> lock(g_mx);molly=g_enabled&&g_mollyLights.load();he=g_enabled&&g_heLights.load();flash=g_enabled&&g_flashLights.load();enabled=molly||he||flash;}
    if(!enabled||!FxMainThreadContextActive()){
        if(!lights.empty()){Clear();MvmDebugLog_Linef("fx.molly.cleanup","native lights removed (disabled or demo ended)");}
        std::lock_guard<std::mutex> lock(detonationMx);pendingDetonations.clear();recentDetonations.clear();
        return;
    }
    if(!Available()){ready.store(false);return;}
    const auto wall=GetTickCount64();
    if(!ready.load()){
        if(wall<retryAt)return;retryAt=wall+2000;
        const bool bottleLoaded=ParticleFx_PrecacheSystem(kMollyLight);
        const bool areaLoaded=ParticleFx_PrecacheSystem(kMollyArea);
        const bool flashLoaded=ParticleFx_PrecacheSystem(kFlashLight);
        const bool loaded=bottleLoaded&&areaLoaded&&flashLoaded;ready.store(loaded);
        MvmDebugLog_Linef("fx.molly.precache","ready=%d managerFn=%p createFn=%p updateFn=%p destroyFn=%p epoch=%u",loaded,
            g_Original_getParticleManager,g_Original_createParticle,g_Original_updateParticle,g_Original_destroyParticle,currentEpoch);
        if(!loaded)return;
    }
    const int tick=g_demoTickNow.load();
    if(lastTick>=0&&tick>=0&&(tick<lastTick||tick-lastTick>128)){
        Clear();std::lock_guard<std::mutex> lock(detonationMx);recentDetonations.clear();
    }
    lastTick=tick;
    double time=0;if(!g_MirvTime.GetCurrentDemoTime(time))time=g_MirvTime.curtime_get();
    if(!std::isfinite(time))return;
    for(auto& light:lights)light.seen=false;
    for(auto& light:lights)if((light.burst?!(light.flash?flash:he):!molly))Destroy(light);
    lights.erase(std::remove_if(lights.begin(),lights.end(),[](const auto& light){return light.particle<0;}),lights.end());
    std::vector<Detonation> pending;
    {std::lock_guard<std::mutex> lock(detonationMx);pending.swap(pendingDetonations);}
    for(const auto& event:pending){
        const double duration=event.flash?.12:.3;
        const bool stale=event.epoch!=currentEpoch||wall-event.wall>500||
            (tick>=0&&event.tick>=0&&std::abs(tick-event.tick)>8)||time<event.time||time>=event.time+duration;
        if(stale||!(event.flash?flash:he)||lights.size()>=48){
            MvmDebugLog_Linef("fx.blast.skip","kind=%s entity=%d eventTick=%d tick=%d age=%.3f stale=%d",event.flash?"flash":"he",event.entity,event.tick,tick,time-event.time,stale);
            continue;
        }
        NativeLight light;light.burst=true;light.flash=event.flash;
        light.source={unsigned(event.entity),0,event.entity,event.flash?"vfx:flash":"vfx:he",event.position,event.flash?6000.f:9000.f,event.flash?500.f:650.f,48};
        light.pulse.start=event.time;light.pulse.duration=duration;
        if(!Create(light))continue;
        Update(light,Environment::VfxMuzzleEnergy(time,event.time,duration));
        MvmDebugLog_Linef("fx.blast.create","kind=%s entity=%d particle=%d eventTick=%d tick=%d position=%.2f,%.2f,%.2f lumens=%.1f duration=%.2f",
            event.flash?"flash":"he",event.entity,light.particle,event.tick,tick,event.position.x,event.position.y,event.position.z,light.brightness,duration);
        lights.push_back(light);
    }
    const auto gathered=molly?Environment::CaptureMollyLightSources():std::vector<Environment::MollyLightSource>{};
    std::vector<Environment::MollyLightSource> sources;
    for(const auto& source:gathered){
        if(strcmp(source.kind,"vfx:fire")){sources.push_back(source);continue;}
        // Keep the distributed real burning patches. Broad native falloff blends
        // them without moving their origins up the wall or beyond the fire.
        auto patch=source;patch.lumens*=.35f;patch.range=260;
        // Opposing broad area faces illuminate the floor and wall from the flame bed.
        // Divide total energy across both faces rather than doubling fire brightness.
        patch.slot=source.slot*2;sources.push_back(patch);
        ++patch.slot;sources.push_back(patch);
    }
    for(const auto& source:sources){
        auto found=std::find_if(lights.begin(),lights.end(),[&](const auto& light){return light.source.handle==source.handle&&light.source.slot==source.slot&&!strcmp(light.source.kind,source.kind);});
        if(found==lights.end()){
            if(lights.size()>=32)continue;
            NativeLight light;light.source=source;light.pulse.start=time-(DemoIsPaused()?Environment::VfxIgnitionFade:0);light.pulse.duration=2;light.pulse.sustained=true;
            light.pulse.light=Environment::VfxEmitter(source.kind,source.position,{1,.52f,.16f},source.lumens,source.range,source.radius);
            if(!Create(light))continue;
            MvmDebugLog_Linef("fx.molly.create","entity=%d handle=%u kind=%s slot=%u particle=%d position=%.2f,%.2f,%.2f range=%.1f",
                source.entity,source.handle,source.kind,source.slot,light.particle,source.position.x,source.position.y,source.position.z,source.range);
            lights.push_back(light);found=lights.end()-1;
        }
        found->seen=true;found->source=source;found->lastSeen=time;
        // A briefly unavailable attachment must not restart ignition from black.
        found->pulse.end=-1;
        found->pulse.duration=time-found->pulse.start+2;
    }
    const bool log=MvmDebugLog_Active()&&wall-lastLog>=500;if(log)lastLog=wall;
    for(auto& light:lights){
        if(light.burst){
            const float energy=Environment::VfxMuzzleEnergy(time,light.pulse.start,light.pulse.duration);
            if(time>=light.pulse.start+light.pulse.duration||time<light.pulse.start){
                MvmDebugLog_Linef("fx.blast.end","kind=%s particle=%d age=%.3f",light.flash?"flash":"he",light.particle,time-light.pulse.start);
                Destroy(light);
            }else if(!Update(light,energy))Destroy(light);
            continue;
        }
        // Bridge brief pose / fire-anchor updates, then fade out normally.
        if(!light.seen&&time-light.lastSeen>.06)Environment::EndVfxPulse(light.pulse,time);
        const float envelope=light.seen&&DemoIsPaused()?1.f:Environment::VfxLightEnergy(light.pulse,time);
        const float energy=envelope*Environment::VfxNativePatchFlicker(time,light.source.handle,light.source.slot);
        const bool expired=Environment::VfxLightExpired(light.pulse,time);
        const bool updated=Update(light,expired?0.f:energy);
        if(expired||!updated)Destroy(light);
        if(log)MvmDebugLog_LinefAlways("fx.molly.light","entity=%d targetParticle=%d kind=%s cp0=%.2f,%.2f,%.2f cp1=%.3f,%.1f,%.1f seen=%d",
            light.source.entity,light.particle,light.source.kind,light.source.position.x,light.source.position.y,light.source.position.z,light.brightness,light.source.range,light.source.radius,light.seen);
    }
    lights.erase(std::remove_if(lights.begin(),lights.end(),[](const auto& light){return light.particle<0;}),lights.end());
}
}
