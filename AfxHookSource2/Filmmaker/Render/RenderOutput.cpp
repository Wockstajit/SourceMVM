#include "RenderOutput.h"
#include "RenderHost.h"

#include "../Platform/FolderPicker.h"
#include "../Debug/MvmDebugLog.h" // MvmDebugLog_* (render.output probes)

#include "../../../Shared/RecordingSettings.h" // SetFfmpegSetting (per-job dynamic presets)

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <windows.h>

namespace Filmmaker {

namespace {

struct FormatInfo {
	RenderOutputFormat format;
	const char* name;
	const char* token;
	bool supported; // false == the bundled encoder path cannot produce it yet
	const char* note;
	bool sequence;
};

// `supported` false means the bundled path genuinely cannot produce that format yet, and the UI
// must say so rather than offering a button that fails at start time. The recorder preset itself
// is registered per pass at start time (RenderOutput_PrepareRecorderPreset).

const FormatInfo kFormats[] = {
	{ RenderOutputFormat::Mp4, "MP4 (H.264)", "mp4", true,
	  "H.264 MP4 with adjustable quality (CRF or bitrate) and optional NVENC GPU encoding.", false },
	{ RenderOutputFormat::Avi, "AVI (HuffYUV)", "avi", true,
	  "Lossless HuffYUV in AVI. Very large files; use as an intermediate, not a delivery.", false },
	{ RenderOutputFormat::Gif, "GIF", "gif", true,
	  "Single-pass palette GIF for short preview clips. Long or large renders make huge files.", false },
	{ RenderOutputFormat::Png, "PNG sequence", "png", true,
	  "Lossless numbered PNG frames (via FFmpeg). A good master for compositing.", true },
	{ RenderOutputFormat::Tiff, "TIFF sequence", "tiff", true,
	  "Lossless numbered TIFF frames (via FFmpeg).", true },
	{ RenderOutputFormat::Tga, "TGA sequence", "tga", true,
	  "Lossless numbered TGA frames from the classic writer. The safest master for compositing.", true },
	{ RenderOutputFormat::Bmp, "BMP sequence", "bmp", true,
	  "Lossless numbered BMP frames. Larger than TGA with no quality gain.", true },
	{ RenderOutputFormat::Exr, "EXR sequence", "exr", true,
	  "Half-float EXR frames of the PRE-TONEMAP linear HDR scene (the exact-compositing master; "
	  "light above 1.0 survives). Written to hdr/; an LDR TGA reference of each frame rides the "
	  "beauty stream. Requires HUD mode None.", true },
};

const FormatInfo& Info(RenderOutputFormat format) {
	for (const FormatInfo& f : kFormats) {
		if (f.format == format)
			return f;
	}
	return kFormats[0];
}

RenderOutputSettings s_settings;
RenderPasses s_passes;
std::atomic<bool> s_pickPending{false};
// The picker runs on a worker thread, so its result is handed over through a guarded staging
// slot and copied into s_settings by the main thread in RenderOutput_PumpMainThread. Writing
// s_settings.folder directly from the worker would be a data race against every per-frame
// reader of the settings (both HUDs build their state JSON from them).
std::mutex s_pickMutex;
std::wstring s_pickedFolder;
std::atomic<bool> s_pickedReady{false};

bool DirectoryExists(const std::wstring& path) {
	const DWORD attr = ::GetFileAttributesW(path.c_str());
	return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

} // namespace

const char* RenderFormatName(RenderOutputFormat format) { return Info(format).name; }
const char* RenderFormatToken(RenderOutputFormat format) { return Info(format).token; }
// A format the bundled path makes can still be one this game cannot feed (RenderHost.h).
bool RenderFormatSupported(RenderOutputFormat format) { return Info(format).supported && !RenderHost_FormatUnavailable(format); }
const char* RenderFormatNote(RenderOutputFormat format) {
	const char* unavailable = RenderHost_FormatUnavailable(format);
	return unavailable ? unavailable : Info(format).note;
}
bool RenderFormatIsSequence(RenderOutputFormat format) { return Info(format).sequence; }

bool RenderFormatFromToken(const char* token, RenderOutputFormat& out) {
	if (!token)
		return false;
	for (const FormatInfo& f : kFormats) {
		if (0 == _stricmp(f.token, token)) {
			out = f.format;
			return true;
		}
	}
	return false;
}

std::string SanitizeProjectName(const std::string& raw) {
	std::string out;
	out.reserve(raw.size());
	bool lastWasSpace = false;
	for (char c : raw) {
		// Anything that could traverse out of the chosen root, or that Windows rejects in a
		// filename, becomes an underscore. Doing this here (not at the call site) is what makes
		// "never write outside the folder the user picked" a property of the module.
		const bool illegal = (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
			c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char)c < 0x20);
		if (illegal) {
			out.push_back('_');
			lastWasSpace = false;
			continue;
		}
		if (c == ' ' || c == '\t') {
			if (!lastWasSpace && !out.empty())
				out.push_back('_');
			lastWasSpace = true;
			continue;
		}
		out.push_back(c);
		lastWasSpace = false;
	}
	while (!out.empty() && (out.back() == '_' || out.back() == '.'))
		out.pop_back();
	if (out.empty())
		return "render";
	if (out.size() > 64)
		out.resize(64);
	return out;
}

std::wstring BuildTakeDirectory(const RenderOutputSettings& settings) {
	if (settings.folder.empty())
		return std::wstring();

	const std::string project = SanitizeProjectName(settings.project);

	std::time_t now = std::time(nullptr);
	std::tm tm{};
	localtime_s(&tm, &now);
	char stamp[32] = {0};
	std::snprintf(stamp, sizeof(stamp), "_%04d-%02d-%02d_%02d-%02d-%02d",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

	std::wstring base = settings.folder;
	if (!base.empty() && base.back() != L'\\' && base.back() != L'/')
		base.push_back(L'\\');
	base.append(project.begin(), project.end());
	base.append(stamp, stamp + std::strlen(stamp));

	// Two jobs started inside the same second must not land in one directory: the manifest and
	// the frame map are per-take, so a collision would interleave two jobs' metadata.
	std::wstring candidate = base;
	for (int suffix = 2; suffix < 100 && DirectoryExists(candidate); ++suffix) {
		wchar_t tail[8] = {0};
		swprintf_s(tail, L"_%d", suffix);
		candidate = base + tail;
	}
	return candidate;
}

RenderOutputSettings& RenderOutputSettingsRef() { return s_settings; }
RenderPasses& RenderPassesRef() { return s_passes; }

bool RenderOutput_PickFolderPending() { return s_pickPending.load(); }

void RenderOutput_PickFolderAsync() {
	bool expected = false;
	if (!s_pickPending.compare_exchange_strong(expected, true))
		return; // a dialog is already up; a second one would be modal against nothing

	// Detached worker: PickFolderBlocking initialises its own COM apartment and blocks until the
	// user answers. Running it on the main thread would freeze the game (and on the render thread
	// would deadlock the present hook).
	std::thread([] {
		std::wstring chosen;
		const bool ok = PickFolderBlocking(chosen, L"Select the render output folder");
		if (ok && !chosen.empty()) {
			std::lock_guard<std::mutex> lock(s_pickMutex);
			s_pickedFolder = chosen;
			s_pickedReady.store(true);
		}
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("render.output", "folderpick ok=%d len=%d",
				ok ? 1 : 0, (int)chosen.size());
		s_pickPending.store(false);
	}).detach();
}

std::string RenderOutput_PrepareRecorderPreset(const RenderOutputSettings& settings,
	bool nvencAvailable, const std::string& presetName, const std::string& fileStem,
	std::string& outNote) {
	outNote.clear();

	auto registerPreset = [&](const std::string& options) -> std::string {
		if (!advancedfx::CRecordingSettings::SetFfmpegSetting(presetName.c_str(), options.c_str())) {
			outNote += "Could not register the job's FFmpeg preset. ";
			return std::string();
		}
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("render.output", "preset %s = %s", presetName.c_str(), options.c_str());
		return presetName;
	};
	// Every file a pass writes is named for what it shows ("world", "everything"), inside the
	// stream folder the driver points at the pass type ("beauty"). Single backslash: the recorder
	// expands {AFX_STREAM_PATH} and passes the rest through.
	const std::string file = "{QUOTE}{AFX_STREAM_PATH}\\" + fileStem;

	switch (settings.format) {
	case RenderOutputFormat::Mp4: {
		// Auto picks GPU whenever the real test encode proved it works. Encode is the dominant
		// per-frame cost of a job, so this is the difference between a render that keeps up with
		// the capture pipeline and one that blocks on x264 every frame.
		const bool autoEncoder = settings.encoder == RenderEncoder::Auto;
		const bool wantNvenc = settings.encoder == RenderEncoder::Nvenc
			|| (autoEncoder && nvencAvailable);
		const bool useNvenc = wantNvenc && nvencAvailable;
		if (wantNvenc && !useNvenc)
			outNote += "NVENC unavailable on this machine; encoding on CPU x264 instead. ";
		else if (autoEncoder && useNvenc)
			outNote += "Encoding on GPU (NVENC) -- proven available by the startup probe. ";
		else if (autoEncoder)
			outNote += "Encoding on CPU (x264): no working NVENC on this machine. ";
		return registerPreset(BuildFfmpegVideoOptions(settings, useNvenc, fileStem));
	}
	case RenderOutputFormat::Avi:
		return registerPreset("-c:v huffyuv " + file + ".avi{QUOTE}");
	case RenderOutputFormat::Gif: {
		// Single-pass palette: split the stream, build the palette from the first branch, apply
		// it to the second. All one argument with no spaces, so no quoting needed.
		return registerPreset("-filter_complex [0:v]split[a][b];[a]palettegen=stats_mode=diff[p];"
			"[b][p]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle"
			" -loop 0 " + file + ".gif{QUOTE}");
	}
	// Image sequences: the driver gives each sequence its own folder (beauty\world\), so the
	// frames keep the plain frame_ name.
	case RenderOutputFormat::Png:
		return registerPreset("-c:v png -f image2 {QUOTE}{AFX_STREAM_PATH}\\frame_%06d.png{QUOTE}");
	case RenderOutputFormat::Tiff:
		return registerPreset("-c:v tiff -f image2 {QUOTE}{AFX_STREAM_PATH}\\frame_%06d.tiff{QUOTE}");
	case RenderOutputFormat::Tga:
	case RenderOutputFormat::Bmp:
		return "afxClassic";
	case RenderOutputFormat::Exr:
		// The EXR frames themselves come from the HDR capture path (RenderHdrCapture), not the
		// recorder; the classic TGA stream provides frame pacing plus an LDR reference.
		outNote += "HDR EXR frames land in hdr/ next to the LDR reference stream. ";
		return "afxClassic";
	default:
		outNote += "Format has no runnable encoder path. ";
		return std::string();
	}
}

void RenderOutput_PumpMainThread() {
	if (!s_pickedReady.load())
		return; // cheap unlocked check; the lock below re-tests before taking the value
	std::wstring chosen;
	{
		std::lock_guard<std::mutex> lock(s_pickMutex);
		if (!s_pickedReady.load())
			return;
		chosen.swap(s_pickedFolder);
		s_pickedReady.store(false);
	}
	s_settings.folder = chosen;
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.output", "folder applied len=%d", (int)s_settings.folder.size());
}

} // namespace Filmmaker
