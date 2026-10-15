#include "DemoLibrary.h"

#include "DemoScanner.h"
#include "DemoHeaderReader.h"
#include "DemoInfoReader.h"
#include "DemoInfoHelper.h"
#include "DemoProjectStore.h"
#include "DemoRepair.h"
#include "../Platform/TextEncoding.h"
#include "../Platform/JsonBuilder.h"
#include "../../hlaeFolder.h"

#include <Windows.h>
#include <filesystem>
#include <algorithm>
#include <cwctype>  // towlower, for the case-insensitive path compare below
#include <atomic>
#include <thread>
#include <vector>
#include <cstdio>
#include <ctime>

namespace fs = std::filesystem;

namespace Filmmaker {

namespace {

// Case-insensitive whole-path comparison (Windows paths are case-insensitive).
bool SamePathCI(const std::wstring& a, const std::wstring& b) {
	if (a.size() != b.size())
		return false;
	return std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
		return towlower(x) == towlower(y);
	});
}

int64_t FileModifiedUnix(const std::wstring& path) {
	WIN32_FILE_ATTRIBUTE_DATA fad;
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
		return 0;
	ULARGE_INTEGER ull;
	ull.LowPart = fad.ftLastWriteTime.dwLowDateTime;
	ull.HighPart = fad.ftLastWriteTime.dwHighDateTime;
	// FILETIME is 100ns ticks since 1601-01-01; convert to unix seconds.
	return (int64_t)((ull.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

std::string DurationText(int seconds) {
	if (seconds <= 0)
		return "";
	int m = seconds / 60;
	int s = seconds % 60;
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
	return buf;
}

std::string DateText(int64_t unixSeconds) {
	if (unixSeconds <= 0)
		return "";
	std::time_t t = (std::time_t)unixSeconds;
	std::tm tm{};
	localtime_s(&tm, &t);
	char buf[64];
	std::strftime(buf, sizeof(buf), "%d %b %Y, %H:%M", &tm);
	return buf;
}

// Merge a full helper parse into an entry. The .dem.info matchmaking sidecar is
// authoritative for per-player K/A/D/score, but it splits the roster at the
// halfway index (wrong CT/T side) and has no MVPs or per-round timeline; take
// all of those - plus the CT/T-aligned team scores - from the helper's full parse.
void MergeHelper(DemoEntry& e, const DemoHelperResult& helper) {
	if (!helper.map.empty())
		e.map = helper.map;
	if (helper.durationSeconds > 0)
		e.durationSeconds = helper.durationSeconds;

	bool helperHasRounds = false;
	for (const auto& hp : helper.players)
		if (!hp.perRound.empty()) { helperHasRounds = true; break; }

	if (e.hasScoreboard && !e.players.empty()) {
		// Correct the sidecar roster from the full parse, matched by account id.
		for (auto& sp : e.players) {
			for (const auto& hp : helper.players) {
				if (hp.accountId != 0 && hp.accountId == sp.accountId) {
					if (!hp.name.empty() && hp.name != "[unknown]")
						sp.name = hp.name;
					sp.teamIndex = hp.teamIndex; // real end-of-match side (fixes CT/T)
					sp.mvps = hp.mvps;
					if (!hp.perRound.empty())
						sp.perRound = hp.perRound;
					break;
				}
			}
		}
		// Team scores from the full parse are aligned to the real CT/T sides, so the
		// score shown above each side's block matches the players grouped there.
		if (helper.teamScore0 + helper.teamScore1 > 0) {
			e.teamScore0 = helper.teamScore0;
			e.teamScore1 = helper.teamScore1;
		}
	} else if (helper.hasScoreboard && !helper.players.empty()) {
		// No sidecar: the helper's full parse is the only source.
		e.hasScoreboard = true;
		e.teamScore0 = helper.teamScore0;
		e.teamScore1 = helper.teamScore1;
		e.players = helper.players;
	}

	if (helperHasRounds)
		e.hasRounds = true;
}

} // namespace

DemoLibrary::~DemoLibrary() {
	Shutdown();
}

void DemoLibrary::Init() {
	m_folderStore.Load();
	m_fileStore.Load();
	m_hiddenStore.Load();
	if (!m_enrichWorker.joinable()) {
		{
			std::lock_guard<std::mutex> lock(m_enrichMutex);
			m_enrichStop = false; // re-arm after a prior Shutdown()
		}
		m_enrichWorker = std::thread(&DemoLibrary::EnrichWorker, this);
	}
}

void DemoLibrary::Shutdown() {
	m_cancel.store(true);
	if (m_worker.joinable())
		m_worker.join();
	m_cancel.store(false);

	{
		std::lock_guard<std::mutex> lock(m_enrichMutex);
		m_enrichStop = true;
	}
	m_enrichCv.notify_all();
	if (m_enrichWorker.joinable())
		m_enrichWorker.join();
}

std::wstring DemoLibrary::InstallRoot() const {
	const wchar_t* p = GetProcessFolderW();
	return p ? std::wstring(p) : std::wstring();
}

void DemoLibrary::StartScan() {
	// Stop a previous scan before starting a new one.
	m_cancel.store(true);
	if (m_worker.joinable())
		m_worker.join();
	m_cancel.store(false);
	m_scanning.store(true);
	m_worker = std::thread(&DemoLibrary::ScanWorker, this);
}

void DemoLibrary::ScanWorker() {
	std::vector<std::wstring> roots;
	std::wstring install = InstallRoot();
	if (!install.empty())
		roots.push_back(install);
	std::vector<std::wstring> singles;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (const auto& f : m_folderStore.Folders())
			roots.push_back(f);
		singles = m_fileStore.Files();
	}

	std::vector<std::wstring> files = DemoScanner::Scan(roots, m_cancel);

	// Individually added demo files join the scan results. A file that also lives under a
	// scanned root is already here, so dedupe case-insensitively; one whose file has since
	// been moved or deleted is skipped rather than listed as a broken row.
	for (const auto& single : singles) {
		if (m_cancel.load())
			break;
		std::error_code ec;
		if (!fs::is_regular_file(fs::path(single), ec))
			continue;
		const bool duplicate = std::any_of(files.begin(), files.end(),
			[&single](const std::wstring& existing) { return SamePathCI(existing, single); });
		if (!duplicate)
			files.push_back(single);
	}

	// Phase 1 - fast metadata (.dem header + .dem.info sidecar). Published first so
	// the list appears immediately, before the (slow) helper name parse.
	std::vector<DemoEntry> entries;
	entries.reserve(files.size());
	for (const auto& file : files) {
		if (m_cancel.load())
			break;

		DemoEntry e;
		e.path = file;
		fs::path p(file);
		e.fileName = WideToUtf8(p.filename().wstring());
		e.folder = WideToUtf8(p.parent_path().wstring());
		e.dateModified = FileModifiedUnix(file);

		DemoHeaderInfo header = ReadDemoHeader(file);
		if (header.ok) {
			e.map = header.map;
			e.durationSeconds = header.durationSeconds;
			e.networkProtocol = header.networkProtocol;
			e.buildNum = header.buildNum;
		}

		DemoInfoResult info = ReadDemoInfo(DemoProjectStore_MatchInfoForLoad(ProjectGame::Cs2, file));
		if (info.ok) {
			e.hasScoreboard = true;
			e.teamScore0 = info.teamScore0;
			e.teamScore1 = info.teamScore1;
			if (info.matchTime > 0)
				e.matchTime = info.matchTime; // the real match date, not the download date
			e.players = std::move(info.players);
		}

		entries.push_back(std::move(e));
	}

	// Sort newest-first by the date the card actually shows (real match time when
	// known, else the file's modified time) so the list order matches the labels.
	auto effectiveDate = [](const DemoEntry& e) { return e.matchTime > 0 ? e.matchTime : e.dateModified; };
	std::sort(entries.begin(), entries.end(), [&](const DemoEntry& a, const DemoEntry& b) {
		return effectiveDate(a) > effectiveDate(b);
	});

	std::vector<std::wstring> paths;
	paths.reserve(entries.size());
	for (const auto& e : entries)
		paths.push_back(e.path);

	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_entries = std::move(entries);
		// A scan rebuilds the list from disk, so the hidden flags have to be re-applied or
		// deleted demos would reappear on every rescan.
		ApplyHiddenLocked();
	}
	m_version.fetch_add(1);

	// Phase 2 - enrich each demo with real player names, the correct end-of-match
	// team sides, MVPs and the per-round timeline via the demoinfocs-golang helper
	// (cached as scoreboard.fmjson in the demo's source_mvm project folder). Demos are FULL-parsed in parallel (each helper is
	// its own process) with a bounded pool; results land in place and the version
	// is bumped per demo so the UI fills in live (RunFrame coalesces the bumps into
	// at most one render per frame).
	//
	// A full parse (not the old names-fast hint) is required because the .dem.info
	// matchmaking sidecar splits the roster at the halfway index - so it reports the
	// wrong CT/T side - and has no MVP or per-round data at all. MergeHelper keeps
	// the sidecar's authoritative K/A/D/score and takes the rest from the helper.
	{
		std::atomic<size_t> next{ 0 };
		auto worker = [this, &paths, &next]() {
			for (;;) {
				if (m_cancel.load())
					return;
				size_t i = next.fetch_add(1);
				if (i >= paths.size())
					return;

				DemoHelperResult helper = ReadDemoInfoViaHelper(paths[i], {}, &m_cancel);
				if (!helper.ok)
					continue;

				{
					std::lock_guard<std::mutex> lock(m_mutex);
					if (i >= m_entries.size() || m_entries[i].path != paths[i])
						continue; // a newer scan replaced the list; abandon
					MergeHelper(m_entries[i], helper);
				}
				m_version.fetch_add(1);
			}
		};

		unsigned hw = std::thread::hardware_concurrency();
		if (hw == 0) hw = 2;
		unsigned poolSize = hw < 8u ? hw : 8u; // avoid std::min (Windows.h min macro)
		std::vector<std::thread> pool;
		for (unsigned w = 0; w < poolSize; ++w)
			pool.emplace_back(worker);
		for (auto& t : pool)
			t.join();
	}

	m_version.fetch_add(1);
	m_scanning.store(false);
}

