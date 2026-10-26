#pragma once

// Finds a networked field's byte offset by walking the client's network tables, so the adapter
// asks the running game where m_iHealth lives instead of hardcoding an offset a game update can
// move. Engine-free and header-only; tests build fake tables.
//
// A field is named by the table it is declared in and its prop name, e.g. ("DT_BasePlayer",
// "m_iHealth"). The search starts at the classes' top tables and follows every embedded data
// table (base classes, m_Local and the like), adding each embedded table's own offset on the way
// down, because an embedded table's props are relative to where that table sits in the entity.

#include "SourceSdk2013.h"

#include <cstring>

namespace SourceFilmmaker {

namespace NetvarDetail {

// Depth bound: real tables nest a dozen levels at most, and a corrupt list must not recurse forever.
const int kMaxDepth = 32;

inline bool FindInTable(const Sdk2013::RecvTable* table, const char* tableName, const char* propName,
	bool insideWantedTable, int baseOffset, int depth, int& outOffset) {
	if (!table || depth > kMaxDepth || !table->m_pProps || table->m_nProps <= 0) return false;
	const bool wanted = insideWantedTable
		|| (table->m_pNetTableName && 0 == std::strcmp(table->m_pNetTableName, tableName));
	for (int i = 0; i < table->m_nProps; ++i) {
		const Sdk2013::RecvProp& prop = table->m_pProps[i];
		if (wanted && prop.m_pVarName && 0 == std::strcmp(prop.m_pVarName, propName)) {
			outOffset = baseOffset + prop.m_Offset;
			return true;
		}
	}
	// Embedded tables: a base class keeps the wanted table only if it IS the wanted table, which
	// the name check at the top of the recursion decides; m_Local style members of the wanted table
	// are searched too, so a prop declared in a member table of the wanted table is still found.
	for (int i = 0; i < table->m_nProps; ++i) {
		const Sdk2013::RecvProp& prop = table->m_pProps[i];
		if (prop.m_RecvType != Sdk2013::DPT_DataTable || !prop.m_pDataTable) continue;
		const bool memberOfWanted = wanted && prop.m_pVarName && 0 != std::strcmp(prop.m_pVarName, "baseclass");
		if (FindInTable(prop.m_pDataTable, tableName, propName, memberOfWanted, baseOffset + prop.m_Offset,
			depth + 1, outOffset))
			return true;
	}
	return false;
}

} // namespace NetvarDetail

// Byte offset of tableName.propName inside the entity, or -1. classes is the head of the client's
// class list (IBaseClientDLL::GetAllClasses).
inline int FindNetvarOffset(const Sdk2013::ClientClass* classes, const char* tableName, const char* propName) {
	if (!tableName || !propName) return -1;
	int guard = 0;
	for (const Sdk2013::ClientClass* cls = classes; cls && guard < 4096; cls = cls->m_pNext, ++guard) {
		int offset = -1;
		if (NetvarDetail::FindInTable(cls->m_pRecvTable, tableName, propName, false, 0, 0, offset))
			return offset;
	}
	return -1;
}

// Number of classes in the list, bounded the same way, for health reports.
inline int CountClientClasses(const Sdk2013::ClientClass* classes) {
	int count = 0;
	for (const Sdk2013::ClientClass* cls = classes; cls && count < 4096; cls = cls->m_pNext) ++count;
	return count;
}

} // namespace SourceFilmmaker
