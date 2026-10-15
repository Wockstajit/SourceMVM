#include "RagdollWeaponPhysics.h"
#include "../Debug/MvmDebugLog.h"
#include "../../SchemaSystem.h"
#include <cmath>
#include <cstring>

namespace Filmmaker {
namespace {
using MotionGet = int(__fastcall*)(void*);
using MotionSet = void(__fastcall*)(void*, int);
using PoseGet = float*(__fastcall*)(void*, float*);
using PoseSet = void(__fastcall*)(void*, const float*);
using BodyGet = void*(__fastcall*)(void*, int);
template<class T> T Method(void* object, int index) { return reinterpret_cast<T>((*reinterpret_cast<void***>(object))[index]); }

void* Aggregate(CEntityInstance* weapon) {
    const auto& o=g_clientDllOffsets;
    if(!weapon || !o.ModelChain.m_CBodyComponent || !o.ModelChain.m_skeletonInstance || !o.ModelChain.m_modelState || !o.DeathWeaponPhysics.m_pVPhysicsAggregate) return nullptr;
    auto* component=*(unsigned char**)((unsigned char*)weapon+o.ModelChain.m_CBodyComponent);
    return component ? *(void**)(component+o.ModelChain.m_skeletonInstance+o.ModelChain.m_modelState+o.DeathWeaponPhysics.m_pVPhysicsAggregate) : nullptr;
}
bool Prefix(void* object,int index,const unsigned char* bytes,size_t length) {
    auto* fn=(*reinterpret_cast<unsigned char***>(object))[index];
    HMODULE module=nullptr;
    if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(const char*)fn,&module)
        || module!=GetModuleHandleA("vphysics2.dll")) return false;
    return std::memcmp(fn,bytes,length)==0;
}
bool Verified(void* aggregate,void* body) {
    // Installed vphysics2: aggregate GetBody(0); body motion type, world-origin CTransform
    // (position, scale, quaternion), linear velocity and angular velocity in degrees/sec.
    // Each call is guarded by its verified implementation prefix; no absolute DLL addresses.
    static const unsigned char getBody[]={0x85,0xD2,0x78,0x11,0x3B,0x51,0x38,0x7D,0x0C,0x48,0x8B,0x41,0x40};
    static const unsigned char getMotion[]={0x48,0x8B,0x41,0x08,0x8B,0x40,0x54};
    static const unsigned char setMotion[]={0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9};
    static const unsigned char setPose[]={0x48,0x8B,0x49,0x08,0xE9};
    static const unsigned char getPose[]={0x48,0x83,0xEC,0x68,0x48,0x8B,0x41,0x08};
    static const unsigned char setVelocity[]={0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x8B,0x02};
    static const unsigned char getVelocity[]={0x48,0x8B,0x41,0x08,0xF2,0x0F,0x10,0x80,0x08,0x01,0x00,0x00};
    static const unsigned char setAngular[]={0x40,0x53,0x48,0x83,0xEC,0x40,0xF3,0x0F,0x10,0x05};
    static const unsigned char getAngular[]={0x48,0x83,0xEC,0x18,0x48,0x8B,0x41,0x08};
    return Prefix(aggregate,24,getBody,sizeof(getBody)) && Prefix(body,4,getMotion,sizeof(getMotion))
        && Prefix(body,3,setMotion,sizeof(setMotion)) && Prefix(body,53,setPose,sizeof(setPose))
        && Prefix(body,56,getPose,sizeof(getPose)) && Prefix(body,64,setVelocity,sizeof(setVelocity))
        && Prefix(body,66,getVelocity,sizeof(getVelocity)) && Prefix(body,68,setAngular,sizeof(setAngular))
        && Prefix(body,70,getAngular,sizeof(getAngular));
}
bool Current(const RagdollWeaponPhysicsState& s) {
    return s.active && Aggregate(s.weapon)==s.aggregate && Method<BodyGet>(s.aggregate,24)(s.aggregate,0)==s.body;
}
bool Acquire(RagdollWeaponPhysicsState& s,CEntityInstance* weapon,int user,const char* phase) {
    if(Current(s)) return true;
    if(s.active) return false;
    void* aggregate=Aggregate(weapon);
    if(!aggregate) return false;
    static const unsigned char getBody[]={0x85,0xD2,0x78,0x11,0x3B,0x51,0x38,0x7D,0x0C,0x48,0x8B,0x41,0x40};
    if(!Prefix(aggregate,24,getBody,sizeof(getBody))) return false;
    void* body=Method<BodyGet>(aggregate,24)(aggregate,0);
    if(!body || !Verified(aggregate,body)) return false;
    s.weapon=weapon; s.aggregate=aggregate; s.body=body;
    s.originalMotion=Method<MotionGet>(body,4)(body);
    Method<PoseGet>(body,56)(body,s.originalPose);
    Method<PoseGet>(body,66)(body,s.originalVelocity);
    Method<PoseGet>(body,70)(body,s.originalAngular);
    s.active=true;
    MvmDebugLog_LinefAlways("ragdoll.weapon.native",
        "phase=acquire user=%d reason=%s weapon=%p aggregateOff=%td aggregate=%p body=%p originalMotion=%d",
        user,phase?phase:"-",weapon,g_clientDllOffsets.DeathWeaponPhysics.m_pVPhysicsAggregate,aggregate,body,s.originalMotion);
    return true;
}
}
bool RagdollWeaponPhysics_Release(RagdollWeaponPhysicsState& s,CEntityInstance* weapon,
    const float pose[8],const float velocity[3],const float angular[3],int user) {
    for(int i=0;i<8;++i) if(!std::isfinite(pose[i])) return false;
    for(int i=0;i<3;++i) if(!std::isfinite(velocity[i]) || !std::isfinite(angular[i])) return false;
    __try {
        if(!Acquire(s,weapon,user,"release")) return false;
        void* aggregate=s.aggregate; void* body=s.body;
        std::memcpy(s.pose,pose,sizeof(s.pose));
        std::memcpy(s.velocity,velocity,sizeof(s.velocity)); std::memcpy(s.angular,angular,sizeof(s.angular));
        s.active=true;
        MvmDebugLog_LinefAlways("ragdoll.weapon.native",
            "phase=release user=%d weapon=%p aggregateOff=%td aggregate=%p body=%p oldMotion=%d writeMotion=2 poseFn=%p velocityFn=%p angularFn=%p pos=(%.4f %.4f %.4f) v=(%.4f %.4f %.4f) omega=(%.4f %.4f %.4f)",
            user,weapon,g_clientDllOffsets.DeathWeaponPhysics.m_pVPhysicsAggregate,aggregate,body,s.originalMotion,
            Method<void*>(body,53),Method<void*>(body,64),Method<void*>(body,68),pose[0],pose[1],pose[2],velocity[0],velocity[1],velocity[2],angular[0],angular[1],angular[2]);
        Method<MotionSet>(body,3)(body,2);
        Method<PoseSet>(body,53)(body,s.pose);
        Method<PoseSet>(body,64)(body,s.velocity);
        Method<PoseSet>(body,68)(body,s.angular);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        MvmDebugLog_LinefAlways("ragdoll.weapon.native","phase=release fault=1 weapon=%p",weapon);
        RagdollWeaponPhysics_Restore(s);
        return false;
    }
}
bool RagdollWeaponPhysics_Read(RagdollWeaponPhysicsState& s,float pose[8]) {
    __try {
        if(!Current(s)) return false;
        Method<PoseGet>(s.body,56)(s.body,s.pose);
        Method<PoseGet>(s.body,66)(s.body,s.velocity);
        Method<PoseGet>(s.body,70)(s.body,s.angular);
        for(int i=0;i<8;++i) if(!std::isfinite(s.pose[i])) return false;
        std::memcpy(pose,s.pose,sizeof(s.pose));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void RagdollWeaponPhysics_Restore(RagdollWeaponPhysicsState& s) {
    __try {
        if(Current(s)) {
            Method<PoseSet>(s.body,53)(s.body,s.originalPose);
            Method<PoseSet>(s.body,64)(s.body,s.originalVelocity);
            Method<PoseSet>(s.body,68)(s.body,s.originalAngular);
            Method<MotionSet>(s.body,3)(s.body,s.originalMotion);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    s={};
}
void RagdollWeaponPhysics_Probe(RagdollWeaponPhysicsState& s,int user,const char* phase) {
    if(!MvmDebugLog_Active() || !s.active) return;
    float pose[8];
    if(!RagdollWeaponPhysics_Read(s,pose)) return;
    MvmDebugLog_LinefAlways("ragdoll.weapon.native",
        "phase=%s user=%d body=%p pos=(%.6f %.6f %.6f) q=(%.6f %.6f %.6f %.6f) v=(%.6f %.6f %.6f) omega=(%.6f %.6f %.6f)",
        phase,user,s.body,pose[0],pose[1],pose[2],pose[4],pose[5],pose[6],pose[7],s.velocity[0],s.velocity[1],s.velocity[2],s.angular[0],s.angular[1],s.angular[2]);
}
void RagdollWeaponPhysics_Relinquish(RagdollWeaponPhysicsState& s) {
    __try {
        if(Current(s) && Method<MotionGet>(s.body,4)(s.body)==2) {
            MvmDebugLog_LinefAlways("ragdoll.weapon.native","phase=pickup weapon=%p body=%p restoreMotion=%d",s.weapon,s.body,s.originalMotion);
            Method<MotionSet>(s.body,3)(s.body,s.originalMotion);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    s={};
}
}
