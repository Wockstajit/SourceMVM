#pragma once

// Entity field offsets, resolved BY NAME from the running game.
//
// THE POINT OF THIS FILE. Everything else version-coupled in this port is a byte pattern that has
// to be re-derived when Deadlock updates. Entity fields are not, and treating them as if they were
// is the single most expensive mistake available here. Source 2 ships its own class and field
// table in schemasystem.dll -- every declared class, every field, every offset, keyed by string --
// and the game reads it at startup to wire up networking. So does this. A field offset is
// therefore never a constant in this codebase; it is a lookup, and a Deadlock update moves it
// without anyone noticing because nothing was written down to go stale.
//
// That is also why reference/deadlock/offsets/ carries no entity offsets. A dumped field offset is
// a number that looks authoritative and is wrong after the next patch.
//
// WHAT CAN STILL BREAK. Two things, and both are reported rather than assumed:
//
//   * the pattern that finds CSchemaSystem itself (one byte pattern, in DeadlockAddresses), and
//   * the LAYOUT of the schema structures below, which is Valve's internal ABI and has shifted
//     before on the CS2 side.
//
// The layout is validated before it is trusted -- scope names must be printable and end in .dll,
// class counts must be sane, every pointer must be readable -- so a drift reads as "schema layout
// rejected" in `mirv_filmmaker health` instead of as a silent table of plausible garbage. The two
// layout constants are runtime-overridable (`mirv_filmmaker schema layout <scopePad> <classPad>`)
// so a shifted ABI can be repaired from the console before a rebuild.
//
// THREADING. Init runs once, on the main thread, when schemasystem.dll shows up. Lookups after
// that are reads of a finished map and are safe from anywhere.

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Deadlock {

// Reads the whole client-scope table. Safe to call more than once; the second call is a no-op.
// Returns false when the schema system did not resolve or its layout failed validation.
bool Schema_Init(HMODULE schemaSystemDll);

bool Schema_Ready();

// The offset of `field` within `klass`, or -1 when either is unknown.
//
// ALWAYS check for -1 at the call site. A missing field means Valve renamed something, and the
// right response is to disable that one feature -- never to write at offset 0, which is the object
// header and takes the game down somewhere unrelated ten frames later.
std::ptrdiff_t Schema_FieldOffset(const char* klass, const char* field);

// First match across several candidate class names. Source 2 moves fields between a base class and
// its derived class across updates, so a feature that names both survives the move.
std::ptrdiff_t Schema_FieldOffsetAny(const char* const* classes, int classCount, const char* field);

// Declared size of a class, or 0 when unknown.
uint32_t Schema_ClassSize(const char* klass);

bool Schema_HasClass(const char* klass);

// How many classes and fields the client scope declared. A count of zero with Ready() true means
// the scope was found and was empty, which is a different fault from the scope never resolving.
int Schema_ClassCount();
int Schema_FieldCount();

// Writes the whole resolved table to a JSON file. This is the live counterpart of the offline
// dumper, and the only sanctioned way to read field offsets out of this port -- a diagnostic, never
// an input to the build. `filter` keeps only classes whose name contains it; empty writes all.
bool Schema_DumpJson(const wchar_t* path, const char* filter);

// The default dump location: reference/deadlock/offsets/deadlock-schema.json under the repository,
// or %TEMP% when the hook cannot work out where that is.
std::wstring Schema_DefaultDumpPath();

// Repair hatch for a shifted internal ABI. Both are byte counts inside Valve's structures; see the
// layout block in the .cpp for what they pad past.
// Hex-dumps the first 64 bytes of one class's CSchemaClassInfo, plus how the struct above reads
// them. The repair tool for the next ABI drift: a wrong layout does not error, it produces a full
// table of plausible garbage, and these bytes are what a corrected struct gets written from.
bool Schema_DumpClassBytes(const char* klass, std::string& out);

void Schema_SetLayout(size_t scopeNamePad, size_t scopeCountPad);
void Schema_GetLayout(size_t& scopeNamePad, size_t& scopeCountPad);

// Re-runs resolution with the current layout, after an override. Returns what Init would.
bool Schema_Reload();

void Schema_Report();

} // namespace Deadlock
