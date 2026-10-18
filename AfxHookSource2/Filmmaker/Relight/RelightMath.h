#pragma once
// Engine-free maths for the experimental relighting engine: turning a map's authored
// lights into the native clustered-light records CS2's own forward material shaders
// read, choosing which of them can afford a shadow map, and describing the ambient
// the shader applies where no light reaches.
//
// This is deliberately separate from the Environment feature. Environment adds weather,
// coatings and a handful of user-placed studio lights on top of a map's baked lighting;
// this replaces that baked lighting outright with the map's own authored light set.
#include "../Environment/EnvironmentMath.h"
#include "../Environment/EnvironmentSunVisibilityMath.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Filmmaker::Relight {
using Filmmaker::Environment::Vec3;

// One light as the map authored it, decoded from the map's entity lumps
// (Filmmaker/MapData/MapEntities). These are authored values, not native
// GPU layout -- LightRecord does that conversion.
struct Light {
    std::string identity, kind;
    Vec3 position{}, angles{}, color{1, 1, 1};
    float lumens = 0, brightnessScale = 1, range = 512;
    // Emitter size in Hammer units. A light modelled as a mathematical point diverges as
    // 1/d^2 against the surface it is mounted on. A finite radius bounds that near field
    // without changing the far field. When authored size_params are present, the larger
    // planar half-extent becomes the radius; otherwise this fixture-scale default remains.
    float sourceRadius = 16;
    // Authored planar extents from size_params[0], size_params[1]. Zero means undecoded.
    float sizeX = 0, sizeY = 0;
    float sizeZ = 0, innerAngle = 0, outerAngle = 180;
    float softX = .25f, softY = .25f, skirtNear = .05f, falloff = 1, roundness = 0;
    int shape = -1; // Authored omni shape; 0 is its spherical source.
    // Authored castshadows is a tri-state: 0 none, 1 sunlight only, 2 full.
    int castShadows = 2;
    bool enabled = true;
    bool transient = false; // VFX emitters need current shadows, never cached map tiles.
    bool sceneObject = false; // Editable emitters use moving shadows and their own intensity.
    bool runtimeShadows = true; // Explicit runtime choice; authored castShadows is a bake flag.
    int transientGroup = -1; // Fire patches from one entity share the moving-light budget.
    bool viewmodelEmitter = false; // Emitted by the current first-person attachment.
    float minimumRoughness = .04f;
};

inline constexpr unsigned ShadowFaces = 6;

// Atlas geometry, shared with the shadow pass so the region a face is rendered into and
// the rect the shader samples from cannot drift apart.
//
// The sun and a local cube face need very different resolutions. A cube face covers a
// light's own range, so a thousand pixels across it is roughly half a unit per texel. The
// sun covers the entire map in one projection: on Mirage that is about four units per
// texel at the same size, which is what made its shadows look blocky. It gets a square
// four times wider instead, and the local faces sit beside it rather than sharing a grid.
inline constexpr unsigned SunSize = 4096;
// Four 2048-square projections share the existing sun footprint. Close/detail
// tiers spend texels on nearby receivers; the full-map tier remains the fallback.
inline constexpr unsigned SunCascadeSize = SunSize / 2;
inline constexpr unsigned LocalSize = 1024;
inline constexpr unsigned LocalColumns = 6, LocalRows = 4;
inline constexpr unsigned LocalOriginX = SunSize;
inline constexpr unsigned ShadowBudgetLimit = 4;
// Index of the sun's matrix, past the budgeted cube faces, so adding or dropping a local
// light never moves it.
inline constexpr unsigned SunTile = ShadowBudgetLimit * ShadowFaces;

// The static tier, and the reason it exists: a light with no shadow map is culled only by
// its own influence volume, which is a box of half-extent range. A box does not know about
// walls, so a fixture in one room lights the room next door through the wall between them.
// With a budget of four shadowed lights and a map carrying seventy, sixty-six of them leak
// by construction, which is what a light visible through a wall actually is.
//
// Giving every light a shadow map at the budgeted resolution is impossible -- seventy
// lights times six faces at 1024 squared is about 1.8 GB. But the faces do not need that
// resolution and they do not need re-rendering: a map light never moves and map geometry
// never changes, so each face is drawn once from static world geometry and then lives in
// the atlas for the rest of the session. At 128 squared the whole tier is 100 MB and the
// steady-state cost per frame is zero. What it gives up is moving casters: a player does
// not cast from these lights, only from the budgeted ones. Stopping the light at the wall
// is worth far more than a player's shadow from a distant fixture.
inline constexpr unsigned StaticSize = 128;
inline constexpr unsigned StaticOriginX = LocalOriginX + LocalColumns * LocalSize;
inline constexpr unsigned StaticColumns = 48, StaticRows = 32;
// First static tile, one past the sun, so neither tier can move the other.
inline constexpr unsigned StaticTile = SunTile + 1;
inline constexpr unsigned StaticGridTiles = StaticColumns * StaticRows;
// The grid's last tiles are the stained glass beams' shadow maps (one per window, RelightStainedGlass):
// a beam lights only what it reaches from the glass, never through the wall behind.
inline constexpr unsigned GlassBeamTiles = 64;
inline constexpr unsigned StaticShadowLimit = (StaticGridTiles - GlassBeamTiles) / ShadowFaces;
inline constexpr unsigned GlassBeamTile = StaticTile + StaticGridTiles - GlassBeamTiles;

inline constexpr unsigned AtlasWidth = StaticOriginX + StaticColumns * StaticSize;
inline constexpr unsigned AtlasHeight = SunSize;
// Total addressable tiles: the budgeted cube faces, the sun, then the static tier.
inline constexpr unsigned SunMiddleTile = StaticTile + StaticGridTiles;
inline constexpr unsigned SunDetailTile = SunMiddleTile + 1;
inline constexpr unsigned SunCloseTile = SunMiddleTile + 2;
inline constexpr unsigned ShadowTiles = SunMiddleTile + 3;
static_assert(LocalColumns * LocalRows == ShadowBudgetLimit * ShadowFaces, "budget grid");
static_assert(StaticRows * StaticSize == AtlasHeight, "static grid height");
static_assert(AtlasWidth <= 16384 && AtlasHeight <= 16384, "D3D11 texture limit");

