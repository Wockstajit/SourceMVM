#include "../../AfxHookSource2/Filmmaker/Demo/DemoProjectStoreMath.h"

#include "../../AfxHookSource2/Filmmaker/Demo/DemoMatchInfoFile.h"
#include <fstream>
#include <iostream>
#include <string>

using namespace Filmmaker;

namespace {

int g_failed = 0;

void Expect(bool ok, const char* label) {
	if (ok) return;
	std::cerr << "FAILED " << label << "\n";
	++g_failed;
}

void ExpectWide(const std::wstring& actual, const std::wstring& expected, const char* label) {
	if (actual == expected) return;
	std::wcerr << L"FAILED " << label << L": got " << actual << L", expected " << expected << L"\n";
	++g_failed;
}

} // namespace

int main() {
	// Real filesystem preservation checks, including fresh CS2 metadata taking precedence.
	const auto scratch = std::filesystem::temp_directory_path() /
		(L"source-mvm-matchinfo-test-" + std::to_wstring(GetCurrentProcessId()));
	std::filesystem::create_directories(scratch);
	const auto archived = scratch / L"demo.info", native = scratch / L"match.dem.info";
	const std::string original("Valve\0match\xff", 12);
	{ std::ofstream out(archived, std::ios::binary); out.write(original.data(), original.size()); }
	auto read = [](const std::filesystem::path& path) {
		std::ifstream in(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	};
	Expect(RestoreMatchInfoIfMissing(archived, native) == MatchInfoRestore::Restored, "recover absent native sidecar");
	Expect(read(native) == original && read(archived) == original, "recovery keeps both files byte exact");
	{ std::ofstream out(native, std::ios::binary); out << "fresh CS2 metadata"; }
	Expect(RestoreMatchInfoIfMissing(archived, native) == MatchInfoRestore::Existing, "prefer current sidecar");
	Expect(read(native) == "fresh CS2 metadata" && read(archived) == original, "never overwrite either source");
	Expect(RestoreMatchInfoIfMissing(scratch / L"absent", scratch / L"absent.info") == MatchInfoRestore::Missing, "missing archive creates nothing");
	const auto blocked = scratch / L"blocked.info";
	std::filesystem::create_directory(blocked);
	Expect(RestoreMatchInfoIfMissing(archived, blocked) == MatchInfoRestore::Failed, "directory collision cannot be replaced");
	Expect(read(archived) == original && std::filesystem::is_directory(blocked), "failed restore preserves inputs");
	for (const auto& entry : std::filesystem::directory_iterator(scratch))
		Expect(entry.path().extension() != L".tmp", "completed attempts leave no temporary copy");
	std::filesystem::remove_all(scratch);

	ExpectWide(ProjectGeneratedFileName(ProjectGenerated::Scoreboard), L"scoreboard.fmjson",
		"scoreboard file");
	ExpectWide(ProjectGeneratedFileName(ProjectGenerated::Repaired), L"repaired.dem", "repaired file");
	ExpectWide(ProjectGeneratedFileName(ProjectGenerated::MatchInfo), L"demo.info", "match info file");
	ExpectWide(ProjectGeneratedFileName(ProjectGenerated::Keyframed), L"keyframed.dem", "keyframed file");
	ExpectWide(ProjectGeneratedFileName(ProjectGenerated::RoundPaths), L"roundpaths.fmbin", "round paths file");
	ExpectWide(ValveMatchInfoPath(L"C:\\replays\\match.dem"), L"C:\\replays\\match.dem.info",
		"valve match info");
	ExpectWide(RepairValidityMarkerPath(L"C:\\replays\\repaired\\match.dem"),
		L"C:\\replays\\repaired\\match.dem.src", "repair validity marker");
	ExpectWide(RepairCleanMarkerPath(L"C:\\replays\\repaired\\match.dem"),
		L"C:\\replays\\repaired\\match.dem.clean", "repair clean marker");
	ExpectWide(ProjectGeneratedPath(L"C:\\source_mvm\\demos\\cs2\\match.dem", ProjectGenerated::Repaired),
		L"C:\\source_mvm\\demos\\cs2\\match.dem\\repaired.dem", "project repaired path");
	Expect(IsProjectDemoCopyFileName(L"repaired.dem"), "project repaired name");
	Expect(IsProjectDemoCopyFileName(L"KEYFRAMED.dem"), "project keyframed name");
	Expect(IsProjectDemoCopyFileName(L"keyframed-b.dem"), "second keyframed slot is a copy too");
	Expect(IsKeyframedCopyFileName(L"Keyframed-B.dem") && !IsKeyframedCopyFileName(L"repaired.dem"),
		"keyframed slots, and only those");
	Expect(!IsProjectDemoCopyFileName(L"match.dem"), "a real demo is not a copy");
	Expect(IsLegacyRepairedFolderName(L"RePaIrEd"), "legacy repaired folder");

	// A keyframed copy keys the ORIGINAL demo's project: playing either slot looks the original
	// up in the copy's own project folder, never allocates a project named after the copy.
	const std::wstring projectDir =
		L"C:\\Users\\u\\AppData\\Roaming\\HLAE\\source_mvm\\demos\\cs2\\match.dem";
	for (const wchar_t* slot : {L"keyframed.dem", L"keyframed-b.dem", L"repaired.dem"}) {
		const DemoCopyLookup lookup = PlanDemoCopyLookup(JoinPath(projectDir, slot));
		Expect(lookup.source == DemoCopySource::ProjectFolder, "project copy maps through project.mvm");
		ExpectWide(lookup.projectDir, projectDir, "project copy looks in its own folder");
	}
	Expect(PlanDemoCopyLookup(L"D:/demos/match.dem/Keyframed.DEM").projectDir == L"D:/demos/match.dem",
		"forward slashes and any case");
	const DemoCopyLookup legacy = PlanDemoCopyLookup(L"C:\\replays\\repaired\\match.dem");
	Expect(legacy.source == DemoCopySource::LegacyRepaired, "legacy repaired copy");
	ExpectWide(legacy.original, L"C:\\replays\\match.dem", "legacy copy maps one folder up");
	Expect(PlanDemoCopyLookup(L"C:\\replays\\match.dem").source == DemoCopySource::None,
		"a real demo keys its own project");
	Expect(PlanDemoCopyLookup(L"keyframed.dem").source == DemoCopySource::None,
		"a bare copy name has no project folder to read");
	Expect(PlanDemoCopyLookup(L"repaired\\match.dem").source == DemoCopySource::None,
		"a legacy folder needs a demo folder above it");
	ExpectWide(DemoFileName(L"C:\\csgo\\replays\\foo.dem"), L"foo.dem", "filename");
	ExpectWide(RelativeProjectDir(ProjectGame::Cs2, L"foo.dem"), L"cs2\\foo.dem", "cs2 relative dir");
	ExpectWide(RelativeProjectDir(ProjectGame::Deadlock, L"replay.dem"), L"deadlock\\replay.dem",
		"deadlock relative dir");

	ProjectGame parsed = ProjectGame::Deadlock;
	Expect(ParseProjectGame("cs2", parsed) && parsed == ProjectGame::Cs2, "parse cs2");
	Expect(ParseProjectGame("Deadlock", parsed) && parsed == ProjectGame::Deadlock, "parse deadlock");
	Expect(!ParseProjectGame("hl2", parsed), "reject unknown game");

	ProjectIndex index;
	const std::wstring first = L"C:\\a\\match.dem";
	const std::wstring second = L"C:\\b\\match.dem";
	const std::wstring firstFolder = AllocateFolderName(index, ProjectGame::Cs2, first);
	ExpectWide(firstFolder, L"match.dem", "first folder uses filename");
	UpsertIndexEntry(index, ProjectGame::Cs2, first, firstFolder);
	const std::wstring secondFolder = AllocateFolderName(index, ProjectGame::Cs2, second);
	ExpectWide(secondFolder, L"match.dem__2", "collision suffix");
	UpsertIndexEntry(index, ProjectGame::Cs2, second, secondFolder);
	ExpectWide(AllocateFolderName(index, ProjectGame::Cs2, first), L"match.dem", "existing path reuses folder");

	const std::wstring deadlockSameName = L"E:\\deadlock\\match.dem";
	ExpectWide(AllocateFolderName(index, ProjectGame::Deadlock, deadlockSameName), L"match.dem",
		"other game does not collide");

	ProjectRenamePlan plan;
	Expect(ApplyDemoRename(index, ProjectGame::Cs2, first, L"C:\\a\\renamed.dem", plan), "rename found");
	ExpectWide(plan.oldFolderName, L"match.dem", "rename old folder");
	ExpectWide(plan.newFolderName, L"renamed.dem", "rename new folder");
	Expect(plan.renameFolder, "rename moves folder when the new name is free");
	Expect(FindIndexEntry(index, L"C:\\a\\renamed.dem") != nullptr, "index follows new path");
	Expect(FindIndexEntry(index, first) == nullptr, "old path removed from index");

	if (g_failed) {
		std::cerr << g_failed << " check(s) failed\n";
		return 1;
	}
	return 0;
}
