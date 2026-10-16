// The camera GRAPH dock's model against the camera path it bakes into
// (Filmmaker/Movie/GraphExpModel.h). The graph and the camera timeline are two views of one shot,
// so their markers must agree one to one, including after the path changed while the graph was
// closed.
#include "../../AfxHookSource2/Filmmaker/Movie/CamMarkers.h"
#include "../../AfxHookSource2/Filmmaker/Movie/GraphExpModel.h"
#include "../../Shared/AfxConsole.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

// CamMarkers.cpp reports bad JSON through the console.
namespace advancedfx { void NoConsole(const char*, ...) {} Con_Printf_t Message = NoConsole; Con_Printf_t Warning = NoConsole; }

using Filmmaker::CamMarker;
using Filmmaker::GraphExpModel;

namespace {

int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_failures; std::printf("FAIL %s\n", what); }
}

CamMarker Marker(int tick, double x, double fov = 65.0) {
    CamMarker m;
    m.tick = tick;
    m.x = x; m.y = x * 2.0; m.z = 1600.0 + x;
    m.pitch = 5.0; m.yaw = x * 0.1; m.roll = 0.0;
    m.fov = fov;
    return m;
}

bool SameTicks(const std::vector<CamMarker>& a, const std::vector<int>& ticks) {
    if (a.size() != ticks.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (a[i].tick != ticks[i]) return false;
    return true;
}

int KeysPerChannel(const GraphExpModel& g, int ch) { return (int)g.GetChannel(ch).keys.size(); }

// Angles compare modulo a full turn: the graph unwraps them, the engine does not care.
double ChannelError(int ch, double a, double b) {
    double d = a - b;
    if (ch >= 3 && ch <= 5) d = std::remainder(d, 360.0);
    return std::abs(d);
}

// The project file stores 6 significant digits, so a reloaded path is close, not bit-equal.
bool CloseAfterSave(const std::vector<CamMarker>& a, const std::vector<CamMarker>& b) {
    auto close = [](double x, double y) { return std::abs(x - y) <= 1e-4 * std::max(1.0, std::abs(x)); };
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].tick != b[i].tick || !close(a[i].yaw, b[i].yaw) || !close(a[i].fov, b[i].fov)) return false;
        for (int c = 0; c < 7; ++c) {
            const Filmmaker::GraphCurve::Handle* ha[2] = { &a[i].in[c], &a[i].out[c] };
            const Filmmaker::GraphCurve::Handle* hb[2] = { &b[i].in[c], &b[i].out[c] };
            for (int s = 0; s < 2; ++s) {
                if (ha[s]->active != hb[s]->active) return false;
                if (ha[s]->active && (!close(ha[s]->tx, hb[s]->tx) || !close(ha[s]->dv, hb[s]->dv))) return false;
            }
        }
    }
    return true;
}

} // namespace

