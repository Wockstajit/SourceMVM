#include "WorkshopOrganizer.h"

#include "../Config/WorkshopOrganizerStore.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/TextEncoding.h"
#include "../../hlaeFolder.h"

#include <mutex>
#include <atomic>
#include <vector>
#include <ctime>
#include <cctype>
#include <cwctype>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <sstream>
#include <fstream>

#include <windows.h> // RegGetValueW: Steam's install path, for the library fallback below

namespace fs = std::filesystem;

namespace Filmmaker {

namespace {

struct WorkshopItem {
	std::string id;          // decimal workshop item id
	int64_t installed = 0;   // unix seconds, newest mtime seen in the item folder
};

std::mutex g_mutex;          // the store is touched from the main pump and the UI poll
WorkshopOrganizerStore g_store;
std::atomic<uint64_t> g_version{ 0 };
bool g_loaded = false;

bool g_scanned = false;                              // first scan is lazy; see WorkshopOrganizer_Rescan
std::wstring g_contentRoot;                          // ...\steamapps\workshop\content\730
std::vector<WorkshopItem> g_items;
std::vector<std::pair<std::string, std::string>> g_mapToId; // lower-case map name -> workshop id

std::string g_lastSeenLevel;                         // PollLevel edge detection (main thread only)

std::string LowerAscii(const std::string& in) {
	std::string out;
	out.reserve(in.size());
	for (char c : in)
		out.push_back((char)std::tolower((unsigned char)c));
	return out;
}

bool IsAllDigits(const std::wstring& s) {
	if (s.empty() || s.size() > 20)
		return false;
	for (wchar_t c : s)
		if (c < L'0' || c > L'9')
			return false;
	return true;
}

int64_t ToUnixSeconds(fs::file_time_type t) {
	// clock_cast is C++20 and the file clock's epoch is not the system clock's, so go through
	// the duration difference instead -- portable across the toolchains this repo builds with.
	const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
		t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
	return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

// CS2 normally runs out of <library>\steamapps\common\Counter-Strike Global Offensive\game\bin\
// win64, and its workshop downloads live in <library>\steamapps\workshop\content\730. Rather
// than counting parent hops (which breaks the moment Valve moves the exe), walk up until a path
// component is literally named "steamapps".
std::wstring WorkshopRootFromProcessFolder() {
	const wchar_t* proc = GetProcessFolderW();
	if (!proc || !*proc)
		return std::wstring();

	std::error_code ec;
	fs::path p = fs::path(proc);
	for (int depth = 0; depth < 12 && !p.empty(); ++depth) {
		std::wstring name = p.filename().wstring();
		std::transform(name.begin(), name.end(), name.begin(),
			[](wchar_t c) { return (wchar_t)towlower(c); });
		if (name == L"steamapps") {
			fs::path root = p / L"workshop" / L"content" / L"730";
			if (fs::is_directory(root, ec))
				return root.wstring();
			return std::wstring();
		}
		fs::path parent = p.parent_path();
		if (parent == p)
			break;
		p = parent;
	}
	return std::wstring();
}

std::wstring ReadSteamInstallPath() {
	static const struct { HKEY root; const wchar_t* key; const wchar_t* value; } kCandidates[] = {
		{ HKEY_CURRENT_USER,  L"Software\\Valve\\Steam",                  L"SteamPath" },
		{ HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam",     L"InstallPath" },
		{ HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam",                  L"InstallPath" },
	};
	for (const auto& candidate : kCandidates) {
		wchar_t buffer[MAX_PATH] = { 0 };
		DWORD size = sizeof(buffer);
		if (ERROR_SUCCESS == RegGetValueW(candidate.root, candidate.key, candidate.value,
				RRF_RT_REG_SZ, nullptr, buffer, &size) && buffer[0]) {
			return std::wstring(buffer);
		}
	}
	return std::wstring();
}

// Every Steam library, from steamapps\libraryfolders.vdf. Only the "path" values are needed, and
// they are the only quoted absolute paths in the file, so a full VDF parser would be overkill.
std::vector<std::wstring> SteamLibraryRoots() {
	std::vector<std::wstring> roots;
	const std::wstring steam = ReadSteamInstallPath();
	if (steam.empty())
		return roots;
	roots.push_back(steam);

	std::ifstream f(fs::path(steam) / L"steamapps" / L"libraryfolders.vdf", std::ios::binary);
	if (!f.is_open())
		return roots;
	std::string line;
	while (std::getline(f, line)) {
		const size_t key = line.find("\"path\"");
		if (key == std::string::npos)
			continue;
		const size_t open = line.find('"', key + 6);
		if (open == std::string::npos)
			continue;
		const size_t close = line.find('"', open + 1);
		if (close == std::string::npos)
			continue;
		std::string value = line.substr(open + 1, close - open - 1);
		// VDF escapes backslashes.
		std::string unescaped;
		for (size_t i = 0; i < value.size(); ++i) {
			if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '\\')
				++i;
			unescaped.push_back(value[i]);
		}
		if (!unescaped.empty())
			roots.push_back(Utf8ToWide(unescaped));
	}
	return roots;
}

// The walk-up above only works when CS2 is running from inside a Steam library. It is not,
// whenever the user takes the launcher up on duplicating their install somewhere clean so the
// bot mod cannot touch the copy Steam manages -- and then the Play page would silently lose
// every install/played timestamp. Workshop CONTENT still lives in the real Steam library either
// way, so fall back to asking Steam where its libraries are.
std::wstring ResolveWorkshopContentRoot() {
	std::wstring fromProcess = WorkshopRootFromProcessFolder();
	if (!fromProcess.empty())
		return fromProcess;

	std::error_code ec;
	for (const std::wstring& library : SteamLibraryRoots()) {
		fs::path root = fs::path(library) / L"steamapps" / L"workshop" / L"content" / L"730";
		if (fs::is_directory(root, ec))
			return root.wstring();
	}
	return std::wstring();
}

void ScanLocked() {
	g_scanned = true;
	g_items.clear();
	g_mapToId.clear();
	g_contentRoot = ResolveWorkshopContentRoot();
	if (g_contentRoot.empty())
		return;

	std::error_code ec;
	for (fs::directory_iterator it(fs::path(g_contentRoot), ec), end; it != end; it.increment(ec)) {
		if (ec)
			break;
		if (!it->is_directory(ec))
			continue;
		const std::wstring folder = it->path().filename().wstring();
		if (!IsAllDigits(folder))
			continue;

		WorkshopItem item;
		item.id = WideToUtf8(folder);

		// The folder's own mtime moves when Steam writes into it, but a re-download that only
		// replaces the .vpk in place does not always touch the folder -- so take the newest
		// stamp seen anywhere in the item.
		const auto folderTime = fs::last_write_time(it->path(), ec);
		if (!ec)
			item.installed = ToUnixSeconds(folderTime);

		std::error_code innerEc;
		for (fs::directory_iterator f(it->path(), innerEc), fend; f != fend; f.increment(innerEc)) {
			if (innerEc)
				break;
			if (!f->is_regular_file(innerEc))
				continue;
			const auto fileTime = fs::last_write_time(f->path(), innerEc);
			if (!innerEc) {
				const int64_t stamp = ToUnixSeconds(fileTime);
				if (stamp > item.installed)
					item.installed = stamp;
			}
			// The map name is the .vpk basename; that is what the engine reports as the short
			// level name once the map is loaded, so it is the join key for "recently played".
			std::wstring ext = f->path().extension().wstring();
			std::transform(ext.begin(), ext.end(), ext.begin(),
				[](wchar_t c) { return (wchar_t)towlower(c); });
			if (ext != L".vpk")
				continue;
			const std::string mapName = LowerAscii(WideToUtf8(f->path().stem().wstring()));
			if (!mapName.empty())
				g_mapToId.emplace_back(mapName, item.id);
		}

		g_items.push_back(item);
	}
}

void EnsureLoadedLocked() {
	if (!g_loaded) {
		g_store.Load();
		g_loaded = true;
	}
	if (!g_scanned)
		ScanLocked();
}

void BumpAndSaveLocked(bool changed) {
	if (!changed)
		return;
	g_store.Save();
	g_version.fetch_add(1);
}

// Splits "<token> <rest>" at the first space.
bool SplitFirst(const std::string& line, std::string& token, std::string& rest) {
	const size_t sp = line.find(' ');
	if (sp == std::string::npos) {
		token = line;
		rest.clear();
		return !token.empty();
	}
	token = line.substr(0, sp);
	rest = line.substr(sp + 1);
	return !token.empty();
}

bool ParseBool(const std::string& s) {
	return std::atoi(s.c_str()) != 0;
}

} // namespace

void WorkshopOrganizer_Init() {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_loaded)
		return;
	g_store.Load();
	g_loaded = true;
	// The folder scan stays lazy on purpose: it walks ~200 directories and Init runs on the
	// startup pump, where a cold-cache stall would be a visible hitch. The first BuildJson (the
	// user opening the Play page) or the first PollLevel pays for it instead.
	g_version.fetch_add(1);
}

uint64_t WorkshopOrganizer_Version() {
	return g_version.load();
}

void WorkshopOrganizer_Rescan() {
	std::lock_guard<std::mutex> lock(g_mutex);
	ScanLocked();
	g_version.fetch_add(1);
}

void WorkshopOrganizer_PollLevel(const std::string& levelNameShort) {
	if (levelNameShort == g_lastSeenLevel)
		return;
	g_lastSeenLevel = levelNameShort;
	if (levelNameShort.empty())
		return;

	const std::string key = LowerAscii(levelNameShort);
	std::lock_guard<std::mutex> lock(g_mutex);
	EnsureLoadedLocked();
	for (const auto& entry : g_mapToId) {
		if (entry.first != key)
			continue;
		BumpAndSaveLocked(g_store.SetLastPlayed(entry.second, (int64_t)std::time(nullptr)));
		return;
	}
}

bool WorkshopOrganizer_HandleCommand(const std::string& line) {
	if (line.rfind("ws ", 0) != 0)
		return false;

	std::string verb, rest;
	if (!SplitFirst(line.substr(3), verb, rest))
		return true;

	if (verb == "rescan") {
		WorkshopOrganizer_Rescan();
		return true;
	}

	std::lock_guard<std::mutex> lock(g_mutex);
	EnsureLoadedLocked();

	if (verb == "sort") {
		BumpAndSaveLocked(g_store.SetSort(rest));
	} else if (verb == "hide") {
		BumpAndSaveLocked(g_store.SetHideTraining(ParseBool(rest)));
	} else if (verb == "words") {
		// "words <comma,separated,words>" - the list is last so it may contain spaces.
		BumpAndSaveLocked(g_store.SetWords(rest));
	} else if (verb == "reset") {
		BumpAndSaveLocked(g_store.ResetWords());
	}
	return true;
}

std::string WorkshopOrganizer_BuildJson() {
	std::lock_guard<std::mutex> lock(g_mutex);
	EnsureLoadedLocked();

	JsonBuilder j;
	j.BeginObject();
	j.StringField("sort", g_store.Sort());
	j.BoolField("hideTraining", g_store.HideTraining());
	j.StringField("root", WideToUtf8(g_contentRoot));

	j.Key("words");
	j.BeginArray();
	for (const auto& w : g_store.Words())
		j.ValueString(w);
	j.EndArray();

	// Keyed by workshop id so the UI can look a tile up directly from its "addon" attribute.
	j.Key("items");
	j.BeginObject();
	for (const auto& item : g_items) {
		int64_t played = 0;
		for (const auto& r : g_store.PlayRecords()) {
			if (r.id == item.id) {
				played = r.lastPlayed;
				break;
			}
		}
		j.Key(item.id.c_str());
		j.BeginObject();
		j.IntField("installed", item.installed);
		j.IntField("played", played);
		j.EndObject();
	}
	j.EndObject();

	j.EndObject();
	return j.Str();
}

std::string WorkshopOrganizer_Describe() {
	std::lock_guard<std::mutex> lock(g_mutex);
	EnsureLoadedLocked();
	std::ostringstream out;
	out << "workshop: " << g_items.size() << " item(s), " << g_mapToId.size() << " map name(s), sort '"
	    << g_store.Sort() << "', hide training " << (g_store.HideTraining() ? "ON" : "off") << "\n";
	out << "  content root: " << (g_contentRoot.empty() ? std::string("<not found>") : WideToUtf8(g_contentRoot)) << "\n";
	out << "  keywords: " << g_store.WordsJoined() << "\n";
	return out.str();
}

} // namespace Filmmaker
