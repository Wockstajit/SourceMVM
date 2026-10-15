#pragma once

// Hot-reload arbitrary Panorama JS from a file on disk, into the live in-game HUD context.
//
// WHY. Iterating on HUD JS meant edit -> cmake build -> close CS2 -> cmake install -> relaunch ->
// reload a demo -> reopen the panel: about three minutes per visual tweak. The existing escape
// hatch (`mirv_filmmaker editor eval <js>`) cannot replace it, because the command goes through
// the console: the console SPLITS on ';', MANGLES quotes, and truncates around 250 bytes, so it
// can carry a one-line probe and nothing else. Anything with a string literal or a statement
// separator in it -- i.e. any real UI code -- does not survive the trip.
//
// A file does not have any of those problems. The command only carries a PATH; the JS itself is
// read straight off disk as bytes, so it can be any size and contain anything.
//
//   mirv_filmmaker livejs load <path>      run the file once
//   mirv_filmmaker livejs watch <path>     run it now, then re-run whenever it changes on disk
//   mirv_filmmaker livejs unwatch          stop watching
//   mirv_filmmaker livejs status           what is watched, last result, last error
//
// The watch poll is mtime+size only (a cheap stat, throttled), so leaving it on costs nothing.
//
// SCOPE. This is a development tool. The scripts it runs are ordinary Panorama JS in the CSGOHud
// context -- exactly what the compiled-in fragments are -- so anything built this way can be
// pasted into a *Js.h fragment unchanged once it is right. It deliberately does not persist
// across a restart: nothing shipped should ever depend on a file outside the DLL.

#include "PanoramaBridge.h"

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Console entry: handles "mirv_filmmaker livejs ...".
void UiLive_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// Main/UI thread, once per frame. Re-runs the watched file when it changes on disk. Cheap no-op
// when nothing is being watched.
void UiLive_PumpMainThread();

} // namespace Filmmaker
