#include "DemoKeyframes.h"

#include "DemoHeaderReader.h"
#include "DemoKeyframesMath.h"
#include "DemoProjectStore.h"
#include "DemoRepair.h" // DemoRepair_ResolvePlayable: the source of a rebuild during a session
#include "../Platform/TextEncoding.h"
#include "../../../Shared/AfxConsole.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace Filmmaker {

namespace {

std::mutex g_mx;
KeyframeState g_state = KeyframeState::Idle;
std::string g_status;
std::wstring g_activeDemo;
std::thread g_worker;
std::atomic<bool> g_busy{ false };
// The file the engine is playing, as DemoKeyframes_RunFrame last saw it (under g_mx). CS2 opens a
// demo with FILE_SHARE_DELETE, so deleting the copy it plays SUCCEEDS: the copy's .src went with
// it, the playing file no longer resolved to its demo (ResolveDemoProjectPath), the camera path
// took that for a new demo and dropped every marker, and KeepOnly then deleted the demo's fresh
// keyframes as another demo's (mvm_debug 2026-10-17 08:19:34). No slot it plays is ever touched.
std::wstring g_playingFile;

std::wstring PlayingFile() {
	std::lock_guard<std::mutex> lock(g_mx);
	return g_playingFile;
}

bool FileExists(const std::wstring& p) {
	const DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

unsigned long long FileSize(const std::wstring& p) {
	WIN32_FILE_ATTRIBUTE_DATA fad{};
	if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return 0;
	return ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

std::string FileStamp(const std::wstring& p) {
	WIN32_FILE_ATTRIBUTE_DATA fad{};
	if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return std::string();
	const ULONGLONG size = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
	const ULONGLONG mtime = ((ULONGLONG)fad.ftLastWriteTime.dwHighDateTime << 32)
		| fad.ftLastWriteTime.dwLowDateTime;
	return std::to_string(size) + ":" + std::to_string(mtime);
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

// Same lookup shape as the repair and info helpers: env override, then the staged layouts.
std::wstring FindKeyframesExe() {
	wchar_t over[MAX_PATH];
	const DWORD n = GetEnvironmentVariableW(L"MIRV_FILMMAKER_DEMOKEYFRAMES", over, MAX_PATH);
	if (n > 0 && n < MAX_PATH && FileExists(over)) return std::wstring(over, n);
	const std::wstring dir = SelfModuleDir();
	if (dir.empty()) return std::wstring();
	const wchar_t* rels[] = {
		L"\\FilmmakerDemoKeyframes\\FilmmakerDemoKeyframes.exe",
		L"\\FilmmakerDemoKeyframes.exe",
		L"\\..\\FilmmakerDemoKeyframes\\FilmmakerDemoKeyframes.exe",
	};
	for (const wchar_t* rel : rels) {
		std::wstring cand = dir + rel;
		if (FileExists(cand)) return cand;
	}
	return std::wstring();
}

// "<original size:mtime>|<helper size:mtime>|<dense windows>" beside the copy. A copy of a
// re-recorded demo, or one written by an older helper, is rebuilt rather than trusted by name; the
// windows only say where the copy is dense (KeyframeWindows::Format), never whether it is valid.
std::wstring MarkerPath(const std::wstring& copyPath) { return copyPath + L".src"; }

std::string ReadMarker(const std::wstring& copyPath) {
	FILE* f = _wfopen(MarkerPath(copyPath).c_str(), L"rb");
	if (!f) return std::string();
	std::string stored;
	char buf[4096];
	for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) stored.append(buf, n);
	fclose(f);
	return stored;
}

bool MarkerValid(const std::wstring& demoPath, const std::wstring& copyPath, const std::wstring& exePath) {
	const std::string demoStamp = FileStamp(demoPath);
	if (demoStamp.empty()) return false;
	const std::string stored = ReadMarker(copyPath);
	const size_t bar = stored.find('|');
	if (bar == std::string::npos || stored.substr(0, bar) != demoStamp) return false;
	// With no helper on disk an existing copy still beats the slow original.
	const std::string exeStamp = FileStamp(exePath);
	if (exePath.empty() || exeStamp.empty()) return true;
	const size_t bar2 = stored.find('|', bar + 1);
	return stored.substr(bar + 1, bar2 == std::string::npos ? std::string::npos : bar2 - bar - 1) == exeStamp;
}

std::vector<KeyframeWindows::Window> MarkerWindows(const std::wstring& copyPath) {
	const std::string stored = ReadMarker(copyPath);
	const size_t bar = stored.find('|');
	const size_t bar2 = bar == std::string::npos ? bar : stored.find('|', bar + 1);
	if (bar2 == std::string::npos) return {};
	return KeyframeWindows::Parse(stored.substr(bar2 + 1));
}

void WriteMarker(const std::wstring& demoPath, const std::wstring& copyPath, const std::wstring& exePath,
	const std::string& windows) {
	const std::string value = FileStamp(demoPath) + "|" + FileStamp(exePath) + "|" + windows;
	FILE* f = _wfopen(MarkerPath(copyPath).c_str(), L"wb");
	if (!f) return;
	fwrite(value.data(), 1, value.size(), f);
	fclose(f);
}

// Removes one slot and everything a build leaves beside it. False when the slot itself is still
// there (the engine, or another game instance, holds it open).
bool DeleteSlot(const std::wstring& copy) {
	DeleteFileW((copy + L".part").c_str());
	DeleteFileW((copy + L".log").c_str());
	if (FileExists(copy) && !DeleteFileW(copy.c_str())) return false;
	DeleteFileW(MarkerPath(copy).c_str());
	return true;
}

void SetState(KeyframeState s, const std::string& text) {
	std::lock_guard<std::mutex> lock(g_mx);
	g_state = s;
	g_status = text;
}

// The helper's output goes to `logPath`: on failure its last line is "FAILED: <why>". Only that file's
// handle is inherited, never the game's own handles.
int RunHelper(const std::wstring& exe, const std::wstring& args, const std::wstring& logPath) {
	std::wstring cmd = L"\"" + exe + L"\" " + args;
	std::vector<wchar_t> buf(cmd.begin(), cmd.end());
	buf.push_back(L'\0');
	SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
	HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, &inherit,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	STARTUPINFOEXW si{};
	si.StartupInfo.cb = sizeof(si);
	si.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
	si.StartupInfo.wShowWindow = SW_HIDE;
	std::vector<unsigned char> attributes;
	if (log != INVALID_HANDLE_VALUE) {
		SIZE_T bytes = 0;
		InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
		attributes.resize(bytes);
		auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
		if (InitializeProcThreadAttributeList(list, 1, 0, &bytes) &&
			UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &log, sizeof(log), nullptr, nullptr)) {
			si.lpAttributeList = list;
			si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
			si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = log;
		}
	}
	PROCESS_INFORMATION pi{};
	// Below normal: it is background work beside a running game.
	const bool redirected = si.lpAttributeList != nullptr;
	const BOOL started = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, redirected,
		CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | (redirected ? EXTENDED_STARTUPINFO_PRESENT : 0),
		nullptr, nullptr, &si.StartupInfo, &pi);
	if (redirected) DeleteProcThreadAttributeList(si.lpAttributeList);
	if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
	if (!started) return -1;
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return (int)code;
}

// The last non-empty line of the helper's log, shortened for a console line: "" when there is none.
std::string LastLogLine(const std::wstring& logPath) {
	FILE* f = _wfopen(logPath.c_str(), L"rb");
	if (!f) return std::string();
	std::string text;
	char chunk[4096];
	for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0;) {
		text.append(chunk, n);
		if (text.size() > 64 * 1024) text.erase(0, text.size() - 16 * 1024);
	}
	fclose(f);
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
	const size_t start = text.find_last_of('\n');
	std::string line = start == std::string::npos ? text : text.substr(start + 1);
	if (!line.empty() && line.back() == '\r') line.pop_back();
	if (line.size() > 200) line = line.substr(0, 200) + "...";
	return line;
}

} // namespace

