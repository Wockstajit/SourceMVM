#pragma once

namespace Deadlock {
void* Demo_Player();
bool Demo_IsPlaying();
bool Demo_IsPaused();
bool Demo_IsSeeking();
int Demo_Tick();
int Demo_StartTick();
double Demo_PlaybackScale();
double Demo_Time();
double Demo_IntervalPerTick();
void Demo_Report();
}
