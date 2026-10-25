#pragma once

// Snappy raw-format decompression (no framing), the compression CS2 demo frames use when their
// command carries DEM_IsCompressed. Engine-free; covered by Automation/tests/demo-world-swap-tests.cpp.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Filmmaker {

// Decodes `size` bytes at `src` into `out`. False on malformed or truncated input; `out` is then
// unspecified.
inline bool SnappyDecode(const uint8_t *src, size_t size, std::vector<uint8_t> &out) {
    size_t i = 0;
    uint64_t length = 0;
    for (int shift = 0;; shift += 7) {
        if (i >= size || shift > 35) return false;
        const uint8_t c = src[i++];
        length |= uint64_t(c & 0x7f) << shift;
        if (!(c & 0x80)) break;
    }
    if (length > (1ull << 31)) return false;
    out.resize(size_t(length));
    size_t o = 0;
    while (i < size) {
        const uint8_t tag = src[i++];
        size_t len = 0, offset = 0;
        switch (tag & 3) {
        case 0: { // literal
            len = tag >> 2;
            if (len >= 60) {
                const size_t bytes = len - 59;
                if (size - i < bytes) return false;
                len = 0;
                for (size_t k = 0; k < bytes; ++k) len |= size_t(src[i + k]) << (8 * k);
                i += bytes;
            }
            ++len;
            if (size - i < len || out.size() - o < len) return false;
            std::memcpy(out.data() + o, src + i, len);
            i += len;
            o += len;
            continue;
        }
        case 1:
            if (i >= size) return false;
            len = ((tag >> 2) & 7) + 4;
            offset = (size_t(tag >> 5) << 8) | src[i++];
            break;
        case 2:
            if (size - i < 2) return false;
            len = (tag >> 2) + 1;
            offset = size_t(src[i]) | (size_t(src[i + 1]) << 8);
            i += 2;
            break;
        default:
            if (size - i < 4) return false;
            len = (tag >> 2) + 1;
            offset = size_t(src[i]) | (size_t(src[i + 1]) << 8) | (size_t(src[i + 2]) << 16) | (size_t(src[i + 3]) << 24);
            i += 4;
            break;
        }
        if (offset == 0 || offset > o || out.size() - o < len) return false;
        for (size_t k = 0; k < len; ++k, ++o) out[o] = out[o - offset]; // copies may overlap themselves
    }
    return o == out.size();
}

} // namespace Filmmaker
