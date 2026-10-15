#include "stdafx.h"

#include "GameEvents.h"

#include "../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../Deps/release/prop/cs2/sdk_src/public/igameevents.h"
#include "../Deps/release/prop/cs2/sdk_src/public/tier1/utlstring.h"

#include "../Shared/AfxConsole.h"
#include "../Shared/binutils.h"

#include "AfxHookSource2Rs.h"
#include "Filmmaker/Movie/ParticleFx.h"
#include "Filmmaker/Movie/RagdollWeapon.h"
#include "Filmmaker/Movie/DeathAnimations.h"
#include "Filmmaker/Movie/NervousSystem.h"
#include "Filmmaker/Movie/MethodActing.h"
#include "Filmmaker/Movie/CameraShakeStack.h"
#include "Filmmaker/Movie/WeaponBobProbe.h"
#include "Filmmaker/EasterEggs/EasterEggs.h"

#include <Windows.h>
#include "../Deps/release/Detours/src/detours.h"

enum CS2GameEventKeyType
{
	CS2GameEventKeyType_Local = 0,
	CS2GameEventKeyType_CString = 1,
	CS2GameEventKeyType_Float = 2,
	CS2GameEventKeyType_Long = 3,
	CS2GameEventKeyType_Short = 4,
	CS2GameEventKeyType_Byte = 5,
	CS2GameEventKeyType_Bool = 6,
	CS2GameEventKeyType_Uint64 = 7
};


typedef SOURCESDK::CS2::CGameEvent * (*CGameEventManager_CreateEvent_t)( void * This, const char *name, bool bForce /*= false*/, int *pCookie /*= NULL*/ ); //:006
typedef bool (*CGameEventManager_FireEvent_t)( void * This, SOURCESDK::CS2::CGameEvent *event, bool bDontBroadcast /*= false*/ ); //:007
typedef bool (*CGameEventManager_FireEventClientSide_t)( void * This, SOURCESDK::CS2::CGameEvent *event ); //:008
typedef void (*CGameEventManager_FreeEvent_t)( void * This, SOURCESDK::CS2::CGameEvent *event ); //:010

void * g_pGameEventManager = nullptr;

CGameEventManager_CreateEvent_t g_CGameEventManager_CreateEvent = nullptr;
CGameEventManager_FreeEvent_t g_CGameEventManager_FreeEvent = nullptr;

CGameEventManager_FireEvent_t g_Old_CGameEventManager_FireEvent = nullptr;
CGameEventManager_FireEventClientSide_t g_Old_CGameEventManager_FireEventClientSide = nullptr;

//extern const char * GetStringForSymbol(int value);

//typedef void ( * DebugPrintKV3_t)(const struct KeyValues3 *);
typedef bool ( * SaveKV3AsJSON_t)( const struct SOURCESDK::CS2::KeyValues3* kv, SOURCESDK::CS2::CUtlString* error, SOURCESDK::CS2::CUtlString* output );

//DebugPrintKV3_t g_DebugPrintKV3 = nullptr;
SaveKV3AsJSON_t g_SaveKV3AsJSON = nullptr;


void SendGameEvent(SOURCESDK::CS2::CGameEvent *event) {

    static bool firstRun = true;
    if(firstRun) {
        firstRun = false;
		HMODULE hModule = GetModuleHandleA("tier0.dll");
		if (hModule)
		{
            //g_DebugPrintKV3 = (DebugPrintKV3_t)GetProcAddress(hModule,"?DebugPrintKV3@@YAXPEBVKeyValues3@@@Z");
			g_SaveKV3AsJSON = (SaveKV3AsJSON_t)GetProcAddress(hModule, "?SaveKV3AsJSON@@YA_NPEBVKeyValues3@@PEAVCUtlString@@1@Z");
		}        
    }

    SOURCESDK::CS2::CUtlString error;
    SOURCESDK::CS2::CUtlString output;

    if(g_SaveKV3AsJSON(event->GetDataKeys(),&error,&output) && nullptr != output) AfxHookSourceRs_Engine_OnGameEvent(event->GetName(), event->GetID(), output.Get());
    else advancedfx::Warning("Event: \"%s\" (%i): SaveKV3AsJSON failed: \"%s\"\n", event->GetName(), event->GetID(),error.Get() ? error.Get() : "[nullptr]");
}

