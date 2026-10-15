#pragma once

// Preflight checks for a render job, returned as structured results rather than printed text.
//
// WHY THIS EXISTS. Validation used to be a bare string ("ok" / "invalid" / "unresolved") computed
// inside the console command. That is enough to colour a button and nothing else: the UI cannot
// say WHICH check failed, the render session cannot decide whether a problem is fatal or merely
// worth warning about, and no test can assert on it. Plan section 25 acceptance criterion 15
// requires validation to block impossible configurations BEFORE any game state is mutated, which
// means the session has to be able to ask "may I proceed?" and get a real answer.
//
// FATAL vs WARNING is the distinction that matters:
//   * fatal   -- the job cannot run. Start must refuse.
//   * warning -- the job can run but the user is probably about to get something they did not
//                intend (e.g. a camera-path range whose coverage cannot be confirmed at this
//                layer). Surface it; do not block.
//
// DELIBERATELY DEPENDENCY-FREE. This header and its .cpp touch nothing but RenderTypes and
// RenderRange, so the whole thing is unit-testable without the engine. Checks that genuinely
// need live demo data (round boundaries, demo bounds, camera-path spans, target resolution) are
// NOT silently skipped -- they are reported as unresolved, which is a fatal result, so a job can
// never start on a range this layer could not actually confirm.

#include "RenderTypes.h"

#include <string>
#include <vector>

namespace Filmmaker {

struct RenderRange;

class RenderValidationResult {
public:
	void AddFatal(const std::string& message);
	void AddWarning(const std::string& message);

	// True when nothing fatal was found. Warnings do not block.
	bool Ok() const { return m_fatalCount == 0; }
	int FatalCount() const { return m_fatalCount; }
	int WarningCount() const { return m_warningCount; }
	const std::vector<RenderValidationError>& Errors() const { return m_errors; }

	// First fatal message, or an empty string when Ok(). Handy for a one-line UI readout.
	std::string FirstFatal() const;

	// Short machine-ish token for the existing state bridge: "ok" | "warn" | "invalid".
	// Kept so the Panorama side keeps working unchanged.
	const char* Token() const;

	// Multi-line human summary for the console.
	std::string Summary() const;

private:
	std::vector<RenderValidationError> m_errors;
	int m_fatalCount = 0;
	int m_warningCount = 0;
};

// Camera-path facts, supplied by the caller so this file stays engine-free. `resolved` false
// means the caller could not query the path at all -- which stays fatal, because a job must never
// start on a camera whose coverage nobody confirmed.
struct RenderCameraPathCoverage {
	bool resolved = false;
	int keyCount = 0;
	int firstTick = 0;
	int lastTick = 0;
};

// Validates everything this layer can confirm without touching the engine. The camera-path
// coverage argument defaults to unresolved, which is deliberately the strict answer.
RenderValidationResult RenderValidate(const RenderRange& range,
	RenderCameraSource camera,
	CameraPathExtension extension,
	int outputFps,
	double demoTickRate,
	const RenderCameraPathCoverage& pathCoverage = RenderCameraPathCoverage());

// Output/pass checks, appended to an existing result. Kept as plain parameters (not a
// RenderOutputSettings reference) so this file stays free of RenderOutput -- and therefore of
// <windows.h>, the folder picker and the recorder -- and remains unit-testable standalone.
// `formatNote` is the human reason an unsupported format is unavailable; it is quoted verbatim
// into the fatal entry so the UI never has to invent an explanation.
void RenderValidateOutput(RenderValidationResult& result,
	bool folderChosen,
	bool formatSupported,
	const char* formatName,
	const char* formatNote);

// Frame-exact REPEATABILITY warning. When the sampling rate does not divide the demo tick rate,
// each output frame spans a fractional number of ticks and the engine alternates step sizes (at
// 1x / 30 fps: 2.13 ticks per frame, so 2- and 3-tick steps). Re-running the same range can then
// sample individual frames one tick either side of a boundary. MEASURED 2026-08-10: two 1x takes
// at 30 fps matched on 24/30 frames; the same pair at 32 fps matched 32/32.
//
// This is a WARNING, never fatal: passes captured together in one take share the same source
// frame and are aligned regardless (contract rule 9). It only matters when RE-RUNNING a take.
void RenderValidateRepeatability(RenderValidationResult& result, int captureFps, double tickRate);

} // namespace Filmmaker
