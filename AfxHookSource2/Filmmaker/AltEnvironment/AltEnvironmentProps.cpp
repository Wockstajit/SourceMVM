#include "AltEnvironmentProps.h"

#include "../Cosmetics/CosmeticEntityAccess.h" // CosmeticEntFromIndex, CosmeticReadEntityModelPath
#include "../Debug/MvmDebugLog.h"
#include "../World/WorldSchema.h"
#include "../../ClientEntitySystem.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace Filmmaker::AltEnvironment::Props {

namespace {

constexpr int kScanEvery = 30;              // frames between scans for new entities
constexpr size_t kOwnerHandle = 0xC0;      // CSceneObject: the owner entity handle (measured, see the header)
constexpr int kMaxEntities = 1 << 15;      // a handle's index is its low 15 bits
// The original map's props are told apart by their model, not their class: Inferno's 396 breakable
// bottles, pots and pans report no entity class at all, only the client class CBaseAnimGraph
// (`altenv props census`, measured 2026-10-24). A map prop's model is under models/; players are under
// characters/, weapons, grenades and the bomb under weapons/.
constexpr const char* kPropModels = "models/";

struct Hidden {
	int index = 0;
	CEntityInstance* ent = nullptr;
	bool broken = false; // its model lives in the original map's package: hidden whatever the toggle says
};

std::vector<Hidden> g_hidden;
// Read by the render threads: one bit per entity index, and whether any is set at all.
std::atomic<uint32_t> g_bits[kMaxEntities / 32];
std::atomic<bool> g_hiding{false};
std::atomic<uint32_t> g_mixedBatches{0}; // batches that held hidden and shown draws (only the shown ones drawn)
int g_frame = 0;
bool g_wasActive = false, g_wasShow = false;
std::string g_demoMap;

bool ReadOrigin(CEntityInstance* ent, float out[3]) {
	const std::ptrdiff_t ob = WorldSchema_Offset(kWF_Ent_BodyComponent);
	const std::ptrdiff_t on = WorldSchema_Offset(kWF_Body_SceneNode);
	const std::ptrdiff_t ot = WorldSchema_Offset(kWF_Node_NodeToWorld); // CTransform: position first
	if (!ent || !ob || !on || !ot) return false;
	__try {
		const unsigned char* body = *(const unsigned char* const*)((const unsigned char*)ent + ob);
		const unsigned char* node = body ? *(const unsigned char* const*)(body + on) : nullptr;
		if (!node) return false;
		memcpy(out, node + ot, sizeof(float) * 3);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void SetBit(int index, bool on) {
	if (index <= 0 || index >= kMaxEntities) return;
	const uint32_t bit = 1u << (index & 31);
	if (on) g_bits[index >> 5].fetch_or(bit, std::memory_order_relaxed);
	else g_bits[index >> 5].fetch_and(~bit, std::memory_order_relaxed);
}

void Publish() { g_hiding.store(!g_hidden.empty(), std::memory_order_release); }

bool StartsWithNoCase(const char* s, const std::string& prefix) {
	return s && _strnicmp(s, prefix.c_str(), prefix.size()) == 0;
}

bool Tracked(int index, CEntityInstance* ent) {
	for (const Hidden& h : g_hidden)
		if (h.index == index && h.ent == ent) return true;
	return false;
}

void Drop(size_t i) {
	SetBit(g_hidden[i].index, false);
	g_hidden.erase(g_hidden.begin() + long(i));
}

void Scan(bool showProps) {
	// Gone or replaced entities drop out first.
	for (size_t i = 0; i < g_hidden.size();)
		if (CosmeticEntFromIndex(g_hidden[i].index) != g_hidden[i].ent) Drop(i);
		else ++i;
	const std::string ownPackage = "maps/" + g_demoMap + "/";
	const int highest = GetHighestEntityIndex();
	int added = 0, addedBroken = 0;
	for (int i = 1; i <= highest && i < kMaxEntities; ++i) {
		CEntityInstance* ent = CosmeticEntFromIndex(i);
		if (!ent || Tracked(i, ent) || ent->IsPlayerPawn() || ent->IsPlayerController()) continue;
		char model[260] = {};
		CosmeticReadEntityModelPath(ent, model, sizeof(model));
		const bool broken = !g_demoMap.empty() && StartsWithNoCase(model, ownPackage);
		const bool prop = StartsWithNoCase(model, kPropModels);
		if (!broken && !(prop && !showProps)) continue;
		g_hidden.push_back({ i, ent, broken });
		SetBit(i, true);
		++added;
		if (broken) ++addedBroken;
		if (MvmDebugLog_Active() && (added <= 4 || (broken && addedBroken <= 64))) {
			float o[3] = {};
			ReadOrigin(ent, o);
			MvmDebugLog_LinefAlways("altenv.props", "hide index=%d class=%s client=%s model=%s at=(%.0f %.0f %.0f) %s", i, ent->GetClassName() ? ent->GetClassName() : "?",
				ent->GetClientClassName() ? ent->GetClientClassName() : "?", model, o[0], o[1], o[2], broken ? "broken" : "prop");
		}
	}
	Publish();
	const uint32_t mixed = g_mixedBatches.exchange(0);
	if ((added || mixed) && MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("altenv.props", "scan hidden=%zu (+%d, %d from %s's own package) show=%d mixed batches since last=%u", g_hidden.size(),
			added, addedBroken, g_demoMap.c_str(), showProps ? 1 : 0, mixed);
}

} // namespace

void Census() {
	std::map<std::string, std::pair<int, std::string>> byClass;
	const int highest = GetHighestEntityIndex();
	for (int i = 1; i <= highest; ++i) {
		CEntityInstance* ent = CosmeticEntFromIndex(i);
		if (!ent || ent->IsPlayerPawn() || ent->IsPlayerController()) continue;
		char model[260] = {};
		CosmeticReadEntityModelPath(ent, model, sizeof(model));
		const std::string key = std::string(ent->GetClassName() ? ent->GetClassName() : "?") + " / " + (ent->GetClientClassName() ? ent->GetClientClassName() : "?");
		auto& slot = byClass[key];
		if (!slot.first++ || (slot.second == "(no model)" && model[0])) slot.second = model[0] ? model : "(no model)";
	}
	MvmDebugLog_LinefAlways("altenv.props", "census: %zu entity classes", byClass.size());
	for (const auto& [key, v] : byClass)
		MvmDebugLog_LinefAlways("altenv.props", "census %4d  %s  e.g. %s", v.first, key.c_str(), v.second.c_str());
}

void LightCensus() {
	static const char* const kWords[] = { "light", "probe", "cubemap", "sky", "fog", "postprocess", "post_process", "tonemap", "exposure" };
	const int highest = GetHighestEntityIndex();
	int found = 0;
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ent = CosmeticEntFromIndex(i);
		if (!ent) continue;
		const char* cls = ent->GetClassName();
		const char* client = ent->GetClientClassName();
		std::string both = std::string(cls ? cls : "") + " " + (client ? client : "");
		for (char& c : both) c = char(tolower((unsigned char)c));
		bool match = false;
		for (const char* w : kWords) match = match || both.find(w) != std::string::npos;
		if (!match) continue;
		float o[3] = {};
		const bool at = ReadOrigin(ent, o);
		MvmDebugLog_LinefAlways("altenv.lights", "index=%d %s class=%s client=%s at=(%.0f %.0f %.0f)%s", i, i < 16384 ? "demo" : "client",
			cls && cls[0] ? cls : "-", client ? client : "?", o[0], o[1], o[2], at ? "" : " (no scene node)");
		++found;
	}
	MvmDebugLog_LinefAlways("altenv.lights", "census: %d lighting entities, highest index %d", found, highest);
}

void Update(bool active, bool showProps, const std::string& demoMap) {
	if (!active) {
		if (g_wasActive) RestoreAll();
		g_wasActive = false;
		return;
	}
	if (!g_wasActive || demoMap != g_demoMap) {
		RestoreAll();
		g_demoMap = demoMap;
		g_frame = kScanEvery; // scan now
	}
	// Shown again: the model props come back; the broken ones stay hidden.
	if (g_wasActive && showProps && !g_wasShow) {
		for (size_t i = 0; i < g_hidden.size();)
			if (!g_hidden[i].broken) Drop(i);
			else ++i;
		Publish();
		MvmDebugLog_LinefAlways("altenv.props", "props shown, %zu broken stay hidden", g_hidden.size());
	}
	if (g_wasActive && !showProps && g_wasShow) g_frame = kScanEvery;
	g_wasActive = true;
	g_wasShow = showProps;
	if (++g_frame >= kScanEvery) {
		g_frame = 0;
		Scan(showProps);
	}
}

void RestoreAll() {
	for (const Hidden& h : g_hidden) SetBit(h.index, false);
	if (!g_hidden.empty()) MvmDebugLog_LinefAlways("altenv.props", "shown again: %zu", g_hidden.size());
	g_hidden.clear();
	Publish();
}

void Forget() {
	RestoreAll(); // only bits: nothing of the old level's entities is touched
	g_wasActive = false;
}

size_t HiddenCount() { return g_hidden.size(); }

bool Hiding() { return g_hiding.load(std::memory_order_relaxed); }

int HiddenInBatch(const void* batch, int count, size_t stride, size_t objectOffset) {
	int hidden = 0;
	__try {
		for (int i = 0; i < count; ++i) {
			const unsigned char* object = *(const unsigned char* const*)((const unsigned char*)batch + size_t(i) * stride + objectOffset);
			if (!object) continue;
			const uint32_t owner = *(const uint32_t*)(object + kOwnerHandle);
			if (owner == 0xffffffffu) continue;
			const uint32_t index = owner & (kMaxEntities - 1);
			if (g_bits[index >> 5].load(std::memory_order_relaxed) & (1u << (index & 31))) ++hidden;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
	if (hidden && hidden < count) g_mixedBatches.fetch_add(1, std::memory_order_relaxed);
	return hidden;
}

int CompactBatch(void* batch, int count, size_t stride, size_t objectOffset, unsigned char* saved) {
	unsigned char* b = static_cast<unsigned char*>(batch);
	int kept = 0;
	volatile bool wrote = false; // the batch only needs putting back once something was moved
	__try {
		memcpy(saved, b, size_t(count) * stride);
		for (int i = 0; i < count; ++i) {
			const unsigned char* element = saved + size_t(i) * stride;
			const unsigned char* object = *(const unsigned char* const*)(element + objectOffset);
			if (object) {
				const uint32_t owner = *(const uint32_t*)(object + kOwnerHandle);
				const uint32_t index = owner & (kMaxEntities - 1);
				if (owner != 0xffffffffu && (g_bits[index >> 5].load(std::memory_order_relaxed) & (1u << (index & 31)))) continue;
			}
			if (kept != i) { wrote = true; memcpy(b + size_t(kept) * stride, element, stride); }
			++kept;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		if (wrote) RestoreBatch(batch, count, stride, saved);
		return count;
	}
	return kept;
}

void RestoreBatch(void* batch, int count, size_t stride, const unsigned char* saved) {
	__try {
		memcpy(batch, saved, size_t(count) * stride);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

} // namespace Filmmaker::AltEnvironment::Props
