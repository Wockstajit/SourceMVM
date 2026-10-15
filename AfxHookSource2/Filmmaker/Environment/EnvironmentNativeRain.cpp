#include "EnvironmentNativeRain.h"
#include "../Debug/MvmDebugLog.h"
#include "../Movie/ParticleFx.h"
#include "../../MirvTime.h"
#include "../../ClientEntitySystem.h"
#include "../../Globals.h"
#include "../../SchemaSystem.h"
#include "../Cosmetics/CosmeticEntityAccess.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <Windows.h>

// Engine particle-manager entry points resolved by MirvColors.cpp. The third argument of the
// update call is the CONTROL POINT index (it builds a transform from the position and calls the
// manager's SetControlPointTransform). Destroy is the body behind the "DestroyParticle(int index,
// bool bDestroyImmediately)" script binding and is optional: null when its signature is missing.
struct ParticleData;
extern unsigned char *(__fastcall *g_Original_getParticleManager)();
extern int *(__fastcall *g_Original_createParticle)(unsigned char *, int *, const char *, int, int64_t, int64_t,
                                                     int64_t, int64_t);
extern void(__fastcall *g_Original_updateParticle)(unsigned char *, int, int, ParticleData *, int);
extern void(__fastcall *g_Original_destroyParticle)(unsigned char *, int, bool, bool);

