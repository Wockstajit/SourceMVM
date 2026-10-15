#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentPersistenceMath.h"
#include <cstdlib>
#include <iostream>
using namespace Filmmaker::Environment;
void Check(bool ok, const char* why) { if (!ok) { std::cerr << why << '\n'; std::exit(1); } }
int main() {
    SceneRestoreGate gate;
    Check(!gate.Ready(false, 100, 1000), "loading frames do not arm restoration");
    Check(!gate.Ready(true, 100, 1010), "first world frame starts the buffer");
    for (uint64_t now = 1020; now <= 2500; now += 10)
        Check(!gate.Ready(true, 100, now), "main pump and elapsed time cannot release a frozen renderer");
    gate.Reset();
    Check(!gate.Ready(true, 100, 3000), "new level starts a fresh buffer");
    for (uint64_t i = 1; i < 30; ++i)
        Check(!gate.Ready(true, 100 + i, 3000 + i * 25), "a partial render buffer remains pending");
    Check(gate.Ready(true, 130, 3750), "settled world restores after 30 completed presents and 750ms");
    Check(!gate.Ready(true, 200, 5000), "a long render gap restarts the buffer even if more frames arrived");
    Check(!gate.Ready(false, 230, 5750), "world teardown cancels buffered progress");
    Check(!gate.Ready(true, 230, 5760), "a paused but rendered demo can start another buffer");
    for (uint64_t i = 1; i <= 30; ++i) gate.Ready(true, 230 + i, 5760 + i * 25);
    Check(gate.Ready(true, 260, 6510), "demo ticks are not required when presents keep advancing");
    Check(!gate.Ready(true, 1, 6520), "a reset render counter cannot underflow into readiness");
    Check(!gate.Ready(true, 2, 10), "a backward clock restarts readiness");
    gate.Reset();
    gate.Ready(true, 100, 10000);
    Check(!gate.Ready(true, 130, 10300), "high FPS cannot skip the 750ms buffer");
    Check(gate.Ready(true, 131, 10750), "both time and frame requirements must be met");
    SceneSaveQueue q;
    q.Bind("de_mirage", "original");
    q.Observe("edit", 100);
    Check(!q.Due(849) && q.Due(850), "debounce committed edits");
    q.Observe("edit again", 800);
    Check(!q.Due(1000) && q.Due(1000, true), "map exit flushes latest edit");
    q.Complete(false, 1000);
    Check(!q.Due(5999) && q.Due(6000), "failed save retains work and backs off");
    Check(q.Due(1001, true), "orderly exit retries a pending failed save immediately");
    q.Complete(true, 6000);
    Check(!q.Due(7000) && q.saved == "edit again", "successful save acknowledges exact snapshot");
    q.Observe("reset", 8000);
    Check(q.Due(8750), "reset persists like any other committed edit");
    q.Observe("edit again", 8100);
    Check(!q.dirty, "undo back to saved state needs no write");
    q.Bind("de_nuke", "nuke");
    Check(!q.dirty && q.pending == "nuke", "map bindings isolate snapshots");
    q.Bind(""); q.Observe("menu", 0);
    Check(!q.Due(10000, true), "menu never writes a map autosave");
    std::map<std::string,SceneSaveQueue> failed;
    q.Bind("de_mirage","saved");q.Observe("latest mirage",100);q.Complete(false,900);
    BindSceneSaveQueue(q,failed,"de_nuke");
    Check(failed.at("de_mirage").pending=="latest mirage","failed map exit retains committed edits");
    q.Observe("latest nuke",1000);q.Complete(false,2000);
    BindSceneSaveQueue(q,failed,"de_mirage");
    Check(q.dirty&&q.pending=="latest mirage"&&failed.count("de_mirage")==0,"returning map recovers exact unsaved edits");
    Check(failed.at("de_nuke").pending=="latest nuke","other map remains isolated");
    q.Observe("newer mirage",3000);BindSceneSaveQueue(q,failed,"");
    Check(failed.at("de_mirage").pending=="newer mirage","newer accepted edits supersede the previous failed snapshot");
    std::cout << "Environment persistence tests passed\n";
}
