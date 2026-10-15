#pragma once

// Deadlock's Panorama access.
//
// The accessor functions the shared Panorama bridge actually calls
// (AfxHookSource2_GetPanorama*) are declared in AfxHookSource2/DeathMsg.h and defined in
// DeadlockPanorama.cpp; only the Deadlock-specific extras live here.

namespace Deadlock {
// Reports that workspace state travels in the RunScript payload. The CS2 CUIPanel attribute
// offsets are intentionally absent because they have no verified Deadlock equivalent here.
void Panorama_ReportCandidates();

} // namespace Deadlock
