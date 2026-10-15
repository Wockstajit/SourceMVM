#pragma once

// osu easter egg song playback: the song is decoded ONCE by the bundled ffmpeg straight into
// memory (16-bit stereo 44.1 kHz, no files written) and played by XAudio2 from that buffer. The
// position comes from XAudio2's own count of samples played, so the game clock can never drift
// from the music (Windows MCI, used before, misreported positions on VBR MP3s and drifted).
// Rates are baked into the decode the way osu! sounds: DT/HT stretch time and keep the pitch
// (atempo), NC raises the pitch with the speed (asetrate). Decoding and voice setup run on a
// thread of their own; callers get a request id from Open() and poll Ready()/Failed().

#include <cstdint>
#include <string>

namespace Filmmaker {
namespace OsuAudio {

// Decode + prepare `src` (the beatmap's audio as stored; `nameHint` is its AudioFilename). rate:
// song speed (1.5 DT/NC, 0.75 HT); pitch: NC-style (pitch follows the speed). playNow: start at
// startMs at once; otherwise it waits at startMs for PlayFrom(). loopFromMs >= 0 restarts there
// whenever the song ends (song select).
uint64_t Open(const std::wstring& src, const std::string& nameHint, double startMs, double rate, bool pitch,
	bool playNow, double loopFromMs);
bool Ready(uint64_t id);        // the open for `id` finished (successfully or not)
bool Failed(uint64_t id);       // ...and failed: play on in silence
void PlayFrom(double ms);
void Pause();
void Resume();
void Stop();
void SetVolume(int volume0to1000);
int Volume();

// Song position in ms of the ORIGINAL song (independent of rate), from XAudio2's sample counter,
// carried forward between its ~10 ms updates; PollSeq() moves with every fresh reading.
double Position();
unsigned PollSeq();
bool Playing();
std::wstring CurrentSource();  // the src of the last Open (to skip re-opening the same song)


} // namespace OsuAudio
} // namespace Filmmaker
