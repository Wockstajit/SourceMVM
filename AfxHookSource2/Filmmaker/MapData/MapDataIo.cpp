// Ported from ValveResourceFormat 20.0 (MIT): ValvePak, Resource.cs, BinaryKV3*.cs.
#include "MapDataIo.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>

#include "../../../Deps/release/lz4/lz4.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "../../../Deps/release/zstd/lib/zstd.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#pragma comment(lib, "bcrypt.lib")

namespace Filmmaker::MapData {
namespace {

[[noreturn]] void Fail(const std::string &what) { throw MapDataError(what); }

struct Cursor {
    const uint8_t *p = nullptr;
    size_t n = 0, at = 0;
    void Need(size_t k) const {
        if (k > n || at > n - k) Fail("truncated data");
    }
    template <class T> T Get() {
        Need(sizeof(T));
        T v;
        std::memcpy(&v, p + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    std::string CStr() {
        const size_t start = at;
        while (true) {
            Need(1);
            if (!p[at++]) break;
        }
        return std::string(reinterpret_cast<const char *>(p + start), at - start - 1);
    }
    const uint8_t *Take(size_t k) {
        Need(k);
        const uint8_t *r = p + at;
        at += k;
        return r;
    }
};

std::string Lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = char(c | 0x20);
    return s;
}

bool ReadFileRange(const std::filesystem::path &path, uint64_t offset, uint64_t size, uint8_t *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(std::streamoff(offset));
    in.read(reinterpret_cast<char *>(out), std::streamsize(size));
    return bool(in) && uint64_t(in.gcount()) == size;
}

const Kv3 &NullValue() {
    static const Kv3 value;
    return value;
}

} // namespace

// ==========================================================================================
// VPK

Vpk::Vpk(const std::filesystem::path &path) : m_path(path) {
    if (m_onFileRead) m_onFileRead(path);
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) Fail("cannot open package " + path.u8string());
    const uint64_t fileSize = uint64_t(in.tellg());
    in.seekg(0);
    uint32_t header[7] = {};
    in.read(reinterpret_cast<char *>(header), 12);
    if (!in || header[0] != 0x55AA1234u) Fail("not a VPK: " + path.u8string());
    const uint32_t version = header[1], treeSize = header[2];
    size_t headerSize = 12;
    if (version == 2) {
        in.read(reinterpret_cast<char *>(header + 3), 16);
        if (!in) Fail("truncated VPK header");
        headerSize = 28;
    } else if (version != 1) {
        Fail("unsupported VPK version");
    }
    if (treeSize > fileSize - headerSize) Fail("VPK tree larger than file");
    std::vector<uint8_t> tree(treeSize);
    in.read(reinterpret_cast<char *>(tree.data()), treeSize);
    if (!in) Fail("truncated VPK tree");
    m_dataStart = headerSize + treeSize;
    Parse(tree.data(), tree.size(), headerSize);
}

Vpk::Vpk(std::vector<uint8_t> bytes, std::string name) : m_path(std::move(name)), m_bytes(std::move(bytes)) {
    if (m_bytes.size() < 12) Fail("not a VPK: " + m_path.u8string());
    uint32_t header[7] = {};
    std::memcpy(header, m_bytes.data(), 12);
    if (header[0] != 0x55AA1234u) Fail("not a VPK: " + m_path.u8string());
    const uint32_t version = header[1], treeSize = header[2];
    size_t headerSize = 12;
    if (version == 2) {
        if (m_bytes.size() < 28) Fail("truncated VPK header");
        std::memcpy(header + 3, m_bytes.data() + 12, 16);
        headerSize = 28;
    } else if (version != 1) {
        Fail("unsupported VPK version");
    }
    if (treeSize > m_bytes.size() - headerSize) Fail("VPK tree larger than package");
    m_dataStart = headerSize + treeSize;
    Parse(m_bytes.data() + headerSize, treeSize, headerSize);
}

void Vpk::Parse(const uint8_t *data, size_t treeSize, size_t headerSize) {
    Cursor c{data, treeSize};
    while (true) {
        const std::string ext = c.CStr();
        if (ext.empty()) break;
        while (true) {
            const std::string dir = c.CStr();
            if (dir.empty()) break;
            while (true) {
                const std::string name = c.CStr();
                if (name.empty()) break;
                Entry e;
                e.crc = c.Get<uint32_t>();
                e.preloadBytes = c.Get<uint16_t>();
                e.archive = c.Get<uint16_t>();
                e.offset = c.Get<uint32_t>();
                e.length = c.Get<uint32_t>();
                if (c.Get<uint16_t>() != 0xFFFF) Fail("bad VPK entry terminator");
                e.preloadOffset = headerSize + c.at;
                c.Take(e.preloadBytes);
                std::string full;
                if (dir != " ") full = dir + "/";
                full += name;
                if (ext != " ") full += "." + ext;
                m_entries.emplace(Lower(full), e);
            }
        }
    }
}

std::filesystem::path Vpk::ArchivePath(uint16_t index) const {
    std::wstring stem = m_path.stem().wstring();
    if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, L"_dir") == 0) stem.resize(stem.size() - 4);
    wchar_t suffix[16];
    swprintf_s(suffix, L"_%03u.vpk", unsigned(index));
    return m_path.parent_path() / (stem + suffix);
}

bool Vpk::Contains(const std::string &path) const { return m_entries.count(Lower(path)) != 0; }

