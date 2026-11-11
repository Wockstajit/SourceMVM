#pragma once

// Rebindable shortcuts: the engine-free part (Keybinds.h is the runtime around it).
//
// HOW REBINDING WORKS. The director's key handler (MovieMode::OnKey) tests literal keys in a fixed
// order, and that order is what decides which panel a key belongs to. Rebinding does not touch
// those tests. Instead the key that was physically pressed is translated, before the handler sees
// it, into the key the handler has always listened for:
//
//   pressed key is bound to an action     -> that action's CANONICAL key (the key the handler tests)
//   pressed key is a default that was     -> kUnboundKey, which no test matches, so the key does
//   bound away and nothing else took it      nothing of ours (it still goes through the handler,
//                                            so a text field or a running render still swallows it)
//   any other key                         -> itself
//
// So "place a camera key" moved from K to P means: P arrives as K, and K arrives as nothing.
//
// ONE KEY CAN CARRY TWO ACTIONS. X is X-ray and roll right out of the box: which one happens
// depends on the camera you are in. Dispatch() therefore returns every key the handler should be
// shown for one press, in table order, and the caller stops at the first one the handler takes.
// Roll comes before X-ray in the table for that reason: in CS2's free camera the roll takes the
// press and X-ray never sees it, exactly as before the two could be separated.
//
// Roll's canonical keys are two codes Windows never sends (kRollRightKey / kRollLeftKey) instead
// of X and Z, so that roll and X-ray can be told apart once they sit on different keys, and so
// that Z still reaches the handler as Z for Ctrl+Z.
// Modifiers are not part of a binding; Shift / Ctrl keep their meaning on whatever key is bound
// (Shift+<ease key> still eases in).
//
// Only single keys the handler owns outright are rebindable. Esc, the modifiers, the letters of
// the Ctrl chords (Z A C D V), Delete / Backspace, W A S D (free-cam movement, also "stop the
// path") and the mouse stay fixed: they are shared with CS2, with text entry or with the shared
// free-cam input and have no single owner to move.

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace Filmmaker {
namespace KeybindsMath {

struct Action {
	const char* id;      // settings key suffix (`bind.<id>`) and the name the scripts use
	int defaultKey;      // the key it is on out of the box
	int canonicalKey;    // the key the handler tests for it
};

inline constexpr int kRollRightKey = 0xE8;   // unassigned virtual-key codes
inline constexpr int kRollLeftKey = 0xE9;

// Order is the order the settings page lists them in.
inline constexpr Action kActions[] = {
	{ "marker.place", 0x4B, 0x4B },   // K
	{ "marker.delete", 0x4C, 0x4C },  // L
	{ "marker.edit", 0x46, 0x46 },    // F
	{ "path.play", 0x4A, 0x4A },      // J
	{ "demo.pause", 0x20, 0x20 },     // Space
	{ "demo.back", 0x25, 0x25 },      // Left
	{ "demo.forward", 0x27, 0x27 },   // Right
	{ "hud.hide", 0x77, 0x77 },       // F8
	{ "roll.right", 0x58, kRollRightKey },   // X, ahead of X-ray: see the header comment
	{ "roll.left", 0x5A, kRollLeftKey },     // Z
	{ "xray", 0x58, 0x58 },           // X
	{ "key.ease", 0x78, 0x78 },       // F9
	{ "recoil.test", 0x54, 0x54 },    // T
	{ "takes.record", 0x75, 0x75 },   // F6
	{ "takes.play", 0x76, 0x76 },     // F7
	{ "test.menu", 0x2D, 0x2D },      // Insert
};
inline constexpr size_t kActionCount = sizeof(kActions) / sizeof(kActions[0]);
inline constexpr int kActionsPerKey = 2;   // how many actions may share one key
inline constexpr int kMaxDispatch = kActionsPerKey + 1;
inline constexpr int kUnboundKey = 0xFF;   // VK 0xFF is reserved by Windows; no handler tests it

inline int FindAction(const char* id) {
	if (!id) return -1;
	for (size_t i = 0; i < kActionCount; ++i) if (0 == std::strcmp(kActions[i].id, id)) return static_cast<int>(i);
	return -1;
}

// Keys a shortcut may not be moved to (see the header comment).
inline bool Reserved(int key) {
	switch (key) {
	case 0x1B:                               // Esc
	case 0x10: case 0x11: case 0x12:         // Shift, Ctrl, Alt
	case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5:
	case 0x5B: case 0x5C: case 0x5D:         // Windows keys, menu key
	case 0x08: case 0x2E:                    // Backspace, Delete
	case 0x0D: case 0x09:                    // Enter, Tab
	case 0x14: case 0x90: case 0x91:         // lock keys
	case 0x2C: case 0x13:                    // Print Screen, Pause
	case 0xC0:                               // the console key
	case 0x57: case 0x41: case 0x53: case 0x44:   // W A S D
	case 0x43: case 0x56:                    // C V (Ctrl chords; A and D are above; Z is roll left)
	case kUnboundKey:
		return true;
	default:
		return key <= 0x07 || key > 0xFE;    // mouse buttons and nothing
	}
}

// `bound[i]` is the key action i is on now. Fills `out` (kMaxDispatch entries) with the keys the
// handler should be shown for `pressed`, in order, and returns how many.
//
// A key with no action on it is shown as itself, unless it is some action's canonical key that
// the action has left: then it is kUnboundKey. Z is also shown as itself AFTER its actions,
// because the handler needs the real Z for Ctrl+Z whatever else sits on that key.
inline int Dispatch(const int* bound, int pressed, int* out) {
	int count = 0;
	bool claimed = false, shown = false;
	for (size_t i = 0; i < kActionCount; ++i) {
		if (kActions[i].canonicalKey == pressed) claimed = true;
		if (bound[i] != pressed || count >= kActionsPerKey) continue;
		out[count++] = kActions[i].canonicalKey;
		if (kActions[i].canonicalKey == pressed) shown = true;
	}
	if (!claimed && (count == 0 || pressed == 0x5A)) out[count++] = pressed;
	else if (claimed && !shown && count == 0) out[count++] = kUnboundKey;
	return count;
}

// The key the shared free-camera input (MirvInput, which rolls on X and Z and cannot be told
// otherwise) should see for `pressed`: X / Z for whatever roll is bound to, and nothing for a
// physical X / Z that roll has moved away from.
inline int FreeCamKey(const int* bound, int pressed, int rollRight, int rollLeft) {
	if (bound[rollRight] == pressed) return 0x58;
	if (bound[rollLeft] == pressed) return 0x5A;
	return (pressed == 0x58 || pressed == 0x5A) ? kUnboundKey : pressed;
}

// When `key` already carries as many actions as a key may (not counting `except`), one of them;
// otherwise -1.
inline int Conflict(const int* bound, int key, int except) {
	int holders = 0, last = -1;
	for (size_t i = 0; i < kActionCount; ++i) {
		if (static_cast<int>(i) != except && bound[i] == key) { ++holders; last = static_cast<int>(i); }
	}
	return holders >= kActionsPerKey ? last : -1;
}

// The cap text for a key. Not translated: these are what is printed on the keyboard.
inline void KeyName(int key, char* out, size_t size) {
	const char* fixed = nullptr;
	switch (key) {
	case 0x20: fixed = "SPACE"; break;
	case 0x25: fixed = "\xE2\x86\x90"; break;   // arrows
	case 0x26: fixed = "\xE2\x86\x91"; break;
	case 0x27: fixed = "\xE2\x86\x92"; break;
	case 0x28: fixed = "\xE2\x86\x93"; break;
	case 0x2D: fixed = "INS"; break;
	case 0x24: fixed = "HOME"; break;
	case 0x23: fixed = "END"; break;
	case 0x21: fixed = "PGUP"; break;
	case 0x22: fixed = "PGDN"; break;
	case 0x6A: fixed = "NUM *"; break;
	case 0x6B: fixed = "NUM +"; break;
	case 0x6D: fixed = "NUM -"; break;
	case 0x6E: fixed = "NUM ."; break;
	case 0x6F: fixed = "NUM /"; break;
	case 0xBA: fixed = ";"; break;
	case 0xBB: fixed = "="; break;
	case 0xBC: fixed = ","; break;
	case 0xBD: fixed = "-"; break;
	case 0xBE: fixed = "."; break;
	case 0xBF: fixed = "/"; break;
	case 0xDB: fixed = "["; break;
	case 0xDC: fixed = "\\\\"; break;   // written into a JSON string
	case 0xDD: fixed = "]"; break;
	case 0xDE: fixed = "'"; break;
	default: break;
	}
	if (fixed) std::snprintf(out, size, "%s", fixed);
	else if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z')) std::snprintf(out, size, "%c", static_cast<char>(key));
	else if (key >= 0x70 && key <= 0x87) std::snprintf(out, size, "F%d", key - 0x70 + 1);
	else if (key >= 0x60 && key <= 0x69) std::snprintf(out, size, "NUM %d", key - 0x60);
	else std::snprintf(out, size, "KEY %d", key);
}

} // namespace KeybindsMath
} // namespace Filmmaker
