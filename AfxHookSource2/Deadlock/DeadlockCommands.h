#pragma once

// Direct dispatch for the commands this DLL registers itself.
//
// Netcon used to hand every received line to Engine_ExecuteClientCmd, which calls an engine vtable
// slot that is a HYPOTHESIS on Deadlock -- CS2's index minus one. A wrong slot does not fault, so
// the call returned "ok" and nothing ran, and `mirv_filmmaker health` over netcon answered with
// silence. Our own commands have no business depending on that slot: they are ours, and the
// callback is in this module. So netcon dispatches them here first and only falls through to the
// engine for engine commands.

namespace Deadlock {

/// <summary>
/// Runs one console line if its first token names a command this DLL registered.
/// </summary>
/// <returns>true if the line was ours and was dispatched; false if the caller should hand it to
/// the engine instead.</returns>
bool Commands_DispatchLine(const char* line);

} // namespace Deadlock