// Every authored light of one map, plus the live edits made to them. Transient: nothing
// here is serialised, so a map change or a reload starts from the authored values again.
struct Scene {
    bool enabled = false;
    uint64_t generation = 0;
    std::string map, source;
    std::vector<Light> lights;

    // Global dials.
    // This scales visibility-tested skylight when GI is enabled. Starting at zero
    // silently disabled authored sky colour and all of its reflected light.
    float intensity = 1, indirect = .3f, sun = 1;
    float ao = 1; // Owned depth-derived ambient visibility, independent of direct shadows.
    bool shadows = true;
    // Diagnostics: take the sun's shadow map out, or every local light, without touching the rest.
    bool sunShadows = true, localLights = true;
    // Diagnostic: 0 every program, 1 only programs that read the lightmap, 2 only the rest.
    unsigned programFilter = 0;
    unsigned bindingCheck = 0; // 1: white coverage; 2: black isolates post-material color.
    // Optional diagnostic ceiling in linear radiance. Zero preserves native HDR.
    // The composite performs the encoding required by the native post shader.
    float ceiling = 0;
    // Angular diameter of the sun in degrees. The owned sun-visibility pass sizes the sun shadow
    // penumbra from it (width = distance to the caster * 2 * tan(size / 2)); see
    // Environment::SunPenumbraWidth. 0 is a point sun with hard shadows.
    float sunAngle = Environment::SunRealDiameterDegrees;
    float lampAngle = 1;
    float shadowSoftness = 1;
    float shadowBias = .0002f;
    // World units a player body (csgo_character draws) is moved toward the sun when it reads the sun
    // cascades, so a body never shadows itself. The cascades hold actors drawn with the previous
    // frame's pose, and the native receiver bias is capped at a fraction of a texel, so self-shadow
    // on a body crawled and flickered whenever it moved or breathed (reports 2026-10-13). About a
    // body's thickness along the sun: walls, roofs and props further away still shade players.
    // 0 restores full self-shadowing.
    float characterSelfShadowUnits = 40.f;
    // Diagnostic: -1 normal selection; 0..3 shades only through that native sun cascade
    // (close, detail, middle, whole map), leaving everything outside it unshadowed.
    int sunCascade = -1;
    // Runtime sky visibility applied to the ambient (0 off, 1 full). Generated from map geometry.
    float skyOcclusion = 1;
    // How many lights the cached shadow tier may hold. Every light it covers is one that
    // stops at walls instead of shining through them, and each costs six more records for
    // every pixel to test, so this is the direct handle on that trade. Zero reproduces the
    // old behaviour: four shadowed lights and the rest leaking.
    unsigned staticShadows = StaticShadowLimit;
    int view = 0;
    // Cost attribution: full image, first mask word, or one record. Transient diagnostics.
    unsigned cost = 0;
    bool hoist = true;
    bool reference = false; // Diagnostic: replay untouched native inputs through our composite.
    unsigned probeCheck = 0; // 1 uniform fallback, 2 identical uniform probe
    // Runtime bounce (camera-following probe volume). On by default: without it every surface the
    // sun and fixtures do not reach directly receives only sky light, and interiors go black.
    bool gi = true;
    uint64_t probeRebuild = 0;
    float bounce = 1;
    unsigned bounceSteps = 1; // Probe transport updates per frame (1..3).
    bool screenGi = false; // native per-pixel world queries; half-resolution cache is opt-in
    // Live reflections captured from the bounce volume in place of the map's baked reflection
    // cubes. Always on; the strength exists only for the mirv_relight diagnostic.
    float reflections = 1;
    // Sunlight through stained glass as coloured beams. Always on; diagnostic strength only.
    float stainedGlass = 1;
    Vec3 albedo{.55f,.55f,.55f};
    // Writes a UV-coded checker into the owned lightmap irradiance (t2). Walls and floors
    // that sample our texture show the pattern; a 1x1 fill cannot. Diagnostic only - not GI.
    bool lightmapDiag = false;
    // The map's lightmap layout (MapDataTool lightmap output) and what to draw into the atlas it
    // describes: 0 flat fill, 1 world-space checker, 2 lit from the world's own light field.
    std::string lightmapFile;
    unsigned lightmapMode = 0;
    unsigned clusters = 3; // 0 full mask, 1 native tiles+slices, 2 native slices, 3 owned tiles+slices
    // Visible reach (RelightClusterMath.h): a light stops at the distance where it can no longer
    // change a dark pixel by this many display steps. 0 keeps every authored range. At 4 the
    // lit world matched the authored ranges to capture noise (de_ancient, 2026-10-18) while
    // frames were about 10% faster; up to 8 still matched there.
    float reach = 4;
    // Per frame, from `reach` and the exposure actually applied (0 while that exposure is unknown):
    // the radiance cutoff the light clusters use.
    float visibleRadiance = 0;

    // The map's own light_environment. Its angles and brightness seed the climate sun on
    // load, so the sun is edited with the ordinary sun controls rather than a second set.
    bool hasSun = false;
    Vec3 sunAngles{}, sunColor{1, 1, 1};
    float sunBrightness = 0;
    Vec3 skyColor{.5f, .7f, 1};
    float skyIntensity = 1;
};

// A native clustered-light record: 30 float4 rows, 480 bytes, matching the stride the
// shipped forward pixel shaders declare. Offsets below are byte offsets into this record
// as read by that bytecode; Docs/effects/whole-map-relighting.md carries the decode.
using LightRecord = std::array<float, 120>;


