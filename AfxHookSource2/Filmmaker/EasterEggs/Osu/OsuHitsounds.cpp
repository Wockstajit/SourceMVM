#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "OsuHitsounds.h"
#include "OsuSkin.h"

#include "OsuLibrary.h"
#include "OsuPlatform.h"

#include "../../Platform/TextEncoding.h"
#include "../../Debug/MvmDebugLog.h"

#include <Windows.h>
#include <xaudio2.h>
#pragma comment(lib, "xaudio2.lib")

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace Filmmaker {
namespace OsuHitsounds {
namespace {

constexpr int kVoicesPerSample = 6;

struct Sample {
	WAVEFORMATEX fmt{};
	std::vector<uint8_t> pcm;
	IXAudio2SourceVoice* voices[kVoicesPerSample] = {};
	int next = 0;
};

std::mutex g_mx;                                        // guards g_bank and the engine
IXAudio2* g_xa = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
std::map<std::string, std::shared_ptr<Sample>> g_bank;  // "<name>#<index>" or "file:<name>" or skin "<name>"
std::atomic<bool> g_ready{ true };
std::atomic<uint64_t> g_gen{ 0 };
std::atomic<double> g_volume{ 0.8 };
std::atomic<bool> g_mapSamples{ false }; // the mapper's own sample files (off: skin sounds only)

bool EnsureEngine() {
	if (g_xa) return true;
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) { g_xa = nullptr; return false; }
	if (FAILED(g_xa->CreateMasteringVoice(&g_master))) { g_xa->Release(); g_xa = nullptr; return false; }
	return true;
}

// ---- decoding -----------------------------------------------------------------------------------
// RIFF WAVE with PCM 8/16/24/32-bit or float32 -> 16-bit PCM in its own rate and channel count.
bool ParseWav(const std::string& bytes, Sample& out) {
	auto u16 = [&](size_t o) { return (uint32_t)(uint8_t)bytes[o] | (uint32_t)(uint8_t)bytes[o + 1] << 8; };
	auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
	if (bytes.size() < 12 || bytes.compare(0, 4, "RIFF") != 0 || bytes.compare(8, 4, "WAVE") != 0) return false;
	uint32_t format = 0, channels = 0, rate = 0, bits = 0;
	const char* data = nullptr; size_t dataLen = 0;
	for (size_t o = 12; o + 8 <= bytes.size();) {
		const std::string id = bytes.substr(o, 4);
		const size_t len = u32(o + 4), body = o + 8;
		if (body + len > bytes.size() && id != "data") break;
		if (id == "fmt " && len >= 16) {
			format = u16(body); channels = u16(body + 2); rate = u32(body + 4); bits = u16(body + 14);
			if (format == 0xFFFE && len >= 26) format = u16(body + 24); // WAVE_FORMAT_EXTENSIBLE subformat
		} else if (id == "data") {
			data = bytes.data() + body; dataLen = std::min(len, bytes.size() - body);
		}
		o = body + len + (len & 1);
	}
	if (!data || !channels || !rate || channels > 2) return false;
	const bool isFloat = format == 3;
	if (!(format == 1 || isFloat) || !(bits == 8 || bits == 16 || bits == 24 || bits == 32)) return false;
	const size_t bytesPer = bits / 8, frames = dataLen / (bytesPer * channels);
	out.pcm.resize(frames * channels * 2);
	int16_t* dst = reinterpret_cast<int16_t*>(out.pcm.data());
	for (size_t i = 0; i < frames * channels; ++i) {
		const uint8_t* p = reinterpret_cast<const uint8_t*>(data) + i * bytesPer;
		double v = 0.0;
		if (isFloat && bits == 32) { float f; std::memcpy(&f, p, 4); v = f; }
		else if (bits == 8) v = (p[0] - 128) / 128.0;
		else if (bits == 16) v = (int16_t)(p[0] | p[1] << 8) / 32768.0;
		else if (bits == 24) v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) / 2147483648.0;
		else v = (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24) / 2147483648.0;
		dst[i] = (int16_t)std::lround(std::clamp(v, -1.0, 1.0) * 32767.0);
	}
	out.fmt.wFormatTag = WAVE_FORMAT_PCM;
	out.fmt.nChannels = (WORD)channels;
	out.fmt.nSamplesPerSec = rate;
	out.fmt.wBitsPerSample = 16;
	out.fmt.nBlockAlign = (WORD)(channels * 2);
	out.fmt.nAvgBytesPerSec = rate * channels * 2;
	return !out.pcm.empty();
}

// Any file: WAV directly, else (ogg/mp3/odd wav) one ffmpeg decode, cached.
bool LoadFile(const fs::path& file, Sample& out) {
	std::error_code ec;
	if (!fs::exists(file, ec) || fs::file_size(file, ec) == 0) return false;
	if (ParseWav(OsuPlatform::ReadFile(file), out)) return true;
	const fs::path cache = OsuPlatform::CacheDir() / L"samples";
	fs::create_directories(cache, ec);
	const fs::path wav = cache / Utf8ToWide(OsuPlatform::Hex64(OsuPlatform::Fnv64(file.wstring()) ^ (uint64_t)fs::file_size(file, ec)) + ".wav");
	if (!fs::exists(wav, ec))
		OsuPlatform::RunHidden(L"\"" + OsuPlatform::Ffmpeg().wstring() + L"\" -y -loglevel error -i \"" + file.wstring()
			+ L"\" -vn -ac 2 -ar 44100 -c:a pcm_s16le \"" + wav.wstring() + L"\"", 20000);
	return ParseWav(OsuPlatform::ReadFile(wav), out);
}

// The built-in fallbacks: short synthesized hits, by sample kind.
void Synth(const std::string& name, Sample& out) {
	const int rate = 44100;
	auto has = [&](const char* k) { return name.find(k) != std::string::npos; };
	double len = 0.06;
	if (has("finish")) len = 0.45; else if (has("whistle")) len = 0.16; else if (has("clap")) len = 0.09; else if (has("slidertick")) len = 0.03;
	const bool soft = has("soft"), drum = has("drum");
	const size_t n = (size_t)(len * rate);
	out.pcm.resize(n * 2);
	int16_t* d = reinterpret_cast<int16_t*>(out.pcm.data());
	uint32_t seed = 12345;
	auto noise = [&] { seed = seed * 1664525u + 1013904223u; return ((seed >> 9) & 0xFFFF) / 32768.0 - 1.0; };
	double lp = 0.0;
	for (size_t i = 0; i < n; ++i) {
		const double t = (double)i / rate;
		double v = 0.0;
		if (has("slidertick")) v = 0.35 * std::sin(6.2831853 * 2600.0 * t) * std::exp(-t / 0.006);
		else if (has("whistle")) v = 0.3 * std::sin(6.2831853 * (1900.0 + 900.0 * t) * t) * std::exp(-t / 0.06);
		else if (has("finish")) { lp += (noise() - lp) * 0.6; v = 0.35 * lp * std::exp(-t / 0.14); }
		else if (has("clap")) { lp += (noise() - lp) * 0.5; v = 0.5 * lp * std::exp(-t / 0.02); }
		else {
			// hitnormal: a bright click with a short body
			lp += (noise() - lp) * (soft ? 0.15 : 0.45);
			const double tone = drum ? 160.0 : (soft ? 900.0 : 1700.0);
			v = (0.45 * lp * std::exp(-t / 0.004) + 0.4 * std::sin(6.2831853 * tone * t) * std::exp(-t / (drum ? 0.03 : 0.012)));
		}
		d[i] = (int16_t)std::lround(std::clamp(v, -1.0, 1.0) * 32767.0);
	}
	out.fmt.wFormatTag = WAVE_FORMAT_PCM; out.fmt.nChannels = 1; out.fmt.nSamplesPerSec = rate;
	out.fmt.wBitsPerSample = 16; out.fmt.nBlockAlign = 2; out.fmt.nAvgBytesPerSec = rate * 2;
}

bool MakeVoices(Sample& s) {
	for (auto& v : s.voices)
		if (FAILED(g_xa->CreateSourceVoice(&v, &s.fmt))) { v = nullptr; return false; }
	return true;
}
void DestroyVoices(Sample& s) {
	for (auto& v : s.voices) if (v) { v->DestroyVoice(); v = nullptr; }
}

fs::path FindWithExt(const std::function<std::wstring(const std::string&)>& resolve, const std::string& stem) {
	for (const char* ext : { ".wav", ".ogg", ".mp3" }) {
		const std::wstring p = resolve(stem + ext);
		std::error_code ec;
		if (!p.empty() && fs::exists(p, ec)) return p;
	}
	return {};
}

std::string Key(const Osu::SampleRef& s) { return s.file.empty() ? s.name + "#" + std::to_string(s.index) : "file:" + s.file; }

} // namespace

