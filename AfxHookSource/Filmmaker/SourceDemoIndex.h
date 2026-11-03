#pragma once

// The demo index: the playing demo's game events, read from its file when playback starts, so the
// demo bar can mark every kill, death and round from the first frame instead of only the ones
// playback has reached.
//
// The engine prints "Playing demo from <file>." when a demo starts, by whatever path it was
// started; the console capture passes that line here. The file is then read on a worker thread
// (SourceDemoFile.h), so a long demo never stalls a frame.

#include "SourceDemoFile.h"

#include <memory>
#include <string>

namespace SourceFilmmaker {

struct DemoIndex {
	enum class Status { None, Scanning, Ready, Failed };
	Status status = Status::None;
	std::string file;  // as the engine named it, relative to the game directory
	std::string path;  // the file that was read
	std::string stamp; // its size and write time when it was read
	std::string error; // why it failed, when Failed
	double scanMs = 0;
	unsigned generation = 0; // bumps with every new demo, so consumers can rebuild their markers
	std::shared_ptr<const DemoFile::Scan> scan; // set when Ready
};

// Starts watching for demos. Installs the console capture if nothing else has.
void DemoIndex_Init();

// Main thread, once a frame: starts a scan for a newly started demo and publishes finished ones.
void DemoIndex_Frame();

// The current demo's index.
DemoIndex DemoIndex_Get();

// One line for mirv_filmmaker health.
void DemoIndex_PrintHealth();

} // namespace SourceFilmmaker
