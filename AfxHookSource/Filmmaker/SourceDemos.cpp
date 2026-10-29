#include "stdafx.h"

#include "SourceDemos.h"

#include "DemoConvert/ConvertV34.h"
#include "SourceDemoFile.h"
#include "SourceDemoIndex.h"
#include "SourceEngine.h"
#include "SourceOldDemos.h"

#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Config/DemoFileStore.h>
#include <AfxHookSource2/Filmmaker/Config/DemoFolderStore.h>
#include <AfxHookSource2/Filmmaker/Config/DemoOrganizerStore.h>
#include <AfxHookSource2/Filmmaker/Config/HiddenDemoStore.h>
#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <AfxHookSource2/Filmmaker/Demo/DemoProjectStore.h>
#include <AfxHookSource2/Filmmaker/Demo/DemoScanner.h>
#include <AfxHookSource2/Filmmaker/Platform/DemoFilePicker.h>
#include <AfxHookSource2/Filmmaker/Platform/FolderPicker.h>
#include <AfxHookSource2/Filmmaker/Platform/ShellOpen.h>
#include <AfxHookSource2/Filmmaker/Platform/TextEncoding.h>
#include <Shared/AfxConsole.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern WrpVEngineClient* g_VEngineClient;

namespace SourceFilmmaker {

namespace {

namespace fs = std::filesystem;
using Ui::DemosBrowserMath::Compat;
using Ui::DemosBrowserMath::DemoRow;

struct Entry {
	std::wstring path; // the .dem, full path
	DemoRow row;       // what its file and header say; the user's side is added per model
};

// The scan's result, published by the worker.
std::mutex g_mutex;
std::vector<Entry> g_entries;
uint64_t g_libraryVersion = 0;
double g_scanMs = 0;

std::atomic<bool> g_scanning{ false };
std::atomic<bool> g_cancel{ false };
std::thread g_worker;
// Exiting without Demos_Shutdown must not abort on a finished scan's worker (still joinable): see
// NetconServer.cpp's DetachAtExit. Declared after it, so destroyed first.
struct DetachWorkerAtExit {
	~DetachWorkerAtExit() { if (g_worker.joinable()) g_worker.detach(); }
} g_detachWorkerAtExit;
bool g_scanWanted = false;
bool g_loaded = false;
std::wstring g_gameDir; // cstrike, from the engine

// The user's side: main thread only.
Filmmaker::DemoFolderStore g_folders(L"filmmaker_demo_folders_source1.txt");
Filmmaker::DemoFileStore g_files(L"filmmaker_demo_files_source1.txt");
Filmmaker::HiddenDemoStore g_hidden(L"filmmaker_hidden_demos_source1.txt");
Filmmaker::DemoOrganizerStore g_org(L"filmmaker_demo_organizer_source1.txt");
uint64_t g_userVersion = 0;

// Windows' pickers run on their own threads; what they return waits here for the main thread.
std::atomic<bool> g_picking{ false };
std::mutex g_pickMutex;
std::vector<std::wstring> g_pickedFolders, g_pickedFiles;

// The model last built, by the versions it was built from.
uint64_t g_builtLibrary = ~0ull, g_builtUser = ~0ull;

// Last played: the playing demo, checked once a second.
std::wstring g_lastPlaying;
ULONGLONG g_pollAt = 0;

// An old demo sent to play: the page waits for its conversion, then for it to play.
std::string g_pendingOld;
bool g_convertSeen = false;
bool g_pageDone = false;

std::string g_message;
ULONGLONG g_messageAt = 0;

void Note(const std::string& text) {
	g_message = text;
	g_messageAt = GetTickCount64();
	advancedfx::Message("SOURCE:MVM demos: %s\n", text.c_str());
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("demos", "%s", text.c_str());
}

// The game directory as it is on disk: the engine reports it in lower case, and the page shows the
// folders the scan found under it.
std::wstring GameDir() {
	const char* dir = g_VEngineClient ? g_VEngineClient->GetGameDirectory() : nullptr;
	if (!dir || !*dir) return std::wstring();
	std::wstring path = Filmmaker::Utf8ToWide(dir);
	const HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (h == INVALID_HANDLE_VALUE) return path;
	wchar_t real[MAX_PATH * 2];
	const DWORD n = GetFinalPathNameByHandleW(h, real, (DWORD)(sizeof(real) / sizeof(real[0])), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	CloseHandle(h);
	if (n == 0 || n >= sizeof(real) / sizeof(real[0])) return path;
	std::wstring out(real, n);
	if (out.compare(0, 4, L"\\\\?\\") == 0) out.erase(0, 4);
	return _wcsicmp(out.c_str(), path.c_str()) == 0 ? out : path;
}

bool IsUnder(const std::wstring& path, const std::wstring& folder) {
	if (folder.empty() || path.size() <= folder.size()) return false;
	return _wcsnicmp(path.c_str(), folder.c_str(), folder.size()) == 0 && (path[folder.size()] == L'\\' || path[folder.size()] == L'/');
}

// The demo as playdemo takes it: relative to the game directory with '/' separators, or the full path
// for a demo outside it.
std::string PlayName(const std::wstring& path, const std::wstring& gameDir) {
	std::wstring name = IsUnder(path, gameDir) ? path.substr(gameDir.size() + 1) : path;
	std::replace(name.begin(), name.end(), L'\\', L'/');
	return Filmmaker::WideToUtf8(name);
}

int64_t FileTimeUnix(const std::wstring& path) {
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
	ULARGE_INTEGER t;
	t.LowPart = data.ftLastWriteTime.dwLowDateTime;
	t.HighPart = data.ftLastWriteTime.dwHighDateTime;
	return t.QuadPart > 116444736000000000ull ? (int64_t)((t.QuadPart - 116444736000000000ull) / 10000000ull) : 0;
}

// One demo's row from its file and its header alone.
Entry Read(const std::wstring& path, const std::wstring& gameDir) {
	Entry e;
	e.path = path;
	DemoRow& d = e.row;
	const fs::path p(path);
	d.fileName = Filmmaker::WideToUtf8(p.filename().wstring());
	d.folder = Filmmaker::WideToUtf8(p.parent_path().wstring());
	d.key = Filmmaker::WideToUtf8(Filmmaker::NormalizeDemoKey(path));
	d.date = FileTimeUnix(path);
	unsigned char data[DemoFile::kHeaderBytes] = {};
	size_t size = 0;
	if (FILE* f = _wfopen(path.c_str(), L"rb")) {
		size = std::fread(data, 1, sizeof(data), f);
		std::fclose(f);
	}
	DemoFile::Header h;
	if (!DemoFile::ReadHeader(data, size, h)) return e;
	d.map = h.mapName;
	d.recorder = h.clientName;
	d.server = h.serverName;
	d.seconds = h.playbackTime;
	d.ticks = h.playbackTicks;
	d.protocol = h.networkProtocol;
	if (h.demoProtocol == 3 && h.networkProtocol == 24) d.compat = Compat::Current;
	else if (DemoConvert::IsV34Demo(data, size))
		d.compat = OldDemos_Converted(Filmmaker::WideToUtf8(gameDir), PlayName(path, gameDir)) ? Compat::Converted : Compat::Old;
	else d.compat = Compat::Other;
	return e;
}

void ScanWorker(std::vector<std::wstring> roots, std::vector<std::wstring> skip, std::vector<std::wstring> files, std::wstring gameDir) {
	const auto started = std::chrono::steady_clock::now();
	std::vector<std::wstring> paths = Filmmaker::DemoScanner::Scan(roots, g_cancel, skip);
	std::unordered_set<std::wstring> seen;
	for (const auto& path : paths) seen.insert(Filmmaker::NormalizeDemoKey(path));
	for (const auto& file : files) {
		std::error_code ec;
		if (fs::is_regular_file(fs::path(file), ec) && seen.insert(Filmmaker::NormalizeDemoKey(file)).second) paths.push_back(file);
	}
	std::vector<Entry> entries;
	entries.reserve(paths.size());
	for (const auto& path : paths) {
		if (g_cancel.load()) break;
		entries.push_back(Read(path, gameDir));
	}
	// Newest first, as CS2's list: what the page selects first and the search's order.
	std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
		return a.row.date != b.row.date ? a.row.date > b.row.date : a.row.key < b.row.key;
	});
	if (!g_cancel.load()) {
		std::lock_guard<std::mutex> lock(g_mutex);
		g_entries = std::move(entries);
		++g_libraryVersion;
		g_scanMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	}
	g_scanning = false;
}

void StartScan() {
	if (g_scanning.load()) return;
	if (g_worker.joinable()) g_worker.join();
	g_gameDir = GameDir();
	if (g_gameDir.empty()) return; // the engine is not up yet: the frame retries
	g_scanWanted = false;
	std::vector<std::wstring> roots = { g_gameDir };
	for (const auto& folder : g_folders.Folders()) roots.push_back(folder);
	const std::vector<std::wstring> skip = { g_gameDir + L"\\source_mvm\\converted" };
	g_cancel = false;
	g_scanning = true;
	g_worker = std::thread(ScanWorker, roots, skip, g_files.Files(), g_gameDir);
}

void UserChanged() { ++g_userVersion; }

bool EntryAt(int index, Entry& out) {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (index < 0 || index >= (int)g_entries.size()) return false;
	out = g_entries[(size_t)index];
	return true;
}

void StartPicker(bool folder) {
	bool idle = false;
	if (!g_picking.compare_exchange_strong(idle, true)) return; // one dialog at a time
	std::thread([folder]() {
		std::vector<std::wstring> picked;
		if (folder) {
			std::wstring out;
			if (Filmmaker::PickFolderBlocking(out, L"Select a demo folder")) picked.push_back(out);
		} else {
			Filmmaker::PickDemoFilesBlocking(picked, L"Select demo files", L"Source demos (*.dem)");
		}
		{
			std::lock_guard<std::mutex> lock(g_pickMutex);
			auto& into = folder ? g_pickedFolders : g_pickedFiles;
			into.insert(into.end(), picked.begin(), picked.end());
		}
		g_picking = false;
	}).detach();
}

// A folder to scan from now on; re-adding one brings back the demos removed from under it.
void AddFolder(const std::wstring& folder) {
	g_folders.Add(folder);
	g_folders.Save();
	if (g_hidden.RemoveUnder(folder)) {
		g_hidden.Save();
		UserChanged();
	}
	g_scanWanted = true;
}

void AddFile(const std::wstring& file) {
	g_files.Add(file);
	g_files.Save();
	if (g_hidden.Remove(file)) {
		g_hidden.Save();
		UserChanged();
	}
	g_scanWanted = true;
}

bool Watch(int index) {
	Entry e;
	if (!EntryAt(index, e) || !g_VEngineClient) return false;
	const Compat c = e.row.compat;
	if (c != Compat::Current && c != Compat::Old && c != Compat::Converted) {
		Note(e.row.fileName + " is not a demo this game plays.");
		return false;
	}
	// A demo or map already loaded is left first, as CS2 does; then playdemo, which converts an old
	// demo first (SourceOldDemos.h).
	std::string command = g_VEngineClient->IsConnected() || g_VEngineClient->IsPlayingDemo() ? "disconnect;" : "";
	command += "playdemo \"" + PlayName(e.path, g_gameDir) + "\"";
	g_VEngineClient->ClientCmd_Unrestricted(command.c_str());
	if (g_org.SetLastPlayed(e.path, (int64_t)std::time(nullptr))) {
		g_org.Save();
		UserChanged();
	}
	g_lastPlaying = e.path; // the poll must not count this play twice
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("demos", "watch %d: %s", index, command.c_str());
	if (c == Compat::Old) {
		g_pendingOld = e.row.key;
		g_convertSeen = false;
		return false; // the page shows the conversion, and closes once the copy plays
	}
	return true;
}

// CS2's rename: a sanitised name, the .dem moved first (nothing else moves when that fails), then the
// user's side carried to the new path, then a scan.
void Rename(int index, const std::string& typed) {
	Entry e;
	if (!EntryAt(index, e)) return;
	const std::string file = Ui::DemosBrowserMath::DemoFileName(typed);
	if (file.empty()) {
		Note("that demo name has no usable characters.");
		return;
	}
	const fs::path from(e.path);
	const fs::path to = from.parent_path() / Filmmaker::Utf8ToWide(file);
	if (_wcsicmp(from.filename().c_str(), to.filename().c_str()) == 0) return;
	std::error_code ec;
	if (fs::exists(to, ec)) {
		Note(file + " already exists in that folder.");
		return;
	}
	if (!MoveFileW(from.c_str(), to.c_str())) {
		Note("could not rename the demo (error " + std::to_string(GetLastError()) + "). If it is the demo you are watching, stop playback first.");
		return;
	}
	if (g_org.Rekey(from.wstring(), to.wstring())) g_org.Save();
	if (g_files.Rename(from.wstring(), to.wstring())) g_files.Save();
	Filmmaker::DemoProjectStore_NoteRenamed(Filmmaker::ProjectGame::Css, from.wstring(), to.wstring());
	UserChanged();
	g_scanWanted = true;
}

std::string RestAfter(const std::string& line, int words) {
	size_t at = 0;
	for (int i = 0; i < words; ++i) {
		at = line.find(' ', at);
		if (at == std::string::npos) return std::string();
		++at;
	}
	return line.substr(at);
}

} // namespace