void Prepare(const Osu::Beatmap& b, const Resolver& beatmapFiles) {
	const uint64_t gen = ++g_gen;
	g_ready.store(false);
	// Everything this map can ask for: each set x sound, at every index it uses.
	std::set<int> indices{ 0 };
	for (const auto& tp : b.timing) indices.insert(tp.sampleIndex);
	std::vector<Osu::SampleRef> wanted;
	std::set<std::string> files;
	for (const auto& o : b.objects) { indices.insert(o.index); if (!o.file.empty()) files.insert(o.file); }
	for (int set = 1; set <= 3; ++set)
		for (const char* kind : { "-hitnormal", "-hitwhistle", "-hitfinish", "-hitclap", "-slidertick" })
			for (int idx : indices) wanted.push_back({ std::string(Osu::SampleSetName(set)) + kind, idx, 100, "" });
	for (const auto& f : files) wanted.push_back({ "", 0, 100, f });
	const bool mapSamples = g_mapSamples.load();
	std::thread([gen, wanted, beatmapFiles, mapSamples] {
		const double t0 = OsuPlatform::QpcMs();
		// The player's skin (OsuSkin: lazer's selected skin, else stable's); give it a moment to resolve.
		for (int i = 0; i < 100 && !OsuSkin::Ready(); ++i) Sleep(50);
		const std::string skin = OsuSkin::Name();
		auto skinFiles = [](const std::string& name) { return OsuSkin::File(name); };
		std::map<std::string, std::shared_ptr<Sample>> bank;
		int fromMap = 0, fromSkin = 0, synth = 0;
		for (const Osu::SampleRef& w : wanted) {
			if (g_gen.load() != gen) return; // a newer map took over
			auto s = std::make_shared<Sample>();
			bool ok = false;
			if (!w.file.empty()) {
				if (!mapSamples) continue;
				const std::wstring p = beatmapFiles ? beatmapFiles(w.file) : std::wstring();
				ok = !p.empty() && LoadFile(p, *s);
				if (!ok) continue; // a missing custom file is silence, as in osu!
				fromMap++;
			} else {
				if (mapSamples && w.index >= 1 && beatmapFiles) {
					const fs::path p = FindWithExt(beatmapFiles, w.index == 1 ? w.name : w.name + std::to_string(w.index));
					if (!p.empty() && LoadFile(p, *s)) { ok = true; fromMap++; }
				}
				if (!ok) { const fs::path p = FindWithExt(skinFiles, w.name); if (!p.empty() && LoadFile(p, *s)) { ok = true; fromSkin++; } }
				if (!ok) { Synth(w.name, *s); synth++; }
			}
			bank[Key(w)] = s;
		}
		for (const char* extra : { "combobreak", "spinnerbonus" }) {
			auto s = std::make_shared<Sample>();
			const fs::path p = FindWithExt(skinFiles, extra);
			if (!p.empty() && LoadFile(p, *s)) bank[extra] = s;
		}
		std::lock_guard<std::mutex> lock(g_mx);
		if (g_gen.load() != gen) return;
		if (!EnsureEngine()) {
			MvmDebugLog_LinefAlways("osu.sound", "XAudio2 unavailable: no hitsounds");
			g_ready.store(true);
			return;
		}
		for (auto& kv : g_bank) DestroyVoices(*kv.second);
		g_bank.clear();
		for (auto& kv : bank) if (MakeVoices(*kv.second)) g_bank[kv.first] = kv.second; else DestroyVoices(*kv.second);
		g_ready.store(true);
		MvmDebugLog_LinefAlways("osu.sound", "%zu samples (%d beatmap, %d skin %s, %d built-in) in %.0f ms",
			g_bank.size(), fromMap, fromSkin, skin.c_str(), synth, OsuPlatform::QpcMs() - t0);
	}).detach();
}

