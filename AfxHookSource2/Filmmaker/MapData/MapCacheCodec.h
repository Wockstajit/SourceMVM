#pragma once

// The map cache's binary payloads (.mvmocclusion, .mvmcasters) are stored compressed: about a
// third of their raw size, losslessly, read back block by block so loading never holds the whole
// raw payload in memory.
//
// Container "MVMZXPH1" (little endian):
//   char[8] magic, u64 rawSize, u32 blockSize, u32 blockCount,
//   u32 storedSize[blockCount]  (bit 31 set: the block is stored uncompressed),
//   the blocks back to back.
// Every block but the last holds blockSize raw bytes. Blocks are Windows XPRESS-Huffman in raw
// mode (cabinet.dll's Compression API): ~3.4x on caster geometry, ~0.2 s to decode 160 MB.
// Automation/lib/mvm_cache.py reads and writes the same container.

#include <cstdint>
#include <filesystem>
#include <istream>
#include <memory>
#include <string>
#include <vector>

namespace Filmmaker::MapData {

constexpr char kCacheCodecMagic[8] = {'M', 'V', 'M', 'Z', 'X', 'P', 'H', '1'};
constexpr uint32_t kCacheCodecBlockSize = 4u << 20;

// Raw payload bytes -> container bytes. Deterministic for the same input. Throws MapDataError.
std::vector<uint8_t> CompressCachePayload(const std::vector<uint8_t> &raw);

// A stream of the raw payload inside a container file. Null (with `error` set) when the file is
// missing or is not a valid container; a block that fails to decode sets the stream's failbit.
std::unique_ptr<std::istream> OpenCachePayload(const std::filesystem::path &path, std::string &error);

} // namespace Filmmaker::MapData
