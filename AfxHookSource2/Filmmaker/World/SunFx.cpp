#include "WorldTransient.h"
#include "SunFx.h"

#include "WorldEntities.h"
#include "WorldSchema.h"
#include "LightTintMath.h"
#include "SunRotationMath.h"
#include "LightFx.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_Linef
#include "../../ClientEntitySystem.h"
#include "../../SceneSystem.h"
#include "../../../Shared/AfxConsole.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace Filmmaker {

namespace {

// --------------------------------------------------------------------------------------------
// Parameter names, read off shipped materials (decompiled from pak01_dir.vpk 2026-08-04).
//
//   materials/skybox/sky_de_nuke.vmat_c      shader sky.vfx
//       SolarRadius      0.51
//       SolarIrradiance  [2.107187, 2.123156, 1.794319, 0]
//       SolarPosition    [0.422618, 0, 0.906308, 0]      <- unit vector, elevation 65 deg
//
//   materials/effects/glows/sun_disc_glow_001.vmat_c   shader csgo_effects.vfx
//       g_flColorBoost   129.93                          <- the blown-out halo
// --------------------------------------------------------------------------------------------
constexpr const char* kParamSolarPosition   = "SolarPosition";
constexpr const char* kParamSolarRadius     = "SolarRadius";
constexpr const char* kParamSolarIrradiance = "SolarIrradiance";
constexpr const char* kParamGlowBoost       = "g_flColorBoost";
constexpr const char* kParamGlowOpacity     = "g_flOpacityScale";
constexpr const char* kParamColorTint       = "g_vColorTint";

float g_azimuth = 0.0f;
float g_elevation = 45.0f;
bool  g_haveBaseline = false;   // azimuth/elevation seeded from the map's own sun
bool  g_posOverridden = false;  // the user has actually moved it
float g_sceneLightYaw = 0.0f;
float g_sceneLightPitch = 0.0f;

float g_size = kSunMulDefault;
float g_brightness = kSunMulDefault;
float g_glow = kSunMulDefault;
float g_glowSize = kSunMulDefault;
bool  g_visible = true;
int   g_sceneTintR = 255, g_sceneTintG = 255, g_sceneTintB = 255;
float g_sceneTintAmount = 1.0f;
bool  g_sceneTintColorOnly = true;

int g_skyCount = 0;
int g_glowCount = 0;
int g_solarPositionCount = 0;
bool g_probeArmed = false;

struct SkySnapshot {
	float position[3] = { 0.0f, 0.0f, 1.0f };
	float radius = 0.51f;
	float irradiance[3] = { 1.0f, 1.0f, 1.0f };
	bool  hasPosition = false, hasRadius = false, hasIrradiance = false;
	bool  taken = false;
};
struct GlowSnapshot {
	float boost = 1.0f;
	float opacity = 1.0f;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	bool  hasBoost = false, hasOpacity = false, hasColor = false;
	bool  taken = false;
};

std::map<std::pair<CMaterial2*,std::string>, SkySnapshot> g_skySnaps;
std::map<std::pair<CMaterial2*,std::string>, GlowSnapshot> g_glowSnaps;

bool AnyOverride() {
	return !g_visible || g_sceneLightYaw != 0 || g_sceneLightPitch != 0
        || g_sceneTintR != 255 || g_sceneTintG != 255 || g_sceneTintB != 255
        || g_posOverridden || g_size != kSunMulDefault
		|| g_brightness != kSunMulDefault || g_glow != kSunMulDefault
		|| g_glowSize != kSunMulDefault;
}

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

float SrgbByteToLinear(int value) {
	value = value < 0 ? 0 : (value > 255 ? 255 : value);
	const float srgb = (float)value / 255.0f;
	return srgb <= 0.04045f
		? srgb / 12.92f
		: powf((srgb + 0.055f) / 1.055f, 2.4f);
}

LightTintRgb SceneTint() {
	return { SrgbByteToLinear(float(WorldTransient::Number("lightColor", g_sceneTintR, 0))), SrgbByteToLinear(float(WorldTransient::Number("lightColor", g_sceneTintG, 1))),
		SrgbByteToLinear(float(WorldTransient::Number("lightColor", g_sceneTintB, 2))) };
}

LightTintMode SceneTintMode() {
	return float(WorldTransient::Number("lightUniform", g_sceneTintColorOnly, 0)) ? LightTintMode::Uniform : LightTintMode::Filtered;
}

// Source is Z-up: z = sin(elevation), and azimuth sweeps x/y.
void AnglesToDirection(float azDeg, float elDeg, float out[3]) {
	const double kDeg2Rad = 3.14159265358979323846 / 180.0;
	const double az = azDeg * kDeg2Rad;
	const double el = elDeg * kDeg2Rad;
	const double ce = cos(el);
	out[0] = (float)(ce * cos(az));
	out[1] = (float)(ce * sin(az));
	out[2] = (float)sin(el);
}

void DirectionToAngles(const float d[3], float* azDeg, float* elDeg) {
	const double kRad2Deg = 180.0 / 3.14159265358979323846;
	*elDeg = (float)(asin(Clamp(d[2], -1.0f, 1.0f)) * kRad2Deg);
	double az = atan2((double)d[1], (double)d[0]) * kRad2Deg;
	if (az < 0.0) az += 360.0;
	*azDeg = (float)az;
}

bool NameContains(const std::string& s, const char* needle) {
	return s.find(needle) != std::string::npos;
}

// The sky dome material. Matched on the path rather than the shader because the shader is not
// reachable from CMaterial2 -- every stock sky lives under materials/skybox/, and so does every
// skybox this tool imports (materials/skybox/filmmaker/...).
bool IsSkyMaterial(const std::string& n) { return NameContains(n, "skybox/"); }

// The halo ring. Several variants ship (sun_disc_glow_001..004, sun_glow_001, and a per-map
// train one), so match the family rather than an exact name.
bool IsGlowMaterial(const std::string& n) {
	return NameContains(n, "sun_disc_glow") || NameContains(n, "sun_glow");
}

void ApplyToSky(CMaterial2* mat, const std::string& name, bool restore) {
	const auto key=std::make_pair(mat,name);
    if (restore && g_skySnaps.find(key)==g_skySnaps.end()) return;
    SkySnapshot& snap = g_skySnaps[key];

	float* pos = SceneSystem_FindMaterialParam(mat, kParamSolarPosition);
	float* rad = SceneSystem_FindMaterialParam(mat, kParamSolarRadius);
	float* irr = SceneSystem_FindMaterialParam(mat, kParamSolarIrradiance);

	if (!snap.taken) {
		if (pos) { for (int c = 0; c < 3; ++c) snap.position[c] = pos[c]; snap.hasPosition = true; }
		if (rad) { snap.radius = rad[0]; snap.hasRadius = true; }
		if (irr) { for (int c = 0; c < 3; ++c) snap.irradiance[c] = irr[c]; snap.hasIrradiance = true; }
		snap.taken = true;

		// Seed the sliders from the FIRST sky material that has a direction, so opening the panel
		// shows where this map's sun actually is instead of an invented default.
		if (snap.hasPosition && !g_haveBaseline) {
			DirectionToAngles(snap.position, &g_azimuth, &g_elevation);
			g_haveBaseline = true;
		}
	}

	if (g_probeArmed) {
		MvmDebugLog_Linef("world.sun", "probe sky \"%s\" pos=%s radius=%s irradiance=%s",
			name.c_str(), pos ? "YES" : "no", rad ? "YES" : "no", irr ? "YES" : "no");
	}

	if (restore) {
		if (pos && snap.hasPosition) for (int c = 0; c < 3; ++c) pos[c] = snap.position[c];
		if (rad && snap.hasRadius) rad[0] = snap.radius;
		if (irr && snap.hasIrradiance) for (int c = 0; c < 3; ++c) irr[c] = snap.irradiance[c];
	} else {
		// Only write the direction once the user has actually moved it; otherwise a map whose sun
		// is somewhere unusual would snap to our slider's idea of "the same place".
		if (pos && snap.hasPosition) {
			float source[3] = {};
			if (float(WorldTransient::Number("sunPositionOverride", g_posOverridden, 0))) {
				AnglesToDirection(float(WorldTransient::Number("sunPosition", g_azimuth, 0)), float(WorldTransient::Number("sunPosition", g_elevation, 1)), source);
				LightFx_SetSunReference(source); // the sliders turn about this sun (LightFx.h)
				const SunRotationMath::Vec3 rotated = SunRotationMath::Rotate(
					{ source[0], source[1], source[2] }, float(WorldTransient::Number("lightDirection", g_sceneLightYaw, 0)), float(WorldTransient::Number("lightDirection", g_sceneLightPitch, 1)),
					{ source[0], source[1], source[2] });
				pos[0] = rotated.x; pos[1] = rotated.y; pos[2] = rotated.z;
			} else {
				source[0] = snap.position[0];
				source[1] = snap.position[1];
				source[2] = snap.position[2];
				LightFx_SetSunReference(source); // the sliders turn about this sun (LightFx.h)
				const SunRotationMath::Vec3 rotated = SunRotationMath::Rotate(
					{ source[0], source[1], source[2] },
					float(WorldTransient::Number("lightDirection", g_sceneLightYaw, 0)), float(WorldTransient::Number("lightDirection", g_sceneLightPitch, 1)),
					{ source[0], source[1], source[2] });
				pos[0] = rotated.x; pos[1] = rotated.y; pos[2] = rotated.z;
			}
			if (MvmDebugLog_Active()) {
				static unsigned long long s_directionWrites = 0;
				const unsigned long long write = ++s_directionWrites;
				if ((write % 32ull) == 1ull) {
					MvmDebugLog_Linef("world.sun.direction",
						"write=%llu material='%s' overridden=%d sceneYaw=%.2f scenePitch=%.2f "
						"source=(%.5f %.5f %.5f) final=(%.5f %.5f %.5f)",
						write, name.c_str(), float(WorldTransient::Number("sunPositionOverride", g_posOverridden, 0)) ? 1 : 0,
						float(WorldTransient::Number("lightDirection", g_sceneLightYaw, 0)), float(WorldTransient::Number("lightDirection", g_sceneLightPitch, 1)),
						source[0], source[1], source[2],
						pos[0], pos[1], pos[2]);
				}
			}
		}
		// SolarRadius belongs to the sky shader's sun branch, not just the disc.  Zeroing it to
		// hide the sun makes some maps skip the whole sky/cloud composite when the camera looks
		// through the zenith.  Keep the authored radius valid and remove the visible contribution
		// through irradiance below; this decouples sun visibility from clouds.
		if (rad && snap.hasRadius) rad[0] = snap.radius * float(WorldTransient::Number("sunSize", g_size, 0));
		if (irr && snap.hasIrradiance) {
			const LightTintRgb source = {
				snap.irradiance[0] * float(WorldTransient::Number("sunBrightness", g_brightness, 0)),
				snap.irradiance[1] * float(WorldTransient::Number("sunBrightness", g_brightness, 0)),
				snap.irradiance[2] * float(WorldTransient::Number("sunBrightness", g_brightness, 0))
			};
			const LightTintRgb tinted = ApplyLightTint(
				source, SceneTint(), float(WorldTransient::Number("lightAmount", g_sceneTintAmount, 0)), SceneTintMode());
			irr[0] = float(WorldTransient::Number("sunVisible", g_visible, 0)) ? tinted.r : 0.0f;
			irr[1] = float(WorldTransient::Number("sunVisible", g_visible, 0)) ? tinted.g : 0.0f;
			irr[2] = float(WorldTransient::Number("sunVisible", g_visible, 0)) ? tinted.b : 0.0f;
		}
	}

	SceneSystem_UpdateMaterial(mat);
}

void ApplyToGlow(CMaterial2* mat, const std::string& name, bool restore) {
	const auto key=std::make_pair(mat,name);
    if (restore && g_glowSnaps.find(key)==g_glowSnaps.end()) return;
    GlowSnapshot& snap = g_glowSnaps[key];

	float* boost = SceneSystem_FindMaterialParam(mat, kParamGlowBoost);
	float* opacity = SceneSystem_FindMaterialParam(mat, kParamGlowOpacity);
	float* color = SceneSystem_FindMaterialParam(mat, kParamColorTint);

	if (!snap.taken) {
		if (boost) { snap.boost = boost[0]; snap.hasBoost = true; }
		if (opacity) { snap.opacity = opacity[0]; snap.hasOpacity = true; }
		if (color) { for (int c = 0; c < 3; ++c) snap.color[c] = color[c]; snap.hasColor = true; }
		snap.taken = true;
	}

	if (g_probeArmed) {
		MvmDebugLog_Linef("world.sun", "probe glow \"%s\" boost=%s(%.2f) opacity=%s color=%s",
			name.c_str(), boost ? "YES" : "no", snap.boost, opacity ? "YES" : "no",
			color ? "YES" : "no");
	}

	if (restore) {
		if (boost && snap.hasBoost) boost[0] = snap.boost;
		if (opacity && snap.hasOpacity) opacity[0] = snap.opacity;
		if (color && snap.hasColor) for (int c = 0; c < 3; ++c) color[c] = snap.color[c];
	} else {
		const LightTintRgb tint = SceneTint();
		const float tintPeak = tint.r > tint.g
			? (tint.r > tint.b ? tint.r : tint.b)
			: (tint.g > tint.b ? tint.g : tint.b);
		const float dim = float(WorldTransient::Number("lightUniform", g_sceneTintColorOnly, 0))
			? 1.0f : 1.0f + (tintPeak - 1.0f) * float(WorldTransient::Number("lightAmount", g_sceneTintAmount, 0));
		if (boost && snap.hasBoost) boost[0] = float(WorldTransient::Number("sunVisible", g_visible, 0)) ? snap.boost * float(WorldTransient::Number("sunGlow", g_glow, 0)) * dim : 0.0f;
		if (opacity && snap.hasOpacity) opacity[0] = float(WorldTransient::Number("sunVisible", g_visible, 0)) ? snap.opacity * dim : 0.0f;
		if (color && snap.hasColor) {
			const LightTintRgb source = { snap.color[0], snap.color[1], snap.color[2] };
			const LightTintRgb tinted = ApplyLightTint(
				source, tint, float(WorldTransient::Number("lightAmount", g_sceneTintAmount, 0)), SceneTintMode());
			color[0] = tinted.r; color[1] = tinted.g; color[2] = tinted.b;
		}
	}

	SceneSystem_UpdateMaterial(mat);
}

void ForEachSunMaterial(bool restore) {
	if (!g_pCResourceSystem) return;
	GetMaterialsArrayResult materials = { 0 };
	g_pCResourceSystem->GetMaterials(&materials);

	int skies = 0, glows = 0, solarPosition = 0;
	for (uint64_t i = 0; i < materials.count; ++i) {
		// Freed/pending slots carry a -1 sentinel while the resource system grows this array
		// during a level load; dereferencing one crashed (crash.veh 2026-08-07, read of
		// 0xFFFFFFFFFFFFFFFF in materialsystem2 during the demo-start effects reset).
		CMaterial2** slot = materials.pArrMaterials[i];
		if (!slot || (uintptr_t)slot == (uintptr_t)-1) continue;
		CMaterial2* mat = *slot;
		if (!mat || (uintptr_t)mat == (uintptr_t)-1) continue;
		char nameBuf[512];
		if (!SceneSystem_MaterialName(mat, nameBuf, sizeof(nameBuf)) || !nameBuf[0]) continue;
		std::string name(nameBuf);

		if (IsSkyMaterial(name))       {
			ApplyToSky(mat, name, restore);
			++skies;
			if (SceneSystem_FindMaterialParam(mat, kParamSolarPosition)) ++solarPosition;
		}
		else if (IsGlowMaterial(name)) { ApplyToGlow(mat, name, restore); ++glows; }
	}
	g_skyCount = skies;
	g_glowCount = glows;
	g_solarPositionCount = solarPosition;

	MvmDebugLog_Linef("world.sun",
		"%s %d sky + %d glow material(s) az=%.1f el=%.1f size=%.2f bright=%.2f glow=%.2f",
		restore ? "restored" : "applied", skies, glows,
		g_azimuth, g_elevation, g_size, g_brightness, g_glow);

	g_probeArmed = false;
}

// --------------------------------------------------------------------------------------------
// SUN DIRECTION VIA THE LIGHT ENTITY -- the safe path.
//
// Writing the direction into the scene light object at render-submit time crashed CS2 repeatedly
// (Docs/effects/sun.md 3.5), including with 0x184 alone and with a guard. That is mid-pipeline
// surgery: by then the engine has already derived shadow matrices and cascade setup from the
// direction, and handing it a different one leaves those inconsistent.
//
// The light ENTITY is where the direction comes from, so rotating that lets the whole pipeline
// re-derive itself consistently -- exactly how a mapper orients a sun. Unlike env_sky, which
// ignores its own transform, a light genuinely is oriented by its transform.
//
// Same scene-node chain SkyOrient established: entity -> m_CBodyComponent -> m_pSceneNode ->
// m_angRotation, plus the m_nodeToWorld orientation quaternion the renderer actually samples.
const char* const kSunLightClasses[] = {
	"light_environment", "C_LightEnvironmentEntity",
	"light_directional", "C_LightDirectionalEntity",
	"global_light",      "C_GlobalLight",
	"env_cascade_light",
};

// SUN LIGHT ROTATION IS OFF. Four crashes, three approaches, one cause -- see the bisect in
// Docs/effects/sun.md 3.5. The decisive measurement: yaw 1 deg survives, yaw 5 deg crashes. A
// THRESHOLD rules out a bad pointer and points at the shadow cascade: the engine derives the
// light's shadow frustum earlier in the frame, and moving the light past a small tolerance leaves
// that derived data inconsistent with it.
//
// This is why the entity-transform route failed too. It is not about WHERE we write -- it is that
// changing a light's direction requires the engine's own invalidation, which a memory write
// bypasses. The fix is to find and call whatever the engine uses to move a light, not to poke a
// different field.
constexpr bool kSunLightRotationProven = false;

float g_lightYaw = 0.0f, g_lightPitch = 0.0f;
int   g_sunLightCount = 0;

struct LightSnap {
	float rot[3]; float absRot[3]; float quat[4];
	bool taken = false, hasAbs = false, hasQuat = false;
};
std::map<std::pair<CEntityInstance*, int>, LightSnap> g_lightSnaps;

void AnglesToQuat(float pitchDeg, float yawDeg, float rollDeg, float out[4]) {
	const double k = 3.14159265358979323846 / 180.0 * 0.5;
	const double sy = sin(yawDeg*k), cy = cos(yawDeg*k);
	const double sp = sin(pitchDeg*k), cp = cos(pitchDeg*k);
	const double sr = sin(rollDeg*k), cr = cos(rollDeg*k);
	out[0] = (float)(sr*cp*cy - cr*sp*sy);
	out[1] = (float)(cr*sp*cy + sr*cp*sy);
	out[2] = (float)(cr*cp*sy - sr*sp*cy);
	out[3] = (float)(cr*cp*cy + sr*sp*sy);
}

unsigned char* SceneNodeOf(CEntityInstance* ent) {
	const std::ptrdiff_t ob = WorldSchema_Offset(kWF_Ent_BodyComponent);
	const std::ptrdiff_t on = WorldSchema_Offset(kWF_Body_SceneNode);
	if (!ent || !ob || !on) return nullptr;
	unsigned char* body = *(unsigned char**)((unsigned char*)ent + ob);
	if (!body) return nullptr;
	return *(unsigned char**)(body + on);
}

} // namespace

