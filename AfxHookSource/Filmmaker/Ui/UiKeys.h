#pragma once

// Keys while a workspace is open, shared by the game host (window messages) and the browser mockup
// (DOM keyCode, which uses the same numbers as Windows virtual-key codes for every key here).
// Engine-free and header-only.

#include <imgui.h>

namespace SourceFilmmaker {
namespace Ui {

enum class WorkspaceKey { None, Close, ToggleGame };

// What a key press does to the workspace itself, before ImGui sees it: ESC closes it, G trades the
// mouse between the UI and the game (CS2's rule), except while a text field is taking typing.
inline WorkspaceKey WorkspaceKeyFor(int vk, bool textInput) {
	if (vk == 0x1B) return WorkspaceKey::Close;
	if (vk == 'G' && !textInput) return WorkspaceKey::ToggleGame;
	return WorkspaceKey::None;
}

inline ImGuiKey ImGuiKeyFromVirtualKey(int vk) {
	if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
	if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
	switch (vk) {
	case 0x09: return ImGuiKey_Tab;
	case 0x25: return ImGuiKey_LeftArrow;
	case 0x27: return ImGuiKey_RightArrow;
	case 0x26: return ImGuiKey_UpArrow;
	case 0x28: return ImGuiKey_DownArrow;
	case 0x24: return ImGuiKey_Home;
	case 0x23: return ImGuiKey_End;
	case 0x2E: return ImGuiKey_Delete;
	case 0x08: return ImGuiKey_Backspace;
	case 0x20: return ImGuiKey_Space;
	case 0x0D: return ImGuiKey_Enter;
	case 0x10: return ImGuiKey_LeftShift;
	case 0x11: return ImGuiKey_LeftCtrl;
	case 0x12: return ImGuiKey_LeftAlt;
	default: return ImGuiKey_None;
	}
}

// Feeds one key to ImGui, with the modifier state ImGui tracks separately.
inline void FeedImGuiKey(ImGuiIO& io, int vk, bool down) {
	if (vk == 0x10) io.AddKeyEvent(ImGuiMod_Shift, down);
	if (vk == 0x11) io.AddKeyEvent(ImGuiMod_Ctrl, down);
	if (vk == 0x12) io.AddKeyEvent(ImGuiMod_Alt, down);
	const ImGuiKey key = ImGuiKeyFromVirtualKey(vk);
	if (key != ImGuiKey_None) io.AddKeyEvent(key, down);
}

} // namespace Ui
} // namespace SourceFilmmaker
