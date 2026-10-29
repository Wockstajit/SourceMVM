#include "RenderValidation.h"

#include "RenderRange.h"

#include <cmath>
#include <sstream>

namespace Filmmaker {

void RenderValidationResult::AddFatal(const std::string& message) {
	m_errors.push_back({message, true});
	++m_fatalCount;
}

void RenderValidationResult::AddWarning(const std::string& message) {
	m_errors.push_back({message, false});
	++m_warningCount;
}

std::string RenderValidationResult::FirstFatal() const {
	for (const RenderValidationError& error : m_errors) {
		if (error.isFatal)
			return error.message;
	}
	return {};
}

const char* RenderValidationResult::Token() const {
	if (!Ok())
		return "invalid";
	return m_warningCount > 0 ? "warn" : "ok";
}

std::string RenderValidationResult::Summary() const {
	if (m_errors.empty())
		return "All validation checks passed.";

	std::ostringstream out;
	for (const RenderValidationError& error : m_errors)
		out << (error.isFatal ? "FATAL: " : "WARNING: ") << error.message << '\n';
	return out.str();
}

RenderValidationResult RenderValidate(const RenderRange& range,
	RenderCameraSource camera,
	CameraPathExtension extension,
	int outputFps,
	double demoTickRate,
	const RenderCameraPathCoverage& pathCoverage) {
	RenderValidationResult result;

	// Range modes resolve their ticks eagerly at selection time (see RenderCommand's range
	// handler): FullDemo from demo bounds, PathBounds/Segment from the camera path, CurrentRound
	// pushed back from the timeline's round markers, Custom from In/Out. So by validation time
	// every mode is just ticks -- a mode that could NOT be resolved leaves start >= end and
	// fails here with the reason named.
	if (!range.Valid()) {
		result.AddFatal(
			"Render range is unresolved or invalid (startTick must be < endTickExclusive). "
			"Check that the selected mode's data exists (demo bounds, camera keys, selection, round markers).");
	}

	if (range.Valid() && range.FrameCount(outputFps, demoTickRate) == 0)
		result.AddFatal("Output FPS or demo tick rate is invalid, or frame count overflows.");

	// Camera path. This used to be an unconditional fatal, which made camera-path rendering
	// impossible even though RenderSessionDriver has always implemented it (DrivePathCamera plus
	// the extension handling). The real check is the coverage one, and it needs facts this
	// dependency-free layer cannot look up -- so the caller passes them in.
	if (camera == RenderCameraSource::CameraPath) {
		if (!pathCoverage.resolved) {
			result.AddFatal(
				"Camera-path coverage is unresolved; active keyframe bounds must be checked.");
		} else if (pathCoverage.keyCount < 2) {
			result.AddFatal("Camera Path needs at least 2 camera keys (place them with K).");
		} else if (range.Valid()) {
			const bool startsEarly = range.startTick < pathCoverage.firstTick;
			const bool endsLate = range.endTickExclusive > pathCoverage.lastTick + 1;
			if (startsEarly || endsLate) {
				std::ostringstream why;
				why << "The render range (" << range.startTick << ".." << range.endTickExclusive
					<< ") extends beyond the camera path (" << pathCoverage.firstTick << ".."
					<< (pathCoverage.lastTick + 1) << ")";
				if (extension == CameraPathExtension::Fail) {
					why << " and the extension is Fail. Choose Hold First / Hold Last / Clamp,"
						" or shorten the range.";
					result.AddFatal(why.str());
				} else {
					// Not fatal: the driver clamps or holds the boundary key. Say so, because the
					// frames outside the path will be a still camera and that surprises people.
					why << "; the uncovered frames will hold/clamp to the boundary key.";
					result.AddWarning(why.str());
				}
			}
		}
	}

	return result;
}

void RenderValidateOutput(RenderValidationResult& result,
	bool folderChosen,
	bool formatSupported,
	const char* formatName,
	const char* formatNote) {
	if (!folderChosen) {
		result.AddFatal(
			"No output folder chosen. Pick one in Render > Output -- a job must never write "
			"into the game directory by default.");
	}
	if (!formatSupported) {
		std::string message = "Output format ";
		message += (formatName ? formatName : "?");
		message += " cannot be produced yet";
		if (formatNote && *formatNote) {
			message += ": ";
			message += formatNote;
		} else {
			message += '.';
		}
		result.AddFatal(message);
	}
}

void RenderValidateRepeatability(RenderValidationResult& result, int captureFps, double tickRate) {
	const int rate = (int)(tickRate + 0.5);
	if (captureFps <= 0 || rate <= 0)
		return;
	if (std::abs(tickRate - rate) > 0.001) {
		// CS:S's 66.67 tick: no whole frame rate lands every frame on a whole tick.
		std::ostringstream why;
		why << "At this speed each output frame spans " << tickRate / (double)captureFps
			<< " demo ticks, so re-running this exact range can sample a few individual frames one "
			   "tick apart. Passes captured together in one take are unaffected.";
		result.AddWarning(why.str());
		return;
	}
	if (captureFps % rate == 0 || rate % captureFps == 0)
		return; // whole ticks per frame (or whole frames per tick): exactly repeatable
	std::ostringstream why;
	why << "At this speed each output frame spans " << (double)rate / (double)captureFps
		<< " demo ticks, so re-running this exact range can sample a few individual frames one "
		   "tick apart (measured: 24/30 frames matched at 30 fps, 32/32 at 32 fps). Passes "
		   "captured together in one take are unaffected. For frame-exact repeatability pick a "
		   "frame rate that divides the "
		<< rate << " tick rate (e.g. 32, 64 or 128), or render in slow motion.";
	result.AddWarning(why.str());
}

} // namespace Filmmaker
