#include "RenderRange.h"

#include <cmath>
#include <climits>
#include <cstdint>
#include <limits>

namespace Filmmaker {

bool RenderRange::Valid() const {
	return startTick < endTickExclusive;
}

int64_t RenderRange::TickCount64() const {
	const int64_t start = static_cast<int64_t>(startTick);
	const int64_t end = static_cast<int64_t>(endTickExclusive);

	if (start >= end)
		return 0;

	return end - start;
}

int RenderRange::TickCount() const {
	const int64_t ticks = TickCount64();

	if (ticks <= 0 || ticks > INT_MAX)
		return 0;

	return static_cast<int>(ticks);
}

int RenderRange::FrameCount(int outputFps, double demoTickRate) const {
	const int64_t ticks = TickCount64();

	if (ticks <= 0 || outputFps <= 0)
		return 0;

	if (!std::isfinite(demoTickRate))
		return 0;

	if (demoTickRate < 1.0 ||
		demoTickRate > static_cast<double>(INT_MAX))
		return 0;

	const double roundedRate = std::round(demoTickRate);

	if (std::abs(demoTickRate - roundedRate) > 0.001)
		return 0;

	const int64_t tickRate = static_cast<int64_t>(roundedRate);

	if (tickRate <= 0)
		return 0;

	if (ticks > std::numeric_limits<int64_t>::max() /
				static_cast<int64_t>(outputFps))
		return 0;

	const int64_t numerator =
		ticks * static_cast<int64_t>(outputFps);

	// frames = ceil(ticks * fps / tickRate)
	// Implemented as ceiling division: ceil(a / b) = 1 + ((a - 1) / b)
	const int64_t frames =
		1 + ((numerator - 1) / tickRate);

	if (frames <= 0 || frames > INT_MAX)
		return 0;

	return static_cast<int>(frames);
}

} // namespace Filmmaker
