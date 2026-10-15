#include "RenderEngineSize.h"

#include "../Debug/MvmDebugLog.h"

#include "../../../Shared/AfxConsole.h"
#include "../../../Shared/AfxDetours.h"

#include <atomic>
#include <array>
#include <cstdio>
#include <cstring>
#include <intrin.h> // _ReturnAddress (caller discrimination)
#include <mutex>
#include <sstream>
#include <windows.h>
#include "../../../Deps/release/Detours/src/detours.h"

namespace Filmmaker {

namespace {

// --- version-coupled constants (client.dll, CS2 build of 2026-08-01) ---
// Ghidra image base was 0x180000000, so a decompiler address maps to an offset by subtracting it.
//   DAT_1825c94b8  ->  client.dll + 0x25c94b8   (the render-device interface pointer)
//   vtable +0x20   ->  index 4                  (returns the frame-size info struct)
//   struct +0x8/+0xC -> uint32 width / height
constexpr uintptr_t kClientRenderDeviceGlobalOffset = 0x25c94b8;
constexpr uintptr_t kClientTargetBuilderOffset = 0x1158860;
constexpr uintptr_t kClientTargetBuilderEndOffset = 0x115BA82; // Ghidra body max + 1
constexpr uintptr_t kClientFrameSizeConsumerOffset = 0xDDB0D0;
constexpr uintptr_t kClientFrameSizeConsumerEndOffset = 0xDDB4FF; // Ghidra body max + 1
constexpr size_t kSizeQueryVtableIndex = 4;
constexpr size_t kInfoWidthOffset = 0x8;
constexpr size_t kInfoHeightOffset = 0xC;
constexpr size_t kSizeInfoCopyBytes = 0x30; // frame consumer also reads byte +0x24

// scenesystem.dll, CS2 build of 2026-08-01. This is the common scratch-target allocation
// boundary. Some client passes describe the engine2-owned view target using its real window
// metadata even though its physical resource and the SceneSystem pool are enlarged; a private
// spec copy repairs that final disagreement without changing render-window/swapchain metadata.
constexpr uintptr_t kSceneScratchAllocateOffset = 0x149730;


// The second parameter is NOT a string. Ghidra typed it `char *` (it flows from another call it
// could not resolve), but at runtime it arrives as the small integer 1 -- printing it with %s
// dereferenced address 0x1 and crashed the game inside ucrtbase's formatter. Treat it as an
// opaque integer/handle and never dereference it.
typedef void* (__fastcall* SizeQuery_t)(void* pThis, uintptr_t nSelector);
typedef uintptr_t (__fastcall* SceneScratchAllocate_t)(
	void* pThis, uintptr_t name, const int* spec, unsigned flags);
typedef void (__fastcall* ClientTargetBuilder_t)(void* pThis);

SizeQuery_t s_origSizeQuery = nullptr;
SceneScratchAllocate_t s_origSceneScratchAllocate = nullptr;
ClientTargetBuilder_t s_origClientTargetBuilder = nullptr;
std::atomic<float> s_factor{ 1.0f };
std::atomic<bool> s_installed{ false };
std::atomic<bool> s_targetBuilderGatewayInstalled{ false };
std::atomic<bool> s_sceneScratchBridgeInstalled{ false };
std::string s_status = "not installed";

// Last observed values, for the status readout.
std::atomic<unsigned> s_lastSrcW{ 0 }, s_lastSrcH{ 0 }, s_lastOutW{ 0 }, s_lastOutH{ 0 };
std::atomic<unsigned> s_loggedScaledCallers{ 0 };

HMODULE s_hClient = nullptr;
HMODULE s_hSceneSystem = nullptr;

// The two decompiled consumers read only the prefix through height (+0xC), synchronously. A
// thread-local copy prevents their scaled answer from leaking into engine2/render-system callers
// that share the underlying object and own the real window/swapchain mode.
alignas(16) thread_local unsigned char s_scaledSizeInfo[kSizeInfoCopyBytes] = {};
alignas(16) thread_local int s_scaledScratchSpec[5] = {};

bool PlausibleResolution(unsigned w, unsigned h) {
	return w >= 64 && h >= 64 && w <= 16384 && h <= 16384;
}

// Readable memory check, so a moved offset after a CS2 update produces a warning instead of a
// fault. VirtualQuery is enough here: we only ever read.
bool Readable(const void* p, size_t bytes) {
	if (!p) return false;
	MEMORY_BASIC_INFORMATION mbi = {};
	if (0 == ::VirtualQuery(p, &mbi, sizeof(mbi))) return false;
	if (mbi.State != MEM_COMMIT) return false;
	const DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
	if (mbi.Protect & bad) return false;
	const uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
	return (uintptr_t)p + bytes <= end;
}

std::string DescribeAddress(void* address) {
	HMODULE module = nullptr;
	if (!address || !::GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)address, &module) || !module) {
		char unknown[32];
		snprintf(unknown, sizeof(unknown), "%p", address);
		return unknown;
	}
	char path[MAX_PATH] = {};
	::GetModuleFileNameA(module, path, MAX_PATH);
	const char* name = strrchr(path, '\\');
	name = name ? name + 1 : path;
	char described[MAX_PATH + 48];
	snprintf(described, sizeof(described), "%s+0x%llx", name,
		(unsigned long long)((uintptr_t)address - (uintptr_t)module));
	return described;
}

