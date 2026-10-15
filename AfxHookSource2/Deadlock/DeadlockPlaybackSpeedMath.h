#pragma once

// Playback-speed arithmetic: the preset ladder, the clamp, and how a continuous value re-seeds the
// ladder. Header-only and engine-free so it can be tested without Deadlock running.
//
// WHY THE LADDER AND THE CONTINUOUS VALUE HAVE TO AGREE
//
// Two controls drive one dial. Deadlock's replay bar and our own stepper buttons walk a ladder of
// eight presets; the timeline's and the graph's sliders are continuous and logarithmic. If the
// dial only remembered a ladder INDEX, then setting 0.37x from a slider would have to snap to
// 0.25x, and the slider -- which re-seeds itself from the dial every frame -- would visibly crawl
// away from where the handle was dropped.
//
// So the dial remembers a SPEED, and the ladder is re-entered from it: stepping up from 0.37x goes
// to the first preset strictly above it. Strictly, not nearest, or a press near a preset can move
// the wrong way or not at all, which reads as a dead button.

namespace Deadlock {

// Deadlock's own replay speeds, from citadel_hud_replay.xml's SpeedDropdown, plus 0.1 at the slow
// end because a movie tool needs to crawl further than the game's UI does.
inline const double* PlaybackSpeed_Steps(int& count) {
	static const double kSteps[] = { 0.1, 0.25, 0.5, 1.0, 1.5, 2.0, 4.0, 8.0, 16.0 };
	count = (int)(sizeof(kSteps) / sizeof(kSteps[0]));
	return kSteps;
}

inline double PlaybackSpeed_Min() { return 0.05; }
inline double PlaybackSpeed_Max() { return 20.0; }

// Anything non-finite or non-positive becomes 1x rather than 0: a zero timescale is a frozen
// replay that looks exactly like a hang.
inline double PlaybackSpeed_Clamp(double speed) {
	if (!(speed > 0.0) || !(speed < 1.0e9)) return 1.0;
	if (speed < PlaybackSpeed_Min()) return PlaybackSpeed_Min();
	if (speed > PlaybackSpeed_Max()) return PlaybackSpeed_Max();
	return speed;
}

// The next preset strictly beyond `speed` in the direction `dir`. At either end of the ladder the
// value is returned unchanged, so a press at the limit is a no-op rather than a wrap-around.
inline double PlaybackSpeed_NextStep(double speed, int dir) {
	int count = 0;
	const double* steps = PlaybackSpeed_Steps(count);
	const double cur = PlaybackSpeed_Clamp(speed);

	// A hair of tolerance, so a value that IS a preset (to within floating-point noise) is treated
	// as being on that rung rather than a whisker above or below it.
	const double eps = 1.0e-6;
	if (dir > 0) {
		for (int i = 0; i < count; ++i)
			if (steps[i] > cur + eps) return steps[i];
		return cur;
	}
	if (dir < 0) {
		for (int i = count - 1; i >= 0; --i)
			if (steps[i] < cur - eps) return steps[i];
		return cur;
	}
	return cur;
}

// The preset nearest `speed`, for a readout that wants to name a rung.
inline int PlaybackSpeed_NearestIndex(double speed) {
	int count = 0;
	const double* steps = PlaybackSpeed_Steps(count);
	const double cur = PlaybackSpeed_Clamp(speed);
	int best = 0;
	double bestDist = cur - steps[0];
	if (bestDist < 0.0) bestDist = -bestDist;
	for (int i = 1; i < count; ++i) {
		double d = cur - steps[i];
		if (d < 0.0) d = -d;
		if (d < bestDist) { bestDist = d; best = i; }
	}
	return best;
}

} // namespace Deadlock
