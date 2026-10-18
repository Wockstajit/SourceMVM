#include "DemoRepair.h"

#include "DemoHeaderReader.h"
#include "DemoProjectPath.h"
#include "DemoProjectStore.h"
#include "../Platform/TextEncoding.h"
#include "../../../Shared/AfxConsole.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace Filmmaker {

namespace {

std::mutex g_mx;
RepairState g_state = RepairState::Idle;
std::string g_status;
std::wstring g_activeDemo;   // demo currently being checked/repaired
std::wstring g_resolved;     // playable path once known
std::thread g_worker;
std::atomic<bool> g_busy{ false };

constexpr int kDecalBugFixedFromProtocol = 14172;
constexpr int kRejectedAtOrBelowProtocol = 14141;

bool FileExists(const std::wstring& p) {
	const DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// size:mtime of a file, as a comparable string. Empty on failure.
std::string FileStamp(const std::wstring& p) {
	WIN32_FILE_ATTRIBUTE_DATA fad{};
	if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return std::string();
	const ULONGLONG size = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
	const ULONGLONG mtime = ((ULONGLONG)fad.ftLastWriteTime.dwHighDateTime << 32)
		| fad.ftLastWriteTime.dwLowDateTime;
	return std::to_string(size) + ":" + std::to_string(mtime);
}

// A known-clean verdict persisted beside the (never-created) repaired copy:
//   <project>\repaired.dem.clean   containing the original's size:mtime.
// Without it every clean demo was re-parsed end to end on its first play of EVERY session --
// seconds of "repairing demo" for demos that never needed anything. Only repaired copies were
// remembered (they exist as files); clean verdicts were forgotten at shutdown.
std::wstring CleanMarkerPath(const std::wstring& repairedPath) {
	return repairedPath + L".clean";
}

bool HasValidCleanMarker(const std::wstring& demoPath, const std::wstring& repairedPath) {
	const std::string stamp = FileStamp(demoPath);
	if (stamp.empty()) return false;
	FILE* f = _wfopen(CleanMarkerPath(repairedPath).c_str(), L"rb");
	if (!f) return false;
	char buf[128]{};
	const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	return n > 0 && stamp == std::string(buf, n);
}

void WriteCleanMarker(const std::wstring& demoPath, const std::wstring& repairedPath) {
	const std::string stamp = FileStamp(demoPath);
	if (stamp.empty()) return;
	FILE* f = _wfopen(CleanMarkerPath(repairedPath).c_str(), L"wb");
	if (!f) return;
	fwrite(stamp.data(), 1, stamp.size(), f);
	fclose(f);
}

// The same idea for a REPAIRED copy. Without it, "if (FileExists(repaired)) return repaired;"
// trusted any file that merely had the right NAME: re-record or re-download a demo to the same
// name (routine for match730_*.dem) and the copy built from the PREVIOUS file was played
// forever -- the wrong match, silently, with no error. It also meant updating the repair helper
// never regenerated anything, because nothing recorded which helper produced a copy.
//
// Stores "<demo size:mtime>|<helper size:mtime>". A copy written by an older build carries no
// marker at all and is therefore rebuilt once, which is the honest answer: we cannot verify it.
std::wstring RepairedMarkerPath(const std::wstring& repairedPath) {
	return repairedPath + L".src";
}

bool HasValidRepairedMarker(const std::wstring& demoPath, const std::wstring& repairedPath,
		const std::wstring& exePath) {
	const std::string demoStamp = FileStamp(demoPath);
	if (demoStamp.empty()) return false;
	FILE* f = _wfopen(RepairedMarkerPath(repairedPath).c_str(), L"rb");
	if (!f) return false;
	char buf[256]{};
	const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	if (0 == n) return false;
	const std::string stored(buf, n);
	const size_t bar = stored.find('|');
	if (std::string::npos == bar) return false;
	if (stored.substr(0, bar) != demoStamp) return false;
	// The helper stamp only invalidates when a helper is actually resolvable NOW: with no
	// helper on disk, replaying an existing repaired copy still beats refusing to play.
	const std::string exeStamp = FileStamp(exePath);
	if (exePath.empty() || exeStamp.empty()) return true;
	return stored.substr(bar + 1) == exeStamp;
}

void WriteRepairedMarker(const std::wstring& demoPath, const std::wstring& repairedPath,
		const std::wstring& exePath) {
	const std::string demoStamp = FileStamp(demoPath);
	if (demoStamp.empty()) return;
	const std::string value = demoStamp + "|" + FileStamp(exePath);
	FILE* f = _wfopen(RepairedMarkerPath(repairedPath).c_str(), L"wb");
	if (!f) return;
	fwrite(value.data(), 1, value.size(), f);
	fclose(f);
	InvalidateDemoProjectPathCache();
}

std::wstring SelfModuleDir() {
	HMODULE hm = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCWSTR)&SelfModuleDir, &hm))
		return std::wstring();
	wchar_t buf[MAX_PATH];
	const DWORD n = GetModuleFileNameW(hm, buf, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) return std::wstring();
	std::wstring p(buf, n);
	const size_t slash = p.find_last_of(L"\\/");
	return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash);
}

