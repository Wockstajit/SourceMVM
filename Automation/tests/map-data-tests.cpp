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

// Binary KV3 v4 (uncompressed) with the node types and rules of ValveResourceFormat 21.1: constant
// uint32 nodes (26, 27, written by CS2 since the 2026-11-20 update), int8 (22), uint8 (23), a type
// byte with bit 6 set (one skipped byte follows), an out-of-range name id and a repeated name.
static void TestKv3BinaryV4Nodes() {
    Bytes buffer;
    buffer.Put<uint8_t>(0xFF);           // bytes1: int8 -1
    buffer.Put<uint8_t>(200);            // bytes1: uint8 200
    for (int k = 0; k < 2; ++k) buffer.Put<uint8_t>(0); // align to 4
    for (int32_t v : {3, 5, 0, 1, 2, 99, 1}) buffer.Put<int32_t>(v); // string count, object length, member names
    buffer.Str("one");
    buffer.Str("zero");
    buffer.Str("small");
    const size_t stringBytes = 4 + 5 + 6;
    const uint8_t types[] = {9, 27 | 0x40, 0x00, 26, 22, 23, 27};
    buffer.Raw(types, sizeof types);
    buffer.Put<uint32_t>(0xFFEEDD00u);
    Bytes b;
    b.Put<uint32_t>(0x4B563304u);
    for (int k = 0; k < 16; ++k) b.Put<uint8_t>(0); // format GUID
    b.Put<uint32_t>(0);                  // uncompressed
    b.Put<uint16_t>(0);                  // dictionary
    b.Put<uint16_t>(0);                  // frame size
    b.Put<int32_t>(2);                   // bytes1
    b.Put<int32_t>(7);                   // bytes4
    b.Put<int32_t>(0);                   // bytes8
    b.Put<int32_t>(int32_t(stringBytes + sizeof types));
    b.Put<uint16_t>(1);                  // objects
    b.Put<uint16_t>(0);                  // arrays
    b.Put<int32_t>(int32_t(buffer.b.size()));
    b.Put<int32_t>(int32_t(buffer.b.size()));
    b.Put<int32_t>(0);                   // blocks
    b.Put<int32_t>(0);                   // blob bytes
    b.Put<int32_t>(0);                   // bytes2
    b.Put<int32_t>(0);                   // block compressed sizes
    b.Raw(buffer.b.data(), buffer.b.size());
    const Kv3 v = DecodeKv3(b.b.data(), b.b.size());
    Check(v.type == Kv3::Type::Object && v.Size() == 4, "KV3 v4 object decodes; a repeated name replaces its value");
    Check(v["one"].type == Kv3::Type::UInt && v["one"].width == Kv3::Width::UInt32 && v["one"].Integer() == 1,
          "KV3 node 27 is uint32 one, after a type byte with bit 6 and its skipped byte");
    Check(v["zero"].type == Kv3::Type::UInt && v["zero"].Integer() == 1, "the repeated member keeps the last value");
    Check(v["small"].type == Kv3::Type::Int && v["small"].Integer() == -1, "KV3 node 22 is a signed byte");
    Check(v[""].type == Kv3::Type::UInt && v[""].Integer() == 200, "KV3 node 23 is an unsigned byte; an unknown name id is empty");
    Check(v.obj.size() > 1 && v.obj[1].first == "zero" && v.obj[1].second.Integer(7) == 1, "a replaced member keeps its position");
    Bytes z3 = b;
    z3.b[20] = 4;                        // compression method z3
    bool threw = false;
    try { DecodeKv3(z3.b.data(), z3.b.size()); } catch (const MapDataError &e) { threw = std::string(e.what()).find("z3") != std::string::npos; }
    Check(threw, "z3 compressed KV3 is reported as z3");
}