void Demos_Init() {
	if (g_loaded) return;
	g_loaded = true;
	g_folders.Load();
	g_files.Load();
	g_hidden.Load();
	g_org.Load();
	g_scanWanted = true;
}

void Demos_Frame() {
	Demos_Init();
	{
		std::vector<std::wstring> folders, files;
		{
			std::lock_guard<std::mutex> lock(g_pickMutex);
			folders.swap(g_pickedFolders);
			files.swap(g_pickedFiles);
		}
		for (const auto& folder : folders) AddFolder(folder);
		for (const auto& file : files) AddFile(file);
	}
	if (g_scanWanted && !g_scanning.load()) StartScan();

	// An old demo sent to play: once its conversion ends, it plays (and the page closes), or it failed.
	if (!g_pendingOld.empty()) {
		if (OldDemos_Busy()) g_convertSeen = true;
		else if (g_convertSeen) {
			std::string message;
			const bool ok = OldDemos_Last(message);
			g_convertSeen = false;
			g_scanWanted = true; // its badge turns to converted
			if (!ok) {
				Note(message);
				g_pendingOld.clear();
			}
		}
		if (Demo_State().playing && !OldDemos_Busy()) {
			g_pendingOld.clear();
			g_pageDone = true;
		}
	}

	// Last played: any demo that starts, however it was started, as CS2's poll does.
	const ULONGLONG now = GetTickCount64();
	if (now < g_pollAt) return;
	g_pollAt = now + 1000;
	if (!Demo_State().playing) return;
	const DemoIndex index = DemoIndex_Get();
	if (index.file.empty() || g_gameDir.empty()) return;
	std::wstring name = Filmmaker::Utf8ToWide(OldDemos_PlayedName(index.file));
	std::replace(name.begin(), name.end(), L'/', L'\\');
	const std::wstring path = name.size() > 1 && name[1] == L':' ? name : g_gameDir + L"\\" + name;
	if (_wcsicmp(path.c_str(), g_lastPlaying.c_str()) == 0) return;
	g_lastPlaying = path;
	if (g_org.SetLastPlayed(path, (int64_t)std::time(nullptr))) {
		g_org.Save();
		UserChanged();
	}
}

