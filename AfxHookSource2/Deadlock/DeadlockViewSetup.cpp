#include "DeadlockViewSetup.h"
#include "DeadlockCameraCommands.h"

#include "DeadlockAddresses.h"
#include "DeadlockBoot.h"
#include "DeadlockCameraRuntime.h"
#include "DeadlockEngine.h"
#include "DeadlockFrame.h"
#include "DeadlockViewMath.h"
#include <chrono>

#include "../Globals.h"
#include "../../Shared/AfxConsole.h"
#include "../../Shared/AfxDetours.h"
#include "../Filmmaker/Debug/MvmDebugLog.h"
#include "../Filmmaker/Deadlock/DeadlockDemoClockMath.h"
#include "../Filmmaker/Deadlock/DeadlockGraph.h"
#include "../Filmmaker/Deadlock/DeadlockPathShake.h"
#include "../Filmmaker/Deadlock/DeadlockShot.h"
#include "../Filmmaker/Movie/CameraBridge.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <intrin.h>

namespace Deadlock {

namespace {

// MEASURED against the installed Deadlock build on 2026-09-10, with `mirv_filmmaker view dump`.
//
// The view is embedded at CViewRender +0x10. Reference offsets are relative to
// that embedded view; these offsets are relative to CViewRender itself.
// The measured viewport is 1440x810, origin +0x4b0, FOV +0x4a8, aspect +0x4e8.
ViewOffsets g_offsets = {
	/* origin */ 0x4b0,
	/* angles */ 0x4c8,
	/* fov    */ 0x4a8,
	/* width  */ 0x440,
	/* height */ 0x448,
};

// The viewport ORIGIN displacements, for the preview rect. MEASURED in game on build 6687.
//
// These were inferred first, and inferred WRONG: +0x438/+0x43c, reasoned from the scaled and
// unscaled sizes sitting interleaved at +0x440/+0x444 (width) and +0x448/+0x44c (height), which
// looks like Source 2's {x, y, width, height} block with each entry doubled and would put the
// origin in the two dwords immediately before the width.
//
// What the block actually holds, read off a live `view dump 0x430 0x450` with the preview active:
//
//     +0x430  x                +0x434  y
//     +0x438  0                +0x43c  0          <- a second origin the engine leaves at zero
//     +0x440  width            +0x444  1920       <- unscaled width
//     +0x448  height           +0x44c  1080       <- unscaled height
//
// WHY THE WRONG GUESS SURVIVED SO LONG. It passed its own read-back -- the write landed in a real
// field and stuck -- so the stand-down never fired and `engine accepted=yes`. The size still took
// effect, so the world VISIBLY shrank and the feature looked like it worked. Only the origin was
// lost, leaving the world drawn at the top-left of the frame while the frame sat where the layout
// put it, and the L-shaped remainder inside the frame held whatever was last rendered there. That
// presents as "part of the preview is a static image from earlier and nothing scales correctly",
// which reads as a scaling bug rather than as a wrong struct offset.
//
// The lesson is in the check, not the number: a read-back proves the WRITE took, never that the
// field was the right one. The proof here is the picture -- the engine's viewport and the drawn
// frame now agree to the pixel on all three dock heights (0,120,1400,907 / 181,44,1219,628 /
// 0,52,1400,839), compared against the frame edges found in a screenshot.
//
// Still settable at runtime, so a future build that moves the block can be corrected from a live
// `view dump` without a rebuild.
ptrdiff_t g_viewportXOffset = 0x430;
ptrdiff_t g_viewportYOffset = 0x434;

// OFF BY DEFAULT, because this mechanism is the WRONG ONE and it renders incorrectly.
//
// Shrinking the engine's own viewport does scale the world into the frame, but it also changes
// what the engine is rendering, and three faults follow from that:
//
//   * CULLING FOLLOWS THE VIEWPORT. The frustum and the occlusion pass are solved against the
//     smaller rect, so geometry near the frame's edge is culled away -- reported as "stuff
//     auto-culling itself, black empty space in the world".
//   * NOTHING CLEARS THE REMAINDER. The engine only writes the rect it was given, so every pixel
//     of the render target outside it keeps the previous frame -- old frames and our own graph
//     editor showing through from underneath.
//   * THE OTHER FIELDS IN THE BLOCK STILL SAY FULL SCREEN. +0x438/+0x43c and +0x444/+0x44c hold a
//     second origin and the unscaled size; whatever reads those (the world-space HUD projection is
//     the visible one) places against a rect the world is not in, so the health bars land wrong.
//
// CS2 SOLVES THIS BY NOT TOUCHING THE VIEW AT ALL. See ViewportScaler.h: the world renders
// FULL-SCREEN with the camera and projection untouched, and then, at a callback that runs after
// the world and before Panorama composites, a D3D pass copies the backbuffer, CLEARS it, and
// redraws it scaled into the preview rect. Full-screen render means culling is unchanged; the
// explicit clear means there is no remainder to go stale; and the HUD's CSS transform targets the
// same rect, so it lands on its own world.
//
// That port is blocked on one thing: Deadlock's only render hook is IDXGISwapChain::Present
// (DeadlockPresentHook.cpp), which runs AFTER Panorama has composited -- blitting there would
// shrink our own chrome along with the world. CS2 gets its pre-UI moment from a callback it
// registers inside rendersystemdx11.dll's render-pass command list, and that registration has not
// been derived for Deadlock yet.
//
// Until it is, the honest fallback is a full-screen render with the preview frame drawn as a CROP
// indicator: it shows more of the world than the frame implies, but every pixel of it is correct.
// `mirv_filmmaker preview on` re-arms the viewport write for anyone who wants to keep measuring it.
bool g_previewEnabled = false;     // diagnostic fallback only; the pre-UI frame blit owns scaling
bool g_previewActive = false;      // a panel wants it this frame
bool g_previewTaking = false;      // the engine accepted the last write
bool g_previewStoodDown = false;   // a write did not read back; stop trying this session
float g_previewRect[4] = { 0, 0, 0, 0 };
int g_previewBaseW = 0, g_previewBaseH = 0;  // the engine's own viewport, to restore
std::atomic<unsigned long long> g_previewWrites{ 0 };
std::atomic<unsigned long long> g_previewRejects{ 0 };

ViewPhase g_phase = ViewPhase::After;

bool g_installed = false;

// A kill switch for the hook's own body. The detour stays installed and still counts calls, but
// touches nothing -- which is how "is the camera hook what is breaking this" gets answered in one
// launch instead of by rebuilding twice.
bool g_enabled = true;

bool g_writeEnabled = true;

std::atomic<unsigned long long> g_callCount{ 0 };
std::atomic<unsigned long long> g_writeCount{ 0 };
// Calls rejected by the gate, split by reason, because "the camera does not move" and "the camera
// moved the wrong view" are different problems with the same symptom.
std::atomic<unsigned long long> g_rejectVtable{ 0 };
std::atomic<unsigned long long> g_rejectCaller{ 0 };
std::atomic<unsigned long long> g_rejectSanity{ 0 };

// Last observed values, for the report. Not synchronised: this is diagnostic output, and a torn
// read of a float triple is not worth a lock on the view path.
float g_lastOrigin[3] = { 0, 0, 0 };
float g_lastAngles[3] = { 0, 0, 0 };
float g_lastFov = 0.0f;
int g_lastWidth = 0;
int g_lastHeight = 0;

// The last object the gate accepted as the main view. Kept so the layout can be dumped from the
// console AFTER the fact: the offsets in this file are inherited guesses, and the only way to tell
// a wrong offset from wrong maths is to look at what is actually there.
void* g_lastMainBase = nullptr;

// Shared with DeadlockShot rather than reimplemented in both. The tolerances are measured, and
// having two copies of a measured constant means fixing it twice.
Filmmaker::DemoClock g_clock;

// Build 6774 ClientModeCitadelNormal slot15 fills the embedded CViewSetup in rdx.
// Its caller supplies only (mode, view) and does not consume a return value. Stack
// home-slot spills do not establish argument count; the caller and body do.
using ViewSetupFn_t = void(__fastcall*)(void* mode, void* view);
ViewSetupFn_t g_original = nullptr;
DWORD g_viewInstallThread = 0;

// Reads and writes go through helpers so a bad offset faults in one identifiable place.
bool ReadPointer(void* at, size_t& out) {
	__try {
		std::memcpy(&out, at, sizeof(out));
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool ReadView(void* base, float origin[3], float angles[3], float& fov,
              int& width, int& height) {
	__try {
		std::memcpy(origin, (unsigned char*)base + g_offsets.origin, sizeof(float) * 3);
		std::memcpy(angles, (unsigned char*)base + g_offsets.angles, sizeof(float) * 3);
		std::memcpy(&fov, (unsigned char*)base + g_offsets.fov, sizeof(float));
		std::memcpy(&width, (unsigned char*)base + g_offsets.width, sizeof(int));
		std::memcpy(&height, (unsigned char*)base + g_offsets.height, sizeof(int));
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool WriteView(void* base, const float origin[3], const float angles[3], float fov) {
	__try {
		std::memcpy((unsigned char*)base + g_offsets.origin, origin, sizeof(float) * 3);
		std::memcpy((unsigned char*)base + g_offsets.angles, angles, sizeof(float) * 3);
		std::memcpy((unsigned char*)base + g_offsets.fov, &fov, sizeof(float));
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Writes the preview rect into the view's viewport fields, then reads all four straight back.
//
// The read-back is the whole point. A wrong displacement does not fault -- the memory is committed
// -- so a write to the wrong dword looks exactly like a write to the right one right up until the
// frame renders wrong. Comparing what came back against what went in is the only cheap way to tell
// them apart, and it is what lets a bad inference stand itself down instead of corrupting a
// neighbouring field every frame for the rest of the session.
bool WriteViewportRect(void* base, int x, int y, int w, int h) {
	__try {
		unsigned char* p = (unsigned char*)base;
		std::memcpy(p + g_viewportXOffset, &x, sizeof(int));
		std::memcpy(p + g_viewportYOffset, &y, sizeof(int));
		std::memcpy(p + g_offsets.width, &w, sizeof(int));
		std::memcpy(p + g_offsets.height, &h, sizeof(int));

		int rx = 0, ry = 0, rw = 0, rh = 0;
		std::memcpy(&rx, p + g_viewportXOffset, sizeof(int));
		std::memcpy(&ry, p + g_viewportYOffset, sizeof(int));
		std::memcpy(&rw, p + g_offsets.width, sizeof(int));
		std::memcpy(&rh, p + g_offsets.height, sizeof(int));
		return rx == x && ry == y && rw == w && rh == h;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Applies whatever the preview owner published, or restores the engine's own viewport. Called from
// the same place the pose is written, on the accepted main view only.
void ApplyPreviewViewport(void* base, int baseWidth, int baseHeight) {
	if (baseWidth > 0 && baseHeight > 0) {
		// Remember the engine's own numbers BEFORE overwriting them, and only while we are not
		// already overriding -- otherwise the second frame would record our own shrunk viewport as
		// the thing to restore and the preview would ratchet down to nothing.
		if (!g_previewTaking) { g_previewBaseW = baseWidth; g_previewBaseH = baseHeight; }
	}

	const bool want = g_previewEnabled && g_previewActive && !g_previewStoodDown
		&& g_previewBaseW > 0 && g_previewBaseH > 0;

	if (!want) {
		if (g_previewTaking) {
			// Put the engine's viewport back exactly once, on the transition. Writing it every
			// frame would fight a resolution change.
			WriteViewportRect(base, 0, 0, g_previewBaseW, g_previewBaseH);
			g_previewTaking = false;
		}
		return;
	}

	const int x = (int)(g_previewRect[0] * g_previewBaseW + 0.5f);
	const int y = (int)(g_previewRect[1] * g_previewBaseH + 0.5f);
	const int w = (int)((g_previewRect[2] - g_previewRect[0]) * g_previewBaseW + 0.5f);
	const int h = (int)((g_previewRect[3] - g_previewRect[1]) * g_previewBaseH + 0.5f);

	// A viewport this small is not a preview, and a zero one is a divide by zero inside the
	// engine's projection. Refuse rather than find out.
	if (w < 32 || h < 32 || x < 0 || y < 0
		|| x + w > g_previewBaseW || y + h > g_previewBaseH) {
		g_previewRejects.fetch_add(1);
		return;
	}

	if (WriteViewportRect(base, x, y, w, h)) {
		g_previewTaking = true;
		g_previewWrites.fetch_add(1);
		return;
	}

	// The read-back disagreed. One strike is enough: the displacements are wrong, and every further
	// write lands in a field belonging to something else.
	g_previewStoodDown = true;
	g_previewTaking = false;
	g_previewRejects.fetch_add(1);
	advancedfx::Warning(
		"AfxHookDeadlock: the preview viewport write did not read back, so the viewport X/Y\n"
		"displacements (+0x%zx/+0x%zx) are wrong for this build. The workspace falls back to\n"
		"framing a crop. Correct them with 'mirv_filmmaker preview offsets <x> <y>' after reading\n"
		"'mirv_filmmaker view dump'.\n",
		(size_t)g_viewportXOffset, (size_t)g_viewportYOffset);
}

// Seconds since the previous view setup. All the interesting behaviour -- backwards jitter is not
// a seek, a forward hitch is clamped rather than integrated -- lives in DeadlockDemoClockMath.h
// with its own unit test, because it is the sort of arithmetic that is wrong for months before
// anyone notices.
float FrameDelta() {
	using Clock = std::chrono::steady_clock;
    static auto previous = Clock::now();
    const auto now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - previous).count();
    previous = now;
    const double demoDelta = g_clock.Sample(Engine_DemoTime()).delta;
    const bool freeCam = Filmmaker::CameraBridge_GetFreeCamEnabled();
    const float dt = (float)(freeCam ? CameraFrameDelta(elapsed) : demoDelta);
    // DIAGNOSTIC (2026-09-14, free-cam-frozen-while-paused report): which branch fed the view
    // override this frame, and what each branch actually held. Read alongside deadlock.freecamdt
    // (ApplyOverride) -- this answers "did FrameDelta hand over a live wall-clock number", that one
    // answers "did the override actually move the camera with it".
    if (Filmmaker::MvmDebugLog_Active()) {
        static unsigned s_n = 0;
        if ((s_n++ % 30) == 0)
            Filmmaker::MvmDebugLog_LinefAlways("deadlock.framedelta",
                "freeCam=%d paused=%d elapsed=%.4f demoDelta=%.4f dt=%.4f",
                freeCam ? 1 : 0, Engine_IsDemoPaused() ? 1 : 0, elapsed, demoDelta, dt);
    }
    return dt;
}

// Is this call the MAIN view, on an object whose layout we understand?
//
// This gate is the whole difference between a working hook and one that kills the process, and it
// was what the first build was missing. The view-setup function is called for more than one view
// per frame and on more than one kind of object. Writing origin, angles and fov at +0x4a0 of
// whatever gets passed in does not FAULT -- that memory is committed -- so the process survives
// the write and dies later somewhere unrelated. The boot trace showed exactly that: the first call
// completed, then silence.
//
// Two independent conditions, both derived from the installed Deadlock client:
//   * the object's vtable is the view class, so the offsets mean what we think they mean, and
//   * the return address is the one call site that sets up the main view.
// Both identities and the initialization thread are mandatory; unresolved gates never permit writes.
bool IsMainView(void* base, void* callerReturn) {
	if (!g_ViewVtable || !g_ViewSetupCallerReturn || GetCurrentThreadId() != g_viewInstallThread) return false;
	if (g_ViewVtable) {
		size_t vtable = 0;
		if (!ReadPointer(base, vtable) || vtable != g_ViewVtable) {
			g_rejectVtable.fetch_add(1);
			return false;
		}
	}
	if (g_ViewSetupCallerReturn && (size_t)callerReturn != g_ViewSetupCallerReturn) {
		g_rejectCaller.fetch_add(1);
		return false;
	}
	return true;
}

// Does the view read back as a plausible camera? A vtable match says the offsets are meaningful; it
// does not say the view has been filled in yet. An uninitialised or off-screen view has a zero
// viewport or a nonsense field of view, and writing into one puts the camera somewhere impossible.
bool ViewLooksSane(const float origin[3], const float angles[3], float fov,
                   int width, int height) {
	for (int i = 0; i < 3; ++i) {
		if (!std::isfinite(origin[i]) || !std::isfinite(angles[i])) return false;
	}
	if (!std::isfinite(fov) || fov <= 1.0f || fov >= 179.0f) return false;
	if (width <= 0 || height <= 0) return false;
	return true;
}

// Runs the override stack over the view in `base` and writes it back when anything claimed it.
void ApplyOverride(void* base, float deltaSeconds) {
	float origin[3], angles[3], fov;
	int width = 0, height = 0;
	if (!ReadView(base, origin, angles, fov, width, height)) return;

	if (!ViewLooksSane(origin, angles, fov, width, height)) {
		g_rejectSanity.fetch_add(1);
		return;
	}

	g_lastWidth = width;
	g_lastHeight = height;

	// The preview viewport rides the same accepted-view gate as the pose. It has to: this is the
	// only place in the process that knows the object is the main view and that its layout is
	// understood, and a viewport written into anything else is the same class of mistake as a pose
	// written into anything else.
	ApplyPreviewViewport(base, width, height);
	CameraRuntime_SetViewportSize(width, height);

	const double gameOrigin[3] = { origin[0], origin[1], origin[2] };
	const double gameAngles[3] = { angles[0], angles[1], angles[2] };
	CameraRuntime_SetGameCamera(gameOrigin, gameAngles, fov);

	float x = origin[0], y = origin[1], z = origin[2];
	float pitch = angles[0], yaw = angles[1], roll = angles[2];
	float outFov = fov;

	// SOMEBODY HAS TO CLAIM THE CAMERA BEFORE WE TOUCH IT.
	//
	// The first working build used MirvInput's return value as the claim, which is wrong:
	// MirvInput::Override returns true for its own internal reasons, not to say "a user feature
	// owns the view this frame". So on a game sitting in the dashboard with nothing enabled, the
	// hook wrote a pose built from MirvInput's zero-initialised state into the real view every
	// frame -- origin at the world origin, and whatever field of view its pending-set flags
	// happened to hold. Deadlock survives the first frames of that and then shuts down cleanly,
	// which is the hardest failure of the lot to read: no crash, no dump, no event-log entry.
	//
	// So the claim is explicit, and it is the union of the features that actually drive a camera.
	// The union is DeadlockViewOwnerMath.h's resolver. It used to be free cam || shot || graph, which
	// left the follow camera out entirely: its pose was pushed every frame and never applied.
	const ViewOwner owner = CameraRuntime_ResolveOwner();
	// Recorded so anything reporting the owner from OUTSIDE this render-thread call (a console
	// command, `mirv_filmmaker view`/`camera`) reads what was ACTUALLY just applied rather than
	// recomputing the resolver fresh at whatever moment it happens to run -- see
	// CameraRuntime_OwnerName()'s declaration for the staleness this fixes (measured live in P5: a
	// console report named "the game" while the follow camera's pose was demonstrably being
	// applied every frame).
	CameraRuntime_RecordAppliedOwner(owner);
	const bool baseClaimed = ViewOwnerWrites(owner);
	{
		static int s_lastOwner = -1;
		if ((int)owner != s_lastOwner) {
			if (Filmmaker::MvmDebugLog_Active())
				Filmmaker::MvmDebugLog_LinefAlways("deadlock.viewowner", "owner=%s was=%d pump=%llu",
					ViewOwnerName(owner), s_lastOwner, Frame_MainCount());
			s_lastOwner = (int)owner;
		}
	}
	// Handheld camera shake (DeadlockCameraEffects.cpp, the HandheldCam bridge) was removed
	// entirely 2026-09-14 at the user's explicit request: Deadlock's Config has no Effects page to
	// expose or disable it from, yet it could still claim the view and shake the camera whenever it
	// happened to be left enabled (a stray console `handheld` test earlier in this project's own
	// testing had persisted `enabled: true` into the settings sidecar, so it silently re-armed on
	// every subsequent launch with no UI control anywhere to turn it back off). Zero effects wired.
	const bool claimed = baseClaimed || CameraCommands_Active();

	if (!claimed) {
		// Nothing owns the view. Record what the engine produced, so the free camera starts from
		// the right place the moment it IS enabled, and write nothing.
		//
		// The report reads these too. It used to show only what had been WRITTEN, so with nothing
		// claiming the camera it printed an origin of (0,0,0) and a field of view of 0 no matter
		// what was actually in the struct -- indistinguishable from offsets that do not resolve,
		// which is exactly the question the report exists to answer.
		g_lastOrigin[0] = origin[0]; g_lastOrigin[1] = origin[1]; g_lastOrigin[2] = origin[2];
		g_lastAngles[0] = angles[0]; g_lastAngles[1] = angles[1]; g_lastAngles[2] = angles[2];
		g_lastFov = fov;
		CameraRuntime_SetLastCamera(gameOrigin, gameAngles, fov);
		CameraCommands_Record(width, height, origin[0], origin[1], origin[2], angles[0], angles[1], angles[2], fov);
		return;
	}

	// One-shot markers on the claimed path, into the boot trace rather than the console. Enabling
	// the free camera took the process down with no dump and no log, and a console reply cannot be
	// read by a process that is already gone -- the boot trace writes through raw kernel32 and
	// flushes every line, so it survives.
	static bool tracedClaim = false;
	if (!tracedClaim) { tracedClaim = true; DL_BOOT("override: claimed, calling runtime"); }

	bool changed = baseClaimed
		? CameraRuntime_Override(deltaSeconds, x, y, z, pitch, yaw, roll, outFov)
		: false;
	// DIAGNOSTIC (2026-09-14, free-cam-frozen-while-paused report): the frame delta CameraRuntime_
	// Override just consumed, and what MirvInput actually did with it. If dt stays 0 while free cam
	// is enabled and the demo is paused, FrameDelta's wall-clock branch is not engaging; if dt is
	// healthy but `changed` is still false / x,y,z do not move frame to frame, the stall is inside
	// MirvInput::Override or upstream of it (stale WASD state, a claim MirvInput itself is refusing).
	if (Filmmaker::MvmDebugLog_Active()) {
		static unsigned s_n = 0;
		if ((s_n++ % 15) == 0)
			Filmmaker::MvmDebugLog_LinefAlways("deadlock.freecamdt",
				"owner=%s baseClaimed=%d paused=%d dt=%.4f changed=%d pos=%.1f,%.1f,%.1f",
				ViewOwnerName(owner), baseClaimed ? 1 : 0, Engine_IsDemoPaused() ? 1 : 0,
				deltaSeconds, changed ? 1 : 0, x, y, z);
	}
	if (baseClaimed && Filmmaker::DeadlockPathShake_ApplyToView(
		x, y, z, pitch, yaw, roll, outFov))
		changed = true;
	if (CameraCommands_Apply(width, height, x, y, z, pitch, yaw, roll, outFov))
		changed = true;

	static bool tracedRuntime = false;
	if (!tracedRuntime) { tracedRuntime = true; DL_BOOT("override: runtime returned"); }

	// Validate what we are about to write, not just what we read. An override stack that produces
	// a non-finite position or a degenerate field of view makes the projection matrix invalid, and
	// the engine's response to that is not a visual glitch.
	const bool sane = std::isfinite(x) && std::isfinite(y) && std::isfinite(z)
	               && std::isfinite(pitch) && std::isfinite(yaw) && std::isfinite(roll)
	               && std::isfinite(outFov) && outFov > 1.0f && outFov < 179.0f;

	bool applied = false;
	if (changed && sane) {
		// The POSE, not just the fact that a write happened. "write returned" followed by a clean
		// process exit says the write succeeded and the engine disliked the result, and the only
		// thing that distinguishes a plausible pose from a destructive one is the numbers. Four
		// one-shot lines, because the first write is the one that matters.
		static int tracedWrites = 0;
		if (tracedWrites < 4) {
			++tracedWrites;
			char line[320];
			wsprintfA(line,
				"override: writing origin=(%d %d %d) angles=(%d %d %d) fov=%d  "
				"was origin=(%d %d %d) angles=(%d %d %d) fov=%d",
				(int)x, (int)y, (int)z, (int)pitch, (int)yaw, (int)roll, (int)outFov,
				(int)origin[0], (int)origin[1], (int)origin[2],
				(int)angles[0], (int)angles[1], (int)angles[2], (int)fov);
			DL_BOOT(line);
		}

		const float newOrigin[3] = { x, y, z };
		const float newAngles[3] = { pitch, yaw, roll };
		applied = g_writeEnabled && WriteView(base, newOrigin, newAngles, outFov);
		if (applied)
			g_writeCount.fetch_add(1);

		static bool tracedWrote = false;
		if (!tracedWrote) { tracedWrote = true; DL_BOOT("override: write returned"); }
	} else if (changed && !sane) {
		g_rejectSanity.fetch_add(1);
	}
	if (changed && !applied) {
		x = origin[0]; y = origin[1]; z = origin[2];
		pitch = angles[0]; yaw = angles[1]; roll = angles[2]; outFov = fov;
	}

	g_lastOrigin[0] = x; g_lastOrigin[1] = y; g_lastOrigin[2] = z;
	g_lastAngles[0] = pitch; g_lastAngles[1] = yaw; g_lastAngles[2] = roll;
	g_lastFov = outFov;

	// Every claimed frame for the first few seconds, never deduplicated. The exit lands a handful
	// of frames after the camera is claimed, and the throttled every-64th-frame probe in the
	// detour cannot place it: the log needs the read pose, the written pose and the write result
	// on the exact frame before the engine decided to leave.
	if (Filmmaker::MvmDebugLog_Active()) {
		static unsigned s_claimedFrames = 0;
		if (s_claimedFrames < 240) {
			++s_claimedFrames;
			Filmmaker::MvmDebugLog_LinefAlways("deadlock.view.claimed",
				"n=%u base=%p phase=%s dt=%.4f read=(%.2f %.2f %.2f | %.2f %.2f %.2f | %.2f) "
				"wrote=(%.2f %.2f %.2f | %.2f %.2f %.2f | %.2f) changed=%d sane=%d writeOn=%d "
				"writes=%llu",
				s_claimedFrames, base, ViewSetup_PhaseName(g_phase), deltaSeconds,
				origin[0], origin[1], origin[2], angles[0], angles[1], angles[2], fov,
				x, y, z, pitch, yaw, roll, outFov,
				changed ? 1 : 0, sane ? 1 : 0, g_writeEnabled ? 1 : 0,
				g_writeCount.load());
		}
	}

	const double usedOrigin[3] = { x, y, z };
	const double usedAngles[3] = { pitch, yaw, roll };
	CameraRuntime_SetLastCamera(usedOrigin, usedAngles, outFov);
	CameraCommands_Record(width, height, x, y, z, pitch, yaw, roll, outFov);
}

// Opt-in evidence collection for game updates. This callback observes only; it must never
// drive Frame_MainThread or ApplyOverride until its thread/call-chain contract is established.
using ViewProbeFn = void(__fastcall*)(void*);
ViewProbeFn g_probeOriginal = nullptr;
std::atomic<unsigned long long> g_probeCalls{0};
DWORD g_probeInstallThread = 0;
bool g_probeInstalled = false;

void __fastcall New_ViewProbe(void* base) {
	void* caller = _ReturnAddress();
	const auto n = g_probeCalls.fetch_add(1) + 1;
	g_probeOriginal(base);
	if (!Filmmaker::MvmDebugLog_Active() || n > 16384 || (n > 4 && n % 256)) return;
	float origin[3]{}, angles[3]{}, fov = 0;
	int width = 0, height = 0;
	const bool read = ReadView(base, origin, angles, fov, width, height);
	Filmmaker::MvmDebugLog_LinefAlways("deadlock.view.probe",
		"n=%llu base=%p caller=%p thread=%lu installThread=%lu read=%d "
		"origin=(%.3f %.3f %.3f) angles=(%.3f %.3f %.3f) fov=%.3f viewport=%dx%d",
		n, base, caller, GetCurrentThreadId(), g_probeInstallThread, read,
		origin[0], origin[1], origin[2], angles[0], angles[1], angles[2], fov, width, height);
	if (n <= 4) {
		void* stack[12]{};
		const USHORT count = CaptureStackBackTrace(0, 12, stack, nullptr);
		for (USHORT i = 0; i < count; ++i) {
			HMODULE module = nullptr;
			char path[MAX_PATH]{};
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                  (LPCSTR)stack[i], &module);
			if (module) GetModuleFileNameA(module, path, MAX_PATH);
			const char* name = std::strrchr(path, '\\');
			Filmmaker::MvmDebugLog_LinefAlways("deadlock.view.probe.stack",
				"n=%llu frame=%u module=%s rva=0x%zx address=%p", n, i,
				name ? name + 1 : path, module ? (size_t)stack[i] - (size_t)module : 0, stack[i]);
		}
	}
}

void __fastcall New_ViewSetup(void* mode, void* view) {
	void* const thisPtr = view ? (void*)((uintptr_t)view - 0x10) : nullptr;
	const unsigned long long call = g_callCount.fetch_add(1);
	const bool firstCall = (call == 0);
	if (firstCall) DL_BOOT("view setup: first call, entered");

	// The return address identifies WHICH call site invoked us, which is how the main view is told
	// apart from the others. Read it first, before anything can disturb the frame.
	void* const callerReturn = _ReturnAddress();

	// Camera acceptance is independent of the frame-stage main/UI pump.
	const bool isMain = thisPtr && IsMainView(thisPtr, callerReturn);
	if (isMain) g_lastMainBase = thisPtr;

	if (!isMain) {
		// Not our view: call through and touch nothing. Nearly every call takes this path, so it
		// stays as short as the original function plus a compare.
		g_original(mode, view);
		return;
	}

	if (!g_enabled) {
		g_original(mode, view);
		if (firstCall) DL_BOOT("view setup: body off, passthrough only");
		return;
	}

	const float dt = FrameDelta();
	if (firstCall) DL_BOOT("view setup: main view, frame delta ok");

	if (g_phase == ViewPhase::Before || g_phase == ViewPhase::Both)
		ApplyOverride(thisPtr, dt);

	if (firstCall) DL_BOOT("view setup: calling original");
	g_original(mode, view);
	if (firstCall) DL_BOOT("view setup: original returned");

	if (g_phase == ViewPhase::After || g_phase == ViewPhase::Both)
		ApplyOverride(thisPtr, g_phase == ViewPhase::Both ? 0.0f : dt);
	if (firstCall) DL_BOOT("view setup: override applied");


	if (firstCall) DL_BOOT("view setup: first call complete");

	// Throttled, because this is the hottest function in the hook. A console reply only shows the
	// instant it was typed; ordering and "what did the view look like two hundred frames ago" exist
	// only in the log.
	if (Filmmaker::MvmDebugLog_Active()) {
		const unsigned long long n = g_callCount.load();
		if ((n % 64) == 0) {
			Filmmaker::MvmDebugLog_LinefAlways("deadlock.view",
				"n=%llu writes=%llu dt=%.4f base=%p origin=(%.2f %.2f %.2f) "
				"angles=(%.2f %.2f %.2f) fov=%.2f vp=%dx%d phase=%s",
				n, g_writeCount.load(), dt, thisPtr,
				g_lastOrigin[0], g_lastOrigin[1], g_lastOrigin[2],
				g_lastAngles[0], g_lastAngles[1], g_lastAngles[2],
				g_lastFov, g_lastWidth, g_lastHeight,
				ViewSetup_PhaseName(g_phase));
		}
	}

}

} // namespace

bool ViewSetup_Install() {
	if (g_installed) return true;
	if (!g_ClientViewSetupFn || !g_ClientModeVtable || !g_ViewVtable || !g_ViewSetupCallerReturn) {
		advancedfx::Warning(
			"AfxHookDeadlock: view setup not resolved; the camera cannot be driven.\n");
		return false;
	}

	// An inline detour on the function itself, not a vtable pointer swap: the target is a raw
	// address from a pattern scan, and nothing hands us a pointer slot to rewrite.
	g_original = (ViewSetupFn_t)g_ClientViewSetupFn;
	g_viewInstallThread = GetCurrentThreadId();
	if (DetourTransactionBegin() != NO_ERROR) { g_original = nullptr; return false; }
	if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR
	    || DetourAttach(&(PVOID&)g_original, New_ViewSetup) != NO_ERROR) {
		DetourTransactionAbort();
		g_original = nullptr;
		return false;
	}
	if (NO_ERROR != DetourTransactionCommit()) {
		g_original = nullptr;
		advancedfx::Warning("AfxHookDeadlock: could not detour the view setup function.\n");
		return false;
	}

	g_installed = true;
	advancedfx::Message("AfxHookDeadlock: view setup hooked at rva 0x%zx (phase %s).\n",
	                    g_ClientViewSetupFn - (size_t)g_H_Client, ViewSetup_PhaseName(g_phase));
	return true;
}

bool ViewSetup_InstallProbe() {
	if (g_probeInstalled) return true;
	if (!g_ViewVtable || g_installed) return false;
	// Build 6774: RTTI CViewRender slot 4 computes the view matrices, taking only this.
	// Verify its prologue as well as its class identity before installing this diagnostic.
	const unsigned char expected[] = {
		0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,
		0x41,0x56,0x48,0x81,0xec,0xb0,0x00,0x00,0x00,0x48,0x8b,0x01,0x4c,0x8b,0xf1,
		0xff,0x90,0x88,0x00,0x00,0x00
	};
	void* target = nullptr;
	__try {
		target = ((void**)g_ViewVtable)[4];
		if (!target || std::memcmp(target, expected, sizeof(expected))) return false;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	g_probeOriginal = (ViewProbeFn)target;
	g_probeInstallThread = GetCurrentThreadId();
	if (DetourTransactionBegin() != NO_ERROR) { g_probeOriginal = nullptr; return false; }
	if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
	    DetourAttach(&(PVOID&)g_probeOriginal, New_ViewProbe) != NO_ERROR) {
		DetourTransactionAbort();
		g_probeOriginal = nullptr;
		return false;
	}
	if (DetourTransactionCommit() != NO_ERROR) { g_probeOriginal = nullptr; return false; }
	g_probeInstalled = true;
	Filmmaker::MvmDebugLog_LinefAlways("deadlock.view.probe.install",
		"target=%p rva=0x%zx thread=%lu", target, (size_t)target - (size_t)g_H_Client, g_probeInstallThread);
	return true;
}

bool ViewSetup_Installed() { return g_installed; }

bool ViewSetup_Enabled() { return g_enabled; }

void ViewSetup_SetEnabled(bool enabled) {
	g_enabled = enabled;
	advancedfx::Message("AfxHookDeadlock: view hook body %s\n", enabled ? "on" : "off");
}

void ViewSetup_SetWriteEnabled(bool enabled) {
	g_writeEnabled = enabled;
	advancedfx::Message("AfxHookDeadlock: view write %s\n", enabled ? "on" : "off");
}

ViewPhase ViewSetup_GetPhase() { return g_phase; }
void ViewSetup_SetPhase(ViewPhase phase) { g_phase = phase; }

bool ViewSetup_PhaseFromName(const char* name, ViewPhase* out) {
	if (!name || !out) return false;
	if (0 == _stricmp(name, "before")) { *out = ViewPhase::Before; return true; }
	if (0 == _stricmp(name, "after")) { *out = ViewPhase::After; return true; }
	if (0 == _stricmp(name, "both")) { *out = ViewPhase::Both; return true; }
	return false;
}

const char* ViewSetup_PhaseName(ViewPhase phase) {
	switch (phase) {
	case ViewPhase::Before: return "before";
	case ViewPhase::After: return "after";
	case ViewPhase::Both: return "both";
	}
	return "?";
}

// Dumps a window of the accepted main-view object as raw dwords, read back three ways.
//
// This prints what is actually in the accepted Deadlock view object so field displacements can be
// re-measured after an update. A world position shows up as three plausible floats in a row, a
// field of view as one float between about 40 and 140, and the viewport as two matching integers.
void ViewSetup_Dump(size_t first, size_t last) {
	void* const base = g_lastMainBase;
	if (!base) {
		advancedfx::Warning(
			"AfxHookDeadlock: no main view seen yet, so there is nothing to dump.\n");
		return;
	}
	if (last <= first) last = first + 0x100;
	if (last - first > 0x1000) last = first + 0x1000;

	advancedfx::Message("AfxHookDeadlock: view object %p, +0x%zx..+0x%zx\n", base, first, last);

	for (size_t off = first & ~(size_t)3; off < last; off += 4) {
		unsigned int raw = 0;
		float asFloat = 0.0f;
		bool ok = false;
		__try {
			std::memcpy(&raw, (unsigned char*)base + off, sizeof(raw));
			std::memcpy(&asFloat, (unsigned char*)base + off, sizeof(asFloat));
			ok = true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			ok = false;
		}
		if (!ok) {
			advancedfx::Message("  +0x%03zx  <unreadable>\n", off);
			break;
		}
		advancedfx::Message("  +0x%03zx  0x%08x  f=%.4f  i=%d\n", off, raw, asFloat, (int)raw);
	}
}

ViewOffsets ViewSetup_GetOffsets() { return g_offsets; }
void ViewSetup_SetOffsets(const ViewOffsets& offsets) { g_offsets = offsets; }

unsigned long long ViewSetup_CallCount() { return g_callCount.load(); }
unsigned long long ViewSetup_WriteCount() { return g_writeCount.load(); }

void ViewSetup_SetPreviewViewport(bool active, float x0, float y0, float x1, float y1) {
	g_previewActive = active;
	if (!active) return;
	g_previewRect[0] = x0; g_previewRect[1] = y0;
	g_previewRect[2] = x1; g_previewRect[3] = y1;
}

bool ViewSetup_PreviewViewportTaking() { return g_previewTaking; }

void ViewSetup_SetPreviewViewportEnabled(bool enabled) {
	g_previewEnabled = enabled;
	// Re-enabling is also how a stand-down is retracted, so a corrected displacement can be tried
	// without relaunching.
	if (enabled) g_previewStoodDown = false;
}

bool ViewSetup_PreviewViewportEnabled() { return g_previewEnabled; }

void ViewSetup_SetViewportOriginOffsets(ptrdiff_t x, ptrdiff_t y) {
	g_viewportXOffset = x;
	g_viewportYOffset = y;
	g_previewStoodDown = false; // a new guess deserves a fresh attempt
}

void ViewSetup_GetViewportOriginOffsets(ptrdiff_t& x, ptrdiff_t& y) {
	x = g_viewportXOffset;
	y = g_viewportYOffset;
}

void ViewSetup_PreviewReport() {
	advancedfx::Message(
		"preview viewport: %s  active=%s  engine accepted=%s\n",
		g_previewEnabled ? (g_previewStoodDown ? "enabled but STOOD DOWN" : "enabled") : "off",
		g_previewActive ? "yes" : "no",
		g_previewTaking ? "yes" : "no");
	advancedfx::Message(
		"  offsets:  x=+0x%zx y=+0x%zx width=+0x%zx height=+0x%zx  (measured on build 6687)\n",
		(size_t)g_viewportXOffset, (size_t)g_viewportYOffset,
		(size_t)g_offsets.width, (size_t)g_offsets.height);
	advancedfx::Message(
		"  rect:     %.4f %.4f %.4f %.4f  of %dx%d      writes=%llu rejected=%llu\n",
		g_previewRect[0], g_previewRect[1], g_previewRect[2], g_previewRect[3],
		g_previewBaseW, g_previewBaseH,
		g_previewWrites.load(), g_previewRejects.load());
	if (g_previewStoodDown)
		advancedfx::Warning(
			"  A write did not read back, so the workspace is framing a CROP of the full-screen\n"
			"  render rather than the whole frame. Read 'mirv_filmmaker view dump 0x430 0x460',\n"
			"  find the two dwords holding the viewport origin, then\n"
			"  'mirv_filmmaker preview offsets <x> <y>'.\n");
}

void ViewSetup_Report() {
	advancedfx::Message("view probe: %s calls=%llu (read-only; no pump)\n",
	                    g_probeInstalled ? "hooked" : "off", g_probeCalls.load());
	advancedfx::Message(
		"view setup: %s  phase=%s  calls=%llu  writes=%llu\n",
		g_installed ? "hooked" : "NOT hooked", ViewSetup_PhaseName(g_phase),
		g_callCount.load(), g_writeCount.load());
	advancedfx::Message(
		"  offsets:  origin=+0x%zx angles=+0x%zx fov=+0x%zx width=+0x%zx height=+0x%zx"
		"\n",
		(size_t)g_offsets.origin, (size_t)g_offsets.angles, (size_t)g_offsets.fov,
		(size_t)g_offsets.width, (size_t)g_offsets.height);
	// Everything that can write the view, not just the camera owners: console camera overrides on
	// top of any owner. (Handheld shake used to be listed here too -- removed with the rest of the
	// HandheldCam bridge; see DeadlockViewSetup.cpp's ApplyOverride comment.)
	//
	// The owner itself comes from CameraRuntime_LastAppliedOwner()'s RECORDED value (what the view
	// hook actually applied last), not a fresh CameraRuntime_ResolveOwner() call here -- a console
	// report recomputing it independently can read `Frame_MainCount()` one tick off from when a
	// pushed pose (Follow's, Shot's, Graph's) was actually applied, and disagree with the real
	// write. `console` is derived from the SAME recorded value so the line never contradicts itself
	// (a name from one moment next to a flag computed from another).
	{
		const ViewOwner owner = CameraRuntime_LastAppliedOwner();
		const bool console = CameraCommands_Active();
		advancedfx::Message(
			"  claimed:  owner=%s  console override=%s%s\n",
			ViewOwnerName(owner), console ? "yes" : "no",
			(!ViewOwnerWrites(owner) && !console) ? "  (nothing writes -> writes stay flat)" : "");
	}
	advancedfx::Message(
		"  gate:     vtable=0x%zx caller=0x%zx  rejected: vtable=%llu caller=%llu sanity=%llu\n",
		g_ViewVtable ? (g_ViewVtable - (size_t)g_H_Client) : 0,
		g_ViewSetupCallerReturn ? (g_ViewSetupCallerReturn - (size_t)g_H_Client) : 0,
		g_rejectVtable.load(), g_rejectCaller.load(), g_rejectSanity.load());
	advancedfx::Message(
		"  last:     origin=(%.2f %.2f %.2f) angles=(%.2f %.2f %.2f) fov=%.2f viewport=%dx%d\n",
		g_lastOrigin[0], g_lastOrigin[1], g_lastOrigin[2],
		g_lastAngles[0], g_lastAngles[1], g_lastAngles[2],
		g_lastFov, g_lastWidth, g_lastHeight);

	// The two failure modes this is here to separate. Saying which one it is saves the whole
	// "is the camera broken or is the hook dead" round trip.
	if (g_callCount.load() == 0)
		advancedfx::Warning(
			"  calls=0: the detour never ran. Either it did not install, or 0x%zx is not the\n"
			"  per-frame view function on this build. Re-run the signature verifier first.\n",
			g_ClientViewSetupFn ? (g_ClientViewSetupFn - (size_t)g_H_Client) : 0);
	else if (g_lastWidth <= 0 || g_lastHeight <= 0)
		advancedfx::Warning(
			"  viewport reads as %dx%d, so the offsets are wrong or the base register is not\n"
			"  the view struct. Try 'mirv_filmmaker view offsets ...'.\n",
			g_lastWidth, g_lastHeight);
}

} // namespace Deadlock
