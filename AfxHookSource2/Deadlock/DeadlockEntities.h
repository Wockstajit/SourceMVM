#pragma once

// Reading Deadlock's client entity list.
//
// WHY THIS EXISTS. Until now the port could see the demo clock, the view struct and the Panorama
// tree, and nothing at all about what was IN the replay. That was the one missing piece under the
// follow camera: `FollowCamera_Solve` returned false unconditionally, the hero dropdown was empty,
// and the whole FOLLOW / ATTACH column was a shape with no game behind it.
//
// WHAT IT READS AND HOW. Two halves, and they fail independently:
//
//   * the LIST itself -- the entity array global and the two accessors -- is byte-pattern work in
//     client.dll. Both CS2 patterns hit Deadlock unchanged, because this is shared Source 2 code
//     rather than game logic; they are re-recorded in DeadlockAddresses all the same, because a
//     pattern that happens to transfer today is still version-coupled.
//   * every FIELD (origin, angles, health, the name) is a schema lookup BY NAME through
//     DeadlockSchema. Never an offset literal. A Deadlock update moves these constantly and a
//     hardcoded one is a wrong write into a live object, which does not fault where it happens.
//
// A missing field disables the one thing that reads it and says so in `mirv_filmmaker entities`,
// rather than defaulting to offset zero -- which is the object header, and takes the game down
// somewhere unrelated several frames later.
//
// THREADING. Every read here is a plain memory read of client state and is only ever called from
// the main thread, with the rest of the per-frame filmmaker work.

#include <windows.h>
#include <string>
#include <vector>

namespace Deadlock {

// Resolves the entity-list addresses. Called once, from New_ClientInit (never from the module
// dispatcher, which runs under the loader lock).
bool Entities_InitClient(HMODULE clientDll);

// True when the list resolved AND the schema gave us at least an origin to read.
bool Entities_Ready();

// Highest index worth asking for. The engine's own iterator counts live entities rather than
// reporting a high-water index, so this is the table's capacity -- iterate and skip the nulls.
int Entities_HighestIndex();

// The entity at `index`, or nullptr. Opaque on purpose: nothing outside this module should be
// dereferencing a client object.
void* Entities_Get(int index);

// The ENTITY class name -- `player`, `citadel_player_controller`, `prop_dynamic` -- or an empty
// string. Lowercase, from the entity's own identity block: this is the name the map and the
// game code use, NOT the C++ client class (`C_CitadelPlayerPawn`) the schema declares. Pure
// pointer-chase; no pattern and no schema field involved.
std::string Entities_ClassName(void* entity);

// True when the entity's class name equals `className`, without allocating. The hero scan uses this.
bool Entities_ClassIs(void* entity, const char* className);

// World-space position of the entity's scene node. False when the node or the field is missing,
// in which case `out` is untouched.
bool Entities_Origin(void* entity, double out[3]);

// Scene-node rotation as pitch/yaw/roll degrees. Separate from the origin because a hero can have
// a readable position and an unreadable rotation when only one of the two fields is renamed.
bool Entities_Angles(void* entity, double out[3]);

// Eye position: the scene-node origin plus the pawn's own view offset. This is what a lock-on
// camera should frame -- the hero's head, not the point between their feet.
bool Entities_EyeOrigin(void* entity, double out[3]);

// Current health, or -1 when unknown. The follow camera's release-on-death rule reads this; a -1
// means "cannot tell", and the rule then never fires rather than firing on every frame.
int Entities_Health(void* entity);

// --- heroes ------------------------------------------------------------------------------------

struct HeroEntry {
	int index = -1;
	std::string className;
	// The label the follow-camera picker shows: the player's real name, the hero's real name, or
	// both -- see ComposeHeroListName in DeadlockEntityMath.h for exactly which. `playerName` and
	// `codename` are the two real values it was built from, kept alongside for anything that wants
	// just one of them (a future column, a log line) rather than parsing `name` back apart.
	std::string name;
	std::string playerName;  // from the controller's m_iszPlayerName, e.g. "aitch" -- empty if unresolved
	std::string codename;    // the pawn's own identity codename, e.g. "hero_atlas" -- empty if unresolved
	std::string team;      // "Amber" / "Sapphire", or empty when the team field did not resolve
	int health = -1;
	bool alive = false;
	double origin[3] = { 0, 0, 0 };
};

// Every live hero pawn, in entity-index order. The follow camera's dropdown and its "nearest"
// button are both built from this, so they can never disagree about who exists.
void Entities_Heroes(std::vector<HeroEntry>& out);

// The SAME roster, throttled to 10 Hz -- one rescan shared by Entities_AppendHeroesJson and the
// overlay drawer's per-frame hero boxes, so calling both once a frame never walks the entity table
// twice for the same 100 ms window. Reads back the last rescan's data immediately after a call that
// did rescan; do not call this from a hot per-entity loop expecting per-call freshness -- for that,
// call Entities_Heroes directly, the way Entities_NearestHero does for its one-shot click.
void Entities_HeroesCached(std::vector<HeroEntry>& out);

// The hero pawn nearest to a world point, or -1. Used by the column's NEAREST button.
int Entities_NearestHero(const double from[3]);

// The `heroes` array for the state feed.
void Entities_AppendHeroesJson(std::string& out);

// How many heroes the LAST successful scan found -- the count as of whichever of
// Entities_Heroes/Entities_AppendHeroesJson last actually ran, not a fresh scan. Cheap on
// purpose: the follow panel's `targetsAvailable` flag reads this every frame and must not be
// the thing that brings back the per-frame entity walk P4a removed.
int Entities_HeroCount();

// The pawn's own hero codename (e.g. "hero_atlas"), read straight from its identity block --
// see the definition for why this needed no schema field at all. Empty when unresolved.
std::string Entities_HeroCodename(void* pawn);

// The REAL player name for a hero pawn: resolves the pawn's own controller via its
// m_hController handle, then reads the controller's m_iszPlayerName. Empty when either step
// fails -- a pawn with no live controller (a bot, or a build where the field moved) is not an
// error, just a hero with no name to show.
std::string Entities_PlayerNameFor(void* pawn);

// `mirv_filmmaker listentities [n]`: the player-only roster (mirv_deadlock is the same command).
// Unlike `entities list <class> <n>`, which is a raw class-filtered dump of the whole table,
// this prints the SAME resolved rows the follow-camera dropdown builds from -- codename, player
// name, team, life -- so a name that looks wrong in the UI can be checked against the console
// without guessing which of the two paths disagrees.
void Entities_ListHeroes(int maxRows);

// Prints the live entity table: index, class name, and whether an origin could be read. The first
// thing to run when the hero list comes back empty -- it separates "the list is empty", "the class
// name does not read" and "the hero class is called something else", which are three different
// faults that all present as zero heroes. `filter` keeps only class names containing it.
void Entities_List(const char* filter, int maxRows);

void Entities_Report();

// `mirv_filmmaker entities probe <index>`: the entity's and its identity block's raw qwords (with any
// readable strings), the resolved player fields, and both handle interpretations. Evidence for the
// controller/pawn link, the team numbering and the player name before anything relies on them.
void Entities_Probe(int index);

// Guarded reads that faulted since the hook loaded (probe reads excluded). Stays 0 in normal use.
unsigned long long Entities_FaultCount();

} // namespace Deadlock
