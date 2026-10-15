#pragma once

// `mirv_filmmaker health` -- one screen that says whether the version-coupled machinery
// actually resolved against the CURRENTLY INSTALLED CS2 build.
//
// WHY THIS EXISTS. A CS2 update breaks three classes of thing (see
// Docs/agent/cs2-update-recovery.md). Only one of them is loud:
//
//   * byte-pattern scans        -> an error box per failure, and
//                                  Automation/verify/verify-cs2-signatures.py finds them offline;
//   * vtable slot indices       -> SILENT. A moved QueueCallback index simply stops the render
//                                  pump, and everything downstream (demo scans, FX precache,
//                                  campath drawer) quietly does nothing;
//   * schema field/class NAMES  -> SILENT for the optional fields, which are guarded only by
//                                  `!= 0` at their read sites.
//
// So after an update the game can launch clean and still be broken. This command turns that
// into one readable answer instead of a bisect. It only READS state; it changes nothing.
// The same report is mirrored into the mvm_debug log (category `health`), because the console
// cannot carry our output to a netcon client -- see Docs/agent/mvm-debug-instrumentation.md.

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// argc/args/cmd forwarded from FilmmakerCommand.cpp's dispatcher (ArgV(1) == "health").
void Filmmaker_RunHealthCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// Every main-thread frame; records one line every few seconds into the always-on recorder, so a
// crash or "the game crawled and I closed it" report shows frame times, the hook's own cost, VRAM
// against budget, the GPU and the relight passes leading up to it, without mvm_debug.
void FilmmakerHealth_Heartbeat();

} // namespace Filmmaker
