#include "EnvironmentNativeRain.h"
#include "../Debug/MvmDebugLog.h"
#include "../Movie/ParticleFx.h"
#include "../../MirvTime.h"
#include "../../ClientEntitySystem.h"
#include "../../Globals.h"
#include "../../SchemaSystem.h"
#include "../Cosmetics/CosmeticEntityAccess.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/icvar.h"
#include "../../../Deps/release/Detours/src/detours.h"
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
        // THE RAIN HAS STOPPED: everything it wetted dries. Handed back with every value at 0, the
        // drying amount included, the first-person gloves and gun kept the rain's wet look for the
        // rest of the level, in clear weather and in snow (reported 2026-11-17). Fully dried
        // instead, and wet again only while it rains.
        NativeWorldWeather dry;
        dry.drying = 1;
        for (const auto &t : g_touched) {
            CallExposed(t.world, t.wasExposed, nullptr);
            CallWeather(t.world, dry);
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
    // The game's wet shading belongs to rain: with no rain falling nothing of the game's is made
    // wet, whatever the Wetness control says (that control wets OUR ground film; it used to wet
    // the game's own materials too, rain or no rain). While it rains, how wet they get follows it.
    w.wetness = raining ? std::clamp(wet, 0.f, 1.f) * (1 - snow) : 0.f;
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

// ---- frost ------------------------------------------------------------------------------------
// CS2 frosts players, weapons and chickens itself: it grows outdoors, melts under cover, and a hot
// barrel or a lit molotov clears it. A map asks for it with info_map_parameters "frostenabled";
// r_csgo_frost_force overrides the map (-1 = the map's, 0 = off, 1 = on) and is what we set while
// snow falls or lies, so the people in a snowy scene are frosted by the game's own material.
// The cvar is development-only, so it is found by walking the registry.
namespace {
SOURCESDK::CS2::Cvar_s *g_frostCvar = nullptr;
size_t g_frostIndex = (size_t)-1;
bool g_frostResolved = false, g_frostHeld = false;
int g_frostOriginal = -1;
char g_frostStatus[128] = "frost: not resolved";

// HIDING WHAT IS LEFT. The game has no amount for its frost, only on and off. Each frosted thing
// keeps four numbers (frost now, the most it has had, meltwater, barrel heat) that one small routine
// in client.dll packs for the material; that routine is hooked and the first two are zeroed once our
// snow has stopped, so the frost players still carry is not drawn while it melts.
// ONE WEATHER ON THE HANDS. The same four numbers carry the wet a thing keeps after rain (the
// meltwater), so the first-person gloves and gun stayed wet for the rest of the level once it had
// rained, in clear weather and in snow (reported 2026-11-17). Frost is hidden while it rains and the
// wet whenever it does not (g_waterStrength). The routine is hooked whether or not this CS2 build
// still has r_csgo_frost_force (the November 2026 update removed it).
using FrostPack_t = void *(__fastcall *)(const float *state, float *out);
FrostPack_t g_frostPack = nullptr;
std::atomic<float> g_frostStrength{1.f}, g_waterStrength{1.f};
bool g_frostHooked = false;
void *__fastcall Hook_FrostPack(const float *state, float *out) {
    void *result = g_frostPack(state, out);
    if (!out)
        return result;
    const float strength = g_frostStrength.load(std::memory_order_relaxed);
    const float water = g_waterStrength.load(std::memory_order_relaxed);
    if (MvmDebugLog_Active() && state) {
        static ULONGLONG last = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - last > 2000) {
            last = now;
            MvmDebugLog_LinefAlways("environment.frost.pack", "state=(%.3f %.3f %.3f %.3f) out=(%.3f %.3f %.3f %.3f) frost=%.0f water=%.0f",
                                    state[0], state[1], state[2], state[3], out[0], out[1], out[2], out[3], strength, water);
        }
    }
    if (strength < 1.f) {
        out[0] *= strength;
        out[1] *= strength;
    }
    if (water < 1.f)
        out[2] *= water;
    return result;
}
void HookFrostStrength() {
    HMODULE client = GetModuleHandleA("client.dll");
    // push rbx; sub rsp,30h; movss xmm0,[rcx+0Ch]; mov rbx,rdx; movss xmm3,[rcx+8]; movss xmm2,[rcx+4];
    // movss xmm1,[rcx]; mov rcx,rdx; movss [rsp+20h],xmm0; call
    size_t at = client ? getAddress(client, "40 53 48 83 EC 30 F3 0F 10 41 0C 48 8B DA F3 0F 10 59 08 F3 0F 10 51 04 "
                                            "F3 0F 10 09 48 8B CA F3 0F 11 44 24 20 E8")
                       : 0;
    if (at) {
        g_frostPack = (FrostPack_t)at;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID &)g_frostPack, Hook_FrostPack);
        g_frostHooked = NO_ERROR == DetourTransactionCommit();
    }
    MvmDebugLog_LinefAlways("environment.frost", "frost strength hook %s at=%p", g_frostHooked ? "installed" : "unavailable", (void *)at);
}

