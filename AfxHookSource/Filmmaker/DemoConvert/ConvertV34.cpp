#include "ConvertV34.h"

#include "DemoBits.h"
#include "DemoTables.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <unordered_map>

namespace SourceFilmmaker {
namespace DemoConvert {

namespace {

const size_t kHeaderSize = 1072;
const size_t kCommandInfoSize = 76; // democmdinfo_t with one split-screen slot, the same in both
const int kOldTypeBits = 5;
const int kNewTypeBits = 6;
const int kNewProtocol = 24;
const int kMaxEdicts = 2048;
const int kEdictBits = 11;

enum Command { kSignon = 1, kPacket = 2, kSyncTick = 3, kConsoleCmd = 4, kUserCmd = 5, kDataTables = 6, kStop = 7, kStringTables = 8 };

// Server classes the current client has no receive table for. The engine refuses a demo whose
// tables it cannot match ("No matching RecvTable"), so these classes and their tables are dropped.
const char* const kDroppedClasses[][2] = { { "CWeaponCubemap", "DT_WeaponCubemap" } };

// Episode One arrays that Source 2013 sends as SendPropArray3 tables; the current client refuses
// the old form ("RecvProp type doesn't match server type for DT_Beam/m_hAttachEntity"). Measured by
// comparing every array in the v34 tables with the current game's.
const char* const kArraysNowTables[][2] = { { "DT_Beam", "m_hAttachEntity" }, { "DT_Beam", "m_nAttachIndex" } };

// Arrays the current client declares with a different element count; it refuses a count that
// differs from its own. Only counts whose length field keeps its width are listed, so the values
// stay as they are: 64 and 65 both send the count in 7 bits.
struct ArrayCount {
	const char* table;
	const char* prop;
	int elements;
};
const ArrayCount kArrayCounts[] = { { "DT_Team", "\"player_array\"", 65 } };

// Props Source 2013 renamed without changing their type or meaning. The client matches props by
// name, so under the old name the value is dropped: ropes lost their material (drawn as the error
// texture), shotguns their reload state. Found by comparing the v34 and current tables prop by prop;
// a rename is only listed when the type and encoding match.
const char* const kRenamedProps[][3] = {
	{ "DT_RopeKeyframe", "m_iRopeMaterialModel", "m_iRopeMaterialModelIndex" },
	{ "DT_BaseAnimating", "m_flModelWidthScale", "m_flModelScale" },
	{ "DT_RagdollManager", "m_iMaxRagdollCount", "m_iCurrentMaxRagdollCount" },
	{ "DT_WeaponM3", "m_fInSpecialReload", "m_reloadState" },
	{ "DT_WeaponXM1014", "m_fInSpecialReload", "m_reloadState" },
};

std::string WrittenName(const std::string& table, const std::string& prop) {
	for (const auto& r : kRenamedProps)
		if (table == r[0] && prop == r[1]) return r[2];
	return prop;
}

// Where an old flattened prop goes in the converted class: one new index, or for an array that
// became a table, one per element.
struct PropMove {
	int index = -1;
	std::vector<int> elements;
};

// v34 user message ids to the current client's. The current order is CS:S x64 client.dll's
// RegisterUserMessages; the v34 one comes from matching payloads in old demos against the Episode
// One SDK order. -1 drops a message the current client would misread.
int UserMessageMap(int old) {
	static const int kMap[] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, // Geiger .. VGUIMenu: same ids
		15,  // 14 CloseCaption (Rumble took 14)
		16,  // 15 SendAudio
		17,  // 16 RawAudio
		-1,  // 17 VoiceMask: sized for 32 players then, 64 now
		19,  // 18 RequestState
		20,  // 19 BarTime
		21,  // 20 Damage
		22,  // 21 RadioText
		23,  // 22 HintText (the old one leads with a count byte)
		25,  // 23 ReloadEffect (KeyHintText took 24)
		26,  // 24 PlayerAnimEvent
		27,  // 25 AmmoDenied
		28,  // 26 UpdateRadar
		29,  // 27 KillCam
	};
	return old >= 0 && old < (int)(sizeof(kMap) / sizeof(kMap[0])) ? kMap[old] : -1;
}
const int kOldHintText = 22;

// The tables the current client expects that the old server never created, with the current
// game's parameters. Appended after the demo's own tables, so the old table ids stay valid.
struct NewTable {
	const char* name;
	int maxEntries;
	bool fixed;
	int udSize, udBits;
};
const NewTable kNewTables[] = {
	{ "DynamicModels", 2048, true, 1, 1 },
	{ "ParticleEffectNames", 8192, false, 0, 0 },
	{ "ServerMapCycle", 128, false, 0, 0 },
};

// The brush model and placement of an entity, as far as matching it to the installed map needs.
struct EntityValues {
	int model = -1; // model precache index
	float origin[3] = { 0, 0, 0 };
	float mins[3] = { 0, 0, 0 };
	float maxs[3] = { 0, 0, 0 };
	bool hasMins = false, hasMaxs = false;
};

// Where those props sit in a class's flattened list.
struct Watch {
	int model = -1, origin = -1, mins = -1, maxs = -1;
};

struct EntityState {
	int cls = -1;        // old class id, -1 when the slot is empty
	EntityValues values;
	bool dropped = false; // its brush is gone from the installed map: nothing about it is written
};

struct StringTable {
	std::string name;
	int maxEntries = 0;
	bool fixed = false;
	int udSize = 0;
	int udBits = 0;
	std::vector<std::string> entries;
};

// SoundInfo_t (public/soundinfo.h): the fields svc_Sounds sends as deltas from the sound before.
struct Sound {
	int entity = 0;
	int soundNum = 0;
	int flags = 0;
	int channel = 6; // CHAN_STATIC
	bool ambient = false;
	bool sentence = false;
	uint32_t sequence = 0;
	int volume = 127;     // in the current engine's 7-bit steps (1.0 = 127)
	int soundLevel = 75;  // SNDLVL_NORM
	int pitch = 100;
	int specialDsp = 0;
	bool hasDelay = false;
	int delayRaw = 0;     // the 13-bit signed field as sent
	int origin[3] = { 0, 0, 0 }; // world units
	int speaker = -1;
};
const int kSoundStop = 4; // SND_STOP

void PutInt32(std::vector<unsigned char>& out, int32_t v) {
	unsigned char b[4];
	std::memcpy(b, &v, 4);
	out.insert(out.end(), b, b + 4);
}

int32_t GetInt32(const unsigned char* p) {
	int32_t v;
	std::memcpy(&v, p, 4);
	return v;
}

class Converter {
public:
	Converter(const unsigned char* data, size_t size, std::vector<unsigned char>& out) : m_data(data), m_size(size), m_out(out) {}