bool New_CGameEventManager_FireEvent( void * This, SOURCESDK::CS2::CGameEvent *event, bool bDontBroadcast /*= false*/ ) {
    g_pGameEventManager = This;

    //advancedfx::Message("Server Event: %s\n", event->GetName());
    Filmmaker::ParticleFx_OnGameEvent(event);
    Filmmaker::RagdollWeapon_OnGameEvent(event);   // keep the victim's gun + optional visual discharge
    Filmmaker::DeathAnimations_OnGameEvent(event); // player_hurt hit location, player_death -> clip
    Filmmaker::NervousSystem_OnGameEvent(event);   // player_hurt hit location, player_death -> reaction
    Filmmaker::MethodActing_OnGameEvent(event);    // firing, incoming shots, injury and player_blind -> eye reactions
    Filmmaker::CameraShake_OnGameEvent(event);     // weapon_fire -> recoil, player_hurt -> step bob, player_footstep -> step bob
    Filmmaker::WeaponBobProbe_OnGameEvent(event);  // Phase 0 research probe: weapon_reload / player_footstep arrival
    Filmmaker::EasterEggs_OnGameEvent(event);      // player_death -> Michael Bay, round_start -> Wes chapter

    return g_Old_CGameEventManager_FireEvent(This, event, bDontBroadcast);
}

extern bool g_b_on_game_event;

bool New_CGameEventManager_FireEventClientSide( void * This, SOURCESDK::CS2::CGameEvent *event ) {
    g_pGameEventManager = This;

    Filmmaker::ParticleFx_OnGameEvent(event);
    Filmmaker::RagdollWeapon_OnGameEvent(event);   // duplicate delivery is deduped by demo time/userid
    Filmmaker::DeathAnimations_OnGameEvent(event); // deduped by userid and demo time
    Filmmaker::NervousSystem_OnGameEvent(event);   // deduped by userid and demo time
    Filmmaker::MethodActing_OnGameEvent(event);
    Filmmaker::CameraShake_OnGameEvent(event);     // weapon_fire/player_hurt/player_footstep — demo events also arrive here (deduped)
    Filmmaker::WeaponBobProbe_OnGameEvent(event);  // Phase 0 research probe: weapon_reload / player_footstep arrival
    Filmmaker::EasterEggs_OnGameEvent(event);      // deduped per userid / per chapter

    if(g_b_on_game_event) SendGameEvent(event);

    return g_Old_CGameEventManager_FireEventClientSide(This, event);
}

bool Hook_CGameEventManager(void* addrClientDll) {
    static bool firstResult = false;
    static bool firstRun = true;

    if(firstRun) {
        firstRun = false;

        if(size_t arddrVtable = Afx::BinUtils::FindClassVtable((HMODULE)addrClientDll,".?AVCGameEventManager@@",0,0)) {
            void ** vtable = (void**)arddrVtable;
            g_CGameEventManager_CreateEvent = (CGameEventManager_CreateEvent_t)vtable[6];
            g_Old_CGameEventManager_FireEvent = (CGameEventManager_FireEvent_t)vtable[7];
            g_Old_CGameEventManager_FireEventClientSide = (CGameEventManager_FireEventClientSide_t)vtable[8];
            g_CGameEventManager_FreeEvent = (CGameEventManager_FreeEvent_t)vtable[10];
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)g_Old_CGameEventManager_FireEvent, New_CGameEventManager_FireEvent);
            DetourAttach(&(PVOID&)g_Old_CGameEventManager_FireEventClientSide, New_CGameEventManager_FireEventClientSide);
            firstResult = NO_ERROR == DetourTransactionCommit();
        }
    }

    return firstResult;
}
