#include "stdafx.h"

// WINSOCK2 FIRST, before anything that can pull in windows.h: without WIN32_LEAN_AND_MEAN,
// windows.h brings the original winsock.h, and every winsock2 declaration then collides.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "NetconServer.h"

#include "AfxConsole.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace advancedfx {
namespace NetconServer {

namespace {

// Bounded on purpose: a stalled reader must not be able to slow the game down, so output is
// dropped rather than grown without limit, and the drop count is reported.
const size_t kMaxQueuedLines = 4096;

std::atomic<bool> g_running{ false };
std::atomic<bool> g_stopping{ false };
Config g_config;
std::string g_banner;

SOCKET g_listenSocket = INVALID_SOCKET;

std::mutex g_clientsMutex;
std::vector<SOCKET> g_clients;

std::mutex g_outMutex;
std::deque<std::string> g_outQueue;

std::mutex g_inMutex;
std::deque<std::string> g_inQueue;

std::thread g_acceptThread;
std::thread g_writeThread;
// A process that exits without Stop() must not abort: Windows ends the threads before this module's
// destructors run, and a std::thread destroyed while joinable calls std::terminate. Declared after the
// threads, so it is destroyed first.
struct DetachAtExit {
	~DetachAtExit() {
		if (g_acceptThread.joinable()) g_acceptThread.detach();
		if (g_writeThread.joinable()) g_writeThread.detach();
	}
} g_detachAtExit;

std::atomic<unsigned long long> g_commandsReceived{ 0 };
std::atomic<unsigned long long> g_commandsExecuted{ 0 };
std::atomic<unsigned long long> g_linesSent{ 0 };
std::atomic<unsigned long long> g_linesDropped{ 0 };

void CloseClient(SOCKET s) {
	shutdown(s, SD_BOTH);
	closesocket(s);
}

void DropAllClients() {
	std::lock_guard<std::mutex> lock(g_clientsMutex);
	for (SOCKET s : g_clients) CloseClient(s);
	g_clients.clear();
}

// Reads whatever the clients sent and splits it into lines. A partial line is held per client
// until its terminator arrives, because a batched send can land split across recv calls.
void ServiceClientReads() {
	static std::vector<std::pair<SOCKET, std::string>> partials;
	std::vector<SOCKET> dead;
	bool received = false;

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
				if (WSAGetLastError() != WSAEWOULDBLOCK) dead.push_back(s);
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
				if (line.empty()) continue;
				std::lock_guard<std::mutex> lock(g_inMutex);
				if (g_inQueue.size() < kMaxQueuedLines) {
					g_inQueue.push_back(line);
					g_commandsReceived.fetch_add(1);
					received = true;
				}
			}
			if (held->size() > 64 * 1024) held->clear(); // a client that never sends a newline
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

	if (received && g_config.onLineReceived) g_config.onLineReceived();
}

void AcceptThread() {
	while (!g_stopping.load()) {
		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(g_listenSocket, &readSet);
		timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 50 * 1000;

		int ready = select(0, &readSet, nullptr, nullptr, &tv);
		if (ready > 0 && FD_ISSET(g_listenSocket, &readSet)) {
			SOCKET client = accept(g_listenSocket, nullptr, nullptr);
			if (client != INVALID_SOCKET) {
				u_long nonBlocking = 1;
				ioctlsocket(client, FIONBIO, &nonBlocking);
				BOOL noDelay = TRUE;
				setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
				{
					std::lock_guard<std::mutex> lock(g_clientsMutex);
					g_clients.push_back(client);
				}
				if (!g_banner.empty()) Write(g_banner.c_str());
			}
		}

		ServiceClientReads();
	}
}

void WriteThread() {
	while (!g_stopping.load()) {
		std::string line;
		{
			std::lock_guard<std::mutex> lock(g_outMutex);
			if (!g_outQueue.empty()) {
				line = std::move(g_outQueue.front());
				g_outQueue.pop_front();
			}
		}
		if (line.empty()) {
			Sleep(5);
			continue;
		}

		// Serialized with the reader: a copied SOCKET could be closed and reused by accept before
		// send reaches it. Sends are nonblocking; a stalled client is shut down so a partial line
		// cannot corrupt its next reply.
		{
			std::lock_guard<std::mutex> lock(g_clientsMutex);
			for (SOCKET s : g_clients) {
				size_t sent = 0;
				while (sent < line.size() && !g_stopping.load()) {
					int n = send(s, line.data() + sent, (int)(line.size() - sent), 0);
					if (n > 0) { sent += (size_t)n; continue; }
					g_linesDropped.fetch_add(1);
					shutdown(s, SD_BOTH); // the reader alone closes and reaps the socket
					break;
				}
			}
		}
		g_linesSent.fetch_add(1);
	}
}

} // namespace