	// First pass: record each inline model's first entity (into uses, by model number).
	void Observe(std::vector<BrushUse>* uses) { m_uses = uses; }
	// Second pass: what each model precache entry is written as ("" = as recorded), and which
	// entries' entities are left out.
	void Rewrite(std::vector<std::string> models, std::vector<char> drop) {
		m_modelRewrite = std::move(models);
		m_dropModel = std::move(drop);
	}
	// The model precache as the demo recorded it, by index (after Run).
	std::vector<std::string> ModelPrecache() const {
		for (const StringTable& table : m_stringTables)
			if (table.name == "modelprecache") return table.entries;
		return std::vector<std::string>();
	}

	ConvertResult Run();

private:
	bool Fail(const char* format, ...) {
		char text[256];
		va_list args;
		va_start(args, format);
		std::vsnprintf(text, sizeof(text), format, args);
		va_end(args);
		if (m_error.empty()) m_error = text;
		return false;
	}

	bool LoadTables();
	bool ConvertPacket(const unsigned char* data, size_t size, BitWriter& w);
	bool ConvertMessage(int type, BitReader& b, BitWriter& w);
	bool ConvertPropList(BitReader& b, BitWriter& w, int oldClass, EntityValues* values = nullptr);
	void NoteBrushUse(const EntityValues& values);
	const std::string* ModelName(int precacheIndex) const;
	bool ConvertEntities(BitReader& b, BitWriter& w, int updated, bool delta);
	bool ConvertTempEntities(BitReader& b, BitWriter& w, int count);
	bool ConvertEntries(BitReader& b, BitWriter& w, StringTable& table, int count);
	bool ConvertSounds(BitReader& b, BitWriter& w, int count);
	void AppendNewTables(BitWriter& w);
	void WriteFrame(int command, int32_t tick, const unsigned char* info, const unsigned char* payload, size_t length);

	const unsigned char* m_data;
	size_t m_size;
	std::vector<unsigned char>& m_out;
	std::string m_error;
	ConvertStats m_stats;

