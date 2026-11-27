#define NOMINMAX
#include "EffectNodesRender.h"
#include "EffectNodes.h"

#include "../Debug/GpuPassProfiler.h"
#include "../Debug/MvmDebugLog.h"
#include "../Environment/EnvironmentMath.h"
#include "../Environment/EnvironmentShaderCompiler.h"
#include "../Render/RenderPlayerMatte.h"
#include "../Render/RenderPrivateTextureScope.h"

#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace Filmmaker::EffectNodes {
using Microsoft::WRL::ComPtr;
namespace {

constexpr char kShader[] = R"HLSL(
cbuffer NodeConstants : register(b0) {
    row_major float4x4 invViewProj;
    row_major float4x4 refViewProj;    // the view the world reference was rendered through
    row_major float4x4 refInvViewProj;
    float4 dims;         // target w, h; depth w, h
    float4 depthInfo;    // minDepth, maxDepth, reversed, hasWorldDepth
    float4 cameraTime;   // cam xyz, demoTime
    float4 camFwd;       // fwd xyz, hasMaskA
    float4 stepInfo;     // nodeType, hasMaskB, masterMix, wall-clock seconds (keeps moving while paused)
    float4 refInfo;      // the reference's minDepth, maxDepth; history pass, history valid
    float4 p0;           // params[0..3]
    float4 p1;           // params[4..7]
    int4   pmCounts;     // hasTarget, targetBoneCount, otherCount, 0
    float4 pmBounds;     // xyz, maxR
    float4 pmCapsule;    // feet xyz, height
    float4 pmBones[16];
    float4 pmOthers[16];
    float4 smokes[8];    // the smoke grenades: center xyz, radius
    int4   smokeInfo;    // smoke count, 0, 0, 0
};

Texture2D<float4> colorIn : register(t0);
#if DEPTH_MS
Texture2DMS<float> sceneDepth : register(t1);
#else
Texture2D<float> sceneDepth : register(t1);
#endif
Texture2D<float> worldDepth : register(t2);
Texture2D<float> maskA : register(t3);
Texture2D<float> maskB : register(t4);
Texture2D<float4> origColor : register(t5);
// Echo Trails' trail buffer / Stop Motion's held frame (refInfo.z = pass, refInfo.w = it holds a picture).
Texture2D<float4> history : register(t6);
SamplerState linearClamp : register(s0);

struct VsOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VsOut VS(uint id : SV_VertexID) {
    VsOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = p;
    return o;
}

bool Reversed() { return depthInfo.z > 0.5; }

float RawDepth(int2 p) {
    p = clamp(p, int2(0, 0), int2(dims.zw) - 1);
#if DEPTH_MS
    return sceneDepth.Load(p, 0);
#else
    return sceneDepth.Load(int3(p, 0));
#endif
}

bool WorldAtUv(float2 uv, out float3 world) {
    world = cameraTime.xyz;
    int2 p = clamp(int2(uv * dims.zw), int2(0, 0), int2(dims.zw) - 1);
    float d = RawDepth(p);
    if (Reversed() ? d < 1e-7 : d > 0.9999999) return false;
    if (!Reversed() && d <= 0.100001) return false;
    float span = max(depthInfo.y - depthInfo.x, 1e-12);
    float n = (d - depthInfo.x) / span;
    if (n < 0.0 || n > 1.0) return false;
    float4 h = mul(invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, n, 1.0));
    if (abs(h.w) < 1e-10) return false;
    world = h.xyz / h.w;
    return true;
}

float CameraDistAtUv(float2 uv) {
    float3 w;
    if (!WorldAtUv(uv, w)) return 1e30;
    return dot(w - cameraTime.xyz, camFwd.xyz);
}

// View distance of one reference texel: 1 / h.w of its unprojection.
float RefTexelDist(int2 p) {
    p = clamp(p, int2(0, 0), int2(dims.zw) - 1);
    float d = worldDepth.Load(int3(p, 0));
    if (Reversed() ? d < 1e-7 : d > 0.9999999) return 1e30;
    float n = (d - refInfo.x) / max(refInfo.y - refInfo.x, 1e-12);
    if (n < 0.0 || n > 1.0) return 1e30;
    float2 uv = (float2(p) + 0.5) / dims.zw;
    float4 h = mul(refInvViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, n, 1.0));
    return abs(h.w) < 1e-10 ? 1e30 : 1.0 / abs(h.w);
}

// 1 where the scene point stands in front of the world the reference shows there. The reference is
// from the frame before, so the point is looked up through the reference's own view: compared pixel
// for pixel, a moving camera made slivers of walls and ground read as players. The nearest of the
// four texels around the point, so a floor seen at a grazing angle is not in front of itself.
float InFrontOfWorld(float3 w) {
    float4 c = mul(refViewProj, float4(w, 1.0));
    if (c.w <= 1.0) return 0.0;
    float2 uv = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0)) return 0.0;
    int2 p = int2(floor(uv * dims.zw - 0.5));
    float worldD = min(min(RefTexelDist(p), RefTexelDist(p + int2(1, 0))),
        min(RefTexelDist(p + int2(0, 1)), RefTexelDist(p + int2(1, 1))));
    return c.w < worldD - max(1.5, worldD * 0.004) ? 1.0 : 0.0;
}

float CapsuleSdf(float3 p, float3 feet, float height, float radius) {
    float pad = min(radius, height * 0.45);
    float3 a = feet + float3(0, 0, pad);
    float3 b = feet + float3(0, 0, max(pad, height - pad));
    float3 ab = b - a;
    float denom = dot(ab, ab);
    float t = denom > 1e-6 ? saturate(dot(p - a, ab) / denom) : 0.0;
    return length((a + ab * t) - p) - radius;
}

float SelectedPlayerWeight(float3 p) {
    if (pmCounts.x == 0) return 1.0;
    float3 bd = p - pmBounds.xyz;
    float maxR = pmBounds.w > 1.0 ? pmBounds.w : 96.0;
    if (dot(bd, bd) > maxR * maxR) return 0.0;
    float dTarget = 1e9;
    if (pmCounts.y > 0) {
        [loop] for (int i = 0; i < pmCounts.y; ++i) {
            float r = pmBones[i].w > 1.0 ? pmBones[i].w : 16.0;
            dTarget = min(dTarget, length(p - pmBones[i].xyz) - r);
        }
    } else {
        dTarget = CapsuleSdf(p, pmCapsule.xyz, pmCapsule.w > 1.0 ? pmCapsule.w : 74.0, 22.0);
    }
    if (dTarget > 32.0) return 0.0;
    if (pmCounts.z > 0) {
        float dOther = 1e9;
        [loop] for (int j = 0; j < pmCounts.z; ++j) {
            dOther = min(dOther, CapsuleSdf(p, pmOthers[j].xyz, pmOthers[j].w > 1.0 ? pmOthers[j].w : 74.0, 18.0));
        }
        return saturate((dOther - dTarget + 1.5) / 3.0);
    }
    return 1.0;
}