void SunFx_RunFrame() {
	const bool active = kSunLightRotationProven && (g_lightYaw != 0.0f || g_lightPitch != 0.0f);
	// Even with the experimental entity-writer disabled, keep the per-map detector alive. The
	// GPU sun-disc hook needs to know whether this map has a light_environment or only a baked
	// SolarPosition sprite; returning before enumeration made that branch silently fall back on
	// every map.
	if (!active && g_lightSnaps.empty()) {
		int found = 0;
		if (g_pEntityList && g_GetEntityFromIndex) {
			const int highest = GetHighestEntityIndex();
			for (int i = 0; i <= highest; ++i) {
				CEntityInstance* ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, i);
				if (ent && WorldEntity_Matches(ent, kSunLightClasses,
					sizeof(kSunLightClasses) / sizeof(kSunLightClasses[0]))) ++found;
			}
		}
		g_sunLightCount = found;
		return;
	}

	const std::ptrdiff_t oRot = WorldSchema_Offset(kWF_Node_Rotation);
	const std::ptrdiff_t oAbs = WorldSchema_Offset(kWF_Node_AbsRotation);
	const std::ptrdiff_t oN2W = WorldSchema_Offset(kWF_Node_NodeToWorld);
	if (!oRot) return;

	int found = 0;
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, i);
		if (!ent) continue;
		if (!WorldEntity_Matches(ent, kSunLightClasses,
			sizeof(kSunLightClasses)/sizeof(kSunLightClasses[0]))) continue;
		unsigned char* node = SceneNodeOf(ent);
		if (!node) continue;
		++found;

		auto key = std::make_pair(ent, ent->GetHandle().ToInt());
        if (!active && g_lightSnaps.find(key) == g_lightSnaps.end()) continue;
        LightSnap& s = g_lightSnaps[key];
		float* r = (float*)(node + oRot);
		if (!s.taken) {
			s.rot[0]=r[0]; s.rot[1]=r[1]; s.rot[2]=r[2];
			if (oAbs) { const float* a=(const float*)(node+oAbs); s.absRot[0]=a[0]; s.absRot[1]=a[1]; s.absRot[2]=a[2]; s.hasAbs=true; }
			if (oN2W) { const float* q=(const float*)(node+oN2W)+4; for(int c=0;c<4;++c) s.quat[c]=q[c]; s.hasQuat=true; }
			s.taken = true;
		}

		// Added to the map's own orientation so 0 leaves the mapper's sun where they put it.
		const float pitch = s.rot[0] + (active ? g_lightPitch : 0.0f);
		const float yaw   = s.rot[1] + (active ? g_lightYaw   : 0.0f);
		r[0]=pitch; r[1]=yaw; r[2]=s.rot[2];
		if (oAbs && s.hasAbs) {
			float* a=(float*)(node+oAbs);
			a[0]=s.absRot[0]+(active?g_lightPitch:0.0f);
			a[1]=s.absRot[1]+(active?g_lightYaw:0.0f);
			a[2]=s.absRot[2];
		}
		// The quaternion in m_nodeToWorld is what the renderer samples -- the angles alone are an
		// input the engine does not recompute from (measured while working on the sky).
		if (oN2W && s.hasQuat) {
			float* q=(float*)(node+oN2W)+4;
			if (active) AnglesToQuat(pitch, yaw, s.rot[2], q);
			else { for(int c=0;c<4;++c) q[c]=s.quat[c]; }
		}
	}
	g_sunLightCount = found;
	if (!active) g_lightSnaps.clear();
}

