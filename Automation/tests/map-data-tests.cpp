// Native map extractor (AfxHookSource2/Filmmaker/MapData) format and rule checks on synthetic
// data. Whole-map parity against an earlier extraction is Automation/verify/compare-map-data.py.
#include "../../AfxHookSource2/Filmmaker/MapData/MapCacheCodec.h"
#include "../../AfxHookSource2/Filmmaker/MapData/MapDataIo.h"
#include "../../AfxHookSource2/Filmmaker/MapData/MapEntities.h"
#include "../../AfxHookSource2/Filmmaker/MapData/MapGeometry.h"
#include "../../AfxHookSource2/Filmmaker/MapData/MapPhysics.h"
#include "../../AfxHookSource2/Filmmaker/MapData/MapTextures.h"

#include "../../AfxHookSource2/Filmmaker/MapData/MapSources.h"

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Filmmaker::MapData;

static int g_failures = 0;
static void Check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

struct Bytes {
    std::vector<uint8_t> b;
    template <class T> void Put(T v) { b.insert(b.end(), reinterpret_cast<uint8_t *>(&v), reinterpret_cast<uint8_t *>(&v) + sizeof v); }
    void Str(const std::string &s) { b.insert(b.end(), s.begin(), s.end()); b.push_back(0); }
    void Raw(const void *p, size_t n) { b.insert(b.end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n); }
};

// Single-file VPK v1 with every entry's data after the tree (archive index 0x7FFF).
static std::vector<uint8_t> MakeVpk(const std::vector<std::pair<std::string, std::string>> &files) {
    Bytes tree, data;
    for (const auto &[path, content] : files) {
        const size_t slash = path.rfind('/'), dot = path.rfind('.');
        tree.Str(path.substr(dot + 1));
        tree.Str(path.substr(0, slash));
        tree.Str(path.substr(slash + 1, dot - slash - 1));
        tree.Put<uint32_t>(0);
        tree.Put<uint16_t>(0);
        tree.Put<uint16_t>(0x7FFF);
        tree.Put<uint32_t>(uint32_t(data.b.size()));
        tree.Put<uint32_t>(uint32_t(content.size()));
        tree.Put<uint16_t>(0xFFFF);
        tree.Put<uint8_t>(0);
        tree.Put<uint8_t>(0);
        data.Raw(content.data(), content.size());
    }
    tree.Put<uint8_t>(0);
    Bytes vpk;
    vpk.Put<uint32_t>(0x55AA1234u);
    vpk.Put<uint32_t>(1);
    vpk.Put<uint32_t>(uint32_t(tree.b.size()));
    vpk.Raw(tree.b.data(), tree.b.size());
    vpk.Raw(data.b.data(), data.b.size());
    return vpk.b;
}

static void TestStringToken() {
    Check(StringToken("classname") == 0xc61b1c62u, "StringToken(classname)");
    Check(StringToken("world.glass") == 0x1f422e98u, "StringToken(world.glass)");
    Check(StringToken("ClassName") == StringToken("classname"), "StringToken is case-insensitive");
    Check(StringToken("") == 0, "StringToken of empty string is 0");
    Check(KnownEntityString(0x555cfc79u) == "glass", "known surface property name resolves");
    Check(KnownEntityString(1u).rfind("vrf_unknown_key_", 0) == 0, "unknown hash keeps its placeholder name");
}

static void TestVpk() {
    const auto bytes = MakeVpk({{"maps/test/a.txt", "alpha"}, {"materials/b.vmat_c", "beta"}});
    Vpk vpk(std::vector<uint8_t>(bytes), "test.vpk");
    std::vector<uint8_t> out;
    Check(vpk.Read("maps/test/a.txt", out) && std::string(out.begin(), out.end()) == "alpha", "in-memory VPK reads an entry");
    Check(vpk.Read("MATERIALS/B.VMAT_C", out) && std::string(out.begin(), out.end()) == "beta", "VPK lookup is case-insensitive");
    Check(!vpk.Read("missing.txt", out), "VPK reports a missing entry");
    Check(vpk.List("vmat_c").size() == 1, "VPK lists entries by extension");
    // Workshop addons nest the map package as an entry.
    const auto outer = MakeVpk({{"maps/nested.vpk", std::string(bytes.begin(), bytes.end())}});
    Vpk addon(std::vector<uint8_t>(outer), "addon.vpk");
    std::vector<uint8_t> nestedBytes;
    Check(addon.Read("maps/nested.vpk", nestedBytes), "addon contains the nested map package");
    Vpk nested(std::move(nestedBytes), "maps/nested.vpk");
    Check(nested.Read("maps/test/a.txt", out) && out.size() == 5, "nested VPK reads its own entries");
    Check(CompiledResourcePath("Models\\Props\\Crate.VMDL") == "models/props/crate.vmdl_c", "resource path normalisation");
    bool threw = false;
    try { Vpk bad(std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, "bad"); } catch (const MapDataError &) { threw = true; }
    Check(threw, "malformed VPK is rejected");
}

