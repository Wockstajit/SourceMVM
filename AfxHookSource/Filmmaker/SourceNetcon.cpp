#include "stdafx.h"

#include "SourceNetcon.h"

#include "WrpVEngineClient.h"

#include <Shared/AfxCommandLine.h>
#include <Shared/AfxConsole.h>
#include <Shared/NetconServer.h>

#include <atomic>
#include <cstdlib>
#include <string>

extern advancedfx::CCommandLine* g_CommandLine;
extern WrpVEngineClient* g_VEngineClient;

namespace SourceFilmmaker {

namespace {

const unsigned short kDefaultPort = 29030;

// tier0's spew contract (dbg.h): the return value tells tier0 whether to break, continue or abort.
typedef int (*SpewOutputFunc_t)(int spewType, const char* message);
typedef void (*SetSpewOutputFunc_t)(SpewOutputFunc_t func);
typedef SpewOutputFunc_t (*GetSpewOutputFunc_t)();
const int kSpewContinue = 1;

SpewOutputFunc_t g_previousSpew = nullptr;
GetSpewOutputFunc_t g_getSpew = nullptr;

std::atomic<HWND> g_window{ nullptr };
UINT g_wakeMessage = 0;
std::atomic<bool> g_wakePosted{ false };
std::atomic<unsigned long long> g_pumps{ 0 };

int NetconSpew(int spewType, const char* message) {
	advancedfx::NetconServer::Write(message);
	return g_previousSpew ? g_previousSpew(spewType, message) : kSpewContinue;
}

void InstallConsoleCapture() {
	HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
	if (!tier0) return;
	auto setSpew = (SetSpewOutputFunc_t)GetProcAddress(tier0, "SpewOutputFunc");
	g_getSpew = (GetSpewOutputFunc_t)GetProcAddress(tier0, "GetSpewOutputFunc");
	if (!setSpew || !g_getSpew) {
		advancedfx::Warning("AfxHookSource: netcon cannot capture console output (tier0 spew exports missing).\n");
		return;
	}
	g_previousSpew = g_getSpew();
	setSpew(&NetconSpew);
}

// Listener thread. One wake-up message in flight at a time is enough: the pump drains everything.
void OnLineReceived() {
	HWND window = g_window.load();
	if (!window || !g_wakeMessage) return;
	if (g_wakePosted.exchange(true)) return;
	if (!PostMessageW(window, g_wakeMessage, 0, 0)) g_wakePosted.store(false);
}

void PumpMainThread() {
	g_wakePosted.store(false);
	g_pumps.fetch_add(1);

	// Bounded per wake so a pasted wall of commands cannot stall one frame. Anything left over
	// gets another wake-up.
	for (int i = 0; i < 64; ++i) {
		std::string command;
		if (!advancedfx::NetconServer::PopCommand(command)) return;
		if (!g_VEngineClient) continue;
		// Queued into the engine's command buffer, which runs it on this thread at the frame's
		// normal command point, as if it had been typed into the console.
		g_VEngineClient->ClientCmd_Unrestricted(command.c_str());
		advancedfx::NetconServer::CountExecuted();
	}
	if (advancedfx::NetconServer::HasPendingCommands()) OnLineReceived();
}

} // namespace

void Netcon_Init() {
	static bool initialized = false;
	if (initialized) return;
	initialized = true;

	if (!g_CommandLine) return;
	int index = g_CommandLine->FindParam(L"-mvmnetconport");
	if (!index) return;

	unsigned short port = kDefaultPort;
	if (index + 1 < g_CommandLine->GetArgC()) {
		long value = wcstol(g_CommandLine->GetArgV(index + 1), nullptr, 10);
		if (0 < value && value < 65536) port = (unsigned short)value;
	}

	g_wakeMessage = RegisterWindowMessageW(L"SourceMVM.Netcon.Wake");

	advancedfx::NetconServer::Config config;
	config.port = port;
	config.banner = "AfxHookSource netcon ready\n";
	config.name = "AfxHookSource netcon";
	config.onLineReceived = &OnLineReceived;
	if (!advancedfx::NetconServer::Start(config)) return;

	InstallConsoleCapture();
	advancedfx::Message("AfxHookSource: netcon listening on 127.0.0.1:%u\n", (unsigned)port);
}

bool Netcon_OnWindowMessage(HWND hwnd, UINT msg) {
	if (!g_window.load()) {
		g_window.store(hwnd);
		// Lines that arrived before the window existed are still queued.
		if (advancedfx::NetconServer::HasPendingCommands()) OnLineReceived();
	}
	if (!g_wakeMessage || msg != g_wakeMessage) return false;
	PumpMainThread();
	return true;
}

bool Netcon_ConsoleCaptured() {
	return g_getSpew && g_getSpew() == &NetconSpew;
}

unsigned long long Netcon_PumpCount() { return g_pumps.load(); }

} // namespace SourceFilmmaker
