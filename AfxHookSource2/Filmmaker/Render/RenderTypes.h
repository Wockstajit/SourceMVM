#pragma once

#include "RenderPassPlan.h"

#include <string>
#include <vector>

namespace Filmmaker {

enum class RenderRangeMode {
	FullDemo,
	CurrentRound,
	CameraPathBounds,
	SelectedCameraSegment,
	CustomTicks
};

enum class CameraPathExtension {
	Fail,
	HoldFirst,
	HoldLast,
	Clamp
};

enum class RenderCameraSource {
	CurrentViewer,
	Pov,
	ThirdPerson,
	FreeCamera,
	CameraPath,
	FollowCamera
};

enum class RenderSessionState {
	Idle,
	Validating,
	PreparingOutput,
	SavingState,
	SeekingPreRoll,
	WarmingUp,
	Capturing,
	FlushingGpu,
	FinalizingStreams,
	PostProcessing,
	RestoringState,
	Completed,
	Failed,
	CancelRequested,
	Canceled
};

// What the beauty stream does with the game's own HUD.  Source MVM editor chrome is handled by
// the capture-stage gate; this enum is only the native CS2 HUD policy.
enum class RenderHudMode {
	None = 0,
	Game = 1,
	Full = 2
};

enum class RenderOutputFormat {
	Mp4,
	Avi,
	Gif,
	Png,
	Tiff,
	Tga,
	Bmp,
	Exr
};

// The per-job pass settings. A job records one output stream per pass: a pass is a PassType
// (Beauty / Depth / Normal) combined with a VisibleElements group (Everything, World + Players,
// World only, Players only, Gun only) -- see RenderPassPlan.h for the model and why every pass of
// one output frame is taken from the same simulation state.
struct RenderPasses {
	RenderHudMode hud = RenderHudMode::None; // applies to Beauty + Everything only
	bool cameraData = true; // camera transform sidecar, not an image stream
	// The sidecar's After Effects camera (camera_aftereffects.jsx, Render/CameraExportMath.h):
	// AE pixels per Source unit, and whether the first frame's camera sits at AE's origin.
	float aeScale = 1.0f;
	bool aeOrigin = true;
	// No HUD keeps CS2's kill feed (its death notices, with any names Movie/KillFeed.h shows instead).
	bool killFeed = false;
	// false: no blood is spawned during the job, in any pass (violence_hblood/ablood 0, restored
	// afterwards). Blood the Players Only render cannot skip is otherwise tinted into the key.
	bool blood = true;
	std::vector<PassConfig> passes{ PassConfig{} }; // Beauty + Everything
	// Isolation background colour (Players only / Gun only, and the green-screen sky), 0..1.
	float keyColor[3] = {0.0f, 1.0f, 0.0f};
	// GREEN-SCREEN SKY: the sky (with its clouds and the 3D skybox) painted the key colour in every
	// pass, so it can be keyed out and another sky put in. Shown live while the Render workspace is
	// open, and in every render (PassMask::SkySwap, applied at the BeforeUi boundary).
	bool keySky = false;
	// Depth passes: white at depthNear, black at depthFar (world units, linear).
	float depthNear = 8.0f;
	float depthFar = 4096.0f;
	// Live workspace view of one pass on the game view (never during a job). Beauty + Everything is
	// "off": the normal scene. Gun only shows its viewmodel depth band (no muzzle flash live).
	PassConfig preview;
};

// True when the Beauty + Everything plate is taken after the UI draws (BeforePresent): a HUD mode,
// or No HUD that keeps the kill feed (every other panel hidden, the death notices kept).
inline bool HudPlate(const RenderPasses& p) { return p.hud != RenderHudMode::None || p.killFeed; }

struct RenderValidationError {
	std::string message;
	bool isFatal;
};

} // namespace Filmmaker
