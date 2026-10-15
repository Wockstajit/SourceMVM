#pragma once

// Window/swapchain reporting and the legacy job-time resize attempt.
//
// Measured ownership map (CS2 build of 2026-08-01):
//   * the recorder captures a separate multisampled scene colour target, not the swapchain
//     backbuffer (`sameObject=0`);
//   * window client size drives normal viewport state, but spoofing GetClientRect changes only
//     selected consumers and creates a cropped or otherwise mixed state;
//   * a larger swapchain/backbuffer stretches into the real window but merely upscales the
//     unchanged scene target;
//   * SetWindowPos succeeds transiently, then CS2/SDL reasserts its configured video mode.
// Those three rejected levers remain documented in Docs/agent/render-system-contract.md.
//
// The opt-in MVM_RENDER_ENGINESIZE startup experiment (RenderEngineSize + RenderSupersample +
// RenderTargetProbe + RenderViewportRecordProbe) allocates and routes an enlarged scene target,
// but pixel inspection (2026-08-02) proved the world pass still renders only the source-sized
// corner -- it does NOT produce a usable larger capture. It stays env-gated and disconnected
// from the product; see Docs/agent/render-system-contract.md.
//
// PRODUCT RULE: a job always writes the game resolution; the render workspace has no output
// size (removed 2026-10-14). EngageOverride (the window-resize attempt) is no longer called by
// the session driver and remains only as RE documentation/console tooling.

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

struct RenderResolutionReport {
	bool haveSwapChain = false;
	unsigned swapWidth = 0, swapHeight = 0;
	unsigned swapBufferCount = 0;
	int swapEffect = -1;
	unsigned swapFlags = 0;
	bool swapWindowed = false;
	// From IDXGISwapChain1::GetDesc1 -- not present in legacy DXGI_SWAP_CHAIN_DESC. This confirmed
	// why an enlarged backbuffer could stretch into the real window without enlarging the scene.
	bool haveDesc1 = false;
	int swapScaling = -1;
	int swapAlphaMode = -1;

	bool haveBackBuffer = false;
	unsigned backWidth = 0, backHeight = 0;
	unsigned backSampleCount = 0;
	int backFormat = 0;

	bool haveWindow = false;
	int clientWidth = 0, clientHeight = 0;
	int windowWidth = 0, windowHeight = 0;

	// True when the swapchain description, the actual backbuffer texture and the window client
	// area do NOT all agree. Any disagreement is the interesting case: it tells us which of the
	// three the engine is really sizing its render targets from.
	bool Disagrees() const;
};

// Samples the live D3D/Win32 state. Safe to call from the main thread at any time; returns a
// report with `have*` flags cleared for anything not available yet (no device, no demo, etc.).
//
// NOT SAFE TO POLL EVERY FRAME. It calls IDXGISwapChain::GetBuffer, which takes a reference on the
// backbuffer. `ResizeBuffers` fails outright while any backbuffer reference is outstanding, so a
// per-frame sampler from the main thread can make the engine's own swapchain resize fail and
// corrupt presentation. Use it for one-shot reports; use the accessor below for polling.
RenderResolutionReport RenderResolution_Sample();

// The swapchain's CURRENT buffer size, read from the swapchain description only -- no backbuffer
// reference is taken, so this is safe to poll while the engine may be resizing. ResizeBuffers
// updates the description, so this still answers "has the engine changed size yet?".
bool RenderResolution_SwapChainSize(unsigned& outWidth, unsigned& outHeight);

// Multi-line human-readable dump of the above, for the console.
std::string RenderResolution_Describe(const RenderResolutionReport& report);

// -- stage 2: the override itself (main thread) --

// Engages the client-rect override at width x height and pokes the engine's resize path.
// Returns false (with outError set) when preconditions are missing (no window/swapchain yet,
// size out of range). Success only means the attempt is armed -- poll OverrideSettled().
bool RenderResolution_EngageOverride(int width, int height, std::string& outError);

// Restores truthful client-rect reporting and pokes the resize path again so the engine
// returns to the real window size. Safe to call when not engaged.
void RenderResolution_DisengageOverride();

bool RenderResolution_OverrideActive();

// True when the live backbuffer matches the override target (the engine followed the lie).
bool RenderResolution_OverrideSettled();

// "mirv_filmmaker render res ..." -- report | probe on|off | override <w> <h> | override off.
void RenderResolution_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// Main thread, once per frame. While the probe is enabled, emits a change-gated `render.res` line
// into the mvm_debug log, so a resolution change (or a disagreement appearing mid-session) is
// traceable rather than something you have to catch by running the report at the right moment.
void RenderResolution_PumpMainThread();

} // namespace Filmmaker
