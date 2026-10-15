#include "DemoFileStore.h"

#include "../Platform/TextEncoding.h"
#include "../../hlaeFolder.h"

#include <fstream>
#include <filesystem>
#include <algorithm>

namespace Filmmaker {

namespace {

// Case-insensitive path comparison (Windows paths are case-insensitive).
bool SamePath(const std::wstring& a, const std::wstring& b) {
	if (a.size() != b.size())
		return false;
	return std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
		return towlower(x) == towlower(y);
	});
}

} // namespace

std::wstring DemoFileStore::FilePath() const {
	std::wstring path = GetHlaeRoamingAppDataFolderW();
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
		path += L'\\';
	path += L"filmmaker_demo_files.txt";
	return path;
}

void DemoFileStore::Load() {
	m_files.clear();

	std::ifstream f(std::filesystem::path(FilePath()), std::ios::binary);
	if (!f.is_open())
		return;

	std::string line;
	while (std::getline(f, line)) {
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
			line.pop_back();
		if (line.empty())
			continue;
		std::wstring w = Utf8ToWide(line);
		if (!w.empty())
			Add(w);
	}
}

bool DemoFileStore::Save() const {
	std::ofstream f(std::filesystem::path(FilePath()), std::ios::binary | std::ios::trunc);
	if (!f.is_open())
		return false;
	for (const auto& path : m_files)
		f << WideToUtf8(path) << "\n";
	return true;
}

bool DemoFileStore::Rename(const std::wstring& oldPath, const std::wstring& newPath) {
	if (oldPath.empty() || newPath.empty())
		return false;
	bool moved = false;
	for (auto& f : m_files) {
		if (0 == _wcsicmp(f.c_str(), oldPath.c_str())) {
			f = newPath;
			moved = true;
		}
	}
	return moved;
}

bool DemoFileStore::Add(const std::wstring& path) {
	if (path.empty())
		return false;
	for (const auto& existing : m_files) {
		if (SamePath(existing, path))
			return false;
	}
	m_files.push_back(path);
	return true;
}

} // namespace Filmmaker