// Same lookup shape as DemoInfoHelper's: env override wins, then staging layouts next to us.
std::wstring FindRepairExe() {
	wchar_t over[MAX_PATH];
	const DWORD n = GetEnvironmentVariableW(L"MIRV_FILMMAKER_DEMOREPAIR", over, MAX_PATH);
	if (n > 0 && n < MAX_PATH && FileExists(over)) return std::wstring(over, n);

	const std::wstring dir = SelfModuleDir();
	if (dir.empty()) return std::wstring();
	const wchar_t* rels[] = {
		L"\\FilmmakerDemoRepair\\FilmmakerDemoRepair.exe",
		L"\\FilmmakerDemoRepair.exe",
		L"\\..\\FilmmakerDemoRepair\\FilmmakerDemoRepair.exe",
	};
	for (const wchar_t* rel : rels) {
		std::wstring cand = dir + rel;
		if (FileExists(cand)) return cand;
	}
	return std::wstring();
}

// Is the freshly written copy actually a demo? The helper reported success, but "os.WriteFile
// returned nil" is not the same claim -- a truncated or half-written file passes that and then
// fails in CS2, after we have cached it and told the user it was repaired. Cheap structural
// check rather than a full re-parse (which would cost seconds on a 300 MB demo): the header must
// read, it must name the SAME map as the original, and the copy must not have collapsed in size.
// Catches the failure modes that actually happen -- empty, truncated, or a copy of the wrong
// demo -- without doubling the repair time.
bool RepairedCopyLooksValid(const std::wstring& demoPath, const std::wstring& repairedPath,
		std::string& outWhy) {
	const DemoHeaderInfo src = ReadDemoHeader(demoPath);
	const DemoHeaderInfo out = ReadDemoHeader(repairedPath);
	if (!out.ok) { outWhy = "the repaired copy has no readable demo header"; return false; }
	if (src.ok && !src.map.empty() && out.map != src.map) {
		outWhy = "the repaired copy names map '" + out.map + "', the original names '" + src.map + "'";
		return false;
	}
	WIN32_FILE_ATTRIBUTE_DATA a{}, b{};
	if (GetFileAttributesExW(demoPath.c_str(), GetFileExInfoStandard, &a)
			&& GetFileAttributesExW(repairedPath.c_str(), GetFileExInfoStandard, &b)) {
		const ULONGLONG srcSize = ((ULONGLONG)a.nFileSizeHigh << 32) | a.nFileSizeLow;
		const ULONGLONG outSize = ((ULONGLONG)b.nFileSizeHigh << 32) | b.nFileSizeLow;
		// Stripping a handful of decal messages removes kilobytes, not half the file.
		if (srcSize > 0 && outSize < srcSize / 2) {
			outWhy = "the repaired copy is less than half the size of the original";
			return false;
		}
	}
	return true;
}

// Caller holds g_mx. Shared by the per-demo switch in ResolvePlayable and DemoRepair_Reset.
void ClearStateLocked() {
	g_state = RepairState::Idle;
	g_status.clear();
	g_activeDemo.clear();
	g_resolved.clear();
}

void SetState(RepairState s, const std::string& text) {
	std::lock_guard<std::mutex> lock(g_mx);
	g_state = s;
	g_status = text;
}

// Runs the helper and waits. Returns its exit code, or -1 if it could not be started.
int RunHelper(const std::wstring& exe, const std::wstring& args) {
	std::wstring cmd = L"\"" + exe + L"\" " + args;
	std::vector<wchar_t> buf(cmd.begin(), cmd.end());
	buf.push_back(L'\0');

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
		return -1;
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return (int)code;
}

} // namespace

std::wstring DemoRepair_RepairedPathFor(const std::wstring& demoPath) {
	return DemoProjectStore_GeneratedForLoad(ProjectGame::Cs2, demoPath, ProjectGenerated::Repaired);
}

bool DemoRepair_HelperAvailable() { return !FindRepairExe().empty(); }

DemoCompatibility DemoRepair_ClassifyProtocol(int networkProtocol) {
	if (networkProtocol <= 0) return DemoCompatibility::Unknown;
	if (networkProtocol <= kRejectedAtOrBelowProtocol) return DemoCompatibility::Unsupported;
	if (networkProtocol < kDecalBugFixedFromProtocol) return DemoCompatibility::LegacyCheck;
	return DemoCompatibility::Supported;
}

