#include "DemoFolderStore.h"

#include "../Platform/TextEncoding.h"
#include "../../hlaeFolder.h"

#include <fstream>
#include <filesystem>
#include <algorithm>

namespace Filmmaker {

namespace {

// Case-insensitive comparison of two folder paths (Windows paths are case-insensitive).
bool SamePath(const std::wstring& a, const std::wstring& b) {
	if (a.size() != b.size())
		return false;
	return std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
		return towlower(x) == towlower(y);
	});
}

} // namespace

std::wstring DemoFolderStore::FilePath() const {
	std::wstring path = GetHlaeRoamingAppDataFolderW();
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
		path += L'\\';
	path += L"filmmaker_demo_folders.txt";
	return path;
}

void DemoFolderStore::Load() {
	m_folders.clear();

	std::ifstream f(std::filesystem::path(FilePath()), std::ios::binary);
	if (!f.is_open())
		return;

	std::string line;
	while (std::getline(f, line)) {
		// Strip CR and trailing whitespace.
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
			line.pop_back();
		if (line.empty())
			continue;
		std::wstring w = Utf8ToWide(line);
		if (!w.empty())
			Add(w);
	}
}

bool DemoFolderStore::Save() const {
	std::ofstream f(std::filesystem::path(FilePath()), std::ios::binary | std::ios::trunc);
	if (!f.is_open())
		return false;
	for (const auto& folder : m_folders) {
		f << WideToUtf8(folder) << "\n";
	}
	return true;
}

bool DemoFolderStore::Add(const std::wstring& folder) {
	if (folder.empty())
		return false;
	for (const auto& existing : m_folders) {
		if (SamePath(existing, folder))
			return false;
	}
	m_folders.push_back(folder);
	return true;
}

bool DemoFolderStore::Remove(const std::wstring& folder) {
	for (auto it = m_folders.begin(); it != m_folders.end(); ++it) {
		if (SamePath(*it, folder)) {
			m_folders.erase(it);
			return true;
		}
	}
	return false;
}

} // namespace Filmmaker
