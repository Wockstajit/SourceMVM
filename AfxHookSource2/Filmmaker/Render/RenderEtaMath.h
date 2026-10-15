#pragma once

// How long a render job will take, from its capture progress so far. Engine-free so
// Automation/tests/render-eta-math-tests.cpp can pin it.
//
// The capture rate is the average since capture began (physical renders done / wall seconds), so
// one slow or fast frame cannot swing the estimate; the remaining renders run at that rate. The
// encoder's flush after capture is not counted -- it is short next to the capture itself.

namespace Filmmaker {

struct RenderEta {
	bool valid = false;
	double elapsedS = 0.0;   // capture time so far
	double remainingS = 0.0; // estimated capture time still to go
	double totalS = 0.0;     // elapsedS + remainingS
};

inline RenderEta EstimateRenderTime(double captureElapsedS, long long done, long long total) {
	RenderEta e;
	e.elapsedS = captureElapsedS;
	if (!(captureElapsedS > 0.0) || done <= 0 || total <= 0) return e;
	const double rate = (double)done / captureElapsedS; // renders per second
	const long long left = total > done ? total - done : 0;
	e.remainingS = (double)left / rate;
	e.totalS = captureElapsedS + e.remainingS;
	e.valid = true;
	return e;
}

// The long-render rule: once the capture has run `afterS` seconds, a job estimated to take more
// than `longerThanS` in total qualifies (Snake: 10 s of capture is enough to read the frame rate;
// anything heading past 3 minutes gets the offer to play).
inline bool IsLongRender(const RenderEta& e, double afterS = 10.0, double longerThanS = 180.0) {
	return e.valid && e.elapsedS >= afterS && e.totalS > longerThanS;
}

} // namespace Filmmaker
