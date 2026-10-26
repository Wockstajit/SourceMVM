#pragma once

// A plain TCP console served by the hook itself, for games whose engine has no -netconport.
//
// The protocol is the one every script under Automation/ already speaks to CS2's engine netcon:
// connect to 127.0.0.1:<port>, write ASCII commands terminated by newline, read ASCII console
// output back, no handshake. The game-specific parts stay with the caller: where console output
// comes from (it calls Write), which thread may run commands (it drains PopCommand there), and how
// that thread is woken (OnLineReceived).
//
// Threading: the listener and the writer run on their own threads and never touch the engine.
// Received lines wait in a queue for the caller's main thread. Output goes into a bounded queue
// drained by the writer thread, never written synchronously from the logging callback, so a slow
// or stalled reader drops lines instead of hitching the game. Drops are counted.

#include <string>

namespace advancedfx {
namespace NetconServer {

struct Config {
	unsigned short port = 0;
	// Sent to every client on connect, so "did I reach the hook" is answerable at a glance.
	const char* banner = nullptr;
	// Prefix for the warnings this module prints through advancedfx::Warning.
	const char* name = "netcon";
	// Called from the listener thread after a line was queued. Must be thread-safe and must not
	// block; typically it posts a message to the game window.
	void (*onLineReceived)() = nullptr;
};

// Opens the listener on loopback. Safe to call twice; the second call is a no-op.
bool Start(const Config& config);
void Stop();

bool Active();
unsigned short Port();
int ClientCount();

// Takes the oldest received command. Call from the thread that may run console commands.
bool PopCommand(std::string& outCommand);
bool HasPendingCommands();

// Queues text for every connected client. Safe from any thread; drops rather than blocks when
// the queue is full.
void Write(const char* text);

// The caller counts what it executed, so the stats say how many commands actually ran.
void CountExecuted();

struct Stats {
	bool active;
	unsigned short port;
	int clients;
	unsigned long long commandsReceived;
	unsigned long long commandsExecuted;
	unsigned long long linesSent;
	unsigned long long linesDropped;
};

Stats GetStats();

} // namespace NetconServer
} // namespace advancedfx
