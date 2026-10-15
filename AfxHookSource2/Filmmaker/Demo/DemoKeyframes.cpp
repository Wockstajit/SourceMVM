#include "DemoKeyframes.h"

#include "DemoHeaderReader.h"
#include "DemoProjectStore.h"
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

// "<original size:mtime>|<helper size:mtime>" beside the copy. A copy of a re-recorded demo, or
// one written by an older helper, is rebuilt rather than trusted by name.
std::wstring MarkerPath(const std::wstring& copyPath) { return copyPath + L".src"; }

bool MarkerValid(const std::wstring& demoPath, const std::wstring& copyPath, const std::wstring& exePath) {
	const std::string demoStamp = FileStamp(demoPath);
	if (demoStamp.empty()) return false;
	FILE* f = _wfopen(MarkerPath(copyPath).c_str(), L"rb");
	if (!f) return false;
	char buf[256]{};
	const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	const std::string stored(buf, n);
	const size_t bar = stored.find('|');
	if (bar == std::string::npos || stored.substr(0, bar) != demoStamp) return false;
	// With no helper on disk an existing copy still beats the slow original.
	const std::string exeStamp = FileStamp(exePath);
	if (exePath.empty() || exeStamp.empty()) return true;
	return stored.substr(bar + 1) == exeStamp;
}

void WriteMarker(const std::wstring& demoPath, const std::wstring& copyPath, const std::wstring& exePath) {
	const std::string value = FileStamp(demoPath) + "|" + FileStamp(exePath);
	FILE* f = _wfopen(MarkerPath(copyPath).c_str(), L"wb");
	if (!f) return;
	fwrite(value.data(), 1, value.size(), f);
	fclose(f);
}

void SetState(KeyframeState s, const std::string& text) {
	std::lock_guard<std::mutex> lock(g_mx);
	g_state = s;
	g_status = text;
}

int RunHelper(const std::wstring& exe, const std::wstring& args) {
	std::wstring cmd = L"\"" + exe + L"\" " + args;
	std::vector<wchar_t> buf(cmd.begin(), cmd.end());
	buf.push_back(L'\0');
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi{};
	// Below normal: it is background work beside a running game.
	if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi))
		return -1;
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return (int)code;
}

} // namespace

