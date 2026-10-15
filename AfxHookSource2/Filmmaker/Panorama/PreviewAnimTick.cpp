// Forced, preview-scoped client animgraph tick for the paused Customize player preview.
// Rationale, the disproven global-clock approach, and the scope rule: see PreviewAnimTick.h.

#include "PreviewAnimTick.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_* (the mandatory runtime-diagnosis channel)
#include "../Cosmetics/CosmeticModelSwap.h" // UpdatePawnGlovesNow (paused preview hands/gloves)
#include "../../ClientEntitySystem.h"      // CEntityInstance, g_pEntityList, g_GetEntityFromIndex
#include "../../WrpConsole.h"              // CON_COMMAND (mvm_previewtick)
#include "../../../Shared/binutils.h"      // ImageSectionsReader / FindCString / FindAddrInt32OffsetRef
#include "../../../Shared/AfxConsole.h"    // advancedfx::Message / Warning

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../../Deps/release/Detours/src/detours.h"
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h" // ISource2EngineToClient (paused)

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {
namespace {

// ---------------------------------------------------------------------------------------------
// Layout constants read off client.dll build 14172. Every one of them is VALIDATED at runtime
// before it is used to write anything; a CS2 update that moves them turns the feature off with a
// warning instead of corrupting the engine's update list.
// ---------------------------------------------------------------------------------------------

// CAnimGraphGameSystem's per-tick update list, as passed to the list builder in rdx and consumed by
// the "AG2 - EvaluateAnimList - Job Execute Func" worker:
//   +0x00  int32  count
//   +0x10  void*  entries
// Each entry is 0x60 bytes; the entity CHandle is the first dword, and the job masks it with 0x7FFF
// to get the entity index (it rejects -1 / -2 itself).
constexpr ptrdiff_t kListCountOffset = 0x00;
constexpr ptrdiff_t kListEntriesOffset = 0x10;
constexpr size_t    kListEntryStride = 0x60;
constexpr uint32_t  kHandleIndexMask = 0x7FFF;

// Preview pawn discovery. Client-only entities start at 0x4000; the pooled + live preview pawns all
// live above it, so the periodic rescan never touches the networked range.
constexpr int kMaxHandleIndex = 0x8000;
constexpr int kClientOnlyBase = 0x4000;
constexpr const char* kPreviewClass = "previewplayer";
constexpr int kMaxPreviewPawns = 16;

// A forced frame's dt is clamped hard: a stall (loading, alt-tab, a breakpoint) must never hand the
// animgraph a multi-second step, which would make the idle loop snap.
constexpr double kMaxFrameDt = 1.0 / 15.0;
constexpr double kMinFrameDt = 1.0 / 1000.0;

// The AG2 worker multiplies its integer tick delta by this constant (0.015625 == 1/64) to get the
// evaluation dt, so "one tick" is the smallest step the preview can be advanced by.
constexpr double kTicksPerSecond = 64.0;
constexpr int    kMaxTicksPerFrame = 8;
// How often the discovery tick re-runs, in frames. The preview panel is rebuilt on a model/item
// change, which swaps the pawn (and therefore its animation object), so the set has to refresh.
constexpr uint64_t kLearnRefreshFrames = 60;

// ---------------------------------------------------------------------------------------------
// client.dll resolution: profiling-string anchor -> rip-relative xref -> .pdata function bounds.
//
// This is deliberately NOT a byte-pattern scan. The anchor is the literal
// "AnimGraph Client Tick" that the game system passes to its profile scope, and the function start
// comes from the PE exception directory, so the resolution survives any codegen change that keeps
// the string and the scope. It also self-reports: a resolution failure names which step failed.
// ---------------------------------------------------------------------------------------------

typedef void(__fastcall* AnimTickUpdate_t)(void* self, int mode);
typedef void(__fastcall* BuildUpdateList_t)(void* self, void* outList);

// The AG2 worker's per-entity delta source, and the reason the whole "write curtime/frametime"
// family of attempts could never have worked:
//
//   int ConsumeTickDelta(void* animObj, int currentTick) {
//       int last = animObj->m_nLastUpdateTick;          // [rcx+0xD4]
//       animObj->m_nLastUpdateTick = currentTick;
//       return last ? (currentTick - last) : 1;
//   }
//
// The worker then does `dt = (float)max(delta, 0) * (1/64)` and, if `dt <= 0`, SKIPS that entity
// entirely. So the animgraph delta is an integer TICK COUNT. A paused demo pins the tick, every
// entity reports delta 0, and the graph is never evaluated -- no wall-clock value anywhere in the
// client is consulted. Returning a positive count here is the entire fix.
//
// Resolved by its own byte pattern (verified unique in client.dll build 14172): the function is
// eight instructions long, so there is nothing else to anchor on, and its shape is intrinsic to
// what it does. The two struct offsets are wildcarded.
typedef int(__fastcall* ConsumeTickDelta_t)(void* animObj, int currentTick);

const char* const kTickDeltaPattern =
	"44 8B 81 ?? ?? ?? ?? 44 8B CA 45 2B C8 89 91 ?? ?? ?? ?? 45 85 C0 B8 01 00 00 00 41 0F 45 C1 C3";

AnimTickUpdate_t   g_origAnimTick = nullptr;
BuildUpdateList_t  g_origBuildList = nullptr;
ConsumeTickDelta_t g_origTickDelta = nullptr;

struct ResolveInfo {
	size_t animTickStart = 0;
	size_t animTickEnd = 0;
	size_t buildListStart = 0;
	size_t tickDeltaStart = 0;
	int    stringRefs = 0;
	const char* failure = nullptr;
};
ResolveInfo g_resolve;

// Image bounds straight from the PE headers -- avoids a psapi import just for SizeOfImage.
struct ImageBounds { size_t start = 0; size_t end = 0; };

ImageBounds ModuleBounds(HMODULE mod) {
	ImageBounds b;
	if (!mod) return b;
	auto* dos = (IMAGE_DOS_HEADER*)mod;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) return b;
	auto* nt = (IMAGE_NT_HEADERS64*)((unsigned char*)mod + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) return b;
	b.start = (size_t)mod;
	b.end = b.start + nt->OptionalHeader.SizeOfImage;
	return b;
}

// Enclosing function bounds for `addr` from the module's exception directory, following
// UNW_FLAG_CHAININFO so a fragment resolves to its real parent function.
bool FunctionBoundsFromPdata(HMODULE mod, size_t addr, size_t& outStart, size_t& outEnd) {
	outStart = outEnd = 0;
	if (!mod) return false;
	auto* dos = (IMAGE_DOS_HEADER*)mod;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
	auto* nt = (IMAGE_NT_HEADERS64*)((unsigned char*)mod + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
	const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
	if (dir.VirtualAddress == 0 || dir.Size < sizeof(RUNTIME_FUNCTION)) return false;

	const size_t base = (size_t)mod;
	auto* fns = (RUNTIME_FUNCTION*)(base + dir.VirtualAddress);
	const size_t count = dir.Size / sizeof(RUNTIME_FUNCTION);
	const DWORD rva = (DWORD)(addr - base);

	// The table is sorted by BeginAddress.
	size_t lo = 0, hi = count;
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (fns[mid].BeginAddress <= rva) lo = mid + 1; else hi = mid;
	}
	if (lo == 0) return false;
	RUNTIME_FUNCTION* fn = &fns[lo - 1];
	if (!(fn->BeginAddress <= rva && rva < fn->EndAddress)) return false;

	// Follow chained unwind info up to the primary entry.
	for (int guard = 0; guard < 8; ++guard) {
		auto* ui = (unsigned char*)(base + fn->UnwindData);
		const unsigned char flags = (unsigned char)(ui[0] >> 3);
		if (!(flags & 4 /* UNW_FLAG_CHAININFO */)) break;
		const unsigned char codes = ui[2];
		auto* chained = (RUNTIME_FUNCTION*)(ui + 4 + ((codes + 1) & ~1) * 2);
		if (chained->BeginAddress == 0 || chained->BeginAddress == fn->BeginAddress) break;
		fn = chained;
	}
	outStart = base + fn->BeginAddress;
	outEnd = base + fn->EndAddress;
	return true;
}

// The list builder is reached by a direct `call rel32` from inside AnimTickUpdate. It is identified
// structurally rather than by its own byte pattern: it is the first callee whose prologue sets up a
// huge stack frame (`lea rbp, [rsp+disp32]` -- it holds several inline 0x60-byte entry vectors),
// which no other callee from this function does. Verified against build 14172, and re-validated at
// runtime by the probe below before any filtering is allowed.
size_t FindListBuilder(size_t fnStart, size_t fnEnd, size_t imgStart, size_t imgEnd) {
	for (size_t p = fnStart; p + 5 <= fnEnd; ++p) {
		if (*(unsigned char*)p != 0xE8) continue;
		const int32_t rel = *(int32_t*)(p + 1);
		const size_t target = p + 5 + (size_t)(ptrdiff_t)rel;
		if (target < imgStart || target >= imgEnd) continue;
		// Look for `48 8D AC 24` (lea rbp, [rsp+disp32]) in the callee's first 32 bytes.
		bool bigFrame = false;
		for (size_t q = target; q + 4 <= target + 32; ++q) {
			if (*(unsigned char*)(q + 0) == 0x48 && *(unsigned char*)(q + 1) == 0x8D
				&& *(unsigned char*)(q + 2) == 0xAC && *(unsigned char*)(q + 3) == 0x24) {
				bigFrame = true;
				break;
			}
		}
		if (!bigFrame) continue;
		size_t bs = 0, be = 0;
		if (!FunctionBoundsFromPdata((HMODULE)imgStart, target, bs, be) || bs != target) continue;
		return target;
	}
	return 0;
}

bool ResolveClientFunctions() {
	HMODULE client = GetModuleHandleA("client.dll");
	if (!client) { g_resolve.failure = "client.dll not loaded"; return false; }
	const ImageBounds img = ModuleBounds(client);
	if (!img.start) { g_resolve.failure = "client.dll PE headers unreadable"; return false; }
	const size_t imgStart = img.start;
	const size_t imgEnd = img.end;

	// 1. the profiling literal, in a read-only data section.
	size_t strAddr = 0;
	{
		Afx::BinUtils::ImageSectionsReader sections(client);
		while (!sections.Eof()) {
			Afx::BinUtils::MemRange r = Afx::BinUtils::FindCString(sections.GetMemRange(), "AnimGraph Client Tick");
			if (!r.IsEmpty()) { strAddr = r.Start; break; }
			sections.Next();
		}
	}
	if (!strAddr) { g_resolve.failure = "anchor string \"AnimGraph Client Tick\" not found"; return false; }

	// 2. the single rip-relative reference to it, in an executable section.
	size_t refAddr = 0;
	{
		Afx::BinUtils::ImageSectionsReader sections(client);
		while (!sections.Eof()) {
			Afx::BinUtils::MemRange sec = sections.GetMemRange();
			PIMAGE_SECTION_HEADER hdr = sections.Get();
			if (hdr && (hdr->Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
				Afx::BinUtils::MemRange scan = sec;
				while (true) {
					Afx::BinUtils::MemRange hit = Afx::BinUtils::FindAddrInt32OffsetRef(scan, strAddr, 4);
					if (hit.IsEmpty()) break;
					++g_resolve.stringRefs;
					if (!refAddr) refAddr = hit.Start;
					scan.Start = hit.End;
					if (scan.Start >= scan.End) break;
				}
			}
			sections.Next();
		}
	}
	if (!refAddr) { g_resolve.failure = "no code reference to the anchor string"; return false; }

	// 3. enclosing function == CAnimGraphGameSystem::AnimTickUpdate.
	if (!FunctionBoundsFromPdata(client, refAddr, g_resolve.animTickStart, g_resolve.animTickEnd)) {
		g_resolve.failure = "no .pdata entry for the anchor reference";
		return false;
	}

	// 4. its update-list builder.
	g_resolve.buildListStart = FindListBuilder(g_resolve.animTickStart, g_resolve.animTickEnd, imgStart, imgEnd);
	if (!g_resolve.buildListStart) {
		g_resolve.failure = "update-list builder not found inside AnimTickUpdate";
		return false;
	}

	// 5. the per-entity tick-delta source. Without it a forced tick evaluates nothing at all
	//    (every entity reports delta 0 and is skipped), so a failure here is fatal to the feature.
	{
		Afx::BinUtils::ImageSectionsReader sections(client);
		while (!sections.Eof()) {
			PIMAGE_SECTION_HEADER hdr = sections.Get();
			if (hdr && (hdr->Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
				Afx::BinUtils::MemRange r = Afx::BinUtils::FindPatternString(sections.GetMemRange(), kTickDeltaPattern);
				if (!r.IsEmpty()) { g_resolve.tickDeltaStart = r.Start; break; }
			}
			sections.Next();
		}
	}
	if (!g_resolve.tickDeltaStart) {
		g_resolve.failure = "per-entity tick-delta function not found";
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// preview pawn set
// ---------------------------------------------------------------------------------------------

int      g_previewIdx[kMaxPreviewPawns];
int      g_previewCount = 0;
int      g_previewGloveIdx[kMaxPreviewPawns];
int      g_previewGloveCount = 0;
uint64_t g_lastScanFrame = 0;
uint64_t g_frame = 0;

CEntityInstance* WideResolve(int index) {
	if (index < 0 || index >= kMaxHandleIndex || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	__try { return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index); }
	__except (1) { return nullptr; }
}

const char* SafeClassName(CEntityInstance* e) {
	if (!e) return nullptr;
	__try { return e->GetClassName(); } __except (1) { return nullptr; }
}

const char* SafeClientClassName(CEntityInstance* e) {
	if (!e) return nullptr;
	__try { return e->GetClientClassName(); } __except (1) { return nullptr; }
}

bool ContainsNoCase(const char* haystack, const char* needle) {
	if (!haystack || !needle || !*needle) return false;
	const size_t n = std::strlen(needle);
	for (const char* p = haystack; *p; ++p)
		if (0 == _strnicmp(p, needle, n)) return true;
	return false;
}

bool IsPreviewPawn(int index) {
	CEntityInstance* e = WideResolve(index);
	if (!e) return false;
	return ContainsNoCase(SafeClassName(e), kPreviewClass)
		|| ContainsNoCase(SafeClientClassName(e), kPreviewClass);
}

// Rebuilds the preview index set. Only the client-only range is walked, and only every N frames --
// the set is stable while the modal is open and the panel is rebuilt at most on a model change.
void RescanPreviewPawns() {
	g_previewCount = 0;
	for (int i = kClientOnlyBase; i < kMaxHandleIndex && g_previewCount < kMaxPreviewPawns; ++i) {
		if (IsPreviewPawn(i))
			g_previewIdx[g_previewCount++] = i;
	}
	// Each pawn's glove model (its hands -- agents have none) is a separate bone-merged entity. It must
	// share the paused tick or it is never posed and stays invisible.
	g_previewGloveCount = 0;
	for (int i = 0; i < g_previewCount && g_previewGloveCount < kMaxPreviewPawns; ++i) {
		const int glove = PawnGloveEntityIndex((unsigned char*)WideResolve(g_previewIdx[i]));
		if (glove > 0)
			g_previewGloveIdx[g_previewGloveCount++] = glove;
	}
	g_lastScanFrame = g_frame;
}

bool IsPreviewIndex(int index) {
	for (int i = 0; i < g_previewCount; ++i)
		if (g_previewIdx[i] == index) return true;
	for (int i = 0; i < g_previewGloveCount; ++i)
		if (g_previewGloveIdx[i] == index) return true;
	return false;
}

// ---------------------------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------------------------

bool     g_installed = false;
bool     g_enabled = false;          // resolved arming state for this frame
int      g_mode = -1;                // -1 auto (follow the modal), 0 forced off, 1 forced on
bool     g_modalOpen = false;
bool     g_faulted = false;          // a forced tick faulted once -- never try again this session
bool     g_scopePreviewOnly = true; // false only for explicit A/B diagnostics
bool     g_probe = false;           // log what the builder produced (layout validation)
bool     g_layoutValidated = false; // a real engine list decoded sanely at least once
float    g_rate = 1.0f;

void*    g_gameSystem = nullptr;    // captured from a real engine AnimTickUpdate call
int      g_clientMode = -1;

uint64_t g_engineCalls = 0;
uint64_t g_engineCallsWhilePaused = 0;
uint64_t g_forcedCalls = 0;
uint64_t g_filteredLists = 0;
int      g_lastListCount = -1;
int      g_lastKeptCount = -1;
int64_t  g_lastCounter = 0;
double   g_advanced = 0.0;
double   g_tickDebt = 0.0;   // fractional ticks carried between frames, so the idle runs at 1.0x
// Incremented from whichever worker thread the AG2 job lands on.
std::atomic<unsigned long long> g_deltaGrants{ 0 };

// ---------------------------------------------------------------------------------------------
// Learned preview animation objects.
//
// The engine already runs `AnimTickUpdate` every rendered frame while the demo is paused, and the
// preview pawn IS in its update list (state byte 0x02) -- it is skipped only because its tick delta
// is 0. So the right place to grant a delta is the ENGINE's own tick, not an extra out-of-band call:
// that way evaluate -> commit -> bone setup -> render all happen in the engine's own per-frame
// ordering, exactly as they do while playing. An out-of-band call computed a pose that the rest of
// the frame then ignored (measured: no float in the pawn or its body component advanced at all).
//
// Granting on the engine's tick needs per-object scope, which is what this set provides. One forced,
// list-filtered tick is issued periodically purely to LEARN which animation object belongs to a
// preview pawn (during that call the list contains nothing else, so every object the delta hook sees
// is by construction a preview pawn's). After that the engine's own tick does the work.
constexpr int kMaxLearnedObjs = 8;
void*    g_learnedObj[kMaxLearnedObjs] = {};
int      g_learnedCount = 0;
uint64_t g_engineGrants = 0;

bool IsLearnedObj(void* obj) {
	for (int i = 0; i < g_learnedCount; ++i)
		if (g_learnedObj[i] == obj) return true;
	return false;
}

void LearnObj(void* obj) {
	if (!obj || IsLearnedObj(obj)) return;
	if (g_learnedCount >= kMaxLearnedObjs) return;
	g_learnedObj[g_learnedCount++] = obj;
}

void ForgetObjs() { g_learnedCount = 0; }

// Set for the duration of our own forced AnimTickUpdate call. Deliberately NOT thread_local: the
// AG2 evaluation is dispatched to worker threads, and the tick-delta hook has to recognise our
// scope from whichever thread the job lands on. The engine's own tick and ours are both issued from
// the main thread and each completes synchronously, so the two can never overlap.
std::atomic<int> g_forcedScope{ 0 };
std::atomic<int> g_forcedTicks{ 0 };
// Armed + paused: grant the learned preview objects a delta during the engine's own tick.
std::atomic<int> g_grantOnEngineTick{ 0 };

bool DemoPaused() {
	if (!g_pEngineToClient) return false;
	auto* demo = g_pEngineToClient->GetDemoFile();
	return demo ? demo->IsDemoPaused() : false;
}

bool InDemo() { return g_pEngineToClient && g_pEngineToClient->GetDemoFile() != nullptr; }

double WallSeconds() {
	LARGE_INTEGER freq, now;
	if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) return 0.0;
	if (!QueryPerformanceCounter(&now)) return 0.0;
	const int64_t prev = g_lastCounter;
	g_lastCounter = now.QuadPart;
	if (prev == 0) return 0.0;
	const double dt = (double)(now.QuadPart - prev) / (double)freq.QuadPart;
	if (dt < kMinFrameDt) return 0.0;
	return (dt > kMaxFrameDt) ? kMaxFrameDt : dt;
}

// ---------------------------------------------------------------------------------------------
// list access (POD-only SEH bodies)
// ---------------------------------------------------------------------------------------------

struct ListView { int32_t count = 0; unsigned char* entries = nullptr; bool ok = false; };

void ReadList(void* list, ListView* out) {
	*out = ListView{};
	if (!list) return;
	__try {
		out->count = *(int32_t*)((unsigned char*)list + kListCountOffset);
		out->entries = *(unsigned char**)((unsigned char*)list + kListEntriesOffset);
		out->ok = (out->count >= 0 && out->count < 65536)
			&& (out->count == 0 || (uintptr_t)out->entries > 0x10000);
	} __except (1) {
		*out = ListView{};
	}
}

bool ReadEntryIndex(unsigned char* entries, int i, int* outIndex) {
	*outIndex = -1;
	if (!entries) return false;
	__try {
		const uint32_t handle = *(uint32_t*)(entries + (size_t)i * kListEntryStride);
		if (handle == 0xFFFFFFFFu || handle == 0xFFFFFFFEu) return false;
		*outIndex = (int)(handle & kHandleIndexMask);
		return true;
	} __except (1) {
		return false;
	}
}

// Hex of one 0x60-byte update-list entry. The worker gates on `entry[+0x30] == 2` BEFORE it ever
// asks for a tick delta, so if the paused entry carries a different state byte the entity is skipped
// and no delta substitution can possibly reach it. Dumping the whole entry paused vs playing is what
// identifies that field rather than guessing at it.
bool CopyEntry(unsigned char* entries, int i, unsigned char* out) {
	if (!entries || !out) return false;
	__try {
		memcpy(out, entries + (size_t)i * kListEntryStride, kListEntryStride);
		return true;
	} __except (1) {
		return false;
	}
}

void HexEntry(const unsigned char* copy, char* out) {
	static const char* kHex = "0123456789abcdef";
	for (size_t b = 0; b < kListEntryStride; ++b) {
		out[b * 2 + 0] = kHex[copy[b] >> 4];
		out[b * 2 + 1] = kHex[copy[b] & 0xF];
	}
	out[kListEntryStride * 2] = '\0';
}

bool CompactEntries(unsigned char* entries, int dst, int src) {
	if (!entries || dst == src) return true;
	__try {
		memcpy(entries + (size_t)dst * kListEntryStride,
			entries + (size_t)src * kListEntryStride, kListEntryStride);
		return true;
	} __except (1) { return false; }
}

bool WriteCount(void* list, int32_t value) {
	if (!list) return false;
	__try { *(int32_t*)((unsigned char*)list + kListCountOffset) = value; return true; }
	__except (1) { return false; }
}

// ---------------------------------------------------------------------------------------------
// detours
// ---------------------------------------------------------------------------------------------

void __fastcall Hook_AnimTick(void* self, int mode) {
	if (g_forcedScope.load(std::memory_order_acquire) == 0) {
		// A real engine tick. Capture the game-system instance and the mode value the client uses,
		// so the forced call never has to guess either.
		g_gameSystem = self;
		g_clientMode = mode;
		++g_engineCalls;
		const bool paused = DemoPaused();
		if (paused) ++g_engineCallsWhilePaused;
		if (MvmDebugLog_Active() && (g_engineCalls % 128) == 1)
			MvmDebugLog_LinefAlways("preview.tick",
				"engine call n=%llu mode=%d paused=%d whilePaused=%llu self=%p thread=%lu",
				(unsigned long long)g_engineCalls, mode, paused ? 1 : 0,
				(unsigned long long)g_engineCallsWhilePaused, self, (unsigned long)GetCurrentThreadId());
	}
	g_origAnimTick(self, mode);
}

void __fastcall Hook_BuildList(void* self, void* outList) {
	g_origBuildList(self, outList);

	ListView view;
	ReadList(outList, &view);

	// Layout validation runs on ENGINE lists (the ones we never touch): decode a few entries and
	// confirm the handles resolve to real entities. Filtering is refused until this has passed once.
	if (view.ok && view.count > 0 && (!g_layoutValidated || g_probe)) {
		int decoded = 0, resolved = 0;
		char first[256];
		first[0] = '\0';
		for (int i = 0; i < view.count && i < 8; ++i) {
			int idx = -1;
			if (!ReadEntryIndex(view.entries, i, &idx)) continue;
			++decoded;
			CEntityInstance* e = WideResolve(idx);
			const char* cls = SafeClassName(e);
			if (e) ++resolved;
			if (g_probe) {
				char one[64];
				std::snprintf(one, sizeof(one), "%s%d:%s", first[0] ? " " : "", idx, cls ? cls : "?");
				strncat_s(first, sizeof(first), one, _TRUNCATE);
			}
		}
		if (decoded > 0 && resolved * 2 >= decoded)
			g_layoutValidated = true;
		if (g_probe && MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("preview.tick",
				"buildlist forced=%d count=%d decoded=%d resolved=%d validated=%d entries=[%s]",
				g_forcedScope.load(std::memory_order_relaxed), view.count, decoded, resolved,
				g_layoutValidated ? 1 : 0, first);

		// Raw entry for the preview pawn, so the paused-vs-playing difference is visible byte for byte.
		if (g_probe && MvmDebugLog_Active()) {
			for (int i = 0; i < view.count; ++i) {
				int idx = -1;
				if (!ReadEntryIndex(view.entries, i, &idx) || !IsPreviewIndex(idx)) continue;
				unsigned char copy[kListEntryStride];
				if (CopyEntry(view.entries, i, copy)) {
					char hex[kListEntryStride * 2 + 1];
					HexEntry(copy, hex);
					MvmDebugLog_LinefAlways("preview.tick",
						"entry idx=%d slot=%d paused=%d forced=%d state=0x%02x raw=%s",
						idx, i, DemoPaused() ? 1 : 0, g_forcedScope.load(std::memory_order_relaxed),
						(unsigned)copy[0x30], hex);
				}
				break;
			}
		}
	}

	if (g_forcedScope.load(std::memory_order_acquire) == 0 || !g_scopePreviewOnly)
		return;
	if (!view.ok || view.count <= 0)
		return;

	// Preview-only scope: keep exactly the entries whose entity is one of the preview pawns.
	int kept = 0;
	for (int i = 0; i < view.count; ++i) {
		int idx = -1;
		if (!ReadEntryIndex(view.entries, i, &idx)) continue;
		if (!IsPreviewIndex(idx)) continue;
		if (!CompactEntries(view.entries, kept, i)) break;
		++kept;
	}
	if (WriteCount(outList, kept)) {
		++g_filteredLists;
		g_lastListCount = view.count;
		g_lastKeptCount = kept;
		if (MvmDebugLog_Active() && (g_filteredLists % 64) == 1)
			MvmDebugLog_LinefAlways("preview.tick",
				"filter n=%llu %d -> %d entries (previewPawns=%d)",
				(unsigned long long)g_filteredLists, view.count, kept, g_previewCount);
	}
}

// The delta substitution. Outside our forced scope this is a pure pass-through, so the engine's own
// ticks are completely unaffected. Inside it, every entity being evaluated is a preview pawn (the
// list builder above already dropped everything else), so a blanket positive delta is exactly scoped.
int __fastcall Hook_TickDelta(void* animObj, int currentTick) {
	const int real = g_origTickDelta(animObj, currentTick);
	const int ticks = g_forcedTicks.load(std::memory_order_relaxed);
	const bool forced = g_forcedScope.load(std::memory_order_acquire) != 0;

	if (forced) {
		// Discovery pass: the list holds preview pawns only, so this object is one of theirs.
		LearnObj(animObj);
	} else {
		// The engine's own tick. Grant only for a learned preview animation object, and only while
		// the feature is armed and the demo is actually paused -- otherwise pass the real delta
		// straight through so normal playback is bit-for-bit unaffected.
		if (!(g_grantOnEngineTick.load(std::memory_order_acquire) != 0 && IsLearnedObj(animObj)))
			return real;
		if (ticks <= 0)
			return real;
		const unsigned long long en = ++g_engineGrants;
		g_deltaGrants.fetch_add(1, std::memory_order_relaxed);
		if (g_probe && MvmDebugLog_Active() && (en % 32) == 1)
			MvmDebugLog_LinefAlways("preview.tick",
				"engineGrant n=%llu obj=%p curTick=%d real=%d granted=%d thread=%lu",
				en, animObj, currentTick, real, ticks, (unsigned long)GetCurrentThreadId());
		return ticks;
	}

	if (ticks <= 0)
		return real;
	// The original already stored currentTick, so engine bookkeeping stays consistent; we only
	// change what this evaluation is told has elapsed.
	const unsigned long long n = g_deltaGrants.fetch_add(1, std::memory_order_relaxed) + 1;
	if (g_probe && MvmDebugLog_Active() && (n % 8) == 1)
		MvmDebugLog_LinefAlways("preview.tick",
			"delta n=%llu obj=%p curTick=%d real=%d granted=%d thread=%lu",
			n, animObj, currentTick, real, ticks, (unsigned long)GetCurrentThreadId());
	return ticks;
}

bool Install() {
	if (g_installed) return true;
	if (!ResolveClientFunctions()) return false;

	g_origAnimTick = (AnimTickUpdate_t)g_resolve.animTickStart;
	g_origBuildList = (BuildUpdateList_t)g_resolve.buildListStart;
	g_origTickDelta = (ConsumeTickDelta_t)g_resolve.tickDeltaStart;

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_origAnimTick, Hook_AnimTick);
	DetourAttach(&(PVOID&)g_origBuildList, Hook_BuildList);
	DetourAttach(&(PVOID&)g_origTickDelta, Hook_TickDelta);
	g_installed = (NO_ERROR == DetourTransactionCommit());
	if (!g_installed)
		g_resolve.failure = "DetourTransactionCommit failed";
	return g_installed;
}

// The forced tick itself. SEH-wrapped: a fault inside the engine's tick must disarm the feature
// rather than take the process down.
void ForcedTick(void* self, int mode, bool* outFaulted) {
	*outFaulted = false;
	__try {
		g_forcedScope.store(1, std::memory_order_release);
		g_origAnimTick(self, mode);
		g_forcedScope.store(0, std::memory_order_release);
	} __except (1) {
		g_forcedScope.store(0, std::memory_order_release);
		*outFaulted = true;
	}
}

} // namespace

bool PreviewAnimTick_Available() { return g_installed && g_gameSystem != nullptr; }
bool PreviewAnimTick_Enabled() { return g_enabled; }

namespace {

// Arming transition. Installing the detours is deferred to the first arm so a session that never
// opens the Customize modal never patches client.dll at all.
void SetArmed(bool armed) {
	if (g_enabled == armed) return;
	g_enabled = armed;
	g_lastCounter = 0; // drop the stale baseline so re-arming never grants the whole idle gap at once
	g_tickDebt = 0.0;
	if (!armed) {
		// Stop granting the moment we disarm, and drop the learned objects: the panel (and with it the
		// pawn and its animation object) is rebuilt between modal opens, so a stale pointer must never
		// be handed a delta.
		g_grantOnEngineTick.store(0, std::memory_order_release);
		ForgetObjs();
	}
	if (armed) Install();
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("preview.tick", "%s mode=%d modal=%d installed=%d available=%d",
			armed ? "ARM" : "DISARM", g_mode, g_modalOpen ? 1 : 0, g_installed ? 1 : 0,
			PreviewAnimTick_Available() ? 1 : 0);
}

} // namespace

void PreviewAnimTick_RunFrame(bool customizeModalOpen) {
	++g_frame;
	g_modalOpen = customizeModalOpen;
	SetArmed(!g_faulted && ((g_mode < 0) ? g_modalOpen : (g_mode == 1)));

	if (!g_enabled || !g_installed) {
		g_grantOnEngineTick.store(0, std::memory_order_release);
		return;
	}
	if (!InDemo() || !DemoPaused()) {
		// Playing: the engine supplies real deltas. Stop granting so playback is untouched, and drop
		// the learned set so it is re-learned against whatever pawn exists next time we pause.
		g_grantOnEngineTick.store(0, std::memory_order_release);
		ForgetObjs();
		g_lastCounter = 0;
		return;
	}
	if (!g_gameSystem || g_clientMode < 0) {
		g_grantOnEngineTick.store(0, std::memory_order_release);
		return; // never saw a real call yet -- nothing safe to replay
	}

	// Fail closed: without a validated list layout, or with no preview pawn to scope to, a forced
	// tick would advance the whole world. Refuse instead.
	if (g_scopePreviewOnly && !g_layoutValidated)
		return;
	if (g_frame - g_lastScanFrame >= 30 || g_previewCount == 0)
		RescanPreviewPawns();
	if (g_scopePreviewOnly && g_previewCount == 0)
		return;
	// Hands: the preview agent has none of its own; they come from the glove model that the pawn's
	// per-tick think spawns ("MapPlayerPreview gloves"). Paused, that think never runs, so the character
	// stood with bare stumps until playback resumed. Run the glove step for the preview pawns here.
	for (int i = 0; i < g_previewCount; ++i) {
		CEntityInstance* pawn = WideResolve(g_previewIdx[i]);
		if (pawn && RunPreviewPawnThink((unsigned char*)pawn) && MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("preview.gloves", "paused preview think idx=%d", g_previewIdx[i]);
	}


	const double dt = WallSeconds() * (double)g_rate;
	if (dt <= 0.0)
		return;

	// The animgraph is advanced in whole TICKS, so wall time is converted to ticks with the
	// remainder carried to the next frame -- otherwise rounding 16.7ms down to one 15.6ms tick every
	// frame would run the idle ~6% slow forever.
	g_tickDebt += dt * kTicksPerSecond;
	int ticks = (int)g_tickDebt;
	if (ticks <= 0)
		return; // sub-tick frame; let the debt accumulate rather than evaluate a zero delta
	if (ticks > kMaxTicksPerFrame) ticks = kMaxTicksPerFrame;
	g_tickDebt -= (double)ticks;

	g_forcedTicks.store(ticks, std::memory_order_relaxed);
	// From here on the engine's own per-frame tick will grant this delta to the learned objects.
	g_grantOnEngineTick.store(1, std::memory_order_release);

	// Discovery only: one filtered forced tick every kLearnRefreshFrames, enough to (re)learn which
	// animation objects belong to the preview pawns after a panel rebuild. The pose that this call
	// produces is not what ends up on screen -- the engine's own tick does that -- so it does not
	// need to run every frame.
	bool faulted = false;
	if (g_learnedCount == 0 || (g_frame % kLearnRefreshFrames) == 0) {
		ForcedTick(g_gameSystem, g_clientMode, &faulted);
		++g_forcedCalls;
	}
	g_advanced += (double)ticks / kTicksPerSecond;

	if (faulted) {
		// Latch off for the rest of the session: auto mode would otherwise re-arm next frame and
		// fault again every frame.
		g_faulted = true;
		g_enabled = false;
		advancedfx::Warning("mvm_previewtick: the forced animgraph tick faulted; disarming for this session.\n");
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("preview.tick", "FAULT forced=%llu -- disarmed",
				(unsigned long long)g_forcedCalls);
		return;
	}

	if (MvmDebugLog_Active() && (g_forcedCalls % 64) == 1)
		MvmDebugLog_LinefAlways("preview.tick",
			"forced n=%llu wallDt=%.5f ticks=%d advanced=%.2fs grants=%llu pawns=%d list=%d->%d scope=%s",
			(unsigned long long)g_forcedCalls, dt, ticks, g_advanced,
			(unsigned long long)g_deltaGrants.load(std::memory_order_relaxed), g_previewCount,
			g_lastListCount, g_lastKeptCount, g_scopePreviewOnly ? "preview" : "ALL");
}

void PreviewAnimTick_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* arg = (argc >= 2) ? args->ArgV(1) : "status";

	if (0 == _stricmp(arg, "on") || 0 == _stricmp(arg, "1")) {
		g_mode = 1;
		g_faulted = false;
		Install();
		if (!g_installed) {
			advancedfx::Warning("%s: could not install (%s). The preview will not animate while paused.\n",
				cmd, g_resolve.failure ? g_resolve.failure : "unknown");
			return;
		}
		advancedfx::Message("%s: forced ON (overrides the modal). scope=%s rate=%.2f\n",
			cmd, g_scopePreviewOnly ? "preview-only" : "ALL-ANIMGRAPHS", g_rate);
		if (!g_gameSystem)
			advancedfx::Message("%s: waiting for one real engine tick to capture the game system"
				" (let the demo play a moment).\n", cmd);
		return;
	}
	if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "0")) {
		g_mode = 0;
		advancedfx::Message("%s: forced off (the Customize modal will NOT arm it).\n", cmd);
		return;
	}
	if (0 == _stricmp(arg, "auto")) {
		g_mode = -1;
		g_faulted = false;
		advancedfx::Message("%s: auto -- armed whenever the Customize modal is open (default).\n", cmd);
		return;
	}
	if (0 == _stricmp(arg, "install")) {
		const bool ok = Install();
		advancedfx::Message("%s: install=%d animTick=0x%llx..0x%llx buildList=0x%llx strRefs=%d%s%s\n",
			cmd, ok ? 1 : 0, (unsigned long long)g_resolve.animTickStart,
			(unsigned long long)g_resolve.animTickEnd, (unsigned long long)g_resolve.buildListStart,
			g_resolve.stringRefs, g_resolve.failure ? " failure=" : "",
			g_resolve.failure ? g_resolve.failure : "");
		return;
	}
	if (0 == _stricmp(arg, "scope")) {
		if (argc < 3) { advancedfx::Warning("usage: %s scope preview|all\n", cmd); return; }
		if (0 == _stricmp(args->ArgV(2), "preview")) g_scopePreviewOnly = true;
		else if (0 == _stricmp(args->ArgV(2), "all")) g_scopePreviewOnly = false;
		else { advancedfx::Warning("%s: scope must be preview|all\n", cmd); return; }
		advancedfx::Message("%s: scope=%s%s\n", cmd, g_scopePreviewOnly ? "preview-only" : "ALL-ANIMGRAPHS",
			g_scopePreviewOnly ? "" : "  (DIAGNOSTIC: every client animgraph will advance while paused)");
		return;
	}
	if (0 == _stricmp(arg, "probe")) {
		g_probe = !(argc >= 3 && (0 == _stricmp(args->ArgV(2), "off") || 0 == _stricmp(args->ArgV(2), "0")));
		advancedfx::Message("%s: probe=%d (logs decoded update-list entries to mvm_debug)\n", cmd, g_probe ? 1 : 0);
		if (g_probe && !g_installed) Install();
		return;
	}
	if (0 == _stricmp(arg, "rate")) {
		if (argc < 3) { advancedfx::Warning("usage: %s rate <multiplier>\n", cmd); return; }
		const float r = (float)atof(args->ArgV(2));
		if (!(r > 0.0f) || r > 10.0f) { advancedfx::Warning("%s: rate must be in (0,10].\n", cmd); return; }
		g_rate = r;
		advancedfx::Message("%s: rate=%.2f\n", cmd, g_rate);
		return;
	}
	if (0 == _stricmp(arg, "scan")) {
		RescanPreviewPawns();
		advancedfx::Message("%s: preview pawns=%d\n", cmd, g_previewCount);
		for (int i = 0; i < g_previewCount; ++i) {
			CEntityInstance* e = WideResolve(g_previewIdx[i]);
			const char* cls = SafeClassName(e);
			advancedfx::Message("   idx=%d class=%s\n", g_previewIdx[i], cls ? cls : "?");
		}
		return;
	}
	if (0 == _stricmp(arg, "status")) {
		advancedfx::Message("%s: %s mode=%s modalOpen=%d faulted=%d installed=%d available=%d scope=%s rate=%.2f\n"
			"  animTick=0x%llx..0x%llx buildList=0x%llx tickDelta=0x%llx strRefs=%d failure=%s\n"
			"  gameSystem=%p clientMode=%d layoutValidated=%d previewPawns=%d\n"
			"  engineCalls=%llu (paused=%llu) discovery=%llu advanced=%.2fs grants=%llu lastList=%d->%d\n"
			"  learnedObjs=%d engineGrants=%llu granting=%d\n"
			"  inDemo=%d paused=%d mvm_debug=%s\n",
			cmd, g_enabled ? "ARMED" : "idle",
			(g_mode < 0) ? "auto" : (g_mode == 1 ? "forced-on" : "forced-off"),
			g_modalOpen ? 1 : 0, g_faulted ? 1 : 0,
			g_installed ? 1 : 0, PreviewAnimTick_Available() ? 1 : 0,
			g_scopePreviewOnly ? "preview-only" : "ALL", g_rate,
			(unsigned long long)g_resolve.animTickStart, (unsigned long long)g_resolve.animTickEnd,
			(unsigned long long)g_resolve.buildListStart, (unsigned long long)g_resolve.tickDeltaStart,
			g_resolve.stringRefs, g_resolve.failure ? g_resolve.failure : "-",
			g_gameSystem, g_clientMode, g_layoutValidated ? 1 : 0, g_previewCount,
			(unsigned long long)g_engineCalls, (unsigned long long)g_engineCallsWhilePaused,
			(unsigned long long)g_forcedCalls, g_advanced,
			(unsigned long long)g_deltaGrants.load(std::memory_order_relaxed),
			g_lastListCount, g_lastKeptCount,
			g_learnedCount, (unsigned long long)g_engineGrants,
			g_grantOnEngineTick.load(std::memory_order_relaxed),
			InDemo() ? 1 : 0, DemoPaused() ? 1 : 0, MvmDebugLog_Active() ? "on" : "off");
		return;
	}

	advancedfx::Message(
		"%s auto        - arm whenever the Customize modal is open (DEFAULT; no command needed).\n"
		"%s on|off      - override auto: always / never advance the paused preview.\n"
		"%s install     - resolve + detour client.dll without arming (reports addresses).\n"
		"%s scan        - list the preview pawns the forced tick is scoped to.\n"
		"%s scope p|all - preview-only (default) or every client animgraph (DIAGNOSTIC).\n"
		"%s probe [off] - log decoded update-list entries into mvm_debug.\n"
		"%s rate <m>    - preview animation speed while paused (default 1.0).\n"
		"%s status      - full state.\n",
		cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker

CON_COMMAND(mvm_previewtick, "Run the client animgraph tick for the Customize preview pawns while the demo is paused.") {
	Filmmaker::PreviewAnimTick_RunCommand(args->ArgC(), args, args->ArgV(0));
}