	DataTables m_oldTables;                          // as recorded: values are read with these
	DataTables m_tables;                             // as written
	std::vector<int> m_classMap;                     // old class id -> new, -1 dropped
	std::vector<std::vector<FlatProp>> m_flat;       // by old class id, in the old order
	std::vector<std::vector<PropMove>> m_moves;      // by old class id; empty when the order is unchanged
	int m_oldClassBits = 0;
	int m_newClassBits = 0;
	std::vector<StringTable> m_stringTables;
	bool m_newTablesAdded = false;
	EntityState m_entities[kMaxEdicts];
	std::vector<Watch> m_watch;              // by old class id
	std::vector<EntityValues> m_classBaseline; // by old class id
	float m_tickInterval = 0.015f;
	std::vector<BrushUse>* m_uses = nullptr;  // first pass
	std::vector<std::string> m_modelRewrite;  // second pass, by model precache index
	std::vector<char> m_dropModel;            // second pass, by model precache index
	int m_writtenEntities = 0;                // the last packet's entity count after leaving some out
};

bool Converter::LoadTables() {
	// The data tables frame comes after the first signon packet, whose instance baselines need
	// them, so it is found and read before anything is converted.
	size_t at = kHeaderSize;
	while (at + 5 <= m_size) {
		const int command = m_data[at];
		at += 5;
		size_t skip = 0;
		if (command == kSignon || command == kPacket) skip = kCommandInfoSize + 8;
		else if (command == kUserCmd) skip = 4;
		else if (command == kSyncTick) continue;
		else if (command != kConsoleCmd && command != kDataTables && command != kStringTables) break;
		at += skip;
		if (at + 4 > m_size) break;
		const int32_t length = GetInt32(m_data + at);
		at += 4;
		if (length < 0 || at + (size_t)length > m_size) break;
		if (command == kDataTables) {
			BitReader in(m_data + at, (size_t)length);
			std::string error;
			DataTables old;
			if (!ReadDataTables(in, 7, old, error)) return Fail("data tables: %s", error.c_str());
			m_stats.classesIn = (int)old.classes.size();
			m_oldTables = old;
			// Drop what the current client cannot match, renumber the rest, and clear SPROP_NORMAL on
			// integers: Episode One ignored it there, Source 2013 reads it as SPROP_VARINT.
			for (const SendTable& table : old.tables) {
				bool dropped = false;
				for (const auto& d : kDroppedClasses) dropped |= table.name == d[1];
				if (!dropped) m_tables.tables.push_back(table);
			}
			for (SendTable& table : m_tables.tables) {
				for (SendProp& p : table.props) {
					if (p.type == PropType::Int && (p.flags & kPropNormal)) {
						p.flags &= ~kPropNormal;
						++m_stats.intNormalCleared;
					}
				}
			}
			for (const auto& a : kArraysNowTables) ArrayToTable(m_tables, a[0], a[1]);
			for (SendTable& table : m_tables.tables)
				for (SendProp& p : table.props) p.name = WrittenName(table.name, p.name);
			for (const ArrayCount& a : kArrayCounts) {
				for (SendTable& table : m_tables.tables) {
					if (table.name != a.table) continue;
					for (SendProp& p : table.props)
						if (p.name == a.prop && p.type == PropType::Array && Log2((unsigned)p.elements) == Log2((unsigned)a.elements))
							p.elements = a.elements;
				}
			}
			m_classMap.assign(old.classes.size(), -1);
			int next = 0;
			for (const ServerClass& c : old.classes) {
				bool dropped = false;
				for (const auto& d : kDroppedClasses) dropped |= c.name == d[0];
				if (c.id < 0 || c.id >= (int)old.classes.size()) return Fail("class id %d out of range", c.id);
				if (dropped || !m_tables.Find(c.table)) {
					if (!m_stats.droppedClasses.empty()) m_stats.droppedClasses += ", ";
					m_stats.droppedClasses += c.name;
					continue;
				}
				m_classMap[c.id] = next;
				ServerClass kept = c;
				kept.id = next++;
				m_tables.classes.push_back(kept);
			}
			m_stats.classesOut = next;
			m_oldClassBits = Log2((unsigned)old.classes.size()) + 1;
			m_newClassBits = Log2((unsigned)next) + 1;
			// Flatten against the kept tables (the dropped ones are not referenced by kept classes).
			m_flat.assign(old.classes.size(), std::vector<FlatProp>());
			m_moves.assign(old.classes.size(), std::vector<PropMove>());
			m_watch.assign(old.classes.size(), Watch());
			m_classBaseline.assign(old.classes.size(), EntityValues());
			for (const ServerClass& c : old.classes) {
				if (m_classMap[c.id] < 0) continue;
				m_flat[c.id] = m_oldTables.Flatten(c.table);
				// The written order: the same props, except where an array became a table.
				const std::vector<FlatProp> written = m_tables.Flatten(c.table);
				std::unordered_map<std::string, int> where;
				for (int i = 0; i < (int)written.size(); ++i) where[*written[i].table + "/" + written[i].prop->name] = i;
				std::vector<PropMove> moves(m_flat[c.id].size());
				bool same = written.size() == m_flat[c.id].size();
				for (size_t i = 0; i < m_flat[c.id].size(); ++i) {
					const FlatProp& f = m_flat[c.id][i];
					auto it = where.find(*f.table + "/" + WrittenName(*f.table, f.prop->name));
					if (it != where.end()) {
						moves[i].index = it->second;
					} else if (f.prop->type == PropType::Array) {
						for (int k = 0; k < f.prop->elements; ++k) {
							char name[8];
							std::snprintf(name, sizeof(name), "%03d", k);
							auto e = where.find(f.prop->name + "/" + name);
							moves[i].elements.push_back(e != where.end() ? e->second : -1);
						}
					}
					same &= moves[i].index == (int)i;
				}
				if (!same) m_moves[c.id] = std::move(moves);
				Watch& watch = m_watch[c.id];
				const std::vector<FlatProp>& flat = m_flat[c.id];
				for (int i = 0; i < (int)flat.size(); ++i) {
					const std::string& name = flat[i].prop->name;
					if (name == "m_nModelIndex" && watch.model < 0) watch.model = i;
					else if (name == "m_vecOrigin" && watch.origin < 0) watch.origin = i;
					else if (name == "m_vecMins" && watch.mins < 0) watch.mins = i;
					else if (name == "m_vecMaxs" && watch.maxs < 0) watch.maxs = i;
				}
			}
			return true;
		}
		at += (size_t)length;
	}
	return Fail("no data tables frame");
}

void Converter::WriteFrame(int command, int32_t tick, const unsigned char* info, const unsigned char* payload, size_t length) {
	m_out.push_back((unsigned char)command);
	PutInt32(m_out, tick);
	if (command == kSignon || command == kPacket) m_out.insert(m_out.end(), info, info + kCommandInfoSize + 8);
	if (command == kSignon || command == kPacket || command == kConsoleCmd || command == kDataTables || command == kStringTables) {
		PutInt32(m_out, (int32_t)length);
		if (length) m_out.insert(m_out.end(), payload, payload + length);
	}
	++m_stats.framesOut;
}

const std::string* Converter::ModelName(int precacheIndex) const {
	for (const StringTable& table : m_stringTables)
		if (table.name == "modelprecache")
			return precacheIndex >= 0 && precacheIndex < (int)table.entries.size() ? &table.entries[(size_t)precacheIndex] : nullptr;
	return nullptr;
}

// First pass: the first entity seen with inline model "*N" tells where that brush was.
void Converter::NoteBrushUse(const EntityValues& values) {
	if (!m_uses || values.model < 0 || !values.hasMins || !values.hasMaxs) return;
	const std::string* name = ModelName(values.model);
	if (!name || name->size() < 2 || (*name)[0] != '*') return;
	const int n = std::atoi(name->c_str() + 1);
	if (n <= 0 || n > 4096) return;
	if ((int)m_uses->size() <= n) m_uses->resize((size_t)n + 1);
	BrushUse& use = (*m_uses)[(size_t)n];
	if (use.seen) return;
	use.number = n;
	use.seen = true;
	for (int i = 0; i < 3; ++i) {
		use.origin[i] = values.origin[i];
		use.mins[i] = values.mins[i];
		use.maxs[i] = values.maxs[i];
	}
}

bool Converter::ConvertPropList(BitReader& b, BitWriter& w, int oldClass, EntityValues* values) {
	if (oldClass < 0 || oldClass >= (int)m_flat.size() || m_classMap[oldClass] < 0) return Fail("entity of unknown class %d", oldClass);
	const std::vector<FlatProp>& flat = m_flat[oldClass];
	const Watch& watch = m_watch[oldClass];
	const std::vector<PropMove>& moves = m_moves[oldClass];
	struct Span { int index; size_t from, to; };
	std::vector<Span> spans; // only when the written order differs
	int index = -1;
	while (b.Bit()) {
		const uint32_t step = b.UBitVarEp1();
		index += (int)step + 1;
		if (index >= (int)flat.size() || b.Overrun()) return Fail("property %d of %d in class %d", index, (int)flat.size(), oldClass);
		const size_t start = b.Position();
		if (!moves.empty()) {
			const PropMove& move = moves[(size_t)index];
			if (!move.elements.empty()) {
				// An array that became a table: each element is its own prop now.
				const int count = (int)b.Bits(Log2((unsigned)flat[index].prop->elements) + 1);
				FlatProp element;
				element.prop = flat[index].element;
				for (int k = 0; k < count && !b.Overrun(); ++k) {
					const size_t from = b.Position();
					if (!element.prop || !SkipPropValue(b, element, 7)) return Fail("bad array element in %s", flat[index].prop->name.c_str());
					if (k < (int)move.elements.size() && move.elements[(size_t)k] >= 0) spans.push_back({ move.elements[(size_t)k], from, b.Position() });
				}
			} else {
				if (!SkipPropValue(b, flat[index], 7)) return Fail("bad value for %s", flat[index].prop->name.c_str());
				if (move.index >= 0) spans.push_back({ move.index, start, b.Position() });
			}
		} else {
			w.Bit(true);
			w.UBitVar2013(step);
			if (!SkipPropValue(b, flat[index], 7)) return Fail("bad value for %s", flat[index].prop->name.c_str());
			w.Copy(b, start, b.Position());
		}
		++m_stats.props;
		if (values && (index == watch.model || index == watch.origin || index == watch.mins || index == watch.maxs)) {
			BitReader value(b.Data(), start, b.Position());
			float numbers[3] = { 0, 0, 0 };
			int count = 0;
			if (ReadPropNumbers(value, flat[index], numbers, count)) {
				if (index == watch.model) values->model = (int)numbers[0];
				float* target = index == watch.origin ? values->origin : (index == watch.mins ? values->mins : (index == watch.maxs ? values->maxs : nullptr));
				if (target && count == 3)
					for (int i = 0; i < 3; ++i) target[i] = numbers[i];
				if (index == watch.mins) values->hasMins = true;
				if (index == watch.maxs) values->hasMaxs = true;
			}
		}
	}
	if (!moves.empty()) {
		std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.index < b.index; });
		int last = -1;
		for (const Span& s : spans) {
			if (s.index <= last) continue; // the same prop twice: keep the first
			w.Bit(true);
			w.UBitVar2013((uint32_t)(s.index - last - 1));
			w.Copy(b, s.from, s.to);
			last = s.index;
		}
	}
	w.Bit(false);
	return !b.Overrun();
}