void Demos_Shutdown() {
	g_cancel = true;
	if (g_worker.joinable()) g_worker.join();
}

void Demos_BuildModel(Ui::DemosBrowserModel& m) {
	m.now = (int64_t)std::time(nullptr);
	m.scanning = g_scanning.load() || g_scanWanted;
	m.converting = !g_pendingOld.empty() && OldDemos_Busy();
	m.message = g_messageAt && GetTickCount64() - g_messageAt < 8000 ? g_message : std::string();
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_builtLibrary == g_libraryVersion && g_builtUser == g_userVersion) return;
	g_builtLibrary = g_libraryVersion;
	g_builtUser = g_userVersion;
	std::unordered_map<std::wstring, const Filmmaker::DemoOrganizerRecord*> records;
	for (const auto& r : g_org.Records()) records[r.key] = &r;
	m.demos.clear();
	m.hidden = 0;
	for (size_t i = 0; i < g_entries.size(); ++i) {
		const Entry& e = g_entries[i];
		if (g_hidden.Contains(e.path)) {
			++m.hidden;
			continue;
		}
		DemoRow row = e.row;
		row.index = (int)i;
		const auto it = records.find(Filmmaker::NormalizeDemoKey(e.path));
		if (it != records.end()) {
			row.group = it->second->groupId;
			row.played = it->second->lastPlayed;
		}
		m.demos.push_back(row);
	}
	m.groups.clear();
	for (const auto& g : g_org.Groups()) m.groups.push_back({ g.id, g.name, g.collapsed });
	m.sort = g_org.Sort();
	m.rootCollapsed = g_org.RootCollapsed();
}

