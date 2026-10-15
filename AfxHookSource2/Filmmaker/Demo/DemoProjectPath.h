#pragma once

#include <string>

namespace Filmmaker {

// Returns the stable path used to key project sidecars. A validated demo loaded from the
// repair cache maps back to its source demo; every other path is returned unchanged.
std::wstring ResolveDemoProjectPath(const std::wstring& playingDemoPath);

} // namespace Filmmaker
