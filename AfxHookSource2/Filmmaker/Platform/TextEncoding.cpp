#include "TextEncoding.h"

#include <Windows.h>

namespace Filmmaker {

std::string WideToUtf8(const std::wstring& w) {
	if (w.empty())
		return std::string();
	int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	if (needed <= 0)
		return std::string();
	std::string out((size_t)needed, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], needed, nullptr, nullptr);
	return out;
}

std::wstring Utf8ToWide(const std::string& s) {
	if (s.empty())
		return std::wstring();
	int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	if (needed <= 0)
		return std::wstring();
	std::wstring out((size_t)needed, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], needed);
	return out;
}

} // namespace Filmmaker
