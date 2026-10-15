#include "CosmeticAnimFix.h"

#include "../../../Shared/binutils.h"
#include "../Debug/MvmDebugLog.h"          // MvmDebugLog_Active / MvmDebugLog_LinefAlways
#include "../../../Shared/AfxConsole.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../../Deps/release/Detours/src/detours.h"

#include <atomic>

namespace Filmmaker {

namespace {

// client.dll "find-or-build per-model animation data" (the builder behind the crash). Resolved by the
// unique prologue AOB below (validated unique in the live client.dll via misc/sigscan-style check). It is
// called as func(rcx=animCtx, rdx=modelKey, r8=&outSeqList1, r9=&outSeqList2) and writes the two
// sequence-list out pointers; for an unloaded model it leaves one (or both) NULL, which the caller then
// dereferences. Return value is unused by the caller (it reads the out params), so void* is fine.
typedef void* (__fastcall* BuildAnim_t)(void* animCtx, void* modelKey, void** out1, void** out2);

// The prologue of the builder. FindPatternString consumes TWO hex chars per wildcard, so wildcards are "??".
const char* const kBuildAnimPattern =
	"48 89 4C 24 08 55 53 41 56 48 8D AC 24";

BuildAnim_t g_origBuildAnim = nullptr;
bool g_installed = false;
bool g_animFixOn = true;
std::atomic<unsigned long long> g_fills(0);
std::atomic<unsigned long long> g_calls(0);

// Static EMPTY sequence list shared by every substituted out-param. Layout the caller assumes:
//   int32 count @ +0 (== 0 here) ; void* data @ +8. With count == 0 the caller computes end = data + 0 and
// its `cmp r15, end; je` treats the list as empty -- data is never dereferenced. Read-only at runtime, so
// sharing one static across threads is safe.
alignas(16) unsigned char g_emptySeqList[32] = { 0 };

size_t FindClientPattern(const char* pattern) {
	HMODULE client = GetModuleHandleA("client.dll");
	if (!client)
		return 0;
	Afx::BinUtils::ImageSectionsReader sections(client);
	if (sections.Eof())
		return 0;
	Afx::BinUtils::MemRange result = Afx::BinUtils::FindPatternString(sections.GetMemRange(), pattern);
	return result.IsEmpty() ? 0 : result.Start;
}

void* __fastcall Hook_BuildAnim(void* animCtx, void* modelKey, void** out1, void** out2) {
	void* ret = g_origBuildAnim(animCtx, modelKey, out1, out2);
	const unsigned long long callNo = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
	if (MvmDebugLog_Active() && callNo <= 32) {
		MvmDebugLog_LinefAlways("knife.animfix.call",
			"call=%llu out1=%p val1=%p out2=%p val2=%p ret=%p thread=%lu",
			callNo, (void*)out1, out1 ? *out1 : nullptr, (void*)out2, out2 ? *out2 : nullptr,
			ret, (unsigned long)GetCurrentThreadId());
	}
	if (g_animFixOn) {
		bool filled = false;
		if (out1 && !*out1) { *out1 = g_emptySeqList; filled = true; }
		if (out2 && !*out2) { *out2 = g_emptySeqList; filled = true; }
		if (filled) {
			unsigned long long n = g_fills.fetch_add(1, std::memory_order_relaxed) + 1;
			// Cheap, capped breadcrumb: proves the fix fired (and on which thread) without flooding.
			if (MvmDebugLog_Active() && n <= 64)
				MvmDebugLog_LinefAlways("knife.animfix",
					"substituted EMPTY seq-list for NULL out-param (n=%llu thread=%lu) -> crash avoided",
					n, (unsigned long)GetCurrentThreadId());
		}
	}
	return ret;
}

} // namespace

bool EnsureAnimCrashFixInstalled() {
	if (g_installed)
		return true;
	size_t match = FindClientPattern(kBuildAnimPattern);
	// Build 14172 prepends two five-byte register-spill instructions to this function.
	// The legacy suffix AOB therefore lands at +10; move the Detours target to the real
	// entry when those exact spill bytes are present. Older clients still attach at
	// the suffix, which is their function entry.
	if (match >= 8) {
		__try {
			const unsigned char* p = reinterpret_cast<const unsigned char*>(match);
			const unsigned char spill[] = { 0x4C, 0x89, 0x4C, 0x24, 0x20,
				0x4C, 0x89, 0x44, 0x24, 0x18 };
			bool hasSpill = true;
			for (size_t i = 0; i < sizeof(spill); ++i)
				if (p[-10 + i] != spill[i]) { hasSpill = false; break; }
			if (hasSpill)
				match -= 10;
		} __except (1) {
			// Leave the raw match unchanged if the compatibility probe faults.
		}
	}
	g_origBuildAnim = (BuildAnim_t)match;
	if (!g_origBuildAnim) {
		advancedfx::Warning("cosmetics animfix: could not resolve the anim builder pattern in client.dll; "
			"the knife-type-swap crash fix is INACTIVE (a CS2 update likely moved the pattern).\n");
		return false;
	}
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_origBuildAnim, Hook_BuildAnim);
	g_installed = (NO_ERROR == DetourTransactionCommit());
	if (g_installed)
		advancedfx::Message("cosmetics animfix: knife-type-swap crash fix installed (anim builder detoured).\n");
	else
		advancedfx::Warning("cosmetics animfix: DetourTransactionCommit failed; crash fix INACTIVE.\n");
	return g_installed;
}

void SetAnimCrashFix(bool enabled) { g_animFixOn = enabled; }
bool AnimCrashFix() { return g_animFixOn; }
bool AnimCrashFixInstalled() { return g_installed; }
unsigned long long AnimCrashFixFills() { return g_fills.load(std::memory_order_relaxed); }

} // namespace Filmmaker
