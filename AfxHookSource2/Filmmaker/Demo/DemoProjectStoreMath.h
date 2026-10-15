#pragma once

// Naming and index rules for %APPDATA%\HLAE\source_mvm\demos\<game>\<demo>/.
// Authored data lives in that folder's single project.mvm container (DemoProjectFileMath.h);
// generated caches stay as separate files beside it. Disk IO lives in DemoProjectStore.cpp;
// this header is engine-free and unit-tested.

#include <cwctype>
#include <string>
#include <vector>

namespace Filmmaker {

enum class ProjectGame {
	Cs2,
	Deadlock,
};

// Generated caches beside project.mvm. MatchInfo identifies only the Valve sidecars moved by
// earlier releases, for recovery; current scans read `<name>.dem.info` beside the demo.
enum class ProjectGenerated {
	Scoreboard,
	Repaired,
	MatchInfo,
	Keyframed, // the playable copy with dense full packets for fast backward seeks (DemoKeyframes.h)
};

struct ProjectIndexEntry {
	std::wstring demoPath;
	ProjectGame game = ProjectGame::Cs2;
	std::wstring folderName;
};

struct ProjectIndex {
	int version = 1;
	std::vector<ProjectIndexEntry> entries;
};

struct ProjectRenamePlan {
	bool found = false;
	bool renameFolder = false;
	std::wstring oldFolderName;
	std::wstring newFolderName;
};

inline bool WideEqualsI(const std::wstring& a, const std::wstring& b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i) {
		if (towlower(a[i]) != towlower(b[i])) return false;
	}
	return true;
}

inline std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
	if (a.empty()) return b;
	if (b.empty()) return a;
	const wchar_t last = a.back();
	if (last == L'\\' || last == L'/') return a + b;
	return a + L'\\' + b;
}

inline const wchar_t* ProjectGameDir(ProjectGame game) {
	return game == ProjectGame::Deadlock ? L"deadlock" : L"cs2";
}

inline const char* ProjectGameKey(ProjectGame game) {
	return game == ProjectGame::Deadlock ? "deadlock" : "cs2";
}

