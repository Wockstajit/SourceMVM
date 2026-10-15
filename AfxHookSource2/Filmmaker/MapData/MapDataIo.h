#pragma once

// Engine-free readers for Source 2 game data: VPK packages, compiled resource containers and
// binary KeyValues3. Format handling is ported from ValveResourceFormat (MIT,
// https://github.com/ValveResourceFormat/ValveResourceFormat), version 20.0. Its license, which
// covers every MapData file marked as ported from it, is LICENSE-ValveResourceFormat.txt here.
// Every reader throws MapDataError on malformed input; callers treat that as "map data
// unavailable", never as a crash.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Filmmaker::MapData {

struct MapDataError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ------------------------------------------------------------------------------------------
// VPK packages

class Vpk {
public:
    // Reads the directory tree of a VPK v1/v2 file (a `_dir.vpk` or a single-file package).
    explicit Vpk(const std::filesystem::path &path);
    // A package that lives inside another package's bytes (workshop addons nest the map as
    // a `maps/<name>.vpk` entry). Multi-archive entries cannot appear here.
    Vpk(std::vector<uint8_t> bytes, std::string name);
    const std::filesystem::path &Path() const { return m_path; }
    // Lower-case, forward-slash resource path -> present.
    bool Contains(const std::string &path) const;
    // Reads one entry's bytes. False when the entry does not exist.
    bool Read(const std::string &path, std::vector<uint8_t> &out) const;
    // Every entry whose extension matches (`"vwnod_c"`), sorted ordinally.
    std::vector<std::string> List(const std::string &extension) const;
    // Every entry path, sorted ordinally.
    std::vector<std::string> ListAll() const;
    // Invoked for every physical file the package opens (directory file and archives), for
    // the manifest's source list. Called before parsing the directory as well.
    void SetFileReadCallback(std::function<void(const std::filesystem::path &)> cb) { m_onFileRead = std::move(cb); }

private:
    struct Entry {
        uint32_t crc = 0;
        uint16_t archive = 0;
        uint32_t offset = 0, length = 0;
        uint64_t preloadOffset = 0;
        uint16_t preloadBytes = 0;
    };
    std::filesystem::path m_path;
    std::vector<uint8_t> m_bytes; // in-memory package bytes (empty for on-disk packages)
    uint64_t m_dataStart = 0;
    std::unordered_map<std::string, Entry> m_entries;
    std::function<void(const std::filesystem::path &)> m_onFileRead;
    std::filesystem::path ArchivePath(uint16_t index) const;
    void Parse(const uint8_t *tree, size_t treeSize, size_t headerSize);
};

// Resource lookup across packages: the map package first, then the game's shared packages in
// gameinfo.gi search-path order.
class PackageSet {
public:
    void Add(std::shared_ptr<Vpk> vpk) { m_packages.push_back(std::move(vpk)); }
    // Loose files on disk under these folders are searched last, in order.
    void AddDiskPath(std::filesystem::path folder) { m_diskPaths.push_back(std::move(folder)); }
    // Accepts compiled (`x.vmdl_c`) or source (`x.vmdl`) names; looks up the compiled one.
    bool Read(const std::string &resourcePath, std::vector<uint8_t> &out) const;
    bool Contains(const std::string &resourcePath) const;
    const std::vector<std::shared_ptr<Vpk>> &Packages() const { return m_packages; }
    void SetFileReadCallback(std::function<void(const std::filesystem::path &)> cb) { m_onFileRead = std::move(cb); }

private:
    std::vector<std::shared_ptr<Vpk>> m_packages;
    std::vector<std::filesystem::path> m_diskPaths;
    std::function<void(const std::filesystem::path &)> m_onFileRead;
};

// Normalises a resource reference to the lower-case, forward-slash compiled path.
std::string CompiledResourcePath(std::string path);

// The `Game` search paths of game/csgo/gameinfo.gi, in order, as folder names (`csgo`, ...).
std::vector<std::string> GameInfoSearchPaths(const std::filesystem::path &gameInfo);

// ------------------------------------------------------------------------------------------
// KeyValues3

struct Kv3 {
    enum class Type : uint8_t { Null, Bool, Int, UInt, Double, String, Blob, Array, Object };
    // The width the value was stored with, kept for entity property type names (Int16 etc.).
    enum class Width : uint8_t { Default, Int16, Int32, UInt16, UInt32, Float };
    Type type = Type::Null;
    Width width = Width::Default;
    uint8_t flag = 0;
    bool b = false;
    int64_t i = 0;
    uint64_t u = 0;
    double d = 0;
    std::string s;
    std::vector<uint8_t> blob;
    std::vector<Kv3> arr;
    std::vector<std::pair<std::string, Kv3>> obj;

    const Kv3 *Find(const char *key) const;
    const Kv3 &operator[](const char *key) const; // Null value when absent
    bool IsNull() const { return type == Type::Null; }
    double Number(double fallback = 0) const;
    int64_t Integer(int64_t fallback = 0) const;
    const std::string &Str() const;
    size_t Size() const { return type == Type::Array ? arr.size() : type == Type::Object ? obj.size() : 0; }
    const Kv3 &At(size_t index) const;
};

// Decodes a binary KV3 block (legacy VKV3 binary/LZ4, and KV3 versions 1-5 with LZ4/ZSTD).
Kv3 DecodeKv3(const uint8_t *data, size_t size);

// ------------------------------------------------------------------------------------------
// Compiled resources (`*_c`)

struct ResourceBlock {
    uint32_t type = 0; // FourCC, e.g. 'DATA'
    uint32_t offset = 0, size = 0;
};

class Resource {
public:
    explicit Resource(std::vector<uint8_t> bytes);
    const std::vector<uint8_t> &Bytes() const { return m_bytes; }
    const std::vector<ResourceBlock> &Blocks() const { return m_blocks; }
    const ResourceBlock *Block(const char fourcc[4]) const;
    // Every block of that type in file order (meshes may carry several MBUF/VBIB blocks).
    std::vector<const ResourceBlock *> BlocksOfType(const char fourcc[4]) const;
    // The block decoded as KV3. Throws when it is not KV3.
    Kv3 Kv3Block(const char fourcc[4]) const;
    Kv3 Kv3Block(const ResourceBlock &block) const;
    const uint8_t *Data(const ResourceBlock &block) const { return m_bytes.data() + block.offset; }
    // Byte position where the declared resource ends (texture pixel data follows it).
    uint32_t DeclaredSize() const { return m_declaredSize; }

private:
    std::vector<uint8_t> m_bytes;
    std::vector<ResourceBlock> m_blocks;
    uint32_t m_declaredSize = 0;
};

bool IsKv3Magic(const uint8_t *data, size_t size);

// ------------------------------------------------------------------------------------------
// Hashing

// MurmurHash2 as Source 2 uses it for string tokens (lower-cased input, seed 0x31415926).
uint32_t StringToken(const std::string &text);
// Lower-case hex SHA-256 of a file or buffer (BCrypt).
std::string Sha256File(const std::filesystem::path &path);
std::string Sha256Bytes(const void *data, size_t size);

} // namespace Filmmaker::MapData
