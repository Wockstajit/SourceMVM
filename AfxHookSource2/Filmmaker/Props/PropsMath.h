#pragma once

// Engine-free half of PROPS (real CS2 models placed into a scene): reading the model catalog out of
// the game's VPK index, classifying and searching it, and naming entries for the UI. Tested by
// Automation/tests/props-math-tests.cpp.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace Filmmaker::Props {

// ---- VPK directory -------------------------------------------------------------------------------
// pak01_dir.vpk, version 2: a 28-byte header, then the directory tree as nested NUL-terminated
// strings -- extension, then path, then file name -- each list closed by an empty string, and
// after each file name an 18-byte entry (crc u32, preload u16, archive u16, offset u32, length u32,
// terminator u16) followed by `preload` inline bytes. Only the tree is read; no file data.
// Appends every "models/....vmdl" (the runtime name of a compiled .vmdl_c) to `out`.
// Every file in a version-2 VPK directory tree, as (extension, path, name). False on a malformed tree.
template <class Visit>
inline bool ForEachVpkEntry(const uint8_t* data, size_t size, Visit visit) {
	if (!data || size < 28) return false;
	uint32_t signature = 0, version = 0, treeSize = 0;
	std::memcpy(&signature, data, 4);
	std::memcpy(&version, data + 4, 4);
	std::memcpy(&treeSize, data + 8, 4);
	if (signature != 0x55aa1234u || version != 2) return false;
	size_t at = 28;
	const size_t end = std::min(size, size_t(28) + treeSize);
	auto readString = [&](std::string& s) -> bool {
		const size_t start = at;
		while (at < end && data[at]) ++at;
		if (at >= end) return false;
		s.assign(reinterpret_cast<const char*>(data + start), at - start);
		++at;
		return true;
	};
	std::string ext, path, name;
	while (readString(ext) && !ext.empty()) {
		while (readString(path) && !path.empty()) {
			while (readString(name) && !name.empty()) {
				if (at + 18 > end) return false;
				uint16_t preload = 0;
				std::memcpy(&preload, data + at + 4, 2);
				at += 18 + preload;
				visit(ext, path, name);
			}
		}
	}
	return true;
}

inline bool ParseVpkModelList(const uint8_t* data, size_t size, std::vector<std::string>& out) {
	return ForEachVpkEntry(data, size, [&](const std::string& ext, const std::string& path, const std::string& name) {
		if (ext == "vmdl_c" && path.compare(0, 7, "models/") == 0)
			out.push_back(path + "/" + name + ".vmdl");
	});
}

// Appends every "particles/....vpcf" (a compiled particle system) to `out`.
inline bool ParseVpkEffectList(const uint8_t* data, size_t size, std::vector<std::string>& out) {
	return ForEachVpkEntry(data, size, [&](const std::string& ext, const std::string& path, const std::string& name) {
		if (ext == "vpcf_c" && path.compare(0, 10, "particles/") == 0)
			out.push_back(path + "/" + name + ".vpcf");
	});
}

// Whether a VPK carries the map `mapName`: how a workshop map's package is found, since workshop
// packages are named by item ID, not by map. The workshop nests the map as its own VPK
// (maps/<mapName>.vpk; that inner VPK holds only the map's world and brush models, so the placeable
// ones are the package's own); a loose compiled map is maps/<mapName>.vmap_c.
inline bool VpkHasMap(const uint8_t* data, size_t size, const std::string& mapName) {
	bool found = false;
	ForEachVpkEntry(data, size, [&](const std::string& ext, const std::string& path, const std::string& name) {
		if (!found && path == "maps" && name == mapName && (ext == "vpk" || ext == "vmap_c")) found = true;
	});
	return found;
}

// ---- Catalog -------------------------------------------------------------------------------------
// Effects are particle systems (fire, embers, water streams, drips, steam), placed as
// info_particle_system entities; every other category lists models.
enum class Category { All = 0, Foliage, Props, Map, Effects };