DemoRepairCacheState DemoRepair_CacheState(const std::wstring& demoPath) {
	if (demoPath.empty()) return DemoRepairCacheState::None;
	const std::wstring repaired = DemoRepair_RepairedPathFor(demoPath);
	if (repaired.empty()) return DemoRepairCacheState::None;
	if (FileExists(repaired) && HasValidRepairedMarker(demoPath, repaired, FindRepairExe()))
		return DemoRepairCacheState::Repaired;
	if (HasValidCleanMarker(demoPath, repaired))
		return DemoRepairCacheState::Clean;
	return DemoRepairCacheState::None;
}

void DemoRepair_Reset() {
    // The worker executes code and touches globals owned by this DLL. Detaching here allows it
    // to outlive module shutdown and resume inside unloaded code. Join outside g_mx: the worker's
    // final state commit takes that mutex before it exits.
    if (g_worker.joinable()) g_worker.join();
    g_busy.store(false);
    std::lock_guard<std::mutex> lock(g_mx);
    ClearStateLocked();
}

std::wstring DemoRepair_ResolvePlayable(const std::wstring& demoPath) {
	if (demoPath.empty()) return demoPath;

	// Repair state belongs to ONE demo. It used to be a process-global latch that nothing ever
	// cleared (DemoRepair_Reset had zero callers), so a single failure -- a missing helper exe is
	// enough -- left every demo the user selected afterwards showing "REPAIR FAILED", including
	// perfectly healthy ones, for the rest of the session. Ready never fell back to Idle either.
	{
		std::lock_guard<std::mutex> lock(g_mx);
		if (!g_activeDemo.empty() && g_activeDemo != demoPath && !g_busy.load())
			ClearStateLocked();
	}

	// Already repaired on a previous run? Use it straight away -- but only if the marker still
	// matches this exact demo file AND the helper that produced it (see RepairedMarkerPath).
	const std::wstring repaired = DemoRepair_RepairedPathFor(demoPath);
	if (!repaired.empty() && FileExists(repaired)
			&& HasValidRepairedMarker(demoPath, repaired, FindRepairExe()))
		return repaired;

	// CLASSIFY BEFORE PARSING. Reading CDemoFileHeader is a few KB; the scan below is a full
	// parse of a 300 MB file. The header alone settles most demos:
	//
	//   * At or above kDecalBugFixedFromProtocol the demo was recorded by a client that no
	//     longer emits the legacy decal message the repair exists to strip, so it CANNOT need
	//     repairing. Skipping the scan is what stops a current demo showing "Repairing Demo"
	//     on its first play -- the tester complaint -- rather than merely remembering the
	//     verdict afterwards.
	//   * At or below kRejectedAtOrBelowProtocol this client refuses the demo outright, and
	//     repair cannot change that: it strips messages, it does not rewrite the protocol.
	//     Measured on the local library -- a 14141 demo and its repaired copy carry the same
	//     protocol, and both are rejected with "Demo file is incompatible with this game
	//     version". Say so instead of spending seconds pretending otherwise.
	//
	// Both constants are CS2-version-coupled; see Docs/agent/cs2-update-recovery.md. Between
	// them the answer is genuinely unknown, so those demos still get the full scan.
	const DemoHeaderInfo header = ReadDemoHeader(demoPath);
	if (header.ok && header.networkProtocol > 0) {
		char note[192];
		const DemoCompatibility compatibility = DemoRepair_ClassifyProtocol(header.networkProtocol);
		if (compatibility == DemoCompatibility::Supported) {
			snprintf(note, sizeof(note), "demo is current (protocol %d, build %d); no check needed",
				header.networkProtocol, header.buildNum);
			std::lock_guard<std::mutex> lock(g_mx);
			g_activeDemo = demoPath;
			g_resolved = demoPath;
			g_state = RepairState::Ready;
			g_status = note;
			return demoPath;
		}
		if (compatibility == DemoCompatibility::Unsupported) {
			snprintf(note, sizeof(note),
				"recorded by CS2 build %d (protocol %d), which this client refuses; repair cannot fix a protocol mismatch",
				header.buildNum, header.networkProtocol);
			advancedfx::Warning("mirv_filmmaker: %s\n", note);
			std::lock_guard<std::mutex> lock(g_mx);
			g_activeDemo = demoPath;
			g_resolved = demoPath; // still hand back the original: let CS2 give its own verdict
			g_state = RepairState::Unsupported;
			g_status = note;
			return demoPath;
		}
	}

	// Known clean from a previous session (marker matches the demo's current size+mtime): play
	// the original immediately instead of re-parsing the whole file again.
	if (!repaired.empty() && HasValidCleanMarker(demoPath, repaired))
		return demoPath;

	{
		std::lock_guard<std::mutex> lock(g_mx);
		// A finished decision for THIS demo.
		if (g_activeDemo == demoPath && !g_resolved.empty())
			return g_resolved;
		// Work already in flight for this demo: caller should poll again.
		if (g_activeDemo == demoPath && g_busy.load())
			return std::wstring();
	}

	const std::wstring exe = FindRepairExe();
	if (exe.empty()) {
		// No helper: play the original. A demo that needs repair will still parse-error, but
		// that is strictly better than refusing to play anything.
		SetState(RepairState::Failed, "FilmmakerDemoRepair.exe not found; playing the demo unrepaired");
		std::lock_guard<std::mutex> lock(g_mx);
		g_activeDemo = demoPath;
		g_resolved = demoPath;
		return demoPath;
	}

	bool expected = false;
	if (!g_busy.compare_exchange_strong(expected, true))
		return std::wstring(); // another demo is mid-repair; poll again

	{
		std::lock_guard<std::mutex> lock(g_mx);
		g_activeDemo = demoPath;
		g_resolved.clear();
		// Checking, not Repairing: the single -in/-out pass is a scan that only WRITES when
		// something needs stripping, and most demos need nothing. Announcing "repairing" here
		// told every user their clean demo was being repaired.
		g_state = RepairState::Checking;
		g_status = "checking demo for the CS2 14172 playback bug...";
	}
    // compare_exchange above proved there is no active job. A joinable worker here is therefore
    // the completed previous job; reap it before assigning the next std::thread.
    if (g_worker.joinable()) g_worker.join();

	g_worker = std::thread([exe, demoPath]() {
		const std::wstring repaired = DemoProjectStore_GeneratedForSave(
			ProjectGame::Cs2, demoPath, ProjectGenerated::Repaired);
		if (repaired.empty()) {
			SetState(RepairState::Failed, "could not create the demo project folder; playing unrepaired");
			std::lock_guard<std::mutex> lock(g_mx);
			g_resolved = demoPath;
			g_busy.store(false);
			return;
		}
		SetState(RepairState::Checking, "checking demo for the CS2 playback bug...");
		advancedfx::Message("mirv_filmmaker: checking demo for the CS2 playback bug...\n");
		// ONE pass: -out parses the whole demo and writes only when something was actually
		// stripped (exit 0 either way; non-zero only on a real failure). The old flow ran a
		// full -check pass first and then parsed the whole file AGAIN for -out -- two passes
		// over a 300 MB demo. The check pass probed for exactly the failures -out already
		// reports, so dropping it halves the repair wait.
		const int repairCode = RunHelper(exe, L"-in \"" + demoPath + L"\" -out \"" + repaired + L"\"");

		std::lock_guard<std::mutex> lock(g_mx);
		std::string invalidWhy;
		if (repairCode == 0 && FileExists(repaired)
				&& !RepairedCopyLooksValid(demoPath, repaired, invalidWhy)) {
			// Never cache or play a copy we could not verify. Delete it so the next attempt starts
			// clean rather than trusting it on filename alone, and fall back to the original.
			DeleteFileW(repaired.c_str());
			DeleteFileW(RepairedMarkerPath(repaired).c_str());
			g_resolved = demoPath;
			g_state = RepairState::Failed;
			g_status = "repair produced an invalid demo (" + invalidWhy + "); playing the original";
			advancedfx::Warning("mirv_filmmaker: %s\n", g_status.c_str());
			g_busy.store(false);
			return;
		}
		if (repairCode == 0 && FileExists(repaired)) {
			WriteRepairedMarker(demoPath, repaired, exe); // so the next session can trust this copy
			g_resolved = repaired;
			g_state = RepairState::Ready;
			g_status = "demo repaired and validated";
			advancedfx::Message("mirv_filmmaker: demo repaired -> %s\n", WideToUtf8(repaired).c_str());
		} else if (repairCode == 0) {
			// No output written == nothing to strip == the demo is already fine. Remember that
			// verdict on disk so the next session never re-scans this demo.
			WriteCleanMarker(demoPath, repaired);
			g_resolved = demoPath;
			g_state = RepairState::Ready;
			g_status = "demo needs no repair";
			advancedfx::Message("mirv_filmmaker: demo needs no repair.\n");
		} else {
			g_resolved = demoPath;
			g_state = RepairState::Failed;
			g_status = "repair failed; playing the demo unrepaired";
			advancedfx::Warning("mirv_filmmaker: demo repair failed; playing unrepaired.\n");
		}
		g_busy.store(false);
	});

	return std::wstring(); // poll again next frame
}

RepairState DemoRepair_State() {
	std::lock_guard<std::mutex> lock(g_mx);
	return g_state;
}

std::string DemoRepair_StatusText() {
	std::lock_guard<std::mutex> lock(g_mx);
	return g_status;
}

std::wstring DemoRepair_ActiveDemo() {
	std::lock_guard<std::mutex> lock(g_mx);
	return g_activeDemo;
}


} // namespace Filmmaker
