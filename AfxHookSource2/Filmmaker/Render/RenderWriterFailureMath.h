#pragma once

// What a render job tells the user when its output cannot be written.
//
// A viewer reported every render ending in "capture writer reported N output failure(s);
// refusing a completed manifest" (2026-11-18). The job had rendered the whole take before it said
// so, and the real cause (FFmpeg missing, not starting, or stopping on its arguments) only reached
// the console. So the job now checks FFmpeg before it starts, stops at the first writer failure,
// and puts the cause in its failure text, which the Render workspace shows.
//
// Engine-free; unit-tested in Automation/tests/render-writer-failure-math-tests.cpp.

#include "RenderTypes.h"

#include <string>
#include <vector>

namespace Filmmaker {

// Every format but TGA, BMP and EXR is encoded by FFmpeg. TGA and BMP use the classic image
// writer, and EXR's frames come from the HDR capture with a TGA reference stream.
inline bool RenderFormatUsesFfmpeg(RenderOutputFormat format) {
	switch (format) {
	case RenderOutputFormat::Tga:
	case RenderOutputFormat::Bmp:
	case RenderOutputFormat::Exr:
		return false;
	default:
		return true;
	}
}

// The last `maxLines` non-empty lines of what FFmpeg wrote, joined by " / ", at most `maxChars`
// long (the end is kept: FFmpeg prints its reason last).
inline std::string RenderFfmpegTailLines(const std::string& text, size_t maxLines = 2, size_t maxChars = 300) {
	std::vector<std::string> lines;
	std::string line;
	auto flush = [&]() {
		size_t a = line.find_first_not_of(" \t");
		size_t b = line.find_last_not_of(" \t");
		if (a != std::string::npos) lines.push_back(line.substr(a, b - a + 1));
		line.clear();
	};
	for (char c : text) {
		if (c == '\n' || c == '\r') flush();
		else line.push_back(c);
	}
	flush();
	std::string out;
	const size_t first = lines.size() > maxLines ? lines.size() - maxLines : 0;
	for (size_t i = first; i < lines.size(); ++i) {
		if (!out.empty()) out += " / ";
		out += lines[i];
	}
	if (out.size() > maxChars) {
		if (maxChars <= 3) return out.substr(out.size() - maxChars);
		out = "..." + out.substr(out.size() - (maxChars - 3));
	}
	return out;
}

// The job's failure text for `failures` writer failures. `reason` is the first failure's own
// description (AfxStreams_GetWriterFailure), `ffmpegTail` what FFmpeg last said.
inline std::string RenderWriterFailureText(int failures, const std::string& reason, const std::string& ffmpegTail) {
	std::string out = "the output could not be written";
	if (!reason.empty()) out += ": " + reason;
	const std::string said = RenderFfmpegTailLines(ffmpegTail);
	if (!said.empty()) out += ". FFmpeg said: " + said;
	if (failures > 1) out += " (" + std::to_string(failures) + " failures)";
	return out;
}

} // namespace Filmmaker
