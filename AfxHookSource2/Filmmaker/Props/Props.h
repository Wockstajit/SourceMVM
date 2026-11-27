#pragma once

#include <sstream>
#include <string>
#include <vector>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// PROPS: real CS2 models (trees, plants, props) placed into the scene as client-side entities,
// drawn, lit and shadowed by the engine itself -- they need no Environment pass. Placed props
// belong to the map and are saved per map; they respawn when that map loads and whenever a demo
// seek clears client entities. A placed EFFECT plays all the time, or only when a demo event
// happens or the playhead crosses a tick (`play`), and then for `playLife` seconds.
void Props_RunMainThreadFrame();
// CONFIG's Foliage wind (`props wind`), 0..200; 0 is off.
float Props_WindStrength();
void Props_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);
// `,"props":{...}` for the Config panel state: the catalog search page and the placed props.
void Props_AppendStateJson(std::ostringstream& out);
// Placing or dragging in the view: Esc cancels it (any thread).
bool Props_EditorBusy();
// Drops an unfinished placement and puts a dragged prop back. Main thread.
void Props_StopEditing();
// The client is shutting down cleanly: recent prop spawns did not crash it, so the crash-loop
// guard must not hold this map's props back on the next load.
void Props_OnShutdown();
// A practice session ended: read the current map's saved props again (and spawn them) on the next
// check. During the session props are neither read nor saved. Main thread.
void Props_ReloadAfterPractice();

// ---- Effects for Event FX (EventFx.cpp). Main thread. ----
// Loads a particle system (the precache rule: an effect spawned at an event must not hitch).
bool Props_PrecacheEffect(const char* system);
// Creates a particle system at a world point through the game's particle manager; -1 on failure.
int Props_SpawnEffect(const char* system, const float position[3]);
void Props_DestroyEffect(int particle);
// A demo event happened ("kill", "headshot", "flashbang", "hegrenade", "smoke", "molotov", "bomb",
// "marker"): placed effects set to play on it start again. Any thread; handled on the next frame.
void Props_FireTrigger(const char* token);
// Places `system` at the surface under the crosshair as a Scene Object that plays on `play` (a
// trigger token above) for `life` seconds, in outliner group `group`. Main thread; returns its id or 0.
int Props_AddTriggeredEffect(const char* system, const char* play, float life, const char* group, float lift);

// Placed models marked solid. Takes/Takes.cpp gives each an invisible collider on the local server,
// so players and bots stand on what the client draws. Main thread.
struct SolidProp {
	int id = 0;
	std::string model;
	float pos[3] = {0, 0, 0};
	float ang[3] = {0, 0, 0};
	float scale = 1;
};
void Props_CollectSolid(std::vector<SolidProp>& out);
bool Props_AnySolid();
bool Props_SolidNew();

} // namespace Filmmaker
