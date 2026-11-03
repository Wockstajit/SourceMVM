#pragma once

// A demo's network tables (the dem_datatables frame): every SendTable with its properties, and the
// server classes. Reads the Episode One layout (network protocol 7) and the Source 2013 layout
// (protocol 24), writes the Source 2013 one, flattens a class's properties in the engine's order,
// and steps over a property's value in an entity update. Engine-free.

#include "DemoBits.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace SourceFilmmaker {
namespace DemoConvert {

enum class PropType { Int, Float, Vector, VectorXY, String, Array, DataTable, Int64 };

// dt_common.h SPROP_* flags. The first 13 mean the same in both engines; Source 2013 adds the
// COORD_MP ones, and reads SPROP_NORMAL on an integer as SPROP_VARINT.
enum : uint32_t {
	kPropUnsigned = 1u << 0,
	kPropCoord = 1u << 1,
	kPropNoScale = 1u << 2,
	kPropNormal = 1u << 5,
	kPropExclude = 1u << 6,
	kPropInsideArray = 1u << 8,
	kPropChangesOften = 1u << 10,
	kPropCollapsible = 1u << 12,
	kPropCoordMp = 1u << 13,
	kPropCoordMpLowPrecision = 1u << 14,
	kPropCoordMpIntegral = 1u << 15,
};

struct SendProp {
	PropType type = PropType::Int;
	std::string name;
	uint32_t flags = 0;
	std::string dtName; // DataTable props and excludes
	float low = 0, high = 0;
	int bits = 0;
	int elements = 0;   // Array props
};

struct SendTable {
	std::string name;
	bool needsDecoder = false;
	std::vector<SendProp> props;
};

struct ServerClass {
	int id = 0;
	std::string name;
	std::string table;
};

// One flattened property: the prop, and for an array the prop that describes its elements.
struct FlatProp {
	const SendProp* prop = nullptr;
	const SendProp* element = nullptr;
	const std::string* table = nullptr; // the table the prop is declared in
};

struct DataTables {
	std::vector<SendTable> tables;
	std::vector<ServerClass> classes;

	const SendTable* Find(const std::string& name) const;
	// The class's properties in the order entity updates index them.
	std::vector<FlatProp> Flatten(const std::string& table) const;

private:
	mutable std::unordered_map<std::string, size_t> m_index;
};

// Reads a dem_datatables payload written by the given network protocol (7 or 24).
bool ReadDataTables(BitReader& in, int protocol, DataTables& out, std::string& error);

// Writes the tables in the Source 2013 (protocol 24) layout.
void WriteDataTables(const DataTables& tables, BitWriter& out);

// Turns table.prop, an Episode One array (an Array prop after its element prop), into the Source
// 2013 SendPropArray3 form the current client expects: a DataTable prop of the same name pointing
// at a new table of that name with one prop per element, "000", "001", ... False if it is not an
// array there.
bool ArrayToTable(DataTables& tables, const std::string& table, const std::string& prop);

// Steps over one property value as the given protocol encodes it. False on a malformed value.
bool SkipPropValue(BitReader& in, const FlatProp& prop, int protocol);

// Reads an Int, Float, Vector or VectorXY value of the Episode One encoding (network protocol 7)
// as up to three numbers. False for other types or a malformed value.
bool ReadPropNumbers(BitReader& in, const FlatProp& prop, float out[3], int& count);

} // namespace DemoConvert
} // namespace SourceFilmmaker