void ResolveFrost() {
    if (g_frostResolved || !SOURCESDK::CS2::g_pCVar)
        return;
    g_frostResolved = true;
    for (SOURCESDK::uint32 i = 0; i < SOURCESDK_CS2_MAX_VALID_CVARS; ++i) {
        SOURCESDK::CS2::Cvar_s *cvar = SOURCESDK::CS2::g_pCVar->GetCvar(i);
        if (!cvar)
            break;
        if (!cvar->m_pszName || std::strcmp(cvar->m_pszName, "r_csgo_frost_force"))
            continue;
        if (cvar->m_eVarType == SOURCESDK::CS2::EConVarType_Int32) {
            g_frostCvar = cvar;
            g_frostIndex = i;
        }
        break;
    }
    std::snprintf(g_frostStatus, sizeof(g_frostStatus),
                  g_frostCvar ? "frost: off" : "frost: unavailable (this CS2 build has no r_csgo_frost_force)");
    MvmDebugLog_LinefAlways("environment.frost", "r_csgo_frost_force %s index=%zu", g_frostCvar ? "found" : "missing",
                            g_frostIndex);
    HookFrostStrength();
}

// What the game's frost switch is held at. kFrostReleased gives it back to the map.
constexpr int kFrostReleased = -2;
int g_frostHeldAt = kFrostReleased;
// We have frosted people on this level. From then until the level changes the game's switch stays
// OFF whenever our snow is not wanted, and the frost they carry is shown at zero strength: each of
// them keeps a stored amount that melts only while the switch says off, slowly, and handed back to
// the map (-1) the game stops working it out at all, so they turned white again a moment after a
// reset (seen after Reset weather and after the Environment tab's reset). A level that frosts people
// by itself is left alone, because this is only set once our own snow has run.
bool g_frostMade = false;
void ApplyFrost(int target, bool hide = false) {
    ResolveFrost();
    const bool on = target == 1;
    if (on) g_frostMade = true;
    // The game's own frost, as it draws it, while our snow wants it; none of it otherwise, and none
    // at all while our rain falls (`hide`).
    const float strength = !hide && (on || !g_frostMade) ? 1.f : 0.f;
    if (strength != g_frostStrength.load(std::memory_order_relaxed))
        g_frostStrength.store(strength, std::memory_order_relaxed);
    if (!g_frostCvar || target == g_frostHeldAt)
        return;
    SOURCESDK::CS2::CVValue_t oldValue = {}, newValue = {};
    oldValue.m_i32Value = g_frostCvar->m_Value.m_i32Value;
    if (g_frostHeldAt == kFrostReleased)
        g_frostOriginal = oldValue.m_i32Value;
    newValue.m_i32Value = target == kFrostReleased ? g_frostOriginal : target;
    g_frostHeldAt = target;
    g_frostHeld = on;
    if (newValue.m_i32Value != oldValue.m_i32Value) {
        g_frostCvar->m_Value.m_i32Value = newValue.m_i32Value;
        ++g_frostCvar->m_iTimesChanged;
        SOURCESDK::CS2::ConVarHandle handle;
        handle.Set(g_frostIndex);
        SOURCESDK::CS2::g_pCVar->CallChangeCallback(handle, 0, &newValue, &oldValue);
    }
    if (on)
        std::snprintf(g_frostStatus, sizeof(g_frostStatus), "frost: on (CS2's own, r_csgo_frost_force 1)");
    else if (target == 0)
        std::snprintf(g_frostStatus, sizeof(g_frostStatus), "frost: held off (r_csgo_frost_force 0%s)",
                      g_frostMade ? (g_frostHooked ? ", what players carry hidden" : ", the game's frost routine was not found") : "");
    else
        std::snprintf(g_frostStatus, sizeof(g_frostStatus), "frost: off");
    MvmDebugLog_LinefAlways("environment.frost", "r_csgo_frost_force %d -> %d", oldValue.m_i32Value, newValue.m_i32Value);
}
} // namespace

bool NativeFrostWanted(const Scene &scene, bool active) {
    if (!active)
        return false;
    // ONE WEATHER, ONE SHADING. Rain is the game's wet shading and nothing else: while it rains
    // nobody is frosted, whatever snow still lies (a scene with rain falling on old snow used to
    // frost its players, which read as the snow look in rain).
    if (scene.weather.type == kPrecipitationRain && scene.weather.count > 0)
        return false;
    const bool snowing = scene.weather.type == 2 && scene.weather.count > 0;
    return snowing || (scene.climate[CL_enabled] != 0 && scene.climate[CL_snow] > 0);
}
const char *NativeFrostStatus() { return g_frostStatus; }

void UpdateNativeRain(const Scene &scene, const std::string &map, bool active, Vec3 camera, bool cameraValid) {
    (void)map;
    ApplyWorldWeather(NativeWorldWeatherFor(scene, active));
    // The game's frost switch: on with our snow, held OFF in our rain (a map that asks for frost
    // itself must not frost people standing in our rain), and the map's own otherwise.
    const bool raining = active && scene.weather.type == kPrecipitationRain && scene.weather.count > 0;
    // NEVER HANDED BACK FROSTED. The game works its frost out only while the switch says on or off;
    // given back to the map (-1), players keep the frost they had. So once our snow has frosted
    // anyone on this level the switch stays off until the level changes (g_frostMade).
    // Hands, guns and players: frost only with our snow, never in our rain or our clear weather, and
    // the rain's wet only while it rains. Since the November 2026 update CS2 frosts people outdoors
    // by itself on any map, so frost came back on the gloves the moment the rain stopped. With the
    // Environment off the game's own frost and wet are left as they are.
    const bool frostWanted = NativeFrostWanted(scene, active);
    ApplyFrost(frostWanted ? 1 : (raining || g_frostMade) ? 0 : kFrostReleased, active && !frostWanted);
    g_waterStrength.store(!active || raining ? 1.f : 0.f, std::memory_order_relaxed);
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
    g_frostMade = false; // a new level: its own frost, if it has any, is its own again
}

bool NativeRainActive() { return g_running.load(); }
const char *NativeRainStatus() { return g_status; }
} // namespace Filmmaker::Environment
