#pragma once

// Finds WHO sizes CS2's scene render target, by name and module offset.
//
// WHY THIS EXISTS. Black-box experiments established what the scene render target's size
// FOLLOWS (the engine video mode) but not WHERE that decision is made, so there was nothing to
// hook. This probe answers that directly: it detours ID3D11Device::CreateTexture2D, waits for a
// texture matching the scene-target signature (multisampled colour target), and captures the
// call stack that created it -- resolved to module + offset, which is exactly what a
// disassembler needs as an entry point.
//
// It is a DIAGNOSTIC, not a feature: it changes no behaviour and allocates nothing per frame.
// Armed explicitly with `mirv_filmmaker render rtprobe on`, then reproduced by anything that
// re-creates render targets (a resolution change, a map load). Results go to the mvm_debug log
// under `render.rtcreate`.
//
// Threading: CreateTexture2D is called on whichever thread the engine creates resources on; the
// mvm_debug writer is mutex-guarded and stamps the thread id.

struct ID3D11Device;

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Installs the CreateTexture2D detour on the engine's device. Safe to call repeatedly and with
// a null device (no-op). Called when the render system captures/replaces the device.
void RenderTargetProbe_AttachDevice(ID3D11Device* pDevice);

// `mirv_filmmaker render rtprobe on|off|status`.
void RenderTargetProbe_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

bool RenderTargetProbe_Enabled();

// GPU memory this DLL currently holds in buffers and textures (staging excluded), estimated from
// each creation's description and released when the resource is destroyed. Any thread.
void RenderOwnedMemory_Totals(unsigned long long& bytes, unsigned& resources);
// Console: the total plus the `top` creating call sites by live bytes, as DLL offsets.
void RenderOwnedMemory_Print(unsigned top);

} // namespace Filmmaker