bool Vpk::Read(const std::string &path, std::vector<uint8_t> &out) const {
    const auto it = m_entries.find(Lower(path));
    if (it == m_entries.end()) return false;
    const Entry &e = it->second;
    const auto readRange = [&](const std::filesystem::path &file, uint64_t offset, uint64_t size, uint8_t *dst) {
        if (!m_bytes.empty()) {
            if (offset > m_bytes.size() || size > m_bytes.size() - offset) Fail("VPK entry out of range: " + path);
            std::memcpy(dst, m_bytes.data() + offset, size_t(size));
            return;
        }
        if (m_onFileRead) m_onFileRead(file);
        if (!ReadFileRange(file, offset, size, dst)) Fail("cannot read VPK entry " + path);
    };
    out.resize(size_t(e.preloadBytes) + e.length);
    // Preload bytes always live in the directory file itself, right after the entry record.
    if (e.preloadBytes) readRange(m_path, e.preloadOffset, e.preloadBytes, out.data());
    if (e.length) {
        if (e.archive != 0x7FFF && !m_bytes.empty()) Fail("nested VPK references an archive file");
        const auto file = e.archive == 0x7FFF ? m_path : ArchivePath(e.archive);
        readRange(file, e.archive == 0x7FFF ? m_dataStart + e.offset : e.offset, e.length, out.data() + e.preloadBytes);
    }
    return true;
}

std::vector<std::string> Vpk::List(const std::string &extension) const {
    std::vector<std::string> out;
    const std::string suffix = "." + Lower(extension);
    for (const auto &[name, entry] : m_entries)
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> Vpk::ListAll() const {
    std::vector<std::string> out;
    for (const auto &[name, entry] : m_entries) out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

std::string CompiledResourcePath(std::string path) {
    path = Lower(path);
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    if (path.size() < 2 || path.compare(path.size() - 2, 2, "_c") != 0) path += "_c";
    return path;
}

bool PackageSet::Read(const std::string &resourcePath, std::vector<uint8_t> &out) const {
    const std::string compiled = CompiledResourcePath(resourcePath);
    for (const auto &vpk : m_packages)
        if (vpk->Read(compiled, out)) return true;
    for (const auto &folder : m_diskPaths) {
        const auto file = folder / std::filesystem::u8path(compiled);
        if (!std::filesystem::is_regular_file(file)) continue;
        if (m_onFileRead) m_onFileRead(file);
        std::ifstream in(file, std::ios::binary | std::ios::ate);
        if (!in) continue;
        out.resize(size_t(in.tellg()));
        in.seekg(0);
        in.read(reinterpret_cast<char *>(out.data()), std::streamsize(out.size()));
        if (!in && !out.empty()) continue;
        return true;
    }
    return false;
}

bool PackageSet::Contains(const std::string &resourcePath) const {
    const std::string compiled = CompiledResourcePath(resourcePath);
    for (const auto &vpk : m_packages)
        if (vpk->Contains(compiled)) return true;
    for (const auto &folder : m_diskPaths)
        if (std::filesystem::is_regular_file(folder / std::filesystem::u8path(compiled))) return true;
    return false;
}

std::vector<std::string> GameInfoSearchPaths(const std::filesystem::path &gameInfo) {
    std::ifstream in(gameInfo, std::ios::binary);
    if (!in) Fail("cannot open " + gameInfo.u8string());
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    // Tokenise the KeyValues1 text, then walk FileSystem { SearchPaths { Game <path> ... } }.
    std::vector<std::string> tokens;
    for (size_t i = 0; i < text.size();) {
        const char ch = text[i];
        if (ch == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
        } else if (isspace(static_cast<unsigned char>(ch))) {
            ++i;
        } else if (ch == '{' || ch == '}') {
            tokens.emplace_back(1, ch);
            ++i;
        } else if (ch == '"') {
            const size_t end = text.find('"', i + 1);
            if (end == std::string::npos) break;
            tokens.push_back(text.substr(i + 1, end - i - 1));
            i = end + 1;
        } else {
            size_t end = i;
            while (end < text.size() && !isspace(static_cast<unsigned char>(text[end])) && text[end] != '{' && text[end] != '}') ++end;
            tokens.push_back(text.substr(i, end - i));
            i = end;
        }
    }
    std::vector<std::string> out;
    std::vector<std::string> stack;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] == "}") {
            if (!stack.empty()) stack.pop_back();
            continue;
        }
        if (i + 1 < tokens.size() && tokens[i + 1] == "{") {
            stack.push_back(Lower(tokens[i]));
            ++i;
            continue;
        }
        if (i + 1 >= tokens.size()) break;
        const bool inSearchPaths = stack.size() >= 2 && stack.back() == "searchpaths" && stack[stack.size() - 2] == "filesystem";
        if (inSearchPaths) {
            std::string key = Lower(tokens[i]);
            // Keys can combine path IDs with '+' (e.g. "Game_LowViolence", "Game+Mod").
            bool game = false;
            size_t start = 0;
            while (start <= key.size()) {
                const size_t plus = key.find('+', start);
                if (key.substr(start, plus - start) == "game") game = true;
                if (plus == std::string::npos) break;
                start = plus + 1;
            }
            const std::string value = tokens[i + 1];
            if (game && value.find('|') == std::string::npos &&
                std::find(out.begin(), out.end(), Lower(value)) == out.end())
                out.push_back(Lower(value));
        }
        ++i;
    }
    if (out.empty()) Fail("gameinfo.gi lists no Game search paths");
    return out;
}

