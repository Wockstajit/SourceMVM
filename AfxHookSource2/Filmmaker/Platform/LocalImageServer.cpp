#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>

#include "LocalImageServer.h"

#include "../Debug/MvmDebugLog.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace Filmmaker::LocalImageServer {

namespace {

std::mutex g_mx;
std::map<std::string, Source> g_sources; // prefix -> source
std::once_flag g_startOnce;
std::atomic<int> g_port{ 0 };
std::string g_token; // written once, before g_port is published
std::atomic<unsigned> g_served{ 0 };

void Serve(SOCKET c) {
	char req[2048];
	int got = 0;
	while (got < (int)sizeof(req) - 1) {
		const int n = recv(c, req + got, (int)sizeof(req) - 1 - got, 0);
		if (n <= 0) break;
		got += n;
		req[got] = 0;
		if (strstr(req, "\r\n\r\n")) break;
	}
	req[got] = 0;
	std::string path;
	if (got > 4 && strncmp(req, "GET ", 4) == 0) {
		const char* sp = strchr(req + 4, ' ');
		if (sp) path.assign(req + 4, (size_t)(sp - (req + 4)));
	}
	const size_t query = path.find('?');
	const std::string version = query != std::string::npos ? path.substr(query) : std::string();
	if (query != std::string::npos) path.resize(query); // cache-busting versions
	// /<token>/<prefix>/<name>
	std::string body, type = "application/octet-stream";
	bool ok = false;
	const std::string root = "/" + g_token + "/";
	if (!g_token.empty() && path.compare(0, root.size(), root) == 0) {
		const std::string rest = path.substr(root.size());
		const size_t slash = rest.find('/');
		if (slash != std::string::npos && slash > 0) {
			Source source;
			{
				std::lock_guard<std::mutex> lock(g_mx);
				auto it = g_sources.find(rest.substr(0, slash));
				if (it != g_sources.end()) source = it->second;
			}
			if (source) ok = source(rest.substr(slash + 1), body, type) && !body.empty();
		}
	}
	char head[256];
	if (ok) {
		snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
			"Cache-Control: max-age=86400\r\nConnection: close\r\n\r\n", type.c_str(), body.size());
		g_served.fetch_add(1);
	} else {
		snprintf(head, sizeof(head), "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
	}
	send(c, head, (int)strlen(head), 0);
	size_t off = 0;
	while (off < body.size()) {
		const int n = send(c, body.data() + off, (int)std::min<size_t>(body.size() - off, 1 << 16), 0);
		if (n <= 0) break;
		off += (size_t)n;
	}
	shutdown(c, SD_SEND);
	closesocket(c);
	// One line per request (the token stays out of the log): how often Panorama asks, and for what.
	if (MvmDebugLog_Active()) {
		const size_t name = path.find('/', g_token.empty() ? 0 : g_token.size() + 2);
		MvmDebugLog_Linef("image.server", "%s %s%s bytes=%zu", ok ? "served" : "missing", name == std::string::npos ? "?" : path.c_str() + name + 1,
			version.c_str(), body.size());
	}
}

void Start() {
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) return;
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // loopback only: never reachable from the network
	a.sin_port = 0;                              // any free port
	int len = sizeof(a);
	if (bind(s, (sockaddr*)&a, sizeof(a)) != 0 || listen(s, 16) != 0 || getsockname(s, (sockaddr*)&a, &len) != 0) {
		closesocket(s);
		return;
	}
	std::random_device rd;
	char tok[33];
	snprintf(tok, sizeof(tok), "%08x%08x%08x%08x", rd(), rd(), rd(), rd());
	g_token = tok;
	g_port.store(ntohs(a.sin_port));
	std::thread([s] {
		for (;;) {
			SOCKET c = accept(s, nullptr, nullptr);
			if (c == INVALID_SOCKET) { Sleep(50); continue; }
			Serve(c);
		}
	}).detach();
	MvmDebugLog_LinefAlways("image.server", "image server on 127.0.0.1:%d", g_port.load());
}

} // namespace

void Register(const std::string& prefix, Source source) {
	{
		std::lock_guard<std::mutex> lock(g_mx);
		g_sources[prefix] = std::move(source);
	}
	std::call_once(g_startOnce, Start);
}

std::string Url(const std::string& prefix) {
	const int port = g_port.load();
	if (port == 0) return std::string();
	return "http://127.0.0.1:" + std::to_string(port) + "/" + g_token + "/" + prefix + "/";
}

int Port() { return g_port.load(); }
unsigned Served() { return g_served.load(); }

} // namespace Filmmaker::LocalImageServer
