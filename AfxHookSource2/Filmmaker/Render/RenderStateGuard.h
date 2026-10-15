#pragma once

// Captures the mutable state a render job may touch, and puts it back afterwards.
//
// WHY THIS EXISTS. A render job changes the camera, disables effects, hides the HUD, and may
// change cvars. If any of that survives the job, the user's editing session is silently
// corrupted -- and the failure mode is worst on the paths people exercise least: a cancelled or
// failed job. Restoration must therefore run on success, failure AND cancellation, and must be
// idempotent so a destructor firing after an explicit Restore() is harmless.
//
// HONEST COVERAGE. Plan section 20 lists ~25 things worth snapshotting. This guard does NOT
// cover all of them yet, and pretending otherwise is more dangerous than covering less: an
// earlier revision of this file was a pure shell whose Snapshot() body was six TODO comments,
// which reads exactly like working code at the call site. So:
//
//   * Covered() / Uncovered() return the real lists, compiled in next to the implementation.
//   * CoverageReport() formats them for "render status".
//   * Anything not yet captured is named explicitly rather than silently absent.
//
// Before relying on this guard for a new kind of mutation, add the field here FIRST. Do not
// assume a category is handled just because the guard exists.

#include <string>
#include <vector>

namespace Filmmaker {

// A cvar the job changed, recorded so it can be put back verbatim.
struct RenderCvarBackup {
	std::string name;
	std::string value;
};

class RenderStateRestoreGuard {
public:
	RenderStateRestoreGuard();
	~RenderStateRestoreGuard();

	RenderStateRestoreGuard(const RenderStateRestoreGuard&) = delete;
	RenderStateRestoreGuard& operator=(const RenderStateRestoreGuard&) = delete;

	// Captures the current state. Second and later calls are ignored, so a job cannot overwrite
	// its own baseline halfway through with values it has already modified.
	void Snapshot();

	// Puts everything back, in reverse order of capture. Idempotent, and safe to call when no
	// snapshot was taken. Runs automatically from the destructor.
	void Restore();

	bool HasSnapshot() const { return m_hasSnapshot; }
	bool IsRestored() const { return m_restored; }

	// Record a cvar's CURRENT value before the job changes it. Call BEFORE the change. Repeated
	// calls for the same name keep the first (pre-job) value.
	void TrackCvar(const char* name, const char* currentValue);
	size_t TrackedCvarCount() const { return m_cvars.size(); }

	// What this guard actually captures, and what it does not.
	static const std::vector<const char*>& Covered();
	static const std::vector<const char*>& Uncovered();
	// Human-readable summary for "mirv_filmmaker render status".
	static std::string CoverageReport();

private:
	bool m_hasSnapshot;
	bool m_restored;

	// --- camera ownership ---
	bool m_freeCam;
	bool m_thirdPersonOrbit;
	bool m_uiCursor;
	int m_movieMode; // MovieMode::Mode as int, to keep this header dependency-free

	// --- camera pose ---
	double m_origin[3];
	double m_angles[3];
	double m_fov;

	// --- camera effect modules (each is an independent user setting) ---
	bool m_smoothView;
	bool m_handheld;
	bool m_aimCrosshair;
	double m_fxWeaponSway;
	double m_fxDeadzone;
	double m_fxFloatTightness;

	std::vector<RenderCvarBackup> m_cvars;
};

} // namespace Filmmaker
