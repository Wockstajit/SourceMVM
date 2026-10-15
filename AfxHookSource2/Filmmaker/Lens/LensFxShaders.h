#pragma once

// HLSL for the lens module. Kept as separate string constants and joined at runtime: MSVC caps a
// single string literal at ~16 KB and the whole thing is well past that.
//
// Macros the compiler is given: DEPTH_MS (scene depth is multisampled), COLOR_MS (the target the
// composite writes into is multisampled).
//
// SPACE. The scene arrives in DISPLAY space (it is the tonemapped, pre-UI frame). The gather runs
// in approximately linear light (pow 2.2) so a blurred highlight spreads energy instead of
// averaging gamma numbers, and everything is put back into display space before it is written.
//
// The circle of confusion, iris boundaries and kernel spiral here are mirrored in LensFxMath.h;
// keep the constants in step (lens-fx-math-tests.cpp checks the C++ side).

namespace Filmmaker::Lens {

inline constexpr char kShaderCommon[] = R"HLSL(
cbuffer LensConstants : register(b0) {
    row_major float4x4 inverseViewProjection;
    float4 dims;        // xy target size, zw depth size (px)
    float4 depthInfo;   // x depth min, y depth max (world band), z unused, w reversed
    float4 frame;       // x dt seconds, y unused, z unused, w first-frame flag
    float4 dofA;        // x mode, y focus (units), z f-stop, w autofocus on
    float4 dofB;        // near start, near end, far start, far end
    float4 dofC;        // max blur (px at this size), bokeh gamma (stops), blur weapon, debug
    float4 irisA;       // shape, blades, rotation (rad), roundness
    float4 irisB;       // squeeze, edge, taps, custom mask loaded
    float4 flareA;      // intensity, threshold, ghosts, halo
    float4 flareB;      // streak, streak length, star, blades
    float4 flareC;      // glow, dirt, dirt image loaded, flare-only debug
    float4 flareTint;   // rgb flare tint
    float4 streakTint;  // rgb streak tint
    float4 lowRes;      // xy quarter-res size, zw eighth-res size
    float4 tileInfo;    // xy tile grid size, z dilate radius (tiles), w unused
    float4 passInfo;    // xy source texel size for the current downsample step
    float4 modes;       // x dof active, y flare active, z aspect (w/h), w unused
};
SamplerState linearClamp : register(s0);
Texture2D<float4> colorTex : register(t0);
#if DEPTH_MS
Texture2DMS<float> sceneDepth : register(t1);
#else
Texture2D<float> sceneDepth : register(t1);
#endif
Texture2D<float> focusTex : register(t2);
Texture2D<float> focusPrev : register(t3);
Texture2D<float4> cocTex : register(t4);
Texture2D<float> tileTex : register(t5);
Texture2D<float4> gatherTex : register(t6);
Texture2D<float4> irisMask : register(t7);
Texture2D<float4> dofTex : register(t8);
Texture2D<float4> brightTex : register(t9);
Texture2D<float4> ghostTex : register(t10);
Texture2D<float4> streakTex : register(t11);
Texture2D<float4> glowTex : register(t12);
#if COLOR_MS
Texture2DMS<float4> msaaColor : register(t13);
#endif
Texture2D<float4> bright8Tex : register(t14);
Texture2D<float4> dirtTex : register(t15);
Texture2D<float4> ghostSrcTex : register(t16);
Texture2D<float4> sourceTex : register(t17);   // the source map, quarter resolution
Texture2D<float4> source8Tex : register(t18);  // the source map, eighth resolution
Texture2D<float4> sourceHTex : register(t19);  // the source map, eighth width x half height (Gaussian-filtered streak)

static const float kPi = 3.14159265;
static const float kFarSky = 60000.0;
// Fixed settings (mirrored in LensFxMath.h; lens-fx-math-tests.cpp compares the numbers).
static const float kFocalMm = 50.0;         // kFocalLengthMm
static const float2 kAutofocusPoint = float2(.5, .5); // kAutofocusX, kAutofocusY
static const float kAutofocusPull = 6.0;    // kAutofocusPull
static const float kWeaponDist = 30.0;      // kWeaponDistance
static const float kEdgeSoft = .22;         // kEdgeSoftness
static const float kRingHole = .55;         // kRingHole
static const float kGatherFootprint = .9;   // kGatherFootprint
static const float kGatherJitter = 1.0;     // kGatherJitter
static const float kRingLodBias = 1.0;      // kRingLodBias
static const float kLiftStopsPerGamma = 1.75; // kLiftStopsPerGamma
static const float kMaxLiftStops = 15.0;    // kMaxLiftStops
static const float kHeartCentroid = 0.26759; // kHeartCentroid
static const float kHeartScale = 1.65;       // kHeartScale
struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VOut VS(uint id : SV_VertexID) {
    VOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.uv = p;
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float Luma(float3 c) { return dot(c, float3(.2126, .7152, .0722)); }
float3 ToLinear(float3 c) { return pow(max(c, 0), 2.2); }
float3 ToDisplay(float3 c) { return pow(max(c, 0), 1.0 / 2.2); }
// White noise per pixel (Hoskins hash). The interleaved-gradient noise it replaces has a diagonal structure that the
// denoise cannot average away and showed as stripes inside a bright disc; white noise is undone by the neighbourhood average.
float PixelNoise(float2 p) { float3 q = frac(float3(p.xyx) * .1031); q += dot(q, q.yzx + 33.33); return frac((q.x + q.y) * q.z); }

// ---- depth --------------------------------------------------------------------------------
float RawDepth(float2 pixel) {
    int2 p = clamp(int2(pixel * dims.zw / dims.xy), int2(0, 0), int2(dims.zw) - 1);
#if DEPTH_MS
    uint w, h, n;
    sceneDepth.GetDimensions(w, h, n);
    float d = sceneDepth.Load(p, 0);
    for (uint i = 1; i < n; ++i) d = depthInfo.w > .5 ? max(d, sceneDepth.Load(p, i)) : min(d, sceneDepth.Load(p, i));
    return d;
#else
    return sceneDepth.Load(int3(p, 0));
#endif
}
// Distance from the camera ALONG the view axis, in world units. CS2 draws the first-person
// weapon in its own low depth band (raw <= 0.1) and the sky at 1; neither is a real distance, so
// they are flagged instead of decoded.
float ViewDistance(float raw, float2 pixel, out bool viewmodel) {
    viewmodel = depthInfo.w < .5 && raw <= .100001;
    bool sky = depthInfo.w > .5 ? raw < 1e-7 : raw >= depthInfo.y - 1e-6;
    if (sky) return kFarSky;
    float span = max(depthInfo.y - depthInfo.x, 1e-9);
    float nd = saturate((raw - depthInfo.x) / span);
    float2 ndc = pixel / dims.xy * float2(2, -2) + float2(-1, 1);
    float4 h = mul(inverseViewProjection, float4(ndc, nd, 1));
    return min(1.0 / max(abs(h.w), 1e-9), kFarSky);
}

// ---- circle of confusion (mirrors CircleOfConfusion in LensFxMath.h) ------------------------
float CoC(float dist, float focus) {
    float maxPx = dofC.x;
    float s;
    if (dofA.x < .5) {
        float f = kFocalMm;
        float sMm = max(focus * 25.4, f * 1.05);
        float d = max(dist * 25.4, 1.0);
        float cMm = f * f * abs(d - sMm) / (dofA.z * d * (sMm - f));
        float r = .5 * cMm / 36.0 * dims.x;
        s = d >= sMm ? r : -r;
    } else {
        float nearStart = dofB.x;
        float nearEnd = max(dofB.y, nearStart + 1);
        float farStart = max(dofB.z, nearEnd);
        float farEnd = max(dofB.w, farStart + 1);
        s = 0;
        if (dist < nearEnd) s = -smoothstep(0, 1, (nearEnd - dist) / (nearEnd - nearStart)) * maxPx;
        else if (dist > farStart) s = smoothstep(0, 1, (dist - farStart) / (farEnd - farStart)) * maxPx;
    }
    return clamp(s, -maxPx, maxPx);
}
)HLSL";

inline constexpr char kShaderDof[] = R"HLSL(
// ---- autofocus: a 1x1 target that eases toward whatever sits at the focus point --------------
float FocusPS(VOut i) : SV_Target {
    float2 c = kAutofocusPoint * dims.xy;
    float r = dims.y * .02;
    static const float2 offs[9] = { float2(0,0), float2(1,0), float2(-1,0), float2(0,1), float2(0,-1),
        float2(1,1), float2(-1,1), float2(1,-1), float2(-1,-1) };
    float d[9];
    float nearest = 1e9;
    [unroll] for (int k = 0; k < 9; ++k) {
        float2 pix = clamp(c + offs[k] * r, float2(0, 0), dims.xy - 1);
        bool vm;
        float dist = ViewDistance(RawDepth(pix), pix, vm);
        d[k] = (vm || dist >= kFarSky - 1) ? -1 : dist;
        if (d[k] > 0) nearest = min(nearest, d[k]);
    }
    float sum = 0, cnt = 0;
    [unroll] for (int j = 0; j < 9; ++j) if (d[j] > 0 && d[j] < nearest * 1.25) { sum += d[j]; cnt += 1; }
    float prev = focusPrev.Load(int3(0, 0, 0));
    float target = cnt > 0 ? sum / cnt : prev;
    if (frame.w > .5 || prev <= 0) prev = target;
    float a = 1 - exp(-kAutofocusPull * frame.x);
    return lerp(prev, target, a);
}

// ---- the bokeh gamma -------------------------------------------------------------------------
// The frame is tonemapped, so a lamp is only 1.0 and would spread into a faint ghost. Small bright
// lights are lifted by dofC.y STOPS at pure white here, at full resolution and BEFORE the mip chain
// is built (a lift after the mips tests a lamp already averaged with its dark neighbours). Mirrors
// HighlightKey / HighlightFocusRamp / IsolationRadius / HighlightIsolation / HighlightBoost in
// LensFxMath.h.
float HighlightBoost(int2 p, float3 src, float coc) {
    float peak = max(src.r, max(src.g, src.b));
    float key = pow(saturate(peak), 6);
    float ramp = saturate((abs(coc) - .5) / 2.5);
    float stops = min(dofC.y * kLiftStopsPerGamma * key * ramp, kMaxLiftStops);
    if (stops < .02) return 1;
    // Isolation: a light on a dark surround counts, a lit wall or the sky (as bright as its ring) does not.
    float rho = min(max(.6 * abs(coc), 2), 24 * dims.y / 1080);
    static const float2 ring[8] = { float2(1,0), float2(-1,0), float2(0,1), float2(0,-1),
        float2(.7071,.7071), float2(-.7071,.7071), float2(.7071,-.7071), float2(-.7071,-.7071) };
    float acc = 0, accOuter = 0;
    [unroll] for (int k = 0; k < 8; ++k) {
        int2 q = clamp(p + int2(round(ring[k] * rho)), int2(0, 0), int2(dims.xy) - 1);
        float3 c = colorTex.Load(int3(q, 0)).rgb;
        acc += max(c.r, max(c.g, c.b));
        int2 q2 = clamp(p + int2(round(ring[k] * rho * 1.8)), int2(0, 0), int2(dims.xy) - 1);
        float3 c2 = colorTex.Load(int3(q2, 0)).rgb;
        accOuter += max(c2.r, max(c2.g, c2.b));
    }
    float iso = saturate((.95 - (min(acc, accOuter) * .125) / max(peak, 1e-3)) / .25);
    return exp2(stops * iso);
}

// ---- per-pixel circle of confusion, colour moved to linear light ---------------------------
float4 CocPS(VOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    float3 src = colorTex.Load(int3(p, 0)).rgb;
    bool vm;
    float dist = ViewDistance(RawDepth(i.pos.xy), i.pos.xy, vm);
    float focus = dofA.w > .5 ? focusTex.Load(int3(0, 0, 0)) : dofA.y;
    float coc;
    if (vm) coc = dofC.z > .5 ? CoC(kWeaponDist, focus) : 0;
    else coc = CoC(dist, focus);
    return float4(ToLinear(src) * HighlightBoost(p, src, coc), coc);
}
// One step down the mip chain of the linear-light image + radius: the destination texel centre
// sits on the corner shared by four source texels, so a single bilinear tap IS the 2x2 average. A
// wide gather reads these pre-averaged taps instead of single texels -- that is what keeps a 60 px
// blur from turning to noise. tileInfo.w is the source mip.
float4 MipPS(VOut i) : SV_Target {
    return cocTex.SampleLevel(linearClamp, i.uv, 0);
}
float TilePS(VOut i) : SV_Target {
    int2 base = int2(i.pos.xy) * 16;
    float m = 0;
    [loop] for (int y = 0; y < 8; ++y)
        [loop] for (int x = 0; x < 8; ++x) {
            int2 q = min(base + int2(x, y) * 2, int2(dims.xy) - 1);
            m = max(m, abs(cocTex.Load(int3(q, 0)).a));
        }
    return m;
}
float DilateHPS(VOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    int r = (int)tileInfo.z;
    float m = 0;
    [loop] for (int k = -r; k <= r; ++k) m = max(m, tileTex.Load(int3(clamp(p + int2(k, 0), int2(0, 0), int2(tileInfo.xy) - 1), 0)));
    return m;
}
float DilateVPS(VOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    int r = (int)tileInfo.z;
    float m = 0;
    [loop] for (int k = -r; k <= r; ++k) m = max(m, tileTex.Load(int3(clamp(p + int2(0, k), int2(0, 0), int2(tileInfo.xy) - 1), 0)));
    return m;
}

// ---- the iris -------------------------------------------------------------------------------
float PolyR(float th, float blades, float rot) {
    float sector = 2 * kPi / blades;
    float t = th - rot;
    t = t - sector * floor(t / sector);
    return cos(kPi / blades) / cos(t - sector * .5);
}
// Kernel boundary. The gather reflects the kernel through its centre (a lamp at L lights L + d when
// the kernel has a tap at -d), so the shapes are designed in SCREEN space and `ph` is the screen angle
// this kernel angle maps to: polygon and star point up, the heart's tip points down, positive
// rotation turns clockwise. Mirrors IrisRadius in LensFxMath.h.
float IrisR(float th) {
    int shape = (int)irisA.x;
    float ph = th + kPi;
    float top = -kPi * .5;
    if (shape == 1) { float p = PolyR(ph, irisA.y, irisA.z + top); return p + (1 - p) * irisA.w; }
    if (shape == 2) { float k = .5 + .5 * cos(irisA.y * (ph - irisA.z - top)); return .4 + .6 * pow(k, 1.6); }
    if (shape == 3) {
        float t = ph - irisA.z;
        float s = -sin(t);
        float r = 2 - 2 * s + s * sqrt(abs(cos(t))) / (s + 1.4);
        return max(r, 0) * .25;
    }
    return 1;
}
// Tap idx of n: offset in gather-radius units, radius in disc space, shape weight.
void IrisTap(int idx, int n, float jit, out float2 off, out float discR, out float w) {
    int shape = (int)irisA.x;
    float sq = sqrt(irisB.x);
    if (shape == 5 && irisB.w > .5) {
        // Custom image: low-discrepancy points over the image's square, shrunk to sit inside the disc.
        float2 q = float2((idx + .5) / n, 0);
        uint b = idx; b = (b << 16) | (b >> 16); b = ((b & 0x55555555u) << 1) | ((b & 0xAAAAAAAAu) >> 1);
        b = ((b & 0x33333333u) << 2) | ((b & 0xCCCCCCCCu) >> 2); b = ((b & 0x0F0F0F0Fu) << 4) | ((b & 0xF0F0F0F0u) >> 4);
        b = ((b & 0x00FF00FFu) << 8) | ((b & 0xFF00FF00u) >> 8);
        q.y = float(b) * 2.3283064365386963e-10;
        float2 p = (q + (jit - .5) / sqrt((float)n)) * 2 - 1;
        float cr = cos(irisA.z), sr = sin(irisA.z);
        p = float2(p.x * cr - p.y * sr, p.x * sr + p.y * cr) * .70710678;
        // The disc on screen is the kernel reflected through its centre, so the image is read at -p.
        float m = irisMask.SampleLevel(linearClamp, float2(-p.x, -p.y) / .70710678 * .5 + .5, 0).r;
        off = float2(p.x / sq, p.y * sq);
        discR = min(length(p), 1);
        w = m;
        return;
    }
    float u = sqrt((idx + .5) / n);
    float th = idx * 2.39996323 + jit * 6.2831853;
    float bd = IrisR(th);
    float r = u * bd;
    off = float2(cos(th), sin(th)) * r;
    // The heart's polar curve is drawn about its notch, not its middle, so the disc would sit below its light.
    // Move the area centroid onto the tap centre (it turns with the shape) and scale up to a size that matches the
    // other shapes. Mirrors HeartCentroid / kHeartScale in LensFxMath.h.
    if (shape == 3) off = (off - float2(sin(irisA.z), -cos(irisA.z)) * kHeartCentroid) * kHeartScale;
    off = float2(off.x / sq, off.y * sq);
    discR = u;
    w = bd * bd;
    if (shape == 4) w *= smoothstep(kRingHole - .06, kRingHole + .06, u);
    if (irisB.y > 0) w *= 1 + irisB.y * smoothstep(1 - kEdgeSoft, 1, u);
}

// ---- the gather -----------------------------------------------------------------------------
// Scatter-as-gather bokeh. Each tap asks: does the disc of the pixel I found reach back to me?
// Far-field taps that sit BEHIND the centre pixel are held to the centre's own radius, so a blurred
// background never paints over a sharp foreground; near-field taps always reach.
float4 GatherPS(VOut i) : SV_Target {
    float2 pix = i.pos.xy;
    int2 p = int2(pix);
    float4 c0 = cocTex.Load(int3(p, 0));
    int2 tp = min(p >> 4, int2(tileInfo.xy) - 1);
    float R = min(tileTex.Load(int3(tp, 0)), dofC.x);
    if (R < .6) return float4(c0.rgb, 0);
    int n = (int)irisB.z;
    float jit = lerp(.5, PixelNoise(pix), kGatherJitter);
    // Mip level from the tap SPACING (mirrors GatherLod): each tap averages about the cell it stands
    // in, so a small bright light is seen by some tap wherever it sits and the disc comes out solid.
    float lod = log2(max(R * sqrt(kPi / n) * kGatherFootprint, 1));
    if ((int)irisA.x == 4) lod -= kRingLodBias;   // the Ring's hole is filled by the spill of a coarse tap otherwise
    lod = clamp(lod, 0, 5);
    float soft = max(1.0, exp2(lod) * .5);
    float c0far = max(c0.a, 0), c0near = max(-c0.a, 0);
    float3 nearAcc = 0, farAcc = 0;
    float nearW = 0, farW = 0;
    float tapScale = R * R / n;
    [loop] for (int k = 0; k < n; ++k) {
        float2 off; float discR, kw;
        IrisTap(k, n, jit, off, discR, kw);
        float ur = discR * R;
        float2 tuv = (pix + off * R) / dims.xy;
        float4 s = cocTex.SampleLevel(linearClamp, tuv, lod);
        float3 col = s.rgb;
        float cs = s.a, as = abs(cs);
        float reach = as;
        if (cs > 0 && cs > c0far) reach = min(as, max(c0far, .5));
        float cover = saturate(reach - ur + soft);
        float w = cover * kw * tapScale / max(as * as, .25);
        if (w <= 0) continue;
        // (The highlight lift already happened in CocPS, before the mips: see HighlightBoost.)
        if (cs < 0) { nearAcc += col * w; nearW += w; }
        else { farAcc += col * w; farW += w; }
    }
    // The centre pixel is one more sample, and belongs to whichever layer it is in.
    float wc = tapScale / max(c0.a * c0.a, .25);
    // Read at the taps' own mip level: a full-resolution read would put a sharp bright dot at the centre of every lifted disc.
    float3 cCentre = cocTex.SampleLevel(linearClamp, pix / dims.xy, lod).rgb;
    if (c0.a < 0) { nearAcc += cCentre * wc; nearW += wc; }
    else { farAcc += cCentre * wc; farW += wc; }
    float3 farCol = farW > 1e-6 ? farAcc / farW : c0.rgb;
    float3 nearCol = nearW > 1e-6 ? nearAcc / nearW : farCol;
    float alphaNear = saturate(nearW);
    float3 result = lerp(farCol, nearCol, alphaNear);
    float blend = max(saturate((c0far - .35) / 1.2), saturate(alphaNear * 2) * step(.6, R));
    return float4(result, saturate(max(blend, saturate((c0near - .35) / 1.2))));
}
// The spiral leaves a fine grain at low tap counts; a cross-shaped average over neighbours that
// are blurred by a similar amount removes it without touching real edges.
float4 DenoisePS(VOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    float4 g = gatherTex.Load(int3(p, 0));
    if (g.a < .02) return g;
    float coc = abs(cocTex.Load(int3(p, 0)).a);
    float3 acc = g.rgb; float ws = 1;
    static const int2 nb[16] = { int2(1,0), int2(-1,0), int2(0,1), int2(0,-1), int2(1,1), int2(-1,1), int2(1,-1), int2(-1,-1),
        int2(2,0), int2(-2,0), int2(0,2), int2(0,-2), int2(2,2), int2(-2,2), int2(2,-2), int2(-2,-2) };
    [unroll] for (int k = 0; k < 16; ++k) {
        int2 q = clamp(p + nb[k], int2(0, 0), int2(dims.xy) - 1);
        float4 gn = gatherTex.Load(int3(q, 0));
        float cn = abs(cocTex.Load(int3(q, 0)).a);
        float w = gn.a > .02 ? exp(-abs(cn - coc) * .35) * (k < 4 ? 1 : (k < 8 ? .6 : .35)) : 0;
        acc += gn.rgb * w; ws += w;
    }
    return float4(acc / ws, g.a);
}
)HLSL";

