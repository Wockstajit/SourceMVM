#pragma once

// Pure engine-free trigger, envelope, and keyframe-baking math for Event-Driven FX Presets.
// Turns kills, headshots, grenade pops (flash/HE/smoke/molotov), bomb blasts, or timeline markers
// into reusable multi-action visual cues (exposure pulse + camera shake + light burst + chromatic/FOV).

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace Filmmaker::EventFx {

inline constexpr float kPi = 3.14159265358979323846f;

enum class TriggerKind : int {
	Kill = 0,
	Headshot = 1,
	FlashbangPop = 2,
	HeGrenadePop = 3,
	SmokePop = 4,
	MolotovPop = 5,
	BombExplode = 6,
	TimelineMarker = 7,
	Count
};

inline const char* TriggerToken(TriggerKind k) {
	switch (k) {
	case TriggerKind::Kill: return "kill";
	case TriggerKind::Headshot: return "headshot";
	case TriggerKind::FlashbangPop: return "flashbang";
	case TriggerKind::HeGrenadePop: return "hegrenade";
	case TriggerKind::SmokePop: return "smoke";
	case TriggerKind::MolotovPop: return "molotov";
	case TriggerKind::BombExplode: return "bomb";
	case TriggerKind::TimelineMarker: return "marker";
	default: return "kill";
	}
}

inline const char* TriggerLabel(TriggerKind k) {
	switch (k) {
	case TriggerKind::Kill: return "Kill";
	case TriggerKind::Headshot: return "Headshot";
	case TriggerKind::FlashbangPop: return "Flashbang Pop";
	case TriggerKind::HeGrenadePop: return "HE Grenade";
	case TriggerKind::SmokePop: return "Smoke Pop";
	case TriggerKind::MolotovPop: return "Molotov Burn";
	case TriggerKind::BombExplode: return "Bomb Blast";
	case TriggerKind::TimelineMarker: return "Timeline Marker";
	default: return "Kill";
	}
}

inline bool TriggerFromToken(const char* s, TriggerKind& out) {
	if (!s) return false;
	for (int i = 0; i < (int)TriggerKind::Count; ++i)
		if (0 == _stricmp(s, TriggerToken((TriggerKind)i))) { out = (TriggerKind)i; return true; }
	return false;
}

struct EventPreset {
	int id = 0;
	std::string name;
	TriggerKind trigger = TriggerKind::FlashbangPop;
	bool enabled = true;

	// Envelope timing (seconds on demo clock)
	float attackSec = 0.03f;
	float decaySec = 0.45f;

	// Action 1: Exposure & flash tint pulse
	float exposureEv = 1.6f;     // -2.0 .. +4.0 EV
	float tint[3] = { 1.0f, 1.0f, 1.0f };
	float tintStrength = 0.25f;  // 0..1

	// Action 2: Camera impulse shake + FOV punch
	float shakeAmp = 1.8f;       // degrees
	float shakeRoll = 1.2f;      // degrees
	float shakeFreq = 18.0f;     // Hz
	float fovKick = 3.0f;        // degrees (+ widens, - zooms)

	// Action 3: Native world light burst at detonation / camera
	bool lightBurst = true;
	bool lightFlashStyle = true; // true = flash burst, false = warm HE burst

	// Action 4: Chromatic aberration & vignette pulse
	float chromatic = 0.35f;     // 0..1
	float vignette = 0.20f;      // 0..1
};

struct ActiveCue {
	EventPreset preset;
	double triggerTime = 0.0;
	uint32_t seed = 1;
	bool useWallClock = false;
};

struct EvaluatedFrame {
	bool active = false;
	float exposureEv = 0.0f;
	float tint[3] = { 1.0f, 1.0f, 1.0f };
	float tintStrength = 0.0f;
	float chromatic = 0.0f;
	float vignette = 0.0f;
	float pitchDelta = 0.0f;
	float yawDelta = 0.0f;
	float rollDelta = 0.0f;
	float fovDelta = 0.0f;
};

inline float Clamp(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

// Smooth attack / exponential-quad decay envelope in [0, 1].
inline float EvaluateEnvelope(double ageSec, float attackSec, float decaySec) {
	if (ageSec < 0.0) return 0.0f;
	const float atk = (std::max)(attackSec, 0.005f);
	const float dec = (std::max)(decaySec, 0.02f);
	const float total = atk + dec;
	if ((float)ageSec >= total) return 0.0f;
	if ((float)ageSec < atk) {
		const float t = (float)ageSec / atk;
		return t * t * (3.0f - 2.0f * t);
	}
	const float u = ((float)ageSec - atk) / dec;
	const float inv = 1.0f - Clamp(u, 0.0f, 1.0f);
	return inv * inv; // crisp quadratic falloff
}

inline EvaluatedFrame EvaluateActiveCues(const std::vector<ActiveCue>& cues, double demoTime, double wallTime = -1.0) {
	EvaluatedFrame out;
	float tintR = 0.0f, tintG = 0.0f, tintB = 0.0f, tintW = 0.0f;

	for (const ActiveCue& cue : cues) {
		const double clock = (cue.useWallClock && wallTime >= 0.0) ? wallTime : demoTime;
		const double age = clock - cue.triggerTime;
		const float env = EvaluateEnvelope(age, cue.preset.attackSec, cue.preset.decaySec);
		if (env <= 1e-4f) continue;
		out.active = true;

		out.exposureEv += cue.preset.exposureEv * env;
		const float tw = Clamp(cue.preset.tintStrength, 0.0f, 1.0f) * env;
		tintR += cue.preset.tint[0] * tw;
		tintG += cue.preset.tint[1] * tw;
		tintB += cue.preset.tint[2] * tw;
		tintW += tw;

		out.chromatic += cue.preset.chromatic * env;
		out.vignette += cue.preset.vignette * env;
		out.fovDelta += cue.preset.fovKick * env;

		if (cue.preset.shakeAmp > 1e-4f || cue.preset.shakeRoll > 1e-4f) {
			const float phase = (float)age * cue.preset.shakeFreq * 2.0f * kPi;
			const float s = (float)(cue.seed % 97) * 0.37f;
			out.pitchDelta += std::sin(phase + s) * cue.preset.shakeAmp * env;
			out.yawDelta += std::cos(phase * 0.83f + s * 1.7f) * cue.preset.shakeAmp * 0.75f * env;
			out.rollDelta += std::sin(phase * 1.19f + s * 2.3f) * cue.preset.shakeRoll * env;
		}
	}

	out.exposureEv = Clamp(out.exposureEv, -3.0f, 4.5f);
	out.chromatic = Clamp(out.chromatic, 0.0f, 1.5f);
	out.vignette = Clamp(out.vignette, 0.0f, 1.0f);
	out.tintStrength = Clamp(tintW, 0.0f, 0.85f);
	if (tintW > 1e-4f) {
		out.tint[0] = Clamp(tintR / tintW, 0.0f, 1.0f);
		out.tint[1] = Clamp(tintG / tintW, 0.0f, 1.0f);
		out.tint[2] = Clamp(tintB / tintW, 0.0f, 1.0f);
	}
	return out;
}

// Built-in reusable starter presets.
inline std::vector<EventPreset> DefaultPresets() {
	std::vector<EventPreset> out;

	EventPreset flash;
	flash.id = 1; flash.name = "Flashbang Impact";
	flash.trigger = TriggerKind::FlashbangPop; flash.enabled = true;
	flash.attackSec = 0.02f; flash.decaySec = 0.55f;
	flash.exposureEv = 2.4f; flash.tint[0] = 1.0f; flash.tint[1] = 0.98f; flash.tint[2] = 0.95f; flash.tintStrength = 0.45f;
	flash.shakeAmp = 2.2f; flash.shakeRoll = 1.6f; flash.shakeFreq = 22.0f; flash.fovKick = 4.5f;
	flash.lightBurst = true; flash.lightFlashStyle = true;
	flash.chromatic = 0.45f; flash.vignette = 0.15f;
	out.push_back(flash);

	EventPreset kill;
	kill.id = 2; kill.name = "Kill Punch";
	kill.trigger = TriggerKind::Kill; kill.enabled = true;
	kill.attackSec = 0.02f; kill.decaySec = 0.28f;
	kill.exposureEv = 0.85f; kill.tint[0] = 1.0f; kill.tint[1] = 0.95f; kill.tint[2] = 0.88f; kill.tintStrength = 0.14f;
	kill.shakeAmp = 1.1f; kill.shakeRoll = 0.8f; kill.shakeFreq = 18.0f; kill.fovKick = 2.0f;
	kill.lightBurst = false; kill.chromatic = 0.22f; kill.vignette = 0.18f;
	out.push_back(kill);

	EventPreset hs;
	hs.id = 3; hs.name = "Headshot Impact";
	hs.trigger = TriggerKind::Headshot; hs.enabled = true;
	hs.attackSec = 0.02f; hs.decaySec = 0.40f;
	hs.exposureEv = 1.55f; hs.tint[0] = 1.0f; hs.tint[1] = 0.82f; hs.tint[2] = 0.35f; hs.tintStrength = 0.28f;
	hs.shakeAmp = 1.9f; hs.shakeRoll = 1.8f; hs.shakeFreq = 24.0f; hs.fovKick = 3.5f;
	hs.lightBurst = true; hs.lightFlashStyle = true;
	hs.chromatic = 0.50f; hs.vignette = 0.25f;
	out.push_back(hs);

	EventPreset he;
	he.id = 4; he.name = "HE Shockwave";
	he.trigger = TriggerKind::HeGrenadePop; he.enabled = true;
	he.attackSec = 0.03f; he.decaySec = 0.72f;
	he.exposureEv = 1.35f; he.tint[0] = 1.0f; he.tint[1] = 0.62f; he.tint[2] = 0.28f; he.tintStrength = 0.30f;
	he.shakeAmp = 3.2f; he.shakeRoll = 2.4f; he.shakeFreq = 12.0f; he.fovKick = 5.5f;
	he.lightBurst = true; he.lightFlashStyle = false;
	he.chromatic = 0.60f; he.vignette = 0.35f;
	out.push_back(he);

	EventPreset marker;
	marker.id = 5; marker.name = "Marker Beat";
	marker.trigger = TriggerKind::TimelineMarker; marker.enabled = false;
	marker.attackSec = 0.02f; marker.decaySec = 0.32f;
	marker.exposureEv = 1.1f; marker.tint[0] = 0.82f; marker.tint[1] = 0.92f; marker.tint[2] = 1.0f; marker.tintStrength = 0.18f;
	marker.shakeAmp = 0.9f; marker.shakeRoll = 0.6f; marker.shakeFreq = 16.0f; marker.fovKick = 2.5f;
	marker.lightBurst = false; marker.chromatic = 0.28f; marker.vignette = 0.15f;
	out.push_back(marker);

	return out;
}

// Generates 4 keyframes (before, peak, mid-decay, end) for baking a preset's exposure envelope
// onto the Scene Keyframes timeline starting at `startTick`.
struct BakedKey {
	double tick = 0.0;
	double exposureScale = 1.0;
};

inline std::vector<BakedKey> BuildBakedExposureKeys(const EventPreset& p, double startTick, double tickRate = 64.0) {
	const double atkTicks = (std::max)(1.0, (double)p.attackSec * tickRate);
	const double decTicks = (std::max)(2.0, (double)p.decaySec * tickRate);
	const double peakScale = std::pow(2.0, (double)Clamp(p.exposureEv, -2.0f, 3.0f));
	const double midScale = std::pow(2.0, (double)Clamp(p.exposureEv * 0.25f, -2.0f, 3.0f));
	return {
		{ startTick, 1.0 },
		{ startTick + atkTicks, peakScale },
		{ startTick + atkTicks + decTicks * 0.5, midScale },
		{ startTick + atkTicks + decTicks, 1.0 }
	};
}

} // namespace Filmmaker::EventFx
