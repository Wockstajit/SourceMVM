#pragma once

// Windows plumbing shared by the osu easter egg's library, audio and game: clocks, file IO, the
// bundled ffmpeg, hidden child processes, MD5, and our folders.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Filmmaker {
namespace OsuPlatform {

double QpcMs();
std::string ReadFile(const std::filesystem::path& p);
bool RunHidden(const std::wstring& cmdline, unsigned long timeoutMs);
// Runs a hidden process and returns everything it writes to stdout (false on failure/timeout).
bool RunCapture(const std::wstring& cmdline, unsigned long timeoutMs, std::string& out);
std::filesystem::path Ffmpeg();
std::string Md5Hex(const std::string& bytes);
uint64_t Fnv64(const std::wstring& s);
std::string Hex64(uint64_t v);

std::filesystem::path RoamingRoot();   // %APPDATA%\HLAE\osu  (drop .osz here; cache lives here)
std::filesystem::path BundledRoot();   // <install bin>\osu   (maps shipped with the mod)
std::filesystem::path CacheDir();      // RoamingRoot()\cache

} // namespace OsuPlatform
} // namespace Filmmaker
