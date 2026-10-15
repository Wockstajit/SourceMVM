#include "../../AfxHookSource2/Filmmaker/Deadlock/DeadlockReplaySessionMath.h"
#include "../../AfxHookSource2/Filmmaker/Movie/GraphExpModel.h"
#include <cstdio>

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { ++failures; std::printf("FAIL %s\n", what); }
    };
    Filmmaker::DeadlockReplayIdentity identity;
    check(!identity.Observe(false, L""), "idle is stable");
    check(identity.Observe(true, L"A.dem"), "first replay enters");
    check(!identity.Observe(true, L""), "transient seek preserves identity");
    check(identity.Path() == L"A.dem", "seek does not lose destination");
    check(!identity.Observe(true, L"a.DEM"), "case alias is same project");
    check(identity.Observe(true, L"B.dem"), "direct switch detected without unload");
    check(identity.Observe(false, L"B.dem"), "debounced unload clears even a stale filename");
    check(identity.Path().empty(), "menu has no active project");
    check(!identity.Observe(false, L""), "unload occurs once");
    check(identity.Observe(true, L"B.dem"), "same replay after confirmed unload enters anew");

    Filmmaker::DeadlockReplaySessions<Filmmaker::GraphExpModel> sessions;
    auto& graph = sessions.Current(); // the reference must stay valid across moves
    sessions.Switch(L"A.dem");
    const int a = graph.AddKey(6, 100, 55);
    graph.Select(6, a, false);
    graph.BeginEdit();
    graph.SetKeyValue(6, a, 65);
    graph.EndEdit();
    graph.SetVisible(0, false);
    sessions.Switch(L"B.dem");
    check(graph.TotalKeys() == 0 && !graph.CanUndo(), "B starts without A curves or history");
    graph.AddKey(6, 200, 110);
    sessions.Switch(L"a.DEM");
    check(graph.TotalKeys() == 1 && graph.FindKeyConst(6, a)->value == 65, "A restores its own edits");
    check(graph.CanUndo() && graph.SelectionCount() == 1 && !graph.GetChannel(0).visible,
        "A retains undo selection and visibility");
    graph.Undo();
    check(graph.FindKeyConst(6, a)->value == 55, "restored A undo remains usable");
    sessions.Switch(L"");
    check(graph.TotalKeys() == 0 && !graph.CanUndo(), "menu has no active curves");
    for (int i = 0; i < 1000; ++i) {
        sessions.Switch(L"A.dem");
        sessions.Switch(L"B.dem");
    }
    check(graph.TotalKeys() == 1 && graph.GetChannel(6).keys[0].value == 110, "B remains independent");
    check(sessions.CachedCount() == 1, "repeated switches do not duplicate retained states");
    std::printf("DeadlockReplaySession: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