// ---- helpers for the later nodes ----
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float Hash21(float2 p) { p = frac(p * float2(123.34, 456.21)); p += dot(p, p + 45.32); return frac(p.x * p.y); }
float Noise3(float3 p) {
    float3 i0 = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float2 o = float2(0.0, 1.0);
    float z0 = i0.z * 17.0, z1 = z0 + 17.0;
    float a = lerp(lerp(Hash21(i0.xy + o.xx + z0), Hash21(i0.xy + o.yx + z0), f.x),
                   lerp(Hash21(i0.xy + o.xy + z0), Hash21(i0.xy + o.yy + z0), f.x), f.y);
    float c = lerp(lerp(Hash21(i0.xy + o.xx + z1), Hash21(i0.xy + o.yx + z1), f.x),
                   lerp(Hash21(i0.xy + o.xy + z1), Hash21(i0.xy + o.yy + z1), f.x), f.y);
    return lerp(a, c, f.z);
}
float Fbm(float3 p) {
    float v = 0.0, amp = 0.5;
    // Each octave turned ~37 degrees, so the lattice of the value noise does not show as blocks.
    [unroll] for (int o = 0; o < 4; ++o) {
        v += Noise3(p) * amp;
        p = float3(p.x * 0.8 - p.y * 0.6, p.x * 0.6 + p.y * 0.8, p.z) * 2.03 + 11.7;
        amp *= 0.5;
    }
    return v / 0.9375;
}
float3 RgbToHsv(float3 c) {
    float4 K = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    float4 q = c.g < c.b ? float4(c.bg, K.wz) : float4(c.gb, K.xy);
    float4 r = c.r < q.x ? float4(q.xyw, c.r) : float4(c.r, q.yzx);
    float d = r.x - min(r.w, r.y);
    return float3(abs(r.z + (r.w - r.y) / (6.0 * d + 1e-10)), d / (r.x + 1e-10), r.x);
}
float3 HsvToRgb(float3 c) {
    float3 p = abs(frac(c.xxx + float3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return c.z * lerp(float3(1.0, 1.0, 1.0), saturate(p - 1.0), c.y);
}
// A soft disc of 25 taps, `rad` pixels across.
float3 BlurAt(float2 uv, float rad) {
    float2 tx = max(rad, 0.0) / max(dims.xy, float2(1.0, 1.0));
    float3 acc = colorIn.SampleLevel(linearClamp, uv, 0).rgb;
    float wsum = 1.0;
    [unroll] for (int r = 1; r <= 3; ++r) {
        [unroll] for (int a = 0; a < 8; ++a) {
            float ang = (float(a) + 0.5 * float(r)) * 0.78539816;
            float2 o = float2(cos(ang), sin(ang)) * (float(r) / 3.0) * tx;
            float wt = 1.0 - float(r - 1) * 0.25;
            acc += colorIn.SampleLevel(linearClamp, saturate(uv + o), 0).rgb * wt;
            wsum += wt;
        }
    }
    return acc / wsum;
}
// Where the camera looks: the scene point at the centre of the frame, or 300 units ahead.
float3 FocusPoint() {
    float3 w;
    if (WorldAtUv(float2(0.5, 0.5), w)) return w;
    return cameraTime.xyz + camFwd.xyz * 300.0;
}
float DistOrFar(float2 uv) { return min(CameraDistAtUv(uv), 1e5); }
float RealTime() { return stepInfo.w; }
float3 Ramp4(float t, float3 a, float3 b, float3 c, float3 d) {
    t = saturate(t) * 3.0;
    return t < 1.0 ? lerp(a, b, t) : (t < 2.0 ? lerp(b, c, t - 1.0) : lerp(c, d, t - 2.0));
}
// Color Ramp's gradient palettes, dark to light.
float3 Palette(int p, float t) {
    if (p == 0) return Ramp4(t, float3(0.02, 0.0, 0.0), float3(0.55, 0.02, 0.0), float3(1.0, 0.45, 0.0), float3(1.0, 0.95, 0.6));    // fire
    if (p == 1) return Ramp4(t, float3(0.0, 0.01, 0.04), float3(0.02, 0.12, 0.4), float3(0.2, 0.7, 0.95), float3(0.92, 1.0, 1.0));  // ice
    if (p == 2) return Ramp4(t, float3(0.1, 0.02, 0.25), float3(0.6, 0.08, 0.45), float3(1.0, 0.45, 0.2), float3(1.0, 0.88, 0.45)); // sunset
    if (p == 3) return Ramp4(t, float3(0.0, 0.02, 0.0), float3(0.05, 0.3, 0.05), float3(0.5, 1.0, 0.1), float3(0.95, 1.0, 0.7));    // toxic
    if (p == 4) return Ramp4(t, float3(0.05, 0.02, 0.25), float3(0.45, 0.1, 0.65), float3(1.0, 0.35, 0.75), float3(0.4, 0.95, 1.0)); // vaporwave
    if (p == 5) return Ramp4(t, float3(0.03, 0.02, 0.0), float3(0.35, 0.18, 0.04), float3(0.85, 0.62, 0.18), float3(1.0, 0.95, 0.78)); // gold
    if (p == 6) return Ramp4(t, float3(0.02, 0.0, 0.0), float3(0.35, 0.0, 0.02), float3(0.85, 0.05, 0.05), float3(1.0, 0.7, 0.65)); // blood
    if (p == 7) return Ramp4(t, float3(0.0, 0.02, 0.05), float3(0.0, 0.25, 0.35), float3(0.1, 0.7, 0.7), float3(0.85, 1.0, 0.95));  // ocean
    return HsvToRgb(float3(saturate(t) * 0.85, 0.9, 1.0));                                                                           // rainbow
}
// A head-held device's drift: slow and uneven (two unrelated noises), with a faint fast tremor.
float2 Sway(float t, float amount) {
    float2 s = float2(Noise3(float3(t * 0.31, 1.7, 0.0)), Noise3(float3(4.2, t * 0.27, 0.0))) - 0.5;
    s += (float2(Noise3(float3(t * 2.3, 9.1, 0.0)), Noise3(float3(3.3, t * 2.1, 0.0))) - 0.5) * 0.12;
    return s * 0.05 * amount;
}
// 1 inside a scope or goggle tube (mode 1 one tube, 2 two overlapping tubes), soft at the rim.
// rimDist: 0 at the centre, 1 at the rim.
float TubeMask(float2 uv, float2 c, int mode, out float rimDist) {
    float aspect = dims.x / max(dims.y, 1.0);
    float R = 0.46;
    float2 d0 = (uv - c) * float2(aspect, 1.0);
    float r = length(d0);
    if (mode == 2) { r = min(length(d0 - float2(-0.25, 0.0)), length(d0 - float2(0.25, 0.0))); R = 0.44; }
    rimDist = r / R;
    return 1.0 - smoothstep(R - 0.012, R + 0.004, r);
}
// Surface normal rebuilt from depth: the cross of the slopes to the right and down.
float3 NormalAtUv(float2 uv, float2 tx) {
    float3 c, x, y;
    if (!WorldAtUv(uv, c) || !WorldAtUv(saturate(uv + float2(tx.x, 0)), x) || !WorldAtUv(saturate(uv + float2(0, tx.y)), y))
        return float3(0, 0, 0);
    return normalize(cross(x - c, y - c) + 1e-6);
}
// 1 on a player: through the world reference when the graph has one, else from the player capsules.
float PeopleAt(float2 uv) {
    float3 w;
    if (!WorldAtUv(uv, w)) return 0.0;
    if (depthInfo.w > 0.5) return InFrontOfWorld(w);
    float d = 1e9;
    if (pmCounts.x != 0) d = CapsuleSdf(w, pmCapsule.xyz, pmCapsule.w > 1.0 ? pmCapsule.w : 74.0, 16.0);
    [loop] for (int j = 0; j < pmCounts.z; ++j)
        d = min(d, CapsuleSdf(w, pmOthers[j].xyz, pmOthers[j].w > 1.0 ? pmOthers[j].w : 74.0, 16.0));
    return saturate(1.0 - d / 6.0);
}

// How much smoke hides a point from the camera: 0 clear, 1 fully behind it. Each smoke grenade is a
// sphere; what counts is the length of smoke the view ray crosses before it reaches the point.
float SmokeCover(float3 w) {
    float3 o = cameraTime.xyz, d = w - o;
    float len = length(d);
    if (len < 1e-3) return 0.0;
    d /= len;
    float cover = 0.0;
    [loop] for (int s = 0; s < smokeInfo.x; ++s) {
        float r = smokes[s].w;
        float3 oc = o - smokes[s].xyz;
        float b = dot(oc, d), h = b * b - (dot(oc, oc) - r * r);
        if (h <= 0.0) continue;
        h = sqrt(h);
        float t0 = max(-b - h, 0.0), t1 = min(-b + h, len);
        if (t1 > t0) cover = max(cover, saturate((t1 - t0) / (0.6 * r)));
    }
    return cover;
}

// 1 near a player's body (the picked one or any other). A pixel in front of the world reference but
// far from every player is a plant that swayed between the two renders, a dropped gun or a lagged
// edge, not a person. With no player positions at all, the reference alone decides.
float NearAnyPlayer(float3 w) {
    if (pmCounts.x == 0 && pmCounts.z == 0) return 1.0;
    float d = 1e9;
    if (pmCounts.x != 0) d = CapsuleSdf(w, pmCapsule.xyz, pmCapsule.w > 1.0 ? pmCapsule.w : 74.0, 24.0);
    [loop] for (int j = 0; j < pmCounts.z; ++j)
        d = min(d, CapsuleSdf(w, pmOthers[j].xyz, pmOthers[j].w > 1.0 ? pmOthers[j].w : 74.0, 24.0));
    return d < 14.0 ? 1.0 : 0.0;
}

)HLSL"
// Split: MSVC caps one raw string literal near 16 KB (C2026); adjacent literals concatenate.
R"HLSL(
// ---- Mask Evaluation Pass (writes R8_UNORM) ----
float MaskPS(VsOut i) : SV_Target {
    int type = int(stepInfo.x + 0.5);
    float m = 0.0;
    if (type == 0) {
        // DepthRange: near=p0.x, far=p0.y, softness=p0.z, invert=p0.w
        float d = CameraDistAtUv(i.uv);
        float soft = max(p0.z, 1.0);
        float inNear = smoothstep(p0.x - soft, p0.x + soft, d);
        float inFar = 1.0 - smoothstep(p0.y - soft, p0.y + soft, d);
        m = (d < 1e29) ? (inNear * inFar) : 0.0;
        if (p0.w > 0.5) m = 1.0 - m;
    } else if (type == 1) {
        // SkyMask: cutoff=p0.x, softness=p0.y, invert=p0.z
        float d = CameraDistAtUv(i.uv);
        m = (d > 1e29) ? 1.0 : smoothstep(p0.x - max(p0.y, 1.0), p0.x + max(p0.y, 1.0), d);
        if (p0.z > 0.5) m = 1.0 - m;
    } else if (type == 2) {
        // PlayerMatte: selectedOnly=p0.x, feather=p0.y, invert=p0.z, see through smoke=p0.w,
        // first-person arms=p1.x
        float3 w;
        if (WorldAtUv(i.uv, w)) {
            float here = dot(w - cameraTime.xyz, camFwd.xyz);
            float isPlayer = (depthInfo.w > 0.5)
                ? InFrontOfWorld(w) * NearAnyPlayer(w)
                : ((here < 900.0) ? SelectedPlayerWeight(w) : 0.0);
            if (isPlayer > 0.01 && p0.x > 0.5)
                isPlayer *= SelectedPlayerWeight(w);
            // The first-person arms and gun are drawn within a few dozen units of the camera, and
            // are not in the world reference either: they filled with the players unless asked for.
            if (here < 48.0 && p1.x < 0.5) isPlayer = 0.0;
            // A player behind a smoke stays hidden, as in the game, unless the node sees through it.
            if (p0.w < 0.5) isPlayer *= 1.0 - SmokeCover(w);
            m = isPlayer;
        }
        if (p0.z > 0.5) m = 1.0 - m;
    } else if (type == 3) {
        // FogMask: start=p0.x, end=p0.y, density=p0.z, invert=p0.w
        float d = CameraDistAtUv(i.uv);
        float fog = (d > 1e29) ? 0.65 : saturate((d - p0.x) / max(p0.y - p0.x, 1.0));
        m = saturate(1.0 - exp(-fog * max(p0.z, 0.01) * 1.8));
        if (p0.w > 0.5) m = 1.0 - m;
    } else if (type == 4) {
        // LumaKey: threshold=p0.x, softness=p0.y, invert=p0.z
        float3 c = colorIn.SampleLevel(linearClamp, i.uv, 0).rgb;
        float l = dot(c, float3(0.2126, 0.7152, 0.0722));
        m = smoothstep(p0.x - max(p0.y, 0.01), p0.x + max(p0.y, 0.01), l);
        if (p0.z > 0.5) m = 1.0 - m;
    } else if (type == 5) {
        // MaskCombine: op=p0.x (0=union, 1=intersect, 2=subtract, 3=invert)
        float a = camFwd.w > 0.5 ? maskA.SampleLevel(linearClamp, i.uv, 0) : 1.0;
        float b = stepInfo.y > 0.5 ? maskB.SampleLevel(linearClamp, i.uv, 0) : 0.0;
        int op = int(p0.x + 0.5);
        if (op == 0) m = max(a, b);
        else if (op == 1) m = min(a, b);
        else if (op == 2) m = saturate(a - b);
        else m = 1.0 - a;
    } else if (type == 11) {
        // HeightMask: a band of world height (z). min=p0.x, max=p0.y, softness=p0.z, invert=p0.w
        float3 w;
        if (WorldAtUv(i.uv, w)) {
            float s = max(p0.z, 1.0);
            m = smoothstep(p0.x - s, p0.x + s, w.z) * (1.0 - smoothstep(p0.y - s, p0.y + s, w.z));
        }
        if (p0.w > 0.5) m = 1.0 - m;
    } else if (type == 12) {
        // HueKey: hue=p0.x deg, range=p0.y, softness=p0.z, min saturation=p0.w, invert=p1.x
        float3 hsv = RgbToHsv(saturate(colorIn.SampleLevel(linearClamp, i.uv, 0).rgb));
        float dh = abs(frac((hsv.x * 360.0 - p0.x) / 360.0 + 0.5) - 0.5) * 360.0;
        m = 1.0 - smoothstep(p0.y, p0.y + max(p0.z, 0.5), dh);
        m *= smoothstep(p0.w * 0.5, max(p0.w, 0.001), hsv.y);
        if (p1.x > 0.5) m = 1.0 - m;
    } else if (type == 13) {
        // RadialMask: center=p0.xy, radius=p0.z, softness=p0.w, aspect=p1.x, invert=p1.y
        float2 d = i.uv - p0.xy;
        d.x *= (dims.x / max(dims.y, 1.0)) / max(p1.x, 0.05);
        m = 1.0 - smoothstep(p0.z, p0.z + max(p0.w, 0.001), length(d));
        if (p1.y > 0.5) m = 1.0 - m;
    } else if (type == 14) {
        // GradientMask: angle=p0.x deg, position=p0.y, softness=p0.z, invert=p0.w
        float a = radians(p0.x);
        float t = dot(i.uv - 0.5, float2(cos(a), sin(a))) + 0.5;
        float s = max(p0.z, 0.001) * 0.5;
        m = smoothstep(p0.y - s, p0.y + s, t);
        if (p0.w > 0.5) m = 1.0 - m;
    } else if (type == 15) {
        // EdgeMask: where depth jumps (silhouettes, corners). threshold=p0.x, thickness=p0.y px, invert=p0.z
        float2 tx = max(p0.y, 0.5) / max(dims.xy, float2(1.0, 1.0));
        float dc = DistOrFar(i.uv);
        float gx = abs(DistOrFar(saturate(i.uv + float2(tx.x, 0))) - DistOrFar(saturate(i.uv - float2(tx.x, 0))));
        float gy = abs(DistOrFar(saturate(i.uv + float2(0, tx.y))) - DistOrFar(saturate(i.uv - float2(0, tx.y))));
        float rel = (gx + gy) / max(dc, 20.0);
        m = smoothstep(p0.x * 0.5, max(p0.x, 0.005), rel);
        if (p0.z > 0.5) m = 1.0 - m;
    } else if (type == 16) {
        // NoiseMask: drifting cloudy noise. scale=p0.x, speed=p0.y, threshold=p0.z, softness=p0.w, invert=p1.x
        float2 q = i.uv * float2(dims.x / max(dims.y, 1.0), 1.0) * max(p0.x, 0.1);
        float n = Fbm(float3(q, cameraTime.w * p0.y));
        float s = max(p0.w, 0.001) * 0.5;
        m = smoothstep(p0.z - s, p0.z + s, n);
        if (p1.x > 0.5) m = 1.0 - m;
    } else if (type == 17) {
        // PlayerRadius: a sphere around the picked player (or what the camera looks at).
        // radius=p0.x, softness=p0.y, invert=p0.z
        float3 w;
        if (WorldAtUv(i.uv, w)) {
            float3 c = pmCounts.x != 0 ? (pmCounts.y > 0 ? pmBounds.xyz : pmCapsule.xyz + float3(0, 0, 36))
                                       : FocusPoint();
            m = 1.0 - smoothstep(p0.x, p0.x + max(p0.y, 1.0), length(w - c));
        }
        if (p0.z > 0.5) m = 1.0 - m;
    } else if (type == 18) {
        // MaskFeather: blur the mask on port 0, then grow or shrink it. radius=p0.x px, grow=p0.y, invert=p0.z
        if (camFwd.w > 0.5) {
            float2 tx = max(p0.x, 0.0) / max(dims.xy, float2(1.0, 1.0));
            float acc = maskA.SampleLevel(linearClamp, i.uv, 0), wsum = 1.0;
            [unroll] for (int r = 1; r <= 3; ++r) {
                [unroll] for (int a = 0; a < 8; ++a) {
                    float ang = (float(a) + 0.5 * float(r)) * 0.78539816;
                    float2 o = float2(cos(ang), sin(ang)) * (float(r) / 3.0) * tx;
                    acc += maskA.SampleLevel(linearClamp, saturate(i.uv + o), 0);
                    wsum += 1.0;
                }
            }
            float a0 = acc / wsum, g = clamp(p0.y, -0.95, 0.95);
            float thr = 0.5 - g * 0.45;
            m = abs(g) < 0.01 ? a0 : smoothstep(thr - 0.2, thr + 0.2, a0);
        }
        if (p0.z > 0.5) m = 1.0 - m;
    } else if (type == 39) {
        // BoxMask: center=p0.xy, size=p0.zw (frame fractions), angle=p1.x deg, softness=p1.y, invert=p1.z
        float a = radians(p1.x);
        float aspect = dims.x / max(dims.y, 1.0);
        float2 d = (i.uv - p0.xy) * float2(aspect, 1.0);
        float2 q = float2(cos(a) * d.x + sin(a) * d.y, -sin(a) * d.x + cos(a) * d.y);
        float2 e = abs(q) - float2(p0.z * aspect, p0.w) * 0.5;
        float sd = length(max(e, 0.0)) + min(max(e.x, e.y), 0.0);
        float s = max(p1.y, 0.0005);
        m = 1.0 - smoothstep(-s, s, sd);
        if (p1.z > 0.5) m = 1.0 - m;
    } else if (type == 55) {
        // ColorKey: key colour=p0.xyz, tolerance=p0.w, softness=p1.x, invert=p1.y. Measured mostly on
        // colour (luma counts half), so the key follows the colour through light and shade.
        float3 c = colorIn.SampleLevel(linearClamp, i.uv, 0).rgb, k = p0.xyz;
        float lc = Luma(c), lk = Luma(k);
        float dist = length(float3((lc - lk) * 0.5, (c.b - lc) - (k.b - lk), (c.r - lc) - (k.r - lk)));
        m = 1.0 - smoothstep(p0.w, p0.w + max(p1.x, 0.001), dist);
        if (p1.y > 0.5) m = 1.0 - m;
    } else if (type == 56) {
        // NormalMask (Surface Mask): facing=p0.x (0 floors, 1 ceilings, 2 walls, 3 toward camera,
        // 4 edge-on), tolerance=p0.y, softness=p0.z, invert=p0.w. Normals rebuilt from depth.
        float3 w;
        if (WorldAtUv(i.uv, w)) {
            float3 n = NormalAtUv(i.uv, 1.5 / max(dims.xy, float2(1.0, 1.0)));
            if (dot(n, n) > 0.5) {
                float3 toCam = normalize(cameraTime.xyz - w);
                if (dot(n, toCam) < 0.0) n = -n;
                int dir = int(p0.x + 0.5);
                float s = dir == 0 ? n.z : dir == 1 ? -n.z : dir == 2 ? 1.0 - abs(n.z)
                        : dir == 3 ? dot(n, toCam) : 1.0 - abs(dot(n, toCam));
                float thr = 1.0 - saturate(p0.y), sft = max(p0.z, 0.001) * 0.5;
                m = smoothstep(thr - sft, thr + sft, s);
            }
        }
        if (p0.w > 0.5) m = 1.0 - m;
    } else if (type == 57) {
        // MaskEdge: width=p0.x px, softness=p0.y, side=p0.z (0 both, 1 inside, 2 outside), invert=p0.w.
        // Only the border of the mask on port 0: rims, outlines, edge glows.
        if (camFwd.w > 0.5) {
            float2 tx = max(p0.x, 0.5) / max(dims.xy, float2(1.0, 1.0));
            float c0 = maskA.SampleLevel(linearClamp, i.uv, 0), mn = c0, mx = c0;
            [unroll] for (int a = 0; a < 12; ++a) {
                float ang = float(a) * 0.5235988;
                float s = maskA.SampleLevel(linearClamp, saturate(i.uv + float2(cos(ang), sin(ang)) * tx), 0);
                mn = min(mn, s); mx = max(mx, s);
            }
            int side = int(p0.z + 0.5);
            float e = side == 1 ? c0 - mn : (side == 2 ? mx - c0 : mx - mn);
            m = smoothstep(0.02, 0.02 + max(p0.y, 0.01), e);
        }
        if (p0.w > 0.5) m = 1.0 - m;
    }
    return saturate(m);
}

)HLSL"
R"HLSL(
// ---- Effect Processing Pass ----
float4 EffectPS(VsOut i) : SV_Target {
    int type = int(stepInfo.x + 0.5);
    float2 texel = 1.0 / max(dims.xy, float2(1.0, 1.0));
    float4 src = colorIn.SampleLevel(linearClamp, i.uv, 0);
    float m = camFwd.w > 0.5 ? maskA.SampleLevel(linearClamp, i.uv, 0) : 1.0;
    float3 outRgb = src.rgb;

    if (type == 6) {
        // ColorGrade: sat=p0.x, contrast=p0.y, exp=p0.z, temp=p0.w, tint=p1.xyz, mix=p1.w
        float3 c = src.rgb * exp2(p0.z);
        c = (c - 0.5) * p0.y + 0.5;
        float l = dot(c, float3(0.2126, 0.7152, 0.0722));
        c = lerp(float3(l, l, l), c, p0.x);
        c.r *= (1.0 + p0.w * 0.25);
        c.b *= (1.0 - p0.w * 0.25);
        c *= p1.xyz;
        outRgb = lerp(src.rgb, saturate(c), saturate(m * p1.w));
    } else if (type == 7) {
        // Glow: thresh=p0.x, intensity=p0.y, radius=p0.z, tint=p0.w/p1.xy, mix=p1.z
        float thresh = p0.x;
        float intensity = p0.y;
        float rad = max(p0.z, 1.0);
        float3 tint = float3(p0.w, p1.x, p1.y);
        float3 accum = float3(0, 0, 0);
        float wsum = 0.0;
        [unroll] for (int r = 1; r <= 2; ++r) {
            float rr = rad * (float(r) * 0.55);
            [unroll] for (int a = 0; a < 8; ++a) {
                float ang = (float(a) + 0.5 * float(r)) * 0.78539816;
                float2 off = float2(cos(ang), sin(ang)) * rr * texel;
                float2 suv = saturate(i.uv + off);
                float3 sc = colorIn.SampleLevel(linearClamp, suv, 0).rgb;
                float sm = camFwd.w > 0.5 ? maskA.SampleLevel(linearClamp, suv, 0) : 1.0;
                float sl = dot(sc, float3(0.2126, 0.7152, 0.0722));
                float bright = smoothstep(thresh, thresh + 0.35, sl);
                float wt = (r == 1) ? 1.0 : 0.6;
                accum += sc * bright * sm * wt;
                wsum += wt;
            }
        }
        float3 bloom = (accum / max(wsum, 1e-4)) * tint * intensity;
        outRgb = saturate(src.rgb + bloom * saturate(p1.z));
    } else if (type == 8) {
        // Outline: thickness=p0.x, threshold=p0.y, color=p0.z/p0.w/p1.x, opacity=p1.y
        float th = max(p0.x, 1.0);
        float edge = 0.0;
        if (camFwd.w > 0.5) {
            float c0 = m;
            float n0 = maskA.SampleLevel(linearClamp, saturate(i.uv + float2( th,  0) * texel), 0);
            float n1 = maskA.SampleLevel(linearClamp, saturate(i.uv + float2(-th,  0) * texel), 0);
            float n2 = maskA.SampleLevel(linearClamp, saturate(i.uv + float2( 0,  th) * texel), 0);
            float n3 = maskA.SampleLevel(linearClamp, saturate(i.uv + float2( 0, -th) * texel), 0);
            float grad = max(max(abs(n0 - c0), abs(n1 - c0)), max(abs(n2 - c0), abs(n3 - c0)));
            edge = smoothstep(p0.y * 0.5, max(p0.y, 0.05), grad);
        } else {
            float dc = CameraDistAtUv(i.uv);
            float d0 = CameraDistAtUv(saturate(i.uv + float2(th, 0) * texel));
            float d1 = CameraDistAtUv(saturate(i.uv + float2(0, th) * texel));
            float rel = (abs(d0 - dc) + abs(d1 - dc)) / max(dc, 20.0);
            edge = smoothstep(p0.y * 0.25, max(p0.y * 0.6, 0.05), rel);
        }
        float3 col = float3(p0.z, p0.w, p1.x);
        outRgb = lerp(src.rgb, col, saturate(edge * p1.y));
    } else if (type == 9) {
        // Distortion: mode=p0.x (0=ripple,1=heathaze,2=radial,3=chromatic), strength=p0.y, scale=p0.z, speed=p0.w, chroma=p1.x, mix=p1.y
        int dmode = int(p0.x + 0.5);
        float str = p0.y * 0.025 * m;
        float sc = max(p0.z, 1.0);
        float t = cameraTime.w * p0.w;
        float2 off = float2(0, 0);
        if (dmode == 0) {
            float2 c = i.uv - 0.5;
            float r = length(c);
            off = normalize(c + 1e-5) * sin(r * sc * 3.14 - t * 2.5) * str;
        } else if (dmode == 1) {
            off.x = sin(i.uv.y * sc * 2.2 + t * 2.0 + sin(i.uv.x * sc)) * str * 0.65;
            off.y = cos(i.uv.x * sc * 1.8 - t * 1.6) * str * 0.45;
        } else if (dmode == 2) {
            float2 c = i.uv - 0.5;
            off = c * dot(c, c) * str * 6.0;
        }
        float2 duv = saturate(i.uv + off);
        float ch = (dmode == 3 ? str * 1.4 : str * p1.x * 0.8);
        float2 dir = normalize((i.uv - 0.5) + 1e-5) * ch;
        float3 dcol;
        dcol.r = colorIn.SampleLevel(linearClamp, saturate(duv + dir), 0).r;
        dcol.g = colorIn.SampleLevel(linearClamp, duv, 0).g;
        dcol.b = colorIn.SampleLevel(linearClamp, saturate(duv - dir), 0).b;
        outRgb = lerp(src.rgb, dcol, saturate(m * p1.y));
    }

    // The later effects: each sets `fx` and its mix; the mask and mix blend it over the input.
)HLSL"
R"HLSL(
    float3 fx = src.rgb;
    float mixv = -1.0;
    float aspect = dims.x / max(dims.y, 1.0);
    if (type == 19) {
        // Blur: radius=p0.x px, mix=p0.y
        fx = BlurAt(i.uv, p0.x); mixv = p0.y;
    } else if (type == 20) {
        // Sharpen: amount=p0.x, radius=p0.y px, mix=p0.z
        fx = src.rgb + (src.rgb - BlurAt(i.uv, max(p0.y, 0.5))) * p0.x * 2.0; mixv = p0.z;
    } else if (type == 21) {
        // Vignette: strength=p0.x, radius=p0.y, softness=p0.z, color=p0.w/p1.x/p1.y, mix=p1.z
        float2 d = (i.uv - 0.5) * float2(aspect, 1.0) / sqrt(aspect * aspect * 0.25 + 0.25);
        float v = smoothstep(p0.y - p0.z * 0.5, p0.y + p0.z * 0.5, length(d));
        fx = lerp(src.rgb, float3(p0.w, p1.x, p1.y), saturate(v * p0.x)); mixv = p1.z;
    } else if (type == 22) {
        // FilmGrain: amount=p0.x, size=p0.y px, color=p0.z, mix=p0.w
        float2 cell = floor(i.uv * dims.xy / max(p0.y, 0.5));
        float t = floor(cameraTime.w * 24.0);
        float mono = Hash21(cell + t * 7.13) - 0.5;
        float3 col = float3(Hash21(cell + t * 3.7 + 1.1), Hash21(cell + t * 5.3 + 2.2), Hash21(cell + t * 9.1 + 3.3)) - 0.5;
        float3 g = lerp(float3(mono, mono, mono), col, saturate(p0.z));
        float l = Luma(src.rgb);
        fx = src.rgb + g * p0.x * 2.0 * (0.4 + 0.6 * (1.0 - abs(l - 0.5) * 2.0)); mixv = p0.w;
    } else if (type == 23) {
        // Posterize: levels=p0.x, mix=p0.y
        float lv = max(p0.x, 2.0);
        fx = floor(saturate(src.rgb) * lv + 0.5) / lv; mixv = p0.y;
    } else if (type == 24) {
        // Pixelate: cell=p0.x px, mix=p0.y
        float c = max(p0.x, 1.0);
        float2 uvp = (floor(i.uv * dims.xy / c) + 0.5) * c / max(dims.xy, float2(1.0, 1.0));
        fx = colorIn.SampleLevel(linearClamp, saturate(uvp), 0).rgb; mixv = p0.y;
    } else if (type == 25) {
        // Duotone: shadows=p0.xyz, highlights=p0.w/p1.x/p1.y, mix=p1.z
        fx = lerp(p0.xyz, float3(p0.w, p1.x, p1.y), saturate(Luma(src.rgb))); mixv = p1.z;
    } else if (type == 26) {
        // LiftGammaGain: lift=p0.x, gamma=p0.y, gain=p0.z, saturation=p0.w, mix=p1.x
        float3 c = saturate(src.rgb);
        c = saturate(c * p0.z + p0.x * (1.0 - c));
        c = pow(c, 1.0 / max(p0.y, 0.05));
        float l = Luma(c);
        fx = lerp(float3(l, l, l), c, p0.w); mixv = p1.x;
    } else if (type == 27) {
        // HueShift: degrees=p0.x, saturation=p0.y, vibrance=p0.z, mix=p0.w
        float3 hsv = RgbToHsv(saturate(src.rgb));
        hsv.x = frac(hsv.x + p0.x / 360.0);
        hsv.y = saturate(hsv.y * p0.y);
        hsv.y = saturate(hsv.y + p0.z * hsv.y * (1.0 - hsv.y));
        fx = HsvToRgb(hsv); mixv = p0.w;
    } else if (type == 28) {
        // DepthFog: start=p0.x, end=p0.y, density=p0.z, color=p0.w/p1.x/p1.y, mix=p1.z
        float d = CameraDistAtUv(i.uv);
        float f = d > 1e29 ? 1.0 : saturate((d - p0.x) / max(p0.y - p0.x, 1.0));
        f = 1.0 - exp(-f * max(p0.z, 0.01) * 2.5);
        fx = lerp(src.rgb, float3(p0.w, p1.x, p1.y), f); mixv = p1.z;
    } else if (type == 29) {
        // LightRays: threshold=p0.x, strength=p0.y, length=p0.z, center=p0.w/p1.x, mix=p1.y
        float2 stepUv = (float2(p0.w, p1.x) - i.uv) * saturate(p0.z) / 16.0;
        float2 uv = i.uv;
        float3 acc = float3(0, 0, 0);
        float decay = 1.0;
        [unroll] for (int s = 0; s < 16; ++s) {
            uv += stepUv;
            float3 c = colorIn.SampleLevel(linearClamp, saturate(uv), 0).rgb;
            float hl = smoothstep(p0.x, 1.0, Luma(c));
            acc += c * hl * hl * decay;
            decay *= 0.92;
        }
        // 9.9 is the decay's total weight: the rays are an average of the highlights they cross.
        fx = src.rgb + acc / 9.9 * p0.y * 0.7; mixv = p1.y;
    } else if (type == 30) {
        // Scanlines: lines=p0.x, strength=p0.y, curvature=p0.z, roll=p0.w, mix=p1.x
        float2 c = i.uv - 0.5;
        c *= 1.0 + dot(c, c) * p0.z;
        float2 uv = c + 0.5;
        float inside = all(uv >= 0.0) && all(uv <= 1.0) ? 1.0 : 0.0;
        float3 col = colorIn.SampleLevel(linearClamp, saturate(uv), 0).rgb;
        float scan = sin((uv.y + cameraTime.w * p0.w * 0.05) * max(p0.x, 10.0) * 3.14159);
        fx = col * (1.0 - p0.y * (0.5 + 0.5 * scan)) * inside; mixv = p1.x;
    } else if (type == 31) {
        // Glitch: strength=p0.x, block=p0.y px, speed=p0.z, rgb split=p0.w, mix=p1.x
        float t = floor(cameraTime.w * max(p0.z, 0.1));
        float row = floor(i.uv.y * dims.y / max(p0.y, 4.0));
        float h = Hash21(float2(row, t));
        float shift = h > 1.0 - p0.x * 0.5 ? (Hash21(float2(t, row + 3.0)) - 0.5) * 0.12 * p0.x : 0.0;
        float2 uv = saturate(i.uv + float2(shift, 0));
        float split = p0.w * 0.008 * (0.3 + p0.x);
        fx = float3(colorIn.SampleLevel(linearClamp, saturate(uv + float2(split, 0)), 0).r,
                    colorIn.SampleLevel(linearClamp, uv, 0).g,
                    colorIn.SampleLevel(linearClamp, saturate(uv - float2(split, 0)), 0).b);
        mixv = p1.x;
    } else if (type == 32) {
        // Thermal: palette=p0.x, contrast=p0.y, people heat=p0.z, noise=p0.w, blur=p1.x, scope=p1.y,
        // sway=p1.z, mix=p1.w
        float rt = RealTime();
        int scope = int(p1.y + 0.5);
        float2 c = float2(0.5, 0.5) + (scope > 0 ? Sway(rt, p1.z) : float2(0, 0));
        float rim = 0.0;
        float inScope = scope > 0 ? TubeMask(i.uv, c, 1, rim) : 1.0;
        // A microbolometer resolves little: a soft picture.
        float3 img = BlurAt(i.uv, max(p1.x, 0.0));
        float d = CameraDistAtUv(i.uv);
        // A temperature stand-in: surfaces by the light they hold (sunlit walls are warm), the sky
        // cold, and everything drifting toward the air's temperature with distance.
        float heat = d > 1e29 ? 0.03 : lerp(0.08 + 0.42 * pow(saturate(Luma(img)), 1.3), 0.18, saturate(d / 9000.0) * 0.6);
        // People run hot, with the warm bloom thermal cameras show around them.
        if (p0.z > 0.001) {
            float body = PeopleAt(i.uv);
            float2 tx = 6.0 / max(dims.xy, float2(1.0, 1.0));
            float nearBody = (PeopleAt(saturate(i.uv + float2(tx.x, 0))) + PeopleAt(saturate(i.uv - float2(tx.x, 0)))
                            + PeopleAt(saturate(i.uv + float2(0, tx.y))) + PeopleAt(saturate(i.uv - float2(0, tx.y)))) * 0.25;
            // Skin and clothes are not one temperature: the picture's own shading varies the body.
            heat = lerp(heat, 0.72 + 0.26 * Luma(img), saturate(body * p0.z));
            heat += nearBody * 0.1 * p0.z * (1.0 - body);
        }
        heat = saturate((heat - 0.4) * max(p0.y, 0.05) + 0.4);
        // Sensor noise: temporal grain and faint fixed column stripes.
        float2 px = floor(i.uv * dims.xy);
        heat += ((Hash21(px + floor(rt * 30.0) * 11.3) - 0.5) * 0.06 + (Hash21(float2(px.x, 7.0)) - 0.5) * 0.02) * p0.w * 3.0;
        heat = saturate(heat);
        int pal = int(p0.x + 0.5);
        if (pal == 1) fx = float3(1.0, 1.0, 1.0) - heat;
        else if (pal == 2) {
            // Ironbow: black, deep blue, magenta, orange, yellow, white.
            float3 k0 = float3(0.0, 0.0, 0.02), k1 = float3(0.12, 0.0, 0.45), k2 = float3(0.65, 0.05, 0.55),
                   k3 = float3(0.95, 0.35, 0.05), k4 = float3(1.0, 0.82, 0.15), k5 = float3(1.0, 1.0, 0.92);
            float s = heat * 5.0;
            fx = s < 1.0 ? lerp(k0, k1, s) : s < 2.0 ? lerp(k1, k2, s - 1.0) : s < 3.0 ? lerp(k2, k3, s - 2.0)
               : s < 4.0 ? lerp(k3, k4, s - 3.0) : lerp(k4, k5, s - 4.0);
        } else if (pal == 3) fx = lerp(float3(0.03, 0.02, 0.01), float3(1.0, 0.84, 0.5), heat) + float3(0.0, 0.0, 0.0);
        else fx = float3(heat, heat, heat);
        if (scope > 0) {
            fx *= 1.0 - 0.35 * smoothstep(0.6, 1.0, rim);
            if (scope == 2) {
                // A thin red reticle: a cross with a gap and a centre dot.
                float2 dp = (i.uv - c) * dims.xy;
                float reach = dims.y * 0.3;
                float hv = step(abs(dp.x), 1.0) * step(14.0, abs(dp.y)) * step(abs(dp.y), reach);
                float hh = step(abs(dp.y), 1.0) * step(14.0, abs(dp.x)) * step(abs(dp.x), reach);
                float dotc = step(length(dp), 2.0);
                fx = lerp(fx, float3(1.0, 0.22, 0.15), saturate(hv + hh + dotc));
            }
            fx *= inScope;
        }
        mixv = p1.w;
    } else if (type == 33) {
        // NightVision: gain=p0.x, scintillation=p0.y, halo=p0.z, tube=p0.w, phosphor=p1.x, sway=p1.y, mix=p1.z
        float rt = RealTime();
        int tube = int(p0.w + 0.5);
        float2 c = float2(0.5, 0.5) + (tube > 0 ? Sway(rt, p1.y) : float2(0, 0));
        float rim = 0.0;
        float inTube = tube > 0 ? TubeMask(i.uv, c, tube, rim) : 1.0;
        if (tube == 0) rim = length((i.uv - 0.5) * float2(aspect, 1.0)) / 0.9;
        // The tube's optics: a little barrel toward the rim, and a softer picture than the screen.
        float2 suv = saturate(c + (i.uv - c) * (1.0 - 0.06 * rim * rim));
        float l = Luma(BlurAt(suv, 1.2));
        // Image intensification: shadows lifted, highlights rolled off, then the tube's steep
        // contrast, which keeps the blacks deep (a daylight scene must not wash out to mint).
        float v = pow(saturate(1.0 - exp(-l * max(p0.x, 0.1) * 1.4)), 1.8) * 1.1;
        // Bright sources bloom into wide halos.
        float halo = smoothstep(0.55, 1.0, Luma(BlurAt(suv, 10.0))) * 0.6 + smoothstep(0.45, 1.0, Luma(BlurAt(suv, 28.0))) * 0.7;
        v += halo * p0.z;
        // Automatic brightness control breathing.
        v *= 1.0 + 0.04 * (Noise3(float3(rt * 3.1, 0.0, 0.0)) - 0.5);
        // Scintillation: per-pixel sparkle at about 60 Hz, strongest in the dark.
        float2 px = floor(i.uv * dims.xy);
        float f = floor(rt * 60.0);
        float n1 = Hash21(px + f * 13.7), n2 = Hash21(px * 1.37 + f * 7.1 + 3.0);
        v += ((n1 + n2 - 1.0) * 0.35 + pow(n1, 24.0) * 2.5) * p0.y * (0.35 + (1.0 - saturate(v)));
        // The microchannel plate's faint honeycomb, and the falloff toward the tube's rim.
        v *= 1.0 - 0.035 * (0.5 + 0.5 * sin(px.x * 1.9) * sin(px.y * 1.9 + px.x * 0.95));
        v *= 1.0 - 0.45 * smoothstep(0.55, 1.0, rim);
        v = max(v, 0.0);
        // Phosphor ramp: near-black, the tube's colour in the midtones, pale where it saturates.
        // P43 green, or the cool grey-white of white-phosphor tubes.
        float3 k0 = p1.x > 0.5 ? float3(0.008, 0.01, 0.016) : float3(0.0, 0.016, 0.006);
        float3 k1 = p1.x > 0.5 ? float3(0.40, 0.45, 0.50) : float3(0.09, 0.60, 0.15);
        float3 k2 = p1.x > 0.5 ? float3(0.92, 0.96, 1.0) : float3(0.76, 1.0, 0.70);
        float sv = saturate(v);
        fx = (sv < 0.6 ? lerp(k0, k1, sv / 0.6) : lerp(k1, k2, (sv - 0.6) / 0.4)) + k2 * max(v - 1.0, 0.0) * 0.3;
        fx *= inTube;
        mixv = p1.z;
    } else if (type == 34) {
        // InkLines: threshold=p0.x, strength=p0.y, paper=p0.z, mix=p0.w
        float2 tx = 1.0 / max(dims.xy, float2(1.0, 1.0));
        float l00 = Luma(colorIn.SampleLevel(linearClamp, saturate(i.uv + tx * float2(-1, -1)), 0).rgb);
        float l10 = Luma(colorIn.SampleLevel(linearClamp, saturate(i.uv + tx * float2(0, -1)), 0).rgb);
        float l20 = Luma(colorIn.SampleLevel(linearClamp, saturate(i.uv + tx * float2(1, -1)), 0).rgb);
        float l01 = Luma(colorIn.SampleLevel(linearClamp, saturate(i.uv + tx * float2(-1, 0)), 0).rgb);
        float l21 = Luma(colorIn.SampleLevel(linearClamp, saturate(i.uv + tx * float2(1, 0)), 0).rgb);
        float l02 = Luma(colorIn.SampleLevel(linearClamp, saturate(i.uv + tx * float2(-1, 1)), 0).rgb);
        float l12 = Luma(colorIn.SampleLevel(linearClamp, saturate(i.uv + tx * float2(0, 1)), 0).rgb);
        float l22 = Luma(colorIn.SampleLevel(linearClamp, saturate(i.uv + tx * float2(1, 1)), 0).rgb);
        float gx = (l20 + 2.0 * l21 + l22) - (l00 + 2.0 * l01 + l02);
        float gy = (l02 + 2.0 * l12 + l22) - (l00 + 2.0 * l10 + l20);
        float e = smoothstep(p0.x, p0.x * 2.0 + 0.01, sqrt(gx * gx + gy * gy)) * p0.y;
        float pl = Luma(src.rgb);
        float3 base = lerp(src.rgb, float3(0.96, 0.94, 0.88) * (0.75 + 0.25 * pl), saturate(p0.z));
        fx = base * (1.0 - saturate(e)); mixv = p0.w;
    } else if (type == 35) {
        // Invert: mix=p0.x
        fx = 1.0 - saturate(src.rgb); mixv = p0.x;
    } else if (type == 36) {
        // Halftone: dot=p0.x px, angle=p0.y deg, mix=p0.z
        float a = radians(p0.y), cs = cos(a), sn = sin(a);
        float2 px = i.uv * dims.xy;
        float2 rp = float2(cs * px.x + sn * px.y, -sn * px.x + cs * px.y);
        float c = max(p0.x, 2.0);
        float2 cellC = (floor(rp / c) + 0.5) * c;
        float2 back = float2(cs * cellC.x - sn * cellC.y, sn * cellC.x + cs * cellC.y);
        float3 cc = colorIn.SampleLevel(linearClamp, saturate(back / max(dims.xy, float2(1.0, 1.0))), 0).rgb;
        float r = sqrt(saturate(1.0 - Luma(cc))) * 0.72;
        float dotv = 1.0 - smoothstep(r - 0.06, r + 0.06, length(frac(rp / c) - 0.5));
        fx = lerp(float3(0.97, 0.96, 0.92), cc * 0.45, dotv); mixv = p0.z;
    } else if (type == 37) {
        // DepthBlur: focus=p0.x, range=p0.y, max radius=p0.z px, mix=p0.w
        float d = CameraDistAtUv(i.uv);
        float coc = d > 1e29 ? 1.0 : saturate((abs(d - p0.x) - p0.y * 0.5) / max(p0.y, 1.0));
        fx = BlurAt(i.uv, p0.z * coc); mixv = p0.w;
    } else if (type == 38) {
        // ColorFill: color=p0.xyz, blend=p0.w (0 normal,1 multiply,2 screen,3 overlay,4 add), mix=p1.x
        float3 c = p0.xyz, s = saturate(src.rgb);
        int bm = int(p0.w + 0.5);
        fx = bm == 1 ? s * c
           : bm == 2 ? 1.0 - (1.0 - s) * (1.0 - c)
           : bm == 3 ? lerp(2.0 * s * c, 1.0 - 2.0 * (1.0 - s) * (1.0 - c), step(0.5, s))
           : bm == 4 ? s + c
           : c;
        mixv = p1.x;
    }
)HLSL"
R"HLSL(
    if (type == 40) {
        // LensDistortion: distortion=p0.x, dispersion=p0.y, fit=p0.z, mix=p0.w
        float2 o = i.uv - 0.5;
        float r2 = dot(o * float2(aspect, 1.0), o * float2(aspect, 1.0)) / (aspect * aspect * 0.25 + 0.25);
        float fit = p0.z > 0.5 ? 1.0 / (1.0 + max(p0.x, 0.0)) : 1.0;
        fx = float3(colorIn.SampleLevel(linearClamp, saturate(0.5 + o * (1.0 + (p0.x + p0.y) * r2) * fit), 0).r,
                    colorIn.SampleLevel(linearClamp, saturate(0.5 + o * (1.0 + p0.x * r2) * fit), 0).g,
                    colorIn.SampleLevel(linearClamp, saturate(0.5 + o * (1.0 + (p0.x - p0.y) * r2) * fit), 0).b);
        mixv = p0.w;
    } else if (type == 41) {
        // DirectionalBlur: angle=p0.x deg, length=p0.y px, mix=p0.z
        float a = radians(p0.x);
        float2 dir = float2(cos(a), sin(a)) * max(p0.y, 0.0) / max(dims.xy, float2(1.0, 1.0));
        float3 acc = float3(0, 0, 0);
        [unroll] for (int s = 0; s < 16; ++s) acc += colorIn.SampleLevel(linearClamp, saturate(i.uv + dir * (float(s) / 15.0 - 0.5)), 0).rgb;
        fx = acc / 16.0; mixv = p0.z;
    } else if (type == 42) {
        // RadialBlur: center=p0.xy, amount=p0.z, mode=p0.w (0 zoom, 1 spin), mix=p1.x
        float3 acc = float3(0, 0, 0);
        int md = int(p0.w + 0.5);
        [unroll] for (int s = 0; s < 16; ++s) {
            float t = float(s) / 15.0;
            float2 uv;
            if (md == 1) {
                float ang = (t - 0.5) * p0.z * 0.6;
                float2 d = (i.uv - p0.xy) * float2(aspect, 1.0);
                uv = p0.xy + float2(cos(ang) * d.x - sin(ang) * d.y, sin(ang) * d.x + cos(ang) * d.y) / float2(aspect, 1.0);
            } else uv = p0.xy + (i.uv - p0.xy) * (1.0 - p0.z * t);
            acc += colorIn.SampleLevel(linearClamp, saturate(uv), 0).rgb;
        }
        fx = acc / 16.0; mixv = p1.x;
    } else if (type == 43) {
        // Glare: type=p0.x, threshold=p0.y, length=p0.z, intensity=p0.w, warmth=p1.x, mix=p1.y
        int gt = int(p0.x + 0.5);
        float3 tint = lerp(float3(0.75, 0.85, 1.15), float3(1.15, 0.9, 0.7), saturate(p1.x * 0.5 + 0.5));
        float3 add = float3(0, 0, 0);
        if (gt <= 2) {
            // Streaks (anamorphic, two rays) or a star of four or six rays from every highlight.
            int rays = gt == 0 ? 2 : (gt == 1 ? 4 : 6);
            float base = gt == 0 ? 0.0 : (gt == 1 ? 0.785398 : 0.523599);
            [loop] for (int r = 0; r < rays; ++r) {
                float ang = base + 6.2831853 * float(r) / float(rays);
                float2 dir = float2(cos(ang), sin(ang) * aspect) * p0.z;
                [unroll] for (int s = 1; s <= 12; ++s) {
                    float t = float(s) / 12.0;
                    float3 cc = colorIn.SampleLevel(linearClamp, saturate(i.uv - dir * t), 0).rgb;
                    // Only true highlights throw a streak (squared, so a bright sky edge stays quiet).
                    float hl = smoothstep(p0.y, 1.0, Luma(cc));
                    add += cc * hl * hl * (1.0 - t) * (1.0 - t);
                }
            }
            // 3.51 is the falloff's weight per ray: the streaks average, they do not pile up.
            add *= 1.6 / (3.51 * float(rays));
        } else if (gt == 3) {
            // Ghosts: tinted reflections of the highlights mirrored through the centre.
            float sc[5] = { 0.45, 0.85, 1.35, -0.7, 0.2 };
            float3 tc[5] = { float3(1.0, 0.6, 0.3), float3(0.4, 0.8, 1.0), float3(0.7, 1.0, 0.5), float3(1.0, 0.4, 0.8), float3(0.9, 0.9, 1.0) };
            [unroll] for (int k = 0; k < 5; ++k) {
                float2 uvg = 0.5 - (i.uv - 0.5) * sc[k];
                float3 cc = colorIn.SampleLevel(linearClamp, saturate(uvg), 0).rgb;
                float edge = 1.0 - smoothstep(0.25, 0.7, length(uvg - 0.5));
                add += cc * smoothstep(p0.y, p0.y + 0.15, Luma(cc)) * tc[k] * edge * 0.35;
            }
        } else {
            // Fog glow: a wide soft bloom.
            float3 bb = BlurAt(i.uv, p0.z * 120.0);
            add = bb * smoothstep(p0.y - 0.2, p0.y + 0.2, Luma(bb));
        }
        fx = src.rgb + add * tint * p0.w; mixv = p1.y;
    } else if (type == 44) {
        // Halation: threshold=p0.x, radius=p0.y px, intensity=p0.z, color=p0.w/p1.x/p1.y, mix=p1.z.
        // Film's red-orange glow that bleeds AROUND highlights, less on them.
        float hb = smoothstep(p0.x, p0.x + 0.25, Luma(BlurAt(i.uv, p0.y * 0.5))) * 0.5
                 + smoothstep(p0.x, p0.x + 0.25, Luma(BlurAt(i.uv, p0.y))) * 0.7;
        float self = smoothstep(p0.x, 1.0, Luma(src.rgb));
        float3 col = float3(p0.w, p1.x, p1.y) * hb * p0.z * (1.0 - 0.5 * self);
        fx = 1.0 - (1.0 - src.rgb) * (1.0 - saturate(col)); mixv = p1.z;
    } else if (type == 45) {
        // ChromaticAberration: amount=p0.x px, mode=p0.y (0 radial, 1 lateral), clear centre=p0.z, mix=p0.w
        float2 o = i.uv - 0.5;
        float2 dir = int(p0.y + 0.5) == 1 ? float2(1.0, 0.0) : normalize(o + 1e-5);
        float amt = p0.x / max(dims.x, 1.0) * (p0.z > 0.5 ? saturate(length(o * float2(aspect, 1.0)) * 2.2) : 1.0);
        fx = float3(colorIn.SampleLevel(linearClamp, saturate(i.uv + dir * amt), 0).r, src.g,
                    colorIn.SampleLevel(linearClamp, saturate(i.uv - dir * amt), 0).b);
        mixv = p0.w;
    } else if (type == 46) {
        // Kuwahara: radius=p0.x px, mix=p0.y. Each pixel takes the mean of whichever of its four
        // quadrants varies least, so flat regions merge and edges stay: a painted look.
        float2 tx = max(p0.x, 1.0) / 3.0 / max(dims.xy, float2(1.0, 1.0));
        float best = 1e9;
        [unroll] for (int q = 0; q < 4; ++q) {
            float2 sgn = float2((q & 1) ? 1.0 : -1.0, (q & 2) ? 1.0 : -1.0);
            float3 s1 = float3(0, 0, 0), s2 = float3(0, 0, 0);
            [unroll] for (int y = 0; y < 4; ++y) {
                [unroll] for (int x = 0; x < 4; ++x) {
                    float3 cc = colorIn.SampleLevel(linearClamp, saturate(i.uv + float2(x, y) * sgn * tx), 0).rgb;
                    s1 += cc; s2 += cc * cc;
                }
            }
            float3 mean = s1 / 16.0, vr = s2 / 16.0 - mean * mean;
            float v = vr.r + vr.g + vr.b;
            if (v < best) { best = v; fx = mean; }
        }
        mixv = p0.y;
    }
)HLSL"
R"HLSL(
    if (type == 47) {
        // CelShade (a comic look like Borderlands): bands=p0.x, outline=p0.y px, outline strength=p0.z,
        // depth edges=p0.w, normal edges=p1.x, hatching=p1.y, saturation=p1.z, mix=p1.w
        float2 tx = max(p0.y, 0.5) / max(dims.xy, float2(1.0, 1.0));
        // Shading in flat bands; the colour kept and pushed more saturated, like comic print.
        float l = Luma(src.rgb);
        float bands = max(p0.x, 2.0);
        float lb = lerp((floor(l * bands) + 0.5) / bands, l, 0.15);
        float3 col = src.rgb * (lb / max(l, 1e-3));
        float lc = Luma(col);
        col = saturate(lerp(float3(lc, lc, lc), col, p1.z));
        // Ink where depth jumps (silhouettes) and where the surface turns (creases).
        float dc = DistOrFar(i.uv);
        float lap = abs(DistOrFar(saturate(i.uv - float2(tx.x, 0))) + DistOrFar(saturate(i.uv + float2(tx.x, 0)))
                      + DistOrFar(saturate(i.uv - float2(0, tx.y))) + DistOrFar(saturate(i.uv + float2(0, tx.y))) - 4.0 * dc) / max(dc, 20.0);
        float edgeD = smoothstep(p0.w, p0.w * 2.0 + 0.001, lap);
        float2 ntx = 1.0 / max(dims.xy, float2(1.0, 1.0));
        float3 nc = NormalAtUv(i.uv, ntx);
        float crease = (1.0 - dot(NormalAtUv(saturate(i.uv - float2(tx.x, 0)), ntx), NormalAtUv(saturate(i.uv + float2(tx.x, 0)), ntx)))
                     + (1.0 - dot(NormalAtUv(saturate(i.uv - float2(0, tx.y)), ntx), NormalAtUv(saturate(i.uv + float2(0, tx.y)), ntx)));
        float edgeN = dot(nc, nc) < 0.5 ? 0.0 : smoothstep(p1.x, p1.x * 2.0 + 0.001, crease * 0.5);
        float ink = saturate(max(edgeD, edgeN) * p0.z);
        // Pen hatching in the darker bands: one diagonal, then a cross-hatch in the darkest.
        float2 px = i.uv * dims.xy;
        float h1 = 1.0 - step(0.2, frac((px.x + px.y) / 6.0));
        float h2 = 1.0 - step(0.2, frac((px.x - px.y) / 6.0));
        float darkness = 1.0 - lb;
        col *= 1.0 - saturate((darkness > 0.55 ? h1 : 0.0) + (darkness > 0.75 ? h2 : 0.0)) * p1.y * 0.6;
        fx = lerp(col, float3(0.04, 0.03, 0.03), ink); mixv = p1.w;
    } else if (type == 48) {
        // Refraction: strength=p0.x, scale=p0.y, speed=p0.z, chromatic=p0.w, source=p1.x
        // (0 moving noise: heat, water, glass; 1 the edges of the mask: a lens-like bend), mix=p1.y
        float2 off;
        if (int(p1.x + 0.5) == 1 && camFwd.w > 0.5) {
            float2 tx = 3.0 / max(dims.xy, float2(1.0, 1.0));
            float gx = maskA.SampleLevel(linearClamp, saturate(i.uv + float2(tx.x, 0)), 0) - maskA.SampleLevel(linearClamp, saturate(i.uv - float2(tx.x, 0)), 0);
            float gy = maskA.SampleLevel(linearClamp, saturate(i.uv + float2(0, tx.y)), 0) - maskA.SampleLevel(linearClamp, saturate(i.uv - float2(0, tx.y)), 0);
            off = float2(gx, gy) * p0.x * 0.03;
            m = 1.0;   // the bend lives around the mask's edges, not only inside it
        } else {
            float3 q = float3(i.uv * float2(aspect, 1.0) * max(p0.y, 0.1), cameraTime.w * p0.z);
            off = (float2(Fbm(q), Fbm(q + float3(5.2, 1.3, 0.0))) - 0.5) * p0.x * 0.04;
        }
        float2 duv = saturate(i.uv + off), ch = off * p0.w;
        fx = float3(colorIn.SampleLevel(linearClamp, saturate(duv + ch), 0).r, colorIn.SampleLevel(linearClamp, duv, 0).g,
                    colorIn.SampleLevel(linearClamp, saturate(duv - ch), 0).b);
        mixv = p1.y;
    } else if (type == 49) {
        // FilmDamage: scratches=p0.x, dust=p0.y, flicker=p0.z, gate weave=p0.w, age=p1.x, mix=p1.y
        float t = cameraTime.w, fr = floor(t * 24.0);
        // Gate weave: the frame drifts by a pixel or a few.
        float2 weave = (float2(Noise3(float3(t * 1.7, 0, 0)), Noise3(float3(0, t * 1.3, 2.0))) - 0.5) * p0.w * 6.0 / max(dims.xy, float2(1.0, 1.0));
        float3 c = colorIn.SampleLevel(linearClamp, saturate(i.uv + weave), 0).rgb;
        // Flicker: exposure wobbles frame to frame.
        c *= 1.0 + (Hash21(float2(fr, 3.1)) - 0.5) * 0.25 * p0.z;
        // Scratches: a few thin vertical lines that live for a while and drift.
        float scr = 0.0;
        [unroll] for (int k = 0; k < 3; ++k) {
            float seed = floor(t * (0.8 + k * 0.37)) + k * 17.0;
            float on = step(1.0 - p0.x * 0.8, Hash21(float2(seed, 1.0 + k)));
            float x = Hash21(float2(seed, 5.0 + k)) + sin(t * 3.0 + k) * 0.002;
            float w = 0.6 / max(dims.x, 1.0) * (1.0 + Hash21(float2(seed, 9.0)) * 1.5);
            float ln = (1.0 - smoothstep(w * 0.5, w * 1.5, abs(i.uv.x - x))) * (0.6 + 0.4 * Noise3(float3(i.uv.y * 40.0, seed, 0)));
            scr = max(scr, ln * on);
        }
        // Dust: dark specks that change every frame.
        float2 g = i.uv * float2(aspect, 1.0) * 60.0, cell = floor(g);
        float dust = step(1.0 - p0.y * 0.012, Hash21(cell + fr * 7.7));
        dust *= 1.0 - smoothstep(0.05, 0.35 * Hash21(cell + 2.0) + 0.1, length(frac(g) - 0.5));
        // Age: faded, warm, lifted blacks.
        float lc = Luma(c);
        float3 aged = lerp(c, float3(1.0, 0.9, 0.72) * (lc * 0.9 + 0.06), p1.x);
        fx = aged * (1.0 - dust * 0.85) + scr * 0.5; mixv = p1.y;
    } else if (type == 50) {
        // TiltShift: focus y=p0.x, band=p0.y, blur=p0.z px, saturation=p0.w, mix=p1.x
        float amt = smoothstep(p0.y * 0.5, p0.y * 0.5 + 0.25, abs(i.uv.y - p0.x));
        float3 bb = BlurAt(i.uv, p0.z * amt);
        float l = Luma(bb);
        fx = lerp(float3(l, l, l), bb, p0.w); mixv = p1.x;
    } else if (type == 51) {
        // Tonemap: exposure=p0.x EV, contrast=p0.y, curve=p0.z (0 filmic, 1 soft, 2 punchy), mix=p0.w
        // The input is display-ready, so the curves run on linear light and go back to display.
        int cv = int(p0.z + 0.5);
        float3 lin = pow(saturate(src.rgb), 2.2) * exp2(p0.x);
        float3 c;
        if (cv == 0) { lin *= 1.6; c = saturate((lin * (2.51 * lin + 0.03)) / (lin * (2.43 * lin + 0.59) + 0.14)); }
        else if (cv == 1) { lin *= 1.8; c = lin / (1.0 + lin); }
        else c = saturate(lin);
        c = pow(saturate(c), 1.0 / 2.2);
        if (cv == 2) c = c * c * (3.0 - 2.0 * c);
        fx = saturate((c - 0.5) * p0.y + 0.5); mixv = p0.w;
    } else if (type == 52) {
        // SplitTone: shadow hue=p0.x, shadow amount=p0.y, highlight hue=p0.z, highlight amount=p0.w,
        // balance=p1.x, mix=p1.y
        float l = Luma(src.rgb);
        float bal = clamp(p1.x, -1.0, 1.0) * 0.5;
        float ws = 1.0 - smoothstep(0.0, 0.55 + bal, l), wh = smoothstep(0.45 + bal, 1.0, l);
        float3 sh = HsvToRgb(float3(frac(p0.x / 360.0), 1.0, 1.0)), hi = HsvToRgb(float3(frac(p0.z / 360.0), 1.0, 1.0));
        float3 c = src.rgb;
        c = lerp(c, c * (sh * 0.7 + 0.3) * 1.2 + sh * 0.04, ws * p0.y);
        c = lerp(c, c * (hi * 0.5 + 0.5) + hi * 0.06, wh * p0.w);
        fx = c * (l / max(Luma(c), 1e-3)); mixv = p1.y;
    } else if (type == 53) {
        // Mirror: mode=p0.x (0 left to right, 1 top to bottom, 2 kaleidoscope), segments=p0.y, angle=p0.z, mix=p0.w
        int md = int(p0.x + 0.5);
        float2 uv = i.uv;
        if (md == 0) uv.x = uv.x > 0.5 ? 1.0 - uv.x : uv.x;
        else if (md == 1) uv.y = uv.y > 0.5 ? 1.0 - uv.y : uv.y;
        else {
            float2 d = (uv - 0.5) * float2(aspect, 1.0);
            float r = length(d), a = atan2(d.y, d.x) + radians(p0.z);
            float seg = 6.2831853 / max(round(p0.y), 2.0);
            a = abs(fmod(a + 100.0 * seg, seg) - seg * 0.5);
            uv = 0.5 + float2(cos(a), sin(a)) * r / float2(aspect, 1.0);
        }
        fx = colorIn.SampleLevel(linearClamp, saturate(uv), 0).rgb; mixv = p0.w;
    } else if (type == 54) {
        // AnalogDamage (VHS): tracking=p0.x, noise=p0.y, color bleed=p0.z, wobble=p0.w, mix=p1.x.
        // On the wall clock: a paused tape still jitters.
        float rt = RealTime(), y = i.uv.y;
        float wob = (Noise3(float3(y * 6.0, rt * 1.3, 0.0)) - 0.5) * 0.006 * p0.w
                  + (Hash21(float2(floor(y * dims.y), floor(rt * 50.0))) - 0.5) * 0.0015 * p0.w;
        // A band of torn, snowy lines rolling up the frame, and the bent head-switching lines at the bottom.
        float inBand = 1.0 - smoothstep(0.0, 0.05, abs(y - (1.0 - frac(rt * 0.07))));
        float tear = inBand * (Hash21(float2(floor(y * dims.y * 0.5), floor(rt * 30.0))) - 0.3) * 0.04 * p0.x;
        float head = smoothstep(0.965, 1.0, y) * 0.02 * p0.x;
        float2 uv = saturate(float2(i.uv.x + wob + tear + head, y));
        // Chroma smeared sideways (tape has little colour bandwidth); luma stays sharper.
        float2 bl = float2(p0.z * 6.0 / max(dims.x, 1.0), 0.0);
        float3 a = colorIn.SampleLevel(linearClamp, uv, 0).rgb;
        float3 cs = (colorIn.SampleLevel(linearClamp, saturate(uv - bl * 2.0), 0).rgb + colorIn.SampleLevel(linearClamp, saturate(uv - bl), 0).rgb
                   + a + colorIn.SampleLevel(linearClamp, saturate(uv + bl), 0).rgb) * 0.25;
        float3 c = saturate(Luma(a) + (cs - Luma(cs)));
        float sn = Hash21(floor(i.uv * dims.xy * float2(0.5, 1.0)) + floor(rt * 60.0) * 3.1);
        c += (sn - 0.5) * 0.12 * p0.y + inBand * step(0.7, sn) * 0.5 * p0.x;
        float lc = Luma(c);
        fx = lerp(c, float3(lc, lc, lc), 0.15) * 0.92 + 0.04; mixv = p1.x;
    }
)HLSL"
R"HLSL(
    if (type == 58) {
        // Silhouette: colour=p0.xyz, detail=p0.w, rim=p1.x px, rim strength=p1.y, mix=p1.z.
        // A flat fill -- wire a Player Matte to the mask for solid-colour players -- with some of the
        // picture's own shading if asked, and a bright rim along the inside of the mask's edge.
        float l = Luma(src.rgb);
        float3 col = p0.xyz * lerp(1.0, saturate(0.35 + l * 1.3), saturate(p0.w));
        float rim = 0.0;
        if (camFwd.w > 0.5 && p1.x > 0.01) {
            float2 tx = p1.x / max(dims.xy, float2(1.0, 1.0));
            float mn = 1.0;
            [unroll] for (int a = 0; a < 8; ++a) {
                float ang = float(a) * 0.785398;
                mn = min(mn, maskA.SampleLevel(linearClamp, saturate(i.uv + float2(cos(ang), sin(ang)) * tx), 0));
            }
            rim = saturate(m - mn);
        }
        fx = lerp(col, lerp(col, float3(1.0, 1.0, 1.0), 0.7), saturate(rim * p1.y * 1.5)); mixv = p1.z;
    } else if (type == 59) {
        // Letterbox: aspect=p0.x, softness=p0.y px, offset=p0.z, colour=p0.w/p1.x/p1.y, mix=p1.z.
        // Bars top and bottom for a wider aspect, at the sides for a narrower one. Not masked.
        float a = max(p0.x, 0.1), bar;
        if (a >= aspect) {
            float h = aspect / a;
            float e = abs(i.uv.y - 0.5 - p0.z * (1.0 - h) * 0.5) - h * 0.5;
            float s = max(p0.y, 0.0) / max(dims.y, 1.0);
            bar = s > 0.0 ? smoothstep(-s, s, e) : step(0.0, e);
        } else {
            float w = a / aspect;
            float e = abs(i.uv.x - 0.5 - p0.z * (1.0 - w) * 0.5) - w * 0.5;
            float s = max(p0.y, 0.0) / max(dims.x, 1.0);
            bar = s > 0.0 ? smoothstep(-s, s, e) : step(0.0, e);
        }
        fx = lerp(src.rgb, float3(p0.w, p1.x, p1.y), bar); m = 1.0; mixv = p1.z;
    } else if (type == 60) {
        // CameraShake: amount=p0.x px, rotation=p0.y deg, speed=p0.z, zoom to hide edges=p0.w,
        // motion blur=p1.x, mix=p1.y. A handheld drift on the demo clock, so a render repeats it.
        float t = cameraTime.w * max(p0.z, 0.0), t2 = t - 0.04 * max(p0.z, 0.0);
        float2 sh = float2(Fbm(float3(t, 1.3, 0.0)), Fbm(float3(4.7, t, 0.0))) - 0.5;
        float2 sh2 = float2(Fbm(float3(t2, 1.3, 0.0)), Fbm(float3(4.7, t2, 0.0))) - 0.5;
        float rot = (Fbm(float3(t * 0.8, 9.2, 0.0)) - 0.5) * radians(p0.y) * 2.0;
        float2 px2uv = 2.0 * p0.x / max(dims.xy, float2(1.0, 1.0));
        float2 off = sh * px2uv, vel = (sh - sh2) * px2uv * p1.x * 4.0;
        float zoom = p0.w > 0.5 ? 1.0 + 2.0 * p0.x / max(dims.y, 1.0) + abs(radians(p0.y)) * 0.6 : 1.0;
        float cs = cos(rot), sn = sin(rot);
        float3 acc = float3(0, 0, 0);
        [unroll] for (int s = 0; s < 6; ++s) {
            float2 d = (i.uv - 0.5) * float2(aspect, 1.0);
            d = float2(cs * d.x - sn * d.y, sn * d.x + cs * d.y) / zoom;
            acc += colorIn.SampleLevel(linearClamp, saturate(0.5 + d / float2(aspect, 1.0) + off + vel * (float(s) / 5.0 - 0.5)), 0).rgb;
        }
        fx = acc / 6.0; mixv = p1.y;
    } else if (type == 61) {
        // Warp: mode=p0.x (0 ripple, 1 swirl, 2 wave, 3 bulge, 4 pinch), strength=p0.y, radius=p0.z,
        // center=p0.w/p1.x, frequency=p1.y, speed=p1.z, mix=p1.w
        int md = int(p0.x + 0.5);
        float2 c = float2(p0.w, p1.x);
        float2 d = (i.uv - c) * float2(aspect, 1.0);
        float r = length(d), fall = saturate(1.0 - r / max(p0.z, 0.01));
        float t = cameraTime.w * p1.z;
        float2 nd = d;
        if (md == 0) nd = d + normalize(d + 1e-6) * sin(r * p1.y * 6.2832 - t * 4.0) * 0.02 * p0.y * fall;
        else if (md == 1) {
            float ang = p0.y * 3.0 * fall * fall, cs = cos(ang), sn = sin(ang);
            nd = float2(cs * d.x - sn * d.y, sn * d.x + cs * d.y);
        } else if (md == 2) nd = d + float2(sin(d.y * p1.y * 6.2832 + t * 3.0), sin(d.x * p1.y * 6.2832 + t * 2.3)) * 0.01 * p0.y;
        else if (md == 3) nd = d * (1.0 - p0.y * 0.6 * fall * fall);
        else nd = d * (1.0 + p0.y * 0.6 * fall * fall);
        fx = colorIn.SampleLevel(linearClamp, saturate(c + nd / float2(aspect, 1.0)), 0).rgb; mixv = p1.w;
    } else if (type == 62) {
        // Transform: zoom=p0.x, rotate=p0.y deg, offset=p0.zw, edges=p1.x (0 stretch, 1 mirror, 2 black), mix=p1.y
        float2 d = (i.uv - 0.5 - p0.zw) * float2(aspect, 1.0);
        float a = -radians(p0.y), cs = cos(a), sn = sin(a);
        d = float2(cs * d.x - sn * d.y, sn * d.x + cs * d.y) / max(p0.x, 0.05);
        float2 uv = 0.5 + d / float2(aspect, 1.0);
        int ed = int(p1.x + 0.5);
        float inside = all(uv >= 0.0) && all(uv <= 1.0) ? 1.0 : 0.0;
        float2 suv = ed == 1 ? 1.0 - abs(frac(uv * 0.5) * 2.0 - 1.0) : saturate(uv);
        fx = colorIn.SampleLevel(linearClamp, suv, 0).rgb * (ed == 2 ? inside : 1.0); mixv = p1.y;
    } else if (type == 63) {
        // EchoTrails: persistence=p0.x, blend=p0.y (0 lighten, 1 add, 2 smear), mix=p0.z. Two passes
        // (EffectNodesRender.cpp): refInfo.z = 2 keeps what the mask lets in and fades the rest into
        // the trail buffer; refInfo.z = 1 lays that trail over the picture, also outside the mask.
        float3 hist = refInfo.w > 0.5 ? history.SampleLevel(linearClamp, i.uv, 0).rgb : float3(0, 0, 0);
        int bm = int(p0.y + 0.5);
        float pers = saturate(p0.x);
        if (refInfo.z > 1.5) {
            float3 inc = src.rgb * m, tr;
            if (bm == 0) tr = max(inc, hist * pers);
            else if (bm == 1) tr = inc * 0.5 + hist * pers;
            else tr = lerp(hist * pers, src.rgb, m * (1.0 - pers));
            return float4(tr, 1.0);
        }
        if (refInfo.z > 0.5) {
            fx = bm == 0 ? max(src.rgb, hist) : bm == 1 ? src.rgb + hist * 0.6 : lerp(src.rgb, hist, pers * (1.0 - m));
            m = 1.0;
        }
        mixv = p0.z;
    } else if (type == 64) {
        // StopMotion: frames per second=p0.x, mix=p0.y. refInfo.z = 1 shows the held frame; the
        // renderer refreshes it at that rate on the demo clock.
        if (refInfo.z > 0.5 && refInfo.w > 0.5) fx = history.SampleLevel(linearClamp, i.uv, 0).rgb;
        mixv = p0.y;
    }
)HLSL"
R"HLSL(
    if (type == 65) {
        // ColorRamp: palette=p0.x, offset=p0.y, contrast=p0.z, reverse=p0.w, mix=p1.x (Blender's
        // Color Ramp on the picture's brightness).
        float t = (Luma(src.rgb) - 0.5) * p0.z + 0.5 + p0.y;
        if (p0.w > 0.5) t = 1.0 - t;
        fx = Palette(int(p0.x + 0.5), t); mixv = p1.x;
    } else if (type == 66) {
        // BokehBlur: radius=p0.x px, threshold=p0.y, highlight boost=p0.z, blades=p0.w (0 round),
        // rotation=p1.x deg, mix=p1.y. Highlights weigh more, so they open into bright discs or polygons.
        float2 tx = max(p0.x, 0.0) / max(dims.xy, float2(1.0, 1.0));
        int blades = int(p0.w + 0.5);
        float rotA = radians(p1.x), seg = 6.2831853 / max(float(blades), 3.0);
        float3 acc = float3(0, 0, 0);
        float wsum = 0.0;
        [loop] for (int k = 0; k < 48; ++k) {
            float rr = sqrt((float(k) + 0.5) / 48.0), ang = float(k) * 2.39996323;
            float edge = 1.0;
            if (blades >= 3) edge = cos(seg * 0.5) / cos(fmod(ang - rotA + 100.0 * seg, seg) - seg * 0.5);
            float3 cc = colorIn.SampleLevel(linearClamp, saturate(i.uv + float2(cos(ang), sin(ang)) * rr * edge * tx), 0).rgb;
            float hl = smoothstep(p0.y, 1.0, Luma(cc));
            float w = 1.0 + p0.z * hl * hl * 4.0;
            acc += cc * w; wsum += w;
        }
        fx = acc / wsum; mixv = p1.y;
    } else if (type == 67) {
        // Filter (Blender): type=p0.x (0 soften, 1 sharpen, 2 laplace, 3 sobel, 4 prewitt, 5 kirsch,
        // 6 emboss), strength=p0.y, mix=p0.z
        float2 tx = 1.0 / max(dims.xy, float2(1.0, 1.0));
        float3 s[9];
        [unroll] for (int y = 0; y < 3; ++y) {
            [unroll] for (int x = 0; x < 3; ++x)
                s[y * 3 + x] = colorIn.SampleLevel(linearClamp, saturate(i.uv + float2(x - 1, y - 1) * tx), 0).rgb;
        }
        int ft = int(p0.x + 0.5);
        float k = p0.y;
        float3 ring = s[0] + s[1] + s[2] + s[3] + s[5] + s[6] + s[7] + s[8];
        if (ft == 0) fx = lerp(s[4], (s[0] + s[2] + s[6] + s[8] + 2.0 * (s[1] + s[3] + s[5] + s[7]) + 4.0 * s[4]) / 16.0, saturate(k));
        else if (ft == 1) fx = s[4] + (s[4] * 8.0 - ring) * 0.25 * k;
        else if (ft == 2) fx = abs(s[4] * 8.0 - ring) * k;
        else if (ft == 3 || ft == 4) {
            float c = ft == 3 ? 2.0 : 1.0;
            float3 gx = (s[2] + c * s[5] + s[8]) - (s[0] + c * s[3] + s[6]);
            float3 gy = (s[6] + c * s[7] + s[8]) - (s[0] + c * s[1] + s[2]);
            fx = sqrt(gx * gx + gy * gy) * k;
        } else if (ft == 5) {
            // Kirsch: the strongest of the eight compass kernels.
            const int rg[8] = { 0, 1, 2, 5, 8, 7, 6, 3 };
            float l[8], tot = 0.0;
            [unroll] for (int q = 0; q < 8; ++q) { l[q] = Luma(s[rg[q]]); tot += l[q]; }
            float best = 0.0;
            [unroll] for (int d = 0; d < 8; ++d) best = max(best, 8.0 * (l[d] + l[(d + 1) & 7] + l[(d + 2) & 7]) - 3.0 * tot);
            float v = best / 15.0 * k;
            fx = float3(v, v, v);
        } else {
            float v = 0.5 + (Luma(s[8]) - Luma(s[0])) * 2.0 * k;
            fx = float3(v, v, v);
        }
        mixv = p0.z;
    } else if (type == 68) {
        // ContrastPop: amount=p0.x, radius=p0.y px, mix=p0.z. Local contrast (Resolve's Contrast Pop):
        // the picture against a blurred copy of itself, kept off the deepest shadows and highlights.
        float l = Luma(src.rgb);
        float detail = l - Luma(BlurAt(i.uv, max(p0.y, 1.0)));
        float l2 = saturate(l + detail * p0.x * 2.0 * saturate(1.0 - abs(l - 0.5) * 1.6));
        fx = src.rgb * (l2 / max(l, 1e-3)); mixv = p0.z;
    } else if (type == 69) {
        // JpegDamage: block=p0.x px, quality=p0.y, color bleed=p0.z, corruption=p0.w, mix=p1.x.
        // Each block rebuilt from its mean and slope only, its tones quantized; chroma at twice the
        // block size; some blocks show a neighbour's content, re-rolled a few times a second.
        float B = max(p0.x, 2.0);
        float2 px = i.uv * dims.xy, cell = floor(px / B), f = frac(px / B) - 0.5;
        float2 cuv = (cell + 0.5) * B / dims.xy, hb = B * 0.35 / dims.xy;
        float3 c00 = colorIn.SampleLevel(linearClamp, saturate(cuv - hb), 0).rgb;
        float3 c11 = colorIn.SampleLevel(linearClamp, saturate(cuv + hb), 0).rgb;
        float3 c10 = colorIn.SampleLevel(linearClamp, saturate(cuv + float2(hb.x, -hb.y)), 0).rgb;
        float3 c01 = colorIn.SampleLevel(linearClamp, saturate(cuv + float2(-hb.x, hb.y)), 0).rgb;
        float3 lowf = (c00 + c10 + c01 + c11) * 0.25 + ((c10 + c11) - (c00 + c01)) * 0.5 * f.x / 0.7 + ((c01 + c11) - (c00 + c10)) * 0.5 * f.y / 0.7;
        float q = saturate(p0.y), levels = lerp(4.0, 48.0, q);
        float3 c = lerp(floor(lowf * levels + 0.5) / levels, src.rgb, q * q * 0.6);
        float3 cs = colorIn.SampleLevel(linearClamp, saturate((floor(px / (B * 2.0)) + 0.5) * B * 2.0 / dims.xy), 0).rgb;
        c = lerp(c, Luma(c) + (cs - Luma(cs)), saturate(p0.z));
        float fr = floor(cameraTime.w * 8.0);
        if (Hash21(cell + fr * 3.17) < p0.w * 0.25) {
            float2 other = cell + float2(floor((Hash21(cell + 7.0 + fr) - 0.5) * 6.0), 0.0);
            c = colorIn.SampleLevel(linearClamp, saturate((other + frac(px / B)) * B / dims.xy), 0).rgb
              * float3(1.0 + (Hash21(cell + 2.0) - 0.5), 1.0, 1.0 + (Hash21(cell + 5.0) - 0.5));
        }
        fx = c; mixv = p1.x;
    }
)HLSL"
R"HLSL(
    if (type == 70) {
        // Watercolor: radius=p0.x px, edge darkening=p0.y, paper=p0.z, bleed=p0.w, saturation=p1.x, mix=p1.y.
        // Soft, wobbling washes with flattened tones, pigment pooling where the tone changes, and a
        // cold-pressed paper grain that shows white in the lightest washes.
        float2 q = i.uv * float2(aspect, 1.0) * 18.0;
        float2 wob = (float2(Noise3(float3(q, 1.0)), Noise3(float3(q + 9.1, 2.0))) - 0.5) * p0.w * 8.0 / max(dims.xy, float2(1.0, 1.0));
        float3 base = BlurAt(saturate(i.uv + wob), max(p0.x, 0.5));
        base = lerp(base, floor(base * 7.0 + 0.5) / 7.0, 0.35);
        float lb = Luma(base);
        base = saturate(lerp(float3(lb, lb, lb), base, p1.x));
        float2 tx = max(p0.x, 1.0) / max(dims.xy, float2(1.0, 1.0));
        float g = abs(Luma(colorIn.SampleLevel(linearClamp, saturate(i.uv + float2(tx.x, 0)), 0).rgb) - Luma(colorIn.SampleLevel(linearClamp, saturate(i.uv - float2(tx.x, 0)), 0).rgb))
                + abs(Luma(colorIn.SampleLevel(linearClamp, saturate(i.uv + float2(0, tx.y)), 0).rgb) - Luma(colorIn.SampleLevel(linearClamp, saturate(i.uv - float2(0, tx.y)), 0).rgb));
        base *= 1.0 - saturate(g * 3.0) * p0.y * 0.6;
        float paper = Fbm(float3(i.uv * float2(aspect, 1.0) * 140.0, 0.5));
        base *= 1.0 - (paper - 0.5) * p0.z * 0.35;
        fx = lerp(base, float3(0.98, 0.97, 0.94), smoothstep(0.82, 1.0, lb) * p0.z * 0.6); mixv = p1.y;
    } else if (type == 71) {
        // PencilSketch: lines=p0.x, width=p0.y px, shading=p0.z, colour=p0.w, paper=p1.x, mix=p1.y.
        // Graphite lines on the picture's edges, broken like a real stroke, hatching that thickens
        // and crosses with darkness, on paper.
        float2 tx = max(p0.y, 0.5) / max(dims.xy, float2(1.0, 1.0));
        float l00 = Luma(colorIn.SampleLevel(linearClamp, saturate(i.uv - tx), 0).rgb);
        float l22 = Luma(colorIn.SampleLevel(linearClamp, saturate(i.uv + tx), 0).rgb);
        float l20 = Luma(colorIn.SampleLevel(linearClamp, saturate(i.uv + float2(tx.x, -tx.y)), 0).rgb);
        float l02 = Luma(colorIn.SampleLevel(linearClamp, saturate(i.uv + float2(-tx.x, tx.y)), 0).rgb);
        float e = length(float2(l20 + l22 - l00 - l02, l02 + l22 - l00 - l20));
        float2 px = i.uv * dims.xy;
        float ln = saturate(e * 3.0 * p0.x) * (0.75 + 0.25 * Noise3(float3(px * 0.15, 3.0)));
        float l = Luma(src.rgb), dk = 1.0 - l;
        float jit = (Noise3(float3(px * 0.02, 1.0)) - 0.5) * 3.0;
        float h1 = step(frac((px.x + px.y + jit) / 7.0), dk * 0.6);
        float h2 = dk > 0.55 ? step(frac((px.x - px.y + jit) / 7.0), (dk - 0.4) * 0.6) : 0.0;
        float graphite = saturate(ln + saturate(h1 + h2) * p0.z * 0.7);
        float3 paperC = float3(0.97, 0.96, 0.92) * (0.94 + (Fbm(float3(px * 0.03, 7.0)) - 0.5) * 0.12 * p1.x);
        float3 lead = lerp(float3(0.18, 0.18, 0.2), src.rgb * 0.6, saturate(p0.w));
        fx = lerp(paperC, lead, graphite);
        fx = lerp(fx, fx * lerp(float3(1.0, 1.0, 1.0), src.rgb * 1.4, 0.5), saturate(p0.w) * 0.6); mixv = p1.y;
    } else if (type == 72) {
        // LensFlare: source=p0.xy, intensity=p0.z, ghosts=p0.w, streak=p1.x, warmth=p1.y,
        // hide when blocked=p1.z, mix=p1.w. A core and glow at the source, an anamorphic streak,
        // ghosts along the line through the centre and a halo ring.
        float2 pos = p0.xy, dv = (i.uv - pos) * float2(aspect, 1.0);
        float vis = p1.z > 0.5 ? smoothstep(0.45, 0.9, Luma(BlurAt(pos, 6.0))) : 1.0;
        float3 tint = lerp(float3(0.7, 0.85, 1.1), float3(1.15, 0.85, 0.6), saturate(p1.y * 0.5 + 0.5));
        float d = length(dv);
        float3 add = tint * (exp(-d * 9.0) * 0.9 + exp(-d * 30.0) * 1.5);
        add += tint * exp(-abs(dv.y) * 160.0) * exp(-abs(dv.x) * 2.2) * p1.x * 1.2;
        float2 axis = float2(0.5, 0.5) - pos;
        float gk[5] = { 0.4, 0.75, 1.1, 1.45, 1.9 };
        float gr[5] = { 0.035, 0.06, 0.025, 0.09, 0.05 };
        float3 gc[5] = { float3(0.6, 0.9, 1.0), float3(1.0, 0.6, 0.3), float3(0.5, 1.0, 0.6), float3(0.8, 0.5, 1.0), float3(1.0, 0.9, 0.5) };
        [unroll] for (int k = 0; k < 5; ++k) {
            float gd = length((i.uv - (pos + axis * gk[k])) * float2(aspect, 1.0));
            add += gc[k] * (1.0 - smoothstep(gr[k] * 0.6, gr[k], gd)) * 0.25 * p0.w;
        }
        float rd = abs(length((i.uv - (pos + axis * 1.0)) * float2(aspect, 1.0)) - 0.3);
        add += tint * (1.0 - smoothstep(0.0, 0.03, rd)) * 0.15 * p0.w;
        fx = src.rgb + add * p0.z * vis; mixv = p1.w;
    } else if (type == 73) {
        // DropShadow: angle=p0.x deg, distance=p0.y px, softness=p0.z px, opacity=p0.w,
        // colour=p1.xyz, mix=p1.w. The mask's shape cast as a shadow beside it; needs a mask.
        if (camFwd.w > 0.5) {
            float a = radians(p0.x);
            float2 off = float2(cos(a), sin(a)) * p0.y / max(dims.xy, float2(1.0, 1.0));
            float2 tx = max(p0.z, 0.0) / max(dims.xy, float2(1.0, 1.0));
            float sh = 0.0;
            [unroll] for (int k = 0; k < 9; ++k)
                sh += maskA.SampleLevel(linearClamp, saturate(i.uv - off + float2(k % 3 - 1, k / 3 - 1) * tx), 0);
            fx = lerp(src.rgb, p1.xyz, saturate(sh / 9.0 * (1.0 - m) * p0.w));
        }
        m = 1.0; mixv = p1.w;
    } else if (type == 74) {
        // Prism: amount=p0.x px, sideways=p0.y, mix=p0.z. Light split into a spectrum outward from
        // the centre (or sideways), as through a prism filter.
        float2 o = i.uv - 0.5;
        float2 dir = p0.y > 0.5 ? float2(1.0, 0.0) : normalize(o + 1e-5) * saturate(length(o * float2(aspect, 1.0)) * 2.0);
        float2 stp = dir * p0.x / max(dims.x, 1.0);
        float3 acc = float3(0, 0, 0), wsum = float3(0, 0, 0);
        [unroll] for (int k = 0; k < 8; ++k) {
            float t = float(k) / 7.0;
            float3 w = HsvToRgb(float3(t * 0.8, 1.0, 1.0));
            acc += colorIn.SampleLevel(linearClamp, saturate(i.uv + stp * (t - 0.5) * 2.0), 0).rgb * w;
            wsum += w;
        }
        fx = acc / max(wsum, float3(1e-3, 1e-3, 1e-3)); mixv = p0.z;
    }
    if (mixv >= 0.0) outRgb = lerp(src.rgb, saturate(fx), saturate(m * mixv));
    return float4(outRgb, 1.0);
}

