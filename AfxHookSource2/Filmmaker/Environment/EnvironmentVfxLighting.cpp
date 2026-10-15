#define NOMINMAX
#include "EnvironmentVfxLighting.h"
#include "EnvironmentVfxLightingMath.h"
#include "../Platform/JsonParser.h"
#include "../Cosmetics/CosmeticEntityAccess.h"
#include "../Cosmetics/CosmeticModelSwap.h"
#include "../../SchemaSystem.h"
#include "../Debug/MvmDebugLog.h"
#include "../../ClientEntitySystem.h"
#include "../../MirvTime.h"
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include <Windows.h>
#include <mutex>
#include <cstring>

namespace Filmmaker::Environment {
namespace {
std::mutex mutex;
std::vector<VfxLightPulse> pulses;
std::string currentMap;
double lastTime = -1;
struct InfernoSource {int index=-1;uint32_t handle=0;bool ending=false;};
std::vector<InfernoSource> infernos;
float Number(const JsonValue& data, const char* key, float fallback = 0) {
    const auto value = data.Find(key); return value ? float(value->AsNumber(fallback)) : fallback;
}
// Entity callbacks can arrive while an attachment is being rebuilt. Keep guarded
// engine calls separate from objects requiring C++ unwinding.
struct SourcePose { Vec3 eye{}, position{}; const char* source="none"; void* target=nullptr; };
bool ReadAttachment(CEntityInstance* entity,const char* name,Vec3& position) {
    __try { if(!entity)return false;SOURCESDK::Vector origin;SOURCESDK::Quaternion rotation;
        const auto attachment=entity->LookupAttachment(name);
        if(!attachment||!entity->GetAttachment(attachment,origin,rotation))return false;
        position={origin.x,origin.y,origin.z};return Finite(position);
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool PawnSource(CEntityInstance* pawn,const char* attachment,SourcePose& pose) {
    __try {
        if(!pawn||!pawn->IsPlayerPawn())return false;
        float eye[3]{},angles[3]{};pawn->GetRenderEyeOrigin(eye);pawn->GetRenderEyeAngles(angles);
        pose.eye={eye[0],eye[1],eye[2]};
        const auto handle=pawn->GetActiveWeaponHandle();
        auto weapon=handle.IsValid()?CosmeticEntFromIndex(handle.GetEntryIndex()):nullptr;
        auto viewer=CosmeticEntFromIndex(AfxGetSpectatedPawnIndex());if(!viewer)viewer=AfxGetLocalViewerPawn();
        void* viewmodel=nullptr;
        if(viewer==pawn)ResolveViewmodelWeaponEntityIndex(reinterpret_cast<unsigned char*>(pawn),nullptr,nullptr,0,&viewmodel);
        CEntityInstance* candidates[2]{static_cast<CEntityInstance*>(viewmodel),weapon};
        for(unsigned i=0;i<2;++i){Vec3 position{};
            if(!ReadAttachment(candidates[i],attachment,position)&&
               (strcmp(attachment,"muzzle")||!ReadAttachment(candidates[i],"muzzle_flash",position)))continue;
            const auto delta=position-pose.eye;
            // A hidden third-person weapon can retain an origin at the player's feet.
            if(!Finite(delta)||Dot(delta,delta)>160*160||delta.z < -40)continue;
            pose.position=position;pose.source=i==0?"viewmodel":"world";pose.target=candidates[i];return true;
        }
        // A missing bottle attachment is not evidence of a burning bottle.
        if(strcmp(attachment,"muzzle"))return false;
        const float pitch=Radians(angles[0]),yaw=Radians(angles[1]);
        pose.position=pose.eye+Vec3{std::cos(pitch)*std::cos(yaw),std::cos(pitch)*std::sin(yaw),-std::sin(pitch)}*24.f;
        pose.source="eye";pose.target=pawn;return Finite(pose.position);
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool MuzzlePosition(int userid,SourcePose& pose) {
    __try {
        auto controller=CosmeticEntFromIndex(userid+1);if(!controller||!controller->IsPlayerController())return false;
        const auto handle=controller->GetPlayerPawnHandle();
        return handle.IsValid()&&PawnSource(CosmeticEntFromIndex(handle.GetEntryIndex()),"muzzle",pose);
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
const char* VfxEntityClass(CEntityInstance* entity) {
    __try {const auto kind=entity->GetClassName();return kind&&*kind?kind:entity->GetClientClassName();}
    __except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
}
struct FireSnapshot { int count=0;Vec3 positions[64]{};bool burning[64]{};bool ending=false;float lifetime=0;void* target=nullptr; };
uint32_t EntityHandle(CEntityInstance* entity) {
    __try {return entity?entity->GetHandle().ToInt():0;}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
bool ReadFire(CEntityInstance* entity,FireSnapshot& fire) {
    __try {
        const auto& offsets=g_clientDllOffsets.EnvironmentVfx;
        if(!offsets.firePositions||!offsets.fireBurning||!offsets.fireCount)return false;
        auto base=reinterpret_cast<unsigned char*>(entity);fire.target=entity;
        fire.count=*reinterpret_cast<int*>(base+offsets.fireCount);if(fire.count<0||fire.count>64)return false;
        std::memcpy(fire.positions,base+offsets.firePositions,fire.count*sizeof(Vec3));
        std::memcpy(fire.burning,base+offsets.fireBurning,fire.count*sizeof(bool));
        if(offsets.firePostEffect)fire.ending=*reinterpret_cast<bool*>(base+offsets.firePostEffect);
        if(offsets.fireLifetime)fire.lifetime=*reinterpret_cast<float*>(base+offsets.fireLifetime);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool HeldFire(CEntityInstance* pawn,SourcePose& pose,int& weaponIndex) {
    __try {
        if(!pawn||!pawn->IsPlayerPawn())return false;const auto handle=pawn->GetActiveWeaponHandle();
        auto weapon=handle.IsValid()?CosmeticEntFromIndex(handle.GetEntryIndex()):nullptr;
        if(!weapon||!weapon->GetClassName()||strcmp(weapon->GetClassName(),"weapon_molotov"))return false;
        const auto offset=g_clientDllOffsets.EnvironmentVfx.grenadePinPulled;
        if(!offset||!*reinterpret_cast<bool*>(reinterpret_cast<unsigned char*>(weapon)+offset))return false;
        weaponIndex=handle.GetEntryIndex();return PawnSource(pawn,"molotov_particle",pose);
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool FlyingFire(CEntityInstance* entity,Vec3& position) {
    __try {
        const auto offset=g_clientDllOffsets.EnvironmentVfx.molotovIncendiary;
        if(!offset||*reinterpret_cast<bool*>(reinterpret_cast<unsigned char*>(entity)+offset))return false;
        if(ReadAttachment(entity,"molotov_particle",position))return true;
        entity->GetOrigin(position.x,position.y,position.z);return Finite(position);
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
struct TrackedFire { VfxLightPulse pulse;unsigned slot=0;bool seen=false; };
std::vector<TrackedFire> fires;
void TrackFire(int entity,unsigned slot,const char* kind,Vec3 position,float lumens,float range,float radius,double time) {
    for(auto& fire:fires)if(fire.pulse.entity==entity&&fire.slot==slot&&fire.pulse.light.identity==kind){
        fire.seen=true;if(fire.pulse.end>=0)return;
        fire.pulse.light.position=position;fire.pulse.light.lumens=lumens;
        fire.pulse.duration=time-fire.pulse.start+2;return;
    }
    if(fires.size()>=128)return;
    TrackedFire fire;fire.slot=slot;fire.seen=true;fire.pulse.entity=entity;fire.pulse.start=time;fire.pulse.duration=2;fire.pulse.sustained=true;
    fire.pulse.light=VfxEmitter(kind,position,{1,.52f,.16f},lumens,range,radius);fires.push_back(fire);
}
void GatherMollySources(std::vector<MollyLightSource>& sources,bool log) {
    // Client-only infernos have no script classname and report their network base
    // class. The creation event supplies their actual handle; validate its serial
    // every frame before reading the schema's burning arrays.
    for(auto& source:infernos){const int index=source.index;auto entity=CosmeticEntFromIndex(index);
        if(!entity||EntityHandle(entity)!=source.handle){source.ending=true;continue;}
        FireSnapshot fire;if(!ReadFire(entity,fire)){if(log){const auto& o=g_clientDllOffsets.EnvironmentVfx;
            MvmDebugLog_LinefAlways("environment.vfx.fire.read","entity=%d target=%p class=%s read=0 count=%d posOff=%td burnOff=%td countOff=%td",index,entity,"inferno",fire.count,o.firePositions,o.fireBurning,o.fireCount);}continue;}
        if(fire.ending)source.ending=true;
        std::vector<Vec3> points;if(!source.ending&&!fire.ending)for(int i=0;i<fire.count;++i)if(fire.burning[i]&&Finite(fire.positions[i]))points.push_back(fire.positions[i]);
        const auto anchors=VfxFireAnchors(points);
        for(unsigned i=0;i<anchors.size();++i)sources.push_back({EntityHandle(entity),i,index,"vfx:fire",anchors[i]+Vec3{0,0,24},
            (std::min)(2800.f,1000.f+100.f*points.size()/anchors.size()),340,48});
        if(log){const auto& offsets=g_clientDllOffsets.EnvironmentVfx;
            MvmDebugLog_LinefAlways("environment.vfx.fire","entity=%d target=%p positionsOffset=%td burningOffset=%td countOffset=%td count=%d burning=%zu anchors=%zu post=%d lifetime=%.2f",index,fire.target,offsets.firePositions,offsets.fireBurning,offsets.fireCount,fire.count,points.size(),anchors.size(),fire.ending,fire.lifetime);
            for(unsigned i=0;i<anchors.size();++i)MvmDebugLog_LinefAlways("environment.vfx.anchor","entity=%d slot=%u position=%.2f,%.2f,%.2f",index,i,anchors[i].x,anchors[i].y,anchors[i].z+24);
        }
    }
    infernos.erase(std::remove_if(infernos.begin(),infernos.end(),[&](const auto& source){
        return source.ending;
    }),infernos.end());
    const int highest=(std::min)(GetHighestEntityIndex(),32768);
    for(int index=0;index<=highest;++index){auto entity=CosmeticEntFromIndex(index);if(!entity)continue;
        const char* kind=VfxEntityClass(entity);if(!kind)continue;
        if(!strcmp(kind,"molotov_projectile")||!strcmp(kind,"C_MolotovProjectile")){Vec3 position{};
            if(FlyingFire(entity,position))sources.push_back({EntityHandle(entity),0,index,"vfx:bottle",position,900,220,16});
        } else if(entity->IsPlayerPawn()){
            SourcePose pose;int weapon=-1;if(HeldFire(entity,pose,weapon)){
                sources.push_back({EntityHandle(CosmeticEntFromIndex(weapon)),0,weapon,"vfx:held",pose.position,900,220,16});
                if(log)MvmDebugLog_LinefAlways("environment.vfx.held","entity=%d source=%s target=%p pinOffset=%td position=%.2f,%.2f,%.2f",weapon,pose.source,pose.target,g_clientDllOffsets.EnvironmentVfx.grenadePinPulled,pose.position.x,pose.position.y,pose.position.z);
            }
        }
    }
    if(sources.size()>128)sources.resize(128);
}
bool EffectPosition(int entity,bool bomb,Vec3& position) {
    __try {
        auto effect=entity>=0?CosmeticEntFromIndex(entity):nullptr;
        if(!effect&&bomb){
            const int highest=(std::min)(GetHighestEntityIndex(),32768);
            for(int i=0;i<=highest;++i){auto candidate=CosmeticEntFromIndex(i);
                if(candidate&&candidate->GetClassName()&&!strcmp(candidate->GetClassName(),"planted_c4")){effect=candidate;break;}}
        }
        if(!effect)return false;
        effect->GetOrigin(position.x,position.y,position.z);return Finite(position);
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
}
void ResetVfxLightingSources() {
    std::lock_guard<std::mutex> lock(mutex);
    pulses.clear();fires.clear();infernos.clear();currentMap.clear();lastTime=-1;
}
std::vector<MollyLightSource> CaptureMollyLightSources() {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<MollyLightSource> sources;GatherMollySources(sources,false);return sources;
}
bool IsVfxLightingEvent(const char* name) {
    return name && (!strcmp(name,"weapon_fire") || !strcmp(name,"hegrenade_detonate") ||
        !strcmp(name,"flashbang_detonate") || !strcmp(name,"inferno_startburn") ||
        !strcmp(name,"inferno_expire") || !strcmp(name,"inferno_extinguish") ||
        !strcmp(name,"bomb_exploded") || !strcmp(name,"round_start"));
}
void OnVfxLightingEvent(const char* name, const JsonValue& data) {
    double time = 0;
    if (!g_MirvTime.GetCurrentDemoTime(time)) time = g_MirvTime.curtime_get();
    if (!std::isfinite(time)) return;
    const int entity = int(Number(data,"entityid",-1));
    std::lock_guard<std::mutex> lock(mutex);
    if (!strcmp(name,"round_start")) { pulses.clear(); fires.clear(); infernos.clear(); lastTime=-1; return; }
    if (!strcmp(name,"inferno_expire") || !strcmp(name,"inferno_extinguish")) {
        for(auto& source:infernos)if(entity<0||source.index==entity)source.ending=true;
        for(auto& fire:fires)if(fire.pulse.light.identity=="vfx:fire"&&(entity<0||fire.pulse.entity==entity))EndVfxPulse(fire.pulse,time);
        if(MvmDebugLog_Active())MvmDebugLog_LinefAlways("environment.vfx","event=%s time=%.3f entity=%d fade=%.2f",name,time,entity,VfxExtinguishFade);
        return;
    }
    if(!strcmp(name,"inferno_startburn")){
        auto source=entity>=0?CosmeticEntFromIndex(entity):nullptr;FireSnapshot fire;const bool read=source&&ReadFire(source,fire);
        if(source&&read&&infernos.size()<32){
            infernos.erase(std::remove_if(infernos.begin(),infernos.end(),[&](const auto& old){return old.index==entity;}),infernos.end());
            infernos.push_back({entity,EntityHandle(source),false});
        }
        if(MvmDebugLog_Active()){const auto& o=g_clientDllOffsets.EnvironmentVfx;MvmDebugLog_LinefAlways("environment.vfx.fire.read","entity=%d target=%p class=%s read=%d count=%d posOff=%td burnOff=%td countOff=%td",entity,source,source?VfxEntityClass(source):"none",read,fire.count,o.firePositions,o.fireBurning,o.fireCount);}
        return; // The native burning patches own emission.
    }
    Vec3 position{Number(data,"x"),Number(data,"y"),Number(data,"z")};
    if(strcmp(name,"weapon_fire")&&(!data.Find("x")||!data.Find("y")||!data.Find("z"))&&
       !EffectPosition(entity,!strcmp(name,"bomb_exploded"),position))return;
    VfxLightPulse pulse; pulse.start=time; pulse.entity=entity;
    if (!strcmp(name,"weapon_fire")) {
        const auto weaponValue=data.Find("weapon"); std::string weapon=weaponValue?weaponValue->AsString():"";
        if(weapon.compare(0,7,"weapon_")==0)weapon.erase(0,7);
        if (weapon.find("knife")!=std::string::npos || weapon.find("grenade")!=std::string::npos ||
            weapon=="molotov" || weapon=="incgrenade" || weapon=="c4" || weapon=="taser") return;
        pulse.userid=int(Number(data,"userid",-1));SourcePose pose;
        if (!MuzzlePosition(pulse.userid,pose)) return;position=pose.position;
        if(MvmDebugLog_Active())MvmDebugLog_LinefAlways("environment.vfx.muzzle","source=%s target=%p eye=%.2f,%.2f,%.2f muzzle=%.2f,%.2f,%.2f",pose.source,pose.target,pose.eye.x,pose.eye.y,pose.eye.z,position.x,position.y,position.z);
        const bool suppressed=weapon.find("silencer")!=std::string::npos;
        pulse.light=VfxEmitter("vfx:muzzle",position,{1,.76f,.48f},suppressed?180.f:1200.f,suppressed?100.f:280.f,24);
        pulse.light.viewmodelEmitter=!strcmp(pose.source,"viewmodel");
        pulse.duration=VfxMuzzleDuration;

    } else {
        const bool flash=!strcmp(name,"flashbang_detonate");
        pulse.light=VfxEmitter("vfx:explosion",position,flash?Vec3{1,.94f,.8f}:Vec3{1,.5f,.12f},flash?6000.f:9000.f,flash?500.f:650.f,32);
        pulse.duration=flash?.12:.3;
    }
    if (!Finite(position)) return;
    // Bound concurrent effects without letting a long session accumulate emitters.
    if (pulses.size()>=32) pulses.erase(pulses.begin());
    pulses.push_back(pulse);
    if (MvmDebugLog_Active()) MvmDebugLog_LinefAlways("environment.vfx","event=%s time=%.3f entity=%d position=%.2f,%.2f,%.2f lumens=%.0f duration=%.2f",name,time,entity,position.x,position.y,position.z,pulse.light.lumens,pulse.duration);
}
void CaptureVfxLights(Relight::Scene& scene,double time,const std::string& map,bool active) {
    std::lock_guard<std::mutex> lock(mutex);
    const bool mapChanged=!currentMap.empty()&&currentMap!=map;
    if (mapChanged || (lastTime>=0 && time<lastTime-.001)) {pulses.clear();fires.clear();}
    // A backward seek replays inferno_startburn before this render capture. Do not
    // discard those fresh sources; GatherMollySources validates their entity serials.
    if(mapChanged)infernos.clear();
    currentMap=map; lastTime=time;
    pulses.erase(std::remove_if(pulses.begin(),pulses.end(),[&](const auto& pulse){return VfxLightExpired(pulse,time);}),pulses.end());
    infernos.erase(std::remove_if(infernos.begin(),infernos.end(),[](const auto& source){return source.ending;}),infernos.end());
    if (!active || !scene.enabled) {fires.clear();return;}
    for(auto& fire:fires)fire.seen=false;
    static ULONGLONG lastLog=0;const auto clock=GetTickCount64();const bool log=MvmDebugLog_Active()&&clock-lastLog>=500;
    if(log)lastLog=clock;
    std::vector<MollyLightSource> sources;GatherMollySources(sources,log);
    for(const auto& source:sources)TrackFire(source.entity,source.slot,source.kind,source.position,source.lumens,source.range,source.radius,time);
    for(auto& fire:fires){if(!fire.seen)EndVfxPulse(fire.pulse,time);
        auto light=fire.pulse.light;light.transientGroup=fire.pulse.entity;light.brightnessScale*=VfxLightEnergy(fire.pulse,time)*VfxFireFlicker(time,unsigned(fire.pulse.entity)*4+fire.slot);if(light.brightnessScale>0)scene.lights.push_back(std::move(light));}
    fires.erase(std::remove_if(fires.begin(),fires.end(),[&](const auto& fire){return VfxLightExpired(fire.pulse,time);}),fires.end());
    for (auto& pulse:pulses) {
        if(pulse.userid>=0){SourcePose pose;if(MuzzlePosition(pulse.userid,pose)){pulse.light.position=pose.position;pulse.light.viewmodelEmitter=!strcmp(pose.source,"viewmodel");}}

        auto light=pulse.light;
        light.brightnessScale*=VfxLightEnergy(pulse,time);
        if(light.brightnessScale<=0) continue;
        scene.lights.push_back(std::move(light));
    }
}
}
