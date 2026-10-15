#pragma once

// AimCrosshairHud: draws a crosshair at where the spectated player is ACTUALLY aiming,
// instead of at screen centre.
//
// WHY THIS EXISTS. Every camera effect we apply -- Smooth View's free-aim cone and smoothing
// lag, the handheld/recoil shake, breathing -- moves the RENDERED camera away from the angles
// the demo recorded. The bullets, however, were fired along the recorded angles and nothing can
// change that. So the moment any of those effects is on, screen centre stops being where rounds
// land, and a centre-locked crosshair is simply lying.
//
// The fix follows the same geometry as the GMod mod (cl_crosshair.lua): project the true aim
// direction into the rendered camera rather than drawing at centre. The crosshair then floats
// around the screen as the camera drifts and keeps marking the real impact direction. This is
// the honest way to have free aim in a demo --
// see [[smoothview-port]] for why re-aiming the bullets, which is the mod's other half, is
// impossible here.
//
// Standalone by the repo's coupling rule: its own module, its own JS, its own console command
// ("mirv_filmmaker xhair"), its own enable flag. It reads one published struct and nothing else;
// no camera module knows it exists.
//
//   C++ -> JS : attribute "state" (JSON), then $.AimXhair.render().
//   JS  -> C++: none. Display only.

#include "PanoramaBridge.h"

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Projects the true recorded aim into the final rendered camera. This deliberately uses the
// two directions directly instead of depending on an engine world-to-screen matrix whose
// storage convention and lifetime vary between view passes.
void AimCrosshair_Publish(
	double truePitch, double trueYaw,
	double camPitch, double camYaw, double camRoll,
	double horizontalFovDegrees,
	int viewportWidth, int viewportHeight);

// Marks the aim geometry stale (no demo / not spectating a player). The HUD hides itself rather
// than drawing a reticle frozen wherever the last valid frame left it.
void AimCrosshair_Invalidate();

class AimCrosshairHud {
public:
	// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame.
	void RunFrame();

	void SetEnabled(bool v);
	void Toggle() { SetEnabled(!m_enabled); }
	bool Enabled() const { return m_enabled; }

	// Hide CS2's own centre crosshair while ours is up, so there are not two reticles. Off means
	// both are drawn, which is genuinely useful for verifying the offset is doing something.
	void SetHideNative(bool v) { m_hideNative = v; }
	bool HideNative() const { return m_hideNative; }

	void SetSize(double v);   // arm length, px
	double Size() const { return m_size; }
	void SetGap(double v);    // centre gap, px
	double Gap() const { return m_gap; }
	void SetThickness(double v);
	double Thickness() const { return m_thickness; }
	void SetDot(bool v) { m_dot = v; }
	bool Dot() const { return m_dot; }
	void SetColor(int r, int g, int b, int a);
	// Draws a faint marker at screen centre too, so the drift between "where the camera points"
	// and "where the shots go" is directly visible while tuning.
	void SetShowCentre(bool v) { m_showCentre = v; }
	bool ShowCentre() const { return m_showCentre; }
	// Paints the whole overlay translucent red. Answers the one question a reticle that is
	// "positioned correctly but invisible" cannot: is the container reaching the screen at all?
	void SetDebugBg(bool v) { m_debugBg = v; }
	bool DebugBg() const { return m_debugBg; }

	std::string DebugStateJson() { return BuildStateJson(); }

private:
	void* FindRoot();
	bool BuildIfNeeded();
	void Teardown();
	std::string BuildStateJson();

	PanoramaBridge m_bridge;
	void* m_hudPanel = nullptr;
	void* m_root = nullptr;
	short m_symState = -1;
	bool m_built = false;
	bool m_enabled = false;
	bool m_wasEnabled = false; // actually active in a supported game/demo view; owns native reticle
	// One-shot guard: `crosshair` is persisted, so the native reticle is force-restored once per
	// session rather than only on an active->inactive edge. See AimCrosshairHud::RunFrame.
	bool m_restoredNativeOnce = false;
	bool m_hideNative = true;
	bool m_dot = true;
	bool m_showCentre = false;
	bool m_debugBg = false;
	double m_size = 7.0;
	double m_gap = 4.0;
	double m_thickness = 2.0;
	int m_r = 0, m_g = 255, m_b = 120, m_a = 255;
	std::string m_lastJson;
};

AimCrosshairHud& AimCrosshairHudRef();
void AimCrosshair_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
