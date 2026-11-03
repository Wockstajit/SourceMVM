#pragma once

#include <algorithm>
#include <cmath>

namespace SourceFilmmaker {

// During freeze time the scheduled round start is still ahead. Once play starts, the same
// timestamp anchors the round duration. Source's HUD truncates a partial remaining second.
inline int RoundSecondsRemaining(bool freeze, double start, double duration, double now) {
	if (!std::isfinite(start) || !std::isfinite(duration) || !std::isfinite(now)) return 0;
	return (int)(std::max)(0.0, (std::min)(86400.0, std::floor(start + (freeze ? 0.0 : duration) - now)));
}

} // namespace SourceFilmmaker
