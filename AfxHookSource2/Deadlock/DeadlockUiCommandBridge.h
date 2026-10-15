#pragma once

// How a Panorama button in Deadlock actually does something.
//
// THE PROBLEM THIS SOLVES. CS2's HUD Panorama context exposes `GameInterfaceAPI.ConsoleCommand`,
// and every panel in the CS2 filmmaker runs on it. Deadlock's HUD context exposes NO engine
// bindings whatsoever -- measured in game on build 6687, by enumerating the global object from
// inside a live RunScript:
//
//     globals: ... console PlayUISoundEvent StopUISoundEvent IsUISoundEventPlaying
//              PlayLocalHeroSound StopLocalHeroSound panorama SharedArrayBuffer $
//
// No GameInterfaceAPI, no GameUI, no UiToolkitAPI. Deadlock's own panels call `CitadelConCommand`,
// but that is bound per-XML-context by the panel class that declares it and is undefined here --
// confirmed both as a bare global and through the panel-event expression language, which raised
// "CitadelConCommand is not defined" from inside an onactivate handler.
//
// So the Deadlock port had a UI whose every button was inert, and nothing said so: `cmd()` wrapped
// the call in a try and swallowed the ReferenceError. The panel looked built, responded to hover,
// and did nothing.
//
// THE CHANNEL. Panorama's `$.Msg` writes to the engine console, and tier0's logging system lets
// several listeners observe every line. The hook already relies on that for netcon output. So the
// panel MARKS a line, this module's listener recognises the marker, and the command is queued and
// executed on the main thread.
//
//     JS:   $.Msg('\x02MVMCMD mirv_filmmaker workspace camera\n')
//     C++:  listener -> queue -> main-thread pump -> Commands_DispatchLine / ExecuteClientCmd
//
// WHY A QUEUE AND NOT A DIRECT CALL. The listener runs on whatever thread printed, which for a
// Panorama message is the main thread today and is not guaranteed to stay that way. Executing a
// console command off the main thread is the same class of mistake as calling RunScript off it.
// The queue makes the thread the command runs on a property of this file rather than of whoever
// happened to print.
//
// RE-ENTRANCY. Dispatching a command prints, and printing goes back through tier0 to this
// listener. The marker is the first guard -- our own output never carries it -- and a draining
// flag is the second, so a command that somehow echoed its own marker cannot recurse.

#include <cstddef>

namespace Deadlock {

// Registers the tier0 listener. Safe to call twice; a second call is a no-op.
bool UiCommandBridge_Start();
void UiCommandBridge_Stop();
bool UiCommandBridge_Active();

// MAIN thread, once per frame. Drains whatever the panel asked for since the last frame.
void UiCommandBridge_PumpMainThread();

// The marker a panel prefixes to a command line. Exposed so the JS side and the C++ side cannot
// drift: the *Js.h fragments build their prefix from this value through DeadlockWorkspaces.cpp.
//
// It opens with STX (0x02), a control character no engine message contains and no console font
// renders, so a queued command does not also become a visible line of console noise.
extern const char* const kUiCommandMarker;

// Counters for `mirv_filmmaker health`. A UI that does nothing with `queued=0` is a panel that
// never called through; `queued` climbing with `executed` flat is a pump that is not running.
unsigned long long UiCommandBridge_Queued();
unsigned long long UiCommandBridge_Executed();
unsigned long long UiCommandBridge_Dropped();

void UiCommandBridge_Report();

} // namespace Deadlock
