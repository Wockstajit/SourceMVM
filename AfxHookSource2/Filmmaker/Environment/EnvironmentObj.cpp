#include "EnvironmentScene.h"
#include <stdexcept>
namespace Filmmaker::Environment {
bool LoadObj(std::istream &stream, Mesh &mesh, std::string &error) {
    std::vector<Vec3> positions, normals;
    std::vector<std::array<float, 2>> uv;
    Mesh pending;
    struct Corner {
        int p = -1, t = -1, n = -1;
    };
    size_t bytes = 0;
    std::string line;
    auto fail = [&](const char *message) {
        error = message;
        return false;
    };
    auto index = [](const std::string &token, size_t count) {
        size_t used = 0;
        long value = std::stol(token, &used);
        if (used != token.size() || !value)
            throw std::runtime_error("Invalid OBJ index");
        long resolved = value > 0 ? value - 1 : long(count) + value;
        if (resolved < 0 || resolved >= long(count))
            throw std::runtime_error("OBJ index out of range");
        return int(resolved);
    };
    try {
        while (std::getline(stream, line)) {
            bytes += line.size() + 1;
            if (bytes > 16 * 1024 * 1024 || line.size() > 16384)
                return fail("OBJ exceeds size limit");
            std::istringstream in(line);
            std::string tag;
            in >> tag;
            if (tag == "v" || tag == "vn") {
                Vec3 v;
                if (!(in >> v.x >> v.y >> v.z) || !Finite(v))
                    return fail("Invalid OBJ position or normal");
                if (tag == "vn" && Dot(v, v) < 1e-12f)
                    return fail("Degenerate OBJ normal");
                auto &values = tag == "v" ? positions : normals;
                values.push_back(tag == "vn" ? Normalize(v) : v);
                if (values.size() > 100000)
                    return fail("Too many OBJ positions or normals");
            } else if (tag == "vt") {
                std::array<float, 2> v{};
                if (!(in >> v[0] >> v[1]) || !std::isfinite(v[0]) || !std::isfinite(v[1]))
                    return fail("Invalid OBJ UV");
                uv.push_back(v);
                if (uv.size() > 100000)
                    return fail("Too many OBJ UVs");
            } else if (tag == "f") {
                std::vector<Corner> face;
                std::string token;
                while (in >> token) {
                    if (token[0] == '#')
                        break;
                    Corner c;
                    size_t first = token.find('/'),
                           second = first == std::string::npos ? first : token.find('/', first + 1);
                    c.p = index(token.substr(0, first), positions.size());
                    if (first != std::string::npos) {
                        std::string texture = token.substr(
                            first + 1, second == std::string::npos ? second : second - first - 1);
                        if (!texture.empty())
                            c.t = index(texture, uv.size());
                    }
                    if (second != std::string::npos) {
                        std::string normal = token.substr(second + 1);
                        if (normal.empty())
                            return fail("Missing OBJ normal index");
                        c.n = index(normal, normals.size());
                    }
                    face.push_back(c);
                    if (face.size() > 32)
                        return fail("Triangulate OBJ faces with more than 32 corners");
                }
                if (face.size() < 3)
                    return fail("Invalid OBJ face");
                for (size_t i = 1; i + 1 < face.size(); ++i) {
                    auto a = positions[face[0].p], bb = positions[face[i].p], c = positions[face[i + 1].p];
                    Vec3 n = Cross(bb - a, c - a);
                    if (Dot(n, n) < 1e-12f)
                        continue;
                    n = Normalize(n);
                    for (auto corner : {face[0], face[i], face[i + 1]}) {
                        Vertex v{positions[corner.p], corner.n >= 0 ? normals[corner.n] : n};
                        if (corner.t >= 0) {
                            v.u = uv[corner.t][0];
                            v.v = 1 - uv[corner.t][1];
                        }
                        pending.vertices.push_back(v);
                    }
                    if (pending.vertices.size() > 300000)
                        return fail("OBJ exceeds 100,000 triangles");
                }
            }
        }
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
    if (pending.vertices.empty())
        return fail("OBJ has no triangles");
    Vec3 lo = pending.vertices[0].position, hi = lo;
    for (auto &v : pending.vertices) {
        lo = {std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z)};
        hi = {std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z)};
    }
    Vec3 center = (lo + hi) * .5f;
    float extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    if (!std::isfinite(extent) || extent < 1e-5f)
        return fail("OBJ bounds are degenerate");
    for (auto &v : pending.vertices)
        v.position = (v.position - center) * (2 / extent);
    mesh = std::move(pending);
    error.clear();
    return true;
}
std::shared_ptr<const Mesh> Primitive(const std::string &kind) {
    auto m = std::make_shared<Mesh>();
    auto tri = [&](Vec3 a, Vec3 b, Vec3 c) {
        Vec3 n = Normalize(Cross(b - a, c - a));
        m->vertices.insert(m->vertices.end(), {{a, n}, {b, n}, {c, n}});
    };
    if (kind == "beveledbox") {
        const float coordinates[] = {-1, -.995f, -.98f, -.96f, -.94f, 0, .94f, .96f, .98f, .995f, 1};
        const Vec3 faces[] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const Vec3 right[] = {{0, 1, 0}, {0, -1, 0}, {-1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
        const Vec3 up[] = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
        for (int f = 0; f < 6; ++f)
            for (int y = 0; y < 10; ++y)
                for (int x = 0; x < 10; ++x) {
                    auto vertex = [&](int xx, int yy) {
                        Vec3 p = faces[f] + right[f] * coordinates[xx] + up[f] * coordinates[yy];
                        Vec3 inner{std::clamp(p.x, -.94f, .94f), std::clamp(p.y, -.94f, .94f),
                                   std::clamp(p.z, -.94f, .94f)};
                        Vec3 n = Normalize(p - inner);
                        return Vertex{inner + n * .06f, n, (coordinates[xx] + 1) * .5f,
                                      (1 - coordinates[yy]) * .5f};
                    };
                    auto a = vertex(x, y), b = vertex(x + 1, y), c = vertex(x + 1, y + 1),
                         d = vertex(x, y + 1);
                    m->vertices.insert(m->vertices.end(), {a, b, c, a, c, d});
                }
    } else if (kind == "box") {
        Vec3 p[8] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                     {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
        int f[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
        for (auto &q : f) {
            tri(p[q[0]], p[q[1]], p[q[2]]);
            tri(p[q[0]], p[q[2]], p[q[3]]);
        }
    } else if (kind == "sphere") {
        auto p = [](int x, int y) {
            float a = Radians(x * 15.f), b = Radians(y * 15.f - 90);
            return Vec3{std::cos(a) * std::cos(b), std::sin(a) * std::cos(b), std::sin(b)};
        };
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 24; ++x) {
                tri(p(x, y), p(x + 1, y), p(x + 1, y + 1));
                tri(p(x, y), p(x + 1, y + 1), p(x, y + 1));
            }
        for (auto &v : m->vertices)
            v.normal = v.position;
    } else if (kind == "cylinder") {
        for (int i = 0; i < 32; ++i) {
            float a = Radians(i * 11.25f), b = Radians((i + 1) * 11.25f);
            Vec3 p{std::cos(a), std::sin(a), -1}, q{std::cos(b), std::sin(b), -1}, u{p.x, p.y, 1},
                v{q.x, q.y, 1};
            tri(p, q, v);
            tri(p, v, u);
            tri({0, 0, -1}, q, p);
            tri({0, 0, 1}, u, v);
        }
    }
    for (auto &v : m->vertices) {
        if (kind == "beveledbox")
            continue;
        if (kind == "sphere") {
            v.u = .5f + std::atan2(v.position.y, v.position.x) / 6.28318530718f;
            v.v = .5f - std::asin(std::clamp(v.position.z, -1.f, 1.f)) / 3.14159265359f;
        } else if (kind == "cylinder" && std::abs(v.normal.z) < .5f) {
            v.u = .5f + std::atan2(v.position.y, v.position.x) / 6.28318530718f;
            v.v = (1 - v.position.z) * .5f;
            v.normal = Normalize({v.position.x, v.position.y, 0});
        } else {
            Vec3 n = v.normal;
            if (std::abs(n.z) > .5f) {
                v.u = (v.position.x + 1) * .5f;
                v.v = (1 - v.position.y) * .5f;
            } else if (std::abs(n.x) > .5f) {
                v.u = (v.position.y + 1) * .5f;
                v.v = (1 - v.position.z) * .5f;
            } else {
                v.u = (v.position.x + 1) * .5f;
                v.v = (1 - v.position.z) * .5f;
            }
        }
    }
    if (kind == "sphere" || kind == "cylinder")
        for (size_t i = 0; i < m->vertices.size(); i += 3) {
            float lo = 1, hi = 0;
            for (int j = 0; j < 3; ++j) {
                lo = std::min(lo, m->vertices[i + j].u);
                hi = std::max(hi, m->vertices[i + j].u);
            }
            if (hi - lo > .5f)
                for (int j = 0; j < 3; ++j)
                    if (m->vertices[i + j].u < .5f)
                        m->vertices[i + j].u += 1;
        }
    return m;
}
} // namespace Filmmaker::Environment
