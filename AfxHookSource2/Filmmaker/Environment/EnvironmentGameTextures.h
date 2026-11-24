#pragma once
#include <d3d11.h>
#include <string>

namespace Filmmaker::Environment {
// THE GROUND COATINGS' TEXTURES. CS2's own, read from the installed game at run time and never
// shipped: the snow the Cobblestone winter update laid on its streets (materials/cbble/snow/snow_mix_2:
// colour, normal, height) and the beads of water its wet surfaces use (materials/dev/water_droplets).
// And the sand, which CS2 has no material for: four CC0 Poly Haven sand texture packs, named in
// the UI by their look (Dusty Mix, a blend; Cracked Crust, sand_01; Pebbly Grit, coast_sand_02;
// Trampled, park_sand), shipped beside the hook in
// resources/environment/sand, packed the way CS2's snow is (EnvironmentSandTextureMath.h), so
// the sand coating draws through the snow's path. The snow coating always draws with CS2's snow once it
// has loaded (plain until then), the sand with its selected style; the wet ground carries the beads
// while the rainTexture switch is on.
//
// Render thread only. The first call that wants a set starts one background read of the game's
// packages (the sand: of that style's three PNGs); until it finishes, and if it fails, nothing is bound
// (or the previously loaded Dusty Mix sand stays bound) and the result says so.
inline constexpr int kSandStyleCount = 4;
struct GameTexturesBound {
    bool snow = false, sand = false, droplets = false;
};
// Binds what is ready and wanted to t45 (colour), t46 (normal), t47 (height), t48 (water droplets)
// and a wrapping sampler to s9. The ground's three slots hold the selected sand style when `wantSand`,
// else the snow.
GameTexturesBound BindGameTextures(ID3D11Device *device, ID3D11DeviceContext *ctx, bool wantSnow, bool wantSand,
                                   bool wantDroplets, int sandStyle = 0);
// Units one repeat of each ground texture covers.
inline constexpr float kGameSnowTile = 128.f, kSandTile = 112.f;
// One line on what was found and loaded (environment status, the debug log).
std::string GameTexturesStatus();
// Device teardown.
void ReleaseGameTextures();
} // namespace Filmmaker::Environment
