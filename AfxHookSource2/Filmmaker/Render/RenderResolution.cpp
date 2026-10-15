#include "RenderResolution.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_* (render.res probe)

#include "../../WindowInputHooks.h"        // g_UiCursorProbe.hwnd -- the game window we hook
#include "../../WrpConsole.h"

#include "../../../Shared/AfxConsole.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <sstream>
#include <windows.h>

// Owned by RenderSystemDX11Hooks.cpp: captured when the engine's swapchain is first seen. May be
// null (no device yet, or an anti-cheat blocked the hook -- see the note at its definition).
extern IDXGISwapChain* g_pSwapChain;

namespace Filmmaker {

namespace {

bool s_probeEnabled = false;

const char* ScalingName(int scaling) {
	switch (scaling) {
	case DXGI_SCALING_STRETCH: return "STRETCH (a bigger backbuffer WILL scale into the window)";
	case DXGI_SCALING_NONE: return "NONE (a bigger backbuffer would be CLIPPED, not scaled)";
	case DXGI_SCALING_ASPECT_RATIO_STRETCH: return "ASPECT_RATIO_STRETCH (scales, letterboxed)";
	default: return "?";
	}
}

const char* SwapEffectName(int effect) {
	switch (effect) {
	case DXGI_SWAP_EFFECT_DISCARD: return "DISCARD";
	case DXGI_SWAP_EFFECT_SEQUENTIAL: return "SEQUENTIAL";
	case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL: return "FLIP_SEQUENTIAL";
	case DXGI_SWAP_EFFECT_FLIP_DISCARD: return "FLIP_DISCARD";
	default: return "?";
	}
}

} // namespace

bool RenderResolutionReport::Disagrees() const {
	if (haveSwapChain && haveBackBuffer && (swapWidth != backWidth || swapHeight != backHeight))
		return true;
	if (haveBackBuffer && haveWindow
		&& ((int)backWidth != clientWidth || (int)backHeight != clientHeight))
		return true;
	return false;
}

bool RenderResolution_SwapChainSize(unsigned& outWidth, unsigned& outHeight) {
	outWidth = 0;
	outHeight = 0;
	if (!g_pSwapChain)
		return false;
	DXGI_SWAP_CHAIN_DESC desc = {};
	if (FAILED(g_pSwapChain->GetDesc(&desc)))
		return false;
	outWidth = desc.BufferDesc.Width;
	outHeight = desc.BufferDesc.Height;
	return outWidth > 0 && outHeight > 0;
}

RenderResolutionReport RenderResolution_Sample() {
	RenderResolutionReport out;

	if (g_pSwapChain) {
		DXGI_SWAP_CHAIN_DESC desc = {};
		if (SUCCEEDED(g_pSwapChain->GetDesc(&desc))) {
			out.haveSwapChain = true;
			out.swapWidth = desc.BufferDesc.Width;
			out.swapHeight = desc.BufferDesc.Height;
			out.swapBufferCount = desc.BufferCount;
			out.swapEffect = (int)desc.SwapEffect;
			out.swapFlags = desc.Flags;
			out.swapWindowed = desc.Windowed != FALSE;
		}
		IDXGISwapChain1* pSwapChain1 = nullptr;
		if (SUCCEEDED(g_pSwapChain->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&pSwapChain1))
			&& pSwapChain1) {
			DXGI_SWAP_CHAIN_DESC1 d1 = {};
			if (SUCCEEDED(pSwapChain1->GetDesc1(&d1))) {
				out.haveDesc1 = true;
				out.swapScaling = (int)d1.Scaling;
				out.swapAlphaMode = (int)d1.AlphaMode;
			}
			pSwapChain1->Release();
		}
		// The DESCRIPTION can lag the real buffers (the engine may have resized them), so read the
		// actual texture too -- a disagreement between these two is exactly what stage 2 needs to
		// know about before it touches anything.
		ID3D11Texture2D* pBackBuffer = nullptr;
		if (SUCCEEDED(g_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer))
			&& pBackBuffer) {
			D3D11_TEXTURE2D_DESC td = {};
			pBackBuffer->GetDesc(&td);
			out.haveBackBuffer = true;
			out.backWidth = td.Width;
			out.backHeight = td.Height;
			out.backSampleCount = td.SampleDesc.Count;
			out.backFormat = (int)td.Format;
			pBackBuffer->Release();
		}
	}

	HWND hwnd = g_UiCursorProbe.hwnd;
	if (hwnd && ::IsWindow(hwnd)) {
		RECT client = {}, window = {};
		if (::GetClientRect(hwnd, &client) && ::GetWindowRect(hwnd, &window)) {
			out.haveWindow = true;
			out.clientWidth = client.right - client.left;
			out.clientHeight = client.bottom - client.top;
			out.windowWidth = window.right - window.left;
			out.windowHeight = window.bottom - window.top;
		}
	}

	return out;
}

