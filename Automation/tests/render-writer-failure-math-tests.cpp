#include "../../AfxHookSource2/Filmmaker/Render/RenderWriterFailureMath.h"

#include <iostream>
#include <string>

using namespace Filmmaker;

namespace {

int g_failed = 0;

void Expect(bool ok, const char* label) {
	if (ok) return;
	std::cerr << "FAILED " << label << "\n";
	++g_failed;
}

void ExpectEq(const std::string& actual, const std::string& expected, const char* label) {
	if (actual == expected) return;
	std::cerr << "FAILED " << label << ":\n  got      '" << actual << "'\n  expected '" << expected << "'\n";
	++g_failed;
}

} // namespace

int main() {
	// Only the classic writer formats and EXR run without FFmpeg.
	Expect(RenderFormatUsesFfmpeg(RenderOutputFormat::Mp4), "mp4 needs ffmpeg");
	Expect(RenderFormatUsesFfmpeg(RenderOutputFormat::Avi), "avi needs ffmpeg");
	Expect(RenderFormatUsesFfmpeg(RenderOutputFormat::Gif), "gif needs ffmpeg");
	Expect(RenderFormatUsesFfmpeg(RenderOutputFormat::Png), "png needs ffmpeg");
	Expect(RenderFormatUsesFfmpeg(RenderOutputFormat::Tiff), "tiff needs ffmpeg");
	Expect(!RenderFormatUsesFfmpeg(RenderOutputFormat::Tga), "tga is the classic writer");
	Expect(!RenderFormatUsesFfmpeg(RenderOutputFormat::Bmp), "bmp is the classic writer");
	Expect(!RenderFormatUsesFfmpeg(RenderOutputFormat::Exr), "exr frames come from the HDR capture");

	// FFmpeg prints its reason last, across \r\n, with blank lines and padding.
	const std::string stderrText =
		"Input #0, rawvideo, from 'pipe:':\r\n  Duration: N/A\r\n\r\n"
		"[h264_nvenc @ 000001] OpenEncodeSessionEx failed: unsupported device (2)\r\n"
		"Error while opening encoder - maybe incorrect parameters such as bit_rate, rate, width or height\r\n";
	ExpectEq(RenderFfmpegTailLines(stderrText),
		"[h264_nvenc @ 000001] OpenEncodeSessionEx failed: unsupported device (2) / "
		"Error while opening encoder - maybe incorrect parameters such as bit_rate, rate, width or height",
		"last two lines, trimmed");
	ExpectEq(RenderFfmpegTailLines(""), "", "nothing said");
	ExpectEq(RenderFfmpegTailLines("\r\n \n"), "", "only blank lines");
	ExpectEq(RenderFfmpegTailLines("abcdefghij", 2, 8), "...fghij", "long text keeps its end");
	ExpectEq(RenderFfmpegTailLines("encoder failed", 1, 2), "ed", "short cap keeps the tail");
	ExpectEq(RenderFfmpegTailLines("encoder failed", 1, 0), "", "zero cap is empty");

	// The job's sentence: cause first, FFmpeg's words after, the count only when it adds anything.
	ExpectEq(RenderWriterFailureText(1, "a stream's output could not be opened", ""),
		"the output could not be written: a stream's output could not be opened", "reason only");
	ExpectEq(RenderWriterFailureText(3, "a stream could not write a frame",
		"could not start C:\\x\\ffmpeg\\bin\\ffmpeg.exe (Windows error 2)\n"),
		"the output could not be written: a stream could not write a frame. FFmpeg said: "
		"could not start C:\\x\\ffmpeg\\bin\\ffmpeg.exe (Windows error 2) (3 failures)", "reason, ffmpeg and count");
	ExpectEq(RenderWriterFailureText(1, "", ""), "the output could not be written", "nothing known");

	if (g_failed) {
		std::cerr << g_failed << " check(s) failed\n";
		return 1;
	}
	std::cout << "RenderWriterFailureMath tests passed.\n";
	return 0;
}
