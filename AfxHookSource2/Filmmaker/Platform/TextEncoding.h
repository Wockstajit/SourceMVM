#pragma once

// Small UTF-8 <-> UTF-16 helpers for the filmmaker module, so paths can be
// stored as wide strings (correct for the Windows filesystem) but passed to
// the console / Panorama as UTF-8.

#include <string>

namespace Filmmaker {

std::string WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& s);

} // namespace Filmmaker