namespace {

// Every keyframed copy is ~1.8x its demo and nothing ever removed them, so a few weeks of demos
// held gigabytes of copies the user never asked for. At most kKeptKeyframedCopies exist: starting
// a build for a demo without one deletes the least recently used others first. A demo whose copy
// was deleted rebuilds it in the background the next time it plays.
constexpr size_t kKeptKeyframedCopies = 3;

unsigned long long LastWrite(const std::wstring& p) {
	WIN32_FILE_ATTRIBUTE_DATA d{};
	if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
	return (static_cast<unsigned long long>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
}

void PruneKeyframedCopies(const std::wstring& keepDemoPath) {
	const std::wstring keepDir = DemoProjectStore_ProjectDirectory(ProjectGame::Cs2, keepDemoPath);
	const size_t slash = keepDir.find_last_of(L"\\/");
	if (keepDir.empty() || slash == std::wstring::npos) return;
	const std::wstring gameDir = keepDir.substr(0, slash);
	const std::wstring keyframedName = ProjectGeneratedFileName(ProjectGenerated::Keyframed);

	struct Copy { std::wstring dir; unsigned long long used; };
	std::vector<Copy> copies;
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((gameDir + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
		const std::wstring dir = gameDir + L"\\" + fd.cFileName;
		if (_wcsicmp(dir.c_str(), keepDir.c_str()) == 0) continue; // the copy just built stays
		const std::wstring copy = dir + L"\\" + keyframedName;
		if (!FileExists(copy)) continue;
		// "Used" = the project file, rewritten whenever the demo is opened, or the copy itself.
		const unsigned long long used = (std::max)(LastWrite(dir + L"\\project.mvm"), LastWrite(copy));
		copies.push_back({ dir, used });
	} while (FindNextFileW(h, &fd));
	FindClose(h);

	if (copies.size() + 1 <= kKeptKeyframedCopies) return;
	std::sort(copies.begin(), copies.end(), [](const Copy& a, const Copy& b) { return a.used > b.used; });
	for (size_t i = kKeptKeyframedCopies - 1; i < copies.size(); ++i) {
		const std::wstring copy = copies[i].dir + L"\\" + keyframedName;
		// A copy that is open (a second game instance playing it) stays until it is free.
		if (DeleteFileW(copy.c_str())) {
			DeleteFileW(MarkerPath(copy).c_str());
			advancedfx::Message("mirv_filmmaker: removed the scrub keyframes of a less recently used demo (%s)\n",
				WideToUtf8(copies[i].dir).c_str());
		}
	}
}

} // namespace

std::wstring DemoKeyframes_ReadyPath(const std::wstring& demoPath) {
	if (demoPath.empty()) return std::wstring();
	const std::wstring copy = DemoProjectStore_GeneratedForLoad(ProjectGame::Cs2, demoPath, ProjectGenerated::Keyframed);
	if (copy.empty() || !FileExists(copy) || !MarkerValid(demoPath, copy, FindKeyframesExe()))
		return std::wstring();
	return copy;
}

bool DemoKeyframes_HelperAvailable() { return !FindKeyframesExe().empty(); }

void DemoKeyframes_Build(const std::wstring& demoPath, const std::wstring& sourcePath) {
	if (demoPath.empty() || sourcePath.empty()) return;
	if (!DemoKeyframes_ReadyPath(demoPath).empty()) return;
	const std::wstring exe = FindKeyframesExe();
	if (exe.empty()) {
		SetState(KeyframeState::Failed, "FilmmakerDemoKeyframes.exe not found; backward seeks stay slow");
		return;
	}
	bool expected = false;
	if (!g_busy.compare_exchange_strong(expected, true))
		return; // one build at a time; the next play or prepare asks again
	if (g_worker.joinable()) g_worker.join();
	{
		std::lock_guard<std::mutex> lock(g_mx);
		g_activeDemo = demoPath;
		g_state = KeyframeState::Building;
		g_status = "building scrub keyframes...";
	}
	g_worker = std::thread([exe, demoPath, sourcePath]() {
		const std::wstring copy = DemoProjectStore_GeneratedForSave(ProjectGame::Cs2, demoPath, ProjectGenerated::Keyframed);
		if (copy.empty()) {
			SetState(KeyframeState::Failed, "could not create the demo project folder");
			g_busy.store(false);
			return;
		}
		// Make room first: at most kKeptKeyframedCopies copies exist, this new one included, so the
		// least recently used ones go before the helper writes another ~0.5 GB.
		PruneKeyframedCopies(demoPath);
		// Write beside the final name, then swap in: a half-written copy is never visible under
		// the name the next play would pick up.
		const std::wstring temp = copy + L".part";
		const auto t0 = std::chrono::steady_clock::now();
		advancedfx::Message("mirv_filmmaker: building scrub keyframes for %s...\n", WideToUtf8(demoPath).c_str());
		const int code = RunHelper(exe, L"-verify-every 16 \"" + sourcePath + L"\" \"" + temp + L"\"");
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

		const DemoHeaderInfo src = ReadDemoHeader(sourcePath);
		const DemoHeaderInfo out = ReadDemoHeader(temp);
		std::string why;
		if (code != 0) why = "the helper failed (exit " + std::to_string(code) + ")";
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
		WriteMarker(demoPath, copy, exe);
		char text[160];
		snprintf(text, sizeof(text), "scrub keyframes ready (%.0f s); used from the next play of this demo", secs);
		SetState(KeyframeState::Ready, text);
		advancedfx::Message("mirv_filmmaker: %s\n", text);
		g_busy.store(false);
	});
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
