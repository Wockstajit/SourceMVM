// Ported from the former Automation/tools/LightmapLayoutExporter (built on ValveResourceFormat
// 20.0, MIT): World.GetLightmapUvScale, Material.InputSignature and the draw-call walk.
#include "MapLightmap.h"
#include "MapGeometry.h"
#include "MapMeshes.h"
#include "MapTextures.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>

namespace Filmmaker::MapData {
namespace {

struct Mat {
    float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    Float3 Point(const Float3 &p) const {
        return {p[0] * m[0] + p[1] * m[1] + p[2] * m[2] + m[3], p[0] * m[4] + p[1] * m[5] + p[2] * m[6] + m[7],
                p[0] * m[8] + p[1] * m[9] + p[2] * m[10] + m[11]};
    }
    Float3 Normal(const Float3 &n) const {
        Float3 r{n[0] * m[0] + n[1] * m[1] + n[2] * m[2], n[0] * m[4] + n[1] * m[5] + n[2] * m[6], n[0] * m[8] + n[1] * m[9] + n[2] * m[10]};
        const float l = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        return l > 0 ? Float3{r[0] / l, r[1] / l, r[2] / l} : r;
    }
};

Mat RowsMatrix(const Kv3 &v) {
    Mat t;
    for (size_t r = 0; r < 3; ++r)
        for (size_t c = 0; c < 4; ++c) t.m[r * 4 + c] = float(v.At(r).At(c).Number());
    return t;
}

Mat FlatMatrix(const Kv3 &v) {
    Mat t;
    if (v.type == Kv3::Type::Blob && v.blob.size() >= 48) std::memcpy(t.m, v.blob.data(), 48);
    else
        for (size_t k = 0; k < 12; ++k) t.m[k] = float(v.At(k).Number());
    return t;
}

bool Flag(const Kv3 &dc, const char *key) {
    const Kv3 *v = dc.Find(key);
    return v && (v->type == Kv3::Type::Bool ? v->b : v->Integer() != 0);
}

struct Builder {
    const PackageSet &resources;
    float uvScale[2] = {1, 1};
    std::vector<Float3> positions, normals, uvs; // uv: atlas u, v, page
    std::vector<uint32_t> indices;
    std::map<std::string, std::map<std::pair<std::string, int>, std::string>> signatures; // material -> (d3d name, index) -> name
    std::map<std::string, std::shared_ptr<MeshData>> meshes;
    Float3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};

    const std::map<std::pair<std::string, int>, std::string> &Signature(const std::string &material) {
        auto it = signatures.find(material);
        if (it != signatures.end()) return it->second;
        std::map<std::pair<std::string, int>, std::string> elements;
        std::vector<uint8_t> bytes;
        if (!material.empty() && resources.Read(material, bytes)) {
            const Resource r(std::move(bytes));
            if (const ResourceBlock *insg = r.Block("INSG")) {
                const Kv3 signature = r.Kv3Block(*insg);
                for (const Kv3 &e : signature["m_elems"].arr)
                    elements[{e["m_pD3DSemanticName"].Str(), int(e["m_nD3DSemanticIndex"].Integer())}] = e["m_pName"].Str();
            }
        }
        return signatures.emplace(material, std::move(elements)).first->second;
    }

    std::shared_ptr<MeshData> Mesh(const std::string &path) {
        auto it = meshes.find(path);
        if (it != meshes.end()) return it->second;
        std::vector<uint8_t> bytes;
        std::shared_ptr<MeshData> m;
        if (resources.Read(path, bytes)) m = LoadMesh(std::move(bytes));
        return meshes[path] = m;
    }

