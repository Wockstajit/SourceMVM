#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "OsuLibrary.h"

#include "OsuMath.h"
#include "OsuPlatform.h"
#include "OsuRealm.h"

#include "../../Platform/TextEncoding.h"
#include "../../Debug/MvmDebugLog.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;

namespace Filmmaker {
namespace OsuLibrary {
namespace {

using namespace Osu;
using namespace OsuPlatform;

std::mutex g_mx;
std::shared_ptr<const std::vector<Set>> g_sets = std::make_shared<std::vector<Set>>();
std::atomic<uint64_t> g_rev{ 1 };
std::atomic<bool> g_scanning{ false };
std::atomic<int> g_done{ 0 }, g_total{ 0 };

// ---- one parsed difficulty (also the cache record) ----------------------------------------------
struct Record {
	bool ok = false;
	std::string title, artist, creator, version, mapSource, tags, audio, background, md5;
	double ar = 5, od = 5, cs = 5, hp = 5, preview = -1, firstMs = 0, lastMs = 0, drainMs = 0;
	double bpmMin = 0, bpmMax = 0, bpmMain = 0;
	int circles = 0, sliders = 0, spinners = 0;
};

std::string Clean(std::string s) { for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return s; }

std::string Serialize(const std::string& key, uint64_t size, int64_t mtime, const Record& r) {
	std::ostringstream o;
	o.precision(10);
	o << Clean(key) << '\t' << size << '\t' << mtime << '\t' << (r.ok ? 1 : 0);
	if (r.ok) {
		for (const std::string* f : { &r.title, &r.artist, &r.creator, &r.version, &r.mapSource, &r.tags, &r.audio, &r.background, &r.md5 })
			o << '\t' << Clean(*f);
		for (double d : { r.ar, r.od, r.cs, r.hp, r.preview, r.firstMs, r.lastMs, r.drainMs, r.bpmMin, r.bpmMax, r.bpmMain })
			o << '\t' << d;
		o << '\t' << r.circles << '\t' << r.sliders << '\t' << r.spinners;
	}
	return o.str();
}

bool Deserialize(const std::string& line, std::string& key, uint64_t& size, int64_t& mtime, Record& r) {
	const auto f = Split(line, '\t');
	if (f.size() < 4) return false;
	key = f[0]; size = std::strtoull(f[1].c_str(), nullptr, 10); mtime = std::strtoll(f[2].c_str(), nullptr, 10);
	r.ok = f[3] == "1";
	if (!r.ok) return true;
	if (f.size() < 4 + 9 + 11 + 3) return false;
	size_t i = 4;
	for (std::string* s : { &r.title, &r.artist, &r.creator, &r.version, &r.mapSource, &r.tags, &r.audio, &r.background, &r.md5 }) *s = f[i++];
	for (double* d : { &r.ar, &r.od, &r.cs, &r.hp, &r.preview, &r.firstMs, &r.lastMs, &r.drainMs, &r.bpmMin, &r.bpmMax, &r.bpmMain }) *d = Num(f[i++]);
	r.circles = (int)Num(f[i++]); r.sliders = (int)Num(f[i++]); r.spinners = (int)Num(f[i++]);
	return true;
}

Record ParseRecord(const std::string& text) {
	Record r;
	Beatmap b;
	if (!ParseBeatmap(text, b, nullptr, false)) return r;
	const Summary s = Summarize(b);
	r.ok = true;
	r.title = b.title; r.artist = b.artist; r.creator = b.creator; r.version = b.version;
	r.mapSource = b.source; r.tags = b.tags; r.audio = b.audio; r.background = b.background;
	r.md5 = Md5Hex(text);
	r.ar = b.ar; r.od = b.od; r.cs = b.cs; r.hp = b.hp; r.preview = b.preview;
	r.firstMs = s.firstMs; r.lastMs = s.lastMs; r.drainMs = s.drainMs;
	r.bpmMin = s.bpmMin; r.bpmMax = s.bpmMax; r.bpmMain = s.bpmMain;
	r.circles = s.circles; r.sliders = s.sliders; r.spinners = s.spinners;
	return r;
}

fs::path CachePath() { return RoamingRoot() / L"library.v1.tsv"; }

// ---- jobs ---------------------------------------------------------------------------------------
struct Job {
	std::wstring osu;
	std::string cacheKey;    // path (filesystem) or "lazer:<sha>"
	bool immutable = false;  // lazer store files never change in place
	std::string setKey, source, sha;
	bool bundled = false;    // shipped with the mod (bin\osu\Songs): only used when the player lacks the set
	std::function<std::wstring(const std::string&)> resolve; // a beatmap-relative filename -> file
	Record rec;
	uint64_t size = 0; int64_t mtime = 0;
};

std::wstring RegDefault(HKEY root, const wchar_t* key) {
	wchar_t buf[1024] = {};
	DWORD size = sizeof(buf);
	if (RegGetValueW(root, key, nullptr, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
	return buf;
}

fs::path FindStableInstall() {
	std::error_code ec;
	fs::path install;
	for (const wchar_t* k : { L"osustable.File.osz\\Shell\\Open\\Command", L"osu\\shell\\open\\command" }) {
		const std::wstring cmd = RegDefault(HKEY_CLASSES_ROOT, k);
		const size_t a = cmd.find(L'"'), b = a == std::wstring::npos ? a : cmd.find(L'"', a + 1);
		if (b != std::wstring::npos) {
			const fs::path exe = cmd.substr(a + 1, b - a - 1);
			if (_wcsicmp(exe.filename().c_str(), L"osu!.exe") == 0 && fs::exists(exe, ec)) { install = exe.parent_path(); break; }
		}
	}
	if (install.empty()) {
		wchar_t local[MAX_PATH] = {};
		if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) install = fs::path(local) / L"osu!";
	}
	if (install.empty() || !fs::exists(install / L"osu!.exe", ec)) return {};
	return install;
}

fs::path StableSongs() {
	std::error_code ec;
	const fs::path install = FindStableInstall();
	if (install.empty()) return {};
	fs::path songs = install / L"Songs";
	for (auto& f : fs::directory_iterator(install, ec)) {
		const std::wstring n = f.path().filename().wstring();
		if (n.size() < 9 || _wcsnicmp(n.c_str(), L"osu!.", 5) != 0 || _wcsicmp(n.c_str(), L"osu!.cfg") == 0) continue;
		if (_wcsicmp(f.path().extension().c_str(), L".cfg") != 0) continue;
		std::istringstream in(ReadFile(f.path()));
		std::string line;
		while (std::getline(in, line)) {
			const size_t eq = line.find('=');
			if (eq == std::string::npos || Trim(line.substr(0, eq)) != "BeatmapDirectory") continue;
			const fs::path dir = Utf8ToWide(Trim(line.substr(eq + 1)));
			if (!dir.empty()) songs = dir.is_absolute() ? dir : install / dir;
		}
	}
	return fs::exists(songs, ec) ? songs : fs::path();
}

fs::path LazerRoot() {
	wchar_t app[MAX_PATH] = {};
	if (!GetEnvironmentVariableW(L"APPDATA", app, MAX_PATH)) return {};
	fs::path root = fs::path(app) / L"osu";
	std::error_code ec;
	const fs::path ini = root / L"storage.ini";
	if (fs::exists(ini, ec)) {
		std::istringstream in(ReadFile(ini));
		std::string line;
		while (std::getline(in, line)) {
			const size_t eq = line.find('=');
			if (eq != std::string::npos && Trim(line.substr(0, eq)) == "FullPath") {
				const fs::path p = Utf8ToWide(Trim(line.substr(eq + 1)));
				if (fs::exists(p, ec)) root = p;
			}
		}
	}
	return fs::exists(root / L"client.realm", ec) ? root : fs::path();
}

void ImportOsz(const fs::path& root) {
	std::error_code ec;
	fs::create_directories(root / L"Songs", ec);
	std::vector<fs::path> found;
	for (const fs::path& dir : { root, root / L"Songs" })
		for (auto& e : fs::directory_iterator(dir, ec))
			if (e.is_regular_file() && _wcsicmp(e.path().extension().c_str(), L".osz") == 0) found.push_back(e.path());
	for (const fs::path& osz : found) {
		const fs::path dest = root / L"Songs" / osz.stem();
		fs::create_directories(dest, ec);
		wchar_t sys[MAX_PATH] = {};
		GetSystemDirectoryW(sys, MAX_PATH);
		const bool ok = RunHidden(L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + osz.wstring() + L"\" -C \"" + dest.wstring() + L"\"", 60000);
		if (ok) { fs::create_directories(root / L"imported", ec); fs::rename(osz, root / L"imported" / osz.filename(), ec); }
		MvmDebugLog_LinefAlways("osu.import", "%s %s", ok ? "imported" : "FAILED", WideToUtf8(osz.filename().wstring()).c_str());
	}
}

void FolderJobs(const fs::path& songs, const char* source, std::vector<Job>& jobs) {
	std::error_code ec;
	if (songs.empty() || !fs::exists(songs, ec)) return;
	for (auto& set : fs::directory_iterator(songs, ec)) {
		if (!set.is_directory()) continue;
		const fs::path dir = set.path();
		for (auto& f : fs::directory_iterator(dir, ec)) {
			if (!f.is_regular_file() || _wcsicmp(f.path().extension().c_str(), L".osu") != 0) continue;
			Job j;
			j.osu = f.path().wstring();
			j.cacheKey = WideToUtf8(j.osu);
			j.setKey = std::string(source) + ":" + WideToUtf8(dir.wstring());
			j.source = source;
			j.size = (uint64_t)f.file_size(ec);
			j.mtime = (int64_t)f.last_write_time(ec).time_since_epoch().count();
			j.resolve = [dir](const std::string& name) {
				std::string n = Trim(name);
				for (char& c : n) if (c == '\\') c = '/';
				return (dir / Utf8ToWide(n)).wstring();
			};
			jobs.push_back(std::move(j));
		}
	}
}

struct LazerData {
	std::map<std::string, double> starsBySha, starsByMd5;
};

void LazerJobs(std::vector<Job>& jobs, LazerData& lz) {
	const fs::path root = LazerRoot();
	if (root.empty()) return;
	std::vector<uint8_t> data;
	{
		std::ifstream f(root / L"client.realm", std::ios::binary);
		if (!f) return;
		data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	}
	OsuRealm::Reader reader(data);
	std::vector<OsuRealm::BeatmapSetFiles> sets;
	std::string err;
	if (!reader.ReadSets(sets, &err)) { MvmDebugLog_LinefAlways("osu.scan", "lazer: %s", err.c_str()); return; }
	reader.ReadStars(lz.starsBySha, lz.starsByMd5, &err);
	const fs::path store = root / L"files";
	auto pathOf = [store](const std::string& h) {
		return (store / Utf8ToWide(h.substr(0, 1)) / Utf8ToWide(h.substr(0, 2)) / Utf8ToWide(h)).wstring();
	};
	int si = 0;
	for (const auto& s : sets) {
		auto files = std::make_shared<std::vector<OsuRealm::NamedFile>>(s.files);
		const std::string setKey = "lazer:" + std::to_string(si++);
		for (const auto& nf : s.files) {
			if (nf.name.size() < 5 || _stricmp(nf.name.c_str() + nf.name.size() - 4, ".osu") != 0 || nf.hash.size() < 3) continue;
			Job j;
			j.osu = pathOf(nf.hash);
			j.cacheKey = "lazer:" + nf.hash;
			j.immutable = true;
			j.setKey = setKey;
			j.source = "lazer";
			j.sha = nf.hash;
			j.resolve = [files, pathOf](const std::string& name) -> std::wstring {
				// lazer stores names with '/', maps may write '\\' and stray spaces.
				auto norm = [](std::string v) { v = Trim(v); for (char& c : v) if (c == '\\') c = '/'; return v; };
				const std::string want = norm(name);
				for (const auto& o : *files)
					if (o.hash.size() >= 3 && _stricmp(norm(o.name).c_str(), want.c_str()) == 0) return pathOf(o.hash);
				return L"";
			};
			jobs.push_back(std::move(j));
		}
	}
}

void ScanWorker(bool force) {
	const double t0 = QpcMs();
	ImportOsz(RoamingRoot());
	std::vector<Job> jobs;
	FolderJobs(RoamingRoot() / L"Songs", "mvm", jobs);
	FolderJobs(StableSongs(), "stable", jobs);
	LazerData lz;
	LazerJobs(jobs, lz);
	// The maps shipped with the mod come LAST: the player's own copy of a set (same .osu MD5, or
	// the same title / artist / mapper) wins and the bundled one is dropped, so nothing is listed twice.
	const size_t firstBundled = jobs.size();
	FolderJobs(BundledRoot() / L"Songs", "mvm", jobs);
	for (size_t i = firstBundled; i < jobs.size(); ++i) jobs[i].bundled = true;
	g_total.store((int)jobs.size());
	g_done.store(0);

	// Cache.
	std::unordered_map<std::string, std::tuple<uint64_t, int64_t, Record>> cache;
	if (!force) {
		std::istringstream in(ReadFile(CachePath()));
		std::string line;
		while (std::getline(in, line)) {
			std::string key; uint64_t size; int64_t mtime; Record r;
			if (Deserialize(line, key, size, mtime, r)) cache[key] = { size, mtime, std::move(r) };
		}
	}
	std::atomic<size_t> next{ 0 };
	std::atomic<int> parsed{ 0 };
	auto work = [&] {
		for (size_t i; (i = next.fetch_add(1)) < jobs.size();) {
			Job& j = jobs[i];
			auto it = cache.find(j.cacheKey);
			if (it != cache.end() && (j.immutable || (std::get<0>(it->second) == j.size && std::get<1>(it->second) == j.mtime))) {
				j.rec = std::get<2>(it->second);
			} else {
				j.rec = ParseRecord(ReadFile(j.osu));
				parsed.fetch_add(1);
			}
			g_done.fetch_add(1);
		}
	};
	const unsigned n = std::clamp(std::thread::hardware_concurrency(), 2u, 12u);
	std::vector<std::thread> pool;
	for (unsigned t = 0; t < n; ++t) pool.emplace_back(work);
	for (auto& t : pool) t.join();

	// Save the cache (everything seen this time, so deleted maps fall out of it).
	{
		std::error_code ec;
		fs::create_directories(RoamingRoot(), ec);
		std::ofstream out(CachePath(), std::ios::binary | std::ios::trunc);
		for (const Job& j : jobs) out << Serialize(j.cacheKey, j.size, j.mtime, j.rec) << '\n';
	}

	// Group into sets; a difficulty seen before (same MD5) is skipped.
	std::map<std::string, Set> sets;
	std::vector<std::string> order;
	std::set<std::string> seenMd5;
	std::error_code ec;
	int notStandard = 0, duplicate = 0, noAudio = 0, bundledDiffs = 0;
	std::set<std::string> ownSets; // lower-case title / artist / creator of the player's own sets
	auto identity = [](const Record& r) {
		std::string id = r.title + "\n" + r.artist + "\n" + r.creator;
		for (char& c : id) c = (char)tolower((unsigned char)c);
		return id;
	};
	for (Job& j : jobs) {
		const Record& r = j.rec;
		if (!r.ok) { notStandard++; continue; }
		if (!j.bundled) ownSets.insert(identity(r));
		else if (ownSets.count(identity(r))) { duplicate++; continue; }
		if (!r.md5.empty() && !seenMd5.insert(r.md5).second) { duplicate++; continue; }
		const std::wstring audio = j.resolve(r.audio);
		if (audio.empty() || !fs::exists(audio, ec)) {
			if (noAudio++ < 12)
				MvmDebugLog_LinefAlways("osu.scan", "skipped (audio '%s' not found): %s - %s [%s] %s", r.audio.c_str(),
					r.artist.c_str(), r.title.c_str(), r.version.c_str(), WideToUtf8(j.osu).c_str());
			continue;
		}
		auto it = sets.find(j.setKey);
		if (it == sets.end()) {
			Set s;
			s.key = j.setKey; s.title = r.title; s.artist = r.artist; s.creator = r.creator;
			s.source = j.source; s.mapSource = r.mapSource; s.tags = r.tags;
			it = sets.emplace(j.setKey, std::move(s)).first;
			order.push_back(j.setKey);
		}
		Set& s = it->second;
		if (j.bundled) bundledDiffs++;
		Diff d;
		d.resolve = j.resolve;
		d.osu = j.osu; d.audioSrc = audio; d.audioName = r.audio;
		d.version = r.version; d.creator = r.creator; d.md5 = r.md5;
		d.ar = r.ar; d.od = r.od; d.cs = r.cs; d.hp = r.hp; d.preview = r.preview;
		d.firstMs = r.firstMs; d.lastMs = r.lastMs; d.drainMs = r.drainMs;
		d.bpmMin = r.bpmMin; d.bpmMax = r.bpmMax; d.bpmMain = r.bpmMain;
		d.circles = r.circles; d.sliders = r.sliders; d.spinners = r.spinners;
		auto st = !j.sha.empty() ? lz.starsBySha.find(j.sha) : lz.starsBySha.end();
		if (st != lz.starsBySha.end()) d.stars = st->second;
		else { auto sm = lz.starsByMd5.find(r.md5); if (sm != lz.starsByMd5.end()) d.stars = sm->second; }
		s.diffs.push_back(std::move(d));
	}
	auto list = std::make_shared<std::vector<Set>>();
	for (const std::string& k : order) {
		Set s = std::move(sets[k]);
		std::stable_sort(s.diffs.begin(), s.diffs.end(), [](const Diff& a, const Diff& b) {
			if (a.stars != b.stars) return a.stars < b.stars;
			return a.od + a.ar < b.od + b.ar;
		});
		for (const Diff& d : s.diffs) { s.maxStars = std::max(s.maxStars, d.stars); s.lengthMs = std::max(s.lengthMs, d.lastMs); }
		if (!s.diffs.empty()) s.bpm = s.diffs.back().bpmMain;
		std::string hay = s.title + " " + s.artist + " " + s.creator + " " + s.mapSource + " " + s.tags + " " + s.source;
		for (const Diff& d : s.diffs) hay += " " + d.version + " " + d.creator;
		for (char& c : hay) c = (char)tolower((unsigned char)c);
		s.search = std::move(hay);
		list->push_back(std::move(s));
	}
	{
		std::lock_guard<std::mutex> lock(g_mx);
		g_sets = list;
	}
	g_rev.fetch_add(1);
	g_scanning.store(false);
	size_t diffs = 0; for (const Set& s : *list) diffs += s.diffs.size();
	MvmDebugLog_LinefAlways("osu.scan", "%zu sets, %zu difficulties from %zu files (%d parsed, rest cached) in %.0f ms;"
		" skipped: %d other modes / not beatmaps, %d duplicates, %d without audio; %d bundled difficulties kept"
		" (%zu bundled files in %s)",
		list->size(), diffs, jobs.size(), parsed.load(), QpcMs() - t0, notStandard, duplicate, noAudio, bundledDiffs,
		jobs.size() - firstBundled, WideToUtf8((BundledRoot() / L"Songs").wstring()).c_str());
}

} // namespace

void StartScan(bool force) {
	bool expected = false;
	if (!g_scanning.compare_exchange_strong(expected, true)) return;
	std::thread(ScanWorker, force).detach();
}
bool Scanning() { return g_scanning.load(); }
void Progress(int& done, int& total) { done = g_done.load(); total = g_total.load(); }
uint64_t Revision() { return g_rev.load(); }
std::shared_ptr<const std::vector<Set>> Sets() { std::lock_guard<std::mutex> lock(g_mx); return g_sets; }

fs::path StableInstall() { return FindStableInstall(); }
fs::path LazerStore() { return LazerRoot(); }

} // namespace OsuLibrary
} // namespace Filmmaker
