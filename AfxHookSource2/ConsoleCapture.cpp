#include "stdafx.h"

#include "ConsoleCapture.h"
#ifndef GAME_DEADLOCK
#include "ConsoleLoggingListener.h"
#endif
#include "Filmmaker/Debug/MvmDebugLog.h"
#include "Filmmaker/Panorama/PanoramaBridge.h" // NoteEngineQuitting

#include <Windows.h>
#include <cstdarg>
#include <cstring>

advancedfx::Con_Printf_t Tier0_Message = nullptr;
advancedfx::Con_Printf_t Tier0_Warning = nullptr;
advancedfx::Con_DevPrintf_t Tier0_DevMessage = nullptr;
advancedfx::Con_DevPrintf_t Tier0_DevWarning = nullptr;

namespace {

class CMvmTier0LoggingListener
#ifndef GAME_DEADLOCK
    : public Filmmaker::Cs2LoggingListener
#endif
{
public:
	virtual void Log(const void*, const char* message) {
		// The engine announces a quit before it tears Panorama down; past this point a HUD's
		// script or panel call reads freed memory. Seen as "QueueNewRequest( Quitting" and then
		// "HostStateRequest::Start(HSR_QUIT)" in crash histories.
		if (message && (strstr(message, "HSR_QUIT") || strstr(message, "QueueNewRequest( Quitting")))
			Filmmaker::PanoramaBridge::NoteEngineQuitting();
		Filmmaker::MvmDebugLog_ConsoleLine("engine", -1, message);
	}
};

typedef void (__fastcall* LoggingSystem_ListenerFn_t)(void* listener);

CMvmTier0LoggingListener g_MvmTier0LoggingListener;
LoggingSystem_ListenerFn_t g_LoggingSystem_UnregisterLoggingListener = nullptr;
bool g_MvmTier0LoggingListenerRegistered = false;

} // namespace

bool Filmmaker::MvmConsoleCapture_Start() {
	if (g_MvmTier0LoggingListenerRegistered)
		return true;
	HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
	if (!tier0)
		return false;
	auto registerListener = (LoggingSystem_ListenerFn_t)GetProcAddress(
		tier0, "LoggingSystem_RegisterLoggingListener");
	g_LoggingSystem_UnregisterLoggingListener = (LoggingSystem_ListenerFn_t)GetProcAddress(
		tier0, "LoggingSystem_UnregisterLoggingListener");
	if (!registerListener || !g_LoggingSystem_UnregisterLoggingListener)
		return false;
	registerListener(&g_MvmTier0LoggingListener);
	g_MvmTier0LoggingListenerRegistered = true;
	return true;
}

namespace {
bool g_MvmConsoleCaptureKeepAlive = false;
}

void Filmmaker::MvmConsoleCapture_KeepAlive() {
	g_MvmConsoleCaptureKeepAlive = Filmmaker::MvmConsoleCapture_Start();
}

void Filmmaker::MvmConsoleCapture_Stop() {
	// The bug report's always-on recorder holds the listener for the whole session.
	if (g_MvmConsoleCaptureKeepAlive)
		return;
	if (!g_MvmTier0LoggingListenerRegistered || !g_LoggingSystem_UnregisterLoggingListener)
		return;
	g_LoggingSystem_UnregisterLoggingListener(&g_MvmTier0LoggingListener);
	g_MvmTier0LoggingListenerRegistered = false;
}

bool Filmmaker::MvmConsoleCapture_Active() {
	return g_MvmTier0LoggingListenerRegistered;
}

class CConsolePrint_Message : public IConsolePrint {
public:
	virtual void Print(const char * text) {
		if (!g_MvmTier0LoggingListenerRegistered)
			Filmmaker::MvmDebugLog_ConsoleLine("message", -1, text);
		Tier0_Message("%s", text);
	}
};

class CConsolePrint_Warning : public IConsolePrint {
public:
	virtual void Print(const char * text) {
		if (!g_MvmTier0LoggingListenerRegistered)
			Filmmaker::MvmDebugLog_ConsoleLine("warning", -1, text);
		Tier0_Warning("%s", text);
	}
};

class CConsolePrint_DevMessage : public IConsolePrint {
public:
	CConsolePrint_DevMessage(int level)
	: m_Level(level) {
	}

	virtual void Print(const char * text) {
		if (!g_MvmTier0LoggingListenerRegistered)
			Filmmaker::MvmDebugLog_ConsoleLine("message", m_Level, text);
		Tier0_DevMessage(m_Level, "%s", text);
	}
private:
	int m_Level;
};

class CConsolePrint_DevWarning : public IConsolePrint {
public:
	CConsolePrint_DevWarning(int level)
	: m_Level(level) {
	}

	virtual void Print(const char * text) {
		if (!g_MvmTier0LoggingListenerRegistered)
			Filmmaker::MvmDebugLog_ConsoleLine("warning", m_Level, text);
		Tier0_DevWarning(m_Level, "%s", text);
	}
private:
	int m_Level;
};

CConsolePrinter * g_ConsolePrinter = nullptr;

void My_Console_Message(const char* fmt, ...) {
	CConsolePrint_Message consolePrint;
	va_list args;
	va_start(args, fmt);
	g_ConsolePrinter->Print(&consolePrint, fmt, args);
	va_end(args);
}

void My_Console_Warning(const char* fmt, ...) {
	CConsolePrint_Warning consolePrint;
	va_list args;
	va_start(args, fmt);
	g_ConsolePrinter->Print(&consolePrint, fmt, args);
	va_end(args);
}

void My_Console_DevMessage(int level, const char* fmt, ...) {
	CConsolePrint_DevMessage consolePrint(level);
	va_list args;
	va_start(args, fmt);
	g_ConsolePrinter->Print(&consolePrint, fmt, args);
	va_end(args);
}

void My_Console_DevWarning(int level, const char* fmt, ...) {
	CConsolePrint_DevWarning consolePrint(level);
	va_list args;
	va_start(args, fmt);
	g_ConsolePrinter->Print(&consolePrint, fmt, args);
	va_end(args);
}
