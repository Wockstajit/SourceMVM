#pragma once

// The netcon channel: a plain TCP console, so Deadlock can be driven from automation exactly the
// way CS2 is.
//
// CS2 gets this from the engine itself (`-netconport`), and every script under Automation/ assumes
// it: open a socket to 127.0.0.1, write ASCII commands terminated by newline, read ASCII output
// back, no handshake. Deadlock's engine2.dll carries the same netconsole server code -- the
// "Unable to open netconsole on port %d" and "must send PASS command" strings are both present --
// but whether the client build exposes it through the same launch flag is not something a static
// scan can answer.
//
// Rather than make the automation contract depend on that answer, this hosts the same protocol
// itself. Same port, same line format, same client on the other end. `mirv_filmmaker netcon` says
// which one is actually serving.
//
// Hosting it ourselves also removes the 256-byte truncation on the engine's own command path,
// which is what forces the CS2 UI to be driven through short registered helpers rather than
// inline JavaScript.
//
// Threading: the listener and the writer live on their own threads and never touch the engine.
// Received lines go into a queue that the MAIN thread drains, because that is the only thread
// allowed to run console commands. Output goes into a bounded ring buffer and is drained by the
// writer thread -- deliberately not written synchronously from the logging callback, since a slow
// or stalled reader is otherwise indistinguishable from the game hitching.

#include <string>

namespace Deadlock {

// Opens the listener. `port` of 0 uses the default (29020, chosen so a CS2 session on 29010 and a
// Deadlock session can be driven side by side). Safe to call twice.
bool Netcon_Start(unsigned short port);

void Netcon_Stop();

bool Netcon_Active();
unsigned short Netcon_Port();
int Netcon_ClientCount();

// Executes any commands received since the last call. MAIN THREAD ONLY.
void Netcon_PumpMainThread();

// Queues a line for every connected client. Safe from any thread; drops rather than blocks when
// the buffer is full, and counts the drops.
void Netcon_Write(const char* text);

struct NetconStats {
	bool active;
	unsigned short port;
	int clients;
	unsigned long long commandsExecuted;
	unsigned long long linesSent;
	unsigned long long linesDropped;
};

NetconStats Netcon_GetStats();

} // namespace Deadlock
