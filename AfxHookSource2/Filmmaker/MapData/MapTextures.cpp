// Ported from ValveResourceFormat 20.0 (MIT): ResourceTypes/Texture.cs, TextureDecoders/*.
#include "MapTextures.h"

#define BCDEC_IMPLEMENTATION
#include "../../../Deps/release/bcdec/bcdec.h"
#include "../../../Deps/release/lz4/lz4.h"

#include <algorithm>
#include <cstring>

namespace Filmmaker::MapData {
namespace {

enum Format : uint8_t {
    DXT1 = 1, DXT5 = 2, I8 = 3, RGBA8888 = 4, BC6H = 19, BC7 = 20, ATI2N = 21, IA88 = 22, ATI1N = 27, BGRA8888 = 28,
};
constexpr uint16_t kCube = 1 << 4, kVolume = 1 << 5;

int BlockSize(uint8_t format) {
    switch (format) {
    case DXT1: case ATI1N: case 23 /*ETC2*/: return 8;
    case DXT5: case BC6H: case BC7: case 24 /*ETC2_EAC*/: case 14: return 16;
    case RGBA8888: case BGRA8888: case 6: case 9: case 11: return 4;
    case 5: case 8: case IA88: return 2;
    case 7: case 10: case 12: return 8;
    case 13: return 12;
    default: return 1; // includes ATI2N, as the reference implementation does
    }
}

bool BlockCompressed(uint8_t f) { return f == DXT1 || f == DXT5 || f == BC6H || f == BC7 || f == 23 || f == 24 || f == ATI1N; }

int MipSize(int size, int level) { return std::max(size >> level, 1); }

struct Header {
    uint16_t flags = 0;
    int width = 0, height = 0, depth = 0, displayWidth = 0, displayHeight = 0;
    uint8_t format = 0, mips = 0;
    bool compressedMips = false;
    std::vector<int32_t> compressedMipSizes;