// Empty resource blocks keep their slot, so block indices (m_nBlockIndex, data_block) match the file.
static void TestResourceEmptyBlocks() {
    Bytes b;
    b.Put<uint32_t>(44);                 // declared size
    b.Put<uint16_t>(12);                 // header size
    b.Put<uint16_t>(0);                  // version
    b.Put<uint32_t>(8);                  // block table, relative to this field (offset 8 -> 16)
    b.Put<uint32_t>(2);                  // block count
    b.Raw("MBUF", 4);                    // block 0: empty
    b.Put<uint32_t>(0);
    b.Put<uint32_t>(0);
    b.Raw("DATA", 4);                    // block 1: 4 bytes at 40 (field at 32)
    b.Put<uint32_t>(8);
    b.Put<uint32_t>(4);
    b.Put<uint32_t>(0xDEADBEEFu);
    const Resource r(std::vector<uint8_t>(b.b));
    Check(r.Blocks().size() == 2 && r.Blocks()[1].size == 4 && r.Blocks()[1].offset == 40, "empty block keeps its index");
    Check(r.Block("MBUF") == nullptr && r.BlocksOfType("MBUF").empty(), "type lookups skip empty blocks");
    Check(r.Block("DATA") == &r.Blocks()[1], "type lookup finds the block after an empty one");
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

// A 4 x 4 RGBA8888 texture with three mips, stored smallest first as compiled textures are.
static std::vector<uint8_t> MakeMippedTexture(uint16_t flags) {
    Bytes r;
    r.Put<uint32_t>(0);
    r.Put<uint16_t>(12);
    r.Put<uint16_t>(0);
    r.Put<uint32_t>(8);
    r.Put<uint32_t>(1);
    r.Raw("DATA", 4);
    r.Put<uint32_t>(8);
    r.Put<uint32_t>(40);
    r.Put<uint16_t>(1);
    r.Put<uint16_t>(flags);
    for (int k = 0; k < 4; ++k) r.Put<float>(0);
    r.Put<uint16_t>(4);
    r.Put<uint16_t>(4);
    r.Put<uint16_t>(1);
    r.Put<uint8_t>(4); // RGBA8888
    r.Put<uint8_t>(3);
    r.Put<uint32_t>(0);
    r.Put<uint32_t>(8);
    r.Put<uint32_t>(0);
    for (int level = 2; level >= 0; --level)
        for (int p = 0; p < (16 >> (2 * level)); ++p) {
            const uint8_t texel[4] = {uint8_t(level * 100), uint8_t(p), 7, 255};
            r.Raw(texel, 4);
        }
    return r.b;
}

static void TestTextureMips() {
    const TextureMips mips = ReadTextureMips(MakeMippedTexture(0));
    Check(mips.width == 4 && mips.height == 4 && mips.format == TextureMips::Format::RGBA8, "mipped texture size and format");
    Check(mips.mips.size() == 3 && mips.mips[0].size() == 64 && mips.mips[1].size() == 16 && mips.mips[2].size() == 4,
          "every mip is read, largest first");
    Check(mips.mips[0][0] == 0 && mips.mips[0][4 * 15 + 1] == 15 && mips.mips[1][0] == 100 && mips.mips[2][0] == 200,
          "each mip holds its own texels");
    Check(mips.RowPitch(0) == 16 && mips.RowPitch(2) == 4, "uncompressed row pitch");
    bool threw = false;
    try { ReadTextureMips(MakeMippedTexture(1 << 4)); } catch (const MapDataError &) { threw = true; }
    Check(threw, "a cube texture is not read as a plain 2D texture");
}

// Against the installed game, when this machine has one: the snow and the beads of water the
// Environment coatings draw with are found and read with all their mips.
static void TestGameSurfaceTextures() {
    const auto root = ResolveCs2Root();
    if (root.empty()) {
        std::printf("SKIP game surface textures (no Counter-Strike 2 install)\n");
        return;
    }
    const GameSurfaceTextures t = ReadGameSurfaceTextures(root);
    std::printf("game surface textures: %s\n", t.status.c_str());
    Check(t.snow && t.snowColor.width >= 256 && t.snowColor.mips.size() > 1 && t.snowNormal.mips.size() > 1 && t.snowHeight.mips.size() > 1,
          "CS2 snow textures read with their mips");
    Check(t.snow && t.snowColor.mips[0].size() == size_t(t.snowColor.RowPitch(0)) * size_t((t.snowColor.height + 3) / 4),
          "CS2 snow colour mip 0 is whole");
    Check(t.drops && t.droplets.width > 0 && !t.droplets.mips.empty(), "CS2 water beads read");
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
    TestKv3BinaryV4Nodes();
    TestResourceEmptyBlocks();
    TestOccluderBuilder();
    TestTexture();
    TestTextureMips();
    TestGameSurfaceTextures();
    TestLightsJson();
    TestCacheCodec();
    std::printf(g_failures ? "%d FAILED\n" : "ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