uintptr_t __fastcall New_SceneScratchAllocate(
	void* pThis, uintptr_t name, const int* spec, unsigned flags) {
	void* const caller = _ReturnAddress();
	const int* effectiveSpec = spec;
	const float f = s_factor.load(std::memory_order_relaxed);
	HMODULE callerModule = nullptr;
	const bool callerIsClient = caller && s_hClient && ::GetModuleHandleExA(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)caller, &callerModule) && callerModule == s_hClient;
	if (f > 1.001f && callerIsClient && Readable(spec, sizeof(s_scaledScratchSpec))) {
		const unsigned srcW = s_lastSrcW.load(std::memory_order_relaxed);
		const unsigned srcH = s_lastSrcH.load(std::memory_order_relaxed);
		const unsigned outW = s_lastOutW.load(std::memory_order_relaxed);
		const unsigned outH = s_lastOutH.load(std::memory_order_relaxed);
		if (srcW && srcH && (unsigned)spec[0] == srcW && (unsigned)spec[1] == srcH
			&& PlausibleResolution(outW, outH)) {
			memcpy(s_scaledScratchSpec, spec, sizeof(s_scaledScratchSpec));
			s_scaledScratchSpec[0] = (int)outW;
			s_scaledScratchSpec[1] = (int)outH;
			effectiveSpec = s_scaledScratchSpec;
			if (MvmDebugLog_Active())
				MvmDebugLog_Linef("render.enginesize.scratchrequest",
					"caller=%s flags=0x%x %ux%u -> %ux%u",
					DescribeAddress(caller).c_str(), flags,
					srcW, srcH, outW, outH);
		}
	}

	const uintptr_t result = s_origSceneScratchAllocate(
		pThis, name, effectiveSpec, flags);
	if (result == ~uintptr_t(0) && MvmDebugLog_Active()) {
		const int width = Readable(effectiveSpec, sizeof(s_scaledScratchSpec))
			? effectiveSpec[0] : -1;
		const int height = Readable(effectiveSpec, sizeof(s_scaledScratchSpec))
			? effectiveSpec[1] : -1;
		MvmDebugLog_Linef("render.enginesize.scratchfail",
			"caller=%s request=%dx%d flags=0x%x",
			DescribeAddress(caller).c_str(), width, height, flags);
	}
	return result;
}

void __fastcall New_ClientTargetBuilder(void* pThis) {
	// The render-device global is not populated when CreateSwapChain first gives us a chance to
	// install, but it is ready here. This gateway runs immediately before the client asks vtable
	// slot 4 for the dimensions used to construct its startup render-target pool.
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.enginesize.early", "target-builder enter this=%p", pThis);
	const bool installed = RenderEngineSize_EnsureInstalled();
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.enginesize.early",
			"target-builder continuing installed=%d status='%s'", installed ? 1 : 0,
			s_status.c_str());
	s_origClientTargetBuilder(pThis);
}

