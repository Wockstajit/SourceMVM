// Converts old Counter-Strike: Source demos (network protocol 7, the "v34" engine) to the format
// the current 64-bit game plays, with the same code the Source 1 hook runs when such a demo is
// played (AfxHookSource/Filmmaker/DemoConvert/ConvertV34.cpp).
//
//   SourceDemoConvertTool [--maps <cstrike/maps>] <in.dem> <out.dem>
//   SourceDemoConvertTool [--maps <cstrike/maps>] --dir <folder> <output folder>
//
// With --maps, each demo's brush models are matched to that map as the game would load it.
//
// Prints one line per demo and exits non-zero if any conversion failed.

#include "../../AfxHookSource/Filmmaker/DemoConvert/ConvertV34.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace SourceFilmmaker::DemoConvert;
namespace fs = std::filesystem;

static bool ReadFile(const fs::path& path, std::vector<unsigned char>& data) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}

static fs::path g_maps;

static bool ConvertOne(const fs::path& in, const fs::path& out) {
	std::vector<unsigned char> data, converted;
	if (!ReadFile(in, data)) {
		std::printf("%s: cannot read\n", in.filename().string().c_str());
		return false;
	}
	if (!IsV34Demo(data.data(), data.size())) {
		std::printf("%s: skipped, not a protocol 7 demo\n", in.filename().string().c_str());
		return true;
	}
	MapModels map;
	ConvertOptions options;
	std::string mapNote;
	if (!g_maps.empty()) {
		const std::string name = DemoMapName(data.data(), data.size());
		std::string error;
		if (ReadMapModels((g_maps / (name + ".bsp")).string(), map, error)) options.map = &map;
		else mapNote = ", map " + name + " not read (" + error + ")";
	}
	const auto started = std::chrono::steady_clock::now();
	const ConvertResult result = ConvertV34Demo(data.data(), data.size(), converted, options);
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	if (!result.ok) {
		std::printf("%s: FAILED: %s\n", in.filename().string().c_str(), result.error.c_str());
		return false;
	}
	std::ofstream f(out, std::ios::binary);
	f.write((const char*)converted.data(), (std::streamsize)converted.size());
	const ConvertStats& s = result.stats;
	std::printf("%s: ok %.0f ms, %d -> %d frames, %lld messages, %lld entity packets, %lld updates, %lld props, "
		"%lld temp ents, %lld sounds, %lld baselines, user msgs %lld (%lld dropped), voice %lld dropped, usercmds %d dropped, "
		"classes %d -> %d%s%s, int NORMAL cleared %d, brushes kept %d moved %d missing %d unused %d, "
		"entity updates left out %lld%s%s%s\n",
		in.filename().string().c_str(), ms, s.framesIn, s.framesOut, s.messages, s.packetEntities, s.entityUpdates, s.props,
		s.tempEntities, s.sounds, s.baselines, s.userMessages, s.userMessagesDropped, s.voiceDropped, s.userCmdsDropped,
		s.classesIn, s.classesOut, s.droppedClasses.empty() ? "" : " dropped: ", s.droppedClasses.c_str(), s.intNormalCleared,
		s.brushKept, s.brushMoved, s.brushMissing, s.brushUnused, s.entitiesDropped, mapNote.c_str(),
		s.stoppedEarly.empty() ? "" : ", STOPPED EARLY: ", s.stoppedEarly.c_str());
	if (std::getenv("SOURCEMVM_CONVERT_VERBOSE") && !s.brushDetail.empty()) std::printf("   brushes:%s\n", s.brushDetail.c_str());
	return true;
}

int main(int argc, char** argv) {
	if (argc >= 3 && std::string(argv[1]) == "--maps") {
		g_maps = argv[2];
		argv += 2;
		argc -= 2;
	}
	if (argc == 4 && std::string(argv[1]) == "--dir") {
		fs::create_directories(argv[3]);
		bool ok = true;
		for (const auto& entry : fs::directory_iterator(argv[2])) {
			if (!entry.is_regular_file() || entry.path().extension() != ".dem") continue;
			ok &= ConvertOne(entry.path(), fs::path(argv[3]) / entry.path().filename());
		}
		return ok ? 0 : 1;
	}
	if (argc == 3) return ConvertOne(argv[1], argv[2]) ? 0 : 1;
	std::printf("SourceDemoConvertTool [--maps <cstrike/maps>] <in.dem> <out.dem>\n"
		"SourceDemoConvertTool [--maps <cstrike/maps>] --dir <folder> <output folder>\n");
	return 2;
}
