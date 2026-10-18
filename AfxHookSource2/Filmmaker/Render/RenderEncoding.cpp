#include "RenderOutput.h"
#include <algorithm>
#include <sstream>
namespace Filmmaker {

const char* RenderNvencQualityFlags() {
	// Spatial AQ moves bits into flat and dark areas, where blocking shows first in game footage
	// (smoke, sky, shadowed corners); lookahead lets rate control see scene changes coming.
	// Both exist on every NVENC generation FFmpeg's h264_nvenc supports.
	return "-spatial-aq 1 -aq-strength 8 -rc-lookahead 20";
}

std::string BuildFfmpegVideoOptions(const RenderOutputSettings& settings, bool useNvenc,
	const std::string& fileStem, const std::string& resizeFilter) {
	const int crf = (std::clamp)(settings.crf, 0, 51);
	const int kbps = (std::clamp)(settings.bitrateKbps, 0, 400000);

	std::ostringstream o;
	if (useNvenc) {
		// p3, p5 and p7 rendered 3-second clips in the same time at 2560x1351 (2026-10-18): the GPU
		// encoder is never the slowest step, so it always runs its best preset. The speed setting
		// still selects the x264 preset below, where it changes the time per frame several-fold.
		o << "-c:v h264_nvenc -preset p7 -tune hq -profile:v high " << RenderNvencQualityFlags();
		if (kbps > 0)
			o << " -rc vbr -b:v " << kbps << "k -maxrate " << (kbps * 3 / 2) << "k";
		else
			o << " -rc vbr -cq " << crf << " -b:v 0";
	} else {
		const char* preset = settings.speed == RenderEncodeSpeed::Quality ? "slow"
			: settings.speed == RenderEncodeSpeed::Fast ? "veryfast" : "medium";
		// aq-mode 3 biases bits toward dark flat areas, where CS2 footage bands and blocks first.
		o << "-c:v libx264 -preset " << preset << " -profile:v high -x264-params aq-mode=3";
		if (kbps > 0)
			o << " -b:v " << kbps << "k";
		else
			o << " -crf " << crf;
	}
	// Convert RGB with the BT.709 matrix and tag the file BT.709. Without it FFmpeg converts with
	// BT.601 while every player and editor decodes HD as BT.709, which shifts and washes colours.
	// This -vf replaces the recorder's own "-vf setsar=sar=1/1" (FFmpeg keeps only the last
	// -vf), so the SAR is set here too. One token, no spaces: the command line is not quoted.
	// H.264 4:2:0 needs even dimensions: libx264 refuses an odd game window outright (a 2560x1351
	// window failed every frame, 2026-10-18), so an odd edge loses one pixel row or column here.
	o << " -vf setsar=sar=1/1,crop=trunc(iw/2)*2:trunc(ih/2)*2:0:0," << (resizeFilter.empty() ? "" : resizeFilter + ",")
		<< "scale=out_color_matrix=bt709:out_range=tv:flags=bicubic+accurate_rnd+full_chroma_int,format=yuv420p"
		<< " -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv";
	o << " -movflags +faststart {QUOTE}{AFX_STREAM_PATH}\\" << fileStem << ".mp4{QUOTE}";
	return o.str();
}

}
