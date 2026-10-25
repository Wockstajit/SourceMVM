#include "DemoProjectStore.h"
#include "DemoMatchInfoFile.h"
#include "DemoProjectPath.h"

#include "../Debug/MvmDebugLog.h"
#include "../Config/PracticeSessionFlag.h" // a practice session writes nothing
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include "../Platform/TextEncoding.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace Filmmaker {
namespace {

namespace fs = std::filesystem;

std::mutex g_mutex;
ProjectIndex g_index;
bool g_indexLoaded = false;

std::wstring TrailingSlash(std::wstring path) {
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
		path += L'\\';
	return path;
}

std::wstring RoamingHlae() {
	wchar_t appData[32768] = {};
	const DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, (DWORD)_countof(appData));
	if (!n || n >= (DWORD)_countof(appData)) return {};
	return TrailingSlash(std::wstring(appData, n)) + L"HLAE\\";
}

std::wstring IndexPath(const std::wstring& root) {
	return JoinPath(JoinPath(root, L"demos"), L"index.json");
}

bool FileExists(const std::wstring& path) {
	if (path.empty()) return false;
	std::error_code ec;
	return fs::is_regular_file(fs::path(path), ec);
}

void LoadIndexLocked(const std::wstring& root) {
	if (g_indexLoaded) return;
	g_indexLoaded = true;
	g_index = {};
	std::ifstream in(IndexPath(root), std::ios::binary);
	if (!in) return;
	std::ostringstream buf;
	buf << in.rdbuf();
	JsonValue rootJson;
	if (!JsonParse(buf.str(), rootJson) || rootJson.type != JsonValue::Type::Object) return;
	if (const JsonValue* version = rootJson.Find("version"))
		g_index.version = version->AsInt(1);
	const JsonValue* entries = rootJson.Find("entries");
	if (!entries || entries->type != JsonValue::Type::Array) return;
	for (const JsonValue& item : entries->arr) {
		if (item.type != JsonValue::Type::Object) continue;
		const JsonValue* demoPath = item.Find("demoPath");
		const JsonValue* folder = item.Find("folder");
		if (!demoPath || !folder) continue;
		ProjectIndexEntry entry;
		entry.demoPath = Utf8ToWide(demoPath->AsString(""));
		entry.folderName = Utf8ToWide(folder->AsString(""));
		ProjectGame game = ProjectGame::Cs2;
		if (const JsonValue* gameValue = item.Find("game")) {
			const std::string key = gameValue->AsString("cs2");
			ParseProjectGame(key.c_str(), game);
		}
		entry.game = game;
		if (!entry.demoPath.empty() && !entry.folderName.empty())
			g_index.entries.push_back(std::move(entry));
	}
}

bool SaveIndexLocked(const std::wstring& root) {
	const fs::path path(IndexPath(root));
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	if (ec) return false;
	JsonBuilder json;
	json.BeginObject();
	json.IntField("version", g_index.version > 0 ? g_index.version : 1);
	json.Key("entries");
	json.BeginArray();
	for (const auto& entry : g_index.entries) {
		json.BeginObject();
		json.StringField("demoPath", WideToUtf8(entry.demoPath));
		json.StringField("game", ProjectGameKey(entry.game));
		json.StringField("folder", WideToUtf8(entry.folderName));
		json.EndObject();
	}
	json.EndArray();
	json.EndObject();
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) return false;
	out.write(json.Str().data(), (std::streamsize)json.Str().size());
	return (bool)out;
}

// The "project" section payload: {"game":"cs2"|"deadlock","demoPath":"<utf8>"}.
std::string ProjectInfoJson(ProjectGame game, const std::wstring& demoPath) {
	JsonBuilder json;
	json.BeginObject();
	json.StringField("game", ProjectGameKey(game));
	json.StringField("demoPath", WideToUtf8(demoPath));
	json.EndObject();
	return json.Str();
}

bool ReadFileBytes(const fs::path& path, std::string& out) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return false;
	std::ostringstream buf;
	buf << in.rdbuf();
	out = buf.str();
	return true;
}