// ==========================================================================================
// KV3

const Kv3 *Kv3::Find(const char *key) const {
    if (type != Type::Object) return nullptr;
    for (const auto &[k, v] : obj)
        if (k == key) return &v;
    return nullptr;
}

const Kv3 &Kv3::operator[](const char *key) const {
    const Kv3 *v = Find(key);
    return v ? *v : NullValue();
}

double Kv3::Number(double fallback) const {
    switch (type) {
    case Type::Bool: return b ? 1 : 0;
    case Type::Int: return double(i);
    case Type::UInt: return double(u);
    case Type::Double: return d;
    default: return fallback;
    }
}

int64_t Kv3::Integer(int64_t fallback) const {
    switch (type) {
    case Type::Bool: return b ? 1 : 0;
    case Type::Int: return i;
    case Type::UInt: return int64_t(u);
    case Type::Double: return int64_t(d);
    default: return fallback;
    }
}

const std::string &Kv3::Str() const {
    static const std::string empty;
    return type == Type::String ? s : empty;
}

const Kv3 &Kv3::At(size_t index) const { return type == Type::Array && index < arr.size() ? arr[index] : NullValue(); }

bool IsKv3Magic(const uint8_t *data, size_t size) {
    if (size < 4) return false;
    uint32_t magic;
    std::memcpy(&magic, data, 4);
    return magic == 0x03564B56u || (magic & 0xFFFFFF00u) == 0x4B563300u && (magic & 0xFF) >= 1 && (magic & 0xFF) <= 5;
}

