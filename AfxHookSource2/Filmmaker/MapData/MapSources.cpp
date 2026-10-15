#include "MapSources.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <regex>

namespace Filmmaker::MapData {
namespace {

namespace fs = std::filesystem;

fs::path SteamRoot() {
    wchar_t value[MAX_PATH * 2] = {};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS)
        return fs::path(value);
    size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS)
        return fs::path(value);
    return {};
}

// Package files that belong to one VPK: the directory/single file plus its numbered archives.
std::vector<fs::path> PackageFiles(const fs::path &vpk) {
    std::vector<fs::path> files{vpk};
    std::wstring stem = vpk.stem().wstring();
    if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, L"_dir") == 0) {
        stem.resize(stem.size() - 4);
        std::error_code ec;
        for (int k = 0; k < 1000; ++k) {
            wchar_t suffix[16];
            swprintf_s(suffix, L"_%03d.vpk", k);
            const fs::path archive = vpk.parent_path() / (stem + suffix);
            if (!fs::exists(archive, ec)) break;
            files.push_back(archive);
        }
    }
    return files;
}

void AddShared(MapSources &s) {
    const fs::path game = s.cs2Root / "game";
    for (const std::string &folder : GameInfoSearchPaths(game / "csgo" / "gameinfo.gi")) {
        const fs::path dir = game / fs::u8path(folder) / "pak01_dir.vpk";
        std::error_code ec;
        if (!fs::is_regular_file(dir, ec)) continue;
        s.resources.Add(std::make_shared<Vpk>(dir));
        s.files.push_back(dir);
    }
}

} // namespace

bool ValidMapName(const std::string &map) {
    return !map.empty() && map.size() < 100 &&
           map.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos;
}

fs::path ResolveCs2Root() {
    const fs::path steam = SteamRoot();
    if (steam.empty()) return {};
    std::vector<fs::path> libraries{steam};
    std::ifstream in(steam / "steamapps" / "libraryfolders.vdf", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    static const std::regex pathLine("\"path\"\\s+\"([^\"]+)\"", std::regex::icase);
    for (std::sregex_iterator it(text.begin(), text.end(), pathLine), end; it != end; ++it) {
        std::string value = (*it)[1];
        for (size_t at = value.find("\\\\"); at != std::string::npos; at = value.find("\\\\", at + 1)) value.erase(at, 1);
        libraries.push_back(fs::u8path(value));
    }
    for (const fs::path &library : libraries) {
        const fs::path root = library / "steamapps" / "common" / "Counter-Strike Global Offensive";
        std::error_code ec;
        if (fs::is_regular_file(root / "game" / "csgo" / "gameinfo.gi", ec)) return root;
    }
    return {};
}

MapSources OpenMapSources(const fs::path &cs2Root, const std::string &map) {
    if (!ValidMapName(map)) throw MapDataError("invalid map name");
    MapSources s;
    s.cs2Root = cs2Root;
    s.map = map;
    const std::string entry = "maps/" + map + ".vpk";
    std::error_code ec;
    const fs::path official = cs2Root / "game" / "csgo" / fs::u8path(entry);
    if (fs::is_regular_file(official, ec)) {
        s.package = std::make_shared<Vpk>(official);
        s.files.push_back(official);
        s.mapDigest = Sha256File(official);
    } else {
        // Workshop addons live in the same Steam library as the game.
        const fs::path workshop = cs2Root.parent_path().parent_path() / "workshop" / "content" / "730";
        fs::path best;
        fs::file_time_type bestTime{};
        std::shared_ptr<Vpk> bestAddon;
        if (fs::is_directory(workshop, ec)) {
            for (const auto &item : fs::directory_iterator(workshop, ec)) {
                if (!item.is_directory()) continue;
                for (const auto &file : fs::directory_iterator(item.path(), ec)) {
                    const std::wstring name = file.path().filename().wstring();
                    if (file.path().extension() != L".vpk") continue;
                    const std::wstring stem = file.path().stem().wstring();
                    const bool archive = stem.size() > 4 && stem[stem.size() - 4] == L'_' && stem.substr(stem.size() - 3) != L"dir" &&
                                         iswdigit(stem.back());
                    if (archive) continue;
                    try {
                        auto addon = std::make_shared<Vpk>(file.path());
                        if (!addon->Contains(entry)) continue;
                        const auto time = fs::last_write_time(file.path(), ec);
                        if (!bestAddon || time > bestTime) {
                            best = file.path();
                            bestTime = time;
                            bestAddon = std::move(addon);
                        }
                    } catch (const MapDataError &) {
                    }
                }
            }
        }
        if (!bestAddon) throw MapDataError("map package not found: " + map);
        std::vector<uint8_t> bytes;
        bestAddon->Read(entry, bytes);
        s.mapDigest = Sha256Bytes(bytes.data(), bytes.size());
        s.package = std::make_shared<Vpk>(std::move(bytes), entry);
        s.addon = bestAddon;
        for (const fs::path &file : PackageFiles(best)) s.files.push_back(file);
    }
    s.resources.Add(s.package);
    if (s.addon) s.resources.Add(s.addon);
    // Shipped community maps keep their custom materials/models in a separate mount.
    // The map VPK alone contains world geometry but cannot resolve these resources.
    const fs::path community = cs2Root / "game" / "csgo_community_addons" / fs::u8path(map) / fs::u8path(map + "_dir.vpk");
    if (fs::is_regular_file(community, ec)) {
        s.resources.Add(std::make_shared<Vpk>(community));
        for (const fs::path &file : PackageFiles(community)) s.files.push_back(file);
    }
    AddShared(s);
    return s;
}

std::shared_ptr<Vpk> OpenReferencedPackage(MapSources &s, const std::string &package) {
    // Nested in mounted resource packages (map, addon, community assets), then a loose game file.
    for (const auto &container : s.resources.Packages()) {
        if (!container || !container->Contains(package)) continue;
        std::vector<uint8_t> bytes;
        container->Read(package, bytes);
        return std::make_shared<Vpk>(std::move(bytes), package);
    }
    const fs::path file = s.cs2Root / "game" / "csgo" / fs::u8path(package);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return nullptr;
    if (std::find(s.files.begin(), s.files.end(), file) == s.files.end()) s.files.push_back(file);
    return std::make_shared<Vpk>(file);
}

} // namespace Filmmaker::MapData
