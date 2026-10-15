#include "AimCrosshairHud.h"

#include "PanoramaFindPanel.h"
#include "../Movie/HudBlackout.h" // F8: clear every overlay
#include "AimCrosshairHudJs.h"
#include "ConfigHud.h"

#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel
#include "../Filmmaker.h"
#include "../MvmTest.h"
#include "../Movie/SmoothView.h" // reticle only owns the crosshair while Smooth View is on
#include "../Debug/MvmDebugLog.h"
#include "../../../Shared/AfxConsole.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg2Rad = kPi / 180.0;
// Render thread -> main thread. Independent display scalars; a frame of skew between them is
// invisible and nothing branches on their mutual consistency.
std::atomic<bool>   g_valid{ false };
std::atomic<bool>   g_front{ false };
std::atomic<double> g_screenX{ 0.5 };
std::atomic<double> g_screenY{ 0.5 };

// Valve's AngleVectors. Reproduced rather than pulled from the SDK so this module has no
// dependency on the camera code it is describing.
void AngleVectors(double pitch, double yaw, double roll,
	double f[3], double r[3], double u[3]) {
	const double sp = std::sin(pitch * kDeg2Rad), cp = std::cos(pitch * kDeg2Rad);
	const double sy = std::sin(yaw * kDeg2Rad), cy = std::cos(yaw * kDeg2Rad);
	const double sr = std::sin(roll * kDeg2Rad), cr = std::cos(roll * kDeg2Rad);
	f[0] = cp * cy;              f[1] = cp * sy;              f[2] = -sp;
	r[0] = -sr * sp * cy + cr * sy; r[1] = -sr * sp * sy - cr * cy; r[2] = -sr * cp;
	u[0] = cr * sp * cy + sr * sy;  u[1] = cr * sp * sy - sr * cy;  u[2] = cr * cp;
}

bool PlayingDemo() {
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			return pDemo->IsPlayingDemo() || pDemo->IsDemoPaused();
	}
	return false;
}

} // namespace

void AimCrosshair_Invalidate() {
	g_valid.store(false, std::memory_order_relaxed);
}

// The reticle only means anything while Smooth View is actually displacing the camera off the
// recorded aim. With Smooth View off, camera == aim, so this would draw a second reticle exactly
// on top of the native one AND hide the native one for no benefit -- which is how the crosshair
// went missing in a plain live game. So Smooth View being on is part of "supported view":
// take over the crosshair only when true-aim AND Smooth View are both on.
bool SupportedAimView() {
	return (PlayingDemo() || MvmTest_CanUseMenu()) && SmoothViewRef().Enabled();
}

void AimCrosshair_Publish(
	double truePitch, double trueYaw,
	double camPitch, double camYaw, double camRoll,
	double horizontalFovDegrees,
	int viewportWidth, int viewportHeight)
{
	double af[3], ar[3], au[3];
	AngleVectors(truePitch, trueYaw, 0.0, af, ar, au);

	double cf[3], cr[3], cu[3];
	AngleVectors(camPitch, camYaw, camRoll, cf, cr, cu);
	const double dF = af[0] * cf[0] + af[1] * cf[1] + af[2] * cf[2];
	const double dR = af[0] * cr[0] + af[1] * cr[1] + af[2] * cr[2];
	const double dU = af[0] * cu[0] + af[1] * cu[1] + af[2] * cu[2];
	const bool front = dF > 1e-4 && std::isfinite(dF);

	double fov = horizontalFovDegrees;
	if (!(fov > 1.0 && fov < 179.0)) fov = 90.0;
	const double tanH = std::tan(fov * 0.5 * kDeg2Rad);
	const double aspect = viewportWidth > 0 && viewportHeight > 0
		? (double)viewportWidth / (double)viewportHeight : (16.0 / 9.0);
	const double tanV = tanH / aspect;
	const double fracX = front ? 0.5 + 0.5 * (dR / dF) / tanH : 0.5;
	const double fracY = front ? 0.5 + 0.5 * (dU / dF) / tanV : 0.5;

	g_front.store(front, std::memory_order_relaxed);
	if (front) {
		g_screenX.store(fracX, std::memory_order_relaxed);
		g_screenY.store(fracY, std::memory_order_relaxed);
	}
	g_valid.store(true, std::memory_order_release);

	if (MvmDebugLog_Active()) {
		static unsigned s_logThrottle = 0;
		if (s_logThrottle++ % 30 == 0) {
			MvmDebugLog_Linef(
				"camera.xhair.project",
				"aim=(%.2f %.2f) camera=(%.2f %.2f %.2f) basis=(%.4f %.4f %.4f) screen01=(%.4f %.4f) front=%d",
				truePitch, trueYaw, camPitch, camYaw, camRoll, dF, dR, dU,
				fracX, fracY,
				front ? 1 : 0);
		}
	}
}