// Legacy binary VKV3 (uncompressed): strings, then the typed value tree.
static void TestKv3() {
    static const uint8_t binary[16] = {0x00, 0x05, 0x86, 0x1B, 0xD8, 0xF7, 0xC1, 0x40, 0xAD, 0x82, 0x75, 0xA4, 0x82, 0x67, 0xE7, 0x14};
    Bytes b;
    b.Put<uint32_t>(0x03564B56u);
    b.Raw(binary, 16);
    b.Raw(binary, 16); // format GUID (unused)
    b.Put<uint32_t>(3);
    b.Str("name");
    b.Str("count");
    b.Str("crate");
    b.Put<uint8_t>(9); // object
    b.Put<int32_t>(2);
    b.Put<int32_t>(0);
    b.Put<uint8_t>(6); // string
    b.Put<int32_t>(2);
    b.Put<int32_t>(1);
    b.Put<uint8_t>(11); // int32
    b.Put<int32_t>(42);
    b.Put<uint32_t>(0xFFFFFFFFu);
    const Kv3 v = DecodeKv3(b.b.data(), b.b.size());
    Check(v.type == Kv3::Type::Object && v.Size() == 2, "legacy KV3 object decodes");
    Check(v["name"].Str() == "crate", "KV3 string member");
    Check(v["count"].Integer() == 42 && v["count"].width == Kv3::Width::Int32, "KV3 int32 member keeps its width");
    Check(v["missing"].IsNull(), "absent KV3 member is null");
    bool threw = false;
    try { DecodeKv3(b.b.data(), b.b.size() - 6); } catch (const MapDataError &) { threw = true; }
    Check(threw, "truncated KV3 is rejected");
}

static void TestOccluderBuilder() {
    OccluderBuilder builder;
    builder.AddTriangle({0, 0, 0}, {1, 0, 0}, {0, 1, 0});
    builder.AddTriangle({0, 0, 0}, {1, 0, 0}, {2, 0, 0}); // collinear
    builder.AddTriangle({-0.f, 0, 0}, {0, 1, 0}, {0, 0, 1});
    Check(builder.indices.size() == 6, "degenerate triangles are dropped");
    Check(builder.vertices.size() == 4, "coincident positions (including -0) are shared");
    OcclusionMesh mesh;
    mesh.vertices = builder.vertices;
    mesh.indices = builder.indices;
    BoundsOf(mesh.vertices, mesh.low, mesh.high);
    const auto bytes = SerializeOcclusion(mesh);
    Check(bytes.size() == 8 + 8 + 24 + 4 * 12 + 6 * 4 && std::memcmp(bytes.data(), "MVMOCC01", 8) == 0, "MVMOCC01 layout");
    float high[3];
    std::memcpy(high, bytes.data() + 28, 12);
    Check(high[0] == 1 && high[1] == 1 && high[2] == 1, "MVMOCC01 bounds");
}

static void TestTexture() {
    // Compiled resource: header, one DATA block (texture header), then mip 0 RGBA8888 2x2.
    Bytes r;
    r.Put<uint32_t>(0);
    r.Put<uint16_t>(12);
    r.Put<uint16_t>(0);
    r.Put<uint32_t>(8); // block table follows this field
    r.Put<uint32_t>(1);
    r.Raw("DATA", 4);
    r.Put<uint32_t>(8); // DATA starts right after this block entry
    r.Put<uint32_t>(40);
    const size_t dataStart = r.b.size();
    r.Put<uint16_t>(1);
    r.Put<uint16_t>(0);
    for (int k = 0; k < 4; ++k) r.Put<float>(0);
    r.Put<uint16_t>(2);
    r.Put<uint16_t>(2);
    r.Put<uint16_t>(1);
    r.Put<uint8_t>(4); // RGBA8888
    r.Put<uint8_t>(1);
    r.Put<uint32_t>(0);
    r.Put<uint32_t>(8);
    r.Put<uint32_t>(0);
    Check(r.b.size() - dataStart == 40, "synthetic texture header size");
    const uint8_t pixels[16] = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 10, 20, 30, 40};
    r.Raw(pixels, 16);
    const Image image = DecodeTexture(r.b);
    Check(image.width == 2 && image.height == 2, "texture size");
    Check(image.rgba.size() == 16 && std::memcmp(image.rgba.data(), pixels, 16) == 0, "RGBA8888 texels decode unchanged");
}