bool Start(const Config& config) {
	if (g_running.load()) return true;

	g_config = config;
	g_banner = config.banner ? config.banner : "";
	const char* name = config.name ? config.name : "netcon";

	WSADATA wsa;
	if (0 != WSAStartup(MAKEWORD(2, 2), &wsa)) {
		advancedfx::Warning("%s: WSAStartup failed.\n", name);
		return false;
	}

	g_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (g_listenSocket == INVALID_SOCKET) {
		advancedfx::Warning("%s: socket() failed.\n", name);
		WSACleanup();
		return false;
	}

	// EXCLUSIVEADDRUSE, deliberately not SO_REUSEADDR: on Windows the latter lets a second process
	// bind a port another one is listening on, and connections then go to either. A leftover game
	// would keep answering while the new one looked dead. Exclusive use makes that an error.
	BOOL exclusive = TRUE;
	setsockopt(g_listenSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));

	sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(config.port);
	// Loopback only: a console with no authentication has no business on a routable interface.
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (0 != bind(g_listenSocket, (sockaddr*)&addr, sizeof(addr))) {
		advancedfx::Warning(
			"%s: could not bind 127.0.0.1:%u. Another process is listening there; a leftover game\n"
			"instance is the usual cause. Close it and relaunch.\n",
			name, (unsigned)config.port);
		closesocket(g_listenSocket);
		g_listenSocket = INVALID_SOCKET;
		WSACleanup();
		return false;
	}
	if (0 != listen(g_listenSocket, 4)) {
		advancedfx::Warning("%s: listen() failed.\n", name);
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
	return true;
}

void Stop() {
	if (!g_running.load()) return;
	g_stopping.store(true);

	if (g_acceptThread.joinable()) g_acceptThread.join();
	if (g_writeThread.joinable()) g_writeThread.join();

	DropAllClients();

	if (g_listenSocket != INVALID_SOCKET) {
		closesocket(g_listenSocket);
		g_listenSocket = INVALID_SOCKET;
	}
	WSACleanup();

	g_running.store(false);
}

bool Active() { return g_running.load(); }
unsigned short Port() { return g_config.port; }

int ClientCount() {
	std::lock_guard<std::mutex> lock(g_clientsMutex);
	return (int)g_clients.size();
}

bool PopCommand(std::string& outCommand) {
	std::lock_guard<std::mutex> lock(g_inMutex);
	if (g_inQueue.empty()) return false;
	outCommand = std::move(g_inQueue.front());
	g_inQueue.pop_front();
	return true;
}

bool HasPendingCommands() {
	std::lock_guard<std::mutex> lock(g_inMutex);
	return !g_inQueue.empty();
}

void Write(const char* text) {
	if (!text || !*text || !g_running.load()) return;
	std::lock_guard<std::mutex> lock(g_outMutex);
	if (g_outQueue.size() >= kMaxQueuedLines) {
		g_linesDropped.fetch_add(1);
		return;
	}
	g_outQueue.push_back(std::string(text));
}

void CountExecuted() { g_commandsExecuted.fetch_add(1); }

Stats GetStats() {
	Stats s;
	s.active = g_running.load();
	s.port = g_config.port;
	s.clients = ClientCount();
	s.commandsReceived = g_commandsReceived.load();
	s.commandsExecuted = g_commandsExecuted.load();
	s.linesSent = g_linesSent.load();
	s.linesDropped = g_linesDropped.load();
	return s;
}

} // namespace NetconServer
} // namespace advancedfx
