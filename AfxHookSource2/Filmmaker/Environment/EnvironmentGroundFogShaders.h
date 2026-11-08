#pragma once
// GROUND FOG FLUID: Stable Fluids (Stam 1999) on the GPU, one 2D grid laid over the whole map's floor.
// State texel = (velocity x, velocity y, fog amount, unused); velocity in world units per second, fog
// amount 1 at rest. EnvironmentGroundFog.cpp dispatches these in order each step: Curl, Force, Advect,
// Divergence, Jacobi (several times), Project. Init lays the resting pattern down after a restart;
// Rebase moves the field onto a new pattern when Coverage changes.
// OverlayVS/OverlayPS draw the live field in a corner of
// the frame (`environment groundfog-view`). 8 x 8 threads per group.
namespace Filmmaker::Environment {
inline constexpr char GroundFogShader[] = R"HLSL(
#define BODIES 12
#define BLASTS 16
cbuffer GroundFogStep : register(b0) {
    float4 stepInfo;    // dt (s), cell size (u), refill share this step, swirl strength
    float4 windInfo;    // drift velocity xy (u/s), stir amount, absolute time (s)
    float4 gridInfo;    // grid min corner xy (world), cells across, cells down
    float4 bodyInfo;    // body count, disturbance strength, blast count
    float4 bodies[BODIES];       // xy world position, z footprint radius
    float4 bodyVelocity[BODIES]; // xy velocity (u/s)
    float4 cameraInfo;  // overlay: camera xy (world), view direction xy
    float4 floorInfo;   // overlay: floor map origin xy, texel size, 1 = bound
    float4 floorSize;   // overlay: floor map texels xy
    float4 restInfo;    // billow drift xy (u), floor coverage (1 = unbroken sheet), previous coverage
    float4 overlayInfo; // overlay: centre xy (world), span xy (world units across)
    float4 blasts[BLASTS]; // explosions, fires, smokes: xy world position, z radius, w outward push (u/s)
};
Texture2D<float4> stateIn : register(t0);
Texture2D<float> scalarIn : register(t1);   // curl for Force, divergence for Jacobi
Texture2D<float> pressureIn : register(t2);
Texture2D<float4> floorMap : register(t3);  // overlay: the map's outline
Texture2D<float4> beforeIn : register(t4);  // FrontMask: the HDR frame before CS2's transparent effects
RWTexture2D<float4> stateOut : register(u0);
RWTexture2D<float> scalarOut : register(u1);
SamplerState linearClamp : register(s0);

int2 Cells() { return int2(gridInfo.zw); }
bool Outside(int2 q) { return any(q >= Cells()); }
float4 State(int2 q) { return stateIn.Load(int3(clamp(q, int2(0, 0), Cells() - 1), 0)); }
float Scalar(int2 q) { return scalarIn.Load(int3(clamp(q, int2(0, 0), Cells() - 1), 0)); }
float Pressure(int2 q) { return pressureIn.Load(int3(clamp(q, int2(0, 0), Cells() - 1), 0)); }
float Hash(float2 p) { p = frac(p * float2(.1031, .1030)); p += dot(p, p.yx + 33.33); return frac((p.x + p.y) * p.x); }
float ValueNoise(float2 p) {
    float2 a = floor(p), f = frac(p); f = f * f * (3 - 2 * f);
    return lerp(lerp(Hash(a), Hash(a + float2(1, 0)), f.x), lerp(Hash(a + float2(0, 1)), Hash(a + 1), f.x), f.y);
}

// THE FOG AT REST. Fog is a quantity the flow carries, so this pattern is what you see move. Relief is
// fine unevenness everywhere (it curls and streaks into marbling as air moves through it); Coverage
// below 1 also breaks the fog into patches a few bodies wide (about 150 units), so several are in view
// at once. Everything fills back toward this over the Refill time. It drifts with the wind.
float RestFog(float2 world, float coverage) {
    float2 p = (world - restInfo.xy) * .0068;
    float t = windInfo.w;
    float mask = 1;
    if (coverage < .999) {
        float n = ValueNoise(p + t * float2(.011, -.007)) * .55 + ValueNoise(p * 2.3 + 5.1 - t * float2(.013, .009)) * .3 +
                  ValueNoise(p * 5.3 + 9.7 + t * float2(.017, -.021)) * .15;
        n = saturate((n - .5) * 2.6 + .5);
        float threshold = 1 - coverage;
        mask = smoothstep(threshold - .2, threshold + .2, n);
    }
    float relief = ValueNoise(p * 3.1 + 21.3 + t * float2(-.02, .015)) * .6 + ValueNoise(p * 7.7 + 3.3 - t * float2(.03, .025)) * .4;
    return mask * (.7 + .6 * relief);
}
[numthreads(8, 8, 1)] void Init(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    if (Outside(q)) return;
    stateOut[q] = float4(0, 0, RestFog(gridInfo.xy + (float2(q) + .5) * stepInfo.y, restInfo.z), 0);
}
// Coverage changed (restInfo.w was the old value): move the field onto the new pattern at once, keeping
// what bodies did to it, which is its difference from rest. The slider answers immediately, paused too.
[numthreads(8, 8, 1)] void Rebase(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    if (Outside(q)) return;
    float2 world = gridInfo.xy + (float2(q) + .5) * stepInfo.y;
    float4 s = stateIn.Load(int3(q, 0));
    stateOut[q] = float4(s.xy, clamp(s.z + RestFog(world, restInfo.z) - RestFog(world, restInfo.w), 0, 1.3), 0);
}
// Divergence-free noise: the curl of a smooth field.
float2 CurlNoise(float2 p) {
    float e = .5, n0 = ValueNoise(p);
    return float2(ValueNoise(p + float2(0, e)) - n0, -(ValueNoise(p + float2(e, 0)) - n0)) / e;
}
// Catmull-Rom over 4 x 4 texels, kept within the middle four so it cannot overshoot. A bilinear lookup
// blurs the field a little every step, and at this grid size that erased the streaks within a second.
float CatmullFog(float2 pos) {
    float2 f = pos - .5;
    int2 b = int2(floor(f));
    float2 t = f - float2(b);
    float2 w[4] = {t * (-.5 + t * (1 - .5 * t)), 1 + t * t * (-2.5 + 1.5 * t), t * (.5 + t * (2 - 1.5 * t)), t * t * (-.5 + .5 * t)};
    float sum = 0, lo = 1e9, hi = -1e9;
    [unroll] for (int y = 0; y < 4; ++y) [unroll] for (int x = 0; x < 4; ++x) {
        float v = State(b + int2(x - 1, y - 1)).z;
        sum += v * w[x].x * w[y].y;
        if (x == 1 || x == 2) if (y == 1 || y == 2) { lo = min(lo, v); hi = max(hi, v); }
    }
    return clamp(sum, lo, hi);
}

// Vorticity (1/s): how fast the flow turns at each cell.
[numthreads(8, 8, 1)] void Curl(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    if (Outside(q)) return;
    float h = stepInfo.y;
    scalarOut[q] = ((State(q + int2(1, 0)).y - State(q - int2(1, 0)).y) -
                    (State(q + int2(0, 1)).x - State(q - int2(0, 1)).x)) * .5 / h;
}

// Forces: bodies drag and part the fog and thin it where they pass, vorticity confinement keeps
// the eddies in their wake curling, and a faint stir keeps still air from looking frozen.
[numthreads(8, 8, 1)] void Force(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    if (Outside(q)) return;
    float dt = stepInfo.x, h = stepInfo.y;
    float4 s = State(q);
    float2 v = s.xy;
    float fog = s.z;
    // Vorticity confinement (Fedkiw et al. 2001): push along the curl's gradient, sideways to it.
    float w = Scalar(q);
    float2 g = float2(abs(Scalar(q + int2(1, 0))) - abs(Scalar(q - int2(1, 0))),
                      abs(Scalar(q + int2(0, 1))) - abs(Scalar(q - int2(0, 1)))) * .5;
    float gl = length(g);
    if (gl > 1e-5) {
        float2 n = g / gl;
        v += stepInfo.w * h * float2(n.y * w, -n.x * w) * dt;
    }
    float2 world = gridInfo.xy + (float2(q) + .5) * h;
    // Stir: slow eddies at two sizes, so the fog keeps moving and marbling with nobody near it.
    if (windInfo.z > 0)
        v += (CurlNoise(world * .004 + windInfo.w * float2(.021, -.017)) +
              CurlNoise(world * .011 + 7.3 - windInfo.w * float2(.03, .02)) * .6) * windInfo.z * dt;
    int count = (int)bodyInfo.x;
    [loop] for (int i = 0; i < count; ++i) {
        float2 rel = world - bodies[i].xy;
        float r = bodies[i].z, d = length(rel);
        // Nothing past the rim (4 radii) feels this body.
        if (d >= r * 4) continue;
        float k = 1 - smoothstep(r * .55, r * 1.9, d);
        float2 bv = bodyVelocity[i].xy;
        float speed = length(bv);
        float strength = bodyInfo.y;
        // The body carries the air it occupies with it (that shear is what rolls the wake up)...
        v = lerp(v, bv, saturate(k * strength * 10 * dt));
        // ...and shoves the fog in front and beside it outward.
        v += (rel / max(d, 1)) * speed * k * strength * saturate(14 * dt) * .7;
        // Moving through fog clears it from a footprint a little wider than the body (the legs swing
        // through it); standing still only thins it. The fog it clears is shoved into a thicker rim
        // just outside, the bow wave that shows the push even where the fog is dense.
        float clearZone = 1 - smoothstep(r * .7, r * 2.4, d);
        fog -= fog * saturate(clearZone * strength * dt * (.12 + speed / 35));
        float rim = smoothstep(r * 1.6, r * 2.6, d) * (1 - smoothstep(r * 2.6, r * 4, d));
        fog = min(fog + rim * strength * dt * speed / 200 * saturate(fog + .1), 1.3);
    }
    // Explosions blow the fog away, fires burn it off and smokes displace it: so flames and clouds stand
    // in clear air instead of behind fog drawn over them.
    int blastCount = (int)bodyInfo.z;
    [loop] for (int i = 0; i < blastCount; ++i) {
        float2 rel = world - blasts[i].xy;
        float r = blasts[i].z, d = length(rel);
        if (d >= r) continue;
        float k = 1 - smoothstep(r * .35, r, d);
        v += (rel / max(d, 1)) * blasts[i].w * k * saturate(14 * dt);
        fog -= fog * saturate(k * dt * 9);
    }
    stateOut[q] = float4(v, max(fog, 0), 0);
}

// Semi-Lagrangian advection of velocity and fog by the flow plus the drift, then fog refills toward
// rest and the motion slowly dies down.
[numthreads(8, 8, 1)] void Advect(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    if (Outside(q)) return;
    float dt = stepInfo.x, h = stepInfo.y;
    float4 s = stateIn.Load(int3(q, 0));
    float2 back = (float2(q) + .5) - (s.xy + windInfo.xy) * dt / h;
    float2 uv = back / float2(Cells());
    float rest = RestFog(gridInfo.xy + (float2(q) + .5) * h, restInfo.z);
    float4 a = float4(0, 0, rest, 0);
    if (all(uv >= 0) && all(uv <= 1)) {
        a = stateIn.SampleLevel(linearClamp, uv, 0);
        a.z = CatmullFog(back);
    }
    a.z = lerp(a.z, rest, stepInfo.z);
    a.xy *= exp(-dt * .6);
    stateOut[q] = float4(a.xy, a.z, 0);
}

[numthreads(8, 8, 1)] void Divergence(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    if (Outside(q)) return;
    scalarOut[q] = .5 * ((State(q + int2(1, 0)).x - State(q - int2(1, 0)).x) +
                         (State(q + int2(0, 1)).y - State(q - int2(0, 1)).y));
}

// A soft copy of the fog amount at a quarter of the grid, which the top of the layer follows: the fine
// marbling stays in the density, and the surface stays a gentle dune instead of spiking along it.
[numthreads(8, 8, 1)] void Smooth(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    if (any(q >= (Cells() + 3) / 4)) return;
    int2 c = q * 4 + 2;
    float sum = 0, weight = 0;
    [unroll] for (int y = -2; y <= 2; ++y) [unroll] for (int x = -2; x <= 2; ++x) {
        float w = (3 - abs(x)) * (3 - abs(y));
        sum += State(c + int2(x, y) * 2).z * w;
        weight += w;
    }
    scalarOut[q] = sum / weight;
}

// WHAT THE TRANSPARENT EFFECTS DID TO THE FRAME. Particles write no depth, so the fog cannot tell a close
// flame or explosion cloud from the wall behind it. The finished HDR frame (stateIn) against the same
// frame before CS2's transparent effects (beforeIn) shows it, whatever drew them.
// r: the share of the pixel's light an effect added (flames, tracers, sparks).
// g: the share of what was behind it an effect hid (explosion smoke, glass).
// Both are shares, not cut-offs: a thresholded mask broke a flame into hard-edged chunks.
[numthreads(8, 8, 1)] void FrontMask(uint3 id : SV_DispatchThreadID) {
    int3 q = int3(id.xy, 0);
    const float3 kLuma = float3(.2126, .7152, .0722);
    float3 after = max(stateIn.Load(q).rgb, 0), before = max(beforeIn.Load(q).rgb, 0);
    // By channel: a gold object over grey stone has about the same brightness, and still hides the stone.
    float3 up = max(after - before, 0), down = max(before - after, 0), hid = down / (before + .01);
    float added = max(up.r, max(up.g, up.b)), removed = max(down.r, max(down.g, down.b));
    stateOut[q.xy] = float4(saturate(added / (added + .35 * dot(before, kLuma) + .01)) * smoothstep(.004, .04, added),
                            saturate(max(hid.r, max(hid.g, hid.b))) * smoothstep(.004, .04, removed), 0, 0);
}

// A black cloud over something already dark changes next to nothing, so the mask follows the shapes
// behind the cloud. But a cloud is one soft thing: what it hides close by says what it hides here.
// This holds the average share nearby at a sixteenth of the size: a cloud fills it, a thin streak barely
// shows. The composite uses it only on pixels dark enough to be under such a cloud.
[numthreads(8, 8, 1)] void FrontNearby(uint3 id : SV_DispatchThreadID) {
    uint w, h, cw, ch;
    stateIn.GetDimensions(w, h);
    scalarOut.GetDimensions(cw, ch);
    float2 centre = (id.xy + .5) * float2(w, h) / float2(cw, ch);
    float reach = h / 7., sum = 0, weight = 0;
    for (int y = -5; y <= 5; ++y)
        for (int x = -5; x <= 5; ++x) {
            float2 o = float2(x, y) / 5;
            int2 q = clamp(int2(centre + o * reach), int2(0, 0), int2(w, h) - 1);
            float k = saturate((1 - length(o)) * 2);
            sum += stateIn.Load(int3(q, 0)).g * k;
            weight += k;
        }
    scalarOut[id.xy] = saturate(sum / weight * 2);
}

// One Jacobi relaxation of the pressure Poisson equation (cell units).
[numthreads(8, 8, 1)] void Jacobi(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    if (Outside(q)) return;
    scalarOut[q] = (Pressure(q + int2(1, 0)) + Pressure(q - int2(1, 0)) + Pressure(q + int2(0, 1)) +
                    Pressure(q - int2(0, 1)) - Scalar(q)) * .25;
}

// Subtract the pressure gradient: the flow goes around and past instead of piling up.
[numthreads(8, 8, 1)] void Project(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    if (Outside(q)) return;
    float4 s = stateIn.Load(int3(q, 0));
    s.xy -= .5 * float2(Pressure(q + int2(1, 0)) - Pressure(q - int2(1, 0)),
                        Pressure(q + int2(0, 1)) - Pressure(q - int2(0, 1)));
    stateOut[q] = s;
}

// THE LIVE OVERLAY: the field from above, north up, around overlayInfo. Bright is heaped fog, mid grey
// is at rest, dark is cleared; blue tints moving air. Cells with no floor are dimmed so the map's
// outline reads. Orange dots are players, red dots are blasts, fires and smokes, and the green dot and
// tick are the camera and where it looks.
struct OverlayVertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
OverlayVertex OverlayVS(uint id : SV_VertexID) {
    OverlayVertex o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.uv = p;
    o.position = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 OverlayPS(OverlayVertex i) : SV_Target {
    float2 world = overlayInfo.xy + (float2(i.uv.x, 1 - i.uv.y) - .5) * overlayInfo.zw;
    float2 tex = (world - gridInfo.xy) / (gridInfo.zw * stepInfo.y);
    bool onGrid = all(tex > 0) && all(tex < 1);
    float4 s = stateIn.SampleLevel(linearClamp, tex, 0);
    float3 color = onGrid ? saturate(s.z / 1.3).xxx : float3(0, 0, 0);
    if (onGrid) color = lerp(color, float3(.15, .55, 1), saturate(length(s.xy) / 160) * .6);
    if (floorInfo.w > .5) {
        int2 t = int2(floor((world - floorInfo.xy) / floorInfo.z));
        bool mapped = all(t >= 0) && all(t < int2(floorSize.xy)) && floorMap.Load(int3(t, 0)).x > -5e5;
        if (!mapped) color *= .16;
    }
    // Sizes are in screen pixels, so markers stay readable at any zoom.
    float unit = abs(ddx(world.x));
    int count = (int)bodyInfo.x;
    for (int b = 0; b < count; ++b) {
        float d = length(world - bodies[b].xy);
        color = lerp(color, float3(1, .55, .1), 1 - smoothstep(unit * 2.5, unit * 3.5, d));
    }
    int blastCount = (int)bodyInfo.z;
    for (int b = 0; b < blastCount; ++b) {
        float d = length(world - blasts[b].xy);
        color = lerp(color, float3(1, .15, .1), 1 - smoothstep(unit * 2.5, unit * 3.5, d));
    }
    float2 toCamera = world - cameraInfo.xy;
    float along = dot(toCamera, cameraInfo.zw);
    float beside = abs(toCamera.x * cameraInfo.w - toCamera.y * cameraInfo.z);
    float tick = (along > 0 && along < unit * 14) ? 1 - smoothstep(unit * .9, unit * 1.7, beside) : 0;
    color = lerp(color, float3(.2, 1, .4), max(tick, 1 - smoothstep(unit * 2.5, unit * 3.5, length(toCamera))));
    float2 fromEdge = min(i.uv, 1 - i.uv) * overlayInfo.zw;
    color = lerp(float3(1, .72, .2), color, smoothstep(1.5, 2.5, min(fromEdge.x, fromEdge.y) / unit));
    // The frame's target decodes to display light, so this keeps the greys as they read above.
    return float4(pow(max(color, 0), 2.2), 1);
}
)HLSL";
inline constexpr const char *GroundFogShaderEntries[] = {"Curl", "Force", "Advect", "Divergence", "Jacobi", "Project", "Init", "Rebase", "Smooth", "FrontMask", "FrontNearby"};
inline constexpr int GroundFogShaderEntryCount = int(sizeof(GroundFogShaderEntries) / sizeof(GroundFogShaderEntries[0]));
} // namespace Filmmaker::Environment
