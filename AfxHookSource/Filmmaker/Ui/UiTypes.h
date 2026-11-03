#pragma once

// Plain value types shared by the Source 1 UI layer: rectangles and colours. Engine-free.

#include <cstdint>

namespace SourceFilmmaker {
namespace Ui {

struct Rect {
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	float W() const { return x1 - x0; }
	float H() const { return y1 - y0; }
	bool Valid() const { return x1 > x0 && y1 > y0; }
	bool Contains(float x, float y) const { return Valid() && x >= x0 && x < x1 && y >= y0 && y < y1; }
};

inline Rect MakeRect(float x0, float y0, float x1, float y1) {
	Rect r;
	r.x0 = x0; r.y0 = y0; r.x1 = x1; r.y1 = y1;
	return r;
}

inline Rect Inset(const Rect& r, float dx, float dy) { return MakeRect(r.x0 + dx, r.y0 + dy, r.x1 - dx, r.y1 - dy); }

inline Rect Intersect(const Rect& a, const Rect& b) {
	return MakeRect(a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0,
		a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1);
}

// 8-bit RGBA, written the way VGUI scheme files write colours ("255 176 0 255").
struct Color {
	uint8_t r = 0, g = 0, b = 0, a = 255;
};

inline constexpr Color Rgba(int r, int g, int b, int a = 255) {
	return Color{ (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a };
}

// The colour with its alpha scaled by f (0..1), for fades and dimmed states.
inline Color Faded(Color c, float f) {
	if (f < 0) f = 0;
	if (f > 1) f = 1;
	c.a = (uint8_t)(c.a * f + 0.5f);
	return c;
}

// D3DCOLOR / 0xAARRGGBB, the vertex colour format the renderer uploads.
inline uint32_t PackArgb(Color c) {
	return ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
}

} // namespace Ui
} // namespace SourceFilmmaker
