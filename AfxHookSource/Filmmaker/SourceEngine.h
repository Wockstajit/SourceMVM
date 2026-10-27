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

namespace Sdk2013 {
struct ClientClass;
}

// Resolves the client interfaces, the class list and the networked fields. Safe to call
// repeatedly; it retries until it succeeds.
bool Engine_Init();
bool Engine_Ready();

// The client's class list (IBaseClientDLL::GetAllClasses), or null until Engine_Init succeeds.
const Sdk2013::ClientClass* Engine_ClientClasses();

struct ViewPose {
	double origin[3] = { 0, 0, 0 };
	double angles[3] = { 0, 0, 0 }; // pitch, yaw, roll in degrees
	double fov = 0;
};

// The view the game itself set up this frame, before mirv or SOURCE:MVM overrides.
ViewPose View_Game();
// The view that was actually rendered, after overrides.
ViewPose View_Rendered();

// The fields of view in the engine's own world-to-screen matrix, of the last view it rendered: the
// FOV above as the engine widened it for the screen. False before a view is rendered.
bool Engine_ViewMatrixFov(double& horizontal, double& vertical);

// The horizontal FOV the engine renders fov at on a width x height screen. CS:S widens its 4:3 FOV
// for the screen as Alien Swarm does: measured 2026-10-26 from the engine's own matrix
// (Engine_ViewMatrixFov), FOV 90 renders at 106.30 at 854x480. HLAE's table (GetDefaultFovScaling)
// has no scaling for the 64-bit game, so it is not asked.
double View_ScreenFov(double fov, double width, double height);

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

// Any networked entity: what the follow camera's targets (players, weapons, the C4, grenades,
// ragdolls) are read from. Handles are the engine's (index in the low 12 bits, serial above), so a
// target picked by handle is not mistaken for whatever reuses its index later. Links to other
// entities are entity indices, -1 for none; a field the game does not network for that entity's
// class stays -1 or 0.
struct EntityState {
	int index = -1;
	unsigned int handle = 0xFFFFFFFF;
	std::string className;      // the network class: CCSPlayer, CAK47, CC4, CHEGrenadeProjectile...
	bool dormant = true;
	double origin[3] = { 0, 0, 0 };
	double angles[3] = { 0, 0, 0 };
	int team = 0;
	int ownerIndex = -1;        // m_hOwnerEntity: a held weapon's player, a projectile's thrower
	int weaponOwnerIndex = -1;  // m_hOwner of a weapon
	int throwerIndex = -1;      // m_hThrower of a grenade projectile
	// Players.
	int health = 0;
	bool alive = false;
	int flags = 0;              // m_fFlags: ducking, on the ground
	double eye[3] = { 0, 0, 0 }; // the eyes: the origin raised by the profile's standing or ducking height
	double eyeAngles[3] = { 0, 0, 0 }; // where it looks: the profile's eye angles, else its angles
	int activeWeaponIndex = -1; // m_hActiveWeapon
	int ragdollIndex = -1;      // m_hRagdoll, once dead
	// Ragdolls: the player they were.
	int ragdollPlayerIndex = -1;
};

// Every entity from 1 to the client's highest index; dormant ones only when asked. Returns the count.
int Entities_Snapshot(std::vector<EntityState>& out, bool includeDormant = false);
// One entity; false when there is none at that index.
bool Entity_Get(int index, EntityState& out);
// An entity's model as the game draws it this frame, for the follow camera's Attached mounts. A bone
// by name ("ValveBiped.Bip01_Head1"): where it is, and its axes as the bone matrix's columns (the
// bone's x, y and z in the world). An attachment by name ("muzzle_flash"): where it is and its
// angles. False when the entity has no model, the model has no such bone or attachment, or a read
// faults.
bool Entity_Bone(int index, const char* bone, double pos[3], double axes[3][3]);
bool Entity_Attachment(int index, const char* attachment, double pos[3], double angles[3]);

// mirv_filmmaker entities [all] [class filter]: one line per entity.
void Entities_Print(bool includeDormant, const char* classFilter);

// mirv_filmmaker entities watch <class filter>|off: each frame, logs (mvm_debug "entities.watch") an
// entity of a matching class when it appears and when it goes, with its tick, origin and links, so a
// grenade in flight or a body that lasts a second is caught without stopping the demo.
void Entities_Watch(const char* classFilter);
void Entities_WatchFrame(); // main thread, once a frame

// The local player's entity index (the demo's recorder during playback), or -1.
int Players_LocalIndex();

// Who the view follows, or -1, and the observer mode (0 none, 3 fixed, 4 in eye, 5 chase, 7 roaming;
// Ui/DirectorMath.h). In SourceTV playback that is SourceTV's own camera, which the client keeps
// apart from the networked observer fields; otherwise the local player's.
int Players_ObservedIndex();
int Players_ObserverMode();

// A SourceTV demo is playing: the view is SourceTV's camera.
bool Engine_SourceTvPlayback();

// "SourceTV camera at client.dll+0x5b4730: mode 4, target 2", for health.
std::string Engine_SpectatorCameraText();

// The game's own menu or console is up: the director stands aside and leaves the game the mouse.
bool Engine_GameUiVisible();

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
// Prints the game's client class names containing filter (mirv_filmmaker engine classes [filter]).
void Engine_PrintClasses(const char* filter);

// Writes the same into the mvm_debug log, so a game update that moves a field is visible there.
void Engine_LogFields();

} // namespace SourceFilmmaker
