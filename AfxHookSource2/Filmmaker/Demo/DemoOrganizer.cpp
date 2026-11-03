#include "DemoOrganizer.h"
#include "../Config/PracticeSessionFlag.h" // a practice session writes nothing

#include "DemoProjectPath.h"
#include "../Config/DemoOrganizerStore.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/TextEncoding.h"

#include <mutex>
#include <atomic>
#include <ctime>
#include <cstdlib>
#include <algorithm>

namespace Filmmaker {

namespace {

std::mutex g_mutex;                 // the store is touched from the main pump and the UI poll
DemoOrganizerStore g_store;
std::atomic<uint64_t> g_version{ 0 };
bool g_loaded = false;

std::wstring g_lastSeenPlaying;     // PollPlaying edge detection (main thread only)

// A demo played through the automatic repair path runs from a cached copy in a "repaired"
// sub-folder. That copy is not what the browser lists, so fold it back onto the original the
// user actually sees: <dir>\repaired\<name>.dem -> <dir>\<name>.dem.
std::wstring StripRepairedSegment(const std::wstring& path) {
	const std::wstring mapped = ResolveDemoProjectPath(path);
	if (!mapped.empty() && mapped != path)
		return mapped;
	const std::wstring key = NormalizeDemoKey(path);
	const size_t slash = key.find_last_of(L'\\');
	if (slash == std::wstring::npos || slash == 0)
		return path;
	const size_t parentSlash = key.find_last_of(L'\\', slash - 1);
	if (parentSlash == std::wstring::npos)
		return path;
	const std::wstring parent = key.substr(parentSlash + 1, slash - parentSlash - 1);
	if (parent != L"repaired")
		return path;
	return path.substr(0, parentSlash) + path.substr(slash);
}

void BumpAndSaveLocked(bool changed) {
	if (!changed)
		return;
	g_store.Save();
	g_version.fetch_add(1);
}

} // namespace

void DemoOrganizer_Init() {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_loaded)
		return;
	g_store.Load();
	g_loaded = true;
	g_version.fetch_add(1);
}

uint64_t DemoOrganizer_Version() {
	return g_version.load();
}

void DemoOrganizer_NoteRenamed(const std::wstring& oldPath, const std::wstring& newPath) {
	if (oldPath.empty() || newPath.empty())
		return;
	// StripRepairedSegment for the same reason NotePlayed uses it: state is always recorded
	// against the ORIGINAL demo, never against the repaired copy that may be playing.
	const std::wstring from = StripRepairedSegment(oldPath);
	const std::wstring to = StripRepairedSegment(newPath);
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_loaded) {
		g_store.Load();
		g_loaded = true;
	}
	BumpAndSaveLocked(g_store.Rekey(from, to));
}

void DemoOrganizer_NotePlayed(const std::wstring& demoPath) {
	if (demoPath.empty() || PracticeSession_Active())
		return;
	const std::wstring path = StripRepairedSegment(demoPath);
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_loaded) {
		g_store.Load();
		g_loaded = true;
	}
	BumpAndSaveLocked(g_store.SetLastPlayed(path, (int64_t)std::time(nullptr)));
}

void DemoOrganizer_PollPlaying(const std::wstring& playingPath) {
	if (playingPath == g_lastSeenPlaying)
		return;
	g_lastSeenPlaying = playingPath;
	if (!playingPath.empty())
		DemoOrganizer_NotePlayed(playingPath);
}

bool DemoOrganizer_HandleCommand(const std::string& line) {
	if (line.rfind("org ", 0) != 0)
		return false;

	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_loaded) {
		g_store.Load();
		g_loaded = true;
	}
	// The verbs (sort, newfolder, rename, delfolder, collapse, assign) are the store's, shared with CS:S.
	BumpAndSaveLocked(g_store.Apply(line.substr(4)));
	return true;
}

std::string DemoOrganizer_BuildJson() {
	std::lock_guard<std::mutex> lock(g_mutex);

	JsonBuilder j;
	j.BeginObject();
	j.StringField("sort", g_store.Sort());
	j.BoolField("rootCollapsed", g_store.RootCollapsed());

	j.Key("groups");
	j.BeginArray();
	for (const auto& g : g_store.Groups()) {
		j.BeginObject();
		j.IntField("id", g.id);
		j.StringField("name", g.name);
		j.BoolField("collapsed", g.collapsed);
		j.EndObject();
	}
	j.EndArray();

	// Keyed by normalised demo path so the UI can look a row up directly.
	j.Key("demos");
	j.BeginObject();
	for (const auto& r : g_store.Records()) {
		j.Key(WideToUtf8(r.key).c_str());
		j.BeginObject();
		j.IntField("g", r.groupId);
		j.IntField("played", r.lastPlayed);
		j.EndObject();
	}
	j.EndObject();

	j.EndObject();
	return j.Str();
}

} // namespace Filmmaker