// The clustered loop reads this many 32-bit mask words per tile and per depth slice, so
// capacity is 32 * words. cb3[19].z carries it.
inline unsigned MaskWords(size_t records) {
    return records ? unsigned((records + 31) / 32) : 1u;
}

inline float Linear(float srgb) {
    return srgb <= .04045f ? srgb / 12.92f : std::pow((srgb + .055f) / 1.055f, 2.4f);
}

inline float SourceRadiusSquared(const Light &light) {
    const float radius = std::clamp(light.sourceRadius, 1.f, 256.f);
    return radius * radius;
}

// Prefer the authored planar half-extent when size_params survived the VRF export.
inline float AuthoredSourceRadius(float sizeX, float sizeY, float fallback = 16.f) {
    const float half = .5f * (std::max)(sizeX, sizeY);
    return half >= 1.f ? std::clamp(half, 1.f, 256.f) : fallback;
}
inline float AuthoredShadowNearClip(const Light& light) {
    // A finite spherical source emits from outside its own luminous bulb. A
    // center projection starting inside that shell falsely blocks every ray.
    // Other shapes retain the conservative point projection until represented
    // by their actual surface samples; planar sizeY is not a sphere radius.
    if(light.kind=="light_omni2"&&light.shape==0&&std::isfinite(light.sizeX)&&light.sizeX>0)
        return std::clamp(light.sizeX,1.f,(std::max)(1.f,light.range*.25f));
    return 1.f;
}

// Source entity angles â†’ light-local basis. Local +X is forward (matches VRF barn
// entityForward = rotate(+X, orientationQ)). Pitch positive pitches the aim down.
inline void LightBasis(Vec3 angles, Vec3 &forward, Vec3 &right, Vec3 &up) {
    Environment::CameraBasis(angles, forward, right, up);
}

