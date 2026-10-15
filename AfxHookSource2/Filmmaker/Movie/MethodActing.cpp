#include "MethodActing.h"

#include "MethodActingMath.h"
#include "MethodActingFlex.h"
#include "MethodActingGaze.h"
#include "RagdollBodies.h"
#include "RagdollWeaponGround.h"
#include "../Platform/JsonParser.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_Active / MvmDebugLog_Linef
#include "../../ClientEntitySystem.h"      // EntityAt / GetHighestEntityIndex / CEntityInstance
#include "../../SchemaSystem.h"            // g_clientDllOffsets (m_iShotsFired)
#include "../../../Shared/AfxConsole.h"

// SourceSdkShared.h must precede cdll_int.h -- it defines SOURCESDK_abstract_class, without
// which cdll_int.h does not parse. Same include order as RagdollPhysics.cpp.
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/igameevents.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

using namespace MethodActingMath;

// CS2 runs 64 tick. The demo tick is the only clock that pauses when the demo pauses and rewinds
// when the user scrubs, which is exactly the behaviour a movie tool needs out of a face.
constexpr double kTickRate = 64.0;

// Two ticks of tolerance either way is normal playback; anything larger is a seek or a level
// change, and integrated state must be dropped rather than smeared across the jump.
constexpr int kSeekTickTolerance = 8;

// Entity indices below the client-only range. Player pawns live well under this.
constexpr int kMaxTrackedEntities = 2048;

bool g_enabled = false;
bool g_blinkEnabled = true;
bool g_settleEyes = true;
bool g_gazeEnabled = true, g_deathEyes = true;

SquintParams g_squint;   // level / attack / hold / release
BlinkParams  g_blink;    // interval / close / open

// True once we have written a non-zero weight to somebody, so switching the feature off knows it
// has faces to hand back to the engine.
bool g_dirty = false;

int g_lastTick = -1;

struct PlayerFace {
    unsigned int handle = 0;
    double deathTime = -1;
    double lastThreatTime = -1;
    double lastHurtTime = -1,flashUntil = -1;
    int lastHealth = -1;
    bool wasAlive = false;
    Gaze gaze;
    double headYaw = 0;
    bool haveGaze = false;
	int    lastShots = -1;      // last observed m_iShotsFired
	double lastShotTime = -1.0; // demo seconds; <0 = has not fired
	double squint = 0.0;        // integrated, 0..1
	int    lastSeenTick = -1;
};

std::unordered_map<int, PlayerFace> g_faces;

// Diagnostics, reported by `mirv_filmmaker methodacting` and the health line.
unsigned long long g_framesRun = 0;
unsigned long long g_shotsSeen = 0;
unsigned long long g_seeks = 0;

// Local, like every other module's: the entity-list globals are the shared
// surface, the two-line accessor is not, and duplicating it keeps this module free of any other
// feature's header.
CEntityInstance* EntityAt(int index) {
	if (index < 0 || index > GetHighestEntityIndex() || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index);
}

int DemoTick() {
	if (!g_pEngineToClient) return -1;
	if (auto pDemo = g_pEngineToClient->GetDemoFile()) return pDemo->GetDemoTick();
	return -1;
}

// Per-player blink phase. The entity index is stable for the life of a pawn and differs between
// players, which is all this needs: two players standing together must not blink in lockstep,
// and the same player must blink identically on a second visit to the same tick.
uint32_t SeedFor(int entityIndex) {
	return (uint32_t)entityIndex * 2654435761u + 0x1F123BB5u;
}

void MarkEvent(CEntityInstance* pawn, double now, bool threat) {
    if(!pawn || !pawn->IsPlayerPawn() || pawn->GetHealth()<=0) return;
    const int index=pawn->GetHandle().GetEntryIndex();
    if(index<0 || index>=kMaxTrackedEntities) return;
    PlayerFace& face=g_faces[index];
    const unsigned int handle=pawn->GetHandle().ToInt();
    if(face.handle!=handle) {face=PlayerFace();face.handle=handle;}
    if(threat) face.lastThreatTime=now;
    else face.lastShotTime=now;
}

bool BulletWeapon(const std::string& weapon) {
    if(weapon.empty()) return true;
    static const char* const utility[]={"knife","grenade","flash","smoke","molotov","incendiary","decoy","taser","c4"};
    for(const char* excluded:utility) if(weapon.find(excluded)!=std::string::npos) return false;
    return true;
}

