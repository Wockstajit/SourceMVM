#include "CosmeticProfile.h"

#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"

#include <cstdlib>
#include <cstring>

namespace Filmmaker {

bool CosmeticProfile::Empty() const {
	if (knife.set || gloves.set || agent.set)
		return false;
	for (const auto& [def, item] : weapons)
		if (item.set)
			return false;
	return true;
}

CosmeticItem* CosmeticProfile::FindWeapon(int defIndex) {
	auto it = weapons.find(defIndex);
	return it != weapons.end() ? &it->second : nullptr;
}

const CosmeticItem* CosmeticProfile::FindWeapon(int defIndex) const {
	auto it = weapons.find(defIndex);
	return it != weapons.end() ? &it->second : nullptr;
}

namespace {

// Single-slot names as written in / read from the JSON document (per-weapon overrides live under the
// separate "weapons" object, keyed by def index -- see WriteWeapons/ReadWeapons).
constexpr const char* kSlotNames[] = { "knife", "gloves" };

CosmeticItem* SlotByName(CosmeticProfile& p, const char* name) {
	if (std::strcmp(name, "knife") == 0) return &p.knife;
	if (std::strcmp(name, "gloves") == 0) return &p.gloves;
	return nullptr;
}

// Writes the value fields of one item (no key, no enclosing object) -- the caller opens the object so
// the same body serves both the named single slots and the def-index-keyed weapons map.
void WriteItemFields(JsonBuilder& b, const CosmeticItem& item) {
	b.IntField("defIndex", item.defIndex);
	b.IntField("paintKit", item.paintKit);
	b.DoubleField("wear", item.wear);
	b.IntField("seed", item.seed);
	b.IntField("statTrak", item.statTrak);
}

void WriteItem(JsonBuilder& b, const char* slotName, const CosmeticItem& item) {
	if (!item.set)
		return;
	b.Key(slotName);
	b.BeginObject();
	WriteItemFields(b, item);
	b.EndObject();
}

// Object member lookup helper that tolerates a missing/non-object parent (Find() returning
// nullptr, or the node not being an Object) by returning nullptr instead of crashing.
const JsonValue* FindIn(const JsonValue* obj, const char* key) {
	if (!obj || obj->type != JsonValue::Type::Object)
		return nullptr;
	return obj->Find(key);
}

void ReadItem(const JsonValue* slotObj, CosmeticItem& item) {
	if (!slotObj || slotObj->type != JsonValue::Type::Object)
		return;
	item.set = true;
	const JsonValue* v;
	v = FindIn(slotObj, "defIndex");  item.defIndex = v ? v->AsInt(0) : 0;
	v = FindIn(slotObj, "paintKit");  item.paintKit = v ? v->AsInt(0) : 0;
	v = FindIn(slotObj, "wear");      item.wear = v ? (float)v->AsNumber(0.0) : 0.0f;
	v = FindIn(slotObj, "seed");      item.seed = v ? v->AsInt(0) : 0;
	v = FindIn(slotObj, "statTrak");  item.statTrak = v ? v->AsInt(-1) : -1;
}

} // namespace

CosmeticProfile& CosmeticProfileStore::GetOrCreate(uint64_t steamId) {
	return m_profiles[steamId];
}

CosmeticProfile* CosmeticProfileStore::Find(uint64_t steamId) {
	auto it = m_profiles.find(steamId);
	return it != m_profiles.end() ? &it->second : nullptr;
}

const CosmeticProfile* CosmeticProfileStore::Find(uint64_t steamId) const {
	auto it = m_profiles.find(steamId);
	return it != m_profiles.end() ? &it->second : nullptr;
}

void CosmeticProfileStore::ClearPlayer(uint64_t steamId) {
	m_profiles.erase(steamId);
}

void CosmeticProfileStore::ClearAll() {
	m_profiles.clear();
}

std::string CosmeticProfileStore::Serialize() const {
	bool any = false;
	for (const auto& [steamId, profile] : m_profiles)
		if (!profile.Empty()) { any = true; break; }
	if (!any)
		return std::string(); // nothing to keep: the caller removes the section
	JsonBuilder b;
	b.BeginObject();
	b.IntField("version", 1);
	b.Key("players");
	b.BeginObject();
	for (const auto& [steamId, profile] : m_profiles) {
		if (profile.Empty())
			continue;
		b.Key(std::to_string(steamId).c_str());
		b.BeginObject();
		if (!profile.name.empty())
			b.StringField("name", profile.name);
		// Per-weapon overrides: a "weapons" object keyed by weapon def index.
		bool anyWeapon = false;
		for (const auto& [def, item] : profile.weapons) { if (item.set) { anyWeapon = true; break; } }
		if (anyWeapon) {
			b.Key("weapons");
			b.BeginObject();
			for (const auto& [def, item] : profile.weapons) {
				if (!item.set)
					continue;
				b.Key(std::to_string(def).c_str());
				b.BeginObject();
				WriteItemFields(b, item);
				b.EndObject();
			}
			b.EndObject();
		}
		WriteItem(b, "knife", profile.knife);
		WriteItem(b, "gloves", profile.gloves);
		if (profile.agent.set) {
			b.Key("agent");
			b.BeginObject();
			b.IntField("defIndex", profile.agent.defIndex);
			b.StringField("model", profile.agent.model);
			b.EndObject();
		}
		b.EndObject();
	}
	b.EndObject(); // players
	b.EndObject(); // root
	return b.Str();
}

bool CosmeticProfileStore::Deserialize(const std::string& text) {
	ClearAll();

	JsonValue root;
	if (!JsonParse(text, root) || root.type != JsonValue::Type::Object)
		return false;

	const JsonValue* players = root.Find("players");
	if (players && players->type == JsonValue::Type::Object) {
		for (const auto& [key, value] : players->obj) {
			uint64_t steamId = std::strtoull(key.c_str(), nullptr, 10);
			if (steamId == 0)
				continue;
			if (value.type != JsonValue::Type::Object)
				continue;

			CosmeticProfile& p = m_profiles[steamId];
			if (const JsonValue* name = FindIn(&value, "name"))
				p.name = name->AsString("");

			for (const char* slotName : kSlotNames) {
				CosmeticItem* item = SlotByName(p, slotName);
				if (!item)
					continue;
				ReadItem(FindIn(&value, slotName), *item);
			}

			// Per-weapon overrides under "weapons" (keyed by def index string).
			if (const JsonValue* weapons = FindIn(&value, "weapons")) {
				if (weapons->type == JsonValue::Type::Object) {
					for (const auto& [wkey, wval] : weapons->obj) {
						int def = (int)std::strtol(wkey.c_str(), nullptr, 10);
						if (def <= 0 || wval.type != JsonValue::Type::Object)
							continue;
						CosmeticItem item;
						ReadItem(&wval, item);
						item.defIndex = def; // the map key is authoritative for the weapon identity
						if (item.set)
							p.weapons[def] = item;
					}
				}
			}

			if (const JsonValue* agentObj = FindIn(&value, "agent")) {
				if (agentObj->type == JsonValue::Type::Object) {
					p.agent.set = true;
					if (const JsonValue* defIndex = FindIn(agentObj, "defIndex"))
						p.agent.defIndex = defIndex->AsInt(0);
					if (const JsonValue* model = FindIn(agentObj, "model"))
						p.agent.model = model->AsString("");
				}
			}
			if (p.Empty())
				m_profiles.erase(steamId);
		}
	}

	return true;
}

} // namespace Filmmaker
