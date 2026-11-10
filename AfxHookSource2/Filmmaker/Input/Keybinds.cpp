#include "Keybinds.h"

#include "KeybindsMath.h"
#include "../Config/FilmmakerSettingsStore.h"
#include "../Debug/MvmDebugLog.h"

#include "../../../Shared/AfxConsole.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace Filmmaker {
namespace {

using namespace KeybindsMath;

// The live table, read on the window-procedure thread.
std::atomic<int> g_bound[kActionCount];
std::atomic<bool> g_ready{ false };
std::atomic<int> g_capture{ -1 };          // action waiting for a key, or -1
// A key pressed during capture, handed from the window-procedure thread to the main thread
// (which owns the settings store): the action in the high half, the key in the low half.
std::atomic<int> g_pendingCapture{ -1 };
std::atomic<uint64_t> g_revision{ 1 };

// Main thread only.
int g_conflictAction = -1, g_conflictWith = -1;
unsigned g_frames = 0;

std::string SettingKey(size_t index) { return std::string("bind.") + kActions[index].id; }

void EnsureReady() {
	if (g_ready.load(std::memory_order_acquire)) return;
	for (size_t i = 0; i < kActionCount; ++i) g_bound[i].store(kActions[i].defaultKey, std::memory_order_relaxed);
	g_ready.store(true, std::memory_order_release);
}

// Reads the stored binds into the live table. A stored key that is reserved, or that an earlier
// action already holds, falls back to the default so a hand-edited file cannot bind two actions
// to one key.
void LoadFromSettings() {
	EnsureReady();
	int next[kActionCount];
	for (size_t i = 0; i < kActionCount; ++i) {
		const std::string stored = FilmmakerSettings_UiString(SettingKey(i).c_str(), "");
		const int key = stored.empty() ? 0 : std::atoi(stored.c_str());
		next[i] = (key > 0 && !Reserved(key)) ? key : kActions[i].defaultKey;
	}
	for (size_t i = 0; i < kActionCount; ++i) {
		if (Conflict(next, next[i], static_cast<int>(i)) >= 0) next[i] = kActions[i].defaultKey;
	}
	bool changed = false;
	for (size_t i = 0; i < kActionCount; ++i) {
		if (g_bound[i].exchange(next[i], std::memory_order_relaxed) != next[i]) changed = true;
	}
	if (changed) g_revision.fetch_add(1, std::memory_order_relaxed);
}

void Snapshot(int* out) {
	EnsureReady();
	for (size_t i = 0; i < kActionCount; ++i) out[i] = g_bound[i].load(std::memory_order_relaxed);
}

// Main thread. Puts `action` on `key` unless that is not allowed; records the clash for the page.
bool Bind(int action, int key) {
	int bound[kActionCount];
	Snapshot(bound);
	g_conflictAction = g_conflictWith = -1;
	if (Reserved(key)) { g_conflictAction = action; g_revision.fetch_add(1, std::memory_order_relaxed); return false; }
	const int other = Conflict(bound, key, action);
	if (other >= 0) {
		g_conflictAction = action; g_conflictWith = other;
		g_revision.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	FilmmakerSettings_SetUi(SettingKey(action).c_str(), std::to_string(key).c_str());
	g_bound[action].store(key, std::memory_order_relaxed);
	g_revision.fetch_add(1, std::memory_order_relaxed);
	if (MvmDebugLog_Active()) MvmDebugLog_Linef("keybind", "%s -> vk %d", kActions[action].id, key);
	return true;
}

void SetCapture(int action) {
	g_capture.store(action, std::memory_order_relaxed);
	g_revision.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

void Keybinds_MainThreadFrame() {
	EnsureReady();
	const int pending = g_pendingCapture.exchange(-1, std::memory_order_relaxed);
	if (pending >= 0) {
		const int action = pending >> 16, key = pending & 0xFFFF;
		if (action >= 0 && action < static_cast<int>(kActionCount)) Bind(action, key);
	}
	// A settings load, a reset or a practice session replaces the stored keys behind our back.
	if ((g_frames++ % 30) == 0) LoadFromSettings();
}

int Keybinds_Dispatch(int pressedKey, int* out) {
	int bound[kActionCount];
	Snapshot(bound);
	return Dispatch(bound, pressedKey, out);
}

int Keybinds_FreeCamKey(int pressedKey) {
	static const int rollRight = FindAction("roll.right"), rollLeft = FindAction("roll.left");
	int bound[kActionCount];
	Snapshot(bound);
	return FreeCamKey(bound, pressedKey, rollRight, rollLeft);
}

bool Keybinds_OnKeyDown(int pressedKey) {
	const int action = g_capture.load(std::memory_order_relaxed);
	if (action < 0) return false;
	// A modifier on its way to nothing is not an answer; wait for a real key.
	if (pressedKey == 0x10 || pressedKey == 0x11 || pressedKey == 0x12 || (pressedKey >= 0xA0 && pressedKey <= 0xA5)) return true;
	g_capture.store(-1, std::memory_order_relaxed);
	if (pressedKey != 0x1B) g_pendingCapture.store((action << 16) | (pressedKey & 0xFFFF), std::memory_order_relaxed);
	g_revision.fetch_add(1, std::memory_order_relaxed);
	return true;
}

uint64_t Keybinds_Revision() { return g_revision.load(std::memory_order_relaxed); }

std::string Keybinds_NamesJson() {
	int bound[kActionCount];
	Snapshot(bound);
	std::string json = "{";
	char name[24];
	for (size_t i = 0; i < kActionCount; ++i) {
		KeyName(bound[i], name, sizeof(name));
		if (i) json += ',';
		json += '"'; json += kActions[i].id; json += "\":\""; json += name; json += '"';
	}
	json += '}';
	return json;
}

std::string Keybinds_PageJson() {
	int bound[kActionCount];
	Snapshot(bound);
	std::string json = "{\"rows\":[";
	char name[24];
	for (size_t i = 0; i < kActionCount; ++i) {
		KeyName(bound[i], name, sizeof(name));
		if (i) json += ',';
		json += "[\""; json += kActions[i].id; json += "\",\""; json += name; json += "\",";
		json += bound[i] == kActions[i].defaultKey ? '1' : '0';
		json += ']';
	}
	const int capture = g_capture.load(std::memory_order_relaxed);
	json += "],\"capture\":\"";
	if (capture >= 0 && capture < static_cast<int>(kActionCount)) json += kActions[capture].id;
	json += "\",\"conflict\":[\"";
	if (g_conflictAction >= 0) json += kActions[g_conflictAction].id;
	json += "\",\"";
	if (g_conflictWith >= 0) json += kActions[g_conflictWith].id;
	json += "\"]}";
	return json;
}

void Keybinds_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	EnsureReady();
	const char* arg = (argc >= 3) ? args->ArgV(2) : nullptr;
	if (!arg) {
		int bound[kActionCount];
		Snapshot(bound);
		char name[24];
		for (size_t i = 0; i < kActionCount; ++i) {
			KeyName(bound[i], name, sizeof(name));
			advancedfx::Message("  %-14s vk %3d  %s%s\n", kActions[i].id, bound[i], name,
				bound[i] == kActions[i].defaultKey ? "" : "  (changed)");
		}
		advancedfx::Message("usage: %s keybind <action> <vk|default> | capture <action> | cancel | reset\n", cmd);
		return;
	}
	if (0 == _stricmp(arg, "cancel")) { SetCapture(-1); return; }
	if (0 == _stricmp(arg, "reset")) {
		for (size_t i = 0; i < kActionCount; ++i) {
			FilmmakerSettings_SetUi(SettingKey(i).c_str(), std::to_string(kActions[i].defaultKey).c_str());
			g_bound[i].store(kActions[i].defaultKey, std::memory_order_relaxed);
		}
		g_conflictAction = g_conflictWith = -1;
		SetCapture(-1);
		advancedfx::Message("mirv_filmmaker: shortcuts reset to their defaults.\n");
		return;
	}
	const bool capture = 0 == _stricmp(arg, "capture");
	const char* id = capture ? (argc >= 4 ? args->ArgV(3) : nullptr) : arg;
	const int action = FindAction(id);
	if (action < 0) { advancedfx::Warning("%s keybind: unknown action \"%s\".\n", cmd, id ? id : ""); return; }
	if (capture) { g_conflictAction = g_conflictWith = -1; SetCapture(action); return; }
	const char* value = (argc >= 4) ? args->ArgV(3) : nullptr;
	if (!value) { advancedfx::Warning("usage: %s keybind %s <vk|default>\n", cmd, id); return; }
	const int key = (0 == _stricmp(value, "default")) ? kActions[action].defaultKey : std::atoi(value);
	if (Bind(action, key)) advancedfx::Message("mirv_filmmaker: %s = vk %d.\n", id, key);
	else advancedfx::Warning("%s keybind: vk %d is reserved or already used.\n", cmd, key);
}

} // namespace Filmmaker
