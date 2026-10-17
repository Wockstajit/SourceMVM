#pragma once

// Scrub frame cache, main-thread side. Keeps the preview of every played frame of the current take
// so a backward drag over ticks the world has just played through is shown at frame rate instead
// of one engine restore per update. Policy: ScrubFrameCacheMath.h (what is kept and shown) and
// DemoScrubMath.h ScrubServeFromCache (when to show it). The frames themselves are held by the
// viewport scaler (RenderSystemDX11Hooks.cpp). Contract: Docs/features/filmmaker_demo_scrubbing.md.
//
// Main thread only. Order within a frame (Filmmaker.cpp RunMainThreadFrame):
//   BeginFrame -> DemoScrub_RunFrame (Lookup/Serve/NoteSeek) -> camera drivers (CameraTick,
//   NoteCamera) -> EndFrame (publishes this frame's identity to the render thread).

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

void ScrubFrameCache_BeginFrame();
void ScrubFrameCache_EndFrame();

// The camera timeline's Cache toggle (persisted as the UI key "camtl.scrubCache"). Off: no frame is
// kept or shown, the camera flies the playhead, and scrubbing behaves exactly as without a cache.
bool ScrubFrameCache_Enabled();
void ScrubFrameCache_SetEnabled(bool enabled);

// Camera path drivers (camera timeline scrub, graph editor drive). The tick to evaluate the path at:
// while a drag moves the world and the cache is on, the WORLD's own time, so every rendered frame
// is a true picture of one tick (camera and world agree) and can be kept. Otherwise (and after the
// release, while the world settles on the released tick) the playhead.
double ScrubFrameCache_CameraTick(double playheadTick);
// The pose the driver set this frame, and the tick it evaluated the path at. `pathPose` is the
// path's own pose there when the driver shows something else (playback low-passes it); null means
// `pose` is the path pose. `clockTime`: the driver runs its own demo clock (path playback), and the
// frame's time is that clock -- the world's fractional time is not reliable while a demo plays.
void ScrubFrameCache_NoteCamera(double cameraTick, const double pose[7], const double* pathPose = nullptr,
	bool clockTime = false);

// DemoScrub: is the playhead's frame cached (current take, current revision, and the path still
// flies the pose it was taken with)?
bool ScrubFrameCache_Lookup(double tick);
// DemoScrub: show the cached frame for `tick` this frame instead of moving the world.
void ScrubFrameCache_Serve(double tick);
// DemoScrub: a seek went out. Nothing is played into until it lands and plays on.
void ScrubFrameCache_NoteSeek();
// DemoScrub: the world's take began at or before the shot's first key, so a forward drag plays the
// world and re-records the shot (ScrubCache::Recording). Otherwise forward drags show the cache too.
bool ScrubFrameCache_Recording();
// DemoScrub: a release landed on `tick`. If it is cached, the preview keeps showing that frame while
// the world sits there (a click shows what a drag showed), until play, a drag, a move or an edit.
void ScrubFrameCache_Park(double tick);

// `mirv_filmmaker scrub cache [on|off|clear|budget <MB>]`.
void ScrubFrameCache_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);
// `mirv_filmmaker health` line.
void ScrubFrameCache_PrintHealth();

} // namespace Filmmaker
