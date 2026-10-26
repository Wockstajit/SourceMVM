#pragma once

// Old Counter-Strike: Source demos (the "v34" engine, network protocol 7) play like any other.
// The hook wraps the engine's playdemo command: when the file is an old demo, it is converted once
// (DemoConvert/ConvertV34.h), with its brush models matched to the installed map, into
// source_mvm/converted/ under the game directory, and the converted copy is played. Later plays
// reuse the copy until the original file changes. Conversion runs on a worker thread.

namespace SourceFilmmaker {

// Wraps playdemo. Safe to call every frame; it retries until the command exists.
void OldDemos_Init();

// Main thread, once a frame: reports a finished conversion and plays its result.
void OldDemos_Frame();

// One line for mirv_filmmaker health.
void OldDemos_PrintHealth();

} // namespace SourceFilmmaker
