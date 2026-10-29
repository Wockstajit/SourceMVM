#pragma once

// Seeking back inside the running demo, without reloading it.
//
// CS:S's demo player seeks back by playing the demo again from the start: SkipToTick (demo_gototick,
// the demo bar, the live scrub) calls StartPlayback, which disconnects the client and signs on again.
// The loading screen and the game's menu flash by while the level loads again (about 0.6 s), and only
// then does the demo skip forward to the tick.
//
// The wrap seeks back in place, the way the engine goes on after that reload:
//   * cl_fullupdate's work: the client frees its entity baselines, which the demo's first snapshot was
//     encoded against while they were still empty;
//   * the demo file is read again from its first command after the sign-on data, a sync tick at tick 0
//     (every demo measured), and the skip waits for that sync tick, as SkipToTick's does after a reload;
//   * the first packet is a full snapshot, and on one the engine's CL_ProcessPacketEntities deletes every
//     networked entity and builds them again, so nothing from later in the demo stays behind.
// The engine skips at most 100 packets a frame. Every skip, forward too, may go on in its frame for up
// to kSkipBudgetMs instead, so a seek lands in the frame it was asked in and no frame of the skip shows.
//
// Measured in the 64-bit engine.dll on 2026-10-27 (demo_gototick's callback at engine.dll+0x75460 calls
// SkipToTick, slot 14, on the demo player at engine.dll+0x522e38); the offsets are checked against the
// engine's own code before anything is wrapped, and a mismatch leaves the engine's reload in place.

namespace SourceFilmmaker {

// Installs the wrap. Safe to call every frame; it retries until the commands exist.
void DemoRewind_Init();

// One line for mirv_filmmaker health.
void DemoRewind_PrintHealth();

// The wrap is in: a seek back lands in the frame it was asked in.
bool DemoRewind_Installed();

// How far past its tick a seek may land. The engine ends a skip on the first packet at or after the
// tick, and a SourceTV demo has one every 4 to 10 ticks (one a tick in a player's own demo): the
// largest gap between packets the skips of this demo crossed, nearly all of them (99%), less one.
// 8 before any skip of the demo.
int DemoRewind_SeekLandsPast();

} // namespace SourceFilmmaker
