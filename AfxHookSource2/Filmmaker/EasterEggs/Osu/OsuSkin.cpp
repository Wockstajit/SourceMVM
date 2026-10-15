#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>

#include "OsuSkin.h"

#include "OsuLibrary.h"
#include "OsuMath.h"
#include "OsuPlatform.h"
#include "OsuRealm.h"

#include "../../Platform/JsonBuilder.h"
#include "../../Platform/TextEncoding.h"
#include "../../Debug/MvmDebugLog.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace Filmmaker {
namespace OsuSkin {
namespace {

namespace fs = std::filesystem;

struct Image { int w = 0, h = 0; };

std::mutex g_mx;
std::atomic<bool> g_ready{ false }, g_loading{ false };
std::string g_name;                          // "" = built-in look
std::map<std::string, fs::path> g_files;     // lower-case file name -> file on disk
std::map<std::string, Image> g_images;       // lower-case .png name -> size
std::map<std::string, std::string> g_ini;    // lower-case skin.ini key -> value (last section wins)
std::vector<int> g_combo;
bool g_beatmapSkins = true;
double g_cursorSize = 1.0; // osu!lazer GameplayCursorSize

// ---- HTTP -----------------------------------------------------------------------------------
std::atomic<int> g_port{ 0 };
std::string g_token;
std::atomic<unsigned> g_served{ 0 };

std::string Lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

void Serve(SOCKET c) {
	char req[2048];
	int got = 0;
	while (got < (int)sizeof(req) - 1) {
		const int n = recv(c, req + got, (int)sizeof(req) - 1 - got, 0);
		if (n <= 0) break;
		got += n;
		req[got] = 0;
		if (strstr(req, "\r\n\r\n")) break;
	}
	req[got] = 0;
	std::string path;
	if (got > 4 && strncmp(req, "GET ", 4) == 0) {
		const char* sp = strchr(req + 4, ' ');
		if (sp) path.assign(req + 4, (size_t)(sp - (req + 4)));
	}
	// /<token>/<name>
	std::string body, type = "application/octet-stream";
	bool ok = false;
	const std::string prefix = "/" + g_token + "/";
	if (!g_token.empty() && path.compare(0, prefix.size(), prefix) == 0) {
		const std::string name = Lower(path.substr(prefix.size()));
		fs::path file;
		{
			std::lock_guard<std::mutex> lock(g_mx);
			auto it = g_files.find(name);
			if (it != g_files.end()) file = it->second;
		}
		if (!file.empty()) {
			body = OsuPlatform::ReadFile(file);
			ok = !body.empty();
			if (name.size() > 4 && name.compare(name.size() - 4, 4, ".png") == 0) type = "image/png";
			else if (name.size() > 4 && (name.compare(name.size() - 4, 4, ".jpg") == 0)) type = "image/jpeg";
		}
	}
	char head[256];
	if (ok) {
		snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
			"Cache-Control: max-age=86400\r\nConnection: close\r\n\r\n", type.c_str(), body.size());
		g_served.fetch_add(1);
	} else {
		snprintf(head, sizeof(head), "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
	}
	send(c, head, (int)strlen(head), 0);
	size_t off = 0;
	while (off < body.size()) {
		const int n = send(c, body.data() + off, (int)std::min<size_t>(body.size() - off, 1 << 16), 0);
		if (n <= 0) break;
		off += (size_t)n;
	}
	shutdown(c, SD_SEND);
	closesocket(c);
}

void StartServer() {
	if (g_port.load() != 0) return;
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) return;
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // loopback only: never reachable from the network
	a.sin_port = 0;                              // any free port
	int len = sizeof(a);
	if (bind(s, (sockaddr*)&a, sizeof(a)) != 0 || listen(s, 16) != 0 || getsockname(s, (sockaddr*)&a, &len) != 0) {
		closesocket(s);
		return;
	}
	std::random_device rd;
	char tok[33];
	snprintf(tok, sizeof(tok), "%08x%08x%08x%08x", rd(), rd(), rd(), rd());
	g_token = tok;
	g_port.store(ntohs(a.sin_port));
	std::thread([s] {
		for (;;) {
			SOCKET c = accept(s, nullptr, nullptr);
			if (c == INVALID_SOCKET) { Sleep(50); continue; }
			Serve(c);
		}
	}).detach();
	MvmDebugLog_LinefAlways("osu.skin", "image server on 127.0.0.1:%d", g_port.load());
}

// ---- skin resolution --------------------------------------------------------------------------
Image PngSize(const fs::path& p) {
	Image im;
	std::ifstream f(p, std::ios::binary);
	unsigned char b[24] = {};
	if (!f.read((char*)b, 24)) return im;
	if (b[0] != 0x89 || b[1] != 'P' || b[2] != 'N' || b[3] != 'G') return im;
	im.w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
	im.h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
	return im;
}

std::string IniValue(const std::string& text, const char* key) {
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		const size_t eq = line.find('=');
		if (eq != std::string::npos && Osu::Trim(line.substr(0, eq)) == key) return Osu::Trim(line.substr(eq + 1));
	}
	return {};
}

