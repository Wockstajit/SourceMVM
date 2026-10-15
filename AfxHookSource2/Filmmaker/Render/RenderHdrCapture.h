#pragma once

// Pre-tonemap HDR frame capture -> float EXR.
//
// WHY. Every existing output format samples the LDR (8-bit, tonemapped) capture chain, so
// "albedo x diffuse + specular" can never reconstruct beauty exactly: tonemapping is baked in
// and light above 1.0 is clipped. CS2 renders the scene into a SEPARATE multisampled
// R16G16B16A16 colour target first (render-system-contract: the fmt=9 sibling of the fmt=27
// target the recorder captures); its contents are linear pre-tonemap light. This module tracks
// that texture at creation time (via the existing CreateTexture2D detour in
// RenderTargetProbe.cpp -- the one sanctioned interception site), and on request resolves the
// MSAA target, maps it, and writes an uncompressed half-float EXR (Platform/ExrWriter).
//
// Phase 1 scope: a one-shot diagnostic grab (`mirv_filmmaker render hdrgrab [path]`) that
// proves the tracked target really is the pre-tonemap scene (stats log the fraction of pixels
// above 1.0, which an LDR source cannot contain). Job-integrated EXR sequences build on this
// once the source is proven.

#include <d3d11.h>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Called from the CreateTexture2D detour for every created texture; remembers the newest
// texture matching the HDR scene-target signature. Cheap (a few compares) on the hot path.
void RenderHdrCapture_OnCreateTexture2D(const D3D11_TEXTURE2D_DESC& desc, ID3D11Texture2D* tex);

// Queues a one-shot grab; the next render-thread tick writes the EXR to `path` (UTF-16).
void RenderHdrCapture_RequestGrab(const wchar_t* path);

// Runs on the render thread once per frame at the BeforeUi boundary (post/tonemap already
// consumed the HDR target this frame, so its contents are complete and stable).
void RenderHdrCapture_RenderThread_Tick(ID3D11DeviceContext* ctx);
// The tracked pre-tonemap scene target matching the bound render target's size, AddRef'd (caller
// releases), or null. Render thread, at the BeforeUi boundary. Used by the auto-exposure meter.
ID3D11Texture2D* RenderHdrCapture_AcquireSceneTarget(ID3D11DeviceContext* ctx, D3D11_TEXTURE2D_DESC* outDesc);
// Render-thread callback applies the current environment to the private resolved HDR copy.
// The engine HDR target is never modified. The caller owns pass/depth lifetime and draw guards.
// Original source identity lets material replays validate the private copy's provenance.
using HdrEnvironmentPass = void (*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, ID3D11Texture2D*);
void RenderHdrCapture_SetEnvironmentPass(HdrEnvironmentPass pass);

// `mirv_filmmaker render hdrgrab [path]`.
void RenderHdrCapture_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// --- EXR sequence mode (render jobs with output format `exr`) -------------------------------
//
// The driver arms a sequence directory before capture; then EVERY recorded source frame
// (signalled from the render thread at the BeforeUi capture boundary -- the same event the
// recorder counts) writes `frame_%06d.exr` there, in lockstep with the LDR pacing stream.
// EXR jobs require HUD mode None so the BeforeUi boundary is the single capture site.

void RenderHdrCapture_BeginSequence(const wchar_t* dir);
// Fired once per recorded source frame, render thread. No-op unless a sequence is armed.
void RenderHdrCapture_OnRecordedFrameBeforeUi(ID3D11DeviceContext* ctx);
// Disarms and reports how many frames were written / failed. Idempotent.
void RenderHdrCapture_EndSequence(unsigned* outWritten, unsigned* outFailed);
bool RenderHdrCapture_SequenceActive();

} // namespace Filmmaker