// ---- Final Output Mix Pass ----
float4 OutputPS(VsOut i) : SV_Target {
    float3 a = origColor.SampleLevel(linearClamp, i.uv, 0).rgb;
    float3 b = colorIn.SampleLevel(linearClamp, i.uv, 0).rgb;
    return float4(lerp(a, b, saturate(stepInfo.z)), 1.0);
}
)HLSL";

struct Constants {
	float invViewProj[16];
	float refViewProj[16];
	float refInvViewProj[16];
	float dims[4];
	float depthInfo[4];
	float cameraTime[4];
	float camFwd[4];
	float stepInfo[4];
	float refInfo[4];
	float p0[4];
	float p1[4];
	int pmCounts[4];
	float pmBounds[4];
	float pmCapsule[4];
	float pmBones[16][4];
	float pmOthers[16][4];
	float smokes[8][4];
	int smokeInfo[4];
};
static_assert(sizeof(Constants) % 16 == 0, "cb alignment");

struct Blobs {
	ComPtr<ID3DBlob> vs, maskPS[2], effectPS[2], outputPS;
	bool ok = false;
};
std::mutex g_blobLock;
std::shared_ptr<const Blobs> g_blobs;
std::atomic<bool> g_compileStarted{ false };
std::thread g_compiler;

struct RtSurface {
	ComPtr<ID3D11Texture2D> tex;
	ComPtr<ID3D11ShaderResourceView> srv;
	ComPtr<ID3D11RenderTargetView> rtv;
	UINT w = 0, h = 0;
	DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;