// Identity quaternion is (0,0,0,1). Built so rotate(+X, q) â‰ˆ forward.
inline void LightOrientationQuat(Vec3 forward, Vec3 right, Vec3 up, float *quatXyZw) {
    // Encode the light-to-world rotation used by the native +X/+Y queries.
    // Source +X aims forward, +Y is left, +Z is up: a proper rotation,
    // rather than the previous reflected/permuted basis.
    const float m00 = forward.x, m01 = -right.x, m02 = up.x;
    const float m10 = forward.y, m11 = -right.y, m12 = up.y;
    const float m20 = forward.z, m21 = -right.z, m22 = up.z;
    const float t = m00 + m11 + m22;
    if (t > 0.f) {
        const float s = std::sqrt(t + 1.f) * 2.f;
        quatXyZw[3] = .25f * s;
        quatXyZw[0] = (m21 - m12) / s;
        quatXyZw[1] = (m02 - m20) / s;
        quatXyZw[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
        quatXyZw[0] = .25f * s;
        quatXyZw[1] = (m01 + m10) / s;
        quatXyZw[2] = (m02 + m20) / s;
        quatXyZw[3] = (m21 - m12) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
        quatXyZw[0] = (m01 + m10) / s;
        quatXyZw[1] = .25f * s;
        quatXyZw[2] = (m12 + m21) / s;
        quatXyZw[3] = (m02 - m20) / s;
    } else {
        const float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
        quatXyZw[0] = (m02 + m20) / s;
        quatXyZw[1] = (m12 + m21) / s;
        quatXyZw[2] = .25f * s;
        quatXyZw[3] = (m10 - m01) / s;
    }
}

inline bool IsShapedFixture(const Light &light) {
    return light.kind == "light_rect" || light.kind == "light_barn";
}
inline bool HasFixtureProjection(const Light& light) {
    return IsShapedFixture(light)&&light.sizeX>0&&light.sizeY>0;
}
// Baked fill boundaries acquire a penumbra during compilation. Runtime fills
// need a finite analytic feather even when the authored edge softness is zero.
inline constexpr float RuntimeFillFeather=.15f;
inline float FixtureFeather(float authored) {return std::clamp(authored,RuntimeFillFeather,1.f);}
inline unsigned FixtureFaces(const Light& light) { return HasFixtureProjection(light)?1u:ShadowFaces; }
// Panel lights sit at or just above the ceiling they are recessed into (Office's rect lights are
// two units above the tile surface). Depth that starts exactly at the emitter made that ceiling
// the nearest caster in every direction, so the light lit nothing. Depth starts this far in
// front of the emitter instead.
inline constexpr float FixtureShadowClearance = 4;
// Distance from the projection eye at which fixture shadow depth is zero.
inline float FixtureDepthStart(const Light& light) {
    return (light.sizeZ>0?1/light.sizeZ:0)+FixtureShadowClearance;
}
inline Environment::Matrix FixtureProjection(const Light& light) {
    Vec3 f,r,u;LightBasis(light.angles,f,r,u);
    const float hx=(std::max)(.01f,light.sizeX),hy=(std::max)(.01f,light.sizeY);
    const float range=(std::max)(1.f,light.range);
    Environment::Matrix m{};
    const float nearPlane=light.sizeZ>0?1/light.sizeZ:0;
    const Vec3 eye=light.position-f*nearPlane;
    const float sx=nearPlane>0?nearPlane/hx:1/hx,sy=nearPlane>0?nearPlane/hy:1/hy;
    m[0]=-r.x*sx;m[1]=-r.y*sx;m[2]=-r.z*sx;m[3]=Environment::Dot(r,eye)*sx;
    m[4]=u.x*sy;m[5]=u.y*sy;m[6]=u.z*sy;m[7]=-Environment::Dot(u,eye)*sy;
    // depth*w = a*w - b: zero at the depth start, one at the authored range.
    const float start=FixtureDepthStart(light),farPlane=nearPlane+range;
    if(nearPlane>0){
        const float b=1/(1/start-1/farPlane),a=b/start;
        m[8]=f.x*a;m[9]=f.y*a;m[10]=f.z*a;m[11]=-Environment::Dot(f,eye)*a-b;
        m[12]=f.x;m[13]=f.y;m[14]=f.z;m[15]=-Environment::Dot(f,eye);
    } else {
        const float sz=1/(farPlane-start);
        m[8]=f.x*sz;m[9]=f.y*sz;m[10]=f.z*sz;m[11]=-Environment::Dot(f,eye)*sz-start*sz;m[15]=1;
    }
    return m;
}
inline void ConeFade(float inner,float outer,float& bias,float& scale) {
    if(outer>=180){bias=1;scale=0;return;}
    if(outer<=0){bias=scale=0;return;}
    constexpr float deg=3.14159265358979323846f/180;
    const float co=std::cos(outer*deg),ci=std::cos(std::clamp(inner,0.f,(std::max)(0.f,outer-.001f))*deg);
    // Native cosine points from receiver toward emitter, opposite authored aim.
    scale=-1/(std::max)(1e-7f,ci-co);bias=co*scale;
}

// Radiance is divided by the squared source radius because the shader attenuates by
// radiusSquared / max(radiusSquared, distanceSquared): past the emitter the two cancel, so
// the far field matches a point light exactly, while the value at the emitter is bounded
// by this result instead of diverging.
//
// The x10 flux conversion is inherited from the single-fixture proof and is provisional:
// authored brightness also arrives as a separate log-scale value with per-kind units, so
// this is not established parity and the global multiplier is the tuning handle.
inline float Power(const Light &light, float intensity) {
    if (!light.enabled) return 0.f;
    return light.lumens * light.brightnessScale * (light.sceneObject ? 1.f : intensity) * 10.f / SourceRadiusSquared(light);
}

// Local lights. Omni stays an axis-aligned range box (full sphere). Rect/barn write an
// oriented clip frustum from authored angles and size_params so the shader's NDC cull
// matches the fixture plate instead of lighting every direction inside range.
inline LightRecord ToRecord(const Light &light, float intensity) {
    LightRecord record{};
    const float range = (std::max)(1.f, light.range);
    // Cube partitions are independent of the emission cone. Shaped projectors
    // replace these rows below with their authored expanding frustum.
    record[0] = record[5] = 1 / range;
    record[10] = .5f / range;
    record[15] = 1;
    record[3] = -light.position.x / range;
    record[7] = -light.position.y / range;
    record[11] = .5f - .5f * light.position.z / range;

    // Row 8 (byte 128): world position, w = squared source radius. A non-zero w also marks
    // the row for camera-relative rebasing on the GPU.
    record[32] = light.position.x;
    record[33] = light.position.y;
    record[34] = light.position.z;
    record[35] = SourceRadiusSquared(light);

    // Row 9 (byte 144): saturate(y*distance + x) feeds a smoothstep, so the light reaches
    // zero exactly at the authored range. z and w are near/far fade thresholds the shader
    // only applies when they are positive.
    record[36] = 1;
    record[37] = -1 / range;

    // Row 10 (byte 160): linear radiance. The fourth float selects how the shader shapes
    // the light: zero takes the analytic edge-fade branch, which samples no cookie
    // texture. Non-zero values route into the projected cookie and IES paths, which need
    // resources we do not own yet.
    const float power = Power(light, intensity);
    record[40] = Linear(light.color.x) * power;
    record[41] = Linear(light.color.y) * power;
    record[42] = Linear(light.color.z) * power;
    record[43] = 0;

    // Row 14 (byte 224): edge fade endpoints for the analytic shaping branch. Endpoints of
    // exactly one would divide by zero, so stay just inside.
    record[56] = record[57] = .999999f;
    // Row 15 (byte 240) stays zero: the shader scales a light down by how much of it is
    // already baked, and nothing is baked once the lightmap inputs are neutralised.
    record[64] = std::isfinite(light.minimumRoughness)?std::clamp(light.minimumRoughness,.04f,1.f):.04f;
    record[65] = 0;     // no atlas tile yet; row 13 scale stays zero, so no shadow
    Vec3 f,r,u;LightBasis(light.angles,f,r,u);
    LightOrientationQuat(f,r,u,&record[44]);
    // Omni entities retain cone controls in their exported property bag too.
    // Applying those as a spotlight removed the upper hemisphere of globe
    // lamps, including illumination and moving fan shadows on the ceiling.
    if(light.kind=="light_spot")ConeFade(light.innerAngle,light.outerAngle,record[48],record[49]);
    else {record[48]=1;record[49]=0;}
    if(light.kind=="light_omni2"&&(light.shape==1||light.shape==2))record[50]=light.sizeY;
    if(HasFixtureProjection(light)){
        const auto matrix=FixtureProjection(light);std::copy(matrix.begin(),matrix.end(),record.begin());
        const float nearPlane=light.sizeZ>0?1/light.sizeZ:0,farPlane=nearPlane+range;
        const auto eye=light.position-f*nearPlane;
        record[32]=nearPlane>0?eye.x:-f.x;record[33]=nearPlane>0?eye.y:-f.y;record[34]=nearPlane>0?eye.z:-f.z;
        record[35]=nearPlane*nearPlane;record[36]=1;record[37]=0;
        // Near skirt and far falloff are smoothsteps of the projected depth (ndc.z*record[38] and
        // (1-ndc.z)*record[39]), so they are expressed through the same depth mapping as the
        // matrix: zero at FixtureDepthStart, one at the authored range.
        const float start=FixtureDepthStart(light);
        const auto depth=[&](float w){return nearPlane>0?(1/start-1/w)/(1/start-1/farPlane):(w-start)/(farPlane-start);};
        const float skirtDepth=depth(nearPlane+light.skirtNear*range),falloffDepth=depth(nearPlane+(1-light.falloff)*range);
        record[38]=light.skirtNear>0&&skirtDepth>0?1/skirtDepth:0;
        record[39]=light.falloff>0&&falloffDepth<1?1/(1-falloffDepth):0;
        record[48]=1;record[49]=record[50]=0;
        record[56]=1-FixtureFeather(light.softX);record[57]=1-FixtureFeather(light.softY);
        record[58]=std::clamp(light.roundness,0.f,1.f);
        // Flux over the authored aperture, with the existing x10 conversion.
        const float aperture=4*light.sizeX*light.sizeY;
        const float solid=nearPlane>0?4*std::atan(light.sizeX*light.sizeY/(nearPlane*std::sqrt(nearPlane*nearPlane+light.sizeX*light.sizeX+light.sizeY*light.sizeY))):0;
        const float divisor=nearPlane>0?solid*nearPlane*nearPlane/(4*3.14159265358979323846f*10):aperture/(4*3.14159265358979323846f*10);
        const float radiance=light.enabled?light.lumens*light.brightnessScale*intensity/(std::max)(1e-6f,divisor):0;
        record[40]=Linear(light.color.x)*radiance;record[41]=Linear(light.color.y)*radiance;record[42]=Linear(light.color.z)*radiance;
        record[80]=1;record[81]=range; // owned projection metadata for cluster bounds
    }
    return record;
}

// Elevation is the authored pitch and azimuth the authored yaw. The sun belongs to
// this feature rather than to the Environment climate, so that editing it here cannot
// be undone by a weather preset and vice versa.
inline Vec3 SunDirection(Vec3 angles) {
    const float e = angles.x * 3.14159265358979323846f / 180;
    const float a = angles.y * 3.14159265358979323846f / 180;
    return {std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), std::sin(e)};
}

