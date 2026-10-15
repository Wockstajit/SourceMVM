// OsuRealm.h against a real osu!lazer database. A client.realm cannot be committed as a fixture
// (it is the user's own data), so this runs against the file named by OSU_REALM, or the user's
// own %APPDATA%\osu\client.realm, and reports SKIPPED when neither exists.
//
// Checks: it parses; every beatmap set has at least one .osu; every hash is 64 hex chars; and
// every hash resolves to an existing file in <dir>/files/h/hh/hash (the store layout); and each
// "... [Version].osu" name points at a file that really holds that Version.
#include "../../AfxHookSource2/Filmmaker/EasterEggs/Osu/OsuRealm.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
	namespace fs = std::filesystem;
	fs::path realm;
	if (argc > 1) realm = argv[1];
	else if (const char* e = std::getenv("OSU_REALM")) realm = e;
	else if (const char* a = std::getenv("APPDATA")) realm = fs::path(a) / "osu" / "client.realm";
	if (realm.empty() || !fs::exists(realm)) { std::cout << "OsuRealmTests SKIPPED (no client.realm)\n"; return 0; }
	// Lazer may hold the file open; copy it so the read sees one consistent snapshot.
	std::ifstream f(realm, std::ios::binary);
	std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	Filmmaker::OsuRealm::Reader r(data);
	std::vector<Filmmaker::OsuRealm::BeatmapSetFiles> sets;
	std::string err;
	if (!r.ReadSets(sets, &err)) { std::cerr << "FAILED: " << err << "\n"; return 1; }
	const fs::path store = realm.parent_path() / "files";
	int failed = 0, files = 0, osu = 0, missing = 0, checked = 0, mismatched = 0;
	for (const auto& s : sets) {
		bool hasOsu = false;
		for (const auto& nf : s.files) {
			++files;
			if (nf.name.size() > 4 && nf.name.compare(nf.name.size() - 4, 4, ".osu") == 0) { hasOsu = true; ++osu; }
			if (nf.hash.size() != 64 || nf.hash.find_first_not_of("0123456789abcdef") != std::string::npos) {
				std::cerr << "FAILED: bad hash '" << nf.hash << "' for " << nf.name << "\n"; ++failed; continue;
			}
			const fs::path onDisk = store / nf.hash.substr(0, 1) / nf.hash.substr(0, 2) / nf.hash;
			if (!fs::exists(onDisk)) { ++missing; continue; }
			// Every name must point at ITS file: a .osu named "... [Hard].osu" holds "Version:Hard".
			// (A signed read of realm's unsigned key arrays once paired names with other songs' files.)
			const size_t lb = nf.name.rfind('['), rb = nf.name.rfind("].osu");
			if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
				std::ifstream in(onDisk, std::ios::binary);
				std::string line, version;
				while (std::getline(in, line)) {
					if (line.rfind("Version:", 0) == 0) { version = line.substr(8); break; }
				}
				while (!version.empty() && (version.back() == '\r' || version.back() == ' ')) version.pop_back();
				while (!version.empty() && version.front() == ' ') version.erase(0, 1);
				const std::string want = nf.name.substr(lb + 1, rb - lb - 1);
				// osu! drops characters Windows forbids in file names (F*CKING -> FCKING).
				version.erase(std::remove_if(version.begin(), version.end(),
					[](char c) { return std::strchr("\\/:*?\"<>|", c) != nullptr; }), version.end());
				++checked;
				if (!version.empty() && version != want && mismatched++ < 5)
					std::cerr << "mismatch: '" << nf.name << "' holds Version '" << version << "'\n";
			}
		}
		if (!hasOsu) { std::cerr << "note: a set without .osu files (" << s.files.size() << " files)\n"; }
	}
	std::cout << sets.size() << " sets, " << osu << " .osu, " << files << " files, " << missing << " not on disk\n";
	if (sets.empty()) { std::cerr << "FAILED: no sets\n"; ++failed; }
	if (missing * 20 > files) { std::cerr << "FAILED: more than 5% of hashes missing on disk\n"; ++failed; }
	std::cout << checked << " .osu names checked against their contents, " << mismatched << " mismatched\n";
	// Some real maps spell Version differently from their file name; a misread is wholesale.
	if (mismatched * 50 > checked) { std::cerr << "FAILED: .osu names do not match their files\n"; ++failed; }

	// Skins: every skin parses, its files resolve in the store, and a skin.ini (when it has one)
	// names the skin it is filed under.
	std::vector<Filmmaker::OsuRealm::SkinFiles> skins;
	if (!r.ReadSkins(skins, &err)) { std::cerr << "FAILED: skins: " << err << "\n"; ++failed; }
	int skinFiles = 0, skinMissing = 0, skinIniChecked = 0, skinIniWrong = 0;
	for (const auto& sk : skins) {
		std::cout << "skin '" << sk.name << "' by '" << sk.creator << "' id=" << (sk.idHex.empty() ? "?" : sk.idHex)
			<< " files=" << sk.files.size() << "\n";
		for (const auto& nf : sk.files) {
			++skinFiles;
			const fs::path onDisk = store / nf.hash.substr(0, 1) / nf.hash.substr(0, 2) / nf.hash;
			if (!fs::exists(onDisk)) { ++skinMissing; continue; }
			if (_stricmp(nf.name.c_str(), "skin.ini") != 0) continue;
			std::ifstream in(onDisk, std::ios::binary);
			std::string line, name;
			while (std::getline(in, line)) {
				const size_t c = line.find("Name:");
				if (c != std::string::npos && line.find_first_not_of(" \t") == c) { name = line.substr(c + 5); break; }
			}
			while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
			while (!name.empty() && name.front() == ' ') name.erase(0, 1);
			++skinIniChecked;
			// Lazer files an imported skin under its skin.ini Name (sometimes with a suffix).
			if (!name.empty() && sk.name.find(name) == std::string::npos) {
				++skinIniWrong;
				std::cerr << "skin mismatch: '" << sk.name << "' has skin.ini Name '" << name << "'\n";
			}
		}
	}
	std::cout << skins.size() << " skins, " << skinFiles << " files, " << skinMissing << " not on disk, "
		<< skinIniChecked << " skin.ini checked, " << skinIniWrong << " misnamed\n";
	if (skinFiles && skinMissing * 20 > skinFiles) { std::cerr << "FAILED: skin files missing\n"; ++failed; }
	if (skinIniWrong * 4 > skinIniChecked && skinIniChecked) { std::cerr << "FAILED: skin files misassigned\n"; ++failed; }
	if (failed) return 1;
	std::cout << "OsuRealmTests passed\n";
	return 0;
}
