#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "RenderDoneSound.h"

#include "../Debug/MvmDebugLog.h"

#include <Windows.h>
#include <xaudio2.h>
#pragma comment(lib, "xaudio2.lib")

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

namespace Filmmaker {
namespace {

constexpr int kRate = 44100;
constexpr double kPi = 3.14159265358979323846;

std::mutex g_mx; // guards the engine, the voice and the buffer it plays
IXAudio2* g_xa = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
IXAudio2SourceVoice* g_voice = nullptr;
std::vector<int16_t> g_pcm; // stays alive while the voice plays it
std::atomic<bool> g_enabled{ true };
std::atomic<int> g_volume{ 60 };

bool EnsureVoice() {
	if (g_voice) return true;
	if (!g_xa) {
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) { g_xa = nullptr; return false; }
		if (FAILED(g_xa->CreateMasteringVoice(&g_master))) { g_xa->Release(); g_xa = nullptr; return false; }
	}
	WAVEFORMATEX fmt{};
	fmt.wFormatTag = WAVE_FORMAT_PCM;
	fmt.nChannels = 1;
	fmt.nSamplesPerSec = kRate;
	fmt.wBitsPerSample = 16;
	fmt.nBlockAlign = 2;
	fmt.nAvgBytesPerSec = kRate * 2;
	if (FAILED(g_xa->CreateSourceVoice(&g_voice, &fmt))) { g_voice = nullptr; return false; }
	return true;
}

// Two soft bell notes (a struck sine with a quick attack and an exponential ring-out, plus a quiet
// octave for shine): up a fifth when the job finished, down a fourth when it failed or was cancelled.
void Synthesize(bool finished, std::vector<int16_t>& out) {
	const double notes[2] = { finished ? 783.99 : 659.25, finished ? 1174.66 : 493.88 }; // G5 D6 / E5 B4
	const double noteStart[2] = { 0.0, 0.16 };
	const double length = 0.95;
	out.assign((size_t)(length * kRate), 0);
	for (size_t i = 0; i < out.size(); ++i) {
		const double t = (double)i / kRate;
		double v = 0.0;
		for (int n = 0; n < 2; ++n) {
			const double local = t - noteStart[n];
			if (local < 0.0) continue;
			const double attack = std::min(1.0, local / 0.006);
			const double ring = std::exp(-local * 5.5);
			v += attack * ring * (std::sin(2.0 * kPi * notes[n] * local) + 0.25 * std::sin(4.0 * kPi * notes[n] * local));
		}
		// Fade the last 40 ms so the end never clicks.
		const double tail = std::min(1.0, (length - t) / 0.04);
		out[i] = (int16_t)std::lround(std::clamp(v * 0.32 * tail, -1.0, 1.0) * 32767.0);
	}
}

void PlayNow(bool finished) {
	std::lock_guard<std::mutex> lock(g_mx);
	if (!EnsureVoice()) {
		MvmDebugLog_LinefAlways("render.sound", "XAudio2 unavailable: no chime");
		return;
	}
	// The voice must be done with the old buffer before it is replaced.
	g_voice->Stop(0);
	g_voice->FlushSourceBuffers();
	Synthesize(finished, g_pcm);
	XAUDIO2_BUFFER buffer{};
	buffer.AudioBytes = (UINT32)(g_pcm.size() * sizeof(int16_t));
	buffer.pAudioData = reinterpret_cast<const BYTE*>(g_pcm.data());
	buffer.Flags = XAUDIO2_END_OF_STREAM;
	// Linear gain; squared so the slider's low half stays usable.
	const float level = (float)g_volume.load() / 100.0f;
	g_voice->SetVolume(level * level);
	if (FAILED(g_voice->SubmitSourceBuffer(&buffer)) || FAILED(g_voice->Start(0))) {
		MvmDebugLog_LinefAlways("render.sound", "chime could not be queued");
		return;
	}
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.sound", "chime %s volume=%d", finished ? "finished" : "failed", g_volume.load());
}

} // namespace

void RenderDoneSound_SetEnabled(bool enabled) { g_enabled.store(enabled); }
bool RenderDoneSound_Enabled() { return g_enabled.load(); }
void RenderDoneSound_SetVolume(int percent) { g_volume.store(std::clamp(percent, 0, 100)); }
int RenderDoneSound_Volume() { return g_volume.load(); }

void RenderDoneSound_Play(bool finished) {
	if (g_enabled.load() && g_volume.load() > 0) PlayNow(finished);
}

void RenderDoneSound_Preview() {
	if (g_volume.load() > 0) PlayNow(true);
}

void RenderDoneSound_Shutdown() {
	std::lock_guard<std::mutex> lock(g_mx);
	if (g_voice) { g_voice->DestroyVoice(); g_voice = nullptr; }
	if (g_master) { g_master->DestroyVoice(); g_master = nullptr; }
	if (g_xa) { g_xa->Release(); g_xa = nullptr; }
}

} // namespace Filmmaker
