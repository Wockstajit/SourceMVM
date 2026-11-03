#include "PlayingDemoPath.h"

#include "DemoLibrary.h"
#include "DemoPathCacheMath.h"
#include "DemoProjectPath.h"
#include "../Filmmaker.h"              // Library(), CurrentDemoPath()
#include "../../MirvTime.h"           // g_MirvTime.GetCurrentDemoTick
#include "../Platform/TextEncoding.h" // WideToUtf8, Utf8ToWide

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../../../Shared/AfxConsole.h"

#include <Windows.h>
#include <string.h>
#include <wchar.h>
#include <string>
#include <vector>
#include <algorithm>
#include <mutex>

// Provided by main.cpp (same engine pointer the rest of the hook uses).
extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// ---- guarded raw reads --------------------------------------------------------------------
// An engine-object slot can hold ANYTHING, so every dereference of a value pulled out of the
// demo object is wrapped in SEH. These helpers contain only POD locals (no C++ objects that
// require unwinding), which is required for __try/__except.

// Copy up to `cap` bytes from `src`, stopping early at the first unreadable byte. Returns the
// number of bytes copied (so a partially-mapped object never faults the whole scan).
static int SafeCopyBytes(const unsigned char* src, unsigned char* dst, int cap) {
	int i = 0;
	for (; i < cap; ++i) {
		__try { dst[i] = src[i]; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return i; }
	}
	return i;
}

