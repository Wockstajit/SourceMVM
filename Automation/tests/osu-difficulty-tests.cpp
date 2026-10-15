// OsuDifficulty.h (osu!lazer 2026.1005.0 star rating, ported) against the star ratings lazer itself
// stored for the player's beatmaps in client.realm. Like osu-realm-tests, this needs a real lazer
// install (OSU_REALM or %APPDATA%\osu\client.realm) and reports SKIPPED without one.
//
// Lazer stores the no-mod rating; matching it on hundreds of real maps is what proves the port, so
// the mod ratings (the reason the port exists) use the same code. A handful of maps may legitimately
// differ when lazer computed them with an older calculator version, so the bar is "nearly all".
#include "../../AfxHookSource2/Filmmaker/EasterEggs/Osu/OsuDifficulty.h"
#include "../../AfxHookSource2/Filmmaker/EasterEggs/Osu/OsuRealm.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
	namespace fs = std::filesystem;
	using namespace Filmmaker;
	fs::path realm;
	if (const char* e = std::getenv("OSU_REALM")) realm = e;
	else if (const char* a = std::getenv("APPDATA")) realm = fs::path(a) / "osu" / "client.realm";
	if (realm.empty() || !fs::exists(realm)) { std::printf("OsuDifficultyTests SKIPPED (no client.realm)\n"); return 0; }
	std::ifstream f(realm, std::ios::binary);
	std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	OsuRealm::Reader r(data);
	std::vector<OsuRealm::BeatmapSetFiles> sets;
	std::map<std::string, double> bySha, byMd5;
	std::string err;
	if (!r.ReadSets(sets, &err) || !r.ReadStars(bySha, byMd5, &err)) { std::printf("FAILED: %s\n", err.c_str()); return 1; }
	const fs::path store = realm.parent_path() / "files";
	const int limit = argc > 1 ? std::atoi(argv[1]) : 100000;
	int checked = 0, within01 = 0, within05 = 0;
	double sumErr = 0;
	struct Bad { double err, want, got; std::string name; };
	std::vector<Bad> worst;
	for (const auto& s : sets) {
		for (const auto& nf : s.files) {
			if (checked >= limit) break;
			if (nf.name.size() < 4 || nf.name.compare(nf.name.size() - 4, 4, ".osu") != 0) continue;
			auto it = bySha.find(nf.hash);
			if (it == bySha.end()) continue;
			std::ifstream in(store / nf.hash.substr(0, 1) / nf.hash.substr(0, 2) / nf.hash, std::ios::binary);
			std::stringstream ss; ss << in.rdbuf();
			Osu::Beatmap b;
			if (!Osu::ParseBeatmap(ss.str(), b) || b.mode != 0) continue;
			Osu::ApplyStacking(b);
			const double got = Osu::Diff::StarRating(b, Osu::Diff::Mods{});
			const double want = it->second, e = std::fabs(got - want);
			++checked; sumErr += e;
			if (e <= 0.01) ++within01;
			if (e <= 0.05) ++within05;
			worst.push_back({ e, want, got, nf.name });
		}
	}
	std::sort(worst.begin(), worst.end(), [](const Bad& a, const Bad& b) { return a.err > b.err; });
	std::printf("%d maps: mean |error| %.4f stars, %.1f%% within 0.01, %.1f%% within 0.05\n", checked,
		checked ? sumErr / checked : 0.0, checked ? 100.0 * within01 / checked : 0.0, checked ? 100.0 * within05 / checked : 0.0);
	for (size_t i = 0; i < worst.size() && i < 8; ++i)
		std::printf("  worst: %.3f (lazer %.3f, ours %.3f) %s\n", worst[i].err, worst[i].want, worst[i].got, worst[i].name.c_str());
	if (checked == 0) { std::printf("OsuDifficultyTests SKIPPED (no osu!standard maps with stored ratings)\n"); return 0; }
	if (within05 * 100 < checked * 95) { std::printf("FAILED: under 95%% of maps within 0.05 stars of lazer\n"); return 1; }
	std::printf("OsuDifficultyTests passed\n");
	return 0;
}
