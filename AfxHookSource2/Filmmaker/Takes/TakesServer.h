#pragma once

// The hook's only door into server.dll. A private match runs the game server inside this process,
// so its entities and its command path can be reached directly; a demo or an online match has no
// local server here and nothing below does anything.
//
// Everything is located by signature on first use (TakesServer.cpp), and the two detours go in only
// when someone starts using takes, so a normal demo session never touches the server module.

#include <cstdint>
#include <string>
#include <vector>

namespace Filmmaker::Takes::Server {

struct Hooks {
	// A bot is building this tick's command. Fill `cmd` and return true to replace it entirely.
	bool (*botBuildCmd)(void *pawn, void *cmd) = nullptr;
	// The server just ran one of this player's commands; `cmd` is that command.
	void (*commandRan)(void *pawn, void *cmd) = nullptr;
	// A bot's AI is about to think. True skips it: a performing bot must not buy, aim or decide.
	bool (*skipBotThink)(void *pawn) = nullptr;
};

// Finds every function this needs. False if a game update moved one; ResolveError() names it.
bool Resolve();
const char *ResolveError();
// Puts the detours in. Needs a human with a live pawn, because the movement vtable is read from it.
bool InstallHooks(const Hooks &hooks);
bool HooksInstalled();

// -- World -----------------------------------------------------------------------------------------
bool Active();            // a local server has an entity system and a human on it
int TickCount();
int MaxClients();
void *ControllerBySlot(int slot);
void *HostController();   // the first human
bool ControllerIsBot(void *controller);
void *PawnOf(void *controller);       // the CCSPlayerPawn, alive or dead
void *ControllerOfPawn(void *pawn);
bool Alive(void *pawn);
int Team(void *entity);
int Health(void *entity);
int Armor(void *pawn);
bool HasHelmet(void *pawn);
void SetHealth(void *entity, int health);
// The team this player asked for and has not moved to yet, or 0. The game holds a switch until the next
// round, and once takes are set up there is no next round.
int PendingTeam(void *controller);
// Moves the player now, the way the script API's JoinTeam does. A living pawn is killed by the move.
bool JoinTeam(void *controller, int team);
// Sets what this player can spend. Takes do not restart the round, so mp_startmoney alone would leave a
// match that is past warmup on whatever it had.
void SetMoney(void *controller, int amount);
// Writes the SteamID a demo records for this player. Only for bots: see PerformerSteamId.
void SetSteamId(void *controller, uint64_t steamId);
const char *PlayerName(void *controller);
void SetPlayerName(void *controller, const char *name);
uint32_t HandleOf(void *entity);
void *EntityFromHandle(uint32_t handle);
void *EntityFromIndex(int index);
const char *ClassNameOf(void *entity);
// The live entity with this targetname, or nullptr. Walks the entity list: not for every frame.
void *FindByTargetName(const char *name);
// Warmup is running, or the game is about to restart the round or is holding everyone in freeze time.
// It will move players on its own shortly, so nothing should start until this clears.
bool MatchSettling();
// Skips the wait for what MatchSettling reports. The game acts by itself once the restart time or the
// round start time has passed, so this moves those times rather than any flag, and the game's own
// restart and freeze-end code and events still run.
void HurryMatch();
// One line for `takes status`.
std::string DescribeMatch();

// -- A pawn ----------------------------------------------------------------------------------------
bool Pose(void *pawn, float pos[3], float vel[3]);
bool EyeAngles(void *pawn, float &pitch, float &yaw);
bool EyePosition(void *pawn, float out[3]);
// Any of the three may be null to leave it alone. Clients do not interpolate across a teleport,
// so a correction made with it shows as a jump.
bool Teleport(void *pawn, const float *pos, const float *ang, const float *vel);
// Moves it with no teleport flag, so clients keep interpolating: invisible at per-tick distances.
bool SetAbsOrigin(void *entity, const float pos[3]);
void *ActiveWeapon(void *pawn);
void ListWeapons(void *pawn, std::vector<void *> &out);
void *FindWeapon(void *pawn, const char *className);
// The weapon, or nullptr. `deploy` switches to it as a player's pickup would.
void *Give(void *pawn, const char *className, bool deploy);
bool DestroyWeapons(void *pawn);
bool SetModel(void *entity, const char *model);
// Who the game says this weapon first belonged to, which is what a demo's skin lookup goes by.
void SetOriginalOwner(void *weapon, uint64_t steamId);
// The model this pawn is wearing right now, or "".
std::string ModelOf(void *pawn);
void SetArmor(void *pawn, int armor);
// Rounds in the magazine, or -1.
int ClipOf(void *weapon);
// What holds this entity: nullptr for a gun lying on the floor.
void *OwnerOf(void *entity);
// Takes it out of the world, the way the script API's Entity.Remove does.
void RemoveEntity(void *entity);
// Brings a dead player back through the game's own spawn path, with no round restart. They land on a
// spawn point, holding what the game hands out. True when they are alive afterwards.
bool Respawn(void *controller);

// How far each shot strays from the gun's recoil pattern (1 = as shipped, 0 = exactly on the pattern).
// Scales the weapon's m_flRecoilAngleVariance and m_flRecoilMagnitudeVariance only, for the server and
// for the client that predicts it; the recoil itself is untouched. False when the fields are unknown.
bool SetRecoilRandomness(void *weapon, float scale);

// -- A command -------------------------------------------------------------------------------------
bool SerializeCmd(void *cmd, std::vector<uint8_t> &out);
bool ParseCmd(void *cmd, const uint8_t *data, size_t size);
void SetCmdNumber(void *cmd, int number);

// One line per resolved piece, for `mirv_filmmaker takes status`.
std::string DescribeResolve();

} // namespace Filmmaker::Takes::Server
