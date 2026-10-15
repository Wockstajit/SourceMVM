#pragma once
namespace Filmmaker::Relight {
// Draws every lightmapped triangle (and every texel-sized point) into the atlas in its own
// lightmap coordinates, so each texel is shaded with the world position and normal it stands for.
inline constexpr char kLightmapShader[] = R"HLSL(
cbuffer Params : register(b0) {
    float4 boundsLow;     // xyz world minimum
    float4 boundsExtent;  // xyz world size
    float4 modeFill;      // x mode, yzw flat fill
    float4 spare;
};
struct VSIn { float4 positionPage : POSITION; float2 uv : TEXCOORD0; float2 oct : NORMAL; };
struct VSOut { float4 position : SV_Position; float3 world : TEXCOORD0; float3 normal : TEXCOORD1; };
float3 DecodeOctahedral(float2 o) {
    float3 n = float3(o.x, o.y, 1 - abs(o.x) - abs(o.y));
    if (n.z < 0) n.xy = (1 - abs(n.yx)) * float2(n.x >= 0 ? 1 : -1, n.y >= 0 ? 1 : -1);
    return normalize(n);
}
VSOut VS(VSIn i) {
    VSOut o;
    o.world = boundsLow.xyz + i.positionPage.xyz * boundsExtent.xyz;
    o.normal = DecodeOctahedral(i.oct);
    // Atlas texel space: u right, v down, so clip y runs the other way.
    o.position = float4(i.uv.x * 2 - 1, 1 - i.uv.y * 2, .5, 1);
    return o;
}
float4 PS(VSOut i) : SV_Target {
    float3 n = normalize(i.normal);
    if (modeFill.x < 1.5) {
        if (modeFill.x < .5) return float4(modeFill.yzw, 1);
        // A 64-unit checker in WORLD space, tinted by the normal: if the layout is right the
        // pattern is continuous across every wall and floor and the tint follows each surface.
        float3 cell = floor(i.world / 64);
        float parity = fmod(abs(cell.x + cell.y + cell.z), 2);
        return float4((abs(n) * .5 + .5) * (parity > .5 ? .9 : .15), 1);
    }
    return float4(modeFill.yzw, 1);
}
)HLSL";
}
