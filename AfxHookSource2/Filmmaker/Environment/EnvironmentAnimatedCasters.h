#pragma once
#include <d3d11.h>
#include <cstddef>
#include <cstdint>
#include "EnvironmentMath.h"
#include "EnvironmentCasterBoundsMath.h"

namespace Filmmaker::Environment {
enum class AnimatedCasterSubset { All, Characters, Weapons, Viewmodel, Scene, Actors, FoliageOnly }; // Actors = Characters + Weapons: every posed player and what they hold
// Captures native, already-skinned opaque character/world-weapon triangles. This
// does NOT bypass engine visibility culling; never describe it as all-player coverage.
void NoteAnimatedCasterPixelShader(ID3D11PixelShader*, const void*, size_t);
void NoteAnimatedCasterShader(ID3D11VertexShader*, const void*, size_t);
void BeginAnimatedCasters(ID3D11Device*);
void EndAnimatedCasters();
void RequestAnimatedCasters(bool);
void NextAnimatedCasterFrame();
// Supplies the current receiver camera projection for the bounded caster probe.
// This is diagnostics-only; it does not alter the native replay or shadow map.
void NoteAnimatedCasterReceiverProjection(const Matrix &worldToClip);
// Brackets one replay of the ORIGINAL indexed native draw. No rasterization and
// no mutation of native vertices, poses, constants, movement, or camera state.
bool BeginAnimatedCasterDraw(ID3D11DeviceContext*, UINT indices, UINT instances);
// Only verified skinned opaque/alpha-masked depth prepasses; GPU-resident argument counting.
// The caller replays the original GPU indirect command between Begin/End.
bool BeginAnimatedCasterIndirectDraw(ID3D11DeviceContext*,ID3D11Buffer*,UINT offset,UINT stride,UINT count,bool indexed);
void EndAnimatedCasterDraw(ID3D11DeviceContext*);
uint64_t AnimatedCasterGeneration();
UINT AnimatedCasterVertexCount(AnimatedCasterSubset subset = AnimatedCasterSubset::All);
// Includes GPU-counted batches whose vertex count is deliberately not read by the CPU.
bool HasAnimatedCasterGeometry();
// Current stream-output geometry bounds. A false result means draw conservatively.
bool ReadAnimatedCasterBounds(ID3D11DeviceContext*,std::vector<CasterBounds>&,bool waitForGpu=true);
// Caller supplies depth target and raster/depth state; this binds its coverage shaders.
void DrawAnimatedCoverageCasters(ID3D11DeviceContext*, const Matrix& worldToShadow);
// Caller supplies its depth shader/layout, transform, depth target and raster state.
void DrawAnimatedCasters(ID3D11DeviceContext*, AnimatedCasterSubset subset = AnimatedCasterSubset::All);
}
