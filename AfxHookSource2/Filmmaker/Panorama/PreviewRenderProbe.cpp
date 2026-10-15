// Customize Player's independent 3D-panel render path plus runtime instrumentation.
// The preview panel is allowed through its own scene-render gate while the modal is open; no
// demo state, engine frame counter, or panel scene-frame field is changed.

#include "PreviewRenderProbe.h"

#include "../Debug/MvmDebugLog.h"
#include "../../WrpConsole.h"

#include "../../../Shared/binutils.h"
#include "../../../Deps/release/Detours/src/detours.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {
namespace {

using NoArgMethod = void(__fastcall*)(void*);
using PanelRenderMethod = void(__fastcall*)(
	void*, void*, void*, void*, float, float, float, float, uintptr_t);
using PanelSubmitMethod = void(__fastcall*)(
	void*, void*, void*, void*, uintptr_t, void*);

NoArgMethod g_oldViewRender = nullptr;
NoArgMethod g_oldPlayerDispatch = nullptr;
NoArgMethod g_oldPanelReady = nullptr;
PanelRenderMethod g_oldPanelRender = nullptr;
PanelSubmitMethod g_oldPanelSubmit = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_installAttempted{false};
std::atomic<bool> g_modalOpen{false};
// Config picker's death-animation preview: its panel never reaches the ready hook that arms the
// independent draw (measured ready=0 for its whole life), so while it is open EVERY submitted 3D
// panel is armed as it is submitted. The picker is the only 3D panel on screen then.
std::atomic<bool> g_drawAll{false};
std::atomic<bool> g_probeEnabled{true};
std::atomic<bool> g_bridgeEnabled{true};

std::atomic<uint64_t> g_viewRenderCalls{0};
std::atomic<uint64_t> g_playerDispatchCalls{0};
std::atomic<uint64_t> g_panelReadyCalls{0};
std::atomic<uint64_t> g_panelRenderCalls{0};
std::atomic<uint64_t> g_panelSubmitCalls{0};
std::atomic<uint64_t> g_matchedSubmitCalls{0};
std::atomic<uint64_t> g_mountDrawArms{0};
std::atomic<uint64_t> g_mountDraws{0};

std::atomic<uintptr_t> g_lastPlayerPanel{0};
std::atomic<uintptr_t> g_lastReadyPanel{0};
std::atomic<uintptr_t> g_lastRenderPanel{0};
std::atomic<uintptr_t> g_lastSubmitPanel{0};
std::atomic<unsigned> g_lastThreadId{0};
std::atomic<unsigned> g_lastReady{0};
std::atomic<unsigned> g_lastSceneActive{0};
std::atomic<unsigned> g_lastFirstFrame{0};
std::atomic<unsigned> g_lastSceneState{0};
std::atomic<unsigned> g_lastSceneAge{0};
std::atomic<unsigned> g_lastPanelSceneFrame{0};
std::atomic<unsigned> g_lastCurrentSceneFrame{0};

uintptr_t g_viewRenderAddress = 0;
uintptr_t g_playerDispatchAddress = 0;
uintptr_t g_panelReadyAddress = 0;
uintptr_t g_panelRenderAddress = 0;
uintptr_t g_panelSubmitAddress = 0;
uintptr_t g_sceneFrameOwnerSlotAddress = 0;
void** g_sceneFrameOwnerSlot = nullptr;
uintptr_t g_sceneGateAddress = 0;
uintptr_t g_sceneGateCaveAddress = 0;
const char* g_failure = nullptr;

thread_local void* g_activeRenderPanel = nullptr;
PVOID volatile g_mountDrawPanel = nullptr;
volatile LONG g_mountDrawArmed = 0;
PVOID volatile g_mountDrawLoggedPanel = nullptr;

bool DemoState(bool& playing, bool& paused) {
	playing = false;
	paused = false;
	if (!g_pEngineToClient)
		return false;
	auto* demo = g_pEngineToClient->GetDemoFile();
	if (!demo)
		return false;
	playing = demo->IsPlayingDemo();
	paused = demo->IsDemoPaused();
	return true;
}

void SnapshotPlayerPanel(void* panel) {
	if (!panel)
		return;
	__try {
		auto* p = static_cast<unsigned char*>(panel);
		g_lastReady.store(p[0x58] ? 1u : 0u, std::memory_order_relaxed);
		g_lastSceneActive.store(p[0x6b1] ? 1u : 0u, std::memory_order_relaxed);
		g_lastFirstFrame.store(p[0x8e9] ? 1u : 0u, std::memory_order_relaxed);
		g_lastSceneState.store(*reinterpret_cast<unsigned*>(p + 0x6f0), std::memory_order_relaxed);
		g_lastSceneAge.store(*reinterpret_cast<unsigned*>(p + 0x6f4), std::memory_order_relaxed);
	}
	__except (1) {
		g_lastReady.store(0xffffffffu, std::memory_order_relaxed);
		g_lastSceneActive.store(0xffffffffu, std::memory_order_relaxed);
		g_lastFirstFrame.store(0xffffffffu, std::memory_order_relaxed);
		g_lastSceneState.store(0xffffffffu, std::memory_order_relaxed);
		g_lastSceneAge.store(0xffffffffu, std::memory_order_relaxed);
	}
}

void __fastcall HookViewRender(void* self) {
	g_viewRenderCalls.fetch_add(1, std::memory_order_relaxed);
	g_lastThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
	g_oldViewRender(self);
}

void __fastcall HookPlayerDispatch(void* self) {
	g_playerDispatchCalls.fetch_add(1, std::memory_order_relaxed);
	g_lastPlayerPanel.store(reinterpret_cast<uintptr_t>(self), std::memory_order_relaxed);
	g_lastThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
	SnapshotPlayerPanel(self);
	g_oldPlayerDispatch(self);
}

void __fastcall HookPanelReady(void* self) {
	g_panelReadyCalls.fetch_add(1, std::memory_order_relaxed);
	g_lastReadyPanel.store(reinterpret_cast<uintptr_t>(self), std::memory_order_relaxed);
	g_lastThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
	if (g_bridgeEnabled.load(std::memory_order_acquire)
		&& g_modalOpen.load(std::memory_order_acquire)) {
		InterlockedExchangePointer(&g_mountDrawPanel, self);
		InterlockedExchange(&g_mountDrawArmed, 1);
		g_mountDrawArms.fetch_add(1, std::memory_order_relaxed);
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("preview.render",
				"INDEPENDENT_DRAW_ENABLE panel=%p gate=%p cave=%p"
				" demoWrite=0 frameWrite=0",
				self, reinterpret_cast<void*>(g_sceneGateAddress),
				reinterpret_cast<void*>(g_sceneGateCaveAddress));
	}
	g_oldPanelReady(self);
}

void __fastcall HookPanelRender(
	void* self, void* arg2, void* arg3, void* arg4,
	float minX, float minY, float maxX, float maxY, uintptr_t arg9) {
	g_panelRenderCalls.fetch_add(1, std::memory_order_relaxed);
	g_lastRenderPanel.store(reinterpret_cast<uintptr_t>(self), std::memory_order_relaxed);
	g_lastThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
	SnapshotPlayerPanel(self);
	void* previous = g_activeRenderPanel;
	g_activeRenderPanel = self;
	g_oldPanelRender(self, arg2, arg3, arg4, minX, minY, maxX, maxY, arg9);
	g_activeRenderPanel = previous;
}

void __fastcall HookPanelSubmit(
	void* self, void* arg2, void* arg3, void* arg4, uintptr_t arg5, void* rect) {
	g_panelSubmitCalls.fetch_add(1, std::memory_order_relaxed);
	if (g_activeRenderPanel == self)
		g_matchedSubmitCalls.fetch_add(1, std::memory_order_relaxed);
	g_lastSubmitPanel.store(reinterpret_cast<uintptr_t>(self), std::memory_order_relaxed);
	g_lastThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
	if (g_drawAll.load(std::memory_order_acquire) && g_bridgeEnabled.load(std::memory_order_acquire)) {
		InterlockedExchangePointer(&g_mountDrawPanel, self);
		InterlockedExchange(&g_mountDrawArmed, 1);
	}
	const bool independentDraw =
		self == InterlockedCompareExchangePointer(&g_mountDrawPanel, nullptr, nullptr)
		&& 0 != InterlockedCompareExchange(&g_mountDrawArmed, 0, 0);

	// Read-only evidence for the inner scene-render gate. No counter or panel field is changed.
	__try {
		auto* frameOwner = g_sceneFrameOwnerSlot
			? static_cast<unsigned char*>(*g_sceneFrameOwnerSlot) : nullptr;
		if (frameOwner) {
			g_lastCurrentSceneFrame.store(
				*reinterpret_cast<unsigned*>(frameOwner + 0x44), std::memory_order_relaxed);
			g_lastPanelSceneFrame.store(
				*reinterpret_cast<unsigned*>(static_cast<unsigned char*>(self) + 0x890),
				std::memory_order_relaxed);
		}
	}
	__except (1) {
		g_lastCurrentSceneFrame.store(0xffffffffu, std::memory_order_relaxed);
		g_lastPanelSceneFrame.store(0xffffffffu, std::memory_order_relaxed);
	}
	g_oldPanelSubmit(self, arg2, arg3, arg4, arg5, rect);
	if (independentDraw) {
		g_mountDraws.fetch_add(1, std::memory_order_relaxed);
		if (self != InterlockedCompareExchangePointer(
				&g_mountDrawLoggedPanel, self, self)
			&& MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("preview.render",
				"INDEPENDENT_DRAW_FIRST panel=%p gate=%p cave=%p"
				" panelFrame=%u current=%u demoWrite=0 frameWrite=0",
				self, reinterpret_cast<void*>(g_sceneGateAddress),
				reinterpret_cast<void*>(g_sceneGateCaveAddress),
				g_lastPanelSceneFrame.load(std::memory_order_relaxed),
				g_lastCurrentSceneFrame.load(std::memory_order_relaxed));
	}
}

PanelSubmitMethod ResolvePanelSubmit(PanelRenderMethod panelRender) {
	// CUI_3dPanel slot 88 has two direct calls in this build. The render submission is the one
	// followed by restoration of the saved RBX at [rsp+0x60], immediately before the epilogue.
	auto* code = reinterpret_cast<unsigned char*>(panelRender);
	for (size_t i = 0; i + 10 < 0x180; ++i) {
		if (code[i] != 0xe8
			|| code[i + 5] != 0x48 || code[i + 6] != 0x8b || code[i + 7] != 0x5c
			|| code[i + 8] != 0x24 || code[i + 9] != 0x60)
			continue;
		const int32_t displacement = *reinterpret_cast<const int32_t*>(code + i + 1);
		return reinterpret_cast<PanelSubmitMethod>(code + i + 5 + displacement);
	}
	return nullptr;
}

void** ResolveSceneFrameOwnerSlot(PanelSubmitMethod panelSubmit) {
	// The helper loads the scene-frame owner (mov rax,[rip+X]) and, a few argument moves later,
	// reads [owner+0x44] (mov ecx,[rax+0x44]). Resolve the RIP-relative slot from that pair.
	//
	// The moves in between are NOT pinned: the September 2026 client re-allocated them (r8 now goes
	// to r13 instead of rsi), which broke a byte-exact pattern and silently left this whole probe
	// uninstalled -- the paused Customize preview went blank again. What IS required is
	// `mov r14, rcx` among them, because the scene-gate cave compares r14 against the preview panel.
	auto* code = reinterpret_cast<unsigned char*>(panelSubmit);
	for (size_t i = 0; i + 7 < 0x80; ++i) {
		if (code[i] != 0x48 || code[i + 1] != 0x8b || code[i + 2] != 0x05)
			continue;
		bool thisInR14 = false;
		for (size_t j = i + 7; j + 2 < i + 7 + 24 && j + 2 < 0x90; ++j) {
			if (code[j] == 0x4c && code[j + 1] == 0x8b && code[j + 2] == 0xf1)
				thisInR14 = true;
			if (code[j] == 0x8b && code[j + 1] == 0x48 && code[j + 2] == 0x44) {
				if (!thisInR14)
					break;
				const int32_t displacement = *reinterpret_cast<const int32_t*>(code + i + 3);
				return reinterpret_cast<void**>(code + i + 7 + displacement);
			}
		}
	}
	return nullptr;
}

struct SceneGateSite {
	unsigned char* patch = nullptr;
	unsigned char* drawBody = nullptr;
	unsigned char* throttledReturn = nullptr;
};

SceneGateSite ResolveSceneGateSite(PanelSubmitMethod panelSubmit) {
	// add eax,3; cmp ecx,eax; jl <throttled epilogue>. This is the only gate before the helper
	// constructs and submits the 3D view. The cave below preserves this branch for every call except
	// the active Customize preview panel, which Panorama drives independently of demo playback.
	auto* code = reinterpret_cast<unsigned char*>(panelSubmit);
	for (size_t i = 0; i + 14 < 0x100; ++i) {
		if (code[i] != 0x83 || code[i + 1] != 0xc0 || code[i + 2] != 0x03
			|| code[i + 3] != 0x3b || code[i + 4] != 0xc8
			|| code[i + 5] != 0x0f || code[i + 6] != 0x8c
			|| code[i + 11] != 0x48 || code[i + 12] != 0x8b || code[i + 13] != 0x0d)
			continue;
		const int32_t displacement = *reinterpret_cast<const int32_t*>(code + i + 7);
		return {
			code + i,
			code + i + 11,
			code + i + 11 + displacement
		};
	}
	return {};
}

void* AllocateNear(void* target, size_t size) {
	SYSTEM_INFO info{};
	GetSystemInfo(&info);
	const uintptr_t granularity = info.dwAllocationGranularity;
	const uintptr_t targetAddress = reinterpret_cast<uintptr_t>(target);
	const uintptr_t alignedTarget = targetAddress & ~(granularity - 1);
	const uintptr_t maxDistance = 0x70000000ull;
	for (uintptr_t distance = granularity; distance <= maxDistance; distance += granularity) {
		for (int direction : {1, -1}) {
			const uintptr_t candidate = direction > 0
				? alignedTarget + distance
				: (alignedTarget > distance ? alignedTarget - distance : 0);
			if (!candidate)
				continue;
			void* allocation = VirtualAlloc(
				reinterpret_cast<void*>(candidate), size,
				MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
			if (!allocation)
				continue;
			const intptr_t delta = reinterpret_cast<unsigned char*>(allocation)
				- static_cast<unsigned char*>(target);
			if (delta >= (std::numeric_limits<int32_t>::min)()
				&& delta <= (std::numeric_limits<int32_t>::max)())
				return allocation;
			VirtualFree(allocation, 0, MEM_RELEASE);
		}
	}
	return nullptr;
}

void AppendU64(std::vector<unsigned char>& code, uint64_t value) {
	const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
	code.insert(code.end(), bytes, bytes + sizeof(value));
}

size_t AppendRel32Placeholder(std::vector<unsigned char>& code) {
	const size_t offset = code.size();
	code.insert(code.end(), 4, 0);
	return offset;
}

bool PatchRel32(std::vector<unsigned char>& code, size_t displacementOffset,
	uintptr_t instructionEnd, uintptr_t destination) {
	const int64_t delta = static_cast<int64_t>(destination)
		- static_cast<int64_t>(instructionEnd);
	if (delta < (std::numeric_limits<int32_t>::min)()
		|| delta > (std::numeric_limits<int32_t>::max)())
		return false;
	const int32_t relative = static_cast<int32_t>(delta);
	std::memcpy(code.data() + displacementOffset, &relative, sizeof(relative));
	return true;
}

bool InstallSceneGateBypass(PanelSubmitMethod panelSubmit) {
	const SceneGateSite gate = ResolveSceneGateSite(panelSubmit);
	if (!gate.patch || !gate.drawBody || !gate.throttledReturn)
		return false;

	auto* cave = static_cast<unsigned char*>(AllocateNear(gate.patch, 0x1000));
	if (!cave)
		return false;

	std::vector<unsigned char> code;
	code.reserve(96);
	// mov r11, &g_mountDrawPanel; cmp r14, [r11]; jne normal
	code.insert(code.end(), {0x49, 0xbb});
	AppendU64(code, reinterpret_cast<uint64_t>(&g_mountDrawPanel));
	code.insert(code.end(), {0x4d, 0x3b, 0x33, 0x0f, 0x85});
	const size_t jumpNotTarget = AppendRel32Placeholder(code);
	// mov r11, &g_mountDrawArmed; cmp dword ptr [r11],0; je normal
	code.insert(code.end(), {0x49, 0xbb});
	AppendU64(code, reinterpret_cast<uint64_t>(&g_mountDrawArmed));
	code.insert(code.end(), {
		0x41, 0x83, 0x3b, 0x00,
		0x0f, 0x84
	});
	const size_t jumpNotArmed = AppendRel32Placeholder(code);
	// Active target: enter the existing draw body directly on every Panorama submission.
	code.push_back(0xe9);
	const size_t jumpDraw = AppendRel32Placeholder(code);

	const size_t normalOffset = code.size();
	// All other calls retain Valve's original three-frame throttle.
	code.insert(code.end(), {0x83, 0xc0, 0x03, 0x3b, 0xc8, 0x0f, 0x8c});
	const size_t jumpThrottle = AppendRel32Placeholder(code);
	code.push_back(0xe9);
	const size_t jumpNormalDraw = AppendRel32Placeholder(code);

	const uintptr_t caveAddress = reinterpret_cast<uintptr_t>(cave);
	const uintptr_t normalAddress = caveAddress + normalOffset;
	if (!PatchRel32(code, jumpNotTarget, caveAddress + jumpNotTarget + 4, normalAddress)
		|| !PatchRel32(code, jumpNotArmed, caveAddress + jumpNotArmed + 4, normalAddress)
		|| !PatchRel32(code, jumpDraw, caveAddress + jumpDraw + 4,
			reinterpret_cast<uintptr_t>(gate.drawBody))
		|| !PatchRel32(code, jumpThrottle, caveAddress + jumpThrottle + 4,
			reinterpret_cast<uintptr_t>(gate.throttledReturn))
		|| !PatchRel32(code, jumpNormalDraw, caveAddress + jumpNormalDraw + 4,
			reinterpret_cast<uintptr_t>(gate.drawBody))) {
		VirtualFree(cave, 0, MEM_RELEASE);
		return false;
	}

	std::memcpy(cave, code.data(), code.size());
	FlushInstructionCache(GetCurrentProcess(), cave, code.size());

	const intptr_t patchDelta = cave - (gate.patch + 5);
	if (patchDelta < (std::numeric_limits<int32_t>::min)()
		|| patchDelta > (std::numeric_limits<int32_t>::max)()) {
		VirtualFree(cave, 0, MEM_RELEASE);
		return false;
	}
	unsigned char patch[11] = {0xe9};
	const int32_t patchRelative = static_cast<int32_t>(patchDelta);
	std::memcpy(patch + 1, &patchRelative, sizeof(patchRelative));
	std::memset(patch + 5, 0x90, sizeof(patch) - 5);

	DWORD oldProtection = 0;
	if (!VirtualProtect(gate.patch, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtection)) {
		VirtualFree(cave, 0, MEM_RELEASE);
		return false;
	}
	std::memcpy(gate.patch, patch, sizeof(patch));
	FlushInstructionCache(GetCurrentProcess(), gate.patch, sizeof(patch));
	DWORD ignoredProtection = 0;
	VirtualProtect(gate.patch, sizeof(patch), oldProtection, &ignoredProtection);

	g_sceneGateAddress = reinterpret_cast<uintptr_t>(gate.patch);
	g_sceneGateCaveAddress = reinterpret_cast<uintptr_t>(cave);
	return true;
}

bool Install() {
	if (g_installed.load(std::memory_order_acquire))
		return true;
	bool expected = false;
	if (!g_installAttempted.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
		return g_installed.load(std::memory_order_acquire);

	HMODULE client = GetModuleHandleW(L"client.dll");
	if (!client) {
		g_failure = "client.dll is not loaded";
		return false;
	}

	auto** viewVtable = reinterpret_cast<void**>(
		Afx::BinUtils::FindClassVtable(client, ".?AVCViewRender@@", 0, 0));
	auto** playerVtable = reinterpret_cast<void**>(
		Afx::BinUtils::FindClassVtable(client, ".?AVCUI_Player3dPanel@@", 0, 0));
	auto** panelVtable = reinterpret_cast<void**>(
		Afx::BinUtils::FindClassVtable(client, ".?AVCUI_3dPanel@@", 0, 0));
	if (!viewVtable || !playerVtable || !panelVtable) {
		g_failure = "one or more RTTI vtables did not resolve";
		return false;
	}

	// CS2 build 2000879 / client build 14172. These entries and their no-argument ABIs were verified
	// from this exact client.dll before adding the probe. The resolved pointers are always logged so
	// moved entries after an update cannot silently masquerade as a render result.
	g_oldViewRender = reinterpret_cast<NoArgMethod>(viewVtable[5]);
	g_oldPlayerDispatch = reinterpret_cast<NoArgMethod>(playerVtable[99]);
	g_oldPanelReady = reinterpret_cast<NoArgMethod>(panelVtable[89]);
	g_oldPanelRender = reinterpret_cast<PanelRenderMethod>(panelVtable[88]);
	g_oldPanelSubmit = ResolvePanelSubmit(g_oldPanelRender);
	g_sceneFrameOwnerSlot = ResolveSceneFrameOwnerSlot(g_oldPanelSubmit);
	g_viewRenderAddress = reinterpret_cast<uintptr_t>(g_oldViewRender);
	g_playerDispatchAddress = reinterpret_cast<uintptr_t>(g_oldPlayerDispatch);
	g_panelReadyAddress = reinterpret_cast<uintptr_t>(g_oldPanelReady);
	g_panelRenderAddress = reinterpret_cast<uintptr_t>(g_oldPanelRender);
	g_panelSubmitAddress = reinterpret_cast<uintptr_t>(g_oldPanelSubmit);
	g_sceneFrameOwnerSlotAddress = reinterpret_cast<uintptr_t>(g_sceneFrameOwnerSlot);

	if (!g_oldViewRender || !g_oldPlayerDispatch || !g_oldPanelReady
		|| !g_oldPanelRender || !g_oldPanelSubmit || !g_sceneFrameOwnerSlot) {
		g_failure = "one or more vtable entries were null";
		return false;
	}
	if (!InstallSceneGateBypass(g_oldPanelSubmit)) {
		g_failure = "scene render gate bypass did not install";
		return false;
	}

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(reinterpret_cast<PVOID*>(&g_oldViewRender), HookViewRender);
	DetourAttach(reinterpret_cast<PVOID*>(&g_oldPlayerDispatch), HookPlayerDispatch);
	DetourAttach(reinterpret_cast<PVOID*>(&g_oldPanelReady), HookPanelReady);
	DetourAttach(reinterpret_cast<PVOID*>(&g_oldPanelRender), HookPanelRender);
	DetourAttach(reinterpret_cast<PVOID*>(&g_oldPanelSubmit), HookPanelSubmit);
	const LONG result = DetourTransactionCommit();
	if (NO_ERROR != result) {
		g_failure = "DetourTransactionCommit failed";
		return false;
	}

	g_installed.store(true, std::memory_order_release);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("preview.render",
			"INSTALL viewRender=%p playerDispatch=%p panelReady=%p panelRender=%p"
			" panelSubmit=%p sceneFrameOwnerSlot=%p",
			reinterpret_cast<void*>(g_viewRenderAddress),
			reinterpret_cast<void*>(g_playerDispatchAddress),
			reinterpret_cast<void*>(g_panelReadyAddress),
			reinterpret_cast<void*>(g_panelRenderAddress),
			reinterpret_cast<void*>(g_panelSubmitAddress),
			reinterpret_cast<void*>(g_sceneFrameOwnerSlotAddress));
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("preview.render",
			"INSTALL_MOUNT_DRAW gate=%p cave=%p patchBytes=E9+6NOP"
			" scope=active-customize-panel-continuous demoWrite=0 frameWrite=0",
			reinterpret_cast<void*>(g_sceneGateAddress),
			reinterpret_cast<void*>(g_sceneGateCaveAddress));
	return true;
}

void LogSnapshot(const char* reason) {
	bool playing = false, paused = false;
	const bool haveDemo = DemoState(playing, paused);
	if (!MvmDebugLog_Active())
		return;
	MvmDebugLog_LinefAlways("preview.render",
		"%s modal=%d demo=%d playing=%d paused=%d"
		" calls(view=%llu dispatch=%llu ready=%llu render=%llu submit=%llu matched=%llu)"
		" panel=%p readyPanel=%p renderPanel=%p submitPanel=%p tid=%u"
		" state(ready=%u active=%u first=%u scene=%u age=%u)"
		" sceneGate(panelFrame=%u current=%u delta=%lld)"
		" mountDraw(armed=%ld arms=%llu draws=%llu panel=%p gate=%p cave=%p)"
		" addr(view=%p dispatch=%p ready=%p render=%p submit=%p frameOwnerSlot=%p)",
		reason,
		g_modalOpen.load(std::memory_order_relaxed) ? 1 : 0,
		haveDemo ? 1 : 0, playing ? 1 : 0, paused ? 1 : 0,
		static_cast<unsigned long long>(g_viewRenderCalls.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_playerDispatchCalls.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_panelReadyCalls.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_panelRenderCalls.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_panelSubmitCalls.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_matchedSubmitCalls.load(std::memory_order_relaxed)),
		reinterpret_cast<void*>(g_lastPlayerPanel.load(std::memory_order_relaxed)),
		reinterpret_cast<void*>(g_lastReadyPanel.load(std::memory_order_relaxed)),
		reinterpret_cast<void*>(g_lastRenderPanel.load(std::memory_order_relaxed)),
		reinterpret_cast<void*>(g_lastSubmitPanel.load(std::memory_order_relaxed)),
		g_lastThreadId.load(std::memory_order_relaxed),
		g_lastReady.load(std::memory_order_relaxed),
		g_lastSceneActive.load(std::memory_order_relaxed),
		g_lastFirstFrame.load(std::memory_order_relaxed),
		g_lastSceneState.load(std::memory_order_relaxed),
		g_lastSceneAge.load(std::memory_order_relaxed),
		g_lastPanelSceneFrame.load(std::memory_order_relaxed),
		g_lastCurrentSceneFrame.load(std::memory_order_relaxed),
		static_cast<long long>(g_lastCurrentSceneFrame.load(std::memory_order_relaxed))
			- static_cast<long long>(g_lastPanelSceneFrame.load(std::memory_order_relaxed)),
		InterlockedCompareExchange(&g_mountDrawArmed, 0, 0),
		static_cast<unsigned long long>(g_mountDrawArms.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_mountDraws.load(std::memory_order_relaxed)),
		InterlockedCompareExchangePointer(&g_mountDrawPanel, nullptr, nullptr),
		reinterpret_cast<void*>(g_sceneGateAddress),
		reinterpret_cast<void*>(g_sceneGateCaveAddress),
		reinterpret_cast<void*>(g_viewRenderAddress),
		reinterpret_cast<void*>(g_playerDispatchAddress),
		reinterpret_cast<void*>(g_panelReadyAddress),
		reinterpret_cast<void*>(g_panelRenderAddress),
		reinterpret_cast<void*>(g_panelSubmitAddress),
		reinterpret_cast<void*>(g_sceneFrameOwnerSlotAddress));
}

} // namespace

void PreviewRenderProbe_RunFrame(bool customizeModalOpen, bool drawAllPanels) {
	g_drawAll.store(drawAllPanels, std::memory_order_release);
	customizeModalOpen = customizeModalOpen || drawAllPanels;
	const bool wasOpen = g_modalOpen.exchange(customizeModalOpen, std::memory_order_acq_rel);
	if (!customizeModalOpen) {
		InterlockedExchange(&g_mountDrawArmed, 0);
		InterlockedExchangePointer(&g_mountDrawPanel, nullptr);
	}
	if (customizeModalOpen && !g_installed.load(std::memory_order_acquire))
		Install();
	if (!g_probeEnabled.load(std::memory_order_relaxed))
		return;

	static uint64_t s_frame = 0;
	++s_frame;
	if (wasOpen != customizeModalOpen)
		LogSnapshot(customizeModalOpen ? "MODAL_OPEN" : "MODAL_CLOSE");
	if (customizeModalOpen && (s_frame % 30) == 0)
		LogSnapshot("SAMPLE");
}

void PreviewRenderProbe_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* arg = argc >= 2 ? args->ArgV(1) : "status";
	if (0 == _stricmp(arg, "install")) {
		const bool ok = Install();
		advancedfx::Message("%s: install=%d view=%p dispatch=%p ready=%p render=%p submit=%p%s%s\n",
			cmd, ok ? 1 : 0,
			reinterpret_cast<void*>(g_viewRenderAddress),
			reinterpret_cast<void*>(g_playerDispatchAddress),
			reinterpret_cast<void*>(g_panelReadyAddress),
			reinterpret_cast<void*>(g_panelRenderAddress),
			reinterpret_cast<void*>(g_panelSubmitAddress),
			g_failure ? " failure=" : "", g_failure ? g_failure : "");
		return;
	}
	if (0 == _stricmp(arg, "on") || 0 == _stricmp(arg, "1")) {
		g_probeEnabled.store(true, std::memory_order_relaxed);
		Install();
		LogSnapshot("PROBE_ON");
		advancedfx::Message("%s: probe on (read-only).\n", cmd);
		return;
	}
	if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "0")) {
		LogSnapshot("PROBE_OFF");
		g_probeEnabled.store(false, std::memory_order_relaxed);
		advancedfx::Message("%s: probe off (detours remain installed as pass-through counters).\n", cmd);
		return;
	}
	if (0 == _stricmp(arg, "bridge")) {
		if (argc < 3) {
			advancedfx::Message("%s: bridge=%d (independent draw gate for active Customize panel).\n",
				cmd, g_bridgeEnabled.load(std::memory_order_relaxed) ? 1 : 0);
			return;
		}
		const bool enable = 0 != _stricmp(args->ArgV(2), "off")
			&& 0 != _stricmp(args->ArgV(2), "0");
		g_bridgeEnabled.store(enable, std::memory_order_release);
		if (!enable) {
			InterlockedExchange(&g_mountDrawArmed, 0);
			InterlockedExchangePointer(&g_mountDrawPanel, nullptr);
		}
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("preview.render",
				"BRIDGE enabled=%d demoWrite=0 frameWrite=0", enable ? 1 : 0);
		advancedfx::Message("%s: bridge=%d (demoWrite=0 frameWrite=0).\n",
			cmd, enable ? 1 : 0);
		return;
	}
	if (0 == _stricmp(arg, "mark")) {
		LogSnapshot(argc >= 3 ? args->ArgV(2) : "MARK");
		advancedfx::Message("%s: snapshot written to mvm_debug.\n", cmd);
		return;
	}
	if (0 == _stricmp(arg, "status")) {
		bool playing = false, paused = false;
		const bool haveDemo = DemoState(playing, paused);
		advancedfx::Message(
			"%s: installed=%d enabled=%d bridge=%d modal=%d demo=%d playing=%d paused=%d failure=%s\n"
			"  calls view=%llu dispatch=%llu ready=%llu render=%llu submit=%llu matched=%llu\n"
			"  panels dispatch=%p ready=%p render=%p submit=%p tid=%u\n"
			"  state ready=%u active=%u first=%u scene=%u age=%u\n"
			"  scene gate panelFrame=%u current=%u delta=%lld\n"
			"  mount draw armed=%ld arms=%llu draws=%llu panel=%p gate=%p cave=%p\n"
			"  addresses view=%p dispatch=%p ready=%p render=%p submit=%p frameOwnerSlot=%p"
			" mvm_debug=%d\n",
			cmd,
			g_installed.load(std::memory_order_relaxed) ? 1 : 0,
			g_probeEnabled.load(std::memory_order_relaxed) ? 1 : 0,
			g_bridgeEnabled.load(std::memory_order_relaxed) ? 1 : 0,
			g_modalOpen.load(std::memory_order_relaxed) ? 1 : 0,
			haveDemo ? 1 : 0, playing ? 1 : 0, paused ? 1 : 0,
			g_failure ? g_failure : "-",
			static_cast<unsigned long long>(g_viewRenderCalls.load(std::memory_order_relaxed)),
			static_cast<unsigned long long>(g_playerDispatchCalls.load(std::memory_order_relaxed)),
			static_cast<unsigned long long>(g_panelReadyCalls.load(std::memory_order_relaxed)),
			static_cast<unsigned long long>(g_panelRenderCalls.load(std::memory_order_relaxed)),
			static_cast<unsigned long long>(g_panelSubmitCalls.load(std::memory_order_relaxed)),
			static_cast<unsigned long long>(g_matchedSubmitCalls.load(std::memory_order_relaxed)),
			reinterpret_cast<void*>(g_lastPlayerPanel.load(std::memory_order_relaxed)),
			reinterpret_cast<void*>(g_lastReadyPanel.load(std::memory_order_relaxed)),
			reinterpret_cast<void*>(g_lastRenderPanel.load(std::memory_order_relaxed)),
			reinterpret_cast<void*>(g_lastSubmitPanel.load(std::memory_order_relaxed)),
			g_lastThreadId.load(std::memory_order_relaxed),
			g_lastReady.load(std::memory_order_relaxed),
			g_lastSceneActive.load(std::memory_order_relaxed),
			g_lastFirstFrame.load(std::memory_order_relaxed),
			g_lastSceneState.load(std::memory_order_relaxed),
			g_lastSceneAge.load(std::memory_order_relaxed),
			g_lastPanelSceneFrame.load(std::memory_order_relaxed),
			g_lastCurrentSceneFrame.load(std::memory_order_relaxed),
			static_cast<long long>(g_lastCurrentSceneFrame.load(std::memory_order_relaxed))
				- static_cast<long long>(g_lastPanelSceneFrame.load(std::memory_order_relaxed)),
			InterlockedCompareExchange(&g_mountDrawArmed, 0, 0),
			static_cast<unsigned long long>(g_mountDrawArms.load(std::memory_order_relaxed)),
			static_cast<unsigned long long>(g_mountDraws.load(std::memory_order_relaxed)),
			InterlockedCompareExchangePointer(&g_mountDrawPanel, nullptr, nullptr),
			reinterpret_cast<void*>(g_sceneGateAddress),
			reinterpret_cast<void*>(g_sceneGateCaveAddress),
			reinterpret_cast<void*>(g_viewRenderAddress),
			reinterpret_cast<void*>(g_playerDispatchAddress),
			reinterpret_cast<void*>(g_panelReadyAddress),
			reinterpret_cast<void*>(g_panelRenderAddress),
			reinterpret_cast<void*>(g_panelSubmitAddress),
			reinterpret_cast<void*>(g_sceneFrameOwnerSlotAddress),
			MvmDebugLog_Active() ? 1 : 0);
		return;
	}
	advancedfx::Message(
		"%s install - resolve and attach the read-only render probes.\n"
		"%s on|off - enable/disable periodic mvm_debug snapshots.\n"
		"%s bridge on|off - enable/disable only the independent Customize draw gate.\n"
		"%s mark [label] - write an immediate snapshot to mvm_debug.\n"
		"%s status - print current counters and resolved addresses.\n",
		cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker

CON_COMMAND(mvm_previewrender,
	"Independent Customize Player composition render bridge and diagnostics.") {
	Filmmaker::PreviewRenderProbe_RunCommand(args->ArgC(), args, args->ArgV(0));
}
