#pragma once
#include <cstdint>
namespace Filmmaker::MapData {
// Complete community maps include large alpha masks and stained glass; keep the same
// bounded payload budget in extraction and runtime loading (complete Boulder geometry needs 742 MiB).
inline constexpr uint32_t kVisualCasterMaxVertices = 16000000;
inline constexpr uint32_t kVisualCasterMaxIndices = 64000000;
inline constexpr uint64_t kVisualCasterMemoryLimit = 1024ull * 1024 * 1024;
inline constexpr uint64_t kVisualCasterFileLimit = kVisualCasterMemoryLimit + 16ull * 1024 * 1024;
// A rigid triangle's albedo word is linear R8G8B8 in its low three bytes. This bit in the fourth
// marks a triangle of a material the map authored as letting light through to its back face
// (F_TRANSMISSIVE_BACKFACE_NDOTL: cloth tarps, awnings, hanging cloth). The game draws such a
// sheet's shaded face lit by the light on its other face. The sheet is solid everywhere else: it
// casts a full sun shadow and shelters from weather. The bounce reads the mark, so that the ground
// under a sunlit tarp gets the tarp's coloured light (RelightProbeGiShaders.h, RelightLiteGridMath.h).
// Bit 0x01000000 of the same byte is the bounce's own mark for porous triangles (foliage, grilles).
inline constexpr uint32_t kRigidTransmissive = 0x02000000u;
}