	void Reset() { rtv.Reset(); srv.Reset(); tex.Reset(); w = h = 0; fmt = DXGI_FORMAT_UNKNOWN; }
	bool Ensure(ID3D11Device* dev, UINT width, UINT height, DXGI_FORMAT format) {
		if (tex && w == width && h == height && fmt == format) return true;
		Reset();
		RenderPrivateTextureScope priv;
		D3D11_TEXTURE2D_DESC td{};
		td.Width = width; td.Height = height; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = format; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))
			|| FAILED(dev->CreateShaderResourceView(tex.Get(), nullptr, &srv))
			|| FAILED(dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv))) {
			Reset();
			return false;
		}
		w = width; h = height; fmt = format;
		return true;
	}
};

struct Gpu {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> ctx;
	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader> maskPS[2], effectPS[2], outputPS;
	ComPtr<ID3D11Buffer> cb;
	ComPtr<ID3D11SamplerState> sampler;
	ComPtr<ID3D11RasterizerState> raster;
	ComPtr<ID3D11DepthStencilState> dsOff;
	ComPtr<ID3D11BlendState> blendOpaque;

	RtSurface orig, ping[2], masks[kMaskSlots];
	// Frames kept for Echo Trails (a trail buffer, ping-ponged) and Stop Motion (the held frame),
	// one slot per node, freed when its node leaves the plan.
	struct HistorySlot {
		RtSurface buf[2];
		int nodeId = -1, key = -1, cur = 0;
		bool valid = false, used = false;
		double bucket = -1e300, lastDemo = -1e300;
	} history[8];
	double lastDemoTime = -1e300;
	ComPtr<ID3D11Texture2D> depthCopy;
	ComPtr<ID3D11ShaderResourceView> depthSrv;
	D3D11_TEXTURE2D_DESC depthDesc{};
	bool failed = false;
} g_gpu;

