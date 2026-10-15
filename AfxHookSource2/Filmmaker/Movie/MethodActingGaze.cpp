#include "MethodActingGaze.h"
#include "ModelPoseAccess.h"
#include "DeathAnimationsMath.h"
#include "../Debug/MvmDebugLog.h"
#include <map>
#include <cstring>
#include <cmath>

namespace Filmmaker { namespace MethodActingGaze {
namespace {
using namespace ModelPoseAccess;
using namespace DeathAnimationsMath;
constexpr ptrdiff_t kBuffers[] = {0x18,0x48,0x80}; // local, local copy, rendered world
constexpr int kMaxHeadBones=64;
struct Rig { int head=-1, eyes[2]={-1,-1}, target=-1, headBones[kMaxHeadBones]={}, headBoneCount=0; };
struct Saved {
    const void* model=nullptr;
    unsigned int handle=0;
    float* buffers[3]={};
    int eyes[2]={-1,-1};
    float original[3][2][4]={}, written[3][2][4]={};
    int target=-1;
    float targetOriginal[3][3]={}, targetWritten[3][3]={};
    bool targetApplied=false;
    int head=-1;
    float headOriginal[2][4]={}, headWritten[2][4]={};
    int headBones[kMaxHeadBones]={}, headBoneCount=0;
    float worldOriginal[kMaxHeadBones][8]={}, worldWritten[kMaxHeadBones][8]={};
    bool headApplied=false;
    bool active=false;
};
std::map<const void*,Rig> rigs;
std::map<CEntityInstance*,Saved> saved;
unsigned long long writes=0, faults=0;

Rig FindRig(const void* model) {
    auto found=rigs.find(model); if(found!=rigs.end()) return found->second;
    Rig rig;
    const char* const* names=nullptr; const int16_t* parents=nullptr;
    if(ReadTablePointers(model,names,parents)) {
        int eyeParents[2]={-1,-1},targetParent=-1;
        bool belowHead[160]={};bool tooMany=false;
        for(int b=0;b<160;++b) {
            char name[64]; int parent;
            if(!ReadBoneEntry(names,parents,b,name,sizeof(name),parent) || parent>=b || parent < -1) break;
            if(!_stricmp(name,"head_0")) rig.head=b;
            belowHead[b]=b==rig.head || (parent>=0 && belowHead[parent]);
            if(belowHead[b]) {
                if(rig.headBoneCount<kMaxHeadBones) rig.headBones[rig.headBoneCount++]=b;
                else tooMany=true;
            }
            if(!_stricmp(name,"eyeball_l")) {rig.eyes[0]=b;eyeParents[0]=parent;}
            if(!_stricmp(name,"eyeball_r")) {rig.eyes[1]=b;eyeParents[1]=parent;}
            if(!_stricmp(name,"eye_target")) {rig.target=b;targetParent=parent;}
        }
        if(eyeParents[0]!=rig.head || eyeParents[1]!=rig.head || targetParent!=rig.head) rig=Rig();
        else if(tooMany) rig.headBoneCount=0;
    }
    rigs[model]=rig;
    MvmDebugLog_LinefAlways("acting.rig","model=%p names@0x168 parents@0x180 head=%d eyes=%d,%d target=%d",
        model,rig.head,rig.eyes[0],rig.eyes[1],rig.target);
    return rig;
}
bool Buffers(unsigned char* ms,float* out[3]) {
    __try {
        for(int i=0;i<3;++i) {out[i]=*(float**)(ms+kBuffers[i]);if((uintptr_t)out[i]<0x10000)return false;}
        return true;
    } __except(1) {++faults;return false;}
}
bool ValidPose(const float* p) {
    for(int i=0;i<8;++i) if(!std::isfinite(p[i])) return false;
    const float norm=p[4]*p[4]+p[5]*p[5]+p[6]*p[6]+p[7]*p[7];
    return p[3]>.5f && p[3]<2.f && norm>.95f && norm<1.05f;
}
bool ReadRaw(float* const buffers[3],const Rig& rig,FaceView& view) {
    __try {
        // Capability gate: named eyes must use the shipped +Y-facing head rig.
        const float* target=buffers[0]+rig.target*8;
        if(!ValidPose(target) || std::abs(target[0])>1 || target[1]<90 || target[1]>110 || std::abs(target[2])>1) return false;
        const float* head=buffers[2]+rig.head*8;
        if(!ValidPose(head)) return false;
        for(int j=0;j<2;++j) for(int i=0;i<3;++i)
            if(!ValidPose(buffers[i]+rig.eyes[j]*8)) return false;
        for(int k=0;k<3;++k) view.position[k]=(buffers[2][rig.eyes[0]*8+k]+buffers[2][rig.eyes[1]*8+k])*.5f;
        for(int k=0;k<4;++k) view.headRotation[k]=head[4+k];
        return true;
    } __except(1) {++faults;return false;}
}
bool SameWrittenPose(const float* current,const float* written,int count) {
    for(int k=0;k<count;++k)
        if(!std::isfinite(current[k]) || !std::isfinite(written[k]) ||
           std::abs(current[k]-written[k])>1e-4f) return false;
    return true;
}
void RestoreRaw(float* const buffers[3],Saved& s,bool force) {
    __try {
        if(s.targetApplied) for(int i=0;i<3;++i) if(buffers[i]==s.buffers[i]) {
            float* p=buffers[i]+s.target*8;
            if(force || SameWrittenPose(p,s.targetWritten[i],3))
                std::memcpy(p,s.targetOriginal[i],sizeof(float)*3);
        }
        if(buffers[2]==s.buffers[2]) for(int b=0;b<s.headBoneCount;++b) {
            float* pose=buffers[2]+s.headBones[b]*8;
            if(force || SameWrittenPose(pose,s.worldWritten[b],8))
                std::memcpy(pose,s.worldOriginal[b],sizeof(float)*8);
        }
        // The renderer consumes the parent-local buffers. Restore only if this is still our pose.
        if(s.headApplied) for(int i=0;i<2;++i) if(buffers[i]==s.buffers[i]) {
            float* q=buffers[i]+s.head*8+4;
            if(force || SameWrittenPose(q,s.headWritten[i],4))
                std::memcpy(q,s.headOriginal[i],sizeof(float)*4);
        }
        for(int i=0;i<3;++i) if(buffers[i]==s.buffers[i]) for(int j=0;j<2;++j) {
            float* q=buffers[i]+s.eyes[j]*8+4;
            if(force || SameWrittenPose(q,s.written[i][j],4)) std::memcpy(q,s.original[i][j],sizeof(float)*4);
        }
    } __except(1) {++faults;}
    s.active=false;
}
bool WriteRaw(float* const buffers[3],const Rig& rig,const FaceView& view,MethodActingMath::Gaze gaze,double headYaw,bool moveEyeTarget,Saved& s) {
    if(!std::isfinite(headYaw) || std::abs(headYaw)>45.001 || !rig.headBoneCount) headYaw=0;
    if(headYaw) {
        __try {
            for(int i=0;i<2;++i) if(!ValidPose(buffers[i]+rig.head*8)) headYaw=0;
            for(int b=0;b<rig.headBoneCount;++b)
                if(!ValidPose(buffers[2]+rig.headBones[b]*8)) headYaw=0;
        } __except(1) {++faults;headYaw=0;}
    }
    float local[4],world[4];
    MethodActingMath::EyeRotation({gaze.yaw-headYaw,gaze.up},local);
    QuatMul(view.headRotation,local,world);QuatNormalize(world);
    __try {
        if(moveEyeTarget) {
            // The eye material can aim from eye_target instead of the eyeball bones.
            // Shift its authored point (100 units ahead of the head) along the desired
            // gaze ray. Keep the three pose buffers in agreement for both render paths.
            const float localDelta[3]={
                (float)(100.0*std::tan(gaze.up*0.017453292519943295)),0.0f,
                (float)(100.0*std::tan(gaze.yaw*0.017453292519943295))};
            float worldDelta[3];QuatRotate(view.headRotation,localDelta,worldDelta);
            s.target=rig.target;
            for(int i=0;i<3;++i) {
                float* p=buffers[i]+rig.target*8;
                std::memcpy(s.targetOriginal[i],p,sizeof(float)*3);
                for(int k=0;k<3;++k) s.targetWritten[i][k]=p[k]+(i==2?worldDelta[k]:localDelta[k]);
                std::memcpy(p,s.targetWritten[i],sizeof(float)*3);
            }
            s.targetApplied=true;
        }
        for(int i=0;i<3;++i) {
            s.buffers[i]=buffers[i];
            for(int j=0;j<2;++j) {
                s.eyes[j]=rig.eyes[j];float* q=buffers[i]+rig.eyes[j]*8+4;
                std::memcpy(s.original[i][j],q,sizeof(float)*4);
                std::memcpy(s.written[i][j],i==2?world:local,sizeof(float)*4);
                std::memcpy(q,s.written[i][j],sizeof(float)*4);
            }
        }
        if(headYaw) {
            s.head=rig.head;
            const float half=float(headYaw*0.008726646259971648);
            const float turn[4]={std::sin(half),0,0,std::cos(half)};
            for(int i=0;i<2;++i) {
                float* q=buffers[i]+rig.head*8+4;
                std::memcpy(s.headOriginal[i],q,sizeof(float)*4);
                QuatMul(s.headOriginal[i],turn,s.headWritten[i]);QuatNormalize(s.headWritten[i]);
                std::memcpy(q,s.headWritten[i],sizeof(float)*4);
            }
            // Keep the derived world pose in step with the two local copies. CS2 uses both
            // paths across render passes; a turned local head with stale world descendants
            // visibly alternates with an unturned face during the freeze-time close-up.
            float inverse[4],tmp[4],delta[4];
            QuatConj(view.headRotation,inverse);
            QuatMul(view.headRotation,turn,tmp);
            QuatMul(tmp,inverse,delta);QuatNormalize(delta);
            const float origin[3]={buffers[2][rig.head*8],buffers[2][rig.head*8+1],buffers[2][rig.head*8+2]};
            s.headBoneCount=rig.headBoneCount;
            for(int b=0;b<rig.headBoneCount;++b) {
                s.headBones[b]=rig.headBones[b];
                float* pose=buffers[2]+rig.headBones[b]*8;
                std::memcpy(s.worldOriginal[b],pose,sizeof(float)*8);
                float relative[3],rotated[3];
                for(int k=0;k<3;++k) relative[k]=pose[k]-origin[k];
                QuatRotate(delta,relative,rotated);
                for(int k=0;k<3;++k) s.worldWritten[b][k]=origin[k]+rotated[k];
                s.worldWritten[b][3]=pose[3];
                QuatMul(delta,pose+4,s.worldWritten[b]+4);QuatNormalize(s.worldWritten[b]+4);
                std::memcpy(pose,s.worldWritten[b],sizeof(float)*8);
            }
            s.headApplied=true;
        }
        s.active=true;++writes;return true;
    } __except(1) {++faults;return false;}
}
}
bool ReadFace(CEntityInstance* entity,FaceView& view) {
    auto* ms=ModelStateOf(entity);const void* model=ModelOf(ms);if(!model)return false;
    const Rig rig=FindRig(model);if(rig.head<0||rig.eyes[0]<0||rig.eyes[1]<0||rig.target<0)return false;
    float* buffers[3];return Buffers(ms,buffers)&&ReadRaw(buffers,rig,view);
}
void Release(CEntityInstance* entity,bool force) {
    auto it=saved.find(entity);if(it==saved.end())return;
    Saved& s=it->second;auto* ms=ModelStateOf(entity);
    if(s.active && ModelOf(ms)==s.model && entity->GetHandle().ToInt()==s.handle) {
        float* buffers[3];if(Buffers(ms,buffers))RestoreRaw(buffers,s,force);
    }
    saved.erase(it);
}
bool Apply(CEntityInstance* entity,MethodActingMath::Gaze gaze,double headYaw,bool moveEyeTarget) {
    Release(entity);
    auto* ms=ModelStateOf(entity);const void* model=ModelOf(ms);if(!model)return false;
    const Rig rig=FindRig(model);if(rig.head<0||rig.eyes[0]<0||rig.eyes[1]<0||rig.target<0)return false;
    float* buffers[3];FaceView view;
    if(!Buffers(ms,buffers)||!ReadRaw(buffers,rig,view))return false;
    Saved& s=saved[entity];s.model=model;s.handle=entity->GetHandle().ToInt();
    const bool ok=WriteRaw(buffers,rig,view,gaze,headYaw,moveEyeTarget,s);
    if(MvmDebugLog_Active() && writes%240==1) MvmDebugLog_Linef("acting.gaze",
        "entity=%p modelState=%p local@0x18=%p copy@0x48=%p world@0x80=%p eyes=%d,%d yaw=%.2f up=%.2f head=%.2f applied=%d bones=%d q=%.4f,%.4f,%.4f,%.4f ok=%d",
        entity,ms,buffers[0],buffers[1],buffers[2],rig.eyes[0],rig.eyes[1],gaze.yaw,gaze.up,headYaw,(int)s.headApplied,s.headBoneCount,
        s.written[0][0][0],s.written[0][0][1],s.written[0][0][2],s.written[0][0][3],ok);
    return ok;
}
void ReleaseAll(bool force) {
    // Resolve each saved handle again; never dereference a freed pawn from an old frame.
    for(auto it=saved.begin();it!=saved.end();) {
        auto* e=it->first;const unsigned int handle=it->second.handle;++it;
        const int idx=handle&0x7fff;
        auto* current=(g_pEntityList&&*g_pEntityList&&g_GetEntityFromIndex)?
            (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList,idx):nullptr;
        if(current==e && current->GetHandle().ToInt()==handle)Release(e,force);else saved.erase(e);
    }
}
unsigned long long Writes(){return writes;}
unsigned long long Faults(){return faults;}
} }
