// Ported from ValveResourceFormat 20.0 (MIT): RubikonPhysics/*, PhysAggregateData.cs,
// Model.GetEmbeddedPhys and GltfModelExporter.Physics.cs (geometry only).
#include "MapPhysics.h"
#include "MapEntities.h"
#include "MapGeometry.h"

#include <cmath>
#include <cstring>
#include <unordered_map>

namespace Filmmaker::MapData {
namespace {

using Vec3 = std::array<float, 3>;

Vec3 ToVec3(const Kv3 &v) { return {float(v.At(0).Number()), float(v.At(1).Number()), float(v.At(2).Number())}; }

// Raw bytes of a field that is either a binary blob or an array of byte-sized integers.
std::vector<uint8_t> Bytes(const Kv3 &v) {
    if (v.type == Kv3::Type::Blob) return v.blob;
    std::vector<uint8_t> out;
    out.reserve(v.arr.size());
    for (const Kv3 &e : v.arr) out.push_back(uint8_t(e.Integer()));
    return out;
}

template <class T> std::vector<T> Cast(const std::vector<uint8_t> &bytes) {
    std::vector<T> out(bytes.size() / sizeof(T));
    if (!out.empty()) std::memcpy(out.data(), bytes.data(), out.size() * sizeof(T));
    return out;
}

std::vector<Vec3> Vertices(const Kv3 &shape) {
    const Kv3 &v = shape["m_Vertices"];
    if (v.type != Kv3::Type::Blob) {
        std::vector<Vec3> out;
        for (const Kv3 &e : v.arr) out.push_back(ToVec3(e));
        return out;
    }
    // Since 2023-11 hulls index explicit positions; m_Vertices then holds byte indices.
    const char *name = shape.Find("m_VertexPositions") ? "m_VertexPositions" : "m_Vertices";
    return Cast<Vec3>(Bytes(shape[name]));
}

struct Edge { uint8_t next, twin, origin, face; };

// Row-major 3x4 (or 4x4) pose: Vector3.Transform with the matrix VRF builds from it.
struct Pose {
    float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    Vec3 Apply(const Vec3 &p) const {
        return {p[0] * m[0] + p[1] * m[1] + p[2] * m[2] + m[3], p[0] * m[4] + p[1] * m[5] + p[2] * m[6] + m[7],
                p[0] * m[8] + p[1] * m[9] + p[2] * m[10] + m[11]};
    }
};

using Tri = std::array<Vec3, 3>;

void Sphere(std::vector<Tri> &out, Vec3 c, float r) {
    const int lat = 16, lon = 16;
    std::vector<Vec3> v;
    for (int a = 0; a <= lat; ++a) {
        const float theta = a * 3.14159265358979f / lat, st = std::sin(theta), ct = std::cos(theta);
        for (int b = 0; b <= lon; ++b) {
            const float phi = b * 6.28318530717959f / lon;
            const Vec3 n{std::cos(phi) * st, ct, std::sin(phi) * st};
            v.push_back({c[0] + n[0] * r, c[1] + n[1] * r, c[2] + n[2] * r});
        }
    }
    for (int a = 0; a < lat; ++a)
        for (int b = 0; b < lon; ++b) {
            const int first = a * (lon + 1) + b, second = first + lon + 1;
            out.push_back({v[first], v[second], v[first + 1]});
            out.push_back({v[second], v[second + 1], v[first + 1]});
        }
}

Vec3 Normalize(Vec3 v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return {v[0] / l, v[1] / l, v[2] / l};
}
Vec3 Cross(Vec3 a, Vec3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }

void Capsule(std::vector<Tri> &out, Vec3 s, Vec3 e, float r) {
    const Vec3 d = Normalize({e[0] - s[0], e[1] - s[1], e[2] - s[2]});
    const float len = std::sqrt((e[0] - s[0]) * (e[0] - s[0]) + (e[1] - s[1]) * (e[1] - s[1]) + (e[2] - s[2]) * (e[2] - s[2]));
    const Vec3 c{(s[0] + e[0]) * .5f, (s[1] + e[1]) * .5f, (s[2] + e[2]) * .5f};
    const Vec3 right = Normalize(Cross(d, std::fabs(d[1]) < .9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0})), up = Cross(right, d);
    const int segments = 16, rings = 8;
    std::vector<Vec3> v;
    for (int ring = 0; ring <= rings; ++ring) {
        const float y = (float(ring) / rings - .5f) * len;
        const Vec3 p{c[0] + d[0] * y, c[1] + d[1] * y, c[2] + d[2] * y};
        for (int seg = 0; seg <= segments; ++seg) {
            const float angle = seg * 6.28318530717959f / segments, x = std::cos(angle) * r, z = std::sin(angle) * r;
            const Vec3 n = Normalize({right[0] * x + up[0] * z, right[1] * x + up[1] * z, right[2] * x + up[2] * z});
            v.push_back({p[0] + n[0] * r, p[1] + n[1] * r, p[2] + n[2] * r});
        }
    }
    for (int ring = 0; ring < rings; ++ring)
        for (int seg = 0; seg < segments; ++seg) {
            const int cur = ring * (segments + 1) + seg, next = cur + segments + 1;
            out.push_back({v[cur], v[next], v[cur + 1]});
            out.push_back({v[next], v[next + 1], v[cur + 1]});
        }
    Sphere(out, s, r);
    Sphere(out, e, r);
}

std::string GroupName(const Kv3 &attributes, const std::string &surface) {
    const Kv3 *tags = attributes.Find("m_InteractAsStrings");
    if (!tags) tags = attributes.Find("m_PhysicsTagStrings");
    const std::string group = attributes["m_CollisionGroupString"].Str();
    std::string name = "physics_group";
    if (!group.empty() && _stricmp(group.c_str(), "default") != 0) name = "physics_" + group;
    if (tags && !tags->arr.empty()) {
        name = "physics";
        for (const Kv3 &t : tags->arr) name += "_" + t.Str();
    }
    if (!surface.empty() && _stricmp(surface.c_str(), "default") != 0) name += "_" + surface;
    return name;
}

Kv3 LoadPhysics(const Vpk &package, const std::string &map) {
    std::vector<uint8_t> bytes;
    const std::string base = "maps/" + map + "/world_physics";
    if (package.Read(base + ".vphys_c", bytes)) return Resource(std::move(bytes)).Kv3Block("DATA");
    if (!package.Read(base + ".vmdl_c", bytes)) throw MapDataError("map has no world physics");
    Resource model(std::move(bytes));
    const Kv3 ctrl = model.Kv3Block("CTRL");
    const Kv3 &embedded = ctrl["embedded_physics"];
    if (embedded.IsNull()) throw MapDataError("world physics model has no embedded physics");
    const int64_t index = embedded["phys_data_block"].Integer(-1);
    if (index < 0 || size_t(index) >= model.Blocks().size()) throw MapDataError("invalid embedded physics block");
    return model.Kv3Block(model.Blocks()[size_t(index)]);
}

} // namespace

