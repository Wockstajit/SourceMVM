#pragma once

// Turning a demo timestamp into a usable frame delta.
//
// Header-only and engine-free on purpose, following the repo's rule that anything with real
// arithmetic gets split out and unit tested: the hook itself cannot be tested, but this can, and
// it is exactly the kind of code where a plausible-looking mistake stays invisible for months.
//
// It exists because demo time is not monotonic. On the CS2 side, view-setup demo time steps
// BACKWARDS by a few milliseconds on ordinary frames, routinely. The obvious implementation --
// treat any negative delta as a seek and reset -- therefore fires constantly during normal
// playback, and every shot-driven camera effect built on it silently resets itself and reads as a
// smoothing bug rather than a clock bug. That cost real time to find.
//
// So: small backwards steps are jitter and yield no elapsed time. Only a large backwards jump is a
// seek. A large forwards jump is a hitch or a load and is clamped rather than integrated, because
// a camera that lurches half a second in one frame looks broken even when the maths is right.
//
// Both DeadlockViewSetup and DeadlockShot need this, and having two copies of the tolerances would
// mean fixing a measured constant twice.

namespace Filmmaker {

struct DemoClockConfig {
	// Below this magnitude, a backwards step is ordinary jitter rather than a seek. Measured on
	// CS2 as a few milliseconds; 50 ms leaves generous headroom without swallowing a real seek,
	// which is always far larger.
	double backwardsJitterTolerance = 0.050;

	// A forwards jump larger than this is a hitch, a load or a resume from pause. Clamped, not
	// integrated.
	double maxForwardStep = 0.5;
};

enum class DemoClockEvent {
	FirstSample,   // no previous time; nothing to integrate yet
	Advanced,      // ordinary forward progress
	Jitter,        // a small backwards step: not a seek, no elapsed time
	Seek,          // a large backwards jump: the world moved
	Clamped,       // a large forward jump: clamped to maxForwardStep
};

struct DemoClockStep {
	double delta = 0.0;
	DemoClockEvent event = DemoClockEvent::FirstSample;

	// True when downstream state (a playback cursor, a smoothing filter) should be reset. Only a
	// real seek justifies that; jitter explicitly does not.
	bool shouldReset() const { return event == DemoClockEvent::Seek; }
};

class DemoClock {
public:
	explicit DemoClock(DemoClockConfig config = DemoClockConfig()) : m_config(config) {}

	// Feeds the current demo time and returns the step to integrate.
	DemoClockStep Sample(double demoTime) {
		DemoClockStep step;

		if (!m_haveLast) {
			m_haveLast = true;
			m_last = demoTime;
			step.event = DemoClockEvent::FirstSample;
			step.delta = 0.0;
			return step;
		}

		const double raw = demoTime - m_last;

		if (raw < -m_config.backwardsJitterTolerance) {
			m_last = demoTime;
			step.event = DemoClockEvent::Seek;
			step.delta = 0.0;
			return step;
		}
		if (raw < 0.0) {
			// Jitter deliberately does NOT move the baseline.
			//
			// Advancing it here looks harmless and quietly over-counts: the next real frame then
			// measures from the dipped value and re-covers the dip, so a run of jittery frames
			// accumulates MORE elapsed time than the demo actually advanced and every camera move
			// finishes early. Holding the high-water mark makes the integrated total come out
			// exactly equal to the true span, which is what a shot's duration depends on. The unit
			// test asserts precisely that.
			step.event = DemoClockEvent::Jitter;
			step.delta = 0.0;
			return step;
		}

		m_last = demoTime;

		if (raw > m_config.maxForwardStep) {
			step.event = DemoClockEvent::Clamped;
			step.delta = m_config.maxForwardStep;
			return step;
		}

		step.event = DemoClockEvent::Advanced;
		step.delta = raw;
		return step;
	}

	// Forgets the previous sample, so the next one is a FirstSample again. Use on a demo change,
	// never as a response to jitter.
	void Reset() { m_haveLast = false; m_last = 0.0; }

	bool HaveSample() const { return m_haveLast; }
	double LastTime() const { return m_last; }

	const DemoClockConfig& Config() const { return m_config; }
	void SetConfig(const DemoClockConfig& config) { m_config = config; }

private:
	DemoClockConfig m_config;
	bool m_haveLast = false;
	double m_last = 0.0;
};

} // namespace Filmmaker
