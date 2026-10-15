#include "RenderSupersample.h"
#include "RenderEngineSize.h"

#include "../Debug/MvmDebugLog.h"

#include "../../../Shared/AfxConsole.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d11.h>
#include <windows.h>

namespace Filmmaker {

namespace {

std::atomic<float> s_factor{ 1.0f };
bool s_envChecked = false;

// The pre-scale scene size, learned from the first scene target we enlarge. Viewports are only
// scaled when they match this, so unrelated viewports (shadow maps, UI, small utility passes)
// are left completely alone.
std::atomic<unsigned> s_baseWidth{ 0 };
std::atomic<unsigned> s_baseHeight{ 0 };

void EnsureEnvOnce() {
	if (s_envChecked) return;
	s_envChecked = true;
	char buf[16] = {0};
	DWORD n = ::GetEnvironmentVariableA("MVM_RENDER_SS", buf, (DWORD)sizeof(buf));
	if (n == 0 || n >= sizeof(buf)) return;
	const double v = atof(buf);
	if (v > 1.0 && v <= 4.0) {
		s_factor.store((float)v);
		advancedfx::Message("SOURCE:MVM: MVM_RENDER_SS=%s -- scene supersampling x%.2f is ACTIVE.\n", buf, v);
	}
}

// Scene-chain targets: multisampled, full-size, and bound as a render target OR a depth-stencil.
//
// The DEPTH-STENCIL half is not optional, and leaving it out is what made the first attempt
// capture pure black: D3D11 requires every bound render target AND the depth-stencil view to
// have identical dimensions, so scaling only the colour targets left the depth buffer at the old
// size, the runtime rejected the binding, and nothing was drawn at all.
bool IsSceneTarget(const D3D11_TEXTURE2D_DESC& d) {
	if (d.SampleDesc.Count < 2) return false;
	const bool isRt = 0 != (d.BindFlags & D3D11_BIND_RENDER_TARGET);
	const bool isDs = 0 != (d.BindFlags & D3D11_BIND_DEPTH_STENCIL);
	if (!isRt && !isDs) return false;
	if (d.Width < 256 || d.Height < 256) return false;
	if (d.ArraySize != 1 || d.MipLevels != 1) return false;
	return true;
}

unsigned ScaleDim(unsigned v, float f) {
	unsigned out = (unsigned)(v * f + 0.5f);
	out &= ~1u; // keep dimensions even
	return out ? out : v;
}

bool ViewportScaleParameters(
	float& factor, unsigned& baseWidth, unsigned& baseHeight,
	unsigned& targetWidth, unsigned& targetHeight) {
	// The metadata-consistent engine-size path owns the target when enabled. Its measured source
	// and output pair also avoids drift from independently applying/rounding the factor here.
	if (RenderEngineSize_Factor() > 1.001f) {
		factor = RenderEngineSize_Factor();
		if (RenderEngineSize_TargetDimensions(
			baseWidth, baseHeight, targetWidth, targetHeight))
			return true;
		baseWidth = s_baseWidth.load();
		baseHeight = s_baseHeight.load();
		targetWidth = ScaleDim(baseWidth, factor);
		targetHeight = ScaleDim(baseHeight, factor);
		return baseWidth != 0 && baseHeight != 0;
	}

	factor = RenderSupersample_Factor();
	baseWidth = s_baseWidth.load();
	baseHeight = s_baseHeight.load();
	targetWidth = ScaleDim(baseWidth, factor);
	targetHeight = ScaleDim(baseHeight, factor);
	return factor > 1.001f && baseWidth != 0 && baseHeight != 0;
}

} // namespace

float RenderSupersample_Factor() {
	EnsureEnvOnce();
	return s_factor.load();
}

bool RenderSupersample_Active() {
	return RenderSupersample_Factor() > 1.001f;
}

bool RenderSupersample_TextureAdjustmentActive() {
	if (RenderEngineSize_Factor() > 1.001f) return true;
	return RenderSupersample_Active();
}

bool RenderSupersample_ViewportAdjustmentActive() {
	if (RenderEngineSize_Factor() > 1.001f) return true;
	return RenderSupersample_Active();
}

bool RenderSupersample_AdjustTextureDesc(D3D11_TEXTURE2D_DESC& desc) {
	const float engineFactor = RenderEngineSize_Factor();
	const float f = engineFactor > 1.001f ? engineFactor : RenderSupersample_Factor();
	if (f <= 1.001f) return false;
	if (!IsSceneTarget(desc)) return false;

	// Learn the base size from the first scene target seen, so viewport matching has a reference.
	unsigned expected = s_baseWidth.load();
	if (expected == 0) {
		s_baseWidth.store(desc.Width);
		s_baseHeight.store(desc.Height);
	} else if (desc.Width != expected || desc.Height != s_baseHeight.load()) {
		// A differently sized multisampled target: not the scene chain we measured. Leave it.
		return false;
	}

	const unsigned oldW = desc.Width, oldH = desc.Height;
	desc.Width = ScaleDim(oldW, f);
	desc.Height = ScaleDim(oldH, f);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways(
			engineFactor > 1.001f ? "render.enginesize.texture" : "render.ss",
			"scene target %ux%u -> %ux%u (x%.2f fmt=%d samples=%u)",
			oldW, oldH, desc.Width, desc.Height, f, (int)desc.Format, desc.SampleDesc.Count);
	return true;
}

bool RenderSupersample_AdjustViewport(D3D11_VIEWPORT& vp) {
	float f = 1.0f;
	unsigned baseW = 0, baseH = 0, targetW = 0, targetH = 0;
	if (!ViewportScaleParameters(f, baseW, baseH, targetW, targetH)) return false;

	// Only the full-scene viewport is scaled. Anything of another size belongs to a pass we did
	// not enlarge, and scaling it would corrupt that pass.
	if ((unsigned)(vp.Width + 0.5f) != baseW || (unsigned)(vp.Height + 0.5f) != baseH)
		return false;

	vp.TopLeftX *= (float)targetW / (float)baseW;
	vp.TopLeftY *= (float)targetH / (float)baseH;
	vp.Width = (float)targetW;
	vp.Height = (float)targetH;

	if (MvmDebugLog_Active()) {
		static int s_throttle = 0;
		if (0 == (s_throttle++ % 600))
			MvmDebugLog_Linef("render.viewportscale", "viewport scaled to %.0fx%.0f (x%.2f)",
				vp.Width, vp.Height, f);
	}
	return true;
}

bool RenderSupersample_AdjustScissor(RECT& rect) {
	float f = 1.0f;
	unsigned baseW = 0, baseH = 0, targetW = 0, targetH = 0;
	if (!ViewportScaleParameters(f, baseW, baseH, targetW, targetH)) return false;

	// Scale only the exact full-scene clip. CS2 also emits a near-full variant one pixel short on
	// the right or bottom (measured: 0,0..1600,1199); scaling that too was screenshot-rejected
	// (seams, shifted tiles, oversized HUD) because depth/culling/post passes still disagree about
	// the view extent. The upstream view-rect owner must be routed instead of repairing scissors
	// here after the fact.
	if (rect.left != 0 || rect.top != 0) return false;
	if (rect.right != (LONG)baseW || rect.bottom != (LONG)baseH) return false;

	const float scaleX = (float)targetW / (float)baseW;
	const float scaleY = (float)targetH / (float)baseH;
	rect.right = (LONG)std::ceil((float)rect.right * scaleX);
	rect.bottom = (LONG)std::ceil((float)rect.bottom * scaleY);

	if (MvmDebugLog_Active()) {
		static int s_throttle = 0;
		if (0 == (s_throttle++ % 600))
			MvmDebugLog_Linef("render.viewportscale", "scissor scaled to %ld,%ld..%ld,%ld (x%.2f)",
				rect.left, rect.top, rect.right, rect.bottom, f);
	}
	return true;
}

void RenderSupersample_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	EnsureEnvOnce();
	if (argc < 4) {
		advancedfx::Message(
			"%s: scene supersampling is x%.2f (base scene size %ux%u).\n"
			"  %s render supersample <factor 1..4> | off\n"
			"  EXPERIMENTAL: renders the scene at <factor> times the game resolution so a capture has\n"
			"  genuinely more pixels, without resizing the window. The engine may fail to resolve the\n"
			"  enlarged target to the window (black frame / resolve errors) -- turn it off if so.\n"
			"  Set MVM_RENDER_SS=<factor> before launch to apply it from startup (targets are created then).\n",
			cmd, s_factor.load(), s_baseWidth.load(), s_baseHeight.load(), cmd);
		return;
	}
	const char* v = args->ArgV(3);
	if (0 == _stricmp(v, "off")) {
		s_factor.store(1.0f);
		advancedfx::Message("%s: supersampling off (already-created targets keep their size until "
			"they are recreated -- restart the game for a clean state).\n", cmd);
		return;
	}
	const double f = atof(v);
	if (!(f >= 1.0 && f <= 4.0)) {
		advancedfx::Warning("%s: factor must be between 1 and 4.\n", cmd);
		return;
	}
	s_factor.store((float)f);
	advancedfx::Message("%s: supersampling x%.2f. NOTE: the scene targets already exist at the old "
		"size; this only takes effect when they are recreated. Use MVM_RENDER_SS=%s before launch "
		"for a from-startup run.\n", cmd, f, v);
}

} // namespace Filmmaker