bool Demos_Command(const std::string& line) {
	std::istringstream in(line);
	std::string verb;
	in >> verb;
	int index = -1;
	if (verb == "refresh") {
		g_scanWanted = true;
	} else if (verb == "org") {
		if (g_org.Apply(RestAfter(line, 1))) {
			g_org.Save();
			UserChanged();
		}
	} else if (verb == "watch") {
		in >> index;
		return Watch(index);
	} else if (verb == "hide") {
		in >> index;
		Entry e;
		if (EntryAt(index, e) && g_hidden.Add(e.path)) {
			g_hidden.Save();
			UserChanged();
		}
	} else if (verb == "clearall") {
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			for (const Entry& e : g_entries) g_hidden.Add(e.path);
		}
		g_hidden.Save();
		UserChanged();
	} else if (verb == "unhideall") {
		if (g_hidden.Clear()) {
			g_hidden.Save();
			UserChanged();
		}
	} else if (verb == "openfolder") {
		in >> index;
		Entry e;
		if (EntryAt(index, e)) Filmmaker::ShowInExplorerAsync(e.path);
	} else if (verb == "renamedemo") {
		in >> index;
		Rename(index, RestAfter(line, 2));
	} else if (verb == "adddemo") {
		StartPicker(false);
	} else if (verb == "addfolder") {
		StartPicker(true);
	} else {
		advancedfx::Message("mirv_filmmaker demos: unknown command \"%s\".\n", line.c_str());
	}
	return false;
}

