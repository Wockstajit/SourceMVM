#pragma once

// Probes the bundled FFmpeg for a WORKING NVENC hardware encoder.
//
// WHY A REAL PROBE. "h264_nvenc appears in `ffmpeg -encoders`" only proves the build was
// compiled with NVENC support; it says nothing about the machine's GPU or driver. The probe
// therefore runs an actual 3-frame encode of a synthetic input to /dev/null: exit code 0 means
// hardware encoding genuinely works here, anything else means it does not. The result is what
// lets the render UI offer GPU encoding honestly instead of failing at job start.
//
// The probe is a one-shot background process (a few hundred ms on success, bounded by a hard
// timeout on failure); EnsureProbe is idempotent and cheap to call every frame.

namespace Filmmaker {

enum class NvencProbe {
	Unknown = 0,     // never probed
	Probing = 1,     // probe process running
	Available = 2,   // test encode succeeded -- NVENC works on this machine
	Unavailable = 3  // ffmpeg missing, NVENC not compiled in, or no working NVIDIA GPU/driver
};

NvencProbe RenderEncoderDetect_State();

// Starts the background probe if it has not run yet. Safe to call per frame.
void RenderEncoderDetect_EnsureProbe();

// One-line human answer for the UI/console ("GPU encoding available (NVENC)" etc).
const char* RenderEncoderDetect_Describe();

} // namespace Filmmaker