int main() {
    // The reported case: 2 markers when the graph first opened, 3 more placed from the marker HUD
    // while the dock showed the camera timeline, then the graph reopened and a key was clicked.
    const std::vector<CamMarker> two = { Marker(5756, -40.0), Marker(5827, -30.0) };
    std::vector<CamMarker> five = two;
    five.push_back(Marker(5857, -16.0));
    five.push_back(Marker(5931, 17.1));
    five.push_back(Marker(5961, 42.3, 41.0));

    {
        GraphExpModel g;
        g.SeedFromMarkers(two);
        check(!GraphExpModel::SameMarkers(five, two), "a placed marker is a path change");

        // Without adopting the change, a bake from this model is the bug: 5 markers become 2.
        check(g.BakeMarkers(five, 1.0 / 64.0).size() == 2, "a stale model bakes the old marker set");

        check(g.ReconcileWithMarkers(five, false), "reconcile reports the change");
        for (int c = 0; c < GraphExpModel::kChannelCount; ++c)
            check(KeysPerChannel(g, c) == 5, "every channel has one key per marker");
        const std::vector<CamMarker> baked = g.BakeMarkers(five, 1.0 / 64.0);
        check(SameTicks(baked, { 5756, 5827, 5857, 5931, 5961 }), "the bake keeps all 5 markers");
        check(GraphExpModel::SameMarkers(baked, five), "the bake reproduces the path exactly");
        check(std::abs(baked[4].fov - 41.0) < 1e-9, "the new marker's FOV survives the round trip");
        check(!g.ReconcileWithMarkers(five, false), "reconcile is a no-op once in line");
    }

    // Keys that survive keep their id, handles and selection; removed keys leave the selection.
    {
        GraphExpModel g;
        g.SeedFromMarkers(two);
        const int keptId = g.GetChannel(0).keys[0].id;
        const int goneId = g.GetChannel(0).keys[1].id;
        g.SetHandle(0, keptId, +1, 0.25, 7.5, false);
        g.Select(0, keptId, false);
        g.SelectAdd(0, goneId);

        std::vector<CamMarker> moved = { two[0], Marker(5900, -30.0) }; // #2 retimed on the timeline
        check(g.ReconcileWithMarkers(moved, false), "a retimed marker is a change");
        const GraphExpModel::Keyframe* kept = g.FindKeyConst(0, keptId);
        check(kept != nullptr, "the unchanged key keeps its id");
        check(kept && kept->cpRight.active && std::abs(kept->cpRight.dv - 7.5) < 1e-9, "and its handle");
        check(g.IsSelected(0, keptId), "and its selection");
        check(g.FindKeyConst(0, goneId) == nullptr && !g.IsSelected(0, goneId), "the old tick is gone");
        check(g.GetChannel(0).keys[1].tick == 5900.0, "the key follows the marker to its new tick");
    }

    // Undo after an outside change must not resurrect the old marker set.
    {
        GraphExpModel g;
        g.SeedFromMarkers(two);
        g.BeginEdit();
        g.SetKeyValue(6, g.GetChannel(6).keys[0].id, 50.0);
        check(g.CanUndo(), "a graph edit is undoable");
        g.ReconcileWithMarkers(five, false);
        check(!g.CanUndo() && !g.CanRedo(), "history from the old marker set is dropped");
        g.Undo();
        check(g.BakeMarkers(five, 0.0).size() == 5, "an undo afterwards still bakes 5 markers");
    }

    // Smooth paths give NEW keys auto tangents and leave existing handles alone.
    {
        GraphExpModel g;
        g.SeedFromMarkers(two);
        g.ReconcileWithMarkers(five, true);
        const GraphExpModel::Keyframe& mid = g.GetChannel(0).keys[2];
        check(mid.cpLeft.active && mid.cpRight.active, "a new key on a smooth path gets handles");
        const GraphExpModel::Keyframe& first = g.GetChannel(0).keys[0];
        check(!first.cpLeft.active && !first.cpRight.active, "a kept key's (absent) handles are kept");
    }

    // One marker per whole tick: keys that round to the same tick never make two markers.
    {
        GraphExpModel g;
        g.AddKey(0, 99.6, 1.0);
        g.AddKey(1, 100.4, 2.0);
        g.AddKey(0, 200.0, 3.0);
        const std::vector<CamMarker> baked = g.BakeMarkers({}, 0.0);
        check(SameTicks(baked, { 100, 200 }), "99.6 and 100.4 bake to one marker at tick 100");
    }

    // An empty path empties the graph (Delete All on the timeline).
    {
        GraphExpModel g;
        g.SeedFromMarkers(five);
        check(g.ReconcileWithMarkers({}, false), "deleting every marker is a change");
        check(g.TotalKeys() == 0, "the graph follows the path to empty");
    }

    // THE GRAPH'S CURVES ARE THE CAMERA'S. Ease and bend a channel with handles, add a key between
    // two markers on one channel only, bake: the camera's evaluation of the baked markers
    // (CurvePoseBetween -- what CamPathEval::EvalCurves flies) equals the graph's curve at every tick.
    {
        std::vector<CamMarker> three = { Marker(1000, 0.0), Marker(1100, 50.0), Marker(1300, 20.0) };
        three[1].yaw = 175.0; three[2].yaw = -170.0; // the shot turns across +/-180
        GraphExpModel g;
        g.SeedFromMarkers(three);
        g.SetHandle(0, g.GetChannel(0).keys[0].id, +1, 0.6, 0.0, false);  // ease out of the first key
        g.SetHandle(0, g.GetChannel(0).keys[1].id, -1, 0.4, 30.0, true);  // overshoot through the second
        g.SetHandle(4, g.GetChannel(4).keys[1].id, +1, 0.3, 12.0, true);  // bend the yaw
        g.AddKey(6, 1200.0, 30.0);                                         // a FOV key between markers
        std::vector<double> before(7 * 301);
        for (int c = 0; c < 7; ++c)
            for (int t = 1000; t <= 1300; ++t) g.SampleChannel(c, t, before[c * 301 + (t - 1000)]);

        const std::vector<CamMarker> baked = g.BakeMarkers(three, 1.0 / 64.0);
        check(SameTicks(baked, { 1000, 1100, 1200, 1300 }), "the FOV key adds a marker at its tick");
        check(GraphExpModel::MarkersHaveCurves(baked), "the baked markers carry the graph's handles");
        double worst = 0.0;
        for (size_t i = 0; i + 1 < baked.size(); ++i) {
            for (int t = baked[i].tick; t <= baked[i + 1].tick; ++t) {
                double pose[7];
                Filmmaker::CurvePoseBetween(baked[i], baked[i + 1], t, pose);
                for (int c = 0; c < 7; ++c) worst = std::max(worst, ChannelError(c, pose[c], before[c * 301 + (t - 1000)]));
            }
        }
        check(worst < 1e-6, "the camera flies the curve the graph draws");

        // A curve edit moves the camera between keys: without handles the same keys fly elsewhere.
        std::vector<CamMarker> flat = baked;
        for (CamMarker& m : flat) for (int c = 0; c < 7; ++c) { m.in[c] = {}; m.out[c] = {}; }
        double flatPose[7], curvedPose[7];
        Filmmaker::CurvePoseBetween(flat[0], flat[1], 1050, flatPose);
        Filmmaker::CurvePoseBetween(baked[0], baked[1], 1050, curvedPose);
        check(std::abs(flatPose[0] - curvedPose[0]) > 1.0, "the handles change the camera's position mid-segment");

        // Adopting the baked path back leaves the curve alone (the split keys are on it) and is stable.
        g.ReconcileWithMarkers(baked, true);
        double drift = 0.0;
        for (int c = 0; c < 7; ++c)
            for (int t = 1000; t <= 1300; ++t) {
                double v = 0.0;
                g.SampleChannel(c, t, v);
                drift = std::max(drift, ChannelError(c, v, before[c * 301 + (t - 1000)]));
            }
        check(drift < 1e-6, "adopting the baked path leaves the graph's curves unchanged");
        check(GraphExpModel::SameMarkers(g.BakeMarkers(baked, 1.0 / 64.0), baked), "a second bake is identical");

        // A graph opened on the curved path shows those curves, not fresh auto tangents.
        GraphExpModel reopened;
        reopened.SeedFromMarkers(baked);
        double reopenDrift = 0.0;
        for (int c = 0; c < 7; ++c)
            for (int t = 1000; t <= 1300; ++t) {
                double v = 0.0;
                reopened.SampleChannel(c, t, v);
                reopenDrift = std::max(reopenDrift, ChannelError(c, v, before[c * 301 + (t - 1000)]));
            }
        check(reopenDrift < 1e-6, "reopening the graph shows the curve the camera flies");

        // The handles survive the project file.
        Filmmaker::PathSettings st;
        Filmmaker::CamMarkers saved;
        saved.Restore(baked, 0);
        Filmmaker::CamMarkers loaded;
        int sel = 0;
        check(loaded.Parse(saved.Serialize(st, 0), st, sel), "the curved path parses back");
        check(CloseAfterSave(loaded.All(), baked), "and keeps every handle");
    }

    if (g_failures == 0) std::printf("graph-exp-model-tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
