#pragma once
#include "EnvironmentMath.h"
namespace Filmmaker::Environment {
// Reject depth discontinuities and hard corners while allowing small neighboring facets to blend.
inline float CoatingNormalWeight(Vec3 centerNormal, Vec3 neighborNormal, Vec3 offset, float radius) {
    float agreement = std::clamp((Dot(centerNormal, neighborNormal) - .8f) / .15f, 0.f, 1.f);
    agreement *= agreement * (3 - 2 * agreement);
    float distance2 = Dot(offset, offset);
    float planeError = std::abs(Dot(offset, centerNormal));
    float planeLimit = .5f + std::sqrt(distance2) * .3f;
    float sameSurface = std::clamp(1 - planeError / planeLimit, 0.f, 1.f);
    return agreement * sameSurface * std::exp(-distance2 / (radius * radius));
}
inline float CoatingDetailFade(float footprint, float frequency) {
    float t = std::clamp((footprint * frequency - .25f) / .75f, 0.f, 1.f);
    return 1 - t * t * (3 - 2 * t);
}
// SETTLED SNOW keeps the map image's broad shading: its brightness follows the luminance of the
// surface it covers on a curve that lifts dark ground (snow is far brighter than asphalt). That curve
// alone lifted night ground at luminance .02 to .35, seventeen times, and switching snow on under a
// night sky turned the scene grey (issue 135). Snow reflects a few times what the ground under it
// does, never more, so it is also bounded by kSnowMaxLift times the surface. Daylit ground (about
// .15 and up) keeps the curve. Mirror of SnowCoverShade in EnvironmentClimateShaders.h.
inline constexpr float kSnowMaxLift = 4.f;
inline float SnowCoverShade(float mapLuminance) {
    const float curve = .03f + .97f * std::pow(std::clamp(mapLuminance, 0.f, 1.f), .28f);
    return (std::min)(curve, kSnowMaxLift * (std::max)(mapLuminance, 0.f));
}
// A LAMP SEEN IN WATER. Wet ground and puddles show a placed light as a highlight: water's own
// Fresnel at the half vector times a normalized Blinn-Phong lobe, the microfacet form F*D*N.L/4
// with the visibility term taken as 1/(N.L N.V). The lobe is normalized so it integrates to one
// over the hemisphere (D*N.H): a tight puddle lobe is a small, bright image of the lamp and a broad
// wet-street lobe a dim smear, with the same energy. The old term, pow(N.H, n)*.25, had neither, so
// a puddle showed a lamp no brighter than the pool of light around it. Mirror of WaterLightGlint in
// EnvironmentClimateShaders.h.
inline float WaterFresnel(float cosine) {
    const float m = 1 - std::clamp(cosine, 0.f, 1.f);
    return .0204f + .9796f * m * m * m * m * m;
}
inline float WaterGlintLobe(float nDotH, float exponent) {
    return nDotH > 0 ? (exponent + 2) / 6.2831853f * std::pow(nDotH, exponent) : 0.f;
}
inline float WaterLightGlint(float nDotH, float nDotL, float vDotH, float exponent) {
    if (nDotL <= 0) return 0;
    return WaterFresnel(vDotH) * WaterGlintLobe(nDotH, exponent) * nDotL * .25f;
}
// How tight the lobe is: wet ground keeps the street's spread (28..100, rougher where porous) and
// standing water is near a mirror (up to 900). Both widen to the street's spread where the surface
// is under a pixel and a tight lobe would only flicker (`detail`, CoatingDetailFade).
inline float WaterGlintExponent(float puddle, float porous, float detail) {
    const float wet = (28 + 72 * detail) * (1 - porous * .4f);
    const float pool = 100 + 800 * detail;
    return wet + (pool - wet) * std::clamp(puddle, 0.f, 1.f);
}
inline float CoatingCoverage(float amount, float noise, float softness) {
    float threshold = 1 - (1 + softness) * amount;
    float t = std::clamp((noise - threshold) / softness, 0.f, 1.f);
    return t * t * (3 - 2 * t) * std::clamp(amount * 8, 0.f, 1.f);
}
// DOES THIS PIXEL BELONG TO A PLAYER? Reference for CoatingActor in EnvironmentClimateShaders.h --
// the two must change together. Players get no world coating and no rain impacts.
//
// The floor under a player sits inside a plain column around the pawn, so a radius alone would
// claim the ground. A point is body only if it is inside a body-sized capsule AND does not look
// like the ground (upward-facing below knee height) or like a wall or crate the player is
// touching (a VERTICAL face turned back toward the body's axis). A sloped face turned inward is
// body -- the top of a bent arm, a shoulder strap -- and must not drop to the map.
constexpr float kCoatingBodyRadius = 24;   // a player hull is 16; arms and a held weapon reach further
constexpr float kCoatingBodyBottom = -8;
constexpr float kCoatingBodyTop = 80;
constexpr float kCoatingFloorBand = 20;    // upward-facing below this height is ground, not body
inline bool CoatingActorClaims(Vec3 p, Vec3 geometricNormal, Vec3 origin, float radius) {
    if (!(radius > 0)) return false;
    const float ox = p.x - origin.x, oy = p.y - origin.y, height = p.z - origin.z;
    const float d2 = ox * ox + oy * oy;
    if (d2 > radius * radius || height < kCoatingBodyBottom || height > kCoatingBodyTop) return false;
    const bool upward = geometricNormal.z > .6f;
    if (upward && height < kCoatingFloorBand) return false;
    if (!upward && std::abs(geometricNormal.z) < .35f && d2 > 1e-4f) {
        const float inv = 1 / std::sqrt(d2);
        if (geometricNormal.x * ox * inv + geometricNormal.y * oy * inv < -.15f) return false;
    }
    return true;
}
} // namespace Filmmaker::Environment