inline constexpr char kShaderFlare[] = R"HLSL(
// ---- LENS FLARE ---------------------------------------------------------------------------------
// The design, and why (the numbers are mirrored in LensFxMath.h, which also has the reasoning):
//   * a SOURCE is a luminance above the user's threshold (a fixed soft knee), and it must be SMALL: a
//     ring of the frame around it has to be dark, so a bright wall, the sky or a lit doorway never
//     becomes a frame-sized copy of itself, and the flare's energy does not follow the area of a bright
//     region. Sources fade out over the last 5 % of the frame;
//   * ghosts are the aperture-shaped blob of each source, scaled about the frame centre, so every ghost
//     lies on the straight line through its source and the centre; each is windowed by the position it
//     is sampled from, so no copy of the frame ends in an edge;
//   * the halo is the angular mean of those blobs on a circle around the frame centre: a ring;
//   * the streak and the diffraction spikes are line integrals of the source map, centred on each source;
//   * every layer saturates on its own (1 - exp(-x)) and the composite blends with a screen blend, so
//     nothing scales without bound.
static const float kFlareCeil = 1.05;        // kFlareCeil
static const float kFlareBudgetArea = 0.01;  // kFlareBudgetArea
static const float kFlareBudgetPow = 0.6;    // kFlareBudgetPow
static const float kFlareBudgetLo = 0.75;    // kFlareBudgetLo
static const float kFlareBudgetHi = 0.95;    // kFlareBudgetHi
static const float kFlareWideRadius = 0.13;  // kFlareWideRadius
static const float kFlareWideLo = 0.75;      // kFlareWideLo
static const float kFlareWideHi = 0.95;      // kFlareWideHi
static const float kFlareWideIsoLo = 0.30;   // kFlareWideIsoLo
static const float kFlareWideIsoHi = 0.70;   // kFlareWideIsoHi
static const float kFlareIsoRadius = 0.045;  // kFlareIsoRadius
static const float kFlareIsoLo = 0.04;       // kFlareIsoLo
static const float kFlareIsoHi = 0.20;       // kFlareIsoHi
static const float kFlareBorder = 0.05;      // kFlareBorder
static const float kFlareRelMargin = 0.06;  // kFlareRelMargin
static const float kFlareRelSoft = 0.06;    // kFlareRelSoft
static const float kFlareMinLo = 0.10;      // kFlareMinLo
static const float kFlareMinHi = 0.30;      // kFlareMinHi
static const float kGhostRadius = 0.04;      // kGhostRadius
static const float kGhostBlobGain = 50.0;    // kGhostBlobGain
static const float kBladeRoundness = 0.15;   // kBladeRoundness
static const float kGhostGain = 1.2;         // kGhostGain
static const float kGhostFringe = 0.012;     // kGhostFringe
static const float kHaloGain = 0.25;         // kHaloGain
static const float kHaloFringe = 0.02;       // kHaloFringe
static const float kStreakGain = 260.0;      // kStreakGain
static const float kSpikeGain = 200.0;       // kSpikeGain
static const float kGlowGain = 90.0;         // kGlowGain
static const float kGlowStep = 1.7;          // glow blur step, in eighth-resolution texels
static const float kLineTaper = 0.5;         // kLineTaper
static const float kStreakSoft = 6.0;        // kStreakSoft
static const float kGhostScales[8] = { -0.50, 0.35, -0.80, 0.62, -1.10, 0.85, -1.45, 1.20 }; // GhostScale

