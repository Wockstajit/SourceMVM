#pragma once

// What the in-world scene overlay draws (Docs/features/scene-object-rigs.md, "In the world"):
// the placed cameras and lights of the Environment scene, and the path each keyed one takes.
// Built on the main thread by Filmmaker/Movie/SceneRig.cpp and handed to CCampathDrawer whole
// (CameraBridge_SetSceneOverlay), which draws it next to the camera path as solid markers.
// Plain data and no Direct3D or SDK types, so both sides can include it.

#include <array>
#include <memory>
#include <vector>

namespace Filmmaker {

struct SceneOverlayColour { unsigned char r = 255, g = 255, b = 255, a = 255; };

struct SceneOverlay {
	enum class GlyphKind { Camera, PointLight, SpotLight, AreaLight };
	// A keyed object's world positions over its keys, drawn as one line with a dot on each key.
	struct Path {
		std::vector<std::array<float, 3>> points;
		std::vector<std::array<float, 3>> keys;
		SceneOverlayColour colour; // its kind's colour (SceneMarkerMath.h)
		bool selected = false;
	};
	// Where an object is now: a camera body with its lens and field of view; a bulb (point), a lamp
	// with its beam (spot) or a lit panel (area), each in its kind's colour, lit in the light's own.
	struct Glyph {
		GlyphKind kind = GlyphKind::PointLight;
		float pos[3] = {};
		float ang[3] = {}; // pitch, yaw, roll in degrees
		float fov = 90.0f;
		SceneOverlayColour colour; // its kind's colour
		SceneOverlayColour emit;   // a light's own colour
		float cone = 45.0f;        // a spot light's cone, full angle in degrees
		float width = 64.0f, height = 64.0f; // an area light's panel
		bool selected = false;
	};
	std::vector<Path> paths;
	std::vector<Glyph> glyphs;
	bool Empty() const { return paths.empty() && glyphs.empty(); }
};

} // namespace Filmmaker
