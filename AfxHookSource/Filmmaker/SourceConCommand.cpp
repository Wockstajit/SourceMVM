#include "stdafx.h"

#include "SourceConCommand.h"

// The hook's SDK prelude (Assert and friends), which the CS:S SDK headers below expect.
#include "WrpVEngineClient.h"

#include <css/sdk_src/public/icvar.h>
#include <css/sdk_src/public/tier1/convar.h>

#include <cstring>

namespace SourceFilmmaker {

namespace {

// tier1/convar.h's ConCommand data, which the SDK keeps private: the base's vtable, next, registered,
// name, help and flags, then the callback union, the completion union and the callback-kind bits.
struct ConCommandData {
	void* vtable;
	void* next;
	bool registered;
	const char* name;
	const char* help;
	int flags;
	void* callback;
	void* completion;
	unsigned char bits; // bit 1: m_bUsingNewCommandCallback, bit 2: m_bUsingCommandCallbackInterface
};

ConCommandData* Find(const char* name) {
	if (!SOURCESDK::CSS::g_pCVar) return nullptr;
	SOURCESDK::CSS::ConCommand* command = SOURCESDK::CSS::g_pCVar->FindCommand(name);
	if (!command) return nullptr;
	ConCommandData* data = (ConCommandData*)command;
	// Only a command whose layout reads back as expected is used: its own name, and a plain
	// CCommand callback (not the old no-argument kind, not a callback interface).
	if (!data->name || std::strcmp(data->name, name) != 0 || !(data->bits & 2) || (data->bits & 4) || !data->callback)
		return nullptr;
	return data;
}

} // namespace

CommandCallback ConCommand_Callback(const char* name) {
	ConCommandData* data = Find(name);
	return data ? (CommandCallback)data->callback : nullptr;
}

CommandCallback ConCommand_Wrap(const char* name, CommandCallback wrapper) {
	ConCommandData* data = Find(name);
	if (!data || data->callback == (void*)wrapper) return nullptr;
	const CommandCallback original = (CommandCallback)data->callback;
	data->callback = (void*)wrapper;
	return original;
}

} // namespace SourceFilmmaker
