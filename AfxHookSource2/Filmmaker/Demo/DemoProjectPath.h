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

// Canonicalize a path to the filesystem's true-cased absolute form (GetFinalPathNameByHandle,
// \\?\ prefix stripped) so two independently-derived strings for the SAME file compare equal
// and produce the same sidecar. Falls back to GetFullPathName when the file can't be opened.
std::wstring CanonicalDemoPath(const std::wstring& path);

} // namespace Filmmaker