AimCrosshairHud& AimCrosshairHudRef() {
	static AimCrosshairHud s_instance;
	return s_instance;
}

void AimCrosshairHud::SetEnabled(bool v) { m_enabled = v; }
void AimCrosshairHud::SetSize(double v) { m_size = v < 0.0 ? 0.0 : (v > 60.0 ? 60.0 : v); }
void AimCrosshairHud::SetGap(double v) { m_gap = v < 0.0 ? 0.0 : (v > 60.0 ? 60.0 : v); }
void AimCrosshairHud::SetThickness(double v) { m_thickness = v < 1.0 ? 1.0 : (v > 10.0 ? 10.0 : v); }

void AimCrosshairHud::SetColor(int r, int g, int b, int a) {
	auto c = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
	m_r = c(r); m_g = c(g); m_b = c(b); m_a = c(a);
}

void* AimCrosshairHud::FindRoot() {
	void* ctx = m_bridge.ContextPanel();
	if (!ctx) return nullptr;
	return FindChildById(ctx, "AimXhairRoot");
}

bool AimCrosshairHud::BuildIfNeeded() {
	if (m_built) return true;
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel()) return false;
	if (!m_bridge.RunScript(kAimCrosshairHudJs)) return false;
	m_symState = m_bridge.MakeSymbol("state");
	m_root = FindRoot();
	m_built = (m_root != nullptr);
	m_lastJson.clear();
	return m_built;
}

void AimCrosshairHud::Teardown() {
	if (m_built && m_hudPanel && m_bridge.ContextPanel()) {
		m_bridge.RunScript(
			"(function(){var e=$('#AimXhairRoot'); if(e) e.DeleteAsync(0); $.AimXhair=null;})();");
	}
	m_built = false;
	m_root = nullptr;
	m_hudPanel = nullptr;
	m_lastJson.clear();
}

std::string AimCrosshairHud::BuildStateJson() {
	float x0 = 0.0f, y0 = 0.0f, x1 = 1.0f, y1 = 1.0f;
	bool hudScale = CameraEditor_ScaledHud(x0, y0, x1, y1);
	if (!hudScale)
		hudScale = ConfigHud_ScaledHud(x0, y0, x1, y1);

	std::ostringstream o;
	// F8 blackout hides the aim reticle with everything else. It is our overlay, not the game's,
	// so applyHudView() cannot reach it -- without this a green cross is left floating in the
	// middle of an otherwise clean frame.
	o << "{\"on\":" << ((m_enabled && !HudBlackout_Active()) ? "true" : "false");
	o << ",\"valid\":" << (g_valid.load(std::memory_order_relaxed) ? "true" : "false");
	o << ",\"front\":" << (g_front.load(std::memory_order_relaxed) ? "true" : "false");
	// Normalised SCREEN fractions in [0,1]; sy=1 is UP. See the range note on g_screenX.
	o << ",\"sx\":" << g_screenX.load(std::memory_order_relaxed);
	o << ",\"sy\":" << g_screenY.load(std::memory_order_relaxed);
	o << ",\"hudScale\":" << (hudScale ? "true" : "false");
	o << ",\"view\":[" << x0 << "," << y0 << "," << x1 << "," << y1 << "]";
	o << ",\"size\":" << m_size << ",\"gap\":" << m_gap << ",\"thick\":" << m_thickness;
	o << ",\"dot\":" << (m_dot ? "true" : "false");
	o << ",\"centre\":" << (m_showCentre ? "true" : "false");
	o << ",\"debugbg\":" << (m_debugBg ? "true" : "false");
	o << ",\"col\":\"rgba(" << m_r << "," << m_g << "," << m_b << "," << (m_a / 255.0) << ")\"";
	o << "}";
	return o.str();
}

