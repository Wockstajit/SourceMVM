#define NOMINMAX
#include "EnvironmentNativeFog.h"
#include "EnvironmentClimateMath.h"
#include "../Debug/MvmDebugLog.h"
#include "../World/WorldEntities.h"
#include "../../ClientEntitySystem.h"
#include "../../SchemaSystem.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <type_traits>
#include <utility>

namespace Filmmaker::Environment {
namespace {

const char *const kGradientFogClasses[] = {
    "env_gradient_fog",
    "gradient_fog",
    "C_GradientFog",
    "C_EnvGradientFog",
};

struct GradientFogSnapshot {
    bool taken = false;
    bool fogEnable = false;
    bool startDisabled = false;
    bool heightFogEnabled = false;
    bool needsTextures = false;
    uint32_t fogColor = 0;
    float fogStart = 0.f;
    float fogEnd = 0.f;
    float falloffExp = 1.f;
    float startHeight = 0.f;
    float endHeight = 0.f;
    float verticalExp = 1.f;
    float strength = 1.f;
    float maxOpacity = 1.f;
};

struct AppliedGradientFog {
    uint32_t colorRgba = 0;
    float fogStart = 0.f;
    float fogEnd = 0.f;
    float falloffExp = 1.f;
    float startHeight = 0.f;
    float endHeight = 0.f;
    float verticalExp = 1.f;
    float strength = 1.f;
    float maxOpacity = 1.f;
    bool needsTextures = false;
};

std::map<std::pair<CEntityInstance *, int>, GradientFogSnapshot> g_snapshots;
std::atomic<bool> g_nativeAlyxFogActive{false};
AppliedGradientFog g_lastLogged{};
bool g_haveLastLogged = false;
ULONGLONG g_lastRescanMs = 0;

template <typename T>
bool ReadField(unsigned char *base, ptrdiff_t off, T &out) {
    if (!off) return false;
    __try {
        out = *(const T *)(base + off);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
bool WriteField(unsigned char *base, ptrdiff_t off, T value) {
    if (!off) return false;
    __try {
        *(T *)(base + off) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
bool FieldDiffers(unsigned char *base, ptrdiff_t off, T desired, float eps = 0.f) {
    if (!off) return false;
    T cur{};
    if (!ReadField(base, off, cur)) return true;
    if constexpr (std::is_floating_point_v<T>) {
        return std::fabs(cur - desired) > eps;
    } else {
        return cur != desired;
    }
}

bool CaptureSnapshot(CEntityInstance *ent, GradientFogSnapshot &s) {
    const auto &gf = g_clientDllOffsets.C_GradientFog;
    unsigned char *base = (unsigned char *)ent;
    ReadField(base, gf.m_bFogEnable, s.fogEnable);
    ReadField(base, gf.m_bStartDisabled, s.startDisabled);
    ReadField(base, gf.m_bHeightFogEnabled, s.heightFogEnabled);
    ReadField(base, gf.m_bGradientFogNeedsTextures, s.needsTextures);
    ReadField(base, gf.m_FogColor, s.fogColor);
    ReadField(base, gf.m_flFogStart, s.fogStart);
    ReadField(base, gf.m_flFogEnd, s.fogEnd);
    ReadField(base, gf.m_flFogFalloffExponent, s.falloffExp);
    ReadField(base, gf.m_flFogStartHeight, s.startHeight);
    ReadField(base, gf.m_flFogEndHeight, s.endHeight);
    ReadField(base, gf.m_flFogVerticalExponent, s.verticalExp);
    ReadField(base, gf.m_flFogStrength, s.strength);
    ReadField(base, gf.m_flFogMaxOpacity, s.maxOpacity);
    s.taken = true;
    return true;
}

void RestoreSnapshot(CEntityInstance *ent, const GradientFogSnapshot &s) {
    if (!s.taken) return;
    const auto &gf = g_clientDllOffsets.C_GradientFog;
    unsigned char *base = (unsigned char *)ent;
    WriteField(base, gf.m_bFogEnable, s.fogEnable);
    WriteField(base, gf.m_bStartDisabled, s.startDisabled);
    WriteField(base, gf.m_bHeightFogEnabled, s.heightFogEnabled);
    WriteField(base, gf.m_FogColor, s.fogColor);
    WriteField(base, gf.m_flFogStart, s.fogStart);
    WriteField(base, gf.m_flFogEnd, s.fogEnd);
    WriteField(base, gf.m_flFogFalloffExponent, s.falloffExp);
    WriteField(base, gf.m_flFogStartHeight, s.startHeight);
    WriteField(base, gf.m_flFogEndHeight, s.endHeight);
    WriteField(base, gf.m_flFogVerticalExponent, s.verticalExp);
    WriteField(base, gf.m_flFogStrength, s.strength);
    WriteField(base, gf.m_flFogMaxOpacity, s.maxOpacity);
    if (gf.m_bGradientFogNeedsTextures) WriteField(base, gf.m_bGradientFogNeedsTextures, true);
}

uint8_t ToSrgbByte(float linearVal) {
    const float c = std::clamp(linearVal, 0.f, 1.f);
    const float srgb = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
    return uint8_t(std::clamp(std::round(srgb * 255.f), 0.f, 255.f));
}

AppliedGradientFog ComputeAlyxGradientFog(const Scene &scene, Vec3 camera, bool cameraValid) {
    const auto &c = scene.climate;
    AppliedGradientFog out;
    const float density = std::clamp(c[CL_fogDensity], 0.f, 6.f);

    // Calibrated against live Half-Life: Alyx (`a2_quarantine_entrance` & `a3_c17_processing_plant`):
    // keep the foreground crisp (`fadeinstart ~ 50`) while bending the ramp smoothly (`exponent 1.65`)
    // instead of simply translating a linear fog start outward.
    out.fogStart = (std::max)(50.f, c[CL_fogStart]);
    const float rawSpan = (c[CL_fogRange] > 10.f ? c[CL_fogRange] : 1400.f) / (std::max)(0.35f, std::pow(density, 0.72f));
    out.fogEnd = std::clamp(out.fogStart + (std::max)(240.f, rawSpan), out.fogStart + 240.f, 8000.f);
    out.falloffExp = 1.65f;

    const float baseZ = (std::abs(c[CL_fogBase]) > 1e-3f) ? c[CL_fogBase]
                                                           : (cameraValid ? camera.z - 96.f : 0.f);
    out.startHeight = baseZ;
    out.endHeight = baseZ + (std::max)(120.f, c[CL_fogHeight]);
    out.verticalExp = 2.4f;
    out.strength = std::clamp(0.55f + density * 0.45f, 0.f, 1.f);
    out.maxOpacity = std::clamp(0.28f + density * 0.45f, 0.f, 0.85f);

    // Modulate fog color so it harmonizes with both regular CS2 lighting (`relighting=0`) and
    // Filmmaker's sun/night/weather lighting (`relighting=1`).
    float lightScale = 0.88f;
    if (c[CL_relighting] > 0.5f) {
        const float elev = c[CL_sunElevation];
        const float day = std::clamp((elev + 4.f) / 28.f, 0.18f, 1.05f);
        lightScale = day * std::clamp(0.65f + 0.35f * c[CL_fogLightIntensity], 0.4f, 1.35f);
    }
    const Vec3 tint{
        scene.fogColor.x * lightScale * (c[CL_relighting] > 0.5f ? std::clamp(c[CL_sunColorR], 0.35f, 1.4f) : 1.f),
        scene.fogColor.y * lightScale * (c[CL_relighting] > 0.5f ? std::clamp(c[CL_sunColorG], 0.35f, 1.4f) : 1.f),
        scene.fogColor.z * lightScale * (c[CL_relighting] > 0.5f ? std::clamp(c[CL_sunColorB], 0.35f, 1.4f) : 1.f),
    };
    const uint8_t r = ToSrgbByte(tint.x);
    const uint8_t g = ToSrgbByte(tint.y);
    const uint8_t b = ToSrgbByte(tint.z);
    out.colorRgba = uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (255u << 24);
    return out;
}

bool SameApplied(const AppliedGradientFog &a, const AppliedGradientFog &b) {
    return a.colorRgba == b.colorRgba &&
           std::fabs(a.fogStart - b.fogStart) < 0.5f &&
           std::fabs(a.fogEnd - b.fogEnd) < 1.0f &&
           std::fabs(a.startHeight - b.startHeight) < 0.5f &&
           std::fabs(a.endHeight - b.endHeight) < 0.5f &&
           std::fabs(a.strength - b.strength) < 0.005f &&
           std::fabs(a.maxOpacity - b.maxOpacity) < 0.005f;
}

bool ValidFogEntity(CEntityInstance *ent) {
    return ent && WorldEntity_Matches(ent, kGradientFogClasses, sizeof(kGradientFogClasses) / sizeof(kGradientFogClasses[0]));
}

} // namespace

void UpdateNativeFog(const Scene &scene, const std::string &map, bool active, Vec3 camera, bool cameraValid) {
    const auto &gf = g_clientDllOffsets.C_GradientFog;
    if (!gf.m_flFogStart || !gf.m_flFogEnd || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) {
        g_nativeAlyxFogActive.store(false, std::memory_order_relaxed);
        return;
    }

    const bool want = active && !map.empty() && scene.climate[CL_enabled] > 0.5f &&
                      scene.climate[CL_alyxFog] > 0.5f && scene.climate[CL_fogDensity] > 1e-4f;

    if (!want) {
        g_nativeAlyxFogActive.store(false, std::memory_order_relaxed);
        if (g_snapshots.empty()) return;
        const int highest = GetHighestEntityIndex();
        for (const auto &kv : g_snapshots) {
            CEntityInstance *ent = kv.first.first;
            // Verify entity pointer is still in range/live before writing.
            bool stillPresent = false;
            for (int i = 0; i <= highest; ++i) {
                if ((CEntityInstance *)g_GetEntityFromIndex(*g_pEntityList, i) == ent && ValidFogEntity(ent)) {
                    stillPresent = true;
                    break;
                }
            }
            if (stillPresent) {
                RestoreSnapshot(ent, kv.second);
                MvmDebugLog_Linef("environment.nativefog", "restored C_GradientFog ent=%p", (void *)ent);
            }
        }
        g_snapshots.clear();
        g_haveLastLogged = false;
        return;
    }

    const AppliedGradientFog target = ComputeAlyxGradientFog(scene, camera, cameraValid);
    const ULONGLONG now = GetTickCount64();
    const bool rescan = g_snapshots.empty() || (now - g_lastRescanMs > 1000);
    if (rescan) g_lastRescanMs = now;

    int drivenCount = 0;
    const int highest = GetHighestEntityIndex();
    for (int i = 0; i <= highest; ++i) {
        CEntityInstance *ent = (CEntityInstance *)g_GetEntityFromIndex(*g_pEntityList, i);
        if (!ValidFogEntity(ent)) continue;
        const auto key = std::make_pair(ent, ent->GetHandle().ToInt());
        GradientFogSnapshot &snap = g_snapshots[key];
        const bool firstApply = !snap.taken;
        if (firstApply) CaptureSnapshot(ent, snap);

        unsigned char *base = (unsigned char *)ent;
        const bool dirty =
            firstApply ||
            FieldDiffers(base, gf.m_bFogEnable, true) ||
            FieldDiffers(base, gf.m_bStartDisabled, false) ||
            FieldDiffers(base, gf.m_bHeightFogEnabled, true) ||
            FieldDiffers(base, gf.m_FogColor, target.colorRgba) ||
            FieldDiffers(base, gf.m_flFogStart, target.fogStart, 0.25f) ||
            FieldDiffers(base, gf.m_flFogEnd, target.fogEnd, 0.5f) ||
            FieldDiffers(base, gf.m_flFogFalloffExponent, target.falloffExp, 0.005f) ||
            FieldDiffers(base, gf.m_flFogStartHeight, target.startHeight, 0.25f) ||
            FieldDiffers(base, gf.m_flFogEndHeight, target.endHeight, 0.25f) ||
            FieldDiffers(base, gf.m_flFogVerticalExponent, target.verticalExp, 0.005f) ||
            FieldDiffers(base, gf.m_flFogStrength, target.strength, 0.002f) ||
            FieldDiffers(base, gf.m_flFogMaxOpacity, target.maxOpacity, 0.002f);

        WriteField(base, gf.m_bFogEnable, true);
        WriteField(base, gf.m_bStartDisabled, false);
        WriteField(base, gf.m_bHeightFogEnabled, true);
        WriteField(base, gf.m_FogColor, target.colorRgba);
        WriteField(base, gf.m_flFogStart, target.fogStart);
        WriteField(base, gf.m_flFogEnd, target.fogEnd);
        WriteField(base, gf.m_flFogFalloffExponent, target.falloffExp);
        WriteField(base, gf.m_flFogStartHeight, target.startHeight);
        WriteField(base, gf.m_flFogEndHeight, target.endHeight);
        WriteField(base, gf.m_flFogVerticalExponent, target.verticalExp);
        WriteField(base, gf.m_flFogStrength, target.strength);
        WriteField(base, gf.m_flFogMaxOpacity, target.maxOpacity);
        if (dirty && gf.m_bGradientFogNeedsTextures) WriteField(base, gf.m_bGradientFogNeedsTextures, true);
        ++drivenCount;

        if (!g_haveLastLogged || !SameApplied(target, g_lastLogged)) {
            g_lastLogged = target;
            g_haveLastLogged = true;
            MvmDebugLog_Linef(
                "environment.nativefog",
                "ent=%d ptr=%p start=%.1f end=%.1f exp=%.2f h0=%.1f h1=%.1f vexp=%.2f str=%.2f maxOp=%.2f rgba=0x%08x",
                i, (void *)ent, target.fogStart, target.fogEnd, target.falloffExp,
                target.startHeight, target.endHeight, target.verticalExp,
                target.strength, target.maxOpacity, target.colorRgba);
        }
    }
    g_nativeAlyxFogActive.store(drivenCount > 0, std::memory_order_relaxed);
}

void ResetNativeFog() {
    g_snapshots.clear();
    g_nativeAlyxFogActive.store(false, std::memory_order_relaxed);
    g_haveLastLogged = false;
    g_lastRescanMs = 0;
}

bool NativeAlyxFogActive() {
    return g_nativeAlyxFogActive.load(std::memory_order_relaxed);
}

} // namespace Filmmaker::Environment
