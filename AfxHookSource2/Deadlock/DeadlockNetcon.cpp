#include "DeadlockNetcon.h"

// WINSOCK2 FIRST, before anything that can pull in windows.h.
//
// windows.h includes the ORIGINAL winsock.h unless WIN32_LEAN_AND_MEAN is set, and this target
// deliberately does not set it (the reused Shared/ sources need the full header). Including
// windows.h first therefore makes every winsock2 declaration a redefinition. DeadlockBoot.h
// includes windows.h, so it has to come after these three.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "DeadlockBoot.h"
#include "DeadlockCommands.h"
#include "DeadlockEngine.h"

#include "../../Shared/AfxConsole.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace Deadlock {

namespace {

const unsigned short kDefaultPort = 29020;

// Bounded on purpose. The whole point of not writing from the logging callback is that a stalled
// reader must not be able to slow the game down, and the only way to keep that promise is to drop
// output rather than grow without limit. The drop count is reported so a truncated capture is
// visible instead of silent.
const size_t kMaxQueuedLines = 4096;

std::atomic<bool> g_running{ false };
std::atomic<bool> g_stopping{ false };
unsigned short g_port = 0;

SOCKET g_listenSocket = INVALID_SOCKET;

std::mutex g_clientsMutex;
std::vector<SOCKET> g_clients;

std::mutex g_outMutex;
std::deque<std::string> g_outQueue;

std::mutex g_inMutex;
std::deque<std::string> g_inQueue;

std::thread g_acceptThread;
std::thread g_writeThread;

std::atomic<unsigned long long> g_commandsExecuted{ 0 };
std::atomic<unsigned long long> g_linesSent{ 0 };
std::atomic<unsigned long long> g_linesDropped{ 0 };

// One tier0 logging listener, forwarding console output into the output queue. Registered
// alongside (not instead of) the mvm_debug listener; tier0 supports several.
class NetconLoggingListener {
public:
	virtual void Log(const void*, const char* message) {
		static bool firstLine = true;
		if (firstLine) {
			firstLine = false;
			DL_BOOT("netcon: tier0 logging listener delivered its first line");
		}
		if (message) Netcon_Write(message);
	}
};

typedef void(__fastcall* LoggingSystem_ListenerFn_t)(void* listener);

NetconLoggingListener g_logListener;
LoggingSystem_ListenerFn_t g_unregisterListener = nullptr;
bool g_logListenerRegistered = false;

void RegisterLogListener() {
	if (g_logListenerRegistered) return;
	HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
	if (!tier0) return;
	auto reg = (LoggingSystem_ListenerFn_t)GetProcAddress(
		tier0, "LoggingSystem_RegisterLoggingListener");
	g_unregisterListener = (LoggingSystem_ListenerFn_t)GetProcAddress(
		tier0, "LoggingSystem_UnregisterLoggingListener");
	if (!reg || !g_unregisterListener) return;
	reg(&g_logListener);
	g_logListenerRegistered = true;
	DL_BOOT("netcon: tier0 logging listener registered");
}

void UnregisterLogListener() {
	if (!g_logListenerRegistered || !g_unregisterListener) return;
	g_unregisterListener(&g_logListener);
	g_logListenerRegistered = false;
}

void CloseClient(SOCKET s) {
	shutdown(s, SD_BOTH);
	closesocket(s);
}

void DropAllClients() {
	std::lock_guard<std::mutex> lock(g_clientsMutex);
	for (SOCKET s : g_clients) CloseClient(s);
	g_clients.clear();
}

// Reads whatever a client has sent and splits it into lines. Partial lines are held per client
// until the terminator arrives, because a batched send can land split across recv boundaries.
void ServiceClientReads() {
	std::vector<SOCKET> dead;
	static std::vector<std::pair<SOCKET, std::string>> partials;

	std::vector<SOCKET> snapshot;
	{
		std::lock_guard<std::mutex> lock(g_clientsMutex);
		snapshot = g_clients;
	}

	for (SOCKET s : snapshot) {
		char buf[4096];
		for (;;) {
			int n = recv(s, buf, sizeof(buf), 0);
			if (n == 0) { dead.push_back(s); break; }
			if (n < 0) {
				int err = WSAGetLastError();
				if (err == WSAEWOULDBLOCK) break;
				dead.push_back(s);
				break;
			}

			std::string* held = nullptr;
			for (auto& p : partials) {
				if (p.first == s) { held = &p.second; break; }
			}
			if (!held) {
				partials.push_back({ s, std::string() });
				held = &partials.back().second;
			}
			held->append(buf, (size_t)n);

			for (;;) {
				size_t nl = held->find('\n');
				if (nl == std::string::npos) break;
				std::string line = held->substr(0, nl);
				held->erase(0, nl + 1);
				while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
					line.pop_back();
				if (!line.empty()) {
					std::lock_guard<std::mutex> lock(g_inMutex);
					if (g_inQueue.size() < kMaxQueuedLines) g_inQueue.push_back(line);
				}
			}
			if (held->size() > 64 * 1024) held->clear(); // a client sending no newlines at all
		}
	}

	if (!dead.empty()) {
		std::lock_guard<std::mutex> lock(g_clientsMutex);
		for (SOCKET s : dead) {
			for (auto it = g_clients.begin(); it != g_clients.end(); ++it) {
				if (*it == s) { g_clients.erase(it); break; }
			}
			CloseClient(s);
		}
		for (auto it = partials.begin(); it != partials.end();) {
			bool isDead = false;
			for (SOCKET s : dead) if (it->first == s) { isDead = true; break; }
			it = isDead ? partials.erase(it) : it + 1;
		}
	}
}

void AcceptThread() {
	while (!g_stopping.load()) {
		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(g_listenSocket, &readSet);
		timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 100 * 1000;

		int ready = select(0, &readSet, nullptr, nullptr, &tv);
		if (ready > 0 && FD_ISSET(g_listenSocket, &readSet)) {
			SOCKET client = accept(g_listenSocket, nullptr, nullptr);
			if (client != INVALID_SOCKET) {
				u_long nonBlocking = 1;
				ioctlsocket(client, FIONBIO, &nonBlocking);
				BOOL noDelay = TRUE;
				setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay,
				           sizeof(noDelay));
				{
					std::lock_guard<std::mutex> lock(g_clientsMutex);
					g_clients.push_back(client);
				}
				// The CS2 netcon client drains a banner for a moment after connecting and then
				// starts sending, so announce ourselves the same way. It also makes "did I connect
				// to the hook or to something else on this port" answerable at a glance.
					DL_BOOT("netcon: client accepted");
					Netcon_Write("AfxHookDeadlock netcon ready\n");
			}
		}

		ServiceClientReads();
	}
}

