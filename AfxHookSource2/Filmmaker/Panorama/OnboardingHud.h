#pragma once

// Clippy's onboarding panel (prototype): one full-screen Panorama root (#MvmOnboardingRoot) with
// the clapperboard guide and his speech bubble.
//
// Like the easter-egg overlay it lives in WHICHEVER context is up -- the main menu, or the in-game
// HUD once a map or demo is loaded -- and is rebuilt when the context changes. It is built only
// after someone opens the tutorial (Onboarding/Onboarding.h), so it costs nothing until then.
//
//   C++ -> JS : attribute "state" (tutorial state + cursor + dock height), then
//               $.MvmOnboarding.render() every frame while the guide is up.
//   JS  -> C++: the bubble's buttons issue `mirv_filmmaker onboarding ...` console commands.

#include "PanoramaBridge.h"

#include <string>

namespace Filmmaker {

class OnboardingHud {
public:
	// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame.
	void RunFrame();

private:
	bool BuildIfNeeded();
	void Teardown();

	PanoramaBridge m_bridge;
	void* m_context = nullptr;
	void* m_root = nullptr;
	short m_symState = -1;
	bool m_built = false;
	bool m_wasOn = false;
	// Set when the panel is built in a demo (the tutorial opened mid-demo, or a demo loaded while
	// it was up); cleared once the demo has been paused. Onboarding happens on a paused demo.
	bool m_pauseOnDemo = false;
	unsigned long long m_pauseAfterMs = 0;  // only once nothing has moved the demo for a beat
	unsigned long long m_pausedSinceMs = 0; // paused this long, steadily, and it is done
	unsigned long long m_freshPlayingSinceMs = 0; // playing this long, steadily, before it is paused
	unsigned long long m_seekWaitSinceMs = 0; // a lesson seek has waited this long for the demo to run
	unsigned long long m_pauseUntilMs = 0;  // give up after this (30 s), so it never catches a later Space
	unsigned m_seekSerial = 0;              // the last lesson seek seen (OnboardingState::seekSerial)
	unsigned long long m_holdAfterMs = 0;   // the hold's next pause, at most one per second
	unsigned long long m_playingSinceMs = 0;// how long the demo has been playing while it is held
	int m_playToSent = -1;                  // the `playto` target the demo was resumed for
	unsigned long long m_playToSentMs = 0;  // when it was resumed, to ask again if it did not take
	std::string m_lastJson;
};

OnboardingHud& OnboardingHudRef();

} // namespace Filmmaker
