#pragma once

// Onboarding tutorial (prototype): Clippy the clapperboard walks a new user through a short
// sample lesson. Nothing opens it automatically; `mvm_onboarding` (or
// `mirv_filmmaker onboarding`) is the only way in.
//
// This module owns the tutorial STATE -- open/paused, chosen route, step, chatter -- so it
// survives the Panorama context switch between the main menu and the in-game HUD. The words, the
// character and every animation live in the panel script (Panorama/OnboardingJs.h); its buttons
// come back here as console commands. Main/UI thread only: the console dispatch and the HUD pump
// both run there.

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

struct OnboardingState {
	bool on = false;       // the guide is up (bubble, or the paused pill)
	bool closed = false;   // exited: only the "Resume / Restart" pill shows
	bool voice = true;     // chatter sound while he talks (on until the user turns it off)
	const char* route = ""; // "" (opening choices), "film", "cs2" or "app"
	int step = 0;          // lesson step; one past the last step is the finale
	unsigned rev = 0;      // bumps on every change that should make him say his line again
	// Where the user dragged him: the panel's bottom-left corner as screen fractions. Negative
	// means "not placed", and the panel picks its own spot on the left.
	double posX = -1.0, posY = -1.0;
	// Linear mode: while a lesson step is up, the director hotkeys it does not name do nothing
	// (see Onboarding_BlocksKey). Set by the panel on every step.
	bool locked = false;
	unsigned seekSerial = 0; // bumps on every lesson seek; the panel driver re-pauses after it
	int seekTick = -1;       // a lesson seek waiting for the demo to run (the panel driver sends it)
	int playToTick = -1;     // a lesson is letting the demo play up to this tick (`playto`)
};

const OnboardingState& Onboarding_State();

// Linear mode for MovieMode::OnKey: true when the tutorial is up, the current step is locked
// and `vkey` is a director hotkey (Space, arrows, J, K, L, F, T, Esc, F8, Delete, Z) the step
// does not allow. Movement and mouse look are never blocked.
bool Onboarding_BlocksKey(int vkey);

// Linear mode for mouse clicks, checked by the window procedure before anything else sees a
// click (WindowInputHooks.cpp): true when a locked step is up and (x, y) falls outside every zone
// the panel allows (Clippy's bubble, the lit control). Panorama's own hit testing cannot be relied
// on for this: some workspace panels take clicks through layers drawn above the tutorial's dim.
bool Onboarding_BlocksClick(int clientX, int clientY, int clientW, int clientH);
// True when (x, y) is on Clippy himself (his bubble or the clapper), whatever the step. Such a press
// belongs to Panorama alone: the window procedure keeps it from the preview's click-to-steer, the
// free camera's click swallow and CS2 (a Next button over the preview used to grab the camera).
bool Onboarding_ClickOnAssistant(int clientX, int clientY, int clientW, int clientH);

// The tutorial runs on a paused demo: true while a locked step is up that gives no way to play
// it (neither Space nor J allowed) and no lesson roll (`playto`) is under way. The panel driver
// pauses a demo that plays anyway (OnboardingHud.cpp).
bool Onboarding_HoldsDemo();
// The lesson roll ended: the demo reached `playToTick` and was paused.
void Onboarding_PlayToDone();
// The panel driver sent the waiting lesson seek (`seekTick`) to DemoScrub.
void Onboarding_SeekIssued();

// `firstArg` is the index of the subcommand: 1 for mvm_onboarding, 2 for mirv_filmmaker onboarding.
void Onboarding_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd, int firstArg);

} // namespace Filmmaker