// Temp file in the same folder, flushed, then renamed over the target.
bool WriteFileAtomic(const fs::path& target, const std::string& bytes) {
	const fs::path temp = fs::path(target.wstring() + L".tmp");
	HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	DWORD wrote = 0;
	const bool ok = WriteFile(file, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr)
		&& wrote == bytes.size() && FlushFileBuffers(file);
	CloseHandle(file);
	const bool published = ok && MoveFileExW(temp.c_str(), target.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
	if (published) InvalidateDemoProjectPathCache();
	return published;
}

// A project.mvm that fails to decode is never silently overwritten: rename it aside with a
// UTC timestamp, log it, and let the caller write a fresh file.
void QuarantineProjectFile(const fs::path& path, const char* error) {
	SYSTEMTIME now{};
	GetSystemTime(&now);
	wchar_t stamp[32];
	swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u",
		now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
	const fs::path aside = fs::path(path.wstring() + L".corrupt-" + stamp);
	std::error_code ec;
	fs::rename(path, aside, ec);
	MvmDebugLog_LinefAlways("project.store",
		"project.mvm unreadable (%s); moved to %s%s", error,
		WideToUtf8(aside.wstring()).c_str(), ec ? " -- rename failed" : "");
}

// Loads the existing container into `file`; absent is fine, corrupt is quarantined.
void LoadContainerLocked(const fs::path& path, ProjectGame game, ProjectFile& file) {
	file = {};
	file.game = game;
	std::string bytes;
	if (!ReadFileBytes(path, bytes)) return;
	std::string error;
	if (DecodeProjectFile(bytes.data(), bytes.size(), game, file, error)) return;
	QuarantineProjectFile(path, error.c_str());
	file = {};
	file.game = game;
}

bool WriteProjectFileLocked(const fs::path& dir, ProjectGame game, const std::wstring& demoPath) {
	std::error_code ec;
	fs::create_directories(dir, ec);
	if (ec) return false;
	const fs::path target = dir / kProjectFileName;
	ProjectFile file;
	LoadContainerLocked(target, game, file);
	SetProjectSection(file, kProjectFileInfoTag, ProjectInfoJson(game, demoPath));
	return WriteFileAtomic(target, EncodeProjectFile(file));
}

std::wstring EnsureDirectoryLocked(ProjectGame game, const std::wstring& demoPath, std::wstring& rootOut) {
	rootOut = DemoProjectStore_Root();
	if (rootOut.empty() || demoPath.empty()) return {};
	LoadIndexLocked(rootOut);
	const std::wstring folder = AllocateFolderName(g_index, game, demoPath);
	UpsertIndexEntry(g_index, game, demoPath, folder);
	if (!SaveIndexLocked(rootOut)) return {};
	const fs::path dir = fs::path(rootOut) / L"demos" / ProjectGameDir(game) / folder;
	// The folder IS the project once it has a container: the scanner keys on project.mvm,
	// so it must exist even when only generated caches have been written so far.
	if (!FileExists((dir / kProjectFileName).wstring()))
		WriteProjectFileLocked(dir, game, demoPath);
	return dir.wstring();
}

} // namespace

std::wstring DemoProjectStore_Root() {
	const std::wstring hlae = RoamingHlae();
	if (hlae.empty()) return {};
	return TrailingSlash(hlae) + L"source_mvm";
}

std::wstring DemoProjectStore_ProjectDirectory(ProjectGame game, const std::wstring& demoPath) {
	std::lock_guard<std::mutex> lock(g_mutex);
	const std::wstring root = DemoProjectStore_Root();
	if (root.empty() || demoPath.empty()) return {};
	LoadIndexLocked(root);
	if (const ProjectIndexEntry* entry = FindIndexEntry(g_index, demoPath)) {
		if (entry->game == game)
			return (fs::path(root) / L"demos" / ProjectGameDir(game) / entry->folderName).wstring();
	}
	const std::wstring folder = AllocateFolderName(g_index, game, demoPath);
	return (fs::path(root) / L"demos" / ProjectGameDir(game) / folder).wstring();
}