void AimCrosshairHud::RunFrame() {
	m_bridge.Init();

	// Hide/restore CS2's own centre reticle. Two reticles on screen at once is the whole
	// problem this feature exists to solve -- the native one still sits at centre, which is
	// exactly where the shots are NOT going. Driven through the engine's console because
	// `crosshair` is a client convar with no exposed setter here.
	//
	// SELF-HEAL: `crosshair` is PERSISTED to the user's config, so a session that exited while
	// the reticle owned it (or an older build that hid it unconditionally) leaves the player
	// with no crosshair at all on the next launch. A purely edge-triggered restore cannot
	// recover from that: inactive->inactive is not an edge, so `crosshair 1` never runs and the
	// crosshair stays gone forever. Force the restore once per session on the first inactive
	// frame as well, then fall back to edge-only so the user's own `crosshair` setting is
	// respected from that point on.
	const bool active = m_enabled && SupportedAimView();
	if (active != m_wasEnabled || (!active && !m_restoredNativeOnce)) {
		if (!active) m_restoredNativeOnce = true;
		if (m_hideNative && g_pEngineToClient)
			g_pEngineToClient->ExecuteClientCmd(0, active ? "crosshair 0" : "crosshair 1", true);
		m_wasEnabled = active;
	}

	unsigned char* hud = active ? AfxHookSource2_GetPanoramaHudPanel() : nullptr;
	if (!hud) { Teardown(); return; }
	if (hud != m_hudPanel) { m_hudPanel = hud; m_built = false; }
	m_bridge.SetContextPanel(hud);

	if (!BuildIfNeeded()) return;

	m_root = FindRoot();
	if (!m_root) { m_built = false; return; }

	// Unlike the other HUDs this one MUST re-render every frame even when the JSON is unchanged:
	// the reticle position is the payload, and it changes continuously while the camera moves.
	// Only the attribute write is skipped when nothing changed.
	std::string json = BuildStateJson();
	if (json != m_lastJson) {
		m_bridge.SetAttributeString(m_root, m_symState, json.c_str());
		m_lastJson = json;
	}
	m_bridge.RunScriptCached("$.AimXhair && $.AimXhair.render();", "mvm/aimxhair.render");
}

void AimCrosshair_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	AimCrosshairHud& h = AimCrosshairHudRef();
	const char* a = (argc >= 3) ? args->ArgV(2) : "";

	if (0 == _stricmp(a, "on") || 0 == _stricmp(a, "off") || 0 == _stricmp(a, "toggle")) {
		const bool on = (0 == _stricmp(a, "toggle")) ? !h.Enabled() : (0 == _stricmp(a, "on"));
		h.SetEnabled(on);
		advancedfx::Message("mirv_filmmaker: true-aim crosshair %s.\n", on ? "ON" : "OFF");
		return;
	}
	if (0 == _stricmp(a, "state")) {
		advancedfx::Message("[xhair][state] %s\n", h.DebugStateJson().c_str());
		return;
	}
	if (0 == _stricmp(a, "size") && argc >= 4) { h.SetSize(std::atof(args->ArgV(3))); }
	else if (0 == _stricmp(a, "gap") && argc >= 4) { h.SetGap(std::atof(args->ArgV(3))); }
	else if (0 == _stricmp(a, "thick") && argc >= 4) { h.SetThickness(std::atof(args->ArgV(3))); }
	else if (0 == _stricmp(a, "dot") && argc >= 4) { h.SetDot(0 != std::atoi(args->ArgV(3))); }
	else if (0 == _stricmp(a, "centre") || 0 == _stricmp(a, "center")) {
		if (argc >= 4) h.SetShowCentre(0 != std::atoi(args->ArgV(3)));
		advancedfx::Message("mirv_filmmaker: centre reference marker = %s.\n", h.ShowCentre() ? "ON" : "OFF");
		return;
	}
	else if (0 == _stricmp(a, "debugbg")) {
		if (argc >= 4) h.SetDebugBg(0 != std::atoi(args->ArgV(3)));
		advancedfx::Message("mirv_filmmaker: crosshair debug background = %s.\n", h.DebugBg() ? "ON (screen should be red)" : "OFF");
		return;
	}
	else if (0 == _stricmp(a, "color") && argc >= 6) {
		h.SetColor(std::atoi(args->ArgV(3)), std::atoi(args->ArgV(4)), std::atoi(args->ArgV(5)),
			argc >= 7 ? std::atoi(args->ArgV(6)) : 255);
		advancedfx::Message("mirv_filmmaker: crosshair colour set.\n");
		return;
	}
	else {
		advancedfx::Message(
			"%s xhair on|off|toggle - draw the crosshair where the player is ACTUALLY aiming\n"
			"    instead of at screen centre. With free aim / shake / smoothing on, screen centre\n"
			"    is not where the bullets went; this reticle is.\n"
			"%s xhair size <0-60> | gap <0-60> | thick <1-10> | dot <0|1>\n"
			"%s xhair color <r> <g> <b> [a]\n"
			"%s xhair centre <0|1> - also mark screen centre, so the drift is visible\n"
			"%s xhair state\n",
			cmd, cmd, cmd, cmd, cmd);
		return;
	}
	advancedfx::Message("mirv_filmmaker: crosshair size=%.0f gap=%.0f thick=%.0f dot=%s.\n",
		h.Size(), h.Gap(), h.Thickness(), h.Dot() ? "on" : "off");
}

} // namespace Filmmaker