// Copy a NUL-terminated ASCII string from an ARBITRARY pointer (guarded). Returns length, or
// -1 on a fault before the terminator.
static int SafeCopyAscii(const char* src, char* dst, int cap) {
	int i = 0;
	for (; i < cap - 1; ++i) {
		char c = 0;
		__try { c = src[i]; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		if (c == '\0') break;
		dst[i] = c;
	}
	dst[i] = '\0';
	return i;
}

// Copy a NUL-terminated wide string from an ARBITRARY pointer (guarded). Returns length (in
// wchar_t) or -1 on a fault.
static int SafeCopyWide(const wchar_t* src, wchar_t* dst, int cap) {
	int i = 0;
	for (; i < cap - 1; ++i) {
		wchar_t c = 0;
		__try { c = src[i]; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		if (c == L'\0') break;
		dst[i] = c;
	}
	dst[i] = L'\0';
	return i;
}

// Preferred path source: the engine's own getter (HLAE upstream commit 4f25fb5 added
// ISource2EngineToClient::GetDemoFilePath at vtable :043). POD-only + SEH so a vtable shift in
// a future CS2 build can never crash -- it just yields "" and the object scan takes over.
// Returns the copied length, 0 if the engine returned null/empty, or -1 on a fault.
static int SafeGetEngineDemoPath(char* dst, int cap) {
	const char* p = nullptr;
	__try { p = g_pEngineToClient->GetDemoFilePath(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
	if (!p) { dst[0] = '\0'; return 0; }
	return SafeCopyAscii(p, dst, cap); // guarded read of the returned string
}

// ---- path predicates / filesystem ---------------------------------------------------------

bool EndsWithDemCI(const std::wstring& s) {
	if (s.size() < 4) return false;
	return 0 == _wcsnicmp(s.c_str() + s.size() - 4, L".dem", 4);
}
bool LooksLikeDemPathAscii(const char* s, int n) {
	if (n < 5) return false;
	for (int k = 0; k < n; ++k) { unsigned char c = (unsigned char)s[k]; if (c < 0x20 || c > 0x7e) return false; }
	return 0 == _strnicmp(s + n - 4, ".dem", 4);
}
bool LooksLikeDemPathWide(const wchar_t* s, int n) {
	if (n < 5) return false;
	for (int k = 0; k < n; ++k) { wchar_t c = s[k]; if (c < 0x20 || c > 0x7e) return false; }
	return 0 == _wcsnicmp(s + n - 4, L".dem", 4);
}

bool FileExists(const std::wstring& p) {
	if (p.empty()) return false;
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && 0 == (a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---- candidate collection -----------------------------------------------------------------
// Pull every plausible ".dem" path out of a SAFE local snapshot of the demo object: pointer
// members (CUtlString-style char*/wchar_t*) and inline char buffers (char m_szName[260]).

void CollectFromSnapshot(const unsigned char* snap, int snapLen,
	std::vector<std::wstring>& out, std::vector<size_t>* offs) {
	auto pushUnique = [&](const std::wstring& v, size_t off) {
		for (auto& e : out) if (0 == _wcsicmp(e.c_str(), v.c_str())) return;
		out.push_back(v);
		if (offs) offs->push_back(off);
	};

	// (a) pointer members (pointer-aligned slots; offset 0 is the vtable, harmlessly skipped).
	for (int off = 0; off + (int)sizeof(void*) <= snapLen; off += (int)sizeof(void*)) {
		void* slot = nullptr;
		memcpy(&slot, snap + off, sizeof(void*));
		if (!slot) continue;
		char abuf[1024];
		int n = SafeCopyAscii((const char*)slot, abuf, (int)sizeof(abuf));
		if (n > 4 && LooksLikeDemPathAscii(abuf, n)) { pushUnique(Utf8ToWide(std::string(abuf, n)), (size_t)off); continue; }
		wchar_t wbuf[1024];
		int wn = SafeCopyWide((const wchar_t*)slot, wbuf, 1024);
		if (wn > 4 && LooksLikeDemPathWide(wbuf, wn)) { pushUnique(std::wstring(wbuf, wn), (size_t)off); continue; }
	}

	// (b) inline ASCII runs within the object (scanned from the safe snapshot -> never faults).
	int i = 0;
	while (i < snapLen) {
		unsigned char c = snap[i];
		bool printable = (c >= 0x20 && c <= 0x7e);
		bool runStart = printable && (i == 0 || !(snap[i - 1] >= 0x20 && snap[i - 1] <= 0x7e));
		if (runStart) {
			int j = i, n = 0; char rbuf[1024];
			while (j < snapLen && n < (int)sizeof(rbuf) - 1 && snap[j] >= 0x20 && snap[j] <= 0x7e) { rbuf[n++] = (char)snap[j]; ++j; }
			rbuf[n] = '\0';
			if (n > 4 && LooksLikeDemPathAscii(rbuf, n)) pushUnique(Utf8ToWide(std::string(rbuf, n)), (size_t)i);
			i = j;
		} else {
			++i;
		}
	}
}

// Resolve a (possibly relative) demo path string to an absolute path. Relative paths are
// resolved against the CS2 install (the engine usually stores them relative to csgo/), mirror
// of Filmmaker::BuildPlaydemoCommand's csgo-relative logic.
std::wstring ResolveAbsolute(const std::wstring& cand) {
	bool absolute = (cand.size() >= 2 && cand[1] == L':') || (cand.size() >= 2 && cand[0] == L'\\' && cand[1] == L'\\');
	if (absolute) return cand;

	std::wstring install = Library().InstallRoot();
	if (!install.empty() && install.back() != L'\\' && install.back() != L'/') install += L'\\';
	std::wstring rel = cand;
	std::replace(rel.begin(), rel.end(), L'/', L'\\');
	while (!rel.empty() && (rel.front() == L'\\')) rel.erase(0, 1);

	std::wstring c1 = install + L"csgo\\" + rel;
	if (FileExists(c1)) return c1;
	std::wstring c2 = install + rel;
	if (FileExists(c2)) return c2;
	return c1; // best guess when neither exists yet
}

// Snapshot the demo object and resolve the best candidate to a canonical absolute path.
std::wstring ScanAndResolve(void* demoObj) {
	unsigned char snap[0x800];
	int snapLen = SafeCopyBytes((const unsigned char*)demoObj, snap, (int)sizeof(snap));
	if (snapLen < (int)sizeof(void*)) return L"";

	std::vector<std::wstring> cands;
	CollectFromSnapshot(snap, snapLen, cands, nullptr);
	if (cands.empty()) return L"";

	std::wstring best;
	for (auto& c : cands) {
		std::wstring abs = ResolveAbsolute(c);
		if (FileExists(abs)) { best = abs; break; } // prefer a candidate that exists on disk
		if (best.empty()) best = abs;                // else keep the first as a fallback
	}
	if (best.empty()) return L"";
	if (!EndsWithDemCI(best)) best += L".dem";
	return CanonicalDemoPath(best);
}

} // namespace

// ---- public API ---------------------------------------------------------------------------

std::wstring ResolvePlayingDemoPath() {
	if (!g_pEngineToClient) return L"";
	SOURCESDK::CS2::IDemoFile* demo = g_pEngineToClient->GetDemoFile();

	// Cache by demo-object pointer only for the guarded memory scan. The engine can reuse the
	// same IDemoFile object across direct demo-to-demo transitions, so the authoritative engine
	// path getter must be sampled every call; otherwise the previous demo path can survive into
	// the next demo and carry its camera markers over.
	static std::mutex s_m;
	std::lock_guard<std::mutex> lk(s_m);
	static void* s_lastDemo = nullptr;
	static std::wstring s_lastPath;
	static DemoPathCache<std::string> s_enginePath;

	if (!demo || !demo->IsPlayingDemo()) { s_lastDemo = nullptr; s_lastPath.clear(); s_enginePath.Clear(); return L""; }

	// Primary: the engine's own getter (exact + robust). Fall back to the guarded object scan
	// only if it yields nothing, so behaviour never regresses on an SDK the slot doesn't match.
	std::wstring resolved;
	char ebuf[1024];
	int en = SafeGetEngineDemoPath(ebuf, (int)sizeof(ebuf));
	if (en > 0) {
		const std::string raw(ebuf, en);
		resolved = s_enginePath.Get(raw, reinterpret_cast<uintptr_t>(demo),
			DemoProjectPathRevision(), GetTickCount64(), [&] {
				std::wstring abs = ResolveAbsolute(Utf8ToWide(raw));
				if (!EndsWithDemCI(abs)) abs += L".dem";
				return CanonicalDemoPath(abs);
			});
	} else {
		s_enginePath.Clear();
	}
	if (!resolved.empty()) {
		s_lastPath = resolved;
		s_lastDemo = demo;
		return s_lastPath;
	}

	if (demo == s_lastDemo)
		return s_lastPath;

	if (resolved.empty()) resolved = ScanAndResolve(demo);

	s_lastPath = resolved;
	s_lastDemo = demo;
	return s_lastPath;
}

// SEH needs a function with no C++ objects to unwind (C2712), so the guarded read lives here.
static bool ReadDemoPaused(SOURCESDK::CS2::IDemoFile* demo) {
	__try { return demo && demo->IsDemoPaused(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void DebugProbePlayingDemoPath() {
	if (!g_pEngineToClient) { advancedfx::Message("[demoprobe] engine not ready.\n"); return; }
	SOURCESDK::CS2::IDemoFile* demo = g_pEngineToClient->GetDemoFile();
	if (!demo) { advancedfx::Message("[demoprobe] GetDemoFile() == null (load/play a demo first).\n"); return; }
	advancedfx::Message("[demoprobe] IDemoFile=%p IsPlayingDemo=%d\n", (void*)demo, demo->IsPlayingDemo() ? 1 : 0);

	// Primary source: the engine getter (vtable :043). -1 == it faulted (slot mismatch -> scan used).
	char ebuf[1024];
	int en = SafeGetEngineDemoPath(ebuf, (int)sizeof(ebuf));
	if (en < 0)      advancedfx::Message("[demoprobe] GetDemoFilePath() FAULTED (vtable slot mismatch); using scan.\n");
	else if (en == 0) advancedfx::Message("[demoprobe] GetDemoFilePath() = (null/empty); using scan.\n");
	else             advancedfx::Message("[demoprobe] GetDemoFilePath() = '%s'\n", ebuf);

	unsigned char snap[0x800];
	int snapLen = SafeCopyBytes((const unsigned char*)demo, snap, (int)sizeof(snap));
	advancedfx::Message("[demoprobe] readable object bytes: 0x%x\n", (unsigned)snapLen);

	std::vector<std::wstring> cands; std::vector<size_t> offs;
	CollectFromSnapshot(snap, snapLen, cands, &offs);
	if (cands.empty())
		advancedfx::Message("[demoprobe] no '.dem' path strings found in the first 0x800 bytes.\n");
	for (size_t i = 0; i < cands.size(); ++i) {
		std::wstring abs = ResolveAbsolute(cands[i]);
		advancedfx::Message("[demoprobe] +0x%03zx raw='%s' abs='%s' exists=%d\n",
			offs[i], WideToUtf8(cands[i]).c_str(), WideToUtf8(abs).c_str(), FileExists(abs) ? 1 : 0);
	}

	std::wstring resolved = ResolvePlayingDemoPath();
	advancedfx::Message("[demoprobe] RESOLVED = '%s'\n", WideToUtf8(resolved).c_str());
	const char* lvl = g_pEngineToClient->GetLevelNameShort();
	advancedfx::Message("[demoprobe] level = '%s'\n", lvl ? lvl : "(null)");
	// Tick and pause state, so automation can CONFIRM a seek landed and stayed paused instead of
	// guessing from timing (Automation/netcon/cs2-demo.ps1 -Tick).
	int tick = -1;
	if (!g_MirvTime.GetCurrentDemoTick(tick)) tick = -1;
	bool paused = false;
	paused = ReadDemoPaused(demo);
	advancedfx::Message("[demoprobe] tick=%d paused=%d\n", tick, paused ? 1 : 0);
}

} // namespace Filmmaker
