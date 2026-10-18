#include "DemoProjectPath.h"

#include "DemoProjectFileMath.h"
#include "DemoProjectStoreMath.h"

#include <Windows.h>

#include <cstdio>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace Filmmaker {

namespace {

std::atomic<uint64_t> g_pathRevision{0};

std::string FileStamp(const std::wstring& path) {
	WIN32_FILE_ATTRIBUTE_DATA data{};
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return {};
	const ULONGLONG size = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32)
		| data.nFileSizeLow;
	const ULONGLONG mtime = (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32)
		| data.ftLastWriteTime.dwLowDateTime;
	return std::to_string(size) + ":" + std::to_string(mtime);
}

bool MarkerMatchesSource(const std::wstring& originalPath, const std::wstring& repairedPath) {
	const std::string sourceStamp = FileStamp(originalPath);
	if (sourceStamp.empty()) return false;

	FILE* marker = _wfopen(RepairValidityMarkerPath(repairedPath).c_str(), L"rb");
	if (!marker) return false;
	char value[256]{};
	const size_t count = fread(value, 1, sizeof(value) - 1, marker);
	fclose(marker);
	if (!count) return false;

	const std::string stored(value, count);
	const size_t separator = stored.find('|');
	return separator != std::string::npos && stored.substr(0, separator) == sourceStamp;
}

std::wstring Utf8ToWide(const std::string& utf8) {
	if (utf8.empty()) return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
	if (n <= 0) return {};
	std::wstring out((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), n);
	return out;
}

std::wstring ReadJsonStringField(const std::string& text, const char* key) {
	const std::string needle = std::string("\"") + key + "\"";
	size_t pos = text.find(needle);
	if (pos == std::string::npos) return {};
	pos += needle.size();
	while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r'
			|| text[pos] == '\n'))
		++pos;
	if (pos >= text.size() || text[pos] != ':') return {};
	++pos;
	while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r'
			|| text[pos] == '\n'))
		++pos;
	if (pos >= text.size() || text[pos] != '"') return {};
	++pos;
	std::string value;
	while (pos < text.size()) {
		const char c = text[pos++];
		if (c == '"') break;
		if (c == '\\' && pos < text.size()) {
			const char escaped = text[pos++];
			switch (escaped) {
			case '"': case '\\': case '/': value.push_back(escaped); break;
			case 'n': value.push_back('\n'); break;
			case 'r': value.push_back('\r'); break;
			case 't': value.push_back('\t'); break;
			default: value.push_back(escaped); break;
			}
			continue;
		}
		value.push_back(c);
	}
	return Utf8ToWide(value);
}

std::wstring ReadProjectDemoPath(const std::filesystem::path& projectFile) {
	std::ifstream in(projectFile, std::ios::binary);
	if (!in) return {};
	const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	ProjectFile file;
	std::string error;
	if (!DecodeProjectFile(bytes.data(), bytes.size(), ProjectGame::Cs2, file, error)
		&& !DecodeProjectFile(bytes.data(), bytes.size(), ProjectGame::Deadlock, file, error))
		return {};
	const ProjectFileSection* info = FindProjectSection(file, kProjectFileInfoTag);
	if (!info) return {};
	return ReadJsonStringField(info->payload, "demoPath");
}

} // namespace

uint64_t DemoProjectPathRevision() { return g_pathRevision.load(std::memory_order_acquire); }
void InvalidateDemoProjectPathCache() { g_pathRevision.fetch_add(1, std::memory_order_release); }

std::wstring ResolveDemoProjectPath(const std::wstring& playingDemoPath) {
	if (playingDemoPath.empty()) return {};

	const DemoCopyLookup lookup = PlanDemoCopyLookup(playingDemoPath);
	std::wstring original;
	switch (lookup.source) {
	case DemoCopySource::None: return playingDemoPath;
	case DemoCopySource::ProjectFolder:
		original = ReadProjectDemoPath(std::filesystem::path(lookup.projectDir) / kProjectFileName);
		break;
	case DemoCopySource::LegacyRepaired: original = lookup.original; break;
	}
	if (!original.empty() && MarkerMatchesSource(original, playingDemoPath))
		return original;
	return playingDemoPath;
}

} // namespace Filmmaker