// Lazer's selected skin -> name + its files in the store.
bool FromLazer(std::string& name, std::map<std::string, fs::path>& files, bool& beatmapSkins, double& cursorSize) {
	const fs::path root = OsuLibrary::LazerStore();
	if (root.empty()) return false;
	std::error_code ec;
	fs::path gameIni = root / L"game.ini";
	if (!fs::exists(gameIni, ec)) {
		wchar_t app[MAX_PATH] = {};
		if (GetEnvironmentVariableW(L"APPDATA", app, MAX_PATH)) gameIni = fs::path(app) / L"osu" / L"game.ini";
	}
	const std::string ini = OsuPlatform::ReadFile(gameIni);
	const std::string bs = Lower(IniValue(ini, "BeatmapSkins"));
	if (!bs.empty()) beatmapSkins = bs != "false";
	const std::string cs = IniValue(ini, "GameplayCursorSize");
	if (!cs.empty()) cursorSize = std::clamp(atof(cs.c_str()), 0.1, 2.0);
	std::string id = Lower(IniValue(ini, "Skin"));
	id.erase(std::remove(id.begin(), id.end(), '-'), id.end());
	if (id.size() != 32) return false;
	const std::string realm = OsuPlatform::ReadFile(root / L"client.realm");
	if (realm.empty()) return false;
	const std::vector<uint8_t> data(realm.begin(), realm.end());
	OsuRealm::Reader r(data);
	std::vector<OsuRealm::SkinFiles> skins;
	std::string err;
	if (!r.ReadSkins(skins, &err)) {
		MvmDebugLog_LinefAlways("osu.skin", "realm skins unreadable: %s", err.c_str());
		return false;
	}
	for (const auto& sk : skins) {
		if (sk.idHex != id) continue;
		name = sk.name + (sk.creator.empty() ? "" : " (" + sk.creator + ")");
		for (const auto& nf : sk.files) {
			if (nf.hash.size() < 2) continue;
			const fs::path p = root / L"files" / Utf8ToWide(nf.hash.substr(0, 1)) / Utf8ToWide(nf.hash.substr(0, 2)) / Utf8ToWide(nf.hash);
			files[Lower(nf.name)] = p;
		}
		return true; // a built-in skin (no files) still counts: it is what the player picked
	}
	return false;
}

// osu!stable: Skin = in osu!.<user>.cfg -> <install>\Skins\<name>.
bool FromStable(std::string& name, std::map<std::string, fs::path>& files) {
	const fs::path install = OsuLibrary::StableInstall();
	if (install.empty()) return false;
	std::error_code ec;
	std::string skin;
	for (auto& f : fs::directory_iterator(install, ec)) {
		const std::wstring n = f.path().filename().wstring();
		if (_wcsnicmp(n.c_str(), L"osu!.", 5) != 0 || _wcsicmp(n.c_str(), L"osu!.cfg") == 0 || _wcsicmp(f.path().extension().c_str(), L".cfg") != 0) continue;
		const std::string v = IniValue(OsuPlatform::ReadFile(f.path()), "Skin");
		if (!v.empty()) skin = v;
	}
	if (skin.empty()) return false;
	const fs::path dir = install / L"Skins" / Utf8ToWide(skin);
	if (!fs::exists(dir, ec)) return false;
	name = skin;
	for (auto& f : fs::directory_iterator(dir, ec))
		if (f.is_regular_file(ec)) files[Lower(WideToUtf8(f.path().filename().wstring()))] = f.path();
	return true;
}

// The skin shipped with the mod (<install bin>\osu\Skin\<skin>\, staged from
// Assets\easter-eggs\osu-skin): ExpressoKuns japan (WB) by Cappuccino. Everyone without an osu! skin
// of their own -- no osu! installed, or a built-in lazer skin picked -- plays with it.
bool FromBundled(std::string& name, std::map<std::string, fs::path>& files) {
	std::error_code ec;
	const fs::path root = OsuPlatform::BundledRoot() / L"Skin";
	if (!fs::exists(root, ec)) return false;
	for (auto& d : fs::directory_iterator(root, ec)) {
		if (!d.is_directory(ec)) continue;
		std::map<std::string, fs::path> found;
		for (auto& f : fs::directory_iterator(d.path(), ec))
			if (f.is_regular_file(ec)) found[Lower(WideToUtf8(f.path().filename().wstring()))] = f.path();
		if (found.empty()) continue;
		name = WideToUtf8(d.path().filename().wstring());
		files = std::move(found);
		return true;
	}
	return false;
}

