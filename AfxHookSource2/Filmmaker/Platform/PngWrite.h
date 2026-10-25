#pragma once

// A PNG encoder for pictures the hook draws itself and hands to Panorama over http (Panorama
// cannot show a loose image file, see LocalImageServer.h). RGBA8, filter 0, deflate STORED blocks:
// no compression, which costs nothing over loopback and needs no zlib. Engine-free; covered by
// Automation/tests/alt-environment-math-tests.cpp.

#include <array>
#include <cstdint>
#include <string>

namespace Filmmaker {

inline uint32_t PngCrc(const uint8_t *p, size_t n, uint32_t crc = 0xffffffffu) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return crc;
}

// `rgba` holds width * height * 4 bytes, rows top to bottom. Empty for a zero or oversized image.
inline std::string EncodePngRgba(int width, int height, const uint8_t *rgba) {
    if (width <= 0 || height <= 0 || !rgba || double(width) * height > 64.0e6) return std::string();
    auto be32 = [](std::string &s, uint32_t v) {
        for (int k = 3; k >= 0; --k) s.push_back(char((v >> (8 * k)) & 0xff));
    };
    std::string png("\x89PNG\r\n\x1a\n", 8);
    auto chunk = [&](const char *type, const std::string &data) {
        be32(png, uint32_t(data.size()));
        const size_t start = png.size();
        png.append(type, 4);
        png += data;
        be32(png, PngCrc(reinterpret_cast<const uint8_t *>(png.data() + start), png.size() - start) ^ 0xffffffffu);
    };
    std::string ihdr;
    be32(ihdr, uint32_t(width));
    be32(ihdr, uint32_t(height));
    ihdr += std::string("\x08\x06\x00\x00\x00", 5); // 8-bit RGBA, deflate, adaptive filtering, no interlace
    chunk("IHDR", ihdr);

    // zlib stream: header, stored blocks of at most 65535 bytes, Adler-32 of the raw rows.
    const size_t rowBytes = size_t(width) * 4 + 1;
    const size_t total = rowBytes * size_t(height);
    std::string z("\x78\x01", 2);
    z.reserve(total + total / 65535 * 5 + 16);
    uint32_t a = 1, b = 0;
    size_t written = 0;
    std::string block;
    auto flush = [&](bool last) {
        z.push_back(char(last ? 1 : 0));
        const uint16_t len = uint16_t(block.size()), nlen = uint16_t(~len);
        z.push_back(char(len & 0xff)), z.push_back(char(len >> 8));
        z.push_back(char(nlen & 0xff)), z.push_back(char(nlen >> 8));
        z += block;
        block.clear();
    };
    for (int y = 0; y < height; ++y)
        for (size_t x = 0; x < rowBytes; ++x) {
            const uint8_t v = x == 0 ? 0 : rgba[size_t(y) * size_t(width) * 4 + (x - 1)];
            a = (a + v) % 65521u;
            b = (b + a) % 65521u;
            block.push_back(char(v));
            ++written;
            if (block.size() == 65535) flush(written == total);
        }
    if (!block.empty() || written == 0) flush(true);
    be32(z, (b << 16) | a);
    chunk("IDAT", z);
    chunk("IEND", std::string());
    return png;
}

} // namespace Filmmaker