std::vector<DemoEntry> DemoLibrary::Snapshot() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_entries;
}

bool DemoLibrary::PathByIndex(size_t index, std::wstring& outPath) const {
	std::lock_guard<std::mutex> lock(m_mutex);
	if (index >= m_entries.size())
		return false;
	outPath = m_entries[index].path;
	return true;
}

bool DemoLibrary::AddFolder(const std::wstring& folder) {
	std::lock_guard<std::mutex> lock(m_mutex);

	const bool addedFolder = m_folderStore.Add(folder);
	if (addedFolder)
		m_folderStore.Save();

	// Re-adding a folder is the documented way to bring back demos removed with the delete
	// button, so un-hide everything under it. This also means re-picking an ALREADY-saved
	// folder is a meaningful action rather than a silent no-op.
	const size_t unhidden = m_hiddenStore.RemoveUnder(folder);
	if (unhidden) {
		m_hiddenStore.Save();
		ApplyHiddenLocked();
		m_version.fetch_add(1);
	}

	return addedFolder || unhidden > 0;
}

void DemoLibrary::ApplyHiddenLocked() {
	for (auto& e : m_entries)
		e.hidden = m_hiddenStore.Contains(e.path);
}

void DemoLibrary::NoteRenamed(const std::wstring& oldPath, const std::wstring& newPath) {
	if (oldPath.empty() || newPath.empty())
		return;
	std::lock_guard<std::mutex> lock(m_mutex);
	// An individually added demo is listed BY PATH, so without this the renamed file simply stops
	// being listed and reads as deleted.
	if (m_fileStore.Rename(oldPath, newPath))
		m_fileStore.Save();
	// A hidden demo stays hidden under its new name. Carrying the flag rather than dropping it is
	// the conservative direction: the user removed this demo from the list, and a rename is not
	// them asking for it back.
	if (m_hiddenStore.Contains(oldPath)) {
		m_hiddenStore.Remove(oldPath);
		m_hiddenStore.Add(newPath);
		m_hiddenStore.Save();
	}
	m_version.fetch_add(1);
}

