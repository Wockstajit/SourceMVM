#pragma once

// Runtime diagnostics for the Deadlock hook: the `mvm_debug` console command and the exit trace.
//
// `mvm_debug start|stop|status` was a stub on this target. The Deadlock command table wired it to
// MvmDebugLog_Command, which only records the command line of a log that is ALREADY running and
// returns silently otherwise -- so `mvm_debug start` did nothing, no file appeared, and every
// crash investigation fell back to guessing. The real start/stop lives in CS2's
// FilmmakerDebugCommand.cpp, which cannot be shared because it dumps CS2-only state (follow
// camera, cosmetics, particle FX). This is the Deadlock-only equivalent over the same logger.
//
// The exit trace is the other half. Deadlock does not crash when the hook does something it
// dislikes -- it EXITS CLEANLY: no dump, no exception, no event-log entry. A vectored exception
// handler therefore sees nothing. What does see it is a detour on ExitProcess / TerminateProcess
// that records who called it, module and offset, with a short stack, into both the boot trace
// (raw kernel32, survives anything) and the debug log (which it then closes so the file is whole).

#include "../../Shared/AfxConsole.h"

namespace Deadlock {

/// <summary>Installs the exit trace. Call from the client's Init, never under the loader lock.</summary>
void Debug_Install();

/// <summary>`mvm_debug start|stop|status`, and any other line whose first token is mvm_debug.</summary>
void Debug_Command(advancedfx::ICommandArgs* args);

/// <summary>Breadcrumb for DLL_PROCESS_DETACH; closes an open debug log so it is flushed.</summary>
void Debug_OnProcessDetach();

} // namespace Deadlock
