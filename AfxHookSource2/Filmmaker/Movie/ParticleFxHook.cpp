// ParticleFx hook: SEH-guarded engine-memory access, CParticleSystemMgr vtable
// resolution + the create-collection detour (Hook_CreateBody), swap-handle resolution
// with its resource-system JIT-manifest redirect, and hook install. Every hard-won
// CRASH LESSON from the 2026-07-02 dump sessions lives in this file -- read the
// comments before touching anything.

#include "ParticleFxInternal.h"

#include "FxAlign.h"                       // Modern muzzle-FX alignment probe (fx align)
#include "PerWeaponFx.h"                   // per-weapon overrides for shared flash systems
#include "../Debug/MvmDebugLog.h" // MvmDebugLog_* (thread-safe flight recorder)
#include "../World/NightMode.h"            // NightMode_Exposure: auto vs pinned tonemap breadcrumb
#include "../../../Shared/AfxConsole.h"    // advancedfx::Message/Warning

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../../Deps/release/Detours/src/detours.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace Filmmaker {
namespace fx {

// ============================== engine binding =====================================

namespace {

// CParticleSystemMgr::FindParticleSystem (vtable +0x78). rdx is an out slot the engine
// writes the resolved handle into (it also returns a pointer whose pointee is the handle);
// r9b=1 mirrors what the engine's own create-by-name wrapper passes. This function also
// owns the "not precached -> blocking load" path, so resolving a never-yet-seen system by
// name self-heals (one-time hitch) instead of failing.
typedef void* (__fastcall* FindSystem_t)(void* self, void** out, const char* name, unsigned char byName);

// The create-by-handle vtable STUB (+0x88): a standard vtable method (rcx = manager
// `this`, discarded by its own `xor ecx,ecx`; rdx = particle-system strong handle). We
// detour THIS, never the shared body behind it -- see the crash-lesson comment at the
// install site. Args are forwarded as RAW 8-byte slots with headroom (12): typed
// narrowing (u8/float/int) truncates stack slots, and static RE undercounted the arg
// count -- both crashed in-demo (2026-07-02 dumps).
typedef void* (__fastcall* CreateBody_t)(unsigned long long thisOrFlag, void* handle, void* a3, void* a4,
	unsigned long long a5, unsigned long long a6, unsigned long long a7, unsigned long long a8,
	unsigned long long a9, unsigned long long a10, unsigned long long a11, unsigned long long a12);

void* g_mgr = nullptr;                 // ParticleSystemMgr003 interface pointer
FindSystem_t g_find = nullptr;
CreateBody_t g_origCreateBody = nullptr;

// CResourceSystem ("ResourceSystem013") blocking-load pair. Both take the SAME args
// (this, resourceId*, const char* who). vtable +0x140 is the naked single-resource load
// FindParticleSystem falls back to for a non-precached system; +0x148 is
// CResourceSystem::BlockingLoadResourceByNameIntoJustInTimeManifest, which wraps the load
// in a one-resource MANIFEST -- the same pipeline map precache uses, so dependent
// resources load and the definition's strong-handle fields get fixed up. See the
// CRASH LESSON 6 comment on HandleCreateReady for why the difference is fatal.
typedef void* (__fastcall* ResBlockingLoad_t)(void* self, void* resourceId, const char* who);
ResBlockingLoad_t g_origPlainLoad = nullptr;   // detoured: vt+0x140
ResBlockingLoad_t g_jitManifestLoad = nullptr; // called instead while t_jitRedirect: vt+0x148
bool g_jitInstallTried = false;
// Set ONLY around the resolver's FindParticleSystem call (main thread). The detour is a
// strict pass-through for every other caller in the process.
thread_local bool t_jitRedirect = false;

// Six of ten impact sequence slots keep Insurgency's flying smoke ribbons. Povarhok's
// local impact puffs are a separate effect and stay present on every Povarhok selection. A
// counter is deterministic across demo replays and avoids RNG in the render/create path.
std::atomic<unsigned int> g_impactSequence{ 0 };

} // namespace

std::atomic<bool> g_installed{ false };
bool g_installFailedHard = false;      // shape mismatch: stop retrying, feature inactive
unsigned long long g_lastInstallTryMs = 0;
bool g_jitRedirectInstalled = false;
std::unordered_map<std::string, HandleCacheEntry> g_handleCache; // guarded by g_mx
std::vector<std::string> g_activeTargets; // guarded by g_mx
// Swap-target names waiting for MAIN-THREAD resolution (see ResolveHandleOnMainThread).
std::vector<std::string> g_resolveQueue;

// ============================== guarded memory access ==============================
// The hook dereferences engine-owned structures on whatever thread creates particles.
// Every read is SEH-guarded so a layout change after a CS2 update degrades to "no name ->
// pass through" instead of crashing the game. (Separate functions: MSVC forbids __try in
// functions with C++ objects needing unwinding.)

namespace {

bool SehReadPtr(const void* addr, void** out) {
	__try {
		*out = *(void* const*)addr;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool SehCopyStr(const char* src, char* dst, size_t cap) {
	__try {
		size_t i = 0;
		for (; i + 1 < cap; ++i) {
			char c = src[i];
			dst[i] = c;
			if (!c)
				return true;
		}
		dst[cap - 1] = 0;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool SehReadDword(const void* addr, int* out) {
	__try {
		*out = *(const int*)addr;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool SehReadBytes(const void* addr, unsigned char* dst, size_t n) {
	__try {
		std::memcpy(dst, addr, n);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool InImage(unsigned char* base, size_t size, const void* p) {
	return p >= base && p < base + size;
}

// A strong handle is CREATE-READY only when the engine's own create-by-name wrapper says
// so (particles.dll 0xA0E78..0xA0E8B, build 14166): data ptr [handle] non-null AND the
// live counter dword [handle+0x20] > 0. CRASH LESSON 5 (2026-07-02, dump 09:23 + the
// impact_concrete->impact_plaster control test): a handle that only passes the null check
// is a header-loaded definition whose CHILD systems were never resolved -- creating from
// it dies at particles.dll+0x3E3xx walking null children. Precached systems pass both.
//
// CRASH LESSON 6 (2026-07-02, mvm_debug_093425: first tracer swap AV'd reading
// 0xFFFFFFFFFFFFFFFF at particles.dll+0x3e36a): the two checks above are NOT sufficient.
// Every top-level create unconditionally walks the definition's fallback strong-handle
// fields at [def+0x308] and [def+0x310] (particles.dll+0x3e33b/+0x3e359, build 14166).
// A definition loaded via the naked blocking load has those fields left UNFIXED (-1),
// and the walk derefs the -1. A properly (manifest-)loaded definition holds 0 or a valid
// binding pointer there. So: reject any definition still carrying a -1 fallback field.
bool HandleCreateReady(void* handle) {
	if (!handle)
		return false;
	void* data = nullptr;
	if (!SehReadPtr(handle, &data) || !data)
		return false;
	int live = 0;
	if (!SehReadDword((unsigned char*)handle + 0x20, &live) || live <= 0)
		return false;
	void* fb = nullptr;
	if (!SehReadPtr((unsigned char*)data + 0x308, &fb) || fb == (void*)~0ull)
		return false;
	if (!SehReadPtr((unsigned char*)data + 0x310, &fb) || fb == (void*)~0ull)
		return false;
	return true;
}

// Resource name off a particle-system strong handle: [handle+8] -> info, [info] -> char*.
// (Matches the engine's own "Attempting to use invalid particle definition \"%s\"" print.)
bool HandleName(void* handle, char* buf, size_t cap) {
	if (!handle)
		return false;
	void* info = nullptr;
	if (!SehReadPtr((unsigned char*)handle + 8, &info) || !info)
		return false;
	void* str = nullptr;
	if (!SehReadPtr(info, &str) || !str)
		return false;
	if (!SehCopyStr((const char*)str, buf, cap))
		return false;
	return buf[0] != 0;
}

} // namespace

// ============================== swap-handle resolution =============================

// CRASH LESSON (2026-07-02, two access-violation dumps): swap-target names must NEVER be
// resolved from inside the create-collection hook. FindParticleSystem's "not precached ->
// blocking load" path re-enters the resource/particle system mid-create on whatever thread
// is creating particles; that survived for the tiny always-resident dev/empty.vpcf but
// crashed the first time a "More" target (a real, unloaded system) had to load. So the
// hook is CACHE-HIT-ONLY: a miss fails open (original effect plays
// once) and queues the name; the main-thread pump resolves it in the engine's own precache
// context, and the next creation swaps.

namespace {

// The detour on CResourceSystem's plain blocking load (vt+0x140). While the resolver's
// FindParticleSystem call is on this thread's stack, the load is upgraded to the
// JIT-manifest load (vt+0x148, identical signature) so the particle definition arrives
// with its dependencies loaded and its resource-handle fields fixed up instead of -1.
// Every other caller (models, materials, other threads) passes through untouched.
void* __fastcall Hook_PlainBlockingLoad(void* self, void* resourceId, const char* who) {
	if (t_jitRedirect && g_jitManifestLoad) {
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("fx.resolve", "plain blocking load redirected to JIT manifest load (%s)",
				who ? who : "?");
		return g_jitManifestLoad(self, resourceId, who);
	}
	return g_origPlainLoad(self, resourceId, who);
}

// Canary: does fn's body (first scanLen bytes) contain a rip-relative lea to a string
// starting with prefix? Confirms a vtable slot still holds the function we RE'd after a
// CS2 update reshuffles things. SEH-guarded byte reads throughout.
bool FnReferencesString(const void* fn, size_t scanLen, const char* prefix) {
	unsigned char buf[0x800];
	if (scanLen > sizeof(buf))
		scanLen = sizeof(buf);
	if (!SehReadBytes(fn, buf, scanLen))
		return false;
	const size_t prefLen = std::strlen(prefix);
	for (size_t i = 0; i + 7 <= scanLen; ++i) {
		// 48/49/4C/4D 8D modrm(mod00 rm101): lea r64, [rip+disp32]
		if ((buf[i] & 0xF8) != 0x48 || buf[i + 1] != 0x8D || (buf[i + 2] & 0xC7) != 0x05)
			continue;
		int disp = 0;
		std::memcpy(&disp, buf + i + 3, 4);
		const char* s = (const char*)fn + i + 7 + disp;
		char str[96];
		if (SehCopyStr(s, str, sizeof(str)) && 0 == std::strncmp(str, prefix, prefLen))
			return true;
	}
	return false;
}

// Install the plain->JIT blocking-load redirect on CResourceSystem. Failure is NON-fatal:
// the feature degrades to "swap targets must be precached by the demo" (the
// HandleCreateReady gate keeps that safe), it does not crash or disable the hook.
void TryInstallJitRedirect() {
	if (g_jitInstallTried)
		return;
	g_jitInstallTried = true;

	HMODULE mod = GetModuleHandleA("resourcesystem.dll");
	if (!mod) {
		advancedfx::Warning("fx: resourcesystem.dll not loaded; swap targets limited to demo-precached systems.\n");
		return;
	}
	unsigned char* base = (unsigned char*)mod;
	const int ntOff = *(const int*)(base + 0x3C);
	const size_t imageSize = *(const unsigned int*)(base + ntOff + 0x50);

	typedef void* (*CreateInterface_t)(const char*, int*);
	CreateInterface_t ci = (CreateInterface_t)GetProcAddress(mod, "CreateInterface");
	void* rs = ci ? ci("ResourceSystem013", nullptr) : nullptr;
	void** vt = nullptr;
	if (!rs || !SehReadPtr(rs, (void**)&vt) || !InImage(base, imageSize, vt)) {
		advancedfx::Warning("fx: ResourceSystem013 not resolvable; swap targets limited to demo-precached systems.\n");
		return;
	}
	void* plain = nullptr; void* jit = nullptr;
	SehReadPtr(vt + 0x140 / 8, &plain); // naked blocking load (FindParticleSystem's fallback)
	SehReadPtr(vt + 0x148 / 8, &jit);   // BlockingLoadResourceByNameIntoJustInTimeManifest
	if (!InImage(base, imageSize, plain) || !InImage(base, imageSize, jit)
		|| !FnReferencesString(jit, 0x600, "CResourceSystem::BlockingLoadResourceByName")) {
		advancedfx::Warning("fx: resource-system blocking-load slots moved (CS2 update?); "
			"swap targets limited to demo-precached systems.\n");
		return;
	}

	g_origPlainLoad = (ResBlockingLoad_t)plain;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_origPlainLoad, Hook_PlainBlockingLoad);
	if (NO_ERROR != DetourTransactionCommit()) {
		g_origPlainLoad = nullptr;
		advancedfx::Warning("fx: blocking-load detour failed; swap targets limited to demo-precached systems.\n");
		return;
	}
	g_jitManifestLoad = (ResBlockingLoad_t)jit;
	g_jitRedirectInstalled = true;
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("fx.install", "jit redirect armed: rs=%p plain=%p jit=%p", rs, plain, jit);
}

} // namespace

// MAIN THREAD ONLY. Resolves via the manager's FindParticleSystem (owns the blocking-load
// fallback, safe here). Failed resolves are cached for 5s; success is cached until the
// resource context is torn down (g_levelEpoch), NOT for the process lifetime -- see
// CRASH LESSON 7 below.
void ResolveHandleOnMainThread(const char* name) {
	const unsigned epoch = g_levelEpoch.load(std::memory_order_acquire);
	{
		std::lock_guard<std::mutex> lock(g_mx);
		if (std::find(g_activeTargets.begin(), g_activeTargets.end(), name) == g_activeTargets.end())
			return;
		auto it = g_handleCache.find(name);
		if (it != g_handleCache.end() && it->second.epoch == epoch) {
			if (it->second.handle)
				return;
			if (GetTickCount64() < it->second.retryAtMs)
				return;
		}
	}
	void* h = nullptr;
	int live = -1;
	void* fb308 = nullptr; void* fb310 = nullptr;
	if (g_find && g_mgr) {
		void* out[4] = {};
		// Upgrade any blocking load this triggers to the JIT-manifest load (dependencies
		// + handle fixup) -- see Hook_PlainBlockingLoad. Without it, a non-precached
		// system loads with -1 fallback fields and creating from it crashes the engine.
		t_jitRedirect = true;
		void* r = g_find(g_mgr, out, name, 1);
		t_jitRedirect = false;
		if (r)
			SehReadPtr(r, &h);
		if (!h)
			h = out[0];
		if (h) {
			SehReadDword((unsigned char*)h + 0x20, &live);
			void* data = nullptr;
			if (SehReadPtr(h, &data) && data) {
				SehReadPtr((unsigned char*)data + 0x308, &fb308);
				SehReadPtr((unsigned char*)data + 0x310, &fb310);
			}
		}
		// Full create-ready gate (null data / live<=0 / -1 fallback fields): a handle
		// that is not fully loaded+fixed-up must NOT be cached as usable.
		if (h && !HandleCreateReady(h))
			h = nullptr;
	}
	// The blocking load above can span a level teardown (it pumps the resource system).
	// A handle resolved in the OLD context must not be cached under the new one.
	if (g_levelEpoch.load(std::memory_order_acquire) != epoch) {
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("fx.level", "resolve of %s spanned a level change; result discarded", name);
		return;
	}
	char identity[260] = {};
	if (h && !HandleName(h, identity, sizeof(identity)))
		identity[0] = '\0';
	{
		std::lock_guard<std::mutex> lock(g_mx);
		HandleCacheEntry& e = g_handleCache[name];
		e.handle = h;
		e.epoch = epoch;
		e.identity = identity;
		// Failed/not-ready resolves retry (the system may finish loading as the demo
		// streams in); ready handles are refreshed only on level change.
		e.retryAtMs = h ? 0 : GetTickCount64() + 5000;
	}
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("fx.resolve", "%s -> %p live=%d fb=%p/%p jit=%d epoch=%u %s", name, h, live,
			fb308, fb310, g_jitRedirectInstalled ? 1 : 0, epoch, h ? "READY" : "not-ready");
}

// Public precache (declared in ParticleFx.h): same manifest-redirected find the resolver uses,
// without the swap-target gate, and only true once the handle is genuinely create-ready.
bool PrecacheSystemNow(const char* name) {
	if (!name || !*name || !TryInstall() || !g_find || !g_mgr)
		return false;
	void* out[4] = {};
	t_jitRedirect = true;
	void* r = g_find(g_mgr, out, name, 1);
	t_jitRedirect = false;
	void* h = nullptr;
	if (r)
		SehReadPtr(r, &h);
	if (!h)
		h = out[0];
	return HandleCreateReady(h);
}

namespace {

// CRASH LESSON 7 (2026-08-02, mvm_debug_143443: AV reading a non-canonical address at
// particles.dll+0x35e010, 34s after the demo's round-boundary jump-back). That jump-back
// was not a seek -- the engine RELOADED the level inside the demo (its frame counter reset
// from 1164073 to 0), which frees every loaded particle definition. The handle cache was
// keyed on the DEMO PATH, which does not change across an in-demo reload, so the pre-reload
// handles stayed cached; the first post-reload swap stored one into the new collection and
// the engine dereferenced the freed definition (+0x33f9ac null-checks the pointer, then
// reads through it -- non-null but freed passes that check). Two independent gates now stop
// that: a resource-context EPOCH stamped on every cached handle, and a re-read of the
// resource name off the handle at use time. The epoch is the real fix (it does not depend
// on the freed block having been reused); the identity check is the net under it.
bool CacheEntryUsable(const HandleCacheEntry& e, unsigned epoch, const char** why) {
	if (e.epoch != epoch) {
		*why = "stale epoch";
		return false;
	}
	char now[260];
	if (!HandleName(e.handle, now, sizeof(now))) {
		*why = "name unreadable";
		return false;
	}
	if (e.identity != now) {
		*why = "identity changed";
		return false;
	}
	return true;
}

// Any thread. Cache hit or nothing: on a miss (or an expired failed resolve) the name is
// queued for the main-thread pump and null is returned (caller fails open).
void* LookupHandleOrQueue(const char* name) {
	const unsigned epoch = g_levelEpoch.load(std::memory_order_acquire);
	std::lock_guard<std::mutex> lock(g_mx);
	if (std::find(g_activeTargets.begin(), g_activeTargets.end(), name) == g_activeTargets.end())
		return nullptr;
	auto it = g_handleCache.find(name);
	if (it != g_handleCache.end()) {
		if (it->second.handle) {
			const char* why = nullptr;
			if (CacheEntryUsable(it->second, epoch, &why))
				return it->second.handle;
			if (MvmDebugLog_Active())
				MvmDebugLog_Linef("fx.stale", "rejected cached handle for %s (%s; entryEpoch=%u now=%u)",
					name, why, it->second.epoch, epoch);
			g_handleCache.erase(it); // fall through: fail open once, requeue for re-resolve
		} else if (GetTickCount64() < it->second.retryAtMs) {
			return nullptr; // failed resolve cooling down
		} else {
			g_handleCache.erase(it); // cooldown over: fall through and requeue
		}
	}
	for (const std::string& q : g_resolveQueue)
		if (q == name)
			return nullptr;
	g_resolveQueue.push_back(name);
	return nullptr;
}

// ============================== the hook ===========================================

void* __fastcall Hook_CreateBody(unsigned long long thisOrFlag, void* handle, void* a3, void* a4,
	unsigned long long a5, unsigned long long a6, unsigned long long a7, unsigned long long a8,
	unsigned long long a9, unsigned long long a10, unsigned long long a11, unsigned long long a12) {
	void* useHandle = handle;
	// Alignment probe capture (fx align): filled for swapped creations so the note after
	// the original create can hand FxAlign the engine's returned collection pointer.
	bool alignNote = false;
	char alignLow[260] = {};
	std::string alignTarget;
	char raw[260];
	if (handle && HandleName(handle, raw, sizeof(raw))) {
		g_totalSeen.fetch_add(1, std::memory_order_relaxed);
		char low[260];
		LowerCopy(raw, low, sizeof(low));

		char action = '=';
		std::string target;
		bool logging;
		{
			std::lock_guard<std::mutex> lock(g_mx);
			logging = g_logging;
			// Camera detached from the spectated eye (third person / free cam): the engine
			// still spawns the FIRST-PERSON viewmodel weapon FX anchored to the camera, so
			// flashes/smoke float in mid-air where the camera's "gun" would be. Block the
			// _fp/_fps systems entirely while detached. Checked BEFORE (and independent of)
			// g_enabled -- this is a camera-correctness gate, not an EFFECTS mode.
			if (g_fpFxSuppress.load(std::memory_order_relaxed) && IsFirstPersonWeaponFxPath(low))
				action = 'X';
			// UwU: no blood anywhere (sprays, headshot bursts, pools, blood decals).
			if (action == '=' && g_bloodSuppress.load(std::memory_order_relaxed) && std::strstr(low, "blood")) {
				action = 'X';
				g_bloodBlocked.fetch_add(1, std::memory_order_relaxed);
			}
			// No effects at the main menu: a hook left armed by the last demo passes everything.
			if (action == '=' && g_enabled && g_sceneActive.load(std::memory_order_relaxed)) {
				// Custom rules outrank category modes (they're the power-user override).
				for (const CustomRule& r : g_customRules) {
					if (std::strstr(low, r.match.c_str())) {
						action = r.target.empty() ? 'X' : '>';
						target = r.target;
						break;
					}
				}
				// No event gate: these systems only spawn on real headshot hits (see the
				// ParticleFxMoney.cpp file comment for why the old event window missed kills).
				if (action == '=' && g_moneyHeadshot && IsMoneyHeadshotCandidateLower(low)) {
					action = '>';
					target = kMoneyBurst;
					// This head is covered: the headshot event in this tick spawns nothing more.
					g_helmetMoneyTick.store(g_demoTickNow.load(std::memory_order_relaxed), std::memory_order_relaxed);
				}
				if (action == '=') {
					const int cat = ClassifyLower(low);
					if (cat >= 0) {
						const FxMode m = g_modes[cat];
						if (m != FxMode::Off) {
							if (const char* t = VariantTargetLower(cat, m, low)) {
								action = '>';
								target = t;
								if (cat == kFxImpacts) {
									const unsigned int sequence = g_impactSequence.fetch_add(1, std::memory_order_relaxed);
									if (((sequence * 0x9e3779b9u) >> 31) != 0) {
										if (const char* hybrid = ImpactHybridTargetLower(low))
											target = hybrid;
									}
									// Only Insurgency owns the excessive flying debris ribbons.
									// Applying this closure to Povarhok removed its intended puff.
									if (target.find("/impact_fx_ins/") != std::string::npos
										&& (sequence % 10u) >= kInsurgencySmokeTrailKeepPerTen)
										target = ImpactNoSmokeTrailTarget(target.c_str());
									// Play CS2's own impact for THIS surface underneath ours: retarget
									// to the per-surface wrapper (ImpactSurfaceLayeredTarget). The
									// hybrid / no-trails choice above is already made, so the wrapper
									// wraps exactly the system the randomiser picked.
									const std::string layered = ImpactSurfaceLayeredTarget(target.c_str(), low);
									if (!layered.empty())
										target = layered;
								}
								// Same CS2 name, different weapon: redirect to the per-weapon fork
								// when one is registered (see PerWeaponFx.h).
								if (const char* pw = PerWeaponFx_Override(t))
									target = pw;
								if (DbgIsBarrelSmokePath(low) || DbgIsBarrelSmokePath(t)) {
									// #region agent log
									DbgAgentLog("H4", "ParticleFxHook.cpp:Hook_CreateBody",
										"smoke-related swap",
										low, t, target.c_str(), g_demoTickNow.load(std::memory_order_relaxed));
									// #endregion
								}
							}
						}
					}
				}
			}
		}

		if (action == 'X')
			target = kEmptySystem;
		if (action != '=') {
			// Cache-hit-only: resolving here can trigger a blocking load inside the
			// create path (crashes -- see ResolveHandleOnMainThread). A miss queues the
			// name for the main-thread pump and this one creation plays unmodified.
			void* nh = LookupHandleOrQueue(target.c_str());
			// Last-line probe: the engine's full create-ready gate, re-checked at use time.
			if (nh && !HandleCreateReady(nh))
				nh = nullptr;
            // Until the complete layered wrapper is ready, preserve the original CS2
            // impact. A donor-only fallback would replace the native layer instead.
            if(!nh && target.find("/impact_layered/")!=std::string::npos && MvmDebugLog_Active())
                MvmDebugLog_Linef("fx.impact.wait","raw=%s target=%s fallback=vanilla reason=wrapper-not-ready",
                    low,target.c_str());
			if (nh && nh != handle) {
				useHandle = nh;
				if (action == 'X') g_totalBlocked.fetch_add(1, std::memory_order_relaxed);
				else g_totalSwapped.fetch_add(1, std::memory_order_relaxed);
			} else {
				action = '='; // fail-open: original effect plays
			}
		}

		// Indoor black sprite rectangles are only visible when an additive flash/smoke
		// texture leaks RGB through fully transparent texels and the map's auto exposure
		// lifts that leaked colour. Record the exact swapped target and exposure policy
		// for each muzzle-related creation (throttled by the debug logger's dedupe) so a
		// capture can prove the rendered asset path rather than guessing from a screenshot.
		if (action == '>' && MvmDebugLog_Active()
			&& (std::strstr(low, "muzzle") || std::strstr(target.c_str(), "muzzle"))) {
			const float exposure = NightMode_Exposure();
			MvmDebugLog_Linef("fx.exposure",
				"raw=%s target=%s exposure=%s%.3f weapon=%s",
				low, target.c_str(), exposure == kExposureAuto ? "auto/" : "pinned/",
				exposure, PerWeaponFx_ActiveWeaponClass());
		}
		// Impact tuning lives inside the authored child closure, below this top-level swap.
		// Record the selected root so captures prove On/Less routing and rule out a fail-open
		// to Valve's original effect while diagnosing count or motion complaints.
        const bool layeredImpact=target.find("/impact_layered/")!=std::string::npos;
        const bool insurgencyImpact = target.find("/impact_fx_ins/") != std::string::npos
            || target.find("/insurgency/impact_layered/") != std::string::npos;
		const bool povarehokImpact = target.find("/povarehok/less/impacts/") != std::string::npos;
		if (action == '>' && MvmDebugLog_Active() && (insurgencyImpact || povarehokImpact)) {
			MvmDebugLog_Linef("fx.impact",
                "raw=%s target=%s donor=%s vanillaLayer=%d smokePolicy=%s trailKeepPct=%u profile=%s",
                low, target.c_str(), povarehokImpact ? "povarehok" : "insurgency", (int)layeredImpact,
                povarehokImpact ? "impact-puff-preserved"
                    : (target.find("_nosmoketrails") == std::string::npos
                        ? "flying-trails" : "flying-trails-stripped"),
                kInsurgencySmokeTrailKeepPerTen*10u,
				povarehokImpact ? "legacy-less/count.50/bounce0/spinStop.5"
					: "generated-less/count.70+indoorTune");
		}

		// #region FxDebugHud squares (spectated FP player only -- _fps paths + weapon_fire window)
		if (action != '=' && g_debugHudEnabled.load(std::memory_order_relaxed)) {
			const int tick = g_demoTickNow.load(std::memory_order_relaxed);
			const bool watchedShot = ShouldUpdateFxDebugHud(low, tick, action);
			if (watchedShot) {
				const unsigned long long nowMs = GetTickCount64();
				const int cat = ClassifyLower(low);
				if (cat == kFxTracers) {
					g_dbgTracerMs.store(nowMs, std::memory_order_relaxed);
					g_dbgTracerN.fetch_add(1, std::memory_order_relaxed);
				} else if (cat == kFxWeaponFx && std::strstr(low, "muzzle") != nullptr) {
					g_dbgMuzzleMs.store(nowMs, std::memory_order_relaxed);
					g_dbgMuzzleN.fetch_add(1, std::memory_order_relaxed);
				}

				const bool isModern = target.find("/modern/") != std::string::npos;
				const bool isPvrh = target.find("/insurgency/") != std::string::npos;
				const bool isSmoke = target.find("muzzle_smoke") != std::string::npos
					|| target.find("barrel_smoke") != std::string::npos;
				if (isPvrh && isSmoke) {
					g_dbgOnSmokeMs.store(nowMs, std::memory_order_relaxed);
					g_dbgOnSmokeN.fetch_add(1, std::memory_order_relaxed);
				}
				if (isModern && isSmoke) {
					g_dbgModSmokeMs.store(nowMs, std::memory_order_relaxed);
					g_dbgModSmokeN.fetch_add(1, std::memory_order_relaxed);
				}
				// #region agent log
				DbgWatchLog("H-watch-hud", "debug square lit for watched player shot",
					g_watchedUserId.load(std::memory_order_relaxed),
					g_watchedUserId.load(std::memory_order_relaxed), tick, raw);
				// #endregion
			} else if (ClassifyLower(low) == kFxWeaponFx || ClassifyLower(low) == kFxTracers) {
				// #region agent log
				DbgWatchLog("H-hud-reject", "weapon fx swap rejected by debug hud gate",
					g_watchedUserId.load(std::memory_order_relaxed),
					g_watchedFireDemoTick.load(std::memory_order_relaxed), tick, raw);
				// #endregion
			}
		}
		// #endregion

		// #region agent log (session 7803fe: Modern barrel FX alignment per weapon)
		if (action != '=' && target.find("/modern/") != std::string::npos) {
			const char* wclass = DbgModernAlignClass(target);
			const bool isSmoke = target.find("barrel_smoke_plume") != std::string::npos
				|| target.find("barrel_smoke.vpcf") != std::string::npos;
			const bool isFlash = target.find("muzzleflash_") != std::string::npos;
			if (wclass && (isSmoke || isFlash)) {
				const char* effect = isSmoke ? "barrelsmoke" : "muzzleflash";
				char msg[96];
				snprintf(msg, sizeof(msg), "modern %s %s swap (vpcf offset 0.5,0,0)", wclass, effect);
				DbgWatchLog("H-barrel-align", msg,
					g_watchedUserId.load(std::memory_order_relaxed),
					g_watchedUserId.load(std::memory_order_relaxed),
					g_demoTickNow.load(std::memory_order_relaxed), target.c_str());
			}
		}
		// #endregion

		// #region agent log (session 7803fe)
		if (DbgIsFlickerRelevant(low)) {
			const bool isComposition = target.find("mvm_muzzleflash_sniper") != std::string::npos
				|| target.find("mvm_grenade_trail") != std::string::npos;
			const unsigned long long nowMs = GetTickCount64();
			unsigned long long lastMs = 0;
			bool isDuplicate = false;
			{
				std::lock_guard<std::mutex> lock(g_dbgFlickerMx);
				auto it = g_dbgLastCreateMs.find(low);
				if (it != g_dbgLastCreateMs.end()) {
					lastMs = it->second;
					isDuplicate = (nowMs - lastMs) < 20;
				}
				g_dbgLastCreateMs[low] = nowMs;
			}
			DbgFlickerLog(isDuplicate ? "A-dup" : (isComposition ? "B-comp" : "C-normal"),
				isDuplicate ? "possible duplicate create (same name <20ms apart)"
					: (isComposition ? "composition target created" : "muzzle/smoke/tracer create"),
				raw, action == '=' ? "(pass)" : target.c_str(), isComposition, isDuplicate,
				(long long)(lastMs ? (nowMs - lastMs) : -1),
				g_demoTickNow.load(std::memory_order_relaxed));
		}
		// #endregion

		// Alignment probe capture: only real swaps ('>', post fail-open), and only while
		// the probe is armed (atomic check keeps the normal path free of string copies).
		if (action == '>' && FxAlign_Enabled()) {
			alignNote = true;
			std::memcpy(alignLow, low, sizeof(alignLow));
			alignTarget = target;
		}

		{
			std::lock_guard<std::mutex> lock(g_mx);
			RecordNameLocked(low, action != '=');
			if (logging || action != '=')
				RecordEventLocked(raw, action, target.c_str());
		}
		// Flight recorder (thread-safe; same pattern as the knife animfix breadcrumbs).
		if (MvmDebugLog_Active() && (logging || action != '=')) {
			if (action == '=')
				MvmDebugLog_Linef("fx.create", "= %s", raw);
			else
				MvmDebugLog_Linef("fx.create", "%c %s -> %s", action, raw, target.c_str());
		}
	} else if (handle) {
		g_totalNoName.fetch_add(1, std::memory_order_relaxed);
	}
	void* inst = g_origCreateBody(thisOrFlag, useHandle, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
	// Alignment probe (fx align): hand FxAlign the engine's returned collection so the
	// main-thread pump can measure spawn-vs-muzzle distance for this creation.
	if (alignNote)
		FxAlign_OnSwapCreate(alignLow, alignTarget.c_str(), inst,
			g_demoTickNow.load(std::memory_order_relaxed));
	return inst;
}

// ============================== install ============================================

void WarnOnceInstallFailed(const char* why) {
	g_installFailedHard = true;
	advancedfx::Warning("fx: particle hook NOT installed (%s). Effects play unmodified; "
		"a CS2 update likely changed CParticleSystemMgr -- the toggles need a mod update.\n", why);
}

} // namespace

bool TryInstall() {
	if (g_installed.load())
		return true;
	if (g_installFailedHard)
		return false;

	HMODULE mod = GetModuleHandleA("particles.dll");
	if (!mod)
		return false; // not loaded yet; pump retries later
	unsigned char* base = (unsigned char*)mod;
	// Image size straight from the PE optional header (no psapi dependency).
	const int ntOff = *(const int*)(base + 0x3C);
	const size_t imageSize = *(const unsigned int*)(base + ntOff + 0x50);

	typedef void* (*CreateInterface_t)(const char*, int*);
	CreateInterface_t ci = (CreateInterface_t)GetProcAddress(mod, "CreateInterface");
	if (!ci) { WarnOnceInstallFailed("no CreateInterface export"); return false; }
	void* mgr = ci("ParticleSystemMgr003", nullptr);
	if (!mgr) { WarnOnceInstallFailed("ParticleSystemMgr003 interface not found"); return false; }

	void** vt = nullptr;
	if (!SehReadPtr(mgr, (void**)&vt) || !InImage(base, imageSize, vt)) {
		WarnOnceInstallFailed("manager vtable outside particles.dll");
		return false;
	}
	void* find = nullptr; void* stub = nullptr; void* nameCreate = nullptr;
	SehReadPtr(vt + 15, &find);      // FindParticleSystem
	SehReadPtr(vt + 17, &stub);      // create-by-handle entry stub
	SehReadPtr(vt + 18, &nameCreate);// create-by-name (unused; validated as a slot-order canary)
	if (!InImage(base, imageSize, find) || !InImage(base, imageSize, stub)
		|| !InImage(base, imageSize, nameCreate)) {
		WarnOnceInstallFailed("vtable slots out of image (slot order changed?)");
		return false;
	}

	// The +0x88 slot is a tiny stub: `xor ecx,ecx` then a tail-jmp into the shared body.
	// Detour the STUB, NOT the body. CRASH LESSON 3 (2026-07-02, dumps 084146..090524):
	// detouring the body crashed in-demo even in pure pass-through (swapped=0) -- the
	// body's INTERNAL callers (child-system creation, i.e. every composite combat effect)
	// use an LTCG custom register convention a C detour cannot preserve, so children were
	// created with trashed volatile-register state and the child-walk loop died at
	// particles.dll+0x3E3xx. The stub is a real vtable method with a guaranteed standard
	// convention, and the internal child path bypassing us is FINE: we only ever classify
	// top-level systems (children of a swapped/blocked parent never spawn anyway).
	// The stub-shape canary below just confirms the slot still looks right after updates.
	unsigned char sb[0x20] = {};
	void* hookTarget = stub;
	if (!SehReadBytes(stub, sb, sizeof(sb)) || !((sb[0] == 0x33 || sb[0] == 0x31) && sb[1] == 0xC9)) {
		WarnOnceInstallFailed("create stub shape unrecognized (not xor ecx,ecx)");
		return false;
	}

	g_origCreateBody = (CreateBody_t)hookTarget;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_origCreateBody, Hook_CreateBody);
	if (NO_ERROR != DetourTransactionCommit()) {
		g_origCreateBody = nullptr;
		WarnOnceInstallFailed("DetourTransactionCommit failed");
		return false;
	}
	g_mgr = mgr;
	g_find = (FindSystem_t)find;
	TryInstallJitRedirect();  // upgrade resolver blocking loads to manifest loads
	RebuildActiveSwapTargets(false); // queue only currently enabled targets
	g_installed.store(true);
	advancedfx::Message("fx: particle hook installed (create-collection %s detoured).\n",
		hookTarget == stub ? "stub" : "body");
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("fx.install", "mgr=%p vt=%p find=%p stub=%p target=%p",
			mgr, (void*)vt, find, stub, hookTarget);
	return true;
}

} // namespace fx

bool ParticleFx_PrecacheSystem(const char* name) { return fx::PrecacheSystemNow(name); }
} // namespace Filmmaker
