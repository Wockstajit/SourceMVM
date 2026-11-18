#pragma once
// WHAT WET GROUND MIRRORS WHEN THE MIRRORED THING IS OFF SCREEN (the screen trace in
// EnvironmentRainSurfaceShaders.h only finds what is on screen: look down at a puddle and the
// buildings it should hold are gone). The fallback is CS2's own reflection probes under the map's
// lighting, and the live capture through the bounce volume under Environment lighting. Engine-free
// parts: reading CS2's probe records, choosing the probes near the camera, and the tone curve
// that puts probe light into the finished frame's colours. Tests: reflection-probe-math-tests.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Filmmaker::Environment {

// CS2's clustered reflection record, nine float4 rows from cb3[25 + 9n] (decoded from the shipped
// material loop; RelightProbeMath.h ReflectionRecordRows writes the same layout): rows 0-2 the
// world-to-box 3x4 (the capture sits at the box origin), 3 box min with the cube index as a raw
// integer in w, 4 box max, 5 inverse edge feather, 6 unread, 7 tint, 8 ambient normalization.
inline constexpr unsigned kProbeRecordFirstRow = 25, kProbeRecordRows = 9, kProbeRecordMax = 128;
// How many probes the wet film looks through per frame (the ones nearest the camera).
inline constexpr unsigned kProbesPerFrame = 16;

struct ProbeRecord {
    unsigned index = 0;         // n: rows start at kProbeRecordFirstRow + kProbeRecordRows * n
    uint32_t cube = 0;          // slice of the bound cube array
    float toBox[3][4]{};        // world -> box
    float boxMin[3]{}, boxMax[3]{};
    float worldMin[3]{}, worldMax[3]{}; // the box's world bounds
    float volume = 0;           // in box units
};

