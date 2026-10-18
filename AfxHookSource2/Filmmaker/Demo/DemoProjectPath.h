#pragma once

#include <string>
#include <cstdint>

namespace Filmmaker {

// Returns the stable path used to key project sidecars. A validated demo loaded from the
// repair cache maps back to its source demo; every other path is returned unchanged.
std::wstring ResolveDemoProjectPath(const std::wstring& playingDemoPath);

// Publish after a project/repair marker is written or removed. Never takes the store lock.
uint64_t DemoProjectPathRevision();
void InvalidateDemoProjectPathCache();

} // namespace Filmmaker
