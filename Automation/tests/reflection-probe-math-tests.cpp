// EnvironmentReflectionProbeMath.h: reading CS2's reflection probe records out of a cb3 copy,
// choosing the probes near the camera, and filling the measured tone curve.
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentReflectionProbeMath.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace Filmmaker::Environment;

static int failures = 0;
static void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
static bool Near(float a, float b, float eps = 1e-3f) { return std::abs(a - b) <= eps * (1 + std::abs(b)); }

// Writes a record the way CS2 lays it out: rows 0-2 world->box, 3 min + cube (raw int), 4 max.
static void WriteRecord(std::vector<float>& rows, unsigned n, const float toBox[3][4], const float mn[3], const float mx[3], uint32_t cube) {
    float* r = rows.data() + (kProbeRecordFirstRow + kProbeRecordRows * n) * 4;
    for (int a = 0; a < 3; ++a) for (int c = 0; c < 4; ++c) r[a * 4 + c] = toBox[a][c];
    for (int a = 0; a < 3; ++a) { r[12 + a] = mn[a]; r[16 + a] = mx[a]; r[20 + a] = 1.f / 16; r[28 + a] = 1; }
    std::memcpy(&r[15], &cube, sizeof(cube));
}

static std::vector<float> EmptyCb3() { return std::vector<float>((kProbeRecordFirstRow + kProbeRecordRows * kProbeRecordMax + 8) * 4, 0.f); }

static void TestDecodeAndBounds() {
    auto rows = EmptyCb3();
    // A probe captured at (100, 200, 50): the box runs 300 units either way in x and y, 100 up and down.
    const float identity[3][4]{{1, 0, 0, -100}, {0, 1, 0, -200}, {0, 0, 1, -50}};
    const float mn[3]{-300, -300, -100}, mx[3]{300, 300, 100};
    WriteRecord(rows, 0, identity, mn, mx, 2);
    // Rotated a quarter turn about z: box x is world y.
    const float turned[3][4]{{0, 1, 0, 0}, {-1, 0, 0, 0}, {0, 0, 1, 0}};
    const float tmn[3]{-10, -40, -5}, tmx[3]{10, 40, 5};
    WriteRecord(rows, 3, turned, tmn, tmx, 0);
    // A cube index past the array, and an empty box: both refused.
    WriteRecord(rows, 5, identity, mn, mx, 9);
    const float flat[3]{0, 0, 0};
    WriteRecord(rows, 6, identity, flat, flat, 1);
    const unsigned rowCount = unsigned(rows.size() / 4);
    const auto records = DecodeProbeRecords(rows.data(), rowCount, 4);
    Check(records.size() == 2, "two usable records out of four written (bad cube, empty box refused)");
    Check(records[0].index == 0 && records[0].cube == 2, "record 0 keeps its cube index read as an integer");
    Check(Near(records[0].worldMin[0], -200) && Near(records[0].worldMax[0], 400), "box x bounds in world: 100 +- 300");
    Check(Near(records[0].worldMin[2], -50) && Near(records[0].worldMax[2], 150), "box z bounds in world: 50 +- 100");
    Check(records[1].index == 3, "record 3 is found where it was written");
    // Box x (+-10) is world y, box y (+-40) is world -x: world x spans +-40, y spans +-10.
    Check(Near(records[1].worldMin[0], -40) && Near(records[1].worldMax[0], 40), "rotated box: world x from box y");
    Check(Near(records[1].worldMin[1], -10) && Near(records[1].worldMax[1], 10), "rotated box: world y from box x");
    Check(Near(records[0].volume, 600.f * 600.f * 200.f), "volume in box units");
    ProbeRecord unused;
    Check(!DecodeProbeRecord(rows.data(), rowCount, 1, 4, unused), "an unwritten (zero) slot is no probe");
    Check(!DecodeProbeRecord(rows.data(), kProbeRecordFirstRow + 3, 0, 4, unused), "a record past the copied rows is not read");
}

static void TestSelection() {
    auto rows = EmptyCb3();
    const float id[3][4]{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    // A map-wide box, a street box the camera is in, and a small room box further off.
    const float wideMin[3]{-1e5f, -1e5f, -1e5f}, wideMax[3]{1e5f, 1e5f, 1e5f};
    const float streetMin[3]{-500, -100, -50}, streetMax[3]{500, 100, 200};
    const float roomMin[3]{2000, 0, 0}, roomMax[3]{2200, 200, 150};
    WriteRecord(rows, 0, id, wideMin, wideMax, 0);
    WriteRecord(rows, 1, id, streetMin, streetMax, 1);
    WriteRecord(rows, 2, id, roomMin, roomMax, 2);
    const auto records = DecodeProbeRecords(rows.data(), unsigned(rows.size() / 4), 3);
    const float camera[3]{0, 0, 60};
    auto chosen = SelectProbes(records, camera, 16);
    Check(chosen.size() == 3, "all three probes are near enough");
    Check(chosen[0] == 2 && chosen[1] == 1 && chosen[2] == 0, "smallest box first, the map-wide one last");
    chosen = SelectProbes(records, camera, 2);
    Check(chosen.size() == 2 && chosen[0] == 1 && chosen[1] == 0, "with room for two: the ones the camera is in, not the far room");
    Check(ProbeDistance(records[2], camera) > 1400, "distance to a box outside it");
    Check(ProbeDistance(records[1], camera) == 0, "inside a box is distance 0");
}

static void TestToneCurve() {
    std::array<float, kToneBins> sum{};
    std::array<uint32_t, kToneBins> count{};
    bool any = true;
    auto empty = FillToneCurve(sum, count, any);
    Check(!any, "nothing measured: no curve");
    // Two measured bins with a gap between them, and a third above them.
    sum[20] = .2f * 4; count[20] = 4;
    sum[24] = .6f * 2; count[24] = 2;
    sum[30] = .5f; count[30] = 1; // lower than bin 24: the curve is held monotone
    auto curve = FillToneCurve(sum, count, any);
    Check(any, "measured bins make a curve");
    Check(Near(curve[20], .2f) && Near(curve[24], .6f), "measured bins keep their mean");
    Check(Near(curve[22], .4f), "a gap is filled in a straight line");
    Check(Near(curve[30], .6f) && Near(curve[kToneBins - 1], .6f), "monotone, and held above the last bin");
    Check(Near(curve[18], .2f * std::exp2(ToneBinCentre(18) - ToneBinCentre(20))), "below the first bin it scales toward black");
    Check(curve[0] >= 0 && curve[0] < .01f, "far below: near black");
    Check(Near(ToneBinCentre(kToneBins / 2), .25f), "bin centres are half a stop apart around 0");
}

int main() {
    TestDecodeAndBounds();
    TestSelection();
    TestToneCurve();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("ReflectionProbeMathTests: all passed\n");
    return 0;
}
