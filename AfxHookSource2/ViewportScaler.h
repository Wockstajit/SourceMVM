#pragma once

#include <d3d11.h>

#include "Filmmaker/Movie/ScrubFrameCacheMath.h"

// CViewportScaler -- render-layer post-process that scales the just-rendered full-screen
// world frame down into a sub-rectangle of the backbuffer, so the Panorama camera-editor
// chrome (CameraEditorHud) can frame it as a real shrunk PREVIEW VIEWPORT instead of a crop.
//
// Pipeline, run on the render thread at the BeforeUi2 callback (world rendered, Panorama UI
// not yet composited; the callback hands us the backbuffer RTV):
//   1. copy (or MSAA-resolve) the backbuffer into a temp shader-resource texture,
//   2. clear the backbuffer black,
//   3. draw a full-NDC quad sampling the temp texture into the preview rect (RSSetViewports),
// after which Panorama composites the editor chrome on top -- the chrome's preview region is
// transparent, so it reveals the D3D-scaled world; opaque chrome covers the black remainder.
//
// All draw work is recorded on a DEFERRED context and replayed with
// ExecuteCommandList(RestoreContextState = TRUE), so the engine's immediate-context state for
// the following UI pass is left untouched (same isolation pattern as CDepthCompositor).
//
// The camera/projection are never touched -- the world still renders full-screen, so freecam,
// scrubbing, gizmos and hover-picking all keep working; this only re-blits the finished frame.

class CViewportScaler {
public:
	~CViewportScaler();

	// Create / release the D3D objects. Called from the swapchain-backbuffer device hook,
	// right next to g_CampathDrawer.BeginDevice / EndDevice.
	void BeginDevice(ID3D11Device* device);
	void EndDevice();

	// True once all device objects exist (i.e. in a demo with the device created).
	bool Ready() const;

	// Scale the full backbuffer into the rect given as NORMALISED backbuffer fractions in
	// [0,1]: (x0,y0) = top-left, (x1,y1) = bottom-right. Runs on the render thread with the
	// immediate context + backbuffer RTV delivered by the BeforeUi2 callback.
	void Blit(ID3D11DeviceContext* pImmediateContext, ID3D11RenderTargetView* pTarget,
		float x0, float y0, float x1, float y1);

	// Debug overlay readout: copy the last successful blit's backbuffer size + viewport px rect
	// (TopLeftX, TopLeftY, Width, Height). Returns whether a blit has run since device creation.
	// Written on the render thread at the end of Blit, read on the main thread (benign data race).
	bool GetLastBlit(UINT& bbW, UINT& bbH, float vp[4]) const {
		bbW = m_LastBbWidth; bbH = m_LastBbHeight;
		vp[0] = m_LastVp[0]; vp[1] = m_LastVp[1]; vp[2] = m_LastVp[2]; vp[3] = m_LastVp[3];
		return m_LastBlitRan;
	}

private:
	void ReleaseTemp();
	// viewFormat is the TYPED format used for the temp texture + SRV (the backbuffer itself may
	// be TYPELESS, which cannot back an SRV); it comes from the render-target view's format.
	bool EnsureTemp(const D3D11_TEXTURE2D_DESC& backbufferDesc, DXGI_FORMAT viewFormat);

	ID3D11Device* m_Device = nullptr;
	ID3D11DeviceContext* m_DeviceContext = nullptr; // deferred

	ID3D11VertexShader* m_VertexShader = nullptr;   // reuses afx_drawtexture_vs_5_0
	ID3D11PixelShader* m_PixelShader = nullptr;     // afx_blit_ps_5_0 (no alpha clip)
	ID3D11InputLayout* m_InputLayout = nullptr;
	ID3D11Buffer* m_VertexBuffer = nullptr;         // static 4-vertex NDC quad
	ID3D11Buffer* m_ConstantBuffer = nullptr;       // static identity matrix
	ID3D11SamplerState* m_SamplerState = nullptr;   // linear clamp
	ID3D11RasterizerState* m_RasterizerState = nullptr; // solid, cull none
	ID3D11DepthStencilState* m_DepthStencilState = nullptr; // depth off
	ID3D11BlendState* m_BlendState = nullptr;       // opaque overwrite

	ID3D11Texture2D* m_TempTexture = nullptr;       // single-sample copy of the backbuffer
	ID3D11ShaderResourceView* m_TempSrv = nullptr;
	UINT m_TempWidth = 0;
	UINT m_TempHeight = 0;
	DXGI_FORMAT m_TempFormat = DXGI_FORMAT_UNKNOWN;

	// Last blit's actual numbers (debug overlay readout; see GetLastBlit).
	UINT m_LastBbWidth = 0, m_LastBbHeight = 0;
	float m_LastVp[4] = { 0, 0, 0, 0 }; // TopLeftX, TopLeftY, Width, Height (backbuffer px)
	bool m_LastBlitRan = false;
};

// Free-function bridge used by the camera-editor host, which lives in the Filmmaker module and
// must not reach into the render globals. The single CViewportScaler instance, g_RenderCommands
// and the recording flag all live in RenderSystemDX11Hooks.cpp, where these are implemented.
namespace AfxViewportScaler {

