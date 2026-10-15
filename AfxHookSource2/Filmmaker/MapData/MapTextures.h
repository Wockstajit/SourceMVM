#pragma once

// Compiled texture (.vtex_c) decoding to 8-bit RGBA, top mip only. Ported from
// ValveResourceFormat 20.0 (MIT) ResourceTypes/Texture.cs and TextureDecoders.

#include "MapDataIo.h"

#include <string>
#include <vector>

namespace Filmmaker::MapData {

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba; // width * height * 4, unpremultiplied
};

// Decodes the first face/slice of mip 0 at the texture's display size. Throws MapDataError
// for unsupported formats or malformed data.
Image DecodeTexture(const std::vector<uint8_t> &compiledTexture);

// Stored size of a compiled texture without decoding it.
struct TextureSize {
    int width = 0, height = 0, depth = 0;
};
TextureSize ReadTextureSize(const std::vector<uint8_t> &compiledTexture);

} // namespace Filmmaker::MapData
