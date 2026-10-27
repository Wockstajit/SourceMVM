#include "stdafx.h"

#include "SourceOldDemos.h"

#include "DemoConvert/ConvertV34.h"
#include "SourceConCommand.h"

#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <Shared/AfxConsole.h>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern WrpVEngineClient* g_VEngineClient;

namespace SourceFilmmaker {

namespace {

namespace fs = std::filesystem;

const char kConvertedFolder[] = "source_mvm/converted/";

CommandCallback g_playdemo = nullptr;
bool g_installed = false;
int g_installTries = 0;

struct Job {
	std::string name;      // as typed, relative to the game directory
	std::string converted; // the copy to play, relative to the game directory
	bool ok = false;
	std::string message;
};

std::mutex g_mutex;
bool g_busy = false;
bool g_haveResult = false;
Job g_result;
std::string g_last; // the last finished conversion, for health

std::string GameDir() {
	const char* dir = g_VEngineClient ? g_VEngineClient->GetGameDirectory() : nullptr;
	return dir ? std::string(dir) : std::string();
}

std::string Normalized(std::string name) {
	for (char& c : name)
		if (c == '\\') c = '/';
	while (!name.empty() && name.front() == '/') name.erase(0, 1);
	if (name.size() < 4 || _stricmp(name.c_str() + name.size() - 4, ".dem") != 0) name += ".dem";
	return name;
}

// Size and write time, so a copy is redone when the original changes. The converter version is
// part of it, so a converter fix redoes every copy.
std::string SourceStamp(const fs::path& path) {
	std::error_code ec;
	const auto size = fs::file_size(path, ec);
	const auto time = fs::last_write_time(path, ec).time_since_epoch().count();
	char text[128];
	std::snprintf(text, sizeof(text), "v34-converter %d size %llu time %lld", DemoConvert::kV34ConverterVersion,
		(unsigned long long)size, (long long)time);
	return text;
}

bool ReadAll(const fs::path& path, std::vector<unsigned char>& data) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}

void Convert(std::string gameDir, std::string name, std::string converted) {
	Job job;
	job.name = name;
	job.converted = converted;
	const auto started = std::chrono::steady_clock::now();
	const fs::path source = fs::u8path(gameDir) / fs::u8path(name);
	const fs::path target = fs::u8path(gameDir) / fs::u8path(converted);
	std::vector<unsigned char> data, out;
	if (!ReadAll(source, data)) {
		job.message = "cannot read " + name;
	} else {
		const std::string map = DemoConvert::DemoMapName(data.data(), data.size());
		const fs::path bsp = fs::u8path(gameDir) / "maps" / fs::u8path(map + ".bsp");
		DemoConvert::MapModels models;
		std::string error;
		if (!fs::exists(bsp)) {
			job.message = name + " was recorded on " + map + ", which is not installed. Put " + map +
				".bsp in cstrike/maps and play it again.";
		} else if (!DemoConvert::ReadMapModels(bsp.string(), models, error)) {
			job.message = "cannot read " + map + ".bsp: " + error;
		} else {
			DemoConvert::ConvertOptions options;
			options.map = &models;
			const DemoConvert::ConvertResult result = DemoConvert::ConvertV34Demo(data.data(), data.size(), out, options);
			if (!result.ok) {
				job.message = "cannot convert " + name + ": " + result.error;
			} else {
				std::error_code ec;
				fs::create_directories(target.parent_path(), ec);
				std::ofstream f(target, std::ios::binary | std::ios::trunc);
				f.write((const char*)out.data(), (std::streamsize)out.size());
				f.close();
				std::ofstream stamp(fs::path(target.string() + ".src"), std::ios::trunc);
				stamp << SourceStamp(source);
				if (!f || !stamp) {
					job.message = "cannot write " + converted;
				} else {
					const DemoConvert::ConvertStats& s = result.stats;
					const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
					char text[400];
					std::snprintf(text, sizeof(text), "converted %s (old CS:S v34 demo) in %.0f ms: %d frames, map %s, brushes %d kept, %d moved, %d gone%s%s",
						name.c_str(), ms, s.framesOut, map.c_str(), s.brushKept, s.brushMoved, s.brushMissing,
						s.stoppedEarly.empty() ? "" : "; the recording ends early: ", s.stoppedEarly.c_str());
					job.message = text;
					job.ok = true;
					if (Filmmaker::MvmDebugLog_Active())
						Filmmaker::MvmDebugLog_Linef("demo.convert", "%s brushes:%s", text, s.brushDetail.c_str());
				}
			}
		}
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	g_result = job;
	g_haveResult = true;
	g_busy = false;
}

} // namespace

std::string OldDemos_PlayedName(const std::string& engineName) {
	std::string name = engineName;
	for (char& c : name) if (c == '\\') c = '/';
	const size_t prefix = sizeof(kConvertedFolder) - 1;
	return name.compare(0, prefix, kConvertedFolder) == 0 ? name.substr(prefix) : name;
}

namespace {

// True when it took the command: an old demo that is converted (or being converted) first.
bool InterceptPlaydemo(const char* typed) {
	if (!typed || !*typed) return false;
	const std::string gameDir = GameDir();
	if (gameDir.empty()) return false;
	const std::string name = Normalized(typed);
	if (name.compare(0, sizeof(kConvertedFolder) - 1, kConvertedFolder) == 0) return false; // a copy already
	const fs::path source = fs::u8path(gameDir) / fs::u8path(name);
	unsigned char header[16] = {};
	{
		std::ifstream f(source, std::ios::binary);
		if (!f || !f.read((char*)header, sizeof(header))) return false;
	}
	// Only the header is read here; IsV34Demo checks the magic and both protocol numbers.
	unsigned char full[1072] = {};
	std::memcpy(full, header, sizeof(header));
	if (!DemoConvert::IsV34Demo(full, sizeof(full))) return false;

	const std::string converted = kConvertedFolder + name;
	const fs::path target = fs::u8path(gameDir) / fs::u8path(converted);
	std::string stamp;
	{
		std::ifstream f(fs::path(target.string() + ".src"));
		std::getline(f, stamp);
	}
	std::error_code ec;
	if (fs::exists(target, ec) && stamp == SourceStamp(source)) {
		const std::string command = "playdemo \"" + converted + "\"";
		g_VEngineClient->ClientCmd_Unrestricted(command.c_str());
		return true;
	}
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_busy) {
			advancedfx::Message("SOURCE:MVM: still converting an old demo, play it again in a moment.\n");
			return true;
		}
		g_busy = true;
	}
	advancedfx::Message("SOURCE:MVM: %s is an old CS:S (v34) demo; converting it for this version of the game...\n", name.c_str());
	std::thread(Convert, gameDir, name, converted).detach();
	return true;
}

