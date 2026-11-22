#pragma once
#include <d3d11.h>
#include <string>

namespace Filmmaker::Environment {
// CS2'S OWN SURFACE TEXTURES, read from the installed game at run time and never shipped: the snow
// the Cobblestone winter update laid on its streets (materials/cbble/snow/snow_mix_2: colour, normal,
// height) and the beads of water its wet surfaces use (materials/dev/water_droplets). The snow
// coating always draws with CS2's snow once it has loaded (plain until then); the wet ground carries
// the beads while the rainTexture switch is on.
//
// Render thread only. The first call that wants them starts one background read of the game's
// packages; until it finishes, and if it fails, nothing is bound and the result says so.
struct GameTexturesBound {
    bool snow = false, droplets = false;
};
// Binds what is ready and wanted to t45 (snow colour), t46 (snow normal), t47 (snow height),
// t48 (water droplets) and a wrapping sampler to s9.
GameTexturesBound BindGameTextures(ID3D11Device *device, ID3D11DeviceContext *ctx, bool wantSnow, bool wantDroplets);
// One line on what was found and loaded (environment status, the debug log).
std::string GameTexturesStatus();
// Device teardown.
void ReleaseGameTextures();
} // namespace Filmmaker::Environment
