// Ported from ValveResourceFormat 20.0 (MIT): Blocks/VBIB.cs, ResourceTypes/Mesh.cs,
// ResourceTypes/Model.cs, ResourceTypes/ModelLodInfo.cs.
#include "MapMeshes.h"

#include "../../../Deps/release/meshoptimizer/meshoptimizer.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "../../../Deps/release/zstd/lib/zstd.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Filmmaker::MapData {
namespace {

enum Dxgi : uint32_t {
    R32G32B32A32_FLOAT = 2, R32G32B32A32_SINT = 4, R32G32B32_FLOAT = 6, R16G16B16A16_FLOAT = 10, R16G16B16A16_UNORM = 11,
    R16G16B16A16_UINT = 12, R16G16B16A16_SINT = 14, R32G32_FLOAT = 16, R8G8B8A8_UNORM = 28, R8G8B8A8_UINT = 30,
    R16G16_FLOAT = 34, R16G16_UNORM = 35, R16G16_SNORM = 37, R16G16_SINT = 38, R32_FLOAT = 41, R32_UINT = 42,
};

float Half(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000) << 16, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) bits = sign;
        else {
            int e = -1;
            uint32_t m = mant;
            do { ++e; m <<= 1; } while (!(m & 0x400));
            bits = sign | uint32_t(127 - 15 - e) << 23 | (m & 0x3FF) << 13;
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000 | mant << 13;
    } else {
        bits = sign | (exp + 112) << 23 | mant << 13;
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

std::vector<uint8_t> Decompress(const GpuBuffer &b, const uint8_t *src, size_t size, bool vertex, bool zstd) {
    std::vector<uint8_t> unpacked;
    if (zstd) {
        unpacked.resize(size_t(b.count) * b.stride);
        const size_t written = ZSTD_decompress(unpacked.data(), unpacked.size(), src, size);
        if (ZSTD_isError(written)) throw MapDataError("mesh ZSTD decompression failed");
        unpacked.resize(written);
        src = unpacked.data();
        size = unpacked.size();
    }
    std::vector<uint8_t> out(size_t(b.count) * b.stride);
    const int result = vertex ? meshopt_decodeVertexBuffer(out.data(), b.count, b.stride, src, size)
                              : meshopt_decodeIndexBuffer(out.data(), b.count, b.stride, src, size);
    if (result != 0) throw MapDataError("mesh buffer decode failed");
    return out;
}

template <class T> T Get(const std::vector<uint8_t> &b, size_t at) {
    if (at > b.size() || sizeof(T) > b.size() - at) throw MapDataError("truncated mesh buffer");
    T v;
    std::memcpy(&v, b.data() + at, sizeof(T));
    return v;
}

GpuBuffer ReadBinaryBuffer(const std::vector<uint8_t> &bytes, size_t at, bool vertex) {
    GpuBuffer b;
    b.count = Get<uint32_t>(bytes, at);
    const int32_t size = Get<int32_t>(bytes, at + 4);
    b.stride = uint32_t(size & 0x3FFFFFF);
    const bool zstd = (size & 0x8000000) != 0;
    const size_t refA = at + 8;
    const uint32_t attributeOffset = Get<uint32_t>(bytes, refA), attributeCount = Get<uint32_t>(bytes, refA + 4);
    const size_t refB = at + 16;
    const uint32_t dataOffset = Get<uint32_t>(bytes, refB);
    const int32_t totalSize = Get<int32_t>(bytes, refB + 4);
    if (attributeCount > 64 || totalSize < 0) throw MapDataError("invalid mesh buffer");
    for (uint32_t k = 0; k < attributeCount; ++k) {
        const size_t a = refA + attributeOffset + size_t(k) * 56;
        if (a + 56 > bytes.size()) throw MapDataError("truncated vertex layout");
        VertexAttribute attr;
        const char *name = reinterpret_cast<const char *>(bytes.data() + a);
        attr.semantic.assign(name, strnlen(name, 32));
        for (char &c : attr.semantic) c = char(toupper(static_cast<unsigned char>(c)));
        attr.semanticIndex = Get<int32_t>(bytes, a + 32);
        attr.format = Get<uint32_t>(bytes, a + 36);
        attr.offset = Get<uint32_t>(bytes, a + 40);
        b.attributes.push_back(attr);
    }
    const size_t data = refB + dataOffset;
    if (data > bytes.size() || size_t(totalSize) > bytes.size() - data) throw MapDataError("truncated mesh data");
    if (uint64_t(b.count) * b.stride > uint64_t(uint32_t(totalSize))) b.data = Decompress(b, bytes.data() + data, size_t(totalSize), vertex, zstd);
    else b.data.assign(bytes.data() + data, bytes.data() + data + totalSize);
    return b;
}

void ReadBinaryVbib(const Resource &r, const ResourceBlock &block, MeshData &mesh) {
    const auto &bytes = r.Bytes();
    const size_t at = block.offset;
    const uint32_t vbOffset = Get<uint32_t>(bytes, at), vbCount = Get<uint32_t>(bytes, at + 4);
    const uint32_t ibOffset = Get<uint32_t>(bytes, at + 8), ibCount = Get<uint32_t>(bytes, at + 12);
    if (vbCount > 1024 || ibCount > 1024) throw MapDataError("invalid VBIB");
    for (uint32_t k = 0; k < vbCount; ++k) mesh.vertexBuffers.push_back(ReadBinaryBuffer(bytes, at + vbOffset + size_t(k) * 24, true));
    for (uint32_t k = 0; k < ibCount; ++k) mesh.indexBuffers.push_back(ReadBinaryBuffer(bytes, at + 8 + ibOffset + size_t(k) * 24, false));
}

GpuBuffer ReadKvBuffer(const Resource &r, const Kv3 &kv, bool vertex) {
    GpuBuffer b;
    b.count = uint32_t(kv["m_nElementCount"].Integer());
    b.stride = uint32_t(kv["m_nElementSizeInBytes"].Integer());
    for (const Kv3 &il : kv["m_inputLayoutFields"].arr) {
        VertexAttribute a;
        const Kv3 &name = il["m_pSemanticName"];
        if (name.type == Kv3::Type::String) a.semantic = name.s;
        else if (name.type == Kv3::Type::Blob) a.semantic.assign(reinterpret_cast<const char *>(name.blob.data()), strnlen(reinterpret_cast<const char *>(name.blob.data()), name.blob.size()));
        for (char &c : a.semantic) c = char(toupper(static_cast<unsigned char>(c)));
        a.semanticIndex = int(il["m_nSemanticIndex"].Integer());
        a.format = uint32_t(il["m_Format"].Integer());
        a.offset = uint32_t(il["m_nOffset"].Integer());
        b.attributes.push_back(a);
    }
    const size_t full = size_t(b.count) * b.stride;
    if (const Kv3 *data = kv.Find("m_pData")) {
        std::vector<uint8_t> bytes;
        if (data->type == Kv3::Type::Blob) bytes = data->blob;
        else for (const Kv3 &e : data->arr) bytes.push_back(uint8_t(e.Integer()));
        b.data = bytes.size() == full ? std::move(bytes) : Decompress(b, bytes.data(), bytes.size(), vertex, false);
        return b;
    }
    const int64_t index = kv["m_nBlockIndex"].Integer(-1);
    if (index < 0 || size_t(index) >= r.Blocks().size()) throw MapDataError("invalid mesh buffer block");
    const ResourceBlock &block = r.Blocks()[size_t(index)];
    const uint8_t *src = r.Data(block);
    if (kv["m_bMeshoptCompressed"].Integer() == 1) b.data = Decompress(b, src, block.size, vertex, kv["m_bCompressedZSTD"].Integer() == 1);
    else b.data.assign(src, src + block.size);
    return b;
}

void ReadKvBuffers(const Resource &r, const Kv3 &kv, MeshData &mesh) {
    for (const Kv3 &vb : kv["m_vertexBuffers"].arr) mesh.vertexBuffers.push_back(ReadKvBuffer(r, vb, true));
    for (const Kv3 &ib : kv["m_indexBuffers"].arr) mesh.indexBuffers.push_back(ReadKvBuffer(r, ib, false));
}

const ResourceBlock &BlockAt(const Resource &r, int64_t index) {
    if (index < 0 || size_t(index) >= r.Blocks().size()) throw MapDataError("invalid resource block index");
    return r.Blocks()[size_t(index)];
}

template <size_t N> bool ReadFloats(const GpuBuffer &b, const VertexAttribute &a, std::vector<std::array<float, N>> &out) {
    out.assign(b.count, {});
    for (uint32_t i = 0; i < b.count; ++i) {
        const size_t at = size_t(i) * b.stride + a.offset;
        auto &v = out[i];
        switch (a.format) {
        case R32G32B32A32_FLOAT: case R32G32B32_FLOAT: case R32G32_FLOAT:
            if (at + 4 * N > b.data.size()) return false;
            std::memcpy(v.data(), b.data.data() + at, 4 * N);
            break;
        case R16G16_FLOAT: case R16G16B16A16_FLOAT:
            if (at + 2 * N > b.data.size()) return false;
            for (size_t k = 0; k < N; ++k) v[k] = Half(Get<uint16_t>(b.data, at + 2 * k));
            break;
        case R16G16_UNORM:
            if (at + 4 > b.data.size()) return false;
            for (size_t k = 0; k < N; ++k) v[k] = Get<uint16_t>(b.data, at + 2 * k) / 65535.f;
            break;
        case R16G16_SNORM:
            if (at + 4 > b.data.size()) return false;
            for (size_t k = 0; k < N; ++k) v[k] = Get<int16_t>(b.data, at + 2 * k) / 32767.f;
            break;
        case R8G8B8A8_UNORM:
            if (at + 4 > b.data.size()) return false;
            for (size_t k = 0; k < N; ++k) v[k] = b.data[at + k] / 255.f;
            break;
        default: return false;
        }
    }
    return true;
}

} // namespace

