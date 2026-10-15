#include "DemoOrganizer.h"

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
	if (demoPath.empty())
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

	std::string verb, rest;
	if (!SplitFirst(line.substr(4), verb, rest))
		return true;

	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_loaded) {
		g_store.Load();
		g_loaded = true;
	}

	if (verb == "sort") {
		BumpAndSaveLocked(g_store.SetSort(rest));
	} else if (verb == "newfolder") {
		BumpAndSaveLocked(g_store.CreateGroup(rest) >= 0);
	} else if (verb == "rename") {
		std::string idText, name;
		if (SplitFirst(rest, idText, name))
			BumpAndSaveLocked(g_store.RenameGroup(std::atoi(idText.c_str()), name));
	} else if (verb == "delfolder") {
		BumpAndSaveLocked(g_store.DeleteGroup(std::atoi(rest.c_str())));
	} else if (verb == "collapse") {
		// "collapse <id|-1> <0|1>"; -1 is the ungrouped bucket's own header.
		std::string idText, stateText;
		if (SplitFirst(rest, idText, stateText)) {
			const int id = std::atoi(idText.c_str());
			const bool collapsed = (std::atoi(stateText.c_str()) != 0);
			BumpAndSaveLocked(id < 0 ? g_store.SetRootCollapsed(collapsed)
			                         : g_store.SetGroupCollapsed(id, collapsed));
		}
	} else if (verb == "assign") {
		// "assign <groupId|-1> <normalised demo key>" - the key is last so it may contain spaces.
		std::string idText, key;
		if (SplitFirst(rest, idText, key) && !key.empty())
			BumpAndSaveLocked(g_store.Assign(Utf8ToWide(key), std::atoi(idText.c_str())));
	}
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