void SunFx_SetLightYaw(float d)   { d = std::isfinite(d) ? std::fmod(d, 360.0f) : 0.0f; if (d < 0) d += 360; if (d >= 360) d = 0; g_lightYaw = d; }
void SunFx_SetLightPitch(float d) { g_lightPitch = Clamp(d, -90.0f, 90.0f); }
float SunFx_LightYaw()   { return g_lightYaw; }
float SunFx_LightPitch() { return g_lightPitch; }
int  SunFx_SunLightCount() { return g_sunLightCount; }
int  SunFx_SolarPositionCount() { return g_solarPositionCount; }
void SunFx_ResetLightAngles() { g_lightYaw = 0.0f; g_lightPitch = 0.0f; SunFx_RunFrame(); g_lightSnaps.clear(); }

void SunFx_SetSceneLightRotation(float yawDegrees, float pitchDegrees) {
	g_sceneLightYaw = SunRotationMath::WrapYaw(yawDegrees);
	g_sceneLightPitch = SunRotationMath::ClampPitch(pitchDegrees);
	SunFx_Apply();
}

void SunFx_SetSceneLightTint(int r, int g, int b, float amount, bool colorOnly) {
	g_sceneTintR = r < 0 ? 0 : (r > 255 ? 255 : r);
	g_sceneTintG = g < 0 ? 0 : (g > 255 ? 255 : g);
	g_sceneTintB = b < 0 ? 0 : (b > 255 ? 255 : b);
	g_sceneTintAmount = std::isfinite(amount) ? Clamp(amount, 0.0f, 1.0f) : 0.0f;
	g_sceneTintColorOnly = colorOnly;
	SunFx_Apply();
}