void ParseSkinIni(const std::string& text, std::map<std::string, std::string>& ini, std::vector<int>& combo) {
	std::istringstream in(text);
	std::string line;
	std::map<int, int> byIndex;
	while (std::getline(in, line)) {
		if (!line.empty() && (unsigned char)line[0] == 0xEF) line.erase(0, std::min<size_t>(3, line.size())); // BOM
		const size_t cm = line.find("//");
		if (cm != std::string::npos) line.erase(cm);
		line = Osu::Trim(line);
		if (line.empty() || line[0] == '[') continue;
		const size_t c = line.find(':');
		if (c == std::string::npos) continue;
		const std::string key = Lower(Osu::Trim(line.substr(0, c))), val = Osu::Trim(line.substr(c + 1));
		ini[key] = val;
		if (key.size() == 6 && key.compare(0, 5, "combo") == 0 && isdigit((unsigned char)key[5])) {
			int r = 0, g = 0, b = 0;
			if (sscanf_s(val.c_str(), "%d,%d,%d", &r, &g, &b) == 3)
				byIndex[key[5] - '0'] = (std::clamp(r, 0, 255) << 16) | (std::clamp(g, 0, 255) << 8) | std::clamp(b, 0, 255);
		}
	}
	for (auto& kv : byIndex) combo.push_back(kv.second);
}

void DoLoad() {
	const double t0 = OsuPlatform::QpcMs();
	std::string name;
	std::map<std::string, fs::path> files;
	bool beatmapSkins = true;
	double cursorSize = 1.0;
	const char* from = "none";
	bool bundled = false;
	if (FromLazer(name, files, beatmapSkins, cursorSize) && !files.empty()) from = "lazer";
	else if (files.clear(), FromStable(name, files) && !files.empty()) from = "stable";
	else if (files.clear(), FromBundled(name, files)) { from = "bundled"; bundled = true; }
	std::map<std::string, Image> images;
	for (const auto& kv : files)
		if (kv.first.size() > 4 && kv.first.compare(kv.first.size() - 4, 4, ".png") == 0) images[kv.first] = PngSize(kv.second);
	std::map<std::string, std::string> ini;
	std::vector<int> combo;
	auto si = files.find("skin.ini");
	if (si != files.end()) ParseSkinIni(OsuPlatform::ReadFile(si->second), ini, combo);
	if (files.empty()) name.clear();
	// The bundled skin is named from its skin.ini, with its author's credit.
	if (bundled) {
		auto nm = ini.find("name"), au = ini.find("author");
		if (nm != ini.end() && !nm->second.empty()) name = nm->second;
		if (au != ini.end() && !au->second.empty()) name += " by " + au->second;
		name += " (bundled)";
	}
	{
		std::lock_guard<std::mutex> lock(g_mx);
		g_name = name; g_files = std::move(files); g_images = std::move(images); g_ini = std::move(ini);
		g_combo = std::move(combo); g_beatmapSkins = beatmapSkins; g_cursorSize = cursorSize;
	}
	if (!g_name.empty()) StartServer();
	g_ready.store(true);
	g_loading.store(false);
	MvmDebugLog_LinefAlways("osu.skin", "skin '%s' from %s: %zu files, %zu combo colours, beatmap skins %s (%.0f ms)",
		g_name.c_str(), from, g_files.size(), g_combo.size(), g_beatmapSkins ? "on" : "off", OsuPlatform::QpcMs() - t0);
}

std::string IniGet(const char* key, const char* def) {
	auto it = g_ini.find(key);
	return it == g_ini.end() ? def : it->second;
}

std::string Hex(int c) { char b[8]; snprintf(b, sizeof(b), "#%06x", c & 0xFFFFFF); return b; }

int ParseRgb(const std::string& v, int def) {
	int r = 0, g = 0, b = 0;
	if (sscanf_s(v.c_str(), "%d,%d,%d", &r, &g, &b) != 3) return def;
	return (std::clamp(r, 0, 255) << 16) | (std::clamp(g, 0, 255) << 8) | std::clamp(b, 0, 255);
}

// One element as [url, width, height, ratio] -- the @2x file when there is one (lazer
// LegacySkin.GetTexture: "@2x" wins, ScaleAdjust 2). g_mx held.
bool Element(JsonBuilder& j, const std::string& base, const char* key) {
	const std::string hd = base + "@2x.png", sd = base + ".png";
	const std::string* pick = nullptr;
	int ratio = 1;
	if (g_images.count(hd)) { pick = &hd; ratio = 2; }
	else if (g_images.count(sd)) pick = &sd;
	if (!pick) return false;
	const Image im = g_images[*pick];
	if (im.w <= 0) return false;
	j.Key(key); j.BeginArray();
	j.ValueString("http://127.0.0.1:" + std::to_string(g_port.load()) + "/" + g_token + "/" + *pick);
	j.ValueInt(im.w); j.ValueInt(im.h); j.ValueInt(ratio);
	j.EndArray();
	return true;
}

} // namespace

