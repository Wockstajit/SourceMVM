// CS2's answers to the scrub frame cache (ScrubFrameCache.cpp, the rules shared with CS:S): the toggle
// in the settings store, the picture revision from every CS2 edit clock, the camera path and bridge,
// and the frames themselves in the viewport scaler (RenderSystemDX11Hooks.cpp).

#include "ScrubFrameCacheHost.h"

#include "CameraPath.h"                    // EvalPoseAtTick / PathRevision: the pose a frame was taken with
#include "CamPathShake.h"                  // CamPathShake_Revision: per-key shake edits change the picture
#include "SceneKeys.h"                     // SceneKeys_Revision: scene key edits change the picture
#include "TimelinePlayhead.h"              // a camera-path surface (timeline, graph) is being dragged
#include "CameraBridge.h"                  // a parked frame lets go when the camera moves
#include "../Config/FilmmakerSettingsStore.h" // the Cache toggle; PictureRevision
#include "../Environment/EnvironmentScene.h"  // SceneGeneration: lights, sky, map lighting
#include "../Heatmap/RoundHeatmap.h"           // the round heat map painted on the floor
#include "../Props/SceneUndoClock.h"       // every scene object edit
#include "../../ViewportScaler.h"          // the frames themselves (render thread)

namespace Filmmaker {

namespace {

constexpr const char* kUiKey = "camtl.scrubCache";

uint64_t Mix(uint64_t h, uint64_t v) {
	h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
	return h;
}

} // namespace

bool ScrubCacheHost_Enabled() { return FilmmakerSettings_UiBool(kUiKey, true); }

void ScrubCacheHost_SetEnabled(bool enabled) { FilmmakerSettings_SetUi(kUiKey, enabled ? "1" : "0"); }

uint64_t ScrubCacheHost_PictureRevision() {
	uint64_t h = 0x84222325cbf29ce4ull;
	h = Mix(h, FilmmakerSettings_PictureRevision());
	h = Mix(h, (uint64_t)(int64_t)SceneKeys_Revision());
	h = Mix(h, g_sceneUndoClock.load(std::memory_order_relaxed));
	h = Mix(h, Environment::SceneGeneration());
	h = Mix(h, CameraPathRef().PathRevision());
	// The per-key shake is a view-stage layer the pose check cannot see (it compares the path).
	h = Mix(h, CamPathShake_Revision());
	h = Mix(h, Heatmap::PictureRevision());
	return h ? h : 1;
}

bool ScrubCacheHost_PathPose(double tick, double out[7]) { return CameraPathRef().EvalPoseAtTick(tick, out); }

bool ScrubCacheHost_PathFirstTick(double& tick) {
	const CameraPath& cp = CameraPathRef();
	if (!cp.HasPathRange()) return false;
	tick = (double)cp.FirstPathTick();
	return true;
}

bool ScrubCacheHost_PathScrubbing() { return TimelinePlayhead_Scrubbing(); }

void ScrubCacheHost_Camera(double out[7]) {
	double o[3], a[3], fov = 90.0;
	CameraBridge_GetCurrentCamera(o, a, fov);
	out[0] = o[0]; out[1] = o[1]; out[2] = o[2]; out[3] = a[0]; out[4] = a[1]; out[5] = a[2]; out[6] = fov;
}

void ScrubFrames_Publish(const ScrubFramePublish& frame) { AfxViewportScaler::ScrubCache_Publish(frame); }

void ScrubFrames_Configure(bool enabled, int budgetMb) { AfxViewportScaler::ScrubCache_Configure(enabled, budgetMb); }

int ScrubFrames_Find(double tick, uint64_t revision, ScrubCache::Index::Entry* out) {
	return AfxViewportScaler::ScrubCache_Find(tick, revision, out);
}

int ScrubFrames_DropOtherRevisions(uint64_t revision) { return AfxViewportScaler::ScrubCache_DropOtherRevisions(revision); }

int ScrubFrames_DropSlot(int slot) { return AfxViewportScaler::ScrubCache_DropSlot(slot); }

int ScrubFrames_Clear() { return AfxViewportScaler::ScrubCache_Clear(); }

void ScrubFrames_GetStats(ScrubFrameStats& out) { AfxViewportScaler::ScrubCache_GetStats(out); }

} // namespace Filmmaker
