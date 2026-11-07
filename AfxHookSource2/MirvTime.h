#pragma once

/*struct DemoPausedData {
	bool IsPaused = false;
	float FirstPausedCurtime = 0.0f;
	float FirstPausedInterpolationAmount = 0.0f;
};

extern DemoPausedData g_DemoPausedData;*/

class CMirvTime
{
public:
	float curtime_get(void);
	void curtime_set(float value);
	int framecount_get(void);
	float frametime_get(void);
	// Client-globals frametime write. Pairs with curtime_set: a client-side animgraph advances by
	// frametime, so a paused demo (frametime 0) freezes it. See Filmmaker/Panorama/PreviewAnimClock.h.
	void frametime_set(float value);
	float absoluteframetime_get(void);
	float interval_per_tick_get(void);
	float interpolation_amount_get(void);

	bool GetCurrentDemoTick(int& outTick);
	// Whole ticks only: interpolation_amount reads 0 in CS2, so this holds still between ticks and
	// steps at a low demo_timescale. Anything drawn per frame wants GetFractionalDemoTime.
	bool GetCurrentDemoTime(double& outDemoTime);
	// The demo tick WITH its interpolated fraction, from the client clock
	// (Filmmaker/Movie/DemoClockMath.h). Smooth at any demo_timescale, holds a paused fraction, and
	// never steps backward while playing. False outside demo playback.
	bool GetFractionalDemoTick(double& outTick);
	// GetFractionalDemoTick in seconds.
	bool GetFractionalDemoTime(double& outDemoTime);
};

extern CMirvTime g_MirvTime;
