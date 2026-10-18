// The FFmpeg output options a render job builds: encoder choice, the speed presets that only the
// CPU encoder honours, and the even-size crop H.264 4:2:0 needs.
#include "../../AfxHookSource2/Filmmaker/Render/RenderOutput.h"

#include <iostream>
#include <string>

using namespace Filmmaker;

namespace {

int g_failed = 0;

void Expect(bool ok, const char* label, const std::string& text) {
	if (ok) return;
	std::cerr << "FAILED " << label << ": " << text << "\n";
	++g_failed;
}

bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

} // namespace

int main() {
	RenderOutputSettings s;
	s.crf = 16;

	// NVENC: p3, p5 and p7 rendered in the same time, so every speed runs the best preset.
	for (RenderEncodeSpeed speed : {RenderEncodeSpeed::Quality, RenderEncodeSpeed::Balanced, RenderEncodeSpeed::Fast}) {
		s.speed = speed;
		const std::string o = BuildFfmpegVideoOptions(s, true, "everything");
		Expect(Has(o, "-c:v h264_nvenc -preset p7 "), "nvenc always uses p7", o);
		Expect(Has(o, "-rc vbr -cq 16 -b:v 0"), "nvenc constant quality", o);
	}

	// x264: the speed presets change the encode time several-fold, so they stay.
	s.speed = RenderEncodeSpeed::Quality;
	Expect(Has(BuildFfmpegVideoOptions(s, false, "e"), "-c:v libx264 -preset slow "), "x264 quality = slow", "");
	s.speed = RenderEncodeSpeed::Balanced;
	Expect(Has(BuildFfmpegVideoOptions(s, false, "e"), "-c:v libx264 -preset medium "), "x264 balanced = medium", "");
	s.speed = RenderEncodeSpeed::Fast;
	const std::string fast = BuildFfmpegVideoOptions(s, false, "e");
	Expect(Has(fast, "-c:v libx264 -preset veryfast "), "x264 fast = veryfast", fast);
	Expect(Has(fast, " -crf 16"), "x264 constant quality", fast);

	// An odd game window (2560x1351) made libx264 refuse every frame: the chain crops to even.
	for (bool nvenc : {false, true}) {
		const std::string o = BuildFfmpegVideoOptions(s, nvenc, "e");
		Expect(Has(o, " -vf setsar=sar=1/1,crop=trunc(iw/2)*2:trunc(ih/2)*2:0:0,scale="), "even-size crop precedes the colour scale", o);
		Expect(Has(o, "format=yuv420p -colorspace bt709"), "bt709 yuv420p output", o);
		Expect(o.find(' ') == std::string::npos || Has(o, " -vf setsar"), "filter chain is one token", o);
	}
	// A Deadlock resize goes after the crop and before the colour conversion.
	const std::string resized = BuildFfmpegVideoOptions(s, true, "e", "scale=1920:1080");
	Expect(Has(resized, "crop=trunc(iw/2)*2:trunc(ih/2)*2:0:0,scale=1920:1080,scale=out_color_matrix"), "resize filter order", resized);

	// Bitrate mode replaces constant quality on both encoders.
	s.bitrateKbps = 20000;
	Expect(Has(BuildFfmpegVideoOptions(s, true, "e"), "-rc vbr -b:v 20000k -maxrate 30000k"), "nvenc bitrate", "");
	Expect(Has(BuildFfmpegVideoOptions(s, false, "e"), " -b:v 20000k"), "x264 bitrate", "");

	if (g_failed) {
		std::cerr << g_failed << " check(s) failed\n";
		return 1;
	}
	std::cout << "Render encoding options: encoder presets, even-size crop, bt709 chain and bitrate mode passed\n";
	return 0;
}