int FormatComponents(uint32_t f) {
    switch (f) {
    case R8G8B8A8_UINT: case R8G8B8A8_UNORM: case R16G16B16A16_FLOAT: case R16G16B16A16_SINT: case R16G16B16A16_UINT:
    case R16G16B16A16_UNORM: case R32G32B32A32_FLOAT: case R32G32B32A32_SINT: return 4;
    case R16G16_FLOAT: case R16G16_SINT: case R16G16_SNORM: case R16G16_UNORM: case R32G32_FLOAT: return 2;
    case R32_FLOAT: case R32_UINT: return 1;
    case R32G32B32_FLOAT: return 3;
    default: return 0;
    }
}

bool ReadFloat3(const GpuBuffer &b, const VertexAttribute &a, std::vector<std::array<float, 3>> &out) {
    return a.format == R32G32B32_FLOAT && ReadFloats<3>(b, a, out);
}
bool ReadFloat2(const GpuBuffer &b, const VertexAttribute &a, std::vector<std::array<float, 2>> &out) {
    return (a.format == R32G32_FLOAT || a.format == R16G16_FLOAT || a.format == R16G16_UNORM || a.format == R16G16_SNORM) && ReadFloats<2>(b, a, out);
}
bool ReadFloat4(const GpuBuffer &b, const VertexAttribute &a, std::vector<std::array<float, 4>> &out) {
    return (a.format == R32G32B32A32_FLOAT || a.format == R16G16B16A16_FLOAT || a.format == R8G8B8A8_UNORM) && ReadFloats<4>(b, a, out);
}