    void DrawCall(const MeshData &mesh, const Kv3 &dc, const Mat &transform) {
        if (!Flag(dc, "m_bHasBakedLightingFromLightMap")) return;
        std::string material = dc["m_material"].Str();
        if (material.empty()) material = dc["m_pMaterial"].Str();
        const auto &signature = Signature(material);
        const GpuBuffer *pb = nullptr, *nb = nullptr, *ub = nullptr;
        const VertexAttribute *pa = nullptr, *na = nullptr, *ua = nullptr;
        for (const Kv3 &binding : dc["m_vertexBuffers"].arr) {
            const int64_t index = binding["m_hBuffer"].Integer(-1);
            if (index < 0 || size_t(index) >= mesh.vertexBuffers.size()) continue;
            const GpuBuffer &b = mesh.vertexBuffers[size_t(index)];
            for (const VertexAttribute &f : b.attributes) {
                if (f.semantic == "POSITION" && !pb) { pb = &b; pa = &f; }
                else if (f.semantic == "NORMAL" && !nb) { nb = &b; na = &f; }
                else if (f.semantic == "TEXCOORD" && !ub) {
                    const auto it = signature.find({f.semantic, f.semanticIndex});
                    if (it != signature.end() && (it->second == "vLightmapUV" || it->second == "vLightmapUVW")) { ub = &b; ua = &f; }
                }
            }
        }
        if (!pb || !nb || !ub) return;
        std::vector<Float3> pos, nrm, uv3;
        std::vector<std::array<float, 2>> uv2;
        if (!ReadFloat3(*pb, *pa, pos) || !ReadNormals(*nb, *na, nrm, false)) return;
        const bool three = FormatComponents(ua->format) == 3;
        if (three ? !ReadFloat3(*ub, *ua, uv3) : !ReadFloat2(*ub, *ua, uv2)) return;
        const int64_t ibIndex = dc["m_indexBuffer"]["m_hBuffer"].Integer(-1);
        if (ibIndex < 0 || size_t(ibIndex) >= mesh.indexBuffers.size()) return;
        const GpuBuffer &ib = mesh.indexBuffers[size_t(ibIndex)];
        const int64_t base = dc["m_nBaseVertex"].Integer(), start = dc["m_nStartIndex"].Integer(), count = dc["m_nIndexCount"].Integer();
        if (start < 0 || count < 0 || (ib.stride != 2 && ib.stride != 4) || size_t(start + count) * ib.stride > ib.data.size()) return;
        std::unordered_map<int64_t, uint32_t> remap;
        for (int64_t i = 0; i < count; ++i) {
            int64_t v;
            if (ib.stride == 4) { int32_t x; std::memcpy(&x, ib.data.data() + size_t(start + i) * 4, 4); v = int64_t(x) + base; }
            else { uint16_t x; std::memcpy(&x, ib.data.data() + size_t(start + i) * 2, 2); v = int64_t(x) + base; }
            if (v < 0 || size_t(v) >= pos.size() || size_t(v) >= nrm.size()) return;
            auto it = remap.find(v);
            if (it == remap.end()) {
                it = remap.emplace(v, uint32_t(positions.size())).first;
                const Float3 world = transform.Point(pos[size_t(v)]);
                const Float3 atlas = three ? uv3[size_t(v)] : Float3{uv2[size_t(v)][0], uv2[size_t(v)][1], 0};
                positions.push_back(world);
                normals.push_back(transform.Normal(nrm[size_t(v)]));
                uvs.push_back({atlas[0] * uvScale[0], atlas[1] * uvScale[1], atlas[2]});
                for (int c = 0; c < 3; ++c) { lo[size_t(c)] = std::min(lo[size_t(c)], world[size_t(c)]); hi[size_t(c)] = std::max(hi[size_t(c)], world[size_t(c)]); }
            }
            indices.push_back(it->second);
        }
    }

    void MeshAll(const MeshData &mesh, const Mat &t) {
        for (const Kv3 &so : mesh.data["m_sceneObjects"].arr)
            for (const Kv3 &dc : so["m_drawCalls"].arr) DrawCall(mesh, dc, t);
    }
};

uint16_t Quantize(float v) { return uint16_t(std::clamp(int(std::lround(std::clamp(v, 0.f, 1.f) * 65535.f)), 0, 65535)); }
int16_t SNorm(float v) { return int16_t(std::clamp(int(std::lround(std::clamp(v, -1.f, 1.f) * 32767.f)), -32767, 32767)); }
std::pair<int16_t, int16_t> Octahedral(const Float3 &n) {
    const float l1 = std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]);
    if (l1 < 1e-8f) return {0, 32767};
    float x = n[0] / l1, y = n[1] / l1;
    if (n[2] < 0) {
        const float ox = (1 - std::fabs(y)) * (x >= 0 ? 1.f : -1.f), oy = (1 - std::fabs(x)) * (y >= 0 ? 1.f : -1.f);
        x = ox;
        y = oy;
    }
    return {SNorm(x), SNorm(y)};
}

} // namespace

