#pragma once
// The irradiance atlas live relighting feeds to CS2's world shaders in place of the map's baked
// one. The layout (which world point every atlas texel is) comes from the compiled map through
// MapDataTool lightmap (Filmmaker/MapData/MapLightmap); the values are computed here and nothing baked is used.
#include "RelightMath.h"
#include <d3d11.h>
#include <string>

namespace Filmmaker::Relight {

struct LightmapParams {
    // 0 flat fill, 1 world-space checker (proves the layout), 2 lit from the world's own light field.
    unsigned mode = 0;
    Vec3 fill{};
    uint64_t generation = 0;
};

// Starts reading the layout file on a worker thread. Repeating the same path is free.
void LightmapRequest(const std::string &path);
// Render thread only. Returns the atlas as a texture-array view to bind where the shader expects
// its lightmap, or nullptr until the layout has loaded and the first fill has been drawn.
ID3D11ShaderResourceView *LightmapAtlas(ID3D11Device *device, ID3D11DeviceContext *immediate,
                                        const LightmapParams &params);
std::string LightmapStatus();
void EndLightmap();

} // namespace Filmmaker::Relight
