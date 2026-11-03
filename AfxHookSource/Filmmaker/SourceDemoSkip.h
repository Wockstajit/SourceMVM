#pragma once

// Ragdolls made while a demo skips (demo_gototick, and every seek of the demo bar).
//
// CS:S's client drops a ragdoll that is created while the demo is skipping: the CCSRagdoll class's
// create function stores engine->IsSkippingPlayback(), and C_CSRagdoll::OnDataChanged releases the
// entity on its first update when that was true and a demo is playing. The ragdoll is a networked
// entity, so the engine still holds it in its last snapshot, and the next packet that keeps it
// stops the engine with "Host_Error: CL_PreserveExistingEntity: missing client entity N" (one that
// changes it: CL_CopyExistingEntity). Plain demo_gototick does it, without the hook. Measured in the
// 64-bit client.dll on 2026-10-27: the create function at client.dll+0x1edad0 is the only caller of
// the engine's IsSkippingPlayback, and OnDataChanged at client.dll+0x1ec060 does the release.
//
// The hook wraps the create function of the profile's ragdoll class, and while it runs, on that
// thread, the engine client's IsSkippingPlayback answers false. The ragdoll is then kept, as it is
// in normal playback, and the engine's entities and the client's stay the same.

namespace SourceFilmmaker {

// Installs both wraps. Safe to call every frame; it retries until the client classes are there.
void DemoSkip_Init();

// One line for mirv_filmmaker health.
void DemoSkip_PrintHealth();

} // namespace SourceFilmmaker
