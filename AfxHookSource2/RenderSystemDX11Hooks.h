#pragma once

#include "../Deps/release/prop/AfxHookSource/SourceSdkShared.h"

#define ADVANCEDFX_STARTMOVIE_WAV_KEY "advancedfx-802bb089-972b-4841-bdf3-5108175ab59d"

bool AfxStreams_IsRcording();
const wchar_t * AfxStreams_GetTakeDir();

// True only while a recording whose capture stage is AFTER the UI/scaler passes is live
// (screen recording, or any stream with Capture_e::BeforePresent). That is the ONLY case in
// which the editor's scaled-preview blit and the Source MVM Panorama chrome would be baked
// into captured frames. A BeforeUi ("no HUD") capture is taken before both, so the preview and
// the UI can stay exactly as they are while such a job records -- which is what the render
// workspace relies on to keep the game looking unchanged during a clean-plate render.
bool AfxStreams_RecordingCapturesAfterUi();

void AfxStreams_ShutDown();

// What the D3D detours themselves cost last frame, in wall milliseconds, EXCLUDING the native
// calls they forward to. `mirv_filmmaker health` prints it so a frame-rate complaint can be
// split into the hook's share and the game's share without a profiler.
struct RenderHookCost {
    double drawHookMs = 0;        // inside the Draw* detours, minus the forwarded native draw
    double drawNativeMs = 0;      // the forwarded native draws (the game's own submission cost)
    double presentHookMs = 0;     // Before_Present: Filmmaker::RunFrame and the rest
    double environmentMs = 0;     // the pre-UI Environment render call
    unsigned draws = 0;           // Draw* calls seen
    double avgDrawHookMs = 0, avgPresentHookMs = 0, avgEnvironmentMs = 0; // ~20-frame averages
    // Parts of the draw detours (~20-frame averages): the Lighting pipeline's per-draw decision, its
    // extra redraws, and the animated-caster capture decision.
    double avgDrawBeginMs = 0, avgDrawExtraMs = 0, avgDrawCasterMs = 0;
};
RenderHookCost RenderHookCost_Sample();
// Always-on, lock-free progress sampled by the main-thread heartbeat. Completed-frame costs
// stay stale during a render stall; this identifies the active boundary and its wall-clock age.
struct RenderProgress {
    const char* phase = "not-started";
    unsigned long long phaseAgeMs = 0, frameAgeMs = 0;
    bool started = false;
};
RenderProgress RenderProgress_Sample();
// Successful physical presents only: excludes suppressed recording passes and DXGI tests.
unsigned long long RenderCompletedPresents();
struct RenderFrameTiming {
    unsigned samples=0, over33=0;
    double meanMs=0, p50Ms=0, p95Ms=0, p99Ms=0, maxMs=0;
};
RenderFrameTiming RenderFrameTiming_Sample();
void RenderFrameTiming_Reset();
// The game's GPU and its local video memory use against the OS budget (DXGI 1.4). Over budget,
// Windows pages textures across the bus and every present crawls. False before the device exists.
struct RenderGpuMemory {
    char adapter[128] = {};
    unsigned long long usedBytes = 0, budgetBytes = 0;
};
bool RenderGpuMemory_Sample(RenderGpuMemory& out);

// --- Filmmaker render-job control surface -------------------------------------------------
// Typed accessors over the same CAfxStreams state the `mirv_streams` console grammar edits.
// The Filmmaker render session (Filmmaker/Render/RenderSessionDriver.cpp) drives a job through
// these instead of re-entering the console parser: no string round-trips, and every knob it
// changes can be read back first so the previous value is restorable. Main thread only.

// Starts/ends the recorder exactly like `mirv_streams record start|end`. Returns whether
// recording actually started (directory creation can fail).
bool AfxStreams_RecordStart();
void AfxStreams_RecordEnd();
// Number of source frames that entered HLAE's recording scheduler since RecordStart. Unlike the
// UI/render pump this advances exactly once per scheduled source frame.
int AfxStreams_GetRecordedSourceFrameCount();
// Asynchronous encoder/image-writer errors observed since the most recent RecordStart.
int AfxStreams_GetWriterFailureCount();

// Record name = the folder the recorder creates its `take****` directory under (UTF8).
const char * AfxStreams_GetRecordNameUtf8();
void AfxStreams_SetRecordNameUtf8(const char * value);

// Screen recording (the beauty capture) on/off + which named recording settings preset it uses
// (see Shared/RecordingSettings.cpp). Setting an unknown preset name returns false.
bool AfxStreams_GetRecordScreenEnabled();
void AfxStreams_SetRecordScreenEnabled(bool enabled);
const char * AfxStreams_GetRecordScreenSettingsName();
bool AfxStreams_SetRecordScreenSettingsByName(const char * name);

// `mirv_streams record fps` equivalent: overrides host_framerate for the recording.
void AfxStreams_GetOverrideFps(bool & outEnabled, float & outValue);
void AfxStreams_SetOverrideFps(bool enabled, float value);

// Output/mux framerate for the encoded streams (0 = follow the host_framerate value, the
// inherited default). The Filmmaker render job sets this to the PLAYBACK fps: under the
// timescale dial the recorder SAMPLES at fps/timescale, but the file must play at fps or the
// slow motion collapses back to real time in the container.
float AfxStreams_GetOutputFpsOverride();
void AfxStreams_SetOutputFpsOverride(float fps);

