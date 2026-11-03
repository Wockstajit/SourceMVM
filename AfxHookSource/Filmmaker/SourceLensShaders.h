#pragma once

// D3D9 syntax adapter for the shared CS2 lens shaders. Effect maths stays in LensFxShaders.h.
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace SourceFilmmaker::LensShaders {
const std::array<const char*,21>& Entries();
std::string Source();
struct Binding {
    std::string name;
    unsigned set = 0, first = 0, count = 0; // D3DXRS_FLOAT4=2, D3DXRS_SAMPLER=3
};
// SM3 embeds its register bindings in a bounded CTAB comment. No D3DX runtime dependency.
bool ReadBindings(const void* bytecode,size_t size,std::vector<Binding>& out);
} // namespace SourceFilmmaker::LensShaders
