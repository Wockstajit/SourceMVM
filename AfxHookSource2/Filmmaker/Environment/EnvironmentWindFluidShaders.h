#pragma once
// WIND FLUID: the air the Gusts control moves, as Stable Fluids (Stam 1999) on the GPU: one 2D grid
// laid over the whole map, seen from above. State texel = (velocity x, velocity y, what the wind
// carries, 1 inside a building); velocity in world units a second. EnvironmentWindFluid.cpp
// dispatches these in order each step: Curl, Force, Advect, Divergence, Jacobi (several times),
// Project. OverlayVS/OverlayPS draw the live field in a corner of the frame (`environment
// wind-view`). 8 x 8 threads a group.
//
// WHAT DRIVES IT. The steady wind blows across the whole map, and gust FRONTS (bands a few hundred
// units deep and much longer across the wind) sweep downwind through it: inside a front the air is
// pushed to several times the steady speed. What it carries (drawn as blown snow, spray or ash by
// GustVolumeMarch) is lifted in banks that ride downwind (Banks), torn by a foam pattern (Lifted),
// more while a front passes. The solver does the rest: the air cannot pass through buildings, so it
// squeezes down streets, spills round corners and curls into eddies in their lee, and what it
// carries is drawn out along the way it went. Players stir it as they move.
namespace Filmmaker::Environment {
inline constexpr char WindFluidShader[] = R"HLSL(
#define BODIES 12
cbuffer WindStep : register(b0) {
    float4 stepInfo;    // dt (s), cell size (u), absolute time (s), swirl strength
    float4 windInfo;    // steady wind xy (u/s), how fast the fronts travel (u/s), share relaxed to the wind this step
    float4 gridInfo;    // grid min corner xy (world), cells across, cells down
    float4 bodyInfo;    // body count, 1 = the obstacle map is bound, how much of what is carried stays after this step
    float4 bodies[BODIES];       // xy world position, z footprint radius
    float4 bodyVelocity[BODIES]; // xy velocity (u/s)
    float4 overlayInfo; // overlay: centre xy (world), span xy (world units across)
    float4 cameraInfo;  // overlay: camera xy (world)
};
Texture2D<float4> stateIn : register(t0);
Texture2D<float> scalarIn : register(t1);   // curl for Force, divergence for Jacobi
Texture2D<float> pressureIn : register(t2);
Texture2D<float> obstacleIn : register(t3); // 1 inside a building
RWTexture2D<float4> stateOut : register(u0);
RWTexture2D<float> scalarOut : register(u1);
SamplerState linearClamp : register(s0);

int2 Cells() { return int2(gridInfo.zw); }
bool Outside(int2 q) { return any(q >= Cells()); }
int2 Inside(int2 q) { return clamp(q, int2(0, 0), Cells() - 1); }
bool Solid(int2 q) { return bodyInfo.y > .5 && obstacleIn.Load(int3(Inside(q), 0)) > .5; }
// How open the ground around an air cell is, 0..1 (WindAirMap keeps it in the map's low half).
float Open(int2 q) {
    if (bodyInfo.y <= .5) return 1;
    float m = obstacleIn.Load(int3(Inside(q), 0));
    return m > .5 ? 0 : saturate(m * 255 / 127);
}
float4 State(int2 q) { return stateIn.Load(int3(Inside(q), 0)); }
float2 Velocity(int2 q) { return Solid(q) ? float2(0, 0) : State(q).xy; }
float Scalar(int2 q) { return scalarIn.Load(int3(Inside(q), 0)); }
float Hash(float2 p) { p = frac(p * float2(.1031, .1030)); p += dot(p, p.yx + 33.33); return frac((p.x + p.y) * p.x); }
float ValueNoise(float2 p) {
    float2 a = floor(p), f = frac(p); f = f * f * (3 - 2 * f);
    return lerp(lerp(Hash(a), Hash(a + float2(1, 0)), f.x), lerp(Hash(a + float2(0, 1)), Hash(a + 1), f.x), f.y);
}
// Divergence-free noise: the curl of a smooth field.
float2 CurlNoise(float2 p) {
    float e = .5, n0 = ValueNoise(p);
    return float2(ValueNoise(p + float2(0, e)) - n0, -(ValueNoise(p + float2(e, 0)) - n0)) / e;
}
float2 Hash2(float2 p) { return float2(Hash(p), Hash(p + 19.19)); }
// A FOAM LACE: the white veins of sea foam, as the edges between cells (F2 - F1 of a cell noise),
// over two sizes. `p` in cells. 1 on a vein, 0 in the dark between.
float Lace(float2 p) {
    float2 a = floor(p), f = p - a;
    float f1 = 9, f2 = 9;
    [unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x) {
        float2 o = float2(x, y), c = o + Hash2(a + o) * .85 + .075 - f;
        float d = dot(c, c);
        if (d < f1) { f2 = f1; f1 = d; } else if (d < f2) f2 = d;
    }
    return 1 - smoothstep(.02, .2, sqrt(f2) - sqrt(f1));
}
float FoamLace(float2 p) { return max(Lace(p), Lace(p * 2.3 + 5.1) * .7); }
// THE TEXTURE OF WHAT THE WIND LIFTS: a foam lace fixed to the ground (cells about 100 units across),
// cut by a second lace twice as fine that drifts slowly over it (2.5 and 5 units a second). The first
// shows only through the dark between the second's veins (it is taken away 200 times as strongly as
// it is put in), squared, so a bank is torn into threads and clumps rather than lifted evenly.
float Lifted(float2 world) {
    float t = stepInfo.z;
    float a = FoamLace(world / 100);
    float b = FoamLace(world / 50 + t * float2(.05, .1));
    float d = max(a - 200 * b, 0);
    return d * d;
}
float2 Along() { float s = length(windInfo.xy); return s > 1 ? windInfo.xy / s : float2(1, 0); }
// How far inside a gust front a place is, 0..1.
float Front(float2 world) {
    float2 d = Along();
    float2 q = float2(dot(world, d) - stepInfo.z * windInfo.z, dot(world, float2(-d.y, d.x)));
    float n = ValueNoise(q / float2(1100, 2800) + stepInfo.z * float2(0, .05)) * .65 + ValueNoise(q / float2(380, 900) + 7.3 - stepInfo.z * float2(0, .09)) * .35;
    return smoothstep(.46, .76, n);
}
// WHERE THE WIND LIFTS MOST: in banks a few hundred units across (about 620 along the wind and 380
// across it, with smaller ones among them) that ride downwind at seven tenths of the fronts' speed:
// blowing snow sweeping over the ground in banks, over a low drift everywhere the wind blows. Lifted
// evenly, the load was a thin speckle over the whole map that no layer could be drawn from. 0..1.
float Banks(float2 world) {
    float2 d = Along(), n = float2(-d.y, d.x);
    float2 q = float2(dot(world, d) - stepInfo.z * windInfo.z * .7, dot(world, n));
    float b = ValueNoise(q / float2(620, 380) + float2(0, stepInfo.z * .03)) * .62 +
              ValueNoise(q / float2(240, 150) + 5.3 - float2(0, stepInfo.z * .05)) * .38;
    return smoothstep(.40, .70, b);
}
// The wind the map would have with nothing in its way. (Among houses, Sheltered.)
// Always gusty, with the fronts as surges on top: with nothing between fronts the gusts went out
// altogether for seconds at a time.
float2 Target(float2 world) { return Along() * (length(windInfo.xy) + (140 + length(windInfo.xy)) * 1.7 * (.3 + .7 * Front(world))); }
// STREETS ARE SHELTERED. Down among houses the wind is a good part slower than over the roofs and
// lifts less: the share kept, from about half in a narrow street to all of it in the open. At the
// open wind, with what a gust lifts put in evenly, Dust2's catwalk ran fast and stood full of blown
// snow while every gust crossed it (reported 2026-11-18).
float Sheltered(int2 q) { return lerp(.5, 1, Open(q)); }

[numthreads(8, 8, 1)] void Curl(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    if (Outside(q)) return;
    scalarOut[q] = (Velocity(q + int2(1, 0)).y - Velocity(q - int2(1, 0)).y - Velocity(q + int2(0, 1)).x + Velocity(q - int2(0, 1)).x) / (2 * stepInfo.y);
}
[numthreads(8, 8, 1)] void Force(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    if (Outside(q)) return;
    if (Solid(q)) { stateOut[q] = float4(0, 0, 0, 1); return; }
    float4 s = State(q);
    float2 world = gridInfo.xy + (float2(q) + .5) * stepInfo.y;
    float2 target = Target(world) * Sheltered(q);
    float2 v = s.xy + (target - s.xy) * windInfo.w;
    // Vorticity confinement: the grid smears eddies away, this winds them back up.
    float2 slope = float2(abs(Scalar(q + int2(1, 0))) - abs(Scalar(q - int2(1, 0))), abs(Scalar(q + int2(0, 1))) - abs(Scalar(q - int2(0, 1))));
    float steep = length(slope);
    if (steep > 1e-5) v += float2(slope.y, -slope.x) / steep * Scalar(q) * stepInfo.y * stepInfo.w * stepInfo.x;
    // Turbulence in step with how fast the air moves: a gust tumbles and wanders instead of running
    // in ruled lines, and still air stays still.
    v += (CurlNoise(world * .0045 + stepInfo.z * float2(.11, -.08)) * 1.6 + CurlNoise(world * .013 + 4.7 - stepInfo.z * float2(.19, .15)) +
          CurlNoise(world * .04 + 9.1 + stepInfo.z * float2(.4, .3)) * .7) * length(v) * .9 * stepInfo.x;
    // Round turbulence, swirls about 490 units across rolling slowly downwind: the big eddies a gust
    // bank is turned by.
    v += CurlNoise((world - Along() * stepInfo.z * windInfo.z * .25) / 490) * length(v) * 1.4 * stepInfo.x;
    // Players drag the air with them.
    int count = int(bodyInfo.x);
    for (int i = 0; i < count; ++i) {
        float2 away = world - bodies[i].xy;
        float share = saturate(1 - length(away) / max(bodies[i].z, 1));
        v = lerp(v, bodyVelocity[i].xy, share * share * .8);
    }
    // What the wind lifts is put in where a bank passes (Banks), torn by the foam pattern (Lifted),
    // more while a front passes, and thins out over a few seconds; the flow draws it out and curls it.
    float front = .3 + .7 * Front(world);
    float lift = (.35 + .65 * Banks(world)) * (.55 + .45 * saturate(Lifted(world) * 3)) * front * Sheltered(q);
    float carried = s.z * bodyInfo.z + lift * 1.4 * stepInfo.x;
    // The edge of the map is open air: the wind comes in there as it is.
    if (any(q < 2) || any(q >= Cells() - 2)) v = target;
    stateOut[q] = float4(v, min(carried, 2), 0);
}
[numthreads(8, 8, 1)] void Advect(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    if (Outside(q)) return;
    if (Solid(q)) { stateOut[q] = float4(0, 0, 0, 1); return; }
    float2 from = float2(q) + .5 - State(q).xy * stepInfo.x / stepInfo.y;
    float4 s = stateIn.SampleLevel(linearClamp, from / gridInfo.zw, 0);
    // What was read may have blended in a wall's still air: that is the wall slowing it.
    // What the air carries is read with a sharper filter (Catmull-Rom over 4 x 4 cells, kept within
    // the middle four): a plain blend blurs it a little every step, and its threads were gone in a
    // second.
    float2 f = from - .5;
    int2 b = int2(floor(f));
    float2 t = f - float2(b);
    float2 w[4] = {t * (-.5 + t * (1 - .5 * t)), 1 + t * t * (-2.5 + 1.5 * t), t * (.5 + t * (2 - 1.5 * t)), t * t * (-.5 + .5 * t)};
    float sum = 0, lo = 1e9, hi = -1e9;
    [unroll] for (int y = 0; y < 4; ++y) [unroll] for (int x = 0; x < 4; ++x) {
        float c = State(b + int2(x - 1, y - 1)).z;
        sum += c * w[x].x * w[y].y;
        if (x == 1 || x == 2) if (y == 1 || y == 2) { lo = min(lo, c); hi = max(hi, c); }
    }
    stateOut[q] = float4(s.xy, clamp(sum, lo, hi), 0);
}
[numthreads(8, 8, 1)] void Divergence(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    if (Outside(q)) return;
    // AIR GOES OVER THE ROOFS. Among houses only part of what flows together is pushed on through the
    // streets; the rest is taken as rising over the roofs (falling back down where it spreads), so a
    // street through a block is not a jet carrying the whole gust (WindAirMap). In the open the fluid
    // keeps its volume in full.
    float open = Open(q);
    scalarOut[q] = (Velocity(q + int2(1, 0)).x - Velocity(q - int2(1, 0)).x + Velocity(q + int2(0, 1)).y - Velocity(q - int2(0, 1)).y) / (2 * stepInfo.y) * open * open;
}
float Pressure(int2 q, float here) { return Solid(q) ? here : pressureIn.Load(int3(Inside(q), 0)); }
[numthreads(8, 8, 1)] void Jacobi(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    if (Outside(q)) return;
    float here = pressureIn.Load(int3(q, 0));
    scalarOut[q] = (Pressure(q + int2(1, 0), here) + Pressure(q - int2(1, 0), here) + Pressure(q + int2(0, 1), here) + Pressure(q - int2(0, 1), here) -
                    Scalar(q) * stepInfo.y * stepInfo.y) * .25;
}
[numthreads(8, 8, 1)] void Project(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    if (Outside(q)) return;
    if (Solid(q)) { stateOut[q] = float4(0, 0, 0, 1); return; }
    float4 s = State(q);
    float here = pressureIn.Load(int3(q, 0));
    float2 v = s.xy - float2(Pressure(q + int2(1, 0), here) - Pressure(q - int2(1, 0), here), Pressure(q + int2(0, 1), here) - Pressure(q - int2(0, 1), here)) / (2 * stepInfo.y);
    // NOT MUCH FASTER THAN THE WIND IT IS PART OF. A gust squeezed between two walls speeds up; the air
    // is held to a third over the wind it would have there. (The cap alone left Dust2's catwalk a steady
    // jet at this speed: what keeps a street from carrying a whole gust is the air going over the
    // roofs, in Divergence.)
    float2 world = gridInfo.xy + (float2(q) + .5) * stepInfo.y;
    float most = length(Target(world)) * Sheltered(q) * 1.35 + 60, now = length(v);
    if (now > most) v *= most / now;
    // No air goes into a wall.
    if (Solid(q + int2(1, 0))) v.x = min(v.x, 0);
    if (Solid(q - int2(1, 0))) v.x = max(v.x, 0);
    if (Solid(q + int2(0, 1))) v.y = min(v.y, 0);
    if (Solid(q - int2(0, 1))) v.y = max(v.y, 0);
    stateOut[q] = float4(clamp(v, -1500, 1500), s.z, 0);
}
struct OverlayVertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
OverlayVertex OverlayVS(uint id : SV_VertexID) {
    OverlayVertex o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
// From above, north up: buildings dark red, what the wind carries white, fast air blue, the camera green.
float4 OverlayPS(OverlayVertex i) : SV_Target {
    float2 world = overlayInfo.xy + (float2(i.uv.x, 1 - i.uv.y) - .5) * overlayInfo.zw;
    float2 uv = (world - gridInfo.xy) / (gridInfo.zw * stepInfo.y);
    if (any(uv < 0) || any(uv > 1)) return float4(0, 0, 0, .6);
    float4 s = stateIn.SampleLevel(linearClamp, uv, 0);
    float3 colour = float3(.04, .05, .08) + float3(.1, .3, .9) * saturate(length(s.xy) / 700) + saturate(s.z) * .8;
    if (bodyInfo.y > .5 && obstacleIn.SampleLevel(linearClamp, uv, 0) > .5) colour = float3(.28, .06, .05);
    if (length(world - cameraInfo.xy) < overlayInfo.z * .012) colour = float3(.2, 1, .3);
    return float4(colour, .92);
}
)HLSL";
inline constexpr const char *WindFluidShaderEntries[] = {"Curl", "Force", "Advect", "Divergence", "Jacobi", "Project"};
inline constexpr int WindFluidShaderEntryCount = int(sizeof(WindFluidShaderEntries) / sizeof(WindFluidShaderEntries[0]));
} // namespace Filmmaker::Environment