namespace {

std::array<float, 3> DecompressNormal(float x, float y) {
    x -= 128.0f;
    y -= 128.0f;
    const float zSignBit = x < 0 ? 1.0f : 0.0f, tSignBit = y < 0 ? 1.0f : 0.0f;
    const float zSign = -((2 * zSignBit) - 1), tSign = -((2 * tSignBit) - 1);
    x = (x * zSign) - zSignBit;
    y = (y * tSign) - tSignBit;
    x -= 64;
    y -= 64;
    const float xSignBit = x < 0 ? 1.0f : 0.0f, ySignBit = y < 0 ? 1.0f : 0.0f;
    const float xSign = -((2 * xSignBit) - 1), ySign = -((2 * ySignBit) - 1);
    x = ((x * xSign) - xSignBit) / 63.0f;
    y = ((y * ySign) - ySignBit) / 63.0f;
    float z = 1.0f - x - y;
    const float oolen = 1.0f / std::sqrt(x * x + y * y + z * z);
    return {x * oolen * xSign, y * oolen * ySign, z * oolen * zSign};
}

std::array<float, 3> DecompressFrame(uint32_t packed) {
    const float xBits = float((packed >> 12) & 0x3ff), yBits = float((packed >> 22) & 0x3ff);
    float x = (xBits / 1023.0f) * 2.0f - 1.0f, y = (yBits / 1023.0f) * 2.0f - 1.0f;
    const float z = 1.0f - std::fabs(x) - std::fabs(y);
    const float compensation = std::clamp(-z, 0.0f, 1.0f);
    const float xPositive = x >= 0.0f ? 1.0f : 0.0f, yPositive = y >= 0.0f ? 1.0f : 0.0f;
    x += compensation * (1.f - xPositive) + -compensation * xPositive;
    y += compensation * (1.f - yPositive) + -compensation * yPositive;
    const float len = std::sqrt(x * x + y * y + z * z);
    return {x / len, y / len, z / len};
}

} // namespace