OcclusionMesh BuildOcclusionMesh(const Vpk &package, const std::string &map) {
    const Kv3 phys = LoadPhysics(package, map);
    std::vector<Pose> bindPose;
    for (const Kv3 &m : phys["m_bindPose"].arr) {
        Pose p;
        for (size_t k = 0; k < 12; ++k) p.m[k] = float(m.At(k).Number());
        bindPose.push_back(p);
    }
    const auto &attributes = phys["m_collisionAttributes"].arr;
    std::vector<std::string> surfaces;
    for (const Kv3 &h : phys["m_surfacePropertyHashes"].arr) surfaces.push_back(KnownEntityString(uint32_t(h.Integer())));
    const auto &parts = phys["m_parts"].arr;

    OccluderBuilder builder;
    OcclusionMesh mesh;
    std::vector<Tri> tris;
    for (size_t ca = 0; ca < attributes.size(); ++ca) {
        for (size_t sp = 0; sp < surfaces.size(); ++sp) {
            tris.clear();
            const auto matches = [&](const Kv3 &d) {
                return d["m_nCollisionAttributeIndex"].Integer() == int64_t(ca) && d["m_nSurfacePropertyIndex"].Integer() == int64_t(sp);
            };
            for (size_t p = 0; p < parts.size(); ++p) {
                const Kv3 &shape = parts[p]["m_rnShape"];
                const Pose pose = bindPose.empty() ? Pose{} : bindPose.at(p);
                for (const Kv3 &d : shape["m_spheres"].arr) {
                    if (!matches(d)) continue;
                    const Kv3 &s = d["m_Sphere"];
                    Sphere(tris, pose.Apply(ToVec3(s["m_vCenter"])), float(s["m_flRadius"].Number()));
                }
                for (const Kv3 &d : shape["m_capsules"].arr) {
                    if (!matches(d)) continue;
                    const Kv3 &s = d["m_Capsule"];
                    Capsule(tris, pose.Apply(ToVec3(s["m_vCenter"].At(0))), pose.Apply(ToVec3(s["m_vCenter"].At(1))),
                            float(s["m_flRadius"].Number()));
                }
                for (const Kv3 &d : shape["m_hulls"].arr) {
                    if (!matches(d)) continue;
                    const Kv3 &s = d["m_Hull"];
                    std::vector<Vec3> v = Vertices(s);
                    for (Vec3 &x : v) x = pose.Apply(x);
                    std::vector<Edge> edges;
                    std::vector<uint8_t> faces;
                    if (s["m_Edges"].type == Kv3::Type::Blob) edges = Cast<Edge>(s["m_Edges"].blob);
                    else
                        for (const Kv3 &e : s["m_Edges"].arr)
                            edges.push_back({uint8_t(e["m_nNext"].Integer()), uint8_t(e["m_nTwin"].Integer()),
                                             uint8_t(e["m_nOrigin"].Integer()), uint8_t(e["m_nFace"].Integer())});
                    if (s["m_Faces"].type == Kv3::Type::Blob) faces = s["m_Faces"].blob;
                    else
                        for (const Kv3 &f : s["m_Faces"].arr) faces.push_back(uint8_t(f["m_nEdge"].Integer()));
                    for (uint8_t start : faces) {
                        if (start >= edges.size()) throw MapDataError("invalid hull edge");
                        for (uint8_t edge = edges[start].next; edge != start;) {
                            if (edge >= edges.size()) throw MapDataError("invalid hull edge");
                            const uint8_t next = edges[edge].next;
                            if (next == start) break;
                            if (edges[start].origin >= v.size() || edges[edge].origin >= v.size() || next >= edges.size() ||
                                edges[next].origin >= v.size())
                                throw MapDataError("invalid hull vertex");
                            tris.push_back({v[edges[start].origin], v[edges[edge].origin], v[edges[next].origin]});
                            edge = next;
                        }
                    }
                }
                for (const Kv3 &d : shape["m_meshes"].arr) {
                    if (!matches(d)) continue;
                    const Kv3 &s = d["m_Mesh"];
                    std::vector<Vec3> v = Vertices(s);
                    for (Vec3 &x : v) x = pose.Apply(x);
                    std::vector<std::array<int32_t, 3>> triangles;
                    if (s["m_Triangles"].type == Kv3::Type::Blob) triangles = Cast<std::array<int32_t, 3>>(s["m_Triangles"].blob);
                    else
                        for (const Kv3 &t : s["m_Triangles"].arr)
                            triangles.push_back({int32_t(t["m_nIndex"].At(0).Integer()), int32_t(t["m_nIndex"].At(1).Integer()),
                                                 int32_t(t["m_nIndex"].At(2).Integer())});
                    for (const auto &t : triangles) {
                        for (int32_t i : t)
                            if (i < 0 || size_t(i) >= v.size()) throw MapDataError("invalid physics mesh index");
                        tris.push_back({v[size_t(t[0])], v[size_t(t[1])], v[size_t(t[2])]});
                    }
                }
            }
            if (tris.empty()) continue;
            const Kv3 &attr = attributes[ca];
            bool skip = false;
            for (const Kv3 &tag : attr["m_InteractAsStrings"].arr) {
                const std::string &t = tag.Str();
                skip |= t.find("clip") != std::string::npos || t == "sky" || t == "window";
            }
            skip |= surfaces[sp].find("glass") != std::string::npos;
            if (skip) {
                mesh.excluded.push_back(GroupName(attr, surfaces[sp]));
                continue;
            }
            for (const Tri &t : tris) builder.AddTriangle(t[0], t[1], t[2]);
        }
    }
    if (builder.indices.empty()) throw MapDataError("no opaque collision geometry");
    if (builder.vertices.size() > OccluderBuilder::kMaxVertices || builder.indices.size() > OccluderBuilder::kMaxIndices)
        throw MapDataError("collision mesh exceeds runtime bounds: " + std::to_string(builder.vertices.size()) + " vertices, " +
                           std::to_string(builder.indices.size() / 3) + " triangles");
    mesh.vertices = std::move(builder.vertices);
    mesh.indices = std::move(builder.indices);
    BoundsOf(mesh.vertices, mesh.low, mesh.high);
    return mesh;
}

std::vector<uint8_t> SerializeOcclusion(const OcclusionMesh &mesh) {
    ByteWriter w;
    w.Bytes("MVMOCC01", 8);
    w.Put(int32_t(mesh.vertices.size()));
    w.Put(int32_t(mesh.indices.size()));
    for (float f : mesh.low) w.Put(f);
    for (float f : mesh.high) w.Put(f);
    for (const auto &v : mesh.vertices) w.Bytes(v.data(), 12);
    w.Bytes(mesh.indices.data(), mesh.indices.size() * 4);
    return std::move(w.out);
}

} // namespace Filmmaker::MapData
