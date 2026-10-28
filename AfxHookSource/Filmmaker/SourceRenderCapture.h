#pragma once

// A render job's frames in CS:S: the game's finished frame, read back at Present before the
// filmmaker's UI is drawn over it, converted to BGR, and handed on a worker thread to the shared
// recorder (Shared/RecordingSettings: ffmpeg or the classic image writer), as CS2's capture thread
// does (RenderSystemDX11Hooks.cpp). The job keeps the engine single-threaded (mat_queue_mode 0), so
// Present runs inside the frame that armed it and each armed frame is that frame's picture.

#include <string>

struct IDirect3DDevice9;

namespace advancedfx {
class COutVideoStreamCreator;
}

namespace SourceFilmmaker {

// Main thread: opens the output. Takes a reference on creator; the stream itself is made from the
// first frame's size.
bool RenderCapture_Begin(advancedfx::COutVideoStreamCreator* creator, std::string& error);
// Main thread: the next frame presented belongs to the job.
void RenderCapture_Arm();
// Present (before the UI is drawn): reads back an armed frame.
void RenderCapture_Present(IDirect3DDevice9* device);
// Frames read back, frames the recorder took, and frames it refused.
int RenderCapture_Captured();
int RenderCapture_Written();
int RenderCapture_Failures();
// Main thread: no more frames. The worker writes what is queued, closes the stream (the encoder
// finishes the file) and stops; RenderCapture_Finished turns true then.
void RenderCapture_End();
bool RenderCapture_Finished();
// Before a device reset: the readback surfaces go.
void RenderCapture_DeviceLost();

} // namespace SourceFilmmaker
