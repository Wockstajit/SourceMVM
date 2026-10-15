#define NOMINMAX
#include "ClientEntitySpawn.h"
#include "../../SceneSystem.h" // g_pCResourceSystem: blocking model precache
#include "../Debug/MvmDebugLog.h"
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "../../Globals.h"
#include "../../SchemaSystem.h"
#include <string>
bool getOffset(std::ptrdiff_t* offset, std::string moduleName, std::string className, std::string fieldName);
namespace Filmmaker::Props {
namespace {
unsigned char* client=nullptr;
struct Field { int64_t hash;const char* name; };
uint32_t Token(const char* name){
    auto data=reinterpret_cast<const unsigned char*>(name);size_t count=strlen(name);
    uint32_t h=0x31415926u^uint32_t(count),m=0x5bd1e995u;
    while(count>=4){uint32_t k;memcpy(&k,data,4);k*=m;k^=k>>24;k*=m;h=h*m^k;data+=4;count-=4;}
    if(count==3)h^=data[2]<<16;if(count>=2)h^=data[1]<<8;if(count>=1){h^=data[0];h*=m;}
    h^=h>>13;h*=m;h^=h>>15;return h;
}
Field Key(const char* name){return {int64_t(int32_t(Token(name))),name};}
// CLIENT-SIDE ENTITY SPAWN, the same sequence the engine's own CEntitySpawner<T>::Spawn runs.
// Everything is located by signature: the addresses were hard-coded to one client.dll build and
// the first CS2 update after they were traced silently disabled demo puddles.
struct Api {
    void (*allocKeyValues)(uintptr_t *spawner) = nullptr;
    void *(*findMember)(void *kv, Field *field) = nullptr;
    void (*setString)(void *kv, void *node, const char *value) = nullptr;
    void (*setVector)(void *kv, Field *field, const Vec3 *value) = nullptr;
    void (*setTyped)(void *node, int type, const void *data, uint8_t subtype) = nullptr;
    void *(*findClass)(void *system, const char *classname, void *fallback) = nullptr;
    unsigned char *(*create)(void *system, unsigned group, void *classInfo, const char *name, int, int, unsigned, uint8_t) = nullptr;
    void (*spawn)(void *system, void *entity, void *kv) = nullptr;
    void (*finish)(void *system) = nullptr;
    void (*destroyKeyValues)(void *kv) = nullptr;
    void (*free)(void *block, size_t size) = nullptr;
    unsigned char **system = nullptr; // CGameEntitySystem*
    // CEntitySpawner<T>::Spawn fills its spawner from these (traced 2026-10-09 in
    // CEntitySpawner<C_CSGO_PreviewModel>::Spawn). The offset and the symbol table used to be
    // hard-coded (system+3272 and nothing at all): a stale offset and an empty symbol table make
    // the key-value pool fail with "CUtlSymbolTableLarge element limit of 0 exceeded".
    uint32_t kvContextOffset = 0;      // `lea r14, [rcx+X]`: spawner[3] = system + X
    uintptr_t *kvSymbols = nullptr;    // global whose VALUE is spawner[4]
    unsigned (*currentGroup)(void *system) = nullptr;
    const void *(*currentContext)(void *system) = nullptr;
    void (*remove)(void *entity) = nullptr; // UTIL_Remove (client): refuses networked entities
    void ***spawnApi = nullptr;       // interface whose slot 35 initialises the spawned entity
};
Api api;
bool resolved = false, available = false;
size_t Scan(HMODULE module, const char *pattern) { return getAddress(module, pattern); }
uintptr_t Rel(size_t match, int at) { return match ? match + at + 4 + *reinterpret_cast<int32_t *>(match + at) : 0; }
bool Resolve() {
    if (resolved)
        return available;
    resolved = true;
    HMODULE module = GetModuleHandleW(L"client.dll");
    if (!module)
        return false;
    client = reinterpret_cast<unsigned char *>(module);
    auto direct = [&](const char *pattern) { return static_cast<uintptr_t>(Scan(module, pattern)); };
    api.allocKeyValues = reinterpret_cast<decltype(api.allocKeyValues)>(
        Rel(Scan(module, "44 89 7D C7 48 8B 10 48 8D 05 ?? ?? ?? ?? 48 3B D0 75 07 E8"), 20));
    api.findMember = reinterpret_cast<decltype(api.findMember)>(direct("48 83 EC 38 8B 02 45 33 C0 89 44 24 20 8B 42 04"));
    api.setString = reinterpret_cast<decltype(api.setString)>(direct(
        "48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 20 48 83 39 00 49 8B F0 48 8B EA 48 8B D9 75 ?? 0F B6 41 25"));
    api.setVector = reinterpret_cast<decltype(api.setVector)>(direct(
        "40 53 48 83 EC 40 8B 02 49 8B D8 89 44 24 30 8B 42 04 89 44 24 34 48 8B 42 08 48 8D 54 24 30 48 89 44 24 38 "
        "E8 ?? ?? ?? ?? 48 85 C0 74 ?? F3 0F 10 03 4C 8D 44 24 20 F3 0F 10 4B 04 41 B1 1D"));
    api.setTyped = reinterpret_cast<decltype(api.setTyped)>(direct("48 83 EC 58 C6 44 24 40 18 C6 44 24 38 05 44 88 4C 24 30"));
    api.findClass = reinterpret_cast<decltype(api.findClass)>(direct(
        "48 89 5C 24 08 48 89 74 24 18 48 89 54 24 10 57 48 83 EC 20 49 8B D8 48 8B F9 48 85 D2 0F 84 ?? ?? ?? ?? "
        "80 3A 00 0F 84 ?? ?? ?? ?? 48 81 C1 B0 0A 00 00"));
    api.create = reinterpret_cast<decltype(api.create)>(direct(
        "48 89 5C 24 08 48 89 6C 24 10 56 57 41 56 48 83 EC 40 4D 8B F1 49 8B D8 8B EA 48 8B F1 4D 85 C0 75"));
    // Build 14182 moved CGameEntitySystem's finish counter from +0xBC0 to +0xC48.
    api.finish = reinterpret_cast<decltype(api.finish)>(direct("48 89 5C 24 08 57 48 81 EC 00 01 00 00 FF 81 48 0C 00 00"));
    // Release site: `dec word [kv+20h]; jg; call KvDestruct; mov edx,38h; call Free`.
    size_t release = Scan(module, "66 FF 4B 20 66 83 7B 20 00 7F ?? 48 8B CB E8 ?? ?? ?? ?? BA 38 00 00 00 48 8B CB E8 ?? ?? ?? ?? "
                                  "48 83 7D 98 00 74 ?? 48 8D 4D 98 FF 15 ?? ?? ?? ?? 49 8B C4 48 81 C4 50 01 00 00");
    api.destroyKeyValues = reinterpret_cast<decltype(api.destroyKeyValues)>(Rel(release, 15));
    api.free = reinterpret_cast<decltype(api.free)>(Rel(release, 28));
    // Inside the engine's spawn routine: the init interface, the Spawn thunk and the entity system.
    size_t spawner = Scan(module,
        "41 BE FF FF FF FF 48 8B 0D ?? ?? ?? ?? 48 8D 55 B0 44 89 6C 24 60 4C 8D 4C 24 50 C7 44 24 64 FF FF 01 00 "
        "41 B8 01 00 00 00 49 8B 47 10 48 89 44 24 50 48 8B 45 80 48 89 44 24 58 48 8B 01 48 89 54 24 20 8B 55 A4 "
        "FF 90 18 01 00 00 49 8B 47 10 8B 48 30 C1 E9 09 F6 C1 01 0F 85 ?? ?? ?? ?? 4C 8B 45 80 49 8B D7 48 8B 0D "
        "?? ?? ?? ?? E8 ?? ?? ?? ?? 48 39 75 00 75 ?? 48 8B 0D ?? ?? ?? ?? E8");
    api.spawnApi = reinterpret_cast<void ***>(Rel(spawner, 9));
    api.spawn = reinterpret_cast<decltype(api.spawn)>(Rel(spawner, 110));
    api.system = reinterpret_cast<unsigned char **>(Rel(spawner, 105));
    if (size_t at = Scan(module, "48 89 75 98 4C 8D B1 ?? ?? ?? ?? 40 88 75 E1 48 89 75 E8 48 89 44 24 70"))
        api.kvContextOffset = *reinterpret_cast<uint32_t *>(at + 7);
    if (size_t at = Scan(module, "48 8B 0D ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 0F 29 45 D0 48 89 75 80 4C 89 75 88 48 89 45 90"))
        api.kvSymbols = reinterpret_cast<uintptr_t *>(Rel(at, 10));
    if (size_t at = Scan(module, "83 FB FF 75 ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 89 45 A4 E8 ?? ?? ?? ?? 0F 28 00 0F 29 45 B0")) {
        api.currentGroup = reinterpret_cast<decltype(api.currentGroup)>(Rel(at, 6));
        api.currentContext = reinterpret_cast<decltype(api.currentContext)>(Rel(at, 21));
    }
    api.remove = reinterpret_cast<decltype(api.remove)>(direct(
        "48 83 EC 28 48 85 C9 0F 84 ?? ?? ?? ?? 48 8B 51 10 8B 42 30 0F BA E0 0B 73 07 C1 E8 06 24 01 EB"));
    available = api.remove && api.kvContextOffset && api.kvSymbols && api.currentGroup && api.currentContext &&
                api.allocKeyValues && api.findMember && api.setString && api.setVector && api.setTyped &&
                api.findClass && api.create && api.spawn && api.finish && api.destroyKeyValues && api.free &&
                api.system && api.spawnApi;
    MvmDebugLog_LinefAlways("props.spawn", "resolved=%d alloc=%p member=%p string=%p vector=%p typed=%p "
                            "class=%p create=%p spawn=%p finish=%p destroy=%p free=%p system=%p api=%p",
                            int(available), api.allocKeyValues, api.findMember, api.setString, api.setVector,
                            api.setTyped, api.findClass, api.create, api.spawn, api.finish, api.destroyKeyValues,
                            api.free, api.system, api.spawnApi);
    return available;
}
void SetString(void* kv,const char* name,const char* value){
    auto field=Key(name);auto node=api.findMember(kv,&field);
    if(node)api.setString(kv,node,value);
}
unsigned char* Spawn(const char* classname,const char* name,const char* model,Vec3 position,Vec3 angles,float scale,const char* const* extra){
    auto system=*api.system;
    auto init=*api.spawnApi;
    if(!system||!init)return nullptr;
    // The engine passes a zeroed slot here; it is read when the class is not registered.
    void* fallback=nullptr;
    void* classInfo=api.findClass(system,classname,&fallback);
    if(!classInfo)return nullptr;
    // Match CEntitySpawner: a pooled KV block, the current spawn group/context,
    // class initialization, dispatch spawn, then release our KV reference.
    unsigned group=api.currentGroup(system);
    alignas(16) unsigned char context[48]{};
    memcpy(context,api.currentContext(system),sizeof(context));
    // The spawner exactly as the engine fills it: [2] kv (out), [3] the entity system's key-value
    // context, [4] the key symbol table, then the spawn group and its 48-byte context.
    alignas(16) uintptr_t spawner[24]{};
    spawner[3]=reinterpret_cast<uintptr_t>(system+api.kvContextOffset);
    spawner[4]=*api.kvSymbols;
    reinterpret_cast<unsigned*>(&spawner[6])[1]=group;
    memcpy(&spawner[8],context,sizeof(context));
    api.allocKeyValues(spawner);
    auto kv=reinterpret_cast<unsigned char*>(spawner[2]);if(!kv)return nullptr;
    SetString(kv,"classname",classname);SetString(kv,"targetname",name);
    if(model&&*model)SetString(kv,"model",model);
    auto origin=Key("origin");api.setVector(kv,&origin,&position);
    auto anglesKey=Key("angles");auto node=api.findMember(kv,&anglesKey);
    if(node)api.setTyped(node,3,&angles,34);
    // Keyvalues are parsed from text by the entity's own spawn, exactly as from a map.
    if(scale>0&&scale!=1){char text[32];snprintf(text,sizeof(text),"%.4f",scale);SetString(kv,"modelscale",text);}
    for(int i=0;extra&&extra[i]&&extra[i+1];i+=2)SetString(kv,extra[i],extra[i+1]);
    auto entity=api.create(system,group,classInfo,"",2,-1,0xffffffff,0);
    bool success=false;
    if(entity){
        struct SpawnData {void* identity;void* kv;int index;unsigned flags;};
        SpawnData data{*reinterpret_cast<void**>(entity+16),kv,-1,0x1ffff};
        auto initialise=reinterpret_cast<void(*)(void*,unsigned,int,SpawnData*,const void*)>((*reinterpret_cast<void***>(init))[35]);
        initialise(init,group,1,&data,context);
        if(((*reinterpret_cast<unsigned*>(static_cast<unsigned char*>(data.identity)+48))&0x200)==0){
            api.spawn(system,entity,kv);
            api.finish(system);success=true;
            // Spawned into a PAUSED demo the entity never interpolates and the renderer skips it;
            // the engine's own override draws it anyway (see Panorama/PreviewRenderFlag.h).
            if(auto at=g_clientDllOffsets.C_BaseEntity.m_bRenderEvenWhenNotSuccessfullyInterpolated)entity[at]=1;
        }
        MvmDebugLog_LinefAlways("props.spawn","class=%s name=%s model=%s entity=%p group=%u origin=(%.2f %.2f %.2f) scale=%.3f spawned=%d",classname,name,model,entity,group,position.x,position.y,position.z,scale,int(success));
    }
    if(--*reinterpret_cast<int16_t*>(kv+32)<=0){api.destroyKeyValues(kv);api.free(kv,56);}
    return success?entity:nullptr;
}
}
bool ClientEntitySpawnAvailable(){__try{return Resolve();}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
bool RemoveClientEntity(unsigned char* entity){
    if(!entity)return false;
    __try{if(!Resolve())return false;api.remove(entity);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
int ClientEntityIndex(unsigned char* entity){
    // CEntityInstance+16 is its CEntityIdentity; identity+16 is the handle, index in the low 15 bits.
    if(!entity)return -1;
    __try{auto identity=*reinterpret_cast<unsigned char**>(entity+16);if(!identity)return -1;
        return int(*reinterpret_cast<uint32_t*>(identity+16)&0x7fff);}
    __except(EXCEPTION_EXECUTE_HANDLER){return -1;}
}
bool PrecacheModel(const char* model){
    if(!model||!*model||!g_pCResourceSystem)return false;
    __try{g_pCResourceSystem->PreCache(model);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
unsigned char* SpawnClientEntity(const char* classname,const char* name,const char* model,Vec3 position,Vec3 angles,float scale,const char* const* extra){
    // A model the level never loaded spawns as the ERROR placeholder (and the resource warning),
    // so it is made resident first: a blocking load here, never a hitch inside the frame.
    PrecacheModel(model);
    __try {return Resolve()?Spawn(classname,name,model,position,angles,scale,extra):nullptr;}
    __except(EXCEPTION_EXECUTE_HANDLER){MvmDebugLog_LinefAlways("props.spawn","spawn fault class=%s model=%s",classname,model);return nullptr;}
}
// Collision bounds (CCollisionProperty::m_vecMins/m_vecMaxs) of a spawned entity, in model space.
// The offsets are resolved by name at startup: the schema name map is cleared once the hook has
// read it, so a lookup made later finds nothing (see SchemaSystem.cpp).
namespace { std::ptrdiff_t g_collision=0, g_minsAt=0, g_maxsAt=0; }
void ClientEntitySpawn_InitSchema(){
    getOffset(&g_collision,"client.dll","C_BaseModelEntity","m_Collision");
    getOffset(&g_minsAt,"client.dll","CCollisionProperty","m_vecMins");
    getOffset(&g_maxsAt,"client.dll","CCollisionProperty","m_vecMaxs");
}
namespace {
bool BoundsOffsets(){ return g_collision&&g_minsAt&&g_maxsAt; }
bool ReadBounds(unsigned char* entity, float* mins, float* maxs){
    __try{
        std::memcpy(mins,entity+g_collision+g_minsAt,12);
        std::memcpy(maxs,entity+g_collision+g_maxsAt,12);
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
}
bool ClientEntityBounds(unsigned char* entity, Vec3& mins, Vec3& maxs){
    float a[3],b[3];
    if(!entity||!BoundsOffsets()||!ReadBounds(entity,a,b))return false;
    mins={a[0],a[1],a[2]}; maxs={b[0],b[1],b[2]};
    return Environment::Finite(mins)&&Environment::Finite(maxs)&&maxs.x>=mins.x&&maxs.y>=mins.y&&maxs.z>=mins.z;
}
}

