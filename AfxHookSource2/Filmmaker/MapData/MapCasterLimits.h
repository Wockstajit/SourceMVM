#pragma once
#include <cstdint>
namespace Filmmaker::MapData {
// Complete community maps include large alpha masks and stained glass; keep the same
// bounded payload budget in extraction and runtime loading (complete Boulder geometry needs 742 MiB).
inline constexpr uint32_t kVisualCasterMaxVertices = 16000000;
inline constexpr uint32_t kVisualCasterMaxIndices = 64000000;
inline constexpr uint64_t kVisualCasterMemoryLimit = 1024ull * 1024 * 1024;
inline constexpr uint64_t kVisualCasterFileLimit = kVisualCasterMemoryLimit + 16ull * 1024 * 1024;
}
