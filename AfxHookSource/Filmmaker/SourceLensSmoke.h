#pragma once
#include <d3d9.h>
#include <array>

namespace SourceFilmmaker {
struct LensSmokeFrame {
    std::array<std::array<float,4>,8> spheres{};
    std::array<float,3> camera{};
    unsigned count=0;
};
// Render-thread only. The depth helper's actual smoke shader/geometry writes its
// transmittance into alpha; opaque scene depth still rejects hidden particles.
void LensSmoke_Begin(IDirect3DDevice9* device,IDirect3DSurface9* target);
void LensSmoke_Material(bool smoke);
bool LensSmoke_BeforeDraw(IDirect3DDevice9* device);
void LensSmoke_AfterDraw();
unsigned LensSmoke_End();
}
