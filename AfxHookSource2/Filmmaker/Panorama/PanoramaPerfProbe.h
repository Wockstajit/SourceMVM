#pragma once

// MAIN-THREAD UI AND BACKEND COST PROBE.
//
// "The FPS halves while the editor UI is open" is a claim about main-thread time, and a netcon
// reply cannot show it: the cost is spread over every frame and lives in whichever of the eight
// Panorama HUD pumps or backend systems is doing the most work. This measures them.
//
//   mvm_debug start                -> the probe arms with the log
//   mvm_debug perf on|off         -> arm it without the full debug log
//
// Every slot is a named accumulator. PanoramaPerf_Scope adds its own lifetime to one; once per
// kWindow main-thread frames PanoramaPerf_EndFrame() emits ONE line with the frame interval and
// every slot that cost anything, then clears. Nothing is measured while the probe is disarmed --
// Active() is a relaxed bool load and the scopes early-out on it, so leaving the calls in costs
// a predictable-branch per pump.
//
// Frame interval is measured HERE, on the main thread, between successive EndFrame calls. That is
// the number the user sees as FPS (CS2's main thread gates presentation), so a regression that
// shows up in the interval but in no slot means the cost is on the render thread or inside
// Panorama's own layout/paint -- which is itself a useful answer.

#include <cstdint>

namespace Filmmaker {

// Arm/disarm. `mvm_debug start` arms automatically; this is the explicit switch.
void PanoramaPerf_SetEnabled(bool enable);
bool PanoramaPerf_Enabled();

// True while measurements should be taken (armed AND a sink exists).
bool PanoramaPerf_Active();

// Adds `ns` to the named slot. `name` must be a string literal (stored by pointer).
void PanoramaPerf_Add(const char* name, long long ns);

// Call once per main-thread frame, at the very end of the pump.
void PanoramaPerf_EndFrame();

// Formats the most recent window's report for a console reply (empty until one window elapses).
const char* PanoramaPerf_LastReport();

// RAII timer. Declare one at the top of a pump:  PanoramaPerf_Scope _p("camtl");
class PanoramaPerf_Scope {
public:
	explicit PanoramaPerf_Scope(const char* name);
	~PanoramaPerf_Scope();
private:
	const char* m_name;
	long long m_start;
};

} // namespace Filmmaker