// Sunlight stops at the horizon: below it the sky is still lit but no disk remains, and a
// sun that kept shining from underneath would light every ceiling in the map.
inline Vec3 SunRadiance(const Scene &scene) {
    const float horizon = std::clamp(scene.sunAngles.x, 0.f, 1.f);
    const float visibility = horizon * horizon * (3 - 2 * horizon);
    const float power = (std::max)(0.f, scene.sunBrightness) * (std::max)(0.f, scene.sun) * visibility;
    return Vec3{Linear(scene.sunColor.x) * power, Linear(scene.sunColor.y) * power,
                Linear(scene.sunColor.z) * power};
}

inline bool HasRadiance(Vec3 radiance) {
    return radiance.x>0||radiance.y>0||radiance.z>0;
}
inline bool HasLocalRadiance(const Scene& scene) {
    if(!scene.localLights)return false;
    return std::any_of(scene.lights.begin(),scene.lights.end(),[&](const Light& light){
        return light.enabled&&Power(light,scene.intensity)>0&&HasRadiance(light.color);
    });
}

// Nearby emitting fixtures get the high-resolution tier. The authored castshadows
// mode belongs to the map compiler: every emitter needs occlusion in our runtime
// replacement, including fixtures whose original shadows were entirely baked.
// Distance to the emitter prioritizes room fixtures over remote large-range lights.
inline std::vector<unsigned> ShadowBudget(const std::vector<Light> &lights, Vec3 camera,
                                          unsigned budget) {
    std::vector<unsigned> chosen;
    if (!budget) return chosen;
    std::vector<std::pair<float, unsigned>> ranked;
    for (unsigned i = 0; i < lights.size(); ++i) {
        const auto &light = lights[i];
        if (!light.enabled || !light.runtimeShadows || Power(light,1)<=0 || !HasRadiance(light.color)) continue;
        const Vec3 delta{light.position.x - camera.x, light.position.y - camera.y,
                         light.position.z - camera.z};
        const float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        ranked.push_back({light.transient || light.sceneObject ? distance - 100000.f : distance, i});
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    // A placed light is the user's deliberate key light: gunfire must never make
    // its shadow flicker. Then every active fire gets a representative before
    // its extra patches; ungrouped flashes precede sustained patches.
    std::vector<std::pair<unsigned,unsigned>> order;
    std::vector<int> groups;
    for(unsigned n=0;n<ranked.size();++n){const auto& light=lights[ranked[n].second];
        unsigned tier=light.sceneObject ? 0u : unsigned(lights.size())+2;
        if(light.transient){tier=1;if(light.transientGroup>=0){
            tier=2+unsigned(std::count(groups.begin(),groups.end(),light.transientGroup));
            groups.push_back(light.transientGroup);}}
        order.push_back({tier,n});
    }
    std::stable_sort(order.begin(),order.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    for(const auto& entry:order){if(chosen.size()>=budget)break;chosen.push_back(ranked[entry.second].second);}
    return chosen;
}
// Keep resident atlas slots when distances cross. Promote at most one fixture
// per frame, only when it is substantially closer than the current occupant.
inline std::vector<unsigned> StableShadowBudget(const std::vector<Light>& lights,Vec3 camera,
                                                unsigned budget,const std::vector<unsigned>& previous) {
    const auto ranked=ShadowBudget(lights,camera,unsigned(lights.size()));
    std::vector<unsigned> chosen;
    // Re-evaluate transient priority each frame; residency belongs to static fixtures.
    for(auto index:ranked)if((lights[index].transient||lights[index].sceneObject)&&chosen.size()<budget)chosen.push_back(index);
    for(auto index:previous)if(chosen.size()<budget&&index<lights.size()&&!lights[index].transient&&!lights[index].sceneObject&&
        std::find(ranked.begin(),ranked.end(),index)!=ranked.end()&&
        std::find(chosen.begin(),chosen.end(),index)==chosen.end())chosen.push_back(index);
    for(auto index:ranked)if(chosen.size()<budget&&std::find(chosen.begin(),chosen.end(),index)==chosen.end())chosen.push_back(index);
    if(chosen.empty())return chosen;
    const auto distance=[&](unsigned i){const auto d=lights[i].position-camera;return Environment::Dot(d,d);};
    auto farthest=std::max_element(chosen.begin(),chosen.end(),[&](unsigned a,unsigned b){
        return (lights[a].transient||lights[a].sceneObject?-1.f:distance(a))<(lights[b].transient||lights[b].sceneObject?-1.f:distance(b));});
    for(auto index:ranked)if(std::find(chosen.begin(),chosen.end(),index)==chosen.end()){
        if(!lights[*farthest].transient&&!lights[*farthest].sceneObject&&distance(index)<distance(*farthest)*.64f)*farthest=index;
        break;
    }
    return chosen;
}

// Entity angles describe the direction the rays travel. The native lighting ABI
// wants the direction toward the sun. Its positive authored pitch points downward.
inline Vec3 AuthoredSunAngles(Vec3 angles) {
    return {angles.x, std::fmod(angles.y + 540.f, 360.f), angles.z};
}

// Which lights get a cached static shadow map, given the budgeted set that already has a
// full one. Ranked by authored output rather than by distance: the ranking has to be
// camera-independent, because a set that reshuffles as the camera moves would re-render
// faces that are meant to be drawn once and kept. Brightest first, so if a map carries
// more lights than the tier holds, the ones that lose out are the ones least likely to be
// noticed leaking.
//
// The authored castshadows flag is deliberately not consulted here. It describes what
// Valve's *baked* pipeline did with the light; in a real-time replacement a light with no
// shadow map does not become cheap, it becomes a light that shines through walls.
inline std::vector<unsigned> StaticShadowSet(const std::vector<Light> &lights,
                                             const std::vector<unsigned> &budgeted,
                                             unsigned limit) {
    std::vector<unsigned> chosen;
    if (!limit) return chosen;
    std::vector<std::pair<float, unsigned>> ranked;
    for (unsigned i = 0; i < lights.size(); ++i) {
        // Reserve stable residency even while a fixture is disabled or promoted to
        // the nearby tier. Camera motion and intensity edits cannot reshuffle tiles.
        if (lights[i].lumens <= 0 || lights[i].transient || lights[i].sceneObject || !lights[i].runtimeShadows) continue;
        ranked.push_back({-lights[i].lumens, i});
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    if (ranked.size() > limit) ranked.resize(limit);
    for (const auto &entry : ranked) chosen.push_back(entry.second);
    // Back into authored order so the tile a light occupies depends only on which lights
    // are in the set, never on the order they were ranked in.
    std::sort(chosen.begin(), chosen.end());
    return chosen;
}

// Where a square region of the atlas lands, in the form the record carries at byte 208:
// uv = clip.xy * scale + offset. The V scale is negative because clip space runs up and
// texture space runs down.
inline void AtlasRectPixels(unsigned x, unsigned y, unsigned size, float *rect) {
    rect[0] = (x + size * .5f) / AtlasWidth;
    rect[1] = (y + size * .5f) / AtlasHeight;
    rect[2] = size * .5f / AtlasWidth;
    rect[3] = -(size * .5f) / AtlasHeight;
}
// Where a tile lives and how big it is. The sun's square sits at the origin, the budgeted
// cube faces in a grid beside it, and the static tier in a finer grid beyond that.
inline void TileOrigin(unsigned tile, unsigned &x, unsigned &y, unsigned &size) {
    if (tile < SunTile) {
        size = LocalSize;
        x = LocalOriginX + (tile % LocalColumns) * LocalSize;
        y = (tile / LocalColumns) * LocalSize;
    } else if (tile == SunTile) {
        size = SunCascadeSize;
        x = y = 0;
    } else if (tile >= SunMiddleTile && tile <= SunCloseTile) {
        const unsigned quadrant=tile-SunMiddleTile+1;
        size=SunCascadeSize;x=(quadrant&1)*size;y=(quadrant>>1)*size;
    } else {
        const unsigned slot = tile - StaticTile;
        size = StaticSize;
        x = StaticOriginX + (slot % StaticColumns) * StaticSize;
        y = (slot / StaticColumns) * StaticSize;
    }
}
// Static-tier faces are 128 texels. The shadow filter spans a few texels, so at a face edge it
// read the neighbouring tile (another face or light): thin lit/unlit lines outlining each cube
// face on walls, visible whenever a light used this tier (from a distance). Each static face is
// rendered with a guard band (a projection StaticFaceGuard wider than 90 degrees) while its record
// keeps the exact 90-degree frustum and samples only the inner part of the tile.
// The same holds for every cube-face tier (the 1024 budgeted faces too: their edge lines showed up
// as a faint seam once the static tier was fixed), so the guard is derived from the tile's size.
inline constexpr float FaceGuardTexels = 12;
inline constexpr float LocalFaceGuardTexels = 96;
inline constexpr float FaceGuardFor(unsigned size) { return float(size) / (float(size) - 2 * (size==LocalSize?LocalFaceGuardTexels:FaceGuardTexels)); }
inline constexpr float StaticFaceGuardTexels = FaceGuardTexels;
inline constexpr float StaticFaceGuard = FaceGuardFor(StaticSize);
inline constexpr float LocalFaceGuard = FaceGuardFor(LocalSize);
inline bool IsStaticFaceTile(unsigned tile) { return tile >= StaticTile && tile < StaticTile + StaticShadowLimit * ShadowFaces; }
inline bool IsLocalFaceTile(unsigned tile) { return tile < SunTile; }
inline bool IsCubeFaceTile(unsigned tile) { return IsLocalFaceTile(tile) || IsStaticFaceTile(tile); }
// Guard factor of a tile: 1 for the sun cascades, which are not cube faces.
inline float TileGuard(unsigned tile) {
    if (IsLocalFaceTile(tile)) return LocalFaceGuard;
    if (IsStaticFaceTile(tile)) return StaticFaceGuard;
    return 1.f;
}
// The rendered projection: clip x and y of the exact face matrix divided by the guard factor.
inline void GuardFace(float *matrix, float factor) {
    for (unsigned i = 0; i < 4; ++i) { matrix[i] /= factor; matrix[4 + i] /= factor; }
}
inline void GuardStaticFace(float *matrix, float factor = StaticFaceGuard) { GuardFace(matrix, factor); }
inline void TileRect(unsigned tile, float *rect) {
    unsigned x = 0, y = 0, size = 0;
    TileOrigin(tile, x, y, size);
    AtlasRectPixels(x, y, size, rect);
    const float guard = TileGuard(tile);
    rect[2] /= guard; rect[3] /= guard;
}
inline void SunRect(float *rect) { TileRect(SunTile, rect); }
// A lamp is not a point: its bulb or diffuser has a size. Seen from that source, a caster thinner
// than the source (a pull cord, a chain, a wire) blocks only part of it and casts no umbra a
// little way off, while a point light drew it as a sharp dark band metres long across the far
// wall. The cached lamp faces are therefore "opened" (eroded, then dilated back) by the source's
// angular radius: features thinner than the source vanish, larger casters keep their exact
// silhouette, so lampshades, furniture and contact shadows are unchanged. LampSourceRadius is
// conservative (a small bulb), so a chair leg or railing still casts.
inline constexpr float LampSourceRadius = .5f;
inline constexpr float LampOpeningMaxTexels = 48;
// The opening uses each lamp's own authored source radius, kept between a small bulb and a globe
// (a ceiling-fan light kit): a rod or cord a few units from the source then casts no umbra.
inline float LampOpeningRadius(float sourceRadius) {
    return std::isfinite(sourceRadius) ? std::clamp(sourceRadius, LampSourceRadius, 3.f) : LampSourceRadius;
}
// Linear distance of a stored perspective depth (LocalShadowMatrix's z mapping).
inline float ShadowLinearDepth(float depth, float nearClip, float farClip) {
    return nearClip * farClip / (farClip - depth * (farClip - nearClip));
}
// Opening radius in texels for a caster `distance` from the lamp, on a face `size` texels wide
// spanning `guard` times 90 degrees.
inline float LampOpeningTexels(float distance, float size, float guard, float radius = LampSourceRadius) {
    if (!(distance > 0)) return LampOpeningMaxTexels;
    return (std::min)(LampOpeningMaxTexels, radius * size / (2 * guard * distance));
}

// Overlap adjacent cube faces and normalize their contribution across the seam.
// A hard 90-degree ownership switch cuts a wide soft-shadow footprint into boxes.
inline constexpr float ShadowFaceBlendStart = .9f;
inline constexpr float ShadowFaceBlendWidth = 1 / ShadowFaceBlendStart;
inline Vec3 ShadowFaceWeights(Vec3 direction) {
    const Vec3 a{std::abs(direction.x),std::abs(direction.y),std::abs(direction.z)};
    const float maximum=(std::max)({a.x,a.y,a.z,1e-8f});
    const auto weight=[&](float component){float t=std::clamp((component/maximum-ShadowFaceBlendStart)/(1-ShadowFaceBlendStart),0.f,1.f);return t*t*(3-2*t);};
    const Vec3 w{weight(a.x),weight(a.y),weight(a.z)};
    const float sum=w.x+w.y+w.z;
    return sum>0?w*(1/sum):Vec3{1,0,0};
}

// Cube records overlap their cull wedges; normalized shader weights blend them.
// The compensated atlas scale still addresses the original rendered projection.
inline LightRecord ToShadowedRecord(const Light &light, float intensity, const float *faceMatrix,
                                    unsigned tile) {
    LightRecord record = ToRecord(light, intensity);
    for (unsigned i = 0; i < 16; ++i) record[i] = faceMatrix[i];
    TileRect(tile, &record[52]);
    if(!HasFixtureProjection(light)){
        GuardFace(record.data(),ShadowFaceBlendWidth);
        record[54]*=ShadowFaceBlendWidth;record[55]*=ShadowFaceBlendWidth;
    }
    record[65] = 1;  // shadow strength; the atlas scale above is what enables the lookup
    // Perspective depth has 1/z linear in stored depth. This coefficient
    // converts receiver-minus-blocker depth into the source's angular penumbra.
    const float nearPlane=HasFixtureProjection(light)?(light.sizeZ>0?FixtureDepthStart(light):0):AuthoredShadowNearClip(light);
    const float farPlane=HasFixtureProjection(light)?(light.sizeZ>0?1/light.sizeZ:0)+light.range:light.range;
    const float radius=HasFixtureProjection(light)?light.sourceRadius:
        (light.kind=="light_omni2"&&(light.shape==1||light.shape==2)?(std::max)(light.sourceRadius,light.sizeY):light.sourceRadius);
    if(nearPlane>0&&farPlane>nearPlane){
        record[84]=radius*(farPlane-nearPlane)/(nearPlane*farPlane);
        record[85]=radius/nearPlane;
        record[86]=IsLocalFaceTile(tile)?LocalFaceGuardTexels:FaceGuardTexels;
        if(!HasFixtureProjection(light)){
            // The blend overlap uses part of the existing atlas guard. Leave
            // every filter sample inside this face, including at the overlap edge.
            const float size=IsLocalFaceTile(tile)?float(LocalSize):float(StaticSize);
            record[86]=size*.5f-(size*.5f-record[86])*ShadowFaceBlendWidth;
            record[84]/=ShadowFaceBlendWidth;record[85]/=ShadowFaceBlendWidth;
        }
        record[87]=HasFixtureProjection(light)?-12345.f:-12346.f;
    }
    return record;
}

// The sun as a clustered record. A directional source carries its direction where a local
// light carries position, and a zero w marks it so the GPU rebasing translates local
// lights into camera-relative space without also translating a direction. Attenuation is
// flat: sunlight does not fall off across a map.
inline LightRecord SunRecord(Vec3 direction, Vec3 radiance, const float *shadowMatrix,
                             bool shadowed) {
    LightRecord record{};
    if (shadowMatrix)
        for (unsigned i = 0; i < 16; ++i) record[i] = shadowMatrix[i];
    record[32] = direction.x;
    record[33] = direction.y;
    record[34] = direction.z;
    record[35] = 0;
    record[36] = 1;
    record[37] = 0;
    record[40] = radiance.x;
    record[41] = radiance.y;
    record[42] = radiance.z;
    record[43] = 0;
    record[47] = 1;
    record[48] = 1;
    record[56] = record[57] = .999999f;
    record[64] = .04f;
    if (shadowed && shadowMatrix) {
        SunRect(&record[52]);
        record[65] = 1;
    }
    return record;
}

// The native shader's fallback ambient is three rows evaluated against the surface normal,
// and it multiplies the result by the surface albedo itself. A flat constant there lights
// every face equally. These rows carry a sky-over-ground hemisphere instead:
//
//   ambient(n) = mix(ground, sky, n.z * .5 + .5)
//              = (sky + ground) / 2  +  (sky - ground) / 2 * n.z
//
// so the z coefficient holds half the difference and the constant holds the mean. This
// approximates skylight plus one grey bounce. It has no spatial variation, so it cannot
// put a bright patch where the sun actually lands through a doorway.
inline void AmbientRows(Vec3 sky, Vec3 ground, float *rows) {
    const float *skyChannel = &sky.x, *groundChannel = &ground.x;
    for (unsigned channel = 0; channel < 3; ++channel) {
        const float high = (std::max)(0.f, skyChannel[channel]);
        const float low = (std::max)(0.f, groundChannel[channel]);
        float *row = rows + channel * 4;
        row[0] = row[1] = 0;
        row[2] = (high - low) * .5f;
        row[3] = (high + low) * .5f;
    }
}

inline Vec3 SkyAmbient(const Scene &scene, float indirect) {
    return Vec3{scene.skyColor.x, scene.skyColor.y, scene.skyColor.z} *
           ((std::max)(0.f, scene.skyIntensity) * (std::max)(0.f, indirect));
}
// One texel of the lightmap-UV diagnostic. Neighbouring charts pick different colours when
// the native shader samples t2 with lightmap coordinates; that is the world-surface proof.
inline constexpr unsigned LightmapDiagSize = 64;
inline void LightmapDiagTexel(unsigned x, unsigned y, unsigned size, float *rgba) {
    const float u = (x + .5f) / (std::max)(1u, size);
    const float v = (y + .5f) / (std::max)(1u, size);
    const bool check = ((x / 8) ^ (y / 8)) & 1;
    const float scale = check ? .8f : .25f;
    rgba[0] = u * scale;
    rgba[1] = v * scale;
    rgba[2] = check ? .15f : .45f;
    rgba[3] = 1;
}

inline Vec3 GroundAmbient(Vec3 sunRadiance, float albedo, float indirect) {
    const float scale = (std::max)(0.f, albedo) * (std::max)(0.f, indirect);
    return Vec3{sunRadiance.x * scale, sunRadiance.y * scale, sunRadiance.z * scale};
}

// The nine-tap percentage-closer filter the shipped shader performs, expressed as the
// four constant rows it reads the kernel from. Decoded from the bytecode: it averages four
// corner taps, and where that average is neither fully lit nor fully shadowed it refines
// with four edge taps and a centre tap.
//
//   value = corners.average * (4 * rows[0].w)
//         + sum(edge taps)  * rows[1].x
//         + centre tap      * rows[1].y
//
// The weights below are a 3x3 tent (corners 1/16, edges 2/16, centre 4/16), which sums to
// one so a fully lit or fully shadowed region stays exactly that. rows[2].yz carry the two
// horizontal offsets and rows[3].yz the two vertical ones, in atlas UV.
// Radius of the fixed kernel the native shader's 9-tap filter runs with, in atlas texels. Local
// lights (and any program that has not taken over the sun term, see NativeAtmosphereProgram) are
// filtered by it; the sun's physically sized penumbra comes from the owned visibility pass instead.
inline constexpr float NativeFilterTexels = 1.5f;
// Apparent lamp diameter controls the fixed native filter. It does not alter radiance or
// attenuation. Limit the footprint to the eight-texel guard so adjacent atlas faces stay isolated.
inline float LampFilterTexels(float angle, float softness) {
    if (!std::isfinite(angle) || !std::isfinite(softness)) return 0;
    return std::clamp(float(StaticSize) * std::tan(std::clamp(angle, 0.f, 5.f) * .00872664626f)
                      * std::clamp(softness, 0.f, 4.f) / StaticFaceGuard, 0.f, FaceGuardTexels);
}
inline void ShadowFilterRows(float softness, float *rows) {
    for (unsigned i = 0; i < 16; ++i) rows[i] = 0;
    const float radius = std::clamp(softness, 0.f, 8.f);
    const float texelX = radius / AtlasWidth, texelY = radius / AtlasHeight;
    rows[3] = .0625f;   // rows[0].w: four corners, 1/16 each
    rows[4] = .125f;    // rows[1].x: each edge tap, 2/16
    rows[5] = .25f;     // rows[1].y: centre tap, 4/16
    rows[9] = texelX;   // rows[2].y
    rows[10] = -texelX; // rows[2].z
    rows[13] = texelY;  // rows[3].y
    rows[14] = -texelY; // rows[3].z
}

// Packs the per-tile and per-slice bitmasks the clustered loop ANDs together. Every record
// is live in every cluster: the shader's own per-record frustum and box test culls, which
// is correct but costs a test per light per pixel. Real cluster addressing is the
// performance answer, not a correctness one.
inline std::vector<uint32_t> FullMask(size_t records, size_t clusters) {
    const auto words = MaskWords(records);
    std::vector<uint32_t> mask(size_t(words) * (clusters ? clusters : 1), 0u);
    for (size_t word = 0; word < words; ++word) {
        const size_t base = word * 32;
        uint32_t bits = 0;
        for (unsigned bit = 0; bit < 32; ++bit)
            if (base + bit < records) bits |= 1u << bit;
        for (size_t cluster = 0; cluster < (clusters ? clusters : 1); ++cluster)
            mask[cluster * words + word] = bits;
    }
    return mask;
}

} // namespace Filmmaker::Relight