void TryInstallClientTargetBuilderGateway(HMODULE hClient) {
	if (!hClient || s_targetBuilderGatewayInstalled.load(std::memory_order_acquire))
		return;
	ClientTargetBuilder_t target = (ClientTargetBuilder_t)(
		(uintptr_t)hClient + kClientTargetBuilderOffset);
	if (!Readable((void*)target, 16))
		return;

	s_origClientTargetBuilder = target;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)s_origClientTargetBuilder, New_ClientTargetBuilder);
	if (NO_ERROR == DetourTransactionCommit()) {
		s_targetBuilderGatewayInstalled.store(true, std::memory_order_release);
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("render.enginesize.early",
				"target-builder gateway installed client=%p target=+0x%llx",
				(void*)hClient, (unsigned long long)kClientTargetBuilderOffset);
	}
}

void TryInstallSceneScratchBridge() {
	if (s_sceneScratchBridgeInstalled.load(std::memory_order_acquire))
		return;
	HMODULE hSceneSystem = ::GetModuleHandleA("scenesystem.dll");
	if (!hSceneSystem)
		return;
	s_hSceneSystem = hSceneSystem;

	SceneScratchAllocate_t target = (SceneScratchAllocate_t)(
		(uintptr_t)hSceneSystem + kSceneScratchAllocateOffset);
	if (!Readable((void*)target, 16))
		return;

	s_origSceneScratchAllocate = target;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)s_origSceneScratchAllocate, New_SceneScratchAllocate);
	if (NO_ERROR == DetourTransactionCommit())
		s_sceneScratchBridgeInstalled.store(true, std::memory_order_release);
}

// Client builds the scratch-target descriptors from this size. SceneSystem::FrameUpdate later
// resolves size-mode 2 descriptors by calling this same slot again; if that second call sees the
// real video mode, it destroys/recreates the pool at the window size and the next scaled client
// request cannot allocate. Keep both scene-size owners consistent while leaving
// rendersystemdx11's swapchain/present callers on the real backbuffer size.
enum class SizeCaller : unsigned {
	Other = 0,
	ClientTargetBuilder = 1,
	ClientFrameSize = 2,
	SceneSystem = 4,
	OtherClient = 8,
};

SizeCaller ClassifySizeCaller(void* retAddr) {
	if (!retAddr || !s_hClient) return SizeCaller::Other;
	HMODULE owner = nullptr;
	if (!::GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)retAddr, &owner))
		return SizeCaller::Other;
	if (owner == s_hClient) {
		const uintptr_t offset = (uintptr_t)retAddr - (uintptr_t)s_hClient;
		if (kClientTargetBuilderOffset <= offset && offset < kClientTargetBuilderEndOffset)
			return SizeCaller::ClientTargetBuilder;
		if (kClientFrameSizeConsumerOffset <= offset
			&& offset < kClientFrameSizeConsumerEndOffset)
			return SizeCaller::ClientFrameSize;
		return SizeCaller::OtherClient;
	}
	if (owner == s_hSceneSystem) return SizeCaller::SceneSystem;
	return SizeCaller::Other;
}

const char* SizeCallerName(SizeCaller caller) {
	switch (caller) {
	case SizeCaller::ClientTargetBuilder: return "client-target-builder";
	case SizeCaller::ClientFrameSize: return "client-frame-size";
	case SizeCaller::SceneSystem: return "scenesystem";
	case SizeCaller::OtherClient: return "other-client";
	default: return "other";
	}
}

struct SizeQuerySite {
	void* returnAddress = nullptr;
	uintptr_t selector = 0;
	unsigned width = 0;
	unsigned height = 0;
	SizeCaller caller = SizeCaller::Other;
	unsigned calls = 0;
	bool routed = false;
	bool logged = false;
};

