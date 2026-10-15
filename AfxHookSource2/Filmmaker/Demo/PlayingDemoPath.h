#pragma once

// Resolves the ABSOLUTE path of the demo the ENGINE is currently playing -- regardless of
// how playback started (our Downloaded-tab Watch button, CS2's native Your Matches tab via
// MatchInfoAPI.Watch, or a raw console "playdemo"). The camera-path (dolly) sidecar keys off
// this so the SAME .dem file always maps to the SAME markers no matter which UI launched it.
//
// Primary source is the engine's own getter, ISource2EngineToClient::GetDemoFilePath (vtable
// :043), adopted from HLAE upstream commit 4f25fb5. If that ever returns null/empty (or faults
// on an SDK build where the slot has shifted) we fall back to a guarded (SEH-snapshot) scan of
// the demo object's memory for a string ending in ".dem", so resolution never regresses. See
// PlayingDemoPath.cpp for both mechanisms and the "mirv_filmmaker demoprobe" diagnostic that
// prints the getter result alongside every scanned candidate.

#include <string>

namespace Filmmaker {

// Absolute, canonical path of the engine's currently-playing demo, or L"" when no demo is
// playing or no path could be recovered. Result is cached per demo-object instance, so this
// is cheap to call every frame.
std::wstring ResolvePlayingDemoPath();

// Canonicalize a path to the filesystem's true-cased absolute form (GetFinalPathNameByHandle,
// \\?\ prefix stripped) so two independently-derived strings for the SAME file compare equal
// and produce the same sidecar. Falls back to GetFullPathName when the file can't be opened.
std::wstring CanonicalDemoPath(const std::wstring& path);

// Diagnostic backing "mirv_filmmaker demoprobe": prints every .dem candidate found in the
// demo object (offset + raw + resolved-absolute + exists-on-disk) and the final resolved path.
void DebugProbePlayingDemoPath();

} // namespace Filmmaker
