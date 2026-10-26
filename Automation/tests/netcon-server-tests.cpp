// Shared/NetconServer against a real loopback client: the banner, lines split across sends and
// ended by CRLF, output reaching the client, the counters, and refusing a port someone else holds.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "../../Shared/NetconServer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

static void Check(bool ok, const char* what) {
	if (ok) return;
	std::fprintf(stderr, "FAILED: %s\n", what);
	std::exit(1);
}

static std::atomic<int> g_wakes{ 0 };
static void OnLine() { g_wakes.fetch_add(1); }

static SOCKET Connect(unsigned short port) {
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (0 != connect(s, (sockaddr*)&addr, sizeof(addr))) {
		closesocket(s);
		return INVALID_SOCKET;
	}
	DWORD timeoutMs = 2000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
	return s;
}

// Reads until `expected` has arrived or two seconds pass.
static bool ReceiveUntil(SOCKET s, const char* expected, std::string& received) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (received.find(expected) == std::string::npos) {
		if (std::chrono::steady_clock::now() > deadline) return false;
		char buf[256];
		int n = recv(s, buf, sizeof(buf), 0);
		if (n <= 0) return false;
		received.append(buf, (size_t)n);
	}
	return true;
}

static bool PopWithin(std::string& out) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < deadline) {
		if (advancedfx::NetconServer::PopCommand(out)) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return false;
}

int main() {
	WSADATA wsa;
	Check(0 == WSAStartup(MAKEWORD(2, 2), &wsa), "WSAStartup");

	const unsigned short port = 29097;
	advancedfx::NetconServer::Config config;
	config.port = port;
	config.banner = "test netcon ready\n";
	config.name = "netcon test";
	config.onLineReceived = &OnLine;

	Check(advancedfx::NetconServer::Start(config), "start");
	Check(advancedfx::NetconServer::Start(config), "a second start is a no-op");
	Check(advancedfx::NetconServer::Active() && advancedfx::NetconServer::Port() == port, "active on its port");

	SOCKET client = Connect(port);
	Check(client != INVALID_SOCKET, "client connects");
	std::string received;
	Check(ReceiveUntil(client, "test netcon ready\n", received), "banner arrives on connect");

	// One command split across two sends, the second ended by CRLF, plus a blank line.
	const char* first = "first\nsec";
	const char* second = "ond\r\n\n";
	send(client, first, (int)std::strlen(first), 0);
	std::this_thread::sleep_for(std::chrono::milliseconds(80));
	send(client, second, (int)std::strlen(second), 0);

	std::string command;
	Check(PopWithin(command) && command == "first", "first line");
	Check(PopWithin(command) && command == "second", "line split across sends, CR trimmed");
	Check(!advancedfx::NetconServer::PopCommand(command), "blank lines are dropped");
	Check(!advancedfx::NetconServer::HasPendingCommands(), "queue drained");
	Check(g_wakes.load() >= 1, "the owner was woken for received lines");

	advancedfx::NetconServer::CountExecuted();
	advancedfx::NetconServer::Write("reply line\n");
	received.clear();
	Check(ReceiveUntil(client, "reply line\n", received), "output reaches the client");

	const advancedfx::NetconServer::Stats stats = advancedfx::NetconServer::GetStats();
	Check(stats.active && stats.clients == 1, "one client");
	Check(stats.commandsReceived == 2 && stats.commandsExecuted == 1, "received and executed counts");
	Check(stats.linesDropped == 0, "nothing dropped");

	closesocket(client);
	advancedfx::NetconServer::Stop();
	Check(!advancedfx::NetconServer::Active(), "stopped");

	// A port another process already listens on is refused instead of shared: a leftover game
	// would otherwise keep answering for the new one.
	SOCKET squatter = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	BOOL exclusive = TRUE;
	setsockopt(squatter, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	Check(0 == bind(squatter, (sockaddr*)&addr, sizeof(addr)) && 0 == listen(squatter, 1), "squatter binds");
	Check(!advancedfx::NetconServer::Start(config), "a held port is refused");
	Check(!advancedfx::NetconServer::Active(), "not active after the refusal");
	closesocket(squatter);

	WSACleanup();
	std::printf("NetconServerTests passed\n");
	return 0;
}
