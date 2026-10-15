#include "../../AfxHookSource2/Filmmaker/Demo/DemoProjectPath.h"
#include "../../AfxHookSource2/Filmmaker/Demo/DemoProjectFileMath.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string FileStamp(const fs::path& path) {
	WIN32_FILE_ATTRIBUTE_DATA data{};
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
		return {};
	ULARGE_INTEGER size{};
	size.HighPart = data.nFileSizeHigh;
	size.LowPart = data.nFileSizeLow;
	ULARGE_INTEGER time{};
	time.HighPart = data.ftLastWriteTime.dwHighDateTime;
	time.LowPart = data.ftLastWriteTime.dwLowDateTime;
	return std::to_string(size.QuadPart) + ":" + std::to_string(time.QuadPart);
}

void WriteFile(const fs::path& path, const std::string& contents) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << contents;
}

bool SamePath(const std::wstring& actual, const fs::path& expected) {
	return fs::path(actual).lexically_normal() == expected.lexically_normal();
}

bool ExpectPath(const char* label, const std::wstring& actual, const fs::path& expected) {
	if (SamePath(actual, expected)) return true;
	std::wcerr << L"FAILED " << label << L": got " << actual << L", expected "
		<< expected.wstring() << L"\n";
	return false;
}

} // namespace

int main() {
	const fs::path root = fs::temp_directory_path() /
		(L"mvm-demo-project-path-" + std::to_wstring(GetCurrentProcessId()));
	std::error_code ec;
	fs::remove_all(root, ec);
	fs::create_directories(root / L"repaired", ec);

	const fs::path original = root / L"example.dem";
	const fs::path repaired = root / L"repaired" / L"example.dem";
	WriteFile(original, "original demo");
	WriteFile(repaired, "repaired demo");
	WriteFile(fs::path(repaired.wstring() + L".src"), FileStamp(original) + "|helper-stamp");

	bool ok = ExpectPath("valid repaired marker",
		Filmmaker::ResolveDemoProjectPath(repaired.wstring()), original);
	ok = ExpectPath("ordinary demo",
		Filmmaker::ResolveDemoProjectPath(original.wstring()), original) && ok;

	const fs::path missingMarker = root / L"repaired" / L"missing-marker.dem";
	WriteFile(missingMarker, "demo");
	ok = ExpectPath("missing marker",
		Filmmaker::ResolveDemoProjectPath(missingMarker.wstring()), missingMarker) && ok;

	const fs::path staleOriginal = root / L"stale.dem";
	const fs::path staleRepaired = root / L"repaired" / L"stale.dem";
	WriteFile(staleOriginal, "new source");
	WriteFile(staleRepaired, "old repair");
	WriteFile(fs::path(staleRepaired.wstring() + L".src"), "stale-stamp|helper");
	ok = ExpectPath("stale marker",
		Filmmaker::ResolveDemoProjectPath(staleRepaired.wstring()), staleRepaired) && ok;

	const fs::path mixedDirectory = root / L"RePaIrEd";
	fs::create_directories(mixedDirectory, ec);
	const fs::path mixedRepaired = mixedDirectory / original.filename();
	WriteFile(mixedRepaired, "repair");
	WriteFile(fs::path(mixedRepaired.wstring() + L".src"),
		FileStamp(original) + "|different-helper-version");
	ok = ExpectPath("case-insensitive repaired directory",
		Filmmaker::ResolveDemoProjectPath(mixedRepaired.wstring()), original) && ok;

	const fs::path projectDir = root / L"source_mvm" / L"example.dem";
	fs::create_directories(projectDir, ec);
	const fs::path projectRepaired = projectDir / L"repaired.dem";
	WriteFile(projectRepaired, "project repaired");
	WriteFile(fs::path(projectRepaired.wstring() + L".src"), FileStamp(original) + "|helper-stamp");
	{
		const std::wstring src = original.wstring();
		std::string utf8;
		int n = WideCharToMultiByte(CP_UTF8, 0, src.c_str(), (int)src.size(), nullptr, 0, nullptr, nullptr);
		utf8.resize((size_t)n);
		WideCharToMultiByte(CP_UTF8, 0, src.c_str(), (int)src.size(), utf8.data(), n, nullptr, nullptr);
		std::string escaped;
		for (char c : utf8) {
			if (c == '\\' || c == '"') escaped.push_back('\\');
			escaped.push_back(c);
		}
		Filmmaker::ProjectFile file;
		file.game = Filmmaker::ProjectGame::Cs2;
		Filmmaker::SetProjectSection(file, Filmmaker::kProjectFileInfoTag,
			"{\"game\":\"cs2\",\"demoPath\":\"" + escaped + "\"}");
		WriteFile(projectDir / L"project.mvm", Filmmaker::EncodeProjectFile(file));
	}
	ok = ExpectPath("project repaired.dem maps via project.mvm",
		Filmmaker::ResolveDemoProjectPath(projectRepaired.wstring()), original) && ok;

	fs::remove_all(root, ec);
	if (!ok) return 1;

	std::cout << "Demo project path test passed.\n";
	return 0;
}
