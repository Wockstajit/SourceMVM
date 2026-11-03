#pragma once

// Converts an old Counter-Strike: Source demo (the "v34" engine: demo protocol 3, network protocol
// 7) into the format the current 64-bit game plays (network protocol 24). Engine-free.
//
// The demo's own network tables are kept and written in the new layout, so the new client decodes
// every property value exactly as the old one did and the values are copied bit for bit. What is
// rewritten is the framing that changed between the engines: message type width, the messages
// whose fields changed (tick, server info, string tables, sounds, decals, temp entities, prefetch),
// the entity and property index encoding, string table entry lengths, and the user message
// numbering, and the user commands' layout (the recorder's own commands, which playback of a
// first-person demo predicts the player's gun from). Voice is dropped; the three string tables the
// new client expects and the old server never made are added empty. Docs/source/README.md has the
// full list.
//
// Given the installed map's brush models, the demo's "*N" models are matched to them (MapModels.h):
// renumbered where Valve moved them, and entities whose brush the map no longer has are left out.

#include "MapModels.h"

#include <cstddef>
#include <string>
#include <vector>

namespace SourceFilmmaker {
namespace DemoConvert {

// Part of the stamp next to every converted copy: bump it when the output changes, and copies made
// by an older converter are made again.
constexpr int kV34ConverterVersion = 2;

struct ConvertStats {
	int framesIn = 0;
	int framesOut = 0;
	int userCmdsConverted = 0;
	int userCmdsDropped = 0; // commands that are not exactly one command in the old layout
	long long messages = 0;
	long long packetEntities = 0;
	long long entityUpdates = 0;
	long long props = 0;
	long long tempEntities = 0;
	long long sounds = 0;
	long long userMessages = 0;
	long long userMessagesDropped = 0;
	long long voiceDropped = 0;
	long long baselines = 0;
	int classesIn = 0;
	int classesOut = 0;
	int intNormalCleared = 0;
	std::string droppedClasses;
	std::string stoppedEarly; // why conversion stopped before the demo's end, when it did
	bool mapMatched = false;  // brush models were matched to the installed map
	int brushKept = 0, brushMoved = 0, brushMissing = 0, brushUnused = 0;
	long long entitiesDropped = 0; // entity updates left out because their brush is gone
	std::string brushDetail;       // which brushes moved or are missing
};

struct ConvertOptions {
	// The installed map's brush models. Without them "*N" models are left as recorded, which the
	// game refuses ("bad inline model number") wherever Valve removed one.
	const MapModels* map = nullptr;
};

struct ConvertResult {
	bool ok = false;
	std::string error;
	ConvertStats stats;
};

// True when data starts with an old-engine demo header (demo protocol 3, network protocol 7).
bool IsV34Demo(const unsigned char* data, size_t size);

// The map named in a demo header ("de_dust2"), empty when data is not a demo.
std::string DemoMapName(const unsigned char* data, size_t size);

// Writes the converted demo to out. A demo that ends in an unreadable frame (a crash or a cut
// recording) is converted up to that frame and closed normally; stats.stoppedEarly says so.
ConvertResult ConvertV34Demo(const unsigned char* data, size_t size, std::vector<unsigned char>& out,
	const ConvertOptions& options = ConvertOptions());

// One user command (a usercmd frame's payload, a delta against an empty command) rewritten in the
// current layout. The old engine writes pitch and yaw as 16-bit angles, roll as an 8-bit angle and the
// three moves as 16-bit integers where the current one writes floats; every other field is the same
// (measured: all 629,752 commands of 59 v34 demos decode to exactly their recorded length). False when
// data is not exactly one command in the old layout.
bool ConvertV34UserCmd(const unsigned char* data, size_t size, std::vector<unsigned char>& out);

} // namespace DemoConvert
} // namespace SourceFilmmaker
