#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "OsuAudio.h"

#include "OsuPlatform.h"
#include "../../Platform/TextEncoding.h"
#include "../../Debug/MvmDebugLog.h"

#include <Windows.h>
#include <xaudio2.h>
#pragma comment(lib, "xaudio2.lib")

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace Filmmaker {
namespace OsuAudio {
namespace {

using OsuPlatform::QpcMs;

constexpr int kRateHz = 44100, kChannels = 2;

enum Op { kOpen = 1, kPlayFrom, kPause, kResume, kStop, kVolume };
struct Cmd { Op op; uint64_t id; std::wstring src; std::string hint; double ms, rate, loop; bool pitch, playNow; };

std::mutex g_mx;
std::condition_variable g_cv;
std::deque<Cmd> g_q;
std::thread g_thread;
std::atomic<uint64_t> g_nextId{ 1 };
std::atomic<uint64_t> g_readyId{ 0 }, g_failedId{ 0 };
std::atomic<bool> g_playing{ false };
std::atomic<double> g_pos{ 0.0 }, g_posStamp{ 0.0 };
std::atomic<unsigned> g_pollSeq{ 0 };
std::atomic<int> g_volume{ 650 };
std::atomic<double> g_rate{ 1.0 };
std::mutex g_srcMx;
std::wstring g_src;

// Audio-thread state.
IXAudio2* g_xa = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
IXAudio2SourceVoice* g_voice = nullptr;
std::shared_ptr<std::vector<int16_t>> g_pcm; // the decoded song (kept alive while a voice plays it)
uint64_t g_startFrame = 0, g_lastSamples = UINT64_MAX;
double g_loopFrom = -1.0;
bool g_voicePaused = false;

bool Engine() {
	if (g_xa) return true;
	if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) { g_xa = nullptr; return false; }
	if (FAILED(g_xa->CreateMasteringVoice(&g_master))) { g_xa->Release(); g_xa = nullptr; return false; }
	return true;
}

void KillVoice() {
	if (g_voice) { g_voice->DestroyVoice(); g_voice = nullptr; }
	g_playing.store(false);
}

uint64_t Frames() { return g_pcm ? g_pcm->size() / kChannels : 0; }
uint64_t FrameAt(double songMs) {
	const double f = std::max(0.0, songMs) / 1000.0 / g_rate.load() * kRateHz;
	return std::min<uint64_t>((uint64_t)f, Frames() ? Frames() - 1 : 0);
}
double MsAt(uint64_t frame) { return (double)frame / kRateHz * 1000.0 * g_rate.load(); }

// Decode the whole song into memory: 16-bit stereo 44.1 kHz, with the rate baked in.
bool Decode(const std::wstring& src, double rate, bool pitch, std::vector<int16_t>& out) {
	std::wstring filter;
	if (std::abs(rate - 1.0) > 1e-3) {
		wchar_t f[96];
		if (pitch) swprintf(f, 96, L" -af \"asetrate=%d,aresample=%d\"", (int)std::lround(kRateHz * rate), kRateHz);
		else swprintf(f, 96, L" -af \"atempo=%.4f\"", rate);
		filter = f;
	}
	std::string bytes;
	const std::wstring cmd = L"\"" + OsuPlatform::Ffmpeg().wstring() + L"\" -v error -nostdin -i \"" + src
		+ L"\" -vn -ac 2 -ar 44100" + filter + L" -f s16le -";
	if (!OsuPlatform::RunCapture(cmd, 60000, bytes) || bytes.size() < 4) return false;
	out.resize(bytes.size() / 2);
	std::memcpy(out.data(), bytes.data(), out.size() * 2);
	return true;
}

void PlayAt(double ms) {
	KillVoice();
	if (!g_pcm || Frames() == 0 || !Engine()) return;
	WAVEFORMATEX fmt{};
	fmt.wFormatTag = WAVE_FORMAT_PCM; fmt.nChannels = kChannels; fmt.nSamplesPerSec = kRateHz;
	fmt.wBitsPerSample = 16; fmt.nBlockAlign = kChannels * 2; fmt.nAvgBytesPerSec = kRateHz * kChannels * 2;
	if (FAILED(g_xa->CreateSourceVoice(&g_voice, &fmt))) { g_voice = nullptr; return; }
	g_startFrame = FrameAt(ms);
	XAUDIO2_BUFFER buf{};
	buf.pAudioData = reinterpret_cast<const BYTE*>(g_pcm->data());
	buf.AudioBytes = (UINT32)(Frames() * kChannels * 2);
	buf.PlayBegin = (UINT32)g_startFrame;
	buf.Flags = XAUDIO2_END_OF_STREAM;
	if (g_loopFrom >= 0.0) {
		buf.LoopBegin = (UINT32)std::max<uint64_t>(FrameAt(g_loopFrom), g_startFrame);
		if (buf.LoopBegin + 1 >= Frames()) buf.LoopBegin = (UINT32)g_startFrame;
		buf.LoopCount = XAUDIO2_LOOP_INFINITE;
	}
	if (FAILED(g_voice->SubmitSourceBuffer(&buf))) { KillVoice(); return; }
	g_voice->SetVolume(g_volume.load() / 1000.0f);
	g_voice->Start(0);
	g_lastSamples = UINT64_MAX;
	g_voicePaused = false;
	g_pos.store(MsAt(g_startFrame)); g_posStamp.store(QpcMs());
	g_playing.store(true);
}

// The sample counter -> song position (handles the song-select loop).
void Poll() {
	if (!g_voice || g_voicePaused) return;
	XAUDIO2_VOICE_STATE st{};
	g_voice->GetState(&st, 0);
	if (st.SamplesPlayed == g_lastSamples) {
		if (st.BuffersQueued == 0) g_playing.store(false); // the song ended
		return;
	}
	g_lastSamples = st.SamplesPlayed;
	uint64_t frame = g_startFrame + st.SamplesPlayed;
	const uint64_t total = Frames();
	if (frame >= total && g_loopFrom >= 0.0 && total > 0) {
		const uint64_t lb = std::max<uint64_t>(FrameAt(g_loopFrom), g_startFrame);
		frame = lb + (frame - total) % std::max<uint64_t>(1, total - lb);
	}
	g_pos.store(MsAt(std::min(frame, total))); g_posStamp.store(QpcMs());
	g_pollSeq.fetch_add(1);
	if (st.BuffersQueued == 0) g_playing.store(false);
}

void ThreadMain() {
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	for (;;) {
		Cmd cmd{};
		bool have = false;
		{
			std::unique_lock<std::mutex> lock(g_mx);
			g_cv.wait_for(lock, std::chrono::milliseconds(2), [] { return !g_q.empty(); });
			if (!g_q.empty()) { cmd = g_q.front(); g_q.pop_front(); have = true; }
		}
		if (have) switch (cmd.op) {
		case kOpen: {
			// A later Open supersedes this one: skip straight to it.
			bool superseded = false;
			{ std::lock_guard<std::mutex> lock(g_mx); for (auto& c : g_q) if (c.op == kOpen) { superseded = true; break; } }
			if (superseded) { g_failedId.store(cmd.id); g_readyId.store(cmd.id); break; }
			const double t0 = QpcMs();
			KillVoice();
			g_pcm.reset();
			auto pcm = std::make_shared<std::vector<int16_t>>();
			const bool ok = Decode(cmd.src, cmd.rate, cmd.pitch, *pcm) && Engine();
			if (ok) {
				g_pcm = pcm;
				g_rate.store(cmd.rate);
				g_loopFrom = cmd.loop;
				if (cmd.playNow) PlayAt(cmd.ms);
				else { g_pos.store(cmd.ms); g_posStamp.store(QpcMs()); g_playing.store(false); }
			}
			MvmDebugLog_LinefAlways("osu.audio", "open #%llu %s: decoded %.1f s of audio in %.0f ms (rate %.2f%s): %s",
				(unsigned long long)cmd.id, ok ? "ok" : "FAILED", ok ? (double)pcm->size() / kChannels / kRateHz : 0.0,
				QpcMs() - t0, cmd.rate, cmd.pitch ? " pitched" : "", WideToUtf8(cmd.src).c_str());
			if (!ok) g_failedId.store(cmd.id);
			g_readyId.store(cmd.id);
			break;
		}
		case kPlayFrom: PlayAt(cmd.ms); break;
		case kPause:
			if (g_voice && !g_voicePaused) { g_voice->Stop(0); g_voicePaused = true; }
			g_playing.store(false);
			break;
		case kResume:
			if (g_voice && g_voicePaused) {
				g_voicePaused = false;
				g_voice->Start(0);
				g_posStamp.store(QpcMs());
				g_playing.store(true);
			}
			break;
		case kStop: KillVoice(); g_pcm.reset(); g_loopFrom = -1.0; break;
		case kVolume: if (g_voice) g_voice->SetVolume(g_volume.load() / 1000.0f); break;
		}
		Poll();
	}
}

void Push(const Cmd& c) {
	{
		std::lock_guard<std::mutex> lock(g_mx);
		if (!g_thread.joinable()) g_thread = std::thread(ThreadMain);
		g_q.push_back(c);
	}
	g_cv.notify_one();
}

} // namespace

uint64_t Open(const std::wstring& src, const std::string& nameHint, double startMs, double rate, bool pitch,
	bool playNow, double loopFromMs) {
	const uint64_t id = g_nextId.fetch_add(1);
	{ std::lock_guard<std::mutex> lock(g_srcMx); g_src = src; }
	Push({ kOpen, id, src, nameHint, startMs, rate, loopFromMs, pitch, playNow });
	return id;
}
bool Ready(uint64_t id) { return g_readyId.load() >= id; }
bool Failed(uint64_t id) { return g_failedId.load() == id; }
void PlayFrom(double ms) { Push({ kPlayFrom, 0, L"", "", ms, 1.0, -1.0, false, true }); }
void Pause() { Push({ kPause, 0, L"", "", 0, 1, -1, false, false }); }
void Resume() { Push({ kResume, 0, L"", "", 0, 1, -1, false, false }); }
void Stop() { { std::lock_guard<std::mutex> lock(g_srcMx); g_src.clear(); } Push({ kStop, 0, L"", "", 0, 1, -1, false, false }); }
void SetVolume(int v) { g_volume.store(std::clamp(v, 0, 1000)); Push({ kVolume, 0, L"", "", 0, 1, -1, false, false }); }
int Volume() { return g_volume.load(); }
double Position() {
	const double p = g_pos.load();
	return g_playing.load() ? p + (QpcMs() - g_posStamp.load()) * g_rate.load() : p;
}
unsigned PollSeq() { return g_pollSeq.load(); }
bool Playing() { return g_playing.load(); }
std::wstring CurrentSource() { std::lock_guard<std::mutex> lock(g_srcMx); return g_src; }


} // namespace OsuAudio
} // namespace Filmmaker