DXGI_FORMAT TypedColorFormat(DXGI_FORMAT f) {
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default: return f;
	}
}

DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT f) {
	switch (f) {
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

DXGI_FORMAT DepthTypelessFormat(DXGI_FORMAT f) {
	switch (f) {
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24G8_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32G8X24_TYPELESS;
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_TYPELESS;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

void CompileAll() {
	auto b = std::make_shared<Blobs>();
	ComPtr<ID3DBlob> err;
	const D3D_SHADER_MACRO s0[] = { { "DEPTH_MS", "0" }, { nullptr, nullptr } };
	const D3D_SHADER_MACRO s1[] = { { "DEPTH_MS", "1" }, { nullptr, nullptr } };
	const size_t n = sizeof(kShader) - 1;
	b->ok = SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "EffectNodes", s0, "VS", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->vs, &err))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "EffectNodes", s0, "MaskPS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->maskPS[0], &err))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "EffectNodes", s1, "MaskPS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->maskPS[1], &err))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "EffectNodes", s0, "EffectPS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->effectPS[0], &err))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "EffectNodes", s1, "EffectPS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->effectPS[1], &err))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "EffectNodes", s0, "OutputPS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->outputPS, &err));
	if (!b->ok)
		MvmDebugLog_LinefAlways("effectnodes", "shader compile failed: %s",
			err ? static_cast<const char*>(err->GetBufferPointer()) : "compiler unavailable");
	std::lock_guard<std::mutex> lock(g_blobLock);
	g_blobs = b;
}

