#pragma once
#include <cstring>
namespace SourceFilmmaker { namespace RenderPassMath {
enum Subject { Player = 1, Weapon = 2, Held = 4, Viewmodel = 8 };
// Network classes identify subjects; unknown classes stay in the world. The adapter adds Held
// after resolving ownership handles, so player metadata and thrown grenades remain distinct.
inline unsigned SubjectClass(const char* name) {
    if (!name) return 0;
    if (0 == std::strcmp(name,"CCSPlayer") || 0 == std::strcmp(name,"CCSRagdoll")) return Player;
    if (std::strstr(name,"ViewModel")) return Viewmodel;
    if (0 == std::strncmp(name,"CWeapon",7)
        || 0 == std::strcmp(name,"CHEGrenade") || 0 == std::strcmp(name,"CFlashbang")
        || 0 == std::strcmp(name,"CSmokeGrenade")) return Weapon;
    return 0;
}
} }