namespace {

enum NodeType : uint8_t {
    KV_NULL = 1, KV_BOOLEAN = 2, KV_INT64 = 3, KV_UINT64 = 4, KV_DOUBLE = 5, KV_STRING = 6, KV_BLOB = 7,
    KV_ARRAY = 8, KV_OBJECT = 9, KV_ARRAY_TYPED = 10, KV_INT32 = 11, KV_UINT32 = 12, KV_TRUE = 13,
    KV_FALSE = 14, KV_INT64_ZERO = 15, KV_INT64_ONE = 16, KV_DOUBLE_ZERO = 17, KV_DOUBLE_ONE = 18,
    KV_FLOAT = 19, KV_INT16 = 20, KV_UINT16 = 21, KV_UNKNOWN_22 = 22, KV_INT32_AS_BYTE = 23,
    KV_ARRAY_BYTE_LENGTH = 24, KV_ARRAY_AUX = 25,
};

struct Span {
    const uint8_t *p = nullptr;
    size_t n = 0;
    template <class T> T Pop() {
        if (n < sizeof(T)) Fail("KV3 buffer underrun");
        T v;
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        n -= sizeof(T);
        return v;
    }
    Span Slice(size_t from, size_t to) const {
        if (from > to || to > n) Fail("KV3 slice out of range");
        return Span{p + from, to - from};
    }
};

struct Buffers {
    Span b1, b2, b4, b8;
};

struct Context {
    int version = 0;
    Span types, objectLengths, blobs, blobLengths;
    std::vector<std::string> strings;
    Buffers buffer, aux;
};

void Align(size_t &offset, size_t alignment) { offset = (offset + alignment - 1) & ~(alignment - 1); }

std::string ReadString(Context &ctx, int32_t id) {
    if (id == -1) return {};
    if (id < 0 || size_t(id) >= ctx.strings.size()) Fail("KV3 string index out of range");
    return ctx.strings[size_t(id)];
}

// Flag byte -> ValveKeyValue KVFlag (resource=1, resource_name=2, panorama=3, soundevent=4,
// subclass=5, entity_name=6). Versions before 3 store it as a bit position instead.
uint8_t NormalizeFlag(uint8_t raw, int version) {
    if (version >= 3) {
        if (raw > 6) Fail("unexpected KV3 flag " + std::to_string(raw));
        return raw;
    }
    if (raw & 4) raw ^= 4; // multiline-string marker, not a real flag
    switch (raw) {
    case 0: return 0;
    case 1: return 1;
    case 2: return 2;
    case 8: return 3;
    case 16: return 4;
    case 32: return 5;
    default: Fail("unexpected KV3 flag " + std::to_string(raw));
    }
}

std::pair<uint8_t, uint8_t> ReadType(Context &ctx) {
    uint8_t type = ctx.types.Pop<uint8_t>();
    uint8_t flag = 0;
    if (ctx.version >= 3) {
        if (type & 0x80) {
            type &= 0x3F;
            flag = NormalizeFlag(ctx.types.Pop<uint8_t>(), ctx.version);
        }
    } else if (type & 0x80) {
        type &= 0x7F;
        flag = NormalizeFlag(ctx.types.Pop<uint8_t>(), ctx.version);
    }
    return {type, flag};
}

Kv3 ReadValue(Context &ctx, uint8_t type, uint8_t flag);

void ParseMember(Context &ctx, Kv3 &parent) {
    const auto [type, flag] = ReadType(ctx);
    if (parent.type == Kv3::Type::Array) {
        parent.arr.push_back(ReadValue(ctx, type, flag));
        return;
    }
    const int32_t id = ctx.buffer.b4.Pop<int32_t>();
    std::string name = ReadString(ctx, id);
    parent.obj.emplace_back(std::move(name), ReadValue(ctx, type, flag));
}

Kv3 ReadValue(Context &ctx, uint8_t type, uint8_t flag) {
    Kv3 v;
    v.flag = flag;
    Buffers &buf = ctx.buffer;
    switch (type) {
    case KV_NULL: v.type = Kv3::Type::Null; break;
    case KV_TRUE: v.type = Kv3::Type::Bool; v.b = true; break;
    case KV_FALSE: v.type = Kv3::Type::Bool; v.b = false; break;
    case KV_INT64_ZERO: v.type = Kv3::Type::Int; v.i = 0; break;
    case KV_INT64_ONE: v.type = Kv3::Type::Int; v.i = 1; break;
    case KV_DOUBLE_ZERO: v.type = Kv3::Type::Double; v.d = 0; break;
    case KV_DOUBLE_ONE: v.type = Kv3::Type::Double; v.d = 1; break;
    case KV_BOOLEAN: v.type = Kv3::Type::Bool; v.b = buf.b1.Pop<uint8_t>() == 1; break;
    case KV_INT32_AS_BYTE: v.type = Kv3::Type::Int; v.width = Kv3::Width::Int32; v.i = buf.b1.Pop<uint8_t>(); break;
    case KV_INT16: v.type = Kv3::Type::Int; v.width = Kv3::Width::Int16; v.i = buf.b2.Pop<int16_t>(); break;
    case KV_UINT16: v.type = Kv3::Type::UInt; v.width = Kv3::Width::UInt16; v.u = buf.b2.Pop<uint16_t>(); break;
    case KV_INT32: v.type = Kv3::Type::Int; v.width = Kv3::Width::Int32; v.i = buf.b4.Pop<int32_t>(); break;
    case KV_UINT32: v.type = Kv3::Type::UInt; v.width = Kv3::Width::UInt32; v.u = buf.b4.Pop<uint32_t>(); break;
    case KV_FLOAT: v.type = Kv3::Type::Double; v.width = Kv3::Width::Float; v.d = buf.b4.Pop<float>(); break;
    case KV_INT64: v.type = Kv3::Type::Int; v.i = buf.b8.Pop<int64_t>(); break;
    case KV_UINT64: v.type = Kv3::Type::UInt; v.u = buf.b8.Pop<uint64_t>(); break;
    case KV_DOUBLE: v.type = Kv3::Type::Double; v.d = buf.b8.Pop<double>(); break;
    case KV_STRING: v.type = Kv3::Type::String; v.s = ReadString(ctx, buf.b4.Pop<int32_t>()); break;
    case KV_BLOB: {
        v.type = Kv3::Type::Blob;
        if (ctx.version < 2) {
            const int32_t length = buf.b4.Pop<int32_t>();
            if (length < 0 || size_t(length) > buf.b1.n) Fail("KV3 blob out of range");
            v.blob.assign(buf.b1.p, buf.b1.p + length);
            buf.b1 = buf.b1.Slice(size_t(length), buf.b1.n);
        } else {
            const int32_t length = ctx.blobLengths.Pop<int32_t>();
            if (length < 0 || size_t(length) > ctx.blobs.n) Fail("KV3 blob out of range");
            v.blob.assign(ctx.blobs.p, ctx.blobs.p + length);
            ctx.blobs = ctx.blobs.Slice(size_t(length), ctx.blobs.n);
        }
        break;
    }
    case KV_ARRAY: {
        v.type = Kv3::Type::Array;
        const int32_t length = buf.b4.Pop<int32_t>();
        if (length < 0) Fail("negative KV3 array length");
        v.arr.reserve(size_t(length));
        for (int32_t k = 0; k < length; ++k) ParseMember(ctx, v);
        break;
    }
    case KV_ARRAY_TYPED:
    case KV_ARRAY_BYTE_LENGTH: {
        v.type = Kv3::Type::Array;
        const int32_t length = type == KV_ARRAY_BYTE_LENGTH ? int32_t(buf.b1.Pop<uint8_t>()) : buf.b4.Pop<int32_t>();
        if (length < 0) Fail("negative KV3 array length");
        const auto [sub, subFlag] = ReadType(ctx);
        v.arr.reserve(size_t(length));
        for (int32_t k = 0; k < length; ++k) v.arr.push_back(ReadValue(ctx, sub, subFlag));
        break;
    }
    case KV_ARRAY_AUX: {
        v.type = Kv3::Type::Array;
        const int32_t length = buf.b1.Pop<uint8_t>();
        const auto [sub, subFlag] = ReadType(ctx);
        std::swap(ctx.buffer, ctx.aux);
        v.arr.reserve(size_t(length));
        for (int32_t k = 0; k < length; ++k) v.arr.push_back(ReadValue(ctx, sub, subFlag));
        std::swap(ctx.buffer, ctx.aux);
        break;
    }
    case KV_OBJECT: {
        v.type = Kv3::Type::Object;
        const int32_t length = ctx.version >= 5 ? ctx.objectLengths.Pop<int32_t>() : buf.b4.Pop<int32_t>();
        if (length < 0) Fail("negative KV3 object length");
        v.obj.reserve(size_t(length));
        for (int32_t k = 0; k < length; ++k) ParseMember(ctx, v);
        break;
    }
    default: Fail("unknown KV3 node type " + std::to_string(type));
    }
    return v;
}

void Lz4Exact(const uint8_t *src, size_t srcSize, uint8_t *dst, size_t dstSize) {
    const int written = LZ4_decompress_safe(reinterpret_cast<const char *>(src), reinterpret_cast<char *>(dst), int(srcSize), int(dstSize));
    if (written < 0 || size_t(written) != dstSize) Fail("LZ4 decompression failed");
}

void ZstdExact(const uint8_t *src, size_t srcSize, uint8_t *dst, size_t dstSize) {
    const size_t written = ZSTD_decompress(dst, dstSize, src, srcSize);
    if (ZSTD_isError(written) || written != dstSize) Fail("ZSTD decompression failed");
}

std::vector<std::string> ReadStrings(Span &s, int count) {
    std::vector<std::string> out;
    out.reserve(size_t(count));
    for (int k = 0; k < count; ++k) {
        const uint8_t *end = static_cast<const uint8_t *>(std::memchr(s.p, 0, s.n));
        if (!end) Fail("unterminated KV3 string");
        out.emplace_back(reinterpret_cast<const char *>(s.p), size_t(end - s.p));
        s = s.Slice(size_t(end - s.p) + 1, s.n);
    }
    return out;
}

// Legacy (VKV3) reader.
struct LegacyReader {
    Cursor c;
    std::vector<std::string> strings;
    std::pair<uint8_t, uint8_t> Type() {
        uint8_t t = c.Get<uint8_t>(), f = 0;
        if (t & 0x80) {
            t &= 0x7F;
            f = NormalizeFlag(c.Get<uint8_t>(), 0);
        }
        return {t, f};
    }
    std::string Str(int32_t id) {
        if (id == -1) return {};
        if (id < 0 || size_t(id) >= strings.size()) Fail("KV3 string index out of range");
        return strings[size_t(id)];
    }
    void Member(Kv3 &parent) {
        std::string name;
        const bool object = parent.type == Kv3::Type::Object;
        if (object) name = Str(c.Get<int32_t>());
        const auto [t, f] = Type();
        Kv3 v = Value(t, f);
        if (object) parent.obj.emplace_back(std::move(name), std::move(v));
        else parent.arr.push_back(std::move(v));
    }
    Kv3 Value(uint8_t t, uint8_t f) {
        Kv3 v;
        v.flag = f;
        switch (t) {
        case KV_NULL: break;
        case KV_BOOLEAN: v.type = Kv3::Type::Bool; v.b = c.Get<uint8_t>() != 0; break;
        case KV_TRUE: v.type = Kv3::Type::Bool; v.b = true; break;
        case KV_FALSE: v.type = Kv3::Type::Bool; break;
        case KV_INT64_ZERO: v.type = Kv3::Type::Int; break;
        case KV_INT64_ONE: v.type = Kv3::Type::Int; v.i = 1; break;
        case KV_INT64: v.type = Kv3::Type::Int; v.i = c.Get<int64_t>(); break;
        case KV_UINT64: v.type = Kv3::Type::UInt; v.u = c.Get<uint64_t>(); break;
        case KV_INT32: v.type = Kv3::Type::Int; v.width = Kv3::Width::Int32; v.i = c.Get<int32_t>(); break;
        case KV_UINT32: v.type = Kv3::Type::UInt; v.width = Kv3::Width::UInt32; v.u = c.Get<uint32_t>(); break;
        case KV_DOUBLE: v.type = Kv3::Type::Double; v.d = c.Get<double>(); break;
        case KV_DOUBLE_ZERO: v.type = Kv3::Type::Double; break;
        case KV_DOUBLE_ONE: v.type = Kv3::Type::Double; v.d = 1; break;
        case KV_STRING: v.type = Kv3::Type::String; v.s = Str(c.Get<int32_t>()); break;
        case KV_BLOB: {
            v.type = Kv3::Type::Blob;
            const int32_t length = c.Get<int32_t>();
            if (length < 0) Fail("negative KV3 blob");
            const uint8_t *p = c.Take(size_t(length));
            v.blob.assign(p, p + length);
            break;
        }
        case KV_ARRAY: {
            v.type = Kv3::Type::Array;
            const int32_t length = c.Get<int32_t>();
            for (int32_t k = 0; k < length; ++k) Member(v);
            break;
        }
        case KV_ARRAY_TYPED: {
            v.type = Kv3::Type::Array;
            const int32_t length = c.Get<int32_t>();
            const auto [st, sf] = Type();
            for (int32_t k = 0; k < length; ++k) v.arr.push_back(Value(st, sf));
            break;
        }
        case KV_OBJECT: {
            v.type = Kv3::Type::Object;
            const int32_t length = c.Get<int32_t>();
            for (int32_t k = 0; k < length; ++k) Member(v);
            break;
        }
        default: Fail("unknown legacy KV3 node type");
        }
        return v;
    }
};

const uint8_t kEncodingBinary[16] = {0x00, 0x05, 0x86, 0x1B, 0xD8, 0xF7, 0xC1, 0x40, 0xAD, 0x82, 0x75, 0xA4, 0x82, 0x67, 0xE7, 0x14};
const uint8_t kEncodingLz4[16] = {0x8A, 0x34, 0x47, 0x68, 0xA1, 0x63, 0x5C, 0x4F, 0xA1, 0x97, 0x53, 0x80, 0x6F, 0xD9, 0xB1, 0x19};

Kv3 DecodeLegacy(Cursor c) {
    const uint8_t *encoding = c.Take(16);
    c.Take(16); // format
    std::vector<uint8_t> raw;
    if (!std::memcmp(encoding, kEncodingLz4, 16)) {
        const int32_t outSize = c.Get<int32_t>();
        if (outSize < 0 || outSize > 1024 * 1024 * 1024) Fail("bad legacy KV3 size");
        raw.resize(size_t(outSize));
        Lz4Exact(c.p + c.at, c.n - c.at, raw.data(), raw.size());
    } else if (!std::memcmp(encoding, kEncodingBinary, 16)) {
        raw.assign(c.p + c.at, c.p + c.n);
    } else {
        Fail("unsupported legacy KV3 encoding");
    }
    LegacyReader r{Cursor{raw.data(), raw.size()}};
    const uint32_t count = r.c.Get<uint32_t>();
    for (uint32_t k = 0; k < count; ++k) r.strings.push_back(r.c.CStr());
    const auto [t, f] = r.Type();
    Kv3 root = r.Value(t, f);
    if (r.c.Get<uint32_t>() != 0xFFFFFFFFu) Fail("bad legacy KV3 trailer");
    return root;
}

} // namespace

