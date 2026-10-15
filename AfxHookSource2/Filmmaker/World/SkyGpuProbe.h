#pragma once

#include <cstddef>

// Bounded DirectX draw probe for the sky-orientation investigation.
//
// The scene-object detour never observes CS2's actual sky draw, so this probe looks at the
// lower-level D3D11 state instead. It records only draws that bind a cubemap (or a shader whose
// debug name contains "sky") and never changes graphics state. This is research tooling, not the
// rotation implementation.

struct ID3D11DeviceContext;
struct ID3D11DeviceChild;
struct ID3D11Resource;
struct ID3D11VertexShader;
struct ID3D11PixelShader;

namespace Filmmaker {

// Arm a bounded number of candidate draws. Zero disarms. Call from the game/main thread.
void SkyGpuProbe_Arm(int candidateLimit);
// Same bounded trace, but skip sky.vfx draws so the separate sun/glow effect pass is visible.
void SkyGpuProbe_ArmEffects(int candidateLimit);

// Render thread, immediately before an engine Draw call. Cheap no-op while disarmed.
void SkyGpuProbe_NoteDraw(ID3D11DeviceContext* context, const char* operation,
	unsigned arg0, unsigned arg1, unsigned arg2, unsigned arg3, void* caller);

// Returns true only for the small csgo_effects cubemap draw used to paint the 3D sky into the
// full scene target. The render hook uses this predicate while the Players Only / Green policy
// is active; it deliberately does not suppress ordinary particle/smoke draws.
bool SkyGpuProbe_IsPlayersOnlyGreenSkyDraw(ID3D11DeviceContext* context,
	unsigned drawArg0, unsigned drawArg1);

// Render thread, around a successful D3D11 Map/Unmap. Once a real sky draw has identified its
// vertex constant buffers, this captures the CPU upload at Unmap without reading GPU resources or
// altering graphics state.
void SkyGpuProbe_NoteMap(ID3D11Resource* resource, void* data);
void SkyGpuProbe_NoteUnmap(ID3D11Resource* resource);
// D3D11 structured buffers are normally populated with UpdateSubresource rather than Map. The
// effect trace records a bounded sample of those uploads so the sun sprite's transform buffer can
// be identified without reading arbitrary GPU memory.
void SkyGpuProbe_NoteUpdateSubresource(ID3D11Resource* resource, const void* data,
	unsigned rowPitch, unsigned depthPitch);

// The D3D device hook records shader bytecode at creation time. Once a draw is identified as the
// actual sky.vfx draw, the named VS/PS bytecode is written once to the HLAE debug-log folder so
// fxc/D3DReflect can settle which constant registers feed the cubemap direction. This is opt-in
// through the existing mvm_debug session and never runs in normal gameplay.
void SkyGpuProbe_NoteVertexShaderCreated(ID3D11VertexShader* shader, const void* bytecode,
	std::size_t bytecodeBytes);
void SkyGpuProbe_NotePixelShaderCreated(ID3D11PixelShader* shader, const void* bytecode,
	std::size_t bytecodeBytes);
void SkyGpuProbe_DumpBoundShader(ID3D11DeviceChild* shader, const char* stage);
// Render-thread diagnostic used by the sky rotation path. Logs the constant-buffer/resource
// bindings of a named sky shader when its bytecode was observed by the D3D11 creation detour.
void SkyGpuProbe_LogShaderReflection(ID3D11DeviceChild* shader, const char* stage);

} // namespace Filmmaker
