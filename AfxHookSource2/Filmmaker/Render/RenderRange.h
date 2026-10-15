#pragma once

#include "RenderTypes.h"

#include <cstdint>

namespace Filmmaker {

struct RenderRange {
	RenderRangeMode mode = RenderRangeMode::CustomTicks;
	int startTick = 0;
	int endTickExclusive = 0;
	bool snapToTicks = false;

	bool Valid() const;

	int64_t TickCount64() const;
	int TickCount() const;
	int FrameCount(int outputFps, double demoTickRate) const;
};

// Camera Path is one choice that shows up in two places: picking the Camera Path RANGE also makes
// the path the camera, and picking the Camera Path CAMERA renders the path's span. The one range
// kept is Selected Segment: it is already a piece of the path, chosen on purpose.
inline bool PathCameraTakesPathRange(RenderRangeMode current) {
	return current != RenderRangeMode::CameraPathBounds && current != RenderRangeMode::SelectedCameraSegment;
}

} // namespace Filmmaker