bool Converter::ConvertEntities(BitReader& b, BitWriter& w, int updated, bool delta) {
	if (!delta)
		for (EntityState& e : m_entities) e = EntityState();
	// Entities whose brush the installed map no longer has are left out, so the index deltas are
	// counted against the entities actually written, and the count is the number written.
	BitWriter body;
	int index = -1, lastWritten = -1, written = 0;
	for (int i = 0; i < updated; ++i) {
		const uint32_t step = b.UBitVarEp1();
		index += (int)step + 1;
		if (index >= kMaxEdicts || b.Overrun()) return Fail("entity index %d", index);
		EntityState& entity = m_entities[index];
		BitWriter one;
		bool keep = true;
		const bool leave = b.Bit();
		one.Bit(leave);
		if (!leave) {
			const bool enter = b.Bit();
			one.Bit(enter);
			if (enter) {
				const int oldClass = (int)b.Bits(m_oldClassBits);
				const uint32_t serial = b.Bits(10);
				if (oldClass >= (int)m_classMap.size() || m_classMap[oldClass] < 0) return Fail("entity %d of dropped or unknown class %d", index, oldClass);
				one.Bits((uint32_t)m_classMap[oldClass], m_newClassBits);
				one.Bits(serial, 10);
				entity = EntityState();
				entity.cls = oldClass;
				entity.values = m_classBaseline[oldClass];
			} else if (entity.cls < 0) {
				return Fail("update for entity %d that does not exist", index);
			}
			if (!ConvertPropList(b, one, entity.cls, &entity.values)) return false;
			if (enter) {
				NoteBrushUse(entity.values);
				const int model = entity.values.model;
				entity.dropped = model >= 0 && model < (int)m_dropModel.size() && m_dropModel[(size_t)model];
			}
			keep = !entity.dropped;
			++m_stats.entityUpdates;
		} else {
			const bool remove = b.Bit();
			one.Bit(remove);
			keep = !entity.dropped;
			if (remove) entity = EntityState();
		}
		if (!keep) {
			++m_stats.entitiesDropped;
			continue;
		}
		body.UBitVar2013((uint32_t)(index - lastWritten - 1));
		body.Append(one);
		lastWritten = index;
		++written;
	}
	if (delta) {
		while (b.Bit()) {
			const uint32_t removed = b.Bits(kEdictBits);
			const bool dropped = removed < (uint32_t)kMaxEdicts && m_entities[removed].dropped;
			if (removed < (uint32_t)kMaxEdicts) m_entities[removed] = EntityState();
			if (dropped) continue;
			body.Bit(true);
			body.Bits(removed, kEdictBits);
		}
		body.Bit(false);
	}
	if (b.Overrun()) return Fail("packet entities run past their length");
	if (b.Left() != 0) return Fail("packet entities leave %d bits", (int)b.Left());
	m_writtenEntities = written;
	w.Append(body);
	return true;
}

bool Converter::ConvertTempEntities(BitReader& b, BitWriter& w, int count) {
	int oldClass = -1;
	for (int i = 0; i < count; ++i) {
		const bool hasDelay = b.Bit();
		w.Bit(hasDelay);
		if (hasDelay) w.Bits(b.Bits(8), 8);
		const bool hasClass = b.Bit();
		w.Bit(hasClass);
		if (hasClass) {
			oldClass = (int)b.Bits(m_oldClassBits) - 1; // sent as id + 1
			if (oldClass < 0 || oldClass >= (int)m_classMap.size() || m_classMap[oldClass] < 0)
				return Fail("temp entity of dropped or unknown class %d", oldClass);
			w.Bits((uint32_t)m_classMap[oldClass] + 1, m_newClassBits);
		}
		if (oldClass < 0) return Fail("temp entity without a class");
		if (!ConvertPropList(b, w, oldClass)) return false;
		++m_stats.tempEntities;
	}
	if (b.Left() != 0 || b.Overrun()) return Fail("temp entities leave %d bits", (int)b.Left());
	return true;
}