void PlaydemoHook(const CommandArgs& args) {
	if (args.argc >= 2 && InterceptPlaydemo(args.argv[1])) return;
	g_playdemo(args);
}

} // namespace

void OldDemos_Init() {
	if (g_installed || g_installTries > 600) return;
	if (++g_installTries > 600 && Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("demo.convert", "playdemo not wrapped: not found, or an unexpected layout");
	g_playdemo = ConCommand_Wrap("playdemo", &PlaydemoHook);
	g_installed = g_playdemo != nullptr;
}

void OldDemos_Frame() {
	OldDemos_Init();
	Job job;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_haveResult) return;
		job = g_result;
		g_haveResult = false;
		g_last = job.message;
	}
	advancedfx::Message("SOURCE:MVM: %s\n", job.message.c_str());
	if (job.ok && g_VEngineClient) {
		const std::string command = "playdemo \"" + job.converted + "\"";
		g_VEngineClient->ClientCmd_Unrestricted(command.c_str());
	}
}

void OldDemos_PrintHealth() {
	std::string last;
	bool busy;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		last = g_last;
		busy = g_busy;
	}
	advancedfx::Message("  old demos:  %s%s%s\n", g_installed ? "playdemo converts v34 demos" : "playdemo NOT wrapped",
		busy ? ", converting one now" : "", last.empty() ? "" : ("; last: " + last).c_str());
}

} // namespace SourceFilmmaker