inline char AsciiLower(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

inline bool AsciiEqualsI(const char* a, const char* b) {
	if (!a || !b) return false;
	while (*a && *b) {
		if (AsciiLower(*a) != AsciiLower(*b)) return false;
		++a;
		++b;
	}
	return *a == 0 && *b == 0;
}

inline bool ParseProjectGame(const char* key, ProjectGame& out) {
	if (AsciiEqualsI(key, "cs2")) {
		out = ProjectGame::Cs2;
		return true;
	}
	if (AsciiEqualsI(key, "deadlock")) {
		out = ProjectGame::Deadlock;
		return true;
	}
	return false;
}

inline std::wstring DemoFileName(const std::wstring& demoPath) {
	if (demoPath.empty()) return {};
	const size_t pos = demoPath.find_last_of(L"\\/");
	return pos == std::wstring::npos ? demoPath : demoPath.substr(pos + 1);
}

inline std::wstring RelativeProjectDir(ProjectGame game, const std::wstring& folderName) {
	return JoinPath(ProjectGameDir(game), folderName);
}

inline const ProjectIndexEntry* FindIndexEntry(const ProjectIndex& index, const std::wstring& demoPath) {
	for (const auto& entry : index.entries) {
		if (WideEqualsI(entry.demoPath, demoPath)) return &entry;
	}
	return nullptr;
}

inline ProjectIndexEntry* FindIndexEntry(ProjectIndex& index, const std::wstring& demoPath) {
	for (auto& entry : index.entries) {
		if (WideEqualsI(entry.demoPath, demoPath)) return &entry;
	}
	return nullptr;
}

inline bool FolderNameTaken(const ProjectIndex& index, ProjectGame game, const std::wstring& folderName,
	const std::wstring& exceptDemoPath) {
	for (const auto& entry : index.entries) {
		if (entry.game != game) continue;
		if (!WideEqualsI(entry.folderName, folderName)) continue;
		if (!exceptDemoPath.empty() && WideEqualsI(entry.demoPath, exceptDemoPath)) continue;
		return true;
	}
	return false;
}

inline std::wstring CollisionFolderName(const std::wstring& base, int n) {
	return base + L"__" + std::to_wstring(n);
}

inline std::wstring AllocateFolderName(const ProjectIndex& index, ProjectGame game,
	const std::wstring& demoPath) {
	if (const ProjectIndexEntry* existing = FindIndexEntry(index, demoPath)) {
		if (existing->game == game && !existing->folderName.empty())
			return existing->folderName;
	}
	std::wstring base = DemoFileName(demoPath);
	if (base.empty()) base = L"demo";
	std::wstring candidate = base;
	int n = 2;
	while (FolderNameTaken(index, game, candidate, demoPath)) {
		candidate = CollisionFolderName(base, n++);
		if (n > 10000) break;
	}
	return candidate;
}

inline void UpsertIndexEntry(ProjectIndex& index, ProjectGame game, const std::wstring& demoPath,
	const std::wstring& folderName) {
	if (ProjectIndexEntry* existing = FindIndexEntry(index, demoPath)) {
		existing->game = game;
		existing->folderName = folderName;
		existing->demoPath = demoPath;
		return;
	}
	ProjectIndexEntry entry;
	entry.demoPath = demoPath;
	entry.game = game;
	entry.folderName = folderName;
	index.entries.push_back(std::move(entry));
}

inline ProjectRenamePlan PlanDemoRename(const ProjectIndex& index, ProjectGame game,
	const std::wstring& oldPath, const std::wstring& newPath) {
	ProjectRenamePlan plan;
	const ProjectIndexEntry* existing = FindIndexEntry(index, oldPath);
	if (!existing) return plan;
	plan.found = true;
	plan.oldFolderName = existing->folderName;
	ProjectIndex without = index;
	for (auto it = without.entries.begin(); it != without.entries.end(); ++it) {
		if (WideEqualsI(it->demoPath, oldPath)) {
			without.entries.erase(it);
			break;
		}
	}
	plan.newFolderName = AllocateFolderName(without, game, newPath);
	plan.renameFolder = !WideEqualsI(plan.newFolderName, plan.oldFolderName);
	return plan;
}

inline bool ApplyDemoRename(ProjectIndex& index, ProjectGame game, const std::wstring& oldPath,
	const std::wstring& newPath, ProjectRenamePlan& plan) {
	plan = PlanDemoRename(index, game, oldPath, newPath);
	if (!plan.found) return false;
	if (ProjectIndexEntry* existing = FindIndexEntry(index, oldPath)) {
		existing->demoPath = newPath;
		existing->game = game;
		existing->folderName = plan.newFolderName;
		return true;
	}
	return false;
}

inline const wchar_t* ProjectGeneratedFileName(ProjectGenerated kind) {
	switch (kind) {
	case ProjectGenerated::Scoreboard: return L"scoreboard.fmjson";
	case ProjectGenerated::Repaired: return L"repaired.dem";
	case ProjectGenerated::MatchInfo: return L"demo.info";
	case ProjectGenerated::Keyframed: return L"keyframed.dem";
	}
	return L"generated.bin";
}

inline std::wstring ProjectGeneratedPath(const std::wstring& projectDir, ProjectGenerated kind) {
	if (projectDir.empty()) return {};
	return JoinPath(projectDir, ProjectGeneratedFileName(kind));
}

inline std::wstring ValveMatchInfoPath(const std::wstring& demoPath) {
	if (demoPath.empty()) return {};
	return demoPath + L".info";
}

// The pre-project repair cache layout was <demo dir>\repaired\<name>.dem (matched by
// IsLegacyRepairedFolderName); repairs now land in the project folder as repaired.dem.
inline std::wstring RepairValidityMarkerPath(const std::wstring& repairedPath) {
	if (repairedPath.empty()) return {};
	return repairedPath + L".src";
}

inline std::wstring RepairCleanMarkerPath(const std::wstring& repairedPath) {
	if (repairedPath.empty()) return {};
	return repairedPath + L".clean";
}

// A demo COPY the tool generated into a project folder (not a demo of its own).
inline bool IsProjectDemoCopyFileName(const std::wstring& fileName) {
	return WideEqualsI(fileName, L"repaired.dem") || WideEqualsI(fileName, L"keyframed.dem");
}

inline bool IsLegacyRepairedFolderName(const std::wstring& folderName) {
	return WideEqualsI(folderName, L"repaired");
}

} // namespace Filmmaker
