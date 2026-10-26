// SourceNetvars.h against fake network tables shaped like CS:S's: a field is found by the table
// that declares it, embedded tables add their offset, base classes do not stand in for the wanted
// table, and a malformed list cannot loop.

#include "../../AfxHookSource/Filmmaker/SourceNetvars.h"

#include <cstdio>
#include <cstdlib>

using namespace SourceFilmmaker;
using namespace SourceFilmmaker::Sdk2013;

static void Check(bool ok, const char* what) {
	if (ok) return;
	std::fprintf(stderr, "FAILED: %s\n", what);
	std::exit(1);
}

static RecvProp Prop(const char* name, int offset) {
	RecvProp p = {};
	p.m_pVarName = name;
	p.m_RecvType = DPT_Int;
	p.m_Offset = offset;
	return p;
}

static RecvProp Table(const char* name, RecvTable* table, int offset) {
	RecvProp p = {};
	p.m_pVarName = name;
	p.m_RecvType = DPT_DataTable;
	p.m_pDataTable = table;
	p.m_Offset = offset;
	return p;
}

static RecvTable MakeTable(const char* name, RecvProp* props, int count) {
	RecvTable t = {};
	t.m_pNetTableName = name;
	t.m_pProps = props;
	t.m_nProps = count;
	return t;
}

int main() {
	RecvProp baseEntityProps[] = { Prop("m_iTeamNum", 0x90), Prop("m_vecOrigin", 0x2CC) };
	RecvTable baseEntity = MakeTable("DT_BaseEntity", baseEntityProps, 2);

	RecvProp localProps[] = { Prop("m_iHideHUD", 0x4C) };
	RecvTable local = MakeTable("DT_Local", localProps, 1);

	RecvProp exclusiveProps[] = { Prop("m_vecViewOffset[0]", 0xE8), Table("m_Local", &local, 0xC00) };
	RecvTable exclusive = MakeTable("DT_LocalPlayerExclusive", exclusiveProps, 2);

	RecvProp basePlayerProps[] = {
		Table("baseclass", &baseEntity, 0),
		Prop("m_iHealth", 0xD0),
		Prop("m_lifeState", 0x8F),
		Table("localdata", &exclusive, 0),
	};
	RecvTable basePlayer = MakeTable("DT_BasePlayer", basePlayerProps, 4);

	RecvProp csPlayerProps[] = { Table("baseclass", &basePlayer, 0), Prop("m_angEyeAngles[0]", 0x1400) };
	RecvTable csPlayer = MakeTable("DT_CSPlayer", csPlayerProps, 2);

	RecvTable world = MakeTable("DT_WORLD", nullptr, 0);

	ClientClass worldClass = {};
	worldClass.m_pNetworkName = "CWorld";
	worldClass.m_pRecvTable = &world;
	ClientClass playerClass = {};
	playerClass.m_pNetworkName = "CCSPlayer";
	playerClass.m_pRecvTable = &csPlayer;
	playerClass.m_pNext = &worldClass;

	Check(CountClientClasses(&playerClass) == 2, "class count");
	Check(FindNetvarOffset(&playerClass, "DT_CSPlayer", "m_angEyeAngles[0]") == 0x1400, "field of the class's own table");
	Check(FindNetvarOffset(&playerClass, "DT_BasePlayer", "m_iHealth") == 0xD0, "field of a base class table");
	Check(FindNetvarOffset(&playerClass, "DT_BaseEntity", "m_iTeamNum") == 0x90, "field two base classes down");
	Check(FindNetvarOffset(&playerClass, "DT_BasePlayer", "m_vecViewOffset[0]") == 0xE8,
		"field of a member table of the wanted table");
	Check(FindNetvarOffset(&playerClass, "DT_Local", "m_iHideHUD") == 0xC00 + 0x4C,
		"nested table adds its own offset");
	Check(FindNetvarOffset(&playerClass, "DT_CSPlayer", "m_iHealth") == -1,
		"a field is named by the table that declares it, not a derived one");
	Check(FindNetvarOffset(&playerClass, "DT_BasePlayer", "m_nope") == -1, "unknown field");
	Check(FindNetvarOffset(&playerClass, "DT_Nope", "m_iHealth") == -1, "unknown table");
	Check(FindNetvarOffset(nullptr, "DT_BasePlayer", "m_iHealth") == -1, "no class list");

	// A table that embeds itself must end at the depth bound instead of recursing forever.
	RecvProp loopProps[1];
	RecvTable loop = MakeTable("DT_Loop", loopProps, 1);
	loopProps[0] = Table("self", &loop, 4);
	ClientClass loopClass = {};
	loopClass.m_pRecvTable = &loop;
	Check(FindNetvarOffset(&loopClass, "DT_Elsewhere", "m_x") == -1, "self-embedding table ends");

	std::printf("SourceNetvarsTests passed\n");
	return 0;
}
