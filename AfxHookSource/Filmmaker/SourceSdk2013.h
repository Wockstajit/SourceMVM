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

// public/const.h, m_lifeState.
const int kLifeAlive = 0;

// Virtual slots, counted from each interface's first virtual function.
namespace Slot {
// IBaseClientDLL (VClient017): Init, PostInit, Shutdown, ReplayInit, ReplayPostInit,
// LevelInitPreEntity, LevelInitPostEntity, LevelShutdown, GetAllClasses.
const int Client_GetAllClasses = 8;
// IVEngineClient (VEngineClient013): ... ServerCmd 6, ClientCmd 7, GetPlayerInfo 8.
const int EngineClient_GetPlayerInfo = 8;
// IClientEntityList (VClientEntityList003).
const int EntityList_GetClientEntity = 3;
const int EntityList_GetHighestEntityIndex = 6;
// IClientUnknown, after IHandleEntity's destructor, SetRefEHandle and GetRefEHandle.
const int Unknown_GetClientNetworkable = 4;
// IClientEntity, after IClientUnknown's nine slots. Its Release overrides the Release of its
// secondary bases (IClientNetworkable, IClientThinkable), so MSVC gives it no slot of its own in
// the primary table: GetAbsOrigin is 9 and GetAbsAngles 10. Measured in CS:S on 2026-10-26, when
// slot 10 returned the local player's view angles.
const int Entity_GetAbsOrigin = 9;
const int Entity_GetAbsAngles = 10;
// IClientNetworkable.
const int Networkable_GetClientClass = 2;
const int Networkable_IsDormant = 8;
} // namespace Slot

} // namespace Sdk2013
} // namespace SourceFilmmaker