    int BufferSize(int level) const {
        int w = MipSize(width, level), h = MipSize(height, level);
        int d = (flags & kVolume) ? MipSize(depth, level) : depth;
        if (flags & kCube) d *= 6;
        if (BlockCompressed(format)) {
            if (w % 4) w += 4 - w % 4;
            if (h % 4) h += 4 - h % 4;
            if (d < 4 && d > 1) d = 4;
            return ((w * h) >> 4) * d * BlockSize(format);
        }
        return w * h * d * BlockSize(format);
    }
    int ActualWidth() const { return displayWidth > 0 && (displayWidth != 1 || width == 4) ? displayWidth : width; }
    int ActualHeight() const { return displayHeight > 0 && (displayHeight != 1 || height == 4) ? displayHeight : height; }
};

template <class T> T Read(const std::vector<uint8_t> &b, size_t at) {
    if (at > b.size() || sizeof(T) > b.size() - at) throw MapDataError("truncated texture");
    T v;
    std::memcpy(&v, b.data() + at, sizeof(T));
    return v;
}

Header ParseHeader(const std::vector<uint8_t> &b, size_t at) {
    Header h;
    if (Read<uint16_t>(b, at) != 1) throw MapDataError("unknown texture version");
    h.flags = Read<uint16_t>(b, at + 2);
    h.width = Read<uint16_t>(b, at + 20);
    h.height = Read<uint16_t>(b, at + 22);
    h.depth = Read<uint16_t>(b, at + 24);
    h.format = Read<uint8_t>(b, at + 26);
    h.mips = Read<uint8_t>(b, at + 27);
    const size_t extraField = at + 32;
    const uint32_t extraOffset = Read<uint32_t>(b, extraField), extraCount = Read<uint32_t>(b, extraField + 4);
    size_t entry = extraField + extraOffset;
    for (uint32_t k = 0; k < extraCount && k < 64; ++k, entry += 12) {
        const uint32_t type = Read<uint32_t>(b, entry);
        const size_t data = entry + 4 + Read<uint32_t>(b, entry + 4);
        if (type == 3) { // METADATA
            const int w = Read<uint16_t>(b, data + 2), hh = Read<uint16_t>(b, data + 4);
            if (w > 0 && hh > 0 && h.width >= w && h.height >= hh) {
                h.displayWidth = w;
                h.displayHeight = hh;
            }
        } else if (type == 4) { // COMPRESSED_MIP_SIZE
            const uint32_t flag = Read<uint32_t>(b, data);
            const size_t mipsField = data + 4;
            const uint32_t count = Read<uint32_t>(b, data + 8);
            if (flag > 1 || count > 32) throw MapDataError("invalid compressed mip table");
            h.compressedMips = flag == 1;
            for (uint32_t m = 0; m < count; ++m) h.compressedMipSizes.push_back(Read<int32_t>(b, mipsField + Read<uint32_t>(b, mipsField) + m * 4));
        }
    }
    return h;
}

} // namespace

TextureSize ReadTextureSize(const std::vector<uint8_t> &bytes) {
    const Resource resource(bytes);
    const ResourceBlock *data = resource.Block("DATA");
    if (!data) throw MapDataError("texture has no DATA block");
    const Header h = ParseHeader(bytes, data->offset);
    return {h.width, h.height, h.depth};
}

Image DecodeTexture(const std::vector<uint8_t> &bytes) {
    const Resource resource(bytes);
    const ResourceBlock *data = resource.Block("DATA");
    if (!data) throw MapDataError("texture has no DATA block");
    const Header h = ParseHeader(bytes, data->offset);
    if (h.width <= 0 || h.height <= 0 || h.width > 16384 || h.height > 16384 || h.mips == 0) throw MapDataError("invalid texture size");

    Image image;
    image.width = h.ActualWidth();
    image.height = h.ActualHeight();
    image.rgba.assign(size_t(image.width) * image.height * 4, 0);

    size_t at = size_t(data->offset) + data->size;
    for (int level = h.mips - 1; level > 0; --level) {
        int size = h.BufferSize(level);
        if (!h.compressedMipSizes.empty() && size_t(level) < h.compressedMipSizes.size()) size = std::min(size, h.compressedMipSizes[size_t(level)]);
        at += size_t(size);
    }
    const int size = h.BufferSize(0);
    std::vector<uint8_t> mip(static_cast<size_t>(size));
    const int stored = h.compressedMips && !h.compressedMipSizes.empty() ? h.compressedMipSizes[0] : size;
    if (at > bytes.size() || size_t(std::min(stored, size)) > bytes.size() - at) throw MapDataError("truncated texture data");
    if (stored >= size) {
        std::memcpy(mip.data(), bytes.data() + at, size_t(size));
    } else {
        const int written = LZ4_decompress_safe(reinterpret_cast<const char *>(bytes.data() + at), reinterpret_cast<char *>(mip.data()), stored, size);
        if (written != size) throw MapDataError("texture LZ4 decompression failed");
    }

    const int w = image.width, hgt = image.height, bw = h.width, bh = h.height;
    uint8_t *out = image.rgba.data();
    const size_t pixels = size_t(w) * hgt;
    switch (h.format) {
    case DXT1: case DXT5: case BC7: case ATI1N: case ATI2N: {
        const int blockBytes = h.format == DXT1 || h.format == ATI1N ? 8 : 16;
        const int bx = (bw + 3) / 4, by = (bh + 3) / 4;
        if (size_t(bx) * by * blockBytes > mip.size()) throw MapDataError("truncated texture blocks");
        uint8_t block[4 * 4 * 4];
        for (int y = 0; y < by; ++y)
            for (int x = 0; x < bx; ++x) {
                const uint8_t *src = mip.data() + (size_t(y) * bx + x) * blockBytes;
                std::memset(block, 0, sizeof block);
                switch (h.format) {
                case DXT1:
                    bcdec_bc1(src, block, 16);
                    for (int p = 0; p < 16; ++p) block[p * 4 + 3] = 255;
                    break;
                case DXT5: bcdec_bc3(src, block, 16); break;
                case BC7: bcdec_bc7(src, block, 16); break;
                case ATI1N: {
                    uint8_t r[16];
                    bcdec_bc4(src, r, 4);
                    for (int p = 0; p < 16; ++p) { block[p * 4] = r[p]; block[p * 4 + 3] = 255; }
                    break;
                }
                default: {
                    uint8_t rg[32];
                    bcdec_bc5(src, rg, 8);
                    for (int p = 0; p < 16; ++p) { block[p * 4] = rg[p * 2]; block[p * 4 + 1] = rg[p * 2 + 1]; block[p * 4 + 3] = 255; }
                    break;
                }
                }
                for (int py = 0; py < 4; ++py)
                    for (int px = 0; px < 4; ++px) {
                        const int ox = x * 4 + px, oy = y * 4 + py;
                        if (ox >= w || oy >= hgt || ox >= bw || oy >= bh) continue;
                        std::memcpy(out + (size_t(oy) * w + ox) * 4, block + (py * 4 + px) * 4, 4);
                    }
            }
        break;
    }
    case RGBA8888: case BGRA8888:
        if (mip.size() < pixels * 4) throw MapDataError("truncated texture pixels");
        for (size_t p = 0; p < pixels; ++p) {
            const uint8_t *s = mip.data() + p * 4;
            uint8_t *d = out + p * 4;
            if (h.format == RGBA8888) std::memcpy(d, s, 4);
            else { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; }
        }
        break;
    case I8: case IA88: {
        const size_t stride = h.format == I8 ? 1 : 2;
        if (mip.size() < pixels * stride) throw MapDataError("truncated texture pixels");
        for (size_t p = 0; p < pixels; ++p) {
            const uint8_t i = mip[p * stride];
            out[p * 4] = out[p * 4 + 1] = out[p * 4 + 2] = i;
            out[p * 4 + 3] = h.format == I8 ? 255 : mip[p * stride + 1];
        }
        break;
    }
    default:
        throw MapDataError("unsupported texture format " + std::to_string(h.format));
    }
    return image;
}

} // namespace Filmmaker::MapData