bool DemoLibrary::HideDemo(size_t index) {
	std::lock_guard<std::mutex> lock(m_mutex);
	if (index >= m_entries.size())
		return false;
	if (!m_hiddenStore.Add(m_entries[index].path))
		return false;
	m_hiddenStore.Save();
	m_entries[index].hidden = true;
	m_version.fetch_add(1);
	return true;
}

bool DemoLibrary::UnhideDemo(const std::wstring& path) {
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_hiddenStore.Remove(path))
		return false;
	m_hiddenStore.Save();
	ApplyHiddenLocked();
	m_version.fetch_add(1);
	return true;
}

size_t DemoLibrary::UnhideAll() {
	std::lock_guard<std::mutex> lock(m_mutex);
	const size_t n = m_hiddenStore.Clear();
	if (n) {
		m_hiddenStore.Save();
		ApplyHiddenLocked();
		m_version.fetch_add(1);
	}
	return n;
}

size_t DemoLibrary::HideAllVisible() {
	std::lock_guard<std::mutex> lock(m_mutex);
	size_t n = 0;
	for (auto& e : m_entries) {
		if (e.hidden)
			continue;
		if (m_hiddenStore.Add(e.path)) {
			e.hidden = true;
			++n;
		}
	}
	if (n) {
		m_hiddenStore.Save();
		m_version.fetch_add(1);
	}
	return n;
}

