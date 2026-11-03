#include "../../AfxHookSource2/Filmmaker/Demo/DemoScanner.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace fs = std::filesystem;

namespace {

void Touch(const fs::path& path) {
	fs::create_directories(path.parent_path());
	std::ofstream(path, std::ios::binary).put('\0');
}

std::wstring Key(const fs::path& path) {
	std::wstring value = fs::weakly_canonical(path).wstring();
	for (wchar_t& ch : value)
		ch = (wchar_t)towlower(ch);
	return value;
}

} // namespace

int main() {
	const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	const fs::path root = fs::temp_directory_path() / ("source-mvm-demo-scanner-" + std::to_string(stamp));

	try {
		const fs::path rootDemo = root / "root.dem";
		const fs::path nestedDemo = root / "normal" / "nested.DEM";
		const fs::path repairedDemo = root / "repaired" / "cached.dem";
		const fs::path nestedRepairedDemo = root / "normal" / "RePaIrEd" / "cached.dem";
		Touch(rootDemo);
		Touch(nestedDemo);
		Touch(repairedDemo);
		Touch(nestedRepairedDemo);
		Touch(root / "normal" / "not-a-demo.txt");

		const fs::path projectDemo = root / "project" / "repaired.dem";
		Touch(projectDemo);
		Touch(root / "project" / "project.mvm");

		std::atomic<bool> cancel{ false };
		const auto found = Filmmaker::DemoScanner::Scan({ root.wstring() }, cancel);
		std::set<std::wstring> keys;
		for (const auto& path : found)
			keys.insert(Key(path));

		if (keys.size() != 2 || !keys.count(Key(rootDemo)) || !keys.count(Key(nestedDemo)) ||
			keys.count(Key(repairedDemo)) || keys.count(Key(nestedRepairedDemo)) ||
			keys.count(Key(projectDemo))) {
			std::cerr << "DemoScanner returned repaired-cache files or missed valid demos.\n";
			fs::remove_all(root);
			return 1;
		}

		// A skipped folder is not entered, matched in any case and with either separator: CS:S keeps
		// its converted copies of old demos in <game>\source_mvm\converted\.
		const fs::path convertedDemo = root / "source_mvm" / "converted" / "replays" / "old.dem";
		Touch(convertedDemo);
		std::wstring skipFolder = (root / "SOURCE_MVM" / "Converted").wstring();
		std::replace(skipFolder.begin(), skipFolder.end(), L'\\', L'/');
		const auto skipped = Filmmaker::DemoScanner::Scan({ root.wstring() }, cancel, { skipFolder + L"/" });
		std::set<std::wstring> skippedKeys;
		for (const auto& path : skipped)
			skippedKeys.insert(Key(path));
		const auto all = Filmmaker::DemoScanner::Scan({ root.wstring() }, cancel);
		if (skippedKeys.size() != 2 || skippedKeys.count(Key(convertedDemo)) || all.size() != 3) {
			std::cerr << "DemoScanner entered a skipped folder, or skipped one it was not given.\n";
			fs::remove_all(root);
			return 1;
		}

		fs::remove_all(root);
		std::cout << "DemoScanner repaired-directory and skip-folder exclusion passed.\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << e.what() << '\n';
		std::error_code ec;
		fs::remove_all(root, ec);
		return 1;
	}
}