Kv3 DecodeKv3(const uint8_t *data, size_t size) {
    Cursor c{data, size};
    const uint32_t magic = c.Get<uint32_t>();
    if (magic == 0x03564B56u) return DecodeLegacy(c);
    const int version = int(magic & 0xFF);
    if ((magic & 0xFFFFFF00u) != 0x4B563300u || version < 1 || version > 5) Fail("not binary KV3");
    Context ctx;
    ctx.version = version;
    c.Take(16); // format GUID
    const uint32_t compression = c.Get<uint32_t>();
    if (compression > 2) Fail("unknown KV3 compression");
    uint16_t frameSize = 0;
    int32_t countBytes1 = 0, countBytes4 = 0, countBytes8 = 0, countTypes = 0, sizeUncompressedTotal = 0,
            sizeCompressedTotal = 0, countBlocks = 0, sizeBlobs = 0, countBytes2 = 0;
    if (version == 1) {
        countBytes1 = c.Get<int32_t>();
        countBytes4 = c.Get<int32_t>();
        countBytes8 = c.Get<int32_t>();
        sizeUncompressedTotal = c.Get<int32_t>();
        sizeCompressedTotal = int32_t(size - c.at);
    } else {
        if (c.Get<uint16_t>() != 0) Fail("KV3 compression dictionaries unsupported");
        frameSize = c.Get<uint16_t>();
        countBytes1 = c.Get<int32_t>();
        countBytes4 = c.Get<int32_t>();
        countBytes8 = c.Get<int32_t>();
        countTypes = c.Get<int32_t>();
        c.Get<uint16_t>(); // object count
        c.Get<uint16_t>(); // array count
        sizeUncompressedTotal = c.Get<int32_t>();
        sizeCompressedTotal = c.Get<int32_t>();
        countBlocks = c.Get<int32_t>();
        sizeBlobs = c.Get<int32_t>();
    }
    if (version >= 4) {
        countBytes2 = c.Get<int32_t>();
        c.Get<int32_t>(); // block compressed sizes bytes
    }
    int32_t sizeU1 = sizeUncompressedTotal, sizeC1 = sizeCompressedTotal, sizeU2 = 0, sizeC2 = 0;
    int32_t b2c1 = 0, b2c2 = 0, b2c4 = 0, b2c8 = 0, b2objects = 0;
    if (version >= 5) {
        sizeU1 = c.Get<int32_t>();
        sizeC1 = c.Get<int32_t>();
        sizeU2 = c.Get<int32_t>();
        sizeC2 = c.Get<int32_t>();
        b2c1 = c.Get<int32_t>();
        b2c2 = c.Get<int32_t>();
        b2c4 = c.Get<int32_t>();
        b2c8 = c.Get<int32_t>();
        c.Get<int32_t>();
        b2objects = c.Get<int32_t>();
        c.Get<int32_t>(); // arrays
        c.Get<int32_t>();
    }
    const auto sane = [](int32_t v) { return v >= 0 && v <= 1024 * 1024 * 1024; };
    if (!sane(sizeU1) || !sane(sizeC1) || !sane(sizeU2) || !sane(sizeC2) || !sane(sizeBlobs) || countBlocks < 0)
        Fail("KV3 sizes out of range");

    // Buffer 1
    const bool zstdWithBlobs = version < 5 && compression == 2;
    std::vector<uint8_t> buffer1(size_t(sizeU1) + (zstdWithBlobs ? size_t(sizeBlobs) : 0));
    if (compression == 0) {
        std::memcpy(buffer1.data(), c.Take(size_t(sizeU1)), size_t(sizeU1));
    } else if (compression == 1) {
        Lz4Exact(c.Take(size_t(sizeC1)), size_t(sizeC1), buffer1.data(), size_t(sizeU1));
    } else {
        ZstdExact(c.Take(size_t(sizeC1)), size_t(sizeC1), buffer1.data(), buffer1.size());
    }
    Span s1{buffer1.data(), size_t(sizeU1)};
    Buffers b1;
    size_t offset = 0;
    if (countBytes1 > 0) { b1.b1 = s1.Slice(offset, offset + size_t(countBytes1)); offset += size_t(countBytes1); }
    if (countBytes2 > 0) { Align(offset, 2); b1.b2 = s1.Slice(offset, offset + size_t(countBytes2) * 2); offset += size_t(countBytes2) * 2; }
    if (countBytes4 > 0) { Align(offset, 4); b1.b4 = s1.Slice(offset, offset + size_t(countBytes4) * 4); offset += size_t(countBytes4) * 4; }
    if (countBytes8 > 0) { Align(offset, 8); b1.b8 = s1.Slice(offset, offset + size_t(countBytes8) * 8); offset += size_t(countBytes8) * 8; }
    else if (version < 5) Align(offset, 8);
    const int32_t countStrings = b1.b4.Pop<int32_t>();
    if (countStrings < 0) Fail("negative KV3 string count");
    Span blobSizes;
    if (version >= 5) {
        ctx.aux = b1;
        ctx.strings = ReadStrings(ctx.aux.b1, countStrings);
    } else {
        ctx.buffer = b1;
        Span strings = s1.Slice(offset, s1.n);
        const size_t before = strings.n;
        ctx.strings = ReadStrings(strings, countStrings);
        const size_t stringsStart = offset;
        offset += before - strings.n;
        const size_t typesLength = version == 1 ? size_t(sizeUncompressedTotal) - offset - 4
                                                : size_t(countTypes) - offset + stringsStart;
        ctx.types = s1.Slice(offset, offset + typesLength);
        offset += typesLength;
        if (countBlocks == 0) {
            Span trailer = s1.Slice(offset, s1.n);
            if (trailer.Pop<uint32_t>() != 0xFFEEDD00u) Fail("bad KV3 trailer");
        } else {
            blobSizes = s1.Slice(offset, s1.n);
        }
    }

    // Buffer 2
    std::vector<uint8_t> buffer2;
    if (version >= 5) {
        buffer2.resize(size_t(sizeU2));
        if (compression == 0) std::memcpy(buffer2.data(), c.Take(size_t(sizeU2)), size_t(sizeU2));
        else if (compression == 1) Lz4Exact(c.Take(size_t(sizeC2)), size_t(sizeC2), buffer2.data(), size_t(sizeU2));
        else ZstdExact(c.Take(size_t(sizeC2)), size_t(sizeC2), buffer2.data(), size_t(sizeU2));
        Span s2{buffer2.data(), buffer2.size()};
        size_t o = size_t(b2objects) * 4;
        ctx.objectLengths = s2.Slice(0, o);
        Buffers b;
        if (b2c1 > 0) { b.b1 = s2.Slice(o, o + size_t(b2c1)); o += size_t(b2c1); }
        if (b2c2 > 0) { Align(o, 2); b.b2 = s2.Slice(o, o + size_t(b2c2) * 2); o += size_t(b2c2) * 2; }
        if (b2c4 > 0) { Align(o, 4); b.b4 = s2.Slice(o, o + size_t(b2c4) * 4); o += size_t(b2c4) * 4; }
        if (b2c8 > 0) { Align(o, 8); b.b8 = s2.Slice(o, o + size_t(b2c8) * 8); o += size_t(b2c8) * 8; }
        ctx.buffer = b;
        ctx.types = s2.Slice(o, o + size_t(countTypes));
        o += size_t(countTypes);
        if (countBlocks == 0) {
            Span trailer = s2.Slice(o, s2.n);
            if (trailer.Pop<uint32_t>() != 0xFFEEDD00u) Fail("bad KV3 trailer");
        } else {
            blobSizes = s2.Slice(o, s2.n);
        }
    }

    // Binary blobs
    std::vector<uint8_t> blobs;
    if (countBlocks > 0) {
        ctx.blobLengths = blobSizes.Slice(0, size_t(countBlocks) * 4);
        blobSizes = blobSizes.Slice(size_t(countBlocks) * 4, blobSizes.n);
        if (blobSizes.Pop<uint32_t>() != 0xFFEEDD00u) Fail("bad KV3 blob trailer");
        blobs.resize(size_t(sizeBlobs));
        if (compression == 0) {
            std::memcpy(blobs.data(), c.Take(size_t(sizeBlobs)), size_t(sizeBlobs));
        } else if (compression == 1) {
            // Chained LZ4 frames: each frame may reference the previously decoded output.
            LZ4_streamDecode_t *stream = LZ4_createStreamDecode();
            size_t written = 0;
            try {
                while (blobSizes.n > 0) {
                    const uint16_t compressed = blobSizes.Pop<uint16_t>();
                    const size_t frame = std::min<size_t>(frameSize, blobs.size() - written);
                    const int got = LZ4_decompress_safe_continue(stream, reinterpret_cast<const char *>(c.Take(compressed)),
                                                                 reinterpret_cast<char *>(blobs.data() + written), compressed, int(frame));
                    if (got < 1) Fail("LZ4 blob decompression failed");
                    written += size_t(got);
                }
            } catch (...) {
                LZ4_freeStreamDecode(stream);
                throw;
            }
            LZ4_freeStreamDecode(stream);
            if (written != blobs.size()) Fail("LZ4 blob size mismatch");
        } else if (version >= 5) {
            const int32_t compressedBlobs = sizeCompressedTotal - sizeC1 - sizeC2;
            if (compressedBlobs < 0) Fail("bad KV3 blob size");
            ZstdExact(c.Take(size_t(compressedBlobs)), size_t(compressedBlobs), blobs.data(), blobs.size());
        } else {
            std::memcpy(blobs.data(), buffer1.data() + sizeU1, size_t(sizeBlobs));
        }
        ctx.blobs = Span{blobs.data(), blobs.size()};
        if (c.Get<uint32_t>() != 0xFFEEDD00u) Fail("bad KV3 blob trailer");
    }

    const auto [rootType, rootFlag] = ReadType(ctx);
    return ReadValue(ctx, rootType, rootFlag);
}