float3 HueColor(float h) { return saturate(abs(frac(h + float3(0, 2.0 / 3, 1.0 / 3)) * 6 - 3) - 1); }
// Fade toward the frame edge (uv outside [0,1] is 0). Mirrors FlareBorderFadeUv.
float BorderFade(float2 uv) {
    float2 d = min(uv, 1 - uv) * float2(modes.z, 1);
    return smoothstep(0, kFlareBorder, d.x) * smoothstep(0, kFlareBorder, d.y);
}

// Bright pass, full -> half. The 13-tap "Call of Duty" downsample, but every tap goes through the
// source gate BEFORE it is averaged, so a lamp a few pixels wide is not diluted below the threshold
// by its dark neighbours. The gate is on luminance (mirrors FlareGate): a saturated sky is not a source.
// rgb is the gated colour; alpha is the plain (ungated) luminance, averaged the same way, which the
// isolation test in SourcePS reads: it must not depend on the threshold, or lowering the threshold
// could make a lamp vanish.
float4 Gated(float2 uv) {
    float3 c = dofTex.SampleLevel(linearClamp, uv, 0).rgb;
    float l = Luma(c);
    return float4(c * smoothstep(flareA.y, kFlareCeil, l), l);
}
float4 BrightPS(VOut i) : SV_Target {
    float2 t = 1.0 / dims.xy;
    float2 uv = i.uv;
    float4 a = Gated(uv + t * float2(-2, -2));
    float4 b = Gated(uv + t * float2(0, -2));
    float4 c = Gated(uv + t * float2(2, -2));
    float4 d = Gated(uv + t * float2(-2, 0));
    float4 e = Gated(uv);
    float4 f = Gated(uv + t * float2(2, 0));
    float4 g = Gated(uv + t * float2(-2, 2));
    float4 h = Gated(uv + t * float2(0, 2));
    float4 ii = Gated(uv + t * float2(2, 2));
    float4 j = Gated(uv + t * float2(-1, -1));
    float4 k = Gated(uv + t * float2(1, -1));
    float4 l = Gated(uv + t * float2(-1, 1));
    float4 m = Gated(uv + t * float2(1, 1));
    float4 corners = (a + b + d + e) * .25 + (b + c + e + f) * .25 + (d + e + g + h) * .25 + (e + f + h + ii) * .25;
    return corners * .125 + (j + k + l + m) * .25 * .5;
}
// Plain 13-tap step down (quarter, eighth). passInfo.xy is the SOURCE texel size.
float4 Down13PS(VOut i) : SV_Target {
    float2 t = passInfo.xy;
    float2 uv = i.uv;
    float4 a = brightTex.SampleLevel(linearClamp, uv + t * float2(-2, -2), 0);
    float4 b = brightTex.SampleLevel(linearClamp, uv + t * float2(0, -2), 0);
    float4 c = brightTex.SampleLevel(linearClamp, uv + t * float2(2, -2), 0);
    float4 d = brightTex.SampleLevel(linearClamp, uv + t * float2(-2, 0), 0);
    float4 e = brightTex.SampleLevel(linearClamp, uv, 0);
    float4 f = brightTex.SampleLevel(linearClamp, uv + t * float2(2, 0), 0);
    float4 g = brightTex.SampleLevel(linearClamp, uv + t * float2(-2, 2), 0);
    float4 h = brightTex.SampleLevel(linearClamp, uv + t * float2(0, 2), 0);
    float4 ii = brightTex.SampleLevel(linearClamp, uv + t * float2(2, 2), 0);
    float4 j = brightTex.SampleLevel(linearClamp, uv + t * float2(-1, -1), 0);
    float4 k = brightTex.SampleLevel(linearClamp, uv + t * float2(1, -1), 0);
    float4 l = brightTex.SampleLevel(linearClamp, uv + t * float2(-1, 1), 0);
    float4 m = brightTex.SampleLevel(linearClamp, uv + t * float2(1, 1), 0);
    return e * .125 + (a + c + g + ii) * .03125 + (b + d + f + h) * .0625 + (j + k + l + m) * .125;
}

