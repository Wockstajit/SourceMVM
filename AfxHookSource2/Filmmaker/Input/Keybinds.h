#pragma once

// Rebindable shortcuts (KeybindsMath.h explains the model: a pressed key is translated to the key
// the director's handler has always tested, so the handler itself is unchanged).
//
//   stored    UI keys `bind.<action>` = the virtual-key code, in the settings store. A missing
//             key means the default.
//   console   mirv_filmmaker keybind                     list
//             mirv_filmmaker keybind <action> <vk|default>
//             mirv_filmmaker keybind capture <action>    the next key pressed becomes the binding
//             mirv_filmmaker keybind cancel
//             mirv_filmmaker keybind reset               everything back to default
//   page      the SOURCE:MVM tab in CS2's settings menu (Panorama/SettingsTabJs.h)
//
// Threads: binds are read on the window-procedure thread for every key, so the live table is a
// set of atomics. The settings store is only ever touched on the main thread.

#include <cstdint>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Main thread, once a frame: picks up binds changed by a settings load or reset.
void Keybinds_MainThreadFrame();

// Window-procedure thread. The keys MovieMode::OnKey should be shown for the key that was pressed
// (up to 3, in order; the caller stops at the first one the handler takes). Returns the count.
int Keybinds_Dispatch(int pressedKey, int* out);

// Window-procedure thread. The key the shared free-camera input should see (roll follows its
// bindings; it is hardwired to X and Z in there).
int Keybinds_FreeCamKey(int pressedKey);

// Window-procedure thread, BEFORE the menu gate (the settings page that captures a key is CS2's
// own menu, where every other director key stands down). True when the press was taken as the
// new binding or as the Esc that cancels the capture; the caller swallows it.
bool Keybinds_OnKeyDown(int pressedKey);

// Changes whenever a binding or the capture state changes: what the settings page and the key
// names in the panels are refreshed on.
uint64_t Keybinds_Revision();

// `{"marker.place":"K",...}`: the cap text per action, for the panel scripts.
std::string Keybinds_NamesJson();

// The settings page's rows: `{"rows":[["marker.place","K",1],...],"capture":"","conflict":["id","id"]}`.
// The third value is 1 when the action is on its default key.
std::string Keybinds_PageJson();

void Keybinds_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
