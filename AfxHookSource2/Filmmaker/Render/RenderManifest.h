#pragma once

// Job metadata sidecars: manifest.json and frames.csv, written into the take directory.
//
// WHY THIS EXISTS (plan section 12). A rendered folder full of frames with no record of what
// produced them cannot be reproduced, aligned in a compositor, or debugged after the fact.
// The manifest is written TWICE: once when the job starts (status "rendering") so a crash
// still leaves a truthful record, and once at the end with the outcome. frames.csv gets one
// row per captured output frame (frame index, demo tick, demo time, camera pose) and is
// flushed incrementally so a partially completed render remains readable.
//
// All functions run on the main thread (the session driver owns them). The CSV writer keeps a
// buffered FILE* open across the job; rows are tiny and appended once per rendered frame.

#include <string>

namespace Filmmaker {

struct RenderJob;

// Writes <takeDir>\manifest.json with status "rendering". Returns false when the file cannot
// be created (the driver treats that as an output-path failure and refuses to start).
bool RenderManifest_WriteStart(const RenderJob& job, const std::string& demoPathUtf8,
	const std::string& notes);

// Rewrites the manifest with the final outcome. `status` is "completed" | "canceled" |
// "failed"; framesWritten is the actual captured frame count.
bool RenderManifest_WriteFinal(const RenderJob& job, const std::string& demoPathUtf8,
	const char* status, int framesWritten, const std::string& notes);

// frames.csv lifecycle. Open writes the header row; Append adds one frame; Close flushes.
// Append/Close are safe no-ops when Open failed or was never called.
bool RenderFrameMap_Open(const std::wstring& takeDir);
void RenderFrameMap_Append(int frameIndex, int demoTick, double demoTime,
	const double origin[3], const double angles[3], double fov);
void RenderFrameMap_Close();

} // namespace Filmmaker
