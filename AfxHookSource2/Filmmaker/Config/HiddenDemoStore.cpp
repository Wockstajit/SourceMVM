#include "HiddenDemoStore.h"

#include "../Platform/TextEncoding.h"
#include "../../hlaeFolder.h"

#include <fstream>
#include <filesystem>
#include <algorithm>

namespace Filmmaker {

namespace {

// Windows paths are case-insensitive, so every comparison here is too.
bool SamePath(const std::wstring& a, const std::wstring& b) {
	if (a.size() != b.size())
		return false;
	return std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
		return towlower(x) == towlower(y);
	});
}

// True when `path` sits under `folder` at any depth. Compares on a normalised form so
// mixed separators and a trailing slash on the folder all behave.
bool IsUnder(const std::wstring& path, const std::wstring& folder) {
	auto norm = [](std::wstring s) {
		std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
			return c == L'/' ? L'\\' : towlower(c);
		});
		while (!s.empty() && s.back() == L'\\')
			s.pop_back();
		return s;
	};
	const std::wstring p = norm(path);
	const std::wstring f = norm(folder);
	if (f.empty() || p.size() <= f.size())
		return false;
	if (p.compare(0, f.size(), f) != 0)
		return false;
	return p[f.size()] == L'\\'; // a real child, not just a shared prefix
}

} // namespace

std::wstring HiddenDemoStore::FilePath() const {
	std::wstring path = GetHlaeRoamingAppDataFolderW();
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
		path += L'\\';
	path += m_fileName;
	return path;
}

void HiddenDemoStore::Load() {
	m_paths.clear();

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

bool HiddenDemoStore::Save() const {
	std::ofstream f(std::filesystem::path(FilePath()), std::ios::binary | std::ios::trunc);
	if (!f.is_open())
		return false;
	for (const auto& p : m_paths)
		f << WideToUtf8(p) << "\n";
	return true;
}

bool HiddenDemoStore::Contains(const std::wstring& path) const {
	for (const auto& p : m_paths)
		if (SamePath(p, path))
			return true;
	return false;
}

bool HiddenDemoStore::Add(const std::wstring& path) {
	if (path.empty())
		return false;
	if (Contains(path))
		return false;
	m_paths.push_back(path);
	return true;
}

bool HiddenDemoStore::Remove(const std::wstring& path) {
	for (auto it = m_paths.begin(); it != m_paths.end(); ++it) {
		if (SamePath(*it, path)) {
			m_paths.erase(it);
			return true;
		}
	}
	return false;
}

size_t HiddenDemoStore::RemoveUnder(const std::wstring& folder) {
	const size_t before = m_paths.size();
	m_paths.erase(
		std::remove_if(m_paths.begin(), m_paths.end(),
			[&](const std::wstring& p) { return IsUnder(p, folder) || SamePath(p, folder); }),
		m_paths.end());
	return before - m_paths.size();
}

size_t HiddenDemoStore::Clear() {
	const size_t n = m_paths.size();
	m_paths.clear();
	return n;
}

} // namespace Filmmaker
