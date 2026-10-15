#include "MapCacheCodec.h"
#include "MapDataIo.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <compressapi.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <streambuf>
#include <string>

namespace Filmmaker::MapData {
namespace {

constexpr DWORD kAlgorithm = COMPRESS_ALGORITHM_XPRESS_HUFF | COMPRESS_RAW;
constexpr uint32_t kStored = 0x80000000u;
constexpr size_t kHeaderSize = 8 + 8 + 4 + 4;

template <typename T> void Put(std::vector<uint8_t> &out, T value) {
    const auto at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

struct Compressor {
    COMPRESSOR_HANDLE handle = nullptr;
    Compressor() {
        if (!CreateCompressor(kAlgorithm, nullptr, &handle)) throw MapDataError("cannot create the cache compressor");
    }
    ~Compressor() { if (handle) CloseCompressor(handle); }
};

struct Decompressor {
    DECOMPRESSOR_HANDLE handle = nullptr;
    Decompressor() { CreateDecompressor(kAlgorithm, nullptr, &handle); }
    ~Decompressor() { if (handle) CloseDecompressor(handle); }
};

// Hands out the raw payload one decoded block at a time.
class PayloadBuffer : public std::streambuf {
public:
    PayloadBuffer(std::ifstream file, uint64_t rawSize, uint32_t blockSize, std::vector<uint32_t> stored)
        : file_(std::move(file)), rawSize_(rawSize), blockSize_(blockSize), stored_(std::move(stored)) {}
    bool Ready() const { return decoder_.handle != nullptr; }

protected:
    int_type underflow() override {
        if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
        if (next_ >= stored_.size()) return traits_type::eof();
        const uint64_t done = uint64_t(next_) * blockSize_;
        const size_t want = size_t(std::min<uint64_t>(blockSize_, rawSize_ - done));
        const uint32_t entry = stored_[next_++];
        const size_t size = entry & ~kStored;
        raw_.resize(want);
        if (entry & kStored) {
            if (size != want || !file_.read(reinterpret_cast<char *>(raw_.data()), std::streamsize(size))) return traits_type::eof();
        } else {
            packed_.resize(size);
            SIZE_T got = 0;
            if (!file_.read(reinterpret_cast<char *>(packed_.data()), std::streamsize(size)) ||
                !Decompress(decoder_.handle, packed_.data(), size, raw_.data(), want, &got) || got != want)
                return traits_type::eof();
        }
        char *begin = reinterpret_cast<char *>(raw_.data());
        setg(begin, begin, begin + want);
        return traits_type::to_int_type(*gptr());
    }

private:
    std::ifstream file_;
    uint64_t rawSize_;
    uint32_t blockSize_;
    std::vector<uint32_t> stored_;
    size_t next_ = 0;
    std::vector<uint8_t> raw_, packed_;
    Decompressor decoder_;
};

class PayloadStream : public std::istream {
public:
    explicit PayloadStream(std::unique_ptr<PayloadBuffer> buffer) : std::istream(buffer.get()), buffer_(std::move(buffer)) {}

private:
    std::unique_ptr<PayloadBuffer> buffer_;
};

} // namespace

std::vector<uint8_t> CompressCachePayload(const std::vector<uint8_t> &raw) {
    const uint64_t blocks64 = (raw.size() + kCacheCodecBlockSize - 1) / kCacheCodecBlockSize;
    if (blocks64 > 0xffffffffu) throw MapDataError("cache payload too large");
    const uint32_t blocks = uint32_t(blocks64);
    Compressor compressor;
    std::vector<uint8_t> out;
    out.reserve(raw.size() / 3 + kHeaderSize + size_t(blocks) * 4);
    out.insert(out.end(), kCacheCodecMagic, kCacheCodecMagic + 8);
    Put<uint64_t>(out, raw.size());
    Put<uint32_t>(out, kCacheCodecBlockSize);
    Put<uint32_t>(out, blocks);
    const size_t table = out.size();
    out.resize(table + size_t(blocks) * 4);
    std::vector<uint8_t> packed(kCacheCodecBlockSize);
    for (uint32_t b = 0; b < blocks; ++b) {
        const size_t at = size_t(b) * kCacheCodecBlockSize;
        const size_t size = std::min<size_t>(kCacheCodecBlockSize, raw.size() - at);
        SIZE_T written = 0;
        uint32_t entry;
        // A block that does not shrink (or that the codec refuses) is stored as it is.
        if (Compress(compressor.handle, raw.data() + at, size, packed.data(), size, &written) && written > 0 && written < size) {
            out.insert(out.end(), packed.begin(), packed.begin() + written);
            entry = uint32_t(written);
        } else {
            out.insert(out.end(), raw.begin() + at, raw.begin() + at + size);
            entry = uint32_t(size) | kStored;
        }
        std::memcpy(out.data() + table + size_t(b) * 4, &entry, 4);
    }
    return out;
}

std::unique_ptr<std::istream> OpenCachePayload(const std::filesystem::path &path, std::string &error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "cannot open " + path.u8string(); return nullptr; }
    char magic[8];
    uint64_t rawSize = 0;
    uint32_t blockSize = 0, blocks = 0;
    file.read(magic, 8);
    file.read(reinterpret_cast<char *>(&rawSize), 8);
    file.read(reinterpret_cast<char *>(&blockSize), 4);
    file.read(reinterpret_cast<char *>(&blocks), 4);
    if (!file || std::memcmp(magic, kCacheCodecMagic, 8) != 0) { error = "not a compressed map data payload"; return nullptr; }
    if (blockSize == 0 || blockSize > (64u << 20) || blocks != (rawSize + blockSize - 1) / blockSize) {
        error = "invalid compressed map data payload header";
        return nullptr;
    }
    std::vector<uint32_t> stored(blocks);
    if (blocks && !file.read(reinterpret_cast<char *>(stored.data()), std::streamsize(size_t(blocks) * 4))) {
        error = "truncated compressed map data payload";
        return nullptr;
    }
    for (const uint32_t entry : stored)
        if ((entry & ~kStored) > blockSize) { error = "invalid compressed map data block"; return nullptr; }
    auto buffer = std::make_unique<PayloadBuffer>(std::move(file), rawSize, blockSize, std::move(stored));
    if (!buffer->Ready()) { error = "cannot create the cache decompressor"; return nullptr; }
    return std::make_unique<PayloadStream>(std::move(buffer));
}

} // namespace Filmmaker::MapData