// -1 when the field never resolved (a CS2 rename) or the read is not trustworthy.
int ReadShotsFired(CEntityInstance* entity) {
	const ptrdiff_t off = g_clientDllOffsets.C_CSPlayerPawn.m_iShotsFired;
	if (!off || !entity) return -1;
	return *(int*)((unsigned char*)entity + off);
}

// Hand every face we have touched back to the engine. Called when the feature is switched off and
// when the last of it is torn down, so a disabled toggle never leaves a player squinting forever.
void ReleaseAll() {
    MethodActingGaze::ReleaseAll(true);
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest && i < kMaxTrackedEntities; ++i) {
		CEntityInstance* entity = EntityAt(i);
		if (!entity || !entity->IsPlayerPawn()) continue;
        // The pawn can have no facial model while its client ragdoll still does.
        // Restore each carrier independently, including paused corpses.
        CEntityInstance* carriers[2]={entity,RagdollBodies::ClientRagdollOf(entity)};
        for(auto* carrier:carriers) {
            MethodActingFlex::EyeIndices eyes;
            if(!carrier || !MethodActingFlex::FindEyeIndices(carrier,&eyes))continue;
            if(eyes.blink>=0) MethodActingFlex::SetWeight(carrier,eyes.blink,0);
            if(eyes.lookUp>=0) MethodActingFlex::SetWeight(carrier,eyes.lookUp,0);
            if(eyes.lookDown>=0) MethodActingFlex::SetWeight(carrier,eyes.lookDown,0);
        }
	}
	g_faces.clear();
	g_dirty = false;
	MvmDebugLog_LinefAlways("acting", "released all faces");
}

} // namespace

// ---- state ---------------------------------------------------------------------------------

bool MethodActing_Enabled() { return g_enabled; }

void MethodActing_SetEnabled(bool enabled) {
    // Off is idempotent: also clear any pose left by an earlier failed restoration.
    if(!enabled) {
        g_enabled=false;
        ReleaseAll();
        g_lastTick=-1;
        return;
    }
    if(g_enabled)return;
    g_enabled=true;
    g_lastTick = -1; // do not treat the first frame's tick delta as a shot or a seek
    MethodActingFlex::Resolve();
}

bool MethodActing_BlinkEnabled() { return g_blinkEnabled; }
void MethodActing_SetBlinkEnabled(bool enabled) { g_blinkEnabled = enabled; }

double MethodActing_SquintLevel() { return g_squint.level; }
void   MethodActing_SetSquintLevel(double level) {
	g_squint.level = level < 0.0 ? 0.0 : (level > 1.0 ? 1.0 : level);
}

double MethodActing_HoldMs() { return g_squint.holdSec * 1000.0; }
void   MethodActing_SetHoldMs(double ms) {
	if (ms < 0.0) ms = 0.0;
	if (ms > 5000.0) ms = 5000.0;
	g_squint.holdSec = ms / 1000.0;
}

double MethodActing_AttackMs() { return g_squint.attackSec * 1000.0; }
void   MethodActing_SetAttackMs(double ms) {
	if (ms < 1.0) ms = 1.0;
	if (ms > 2000.0) ms = 2000.0;
	g_squint.attackSec = ms / 1000.0;
}

double MethodActing_ReleaseMs() { return g_squint.releaseSec * 1000.0; }
void   MethodActing_SetReleaseMs(double ms) {
	if (ms < 1.0) ms = 1.0;
	if (ms > 5000.0) ms = 5000.0;
	g_squint.releaseSec = ms / 1000.0;
}

bool MethodActing_SettleEyes() { return g_settleEyes; }
void MethodActing_SetSettleEyes(bool enabled) { g_settleEyes = enabled; }

double MethodActing_DoubleBlinkChance() { return g_blink.doubleChance; }
void   MethodActing_SetDoubleBlinkChance(double chance) {
	g_blink.doubleChance = chance < 0.0 ? 0.0 : (chance > 1.0 ? 1.0 : chance);
}

double MethodActing_BlinkIntervalSec() { return g_blink.intervalSec; }
void   MethodActing_SetBlinkIntervalSec(double seconds) {
	if (seconds < 0.5) seconds = 0.5;
	if (seconds > 30.0) seconds = 30.0;
	g_blink.intervalSec = seconds;
}

