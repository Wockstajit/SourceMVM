#include "DemoTables.h"

#include <cmath>
#include <cstdio>
#include <set>

namespace SourceFilmmaker {
namespace DemoConvert {

namespace {

// The type numbers each engine writes (dt_common.h SendPropType).
const PropType kTypes7[] = { PropType::Int, PropType::Float, PropType::Vector, PropType::String, PropType::Array, PropType::DataTable };
const PropType kTypes24[] = { PropType::Int, PropType::Float, PropType::Vector, PropType::VectorXY, PropType::String,
	PropType::Array, PropType::DataTable, PropType::Int64 };

int TypeNumber24(PropType type) {
	for (int i = 0; i < (int)(sizeof(kTypes24) / sizeof(kTypes24[0])); ++i)
		if (kTypes24[i] == type) return i;
	return 0;
}

struct Layout {
	const PropType* types;
	int typeCount;
	int propCountBits;
	int flagBits;
	int bitCountBits;
};

Layout LayoutFor(int protocol) {
	if (protocol <= 7) return { kTypes7, 6, 9, 13, 6 };
	return { kTypes24, 8, 10, 16, 7 };
}

} // namespace

const SendTable* DataTables::Find(const std::string& name) const {
	if (m_index.size() != tables.size()) {
		m_index.clear();
		for (size_t i = 0; i < tables.size(); ++i) m_index[tables[i].name] = i;
	}
	auto it = m_index.find(name);
	return it == m_index.end() ? nullptr : &tables[it->second];
}

std::vector<FlatProp> DataTables::Flatten(const std::string& tableName) const {
	// dt_send / public demo parsers: gather the excludes, then walk the tables, inlining collapsible
	// base tables and appending each other child table's props after the walk of its parent, and
	// finally move CHANGES_OFTEN props to the front.
	std::set<std::pair<std::string, std::string>> excludes;
	std::vector<const SendTable*> stack;
	auto gatherExcludes = [&](const SendTable* table, auto& self) -> void {
		if (!table) return;
		for (const SendProp& p : table->props) {
			if (p.flags & kPropExclude) excludes.emplace(p.dtName, p.name);
			else if (p.type == PropType::DataTable) self(Find(p.dtName), self);
		}
	};
	gatherExcludes(Find(tableName), gatherExcludes);

	std::vector<FlatProp> out;
	auto iterate = [&](const SendTable* table, std::vector<FlatProp>& list, auto& gather, auto& self) -> void {
		if (!table) return;
		for (size_t i = 0; i < table->props.size(); ++i) {
			const SendProp& p = table->props[i];
			if ((p.flags & (kPropInsideArray | kPropExclude)) || excludes.count({ table->name, p.name })) continue;
			if (p.type == PropType::DataTable) {
				if (p.flags & kPropCollapsible) self(Find(p.dtName), list, gather, self);
				else gather(Find(p.dtName), gather);
			} else {
				FlatProp flat;
				flat.prop = &p;
				flat.table = &table->name;
				if (p.type == PropType::Array && i > 0) flat.element = &table->props[i - 1];
				list.push_back(flat);
			}
		}
	};
	auto gather = [&](const SendTable* table, auto& self) -> void {
		std::vector<FlatProp> list;
		iterate(table, list, self, iterate);
		out.insert(out.end(), list.begin(), list.end());
	};
	gather(Find(tableName), gather);

	size_t start = 0;
	for (size_t i = 0; i < out.size(); ++i) {
		if (out[i].prop->flags & kPropChangesOften) {
			if (i != start) std::swap(out[i], out[start]);
			++start;
		}
	}
	return out;
}

bool ReadDataTables(BitReader& in, int protocol, DataTables& out, std::string& error) {
	const Layout layout = LayoutFor(protocol);
	out = DataTables();
	while (in.Bit()) {
		SendTable table;
		table.needsDecoder = in.Bit();
		table.name = in.String();
		const int count = (int)in.Bits(layout.propCountBits);
		for (int i = 0; i < count && !in.Overrun(); ++i) {
			SendProp p;
			const int type = (int)in.Bits(5);
			if (type >= layout.typeCount) {
				error = "unknown property type in " + table.name;
				return false;
			}
			p.type = layout.types[type];
			p.name = in.String();
			p.flags = in.Bits(layout.flagBits);
			if (p.type == PropType::DataTable || (p.flags & kPropExclude)) {
				p.dtName = in.String();
			} else if (p.type == PropType::Array) {
				p.elements = (int)in.Bits(10);
			} else {
				p.low = in.Float();
				p.high = in.Float();
				p.bits = (int)in.Bits(layout.bitCountBits);
			}
			table.props.push_back(std::move(p));
		}
		if (in.Overrun()) break;
		out.tables.push_back(std::move(table));
	}
	const int classCount = (int)in.Bits(16);
	for (int i = 0; i < classCount && !in.Overrun(); ++i) {
		ServerClass c;
		c.id = (int)in.Bits(16);
		c.name = in.String();
		c.table = in.String();
		out.classes.push_back(std::move(c));
	}
	if (in.Overrun()) {
		error = "data tables run past the end of their frame";
		return false;
	}
	return true;
}

void WriteDataTables(const DataTables& tables, BitWriter& out) {
	for (const SendTable& table : tables.tables) {
		out.Bit(true);
		out.Bit(table.needsDecoder);
		out.String(table.name);
		out.Bits((uint32_t)table.props.size(), 10);
		for (const SendProp& p : table.props) {
			out.Bits((uint32_t)TypeNumber24(p.type), 5);
			out.String(p.name);
			out.Bits(p.flags, 16);
			if (p.type == PropType::DataTable || (p.flags & kPropExclude)) {
				out.String(p.dtName);
			} else if (p.type == PropType::Array) {
				out.Bits((uint32_t)p.elements, 10);
			} else {
				out.Float(p.low);
				out.Float(p.high);
				out.Bits((uint32_t)p.bits, 7);
			}
		}
	}
	out.Bit(false);
	out.Bits((uint32_t)tables.classes.size(), 16);
	for (const ServerClass& c : tables.classes) {
		out.Bits((uint32_t)c.id, 16);
		out.String(c.name);
		out.String(c.table);
	}
}

bool ArrayToTable(DataTables& tables, const std::string& tableName, const std::string& propName) {
	SendTable* table = nullptr;
	for (SendTable& t : tables.tables)
		if (t.name == tableName) table = &t;
	if (!table) return false;
	for (size_t i = 1; i < table->props.size(); ++i) {
		SendProp& array = table->props[i];
		if (array.name != propName || array.type != PropType::Array) continue;
		SendTable elements;
		elements.name = propName;
		const SendProp element = table->props[i - 1];
		for (int k = 0; k < array.elements; ++k) {
			SendProp p = element;
			char name[8];
			std::snprintf(name, sizeof(name), "%03d", k);
			p.name = name;
			p.flags &= ~kPropInsideArray;
			elements.props.push_back(p);
		}
		array.type = PropType::DataTable;
		array.dtName = propName;
		array.flags = 1u << 9; // SPROP_PROXY_ALWAYS_YES, as SendPropArray3 sets it
		array.elements = 0;
		table->props.erase(table->props.begin() + (long)(i - 1));
		tables.tables.push_back(std::move(elements));
		return true;
	}
	return false;
}

namespace {

bool SkipFloat(BitReader& in, const SendProp& p, int protocol) {
	if (p.flags & kPropCoord) {
		in.SkipCoord();
	} else if (protocol >= 24 && (p.flags & (kPropCoordMp | kPropCoordMpLowPrecision | kPropCoordMpIntegral))) {
		const bool inBounds = in.Bit();
		const int intBits = inBounds ? 11 : 14;
		if (p.flags & kPropCoordMpIntegral) {
			if (in.Bit()) { in.Bit(); in.Bits(intBits); }
		} else {
			const bool hasInt = in.Bit();
			in.Bit();
			if (hasInt) in.Bits(intBits);
			in.Bits((p.flags & kPropCoordMpLowPrecision) ? 3 : 5);
		}
	} else if (p.flags & kPropNoScale) {
		in.Bits(32);
	} else if (p.flags & kPropNormal) {
		in.Bit();
		in.Bits(11);
	} else {
		in.Bits(p.bits);
	}
	return !in.Overrun();
}

} // namespace

bool SkipPropValue(BitReader& in, const FlatProp& flat, int protocol) {
	const SendProp& p = *flat.prop;
	switch (p.type) {
	case PropType::Int:
		if (protocol >= 24 && (p.flags & kPropNormal)) in.VarInt(); // SPROP_VARINT
		else in.Bits(p.bits);
		break;
	case PropType::Float:
		return SkipFloat(in, p, protocol);
	case PropType::Vector:
		SkipFloat(in, p, protocol);
		SkipFloat(in, p, protocol);
		if (p.flags & kPropNormal) in.Bit();
		else SkipFloat(in, p, protocol);
		break;
	case PropType::VectorXY:
		SkipFloat(in, p, protocol);
		SkipFloat(in, p, protocol);
		break;
	case PropType::String:
		in.Skip((size_t)in.Bits(9) * 8);
		break;
	case PropType::Array: {
		if (!flat.element) return false;
		const int count = (int)in.Bits(Log2((unsigned)p.elements) + 1);
		FlatProp element;
		element.prop = flat.element;
		for (int i = 0; i < count && !in.Overrun(); ++i)
			if (!SkipPropValue(in, element, protocol)) return false;
		break;
	}
	case PropType::Int64:
		in.Skip(64);
		break;
	default:
		return false;
	}
	return !in.Overrun();
}

namespace {

float ReadFloat7(BitReader& in, const SendProp& p) {
	if (p.flags & kPropCoord) {
		const bool integer = in.Bit(), fraction = in.Bit();
		if (!integer && !fraction) return 0.0f;
		const bool negative = in.Bit();
		float v = 0;
		if (integer) v += (float)(in.Bits(14) + 1);
		if (fraction) v += in.Bits(5) / 32.0f;
		return negative ? -v : v;
	}
	if (p.flags & kPropNoScale) return in.Float();
	if (p.flags & kPropNormal) {
		const bool negative = in.Bit();
		const float v = in.Bits(11) / 2047.0f;
		return negative ? -v : v;
	}
	if (p.bits <= 0) return p.low;
	const uint32_t raw = in.Bits(p.bits);
	return p.low + (p.high - p.low) * ((float)raw / (float)((1u << p.bits) - 1));
}

} // namespace

bool ReadPropNumbers(BitReader& in, const FlatProp& flat, float out[3], int& count) {
	const SendProp& p = *flat.prop;
	count = 0;
	switch (p.type) {
	case PropType::Int:
		out[0] = (p.flags & kPropUnsigned) ? (float)in.Bits(p.bits) : (float)in.Signed(p.bits);
		count = 1;
		break;
	case PropType::Float:
		out[0] = ReadFloat7(in, p);
		count = 1;
		break;
	case PropType::Vector:
		out[0] = ReadFloat7(in, p);
		out[1] = ReadFloat7(in, p);
		if (p.flags & kPropNormal) {
			const bool negative = in.Bit();
			const float s = out[0] * out[0] + out[1] * out[1];
			out[2] = s < 1.0f ? std::sqrt(1.0f - s) * (negative ? -1.0f : 1.0f) : 0.0f;
		} else {
			out[2] = ReadFloat7(in, p);
		}
		count = 3;
		break;
	case PropType::VectorXY:
		out[0] = ReadFloat7(in, p);
		out[1] = ReadFloat7(in, p);
		count = 2;
		break;
	default:
		return false;
	}
	return !in.Overrun();
}

} // namespace DemoConvert
} // namespace SourceFilmmaker
