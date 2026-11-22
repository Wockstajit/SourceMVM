#pragma once

// World-Space Decals & Wall Media controller:
// Text, Logos/Images, Videos on walls, Arrows, and Animated Painted Routes projected onto
// walls and floors, optionally attached to any Scene Object.

#include "WorldDecalsMath.h"

#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker::WorldDecals {

struct TextureBitmap {
	uint32_t width = 0;
	uint32_t height = 0;
	uint64_t revision = 0;
	std::vector<uint8_t> rgba;
};

struct RenderItem {
	Decal decal;
	std::shared_ptr<const TextureBitmap> texture;
};

void RunMainThreadFrame();
void OnLevelInit();
void Shutdown();

bool AnyEnabled();
std::vector<RenderItem> RenderSnapshot();

void AppendStateJson(std::ostringstream& out);
void RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker::WorldDecals
