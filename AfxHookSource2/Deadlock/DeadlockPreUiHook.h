#pragma once

// THE PRE-UI RENDER MOMENT: world drawn, Panorama not yet composited.
//
// This is the moment a correct preview needs, and Deadlock had no way to reach it. Its only render
// hook is `IDXGISwapChain::Present` (DeadlockPresentHook.cpp), which runs AFTER Panorama -- so the
// port scaled the preview by shrinking the engine's own viewport instead, which changes what the
// engine culls, never clears the remainder of the render target, and leaves the view block's other
// fields still claiming the full screen. Culled geometry, stale frames showing through and
// misplaced health bars all come from that one wrong choice.
//
// CS2 does it properly (AfxHookSource2/ViewportScaler.h): the world renders FULL-SCREEN with the
// camera and projection untouched, and at this callback a D3D pass copies the backbuffer, CLEARS
// it, and redraws it scaled into the preview rect. Full-screen render means culling is unchanged;
// the explicit clear means there is no remainder to go stale.
//
// HOW THE MOMENT IS REACHED. `CSceneSystem::CreateRenderContextPtr` is called once per render pass
// with a format string naming it. When that string is `#%s/SetupLightsAndViewConstants` and its
// first argument is the HUD's name, the HUD's render context is being set up -- the world is done.
// A render-thread callback queued on that context's software command list then runs at exactly the
// right point.
//
// EVERY VALUE HERE WAS MEASURED, NOT ASSUMED. Automation/verify/derive-deadlock-preui-hook.py
// derives all four offline from the shipped DLLs and writes
// reference/deadlock/offsets/preui-hook.json. Re-run it after a Deadlock update.
//
//   * the two CreateRenderContextPtr overloads -- found by the warning string they emit, then
//     pattern-checked for UNIQUENESS, because a pattern matching twice detours the wrong copy;
//   * the HUD pass name -- "CitadelHud", from client.dll, not guessed from CS2's "CSGOHud";
//   * the QueueCallback vtable slot -- 137 on this build, NOT CS2's 142. It was read off by
//     matching the same function across both games' vtables (49 identical bytes, margin 21 over
//     the next candidate) rather than carried over. A wrong slot here is silently catastrophic:
//     the callback never fires AND some unrelated virtual is called with a callback pointer.

#include <windows.h>

namespace Deadlock {

// Detours both CreateRenderContextPtr overloads in scenesystem.dll. Safe to call twice.
bool PreUiHook_Install(HMODULE sceneSystemDll);

bool PreUiHook_Installed();

// Register a function to run on the RENDER thread at the pre-UI moment. Passing nullptr clears it.
// The callback must do render-thread work only -- no Panorama, no console dispatch.
typedef void (*PreUiCallback_t)();
void PreUiHook_SetCallback(PreUiCallback_t callback);

// How many times each stage has fired. The three counters separate the three ways this can fail,
// which otherwise all present identically as "the preview does nothing":
//   contexts   the detour is installed and the engine is calling it at all
//   hudPasses  the pass name matched, so the HUD moment is being recognised
//   callbacks  the queued callback actually ran, so the vtable slot is right
void PreUiHook_Counters(unsigned long long& contexts,
                        unsigned long long& hudPasses,
                        unsigned long long& callbacks);

// Collect the distinct render-pass names the engine sets up, so the HUD's own name can be read off
// rather than guessed. Off by default: this path runs thousands of times a second.
void PreUiHook_SetCollectNames(bool on);
void PreUiHook_DumpNames();

void PreUiHook_Report();

} // namespace Deadlock