bool Converter::ConvertEntries(BitReader& b, BitWriter& w, StringTable& table, int count) {
	const int entryBits = Log2((unsigned)table.maxEntries);
	const bool baselines = table.name == "instancebaseline";
	const bool models = table.name == "modelprecache";
	std::deque<std::string> history;
	int last = -1;
	for (int i = 0; i < count; ++i) {
		int index = last + 1;
		const bool next = b.Bit();
		if (!next) index = (int)b.Bits(entryBits);
		last = index;
		if (index < 0 || index >= table.maxEntries || b.Overrun()) return Fail("%s: entry %d of %d", table.name.c_str(), index, table.maxEntries);
		w.Bit(next);
		if (!next) w.Bits((uint32_t)index, entryBits);

		std::string& entry = table.entries[index];
		const bool hasString = b.Bit();
		if (hasString) {
			if (b.Bit()) {
				const uint32_t from = b.Bits(5), bytes = b.Bits(5);
				if (from >= history.size()) return Fail("%s: history reference %u of %u", table.name.c_str(), from, (unsigned)history.size());
				entry = history[from].substr(0, bytes) + b.String();
			} else {
				entry = b.String();
			}
		}
		// Instance baselines are named by class id, which the dropped classes renumber.
		std::string outName = entry;
		int oldClass = -1;
		if (baselines) {
			oldClass = std::atoi(entry.c_str());
			outName = oldClass >= 0 && oldClass < (int)m_classMap.size() && m_classMap[oldClass] >= 0
				? std::to_string(m_classMap[oldClass]) : std::string("dropped");
		}
		if (models && index < (int)m_modelRewrite.size() && !m_modelRewrite[(size_t)index].empty())
			outName = m_modelRewrite[(size_t)index];
		// Whole strings only: the history the old demo's substrings point into is the old names.
		w.Bit(hasString);
		if (hasString) {
			w.Bit(false);
			w.String(outName);
		}

		const bool hasData = b.Bit();
		w.Bit(hasData);
		if (hasData) {
			if (table.fixed) {
				w.Bits(b.Bits(table.udBits), table.udBits);
			} else {
				const uint32_t bytes = b.Bits(12); // 14 bits in the current engine
				BitReader data = b.Sub((size_t)bytes * 8);
				if (b.Overrun()) return Fail("%s: entry data runs past its message", table.name.c_str());
				if (baselines && oldClass >= 0 && oldClass < (int)m_classMap.size() && m_classMap[oldClass] >= 0) {
					BitWriter list;
					m_classBaseline[(size_t)oldClass] = EntityValues();
					if (!ConvertPropList(data, list, oldClass, &m_classBaseline[(size_t)oldClass])) return Fail("baseline for class %d", oldClass);
					list.PadToByte();
					w.Bits((uint32_t)list.Bytes(), 14);
					w.Append(list);
					++m_stats.baselines;
				} else {
					w.Bits(bytes, 14);
					w.Copy(data, data.Position(), data.End());
				}
			}
		}
		if (history.size() == 32) history.pop_front();
		history.push_back(entry);
	}
	if (b.Overrun()) return Fail("%s: entries run past their message", table.name.c_str());
	return true;
}

bool Converter::ConvertSounds(BitReader& b, BitWriter& w, int count) {
	// Read with Episode One's SoundInfo_t::ReadDelta, write with Source 2013's WriteDelta. Both are
	// deltas against the sound before, starting from the defaults, so each side keeps its own.
	Sound in, out;
	for (int i = 0; i < count; ++i) {
		if (b.Bit()) in.entity = (int)b.Bits(11);
		if (b.Bit()) in.soundNum = (int)b.Bits(13);
		if (b.Bit()) in.flags = (int)b.Bits(8);
		if (b.Bit()) in.channel = (int)b.Bits(3);
		in.ambient = b.Bit();
		in.sentence = b.Bit();
		in.hasDelay = false;
		if (in.flags != kSoundStop) {
			if (!b.Bit()) {
				if (b.Bit()) ++in.sequence;
				else in.sequence = b.Bits(31);
			}
			if (b.Bit()) in.volume = (int)((b.Bits(8) / 255.0f) * 127.0f);
			if (b.Bit()) in.soundLevel = (int)b.Bits(9);
			if (b.Bit()) in.pitch = (int)b.Bits(8);
			if (b.Bit()) { in.hasDelay = true; in.delayRaw = b.Signed(13); }
			for (int& c : in.origin)
				if (b.Bit()) c = b.Signed(15);
			if (b.Bit()) in.speaker = b.Signed(12);
		}
		if (b.Overrun()) return Fail("sound %d of %d runs past its message", i, count);

		auto uintDelta = [&](int value, int& previous, int bits) {
			if (value == previous) { w.Bit(false); return; }
			w.Bit(true);
			w.Bits((uint32_t)value, bits);
			previous = value;
		};
		if (in.entity == out.entity) {
			w.Bit(false);
		} else {
			w.Bit(true);
			if (in.entity <= 31) { w.Bit(true); w.Bits((uint32_t)in.entity, 5); }
			else { w.Bit(false); w.Bits((uint32_t)in.entity, kEdictBits); }
			out.entity = in.entity;
		}
		uintDelta(in.soundNum, out.soundNum, 14);
		uintDelta(in.flags, out.flags, 11);
		uintDelta(in.channel, out.channel, 3);
		w.Bit(in.ambient);
		w.Bit(in.sentence);
		if (in.flags != kSoundStop) {
			const uint32_t sequence = in.sequence & 1023; // SOUND_SEQNUMBER_BITS 10
			if (sequence == out.sequence) w.Bit(true);
			else if (sequence == ((out.sequence + 1) & 1023)) { w.Bit(false); w.Bit(true); }
			else { w.Bits(0, 2); w.Bits(sequence, 10); }
			out.sequence = sequence;
			uintDelta(in.volume, out.volume, 7);
			uintDelta(in.soundLevel, out.soundLevel, 9);
			uintDelta(in.pitch, out.pitch, 8);
			uintDelta(in.specialDsp, out.specialDsp, 8);
			if (in.hasDelay) { w.Bit(true); w.Signed(in.delayRaw, 13); }
			else w.Bit(false);
			for (int axis = 0; axis < 3; ++axis) {
				// The current engine sends origins in 8-unit steps in 12 signed bits.
				int scaled = (int)std::lround(in.origin[axis] / 8.0);
				if (scaled > 2047) scaled = 2047;
				if (scaled < -2048) scaled = -2048;
				if (scaled == out.origin[axis]) { w.Bit(false); continue; }
				w.Bit(true);
				w.Signed(scaled, 12);
				out.origin[axis] = scaled;
			}
			if (in.speaker == out.speaker) w.Bit(false);
			else { w.Bit(true); w.Signed(in.speaker, 12); out.speaker = in.speaker; }
		}
		++m_stats.sounds;
	}
	if (b.Left() != 0) return Fail("sounds leave %d bits", (int)b.Left());
	return true;
}

void Converter::AppendNewTables(BitWriter& w) {
	for (const NewTable& t : kNewTables) {
		w.Bits(12, kNewTypeBits);
		w.String(t.name);
		w.Bits((uint32_t)t.maxEntries, 16);
		w.Bits(0, Log2((unsigned)t.maxEntries) + 1); // no entries
		w.VarInt(0);
		w.Bit(t.fixed);
		if (t.fixed) { w.Bits((uint32_t)t.udSize, 12); w.Bits((uint32_t)t.udBits, 4); }
		w.Bit(false); // not compressed
		StringTable table;
		table.name = t.name;
		table.maxEntries = t.maxEntries;
		table.fixed = t.fixed;
		table.udSize = t.udSize;
		table.udBits = t.udBits;
		table.entries.assign((size_t)t.maxEntries, std::string());
		m_stringTables.push_back(std::move(table));
	}
	m_newTablesAdded = true;
}