// The source map (quarter resolution): the gated bright pass, kept only where the source is SMALL and
// brighter than what surrounds it. Sixteen taps of the eighth-resolution mean luminance on a ring
// kFlareIsoRadius (of the frame height) out; a tap counts when it is nearly as bright as this source's
// own peak (kFlareRelMargin), and a source whose ring is mostly such taps is part of a big bright area
// and is dropped (mirrors FlareRingWeight / FlareIsolation). Only the frame goes into that test, never
// the threshold, so raising the threshold can only take light away. Faded toward the frame edge
// (FlareBorderFadeUv) and thinned below a size floor (FlareSourceFloor).
float4 SourcePS(VOut i) : SV_Target {
    float3 s = brightTex.SampleLevel(linearClamp, i.uv, 0).rgb;
    // This source's peak luminance: nine taps of the frame spanning the quarter-resolution texel.
    float2 t = 3.0 / dims.xy;
    float peak = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            peak = max(peak, Luma(dofTex.SampleLevel(linearClamp, i.uv + float2(x, y) * t, 0).rgb));
    float2 ringUv = float2(dims.y / dims.x, 1) * kFlareIsoRadius;
    float ring = 0;
    [unroll] for (int k = 0; k < 16; ++k) {
        float a = (k + .5) * (2 * kPi / 16);
        float l = bright8Tex.SampleLevel(linearClamp, i.uv + float2(cos(a), sin(a)) * ringUv, 0).a;
        ring += smoothstep(peak - kFlareRelMargin - kFlareRelSoft, peak - kFlareRelMargin, l);
    }
    float iso = 1 - smoothstep(kFlareIsoLo, kFlareIsoHi, ring / 16);
    // A second, much wider ring: a source inside a large bright area (an opening full of sky, a lit window
    // wall) is not a point light even where its immediate surround is darker. Mirrors FlareWideRingWeight /
    // FlareWideIsolation. It reads the frame only, never the threshold.
    float2 wideUv = float2(dims.y / dims.x, 1) * kFlareWideRadius;
    float wideRing = 0;
    [unroll] for (int m = 0; m < 16; ++m) {
        float a = (m + .5) * (2 * kPi / 16);
        float l = bright8Tex.SampleLevel(linearClamp, i.uv + float2(cos(a), sin(a)) * wideUv, 0).a;
        wideRing += smoothstep(peak * kFlareWideLo, peak * kFlareWideHi, l);
    }
    iso *= 1 - smoothstep(kFlareWideIsoLo, kFlareWideIsoHi, wideRing / 16);
    // Specks a few pixels wide (sparkles on a brick wall) are not lights: they fade out below a size floor.
    float floorW = smoothstep(kFlareMinLo, kFlareMinHi, Luma(s));
    return float4(s * iso * floorW * BorderFade(i.uv), 1);
}

