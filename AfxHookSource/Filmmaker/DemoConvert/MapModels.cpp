#include "MapModels.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace SourceFilmmaker {
namespace DemoConvert {

namespace {

const int kLumpEntities = 0;
const int kLumpModels = 14;
const size_t kModelSize = 48; // dmodel_t: mins, maxs, origin, headnode, firstface, numfaces
// Collision bounds in a demo are the model's bounds widened by about one unit, and origins are sent
// in 1/32-unit steps, so a match allows a little slack.
const float kBoundsSlack = 3.0f;
const float kOriginSlack = 2.0f;
// A brush Valve reshaped or moved a little (de_inferno's elevator doors) still matches the nearest
// unclaimed brush whose centre is this close and whose size differs by at most this per axis.
const float kNearCentre = 48.0f;
const float kNearSize = 24.0f;
// A brush built around its own origin (doors, rotating and attached parts) draws wherever its
// entity is, so the same shape anywhere in the map is the right model: a door first seen open, or
// a part whose origin the demo sends relative to its parent. Its bounds sit around zero.
const float kLocalCentre = 64.0f;

bool ReadLump(std::ifstream& f, int lump, std::vector<char>& out, std::string& error) {
	f.seekg(8 + lump * 16);
	int32_t ofs = 0, len = 0;
	f.read((char*)&ofs, 4);
	f.read((char*)&len, 4);
	if (!f || ofs < 0 || len < 0) { error = "bad lump directory"; return false; }
	out.resize((size_t)len);
	f.seekg(ofs);
	f.read(out.data(), len);
	if (!f) { error = "lump runs past the file"; return false; }
	if (len >= 4 && std::memcmp(out.data(), "LZMA", 4) == 0) { error = "compressed lumps are not read"; return false; }
	return true;
}

// Brush entities in the entity lump: "model" "*N" with their "origin" and "classname".
void ReadEntities(const std::vector<char>& lump, MapModels& out) {
	const std::string text(lump.begin(), lump.end());
	size_t at = 0;
	while ((at = text.find('{', at)) != std::string::npos) {
		const size_t end = text.find('}', at);
		if (end == std::string::npos) break;
		std::string model, origin, className;
		size_t p = at + 1;
		while (p < end) {
			const size_t k0 = text.find('"', p);
			if (k0 == std::string::npos || k0 >= end) break;
			const size_t k1 = text.find('"', k0 + 1);
			const size_t v0 = text.find('"', k1 + 1);
			const size_t v1 = v0 == std::string::npos ? v0 : text.find('"', v0 + 1);
			if (k1 == std::string::npos || v1 == std::string::npos || v1 > end) break;
			const std::string key = text.substr(k0 + 1, k1 - k0 - 1), value = text.substr(v0 + 1, v1 - v0 - 1);
			if (key == "model") model = value;
			else if (key == "origin") origin = value;
			else if (key == "classname") className = value;
			p = v1 + 1;
		}
		if (model.size() > 1 && model[0] == '*') {
			const int n = std::atoi(model.c_str() + 1);
			if (n > 0 && n < (int)out.models.size()) {
				MapModel& m = out.models[(size_t)n];
				m.className = className;
				std::sscanf(origin.c_str(), "%f %f %f", &m.origin[0], &m.origin[1], &m.origin[2]);
			}
		}
		at = end + 1;
	}
}

float BoundsDistance(const BrushUse& use, const MapModel& model) {
	float worst = 0;
	for (int i = 0; i < 3; ++i) {
		worst = std::fmax(worst, std::fabs(use.mins[i] - model.mins[i]));
		worst = std::fmax(worst, std::fabs(use.maxs[i] - model.maxs[i]));
	}
	return worst;
}

float OriginDistance(const BrushUse& use, const MapModel& model) {
	float worst = 0;
	for (int i = 0; i < 3; ++i) worst = std::fmax(worst, std::fabs(use.origin[i] - model.origin[i]));
	return worst;
}

bool Matches(const BrushUse& use, const MapModel& model) {
	return BoundsDistance(use, model) <= kBoundsSlack && OriginDistance(use, model) <= kOriginSlack;
}

bool LocalGeometry(const float* mins, const float* maxs) {
	for (int i = 0; i < 3; ++i)
		if (std::fabs((mins[i] + maxs[i]) * 0.5f) > kLocalCentre) return false;
	return true;
}

// For brushes built around their origin: the same size (within the bounds slack), scored by the
// distance between where they are, so of several identical parts the closest is taken first.
float ShapeScore(const BrushUse& use, const MapModel& model) {
	if (!LocalGeometry(use.mins, use.maxs) || !LocalGeometry(model.mins, model.maxs)) return 1e9f;
	float d2 = 0;
	for (int i = 0; i < 3; ++i) {
		if (std::fabs((use.maxs[i] - use.mins[i]) - (model.maxs[i] - model.mins[i])) > 2 * kBoundsSlack) return 1e9f;
		d2 += (use.origin[i] - model.origin[i]) * (use.origin[i] - model.origin[i]);
	}
	return std::sqrt(d2);
}

// Distance between the world-space centres when every axis's size is close, else a huge number.
float NearScore(const BrushUse& use, const MapModel& model) {
	float d2 = 0;
	for (int i = 0; i < 3; ++i) {
		const float sizeUse = use.maxs[i] - use.mins[i], sizeModel = model.maxs[i] - model.mins[i];
		if (std::fabs(sizeUse - sizeModel) > kNearSize) return 1e9f;
		const float c = use.origin[i] + (use.mins[i] + use.maxs[i]) * 0.5f;
		const float m = model.origin[i] + (model.mins[i] + model.maxs[i]) * 0.5f;
		d2 += (c - m) * (c - m);
	}
	const float d = std::sqrt(d2);
	return d <= kNearCentre ? d : 1e9f;
}

} // namespace

bool ReadMapModels(const std::string& bspPath, MapModels& out, std::string& error) {
	out = MapModels();
	std::ifstream f(bspPath, std::ios::binary);
	if (!f) { error = "cannot open " + bspPath; return false; }
	char ident[4];
	int32_t version = 0;
	f.read(ident, 4);
	f.read((char*)&version, 4);
	if (!f || std::memcmp(ident, "VBSP", 4) != 0) { error = "not a VBSP file"; return false; }
	if (version < 19 || version > 21) { error = "BSP version " + std::to_string(version); return false; }
	std::vector<char> models, entities;
	if (!ReadLump(f, kLumpModels, models, error) || !ReadLump(f, kLumpEntities, entities, error)) return false;
	const size_t count = models.size() / kModelSize;
	out.models.resize(count);
	for (size_t i = 0; i < count; ++i) {
		float v[6];
		std::memcpy(v, models.data() + i * kModelSize, sizeof(v));
		for (int k = 0; k < 3; ++k) {
			out.models[i].mins[k] = v[k];
			out.models[i].maxs[k] = v[3 + k];
		}
	}
	ReadEntities(entities, out);
	return true;
}

InlineRemap MatchInlineModels(const std::vector<BrushUse>& uses, int oldCount, const MapModels& map) {
	InlineRemap remap;
	const int newCount = (int)map.models.size();
	remap.target.assign((size_t)(oldCount > 0 ? oldCount : 0), -1);
	std::vector<bool> taken((size_t)(newCount > 0 ? newCount : 0), false);
	std::vector<const BrushUse*> byNumber((size_t)remap.target.size(), nullptr);
	for (const BrushUse& use : uses)
		if (use.seen && use.number > 0 && use.number < oldCount) byNumber[(size_t)use.number] = &use;

	// Used models that still sit at their own number keep it.
	for (int n = 1; n < oldCount; ++n) {
		const BrushUse* use = byNumber[(size_t)n];
		if (use && n < newCount && Matches(*use, map.models[(size_t)n])) {
			remap.target[(size_t)n] = n;
			taken[(size_t)n] = true;
			++remap.kept;
		}
	}
	// The other used models take the closest unclaimed match anywhere in the map.
	for (int n = 1; n < oldCount; ++n) {
		const BrushUse* use = byNumber[(size_t)n];
		if (!use || remap.target[(size_t)n] >= 0) continue;
		int best = -1;
		float bestScore = 1e9f;
		for (int m = 1; m < newCount; ++m) {
			if (taken[(size_t)m] || !Matches(*use, map.models[(size_t)m])) continue;
			const float score = BoundsDistance(*use, map.models[(size_t)m]) + OriginDistance(*use, map.models[(size_t)m]);
			if (score < bestScore) { bestScore = score; best = m; }
		}
		if (best >= 0) {
			remap.target[(size_t)n] = best;
			taken[(size_t)best] = true;
			++remap.moved;
		}
	}
	// Then the nearest brush of about the same size, then (for brushes built around their origin)
	// the same shape anywhere. What is still unmatched the map no longer has.
	for (int pass = 0; pass < 2; ++pass) {
		for (int n = 1; n < oldCount; ++n) {
			const BrushUse* use = byNumber[(size_t)n];
			if (!use || remap.target[(size_t)n] >= 0) continue;
			int best = -1;
			float bestScore = 1e9f;
			for (int m = 1; m < newCount; ++m) {
				if (taken[(size_t)m]) continue;
				const float score = pass == 0 ? NearScore(*use, map.models[(size_t)m]) : ShapeScore(*use, map.models[(size_t)m]);
				if (score < bestScore) { bestScore = score; best = m; }
			}
			if (best >= 0) {
				remap.target[(size_t)n] = best;
				taken[(size_t)best] = true;
				++remap.moved;
			}
		}
	}
	for (int n = 1; n < oldCount; ++n)
		if (byNumber[(size_t)n] && remap.target[(size_t)n] < 0) ++remap.missing;
	char part[160];
	for (int n = 1; n < oldCount; ++n) {
		const BrushUse* use = byNumber[(size_t)n];
		if (!use) continue;
		const int t = remap.target[(size_t)n];
		if (t == n) continue;
		if (t > 0) std::snprintf(part, sizeof(part), " *%d->*%d", n, t);
		else std::snprintf(part, sizeof(part), " missing *%d (at %.0f %.0f %.0f, %.0f x %.0f x %.0f)", n,
			use->origin[0] + (use->mins[0] + use->maxs[0]) * 0.5f, use->origin[1] + (use->mins[1] + use->maxs[1]) * 0.5f,
			use->origin[2] + (use->mins[2] + use->maxs[2]) * 0.5f, use->maxs[0] - use->mins[0], use->maxs[1] - use->mins[1],
			use->maxs[2] - use->mins[2]);
		remap.detail += part;
	}
	// Models only precached keep their number when it is free, else take a free one; with none
	// left they stay unloaded. Nothing draws them either way.
	int nextFree = 1;
	for (int n = 1; n < oldCount; ++n) {
		if (byNumber[(size_t)n]) continue;
		++remap.unused;
		if (n < newCount && !taken[(size_t)n]) {
			remap.target[(size_t)n] = n;
			taken[(size_t)n] = true;
			continue;
		}
		while (nextFree < newCount && taken[(size_t)nextFree]) ++nextFree;
		if (nextFree < newCount) {
			remap.target[(size_t)n] = nextFree;
			taken[(size_t)nextFree] = true;
		}
	}
	return remap;
}

} // namespace DemoConvert
} // namespace SourceFilmmaker
