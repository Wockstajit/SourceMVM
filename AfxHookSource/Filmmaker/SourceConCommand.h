#pragma once

// The engine's console commands and variables by name: a command's callback, swapping it for a
// wrapper, and a variable's value. The old-demo converter wraps playdemo (SourceOldDemos.cpp); the
// engine adapter follows spec_mode's callback to SourceTV's camera (SourceEngine.cpp); the director
// reads the mouse sensitivity. The SDK keeps ConCommand's data private, so it is read through a
// mirror whose name and callback kind are checked before anything is trusted. HLAE's own cvar
// wrapper (WrpConVarRef) reads only CS:GO's cvar interface, so values come from the CS:S SDK's
// ConVar here.

#include <string>

namespace SourceFilmmaker {

// tier1/convar.h's CCommand, as a ConCommand callback receives it.
struct CommandArgs {
	int argc;
	int argv0Size;
	char argS[512];
	char argvBuffer[512];
	const char* argv[64];
};

typedef void (*CommandCallback)(const CommandArgs& args);

// The callback of console command name, or nullptr when the command does not exist yet or its layout
// does not read back as expected.
CommandCallback ConCommand_Callback(const char* name);

// Swaps name's callback for wrapper and returns the original, or nullptr (nothing changed).
CommandCallback ConCommand_Wrap(const char* name, CommandCallback wrapper);

// The value of console variable name. False when it does not exist or does not read back as one.
bool ConVar_GetFloat(const char* name, float& value);

// A console variable found once and then read and set by handle, for code that changes the same
// variables every frame (the render passes). nullptr when the game has no variable of that name.
void* ConVar_Find(const char* name);
// Its value as text. False when the handle does not read back as a variable.
bool ConVar_GetString(void* var, std::string& value);
// Sets it as the console would, past FCVAR_CHEAT (the cheat check belongs to the console's parser).
void ConVar_SetString(void* var, const char* value);

} // namespace SourceFilmmaker