std::mutex s_sizeQuerySitesMutex;
std::array<SizeQuerySite, 128> s_sizeQuerySites = {};

void RememberSizeQuerySite(
	void* returnAddress, uintptr_t selector, unsigned width, unsigned height,
	SizeCaller caller, bool routed) {
	std::lock_guard<std::mutex> lock(s_sizeQuerySitesMutex);
	SizeQuerySite* freeSite = nullptr;
	for (SizeQuerySite& site : s_sizeQuerySites) {
		if (site.returnAddress == returnAddress && site.selector == selector) {
			site.width = width;
			site.height = height;
			site.caller = caller;
			site.routed = routed;
			++site.calls;
			return;
		}
		if (!freeSite && !site.returnAddress) freeSite = &site;
	}
	if (!freeSite) return;
	freeSite->returnAddress = returnAddress;
	freeSite->selector = selector;
	freeSite->width = width;
	freeSite->height = height;
	freeSite->caller = caller;
	freeSite->calls = 1;
	freeSite->routed = routed;
}

void FlushSizeQuerySitesToDebugLog() {
	if (!MvmDebugLog_Active()) return;
	std::lock_guard<std::mutex> lock(s_sizeQuerySitesMutex);
	for (SizeQuerySite& site : s_sizeQuerySites) {
		if (!site.returnAddress || site.logged) continue;
		site.logged = true;
		MvmDebugLog_LinefAlways("render.enginesize.querysite",
			"site=%s class=%s selector=0x%llx observed=%ux%u routed=%d calls=%u",
			DescribeAddress(site.returnAddress).c_str(), SizeCallerName(site.caller),
			(unsigned long long)site.selector, site.width, site.height,
			site.routed ? 1 : 0, site.calls);
	}
}

void* __fastcall New_SizeQuery(void* pThis, uintptr_t nSelector) {
	void* const retAddr = _ReturnAddress();
	void* result = s_origSizeQuery(pThis, nSelector);

	const float f = s_factor.load(std::memory_order_relaxed);
	const SizeCaller caller = ClassifySizeCaller(retAddr);
	const bool routed = f > 1.001f && nSelector == 1
		&& caller != SizeCaller::Other && caller != SizeCaller::OtherClient;
	unsigned observedWidth = 0, observedHeight = 0;
	if (result && Readable(result, kInfoHeightOffset + sizeof(unsigned))) {
		observedWidth = *(const unsigned*)((const char*)result + kInfoWidthOffset);
		observedHeight = *(const unsigned*)((const char*)result + kInfoHeightOffset);
	}
	RememberSizeQuerySite(
		retAddr, nSelector, observedWidth, observedHeight, caller, routed);
	FlushSizeQuerySitesToDebugLog();
	if (!routed || !result)
		return result; // swapchain/present and unrelated callers must see the true size

	if (!Readable(result, kSizeInfoCopyBytes))
		return result;

	const unsigned srcW = *(const unsigned*)((const char*)result + kInfoWidthOffset);
	const unsigned srcH = *(const unsigned*)((const char*)result + kInfoHeightOffset);
	if (!PlausibleResolution(srcW, srcH))
		return result; // not the struct we think it is -- leave the engine alone
	// The engine may briefly feed our prior answer back as its new reported mode. It is already
	// the requested scene size; applying the factor again would compound across a mode rebuild.
	if (srcW == s_lastOutW.load(std::memory_order_relaxed)
		&& srcH == s_lastOutH.load(std::memory_order_relaxed))
		return result;

	unsigned outW = (unsigned)(srcW * f + 0.5f) & ~1u;
	unsigned outH = (unsigned)(srcH * f + 0.5f) & ~1u;
	if (!PlausibleResolution(outW, outH))
		return result;

	memcpy(s_scaledSizeInfo, result, sizeof(s_scaledSizeInfo));
	*(unsigned*)(s_scaledSizeInfo + kInfoWidthOffset) = outW;
	*(unsigned*)(s_scaledSizeInfo + kInfoHeightOffset) = outH;

	const unsigned callerBit = (unsigned)caller;
	const bool firstForCaller = 0 == (
		s_loggedScaledCallers.fetch_or(callerBit, std::memory_order_relaxed) & callerBit);
	if (firstForCaller || srcW != s_lastSrcW.load() || outW != s_lastOutW.load()) {
		s_lastSrcW.store(srcW); s_lastSrcH.store(srcH);
		s_lastOutW.store(outW); s_lastOutH.store(outH);
		if (MvmDebugLog_Active())
		{
			MvmDebugLog_LinefAlways("render.enginesize",
				"query caller=%s site=%s sel=0x%llx %ux%u -> %ux%u (x%.2f)",
				SizeCallerName(caller),
				DescribeAddress(retAddr).c_str(),
				(unsigned long long)nSelector, srcW, srcH, outW, outH, f);
		}
	}

	return s_scaledSizeInfo;
}

} // namespace