void SunFx_OnLevelInit() {
    if (!g_skySnaps.empty() || !g_glowSnaps.empty()) ForEachSunMaterial(true);
    g_skySnaps.clear();g_glowSnaps.clear();g_lightSnaps.clear();g_haveBaseline=false;
    MvmDebugLog_Linef("environment.baseline","sun materials restored and generation released");
}
bool SunFx_PositionOverridden() { return g_posOverridden; }
void SunFx_Apply() { ForEachSunMaterial(false); }

bool SunFx_HasOverride() { return AnyOverride(); }

void SunFx_ResetAll() {
	if (!g_skySnaps.empty() || !g_glowSnaps.empty()) {
		ForEachSunMaterial(true);
		g_skySnaps.clear();
		g_glowSnaps.clear();
	}
	g_size = g_brightness = g_glow = g_glowSize = kSunMulDefault;
	g_posOverridden = false;
	g_haveBaseline = false;
	g_sceneLightYaw = 0.0f;
	g_sceneLightPitch = 0.0f;
	g_visible = true;
}

void SunFx_SetAzimuth(float degrees) {
	// In one step: a float wrap that walks 360 at a time never finishes on a huge value.
	degrees = std::isfinite(degrees) ? std::fmod(degrees, 360.0f) : 0.0f;
	if (degrees < 0.0f) degrees += 360.0f;
	if (degrees >= 360.0f) degrees = 0.0f;
	g_azimuth = degrees;
	g_posOverridden = true;
	SunFx_Apply();
}

