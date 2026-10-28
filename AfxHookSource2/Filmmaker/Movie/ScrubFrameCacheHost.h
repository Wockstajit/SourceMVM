#pragma once

// What the scrub frame cache (ScrubFrameCache.cpp) asks the game it runs in. Which frames are kept,
// shown and dropped is the same in every game (ScrubFrameCacheMath.h, ScrubFrameCache.cpp); where the
// toggle is saved, what changes the picture, the camera path and the GPU frames are each game's. CS2
// answers in ScrubFrameCacheHostSource2.cpp (the frames in its viewport scaler), CS:S in
// AfxHookSource/Filmmaker/SourceScrubFrameCache.cpp (the frames in its D3D9 renderer).
//
// Main thread only, except where a frame store says otherwise.

#include "ScrubFrameCacheMath.h"

#include <cstdint>

namespace Filmmaker {

// The Cache toggle as the game saved it (on by default), and saving a change.
bool ScrubCacheHost_Enabled();
void ScrubCacheHost_SetEnabled(bool enabled);

// Everything other than time and the camera that changes what a frame shows: settings, scene and
// path edits. Any change drops the frames kept under another value.
uint64_t ScrubCacheHost_PictureRevision();

// The camera path's pose at a tick (x y z pitch yaw roll fov), false off the path.
bool ScrubCacheHost_PathPose(double tick, double out[7]);
// The tick of the path's first key, false without a path.
bool ScrubCacheHost_PathFirstTick(double& tick);
// A surface that flies the path (the camera timeline, the TIMELINE dock) is being dragged now.
bool ScrubCacheHost_PathScrubbing();
// The camera the engine renders with: x y z pitch yaw roll fov.
void ScrubCacheHost_Camera(double out[7]);

// --- The frames, held where the game renders ------------------------------------------------------

// What one main-thread frame renders, published once a frame. The render side takes it with that
// frame's own picture, so a kept frame always carries the identity of what it copied.
struct ScrubFramePublish {
	ScrubCache::FrameId id;
	bool serve = false;     // show the kept frame for serveTick instead of the world
	double serveTick = 0.0;
	double nearTick = 0.0;  // where the user is: a full cache evicts the frames farthest away
};

struct ScrubFrameStats {
	bool enabled = false;
	int frames = 0, capacity = 0;
	unsigned width = 0, height = 0;      // one frame, px
	unsigned long long bytes = 0;        // video memory held by frame textures
	unsigned long long stores = 0, served = 0, serveMissing = 0, evictions = 0, poseMismatches = 0;
};

void ScrubFrames_Publish(const ScrubFramePublish& frame);
// Off releases every frame (and its video memory) at the next present.
void ScrubFrames_Configure(bool enabled, int budgetMb);
// The slot that would be shown for `tick`, or -1 (ScrubCache::Index::Find).
int ScrubFrames_Find(double tick, uint64_t revision, ScrubCache::Index::Entry* out);
// Each returns how many frames were dropped.
int ScrubFrames_DropOtherRevisions(uint64_t revision);
int ScrubFrames_DropSlot(int slot);
int ScrubFrames_Clear();
void ScrubFrames_GetStats(ScrubFrameStats& out);

} // namespace Filmmaker
