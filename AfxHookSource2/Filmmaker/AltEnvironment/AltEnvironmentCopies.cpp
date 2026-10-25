#include "AltEnvironmentCopies.h"
#include "AltEnvironment.h"

#include "../Debug/MvmDebugLog.h"
#include "../Demo/DemoKeyframes.h"
#include "../Demo/DemoProjectStore.h"
#include "../Demo/DemoRepair.h"
#include "../Demo/PlayingDemoPath.h"
#include "../Platform/TextEncoding.h"

#include <Windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>

namespace Filmmaker::AltEnvironment {

namespace {

namespace fs = std::filesystem;

// The first line is "<original's size:mtime>|altenv-copy 1": DemoProjectPath.cpp maps a playing copy
// back to its demo only when its marker starts with the original's stamp and a '|', the same
// convention the repaired and keyframed copies' markers follow.
constexpr const char* kMarkerVersion = "altenv-copy 1";
const ProjectGenerated kSlots[2] = { ProjectGenerated::Environment, ProjectGenerated::EnvironmentB };

std::string FileStamp(const std::wstring& p) {
	WIN32_FILE_ATTRIBUTE_DATA fad{};
	if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return std::string();
	const ULONGLONG size = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
	const ULONGLONG mtime = ((ULONGLONG)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
	return std::to_string(size) + ":" + std::to_string(mtime);
}

bool FileExists(const std::wstring& p) {
	const DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring MarkerPath(const std::wstring& copy) { return copy + L".src"; }

std::wstring FileName(const std::wstring& path) {
	const size_t slash = path.find_last_of(L"\\/");
	return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

struct Marker {
	std::wstring source, original;
	std::string stamp, spec;
	bool keyframed = false;
};

bool ReadMarker(const std::wstring& copy, Marker& m) {
	std::ifstream in(fs::path(MarkerPath(copy)), std::ios::binary);
	if (!in) return false;
	const std::string text((std::istreambuf_iterator<char>(in)), {});
	const size_t eol = text.find('\n');
	const std::string first = text.substr(0, eol);
	const size_t bar = first.find('|');
	if (bar == std::string::npos || first.substr(bar + 1) != kMarkerVersion) return false;
	size_t at = eol == std::string::npos ? text.size() : eol + 1;
	while (at < text.size()) {
		size_t end = text.find('\n', at);
		if (end == std::string::npos) end = text.size();
		const std::string line = text.substr(at, end - at);
		at = end + 1;
		const size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
		if (k == "source") m.source = Utf8ToWide(v);
		else if (k == "original") m.original = Utf8ToWide(v);
		else if (k == "stamp") m.stamp = v;
		else if (k == "spec") m.spec = v;
		else if (k == "keyframed") m.keyframed = v == "1";
	}
	return !m.source.empty();
}

void DeleteCopy(const std::wstring& copy) {
	DeleteFileW(copy.c_str());
	DeleteFileW(MarkerPath(copy).c_str());
	DeleteFileW((copy + L".part").c_str());
}

} // namespace

namespace Copies {

bool SameFile(std::wstring a, std::wstring b) {
	if (a.empty() || b.empty()) return false;
	for (auto& c : a) if (c == L'/') c = L'\\';
	for (auto& c : b) if (c == L'/') c = L'\\';
	return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring SlotPath(const std::wstring& original, int slot, bool forSave) {
	return forSave ? DemoProjectStore_GeneratedForSave(ProjectGame::Cs2, original, kSlots[slot])
		: DemoProjectStore_GeneratedForLoad(ProjectGame::Cs2, original, kSlots[slot]);
}

int FreeSlot(const std::wstring& original) {
	return SameFile(SlotPath(original, 0, false), ResolvePlayingDemoPath()) ? 1 : 0;
}

int CurrentSlot(const std::wstring& original, const std::wstring& source, const std::string& specKey) {
	const std::string stamp = FileStamp(source);
	for (int slot = 0; slot < 2; ++slot) {
		const std::wstring copy = SlotPath(original, slot, false);
		Marker m;
		if (copy.empty() || !FileExists(copy) || !ReadMarker(copy, m)) continue;
		if (SameFile(m.source, source) && m.stamp == stamp && m.spec == specKey) return slot;
	}
	return -1;
}

void WriteMarker(const std::wstring& copy, const std::wstring& source, const std::wstring& original, const std::string& specKey) {
	std::ofstream out(fs::path(MarkerPath(copy)), std::ios::binary | std::ios::trunc);
	out << FileStamp(original) << "|" << kMarkerVersion << "\nsource=" << WideToUtf8(source) << "\noriginal=" << WideToUtf8(original) << "\nstamp=" << FileStamp(source)
		<< "\nspec=" << specKey << "\nkeyframed=" << (IsKeyframedCopyFileName(FileName(source)) ? 1 : 0) << "\n";
}

std::wstring BaseFor(const std::wstring& original) {
	const std::wstring keyframed = DemoKeyframes_ReadyPath(original);
	return keyframed.empty() ? DemoRepair_ResolvePlayable(original) : keyframed;
}

bool PlayingCopy() {
	const std::wstring playing = ResolvePlayingDemoPath();
	return !playing.empty() && IsEnvironmentCopyFileName(FileName(playing));
}

bool IsPlaying(const std::wstring& file) { return SameFile(file, ResolvePlayingDemoPath()); }

void KeepOnly(const std::wstring& original, const std::wstring& playingFile, int keepSlot) {
	for (const std::wstring& dir : DemoProjectStore_OtherProjectDirectories(ProjectGame::Cs2, original))
		for (int slot = 0; slot < 2; ++slot) {
			const std::wstring copy = dir + L"\\" + ProjectGeneratedFileName(kSlots[slot]);
			if (SameFile(copy, playingFile)) continue;
			if (FileExists(copy) || FileExists(MarkerPath(copy)) || FileExists(copy + L".part")) {
				DeleteCopy(copy);
				MvmDebugLog_LinefAlways("altenv", "removed another demo's copy %s", WideToUtf8(copy).c_str());
			}
		}
	for (int slot = 0; slot < 2; ++slot) {
		const std::wstring copy = SlotPath(original, slot, false);
		if (slot == keepSlot || copy.empty() || SameFile(copy, playingFile) || !FileExists(copy)) continue;
		DeleteCopy(copy);
	}
}

} // namespace Copies

std::wstring SourceOf(const std::wstring& file) {
	if (!IsEnvironmentCopyFileName(FileName(file))) return std::wstring();
	Marker m;
	return ReadMarker(file, m) && FileExists(m.source) ? m.source : std::wstring();
}

bool IsKeyframedCopy(const std::wstring& file) {
	if (!IsEnvironmentCopyFileName(FileName(file))) return false;
	Marker m;
	return ReadMarker(file, m) && m.keyframed;
}

} // namespace Filmmaker::AltEnvironment
