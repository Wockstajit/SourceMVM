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

// Every mip of a plain 2D texture as stored, largest first, with block-compressed formats left
// compressed: for uploading straight to the GPU. Only DXT1, DXT5, BC7, ATI1N (BC4), RGBA8888 and
// BGRA8888; anything else, a cube, a volume or an array throws MapDataError.
struct TextureMips {
    enum class Format { BC1, BC3, BC4, BC7, RGBA8, BGRA8 };
    Format format = Format::RGBA8;
    int width = 0, height = 0;
    std::vector<std::vector<uint8_t>> mips; // [0] is the full size
    // Bytes of one row of 4 x 4 blocks (or of pixels, for the uncompressed formats) of a mip.
    int RowPitch(int level) const;
};
TextureMips ReadTextureMips(const std::vector<uint8_t> &compiledTexture);

// CS2's own surface textures the Environment coatings can draw with, read from an install's shared
// packages (never shipped): the Cobblestone winter snow, found through its material so a texture
// renamed by an update is still found, and the beads of water CS2's wet surfaces use.
struct GameSurfaceTextures {
    TextureMips snowColor, snowNormal, snowHeight, droplets;
    bool snow = false, drops = false;
    std::string status; // one line: what was found, or why not
};
GameSurfaceTextures ReadGameSurfaceTextures(const std::filesystem::path &cs2Root);

} // namespace Filmmaker::MapData
