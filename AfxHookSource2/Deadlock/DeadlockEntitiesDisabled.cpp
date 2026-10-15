#include "DeadlockEntities.h"
#include "../../Shared/AfxConsole.h"

// Entity integration is temporarily removed from the Deadlock build while demo FPS is tested.
// Keep the camera/command API linkable without scanning or reading any engine entity memory.
namespace Deadlock {
bool Entities_InitClient(HMODULE) { return false; }
bool Entities_Ready() { return false; }
int Entities_HighestIndex() { return -1; }
void* Entities_Get(int) { return nullptr; }
std::string Entities_ClassName(void*) { return {}; }
bool Entities_Origin(void*, double[3]) { return false; }
bool Entities_Angles(void*, double[3]) { return false; }
bool Entities_EyeOrigin(void*, double[3]) { return false; }
int Entities_Health(void*) { return -1; }
void Entities_Heroes(std::vector<HeroEntry>& out) { out.clear(); }
void Entities_HeroesCached(std::vector<HeroEntry>& out) { out.clear(); }
int Entities_NearestHero(const double[3]) { return -1; }
void Entities_AppendHeroesJson(std::string& out) { out += "[]"; }
int Entities_HeroCount() { return 0; }
std::string Entities_HeroCodename(void*) { return {}; }
std::string Entities_PlayerNameFor(void*) { return {}; }
void Entities_ListHeroes(int) { Entities_Report(); }
void Entities_Report() {
	advancedfx::Message("  entities: disabled for demo FPS testing (follow/attach unavailable)\n");
}
void Entities_List(const char*, int) { Entities_Report(); }
bool Entities_ClassIs(void*, const char*) { return false; }
void Entities_Probe(int) { Entities_Report(); }
unsigned long long Entities_FaultCount() { return 0; }
}