std::vector<uint8_t> BuildLightmapLayout(const Vpk &package, const PackageSet &resources, const std::string &map, LightmapStats &stats) {
    std::vector<uint8_t> bytes;
    if (!package.Read("maps/" + map + "/world.vwrld_c", bytes)) throw MapDataError("map has no world resource");
    const Kv3 world = Resource(std::move(bytes)).Kv3Block("DATA");
    Builder b{resources};
    const Kv3 &lighting = world["m_worldLightingInfo"];
    if (lighting["m_nLightmapVersionNumber"].Integer() == 8 && lighting.Find("m_vLightmapUvScale")) {
        b.uvScale[0] = float(lighting["m_vLightmapUvScale"].At(0).Number());
        b.uvScale[1] = float(lighting["m_vLightmapUvScale"].At(1).Number());
    }
    for (const Kv3 &nodeInfo : world["m_worldNodes"].arr) {
        std::vector<uint8_t> nodeBytes;
        if (!resources.Read(nodeInfo["m_worldNodePrefix"].Str() + ".vwnod", nodeBytes)) throw MapDataError("missing world node");
        const Kv3 node = Resource(std::move(nodeBytes)).Kv3Block("DATA");
        for (const Kv3 &so : node["m_sceneObjects"].arr) {
            const Mat t = RowsMatrix(so["m_vTransform"]);
            const std::string modelPath = so["m_renderableModel"].Str();
            std::vector<uint8_t> modelBytes;
            if (!modelPath.empty() && resources.Read(modelPath, modelBytes)) {
                const ModelData model = LoadModel(std::move(modelBytes));
                for (size_t k = 0; k < std::min(model.embedded.size(), model.lodMasks.size()); ++k)
                    if (model.InLod(model.embedded[k].meshIndex, 0)) b.MeshAll(*model.embedded[k].mesh, t);
                for (size_t k = 0; k < model.refMeshes.size(); ++k)
                    if (!model.refMeshes[k].empty() && model.InLod(int(k), 0))
                        if (auto mesh = b.Mesh(model.refMeshes[k])) b.MeshAll(*mesh, t);
            }
            const std::string renderable = so["m_renderable"].Str();
            if (!renderable.empty())
                if (auto mesh = b.Mesh(renderable)) b.MeshAll(*mesh, t);
        }
        for (const Kv3 &agg : node["m_aggregateSceneObjects"].arr) {
            const std::string modelPath = agg["m_renderableModel"].Str();
            std::vector<uint8_t> modelBytes;
            if (modelPath.empty() || !resources.Read(modelPath, modelBytes)) continue;
            const ModelData model = LoadModel(std::move(modelBytes));
            std::shared_ptr<MeshData> mesh = model.embedded.empty() ? nullptr : model.embedded[0].mesh;
            if (!mesh)
                for (size_t k = 0; k < model.refMeshes.size() && !mesh; ++k)
                    if (!model.refMeshes[k].empty() && model.InLod(int(k), model.LowestLod())) mesh = b.Mesh(model.refMeshes[k]);
            if (!mesh) continue;
            // Fragments index the flattened draw calls, skipping material-less occluders.
            std::vector<const Kv3 *> flattened;
            for (const Kv3 &so : mesh->data["m_sceneObjects"].arr)
                for (const Kv3 &dc : so["m_drawCalls"].arr) {
                    const bool noMaterial = dc["m_material"].Str().empty() && dc["m_pMaterial"].Str().empty();
                    if (noMaterial && Flag(dc, "m_bIsOccluder")) continue;
                    flattened.push_back(&dc);
                }
            const auto &transforms = agg["m_fragmentTransforms"].arr;
            size_t transformIndex = 0;
            for (const Kv3 &f : agg["m_aggregateMeshes"].arr) {
                Mat t;
                if (Flag(f, "m_bHasTransform") && transformIndex < transforms.size()) t = FlatMatrix(transforms[transformIndex++]);
                const int64_t lodMask = f["m_nLODGroupMask"].Integer(0);
                if (lodMask != 0 && (lodMask & 1) == 0) continue;
                const int64_t draw = f["m_nDrawCallIndex"].Integer(-1);
                if (draw < 0 || size_t(draw) >= flattened.size()) continue;
                b.DrawCall(*mesh, *flattened[size_t(draw)], t);
            }
        }
    }

    int atlasWidth = 8192, atlasHeight = 8192, atlasPages = 1;
    std::vector<uint8_t> atlasBytes;
    if (resources.Read("maps/" + map + "/lightmaps/irradiance.vtex", atlasBytes)) {
        const TextureSize size = ReadTextureSize(atlasBytes);
        atlasWidth = size.width;
        atlasHeight = size.height;
        atlasPages = std::max(1, size.depth);
    }

    // Triangles under a quarter texel in atlas space become one point per texel.
    const Float3 lo{b.lo[0] - 1, b.lo[1] - 1, b.lo[2] - 1}, hi{b.hi[0] + 1, b.hi[1] + 1, b.hi[2] + 1};
    const Float3 extent{hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
    struct Point { Float3 p, n; float u, v; int page; };
    const size_t triangles = b.indices.size() / 3;
    std::vector<bool> keep(triangles);
    std::map<int64_t, Point> points;
    for (size_t t = 0; t < triangles; ++t) {
        const uint32_t i0 = b.indices[t * 3], i1 = b.indices[t * 3 + 1], i2 = b.indices[t * 3 + 2];
        const float ax = b.uvs[i0][0] * atlasWidth, ay = b.uvs[i0][1] * atlasHeight, bx = b.uvs[i1][0] * atlasWidth, by = b.uvs[i1][1] * atlasHeight;
        const float cx = b.uvs[i2][0] * atlasWidth, cy = b.uvs[i2][1] * atlasHeight;
        const float area = .5f * std::fabs((bx - ax) * (cy - ay) - (cx - ax) * (by - ay));
        if (area >= .25f) { keep[t] = true; continue; }
        const float centreX = (ax + bx + cx) / 3, centreY = (ay + by + cy) / 3;
        const int page = int(std::lround(b.uvs[i0][2]));
        const int64_t key = (int64_t(page) * atlasHeight + int64_t(centreY)) * atlasWidth + int64_t(centreX);
        if (points.count(key)) continue;
        Float3 n{b.normals[i0][0] + b.normals[i1][0] + b.normals[i2][0], b.normals[i0][1] + b.normals[i1][1] + b.normals[i2][1],
                 b.normals[i0][2] + b.normals[i1][2] + b.normals[i2][2]};
        const float l2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
        n = l2 > 1e-12f ? Float3{n[0] / std::sqrt(l2), n[1] / std::sqrt(l2), n[2] / std::sqrt(l2)} : b.normals[i0];
        const Float3 p{(b.positions[i0][0] + b.positions[i1][0] + b.positions[i2][0]) / 3, (b.positions[i0][1] + b.positions[i1][1] + b.positions[i2][1]) / 3,
                       (b.positions[i0][2] + b.positions[i1][2] + b.positions[i2][2]) / 3};
        points[key] = {p, n, centreX / atlasWidth, centreY / atlasHeight, page};
    }
    std::unordered_map<uint32_t, uint32_t> remap;
    std::vector<uint32_t> outVertices, outIndices;
    for (size_t t = 0; t < triangles; ++t) {
        if (!keep[t]) continue;
        for (int k = 0; k < 3; ++k) {
            const uint32_t source = b.indices[t * 3 + size_t(k)];
            auto it = remap.find(source);
            if (it == remap.end()) {
                it = remap.emplace(source, uint32_t(outVertices.size())).first;
                outVertices.push_back(source);
            }
            outIndices.push_back(it->second);
        }
    }

    ByteWriter w;
    const auto vertex = [&](const Float3 &p, const Float3 &n, float u, float v, int page) {
        for (int c = 0; c < 3; ++c) w.Put(Quantize((p[size_t(c)] - lo[size_t(c)]) / extent[size_t(c)]));
        w.Put(uint16_t(page));
        w.Put(Quantize(u));
        w.Put(Quantize(v));
        const auto [nx, ny] = Octahedral(n);
        w.Put(nx);
        w.Put(ny);
    };
    w.Bytes("MVMLMAP2", 8);
    w.Put(uint32_t(2));
    w.Put(uint32_t(atlasWidth));
    w.Put(uint32_t(atlasHeight));
    w.Put(uint32_t(atlasPages));
    w.Put(uint32_t(outVertices.size()));
    w.Put(uint32_t(outIndices.size()));
    w.Put(uint32_t(points.size()));
    w.Put(b.uvScale[0]);
    w.Put(b.uvScale[1]);
    for (float f : lo) w.Put(f);
    for (float f : hi) w.Put(f);
    for (uint32_t s : outVertices) vertex(b.positions[s], b.normals[s], b.uvs[s][0], b.uvs[s][1], int(std::lround(b.uvs[s][2])));
    for (const auto &[key, point] : points) vertex(point.p, point.n, point.u, point.v, point.page);
    w.Bytes(outIndices.data(), outIndices.size() * 4);
    stats = {outIndices.size() / 3, outVertices.size(), points.size(), atlasWidth, atlasHeight, atlasPages};
    return std::move(w.out);
}

} // namespace Filmmaker::MapData