bool MethodActing_GazeEnabled() { return g_gazeEnabled; }
void MethodActing_SetGazeEnabled(bool enabled) { g_gazeEnabled=enabled; if(!enabled) MethodActingGaze::ReleaseAll(true); }
bool MethodActing_DeathEyesEnabled() { return g_deathEyes; }
void MethodActing_SetDeathEyesEnabled(bool enabled) { if(g_deathEyes!=enabled) ReleaseAll(); g_deathEyes=enabled; }

void MethodActing_ResetAll() {
	MethodActing_SetEnabled(false);
	g_blinkEnabled = true;
	g_settleEyes = true;
    g_gazeEnabled = g_deathEyes = true;
	g_squint = SquintParams();
	g_blink = BlinkParams();
}

void MethodActing_OnGameEvent(SOURCESDK::CS2::CGameEvent* event) {
    if(!g_enabled || !event) return;
    const char* name=event->GetName();
    if(!name) return;
    const bool fire=!_stricmp(name,"weapon_fire");
    const bool impact=!_stricmp(name,"bullet_impact");
    const bool hurt=!_stricmp(name,"player_hurt");
    const bool blind=!_stricmp(name,"player_blind");
    if(!fire && !impact && !hurt && !blind) return;
    const int tick=DemoTick();if(tick<0)return;
    JsonValue root;if(!RagdollBodies::EventJson(event,root))return;
    const double now=(double)tick/kTickRate;
    const int userid=RagdollBodies::JInt(root,"userid");
    if(userid<0)return;
    CEntityInstance* pawn=RagdollBodies::PawnOfUser(userid,nullptr);
    if(MvmDebugLog_Active()) {
        static int fireTraces=0, impactTraces=0, hurtTraces=0, blindTraces=0;
        int& traces=fire?fireTraces:impact?impactTraces:blind?blindTraces:hurtTraces;
        if(traces++<8) MvmDebugLog_Linef("acting.event",
            "name=%s tick=%d userid=%d pawn=%p weapon=%s",
            name,tick,userid,pawn,RagdollBodies::JStr(root,"weapon").c_str());
    }
    if(hurt || blind) {
        if(!pawn || !pawn->IsPlayerPawn() || pawn->GetHealth()<=0)return;
        const int index=pawn->GetHandle().GetEntryIndex();if(index<0||index>=kMaxTrackedEntities)return;
        PlayerFace& face=g_faces[index];const auto handle=pawn->GetHandle().ToInt();
        if(face.handle!=handle){face=PlayerFace();face.handle=handle;}
        if(hurt)face.lastHurtTime=now;
        else {
            const auto value=root.Find("blind_duration");
            const double duration=value?value->AsNumber(0):0;
            if(!std::isfinite(duration)||duration<=0)return;
            face.flashUntil=(std::max)(face.flashUntil,now+(std::min)(6.0,duration));
        }
        MvmDebugLog_Linef("acting.reaction","event=%s tick=%d userid=%d target=%p handle=%u hurtTime=%.3f flashUntil=%.3f",
            name,tick,userid,pawn,handle,face.lastHurtTime,face.flashUntil);
        return;
    }
    if(!pawn || pawn->GetHealth()<=0)return;
    if(fire) {
        if(!BulletWeapon(RagdollBodies::JStr(root,"weapon")))return;
        MarkEvent(pawn,now,false);
        // Some demos omit bullet_impact events. A nearby enemy in the firing direction
        // should still hold a defensive squint during the burst.
        MethodActingGaze::FaceView shooter;
        if(!MethodActingGaze::ReadFace(pawn,shooter))return;
        const int team=pawn->GetTeam();
        if(team!=2 && team!=3)return;
        const float localForward[3]={0,1,0};float forward[3],end[3];
        DeathAnimationsMath::QuatRotate(shooter.headRotation,localForward,forward);
        for(int k=0;k<3;++k)end[k]=shooter.position[k]+forward[k]*900.0f;
        const int highest=GetHighestEntityIndex();
        for(int i=0;i<=highest && i<kMaxTrackedEntities;++i) {
            CEntityInstance* target=EntityAt(i);
            if(!target || target==pawn || !target->IsPlayerPawn() || target->GetHealth()<=0 ||
               target->GetTeam()==team)continue;
            float point[3];MethodActingGaze::FaceView view;
            if(MethodActingGaze::ReadFace(target,view))
                for(int k=0;k<3;++k)point[k]=view.position[k];
            else {
                if(!RagdollBodies::SafeOrigin(target,point))continue;
                point[2]+=60.0f;
            }
            float along=0;
            for(int k=0;k<3;++k)along+=(point[k]-shooter.position[k])*forward[k];
            if(along<0 || along>900.0f)continue;
            if(!NearBullet(shooter.position,end,point,260.0))continue;
            MarkEvent(target,now,true);
            MvmDebugLog_Linef("acting.threat","tick=%d shooter=%d target=%d source=weapon_fire",
                tick,pawn->GetHandle().GetEntryIndex(),i);
        }
        return;
    }
    const JsonValue* x=root.Find("x"),*y=root.Find("y"),*z=root.Find("z");
    if(!x || !y || !z || x->type!=JsonValue::Type::Number ||
       y->type!=JsonValue::Type::Number || z->type!=JsonValue::Type::Number) return;
    const float hit[3]={(float)x->number,(float)y->number,(float)z->number};
    float start[3];MethodActingGaze::FaceView shooter;
    if(MethodActingGaze::ReadFace(pawn,shooter))
        for(int k=0;k<3;++k)start[k]=shooter.position[k];
    else {
        if(!RagdollBodies::SafeOrigin(pawn,start))return;
        start[2]+=60.0f;
    }
    const int team=pawn->GetTeam();
    if(team!=2 && team!=3)return;
    const int highest=GetHighestEntityIndex();
    for(int i=0;i<=highest && i<kMaxTrackedEntities;++i) {
        CEntityInstance* target=EntityAt(i);
        if(!target || target==pawn || !target->IsPlayerPawn() || target->GetHealth()<=0 ||
           target->GetTeam()==team) continue;
        float face[3];MethodActingGaze::FaceView view;
        if(MethodActingGaze::ReadFace(target,view))
            for(int k=0;k<3;++k)face[k]=view.position[k];
        else {
            if(!RagdollBodies::SafeOrigin(target,face))continue;
            face[2]+=60.0f;
        }
        if(!NearBullet(start,hit,face,120.0))continue;
        MarkEvent(target,now,true);
        MvmDebugLog_Linef("acting.threat","tick=%d shooter=%d target=%d miss=near impact=(%.0f,%.0f,%.0f)",
            tick,pawn->GetHandle().GetEntryIndex(),i,hit[0],hit[1],hit[2]);
    }
}

