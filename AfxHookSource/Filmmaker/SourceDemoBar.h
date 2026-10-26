#pragma once

// The game side of the demo bar (Ui/DemoBar.h): fills its model from the engine adapter and the
// demo index, runs what the bar asks for as console commands, and keeps LOOP going.

#include "Ui/DemoBar.h"

#include <vector>

namespace SourceFilmmaker {

// Main thread. Fills model for this frame.
void DemoBar_BuildModel(Ui::DemoBarModel& model);

// Main thread. Runs the bar's actions. Returns true when one asked for the Camera Editor.
bool DemoBar_Apply(const std::vector<Ui::DemoBarAction>& actions);

// Main thread, once a frame: wraps LOOP playback at Out.
void DemoBar_Frame();

// mirv_filmmaker loop on|off|toggle|state, and the seek the bar uses, for the console surface.
// pause: -1 keeps the demo as it is, 0 lets it play on, 1 lands it paused (a camera path's start).
void DemoBar_LoopCommand(const char* action);
void DemoBar_SeekTo(int tick, int pause = -1);

// LOOP is on: a camera path reaching its end plays again (CS2).
bool DemoBar_LoopOn();

} // namespace SourceFilmmaker
