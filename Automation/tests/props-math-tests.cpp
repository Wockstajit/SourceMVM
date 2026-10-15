#include "../../AfxHookSource2/Filmmaker/Props/PropsMath.h"

#include <iostream>

using namespace Filmmaker::Props;

namespace {

int g_failed = 0;

#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

// A minimal version-2 VPK directory: header, then ext/path/name triples with 18-byte entries.
std::vector<uint8_t> MakeVpk(const std::vector<std::vector<std::string>>& tree, uint16_t preload = 0) {
	std::vector<uint8_t> body;
	auto str = [&](const std::string& s) { body.insert(body.end(), s.begin(), s.end()); body.push_back(0); };
	for (const auto& ext : tree) {
		str(ext[0]);           // extension
		str(ext[1]);           // path
		for (size_t i = 2; i < ext.size(); ++i) {
			str(ext[i]);
			uint8_t entry[18] = {};
			entry[4] = uint8_t(preload & 0xff); entry[5] = uint8_t(preload >> 8);
			entry[16] = 0xff; entry[17] = 0xff;
			body.insert(body.end(), entry, entry + 18);
			body.insert(body.end(), preload, 0xAB);
		}
		str("");               // end of names
		str("");               // end of paths
	}
	str("");                   // end of extensions
	std::vector<uint8_t> out(28, 0);
	const uint32_t sig = 0x55aa1234u, version = 2, treeSize = uint32_t(body.size());
	std::memcpy(out.data(), &sig, 4);
	std::memcpy(out.data() + 4, &version, 4);
	std::memcpy(out.data() + 8, &treeSize, 4);
	out.insert(out.end(), body.begin(), body.end());
	return out;
}

void TestVpkParse() {
	auto vpk = MakeVpk({
		{"vmdl_c", "models/anubis/foliage", "palm_tree_01", "ivy02a"},
		{"vtex_c", "materials/x", "ignored"},
		{"vmdl_c", "particles/whatever", "not_a_model_folder"},
	}, 3);
	std::vector<std::string> models;
	CHECK(ParseVpkModelList(vpk.data(), vpk.size(), models));
	CHECK(models.size() == 2);
	CHECK(models[0] == "models/anubis/foliage/palm_tree_01.vmdl");
	CHECK(models[1] == "models/anubis/foliage/ivy02a.vmdl");

	std::vector<std::string> none;
	std::vector<uint8_t> bad(vpk);
	bad[0] = 0;  // wrong signature
	CHECK(!ParseVpkModelList(bad.data(), bad.size(), none));
	CHECK(!ParseVpkModelList(vpk.data(), 10, none));
}

void TestCatalog() {
	const std::vector<std::string> catalog = {
		"models/anubis/foliage/palm_tree_01.vmdl",
		"models/props/de_mirage/towertop_d/towertop_d.vmdl",
		"models/cs_italy/foliage/fern/fern_plant_1.vmdl",
		"models/props_vehicles/car.vmdl",
		"models/mirage/foliage/bush_01.vmdl",
	};
	CHECK(Search(catalog, "", Category::All, "", 0, 100).total == 5);
	CHECK(Search(catalog, "", Category::Foliage, "", 0, 100).total == 3);
	CHECK(Search(catalog, "", Category::Props, "", 0, 100).total == 2);
	CHECK(Search(catalog, "", Category::Map, MapToken("de_mirage"), 0, 100).total == 2);
	// Every word must match, case-insensitively.
	CHECK(Search(catalog, "PALM tree", Category::All, "", 0, 100).total == 1);
	CHECK(Search(catalog, "palm car", Category::All, "", 0, 100).total == 0);
	// Whole words, not substrings: "tree" must not find a street sign, nor land it under Foliage.
	const std::vector<std::string> signs = {"models/cs_italy/italy_signs_street_bixio.vmdl",
		"models/cs_italy/foliage/tree/generic_tree_large_1.vmdl"};
	CHECK(Search(signs, "tree", Category::All, "", 0, 100).total == 1);
	CHECK(Search(signs, "", Category::Foliage, "", 0, 100).total == 1);
	CHECK(Search(signs, "gen tree", Category::All, "", 0, 100).total == 1); // prefixes
	// Paging keeps the total.
	// Generic words reach the names CS2 files things under, still by whole-word prefix.
	CHECK(QueryMatches("models/props/de_dust/hr_dust/dust_crates/dust_crate_style_01.vmdl", "wood"));
	CHECK(QueryMatches("models/props/de_inferno/wood_pallet_01.vmdl", "wood"));
	CHECK(QueryMatches("models/props/de_mirage/pine_tree.vmdl", "tree"));
	CHECK(QueryMatches("models/props/foliage/weed_clump_02.vmdl", "grass"));
	CHECK(QueryMatches("models/props/de_vertigo/boulder_03.vmdl", "rock"));
	CHECK(!QueryMatches("models/props/street_sign_01.vmdl", "tree"));        // still whole words
	CHECK(!QueryMatches("models/props/de_dust/dust_cart.vmdl", "grass"));
	CHECK(QueryMatches("models/props/foliage/weed_clump_02.vmdl", "weed"));   // plain words unchanged
	// A workshop package is found by the map it carries: nested as maps/<name>.vpk (as the workshop
	// ships them, with that VPK's location), or a loose compiled maps/<name>.vmap_c.
	{
		auto pkg = MakeVpk({{"vpk", "maps", "de_custom", "de_custom_skybox"}, {"vmdl_c", "models/myauthor", "lamp"}});
		CHECK(VpkHasMap(pkg.data(), pkg.size(), "de_custom"));
		CHECK(!VpkHasMap(pkg.data(), pkg.size(), "de_mirage"));
		CHECK(!VpkHasMap(pkg.data(), pkg.size(), "de_custom_sky"));
		auto loose = MakeVpk({{"vmap_c", "maps", "de_loose"}});
		CHECK(VpkHasMap(loose.data(), loose.size(), "de_loose"));
		std::vector<std::string> models;
		CHECK(ParseVpkModelList(pkg.data(), pkg.size(), models) && models.size() == 1 && models[0] == "models/myauthor/lamp.vmdl");
	}
	// Map names: the number stays in its word, and a map name also finds its prop folders.
	CHECK(QueryMatches("models/props/de_dust/hr_dust/dust_cart/dust_cart.vmdl", "dust2"));
	CHECK(QueryMatches("models/de_dust2/props/door.vmdl", "dust2"));
	CHECK(QueryMatches("models/props/de_dust/hr_dust/dust_cart/dust_cart.vmdl", "de dust2"));
	CHECK(!QueryMatches("models/props/de_inferno/bench.vmdl", "dust2"));
	CHECK(QueryMatches("models/props/foliage/palm_tree_01.vmdl", "tree 01"));
	CHECK(QueryMatches("models/cs_italy/italy_floodlight/floodlight.vmdl", "lamp"));
	// The map a model was made for, from its folders.
	CHECK(SourceMap("models/props/de_nuke/hr_nuke/nuke_fire_alert_light.vmdl") == "Nuke");
	CHECK(SourceMap("models/de_cache_s2/foliage/ch2_grass_detail_medium_001.vmdl") == "Cache");
	CHECK(SourceMap("models/cs_italy/italy_lamps/italy_lamp_01.vmdl") == "Italy");
	CHECK(SourceMap("models/anubis/foliage/palm_tree_01.vmdl") == "Anubis");
	CHECK(SourceMap("models/props/de_dust2/door.vmdl") == "Dust II");
	CHECK(SourceMap("models/props/crates/wooden_crate_64.vmdl").empty());
	CHECK(SourceMap("models/generic/barrel_kit_01/barrel_01_a.vmdl").empty());
	CHECK(SourceMap("models/de_mirage.vmdl").empty()); // a file name is not a folder
	// A workshop map's own models count as "this map" whatever their folder.
	{
		const std::vector<std::string> mixed = {"models/myauthor/crate_custom.vmdl", "models/props/de_inferno/bench.vmdl"};
		const std::vector<char> own = {1, 0};
		CHECK(Search(mixed, "", Category::Map, "inferno", 0, 10, &own).total == 2);
		CHECK(Search(mixed, "", Category::Map, "mirage", 0, 10, &own).total == 1);
		CHECK(Search(mixed, "", Category::Map, "mirage", 0, 10).total == 0);
	}
	auto page = Search(catalog, "", Category::All, "", 1, 2);
	CHECK(page.total == 5 && page.indices.size() == 2 && page.indices[0] == 1);

	CHECK(MapToken("de_mirage") == "mirage");
	CHECK(MapToken("cs_italy") == "italy");
	CHECK(MapToken("lobby_mapveto") == "lobby_mapveto");
	CHECK(IsPlaceable("models/anubis/foliage/palm_tree_01.vmdl"));
	CHECK(!IsPlaceable("models/weapons/v_rif_ak47.vmdl"));
	CHECK(!IsPlaceable("models/player/custom_player/x.vmdl"));
	CHECK(DisplayName("models/anubis/foliage/palm_tree_01.vmdl") == "palm tree 01");
	CHECK(Folder("models/anubis/foliage/palm_tree_01.vmdl") == "anubis/foliage");

	// Effects: particle systems are their own category, never in the model ones, named without
	// .vpcf, gameplay effects are kept out, and "fire"/"water" find what CS2 files them under.
	const std::vector<std::string> world = {
		"particles/burning_fx/env_fire_medium.vpcf", "particles/burning_fx/env_embers_small.vpcf",
		"particles/ambient/ambient_water_stream.vpcf", "particles/environment/water_drip_area_01.vpcf",
		"models/props/de_inferno/potted_plant1.vmdl", "models/props/crates/crate_01.vmdl"};
	CHECK(IsEffect("particles/burning_fx/env_fire_medium.vpcf") && !IsEffect("models/props/crates/crate_01.vmdl"));
	CHECK(Search(world, "", Category::Effects, "", 0, 100).total == 4);
	CHECK(Search(world, "", Category::Props, "", 0, 100).total == 1);
	CHECK(Search(world, "", Category::Foliage, "", 0, 100).total == 1);
	CHECK(Search(world, "fire", Category::Effects, "", 0, 100).total == 2); // fire + embers
	CHECK(Search(world, "water", Category::All, "", 0, 100).total == 2);    // stream + drip
	CHECK(DisplayName("particles/burning_fx/env_fire_medium.vpcf") == "env fire medium");
	CHECK(IsPlaceableEffect("particles/burning_fx/env_fire_medium.vpcf"));
	CHECK(!IsPlaceableEffect("particles/weapons/cs_weapon_fx/weapon_muzzle_flash_assaultrifle.vpcf"));
	CHECK(!IsPlaceableEffect("particles/blood_impact/blood_impact_heavy.vpcf"));
	CHECK(!IsPlaceableEffect("particles/filmmaker/modern/mw2019_tracer/mw2019_tracer.vpcf"));

	// An effect's number control points: molotov_fire01 burns for 10 x CP3.x (0 = forever).
	const auto values = ParseEffectValues("3:0,0,0;1:4,-2.5,1");
	CHECK(values.size() == 2);
	CHECK(values[0].cp == 3 && values[0].value[0] == 0.f);
	CHECK(values[1].cp == 1 && values[1].value[0] == 4.f && values[1].value[1] == -2.5f && values[1].value[2] == 1.f);
	CHECK(ParseEffectValues("").empty() && ParseEffectValues(nullptr).empty());
	// Malformed entries are skipped, the rest still read; CP0 is the effect's position, never a number.
	const auto mixed = ParseEffectValues("0:1,1,1;x:1;2:1,2;5:7,8,9");
	CHECK(mixed.size() == 1 && mixed[0].cp == 5 && mixed[0].value[2] == 9.f);
}

} // namespace

int main() {
	TestVpkParse();
	TestCatalog();
	if (g_failed) { std::cerr << g_failed << " check(s) failed\n"; return 1; }
	std::cout << "props math tests passed\n";
	return 0;
}