void WriteThread() {
	DL_BOOT("netcon: write thread running");
	while (!g_stopping.load()) {
		std::string line;
		bool have = false;
		{
			std::lock_guard<std::mutex> lock(g_outMutex);
			if (!g_outQueue.empty()) {
				line = g_outQueue.front();
				g_outQueue.pop_front();
				have = true;
			}
		}
		if (!have) {
			Sleep(5);
			continue;
		}

		// Serialize socket lifetime with the reader: a copied SOCKET can be closed and
		// reused by accept before send reaches it. Every send is nonblocking; a stalled
		// client is disconnected so a partial line cannot corrupt its next reply.
		{
			std::lock_guard<std::mutex> lock(g_clientsMutex);
			for (SOCKET s : g_clients) {
				size_t sent = 0;
				while (sent < line.size() && !g_stopping.load()) {
					int n = send(s, line.data() + sent, (int)(line.size() - sent), 0);
					if (n > 0) { sent += (size_t)n; continue; }
					g_linesDropped.fetch_add(1);
					shutdown(s, SD_BOTH); // the reader alone closes/reaps the socket
					break;
				}
			}
		}
		if (0 == g_linesSent.fetch_add(1)) {
			char msg[128];
			wsprintfA(msg, "netcon: first line written to %d client(s)", Netcon_ClientCount());
			DL_BOOT(msg);
		}
	}
}

} // namespace

