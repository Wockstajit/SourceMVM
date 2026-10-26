#include "stdafx.h"

#include "SourceOldDemos.h"

#include "DemoConvert/ConvertV34.h"
#include "SourceDemoIndex.h"
#include "SourceEngine.h"

#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <Shared/AfxConsole.h>

#include <css/sdk_src/public/icvar.h>
#include <css/sdk_src/public/tier1/convar.h>

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

// tier1/convar.h's ConCommand data, which the SDK keeps private: the base's vtable, next, registered,
// name, help and flags, then the callback union, the completion union and the callback-kind bits.
struct ConCommandData {
	void* vtable;
	void* next;
	bool registered;
	const char* name;
	const char* help;
	int flags;
	void* callback;
	void* completion;
	unsigned char bits; // bit 1: m_bUsingNewCommandCallback, bit 2: m_bUsingCommandCallbackInterface
};

// tier1/convar.h's CCommand.
struct CommandArgs {
	int argc;
	int argv0Size;
	char argS[512];
	char argvBuffer[512];
	const char* argv[64];
};

typedef void (*CommandCallback)(const CommandArgs& args);

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

// Old SourceTV demos carry no director camera events (no hltv_fixed or hltv_chase; only
// hltv_status), so the SourceTV camera opens fixed with nowhere to put it: the map origin, under
// the floor, following nobody. Each time a converted SourceTV demo starts (also when a backward
// seek restarts it), once a player is alive, the camera goes into first person on the first
// player, as a fresh CS2 demo opens. spec_mode alone is not enough: with no target the in-eye
// camera stays where it is. The user's own choice afterwards stands.
bool g_cameraArmed = true;
int g_lastTick = 0;
int g_armedFrames = 0;

void OpenInFirstPerson() {
	const DemoState demo = Demo_State();
	if (!demo.playing) {
		g_cameraArmed = true;
		g_armedFrames = 0;
		return;
	}
	if (demo.tick + 10 < g_lastTick) { // restarted
		g_cameraArmed = true;
		g_armedFrames = 0;
	}
	g_lastTick = demo.tick;
	if (!g_cameraArmed || !g_VEngineClient) return;
	if (DemoIndex_Get().file.compare(0, sizeof(kConvertedFolder) - 1, kConvertedFolder) != 0) {
		g_cameraArmed = false;
		return;
	}
	std::vector<PlayerState> players;
	Players_Snapshot(players);
	const int local = Players_LocalIndex();
	bool haveLocal = false, sourceTv = false, someoneAlive = false;
	for (const PlayerState& p : players) {
		if (p.index == local) {
			haveLocal = true;
			sourceTv = p.hltv;
		} else if (p.alive && !p.hltv) {
			someoneAlive = true;
		}
	}
	if (!haveLocal) return; // still loading
	if (!sourceTv || ++g_armedFrames > 1200) {
		g_cameraArmed = false; // a player's own recording (the view is theirs), or no one came alive
		return;
	}
	const ViewPose view = View_Game();
	if (!someoneAlive || view.origin[0] != 0 || view.origin[1] != 0 || view.origin[2] != 0) return;
	g_VEngineClient->ClientCmd_Unrestricted("spec_mode 4; spec_next");
	g_cameraArmed = false;
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("demo.convert", "SourceTV camera at the map origin at tick %d: first person on the first player", demo.tick);
}

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
	if (g_installed || g_installTries > 600 || !SOURCESDK::CSS::g_pCVar) return;
	++g_installTries;
	SOURCESDK::CSS::ConCommand* command = SOURCESDK::CSS::g_pCVar->FindCommand("playdemo");
	if (!command) return;
	ConCommandData* data = (ConCommandData*)command;
	// Only a command whose layout reads back as expected is touched.
	if (!data->name || std::strcmp(data->name, "playdemo") != 0 || !(data->bits & 2) || (data->bits & 4) || !data->callback) {
		g_installTries = 1000;
		if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("demo.convert", "playdemo not wrapped: unexpected layout");
		return;
	}
	g_playdemo = (CommandCallback)data->callback;
	data->callback = (void*)&PlaydemoHook;
	g_installed = true;
}

void OldDemos_Frame() {
	OldDemos_Init();
	OpenInFirstPerson();
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