// ==========================================================================================
// Resource

Resource::Resource(std::vector<uint8_t> bytes) : m_bytes(std::move(bytes)) {
    Cursor c{m_bytes.data(), m_bytes.size()};
    m_declaredSize = c.Get<uint32_t>();
    if (c.Get<uint16_t>() != 12) Fail("not a compiled resource");
    c.Get<uint16_t>(); // version
    const size_t blockOffsetField = c.at;
    const uint32_t blockOffset = c.Get<uint32_t>();
    const uint32_t blockCount = c.Get<uint32_t>();
    if (blockCount > 256) Fail("too many resource blocks");
    c.at = blockOffsetField + blockOffset;
    for (uint32_t k = 0; k < blockCount; ++k) {
        ResourceBlock b;
        b.type = c.Get<uint32_t>();
        const size_t field = c.at;
        b.offset = uint32_t(field + c.Get<uint32_t>());
        b.size = c.Get<uint32_t>();
        if (!b.size) continue;
        if (b.offset > m_bytes.size() || b.size > m_bytes.size() - b.offset) Fail("resource block out of range");
        m_blocks.push_back(b);
    }
}

namespace {
uint32_t FourCC(const char f[4]) {
    return uint32_t(uint8_t(f[0])) | uint32_t(uint8_t(f[1])) << 8 | uint32_t(uint8_t(f[2])) << 16 | uint32_t(uint8_t(f[3])) << 24;
}
} // namespace

