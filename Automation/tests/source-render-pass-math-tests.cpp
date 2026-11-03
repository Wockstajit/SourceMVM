#include "../../AfxHookSource/Filmmaker/SourceRenderPassMath.h"
#include "../../AfxHookSource/Filmmaker/SourceRenderPassTable.h"
#include <cstdio>
#include <initializer_list>
using namespace SourceFilmmaker;
using namespace RenderPassMath;
static int failed = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n",__LINE__,#x); ++failed; } } while (0)
int main() {
    CHECK(SubjectClass("CCSPlayer") == Player);
    CHECK(SubjectClass("CCSRagdoll") == Player);
    CHECK(SubjectClass("CPredictedViewModel") == Viewmodel);
    CHECK(SubjectClass("CBaseViewModel") == Viewmodel);
    for (const char* c : {"CWeaponUSP","CWeaponAK47","CWeaponGlock","CHEGrenade","CFlashbang","CSmokeGrenade"})
        CHECK(SubjectClass(c) == Weapon);
    for (const char* c : {"CCSPlayerResource","CCSTeam","CC4","CPlantedC4","CBaseCSGrenadeProjectile","CPhysicsPropMultiplayer","new_client_class",""})
        CHECK(SubjectClass(c) == 0);
    CHECK(SubjectClass(nullptr) == 0);
    CHECK(RoutePass(0,0).way == PassWay::Main);
    CHECK(RoutePass(0,1).extra == ExtraPass::NoGun);
    CHECK(RoutePass(0,2).extra == ExtraPass::World);
    CHECK(RoutePass(1,2).extra == ExtraPass::DepthWorld);
    CHECK(RoutePass(2,0).extra == ExtraPass::Normal);
    CHECK(RoutePass(2,1).extra == ExtraPass::NormalNoGun);
    CHECK(RoutePass(2,2).extra == ExtraPass::NormalWorld);
    CHECK(RoutePass(3,3).extra == ExtraPass::KeyPlayers);
    CHECK(RoutePass(3,4).extra == ExtraPass::KeyGun);
    int available=0,pending=0,absent=0;
    for(int t=0;t<4;++t) for(int g=0;g<5;++g) {
        auto r=RoutePass(t,g);
        if(r.way==PassWay::Main || r.way==PassWay::Extra) ++available;
        else if(r.way==PassWay::Unavailable) { ++pending; CHECK(r.why && *r.why); }
        else ++absent;
    }
    CHECK(available==15 && pending==0 && absent==5);
    CHECK(PassDepthMask(RoutePass(1,3).extra)==2);
    CHECK(PassDepthMask(RoutePass(2,4).extra)==1);
    CHECK(RoutePass(-1,0).way==PassWay::NotAPass);
    CHECK(RoutePass(0,5).way==PassWay::NotAPass);
    CHECK(PassNeedsDepth(ExtraPass::NormalWorld));
    CHECK(!PassNeedsDepth(ExtraPass::KeyPlayers));
    if(failed) return 1;
    std::puts("SourceRenderPassMathTests passed");
}
