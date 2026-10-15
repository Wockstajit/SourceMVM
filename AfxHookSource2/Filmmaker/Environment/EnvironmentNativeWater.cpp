#define NOMINMAX
#include "EnvironmentNativeWater.h"
#include "../Debug/MvmDebugLog.h"
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include <Windows.h>
#include <map>
#include <string>
extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;
bool getOffset(std::ptrdiff_t*,std::string,std::string,std::string);
namespace Filmmaker::Environment {
namespace {
struct Instance {std::string signature;bool spawned=false;ULONGLONG requestedAt=0;bool clientOnly=false;};
std::map<int,Instance> instances;
std::ptrdiff_t waterLevel=0;
void Command(const std::string& s){if(g_pEngineToClient)g_pEngineToClient->ExecuteClientCmd(0,s.c_str(),true);}
std::string Name(int id){return "mvm_native_water_"+std::to_string(id);}
void Remove(int id){auto name=Name(id);auto it=instances.find(id);
    if(it!=instances.end()&&it->second.clientOnly)Command("cl_ent_remove "+name+";cl_ent_remove "+name+"_volume");
    else Command("ent_fire "+name+" Kill;ent_fire "+name+"_volume Kill");
}
} // namespace
void NativeWater_InitSchema(){getOffset(&waterLevel,"client.dll","C_BaseEntity","m_flWaterLevel");}
void ResetNativeWater(){
    for(const auto& i:instances)if(i.second.spawned)Remove(i.first);
    instances.clear();
}
void UpdateNativeWater(const Scene&,const std::string&){
    // Coverage puddles use the environment shader film. Native CS2 water meshes and the
    // wet-ground geometry job were removed: regenerating them on every puddle/wetness edit
    // stalled the session. Tear down any leftover entities from older builds.
    if(!instances.empty()){
        ResetNativeWater();
        MvmDebugLog_LinefAlways("environment.nativeWater","cleared leftover native water entities");
    }
}
}
