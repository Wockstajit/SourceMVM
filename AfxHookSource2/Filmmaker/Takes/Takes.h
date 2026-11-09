#pragma once

// BOT TAKES: record yourself in a private match, then have real bots perform it, over and over,
// while you shoot them and record the demo. See Docs/features/bot-takes.md.

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker::Takes {

// `mirv_filmmaker takes ...`
void RunCommand(int argc, advancedfx::ICommandArgs *args, const char *cmd);
// Main thread, once per frame. Nothing runs here unless takes are in use.
void RunMainThreadFrame();
// `,"takes":{...}` for the panel.
void AppendStateJson(std::ostringstream &out);
// True while something is being recorded, for the on-screen indicator.
bool Recording();
// The line shown at the top of the screen; empty when nothing is happening. Main thread.
std::string HudLine(bool &recording);
// F6 records, F7 plays, in a private match. True when the key was claimed. Any thread.
bool HandleKey(unsigned vkey, bool down);

} // namespace Filmmaker::Takes