// Pre-filter the streak once: a continuous Gaussian across its width, and a box
// horizontally. The integral then samples a smooth map instead of three separated rails.
float4 SourceHPS(VOut i) : SV_Target {
    float3 acc = 0; float ws = 0;
    [unroll] for (int y = -4; y <= 4; ++y) {
        float w = exp(-y * y / 8.0);
        [unroll] for (int x = 0; x < 4; ++x)
            acc += sourceTex.SampleLevel(linearClamp, i.uv + float2((x - 1.5) * passInfo.x, y * kStreakSoft / (4 * dims.y)), 0).rgb * w;
        ws += 4 * w;
    }
    return float4(acc / ws, 1);
}

// Every ghost is an image of the aperture, so each source is turned into the aperture's shape here: the
// mean of a mip-filtered source map: each tap averages its area instead of leaving holes
// over a blade polygon of kGhostRadius (of the frame width), written at quarter resolution so the polygon
// keeps its corners, then saturated so a small lamp and a big one both make a clear blob (1 - exp(-gain * mean)).
float4 ApertureBlurPS(VOut i) : SV_Target {
    float R = kGhostRadius * lowRes.x;
    float2 t = 1.0 / lowRes.xy;
    float sector = 2 * kPi / flareB.w;
    float3 acc = 0;
    float wsum = 0;
    float footprint = kGhostRadius * dims.x * sqrt(kPi / 256.0) * lowRes.z / dims.x;
    float lod = max(0, log2(max(footprint * 1.25, 1)));
    [loop] for (int k = 0; k < 256; ++k) {
        float u = sqrt((k + .5) / 256.0);
        float th = k * 2.39996323;
        float tt = th - sector * floor(th / sector);
        float p = cos(kPi / flareB.w) / cos(tt - sector * .5);
        float bd = p + (1 - p) * kBladeRoundness;
        float w = bd * bd;
        acc += source8Tex.SampleLevel(linearClamp, i.uv + float2(cos(th), sin(th)) * (u * bd * R) * t, lod).rgb * w;
        wsum += w;
    }
    return float4(1 - exp(-kGhostBlobGain * acc / wsum), 1);
}

