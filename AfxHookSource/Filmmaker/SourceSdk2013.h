#pragma once

// The parts of the Source SDK 2013 multiplayer ABI the filmmaker reads: the network tables, the
// client class list, player info, and the virtual slots it calls. 64-bit CS:S (2025 Steam build)
// shares this engine branch with 64-bit TF2, HL2DM and DoD:S. Declared here rather than taken from
// the full SDK so the adapter depends on exactly these layouts and nothing else.
//
// The structs are laid out as in the SDK headers (public/dt_recv.h, public/client_class.h,
// public/cdll_int.h), so the compiler gives them the same 32- or 64-bit layout the game uses.
// Engine-free: tests include this header directly.

namespace SourceFilmmaker {
namespace Sdk2013 {

// public/dt_common.h
enum SendPropType {
	DPT_Int = 0,
	DPT_Float,
	DPT_Vector,
	DPT_VectorXY,
	DPT_String,
	DPT_Array,
	DPT_DataTable,
	DPT_Int64,
	DPT_NUMSendPropTypes
};

struct RecvTable;

// public/dt_recv.h
struct RecvProp {
	const char* m_pVarName;
	SendPropType m_RecvType;
	int m_Flags;
	int m_StringBufferSize;
	bool m_bInsideArray;
	const void* m_pExtraData;
	RecvProp* m_pArrayProp;
	void* m_ArrayLengthProxy;
	void* m_ProxyFn;
	void* m_DataTableProxyFn;
	RecvTable* m_pDataTable;
	int m_Offset;
	int m_ElementStride;
	int m_nElements;
	const char* m_pParentArrayPropName;
};

struct RecvTable {
	RecvProp* m_pProps;
	int m_nProps;
	void* m_pDecoder;
	const char* m_pNetTableName;
	bool m_bInitialized;
	bool m_bInMainList;
};

// public/client_class.h
struct ClientClass {
	void* m_pCreateFn;
	void* m_pCreateEventFn;
	const char* m_pNetworkName;
	RecvTable* m_pRecvTable;
	ClientClass* m_pNext;
	int m_ClassID;
};

// public/cdll_int.h, player_info_s. Only the leading fields are read. Games built with
// REPLAY_ENABLED insert a bool after ishltv, so callers pass a buffer of kPlayerInfoBufferSize.
struct PlayerInfoPrefix {
	char name[32];
	int userID;
	char guid[33];
	unsigned int friendsID;
	char friendsName[32];
	bool fakeplayer;
	bool ishltv;
};
const int kPlayerInfoBufferSize = 512;

// public/basehandle.h: an entity handle's low NUM_ENT_ENTRY_BITS bits are the entity index.
const unsigned int kEntityHandleIndexMask = 0xFFF;
const unsigned int kInvalidEntityHandle = 0xFFFFFFFF;

// public/const.h, m_lifeState and m_fFlags.
const int kLifeAlive = 0;
const int kFlagOnGround = 1 << 0;
const int kFlagDucking = 1 << 1;

// Virtual slots, counted from each interface's first virtual function.
namespace Slot {
// IBaseClientDLL (VClient017): Init, PostInit, Shutdown, ReplayInit, ReplayPostInit,
// LevelInitPreEntity, LevelInitPostEntity, LevelShutdown, GetAllClasses.
const int Client_GetAllClasses = 8;
// IVEngineClient (VEngineClient013): ... ServerCmd 6, ClientCmd 7, GetPlayerInfo 8.
const int EngineClient_GetPlayerInfo = 8;
// ... GetGamestatsData 126, ServerCmdKeyValues 127, IsSkippingPlayback 128 (the 2013 SDK's list;
// CS:S's client.dll calls it as [vtable + 0x400], measured 2026-10-27).
const int EngineClient_IsSkippingPlayback = 128;
// IClientEntityList (VClientEntityList003).
const int EntityList_GetClientEntity = 3;
const int EntityList_GetHighestEntityIndex = 6;
// IHandleEntity: the destructor, SetRefEHandle, GetRefEHandle (which returns a reference: the
// address of the entity's CBaseHandle).
const int Handle_GetRefEHandle = 2;
// IClientUnknown, after IHandleEntity's destructor, SetRefEHandle and GetRefEHandle.
const int Unknown_GetClientNetworkable = 4;
const int Unknown_GetClientRenderable = 5;
// IClientRenderable, measured in the x64 client.dll's C_BaseAnimating (2026-10-26): GetModel (9,
// returns m_pModel) and SetupBones (16) sit where the 2013 SDK puts them; the x64 game has three
// slots more before LookupAttachment (35, -1 without a model), GetAttachment with a matrix (36) and
// GetAttachment with an origin and angles (37, which falls back to the entity's own and returns false).
const int Renderable_GetModel = 9;
const int Renderable_SetupBones = 16;
const int Renderable_LookupAttachment = 35;
const int Renderable_GetAttachment = 37;
// IVModelInfoClient (VModelInfoClient006), measured in the x64 engine.dll's CModelInfoClient: 28 is
// GetStudiomodel (a studio model's MDLCache GetStudioHdr).
const int ModelInfo_GetStudiomodel = 28;
// IClientEntity, after IClientUnknown's nine slots. Its Release overrides the Release of its
// secondary bases (IClientNetworkable, IClientThinkable), so MSVC gives it no slot of its own in
// the primary table: GetAbsOrigin is 9 and GetAbsAngles 10. Measured in CS:S on 2026-10-26, when
// slot 10 returned the local player's view angles.
const int Entity_GetAbsOrigin = 9;
const int Entity_GetAbsAngles = 10;
// IClientNetworkable.
const int Networkable_GetClientClass = 2;
const int Networkable_IsDormant = 8;
// IEngineVGui (VEngineVGui001): the destructor, GetPanel, IsGameUIVisible.
const int EngineVGui_IsGameUIVisible = 2;
} // namespace Slot

// studiohdr_t, as the MDL file holds it (CS:S's models are version 44): the model's name, the bone
// count and the offset of the bone array, each mstudiobone_t 216 bytes with its name's offset (from
// the bone) first. SetupBones' mask for every bone, and the most bones a model has.
const int kStudioName = 12;
const int kStudioNameSize = 64;
const int kStudioNumBones = 156;
const int kStudioBoneIndex = 160;
const int kStudioBoneSize = 216;
const int kBoneUsedByAnything = 0x0007FF00;
const int kMaxStudioBones = 128;

// client/hltvcamera.h's C_HLTVCamera, SourceTV playback's camera, whose mode and target are not
// networked: CGameEventListener's vtable and flag, then m_nCameraMode, m_iCameraMan, m_vCamOrigin,
// m_aCamAngle, m_iTraget1. Measured in CS:S's x64 client.dll on 2026-10-26: SetMode writes +0x10 and
// fires hltv_changed_mode with "obs_target" read from +0x30.
const int kHltvCameraMode = 0x10;
const int kHltvCameraMan = 0x14;
const int kHltvCameraTarget = 0x30;
const int kObserverModeLast = 7; // OBS_MODE_ROAMING in this SDK, which added OBS_MODE_POI at 6

} // namespace Sdk2013
} // namespace SourceFilmmaker