// ---- per-frame -----------------------------------------------------------------------------

void MethodActing_RunLateFrame(bool afterAnimation) {
	if (!g_enabled)
		return;
	if (!MethodActingFlex::Resolve())
		return;

	MethodActingGaze::ReleaseAll();
	const int tick = DemoTick();
	if (tick < 0) {
		// Not in a demo (or between levels). Drop integrated state so re-entering a demo starts
		// from a relaxed face rather than resuming a burst that ended minutes ago.
		g_lastTick = -1;
		g_faces.clear();
		return;
	}

	const int delta = (g_lastTick < 0) ? 0 : (tick - g_lastTick);
	const bool seek = (g_lastTick >= 0) && (delta < -kSeekTickTolerance || delta > kSeekTickTolerance);
	if (seek) { ++g_seeks; g_faces.clear(); }

	// Clamp dt to a sane forward step. The demo clock steps backwards by a few milliseconds during
	// normal playback, and integrating a negative dt would run the squint envelope in reverse.
	double dt = (double)delta / kTickRate;
	if (!(dt > 0.0) || seek) dt = 0.0;

	const double now = (double)tick / kTickRate;
	g_lastTick = tick;
	++g_framesRun;

    const int highest = GetHighestEntityIndex();
    if(g_gazeEnabled) RagdollWeaponGround_Pump();
    struct Nearby { CEntityInstance* entity; MethodActingGaze::FaceView view; };
    std::vector<Nearby> nearby;
    if(g_gazeEnabled) for(int i=0;i<=highest && i<kMaxTrackedEntities;++i) {
        auto* e=EntityAt(i); MethodActingGaze::FaceView view;
        if(e && e->IsPlayerPawn() && e->GetHealth()>0 && MethodActingGaze::ReadFace(e,view)) nearby.push_back({e,view});
    }
    for (int i = 0; i <= highest && i < kMaxTrackedEntities; ++i) {
        CEntityInstance* entity = EntityAt(i);
        if (!entity || !entity->IsPlayerPawn()) continue;

        PlayerFace& face = g_faces[i];
        const auto handle=entity->GetHandle().ToInt();
        if(face.handle!=handle) { face=PlayerFace();face.handle=handle; }
        const bool alive=entity->GetHealth()>0;
        if(!alive) {
            face.lastHealth=-1;
            if(face.deathTime<0) face.deathTime=face.wasAlive?now:now-2.0;
            face.wasAlive=false;
            if(!g_deathEyes) continue;
			const DeathEyes pose=DeathEyePose(now-face.deathTime,SeedFor(i));
            CEntityInstance* carriers[2]={entity,RagdollBodies::ClientRagdollOf(entity)};
            for(auto* carrier:carriers) {
                if(!carrier)continue;
                const bool rolled=MethodActingGaze::Apply(carrier,{pose.yaw,pose.up},0.0,true);
                MethodActingFlex::EyeIndices eyes;
                const bool hasLids=MethodActingFlex::FindEyeIndices(carrier,&eyes) && eyes.blink>=0;
                if(hasLids) {
                    MethodActingFlex::SetWeight(carrier,eyes.blink,(float)pose.closed);
                    if(eyes.lookUp>=0) MethodActingFlex::SetWeight(carrier,eyes.lookUp,(float)Clamp01(pose.up/25.0));
                    if(eyes.lookDown>=0) MethodActingFlex::SetWeight(carrier,eyes.lookDown,0);
                }
                if(rolled || hasLids)g_dirty=true;
                if(MvmDebugLog_Active() && g_framesRun%32==0) {
                    float origin[3]={};RagdollBodies::SafeOrigin(carrier,origin);
                    MvmDebugLog_Linef("acting.death",
                        "ent=%d carrier=%p age=%.3f up=%.2f closed=%.3f rolled=%d blinkIdx=%d pos=(%.1f,%.1f,%.1f)",
                        i,carrier,now-face.deathTime,pose.up,pose.closed,(int)rolled,hasLids?eyes.blink:-1,origin[0],origin[1],origin[2]);
                }
            }
            continue;
        }
        face.wasAlive=true;face.deathTime=-1;
        // Demo health updates are a fallback when the victim's hurt event is absent.
        const int health=entity->GetHealth();
        if(!seek && face.lastHealth>=0 && health<face.lastHealth) {
            face.lastHurtTime=now;
            MvmDebugLog_Linef("acting.reaction","event=health_drop tick=%d target=%p handle=%u health=%d->%d",
                tick,entity,handle,face.lastHealth,health);
        }
        face.lastHealth=health;

		// --- shot detection -------------------------------------------------------------------
		// m_iShotsFired counts up through a burst and resets to 0 when the trigger is released,
		// so any CHANGE to a non-zero value is one round leaving the barrel. On a seek the
		// counter jumps arbitrarily, so the change is recorded but never treated as a shot.
		const int shots = ReadShotsFired(entity);
		if (shots >= 0) {
			const bool fired = !seek && face.lastShots >= 0 && shots != face.lastShots && shots > 0;
			if (fired) {
				face.lastShotTime = now;
				++g_shotsSeen;
			}
			face.lastShots = shots;
		}

		// --- envelopes ------------------------------------------------------------------------
		const double ownAge=face.lastShotTime<0.0 ? -1.0 : now-face.lastShotTime;
		const double threatAge=face.lastThreatTime<0.0 ? -1.0 : now-face.lastThreatTime;
		const double ownSquint=SquintTarget(ownAge,g_squint);
		const double incomingSquint=ThreatSquintTarget(threatAge,g_squint);
        const double hurtSquint=HurtSquintTarget(face.lastHurtTime<0?-1:now-face.lastHurtTime,g_squint);
        const double flashSquint=FlashSquintTarget(face.flashUntil-now,g_squint);
        const double squintTarget=(std::max)((std::max)(ownSquint,incomingSquint),(std::max)(hurtSquint,flashSquint));
		if (seek) {
			// Re-derive the squint directly from the (now meaningless) shot clock rather than
			// integrating across the jump: one frame of possibly-wrong squint, never a stuck one.
			face.squint = squintTarget;
		} else {
			face.squint = SquintStep(face.squint,squintTarget,dt,g_squint);
		}

		const double blink = g_blinkEnabled ? BlinkWeight(now, SeedFor(i), g_blink) : 0.0;
		const double weight = Combine(face.squint, blink);
		face.lastSeenTick = tick;

		MethodActingFlex::EyeIndices eyes;
		if (!MethodActingFlex::FindEyeIndices(entity, &eyes) || eyes.blink < 0)
			continue;

		if (MethodActingFlex::SetWeight(entity, eyes.blink, (float)weight) && weight > 0.0)
			g_dirty = true;

        if(g_gazeEnabled) {
            MethodActingGaze::FaceView view;
            if(MethodActingGaze::ReadFace(entity,view)) {
                bool fighting=shots>0 || (ownAge>=0 && ownAge<2.5) || (threatAge>=0 && threatAge<1.5);
                if(!fighting) for(const auto& other:nearby) {
                    if(other.entity==entity || ReadShotsFired(other.entity)<=0) continue;
                    double distance2=0;
                    for(int k=0;k<3;++k) {
                        const double d=other.view.position[k]-view.position[k];distance2+=d*d;
                    }
                    if(distance2<1200.0*1200.0) {fighting=true;break;}
                }
                const bool recovering=hurtSquint>.001 || flashSquint>.001;
                const IdleLook idle=fighting?CombatWanderPose(now,SeedFor(i)):WanderPose(now,SeedFor(i));
                Gaze target=idle.gaze;
                int lookingAt=-1;
                // Each glance has a rest period; people do not stare at the nearest player forever.
                const double phase=std::fmod(now+Unit01(Hash32(SeedFor(i),7))*5,5.0);
                if(recovering) target={0,0};
                else if(!fighting && phase<2.4 && std::strstr(RagdollWeaponGround_Status(),"ready")) {
                    double nearest=221;
                    for(const auto& other:nearby) {
                        if(other.entity==entity)continue;
                        float d[3],local[3],inv[4];double dist2=0;
                        for(int k=0;k<3;++k){d[k]=other.view.position[k]-view.position[k];dist2+=d[k]*d[k];}
                        const double distance=std::sqrt(dist2); if(distance>=nearest)continue;
                        DeathAnimationsMath::QuatConj(view.headRotation,inv);
                        DeathAnimationsMath::QuatRotate(inv,d,local);
                        Gaze candidate;if(!NearbyGaze(local,distance,candidate))continue;
                        float dir[3],hit[3],normal[3];for(int k=0;k<3;++k)dir[k]=(float)(d[k]/distance);
                        if(RagdollWeaponGround_Ray(view.position,dir,(float)distance-4,hit,normal))continue;
                        nearest=distance;target=candidate;lookingAt=other.entity->GetHandle().GetEntryIndex();
                    }
                }
                const double targetHead=recovering ? 0.0 :
                    lookingAt>=0 ? HeadFollowYaw(target.yaw) : idle.headYaw;
                if(!face.haveGaze || seek) {face.gaze=target;face.headYaw=targetHead;}
                else {face.gaze=GazeStep(face.gaze,target,dt);face.headYaw=HeadStep(face.headYaw,targetHead,dt);}
                face.haveGaze=true;
                MethodActingGaze::Apply(entity,face.gaze,afterAnimation?face.headYaw:0.0);
                if(eyes.lookUp>=0) MethodActingFlex::SetWeight(entity,eyes.lookUp,(float)Clamp01(face.gaze.up/20));
                if(eyes.lookDown>=0) MethodActingFlex::SetWeight(entity,eyes.lookDown,(float)Clamp01(-face.gaze.up/20));
                g_dirty=true;
                if(MvmDebugLog_Active() && g_framesRun%64==0) MvmDebugLog_Linef("acting.look",
                    "ent=%d tick=%d target=%d fighting=%d yaw=%.2f up=%.2f head=%.2f",i,tick,lookingAt,(int)fighting,face.gaze.yaw,face.gaze.up,face.headYaw);
            }
        } else if(g_settleEyes && face.squint>.001) {
            if(eyes.lookUp>=0) MethodActingFlex::SetWeight(entity,eyes.lookUp,0);
            if(eyes.lookDown>=0) MethodActingFlex::SetWeight(entity,eyes.lookDown,0);
        }

		if (MvmDebugLog_Active() && weight > 0.0 && (g_framesRun % 16) == 0) {
			MvmDebugLog_Linef("acting",
				"ent=%d tick=%d squint=%.3f blink=%.3f w=%.3f blinkIdx=%d up=%d down=%d shots=%d",
				i, tick, face.squint, blink, weight, eyes.blink, eyes.lookUp, eyes.lookDown, shots);
		}
	}
}

