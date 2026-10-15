#pragma once

// Minimal OpenEXR writer: single-part scanline, uncompressed, three HALF channels (R,G,B).
//
// WHY HAND-ROLLED. The render system needs float HDR output (pre-tonemap scene captures) and
// the repo policy avoids new dependencies; an uncompressed scanline EXR is a small, fully
// documented container (magic, attribute list, offset table, raw half rows) that every
// compositor reads. Compression can be added later without changing callers.
//
// Input is the D3D-mapped layout: rows of RGBA 16-bit halfs (8 bytes per pixel), `rowPitchBytes`
// apart. Alpha is dropped. Rows are written top-down (INCREASING_Y line order).

#include <cstdint>

namespace Filmmaker {

// Writes `path` (UTF-16, overwritten). Returns false on any I/O failure.
bool ExrWriteHalfRgba(const wchar_t* path, const uint16_t* rgbaHalf, int width, int height,
	int rowPitchBytes);

} // namespace Filmmaker