void Load() {
	if (g_ready.load() || g_loading.exchange(true)) return;
	std::thread([] {
		try { DoLoad(); }
		catch (...) { g_ready.store(true); g_loading.store(false); MvmDebugLog_LinefAlways("osu.skin", "skin load threw"); }
	}).detach();
}

void Reload() {
	if (g_loading.load()) return;
	g_ready.store(false);
	Load();
}

bool Ready() { return g_ready.load(); }
std::string Name() { std::lock_guard<std::mutex> lock(g_mx); return g_name; }

std::wstring File(const std::string& name) {
	std::lock_guard<std::mutex> lock(g_mx);
	auto it = g_files.find(Lower(name));
	return it == g_files.end() ? std::wstring() : it->second.wstring();
}

void ComboColours(std::vector<int>& out) { std::lock_guard<std::mutex> lock(g_mx); out = g_combo; }
bool BeatmapSkins() { std::lock_guard<std::mutex> lock(g_mx); return g_beatmapSkins; }

void AppendJson(JsonBuilder& j) {
	if (!g_ready.load()) return;
	std::lock_guard<std::mutex> lock(g_mx);
	if (g_name.empty() || g_port.load() == 0) return;
	j.Key("skin"); j.BeginObject();
	j.StringField("name", g_name);
	j.Key("el"); j.BeginObject();
	static const char* const kEls[] = {
		"hitcircle", "hitcircleoverlay", "sliderstartcircle", "sliderstartcircleoverlay", "sliderendcircle",
		"sliderendcircleoverlay", "approachcircle", "sliderb", "sliderb0", "sliderfollowcircle", "reversearrow",
		"sliderscorepoint", "followpoint", "hit0", "hit50", "hit100", "hit300", "cursor", "cursortrail",
		"cursormiddle", "spinner-circle", "spinner-background", "spinner-metre", "spinner-spin", "spinner-clear",
		"spinner-approachcircle", "spinner-top", "spinner-middle", "spinner-middle2", "spinner-bottom",
	};
	for (const char* e : kEls) Element(j, e, e);
	// The hit-circle number font (HitCirclePrefix, default "default"): default-0 .. default-9.
	std::string prefix = Lower(IniGet("hitcircleprefix", "default"));
	for (char& c : prefix) if (c == '\\') c = '/';
	for (int d = 0; d <= 9; ++d) Element(j, prefix + "-" + std::to_string(d), ("n" + std::to_string(d)).c_str());
	j.EndObject();
	// Follow point animation frames (followpoint-0 .. -N), when the skin animates them.
	j.Key("fp"); j.BeginArray();
	for (int f = 0; f < 64; ++f) {
		const std::string base = "followpoint-" + std::to_string(f);
		if (!g_images.count(base + ".png") && !g_images.count(base + "@2x.png")) break;
		j.BeginObject(); Element(j, base, "i"); j.EndObject();
	}
	j.EndArray();
	j.Key("combo"); j.BeginArray(); for (int c : g_combo) j.ValueString(Hex(c)); j.EndArray();
	j.StringField("border", Hex(ParseRgb(IniGet("sliderborder", ""), 0xffffff)));
	const std::string track = IniGet("slidertrackoverride", "");
	if (!track.empty()) j.StringField("track", Hex(ParseRgb(track, 0)));
	j.DoubleField("overlap", atof(IniGet("hitcircleoverlap", "-2").c_str()));
	j.BoolField("overlayAbove", IniGet("hitcircleoverlayabovenumber", IniGet("hitcircleoverlayabovenumer", "1").c_str()) != "0");
	j.BoolField("ballTint", IniGet("allowsliderballtint", "0") == "1");
	j.BoolField("cursorCentre", IniGet("cursorcentre", "1") != "0");
	j.BoolField("cursorExpand", IniGet("cursorexpand", "1") != "0");
	j.BoolField("cursorRotate", IniGet("cursorrotate", "1") != "0");
	j.DoubleField("version", atof(IniGet("version", "1.0").c_str()));
	j.DoubleField("cursorSize", g_cursorSize);
	j.EndObject();
}

std::string Status() {
	std::lock_guard<std::mutex> lock(g_mx);
	char b[512];
	snprintf(b, sizeof(b), "skin '%s' (%s): %zu files, %zu images, %zu combo colours, beatmap skins %s, server %d (%u served)",
		g_name.c_str(), g_ready.load() ? "ready" : (g_loading.load() ? "loading" : "not loaded"), g_files.size(),
		g_images.size(), g_combo.size(), g_beatmapSkins ? "on" : "off", g_port.load(), g_served.load());
	return b;
}

} // namespace OsuSkin
} // namespace Filmmaker