float RenderEngineSize_Factor() { return s_factor.load(); }

bool RenderEngineSize_TargetDimensions(
	unsigned& sourceWidth, unsigned& sourceHeight,
	unsigned& targetWidth, unsigned& targetHeight) {
	sourceWidth = s_lastSrcW.load(std::memory_order_relaxed);
	sourceHeight = s_lastSrcH.load(std::memory_order_relaxed);
	targetWidth = s_lastOutW.load(std::memory_order_relaxed);
	targetHeight = s_lastOutH.load(std::memory_order_relaxed);
	return PlausibleResolution(sourceWidth, sourceHeight)
		&& PlausibleResolution(targetWidth, targetHeight);
}

// Reads MVM_RENDER_ENGINESIZE once. Returns 0 when unset/invalid.
float ReadEnvFactorOnce() {
	static bool s_read = false;
	static float s_env = 0.0f;
	if (s_read) return s_env;
	s_read = true;
	char buf[16] = {0};
	DWORD n = ::GetEnvironmentVariableA("MVM_RENDER_ENGINESIZE", buf, (DWORD)sizeof(buf));
	if (n > 0 && n < sizeof(buf)) {
		const double v = atof(buf);
		if (v > 1.0 && v <= 4.0) {
			s_env = (float)v;
			advancedfx::Message("SOURCE:MVM: MVM_RENDER_ENGINESIZE=%s -- engine render size x%.2f "
				"from startup (experimental and version-coupled).\n", buf, v);
		}
	}
	return s_env;
}