bool EnsureShaders() {
	if (g_gpu.vs) return true;
	if (!g_gpu.device || g_gpu.failed) return false;
	std::shared_ptr<const Blobs> b;
	{ std::lock_guard<std::mutex> lock(g_blobLock); b = g_blobs; }
	if (!b) return false;
	if (!b->ok) { g_gpu.failed = true; return false; }

	RenderPrivateTextureScope priv;
	auto* d = g_gpu.device.Get();
	const bool ok = SUCCEEDED(d->CreateVertexShader(b->vs->GetBufferPointer(), b->vs->GetBufferSize(), nullptr, &g_gpu.vs))
		&& SUCCEEDED(d->CreatePixelShader(b->maskPS[0]->GetBufferPointer(), b->maskPS[0]->GetBufferSize(), nullptr, &g_gpu.maskPS[0]))
		&& SUCCEEDED(d->CreatePixelShader(b->maskPS[1]->GetBufferPointer(), b->maskPS[1]->GetBufferSize(), nullptr, &g_gpu.maskPS[1]))
		&& SUCCEEDED(d->CreatePixelShader(b->effectPS[0]->GetBufferPointer(), b->effectPS[0]->GetBufferSize(), nullptr, &g_gpu.effectPS[0]))
		&& SUCCEEDED(d->CreatePixelShader(b->effectPS[1]->GetBufferPointer(), b->effectPS[1]->GetBufferSize(), nullptr, &g_gpu.effectPS[1]))
		&& SUCCEEDED(d->CreatePixelShader(b->outputPS->GetBufferPointer(), b->outputPS->GetBufferSize(), nullptr, &g_gpu.outputPS));
	if (!ok) { g_gpu.failed = true; return false; }

	D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = sizeof(Constants); cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	d->CreateBuffer(&cbd, nullptr, &g_gpu.cb);
	D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
	d->CreateSamplerState(&sd, &g_gpu.sampler);
	D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
	d->CreateRasterizerState(&rd, &g_gpu.raster);
	D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable = FALSE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	d->CreateDepthStencilState(&dd, &g_gpu.dsOff);
	D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	d->CreateBlendState(&bd, &g_gpu.blendOpaque);
	return g_gpu.cb && g_gpu.sampler && g_gpu.raster && g_gpu.dsOff && g_gpu.blendOpaque;
}

