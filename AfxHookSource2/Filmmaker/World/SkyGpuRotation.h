#pragma once

// Draw-time sky rotation. The env_sky entity transform is not consumed by the actual sky draw, so
// this module keeps CPU copies of the sky's dynamic transform buffer and substitutes modified
// copies only while sky.vfx is being drawn. The map sun is separate csgo_effects geometry: one
// compact disc plus two broad-halo triangles on Mirage. Those buffers are cloned from a fresh
// CPU read each draw (the engine rebillboards toward the view every frame), scaled around the
// shared disc-pair midpoint, and rigidly rotated from Scene Lights only. Skybox yaw/pitch never
// touch them, so the controls stay independent. It is a strict no-op at the default orientation.

struct ID3D11Buffer;
struct ID3D11DeviceContext;
struct ID3D11Resource;

namespace Filmmaker {

struct SkyGpuRotationScope {
	ID3D11Buffer* originalVsSlot0 = nullptr;
	ID3D11Buffer* originalSunVertexBuffer = nullptr;
	unsigned sunVertexStride = 0;
	unsigned sunVertexOffset = 0;
};

void SkyGpuRotation_NoteMap(ID3D11Resource* resource, void* data);
void SkyGpuRotation_NoteUnmap(ID3D11Resource* resource);

// Call immediately around an engine draw. Begin returns an empty scope unless the draw is sky.vfx
// and the sky material's slot-0 transform upload is cached. The rotated copy is bound only to the
// vertex stage's slot 0: that is the $Globals cbuffer containing g_matSkyWsToOs in sky_pc_50_vs.
SkyGpuRotationScope SkyGpuRotation_BeginDraw(ID3D11DeviceContext* context);
void SkyGpuRotation_EndDraw(ID3D11DeviceContext* context, SkyGpuRotationScope& scope);

} // namespace Filmmaker