bool Ready() { return g_ready.load(); }

namespace {
void PlayKey(const std::string& key, double volume) {
	std::lock_guard<std::mutex> lock(g_mx);
	auto it = g_bank.find(key);
	if (it == g_bank.end() || !g_xa) return;
	Sample& s = *it->second;
	IXAudio2SourceVoice* v = s.voices[s.next];
	s.next = (s.next + 1) % kVoicesPerSample;
	if (!v) return;
	v->Stop(0);
	v->FlushSourceBuffers();
	XAUDIO2_BUFFER buf{};
	buf.AudioBytes = (UINT32)s.pcm.size();
	buf.pAudioData = s.pcm.data();
	buf.Flags = XAUDIO2_END_OF_STREAM;
	if (FAILED(v->SubmitSourceBuffer(&buf))) return;
	v->SetVolume((float)std::clamp(volume, 0.0, 1.0));
	v->Start(0);
}
} // namespace

void Play(const Osu::SampleRef& s) {
	const double vol = g_volume.load() * std::clamp(s.volume, 5, 100) / 100.0;
	// A mapper's custom sample file, with beatmap samples off, plays the skin's plain hit instead.
	if (!s.file.empty() && !g_mapSamples.load()) { PlayKey("normal-hitnormal#0", vol); return; }
	PlayKey(Key(s), vol);
}
void PlayNamed(const char* skinName, double volume01) { PlayKey(skinName, g_volume.load() * volume01); }
void SetBeatmapSamples(bool on) { g_mapSamples.store(on); }
bool BeatmapSamples() { return g_mapSamples.load(); }
void SetVolume(double v) { g_volume.store(std::clamp(v, 0.0, 1.0)); }
double Volume() { return g_volume.load(); }

} // namespace OsuHitsounds
} // namespace Filmmaker