bool RenderEngineSize_EnsureInstalled() {
	if (s_installed.load()) {
		if (!s_hSceneSystem)
			s_hSceneSystem = ::GetModuleHandleA("scenesystem.dll");
		TryInstallSceneScratchBridge();
		return true;
	}

	// DO NOT install unless explicitly asked for.
	//
	// These hooks are build-specific. Gating their installation -- not just their effect -- keeps
	// an ordinary launch on the untouched engine path and makes a moved CS2 offset harmless unless
	// the user explicitly opts into this experiment.
	const float requestedFactor = ReadEnvFactorOnce();
	if (requestedFactor <= 1.0f) {
		s_status = "not installed (set MVM_RENDER_ENGINESIZE=<factor> before launch to enable)";
		return false;
	}
	// Store the factor even when the client interface is not ready yet. The CreateTexture2D hook
	// needs it during startup; the target-builder gateway retries this function before the client
	// constructs its own target pool and installs the scoped size-query detour then.
	s_factor.store(requestedFactor, std::memory_order_relaxed);

	HMODULE hClient = ::GetModuleHandleA("client.dll");
	if (!hClient) { s_status = "client.dll not loaded yet"; return false; }
	s_hClient = hClient;
	s_hSceneSystem = ::GetModuleHandleA("scenesystem.dll");
	TryInstallClientTargetBuilderGateway(hClient);

	void** ppDevice = (void**)((uintptr_t)hClient + kClientRenderDeviceGlobalOffset);
	if (!Readable(ppDevice, sizeof(void*))) {
		s_status = "render-device global not readable (offset moved? CS2 update)";
		return false;
	}
	void* pDevice = *ppDevice;
	if (!pDevice || !Readable(pDevice, sizeof(void*))) {
		s_status = "render-device interface not created yet";
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("render.enginesize.early",
				"device not ready global=%p value=%p", (void*)ppDevice, pDevice);
		return false; // engine has not connected interfaces yet -- retry next frame
	}

	void** vtable = *(void***)pDevice;
	if (!Readable(vtable, sizeof(void*) * (kSizeQueryVtableIndex + 1))) {
		s_status = "render-device vtable not readable";
		return false;
	}
	void* target = vtable[kSizeQueryVtableIndex];
	if (!target || !Readable(target, 16)) {
		s_status = "size-query slot empty";
		return false;
	}
	// The function must live in a real module; a pointer into the heap means the offset is wrong.
	HMODULE owner = nullptr;
	if (!::GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)target, &owner) || !owner) {
		s_status = "size-query target is not inside a module (bad offset)";
		return false;
	}

	s_origSizeQuery = (SizeQuery_t)target;
	AfxDetourPtr(&(vtable[kSizeQueryVtableIndex]), New_SizeQuery, (PVOID*)&s_origSizeQuery);
	s_installed.store(true);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.enginesize.early",
			"size vtable installed device=%p sizeTarget=%s",
			pDevice, DescribeAddress(target).c_str());
	TryInstallSceneScratchBridge();

	// Resources are created from this answer during startup. A live factor change would leave the
	// existing physical targets, client descriptors and SceneSystem pool mutually inconsistent,
	// so the environment variable remains the only supported entry point.
	s_factor.store(requestedFactor);

	char buf[MAX_PATH] = {0};
	::GetModuleFileNameA(owner, buf, MAX_PATH);
	const char* name = strrchr(buf, '\\');
	name = name ? name + 1 : buf;
	std::ostringstream o;
	o << "installed (device=" << pDevice << " slot " << kSizeQueryVtableIndex
		<< " -> " << name << ")";
	s_status = o.str();
	advancedfx::Message("mirv_filmmaker: engine render-size hook %s\n", s_status.c_str());
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.enginesize", "%s", s_status.c_str());
	return true;
}

std::string RenderEngineSize_Status() {
	FlushSizeQuerySitesToDebugLog();
	std::ostringstream o;
	o << "engine render-size hook: " << s_status
		<< ", factor x" << s_factor.load();
	if (s_installed.load())
		o << ", scratch bridge="
			<< (s_sceneScratchBridgeInstalled.load() ? "installed" : "pending");
	if (s_lastSrcW.load())
		o << ", last query " << s_lastSrcW.load() << "x" << s_lastSrcH.load()
			<< " -> " << s_lastOutW.load() << "x" << s_lastOutH.load();
	return o.str();
}

void RenderEngineSize_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	RenderEngineSize_EnsureInstalled();

	if (argc < 4 || 0 == _stricmp(args->ArgV(3), "status")) {
		advancedfx::Message("%s: %s\n", cmd, RenderEngineSize_Status().c_str());
		advancedfx::Message(
			"  Startup-only: set MVM_RENDER_ENGINESIZE=<factor 1..4> before launching CS2.\n"
			"  The window/backbuffer remain at the game resolution while the captured scene target,\n"
			"  client target pool, scratch requests, viewports and scissors use the scaled size.\n");
		return;
	}

	const char* v = args->ArgV(3);
	if (0 == _stricmp(v, "off")) {
		advancedfx::Message("%s: restart CS2 with MVM_RENDER_ENGINESIZE unset to disable the startup scale.\n", cmd);
		return;
	}
	const double f = atof(v);
	if (!(f >= 1.0 && f <= 4.0)) {
		advancedfx::Warning("%s: factor must be between 1 and 4.\n", cmd);
		return;
	}
	advancedfx::Warning("%s: the factor is startup-only because existing render resources cannot "
		"be resized consistently in place. Restart CS2 with MVM_RENDER_ENGINESIZE=%s.\n", cmd, v);
}

} // namespace Filmmaker