static void TestLightsJson() {
    MapEntity light;
    light.resource = "maps/x/entities/default_ents.vents_c";
    const auto prop = [](const char *key, const char *type, const char *value, Kv3 raw) {
        EntityProperty p;
        p.key = key;
        p.type = type;
        p.value = value;
        p.raw = std::move(raw);
        return p;
    };
    Kv3 cls, id, bright;
    cls.type = id.type = Kv3::Type::String;
    cls.s = "light_omni2";
    id.s = "1:2:3";
    bright.type = Kv3::Type::Double;
    bright.d = 2.5;
    light.properties = {prop("brightness", "FloatingPoint64", "2.5", bright), prop("classname", "String", "\"light_omni2\"", cls),
                        prop("hammeruniqueid", "String", "\"1:2:3\"", id)};
    MapEntity prop2 = light;
    prop2.properties[1].raw.s = "prop_static";
    const std::string json = BuildLightsJson("x", {light, prop2});
    Check(json.find("\"identity\":\"maps/x/entities/default_ents.vents_c:hammer:1:2:3\"") != std::string::npos, "hammer identity");
    Check(json.find("\"brightness\":2.5") != std::string::npos, "authored brightness");
    Check(json.find("prop_static") == std::string::npos, "non-lighting entities are left out");
    bool threw = false;
    try { BuildLightsJson("x", {light, light}); } catch (const MapDataError &) { threw = true; }
    Check(threw, "ambiguous identities are rejected");
}

static void TestCommunityMapSources() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("source-mvm-community-map-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "game/csgo/maps");
    fs::create_directories(root / "game/csgo_community_addons/de_fixture");
    const auto write = [](const fs::path &path, const std::vector<uint8_t> &bytes) {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
    };
    const auto map = MakeVpk({{"maps/de_fixture/world.txt", "world"}});
    const auto sky = MakeVpk({{"maps/sky_fixture/world.txt", "sky"}});
    write(root / "game/csgo/maps/de_fixture.vpk", map);
    const fs::path assets = root / "game/csgo_community_addons/de_fixture/de_fixture_dir.vpk";
    write(assets, MakeVpk({{"materials/custom/wall.vmat_c", "opaque wall"},
                          {"maps/sky_fixture.vpk", std::string(sky.begin(), sky.end())}}));
    { std::ofstream gi(root / "game/csgo/gameinfo.gi"); gi << "FileSystem { SearchPaths { Game csgo } }"; }
    const auto numbered = assets.parent_path() / "de_fixture_000.vpk";
    write(numbered, {1, 2, 3});
    auto sources = OpenMapSources(root, "de_fixture");
    std::vector<uint8_t> bytes;
    Check(sources.resources.Read("materials/custom/wall.vmat", bytes) &&
          std::string(bytes.begin(), bytes.end()) == "opaque wall", "official map resolves community wall material");
    Check(std::find(sources.files.begin(), sources.files.end(), assets) != sources.files.end(),
          "community directory package participates in cache freshness");
    Check(std::find(sources.files.begin(), sources.files.end(), numbered) != sources.files.end(),
          "community numbered archive participates in cache freshness");
    const auto skyPackage = OpenReferencedPackage(sources, "maps/sky_fixture.vpk");
    Check(skyPackage && skyPackage->Read("maps/sky_fixture/world.txt", bytes),
          "community mount resolves referenced skybox package");
    // Remove only this test's individually named fixtures, never recursively delete temp.
    for (const auto &path : {root / "game/csgo/maps/de_fixture.vpk", assets, numbered, root / "game/csgo/gameinfo.gi"}) fs::remove(path);
    for (const auto &path : {assets.parent_path(), assets.parent_path().parent_path(), root / "game/csgo/maps", root / "game/csgo", root / "game", root}) fs::remove(path);
}