namespace Filmmaker::Environment {
namespace {
constexpr const char *kRainSystem = "particles/rain_fx/rain_storm.vpcf";
constexpr int kPrecipitationRain = 1;
constexpr bool kUseStormParticle = false;

int g_index = -1;               // main thread only
bool g_precached = false;       // per level
bool g_precacheFailed = false;  // per level; reported, never retried in a loop
int g_lastTick = -1;
ULONGLONG g_retryAtMs = 0;
std::atomic<bool> g_running{false};
const char *g_status = "off";

// ---- world weather ---------------------------------------------------------------------------
// CMapInfo pushes its weather into IWorldRendererMgr (worldrenderer.dll) for the world its entity
// lives in. Found in client.dll at that push; the same bytes give us the three things we call:
//   mov rax,[rip+WorldRendererMgr]; mov rdx,[rsi+10h]; movzx ebx,byte[rsi+m_bRainTraceToSkyEnabled]
//   mov rcx,[rax]; mov edx,[rdx+38h]; mov rdi,[rcx+1B8h]; mov rcx,[rip+RenderGameSystem]; call WorldFor
// vtable 0x1B8 = SetRainExposed(world, bool) (IsRainExposed is 0x1C0 and is what the weapon, glove
// and arm shading reads); 0x1C8 = SetWeather(world, rain, ripple, rippleDirection, wetness, drying).
using WorldFor_t = void *(__fastcall *)(void *renderGameSystem, int worldGroup);
using SetExposed_t = void(__fastcall *)(void *mgr, void *world, bool exposed);
using IsExposed_t = bool(__fastcall *)(void *mgr, void *world);
using SetWeather_t = void(__fastcall *)(void *mgr, void *world, float rain, float ripple, float direction,
                                        float wetness, float drying);
constexpr ptrdiff_t kIdentityWorldGroup = 0x38; // CEntityIdentity::m_worldGroupId
void **g_worldMgr = nullptr;
void **g_renderGameSystem = nullptr;
WorldFor_t g_worldFor = nullptr;
bool g_worldResolved = false;
struct TouchedWorld { void *world; bool wasExposed; };
std::vector<TouchedWorld> g_touched; // worlds we changed; restored when the weather goes away
NativeWorldWeather g_applied;
ULONGLONG g_worldScanAtMs = 0;
char g_worldStatus[160] = "not resolved";

void ResolveWorldWeather() {
    if (g_worldResolved)
        return;
    g_worldResolved = true;
    HMODULE client = GetModuleHandleA("client.dll");
    size_t site = client ? getAddress(client, "48 8B 05 ?? ?? ?? ?? 48 8B 56 10 0F B6 9E ?? ?? ?? ?? 48 8B 08 8B 52 38 "
                                              "48 8B B9 B8 01 00 00 48 8B 0D ?? ?? ?? ?? E8")
                         : 0;
    if (!site) {
        std::snprintf(g_worldStatus, sizeof(g_worldStatus), "unavailable (map weather push not found)");
        return;
    }
    g_worldMgr = (void **)(site + 7 + *(int32_t *)(site + 3));
    g_renderGameSystem = (void **)(site + 0x26 + *(int32_t *)(site + 0x22));
    g_worldFor = (WorldFor_t)(site + 0x2B + *(int32_t *)(site + 0x27));
    std::snprintf(g_worldStatus, sizeof(g_worldStatus), "resolved");
}

void *WorldForGroup(int group) {
    __try {
        return g_worldFor(*g_renderGameSystem, group);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool ReadWorldGroup(CEntityInstance *ent, int &group) {
    __try {
        const ptrdiff_t at = g_clientDllOffsets.CEntityInstance.m_pEntity;
        unsigned char *identity = at ? *(unsigned char **)((unsigned char *)ent + at) : nullptr;
        if (!identity)
            return false;
        group = *(int *)(identity + kIdentityWorldGroup);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsHudModel(CEntityInstance *ent) {
    __try {
        const char *c = ent->GetClassName();
        return c && std::strstr(c, "hudmodel") != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The worlds our weather applies to: the map's (worldent) and the first-person models' (the HUD
// weapon and arms are client-only entities that may sit in a world group of their own).
std::vector<void *> WeatherWorlds() {
    std::vector<int> groups;
    auto add = [&](CEntityInstance *ent) {
        int group = 0;
        if (ent && ReadWorldGroup(ent, group) && std::find(groups.begin(), groups.end(), group) == groups.end())
            groups.push_back(group);
    };
    if (g_pEntityList && *g_pEntityList && g_GetEntityFromIndex) {
        add((CEntityInstance *)g_GetEntityFromIndex(*g_pEntityList, 0));
        for (int i = 0x4000; i < 0x4400; ++i) {
            auto *ent = (CEntityInstance *)g_GetEntityFromIndex(*g_pEntityList, i);
            if (ent && IsHudModel(ent))
                add(ent);
        }
    }
    std::vector<void *> worlds;
    for (int group : groups)
        if (void *w = WorldForGroup(group); w && std::find(worlds.begin(), worlds.end(), w) == worlds.end())
            worlds.push_back(w);
    return worlds;
}

bool CallExposed(void *world, bool exposed, bool *was) {
    __try {
        void *mgr = *g_worldMgr;
        void **vt = *(void ***)mgr;
        if (was)
            *was = ((IsExposed_t)vt[0x1C0 / 8])(mgr, world);
        ((SetExposed_t)vt[0x1B8 / 8])(mgr, world, exposed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallWeather(void *world, const NativeWorldWeather &w) {
    __try {
        void *mgr = *g_worldMgr;
        void **vt = *(void ***)mgr;
        ((SetWeather_t)vt[0x1C8 / 8])(mgr, world, w.rain, w.ripple, w.rippleDirection, w.wetness, w.drying);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// THIRD-PERSON PLAYERS. C_CSPlayerPawn caches whether its world is rain-exposed (0 = not asked,
// 1 = exposed, 2 = not) and only then traces the sky to wet the model. The cache is filled on the
// first think, long before the weather is switched on, so a pawn stays dry for the whole level
// unless it is cleared. The member offset is read from the engine's own write:
//   movzx ecx,al; neg cl; sbb edx,edx; add edx,2; mov [rdi+state],edx; test al,al; jz; movss xmm0,...
ptrdiff_t g_pawnRainState = 0;
bool g_pawnRainResolved = false;
void ResolvePawnRainState() {
    if (g_pawnRainResolved)
        return;
    g_pawnRainResolved = true;
    HMODULE client = GetModuleHandleA("client.dll");
    size_t site = client ? getAddress(client, "0F B6 C8 F6 D9 1B D2 83 C2 02 89 97 ?? ?? ?? ?? 84 C0 74 0A F3 0F 10 05") : 0;
    int32_t offset = site ? *(int32_t *)(site + 12) : 0;
    g_pawnRainState = offset > 0x400 && offset < 0x8000 ? offset : 0;
    MvmDebugLog_LinefAlways("environment.rain", "pawn rain state offset=0x%llx", (unsigned long long)g_pawnRainState);
}
bool ClearPawnRainState(CEntityInstance *ent) {
    __try {
        if (!ent->IsPlayerPawn())
            return false;
        *(int32_t *)((unsigned char *)ent + g_pawnRainState) = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void ResetPawnRainState() {
    ResolvePawnRainState();
    if (!g_pawnRainState)
        return;
    int cleared = 0;
    for (int i = 1, n = GetHighestEntityIndex(); i <= n; ++i)
        if (CEntityInstance *ent = Filmmaker::CosmeticEntFromIndex(i); ent && ClearPawnRainState(ent))
            ++cleared;
    MvmDebugLog_LinefAlways("environment.rain", "pawn rain state cleared on %d players", cleared);
}

bool SameWeather(const NativeWorldWeather &a, const NativeWorldWeather &b) {
    return a.exposed == b.exposed && a.rain == b.rain && a.ripple == b.ripple &&
           a.rippleDirection == b.rippleDirection && a.wetness == b.wetness && a.drying == b.drying;
}

void ApplyWorldWeather(const NativeWorldWeather &want) {
    ResolveWorldWeather();
    if (!g_worldFor || !g_worldMgr || !*g_worldMgr || !g_renderGameSystem || !*g_renderGameSystem)
        return;
    const bool on = want.exposed || want.rain > 0 || want.wetness > 0 || want.ripple > 0;
    const ULONGLONG now = GetTickCount64();
    const bool changed = !SameWeather(want, g_applied);
    // New worlds (a HUD model created after the weather started) are picked up once a second.
    if (!changed && !(on && now >= g_worldScanAtMs))
        return;
    g_worldScanAtMs = now + 1000;
    if (on) {
        for (void *world : WeatherWorlds()) {
            auto it = std::find_if(g_touched.begin(), g_touched.end(),
                                   [&](const TouchedWorld &t) { return t.world == world; });
            const bool fresh = it == g_touched.end();
            if (!fresh && !changed)
                continue;
            bool was = false;
            if (!CallExposed(world, want.exposed, fresh ? &was : nullptr) || !CallWeather(world, want))
                continue;
            if (fresh)
                g_touched.push_back({world, was});
            MvmDebugLog_LinefAlways("environment.rain", "world weather %p exposed=%d rain=%.2f ripple=%.2f wet=%.2f",
                                    world, want.exposed ? 1 : 0, want.rain, want.ripple, want.wetness);
        }
    } else {
        for (const auto &t : g_touched) {
            CallExposed(t.world, t.wasExposed, nullptr);
            CallWeather(t.world, NativeWorldWeather{});
            MvmDebugLog_LinefAlways("environment.rain", "world weather %p restored exposed=%d", t.world,
                                    t.wasExposed ? 1 : 0);
        }
        g_touched.clear();
    }
    if (want.exposed != g_applied.exposed)
        ResetPawnRainState();
    g_applied = want;
    std::snprintf(g_worldStatus, sizeof(g_worldStatus), "worlds=%zu exposed=%d rain=%.2f ripple=%.2f wet=%.2f",
                  g_touched.size(), want.exposed ? 1 : 0, want.rain, want.ripple, want.wetness);
}

bool Available() {
    return g_Original_getParticleManager && g_Original_createParticle && g_Original_updateParticle &&
           g_Original_destroyParticle;
}

bool SetPoint(unsigned char *manager, int index, int cp, const float v[3]) {
    __try {
        g_Original_updateParticle(manager, index, cp, (ParticleData *)v, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Create(unsigned char *manager, int &index) {
    __try {
        index = -1;
        g_Original_createParticle(manager, &index, kRainSystem, 8, 0, 0, 0, 0);
        return index >= 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        index = -1;
        return false;
    }
}

void Destroy(bool immediately) {
    if (g_index < 0)
        return;
    __try {
        if (unsigned char *manager = g_Original_getParticleManager())
            g_Original_destroyParticle(manager, g_index, immediately, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_index = -1;
}
} // namespace

NativeWorldWeather NativeWorldWeatherFor(const Scene &scene, bool active) {
    NativeWorldWeather w;
    if (!active)
        return w;
    const bool raining = scene.weather.type == kPrecipitationRain && scene.weather.count > 0;
    const bool climate = scene.climate[CL_enabled] != 0;
    const float snow = climate ? std::clamp(scene.climate[CL_snow], 0.f, 1.f) : 0.f;
    const float wet = climate ? scene.climate[CL_wet] : 0.f;
    const float puddles = climate ? scene.climate[CL_puddles] : 0.f;
    w.exposed = raining;
    w.rain = raining ? NativeRainIntensity(scene.weather.count) / 4.f : 0.f;
    w.ripple = raining ? (std::max)(puddles, .5f) : 0.f;
    // Weapon droplets follow the rain; how wet materials get follows the Wetness control.
    w.wetness = std::clamp(wet, 0.f, 1.f) * (1 - snow);
    return w;
}

void NativeWorldWeatherReport(char *out, size_t size) {
    ResolveWorldWeather();
    std::string worlds;
    if (g_worldFor && g_renderGameSystem && *g_renderGameSystem) {
        char item[48];
        for (void *w : WeatherWorlds()) {
            std::snprintf(item, sizeof(item), " %p", w);
            worlds += item;
        }
    }
    std::snprintf(out, size, "world weather: %s; worlds:%s", g_worldStatus, worlds.empty() ? " none" : worlds.c_str());
}

void UpdateNativeRain(const Scene &scene, const std::string &map, bool active, Vec3 camera, bool cameraValid) {
    (void)map;
    ApplyWorldWeather(NativeWorldWeatherFor(scene, active));
    // Valve's storm particle stays OFF: it culls every drop against the map's baked precipitation
    // ray-trace data, which only maps built for weather (de_train) ship -- on de_mirage it drew a
    // handful of streaks. Rain is the environment's own shader rain; the world weather above still
    // gives weapons, gloves and arms CS2's native wet shading.
    const bool want = kUseStormParticle && active && cameraValid && scene.weather.type == kPrecipitationRain &&
                      scene.weather.count > 0;
    if (!want) {
        if (g_index >= 0) {
            // Let the drops already in the air finish falling (end caps) instead of vanishing.
            Destroy(false);
            MvmDebugLog_LinefAlways("environment.rain", "native rain stopped");
        }
        g_running.store(false);
        g_status = "off";
        return;
    }
    if (!Available()) {
        g_running.store(false);
        g_status = "unavailable (particle manager entry points unresolved)";
        return;
    }
    // Precache through the manifest loader BEFORE the first create: creating a system CS2 has not
    // loaded as part of this map dies in particles.dll (see ParticleFxHook CRASH LESSON 5/6).
    if (!g_precached) {
        if (g_precacheFailed) {
            g_running.store(false);
            return;
        }
        const ULONGLONG t0 = GetTickCount64();
        g_precached = ParticleFx_PrecacheSystem(kRainSystem);
        MvmDebugLog_LinefAlways("environment.rain", "precache %s ok=%d ms=%llu", kRainSystem, g_precached ? 1 : 0,
                                GetTickCount64() - t0);
        if (!g_precached) {
            g_precacheFailed = true;
            g_status = "unavailable (rain system failed to precache)";
            g_running.store(false);
            return;
        }
    }
    // A demo seek rebuilds client state and can drop client particles; recreate after any jump.
    int tick = -1;
    if (g_MirvTime.GetCurrentDemoTick(tick) && g_lastTick >= 0 && std::abs(tick - g_lastTick) > 128 && g_index >= 0) {
        Destroy(true);
        MvmDebugLog_LinefAlways("environment.rain", "recreate after seek %d -> %d", g_lastTick, tick);
    }
    g_lastTick = tick;
    unsigned char *manager = nullptr;
    __try {
        manager = g_Original_getParticleManager();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        manager = nullptr;
    }
    if (!manager)
        return;
    if (g_index < 0) {
        if (GetTickCount64() < g_retryAtMs)
            return;
        if (!Create(manager, g_index)) {
            g_retryAtMs = GetTickCount64() + 2000;
            g_status = "create failed; retrying";
            g_running.store(false);
            MvmDebugLog_LinefAlways("environment.rain", "create failed");
            return;
        }
        MvmDebugLog_LinefAlways("environment.rain", "native rain started index=%d", g_index);
    }
    // CP0 anchors the system, CP1 is where the storm sphere spawns and locks, CP3.x is intensity.
    const float at[3] = {camera.x, camera.y, camera.z};
    const float intensity[3] = {NativeRainIntensity(scene.weather.count), 0, 0};
    if (!SetPoint(manager, g_index, 0, at) || !SetPoint(manager, g_index, 1, at) ||
        !SetPoint(manager, g_index, 3, intensity)) {
        g_index = -1;
        g_status = "control point write failed; recreating";
        g_running.store(false);
        return;
    }
    g_running.store(true);
    g_status = "native (rain_storm)";
}

void ResetNativeRain() {
    g_index = -1; // the old manager's index; do not destroy
    g_touched.clear(); // those worlds died with the level
    g_applied = NativeWorldWeather{};
    g_worldScanAtMs = 0;
    g_precached = false;
    g_precacheFailed = false;
    g_lastTick = -1;
    g_retryAtMs = 0;
    g_running.store(false);
    g_status = "off";
}

bool NativeRainActive() { return g_running.load(); }
const char *NativeRainStatus() { return g_status; }
} // namespace Filmmaker::Environment
