#pragma once

// Console output capture for the Source 1 hook. tier0 routes every Msg, Warning and console print
// through one spew function; the hook chains its own in front of the engine's and fans each line
// out to the command port and the mvm_debug log. The chain is installed once and stays installed;
// each consumer switches itself on and off.

namespace SourceFilmmaker {

// Installs the spew function. Safe to call repeatedly. False when tier0 lacks the exports.
bool Console_Install();

// Whether our spew function is still the one tier0 calls. The engine may install its own later.
bool Console_Captured();

// Console lines go to the command port while this is on.
void Console_SetNetconEnabled(bool enabled);

// Console lines go to the mvm_debug log while this is on.
void Console_SetDebugLogEnabled(bool enabled);
bool Console_DebugLogEnabled();

// Called with every console line, on whatever thread printed it, for the few engine messages the
// filmmaker reacts to (the demo index watches for "Playing demo from"). Keep it cheap.
typedef void (*ConsoleLineObserver)(const char* line);
void Console_SetLineObserver(ConsoleLineObserver observer);

} // namespace SourceFilmmaker
