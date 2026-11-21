#pragma once
#include <Windows.h>
#include "../Shared/binutils.h"
#include "../Shared/MirvInput.h"
#include "WrpConsole.h"

#define STRINGIZE(x) STRINGIZE2(x)
#define STRINGIZE2(x) #x
#define MkErrStr(file,line) "Problem in " file ":" STRINGIZE(line)

extern void ErrorBox(char const * messageText);

extern void ErrorBox();

// TODO: These two probably should be removed when moving stuff over to addresses.cpp since they don't reuse stuff
extern size_t getAddress(HMODULE dll, char const* pattern);

extern size_t getVTableFn(HMODULE dll, int index, const char* mangledClass);

// Start of the function that contains code address `addr`, read from the module's x64 unwind table
// (.pdata) and following chained cold fragments back to the primary entry. 0 when no entry covers it.
// Use this instead of scanning back for int3 padding: MSVC often pads with a single CC.
extern size_t getFunctionStart(size_t addr);

namespace afxUtils {
	struct RGBA {
		uint8_t r;
		uint8_t g;
		uint8_t b;
		uint8_t a;
	};
	std::string createTable(std::vector<std::vector<std::string>>& rows, char* delimiter, char* emptyRowDelimiter);
	std::string rgbaToHex(std::string str, std::string delim);
	uint32_t hexStrToInt(std::string str);
	uint32_t rgbaToHex(RGBA& color);
	std::string stringToLowerCase(const char* s);
}