std::string RenderResolution_Describe(const RenderResolutionReport& r) {
	std::ostringstream o;
	o << "render resolution:\n";
	if (r.haveSwapChain) {
		o << "  swapchain desc : " << r.swapWidth << "x" << r.swapHeight
			<< "  buffers=" << r.swapBufferCount
			<< "  effect=" << SwapEffectName(r.swapEffect)
			<< "  flags=0x" << std::hex << r.swapFlags << std::dec
			<< "  windowed=" << (r.swapWindowed ? "yes" : "no") << "\n";
	} else {
		o << "  swapchain desc : unavailable (no swapchain captured yet)\n";
	}
	if (r.haveDesc1) {
		o << "  swapchain scale: " << ScalingName(r.swapScaling)
			<< "  alphaMode=" << r.swapAlphaMode << "\n";
	} else {
		o << "  swapchain scale: unavailable (no IDXGISwapChain1)\n";
	}
	if (r.haveBackBuffer) {
		o << "  backbuffer tex : " << r.backWidth << "x" << r.backHeight
			<< "  samples=" << r.backSampleCount
			<< "  format=" << r.backFormat << "\n";
	} else {
		o << "  backbuffer tex : unavailable\n";
	}
	if (r.haveWindow) {
		o << "  window client  : " << r.clientWidth << "x" << r.clientHeight << "\n";
		o << "  window outer   : " << r.windowWidth << "x" << r.windowHeight << "\n";
	} else {
		o << "  window         : unavailable (no HWND captured yet)\n";
	}
	o << "  agreement      : " << (r.Disagrees() ? "MISMATCH (see above)" : "all sizes agree") << "\n";
	o << "  note           : this command reports the window/swapchain path only. The separate\n"
		 "                   startup MVM_RENDER_ENGINESIZE experiment scales the captured scene target.\n";
	return o.str();
}

void RenderResolution_PumpMainThread() {
	if (!s_probeEnabled || !MvmDebugLog_Active())
		return;
	const RenderResolutionReport r = RenderResolution_Sample();
	// Change-gated, not throttled: what matters is every transition (a resize, the device
	// appearing, a mismatch opening up), and a sampled probe would miss the exact frame one
	// happened -- the same rule the other ui.render.* probes follow.
	static unsigned s_lastSig = 0xFFFFFFFFu;
	const unsigned sig = (r.backWidth * 31u + r.backHeight) * 31u
		+ (unsigned)r.clientWidth * 7u + (unsigned)r.clientHeight
		+ (r.haveSwapChain ? 1u : 0u) + (r.Disagrees() ? 2u : 0u);
	if (sig == s_lastSig)
		return;
	s_lastSig = sig;
	MvmDebugLog_Linef("render.res",
		"swap=%ux%u back=%ux%u client=%dx%d outer=%dx%d windowed=%d samples=%u mismatch=%d",
		r.swapWidth, r.swapHeight, r.backWidth, r.backHeight,
		r.clientWidth, r.clientHeight, r.windowWidth, r.windowHeight,
		r.swapWindowed ? 1 : 0, r.backSampleCount, r.Disagrees() ? 1 : 0);
}

// -- stage 2: the override --

namespace {

bool s_overrideActive = false;
int s_overrideW = 0;
int s_overrideH = 0;
RECT s_savedWindowRect = {};
bool s_haveSavedWindowRect = false;

// The legacy override attempts to resize the game window to the target client size. Verified
// live 2026-08-01 that neither this nor the client-rect spoof can do it reliably:
//   * SDL3+rsdx11 hooks only  -> Panorama re-laid out but the swapchain never resized (mixed
//     state, resolve-rect error spam);
//   * adding engine2          -> nothing resized at all within 240 frames.
// SetWindowPos itself returns success, but CS2/SDL restores its configured video mode within a
// frame or two. The caller must therefore use OverrideSettled and fall back honestly when the
// requested size never persists. The g_AfxClientRectOverride plumbing remains inert.
bool ResizeGameWindowClient(HWND hwnd, int clientW, int clientH, const char* why) {
	RECT want = { 0, 0, clientW, clientH };
	const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
	const LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	if (!::AdjustWindowRectEx(&want, (DWORD)style, FALSE, (DWORD)exStyle))
		return false;
	const int outerW = want.right - want.left;
	const int outerH = want.bottom - want.top;
	const BOOL ok = ::SetWindowPos(hwnd, nullptr, 0, 0, outerW, outerH,
		SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.res", "window resize (%s) client=%dx%d outer=%dx%d ok=%d",
			why, clientW, clientH, outerW, outerH, ok ? 1 : 0);
	return ok != FALSE;
}

} // namespace