std::vector<std::wstring> DemoProjectStore_OtherProjectDirectories(ProjectGame game, const std::wstring& demoPath) {
	std::vector<std::wstring> out;
	const std::wstring keepDir = DemoProjectStore_ProjectDirectory(game, demoPath);
	const size_t slash = keepDir.find_last_of(L"\\/");
	if (keepDir.empty() || slash == std::wstring::npos) return out;
	const std::wstring gameDir = keepDir.substr(0, slash);
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((gameDir + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return out;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
		const std::wstring dir = gameDir + L"\\" + fd.cFileName;
		if (_wcsicmp(dir.c_str(), keepDir.c_str()) != 0) out.push_back(dir);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return out;
}

std::wstring DemoProjectStore_EnsureProjectDirectory(ProjectGame game, const std::wstring& demoPath) {
	std::lock_guard<std::mutex> lock(g_mutex);
	std::wstring root;
	return EnsureDirectoryLocked(game, demoPath, root);
}

std::wstring DemoProjectStore_EnsureProjectFile(ProjectGame game, const std::wstring& demoPath) {
	std::lock_guard<std::mutex> lock(g_mutex);
	std::wstring root;
	const std::wstring dir = EnsureDirectoryLocked(game, demoPath, root);
	if (dir.empty()) return {};
	return (fs::path(dir) / kProjectFileName).wstring();
}

bool DemoProjectStore_ReadSection(ProjectGame game, const std::wstring& demoPath,
	ProjectSection section, std::string& payload) {
	payload.clear();
	// A practice session starts every demo empty: nothing the user saved for it is read.
	if (PracticeSession_Active()) return false;
	std::lock_guard<std::mutex> lock(g_mutex);
	const std::wstring root = DemoProjectStore_Root();
	if (root.empty() || demoPath.empty()) return false;
	LoadIndexLocked(root);
	const ProjectIndexEntry* entry = FindIndexEntry(g_index, demoPath);
	if (!entry || entry->game != game) return false;
	const fs::path file = fs::path(root) / L"demos" / ProjectGameDir(game) / entry->folderName
		/ kProjectFileName;
	std::string bytes;
	if (!ReadFileBytes(file, bytes)) return false;
	ProjectFile parsed;
	std::string error;
	if (!DecodeProjectFile(bytes.data(), bytes.size(), game, parsed, error)) {
		MvmDebugLog_LinefAlways("project.store", "project.mvm unreadable (%s): %s",
			error.c_str(), WideToUtf8(file.wstring()).c_str());
		return false;
	}
	const ProjectFileSection* found = FindProjectSection(parsed, ProjectSectionTag(section));
	if (!found) return false;
	payload = found->payload;
	return true;
}

bool DemoProjectStore_WriteSection(ProjectGame game, const std::wstring& demoPath,
	ProjectSection section, const std::string& payload) {
	// A practice session keeps its edits in memory. Reported as written, so no module holds
	// them back to retry later (SceneKeys would, on shutdown).
	if (PracticeSession_Active()) return true;
	std::lock_guard<std::mutex> lock(g_mutex);
	std::wstring root;
	const std::wstring dir = EnsureDirectoryLocked(game, demoPath, root);
	if (dir.empty()) return false;
	const fs::path target = fs::path(dir) / kProjectFileName;
	ProjectFile file;
	LoadContainerLocked(target, game, file);
	SetProjectSection(file, kProjectFileInfoTag, ProjectInfoJson(game, demoPath));
	SetProjectSection(file, ProjectSectionTag(section), payload);
	if (!WriteFileAtomic(target, EncodeProjectFile(file))) {
		MvmDebugLog_LinefAlways("project.store", "could not write %s",
			WideToUtf8(target.wstring()).c_str());
		return false;
	}
	return true;
}

std::wstring DemoProjectStore_GeneratedForLoad(ProjectGame game, const std::wstring& logicalDemoPath,
	ProjectGenerated kind) {
	const std::wstring dir = DemoProjectStore_ProjectDirectory(game, logicalDemoPath);
	if (dir.empty()) return {};
	return ProjectGeneratedPath(dir, kind);
}

std::wstring DemoProjectStore_GeneratedForSave(ProjectGame game, const std::wstring& logicalDemoPath,
	ProjectGenerated kind) {
	const std::wstring dir = DemoProjectStore_EnsureProjectDirectory(game, logicalDemoPath);
	if (dir.empty()) return {};
	return ProjectGeneratedPath(dir, kind);
}

std::wstring DemoProjectStore_MatchInfoForLoad(ProjectGame game, const std::wstring& demoPath) {
	if (demoPath.empty()) return {};
	const std::wstring valve = ValveMatchInfoPath(demoPath);
	if (FileExists(valve)) return valve;
	// Earlier releases moved this ENGINE-owned file away. Recover it at CS2's native path;
	// a scan must never take ownership of it or delete it again.
	const std::wstring archived = DemoProjectStore_GeneratedForLoad(game, demoPath,
		ProjectGenerated::MatchInfo);
	if (archived.empty()) return {};
	const auto result = RestoreMatchInfoIfMissing(fs::path(archived), fs::path(valve));
	if (result == MatchInfoRestore::Restored)
		MvmDebugLog_LinefAlways("demo.matchinfo", "restored Valve sidecar: %s",
			WideToUtf8(valve).c_str());
	else if (result == MatchInfoRestore::Failed)
		MvmDebugLog_LinefAlways("demo.matchinfo", "could not restore Valve sidecar; recovery copy retained: %s",
			WideToUtf8(archived).c_str());
	return result == MatchInfoRestore::Existing || result == MatchInfoRestore::Restored ? valve : std::wstring{};
}

void DemoProjectStore_NoteRenamed(ProjectGame game, const std::wstring& oldPath,
	const std::wstring& newPath) {
	if (oldPath.empty() || newPath.empty() || WideEqualsI(oldPath, newPath)) return;
	std::lock_guard<std::mutex> lock(g_mutex);
	const std::wstring root = DemoProjectStore_Root();
	if (root.empty()) return;
	LoadIndexLocked(root);
	ProjectRenamePlan plan;
	if (!ApplyDemoRename(g_index, game, oldPath, newPath, plan))
		return;

	const fs::path from = fs::path(root) / L"demos" / ProjectGameDir(game) / plan.oldFolderName;
	const fs::path to = fs::path(root) / L"demos" / ProjectGameDir(game) / plan.newFolderName;
	std::error_code ec;
	if (plan.renameFolder && fs::exists(from, ec) && !fs::exists(to, ec)) {
		fs::rename(from, to, ec);
		if (ec) {
			if (ProjectIndexEntry* entry = FindIndexEntry(g_index, newPath))
				entry->folderName = plan.oldFolderName;
		}
	}
	const fs::path live = fs::exists(to, ec) ? to : from;
	if (fs::exists(live, ec))
		WriteProjectFileLocked(live, game, newPath);
	SaveIndexLocked(root);
}

} // namespace Filmmaker
