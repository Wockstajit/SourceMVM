#pragma once

// Automatic repair of demos broken by the CS2 2026-07-20 update.
//
// THE BUG (Valve's): build 14172 clients reject legacy entity message type 138
// (CEntityMessageRemoveAllDecals). Any demo recorded before the update that contains one
// desyncs the netmessage reader the moment playback reaches it:
//
//   Netchan 'DEMO' closed unexpectedly: Unknown message type 138
//   Disconnect reason: NETWORK_DISCONNECT_MESSAGE_PARSE_ERROR
//
// Reproduces on a clean client with no mods. The only fix is to strip those messages from the
// .dem, which FilmmakerDemoRepair.exe does (its own standalone Go tool -- see
// FilmmakerDemoRepairGo/). This module drives it automatically so the user never has to:
//
//   * Watch() asks for a playable path. If the demo is clean, its own path comes straight back.
//   * If it needs repair, a repaired copy is produced ONCE into the demo's source_mvm project
//     folder as repaired.dem and cached; every later play of that demo reuses the copy.
//   * A clean verdict is ALSO cached: repaired.dem.clean records the original's
//     size+mtime, so a demo scanned once is never re-scanned in later sessions (the scan is a
//     full parse -- seconds of wait that used to repeat every launch and read as "repairing").
//   * The original .dem is never modified.
//
// Repair is ~9 s for a 300 MB demo, so it runs on a worker thread and playback starts when it
// finishes rather than freezing the game.

#include <string>

namespace Filmmaker {

enum class RepairState {
	Idle,        // nothing in flight
	Checking,    // scanning the demo for type-138 messages
	Repairing,   // writing the repaired copy
	Ready,       // a repaired copy is available (or the demo never needed one)
	Failed,      // the helper could not run / the demo could not be repaired
	// The demo's own network protocol is one this client rejects outright. Repair CANNOT fix
	// this: it strips messages, it does not rewrite the protocol -- verified on disk, where a
	// repaired copy carries the same network_protocol as its original. Reported honestly
	// instead of running a pointless multi-second repair and handing CS2 a file it refuses.
	Unsupported  // recorded by a build this client will not accept
};

// Cheap per-demo classification from CDemoFileHeader. LegacyCheck means this client accepts the
// protocol, but the file predates the decal-message fix and needs the one-time helper scan before
// we can say whether an actual rewrite is required.
enum class DemoCompatibility {
	Unknown,
	Supported,
	LegacyCheck,
	Unsupported
};

enum class DemoRepairCacheState {
	None,
	Clean,
	Repaired
};

DemoCompatibility DemoRepair_ClassifyProtocol(int networkProtocol);
DemoRepairCacheState DemoRepair_CacheState(const std::wstring& demoPath);

// Where a repaired copy of `demoPath` lives (whether or not it exists yet):
//   %APPDATA%\HLAE\source_mvm\demos\cs2\<demo>\repaired.dem
std::wstring DemoRepair_RepairedPathFor(const std::wstring& demoPath);

// Non-blocking. Returns the path that should actually be played:
//   * the repaired copy when one already exists,
//   * the original when it is known-clean,
//   * an EMPTY string while a check/repair is still running -- call again next frame.
// Kicks off the background check/repair on first use for a given demo.
std::wstring DemoRepair_ResolvePlayable(const std::wstring& demoPath);

// State + last message for the demo most recently passed to ResolvePlayable.
RepairState DemoRepair_State();
std::string DemoRepair_StatusText();
std::wstring DemoRepair_ActiveDemo();

// True once the helper executable has been located (FilmmakerDemoRepair.exe next to the DLL,
// or MIRV_FILMMAKER_DEMOREPAIR).
bool DemoRepair_HelperAvailable();

// Drops any in-flight work (demo change / shutdown).
void DemoRepair_Reset();


} // namespace Filmmaker
