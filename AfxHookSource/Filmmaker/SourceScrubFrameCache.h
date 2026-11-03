#pragma once

// CS:S's side of the scrub frame cache (AfxHookSource2/Filmmaker/Movie/ScrubFrameCache.cpp, the rules
// shared with CS2): the toggle in the settings file, the picture revision, and the frames, kept on the
// GPU by the workspace renderer (Ui/UiRenderDx9.cpp) at the size the preview shows them.
//
// The main thread publishes what each frame shows (ScrubFrames_Publish, in ScrubFrameCacheHost.h); the
// render thread takes it at the next present, when the game's finished frame is drawn into the preview.

#include "Ui/UiTypes.h"

#include <d3d9.h>

namespace SourceFilmmaker {

// Render thread, from the letterbox: `frame` is the game's finished frame (the whole back buffer). Keeps
// it when the published frame says it is a picture of its tick, and returns the texture the preview
// shows: `frame`, a kept frame the scrub serves, or the frame held while a seek is in flight.
IDirect3DTexture9* ScrubFrames_PreviewTexture(IDirect3DDevice9* device, IDirect3DTexture9* frame, const Ui::Rect& preview);

// Render thread: before a device reset or shutdown, every kept frame goes.
void ScrubFrames_DeviceLost();

} // namespace SourceFilmmaker