bool EnsureDepthCopy(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, bool& isMsaa, UINT& w, UINT& h) {
	if (!dsv) return false;
	ComPtr<ID3D11Resource> res; dsv->GetResource(&res);
	ComPtr<ID3D11Texture2D> tex; if (!res || FAILED(res.As(&tex))) return false;
	D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
	const DXGI_FORMAT typeless = DepthTypelessFormat(d.Format);
	const DXGI_FORMAT srvFmt = DepthSrvFormat(d.Format);
	if (typeless == DXGI_FORMAT_UNKNOWN || srvFmt == DXGI_FORMAT_UNKNOWN) return false;
	isMsaa = d.SampleDesc.Count > 1;
	w = d.Width; h = d.Height;
	if (!g_gpu.depthCopy || g_gpu.depthDesc.Width != d.Width || g_gpu.depthDesc.Height != d.Height
		|| g_gpu.depthDesc.Format != typeless || g_gpu.depthDesc.SampleDesc.Count != d.SampleDesc.Count) {
		RenderPrivateTextureScope priv;
		g_gpu.depthCopy.Reset(); g_gpu.depthSrv.Reset();
		D3D11_TEXTURE2D_DESC td = d;
		td.Format = typeless; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.Usage = D3D11_USAGE_DEFAULT; td.CPUAccessFlags = 0; td.MiscFlags = 0;
		D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = srvFmt;
		sd.ViewDimension = isMsaa ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
		if (!isMsaa) sd.Texture2D.MipLevels = 1;
		if (FAILED(g_gpu.device->CreateTexture2D(&td, nullptr, &g_gpu.depthCopy))
			|| FAILED(g_gpu.device->CreateShaderResourceView(g_gpu.depthCopy.Get(), &sd, &g_gpu.depthSrv)))
			return false;
		g_gpu.depthDesc = td;
	}
	ctx->CopyResource(g_gpu.depthCopy.Get(), tex.Get());
	return true;
}

} // namespace

