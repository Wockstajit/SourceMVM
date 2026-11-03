#pragma once

// The Render workspace, CS2's (CameraEditorRenderJs.h, CameraEditorRenderLayoutJs.h) in CS:S paint.
// The JOB column on the left says what the job will render, its problems or its progress, the
// previous renders of this demo, and holds START RENDER. The RENDER column on the right has the
// RANGE & CAMERA, PASSES and OUTPUT tabs, and the playhead's Set In / Set Out / Clear under them.
// Every control sends the console command CS2's panel sends (`mirv_filmmaker render ...`), so the
// console, the workspace and both games share one set of settings
// (AfxHookSource2/Filmmaker/Render/RenderCommand.cpp).
//
// Engine-free: the game host and the browser mockup both draw it inside an ImGui frame.

#include "UiStyle.h"

#include <string>
#include <vector>

namespace SourceFilmmaker {
namespace Ui {

// One earlier take of this demo (PREVIOUS RENDERS).
struct RenderTake {
	std::string when;   // "Oct 27, 23:58"
	std::string status; // completed, canceled, failed, rendering
	std::string detail; // "1000 → 1199 · 0:03 · 1 pass · 60 fps MP4"
};

struct RenderWorkspaceModel {
	bool demo = false;    // a demo plays
	bool running = false; // a job runs
	int tab = 0;          // 0 RANGE & CAMERA, 1 PASSES, 2 OUTPUT
	int playhead = 0;

	// RANGE: RenderRangeMode (full demo, round, camera path, segment, custom In/Out), what each can
	// offer now, and the span it resolves to (out exclusive).
	int rangeMode = 4;
	bool modeLive[5] = { true, true, true, true, true };
	bool rangeOk = false;
	int in = 0, out = 0;
	int demoStart = 0, demoEnd = 0;
	double tickRate = 66.6667;
	int frames = 0; // what the job will write

	// CAMERA: RenderCameraSource, and why a source cannot be used now ("" when it can).
	int camera = 0;
	std::string cameraBlock[6];
	int pathKeys = 0;
	int extension = 0; // CameraPathExtension

	// PASSES: the shared type/group grid, the key colour and the linear depth range.
	bool selectedPass[4][5] = {{ true }};
	int keyColor[3] = { 0, 205, 0 };
	float depthNear = 8, depthFar = 4096;
    int previewType = 0, previewGroup = 0;
	int hud = 0; // RenderHudMode
	bool cameraData = true;
	bool blood = true;

	// OUTPUT
	std::string folder;
	bool folderPending = false;
	std::string project;
	int format = 0; // RenderOutputFormat
	bool formatOk[8] = { true, true, true, true, true, true, true, true };
	std::string formatName, formatNote;
	int fps = 60;
	int crf = 18;
	int bitrateKbps = 0;
	int encoder = 2; // RenderEncoder: 0 CPU, 1 GPU, 2 Auto
	int speed = 0;   // RenderEncodeSpeed
	int nvenc = 0;   // NvencProbe: 2 available, 3 unavailable
	std::string nvencNote;
	int width = 0, height = 0;
	bool soundOn = true;
	int volume = 60;

	// Validation and the job.
	int fatalCount = 0, warnCount = 0;
	std::string firstFatal;
	int sessionState = 0; // RenderSessionState: 11 completed, 12 failed
	std::string phase, lastError, takeDir;
	int framesDone = 0, framesTotal = 0, curTick = 0;
	int percent = 0;
	std::vector<RenderTake> takes;
};

struct RenderWorkspaceState {
	char depthNearText[32] = "8", depthFarText[32] = "4096";
	float depthNearSeed = -1, depthFarSeed = -1;
	bool fpsCustom = false;     // the frame rate's Custom field is open
	char fpsText[8] = "";
	bool bitrateCustom = false; // the quality's Bitrate field is open
	char bitrateText[10] = "";
	int volumeDrag = -1;        // the volume while its row is dragged, -1 otherwise
};

// Draws the JOB column into job and the RENDER column into settings (client px). Commands go to out.
void DrawRenderWorkspace(RenderWorkspaceState& state, const RenderWorkspaceModel& model, const FontSet& fonts, float s,
	const Rect& job, const Rect& settings, std::vector<std::string>& out);

} // namespace Ui
} // namespace SourceFilmmaker