// ---- reporting -----------------------------------------------------------------------------

void MethodActing_AppendStateJson(std::ostringstream& o) {
	o << ",\"actingOn\":" << (g_enabled ? "true" : "false")
	  << ",\"actingGaze\":" << (g_gazeEnabled ? "true" : "false")
      << ",\"actingDeathEyes\":" << (g_deathEyes ? "true" : "false")
	  << ",\"actingBlink\":" << (g_blinkEnabled ? "true" : "false")
	  << ",\"actingSquint\":" << (g_squint.level * 100.0)
	  << ",\"actingHoldMs\":" << MethodActing_HoldMs()
	  << ",\"actingAttackMs\":" << MethodActing_AttackMs()
	  << ",\"actingReleaseMs\":" << MethodActing_ReleaseMs()
	  << ",\"actingBlinkSec\":" << g_blink.intervalSec
	  << ",\"actingDouble\":" << (g_blink.doubleChance * 100.0)
	  << ",\"actingSettle\":" << (g_settleEyes ? "true" : "false");
}

void MethodActing_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	// argv: mirv_filmmaker methodacting [on|off|toggle | blink on|off | squint <0..1> |
	//                                    hold <ms> | attack <ms> | release <ms> |
	//                                    blinkrate <sec> | index <n|auto> | reset | status]
	const char* sub = (argc >= 3) ? args->ArgV(2) : "";
	const char* val = (argc >= 4) ? args->ArgV(3) : "";
	// The Config panel sliders append "quiet", exactly as the ragdoll physics knobs do --
	// a dragged slider must not spray one console line per frame.
	const bool quiet = (argc >= 5) && (0 == _stricmp(args->ArgV(4), "quiet"));

	if (0 == _stricmp(sub, "on") || 0 == _stricmp(sub, "off") || 0 == _stricmp(sub, "toggle")) {
		const bool on = (0 == _stricmp(sub, "toggle")) ? !g_enabled : (0 == _stricmp(sub, "on"));
		MethodActing_SetEnabled(on);
		advancedfx::Message("mirv_filmmaker: method acting %s.\n", on ? "on" : "off");
		return;
	}
    if (!_stricmp(sub,"gaze") || !_stricmp(sub,"deatheyes")) {
        const bool gaze=!_stricmp(sub,"gaze");
        if(argc>=4) { if(gaze) MethodActing_SetGazeEnabled(_stricmp(val,"off")!=0);
                     else MethodActing_SetDeathEyesEnabled(_stricmp(val,"off")!=0); }
        advancedfx::Message("mirv_filmmaker methodacting %s = %s\n",sub,(gaze?g_gazeEnabled:g_deathEyes)?"on":"off");return;
    }
	if (0 == _stricmp(sub, "blink")) {
		if (argc < 4) {
			advancedfx::Message("mirv_filmmaker methodacting blink = %s\n", g_blinkEnabled ? "on" : "off");
			return;
		}
		MethodActing_SetBlinkEnabled(0 != _stricmp(val, "off"));
		advancedfx::Message("mirv_filmmaker: idle blinking %s.\n", g_blinkEnabled ? "on" : "off");
		return;
	}
	if (0 == _stricmp(sub, "squint")) {
		if (argc >= 4) MethodActing_SetSquintLevel(atof(val));
		if (!quiet) advancedfx::Message("mirv_filmmaker methodacting squint = %.2f\n", g_squint.level);
		return;
	}
	if (0 == _stricmp(sub, "hold")) {
		if (argc >= 4) MethodActing_SetHoldMs(atof(val));
		if (!quiet) advancedfx::Message("mirv_filmmaker methodacting hold = %.0f ms\n", MethodActing_HoldMs());
		return;
	}
	if (0 == _stricmp(sub, "attack")) {
		if (argc >= 4) MethodActing_SetAttackMs(atof(val));
		if (!quiet) advancedfx::Message("mirv_filmmaker methodacting attack = %.0f ms\n", MethodActing_AttackMs());
		return;
	}
	if (0 == _stricmp(sub, "release")) {
		if (argc >= 4) MethodActing_SetReleaseMs(atof(val));
		if (!quiet) advancedfx::Message("mirv_filmmaker methodacting release = %.0f ms\n", MethodActing_ReleaseMs());
		return;
	}
	if (0 == _stricmp(sub, "blinkrate")) {
		if (argc >= 4) MethodActing_SetBlinkIntervalSec(atof(val));
		if (!quiet) advancedfx::Message("mirv_filmmaker methodacting blinkrate = %.2f s\n", g_blink.intervalSec);
		return;
	}
	if (0 == _stricmp(sub, "doubleblink")) {
		if (argc >= 4) MethodActing_SetDoubleBlinkChance(atof(val));
		if (!quiet) advancedfx::Message("mirv_filmmaker methodacting doubleblink = %.2f\n",
			g_blink.doubleChance);
		return;
	}
	if (0 == _stricmp(sub, "settle")) {
		if (argc >= 4) MethodActing_SetSettleEyes(0 != _stricmp(val, "off"));
		if (!quiet) advancedfx::Message("mirv_filmmaker: eye settle while firing %s.\n",
			g_settleEyes ? "on" : "off");
		return;
	}
	if (0 == _stricmp(sub, "index")) {
		// Diagnostic escape hatch for a CS2 update that renames the flex controller.
		if (argc >= 4)
			MethodActingFlex::SetIndexOverride(0 == _stricmp(val, "auto") ? -1 : atoi(val));
		const int ov = MethodActingFlex::IndexOverride();
		advancedfx::Message("mirv_filmmaker methodacting index = %s\n",
			ov < 0 ? "auto (resolved by controller name)" : std::to_string(ov).c_str());
		return;
	}
	if (0 == _stricmp(sub, "reset")) {
		MethodActing_ResetAll();
		advancedfx::Message("mirv_filmmaker: method acting reset to defaults (off).\n");
		return;
	}

	advancedfx::Message("  gaze %s death eyes %s bone writes=%llu faults=%llu\n", g_gazeEnabled?"on":"off", g_deathEyes?"on":"off",MethodActingGaze::Writes(),MethodActingGaze::Faults());
	MethodActingFlex::Resolve();
	const MethodActingFlex::Status& fs = MethodActingFlex::GetStatus();
	advancedfx::Message(
		"mirv_filmmaker methodacting - subtle eye reactions for offline demos.\n"
		"  enabled   %s\n"
		"  blink     %s (every ~%.2f s, %.0f%% double)\n"
		"  squint    %.2f  attack %.0f ms  hold %.0f ms  release %.0f ms  settle %s\n"
		"  flex      resolved=%d controllers=%d blink=%d lookUp=%d lookDown=%d names=[%s]\n"
		"            lookups=%d (cached per model)\n"
		"  activity  frames=%llu writes=%llu shots=%llu seeks=%llu faults=%llu\n"
		"Usage:\n"
		"  %s methodacting on|off|toggle\n"
		"  %s methodacting blink on|off\n"
		"  %s methodacting squint <0..1>       - how far the lids close while firing\n"
		"  %s methodacting hold <ms>           - buffer after the last round before relaxing\n"
		"  %s methodacting attack|release <ms> - how fast the lids move\n"
		"  %s methodacting blinkrate <seconds> - mean gap between idle blinks\n"
		"  %s methodacting doubleblink <0..1>  - chance a blink is a quick pair\n"
		"  %s methodacting settle on|off       - hold the look controllers neutral while firing\n"
		"  %s methodacting index <n|auto>      - force the flex controller index (diagnostic)\n"
		"  %s methodacting reset\n"
		"  methodacting gaze on|off         - idle glances and nearby-player looks\n"
        "  methodacting deatheyes on|off    - small upward roll and relaxed lids on death\n",
		g_enabled ? "on" : "off",
		g_blinkEnabled ? "on" : "off", g_blink.intervalSec, g_blink.doubleChance * 100.0,
		g_squint.level, MethodActing_AttackMs(), MethodActing_HoldMs(), MethodActing_ReleaseMs(),
		g_settleEyes ? "on" : "off",
		fs.resolved ? 1 : 0, fs.lastCount, fs.lastEyes.blink, fs.lastEyes.lookUp,
		fs.lastEyes.lookDown, fs.lastNames,
		fs.lookups,
		g_framesRun, (unsigned long long)fs.writes, g_shotsSeen, g_seeks,
		(unsigned long long)fs.faults,
		cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker
