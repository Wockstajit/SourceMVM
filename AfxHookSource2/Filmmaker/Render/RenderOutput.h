#pragma once

// Output + pass settings for a render job, and the translation from those user-facing choices
// into the inherited AdvancedFX recorder's vocabulary.
//
// WHY A SEPARATE MODULE. RenderCommand.cpp owns console grammar and RenderRange owns time; this
// owns "where the files go and what they are". Keeping it apart is what lets the mapping to
// `mirv_streams` / `advancedfx::CRecordingSettings` be changed (or a second encoder path added)
// without touching the range model, validation, or the UI bridge.
//
// HONESTY RULE (plan section 10 "Honest UI Labels"): every format exposed here maps to a
// recording preset that actually exists in Shared/RecordingSettings.cpp. Formats the bundled
// encoder path cannot currently produce are NOT listed as if they worked -- FormatSupported()
// answers that question and the UI shows the unsupported ones as disabled with the reason.

#include "RenderCaptureMath.h"
#include "RenderTypes.h"

#include <string>

namespace Filmmaker {

// Which encoder the video formats use. NVENC is the fast path (hardware) and is only honored
// when RenderEncoderDetect proved it works; the job silently falls back to x264 with a warning
// otherwise, so a job never fails just because the GPU probe was wrong.
enum class RenderEncoder {
	X264 = 0,   // CPU libx264 -- always available
	Nvenc = 1,  // NVIDIA hardware encoder via FFmpeg h264_nvenc
	// Default. Resolves to Nvenc when the real test encode PROVED it works on this machine, else
	// x264. Measured 2026-10-18 at 2560x1351: a rendered frame costs what the game's own frame
	// costs (about 10 ms with the custom lighting off, the live GPU time with it on) and NVENC
	// never adds to that, while x264 adds up to a third at -preset slow. A 600 fps job pays
	// whatever the encoder adds 600 times per demo-second. Auto is a resolution step only: an
	// explicit CPU/GPU pick from the user is always honoured.
	Auto = 2
};

// Speed/quality trade of the encode itself (maps to encoder presets, see BuildFfmpegVideoOptions).
enum class RenderEncodeSpeed {
	Quality = 0,   // x264 slow / nvenc p7
	Balanced = 1,  // x264 medium / nvenc p5
	Fast = 2       // x264 veryfast / nvenc p3
};

struct RenderOutputSettings {
	// Root the user picked with the native folder dialog. Empty = not chosen yet, which is a
	// fatal validation error: a job must never silently write into the game directory.
	std::wstring folder;
	std::string project = "mvm_render";
	RenderOutputFormat format = RenderOutputFormat::Mp4;
	int fps = 60;
	// There is no output size: a job always writes the game resolution. The scene cannot render
	// larger than the game, and a smaller size would only be an FFmpeg resample, so the size is
	// chosen in CS2's own video settings.

	// -- video quality (MP4; AVI stays lossless HuffYUV) --
	RenderEncoder encoder = RenderEncoder::Auto;
	RenderEncodeSpeed speed = RenderEncodeSpeed::Balanced;
	// Constant-quality factor when bitrateKbps == 0. x264 CRF / NVENC CQ; lower = better.
	// 16 by default: renders are editing masters, and at 18 NVENC footage showed blocking in
	// smoke, foliage and dark corners (player reports, 2026-10).
	int crf = 16;
	// > 0 switches to target-bitrate mode at this rate (delivery-size control).
	int bitrateKbps = 0;
};

// Display name for a format, e.g. "MP4 (H.264)".
const char* RenderFormatName(RenderOutputFormat format);

// Console token used by "mirv_filmmaker render output format <token>".
const char* RenderFormatToken(RenderOutputFormat format);

// Parses a console token back to a format. Returns false when unknown.
bool RenderFormatFromToken(const char* token, RenderOutputFormat& out);

// Whether a job may currently be started in this format (the bundled encoder path produces it).
bool RenderFormatSupported(RenderOutputFormat format);

// One short sentence of user-facing truth about the format -- lossy vs lossless, sequence vs
// container, or exactly why it is unavailable. Never markets an unimplemented path.
const char* RenderFormatNote(RenderOutputFormat format);

// True when the format is written as a numbered image sequence rather than one container file.
bool RenderFormatIsSequence(RenderOutputFormat format);

// Strips path separators, drive letters, and anything else that could escape the chosen output
// root, then collapses whitespace. Empty input yields "render".
std::string SanitizeProjectName(const std::string& raw);

// "<folder>\<project>_<yyyy-mm-dd_hh-mm-ss>" -- the take directory for one job. Never returns a
// path that already exists (a suffix is appended), so a job cannot silently overwrite another.
std::wstring BuildTakeDirectory(const RenderOutputSettings& settings);

// The NVENC rate-control and adaptive-quantisation flags every job uses. The startup probe
// (RenderEncoderDetect) test-encodes with exactly these, so "NVENC available" means these work.
const char* RenderNvencQualityFlags();

// FFmpeg output-options string for MP4, honoring encoder/speed/crf/bitrate, writing
// {AFX_STREAM_PATH}\<fileStem>.mp4. `useNvenc` is the PROVEN availability (RenderEncoderDetect),
// not the wish -- the caller passes what the probe said and the builder falls back to x264 when
// it is false. `resizeFilter` (Deadlock's output size) runs before the colour conversion; CS2
// jobs never resize. Exposed for tests; jobs go through RenderOutput_PrepareRecorderPreset.
std::string BuildFfmpegVideoOptions(const RenderOutputSettings& settings, bool useNvenc,
	const std::string& fileStem, const std::string& resizeFilter = std::string());

// Registers/refreshes one pass's recorder preset under `presetName` and returns the
// recording-settings name its capture stream should use ("" = the format has no runnable path).
// Video formats write {AFX_STREAM_PATH}\<fileStem>.<ext>; image sequences write numbered frames
// into the stream folder. TGA/BMP use the static classic preset. outNote receives one human
// sentence about what was chosen (encoder fallback, ...).
std::string RenderOutput_PrepareRecorderPreset(const RenderOutputSettings& settings,
	bool nvencAvailable, const std::string& presetName, const std::string& fileStem,
	std::string& outNote);

// -- live settings owned by the render workspace --

RenderOutputSettings& RenderOutputSettingsRef();
RenderPasses& RenderPassesRef();

// Opens the native folder picker on a detached worker thread (it is blocking and must never run
// on the render thread -- see Platform/FolderPicker.h) and stores the result into the live
// settings. Returns immediately; PickFolderPending() reports whether a dialog is up.
void RenderOutput_PickFolderAsync();
bool RenderOutput_PickFolderPending();

// Main thread, once per frame: applies a folder the picker worker finished choosing. The worker
// cannot write the settings itself without racing every per-frame reader of them.
void RenderOutput_PumpMainThread();

} // namespace Filmmaker