std::vector<std::wstring> DemoLibrary::HiddenDemos() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_hiddenStore.Paths();
}

bool DemoLibrary::RemoveFolder(const std::wstring& folder) {
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_folderStore.Remove(folder))
		return false;
	m_folderStore.Save();
	return true;
}

std::vector<std::wstring> DemoLibrary::Folders() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_folderStore.Folders();
}

bool DemoLibrary::AddDemoFile(const std::wstring& path) {
	std::lock_guard<std::mutex> lock(m_mutex);

	const bool addedFile = m_fileStore.Add(path);
	if (addedFile)
		m_fileStore.Save();

	// Same contract as AddFolder: pointing at a demo again is how you undo the delete button,
	// so re-picking an ALREADY-saved file is meaningful rather than a silent no-op.
	const bool unhidden = m_hiddenStore.Remove(path);
	if (unhidden) {
		m_hiddenStore.Save();
		ApplyHiddenLocked();
		m_version.fetch_add(1);
	}

	return addedFile || unhidden;
}


std::string DemoLibrary::BuildJson() const {
	std::vector<DemoEntry> entries = Snapshot();
	std::vector<std::wstring> folders = Folders();

	JsonBuilder j;
	j.BeginObject();
	j.BoolField("scanning", m_scanning.load());
	j.StringField("installRoot", WideToUtf8(InstallRoot()));

	j.Key("folders");
	j.BeginArray();
	for (const auto& f : folders)
		j.ValueString(WideToUtf8(f));
	j.EndArray();

	// Count of removed-but-still-on-disk demos, so the UI can offer "restore".
	size_t hiddenCount = 0;
	for (const auto& e : entries)
		if (e.hidden) ++hiddenCount;
	j.IntField("hiddenCount", (int64_t)hiddenCount);

	j.Key("repair");
	j.BeginObject();
	j.IntField("state", (int64_t)DemoRepair_State());
	j.StringField("text", DemoRepair_StatusText());
	const std::wstring repairActiveDemo = DemoRepair_ActiveDemo();
	int repairDemoIndex = -1;
	for (size_t i = 0; i < entries.size(); ++i) {
		if (SamePathCI(entries[i].path, repairActiveDemo)) {
			repairDemoIndex = (int)i;
			break;
		}
	}
	j.IntField("demoIndex", repairDemoIndex);
	j.EndObject();

	j.Key("demos");
	j.BeginArray();
	for (size_t i = 0; i < entries.size(); ++i) {
		const DemoEntry& e = entries[i];
		// Hidden entries are skipped but keep their slot in m_entries, so the emitted "index"
		// stays a valid argument for Watch()/EnsureRounds()/HideDemo().
		if (e.hidden)
			continue;
		j.BeginObject();
		j.IntField("index", (int64_t)i);
		j.StringField("fileName", e.fileName);
		j.StringField("folder", e.folder);
		j.StringField("map", e.map);
		j.IntField("duration", e.durationSeconds);
		j.IntField("networkProtocol", e.networkProtocol);
		j.IntField("buildNum", e.buildNum);
		const DemoCompatibility compatibility = DemoRepair_ClassifyProtocol(e.networkProtocol);
		const char* compatibilityText = compatibility == DemoCompatibility::Supported ? "supported"
			: compatibility == DemoCompatibility::LegacyCheck ? "legacy"
			: compatibility == DemoCompatibility::Unsupported ? "unsupported" : "unknown";
		j.StringField("compatibility", compatibilityText);
		const DemoRepairCacheState cacheState = DemoRepair_CacheState(e.path);
		j.StringField("repairCache", cacheState == DemoRepairCacheState::Repaired ? "repaired"
			: cacheState == DemoRepairCacheState::Clean ? "clean" : "none");
		j.StringField("durationText", DurationText(e.durationSeconds));
		// Prefer the real match date (.dem.info matchtime) over the file's
		// download/modified time so the card shows when the match was played.
		int64_t dateUnix = e.matchTime > 0 ? e.matchTime : e.dateModified;
		j.IntField("date", dateUnix);
		j.StringField("dateText", DateText(dateUnix));
		j.BoolField("hasScoreboard", e.hasScoreboard);
		j.BoolField("hasRounds", e.hasRounds);
		j.IntField("teamScore0", e.teamScore0);
		j.IntField("teamScore1", e.teamScore1);

		j.Key("players");
		j.BeginArray();
		for (const auto& p : e.players) {
			j.BeginObject();
			j.StringField("name", p.name);
			j.IntField("accountId", p.accountId);
			j.IntField("k", p.kills);
			j.IntField("a", p.assists);
			j.IntField("d", p.deaths);
			j.IntField("score", p.score);
			j.IntField("mvps", p.mvps);
			j.IntField("team", p.teamIndex);

			j.Key("perRound");
			j.BeginArray();
			for (const auto& r : p.perRound) {
				j.BeginObject();
				j.IntField("k", r.kills);
				j.IntField("hs", r.headshots);
				j.IntField("d", r.died);
				j.IntField("w", r.won);
				j.IntField("side", r.side);
				j.IntField("mvp", r.mvp);
				j.EndObject();
			}
			j.EndArray();

			j.EndObject();
		}
		j.EndArray();

		j.EndObject();
	}
	j.EndArray();

	j.EndObject();
	return j.Str();
}

