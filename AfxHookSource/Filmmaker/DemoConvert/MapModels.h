#pragma once

// The installed map's brush models ("*1", "*2", ...) and the matching that lets an old demo use
// them. Valve rebuilt the CS:S maps after the v34 engine, so an old demo's "*N" may now be another
// number (de_inferno's doors and portal windows moved) or gone (de_dust2's smoke volume): loading a
// number the map does not have stops the game with "bad inline model number". Each brush entity
// in the demo is matched to the installed model with the same position and size instead.
// Engine-free.

#include <string>
#include <vector>

namespace SourceFilmmaker {
namespace DemoConvert {

struct MapModel {
	float mins[3] = { 0, 0, 0 };
	float maxs[3] = { 0, 0, 0 };
	float origin[3] = { 0, 0, 0 }; // from the entity that uses the model, 0 when it has none
	std::string className;
};

struct MapModels {
	std::vector<MapModel> models; // index = inline model number; 0 is the world
};

// Reads the model lump and the entity lump of a BSP (versions 19 to 21).
bool ReadMapModels(const std::string& bspPath, MapModels& out, std::string& error);

// What the demo shows of the entity that uses inline model "*N".
struct BrushUse {
	int number = -1;         // N
	float origin[3] = { 0, 0, 0 };
	float mins[3] = { 0, 0, 0 };
	float maxs[3] = { 0, 0, 0 };
	bool seen = false;        // an entity used it; otherwise only precached
};

struct InlineRemap {
	std::vector<int> target;  // by old N: the installed model to load, -1 when there is none
	int kept = 0, moved = 0, missing = 0, unused = 0;
	std::string detail; // "*14->*33 *34->*32 ... missing *16 (at x y z, size x y z)", for logs
};

// Matches old inline models to installed ones. A used model keeps its number when the installed
// model there has the same bounds and origin (within a couple of units), otherwise takes the best
// unclaimed match. Models no entity used keep their number if it is free, else take a free one.
// oldCount is the number of inline models the demo precaches (highest N + 1).
InlineRemap MatchInlineModels(const std::vector<BrushUse>& uses, int oldCount, const MapModels& map);

} // namespace DemoConvert
} // namespace SourceFilmmaker
