#pragma once

// What the end-of-demo hold (DemoEndHold.cpp) asks the game it runs in. The hold's rules (where it
// stops, the release slack, pausing again when play runs on past it) are the same in every game;
// how a game answers these is not. CS2 answers in DemoEndHoldHostSource2.cpp, CS:S in
// AfxHookSource/Filmmaker/SourceDemoEndHold.cpp.

#include <string>

namespace Filmmaker {

// A demo is open: playing, or paused (a paused demo reads not playing). False when the engine
// cannot be asked, which the hold treats as no demo.
bool EndHoldHost_DemoState(bool& playing, bool& paused);
// The playing demo's file, to notice a new demo. Empty when unknown.
std::wstring EndHoldHost_DemoPath();
// How many ticks the demo plays (its header's playback ticks), -1 when unknown. Asked once a demo.
int EndHoldHost_PlaybackTicks(const std::wstring& path);
// The demo's current tick, and the tick its playback starts at.
bool EndHoldHost_Tick(int& tick);
int EndHoldHost_StartTick();
// The match's game phase (CS2's CCSGameRules::m_gamePhase; 5 is postgame), -1 when the game has
// none or it cannot be read: the last-tick hold covers that.
int EndHoldHost_GamePhase();
// A timeline scrub is seeking: its own seeks are left alone.
bool EndHoldHost_ScrubActive();
// Pauses the demo.
void EndHoldHost_Pause();

} // namespace Filmmaker