void BeginDevice(ID3D11Device* device) {
	EndDevice();
	if (!device) return;
	g_gpu.device = device;
	g_gpu.failed = false;
	device->CreateDeferredContext(0, &g_gpu.ctx);
	if (!g_compileStarted.exchange(true)) {
		g_compiler = std::thread(CompileAll);
		g_compiler.detach();
	}
}

void EndDevice() {
	g_gpu = {};
}

// The smoke grenades for this frame (render thread): Player Matte hides a player behind one.
namespace { float g_smokes[8][4] = {}; int g_smokeCount = 0; }
void SetSmokes(const float* spheres, int count) {
	g_smokeCount = (std::max)(0, (std::min)(count, 8));
	std::memset(g_smokes, 0, sizeof(g_smokes));
	if (spheres) std::memcpy(g_smokes, spheres, sizeof(float) * 4 * g_smokeCount);
}

bool Render(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target,
	ID3D11DepthStencilView* depth, ID3D11ShaderResourceView* worldDepthSrv,
	const float* refWorldToClip, float refDepthMin, float refDepthMax,
	const float* worldToClip, const float* projection,
	const float camera[3], const float forward[3],
	float depthMin, float depthMax, double demoTime,
	int historyKey) {
	if (!Active() || !immediate || !target || !depth || !worldToClip || !g_gpu.ctx || !EnsureShaders())
		return false;

	const CompiledPlan plan = SnapshotPlan();
	if (!plan.enabled || plan.steps.empty()) return false;

	ComPtr<ID3D11Resource> res; target->GetResource(&res);
	ComPtr<ID3D11Texture2D> src; if (!res || FAILED(res.As(&src))) return false;
	D3D11_TEXTURE2D_DESC sd{}; src->GetDesc(&sd);
	D3D11_RENDER_TARGET_VIEW_DESC rtvd{}; target->GetDesc(&rtvd);
	const DXGI_FORMAT colorFmt = TypedColorFormat(rtvd.Format != DXGI_FORMAT_UNKNOWN ? rtvd.Format : sd.Format);
	const UINT W = sd.Width, H = sd.Height;

	if (!g_gpu.orig.Ensure(g_gpu.device.Get(), W, H, colorFmt)
		|| !g_gpu.ping[0].Ensure(g_gpu.device.Get(), W, H, colorFmt)
		|| !g_gpu.ping[1].Ensure(g_gpu.device.Get(), W, H, colorFmt))
		return false;
	for (int m = 0; m < kMaskSlots; ++m)
		if (!g_gpu.masks[m].Ensure(g_gpu.device.Get(), W, H, DXGI_FORMAT_R8_UNORM)) return false;

	auto* c = g_gpu.ctx.Get();
	bool isMsaa = false;
	UINT dw = W, dh = H;
	if (!EnsureDepthCopy(c, depth, isMsaa, dw, dh)) {
		ComPtr<ID3D11CommandList> drop; c->FinishCommandList(FALSE, &drop);
		return false;
	}

	if (sd.SampleDesc.Count > 1) {
		c->ResolveSubresource(g_gpu.orig.tex.Get(), 0, src.Get(), 0, colorFmt);
	} else {
		c->CopyResource(g_gpu.orig.tex.Get(), src.Get());
	}
	c->CopyResource(g_gpu.ping[0].tex.Get(), g_gpu.orig.tex.Get());

	Environment::Matrix vp{}, inv{};
	std::memcpy(vp.data(), worldToClip, sizeof(float) * 16);
	Environment::Inverse(vp, inv);
	const bool reversed = projection && projection[10] < 0.0f;
	const auto pm = RenderPlayerMatte_Snapshot();

	D3D11_VIEWPORT vpRect{}; vpRect.Width = (float)W; vpRect.Height = (float)H; vpRect.MaxDepth = 1.0f;
	c->RSSetViewports(1, &vpRect);
	c->RSSetState(g_gpu.raster.Get());
	c->OMSetDepthStencilState(g_gpu.dsOff.Get(), 0);
	const float bf[4] = {};
	c->OMSetBlendState(g_gpu.blendOpaque.Get(), bf, 0xffffffff);
	c->IASetInputLayout(nullptr);
	c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	c->VSSetShader(g_gpu.vs.Get(), nullptr, 0);
	ID3D11SamplerState* smps[1] = { g_gpu.sampler.Get() };
	c->PSSetSamplers(0, 1, smps);

	const int msIdx = isMsaa ? 1 : 0;
	int curPing = 0;

	Constants baseCb{};
	std::memcpy(baseCb.invViewProj, inv.data(), sizeof(float) * 16);
	Environment::Matrix refVp{}, refInv{};
	if (worldDepthSrv && refWorldToClip) {
		std::memcpy(refVp.data(), refWorldToClip, sizeof(float) * 16);
		if (!Environment::Inverse(refVp, refInv)) worldDepthSrv = nullptr;
	} else worldDepthSrv = nullptr;
	std::memcpy(baseCb.refViewProj, refVp.data(), sizeof(float) * 16);
	std::memcpy(baseCb.refInvViewProj, refInv.data(), sizeof(float) * 16);
	baseCb.refInfo[0] = refDepthMin; baseCb.refInfo[1] = refDepthMax;
	baseCb.dims[0] = (float)W; baseCb.dims[1] = (float)H;
	baseCb.dims[2] = (float)dw; baseCb.dims[3] = (float)dh;
	baseCb.depthInfo[0] = depthMin; baseCb.depthInfo[1] = depthMax;
	baseCb.depthInfo[2] = reversed ? 1.0f : 0.0f;
	baseCb.depthInfo[3] = worldDepthSrv ? 1.0f : 0.0f;
	baseCb.cameraTime[0] = camera ? camera[0] : 0.0f;
	baseCb.cameraTime[1] = camera ? camera[1] : 0.0f;
	baseCb.cameraTime[2] = camera ? camera[2] : 0.0f;
	baseCb.cameraTime[3] = (float)demoTime;
	// Wall-clock seconds for motion that belongs to the device, not the scene (a night-vision tube's
	// sway and sparkle, tape jitter), so it keeps moving while the demo is paused. Wrapped hourly to
	// keep float precision.
	{
		static const auto t0 = std::chrono::steady_clock::now();
		const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		baseCb.stepInfo[3] = (float)std::fmod(s, 3600.0);
	}
	baseCb.camFwd[0] = forward ? forward[0] : 1.0f;
	baseCb.camFwd[1] = forward ? forward[1] : 0.0f;
	baseCb.camFwd[2] = forward ? forward[2] : 0.0f;
	baseCb.pmCounts[0] = pm.hasTarget ? 1 : 0;
	baseCb.pmCounts[1] = pm.targetBoneCount;
	baseCb.pmCounts[2] = pm.otherCount;
	for (int k = 0; k < 4; ++k) {
		baseCb.pmBounds[k] = pm.targetBoundsCenter[k];
		baseCb.pmCapsule[k] = pm.targetCapsule[k];
	}
	for (int i = 0; i < 16; ++i) {
		for (int k = 0; k < 4; ++k) {
			baseCb.pmBones[i][k] = pm.targetBones[i][k];
			baseCb.pmOthers[i][k] = pm.otherCapsules[i][k];
		}
	}
	baseCb.smokeInfo[0] = g_smokeCount;
	std::memcpy(baseCb.smokes, g_smokes, sizeof(g_smokes));

	ID3D11ShaderResourceView* nullSrvs[7] = {};

	// A demo jump (seek, scrub, a new round) makes every kept frame stale.
	if (std::fabs(demoTime - g_gpu.lastDemoTime) > 1.0)
		for (auto& h : g_gpu.history) h.valid = false;
	g_gpu.lastDemoTime = demoTime;
	// Slots are per node AND per render (historyKey), and only those of this render are recycled.
	// A slot whose render stopped happening (a finished job's extra render) is freed once its
	// demo time falls behind.
	for (auto& h : g_gpu.history) {
		if (h.nodeId >= 0 && std::fabs(demoTime - h.lastDemo) > 2.0) { h.nodeId = -1; h.key = -1; h.valid = false; }
		if (h.key == historyKey) h.used = false;
	}
	auto historyFor = [&](int nodeId) -> Gpu::HistorySlot* {
		Gpu::HistorySlot* slot = nullptr;
		for (auto& h : g_gpu.history) if (h.nodeId == nodeId && h.key == historyKey) { slot = &h; break; }
		if (!slot) for (auto& h : g_gpu.history) if (h.nodeId < 0) { slot = &h; slot->nodeId = nodeId; slot->key = historyKey; slot->valid = false; break; }
		if (!slot) return nullptr;   // more such nodes than slots: the rest pass the picture through
		if (!slot->buf[0].Ensure(g_gpu.device.Get(), W, H, colorFmt) || !slot->buf[1].Ensure(g_gpu.device.Get(), W, H, colorFmt)) {
			slot->valid = false;
			return nullptr;
		}
		slot->used = true; slot->lastDemo = demoTime;
		return slot;
	};
	ID3D11RenderTargetView* nullRt[1] = {};

	for (const CompiledStep& st : plan.steps) {
		Constants cb = baseCb;
		cb.stepInfo[0] = (float)(int)st.type;
		cb.camFwd[3] = st.maskSlotA >= 0 ? 1.0f : 0.0f;
		cb.stepInfo[1] = st.maskSlotB >= 0 ? 1.0f : 0.0f;
		cb.stepInfo[2] = plan.masterMix;
		for (int k = 0; k < 4; ++k) {
			cb.p0[k] = st.p[k];
			cb.p1[k] = st.p[k + 4];
		}
		c->UpdateSubresource(g_gpu.cb.Get(), 0, nullptr, &cb, 0, 0);
		ID3D11Buffer* cbs[1] = { g_gpu.cb.Get() };
		c->PSSetConstantBuffers(0, 1, cbs);

		ID3D11ShaderResourceView* srvs[6] = {
			g_gpu.ping[curPing].srv.Get(),
			g_gpu.depthSrv.Get(),
			worldDepthSrv,
			st.maskSlotA >= 0 ? g_gpu.masks[st.maskSlotA].srv.Get() : nullptr,
			st.maskSlotB >= 0 ? g_gpu.masks[st.maskSlotB].srv.Get() : nullptr,
			g_gpu.orig.srv.Get()
		};

		if (IsMaskNode(st.type) && st.outMaskSlot >= 0 && st.outMaskSlot < kMaskSlots) {
			ID3D11RenderTargetView* rts[1] = { g_gpu.masks[st.outMaskSlot].rtv.Get() };
			c->OMSetRenderTargets(1, rts, nullptr);
			c->PSSetShader(g_gpu.maskPS[msIdx].Get(), nullptr, 0);
			c->PSSetShaderResources(0, 6, srvs);
			c->Draw(3, 0);
			c->PSSetShaderResources(0, 7, nullSrvs);
			c->OMSetRenderTargets(1, nullRt, nullptr);
		} else if (IsEffectNode(st.type)) {
			const int nextPing = 1 - curPing;
			auto drawInto = [&](ID3D11RenderTargetView* rtv, const Constants& k, ID3D11ShaderResourceView* hist) {
				c->UpdateSubresource(g_gpu.cb.Get(), 0, nullptr, &k, 0, 0);
				ID3D11RenderTargetView* rts[1] = { rtv };
				c->OMSetRenderTargets(1, rts, nullptr);
				c->PSSetShader(g_gpu.effectPS[msIdx].Get(), nullptr, 0);
				ID3D11ShaderResourceView* all[7] = { srvs[0], srvs[1], srvs[2], srvs[3], srvs[4], srvs[5], hist };
				c->PSSetShaderResources(0, 7, all);
				c->Draw(3, 0);
				c->PSSetShaderResources(0, 7, nullSrvs);
				c->OMSetRenderTargets(1, nullRt, nullptr);
			};
			Gpu::HistorySlot* hs = NeedsHistory(st.type) ? historyFor(st.nodeId) : nullptr;
			if (hs && st.type == NodeType::EchoTrails) {
				// Pass 1: the new trail from the old one; pass 2: the picture with the trail on top.
				Constants k = cb;
				k.refInfo[2] = 2.0f; k.refInfo[3] = hs->valid ? 1.0f : 0.0f;
				const int next = 1 - hs->cur;
				drawInto(hs->buf[next].rtv.Get(), k, hs->valid ? hs->buf[hs->cur].srv.Get() : nullptr);
				hs->cur = next; hs->valid = true;
				k.refInfo[2] = 1.0f; k.refInfo[3] = 1.0f;
				drawInto(g_gpu.ping[nextPing].rtv.Get(), k, hs->buf[hs->cur].srv.Get());
			} else if (hs && st.type == NodeType::StopMotion) {
				// A new frame is captured when the demo clock enters the next step at this rate;
				// in between the held one shows.
				const double bucket = std::floor(demoTime * (double)std::max(st.p[0], 0.5f));
				const bool hold = hs->valid && bucket == hs->bucket;
				Constants k = cb;
				k.refInfo[2] = hold ? 1.0f : 0.0f; k.refInfo[3] = hs->valid ? 1.0f : 0.0f;
				drawInto(g_gpu.ping[nextPing].rtv.Get(), k, hs->valid ? hs->buf[0].srv.Get() : nullptr);
				if (!hold) {
					c->CopyResource(hs->buf[0].tex.Get(), g_gpu.ping[nextPing].tex.Get());
					hs->valid = true; hs->bucket = bucket;
				}
			} else {
				drawInto(g_gpu.ping[nextPing].rtv.Get(), cb, nullptr);
			}
			curPing = nextPing;
		}
	}

	for (auto& h : g_gpu.history) if (h.key == historyKey && !h.used) { h.nodeId = -1; h.key = -1; h.valid = false; }

	// Final composite into the scene render target with masterMix.
	Constants finalCb = baseCb;
	finalCb.stepInfo[2] = plan.masterMix;
	c->UpdateSubresource(g_gpu.cb.Get(), 0, nullptr, &finalCb, 0, 0);
	ID3D11Buffer* cbs[1] = { g_gpu.cb.Get() };
	c->PSSetConstantBuffers(0, 1, cbs);
	ID3D11RenderTargetView* outRt[1] = { target };
	c->OMSetRenderTargets(1, outRt, nullptr);
	c->PSSetShader(g_gpu.outputPS.Get(), nullptr, 0);
	ID3D11ShaderResourceView* outSrvs[6] = {
		g_gpu.ping[curPing].srv.Get(), nullptr, nullptr, nullptr, nullptr, g_gpu.orig.srv.Get()
	};
	c->PSSetShaderResources(0, 6, outSrvs);
	c->Draw(3, 0);
	c->PSSetShaderResources(0, 6, nullSrvs);
	c->OMSetRenderTargets(1, nullRt, nullptr);

	ComPtr<ID3D11CommandList> cl;
	if (FAILED(c->FinishCommandList(FALSE, &cl)) || !cl) return false;
	{ GPU_PASS_SCOPE(immediate, "effect nodes"); immediate->ExecuteCommandList(cl.Get(), TRUE); }
	return true;
}

} // namespace Filmmaker::EffectNodes
