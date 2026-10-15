#pragma once

// The render pipeline: beauty-pass frame capture from the Render workspace.
//
// Capture owns host_framerate while a job runs and restores the previous clock afterward.
// Recording FPS is both mux rate and host_framerate; the timeline dial is preview-only.
// Out is inclusive.
//
// Frames are written from the RENDER thread, inside the present hook, because that is the only
// place the finished back buffer exists. Everything else here is main-thread.

#include <string>

namespace Filmmaker {

enum class RenderState {
	Idle,
	Arming,      // waiting for the demo to reach the start tick
	Capturing,
	Finishing,   // flushing the last queued frames
	Failed,
};

struct RenderSettings {
	int fps = 32;
	int startTick = 0;
	int endTick = 0;      // 0 with range=all means end of replay
	std::wstring outputDir;

	// The job's shape, as the Render column states it. These are option-row choices rather than
	// free values, so they are kept as the exact strings the panel and the console use -- one
	// vocabulary, no enum-to-string table that can drift from the UI's own words.
	//
	//   range  : all | shot | segment | loop | custom
	//   camSrc : current | free | path | follow
	//   hud    : none | game | full
	//   format : mp4 | avi | gif | png | tiff | tga | bmp | jpg; HDR EXR unavailable
	std::string range = "all";
	std::string camSrc = "current";
	std::string hud = "game";
	std::string format = "png";

	int quality = 18;        // MP4 CRF/CQ, 0..51 (lower is better), matching CS2
    int width = 0, height = 0; // 0/0 = current capture size
    int bitrateKbps = 0;
    std::string encoder = "auto";
    std::string speed = "balanced";
	bool names = true;       // hero nameplates and health bars
	bool damage = true;      // floating damage numbers
	bool pauseOnDrop = true; // stop rather than write a gap
	bool beep = false;       // a tone when the last frame lands
};

// Called every MAIN-thread frame. Advances the state machine.
void Render_RunMainThreadFrame();

// Called every RENDER-thread frame from the present hook, with the swap chain. Writes a frame when
// one is due. Returns true when it wrote.
bool Render_RunRenderFrame(void* swapChain);

RenderState Render_GetState();
const char* Render_StateName(RenderState state);

RenderSettings Render_GetSettings();
void Render_SetFps(int fps);

// One option at a time, named by the panel's own vocabulary. Returns false for a value the option
// row does not offer, so a mistyped console command is an error rather than a silent no-op.
bool Render_SetOption(const char* which, const char* value);
bool Render_SetFlag(const char* which, bool value);
void Render_SetQuality(int quality);
void Render_SetSize(int width, int height);
void Render_SetBitrate(int kbps);
void Render_SetRange(int startTick, int endTick);
void Render_PickFolder();
void Render_SetOutputDir(const std::wstring& dir);

// Starts a job. Fails loudly rather than quietly producing drifting footage when the tick
// interval cannot be read from Deadlock.
bool Render_Start();
void Render_Stop();

unsigned long long Render_FramesWritten();
unsigned long long Render_FramesDropped();

// Appends `"render":{...}` to an already-open state object.
void Render_AppendStateJson(std::string& out);
// The per-frame half (state, running, frame counters); see the definition.
void Render_AppendHotJson(std::string& out);

void Render_Report();

} // namespace Filmmaker