bool Converter::ConvertMessage(int type, BitReader& b, BitWriter& w) {
	// Copies the rest of a message whose fields are the same in both engines: read past them,
	// then copy that span after the new type.
	const size_t start = b.Position();
	auto copySame = [&]() {
		if (b.Overrun()) return false;
		w.Bits((uint32_t)type, kNewTypeBits);
		w.Copy(b, start, b.Position());
		return true;
	};
	switch (type) {
	case 0: return true; // net_NOP: padding, not needed
	case 1: b.String(); return copySame();
	case 2: b.Bits(32); b.String(); b.Bit(); return copySame();
	case 3: { // net_Tick: the current engine adds the host frame time and its deviation
		const int32_t tick = b.Signed(32);
		w.Bits(3, kNewTypeBits);
		w.Signed(tick, 32);
		w.Bits((uint32_t)std::lround(m_tickInterval * 100000.0f), 16); // NET_TICK_SCALEUP
		w.Bits(0, 16);
		return !b.Overrun();
	}
	case 4: b.String(); return copySame();
	case 5: {
		const int n = (int)b.Bits(8);
		for (int i = 0; i < n && !b.Overrun(); ++i) { b.String(); b.String(); }
		return copySame();
	}
	case 6: b.Bits(8); b.Bits(32); return copySame();
	case 7: b.String(); return copySame();
	case 8: { // svc_ServerInfo: map CRC becomes an MD5, a replay flag is added
		const uint32_t serverCount = (b.Bits(16), b.Bits(32));
		const bool hltv = b.Bit(), dedicated = b.Bit();
		const uint32_t clientCrc = b.Bits(32);
		b.Bits(16); // max classes, replaced by the kept class count
		b.Bits(32); // map CRC
		const uint32_t slot = b.Bits(8), maxClients = b.Bits(8);
		m_tickInterval = b.Float();
		const uint32_t os = b.Bits(8);
		const std::string gameDir = b.String(), map = b.String(), sky = b.String(), host = b.String();
		if (b.Overrun()) return false;
		w.Bits(8, kNewTypeBits);
		w.Bits(kNewProtocol, 16);
		w.Bits(serverCount, 32);
		w.Bit(hltv);
		w.Bit(dedicated);
		w.Bits(clientCrc, 32);
		w.Bits((uint32_t)m_stats.classesOut, 16);
		for (int i = 0; i < 16; ++i) w.Bits(0, 8); // demo playback does not check the map's MD5
		w.Bits(slot, 8);
		w.Bits(maxClients, 8);
		w.Float(m_tickInterval);
		w.Bits(os, 8);
		w.String(gameDir);
		w.String(map);
		w.String(sky);
		w.String(host);
		w.Bit(false); // not a replay
		return true;
	}
	case 10: { // svc_ClassInfo
		const uint32_t count = b.Bits(16);
		const bool create = b.Bit();
		w.Bits(10, kNewTypeBits);
		w.Bits((uint32_t)m_stats.classesOut, 16);
		w.Bit(create);
		if (!create) {
			const int oldBits = Log2(count) + 1, newBits = Log2((unsigned)m_stats.classesOut) + 1;
			for (uint32_t i = 0; i < count && !b.Overrun(); ++i) {
				const int id = (int)b.Bits(oldBits);
				const std::string name = b.String(), table = b.String();
				if (id < (int)m_classMap.size() && m_classMap[id] >= 0) {
					w.Bits((uint32_t)m_classMap[id], newBits);
					w.String(name);
					w.String(table);
				}
			}
		}
		return !b.Overrun();
	}
	case 11: b.Bit(); return copySame();
	case 12: { // svc_CreateStringTable: varint length and a compression flag; entry data lengths widen
		StringTable table;
		table.name = b.String();
		table.maxEntries = (int)b.Bits(16);
		const int count = (int)b.Bits(Log2((unsigned)table.maxEntries) + 1);
		const uint32_t length = b.Bits(20);
		table.fixed = b.Bit();
		if (table.fixed) { table.udSize = (int)b.Bits(12); table.udBits = (int)b.Bits(4); }
		BitReader body = b.Sub(length);
		if (b.Overrun() || table.maxEntries <= 0) return Fail("CreateStringTable %s", table.name.c_str());
		table.entries.assign((size_t)table.maxEntries, std::string());
		BitWriter entries;
		if (!ConvertEntries(body, entries, table, count)) return false;
		if (body.Left() != 0) return Fail("%s: %d bits left after its entries", table.name.c_str(), (int)body.Left());
		w.Bits(12, kNewTypeBits);
		w.String(table.name);
		w.Bits((uint32_t)table.maxEntries, 16);
		w.Bits((uint32_t)count, Log2((unsigned)table.maxEntries) + 1);
		w.VarInt((uint32_t)entries.Size());
		w.Bit(table.fixed);
		if (table.fixed) { w.Bits((uint32_t)table.udSize, 12); w.Bits((uint32_t)table.udBits, 4); }
		w.Bit(false);
		w.Append(entries);
		m_stringTables.push_back(std::move(table));
		return true;
	}
	case 13: { // svc_UpdateStringTable: 16-bit length becomes 20
		const int id = (int)b.Bits(5);
		const bool multiple = b.Bit();
		const int count = multiple ? (int)b.Bits(16) : 1;
		BitReader body = b.Sub(b.Bits(16));
		if (b.Overrun() || id >= (int)m_stringTables.size()) return Fail("UpdateStringTable %d", id);
		BitWriter entries;
		if (!ConvertEntries(body, entries, m_stringTables[(size_t)id], count)) return false;
		w.Bits(13, kNewTypeBits);
		w.Bits((uint32_t)id, 5);
		w.Bit(multiple);
		if (multiple) w.Bits((uint32_t)count, 16);
		w.Bits((uint32_t)entries.Size(), 20);
		w.Append(entries);
		return true;
	}
	case 14: // svc_VoiceInit: Speex, which the 64-bit game no longer ships
		b.String();
		b.Bits(8);
		++m_stats.voiceDropped;
		return !b.Overrun();
	case 15: // svc_VoiceData
		b.Bits(8);
		b.Skip(b.Bits(16));
		++m_stats.voiceDropped;
		return !b.Overrun();
	case 17: { // svc_Sounds
		const bool reliable = b.Bit();
		int count = 1;
		uint32_t length;
		if (reliable) length = b.Bits(8);
		else { count = (int)b.Bits(8); length = b.Bits(16); }
		BitReader body = b.Sub(length);
		if (b.Overrun()) return false;
		BitWriter sounds;
		if (!ConvertSounds(body, sounds, count)) return false;
		w.Bits(17, kNewTypeBits);
		if (reliable && sounds.Size() < 256) {
			w.Bit(true);
			w.Bits((uint32_t)sounds.Size(), 8);
		} else {
			w.Bit(false);
			w.Bits((uint32_t)count, 8);
			w.Bits((uint32_t)sounds.Size(), 16);
		}
		w.Append(sounds);
		return true;
	}
	case 18: b.Bits(kEdictBits); return copySame();
	case 19: b.Bit(); b.Bits(16); b.Bits(16); b.Bits(16); return copySame();
	case 20: b.Bits(16); b.Bits(16); b.Bits(16); return copySame();
	case 21: { // svc_BSPDecal: the model index widens from 11 to 13 bits
		const bool x = b.Bit(), y = b.Bit(), z = b.Bit();
		const size_t coords = b.Position();
		if (x) b.SkipCoord();
		if (y) b.SkipCoord();
		if (z) b.SkipCoord();
		const size_t coordsEnd = b.Position();
		const uint32_t decal = b.Bits(9);
		const bool hasEntity = b.Bit();
		uint32_t entity = 0, model = 0;
		if (hasEntity) { entity = b.Bits(kEdictBits); model = b.Bits(11); }
		const bool lowPriority = b.Bit();
		if (b.Overrun()) return false;
		w.Bits(21, kNewTypeBits);
		w.Bit(x); w.Bit(y); w.Bit(z);
		w.Copy(b, coords, coordsEnd);
		w.Bits(decal, 9);
		w.Bit(hasEntity);
		if (hasEntity) { w.Bits(entity, kEdictBits); w.Bits(model, 13); }
		w.Bit(lowPriority);
		return true;
	}
	case 23: { // svc_UserMessage: renumbered
		const int old = (int)b.Bits(8);
		BitReader body = b.Sub(b.Bits(11));
		if (b.Overrun()) return false;
		const int id = UserMessageMap(old);
		++m_stats.userMessages;
		if (id < 0) {
			++m_stats.userMessagesDropped;
			return true;
		}
		if (old == kOldHintText) body.Bits(8); // the old HintText leads with a count byte
		w.Bits(23, kNewTypeBits);
		w.Bits((uint32_t)id, 8);
		w.Bits((uint32_t)body.Left(), 11);
		w.Copy(body, body.Position(), body.End());
		return true;
	}
	case 24: { // svc_EntityMessage: the class id is renumbered
		const uint32_t entity = b.Bits(kEdictBits);
		const int oldClass = (int)b.Bits(9);
		BitReader body = b.Sub(b.Bits(11));
		if (b.Overrun()) return false;
		if (oldClass >= (int)m_classMap.size() || m_classMap[oldClass] < 0) return true;
		w.Bits(24, kNewTypeBits);
		w.Bits(entity, kEdictBits);
		w.Bits((uint32_t)m_classMap[oldClass], 9);
		w.Bits((uint32_t)body.Left(), 11);
		w.Copy(body, body.Position(), body.End());
		return true;
	}
	case 25: b.Skip(b.Bits(11)); return copySame();
	case 26: { // svc_PacketEntities: entity and property indexes are re-encoded
		const uint32_t maxEntries = b.Bits(11);
		const bool delta = b.Bit();
		const int32_t from = delta ? b.Signed(32) : 0;
		const bool baseline = b.Bit();
		const int updated = (int)b.Bits(11);
		const uint32_t length = b.Bits(20);
		const bool updateBaseline = b.Bit();
		BitReader body = b.Sub(length);
		if (b.Overrun()) return false;
		BitWriter entities;
		if (!ConvertEntities(body, entities, updated, delta)) return false;
		w.Bits(26, kNewTypeBits);
		w.Bits(maxEntries, 11);
		w.Bit(delta);
		if (delta) w.Signed(from, 32);
		w.Bit(baseline);
		w.Bits((uint32_t)m_writtenEntities, 11);
		w.Bits((uint32_t)entities.Size(), 20);
		w.Bit(updateBaseline);
		w.Append(entities);
		++m_stats.packetEntities;
		return true;
	}
	case 27: { // svc_TempEntities: 17-bit length becomes a varint; indexes are re-encoded
		const int count = (int)b.Bits(8);
		BitReader body = b.Sub(b.Bits(17));
		if (b.Overrun()) return false;
		BitWriter entities;
		if (!ConvertTempEntities(body, entities, count)) return false;
		w.Bits(27, kNewTypeBits);
		w.Bits((uint32_t)count, 8);
		w.VarInt((uint32_t)entities.Size());
		w.Append(entities);
		return true;
	}
	case 28: { // svc_Prefetch: 13-bit sound index becomes 14
		const uint32_t sound = b.Bits(13);
		w.Bits(28, kNewTypeBits);
		w.Bits(sound, 14);
		return !b.Overrun();
	}
	case 29: b.Bits(16); b.Skip((size_t)b.Bits(16) * 8); return copySame();
	case 30: b.Bits(9); b.Skip(b.Bits(20)); return copySame();
	case 31: b.Bits(32); b.String(); return copySame();
	default:
		return Fail("unsupported message %d", type);
	}
}

