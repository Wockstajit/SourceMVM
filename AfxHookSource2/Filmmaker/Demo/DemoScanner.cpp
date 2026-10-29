#include "DemoScanner.h"

#include "DemoProjectFileMath.h"
#include "DemoProjectStoreMath.h"

#include <filesystem>
#include <algorithm>
#include <unordered_set>

namespace fs = std::filesystem;

namespace Filmmaker {

namespace {

bool HasDemExtension(const fs::path& p) {
	std::wstring ext = p.extension().wstring();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return towlower(c); });
	return ext == L".dem";
}

std::wstring KeyFor(const fs::path& p) {
	std::wstring s = p.wstring();
	std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return towlower(c); });
	return s;
}

// A folder's key for the skip list: lower case, backslash separators, no trailing separator.
std::wstring FolderKey(const fs::path& p) {
	std::wstring s = KeyFor(p.lexically_normal());
	std::replace(s.begin(), s.end(), L'/', L'\\');
	while (s.size() > 1 && s.back() == L'\\')
		s.pop_back();
	return s;
}

} // namespace

std::vector<std::wstring> DemoScanner::Scan(const std::vector<std::wstring>& roots, const std::atomic<bool>& cancel,
	const std::vector<std::wstring>& skip) {
	std::vector<std::wstring> found;
	std::unordered_set<std::wstring> seen;
	std::unordered_set<std::wstring> skipped;
	for (const auto& folder : skip)
		skipped.insert(FolderKey(fs::path(folder)));

	for (const auto& root : roots) {
		if (cancel.load())
			break;
		std::error_code ec;
		fs::path rootPath(root);
		if (!fs::exists(rootPath, ec) || !fs::is_directory(rootPath, ec))
			continue;

		// recursive_directory_iterator with skip_permission_denied so locked
		// subfolders don't abort the whole scan.
		auto opts = fs::directory_options::skip_permission_denied;
		fs::recursive_directory_iterator it(rootPath, opts, ec), end;
		for (; it != end; it.increment(ec)) {
			if (cancel.load())
				break;
			if (ec) { ec.clear(); continue; }

			std::error_code statEc;
			const fs::directory_entry& entry = *it;

			// Generated copies are not demos: skip leftover "repaired\" folders and the source_mvm
			// project copies (repaired.dem, keyframed.dem -- those sit next to project.mvm).
			if (entry.is_directory(statEc)) {
				if (IsLegacyRepairedFolderName(entry.path().filename().wstring())
					|| (!skipped.empty() && skipped.count(FolderKey(entry.path())))) {
					it.disable_recursion_pending();
				}
				continue;
			}
			if (!entry.is_regular_file(statEc))
				continue;
			const fs::path& p = entry.path();
			if (!HasDemExtension(p))
				continue;
			if (IsProjectDemoCopyFileName(p.filename().wstring())) {
				std::error_code projectEc;
				if (fs::is_regular_file(p.parent_path() / kProjectFileName, projectEc))
					continue;
			}

			std::wstring key = KeyFor(p);
			if (seen.insert(key).second)
				found.push_back(p.wstring());
		}
	}

	return found;
}

} // namespace Filmmaker
