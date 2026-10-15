#pragma once

// Render mesh data (.vmdl_c / .vmesh_c and embedded meshes): vertex/index buffers and draw
// calls. Ported from ValveResourceFormat 20.0 (MIT) Blocks/VBIB.cs, ResourceTypes/Mesh.cs
// and ResourceTypes/Model.cs, reduced to what shadow casters need.

#include "MapDataIo.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace Filmmaker::MapData {

struct VertexAttribute {
    std::string semantic; // upper case
    int semanticIndex = 0;
    uint32_t format = 0;  // DXGI_FORMAT
    uint32_t offset = 0;
};

struct GpuBuffer {
    uint32_t count = 0, stride = 0;
    std::vector<VertexAttribute> attributes;
    std::vector<uint8_t> data;
};

struct MeshData {
    Kv3 data; // PermRenderMeshData_t
    std::vector<GpuBuffer> vertexBuffers, indexBuffers;
};

struct ModelMesh {
    std::shared_ptr<MeshData> mesh;
    int meshIndex = 0;
    std::string name; // the exporter's mesh node name
};

struct ModelData {
    Kv3 data, ctrl;
    std::vector<ModelMesh> embedded;   // every embedded mesh with its LOD mask order
    std::vector<int64_t> lodMasks;
    std::vector<std::string> refMeshes;
    int LowestLod() const;
    bool InLod(int meshIndex, int level) const;
};

ModelData LoadModel(std::vector<uint8_t> bytes);
std::shared_ptr<MeshData> LoadMesh(std::vector<uint8_t> bytes);

// Attribute readers (VBIB.GetVector*AttributeArray). Return false for unsupported formats.
bool ReadFloat3(const GpuBuffer &b, const VertexAttribute &a, std::vector<std::array<float, 3>> &out);
bool ReadFloat2(const GpuBuffer &b, const VertexAttribute &a, std::vector<std::array<float, 2>> &out);
bool ReadFloat4(const GpuBuffer &b, const VertexAttribute &a, std::vector<std::array<float, 4>> &out);
// NORMAL in any of its encodings (float3, CS2 packed frame, legacy byte pairs). With
// `fixNonUnit`, vectors that are not unit length become (0, 0, -1) as the glTF exporter does.
bool ReadNormals(const GpuBuffer &b, const VertexAttribute &a, std::vector<std::array<float, 3>> &out, bool fixNonUnit = true);
// Component count of a DXGI vertex format, 0 when unknown.
int FormatComponents(uint32_t format);

} // namespace Filmmaker::MapData
