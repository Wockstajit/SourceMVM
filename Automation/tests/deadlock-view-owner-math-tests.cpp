#include "../../AfxHookSource2/Deadlock/DeadlockViewOwnerMath.h"
#include <cstdio>
#include <cstring>

// Who owns the Deadlock view, and whether a pushed pose is still fresh. See DeadlockViewOwnerMath.h
// for why these rules exist: before them the follow camera could never write the view, the graph
// beat an owning shot, and a pose pushed while nothing claimed the view jumped in later.
int main() {
    int failures = 0;
    auto check = [&](bool value, const char* what) {
        if (!value) { ++failures; std::printf("  FAIL %s\n", what); }
    };
    using Deadlock::ViewOwner;
    using Deadlock::ViewOwnerInputs;
    using Deadlock::ResolveViewOwner;

    // Nothing claims the view: the game camera is used and nothing is written.
    ViewOwnerInputs none;
    check(ResolveViewOwner(none) == ViewOwner::Game, "no inputs -> game");
    check(!Deadlock::ViewOwnerWrites(ViewOwner::Game), "the game camera is never written");

    // Each input on its own names its own owner.
    { ViewOwnerInputs in; in.replayAvailable = true; in.shotActive = true;   check(ResolveViewOwner(in) == ViewOwner::Shot, "shot alone"); }
    { ViewOwnerInputs in; in.replayAvailable = true; in.graphDriving = true; check(ResolveViewOwner(in) == ViewOwner::Graph, "graph alone"); }
    { ViewOwnerInputs in; in.replayAvailable = true; in.followLive = true;   check(ResolveViewOwner(in) == ViewOwner::Follow, "follow alone"); }
    { ViewOwnerInputs in; in.replayAvailable = true; in.freeCam = true;      check(ResolveViewOwner(in) == ViewOwner::FreeCam, "free cam alone"); }
    { ViewOwnerInputs in; in.replayAvailable = true; in.shotHold = true;     check(ResolveViewOwner(in) == ViewOwner::ShotHold, "held path alone"); }

    // An owning shot beats the graph -- the pending-pose race used to give this to the graph.
    { ViewOwnerInputs in; in.replayAvailable = true; in.shotActive = true; in.graphDriving = true;
      check(ResolveViewOwner(in) == ViewOwner::Shot, "shot beats graph"); }
    // A playing path interrupts live follow, and follow returns once the path stops.
    { ViewOwnerInputs in; in.replayAvailable = true; in.shotActive = true; in.followLive = true;
      check(ResolveViewOwner(in) == ViewOwner::Shot, "shot beats follow"); }
    { ViewOwnerInputs in; in.replayAvailable = true; in.followLive = true; in.freeCam = true;
      check(ResolveViewOwner(in) == ViewOwner::Follow, "live follow beats free cam"); }
    // The free camera outranks the paused, held path pose, so go-to-player and flying off to place
    // a marker are never frozen by a hold.
    { ViewOwnerInputs in; in.replayAvailable = true; in.freeCam = true; in.shotHold = true;
      check(ResolveViewOwner(in) == ViewOwner::FreeCam, "free cam beats held path"); }
    // Everything at once: the shot.
    { ViewOwnerInputs in; in.replayAvailable = true; in.shotActive = in.graphDriving = in.followLive = in.freeCam = in.shotHold = true;
      check(ResolveViewOwner(in) == ViewOwner::Shot, "all inputs -> shot"); }

    // A stale claim from any combination of owners cannot write a non-replay view.
    // Restoring the replay gate preserves the same precedence, without changing claims.
    for (unsigned mask = 0; mask < 32; ++mask) {
        ViewOwnerInputs in;
        in.shotActive = (mask & 1) != 0; in.graphDriving = (mask & 2) != 0;
        in.followLive = (mask & 4) != 0; in.freeCam = (mask & 8) != 0;
        in.shotHold = (mask & 16) != 0;
        check(ResolveViewOwner(in) == ViewOwner::Game, "unloaded replay rejects every owner combination");
        in.replayAvailable = true;
        const auto expected = in.shotActive ? ViewOwner::Shot : in.graphDriving ? ViewOwner::Graph
            : in.followLive ? ViewOwner::Follow : in.freeCam ? ViewOwner::FreeCam
            : in.shotHold ? ViewOwner::ShotHold : ViewOwner::Game;
        check(ResolveViewOwner(in) == expected, "loaded replay retains camera priority");
        in.replayAvailable = false;
        check(!Deadlock::ViewOwnerWrites(ResolveViewOwner(in)), "first unavailable view stops writes");
    }

    // Every owner except the game writes, and every owner has a distinct, non-empty name.
    const ViewOwner all[] = { ViewOwner::Game, ViewOwner::ShotHold, ViewOwner::FreeCam,
                              ViewOwner::Follow, ViewOwner::Graph, ViewOwner::Shot };
    for (ViewOwner a : all) {
        check(Deadlock::ViewOwnerName(a) && Deadlock::ViewOwnerName(a)[0], "owner has a name");
        if (a != ViewOwner::Game) check(Deadlock::ViewOwnerWrites(a), "non-game owner writes");
        for (ViewOwner b : all)
            if (a != b) check(0 != std::strcmp(Deadlock::ViewOwnerName(a), Deadlock::ViewOwnerName(b)), "names are distinct");
    }
    // The verifier script matches these two strings.
    check(0 == std::strcmp(Deadlock::ViewOwnerName(ViewOwner::Shot), "camera path"), "shot name is 'camera path'");
    check(0 == std::strcmp(Deadlock::ViewOwnerName(ViewOwner::Follow), "follow camera"), "follow name is 'follow camera'");

    // Freshness: a pose pushed in main pump N is consumed by the next view, during which the pump
    // counter still reads N. One pump later it is stale and must be dropped, not applied late.
    Deadlock::OwnedPose pose;
    check(!Deadlock::OwnedPoseFresh(pose, 0), "an empty slot is never fresh");
    pose.have = true;
    pose.stamp = 41;
    check(Deadlock::OwnedPoseFresh(pose, 41), "pushed in pump 41, read before pump 42 -> fresh");
    check(!Deadlock::OwnedPoseFresh(pose, 42), "one pump later -> stale");
    check(!Deadlock::OwnedPoseFresh(pose, 40), "a stamp from the future is not fresh either");

    std::printf("Deadlock view owner math: %d failures\n", failures);
    return failures ? 1 : 0;
}
