#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace Filmmaker::Environment {
struct Vec3 {
    float x = 0, y = 0, z = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 Normalize(Vec3 a) {
    float n = std::sqrt(Dot(a, a));
    return n > 1e-8f ? a * (1 / n) : Vec3{0, 0, 1};
}
inline bool Finite(Vec3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
inline float Radians(float x) { return x * 0.017453292519943295f; }
// Source pitch/yaw/roll. Keep roll: a LookAt with a fixed world-up loses camera roll.
inline void CameraBasis(Vec3 angles, Vec3 &forward, Vec3 &right, Vec3 &up) {
    float sp = std::sin(Radians(angles.x)), cp = std::cos(Radians(angles.x));
    float sy = std::sin(Radians(angles.y)), cy = std::cos(Radians(angles.y));
    float sr = std::sin(Radians(angles.z)), cr = std::cos(Radians(angles.z));
    forward = {cp * cy, cp * sy, -sp};
    right = {-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp};
    up = {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
}
using Matrix = std::array<float, 16>; // row-major, column vectors
inline Matrix Identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
inline Matrix Multiply(const Matrix &a, const Matrix &b) {
    Matrix r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k)
                r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
    return r;
}
inline Matrix ViewProjection(Vec3 origin, Vec3 angles, const Matrix &projection) {
    Vec3 f, r, u;
    CameraBasis(angles, f, r, u);
    Matrix v = {r.x,  r.y,  r.z,  -Dot(r, origin), u.x, u.y, u.z, -Dot(u, origin),
                -f.x, -f.y, -f.z, Dot(f, origin),  0,   0,   0,   1};
    return Multiply(projection, v);
}
inline bool Inverse(const Matrix &m, Matrix &result) {
    double a[4][8]{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j)
            a[i][j] = m[i * 4 + j];
        a[i][i + 4] = 1;
    }
    for (int j = 0; j < 4; ++j) {
        int p = j;
        for (int i = j + 1; i < 4; ++i)
            if (std::abs(a[i][j]) > std::abs(a[p][j]))
                p = i;
        if (!std::isfinite(a[p][j]) || std::abs(a[p][j]) < 1e-12)
            return false;
        for (int k = 0; k < 8; ++k)
            std::swap(a[p][k], a[j][k]);
        double n = a[j][j];
        for (int k = 0; k < 8; ++k)
            a[j][k] /= n;
        for (int i = 0; i < 4; ++i)
            if (i != j) {
                double s = a[i][j];
                for (int k = 0; k < 8; ++k)
                    a[i][k] -= s * a[j][k];
            }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            result[i * 4 + j] = float(a[i][j + 4]);
            if (!std::isfinite(result[i * 4 + j]))
                return false;
        }
    return true;
}
inline Vec3 Transform(const Matrix &m, Vec3 p) {
    float w = m[12] * p.x + m[13] * p.y + m[14] * p.z + m[15];
    return {(m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3]) / w,
            (m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7]) / w,
            (m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]) / w};
}
inline Matrix ModelMatrix(Vec3 p, Vec3 a, Vec3 s) {
    Vec3 f, r, u;
    CameraBasis(a, f, r, u);
    return {f.x * s.x, -r.x * s.y, u.x * s.z, p.x, f.y * s.x, -r.y * s.y, u.y * s.z, p.y,
            f.z * s.x, -r.z * s.y, u.z * s.z, p.z, 0,         0,          0,         1};
}
inline float Random01(uint32_t n) {
    n ^= n >> 16;
    n *= 0x7feb352dU;
    n ^= n >> 15;
    n *= 0x846ca68bU;
    n ^= n >> 16;
    return (n & 0xffffff) / 16777216.f;
}
inline float Wrap(double x, float extent) { return float(x - std::floor(x / extent) * extent); }
// Absolute demo time and seed, never accumulated dt. Seeking reconstructs the same field.
inline Vec3 WeatherPosition(uint32_t index, uint32_t seed, double time, Vec3 center, float radius,
                            float height, Vec3 velocity) {
    uint32_t n = index * 3 + seed * 7919;
    Vec3 low = {center.x - radius, center.y - radius, center.z - height * .5f};
    return {low.x + Wrap(Random01(n) * radius * 2 + time * velocity.x - low.x, radius * 2),
            low.y + Wrap(Random01(n + 1) * radius * 2 + time * velocity.y - low.y, radius * 2),
            low.z + Wrap(Random01(n + 2) * height + time * velocity.z - low.z, height)};
}
} // namespace Filmmaker::Environment
