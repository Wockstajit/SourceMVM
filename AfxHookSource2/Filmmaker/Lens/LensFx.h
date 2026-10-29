#pragma once

// LensFx: depth of field with a configurable iris (bokeh), and cinematic lens flare.
//
// A post pass over the finished world frame -- after the environment renderer, before the
// Panorama UI and before clean movie capture, so it is in every recorded frame and never touches
// the HUD. It reads the scene colour and the scene depth the engine already produced, so the blur
// follows real distances (not a screen-space guess), and it is a module of its own: nothing here
// depends on the environment editor being enabled.
//
//   main thread : settings (console, Config panel)       -> LensFx_Set / _RunCommand / _AppendStateJson
//   render thread: the passes                             -> LensFx_BeginDevice / _Render / _EndDevice
//
// Settings cross the thread boundary as a copied snapshot behind a mutex; the render thread never
// holds the lock while it records commands.
//
// See Docs/features/filmmaker_lens.md for the parameter contract and the pass list.

#include "LensFxState.h"

#include <d3d11.h>
#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// ---- render thread ----------------------------------------------------------------------------

// The smokes in front of the world this frame (see "smoke in front of the world" in LensFxMath.h).
// `absorbance` is CS2's OIT zeroth moment (R32, b0 per pixel), `spheres` holds `count` detonated
// smokes as centre xyz + radius, `camera` is the world position the frame is seen from. Any part may
// be missing; the DOF then treats the depth buffer as the whole truth, as it did before.
struct LensSmoke {
	ID3D11ShaderResourceView* absorbance = nullptr;
	UINT width = 0, height = 0;
	const float* spheres = nullptr;
	int count = 0;
	float camera[3] = {};
	bool haveCamera = false;
};

void LensFx_BeginDevice(ID3D11Device* device);
void LensFx_EndDevice();

// Runs the enabled effects over `target` in place. `worldToClip` is the engine's row-major
// world-to-clip matrix for this frame (16 floats); `depthMin`/`depthMax` are the world viewport's
// depth band (CS2 keeps the rest for the 3D skybox). Returns true when something was drawn.
// The sun: a unit direction toward it in world space (see "the sun" in LensFxMath.h).
// `keyPass` is true while the console green screen (render greenscreen) repaints the scene: the
// flare is then held back, because its additive ghosts, glow and dirt would paint over the chroma
// background and the keyed subject's edges, and nothing in the compositor could key them out again.
// A render job's Key, Depth and Normal passes never see the lens at all: they are built from the
// scene before it (RenderPassCompositor.h, KeepCleanColor).
// `holdFocus`: an extra render of a recorded frame. Autofocus keeps the distance the Main render
// found instead of chasing this render's own depth, so every Beauty plate of the frame shares one
// focus and the extra renders do not drag the next frame's focus toward a scene without the gun
// or the players.
struct LensSun {
	bool have = false;
	float dir[3] = {};
};

bool LensFx_Render(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target,
	ID3D11DepthStencilView* depth, const float* worldToClip, float depthMin, float depthMax,
	const LensSmoke& smoke, const LensSun& sun, bool keyPass, bool holdFocus);

} // namespace Filmmaker
