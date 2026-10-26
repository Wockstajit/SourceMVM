#include "stdafx.h"

#include "SourceDemoIndex.h"
#include "SourceConsole.h"
#include "SourceGameProfile.h"

#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <Shared/AfxConsole.h>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

extern WrpVEngineClient* g_VEngineClient;

namespace SourceFilmmaker {

namespace {

const char kPlayingPrefix[] = "Playing demo from ";

std::mutex g_mutex;
std::string g_startedFile; // set by the console observer, taken by the main thread
DemoIndex g_index;         // guarded by g_mutex

// "Playing demo from replays/x.dem." -> "replays/x.dem". Any thread.
void OnConsoleLine(const char* line) {
	if (std::strncmp(line, kPlayingPrefix, sizeof(kPlayingPrefix) - 1) != 0) return;
	std::string file = line + sizeof(kPlayingPrefix) - 1;
	while (!file.empty() && (file.back() == '\n' || file.back() == '\r' || file.back() == ' ')) file.pop_back();
	if (!file.empty() && file.back() == '.') file.pop_back();
	std::lock_guard<std::mutex> lock(g_mutex);
	g_startedFile = file;
}

bool FileExists(const std::string& path) {
	const DWORD attributes = GetFileAttributesA(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Size and write time, to tell a demo the engine restarted (a backward seek replays it from the
// start) from a new recording under the same name.
std::string FileStamp(const std::string& path) {
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return std::string();
	char text[64];
	std::snprintf(text, sizeof(text), "%lu.%lu.%lu.%lu", data.nFileSizeHigh, data.nFileSizeLow,
		data.ftLastWriteTime.dwHighDateTime, data.ftLastWriteTime.dwLowDateTime);
	return text;
}

// The engine names the demo relative to the game directory (cstrike), with or without ".dem".
std::string ResolveDemoPath(const std::string& file) {
	std::vector<std::string> candidates;
	const bool absolute = file.size() > 1 && (file[1] == ':' || file[0] == '\\' || file[0] == '/');
	const char* gameDir = g_VEngineClient ? g_VEngineClient->GetGameDirectory() : nullptr;
	const std::string base = absolute || !gameDir ? std::string() : std::string(gameDir) + "\\";
	candidates.push_back(base + file);
	candidates.push_back(base + file + ".dem");
	for (const std::string& candidate : candidates)
		if (FileExists(candidate)) return candidate;
	return std::string();
}

void StartScan(const std::string& file) {
	std::vector<std::string> wanted;
	for (const char* const* name = GameProfile_Current().demoEvents; name && *name; ++name) wanted.push_back(*name);
	const std::string path = ResolveDemoPath(file);
	const std::string stamp = path.empty() ? std::string() : FileStamp(path);
	unsigned generation;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!stamp.empty() && stamp == g_index.stamp && path == g_index.path && g_index.status != DemoIndex::Status::Failed)
			return; // the same file again
		generation = g_index.generation + 1;
		g_index = DemoIndex();
		g_index.generation = generation;
		g_index.file = file;
		g_index.path = path;
		g_index.stamp = stamp;
		if (path.empty()) {
			g_index.status = DemoIndex::Status::Failed;
			g_index.error = "demo file not found";
		} else {
			g_index.status = DemoIndex::Status::Scanning;
		}
	}
	if (path.empty()) {
		if (Filmmaker::MvmDebugLog_Active())
			Filmmaker::MvmDebugLog_Linef("demo.index", "file=%s not found", file.c_str());
		return;
	}
	std::thread([path, wanted, generation]() {
		const auto started = std::chrono::steady_clock::now();
		auto scan = std::make_shared<DemoFile::Scan>(DemoFile::ScanDemoFile(path, wanted));
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_index.generation != generation) return; // another demo started meanwhile
		g_index.scanMs = ms;
		// A scan that stopped early still keeps the events before the stop.
		g_index.status = scan->ok || !scan->events.empty() ? DemoIndex::Status::Ready : DemoIndex::Status::Failed;
		g_index.error = scan->error;
		g_index.scan = std::move(scan);
	}).detach();
}

unsigned g_loggedGeneration = 0;

void LogFinished() {
	DemoIndex index = DemoIndex_Get();
	if (index.generation == g_loggedGeneration) return;
	if (index.status == DemoIndex::Status::Scanning || index.status == DemoIndex::Status::None) return;
	g_loggedGeneration = index.generation;
	if (!Filmmaker::MvmDebugLog_Active()) return;
	if (!index.scan) {
		Filmmaker::MvmDebugLog_Linef("demo.index", "file=%s failed: %s", index.file.c_str(), index.error.c_str());
		return;
	}
	const DemoFile::Scan& scan = *index.scan;
	Filmmaker::MvmDebugLog_Linef("demo.index", "file=%s path=%s %s map=%s ticks=%d frames=%d messages=%d events=%d ms=%.1f%s%s",
		index.file.c_str(), index.path.c_str(), scan.ok ? "ok" : "partial", scan.header.mapName.c_str(),
		scan.header.playbackTicks, scan.frames, scan.messages, (int)scan.events.size(), index.scanMs,
		scan.ok ? "" : " error=", scan.error.c_str());
	for (const DemoFile::Event& event : scan.events)
		Filmmaker::MvmDebugLog_Linef("demo.index", "event tick=%d %s userid=%d attacker=%d", event.tick, event.name.c_str(),
			event.Int("userid"), event.Int("attacker"));
}

} // namespace

void DemoIndex_Init() {
	Console_Install();
	Console_SetLineObserver(&OnConsoleLine);
}

void DemoIndex_Frame() {
	std::string started;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		started.swap(g_startedFile);
	}
	if (!started.empty()) StartScan(started);
	LogFinished();
}

DemoIndex DemoIndex_Get() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_index;
}

void DemoIndex_PrintHealth() {
	const DemoIndex index = DemoIndex_Get();
	switch (index.status) {
	case DemoIndex::Status::None:
		advancedfx::Message("  demo index: none (no demo started since the hook loaded)\n");
		return;
	case DemoIndex::Status::Scanning:
		advancedfx::Message("  demo index: reading %s\n", index.file.c_str());
		return;
	case DemoIndex::Status::Failed:
		advancedfx::Message("  demo index: %s FAILED: %s\n", index.file.c_str(), index.error.c_str());
		return;
	default:
		break;
	}
	int deaths = 0, rounds = 0;
	for (const DemoFile::Event& event : index.scan->events) {
		if (event.name == "player_death") ++deaths;
		else if (event.name == "round_start") ++rounds;
	}
	advancedfx::Message("  demo index: %s, %d events (%d deaths, %d round starts) in %.1f ms%s%s\n", index.file.c_str(),
		(int)index.scan->events.size(), deaths, rounds, index.scanMs, index.scan->ok ? "" : ", stopped early: ",
		index.scan->error.c_str());
}

} // namespace SourceFilmmaker
