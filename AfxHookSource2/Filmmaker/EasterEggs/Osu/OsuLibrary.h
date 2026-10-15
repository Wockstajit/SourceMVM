#pragma once

// The osu easter egg's beatmap library: every osu!standard difficulty on this PC, grouped into
// beatmap sets the way osu! shows them.
//
// Sources, in order (the first copy of a difficulty wins; duplicates are matched by MD5):
//   mvm     %APPDATA%\HLAE\osu\Songs (dropped .osz are imported here) and the bundled maps
//   stable  osu!stable's Songs folder: install from the .osz handler in the registry (default
//           %LOCALAPPDATA%\osu!), folder from BeatmapDirectory in osu!.<user>.cfg
//   lazer   osu!lazer's store (%APPDATA%\osu, or storage.ini's FullPath), indexed by client.realm
// Star ratings come from lazer's database (by SHA-256, or by MD5 for a stable copy).
//
// FAST: the scan runs on worker threads, parses headers + timing only (no slider curves), and
// remembers every file in %APPDATA%\HLAE\osu\library.v1.tsv keyed by path + size + mtime (lazer
// files are content-hashed, so by hash) -- a second open re-reads nothing.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Filmmaker {
namespace OsuLibrary {

struct Diff {
	std::wstring osu, audioSrc;
	std::string audioName, version, creator, md5;
	double stars = 0, ar = 5, od = 5, cs = 5, hp = 5, preview = -1;
	double firstMs = 0, lastMs = 0, drainMs = 0, bpmMin = 0, bpmMax = 0, bpmMain = 0;
	int circles = 0, sliders = 0, spinners = 0;
	std::function<std::wstring(const std::string&)> resolve; // a file named by the beatmap -> path
};

struct Set {
	std::string key, title, artist, creator, source, mapSource, tags, search; // search: lowercased haystack
	std::vector<Diff> diffs; // ascending stars
	double maxStars = 0, lengthMs = 0, bpm = 0;
};

void StartScan(bool force = false);
bool Scanning();
void Progress(int& done, int& total);
uint64_t Revision();
std::shared_ptr<const std::vector<Set>> Sets();

// osu!stable's install folder (skins live under it), or empty.
std::filesystem::path StableInstall();
// osu!lazer's data folder (client.realm, files/, game.ini), or empty.
std::filesystem::path LazerStore();

} // namespace OsuLibrary
} // namespace Filmmaker