inline std::string Lower(std::string s) {
	for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

inline bool IsEffect(const std::string& path) {
	return path.size() > 5 && Lower(path.substr(path.size() - 5)) == ".vpcf";
}

// Particle systems that are part of the gameplay pipeline, not world ambience: weapon, impact,
// blood and screen effects (they need a gun, a hit or a camera to make sense) and dev tests.
inline bool IsPlaceableEffect(const std::string& effect) {
	static const char* const kSkip[] = {
		"/weapons/", "weapon_fx", "muzzle", "tracer", "impact", "blood", "/ui/", "inventory",
		"/player/", "screen", "viewmodel", "_fp.", "_fps", "/dev/", "hud", "bullet", "shell",
		"/filmmaker/", "sticker", "spray", "c4_", "bumpmine", "taser", "healthshot"};
	const std::string m = Lower(effect);
	for (const char* skip : kSkip)
		if (m.find(skip) != std::string::npos) return false;
	return true;
}

// The control points an effect reads as NUMBERS, from its generated profile (PropsEffectProfiles.h,
// "cp:x,y,z;cp:x,y,z"): those get these values when it is placed, every other point the effect's
// position. A malformed entry is skipped.
struct EffectValue {
	int cp = 0;
	float value[3] = {0, 0, 0};
};
inline std::vector<EffectValue> ParseEffectValues(const char* text) {
	std::vector<EffectValue> out;
	if (!text) return out;
	const char* s = text;
	while (*s) {
		EffectValue v;
		char* end = nullptr;
		const long cp = std::strtol(s, &end, 10);
		bool ok = end != s && *end == ':' && cp > 0 && cp < 64;
		s = ok ? end + 1 : end;
		for (int c = 0; ok && c < 3; ++c) {
			v.value[c] = std::strtof(s, &end);
			ok = end != s;
			s = end;
			if (ok && c < 2) { ok = *s == ','; if (ok) ++s; }
		}
		if (ok) { v.cp = int(cp); out.push_back(v); }
		while (*s && *s != ';') ++s;
		if (*s == ';') ++s;
	}
	return out;
}

// Models that are not set dressing: the player and weapon pipeline, UI previews, debris gibs.
inline bool IsPlaceable(const std::string& model) {
	static const char* const kSkip[] = {
		"models/player/", "models/weapons/", "models/inventory_items/", "models/characters/",
		"models/chicken/", "models/hostage/", "models/ui/", "models/editor/", "models/dev/",
		"models/sticker", "_gib", "/gibs/", "models/tools/"};
	const std::string m = Lower(model);
	for (const char* skip : kSkip)
		if (m.find(skip) != std::string::npos) return false;
	return true;
}

// The words of a model path: "models/cs_italy/italy_signs_street_bixio.vmdl" -> models, cs, italy,
// italy, signs, street, bixio, vmdl. Digits stay in their word, so "de_dust2" gives "dust2". Matching on whole words (by prefix) is what keeps "tree" from
// finding every "street" sign, and "palm" from finding "napalm".
inline std::vector<std::string> Words(const std::string& lowerModel) {
	std::vector<std::string> words;
	std::string w;
	for (char c : lowerModel) {
		if (std::isalnum(static_cast<unsigned char>(c))) { w += c; continue; }
		if (!w.empty()) { words.push_back(w); w.clear(); }
	}
	if (!w.empty()) words.push_back(w);
	return words;
}

inline bool HasWordStarting(const std::vector<std::string>& words, const std::string& prefix) {
	for (const std::string& w : words)
		if (w.compare(0, prefix.size(), prefix) == 0) return true;
	return false;
}

inline bool IsFoliage(const std::string& lowerModel) {
	static const char* const kWords[] = {"foliage", "tree", "plant", "grass", "bush", "palm", "fern",
		"ivy", "flower", "leaf", "leaves", "shrub", "hedge", "cactus", "vine", "weed", "reed", "moss"};
	const auto words = Words(lowerModel);
	for (const char* w : kWords)
		if (HasWordStarting(words, w)) return true;
	return false;
}

// "de_mirage" -> "mirage": map-made assets live under models/<map>/ or models/props/de_<map>/.
inline std::string MapToken(const std::string& mapName) {
	std::string m = Lower(mapName);
	const size_t underscore = m.find('_');
	if (underscore != std::string::npos && underscore <= 3) m = m.substr(underscore + 1);
	return m;
}

inline bool Matches(const std::string& lowerModel, Category category, const std::string& mapToken) {
	switch (category) {
	case Category::Effects: return IsEffect(lowerModel);
	case Category::Foliage: return !IsEffect(lowerModel) && IsFoliage(lowerModel);
	case Category::Map: return !mapToken.empty() && lowerModel.find(mapToken) != std::string::npos;
	case Category::Props: return !IsEffect(lowerModel) && !IsFoliage(lowerModel);
	case Category::All:
	default: return true;
	}
}

// Every space-separated word of `query` must start one of the model path's words (case-insensitive).
// Generic words: a searched word also matches the names CS2 files that kind of thing under, so
// "wood" finds crates and pallets and "grass" finds weeds and reeds. Prefixes, like the query.
inline const std::vector<const char*>* RelatedWords(const std::string& w) {
	static const struct { const char* word; std::vector<const char*> related; } kTable[] = {
		{"wood", {"wood", "plank", "pallet", "crate", "barrel", "fence", "beam", "log", "door", "shelf", "table", "bench", "chair"}},
		{"tree", {"tree", "palm", "pine", "oak", "trunk", "cypress", "birch", "willow"}},
		{"grass", {"grass", "weed", "reed", "fern", "tuft", "lawn"}},
		{"plant", {"plant", "fern", "bush", "shrub", "flower", "ivy", "vine", "cactus", "grass", "weed"}},
		{"fire", {"fire", "flame", "burn", "burning", "ember", "embers", "torch", "candle", "fireplace", "inferno"}},
		{"water", {"water", "drip", "splash", "stream", "fountain", "puddle", "rain", "waterfall", "leak", "wet"}},
		{"smoke", {"smoke", "steam", "fog", "mist", "dust", "haze"}},
		{"bush", {"bush", "shrub", "hedge"}},
		{"flower", {"flower", "pot", "planter"}},
		{"rock", {"rock", "stone", "boulder", "cliff", "rubble", "pebble"}},
		{"stone", {"stone", "rock", "boulder", "brick", "rubble"}},
		{"metal", {"metal", "steel", "iron", "pipe", "rail", "sheet", "girder", "scaffold"}},
		{"car", {"car", "vehicle", "truck", "van", "bus", "taxi", "forklift", "trailer"}},
		{"vehicle", {"vehicle", "car", "truck", "van", "bus", "taxi", "forklift", "trailer", "boat"}},
		{"light", {"light", "lamp", "lantern", "bulb", "fixture", "chandelier", "floodlight", "sconce"}},
		{"lamp", {"lamp", "light", "lantern", "bulb", "fixture", "chandelier", "floodlight", "sconce"}},
		{"box", {"box", "crate", "carton", "container"}},
		{"crate", {"crate", "box", "pallet"}},
		{"sign", {"sign", "poster", "billboard", "banner", "graffiti"}},
		{"trash", {"trash", "garbage", "bin", "dumpster", "rubbish", "debris", "litter"}},
		{"water", {"water", "fountain", "well", "pool"}},
	};
	for (const auto& e : kTable)
		if (w == e.word) return &e.related;
	return nullptr;
}

// A typed map name ("dust2", "mirage", "inferno") also finds that map's prop folders, which CS2
// names without the number: the props of de_dust2 live under models/props/de_dust/.
inline std::string MapBaseWord(const std::string& q) {
	size_t end = q.size();
	while (end > 0 && std::isdigit(static_cast<unsigned char>(q[end - 1]))) --end;
	return end >= 3 && end < q.size() ? q.substr(0, end) : std::string();
}

inline bool WordMatches(const std::vector<std::string>& words, const std::string& q) {
	if (HasWordStarting(words, q)) return true;
	if (const std::string base = MapBaseWord(q); !base.empty() && HasWordStarting(words, base)) return true;
	if (const auto* related = RelatedWords(q))
		for (const char* r : *related)
			if (HasWordStarting(words, r)) return true;
	return false;
}

inline bool QueryMatches(const std::string& lowerModel, const std::string& query) {
	const auto words = Words(lowerModel);
	std::string q;
	for (size_t i = 0; i <= query.size(); ++i) {
		const char c = i < query.size() ? char(std::tolower(static_cast<unsigned char>(query[i]))) : ' ';
		if (c == ' ' || c == '_' || c == '/') {
			if (!q.empty() && !WordMatches(words, q)) return false;
			q.clear();
		} else {
			q += c;
		}
	}
	return true;
}

struct SearchResult {
	std::vector<int> indices; // into the catalog, in catalog order
	int total = 0;            // matches before paging
};

// `fromMap` (optional, parallel to the catalog) marks models that ship inside the current map's own
// package (a workshop map): they count as "this map" whatever their folder.
inline SearchResult Search(const std::vector<std::string>& catalog, const std::string& query,
                           Category category, const std::string& mapToken, int offset, int limit,
                           const std::vector<char>* fromMap = nullptr) {
	SearchResult result;
	for (int i = 0; i < int(catalog.size()); ++i) {
		const std::string m = Lower(catalog[i]);
		const bool own = fromMap && i < int(fromMap->size()) && (*fromMap)[i];
		if (!(own && category == Category::Map) && !Matches(m, category, mapToken)) continue;
		if (!QueryMatches(m, query)) continue;
		if (result.total >= offset && int(result.indices.size()) < limit) result.indices.push_back(i);
		++result.total;
	}
	return result;
}

// "models/anubis/foliage/palm_tree_01.vmdl" -> "palm tree 01"
inline std::string DisplayName(const std::string& model) {
	size_t slash = model.find_last_of('/');
	std::string name = slash == std::string::npos ? model : model.substr(slash + 1);
	size_t dot = name.rfind(".vmdl");
	if (dot == std::string::npos) dot = name.rfind(".vpcf");
	if (dot != std::string::npos) name.resize(dot);
	std::replace(name.begin(), name.end(), '_', ' ');
	return name;
}

// The map a model was made for, from its folders: "models/props/de_nuke/hr_nuke/..." -> "Nuke",
// "models/cs_italy/..." -> "Italy", "models/anubis/..." -> "Anubis"; "" for shared props
// (models/props/crates, models/generic). The first map folder wins.
inline std::string SourceMap(const std::string& model) {
	static const char* const kBare[] = {"anubis", "ancient", "mirage", "inferno", "nuke", "vertigo",
		"overpass", "train", "italy", "dust", "dust2", "cache", "aztec", "office", "baggage", "shoots",
		"vostok", "venice", "militia", "assault"};
	const std::string m = Lower(model);
	size_t start = m.find('/');
	while (start != std::string::npos) {
		const size_t end = m.find('/', start + 1);
		if (end == std::string::npos) break; // the file name is not a folder
		std::string seg = m.substr(start + 1, end - start - 1);
		std::string name;
		const size_t us = seg.find('_');
		const std::string prefix = us == std::string::npos ? "" : seg.substr(0, us);
		if (prefix == "de" || prefix == "cs" || prefix == "ar" || prefix == "dz" || prefix == "gd") {
			name = seg.substr(us + 1);
			const size_t more = name.find('_');
			if (more != std::string::npos) name.resize(more); // de_cache_s2 -> cache
		} else {
			for (const char* b : kBare)
				if (seg == b) name = seg;
		}
		if (!name.empty() && std::isalpha(static_cast<unsigned char>(name[0]))) {
			if (name == "dust2") return "Dust II";
			name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
			return name;
		}
		start = end;
	}
	return "";
}

// A map name as the UI shows it: "de_dust2" -> "Dust II", "workshop/123/de_dogtown" -> "Dogtown".
inline std::string MapLabel(const std::string& mapName) {
	std::string m = mapName;
	const size_t slash = m.find_last_of("/\\");
	if (slash != std::string::npos) m = m.substr(slash + 1);
	m = MapToken(m);
	if (m == "dust2") return "Dust II";
	if (!m.empty()) m[0] = char(std::toupper(static_cast<unsigned char>(m[0])));
	return m;
}

// "models/anubis/foliage/palm_tree_01.vmdl" -> "anubis/foliage"
inline std::string Folder(const std::string& model) {
	const size_t first = model.find('/');
	const size_t last = model.find_last_of('/');
	if (first == std::string::npos || last == std::string::npos || last <= first) return "";
	return model.substr(first + 1, last - first - 1);
}

} // namespace Filmmaker::Props
