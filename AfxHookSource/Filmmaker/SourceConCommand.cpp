#include "stdafx.h"

#include "SourceConCommand.h"

// The hook's SDK prelude (Assert and friends), which the CS:S SDK headers below expect.
#include "WrpVEngineClient.h"

#include <css/sdk_src/public/icvar.h>
#include <css/sdk_src/public/tier1/convar.h>

#include <windows.h>

#include <cmath>
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

// The variable's value, guarded: read only when the variable's name reads back, and a layout that
// does not match faults into false rather than a crash.
bool ReadFloat(SOURCESDK::CSS::ConVar* var, const char* name, float& value) {
	__try {
		const ConCommandData* data = (const ConCommandData*)var;
		if (!data->name || std::strcmp(data->name, name) != 0) return false;
		value = var->GetFloat();
		return std::isfinite(value);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool ReadString(SOURCESDK::CSS::ConVar* var, const char*& value) {
	__try {
		const ConCommandData* data = (const ConCommandData*)var;
		if (!data->name) return false;
		value = var->GetString();
		return value != nullptr;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool WriteString(SOURCESDK::CSS::ConVar* var, const char* value) {
	__try {
		var->SetValue(value);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

} // namespace

void* ConVar_Find(const char* name) {
	if (!SOURCESDK::CSS::g_pCVar) return nullptr;
	SOURCESDK::CSS::ConVar* var = SOURCESDK::CSS::g_pCVar->FindVar(name);
	const ConCommandData* data = (const ConCommandData*)var;
	return var && data->name && std::strcmp(data->name, name) == 0 ? var : nullptr;
}

bool ConVar_GetString(void* var, std::string& value) {
	const char* text = nullptr;
	if (!var || !ReadString((SOURCESDK::CSS::ConVar*)var, text)) return false;
	value = text;
	return true;
}

void ConVar_SetString(void* var, const char* value) {
	if (var) WriteString((SOURCESDK::CSS::ConVar*)var, value);
}

bool ConVar_GetFloat(const char* name, float& value) {
	if (!SOURCESDK::CSS::g_pCVar) return false;
	SOURCESDK::CSS::ConVar* var = SOURCESDK::CSS::g_pCVar->FindVar(name);
	return var && ReadFloat(var, name, value);
}

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