bool Converter::ConvertPacket(const unsigned char* data, size_t size, BitWriter& w) {
	BitReader b(data, size);
	bool inCreates = false;
	while (b.Left() >= (size_t)kOldTypeBits) {
		const int type = (int)b.Bits(kOldTypeBits);
		if (type != 12 && inCreates && !m_newTablesAdded) AppendNewTables(w);
		inCreates = type == 12;
		if (!ConvertMessage(type, b, w)) return Fail("message %d: %s", type, m_error.empty() ? "malformed" : m_error.c_str());
		if (b.Overrun()) return Fail("message %d runs past its packet", type);
		++m_stats.messages;
	}
	if (inCreates && !m_newTablesAdded) AppendNewTables(w);
	return true;
}

ConvertResult Converter::Run() {
	ConvertResult result;
	if (!IsV34Demo(m_data, m_size)) {
		result.error = "not an old-engine (network protocol 7) demo";
		return result;
	}
	if (!LoadTables()) {
		result.error = m_error;
		return result;
	}

	m_out.clear();
	m_out.reserve(m_size + m_size / 8);
	m_out.insert(m_out.end(), m_data, m_data + kHeaderSize);
	const int32_t protocol = kNewProtocol;
	std::memcpy(&m_out[12], &protocol, 4);

	size_t at = kHeaderSize;
	bool signon = true;
	size_t signonBytes = 0;
	int32_t lastTick = 0;
	bool stopped = false;
	while (!stopped && at + 5 <= m_size) {
		const size_t frameStart = at;
		const size_t outStart = m_out.size();
		const int command = m_data[at];
		const int32_t tick = GetInt32(m_data + at + 1);
		at += 5;
		++m_stats.framesIn;
		if (command == kSyncTick || command == kPacket) signon = false;
		std::string why;
		switch (command) {
		case kSignon:
		case kPacket: {
			if (at + kCommandInfoSize + 12 > m_size) { why = "truncated packet frame"; break; }
			const unsigned char* info = m_data + at;
			at += kCommandInfoSize + 8;
			const int32_t length = GetInt32(m_data + at);
			at += 4;
			if (length < 0 || at + (size_t)length > m_size) { why = "truncated packet frame"; break; }
			BitWriter packet;
			m_error.clear();
			if (!ConvertPacket(m_data + at, (size_t)length, packet)) { why = m_error; break; }
			at += (size_t)length;
			WriteFrame(command, tick, info, packet.Data().data(), packet.Bytes());
			break;
		}
		case kSyncTick:
			WriteFrame(command, tick, nullptr, nullptr, 0);
			break;
		case kConsoleCmd:
		case kStringTables: {
			if (at + 4 > m_size) { why = "truncated frame"; break; }
			const int32_t length = GetInt32(m_data + at);
			at += 4;
			if (length < 0 || at + (size_t)length > m_size) { why = "truncated frame"; break; }
			WriteFrame(command, tick, nullptr, m_data + at, (size_t)length);
			at += (size_t)length;
			break;
		}
		case kUserCmd: {
			// The old engine's user commands use a different layout, and playback does not need them.
			if (at + 8 > m_size) { why = "truncated user command"; break; }
			at += 4;
			const int32_t length = GetInt32(m_data + at);
			at += 4;
			if (length < 0 || at + (size_t)length > m_size) { why = "truncated user command"; break; }
			at += (size_t)length;
			++m_stats.userCmdsDropped;
			break;
		}
		case kDataTables: {
			if (at + 4 > m_size) { why = "truncated frame"; break; }
			const int32_t length = GetInt32(m_data + at);
			at += 4 + (size_t)(length < 0 ? 0 : length);
			BitWriter tables;
			WriteDataTables(m_tables, tables);
			WriteFrame(command, tick, nullptr, tables.Data().data(), tables.Bytes());
			break;
		}
		case kStop:
			WriteFrame(command, tick, nullptr, nullptr, 0);
			stopped = true;
			break;
		default:
			why = "unknown frame command " + std::to_string(command);
			break;
		}
		if (!why.empty()) {
			m_out.resize(outStart); // nothing of the frame that failed
			if (signon) {
				result.error = "signon frame " + std::to_string(m_stats.framesIn) + ": " + why;
				return result;
			}
			char text[300];
			std::snprintf(text, sizeof(text), "frame %d at byte %zu of %zu: %s", m_stats.framesIn, frameStart, m_size, why.c_str());
			m_stats.stoppedEarly = text;
			WriteFrame(kStop, lastTick, nullptr, nullptr, 0);
			stopped = true;
		}
		if (signon) signonBytes = m_out.size() - kHeaderSize;
		if (command == kPacket) lastTick = tick;
	}
	if (!stopped) WriteFrame(kStop, lastTick, nullptr, nullptr, 0);

	const int32_t frames = m_stats.framesOut, signonLength = (int32_t)signonBytes;
	std::memcpy(&m_out[1064], &frames, 4);
	std::memcpy(&m_out[1068], &signonLength, 4);
	result.ok = true;
	result.stats = m_stats;
	return result;
}

} // namespace