bool ReadNormals(const GpuBuffer &b, const VertexAttribute &a, std::vector<std::array<float, 3>> &out, bool fixNonUnit) {
    out.assign(b.count, {});
    for (uint32_t i = 0; i < b.count; ++i) {
        const size_t at = size_t(i) * b.stride + a.offset;
        if (a.format == R32G32B32_FLOAT) {
            if (at + 12 > b.data.size()) return false;
            std::memcpy(out[i].data(), b.data.data() + at, 12);
        } else if (a.format == R32_UINT) {
            out[i] = DecompressFrame(Get<uint32_t>(b.data, at));
        } else if (a.format == R8G8B8A8_UNORM) {
            if (at + 2 > b.data.size()) return false;
            out[i] = DecompressNormal(b.data[at], b.data[at + 1]);
        } else {
            return false;
        }
        const auto &n = out[i];
        if (fixNonUnit && std::fabs(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) - 1.0f) > 0.00674f) out[i] = {0, 0, -1};
    }
    return true;
}

int ModelData::LowestLod() const {
    int64_t combined = 0;
    for (int64_t m : lodMasks) combined |= m;
    if (!combined) return 0;
    int level = 0;
    while (!((uint64_t(combined) >> level) & 1)) ++level;
    return level;
}

bool ModelData::InLod(int meshIndex, int level) const {
    return meshIndex < 0 || size_t(meshIndex) >= lodMasks.size() || (lodMasks[size_t(meshIndex)] & (int64_t(1) << level)) != 0;
}

std::shared_ptr<MeshData> LoadMesh(std::vector<uint8_t> bytes) {
    const Resource r(std::move(bytes));
    auto mesh = std::make_shared<MeshData>();
    mesh->data = r.Kv3Block("DATA");
    if (const ResourceBlock *vbib = r.Block("VBIB")) {
        if (IsKv3Magic(r.Data(*vbib), vbib->size)) ReadKvBuffers(r, r.Kv3Block(*vbib), *mesh);
        else ReadBinaryVbib(r, *vbib, *mesh);
    } else {
        ReadKvBuffers(r, mesh->data, *mesh);
    }
    return mesh;
}

ModelData LoadModel(std::vector<uint8_t> bytes) {
    const Resource r(std::move(bytes));
    ModelData m;
    m.data = r.Kv3Block("DATA");
    for (const Kv3 &v : m.data["m_refLODGroupMasks"].arr) m.lodMasks.push_back(v.Integer());
    for (const Kv3 &v : m.data["m_refMeshes"].arr) m.refMeshes.push_back(v.Str());
    if (const ResourceBlock *ctrl = r.Block("CTRL")) m.ctrl = r.Kv3Block(*ctrl);
    for (const Kv3 &e : m.ctrl["embedded_meshes"].arr) {
        ModelMesh mm;
        mm.mesh = std::make_shared<MeshData>();
        if (e.Find("vbib_block")) {
            mm.name = e["name"].Str();
            mm.meshIndex = int(e["mesh_index"].Integer());
            mm.mesh->data = r.Kv3Block(BlockAt(r, e["data_block"].Integer(-1)));
            const ResourceBlock &vbib = BlockAt(r, e["vbib_block"].Integer(-1));
            if (IsKv3Magic(r.Data(vbib), vbib.size)) ReadKvBuffers(r, r.Kv3Block(vbib), *mm.mesh);
            else ReadBinaryVbib(r, vbib, *mm.mesh);
        } else {
            mm.name = e["m_Name"].Str();
            mm.meshIndex = int(e["m_nMeshIndex"].Integer());
            mm.mesh->data = r.Kv3Block(BlockAt(r, e["m_nDataBlock"].Integer(-1)));
            ReadKvBuffers(r, e, *mm.mesh);
        }
        m.embedded.push_back(std::move(mm));
    }
    return m;
}

} // namespace Filmmaker::MapData