// Two Workshop items and a loose package can hold a map of the same name (Nightly Cache and Rainy
// Cache are both de_cache_n_r). Asked for one item, only that item is read, never the loose package
// or the newest item.
static void TestWorkshopItemSources() {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() /
        ("source-mvm-workshop-map-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path root = base / "common" / "cs2", workshop = base / "workshop" / "content" / "730";
    const auto write = [](const fs::path &path, const std::vector<uint8_t> &bytes) {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
    };
    const auto mapWith = [](const char *text) {
        const auto inner = MakeVpk({{"maps/de_twin/world.txt", text}});
        return MakeVpk({{"maps/de_twin.vpk", std::string(inner.begin(), inner.end())}});
    };
    write(root / "game/csgo/maps/de_twin.vpk", MakeVpk({{"maps/de_twin/world.txt", "loose"}}));
    write(workshop / "111" / "addon.vpk", mapWith("one"));
    write(workshop / "222" / "addon.vpk", mapWith("two"));
    fs::last_write_time(workshop / "222" / "addon.vpk", fs::last_write_time(workshop / "111" / "addon.vpk") + std::chrono::hours(1));
    { std::ofstream gi(root / "game/csgo/gameinfo.gi"); gi << "FileSystem { SearchPaths { Game csgo } }"; }
    const auto world = [](const MapSources &s) {
        std::vector<uint8_t> bytes;
        return s.package->Read("maps/de_twin/world.txt", bytes) ? std::string(bytes.begin(), bytes.end()) : std::string();
    };
    const auto byName = OpenMapSources(root, "de_twin");
    Check(world(byName) == "loose" && !byName.addon, "by name the loose package comes first");
    const auto first = OpenMapSources(root, "de_twin", "111");
    Check(world(first) == "one" && first.addon && first.addon->Path().parent_path().filename() == "111",
          "an asked-for item is read, not the loose package or the newer item");
    Check(world(OpenMapSources(root, "de_twin", "222")) == "two", "the other item reads its own map");
    bool missing = false, invalid = false;
    try { OpenMapSources(root, "de_twin", "333"); } catch (const MapDataError &) { missing = true; }
    try { OpenMapSources(root, "de_twin", "1x"); } catch (const MapDataError &) { invalid = true; }
    Check(missing && invalid, "a missing or malformed item is refused");
    std::error_code ec;
    fs::remove_all(base, ec);
}

// The compressed cache container: byte-exact round trips across block boundaries, incompressible
// blocks stored as they are, deterministic output, and damaged files refused.
static void TestCacheCodec() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "mvm-cache-codec-test";
    fs::create_directories(dir);
    auto roundTrip = [&](const std::vector<uint8_t> &raw, const char *what) {
        const auto packed = CompressCachePayload(raw);
        const fs::path file = dir / "payload.bin";
        { std::ofstream out(file, std::ios::binary | std::ios::trunc); out.write(reinterpret_cast<const char *>(packed.data()), std::streamsize(packed.size())); }
        std::string error;
        auto in = OpenCachePayload(file, error);
        std::vector<uint8_t> back;
        if (in) back.assign(std::istreambuf_iterator<char>(*in), std::istreambuf_iterator<char>());
        Check(in && back == raw, what);
        return packed;
    };
    roundTrip({}, "empty payload round-trips");
    // Mesh-like data (repeating floats with drift) over two and a half blocks.
    std::vector<uint8_t> mesh(kCacheCodecBlockSize * 2 + kCacheCodecBlockSize / 2);
    for (size_t i = 0; i < mesh.size() / 4; ++i) { const float f = float(i % 977) * 0.25f + float(i / 977); std::memcpy(&mesh[i * 4], &f, 4); }
    const auto packed = roundTrip(mesh, "multi-block payload round-trips");
    Check(packed.size() * 2 < mesh.size(), "structured payload compresses at least 2x");
    Check(CompressCachePayload(mesh) == packed, "compression is deterministic");
    // Noise does not shrink: stored raw, still exact.
    std::vector<uint8_t> noise(kCacheCodecBlockSize + 12345);
    uint64_t x = 0x9e3779b97f4a7c15ull;
    for (auto &b : noise) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; b = uint8_t(x); }
    const auto stored = roundTrip(noise, "incompressible payload round-trips");
    Check(stored.size() <= noise.size() + 64, "incompressible blocks are stored, not expanded");
    // A raw (uncompressed) file and a truncated container are refused.
    std::string error;
    { std::ofstream out(dir / "raw.bin", std::ios::binary | std::ios::trunc); out << "MVMOCC01 raw bytes"; }
    Check(!OpenCachePayload(dir / "raw.bin", error), "a raw payload is not accepted as a container");
    { std::ofstream out(dir / "cut.bin", std::ios::binary | std::ios::trunc); out.write(reinterpret_cast<const char *>(packed.data()), std::streamsize(packed.size() / 2)); }
    auto cut = OpenCachePayload(dir / "cut.bin", error);
    std::vector<uint8_t> partial;
    if (cut) partial.assign(std::istreambuf_iterator<char>(*cut), std::istreambuf_iterator<char>());
    Check(!cut || partial.size() < mesh.size(), "a truncated container never yields the full payload");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

int main() {
    TestStringToken();
    TestVpk();
    TestCommunityMapSources();
    TestWorkshopItemSources();
    TestKv3();
    TestOccluderBuilder();
    TestTexture();
    TestLightsJson();
    TestCacheCodec();
    std::printf(g_failures ? "%d FAILED\n" : "ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