// The record at n from a copy of cb3 (`rows` float4 rows), or false when that slot holds no usable
// probe: unwritten (zero), not finite, an empty box, a cube outside the array, or a world-to-box
// matrix that cannot be inverted.
inline bool DecodeProbeRecord(const float* rows, unsigned rowCount, unsigned n, unsigned cubeCount, ProbeRecord& out) {
    const unsigned first = kProbeRecordFirstRow + kProbeRecordRows * n;
    if (first + kProbeRecordRows > rowCount) return false;
    const float* r = rows + first * 4;
    for (unsigned i = 0; i < 6 * 4; ++i) {
        if (i == 3 * 4 + 3) continue; // the cube index is an integer, not a float
        if (!std::isfinite(r[i])) return false;
    }
    ProbeRecord p;
    p.index = n;
    std::memcpy(&p.cube, &r[3 * 4 + 3], sizeof(p.cube));
    if (p.cube >= cubeCount) return false;
    for (int a = 0; a < 3; ++a) {
        for (int c = 0; c < 4; ++c) p.toBox[a][c] = r[a * 4 + c];
        p.boxMin[a] = r[3 * 4 + a];
        p.boxMax[a] = r[4 * 4 + a];
        if (!(p.boxMax[a] > p.boxMin[a])) return false;
    }
    // Inverse of the 3x3 part (box -> world), for the box's world bounds.
    const float(&m)[3][4] = p.toBox;
    const float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                      m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (!(std::abs(det) > 1e-8f)) return false;
    float inv[3][3];
    inv[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
    inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    inv[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
    inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    inv[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
    inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    for (int a = 0; a < 3; ++a) { p.worldMin[a] = 1e30f; p.worldMax[a] = -1e30f; }
    for (int corner = 0; corner < 8; ++corner) {
        const float local[3]{(corner & 1) ? p.boxMax[0] : p.boxMin[0], (corner & 2) ? p.boxMax[1] : p.boxMin[1],
                             (corner & 4) ? p.boxMax[2] : p.boxMin[2]};
        const float shifted[3]{local[0] - m[0][3], local[1] - m[1][3], local[2] - m[2][3]};
        for (int a = 0; a < 3; ++a) {
            const float w = inv[a][0] * shifted[0] + inv[a][1] * shifted[1] + inv[a][2] * shifted[2];
            p.worldMin[a] = (std::min)(p.worldMin[a], w);
            p.worldMax[a] = (std::max)(p.worldMax[a], w);
        }
    }
    p.volume = (p.boxMax[0] - p.boxMin[0]) * (p.boxMax[1] - p.boxMin[1]) * (p.boxMax[2] - p.boxMin[2]);
    out = p;
    return true;
}

inline std::vector<ProbeRecord> DecodeProbeRecords(const float* rows, unsigned rowCount, unsigned cubeCount) {
    std::vector<ProbeRecord> out;
    for (unsigned n = 0; n < kProbeRecordMax; ++n) {
        ProbeRecord p;
        if (DecodeProbeRecord(rows, rowCount, n, cubeCount, p)) out.push_back(p);
    }
    return out;
}

// Distance from a point to a box's world bounds (0 inside).
inline float ProbeDistance(const ProbeRecord& p, const float point[3]) {
    float d2 = 0;
    for (int a = 0; a < 3; ++a) {
        const float d = (std::max)((std::max)(p.worldMin[a] - point[a], 0.f), point[a] - p.worldMax[a]);
        d2 += d * d;
    }
    return std::sqrt(d2);
}

// The probes the wet film looks through this frame: the up to `count` nearest the camera (a box the
// camera is in counts as distance 0), then ordered smallest box first, because the shader takes the
// first box that holds a point and feathers it into the next: the innermost, most local capture
// wins, the way an interior probe overrides the map-wide one.
inline std::vector<unsigned> SelectProbes(const std::vector<ProbeRecord>& records, const float camera[3], unsigned count = kProbesPerFrame) {
    std::vector<std::pair<float, unsigned>> byDistance;
    for (unsigned i = 0; i < records.size(); ++i) byDistance.push_back({ProbeDistance(records[i], camera), i});
    std::sort(byDistance.begin(), byDistance.end(), [&](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : records[a.second].volume < records[b.second].volume;
    });
    if (byDistance.size() > count) byDistance.resize(count);
    std::vector<unsigned> chosen;
    for (const auto& e : byDistance) chosen.push_back(e.second);
    std::sort(chosen.begin(), chosen.end(), [&](unsigned a, unsigned b) { return records[a].volume < records[b].volume; });
    std::vector<unsigned> indices;
    for (unsigned i : chosen) indices.push_back(records[i].index);
    return indices;
}

// THE TONE CURVE, MEASURED. The wet film is painted on the finished frame, after CS2's exposure,
// filmic curve and colour grade; a probe holds raw scene light. Right after CS2's post-process,
// its input (raw) and output (finished, decoded from sRGB) are compared at a grid of central
// pixels, per channel, in bins of half a stop. Bins the frame had no pixels for are filled here:
// between two measured bins by straight interpolation, below the first by scaling toward black,
// above the last by holding it (the curve's shoulder).
inline constexpr unsigned kToneBins = 48;
inline constexpr float kToneLowStop = -12, kToneHighStop = 12; // log2 of raw light
inline float ToneBinCentre(unsigned bin) {
    return kToneLowStop + (kToneHighStop - kToneLowStop) * (bin + .5f) / kToneBins;
}
// `sum[bin]` and `count[bin]` for one channel; returns the finished value per bin, or an empty
// curve when no bin was measured.
inline std::array<float, kToneBins> FillToneCurve(const std::array<float, kToneBins>& sum, const std::array<uint32_t, kToneBins>& count, bool& any) {
    std::array<float, kToneBins> out{};
    std::array<int, kToneBins> known{};
    int first = -1, last = -1;
    for (unsigned b = 0; b < kToneBins; ++b) {
        if (count[b]) { out[b] = sum[b] / float(count[b]); known[b] = 1; if (first < 0) first = int(b); last = int(b); }
    }
    any = first >= 0;
    if (!any) return out;
    for (int b = 0; b < first; ++b) out[b] = out[first] * std::exp2(ToneBinCentre(unsigned(b)) - ToneBinCentre(unsigned(first)));
    for (int b = last + 1; b < int(kToneBins); ++b) out[b] = out[last];
    int previous = first;
    for (int b = first + 1; b <= last; ++b) {
        if (!known[b]) continue;
        for (int g = previous + 1; g < b; ++g) out[g] = out[previous] + (out[b] - out[previous]) * float(g - previous) / float(b - previous);
        previous = b;
    }
    // A finished image never gets darker as the light rises: keep the curve monotone.
    for (unsigned b = 1; b < kToneBins; ++b) out[b] = (std::max)(out[b], out[b - 1]);
    return out;
}

} // namespace Filmmaker::Environment
