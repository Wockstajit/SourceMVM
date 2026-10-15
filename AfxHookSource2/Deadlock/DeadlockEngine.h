#pragma once

// The one place AfxHookDeadlock touches the engine.
//
// The CS2 hook reaches demo state through a hand-written, vtable-index-only stub header
// (Deps/release/prop/cs2/sdk_src/public/cdll_int.h) with `_Unknown_NNN` padding, and then repeats
// the same three-line `GetDemoFile()->IsDemoPaused()` helper inline in roughly fifty-seven files.
// That works, but it bakes every slot index into a header at compile time, which is the wrong
// shape for a port: the slots are exactly the thing that is not yet known.
//
// Slot indices are data, but only measured or offline-verified slots are present. The verifier
// checks each demo-player entry against its accessor machine code before the DLL is launched.
//
// Every call is guarded: a wrong slot returns the type's neutral value instead of crashing, and
// the guard trips a flag that health reports.

#include <cstdint>
#include <string>

namespace Deadlock {

// ---------------------------------------------------------------------------
// Bring-up. Called from the client interface's Connect, with the engine's app-system factory.
// ---------------------------------------------------------------------------

// Resolves Source2EngineToClient001 and the cvar interface. Deadlock's CreateInterface version
// strings are byte-identical to CS2's, so this is the one part of the engine layer that needed no
// re-derivation at all.
bool Engine_Connect(void* appSystemFactory);

bool Engine_HasEngineToClient();

// ---------------------------------------------------------------------------
// Demo state. This is the whole surface the camera, timeline, graph and render code needs.
// ---------------------------------------------------------------------------

// True while a demo is loaded and playing back.
//
// Read the debounce note before gating UI on this: on CS2 both IsPlayingDemo and IsDemoPaused
// blip false partway through a seek, so anything that shows or hides a panel needs roughly thirty
// frames of hysteresis rather than trusting a single frame. Assume Deadlock does the same until
// measured otherwise.
bool Engine_IsPlayingDemo();

bool Engine_IsDemoPaused();

// The demo's current tick, or -1 when no demo is loaded.
int Engine_DemoTick();

// Seconds per tick, read from Deadlock's live global-vars interval field. The supplied replay was
// measured at 0.015625 seconds (64 ticks/s); the fallback remains configurable for diagnostics.
double Engine_IntervalPerTick();
void Engine_SetIntervalPerTick(double seconds);
bool Engine_TickIntervalConfirmed();

// Demo time in seconds, from the tick and the interval above.
double Engine_DemoTime();

// The .dem path currently playing, or an empty string.
std::string Engine_DemoFilePath();

// The loaded level name, or an empty string. Non-empty is the third condition the CS2 demo
// launcher waits on, because IsPlayingDemo can be true while entities are still invalid.
std::string Engine_LevelName();

// ---------------------------------------------------------------------------
// Control. Everything the tool does to playback goes through console commands, not API calls,
// exactly as on the CS2 side: playdemo, demo_pause, demo_resume, demo_gototick, demo_timescale.
// ---------------------------------------------------------------------------

bool Engine_ExecuteClientCmd(const char* command);

// ---------------------------------------------------------------------------
// Slot table. Names are stable; values are per-build.
// ---------------------------------------------------------------------------

enum class EngineSlot {
	// on ISource2EngineToClient
	ExecuteClientCmd,
	GetLevelName,
	GetDemoFilePath,
	// on IDemoFile
	DemoGetTick,
	DemoIsPlaying,
	DemoIsPaused,
	DemoIsSeeking,
	DemoGetStartTick,
	DemoGetPlaybackScale,
	Count,
};

int Engine_GetSlot(EngineSlot slot);
void Engine_SetSlot(EngineSlot slot, int index);
const char* Engine_SlotName(EngineSlot slot);
bool Engine_SlotFromName(const char* name, EngineSlot* out);

// True when any guarded engine call has faulted since load. A single trip means a slot index is
// wrong; health surfaces it because the symptom otherwise is "the timeline is just empty".
bool Engine_HadFault();
void Engine_ClearFault();

} // namespace Deadlock
