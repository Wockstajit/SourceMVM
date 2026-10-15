#pragma once

// COMMUNITY buttons on CS2's main menu (also shown over the in-game pause menu, which is the same
// panel): the HC Tracker badge in the top bar's empty right-hand container, which opens
// hctracker.com in the browser, and "Report Bugs" at the bottom right, which writes a plain-text
// report (Community/BugReport) the player reviews before choosing to send it. A crash prompt offers
// earlier sessions' crash reports (Community/CrashReport) the same way: nothing is sent unasked.
//
// Its own module, host panel and bridge, like PlayMenu: neither knows the other exists.
//
// A hidden host panel (#MvmCommunityHost) carries the C++<->JS bridge attributes:
//   JS  -> C++: appends to host "cmd": "hctracker", "bugreport <text> <discord>" (each
//               encodeURIComponent'd), "sendreport <seq>", "showreport", "issues", "crashsend",
//               "crashdecline", "crashshow", "debuglog on|off".
//   C++ -> JS : sets host "state" (JSON: debug log on, last report, pending crash reports) and runs
//               $.MvmCommunity.onState().

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Per-main-thread-frame pump: injects the JS once the main-menu context exists, drains the
// JS->C++ queue, pushes state when it changes. No-op without a main menu.
void Community_RunMainThreadFrame();

// "mirv_filmmaker community status|rebuild|report <text>|eval <js>".
void Community_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
