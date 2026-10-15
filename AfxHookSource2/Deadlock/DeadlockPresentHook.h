#pragma once

// The render-thread pump.
//
// Deadlock runs D3D11 on Windows -- confirmed by inspecting the live process, which has
// rendersystemdx11.dll and d3d11.dll mapped and no Vulkan module at all. The render pump hangs off
// the swap chain's Present, reached through rendersystemdx11.dll's own CRenderDeviceDx11 rather
// than by hooking DXGI globally: the game owns exactly one device, and hooking its vtable cannot
// be confused by an overlay that also hooks DXGI.
//
// Nothing Panorama-related may run from here. V8 lives on the main thread, so a RunScript call
// from the render thread is a crash rather than a race.

#include <windows.h>

namespace Deadlock {

// Installs the present hook. Called when rendersystemdx11.dll loads.
bool PresentHook_Install(HMODULE renderSystemDll);

bool PresentHook_Installed();

// The swap chain the game is presenting, once seen. Null until the first present.
void* PresentHook_SwapChain();

} // namespace Deadlock