bool Demos_PageDone() {
	const bool done = g_pageDone;
	g_pageDone = false;
	return done;
}

bool Demos_ConsoleCommand(int argc, advancedfx::ICommandArgs* args) {
	const std::string sub = argc >= 3 ? args->ArgV(2) : "";
	if (sub == "list") {
		std::lock_guard<std::mutex> lock(g_mutex);
		advancedfx::Message("idx | map | length | protocol | file\n");
		const char* const kCompat[] = { "current", "old", "old, converted", "not played here", "unreadable" };
		for (size_t i = 0; i < g_entries.size(); ++i) {
			const Entry& e = g_entries[i];
			advancedfx::Message("%3d | %s | %s | %d (%s)%s | %s\n", (int)i, e.row.map.empty() ? "?" : e.row.map.c_str(),
				Ui::DemosBrowserMath::Duration(e.row.seconds).c_str(), e.row.protocol, kCompat[(int)e.row.compat],
				g_hidden.Contains(e.path) ? " | hidden" : "", Filmmaker::WideToUtf8(e.path).c_str());
		}
		return false;
	}
	if (sub == "folders") {
		advancedfx::Message("  game: %s\n", Filmmaker::WideToUtf8(g_gameDir).c_str());
		for (size_t i = 0; i < g_folders.Folders().size(); ++i)
			advancedfx::Message("  %d: %s\n", (int)i, Filmmaker::WideToUtf8(g_folders.Folders()[i]).c_str());
		return false;
	}
	if (sub == "addfolder" && argc >= 4) {
		std::string path = args->ArgV(3);
		for (int i = 4; i < argc; ++i) path += std::string(" ") + args->ArgV(i);
		std::error_code ec;
		if (!fs::is_directory(fs::path(Filmmaker::Utf8ToWide(path)), ec)) advancedfx::Message("mirv_filmmaker demos: %s is not a folder.\n", path.c_str());
		else AddFolder(Filmmaker::Utf8ToWide(path));
		return false;
	}
	if (sub == "removefolder" && argc >= 4) {
		const int i = atoi(args->ArgV(3));
		if (i >= 0 && i < (int)g_folders.Folders().size() && g_folders.Remove(std::wstring(g_folders.Folders()[(size_t)i]))) {
			g_folders.Save();
			g_scanWanted = true;
		}
		return false;
	}
	if (sub == "hidden") {
		for (const auto& path : g_hidden.Paths()) advancedfx::Message("  %s\n", Filmmaker::WideToUtf8(path).c_str());
		return false;
	}
	if (sub.empty()) {
		advancedfx::Message(
			"mirv_filmmaker demos on|off|toggle|state - the demos browser (the main menu's DEMOS button).\n"
			"mirv_filmmaker demos list|folders|hidden - what the library holds.\n"
			"mirv_filmmaker demos addfolder [path]|removefolder <i>|refresh|clearall|unhideall\n"
			"mirv_filmmaker demos watch|hide|openfolder <i>|renamedemo <i> <name>|org <sort|newfolder|rename|delfolder|collapse|assign> ...\n");
		return false;
	}
	std::string line = sub;
	for (int i = 3; i < argc; ++i) line += std::string(" ") + args->ArgV(i);
	return Demos_Command(line);
}

void Demos_PrintHealth() {
	size_t total = 0;
	double ms = 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		total = g_entries.size();
		ms = g_scanMs;
	}
	advancedfx::Message("  demos:      %d found, %d removed from the list, %d added folders, %d added files, %d folders of the user's; %s%s\n",
		(int)total, (int)g_hidden.Paths().size(), (int)g_folders.Folders().size(), (int)g_files.Files().size(), (int)g_org.Groups().size(),
		g_scanning.load() ? "scanning" : ("last scan " + std::to_string((int)ms) + " ms").c_str(), g_picking.load() ? ", a picker is open" : "");
}

} // namespace SourceFilmmaker
