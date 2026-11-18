#pragma once
// THE WET FILM'S OFF-SCREEN REFLECTIONS. The screen trace (EnvironmentRainSurfaceShaders.h) only
// finds what is on screen, so a puddle seen from above lost the buildings it should mirror. Where
// the trace finds nothing, the film now shows CS2's own reflection probes (the map's baked 360
// captures) under the map's lighting, and the live capture through the bounce volume under
// Environment lighting. Both hold raw scene light while the film is painted on the finished frame,
// so a tone curve measured from CS2's own post-process each frame puts them into the frame's
// colours. Engine-free parts and their tests: EnvironmentReflectionProbeMath.h.
//
// Render thread throughout. Nothing is captured or measured unless the composite asked for it in
// the frame before (a wet film was drawn).
#include <d3d11.h>
#include <string>

namespace Filmmaker::Environment {

// The composite, each frame: whether it draws a wet film (so the next frame captures for it).
void ReflectionProbes_Want(bool wanted);
// Before every draw: catches CS2's probe records (cb3) and cube array from a lit world draw, once a
// present. Cheap when not wanted or already caught.
void ReflectionProbes_NoteDraw(ID3D11DeviceContext* ctx);
// Right after CS2's post_process draw: compares its raw input with its finished output at a grid
// of central pixels and folds that into the tone curve.
void ReflectionProbes_AfterTonemap(ID3D11DeviceContext* ctx);
void ReflectionProbes_EndPresent();

struct ReflectionProbeConstants {
    // x: 0 nothing to show, 1 CS2's probes, 2 the live capture; y: the cubes' highest mip;
    // z: how many probe records `index` lists; w: 1 when the tone curve is measured.
    float info[4]{};
    float index[16]{}; // probe record numbers, smallest box first
};
// For the composite: binds t51 (the cube array), t52 (the tone curve) and b8 (the probe rows) and
// returns its constants. environmentLighting: the Environment owns the map's lighting this frame.
ReflectionProbeConstants ReflectionProbes_Bind(ID3D11DeviceContext* ctx, const float camera[3], bool environmentLighting);
void ReflectionProbes_Release();
// One line for `environment status`.
std::string ReflectionProbes_Status();

} // namespace Filmmaker::Environment
