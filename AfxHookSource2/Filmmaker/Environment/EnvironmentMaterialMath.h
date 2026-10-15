#pragma once
#include "EnvironmentMath.h"
#include <memory>
#include <string>
#include <vector>
namespace Filmmaker::Environment {
struct MaterialImage {
    unsigned width = 0, height = 0;
    std::vector<unsigned char> rgba;
};
struct SurfaceMaterial {
    bool enabled = false;
    float roughness = .5f, metallic = 0, coat = 0, coatRoughness = .12f, anisotropy = 0;
    float emission = 0, normalStrength = 1, uvScale = 1;
    std::string albedoTexture, normalTexture, roughnessTexture;
    std::shared_ptr<const MaterialImage> albedoImage, normalImage, roughnessImage;
};
inline float MaterialFresnel(float f0, float cosine) {
    return f0 + (1 - f0) * std::pow(1 - std::clamp(cosine, 0.f, 1.f), 5.f);
}
inline float MaterialGgxDistribution(float noH, float roughness) {
    float a = (std::max)(.0025f, roughness * roughness), a2 = a * a;
    float d = noH * noH * (a2 - 1) + 1;
    return a2 / (3.14159265359f * d * d);
}
inline bool ValidMaterialImage(const MaterialImage &image) {
    return image.width > 0 && image.height > 0 && image.width <= 4096 && image.height <= 4096 &&
           image.rgba.size() == size_t(image.width) * image.height * 4;
}
// Decode on the main thread before scene publication; rendering never opens files.
bool LoadMaterialImage(const std::wstring &path, MaterialImage &image, std::string &error);
// The same from encoded bytes in memory (a PNG or JPEG embedded in a GLB).
bool LoadMaterialImageFromMemory(const unsigned char *data, size_t size, MaterialImage &image, std::string &error);
} // namespace Filmmaker::Environment
