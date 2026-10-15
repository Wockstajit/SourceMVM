#pragma once

// One blocking HTTPS request through WinHTTP (system proxy settings, TLS). Call it from a worker
// thread, never the game's: it waits on the network. Used by the bug report upload and the HC
// Tracker feed (Filmmaker/Community).

#include <string>

namespace Filmmaker {

struct HttpResponse {
	unsigned long status = 0;  // HTTP status; 0 when no answer came back
	std::string body;          // response body, at most maxBody bytes
	std::string error;         // why there was no answer, when status is 0
};

// `url` must be https. `contentType`/`body` may be empty (a GET).
HttpResponse HttpRequest(const char* method, const std::string& url, const std::string& contentType = {},
	const std::string& body = {}, size_t maxBody = 1024 * 1024);

} // namespace Filmmaker