void DemoLibrary::EnsureRounds(size_t index) {
	std::wstring path;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (index >= m_entries.size())
			return;
		const DemoEntry& e = m_entries[index];
		if (e.hasRounds || !e.hasScoreboard)
			return; // already have the timeline, or nothing parseable to enrich
		path = e.path;
	}
	{
		std::lock_guard<std::mutex> lock(m_enrichMutex);
		if (m_enrichStop)
			return;
		if (!m_enrichRequested.insert(path).second)
			return; // already queued/processed this demo
		m_enrichQueue.push_back(path);
	}
	m_enrichCv.notify_one();
}

void DemoLibrary::EnrichWorker() {
	for (;;) {
		std::wstring path;
		{
			std::unique_lock<std::mutex> lock(m_enrichMutex);
			m_enrichCv.wait(lock, [this] { return m_enrichStop || !m_enrichQueue.empty(); });
			if (m_enrichStop)
				return;
			path = m_enrichQueue.front();
			m_enrichQueue.pop_front();
		}
		MergeRoundsForPath(path);
		m_version.fetch_add(1); // UI re-render is coalesced to once per frame
	}
}

void DemoLibrary::MergeRoundsForPath(const std::wstring& path) {
	// Full parse (no account-id hint) so the helper emits the per-round timeline.
	DemoHelperResult helper = ReadDemoInfoViaHelper(path);
	if (!helper.ok || helper.players.empty())
		return;

	std::lock_guard<std::mutex> lock(m_mutex);
	for (auto& e : m_entries) {
		if (e.path != path)
			continue;
		MergeHelper(e, helper);
		break;
	}
}

} // namespace Filmmaker
