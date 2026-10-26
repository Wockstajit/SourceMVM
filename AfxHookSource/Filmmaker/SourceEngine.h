#pragma once

// The engine adapter: what the Source 1 filmmaker reads from the running game. Later steps build on
// these instead of reaching into the engine themselves, the way the Deadlock port has
// DeadlockEngine and DeadlockEntities.
//
// Main thread only. Every engine call goes through a guarded virtual call, so a wrong slot or a
// stale entity costs a counted fault and a neutral value, never the game.

#include <string>
#include <vector>

namespace SourceFilmmaker {

// Resolves the client interfaces, the class list and the networked fields. Safe to call
// repeatedly; it retries until it succeeds.
bool Engine_Init();
bool Engine_Ready();

struct ViewPose {
	double origin[3] = { 0, 0, 0 };
	double angles[3] = { 0, 0, 0 }; // pitch, yaw, roll in degrees
	double fov = 0;
};

// The view the game itself set up this frame, before mirv or SOURCE:MVM overrides.
ViewPose View_Game();
// The view that was actually rendered, after overrides.
ViewPose View_Rendered();

struct DemoState {
	bool playing = false;
	bool paused = false;
	int tick = 0;
	int startTick = 0;
	int totalTicks = 0;
	double timescale = 1.0;
	double intervalPerTick = 0;
};

DemoState Demo_State();

// "de_dust2" from "maps/de_dust2.bsp"; empty when no map is loaded.
std::string Engine_MapName();

struct PlayerState {
	int index = -1;
	std::string name;
	std::string className;
	int userId = -1;
	bool bot = false;
	bool hltv = false;
	bool dormant = true;
	int team = 0;
	const char* teamName = "";
	int health = 0;
	bool alive = false;
	double origin[3] = { 0, 0, 0 };
	double eyeAngles[3] = { 0, 0, 0 };
};

// Every connected player slot that has an entity. Returns the count.
int Players_Snapshot(std::vector<PlayerState>& out);

// The local player's entity index (the demo's recorder during playback), or -1.
int Players_LocalIndex();

// Who the local player is spectating, or -1, and the observer mode (0 none, 4 in eye, 5 chase,
// 6 roaming in CS:S).
int Players_ObservedIndex();
int Players_ObserverMode();

// Diagnostics for health: client classes found, networked fields resolved of those asked for, and
// how many guarded calls faulted.
struct EngineDiagnostics {
	bool ready = false;
	int classCount = 0;
	int fieldsResolved = 0;
	int fieldsWanted = 0;
	unsigned long long faults = 0;
};

EngineDiagnostics Engine_Diagnostics();

// Prints every resolved field and its offset (mirv_filmmaker engine).
void Engine_PrintFields();

// Writes the same into the mvm_debug log, so a game update that moves a field is visible there.
void Engine_LogFields();

} // namespace SourceFilmmaker
