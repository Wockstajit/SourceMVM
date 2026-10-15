#include "../../AfxHookSource2/Filmmaker/Movie/CameraShakeTrackerMath.h"

#include <cstdlib>
#include <iostream>

using Filmmaker::TrackerBreak;
using Filmmaker::ClassifyTrackerSample;
using Filmmaker::TrackerTeleportDistance;
using Filmmaker::kTrackerMinSampleInterval;

namespace {

int g_failures = 0;

void Check(bool ok, const char *message) {
	if (!ok) {
		std::cerr << "FAIL: " << message << '\n';
		++g_failures;
	}
}

} // namespace

int main() {
	const double tick = 1.0 / 64;

	// The reported bug, from the 2026-09-19 log. An ordinary running stride landed on a frame
	// whose demo dt was 0.9 ms, so the old speed test read 4622 u/s and reset the tracker --
	// Weapon Bob stopped and snapped back mid-stride.
	Check(ClassifyTrackerSample(0.0009, 1.8, 3.5) == TrackerBreak::None,
	      "a 3.9-unit stride on a 0.9 ms demo step is continuous, not a teleport");
	Check(ClassifyTrackerSample(0.0004, -1.3, 2.2) == TrackerBreak::None,
	      "a 2.6-unit stride on a 0.4 ms demo step is continuous, not a teleport");

	// The real seeks from the same log must still be caught.
	Check(ClassifyTrackerSample(0.0138, -425.0, -256.4) == TrackerBreak::Teleport,
	      "a 496-unit jump is a seek");
	Check(ClassifyTrackerSample(0.0171, 2164.5, -595.1) == TrackerBreak::Teleport,
	      "a 2245-unit jump is a seek");
	Check(ClassifyTrackerSample(-39.4344, 0, 0) == TrackerBreak::Gap,
	      "a backwards demo clock of many seconds is a seek");

	// Ordinary play at every plausible speed stays continuous, on a normal tick and on a
	// pathologically short one.
	for (double speed : {0.0, 120.0, 250.0, 320.0, 1000.0}) {
		const double move = speed * tick;
		Check(ClassifyTrackerSample(tick, move, 0) == TrackerBreak::None,
		      "a plausible player speed over a full tick is continuous");
		Check(ClassifyTrackerSample(0.0005, move, 0) == TrackerBreak::None,
		      "a plausible player speed over a sub-millisecond step is continuous");
	}

	// The floor is what makes that work, and it must not blunt the test on long samples: a
	// 0.4 s stall still has to catch a jump, just a proportionally larger one.
	Check(TrackerTeleportDistance(0.0001) == TrackerTeleportDistance(0.0),
	      "any interval below the floor is treated as the floor");
	Check(TrackerTeleportDistance(0.4) > TrackerTeleportDistance(kTrackerMinSampleInterval),
	      "a longer sample allows proportionally more travel");
	Check(ClassifyTrackerSample(0.4, 300.0, 0) == TrackerBreak::None,
	      "a long sample of ordinary running is continuous");
	Check(ClassifyTrackerSample(0.4, 5000.0, 0) == TrackerBreak::Teleport,
	      "a long sample still catches a real jump");

	// Boundaries of the gap window.
	Check(ClassifyTrackerSample(-0.04, 0, 0) == TrackerBreak::None,
	      "the routine backwards jitter of the demo clock is not a seek");
	Check(ClassifyTrackerSample(-0.06, 0, 0) == TrackerBreak::Gap,
	      "a backwards step past the tolerance is a seek");
	Check(ClassifyTrackerSample(0.6, 0, 0) == TrackerBreak::Gap,
	      "a long stall restarts the tracker");
	Check(ClassifyTrackerSample(0.0, 0, 0) == TrackerBreak::None,
	      "a same-tick hold is continuous");

	// Vertical movement is not part of the planar test: a lift or a fall is not a cut.
	Check(ClassifyTrackerSample(tick, 0, 0) == TrackerBreak::None,
	      "standing still is continuous");

	if (g_failures) {
		std::cerr << g_failures << " failure(s)\n";
		return EXIT_FAILURE;
	}
	std::cout << "camera-shake-tracker-math-tests: ok\n";
	return EXIT_SUCCESS;
}