bool Netcon_Start(unsigned short port) {
	if (g_running.load()) return true;

	g_port = port ? port : kDefaultPort;

	WSADATA wsa;
	if (0 != WSAStartup(MAKEWORD(2, 2), &wsa)) {
		advancedfx::Warning("AfxHookDeadlock: netcon WSAStartup failed.\n");
		return false;
	}

	g_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (g_listenSocket == INVALID_SOCKET) {
		advancedfx::Warning("AfxHookDeadlock: netcon socket() failed.\n");
		WSACleanup();
		return false;
	}

	// EXCLUSIVEADDRUSE, deliberately NOT SO_REUSEADDR.
	//
	// On Windows SO_REUSEADDR does not mean what it means on POSIX: it lets a SECOND process bind
	// an address another process is already listening on, and the OS then hands connections to one
	// of them without saying which. For a diagnostic console that is precisely wrong -- a leftover
	// game instance keeps answering, the new one looks dead, and every reading taken through it
	// belongs to the wrong process. Exclusive use turns the collision into an error instead, which
	// is information.
	BOOL exclusive = TRUE;
	setsockopt(g_listenSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
	           (const char*)&exclusive, sizeof(exclusive));

	sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(g_port);
	// Loopback only. This is a console with no authentication; it has no business listening on a
	// routable interface.
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (0 != bind(g_listenSocket, (sockaddr*)&addr, sizeof(addr))) {
		advancedfx::Warning(
			"AfxHookDeadlock: netcon could not bind 127.0.0.1:%u -- another process is already\n"
			"listening there. A leftover game instance is the usual cause; close it and relaunch.\n",
			(unsigned)g_port);
		closesocket(g_listenSocket);
		g_listenSocket = INVALID_SOCKET;
		WSACleanup();
		return false;
	}
	if (0 != listen(g_listenSocket, 4)) {
		advancedfx::Warning("AfxHookDeadlock: netcon listen() failed.\n");
		closesocket(g_listenSocket);
		g_listenSocket = INVALID_SOCKET;
		WSACleanup();
		return false;
	}

	u_long nonBlocking = 1;
	ioctlsocket(g_listenSocket, FIONBIO, &nonBlocking);

	g_stopping.store(false);
	g_running.store(true);
	g_acceptThread = std::thread(AcceptThread);
	g_writeThread = std::thread(WriteThread);

	RegisterLogListener();

	advancedfx::Message("AfxHookDeadlock: netcon listening on 127.0.0.1:%u\n", (unsigned)g_port);
	return true;
}

void Netcon_Stop() {
	if (!g_running.load()) return;
	g_stopping.store(true);

	UnregisterLogListener();

	if (g_acceptThread.joinable()) g_acceptThread.join();
	if (g_writeThread.joinable()) g_writeThread.join();

	DropAllClients();

	if (g_listenSocket != INVALID_SOCKET) {
		closesocket(g_listenSocket);
		g_listenSocket = INVALID_SOCKET;
	}
	WSACleanup();

	g_running.store(false);
	advancedfx::Message("AfxHookDeadlock: netcon stopped.\n");
}

bool Netcon_Active() { return g_running.load(); }
unsigned short Netcon_Port() { return g_port; }

int Netcon_ClientCount() {
	std::lock_guard<std::mutex> lock(g_clientsMutex);
	return (int)g_clients.size();
}

void Netcon_Write(const char* text) {
	if (!text || !g_running.load()) return;
	static bool firstQueued = true;
	if (firstQueued) {
		firstQueued = false;
		DL_BOOT("netcon: first line queued for writing");
	}
	std::lock_guard<std::mutex> lock(g_outMutex);
	if (g_outQueue.size() >= kMaxQueuedLines) {
		g_linesDropped.fetch_add(1);
		return;
	}
	g_outQueue.push_back(std::string(text));
}

void Netcon_PumpMainThread() {
	if (!g_running.load()) return;

	// Drain the whole queue each frame, but under a cap: a client that pastes a thousand commands
	// should not stall one frame for all of them.
	for (int i = 0; i < 32; ++i) {
		std::string cmd;
		{
			std::lock_guard<std::mutex> lock(g_inMutex);
			if (g_inQueue.empty()) return;
			cmd = g_inQueue.front();
			g_inQueue.pop_front();
		}
		// Our own commands are dispatched here, not through the engine. Engine_ExecuteClientCmd
		// goes through a vtable slot that is still a hypothesis on Deadlock, and a wrong slot does
		// not fault -- it returns "ok" and runs nothing. Routing `mirv_deadlock` through it made
		// the one command that reports whether anything works depend on the one thing least
		// verified.
		const bool mine = Deadlock::Commands_DispatchLine(cmd.c_str());
		const bool ok = mine || Engine_ExecuteClientCmd(cmd.c_str());

		// Into the BOOT trace, not the console. If the command path is the thing that is broken,
		// console output is exactly the channel that cannot tell us so -- which is how this sat
		// looking like "netcon accepts commands and silently ignores them" for two launches.
		{
			char line[320];
			wsprintfA(line, "netcon: exec %.240s -> %s", cmd.c_str(),
			          mine ? "ok (dll)" : (ok ? "ok (engine)" : "FAILED"));
			DL_BOOT(line);
		}

		if (ok)
			g_commandsExecuted.fetch_add(1);
		else
			advancedfx::Warning("AfxHookDeadlock: netcon could not execute '%s'.\n", cmd.c_str());
	}
}

NetconStats Netcon_GetStats() {
	NetconStats s;
	s.active = g_running.load();
	s.port = g_port;
	s.clients = Netcon_ClientCount();
	s.commandsExecuted = g_commandsExecuted.load();
	s.linesSent = g_linesSent.load();
	s.linesDropped = g_linesDropped.load();
	return s;
}

} // namespace Deadlock