void SunFx_SetElevation(float degrees) {
	g_elevation = Clamp(degrees, -20.0f, 90.0f);
	g_posOverridden = true;
	SunFx_Apply();
}

void SunFx_SetSize(float mul)       { g_size = Clamp(mul, 0.05f, 20.0f); SunFx_Apply(); }
void SunFx_SetBrightness(float mul) { g_brightness = Clamp(mul, 0.0f, 5.0f); SunFx_Apply(); }
void SunFx_SetGlow(float mul)       { g_glow = Clamp(mul, 0.0f, 3.0f); SunFx_Apply(); }
void SunFx_SetGlowSize(float mul)   { g_glowSize = Clamp(mul, 0.05f, 8.0f); }
void SunFx_SetVisible(bool visible) { g_visible = visible; SunFx_Apply(); }

float SunFx_Azimuth()    { return g_azimuth; }
float SunFx_Elevation()  { return g_elevation; }
float SunFx_Size()       { return g_size; }
float SunFx_Brightness() { return g_brightness; }
float SunFx_Glow()       { return g_glow; }
float SunFx_GlowSize()   { return g_glowSize; }
bool  SunFx_Visible()    { return g_visible; }
bool  SunFx_HasBaseline(){ return g_haveBaseline; }
int   SunFx_SkyMaterialCount()  { return g_skyCount; }
int   SunFx_GlowMaterialCount() { return g_glowCount; }

