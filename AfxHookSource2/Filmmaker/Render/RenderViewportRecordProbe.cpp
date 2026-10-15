#include "RenderViewportRecordProbe.h"
#include "RenderEngineSize.h"

#include "../Debug/MvmDebugLog.h"

#include "../../../Shared/AfxDetours.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <windows.h>

namespace Filmmaker {

namespace {

// CS2 build 2000879, rendersystemdx11.dll. Ghidra-derived command-recording layer:
//
// - FUN_180027a70(ctx, count, viewports)  -- records opcode 0x15 (header 0x788015). ONE logical
//   rectangle per viewport that the executor (FUN_18005e2d0 case 0x15) later turns into BOTH the
//   D3D viewport and the scissor. Elements are 0x1c bytes: {flag, x, y, w, h, minZ, maxZ}.
// - FUN_180027c10(ctx, count, rects)      -- records the scissor-only command; elements are
//   0x10 bytes {x, y, w, h}, converted to {l,t,r,b} inside the record.
// - FUN_180025a70(ctx, slot, target)      -- bind recorder; caches the bound target OBJECT at
//   ctx + 0x2690 + slot*8 (slots 0..3) and, for slot 3, the target dimensions from
//   target+0x150/+0x154 into ctx+0x2700/+0x2704.
//
// Re-derive after a CS2 update: search rendersystemdx11 for the composed header immediates
// 0x??8015 / 0x??8013 near CUtlMemoryPoolBase::Alloc of the command-record pool.
constexpr uintptr_t kRecordViewportsOffset = 0x27a70;
constexpr uintptr_t kRecordScissorOffset = 0x27c10;
constexpr uintptr_t kCtxBoundTargetsOffset = 0x2690; // void* [4]
constexpr uintptr_t kTargetWidthOffset = 0x150;      // uint32 in the target object
constexpr uintptr_t kTargetHeightOffset = 0x154;

struct RecordedViewport {
	int flag;
	int x;
	int y;
	int w;
	int h;
	float minDepth;
	float maxDepth;
};

struct RecordedRect {
	int x;
	int y;
	int w;
	int h;
};

typedef void(__fastcall* RecordViewports_t)(void* ctx, int count, const RecordedViewport* vps);
typedef void(__fastcall* RecordScissor_t)(void* ctx, int count, const RecordedRect* rects);

RecordViewports_t s_origRecordViewports = nullptr;
RecordScissor_t s_origRecordScissor = nullptr;
std::atomic<bool> s_installed{ false };
std::mutex s_installMutex;

// MVM_RENDER_RECORDSCALE=0 keeps the probe's stack logging but disables the record-time
// rectangle scaling, so the diagnostic and the fix can be evaluated independently.
bool s_scaleChecked = false;
bool s_scaleEnabled = true;

std::mutex s_seenMutex;
std::set<std::string> s_seenStacks;
std::atomic<unsigned> s_stacksLogged{ 0 };
constexpr unsigned kMaxStacks = 12;

bool Readable(const void* address, size_t bytes) {
	MEMORY_BASIC_INFORMATION info = {};
	if (0 == ::VirtualQuery(address, &info, sizeof(info))) return false;
	if (info.State != MEM_COMMIT) return false;
	if (0 != (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
	return ((uintptr_t)info.BaseAddress + info.RegionSize) >= ((uintptr_t)address + bytes);
}

bool ScaleEnabled() {
	if (!s_scaleChecked) {
		s_scaleChecked = true;
		char buf[8] = {0};
		DWORD n = ::GetEnvironmentVariableA("MVM_RENDER_RECORDSCALE", buf, (DWORD)sizeof(buf));
		if (n > 0 && n < sizeof(buf) && buf[0] == '0') s_scaleEnabled = false;
	}
	return s_scaleEnabled;
}

std::string DescribeAddress(void* addr) {
	HMODULE mod = nullptr;
	if (!::GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)addr, &mod) || !mod) {
		char buf[32];
		snprintf(buf, sizeof(buf), "?+%p", addr);
		return buf;
	}
	char path[MAX_PATH] = {0};
	::GetModuleFileNameA(mod, path, MAX_PATH);
	const char* name = strrchr(path, '\\');
	name = name ? name + 1 : path;
	char buf[MAX_PATH + 48];
	snprintf(buf, sizeof(buf), "%s+0x%llx", name,
		(unsigned long long)((uintptr_t)addr - (uintptr_t)mod));
	return buf;
}

// True when any target currently bound on this RECORDING context has exactly the enlarged scene
// dimensions. This is the discriminator every execution-time repair lacked: panorama/UI passes
// bind source-sized targets and must keep their rectangles, while scene and depth passes bind
// the enlarged targets and need the scaled rectangle from the very first consumer onward.
bool BoundTargetIsEnlarged(
	void* ctx, unsigned targetW, unsigned targetH, unsigned& outW, unsigned& outH) {
	outW = outH = 0;
	if (!ctx) return false;
	void* const* slots = (void* const*)((const unsigned char*)ctx + kCtxBoundTargetsOffset);
	if (!Readable(slots, sizeof(void*) * 4)) return false;
	for (int slot = 0; slot < 4; ++slot) {
		const unsigned char* target = (const unsigned char*)slots[slot];
		if (!target || !Readable(target + kTargetWidthOffset, sizeof(unsigned) * 2)) continue;
		unsigned w = 0, h = 0;
		memcpy(&w, target + kTargetWidthOffset, sizeof(w));
		memcpy(&h, target + kTargetHeightOffset, sizeof(h));
		if (w < 16 || h < 16 || w > 16384 || h > 16384) continue;
		if (outW == 0) { outW = w; outH = h; }
		if (w == targetW && h == targetH) { outW = w; outH = h; return true; }
	}
	return false;
}

bool IsFullSceneSourceRect(int x, int y, int w, int h, unsigned sourceW, unsigned sourceH) {
	// The engine also emits the measured one-pixel-short variant (1600x1199).
	return x == 0 && y == 0
		&& w >= (int)sourceW - 1 && w <= (int)sourceW
		&& h >= (int)sourceH - 1 && h <= (int)sourceH;
}

// Dumps the recording context's cached bind slots so the BoundTargetIsEnlarged assumptions
// (slot array at +0x2690, dims at +0x150/+0x154) can be verified against reality.
void LogBoundSlots(void* ctx) {
	if (!ctx) return;
	void* const* slots = (void* const*)((const unsigned char*)ctx + kCtxBoundTargetsOffset);
	if (!Readable(slots, sizeof(void*) * 4)) {
		MvmDebugLog_LinefAlways("render.vprecord", "  slots unreadable ctx=%p", ctx);
		return;
	}
	for (int slot = 0; slot < 4; ++slot) {
		const unsigned char* target = (const unsigned char*)slots[slot];
		if (!target) continue;
		unsigned dwords[8] = {0};
		if (Readable(target + 0x140, sizeof(dwords)))
			memcpy(dwords, target + 0x140, sizeof(dwords));
		MvmDebugLog_LinefAlways("render.vprecord",
			"  slot=%d obj=%p +140=%08x %08x +148=%08x %08x +150=%u +154=%u +158=%u +15c=%u",
			slot, (const void*)target, dwords[0], dwords[1], dwords[2], dwords[3],
			dwords[4], dwords[5], dwords[6], dwords[7]);
	}
}

void LogOwnerStack(const char* kind, int count, int x, int y, int w, int h) {
	if (s_stacksLogged.load(std::memory_order_relaxed) >= kMaxStacks) return;
	void* frames[24] = {0};
	const USHORT n = ::RtlCaptureStackBackTrace(2, 24, frames, nullptr);
	char sig[160];
	snprintf(sig, sizeof(sig), "%s|%p|%p|%p|%p|%d", kind,
		n > 0 ? frames[0] : nullptr, n > 1 ? frames[1] : nullptr,
		n > 2 ? frames[2] : nullptr, n > 3 ? frames[3] : nullptr, count);
	{
		std::lock_guard<std::mutex> lock(s_seenMutex);
		if (s_seenStacks.size() >= 256 || !s_seenStacks.insert(sig).second) return;
	}
	const unsigned stackIndex = s_stacksLogged.fetch_add(1, std::memory_order_relaxed);
	if (stackIndex >= kMaxStacks) return;
	MvmDebugLog_LinefAlways("render.vprecord", "stack=%u kind=%s count=%d rect=%d,%d %dx%d frames=%u",
		stackIndex, kind, count, x, y, w, h, (unsigned)n);
	for (USHORT i = 0; i < n; ++i)
		MvmDebugLog_LinefAlways("render.vprecord", "stack=%u frame=%u %s",
			stackIndex, (unsigned)i, DescribeAddress(frames[i]).c_str());
}

void __fastcall New_RecordViewports(void* ctx, int count, const RecordedViewport* vps) {
	unsigned sourceW = 0, sourceH = 0, targetW = 0, targetH = 0;
	if (vps && count > 0 && count <= 4
		&& RenderEngineSize_TargetDimensions(sourceW, sourceH, targetW, targetH)
		&& IsFullSceneSourceRect(vps[0].x, vps[0].y, vps[0].w, vps[0].h, sourceW, sourceH)) {

		unsigned boundW = 0, boundH = 0;
		const bool enlarged = BoundTargetIsEnlarged(ctx, targetW, targetH, boundW, boundH);
		if (MvmDebugLog_Active()) {
			LogOwnerStack(enlarged ? "vp-scene" : "vp-other", count,
				vps[0].x, vps[0].y, vps[0].w, vps[0].h);
			static std::atomic<unsigned> s_slotDumps{ 0 };
			if (s_slotDumps.fetch_add(1, std::memory_order_relaxed) < 8)
				LogBoundSlots(ctx);
		}

		if (enlarged && ScaleEnabled()) {
			RecordedViewport scaled[4];
			memcpy(scaled, vps, sizeof(RecordedViewport) * count);
			for (int i = 0; i < count; ++i) {
				if (!IsFullSceneSourceRect(
					scaled[i].x, scaled[i].y, scaled[i].w, scaled[i].h, sourceW, sourceH))
					continue;
				scaled[i].w = (int)targetW - (int)((LONG)sourceW - scaled[i].w) ;
				scaled[i].h = (int)targetH - (int)((LONG)sourceH - scaled[i].h);
			}
			if (MvmDebugLog_Active()) {
				static std::atomic<unsigned> s_throttle{ 0 };
				const unsigned t = s_throttle.fetch_add(1, std::memory_order_relaxed);
				if (t < 8 || 0 == (t % 600))
					MvmDebugLog_LinefAlways("render.vprecord",
						"scaled viewport record %dx%d -> %dx%d (bound %ux%u)",
						vps[0].w, vps[0].h, scaled[0].w, scaled[0].h, boundW, boundH);
			}
			s_origRecordViewports(ctx, count, scaled);
			return;
		}
	}
	s_origRecordViewports(ctx, count, vps);
}

void __fastcall New_RecordScissor(void* ctx, int count, const RecordedRect* rects) {
	unsigned sourceW = 0, sourceH = 0, targetW = 0, targetH = 0;
	if (rects && count > 0 && count <= 4
		&& RenderEngineSize_TargetDimensions(sourceW, sourceH, targetW, targetH)
		&& IsFullSceneSourceRect(rects[0].x, rects[0].y, rects[0].w, rects[0].h, sourceW, sourceH)) {

		unsigned boundW = 0, boundH = 0;
		const bool enlarged = BoundTargetIsEnlarged(ctx, targetW, targetH, boundW, boundH);
		if (MvmDebugLog_Active())
			LogOwnerStack(enlarged ? "sc-scene" : "sc-other", count,
				rects[0].x, rects[0].y, rects[0].w, rects[0].h);

		if (enlarged && ScaleEnabled()) {
			RecordedRect scaled[4];
			memcpy(scaled, rects, sizeof(RecordedRect) * count);
			for (int i = 0; i < count; ++i) {
				if (!IsFullSceneSourceRect(
					scaled[i].x, scaled[i].y, scaled[i].w, scaled[i].h, sourceW, sourceH))
					continue;
				scaled[i].w = (int)targetW - ((int)sourceW - scaled[i].w);
				scaled[i].h = (int)targetH - ((int)sourceH - scaled[i].h);
			}
			s_origRecordScissor(ctx, count, scaled);
			return;
		}
	}
	s_origRecordScissor(ctx, count, rects);
}

} // namespace

void RenderViewportRecordProbe_EnsureInstalled() {
	if (s_installed.load(std::memory_order_acquire)) return;
	if (RenderEngineSize_Factor() <= 1.001f) return;

	std::lock_guard<std::mutex> lock(s_installMutex);
	if (s_installed.load(std::memory_order_acquire)) return;

	HMODULE hRenderSystem = ::GetModuleHandleA("rendersystemdx11.dll");
	if (!hRenderSystem) return;

	RecordViewports_t viewportTarget =
		(RecordViewports_t)((uintptr_t)hRenderSystem + kRecordViewportsOffset);
	RecordScissor_t scissorTarget =
		(RecordScissor_t)((uintptr_t)hRenderSystem + kRecordScissorOffset);
	if (!Readable((void*)viewportTarget, 16) || !Readable((void*)scissorTarget, 16)) return;

	s_origRecordViewports = viewportTarget;
	s_origRecordScissor = scissorTarget;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)s_origRecordViewports, New_RecordViewports);
	DetourAttach(&(PVOID&)s_origRecordScissor, New_RecordScissor);
	if (NO_ERROR == DetourTransactionCommit()) {
		s_installed.store(true, std::memory_order_release);
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("render.vprecord",
				"record detours installed rendersystemdx11=%p viewport=+0x%llx scissor=+0x%llx scale=%d",
				(void*)hRenderSystem, (unsigned long long)kRecordViewportsOffset,
				(unsigned long long)kRecordScissorOffset, ScaleEnabled() ? 1 : 0);
	} else {
		s_origRecordViewports = nullptr;
		s_origRecordScissor = nullptr;
	}
}

} // namespace Filmmaker
