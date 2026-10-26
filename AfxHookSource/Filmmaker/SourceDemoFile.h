#pragma once

// Reads the game events out of a Source 1 demo file without playing it, so the demo bar can show
// every kill, death and round from the start, before playback reaches them.
//
// The format is the Orange Box engine's (demo protocol 3, network protocol 24), which the current
// Counter-Strike: Source, Team Fortress 2, Day of Defeat: Source and Half-Life 2: Deathmatch all
// write. Every network message in a packet is either read or skipped by its length, so one
// unknown message stops the scan with an error instead of reading garbage. Engine-free; the hook
// and Automation/tests/source-demo-file-tests.cpp both compile it.

#include <cstddef>
#include <string>
#include <vector>

namespace SourceFilmmaker {
namespace DemoFile {

struct Header {
	int demoProtocol = 0;
	int networkProtocol = 0;
	std::string serverName;
	std::string clientName;
	std::string mapName;
	std::string gameDirectory;
	float playbackTime = 0;
	int playbackTicks = 0;
	int playbackFrames = 0;
};

// One game event field. Source 1 game events carry strings, floats, longs, shorts, bytes and bools;
// every number is kept as an int except floats.
struct Field {
	std::string key;
	enum class Type { String, Float, Int, Bool } type = Type::Int;
	std::string text;
	float number = 0;
	int value = 0;
};

struct Event {
	int tick = 0; // demo tick, as demo_gototick and the engine's playback tick count
	std::string name;
	std::vector<Field> fields;

	const Field* Find(const char* key) const;
	int Int(const char* key, int fallback = -1) const;
	std::string Text(const char* key) const;
};

struct Scan {
	bool ok = false;
	std::string error;  // why the scan stopped, when !ok
	Header header;
	std::vector<Event> events;
	int frames = 0;     // demo frames read
	int messages = 0;   // network messages read or skipped
};

// Scans a demo held in memory and keeps the events whose names are in wanted (all when empty).
Scan ScanDemo(const unsigned char* data, size_t size, const std::vector<std::string>& wanted);

// Reads path and scans it. A file that cannot be opened is a failed scan with an error.
Scan ScanDemoFile(const std::string& path, const std::vector<std::string>& wanted);

} // namespace DemoFile
} // namespace SourceFilmmaker