const ResourceBlock *Resource::Block(const char fourcc[4]) const {
    const uint32_t type = FourCC(fourcc);
    for (const auto &b : m_blocks)
        if (b.type == type) return &b;
    return nullptr;
}

std::vector<const ResourceBlock *> Resource::BlocksOfType(const char fourcc[4]) const {
    std::vector<const ResourceBlock *> out;
    const uint32_t type = FourCC(fourcc);
    for (const auto &b : m_blocks)
        if (b.type == type) out.push_back(&b);
    return out;
}

Kv3 Resource::Kv3Block(const char fourcc[4]) const {
    const ResourceBlock *b = Block(fourcc);
    if (!b) Fail(std::string("resource has no ") + std::string(fourcc, 4) + " block");
    return Kv3Block(*b);
}

Kv3 Resource::Kv3Block(const ResourceBlock &block) const {
    if (!IsKv3Magic(Data(block), block.size)) Fail("resource block is not KV3");
    return DecodeKv3(Data(block), block.size);
}

// ==========================================================================================
// Hashing

uint32_t StringToken(const std::string &text) {
    const uint32_t m = 0x5bd1e995u;
    const size_t length = text.size();
    if (!length) return 0;
    std::string lower = text;
    for (char &ch : lower)
        if (ch >= 'A' && ch <= 'Z') ch = char(ch | 0x20);
    uint32_t h = 0x31415926u ^ uint32_t(length);
    size_t at = 0, left = length;
    const auto byte = [&](size_t k) { return uint32_t(uint8_t(lower[k])); };
    while (left >= 4) {
        uint32_t k = byte(at) | byte(at + 1) << 8 | byte(at + 2) << 16 | byte(at + 3) << 24;
        at += 4;
        k *= m;
        k ^= k >> 24;
        k *= m;
        h *= m;
        h ^= k;
        left -= 4;
    }
    switch (left) {
    case 3: h ^= byte(at) | byte(at + 1) << 8; h ^= byte(at + 2) << 16; h *= m; break;
    case 2: h ^= byte(at) | byte(at + 1) << 8; h *= m; break;
    case 1: h ^= byte(at); h *= m; break;
    default: break;
    }
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;
    return h;
}