bool IsV34Demo(const unsigned char* data, size_t size) {
	return data && size >= kHeaderSize && std::memcmp(data, "HL2DEMO\0", 8) == 0 && GetInt32(data + 8) == 3 && GetInt32(data + 12) == 7;
}

std::string DemoMapName(const unsigned char* data, size_t size) {
	if (!data || size < kHeaderSize || std::memcmp(data, "HL2DEMO\0", 8) != 0) return std::string();
	const char* name = (const char*)data + 16 + 2 * 260;
	return std::string(name, strnlen(name, 260));
}

ConvertResult ConvertV34Demo(const unsigned char* data, size_t size, std::vector<unsigned char>& out, const ConvertOptions& options) {
	if (!options.map) {
		Converter converter(data, size, out);
		return converter.Run();
	}
	// First pass: where each brush entity is. Then match those to the installed map and write.
	std::vector<BrushUse> uses;
	std::vector<std::string> precache;
	{
		std::vector<unsigned char> scratch;
		Converter observe(data, size, scratch);
		observe.Observe(&uses);
		const ConvertResult first = observe.Run();
		if (!first.ok) return first;
		precache = observe.ModelPrecache();
	}
	int oldCount = 1;
	for (const std::string& name : precache)
		if (name.size() > 1 && name[0] == '*') oldCount = std::max(oldCount, std::atoi(name.c_str() + 1) + 1);
	const InlineRemap remap = MatchInlineModels(uses, oldCount, *options.map);
	std::vector<std::string> rewrite(precache.size());
	std::vector<char> drop(precache.size(), 0);
	for (size_t i = 0; i < precache.size(); ++i) {
		const std::string& name = precache[i];
		if (name.size() < 2 || name[0] != '*') continue;
		const int n = std::atoi(name.c_str() + 1);
		const int target = n > 0 && n < (int)remap.target.size() ? remap.target[(size_t)n] : -1;
		if (target > 0) {
			rewrite[i] = "*" + std::to_string(target);
		} else {
			// A unique stand-in that loads without stopping the game; nothing that used it is written.
			rewrite[i] = "models/sourcemvm/missing_brush_" + std::to_string(n) + ".mdl";
			drop[i] = 1;
		}
	}
	Converter converter(data, size, out);
	converter.Rewrite(std::move(rewrite), std::move(drop));
	ConvertResult result = converter.Run();
	result.stats.mapMatched = true;
	result.stats.brushKept = remap.kept;
	result.stats.brushMoved = remap.moved;
	result.stats.brushMissing = remap.missing;
	result.stats.brushUnused = remap.unused;
	result.stats.brushDetail = remap.detail;
	return result;
}

} // namespace DemoConvert
} // namespace SourceFilmmaker