// A blob sampled at a ghost's source position, windowed by that position so it ends softly (never
// in a straight edge) at the frame border.
float3 Blob(float2 uv) {
    return ghostSrcTex.SampleLevel(linearClamp, uv, 0).rgb * BorderFade(uv);
}
float4 GhostPS(VOut i) : SV_Target {
    float2 cen = .5;
    float2 asp = float2(modes.z, 1);
    float3 acc = 0;
    int ghosts = (int)flareA.z;
    [loop] for (int k = 0; k < 8; ++k) {
        if (k >= ghosts) break;
        float m = kGhostScales[k];
        float2 src = cen + (i.uv - cen) / m;            // this pixel's source point: cen + (uv - cen) / m
        float ca = kGhostFringe * (1 + .5 * k);
        float3 s = float3(Blob(cen + (src - cen) * (1 + ca)).r, Blob(src).g, Blob(cen + (src - cen) * (1 - ca)).b);
        float r = length((src - cen) * asp);
        float fade = 1 - .7 * smoothstep(.4, 1.1, r);   // GhostFade
        float3 tint = lerp(1.0, HueColor(frac(.07 + .19 * k)), .3);
        acc += s * tint * fade * (kGhostGain / (1 + .3 * k));
    }
    // Halo: the mean of the blobs on the circle of this pixel's radius around the frame centre, so it is a
    // ring there whatever direction the source is in; scaled by how many blobs fit round that circle
    // (HaloRingBlobs) so the ring is as bright as one blob at any radius. Red and blue sit on slightly
    // different radii.
    float3 halo = 0;
    if (flareA.w > 0) {
        float r = length((i.uv - cen) * asp);
        float blobs = 2 * kPi * r / (2 * kGhostRadius * modes.z);
        [unroll] for (int c = 0; c < 3; ++c) {
            float rr = r * (1 + (1 - c) * kHaloFringe);
            float sum = 0;
            [loop] for (int j = 0; j < 128; ++j) {
                float a = (j + .5) * (2 * kPi / 128);
                float2 q = cen + float2(cos(a), sin(a)) * rr / asp;
                sum += Blob(q)[c];
            }
            halo[c] = sum / 128;
        }
        halo *= blobs * kHaloGain * flareA.w;
    }
    return float4(acc + halo, 1);
}

)HLSL" R"HLSL(
// Integrate with sub-texel spacing at every resolution. Fixed tap counts skipped
// narrow lights and produced repeated bars along long streaks.
float3 LineSum(Texture2D<float4> tex, float2 uv, float2 dirPx, float reachPx, float falloff) {
    uint tw, th; tex.GetDimensions(tw, th);
    // Horizontal integration uses the source's own pixel centres. Sliding the
    // camera changes only the smooth kernel weights, never which gaps get sampled.
    if (dirPx.y == 0) {
        float3 sum = 0;
        [loop] for (uint sx = 0; sx < tw; ++sx) {
            float x = abs((sx + .5) / tw - uv.x) * dims.x;
            if (x >= reachPx) continue;
            float u = x / reachPx;
            float w = exp(-falloff * u) * (1 - smoothstep(kLineTaper, 1, u));
            sum += tex.SampleLevel(linearClamp, float2((sx + .5) / tw, uv.y), 0).rgb * w;
        }
        return sum / tw;
    }
    float sourceStep = max(abs(dirPx.x) * tw / dims.x, abs(dirPx.y) * th / dims.y);
    int taps = max(1, (int)ceil(reachPx * sourceStep / .75));
    float stepPx = reachPx / taps;
    float3 acc = 0;
    [loop] for (int k = 0; k < taps; ++k) {
        float x = stepPx * (k + .5);
        float u = x / reachPx;
        float w = exp(-falloff * u) * (1 - smoothstep(kLineTaper, 1, u));
        float2 o = dirPx * x / dims.xy;
        acc += (tex.SampleLevel(linearClamp, uv + o, 0).rgb + tex.SampleLevel(linearClamp, uv - o, 0).rgb) * w;
    }
    return acc * stepPx / dims.x;
}
float4 StreakPS(VOut i) : SV_Target {
    float3 outc = 0;
    if (flareB.x > 0) {
        float reach = flareB.y * .8 * dims.x;
        float3 s = LineSum(sourceHTex, i.uv, float2(1, 0), reach, 3.0);
        s = lerp(s, streakTint.rgb * Luma(s) * 1.7, .85);
        outc += (1 - exp(-kStreakGain * s)) * flareB.x;
    }
    if (flareB.z > 0) {
        // One line per blade pair (SpikeLines / SpikeAngle in LensFxMath.h); each is symmetric about the
        // source, so every spike is centred on it.
        int blades = (int)flareB.w;
        int lines = (blades % 2 == 0) ? blades / 2 : blades;
        float reach = .22 * dims.x;
        float3 sp = 0;
        [loop] for (int j = 0; j < 12; ++j) {
            if (j >= lines) break;
            float a = kPi * .5 + j * kPi / lines;
            sp += LineSum(sourceTex, i.uv, float2(cos(a), sin(a)), reach, 4.0);
        }
        outc += (1 - exp(-kSpikeGain * sp)) * flareB.z;
    }
    return float4(outc, 1);
}
float4 GlowHPS(VOut i) : SV_Target {
    float2 t = float2(1.0 / lowRes.z, 0);
    float3 acc = 0; float ws = 0;
    [unroll] for (int k = -8; k <= 8; ++k) {
        float w = exp(-k * k / 24.0);
        acc += source8Tex.SampleLevel(linearClamp, i.uv + t * k * kGlowStep, 0).rgb * w; ws += w;
    }
    return float4(acc / ws, 1);
}
float4 GlowVPS(VOut i) : SV_Target {
    float2 t = float2(0, 1.0 / lowRes.w);
    float3 acc = 0; float ws = 0;
    [unroll] for (int k = -8; k <= 8; ++k) {
        float w = exp(-k * k / 24.0);
        acc += glowTex.SampleLevel(linearClamp, i.uv + t * k * kGlowStep, 0).rgb * w; ws += w;
    }
    return float4(1 - exp(-kGlowGain * acc / ws), 1);
}
)HLSL";