namespace {
struct Sha256 {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
            BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0)
            Fail("SHA-256 unavailable");
    }
    ~Sha256() {
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    void Add(const void *p, size_t n) {
        const uint8_t *bytes = static_cast<const uint8_t *>(p);
        while (n) {
            const ULONG chunk = ULONG(std::min<size_t>(n, 1u << 30));
            if (BCryptHashData(hash, const_cast<PUCHAR>(bytes), chunk, 0) != 0) Fail("SHA-256 failed");
            bytes += chunk;
            n -= chunk;
        }
    }
    std::string Hex() {
        uint8_t digest[32];
        if (BCryptFinishHash(hash, digest, 32, 0) != 0) Fail("SHA-256 failed");
        static const char *digits = "0123456789abcdef";
        std::string out(64, '0');
        for (int k = 0; k < 32; ++k) {
            out[size_t(k) * 2] = digits[digest[k] >> 4];
            out[size_t(k) * 2 + 1] = digits[digest[k] & 15];
        }
        return out;
    }
};
} // namespace

std::string Sha256Bytes(const void *data, size_t size) {
    Sha256 sha;
    sha.Add(data, size);
    return sha.Hex();
}

std::string Sha256File(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) Fail("cannot open " + path.u8string());
    Sha256 sha;
    std::vector<char> chunk(4 * 1024 * 1024);
    while (in) {
        in.read(chunk.data(), std::streamsize(chunk.size()));
        if (in.gcount() > 0) sha.Add(chunk.data(), size_t(in.gcount()));
    }
    return sha.Hex();
}

} // namespace Filmmaker::MapData
