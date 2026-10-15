#pragma once

// One-shot capture command for live-vs-demo Smooth View comparison.
//
// Standalone orchestrator: it starts nothing SmoothView or DemoAimResampler don't already know how
// to do (both already have their own StartTrace), it just arms them together, under one mvm_debug
// log, with a settings snapshot written first -- so a capture is never silently invalidated by a
// forgotten setting or a mismatched leg (tightness 12 live vs 50 demo cost an entire session once).
//
// "mirv_filmmaker smoothtest start [seconds]" / "smoothtest stop". Pair with
// Automation/verify/smoothview-compare.py to turn two captures (or a live one and a demo one) into
// the standard metrics: gun-travel-per-degree, residual RMS/peak, camera tracking ratio, low-motion
// hold decay.

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

void SmoothViewTest_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