inline constexpr char kShaderComposite[] = R"HLSL(
static const float kDirtBoost = 3.0;         // kDirtBoost
float H(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
float VN(float2 p) {
    float2 i = floor(p), f = frac(p);
    f = f * f * (3 - 2 * f);
    return lerp(lerp(H(i), H(i + float2(1, 0)), f.x), lerp(H(i + float2(0, 1)), H(i + 1), f.x), f.y);
}
// Smudges and dust on the front element (or the user's own dirt image): only visible where flare
// light lands on them.
float Dirt(float2 uv) {
    if (flareC.z > .5) return dirtTex.SampleLevel(linearClamp, uv, 0).r;
    float2 p = uv * float2(modes.z, 1) * 3.0;
    float a = .5, s = 0;
    [unroll] for (int k = 0; k < 5; ++k) { s += a * VN(p); p *= 2.03; a *= .5; }
    return smoothstep(.3, .95, s * 1.5);
}
// How much of the frame is near-white: the mean of a soft step on the plain (ungated) luminance over a 16 x 16 grid
// of the eighth-resolution bright pass. It knows nothing of the threshold, so the flare's budget below never
// depends on it. A few lamps are ~0.005 of the frame, a sky-filled opening 0.2. Mirrors FlareBudget in LensFxMath.h.
float BrightArea() {
    float m = 0;
    [loop] for (int y = 0; y < 16; ++y)
        [loop] for (int x = 0; x < 16; ++x)
            m += smoothstep(kFlareBudgetLo, kFlareBudgetHi, bright8Tex.SampleLevel(linearClamp, (float2(x, y) + .5) / 16, 0).a);
    return m / 256;
}
float4 CompositePS(VOut i
#if COLOR_MS
    , uint sampleIndex : SV_SampleIndex
#endif
) : SV_Target {
    int2 p = int2(i.pos.xy);
#if COLOR_MS
    float4 origA = msaaColor.Load(p, sampleIndex);
#else
    float4 origA = colorTex.Load(int3(p, 0));
#endif
    float3 orig = origA.rgb;
    float3 col = orig;
    if (modes.x > .5) {
        float4 g = gatherTex.Load(int3(p, 0));
        col = lerp(orig, ToDisplay(g.rgb), g.a);
        float coc = cocTex.Load(int3(p, 0)).a;
        int dbg = (int)dofC.w;
        if (dbg == 1) {
            float m = saturate(abs(coc) / max(dofC.x, 1));
            col = orig * .3 + (coc < 0 ? float3(1, .25, .1) : float3(.1, .6, 1)) * (.15 + m * .85);
        } else if (dbg == 2) {
            float f = saturate(1 - abs(coc) / .75);
            col = lerp(orig * .5, float3(1, 0, .8), f * .7);
        }
    }
    if (modes.y > .5) {
        // Ghosts + halo and the glow are tinted and lit up by the dirt; the streak and spikes are not. Each
        // layer has already saturated on its own; the sum goes through a soft limiter (FlareLimit) and is
        // SCREEN-blended onto the picture, so the flare adds light but can never push a frame past white.
        float2 uv = i.uv;
        float3 gh = ghostTex.SampleLevel(linearClamp, uv, 0).rgb;
        float3 gl = glowTex.SampleLevel(linearClamp, uv, 0).rgb * flareC.x;
        float3 st = streakTex.SampleLevel(linearClamp, uv, 0).rgb;
        float dirt = 1 + flareC.y * Dirt(uv) * kDirtBoost;
        // A frame with a lot of near-white area (a sky-filled opening) would get streaks and glare from all of it, so
        // the flare's strength is a budget: full while that area is small, falling as it grows.
        float area = BrightArea();
        float budget = pow(saturate(kFlareBudgetArea / max(area, 1e-9)), kFlareBudgetPow);
        float3 f = 1 - exp(-max(((gh + gl) * flareTint.rgb * dirt + st) * flareA.x * budget, 0));
        col = flareC.w > .5 ? f : 1 - (1 - col) * (1 - f);
        if (flareC.w > 1.5) col = float3(area * 10, budget, 0);
    }
    return float4(saturate(col), origA.a);
}
)HLSL";

} // namespace Filmmaker::Lens