void SunFx_Probe() {
	g_probeArmed = true;
	SunFx_Apply();
}

void SunFx_AppendStateJson(std::ostringstream& o) {
	o << ",\"sunAz\":" << g_azimuth;
	o << ",\"sunEl\":" << g_elevation;
	o << ",\"sunSize\":" << g_size;
	o << ",\"sunBright\":" << g_brightness;
	o << ",\"sunGlow\":" << g_glow;
	o << ",\"sunGlowSize\":" << g_glowSize;
	o << ",\"sunVisible\":" << (g_visible ? "true" : "false");
	o << ",\"sunSkyMats\":" << g_skyCount;
	o << ",\"sunGlowMats\":" << g_glowCount;
	o << ",\"sunSolarPositionMats\":" << g_solarPositionCount;
	o << ",\"sunLightYaw\":" << g_lightYaw;
	o << ",\"sunLightPitch\":" << g_lightPitch;
	o << ",\"sunLights\":" << g_sunLightCount;
	o << ",\"sunBase\":" << (g_haveBaseline ? "true" : "false");
}

void SunFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	// argv: mirv_filmmaker world sun <op> <value>
	const char* op = (argc >= 4) ? args->ArgV(3) : "";

	if (0 == _stricmp(op, "reset")) {
		SunFx_ResetLightAngles();
		SunFx_ResetAll();
		advancedfx::Message("Sun restored to the map's own.\n");
		return;
	}
	if (0 == _stricmp(op, "apply")) {
		SunFx_Apply();
		advancedfx::Message("Sun re-applied: %d sky, %d glow material(s).\n", g_skyCount, g_glowCount);
		return;
	}
	if (0 == _stricmp(op, "visible")) {
		if (argc >= 5) SunFx_SetVisible(0 != atoi(args->ArgV(4)));
		advancedfx::Message("Sun material contribution %s.\n", g_visible ? "visible" : "hidden");
		return;
	}
	if (0 == _stricmp(op, "sunyaw") || 0 == _stricmp(op, "sunpitch")) {
		if (!kSunLightRotationProven) {
			advancedfx::Message(
				"Sun light rotation is DISABLED -- it crashes CS2.\n"
				"Measured: yaw 1 deg survives, yaw 5 deg crashes. A threshold, not a bad pointer:\n"
				"the shadow cascade is derived before this point and moving the light invalidates it.\n"
				"See Docs/effects/sun.md 3.5. Glow, size and brightness are unaffected.\n");
			return;
		}
		const bool isYaw = (0 == _stricmp(op, "sunyaw"));
		if (argc >= 5) {
			if (isYaw) SunFx_SetLightYaw((float)atof(args->ArgV(4)));
			else       SunFx_SetLightPitch((float)atof(args->ArgV(4)));
			const bool q = (argc >= 6) && (0 == _stricmp(args->ArgV(5), "quiet"));
			if (!q) advancedfx::Message("Sun light %s %.1f deg (%d sun light entit%s).\n",
				isYaw ? "yaw" : "pitch",
				isYaw ? SunFx_LightYaw() : SunFx_LightPitch(),
				SunFx_SunLightCount(), SunFx_SunLightCount() == 1 ? "y" : "ies");
			return;
		}
		advancedfx::Message("Sun light %s is %.1f deg.\n", isYaw ? "yaw" : "pitch",
			isYaw ? SunFx_LightYaw() : SunFx_LightPitch());
		return;
	}

	if (0 == _stricmp(op, "probe")) {
		SunFx_Probe();
		advancedfx::Message(
			"Probed %d sky + %d glow material(s) into mvm_debug channel world.sun.\n"
			"It reports WHICH parameters resolved -- run it first on a new map.\n",
			g_skyCount, g_glowCount);
		return;
	}

	struct Knob { const char* name; void (*set)(float); float (*get)(); const char* unit; };
	const Knob knobs[] = {
		{ "azimuth",    &SunFx_SetAzimuth,    &SunFx_Azimuth,    "deg" },
		{ "elevation",  &SunFx_SetElevation,  &SunFx_Elevation,  "deg" },
		{ "size",       &SunFx_SetSize,       &SunFx_Size,       "x" },
		{ "brightness", &SunFx_SetBrightness, &SunFx_Brightness, "x" },
		{ "glow",       &SunFx_SetGlow,       &SunFx_Glow,       "x" },
		{ "glow_size",  &SunFx_SetGlowSize,   &SunFx_GlowSize,   "x" },
	};

	for (const Knob& k : knobs) {
		if (0 != _stricmp(op, k.name)) continue;
		if (argc >= 5) {
			k.set((float)atof(args->ArgV(4)));
			const bool quiet = (argc >= 6) && (0 == _stricmp(args->ArgV(5), "quiet"));
			if (!quiet)
				advancedfx::Message("Sun %s %.2f%s (%d sky, %d glow material(s)).\n",
					k.name, k.get(), k.unit, g_skyCount, g_glowCount);
			return;
		}
		advancedfx::Message("Sun %s is %.2f%s.\n", k.name, k.get(), k.unit);
		return;
	}

	advancedfx::Message(
		"mirv_filmmaker world sun - where the sun is, how big, how bright, and its glow ring.\n"
		"Usage:\n"
		"  %s world sun azimuth <deg>     - swing it round the horizon (absolute)\n"
		"  %s world sun elevation <deg>   - raise or lower it, -20..90 (absolute)\n"
		"  %s world sun size <x>          - disc size; multiplies SolarRadius (ships at 0.51 deg)\n"
		"  %s world sun brightness <x>    - the disc itself; multiplies SolarIrradiance\n"
		"  %s world sun glow <x>          - THE HALO RING; multiplies its g_flColorBoost, which\n"
		"                                   ships around 130 and is what blows out on some maps\n"
		"  %s world sun glow_size <x>     - halo footprint multiplier (1.00 = authored size)\n"
		"  %s world sun probe             - report which parameters this map actually exposes\n"
		"  %s world sun apply | reset\n"
		"Size/brightness/glow MULTIPLY the map's own values, so 1.00 is untouched anywhere.\n"
		"Position is absolute and seeded from the map's own sun.\n"
		"Current: az %.1f el %.1f size %.2f bright %.2f glow %.2f glowSize %.2f | %d sky, %d glow material(s).\n",
		cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd,
		g_azimuth, g_elevation, g_size, g_brightness, g_glow, g_glowSize, g_skyCount, g_glowCount);
}

} // namespace Filmmaker
