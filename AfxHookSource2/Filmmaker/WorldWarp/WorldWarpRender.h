#pragma once

// D3D11 GPU renderer for World Effects:
//   - Little Planet: 6-face 360 cubemap stereographic projection
//   - Inception, Rolling Horizon, World Wave, World Fold: true 3D DXBC Vertex-Shader deformation
//     injected into CS2's own world/prop/player vertex shaders (zero pixel stretching), with
//     depth-mesh fallback if no VS draws were intercepted.

#include "WorldWarp.h"
#include "WorldWarpMath.h"
#include <d3d11.h>

namespace Filmmaker::WorldWarp {

void BeginDevice(ID3D11Device* device);
void EndDevice();

// True on the current thread while WorldWarp is creating its own patched VS (skip recursion in
// New_CreateVertexShader).
bool CreatingShader();

// Called from New_CreateVertexShader (any thread) to inspect & track CS2 3D world vertex shaders
// that bind PerViewConstantBuffer_t (g_matWorldToProjection at c0, g_matProjectionToWorld at c4).
void NoteVertexShader(ID3D11VertexShader* shader, const void* bytecode, size_t bytes);

// Called once per frame to publish the main scene viewport size & demoTime to cb13[0..6].
void UpdateFrameConstants(ID3D11DeviceContext* immediate, double demoTime, UINT sceneW, UINT sceneH);

// Diagnostic counts for `mirv_filmmaker worldwarp status`.
void GetVertexWarpStats(uint32_t& outTracked, uint32_t& outPatched, uint32_t& outLastFrameDraws);

// Lightweight RAII draw scope placed beside SmokeAppearance::Scope on CS2's draw detours.
// Fast 1-atomic no-op when vertex warping is OFF; swaps the active world VS to its DXBC-patched
// variant and binds cb13[0..6] for the draw, then restores the original VS and cb13.
class DrawScope {
public:
	DrawScope(ID3D11DeviceContext* ctx, bool enabled = true) noexcept {
		if (enabled && WorldWarp_WantsVertexWarp())
			Enter(ctx);
	}
	~DrawScope() noexcept {
		if (m_active)
			Exit();
	}
	bool ShouldSkip() const noexcept { return m_skip; }
	DrawScope(const DrawScope&) = delete;
	DrawScope& operator=(const DrawScope&) = delete;

private:
	void Enter(ID3D11DeviceContext* ctx) noexcept;
	void Exit() noexcept;

	ID3D11DeviceContext* m_ctx = nullptr;
	ID3D11VertexShader* m_origVs = nullptr;
	ID3D11Buffer* m_origCb13 = nullptr;
	ID3D11DepthStencilState* m_origDss = nullptr;
	UINT m_origStencilRef = 0;
	bool m_swappedDss = false;
	bool m_active = false;
	bool m_skip = false;
};

// Called during Little Planet capture for each face (0..5: Down, Fwd, Left, Back, Right, Up).
// Makes the six faces captured so far the planet's source; called once they share one demo time.
void PublishCubemap(ID3D11DeviceContext* immediate);
void CaptureCubemapFace(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target,
	int faceIndex, const float* projectionMatrix);

// Applies Little Planet composite (or fallback post-process mesh if 0 VS draws ran) in BeforeUi.
bool Render(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target,
	ID3D11DepthStencilView* depth, const float* worldToClip, const float* projection,
	const float camera[3], float depthMin, float depthMax, double demoTime);

} // namespace Filmmaker::WorldWarp