bool RenderResolution_EngageOverride(int width, int height, std::string& outError) {
	outError.clear();
	if (width < 64 || height < 64 || width > 16384 || height > 16384) {
		outError = "override size out of range (64..16384)";
		return false;
	}
	HWND hwnd = g_UiCursorProbe.hwnd;
	if (!hwnd || !::IsWindow(hwnd)) {
		outError = "no game window captured yet";
		return false;
	}
	if (!g_pSwapChain) {
		outError = "no swapchain captured yet";
		return false;
	}

	const RenderResolutionReport before = RenderResolution_Sample();

	if (!s_haveSavedWindowRect) {
		if (!::GetWindowRect(hwnd, &s_savedWindowRect)) {
			outError = "could not read the current window rect";
			return false;
		}
		s_haveSavedWindowRect = true;
	}

	s_overrideActive = true;
	s_overrideW = width;
	s_overrideH = height;

	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.res",
			"override ENGAGE %dx%d (back was %ux%u client %dx%d)",
			width, height, before.backWidth, before.backHeight,
			before.clientWidth, before.clientHeight);

	if (!ResizeGameWindowClient(hwnd, width, height, "engage")) {
		s_overrideActive = false;
		outError = "window resize was refused";
		return false;
	}
	return true;
}

void RenderResolution_DisengageOverride() {
	if (!s_overrideActive && !s_haveSavedWindowRect)
		return;
	s_overrideActive = false;
	// Inert spoof plumbing: clear it in case a future path armed it.
	g_AfxClientRectOverride.active = false;
	g_AfxClientRectOverride.width = 0;
	g_AfxClientRectOverride.height = 0;
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.res", "override DISENGAGE");
	HWND hwnd = g_UiCursorProbe.hwnd;
	if (s_haveSavedWindowRect && hwnd && ::IsWindow(hwnd)) {
		::SetWindowPos(hwnd, nullptr, s_savedWindowRect.left, s_savedWindowRect.top,
			s_savedWindowRect.right - s_savedWindowRect.left,
			s_savedWindowRect.bottom - s_savedWindowRect.top,
			SWP_NOZORDER | SWP_NOACTIVATE);
	}
	s_haveSavedWindowRect = false;
}

bool RenderResolution_OverrideActive() {
	return s_overrideActive;
}

bool RenderResolution_OverrideSettled() {
	if (!s_overrideActive)
		return false;
	const RenderResolutionReport r = RenderResolution_Sample();
	return r.haveBackBuffer
		&& (int)r.backWidth == s_overrideW
		&& (int)r.backHeight == s_overrideH;
}

void RenderResolution_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* sub = (argc >= 4) ? args->ArgV(3) : "report";

	if (0 == _stricmp(sub, "report")) {
		const RenderResolutionReport r = RenderResolution_Sample();
		advancedfx::Message("%s", RenderResolution_Describe(r).c_str());
		if (s_overrideActive)
			advancedfx::Message("  override       : ACTIVE %dx%d, %s\n", s_overrideW, s_overrideH,
				RenderResolution_OverrideSettled() ? "settled (backbuffer matches)" : "NOT settled yet");
		return;
	}
	if (0 == _stricmp(sub, "probe")) {
		if (argc >= 5)
			s_probeEnabled = (0 != atoi(args->ArgV(4)));
		else
			s_probeEnabled = !s_probeEnabled;
		advancedfx::Message("%s: render.res probe %s (needs \"mvm_debug start\" to write anything).\n",
			cmd, s_probeEnabled ? "ON" : "off");
		return;
	}
	if (0 == _stricmp(sub, "override")) {
		if (argc >= 5 && 0 == _stricmp(args->ArgV(4), "off")) {
			RenderResolution_DisengageOverride();
			advancedfx::Message("%s: resolution override off.\n", cmd);
			return;
		}
		if (argc >= 6) {
			std::string err;
			if (RenderResolution_EngageOverride(atoi(args->ArgV(4)), atoi(args->ArgV(5)), err))
				advancedfx::Message("%s: resolution override engaged (%s x %s). Check with \"%s render res report\".\n",
					cmd, args->ArgV(4), args->ArgV(5), cmd);
			else
				advancedfx::Warning("%s: override failed: %s.\n", cmd, err.c_str());
			return;
		}
		advancedfx::Message("usage: %s render res override <width> <height> | override off\n", cmd);
		return;
	}

	advancedfx::Message(
		"%s render res report            - dump swapchain / backbuffer / window sizes.\n"
		"%s render res probe 0|1         - log every size change to the mvm_debug log.\n"
		"%s render res override <w> <h>  - attempt the legacy window resize; check settled state.\n"
		"%s render res override off      - back to the real window size.\n",
		cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker
