#include "AltEnvironmentSceneProbe.h"

#include "../Debug/MvmDebugLog.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>

namespace Filmmaker {

namespace {

constexpr size_t kDumpBytes = 0x200;

std::atomic<int> g_left{0}; // objects still to dump; 0 = disarmed
std::mutex g_mx;
std::set<const void*> g_seen;

bool ReadPointer(const void* at, const void** out) {
	__try {
		*out = *(const void* const*)at;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool Copy(const void* from, void* to, size_t bytes) {
	__try {
		memcpy(to, from, bytes);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Eight lines of 16 floats, each value also as its raw bits where it is not a plausible float, so a
// pointer or a count reads as such.
void Dump(const void* object, const char* via, int element) {
	alignas(16) unsigned char bytes[kDumpBytes] = {};
	if (!Copy(object, bytes, sizeof(bytes))) {
		MvmDebugLog_LinefAlways("altenv.sceneprobe", "obj=%p via=%s element=%d unreadable", object, via, element);
		return;
	}
	const void* vtable = *(const void* const*)bytes;
	MvmDebugLog_LinefAlways("altenv.sceneprobe", "obj=%p via=%s element=%d vtable=%p", object, via, element, vtable);
	const float* f = (const float*)bytes;
	const uint32_t* u = (const uint32_t*)bytes;
	for (size_t line = 0; line < kDumpBytes / 64; ++line) {
		char text[1024];
		int at = std::snprintf(text, sizeof(text), "  +%03zx:", line * 64);
		for (size_t k = 0; k < 16 && at < int(sizeof(text)) - 32; ++k) {
			const size_t i = line * 16 + k;
			const float v = f[i];
			const bool plausible = v == 0.0f || (v == v && v > -1e6f && v < 1e6f && (v > 1e-6f || v < -1e-6f));
			at += plausible ? std::snprintf(text + at, sizeof(text) - size_t(at), " %.4f", v)
			                : std::snprintf(text + at, sizeof(text) - size_t(at), " #%08x", u[i]);
		}
		MvmDebugLog_LinefAlways("altenv.sceneprobe", "%s", text);
	}
}

} // namespace

bool AltEnvironment_SceneProbeArmed() { return g_left.load(std::memory_order_relaxed) > 0; }

void AltEnvironment_SceneProbeNote(const void* batch, int count, size_t stride, size_t objectOffset, const char* via) {
	if (!batch || count <= 0) return;
	std::lock_guard<std::mutex> lock(g_mx);
	for (int i = 0; i < count && g_left.load() > 0; ++i) {
		const void* object = nullptr;
		if (!ReadPointer((const unsigned char*)batch + size_t(i) * stride + objectOffset, &object) || !object) continue;
		if (!g_seen.insert(object).second) continue;
		Dump(object, via, i);
		if (--g_left == 0) MvmDebugLog_LinefAlways("altenv.sceneprobe", "done: %zu objects", g_seen.size());
	}
}

void AltEnvironment_SceneProbeArm(int maxObjects, const char* expected) {
	std::lock_guard<std::mutex> lock(g_mx);
	g_seen.clear();
	MvmDebugLog_LinefAlways("altenv.sceneprobe", "armed for %d objects; %s", maxObjects, expected ? expected : "");
	g_left = maxObjects > 0 ? maxObjects : 0;
}

} // namespace Filmmaker