namespace {

unsigned long long LastWrite(const std::wstring& p) {
	WIN32_FILE_ATTRIBUTE_DATA d{};
	if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
	return (static_cast<unsigned long long>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
}

const ProjectGenerated kSlots[2] = { ProjectGenerated::Keyframed, ProjectGenerated::KeyframedB };

std::wstring SlotPath(const std::wstring& demoPath, int slot, bool forSave) {
	return forSave ? DemoProjectStore_GeneratedForSave(ProjectGame::Cs2, demoPath, kSlots[slot])
		: DemoProjectStore_GeneratedForLoad(ProjectGame::Cs2, demoPath, kSlots[slot]);
}

// Newest valid slot: -1 when there is none. A rebuild always writes the newer one with the windows
// the markers needed when it started, so newest is also best covered.
int ReadySlot(const std::wstring& demoPath) {
	if (demoPath.empty()) return -1;
	const std::wstring exe = FindKeyframesExe();
	int best = -1;
	unsigned long long bestTime = 0;
	for (int slot = 0; slot < 2; ++slot) {
		const std::wstring copy = SlotPath(demoPath, slot, false);
		if (copy.empty() || !FileExists(copy) || !MarkerValid(demoPath, copy, exe)) continue;
		const unsigned long long t = LastWrite(MarkerPath(copy));
		if (best < 0 || t > bestTime) { best = slot; bestTime = t; }
	}
	return best;
}

// The engine reports its own spelling of the playing file (forward slashes, its own case).
bool SamePath(std::wstring a, std::wstring b) {
	if (a.empty() || b.empty()) return false;
	for (auto& c : a) if (c == L'/') c = L'\\';
	for (auto& c : b) if (c == L'/') c = L'\\';
	return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// Set by a finished build, read by DemoKeyframes_RunFrame on the main thread.
std::atomic<bool> g_announce{ false };
std::wstring g_announceDemo; // guarded by g_mx

} // namespace

std::wstring DemoKeyframes_ReadyPath(const std::wstring& demoPath) {
	const int slot = ReadySlot(demoPath);
	return slot < 0 ? std::wstring() : SlotPath(demoPath, slot, false);
}

bool DemoKeyframes_HelperAvailable() { return !FindKeyframesExe().empty(); }

// ONE COPY. Every keyframed copy is ~1.8x its demo and nothing ever removed them, so a few weeks
// of demos held gigabytes of copies the user never asked for, and a three-copy LRU still held two
// copies nobody had open. Only the demo being opened keeps one; a demo whose copy was deleted
// rebuilds it in the background the next time it plays.
void DemoKeyframes_KeepOnly(const std::wstring& demoPath, const std::wstring& playingFile) {
	const std::wstring keepDir = DemoProjectStore_ProjectDirectory(ProjectGame::Cs2, demoPath);
	const size_t slash = keepDir.find_last_of(L"\\/");
	if (keepDir.empty() || slash == std::wstring::npos) return;
	const std::wstring gameDir = keepDir.substr(0, slash);

	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((gameDir + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
		const std::wstring dir = gameDir + L"\\" + fd.cFileName;
		if (_wcsicmp(dir.c_str(), keepDir.c_str()) == 0) continue;
		bool removed = false;
		for (int slot = 0; slot < 2; ++slot) {
			const std::wstring copy = dir + L"\\" + ProjectGeneratedFileName(kSlots[slot]);
			if (SamePath(copy, playingFile)) continue;
			if (FileExists(copy) || FileExists(MarkerPath(copy)) || FileExists(copy + L".part"))
				removed = DeleteSlot(copy) || removed;
		}
		if (removed)
			advancedfx::Message("mirv_filmmaker: removed the scrub keyframes of another demo (%s)\n",
				WideToUtf8(dir).c_str());
	} while (FindNextFileW(h, &fd));
	FindClose(h);

	// This demo: the newest valid slot and the one the engine is playing stay. A superseded slot
	// goes once the engine lets go of it (the next RunFrame after the play switches).
	const int ready = ReadySlot(demoPath);
	for (int slot = 0; slot < 2; ++slot) {
		if (slot == ready) continue;
		const std::wstring copy = SlotPath(demoPath, slot, false);
		if (copy.empty() || SamePath(copy, playingFile) || !FileExists(copy)) continue;
		if (g_busy.load()) continue; // a build may be writing this slot right now
		DeleteSlot(copy);
	}
}

void DemoKeyframes_Build(const std::wstring& demoPath, const std::wstring& sourcePath,
	const std::vector<int>& markerTicks) {
	if (demoPath.empty() || sourcePath.empty()) return;
	const int ready = ReadySlot(demoPath);
	if (ready >= 0 && KeyframeWindows::Covers(MarkerWindows(SlotPath(demoPath, ready, false)), markerTicks))
		return;
	const std::wstring exe = FindKeyframesExe();
	if (exe.empty()) {
		SetState(KeyframeState::Failed, "FilmmakerDemoKeyframes.exe not found; backward seeks stay slow");
		return;
	}
	bool expected = false;
	if (!g_busy.compare_exchange_strong(expected, true))
		return; // one build at a time; the next play, prepare or marker change asks again
	if (g_worker.joinable()) g_worker.join();
	{
		std::lock_guard<std::mutex> lock(g_mx);
		g_activeDemo = demoPath;
		g_state = KeyframeState::Building;
		g_status = "building scrub keyframes...";
	}
	const KeyframeWindows::Plan plan = KeyframeWindows::PlanFor(markerTicks);
	// The slot that is not the ready one. The engine may still be playing it (the copy a rebuild
	// superseded, until the demo is reopened): then there is no slot to write, and the next play
	// builds instead. Overwriting it would pull the demo out from under the engine.
	const int target = ready == 0 ? 1 : 0;
	if (SamePath(SlotPath(demoPath, target, false), PlayingFile())) {
		g_busy.store(false);
		SetState(KeyframeState::Idle, "scrub keyframes: the copy being played is in use; reopen the demo to rebuild");
		return;
	}
	g_worker = std::thread([exe, demoPath, sourcePath, plan, target, ready]() {
		const std::wstring copy = SlotPath(demoPath, target, true);
		if (copy.empty()) {
			SetState(KeyframeState::Failed, "could not create the demo project folder");
			g_busy.store(false);
			return;
		}
		// One copy: the other demos' go before the helper writes another few hundred MB.
		DemoKeyframes_KeepOnly(demoPath, PlayingFile());
		if (!DeleteSlot(copy)) {
			SetState(KeyframeState::Failed, "scrub keyframes: the previous copy is still open");
			g_busy.store(false);
			return;
		}
		// Write beside the final name, then swap in: a half-written copy is never visible under
		// the name the next play would pick up.
		const std::wstring temp = copy + L".part";
		const auto t0 = std::chrono::steady_clock::now();
		const std::string windows = KeyframeWindows::Format(plan.windows);
		advancedfx::Message("mirv_filmmaker: building scrub keyframes for %s (dense %s)...\n",
			WideToUtf8(demoPath).c_str(), windows.empty() ? "nowhere yet: no camera markers" : windows.c_str());
		const std::wstring log = copy + L".log";
		std::wstring args = L"-verify-every 16";
		if (!plan.windows.empty())
			args += L" -dense " + Utf8ToWide(windows) + L" -dense-every " + std::to_wstring(plan.step);
		const int code = RunHelper(exe, args + L" \"" + sourcePath + L"\" \"" + temp + L"\"", log);
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		// A player report only said "exit 1" (2026-10-15); the helper's own last line says why.
		const std::string helperSaid = code != 0 ? LastLogLine(log) : std::string();
		DeleteFileW(log.c_str());

		const DemoHeaderInfo src = ReadDemoHeader(sourcePath);
		const DemoHeaderInfo out = ReadDemoHeader(temp);
		std::string why;
		if (code != 0) why = "the helper failed (exit " + std::to_string(code) + (helperSaid.empty() ? "" : ": " + helperSaid) + ")";
		else if (!out.ok) why = "the copy has no readable demo header";
		else if (src.ok && out.map != src.map) why = "the copy names a different map";
		else if (FileSize(temp) < FileSize(sourcePath)) why = "the copy is smaller than its source";
		if (!why.empty()) {
			DeleteFileW(temp.c_str());
			SetState(KeyframeState::Failed, "scrub keyframes failed: " + why);
			advancedfx::Warning("mirv_filmmaker: scrub keyframes failed: %s\n", why.c_str());
			g_busy.store(false);
			return;
		}
		DeleteFileW(MarkerPath(copy).c_str());
		if (!MoveFileExW(temp.c_str(), copy.c_str(), MOVEFILE_REPLACE_EXISTING)) {
			DeleteFileW(temp.c_str());
			SetState(KeyframeState::Failed, "scrub keyframes: could not replace the previous copy (in use?)");
			g_busy.store(false);
			return;
		}
		WriteMarker(demoPath, copy, exe, windows);
		// The slot this one supersedes goes now unless the engine is playing it; then it goes once
		// the engine lets go of it (DemoKeyframes_KeepOnly from the next play). Its open handle does
		// not stop the delete (see g_playingFile).
		if (ready >= 0) {
			const std::wstring superseded = SlotPath(demoPath, ready, false);
			if (!SamePath(superseded, PlayingFile())) DeleteSlot(superseded);
		}
		char text[200];
		snprintf(text, sizeof(text), "scrub keyframes ready (%.0f s, %.0f MB%s); used from the next play of this demo",
			secs, (double)FileSize(copy) / 1e6, windows.empty() ? "" : ", dense around the camera markers");
		SetState(KeyframeState::Ready, text);
		advancedfx::Message("mirv_filmmaker: %s\n", text);
		{
			std::lock_guard<std::mutex> lock(g_mx);
			g_announceDemo = demoPath;
		}
		g_announce.store(true);
		g_busy.store(false);
	});
}

bool DemoKeyframes_RunFrame(const std::wstring& demoPath, const std::wstring& playingFile,
	const std::vector<int>& markerTicks, bool renderJobActive) {
	// Markers settle before a rebuild starts: placing five keys must not start five builds.
	constexpr unsigned long long kSettleMs = 5000;
	static std::wstring s_demo, s_playing;
	static std::vector<int> s_ticks;
	static unsigned long long s_changedAtMs = 0;
	const unsigned long long now = GetTickCount64();
	{
		std::lock_guard<std::mutex> lock(g_mx);
		if (g_playingFile != playingFile) g_playingFile = playingFile;
	}

	if (!SamePath(demoPath, s_demo) || !SamePath(playingFile, s_playing)) {
		// A new demo, or the same demo on its other slot: the open-time build already covered the
		// markers saved with it, so only later changes count. Tidy what the engine just let go of.
		const bool sameDemo = SamePath(demoPath, s_demo);
		s_demo = demoPath;
		s_playing = playingFile;
		if (!sameDemo) { s_ticks = markerTicks; s_changedAtMs = 0; }
		if (!demoPath.empty()) DemoKeyframes_KeepOnly(demoPath, playingFile);
	}
	if (demoPath.empty()) return false;
	if (markerTicks != s_ticks) {
		s_ticks = markerTicks;
		s_changedAtMs = now;
	}

	if (s_changedAtMs != 0 && now - s_changedAtMs >= kSettleMs && !renderJobActive && !g_busy.load()) {
		s_changedAtMs = 0;
		const int ready = ReadySlot(demoPath);
		if (ready < 0 || !KeyframeWindows::Covers(MarkerWindows(SlotPath(demoPath, ready, false)), markerTicks)) {
			const std::wstring source = DemoRepair_ResolvePlayable(demoPath);
			if (!source.empty()) DemoKeyframes_Build(demoPath, source, markerTicks);
		}
	}

	if (g_announce.load()) {
		std::wstring finished;
		{
			std::lock_guard<std::mutex> lock(g_mx);
			finished = g_announceDemo;
		}
		g_announce.store(false);
		// Only worth saying while that demo is open on something other than the new copy.
		const std::wstring copy = DemoKeyframes_ReadyPath(demoPath);
		if (SamePath(finished, demoPath) && !SamePath(copy, playingFile)) {
			advancedfx::Message("mirv_filmmaker: finer scrub keyframes are ready for this demo -- reopen it to scrub with them.\n");
			return true;
		}
	}
	return false;
}

KeyframeState DemoKeyframes_State() {
	std::lock_guard<std::mutex> lock(g_mx);
	return g_state;
}

std::string DemoKeyframes_StatusText() {
	std::lock_guard<std::mutex> lock(g_mx);
	return g_status;
}

void DemoKeyframes_Reset() {
	if (g_worker.joinable()) g_worker.join();
	g_busy.store(false);
	std::lock_guard<std::mutex> lock(g_mx);
	g_state = KeyframeState::Idle;
	g_status.clear();
	g_activeDemo.clear();
}

void DemoKeyframes_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	(void)argc;
	(void)args;
	std::wstring active;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		active = g_activeDemo;
	}
	advancedfx::Message("%s keyframes: %s%s%s\n  helper: %s\n", cmd,
		DemoKeyframes_StatusText().empty() ? "idle" : DemoKeyframes_StatusText().c_str(),
		active.empty() ? "" : " -- ", WideToUtf8(active).c_str(),
		DemoKeyframes_HelperAvailable() ? "found" : "MISSING (FilmmakerDemoKeyframes.exe)");
}


} // namespace Filmmaker
