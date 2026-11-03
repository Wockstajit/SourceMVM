#pragma once

// Recursively discovers *.dem files under a set of root folders.
// Stateless helper; threading/lifetime is owned by DemoLibrary.

#include <string>
#include <vector>
#include <atomic>

namespace Filmmaker {

class DemoScanner {
public:
	// Recursively scans every root for *.dem files (case-insensitive), de-duplicated.
	// Honors `cancel`: returns early (possibly partial) when it becomes true. A folder in `skip`
	// (full path, any case or separator) is not entered: CS:S's converted copies of old demos.
	static std::vector<std::wstring> Scan(const std::vector<std::wstring>& roots, const std::atomic<bool>& cancel,
		const std::vector<std::wstring>& skip = {});
};

} // namespace Filmmaker