// Camera-motion export (.cam file into the take folder), `mirv_streams record cam enabled`.
bool AfxStreams_GetCamExport();
void AfxStreams_SetCamExport(bool enabled);

// Classic image-sequence format, `mirv_streams record format tga|bmp` (true = BMP).
bool AfxStreams_GetFormatBmpNotTga();
void AfxStreams_SetFormatBmpNotTga(bool bmp);

// Capture streams. AddNormalStream registers an RGB stream (the render job's beauty pass);
// `beforeUi` captures before ANY Panorama UI (clean plate), otherwise before present (includes
// game HUD); `settingsName` picks the recording preset the stream encodes with (nullptr = the
// default). Streams record when `mirv_streams record start` fires, and cannot be added or
// removed mid-recording.
bool AfxStreams_HasStream(const char * name);
bool AfxStreams_AddNormalStream(const char * name, bool beforeUi, const char * settingsName);
// One Filmmaker multipass stream: `passType` / `passElements` are Filmmaker::PassType /
// Filmmaker::VisibleElements (Filmmaker/Render/RenderPassPlan.h). Every pass of one output frame
// renders inside the same engine frame, so all streams stay frame-exact. Depth maps white at
// `depthNear` to black at `depthFar` (units); `keyOut` is the isolation background colour (0..1).
bool AfxStreams_AddPassStream(const char * name, int passType, int passElements, const char * settingsName,
    float depthNear, float depthFar, const float keyOut[3]);
// How far (0..1, per channel) a Players Only pixel may be from the internal chroma key and still
// count as background. Calibration knob: `mirv_filmmaker render passes keytolerance <v>`.
void AfxStreams_SetPassKeyTolerance(float tolerance);
float AfxStreams_GetPassKeyTolerance();
// Any thread: the Filmmaker::RenderKey of the recorded render currently being built and drawn; -1
// outside one (set before its scene update, cleared after it was waited for). The isolation draw
// policies (World Only, Players Only) decide per draw with this.
int AfxStreams_CurrentPassRenderKey();
// Render workspace live view of one pass on the game view: `passType` / `passElements` as in
// AfxStreams_AddPassStream. Depth and Normal are composed over the scene before the UI every frame
// (never while recording); Beauty (0) turns it off -- the visibility group itself is applied by
// the scene draw policies, not here. Main thread.
void AfxStreams_SetLivePassPreview(int passType, int passElements, float depthNear, float depthFar, const float keyOut[3]);
// Diagnostic: depth passes record the raw depth sample (r + g/255 + b/65025) instead of distance.
void AfxStreams_SetPassDepthDebug(bool on);
bool AfxStreams_RemoveStream(const char * name);
// The folder under the take a stream writes into ("beauty" or "beauty\world"); "" puts it back
// to the stream's own name. False when the stream does not exist or a recording is running.
bool AfxStreams_SetStreamFolder(const char * name, const char * folderUtf8);
// Whether RecordStart nests the recording in its own "take0000" folder under the record name
// (mirv_streams default). Render jobs turn it off: their record name already is the take folder.
bool AfxStreams_GetRecordTakeSubfolder();
void AfxStreams_SetRecordTakeSubfolder(bool value);

void RenderSystemDX11_EngineThread_Prepare();
void RenderSystemDX11_EngineThread_BeforeRender();

bool RenderSystemDX11_EngineThread_HasNextRenderPass();

void RenderSystemDX11_EngineThread_BeginNextRenderPass();
void RenderSystemDX11_EngineThread_EndNextRenderPass();

void RenderSystemDX11_EngineThread_BeginMainRenderPass();
void RenderSystemDX11_EngineThread_EndMainRenderPass();

void Hook_RenderSystemDX11(void * hModule);

void Hook_SceneSystem(void * hModule);

void RenderSystemDX11_SupplyProjectionMatrix(const SOURCESDK::VMatrix & projectionMatrix);

// Radial (camera-relative) scene depth as an R32F shader resource, normalised into
// [outNear, outFar]. Render thread only, and only valid while the engine's depth-stencil for this
// frame is still bound -- the returned view is BORROWED, never stored. See the definition for why
// the spherical depth mode is the one that suits world-position reconstruction.
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
// The engine depth-stencil resource bound for the current frame. DLSS wants the real depth
// buffer, not a linearised copy. Borrowed; render thread, same frame only -- this is the raw,
// non-AddRef'd view the engine owns, and holding it across frames is the documented crash.
struct ID3D11Resource;
// The tracked scene colour target for the current frame (the same one the rest of the render
// hooks treat as "the scene"), and the bound depth-stencil. Both borrowed, render thread,
// same-frame only.
struct ID3D11RenderTargetView;
// The scene colour VIEW, for writing a result back into it. CopyResource cannot target an MSAA
// resource from a single-sample one, so a consumer has to rasterise into this instead.
ID3D11RenderTargetView* RenderSystemDX11_GetSceneColorRtv();
ID3D11Resource* RenderSystemDX11_GetSceneColorResource();
ID3D11Resource* RenderSystemDX11_GetSceneDepthResource();
// AddRef'd copy of the current frame's raw depth, created by the depth compositor as an
// SRV-capable R24G8 typeless texture. Caller owns the reference and releases it after the tick.
ID3D11Resource* RenderSystemDX11_GetReadableSceneDepthResource();