	// Main/UI thread (camera-editor host, once per frame): publish whether a scaled preview is
	// wanted this frame and the target rect as normalised backbuffer fractions [0,1]. Passing
	// active = false (or never calling it) disables scaling -> the editor falls back to the crop.
	void SetRequest(bool active, float x0, float y0, float x1, float y1);

	// Engine thread (RenderSystemDX11_EngineThread_Prepare, once per frame): if a request is
	// active, the device is ready and we are NOT recording, queue the BeforeUi2 scale-blit for
	// this frame. No-op otherwise (zero overhead -> normal full-screen render / crop fallback).
	void EngineThread_RunFrame();

	// Debug overlay (main/UI thread): copy the last blit's backbuffer size + viewport px rect.
	// Returns whether a blit has run (false -> not scaling / not in a demo / recording).
	bool GetLastBlit(int& bbW, int& bbH, float& vx, float& vy, float& vw, float& vh);

	// Render thread, at the BeforeUi2 consumption point: run the blit directly when the
	// engine-thread queue starved. During a demo seek the engine thread stalls in "Demo Skipping"
	// message processing and produces no BeforeUi2 commands, while the render thread keeps
	// presenting -- so a sticky scale request rendered FULL-SCREEN for those frames (the
	// "preview zooms to full screen for a few ticks while scrubbing" report). Same gate as
	// EngineThread_RunFrame, at most once per present.
	void RenderThread_MaybeFallbackBlit(ID3D11RenderTargetView* pTarget);

	// Render thread (Before_Present): re-arm the once-per-present fallback latch, and count this
	// present against VpScaleBreakMath.h (each break is numbered in the mvm_debug log as
	// vpscale.break, with the state it started in and how long it lasted).
	void RenderThread_OnPresent();

	// Main thread (DemoScrub, every frame): hold the workspace preview on its last settled frame.
	// While held -- and for any present whose scale blit was missed -- the render thread copies the
	// last good preview REGION back over the frame before it is presented, so a seek's raw restore
	// (an un-simulated world: bind-pose or missing players), its play-in, and the full-size frame of
	// a missed blit never reach the screen. Only the preview rect is held: the panels around it stay
	// live. Capped at kMaxHoldMs so a seek that never settles cannot freeze the preview.
	void SetHoldPreview(bool hold);

	// Any thread: breaks of the scaled preview since the game started, per reason
	// (VpScaleBreakMath.h), and the reasons of the break open right now (0 = healthy).
	void BreakCounts(unsigned& total, unsigned& missed, unsigned& stale, unsigned& stretched, unsigned& openReasons);

	// Render thread: has this present's scale-blit already happened? Anything drawn into the
	// scene AFTER it lands in full-screen coordinates on top of the shrunk image -- i.e. over the
	// panel, not in the preview. The pivot marker's late redraw checks this so it cannot paint a
	// second copy of itself across the Config UI.
	bool BlitRanThisPresent();

	// --- Scrub frame cache (Filmmaker/Movie/ScrubFrameCacheMath.h, ScrubFrameCache.cpp) ---------
	// The preview region of each played, consistent frame is kept on the GPU, keyed by its demo
	// time and take; a backward drag over kept ticks is shown from here instead of restoring the
	// world. Captured and shown at the same place as the preview hold (Before_Present), so the
	// frames are exactly what the preview showed, UI over the preview included.

	// What one main-thread frame renders, published once per frame (main thread). The engine
	// thread hands it to the render thread with that frame's scale blit, so a capture always
	// carries the identity of the frame it copies.
	struct ScrubCacheFrame {
		Filmmaker::ScrubCache::FrameId id;
		bool serve = false;     // show the cached frame for serveTick instead of the world
		double serveTick = 0.0;
		double nearTick = 0.0;  // where the user is: a full cache evicts the frames farthest away
	};
	void ScrubCache_Publish(const ScrubCacheFrame& frame);
	// Main thread. Off releases every frame (and its VRAM) at the next present.
	void ScrubCache_Configure(bool enabled, int budgetMb);
	// Main thread: the slot that would be shown for `tick`, or -1 (ScrubCache::Index::Find).
	int ScrubCache_Find(double tick, uint64_t revision, Filmmaker::ScrubCache::Index::Entry* out);
	// Main thread. Each returns how many frames were dropped.
	int ScrubCache_DropOtherRevisions(uint64_t revision);
	int ScrubCache_DropSlot(int slot);
	int ScrubCache_Clear();

	struct ScrubCacheStats {
		bool enabled = false;
		int frames = 0, capacity = 0;
		unsigned width = 0, height = 0;      // one frame, px
		unsigned long long bytes = 0;        // VRAM held by frame textures
		unsigned long long stores = 0, served = 0, serveMissing = 0, evictions = 0, poseMismatches = 0;
	};
	void ScrubCache_GetStats(ScrubCacheStats& out);

} // namespace AfxViewportScaler
