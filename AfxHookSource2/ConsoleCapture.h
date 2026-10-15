#pragma once

#include "../Shared/AfxConsole.h"
#include "../Shared/ConsolePrinter.h"

extern advancedfx::Con_Printf_t Tier0_Message;
extern advancedfx::Con_Printf_t Tier0_Warning;
extern advancedfx::Con_DevPrintf_t Tier0_DevMessage;
extern advancedfx::Con_DevPrintf_t Tier0_DevWarning;

extern CConsolePrinter * g_ConsolePrinter;

namespace Filmmaker {
bool MvmConsoleCapture_Start();
// Starts capture and keeps it for the whole session (Stop becomes a no-op): the bug report's
// always-on recorder. Call from a main-thread frame, never during DLL load (loader lock).
void MvmConsoleCapture_KeepAlive();
void MvmConsoleCapture_Stop();
bool MvmConsoleCapture_Active();
}

void My_Console_Message(const char* fmt, ...);
void My_Console_Warning(const char* fmt, ...);
void My_Console_DevMessage(int level, const char* fmt, ...);
void My_Console_DevWarning(int level, const char* fmt, ...);
