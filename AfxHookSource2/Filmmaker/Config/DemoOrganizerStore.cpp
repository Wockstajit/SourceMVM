#include "DemoOrganizerStore.h"

#include "../Platform/TextEncoding.h"
#include "../../hlaeFolder.h"

#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cstdlib>

namespace Filmmaker {

namespace {

// Folder names go into a newline-separated command queue and into a single-line record file,
// so control characters have to go. Trimmed and length-capped so a header row stays readable.
std::string SanitizeName(const std::string& in) {
	std::string out;
	out.reserve(in.size());
	for (char c : in) {
		if (c == '\r' || c == '\n' || c == '\t')
			continue;
		out.push_back(c);
	}
	size_t b = out.find_first_not_of(' ');
	size_t e = out.find_last_not_of(' ');
	if (b == std::string::npos)
		return std::string();
	out = out.substr(b, e - b + 1);
	if (out.size() > 40)
		out.resize(40);
	return out;
}

// Splits "<token> <rest>" at the first space. Returns false when there is no token.
bool SplitFirst(const std::string& line, std::string& token, std::string& rest) {
	size_t sp = line.find(' ');
	if (sp == std::string::npos) {
		token = line;
		rest.clear();
		return !token.empty();
	}
	token = line.substr(0, sp);
	rest = line.substr(sp + 1);
	return !token.empty();
}

} // namespace

std::wstring NormalizeDemoKey(const std::wstring& path) {
	std::wstring out;
	out.reserve(path.size());
	for (wchar_t c : path)
		out.push_back(c == L'/' ? L'\\' : (wchar_t)towlower(c));
	while (!out.empty() && out.back() == L'\\')
		out.pop_back();
	return out;
}

std::wstring DemoOrganizerStore::FilePath() const {
	std::wstring path = GetHlaeRoamingAppDataFolderW();
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
		path += L'\\';
	path += L"filmmaker_demo_organizer.txt";
	return path;
}

void DemoOrganizerStore::Load() {
	m_sort = "date";
	m_rootCollapsed = false;
	m_groups.clear();
	m_records.clear();
	m_nextGroupId = 1;

	std::ifstream f(std::filesystem::path(FilePath()), std::ios::binary);
	if (!f.is_open())
		return;

	std::string line;
	while (std::getline(f, line)) {
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
			line.pop_back();
		if (line.empty())
			continue;

		std::string tag, rest;
		if (!SplitFirst(line, tag, rest))
			continue;

		if (tag == "sort") {
			SetSort(rest);
		} else if (tag == "rootcollapsed") {
			m_rootCollapsed = (std::atoi(rest.c_str()) != 0);
		} else if (tag == "g") {
			// g <id> <collapsed> <name>
			std::string idText, tail;
			if (!SplitFirst(rest, idText, tail))
				continue;
			std::string collapsedText, name;
			if (!SplitFirst(tail, collapsedText, name))
				continue;
			DemoOrganizerGroup g;
			g.id = std::atoi(idText.c_str());
			g.collapsed = (std::atoi(collapsedText.c_str()) != 0);
			g.name = SanitizeName(name);
			if (g.id <= 0 || g.name.empty())
				continue;
			m_groups.push_back(g);
			if (g.id >= m_nextGroupId)
				m_nextGroupId = g.id + 1;
		} else if (tag == "p") {
			// p <lastPlayed> <groupId> <path>
			std::string playedText, tail;
			if (!SplitFirst(rest, playedText, tail))
				continue;
			std::string groupText, path;
			if (!SplitFirst(tail, groupText, path))
				continue;
			if (path.empty())
				continue;
			DemoOrganizerRecord r;
			r.key = NormalizeDemoKey(Utf8ToWide(path));
			r.lastPlayed = (int64_t)_atoi64(playedText.c_str());
			r.groupId = std::atoi(groupText.c_str());
			if (r.key.empty())
				continue;
			m_records.push_back(r);
		}
	}

	// A folder that vanished (hand-edited file) must not strand its members.
	for (auto& r : m_records) {
		if (r.groupId < 0)
			continue;
		bool found = false;
		for (const auto& g : m_groups)
			if (g.id == r.groupId) { found = true; break; }
		if (!found)
			r.groupId = -1;
	}
	PruneEmpty();
}

bool DemoOrganizerStore::Save() const {
	std::ofstream f(std::filesystem::path(FilePath()), std::ios::binary | std::ios::trunc);
	if (!f.is_open())
		return false;
	f << "v 1\n";
	f << "sort " << m_sort << "\n";
	f << "rootcollapsed " << (m_rootCollapsed ? 1 : 0) << "\n";
	for (const auto& g : m_groups)
		f << "g " << g.id << " " << (g.collapsed ? 1 : 0) << " " << g.name << "\n";
	for (const auto& r : m_records) {
		if (r.lastPlayed <= 0 && r.groupId < 0)
			continue;
		f << "p " << r.lastPlayed << " " << r.groupId << " " << WideToUtf8(r.key) << "\n";
	}
	return true;
}

bool DemoOrganizerStore::SetSort(const std::string& mode) {
	// Whitelist: an unknown mode from a stale file or a typo'd command must not leave the UI
	// with a sort it cannot render.
	static const char* kModes[] = { "played", "date", "dateasc", "map", "name" };
	std::string m = SanitizeName(mode);
	for (const char* k : kModes) {
		if (m == k) {
			if (m_sort == m)
				return false;
			m_sort = m;
			return true;
		}
	}
	return false;
}

bool DemoOrganizerStore::SetRootCollapsed(bool collapsed) {
	if (m_rootCollapsed == collapsed)
		return false;
	m_rootCollapsed = collapsed;
	return true;
}

int DemoOrganizerStore::CreateGroup(const std::string& name) {
	const std::string clean = SanitizeName(name);
	if (clean.empty())
		return -1;
	DemoOrganizerGroup g;
	g.id = m_nextGroupId++;
	g.name = clean;
	g.collapsed = false;
	m_groups.push_back(g);
	return g.id;
}

bool DemoOrganizerStore::RenameGroup(int id, const std::string& name) {
	const std::string clean = SanitizeName(name);
	if (clean.empty())
		return false;
	for (auto& g : m_groups) {
		if (g.id != id)
			continue;
		if (g.name == clean)
			return false;
		g.name = clean;
		return true;
	}
	return false;
}

bool DemoOrganizerStore::DeleteGroup(int id) {
	auto it = std::find_if(m_groups.begin(), m_groups.end(),
		[&](const DemoOrganizerGroup& g) { return g.id == id; });
	if (it == m_groups.end())
		return false;
	m_groups.erase(it);
	for (auto& r : m_records)
		if (r.groupId == id)
			r.groupId = -1;
	PruneEmpty();
	return true;
}

bool DemoOrganizerStore::SetGroupCollapsed(int id, bool collapsed) {
	for (auto& g : m_groups) {
		if (g.id != id)
			continue;
		if (g.collapsed == collapsed)
			return false;
		g.collapsed = collapsed;
		return true;
	}
	return false;
}

DemoOrganizerRecord* DemoOrganizerStore::Find(const std::wstring& key) {
	for (auto& r : m_records)
		if (r.key == key)
			return &r;
	return nullptr;
}

const DemoOrganizerRecord* DemoOrganizerStore::Find(const std::wstring& key) const {
	for (const auto& r : m_records)
		if (r.key == key)
			return &r;
	return nullptr;
}

void DemoOrganizerStore::PruneEmpty() {
	m_records.erase(
		std::remove_if(m_records.begin(), m_records.end(),
			[](const DemoOrganizerRecord& r) { return r.lastPlayed <= 0 && r.groupId < 0; }),
		m_records.end());
}

bool DemoOrganizerStore::Assign(const std::wstring& path, int groupId) {
	const std::wstring key = NormalizeDemoKey(path);
	if (key.empty())
		return false;
	if (groupId >= 0) {
		bool found = false;
		for (const auto& g : m_groups)
			if (g.id == groupId) { found = true; break; }
		if (!found)
			return false;
	} else {
		groupId = -1;
	}
	if (DemoOrganizerRecord* r = Find(key)) {
		if (r->groupId == groupId)
			return false;
		r->groupId = groupId;
		PruneEmpty();
		return true;
	}
	if (groupId < 0)
		return false; // already ungrouped; no record needed
	DemoOrganizerRecord r;
	r.key = key;
	r.groupId = groupId;
	m_records.push_back(r);
	return true;
}

bool DemoOrganizerStore::Rekey(const std::wstring& oldPath, const std::wstring& newPath) {
	const std::wstring from = NormalizeDemoKey(oldPath);
	const std::wstring to = NormalizeDemoKey(newPath);
	if (from.empty() || to.empty() || from == to)
		return false;
	DemoOrganizerRecord* r = Find(from);
	if (!r)
		return false;          // nothing filed or played: the new path needs no record
	if (Find(to))
		return false;          // something is already recorded there; do not overwrite it
	r->key = to;
	return true;
}

bool DemoOrganizerStore::SetLastPlayed(const std::wstring& path, int64_t whenUnix) {
	const std::wstring key = NormalizeDemoKey(path);
	if (key.empty() || whenUnix <= 0)
		return false;
	if (DemoOrganizerRecord* r = Find(key)) {
		if (r->lastPlayed == whenUnix)
			return false;
		r->lastPlayed = whenUnix;
		return true;
	}
	DemoOrganizerRecord r;
	r.key = key;
	r.lastPlayed = whenUnix;
	m_records.push_back(r);
	return true;
}

int64_t DemoOrganizerStore::LastPlayed(const std::wstring& path) const {
	const DemoOrganizerRecord* r = Find(NormalizeDemoKey(path));
	return r ? r->lastPlayed : 0;
}

} // namespace Filmmaker
