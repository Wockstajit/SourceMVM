#include "HttpRequest.h"

#include "TextEncoding.h"

#include <Windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace Filmmaker {

HttpResponse HttpRequest(const char* method, const std::string& url, const std::string& contentType,
	const std::string& body, size_t maxBody) {
	HttpResponse r;
	if (url.rfind("https://", 0) != 0) { r.error = "not an https URL"; return r; }
	const std::wstring wurl = Utf8ToWide(url);
	URL_COMPONENTS parts{};
	parts.dwStructSize = sizeof(parts);
	wchar_t host[256] = {}, path[4096] = {};
	parts.lpszHostName = host; parts.dwHostNameLength = 255;
	parts.lpszUrlPath = path; parts.dwUrlPathLength = 4095;
	if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) { r.error = "bad URL"; return r; }

	HINTERNET session = WinHttpOpen(L"SOURCE-MVM/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
	HINTERNET request = connect ? WinHttpOpenRequest(connect, Utf8ToWide(method).c_str(), path, nullptr,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
	if (request) {
		WinHttpSetTimeouts(request, 10000, 10000, 30000, 30000);
		std::wstring headers;
		if (!contentType.empty()) headers = L"Content-Type: " + Utf8ToWide(contentType) + L"\r\n";
		const BOOL sent = WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
			headers.empty() ? 0 : DWORD(-1), body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
			DWORD(body.size()), DWORD(body.size()), 0);
		if (sent && WinHttpReceiveResponse(request, nullptr)) {
			DWORD status = 0, size = sizeof(status);
			WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
				&status, &size, WINHTTP_NO_HEADER_INDEX);
			r.status = status;
			DWORD available = 0;
			while (WinHttpQueryDataAvailable(request, &available) && available > 0 && r.body.size() < maxBody) {
				std::string chunk(available, '\0');
				DWORD read = 0;
				if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
				r.body.append(chunk.data(), read);
			}
			if (r.body.size() > maxBody) r.body.resize(maxBody);
		} else {
			r.error = "no connection (error " + std::to_string(GetLastError()) + ")";
		}
	} else {
		r.error = "could not open a connection";
	}
	if (request) WinHttpCloseHandle(request);
	if (connect) WinHttpCloseHandle(connect);
	if (session) WinHttpCloseHandle(session);
	return r;
}

} // namespace Filmmaker
