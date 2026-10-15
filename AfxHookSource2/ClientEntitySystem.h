#pragma once

#include <cstdint>

#include <Windows.h>
#include "../Deps/release/prop/cs2/sdk_src/public/entityhandle.h"

bool Hook_ClientEntitySystem( void* pEntityList, void * pFnGetHighestEntityIterator, void * pFnGetEntityFromIndex );

bool Hook_ClientEntitySystem2();

void Hook_ClientEntitySystem3(HMODULE clientDll);

bool Hook_GetSplitScreenPlayer( void* pAddr);

class CAfxEntityInstanceRef;

class CEntityInstance {
public:
    const char * GetName();

    const char * GetDebugName();

    const char * GetClassName();

    const char * GetClientClassName();

    bool IsPlayerPawn();

    SOURCESDK::CS2::CBaseHandle GetPlayerPawnHandle();

    bool IsPlayerController();

    SOURCESDK::CS2::CBaseHandle GetPlayerControllerHandle();

    unsigned int GetHealth();

    int GetTeam();
	
    /**
     * @remarks FLOAT_MAX if invalid
     */
    void GetOrigin(float & x, float & y, float & z);

    void GetRenderEyeOrigin(float outOrigin[3]);

    void GetRenderEyeAngles(float outAngles[3]);

    SOURCESDK::CS2::CBaseHandle GetViewEntityHandle();

    SOURCESDK::CS2::CBaseHandle GetActiveWeaponHandle();

    const char * GetPlayerName();

    uint64_t GetSteamId();

    const char * GetSanitizedPlayerName();

    uint8_t GetObserverMode();
    SOURCESDK::CS2::CBaseHandle GetObserverTarget();

    SOURCESDK::CS2::CBaseHandle GetHandle();

    SOURCESDK::CS2::CBaseHandle GetOwnerEntityHandle();

    float GetGrenadeTrajectoryCreationTime();

    uint8_t LookupAttachment(const char* attachmentName);
	bool GetAttachment(uint8_t idx, SOURCESDK::Vector &origin, SOURCESDK::Quaternion &angles);
};

// Local viewer's observer mode (OBS_MODE_*: 1=fixed, 2=in-eye/first person, 3=chase/third
// person, 4=roaming/freecam; 0=none). outTargetIndex receives the spectated entity index or -1.
uint8_t AfxGetLocalObserverState(int * outTargetIndex);

// Entity index of the player PAWN currently being viewed during demo playback, or -1 if none.
// Tries the engine observer-services target first, then falls back to matching a player pawn's
// render eye against the active game camera -- robust in POV/GOTV demos where the observer-
// services fields read 0/-1 even while a player is plainly being spectated. First-person only.
int AfxGetSpectatedPawnIndex();

// AfxGetSpectatedPawnIndex with the same short hold as AfxGetSpectatedUserId: the render-eye match
// drops out for single frames (recoil punch, eye lerp, seek frames), and a UI that follows it
// directly flips its target every frame. A good result is kept for up to a second while that
// entity is still a player pawn; leaving first person still reads -1 within the hold.
int AfxGetSpectatedPawnIndexHeld();

// Local demo viewer's pawn (split-screen player 0 controller -> pawn). Used for first-person
// viewmodel mirror when the spectated remote pawn has no m_hHudModelArms (demo spectate).
CEntityInstance* AfxGetLocalViewerPawn();

// CS2 game-event `userid` for the player pawn currently being spectated, or -1 if unknown.
// userid == player-controller entity index - 1 (the DeathMsg.cpp convention). Built on
// AfxGetSpectatedPawnIndex above, with the same AfxGetLocalViewerPawn fallback for
// locally-recorded demos (where there is no observer target and the "watched" player is the
// local viewer). Read-only helper -- used by anything that has to decide whether a game event
// belongs to the player currently on screen.
int AfxGetSpectatedUserId();

typedef int (__fastcall * GetHighestEntityIndex_t)(void * pEntityList, bool bUnknown);
typedef void * (__fastcall * GetEntityFromIndex_t)(void * pEntityList, int index);

extern GetHighestEntityIndex_t  g_GetHighestEntityIndex;
extern GetEntityFromIndex_t g_GetEntityFromIndex;

extern void ** g_pEntityList;

int GetHighestEntityIndex();
