# Live demo scrubbing

Dragging any timeline playhead moves the demo **world** with it, forward and backward. Forward, the demo plays through the drag. Backward, it restores the dragged tick about 12 times a second. Every demo is played from a **keyframed copy** that carries a full snapshot every 32 ticks. A release lands exactly on the tick under the playhead.

| Piece | File |
| --- | --- |
| Policy (engine-free, unit-tested) | `AfxHookSource2/Filmmaker/Movie/DemoScrubMath.h`, tests in `Automation/tests/demo-scrub-math-tests.cpp` (`DemoScrubMathTests`) |
| Driver (issues engine commands, main thread) | `AfxHookSource2/Filmmaker/Movie/DemoScrub.cpp` |
| Keyframed copies (build, cache, play) | `AfxHookSource2/Filmmaker/Demo/DemoKeyframes.cpp`, helper `FilmmakerDemoKeyframesGo/` |
| Camera timeline slider | `CameraPath.cpp` `ScrubToTick` (drag → `DemoScrub_Drag`, release → `DemoScrub_Release`) |
| Graph editor ruler | `GraphEditorExperimentHud.cpp` `CmdPlayhead`, `GraphEditorJs.h` release |
| CS2's own demo bar | `Panorama/DemoBarScrubJs.h`, injected by `DemoBarButtons.cpp` |
| Console | `mirv_filmmaker scrub drag <tick>`, `scrub release <tick>`, `scrub status`, `keyframes` |

## How CS2 seeks

Read from `engine2.dll` (Ghidra) and measured:

- **Backward** seeks restore a full snapshot and replay forward from it. `CDemoPlayer::SkipToTick` keeps an index of `(tick, file offset)` entries, one per `DEM_FullPacket`. Each entry is appended as playback reads past its packet. The function picks the newest entry at or before the target. `CDemoPlayer::FullPacketGoto` then seeks the file there, restores it, and replays up to the target. Without an entry, it reloads the whole demo.
- **Valve writes a full packet every 3840 ticks** (60 s), in every demo recorded here. A backward seek therefore replays up to a minute of demo: 0.5 s to several seconds.
- Restoring a snapshot costs about 45 ms (`ragdolls.dem`) to 65 to 80 ms (a 10-player match), plus about 2 ms per replayed tick. The replay, not the restore, is what makes backward seeks slow.
- `demo_gototick` **forward** is incremental. 10 ticks cost about 10 ms, and 410 ticks cost about 115 ms. To the current tick it is a no-op.
- A **forward** `demo_gototick` sent while the demo is **paused** never runs. The tick holds until playback resumes. The driver sends `demo_resume;demo_gototick N` and pauses once the demo is observed playing on N. `demo_pause` is a toggle, so it stays out of that string. Put in the same batch, it would run while the demo is still paused and leave playback running. Backward, a plain `demo_gototick` from a paused demo lands and holds. The driver pauses once if a seek lands playing.
- Long replays run in chunks of 2500 messages over many frames. Mid-replay, the tick reads the **old** value while paused. A seek has landed only when the tick equals its target (±1).
- The tick read right after a skip can be one past the engine's own "finished at tick N".
- A restore does not *simulate* anything. Animation layers, view interpolation and particles start over, so the restored frame differs slightly from one reached by playing. This holds for Valve's own full packets as much as for ours (camera up to ~2 units, weapon-hold layers). Playing 8 ticks into the target closes that gap. Measured: 8.6% to 1.1% of pixels differ, and 32 ticks gives 0.68%.
- `demo_step_tick N` plays N real ticks at the current `demo_timescale` and pauses on the tick.
- Seeking past the end of the demo ends playback and restarts it at tick 1.

## Keyframed copies

`FilmmakerDemoKeyframes.exe <in.dem> <out.dem>` writes a copy with a synthesized `DEM_FullPacket` after the `DEM_Packet` of every tick where `tick % 32 == 1`. That is the same grid Valve's own packets sit on. Each synthesized packet holds what Valve's would contain:

- `net_Tick` and `svc_UserCmds` copied from that tick's packet.
- A **non-delta `svc_PacketEntities`** that creates every live entity with all of its current fields. The decoder (a vendored copy of demoinfocs' `sendtablescs2`, `st2/`) keeps the **encoded bits** each field last arrived with. The writer re-emits those bits, so no value is re-quantized. Field paths are Huffman-coded with one op per path, because every op except Finish yields a path that takes a value.
- `serialized_entities`: one varint per entity, the bit length of its field section. The engine uses these to decode entities in parallel.
- `non_transmitted_entities` from the newest full packet.
- The string tables updated since the previous full packet. The engine restores each table from the newest full packet that carries it. An update entry without a value *clears* the entry's data.

**Verification.** Each run fails instead of writing a bad copy:

- Keyframes are decoded again and compared with the live state bit for bit (all of them, or every Nth with `-verify-every N`).
- The live state is compared with **each of Valve's full packets**. The entity list, classes and serials must match, and at most 1% of fields may differ. Some difference is legitimate. Valve's snapshot is the server's view, while a keyframe must reproduce what normal playback holds in the client. That includes stale weapon timing fields, per-round stats, and values the server changed back without sending a delta (an `engine2` comment describes that bug). Measured on a 118k-tick match: 0.34% of fields, all of those kinds.

**Measured:**

| Demo | Ticks | Size | Keyframes | Time | Backward seek |
| --- | --- | --- | --- | --- | --- |
| `ragdolls.dem` (cs_office) | 6.4k | 3.7 → 13.1 MB | 197 | 7 s | 42 to 47 ms |
| match, de_inferno | 116k | 230 → 563 MB | 3631 | 68 s | 65 to 80 ms, any distance |
| match, de_mirage (legacy protocol) | 118k | 300 → 522 MB | 3656 | 51 s | (client refuses the protocol) |

**In the product.** The first Watch of a demo plays the normal file (or its repaired copy) at once. In the background, at below-normal priority, it builds `keyframed.dem` in the demo's `source_mvm` project folder. Selecting a demo in the browser starts the build too. Every later play uses the copy. `keyframed.dem.src` stores the size and mtime of the original and of the helper, and the copy is rebuilt when either changes. The copy is not listed as a demo. Camera paths, markers and other sidecars stay keyed to the original.

At most **three** keyframed copies exist. Each is about 1.8× its demo's size. Starting a build for a demo without one first deletes the least recently used others. "Used" is the newer of the project's `project.mvm` (rewritten whenever the demo opens) and the copy itself. A copy that is open in another game instance stays until it is free. `repaired.dem` copies are never pruned.

Diagnostics: `KF_CENSUS=1` prints a per-command message census instead of writing. `KF_TRACE=<field name substring>` prints every write to matching fields.

## Scrub policy

A controller runs every frame and steers the world toward the playhead. The playhead is fractional, and so is the world. A paused demo can rest between two ticks. Play at a low `demo_timescale`, pause, and the whole world is interpolated to that fraction and stays there. Measured: 0.51 ticks of 0.02× playback left the camera 0.52 of the way between two ticks.

- **Ahead by up to 16 ticks** (0.25 s): keep the demo playing toward the playhead, at a speed proportional to the gap. It arrives in about 0.12 s and slows as it gets close, clamped to 0.02 to 16×. It pauses the frame it would reach the playhead, at whatever fraction of a tick that is. A slow drag glides. A burst of 90 captured frames during a 6-tick drag changed a little on every frame (median 0.06, largest 0.09 mean pixel difference), with no frozen-then-jump pattern.
- **Behind by more than a tick, or far ahead:** pause, restore the whole tick before the playhead, then glide in. On a keyframed copy, a restore is one ~80 ms frame, so a backward drag updates about 12 times a second. Less than a tick ahead of the playhead, the world holds, because a restore is not worth doing for a fraction of a tick.
- **Release:** land on the nearest whole tick. From the glide, that is a `demo_step_tick` to it. From behind, it is a restore 8 ticks short and a played-in step, so the final frame has been played into. One tick past the goal is accepted only after that settle. Then the user's own speed (`DemoTimescale_Get()`) comes back, and playback resumes if the demo was playing when the drag began.
- **Other seeks:** CS2's own bar seeks on click and release, and other tools seek too. A seek in flight is judged every frame (`JudgeSeek`). If the world moved outside the range between where the seek started and its target, or the demo plays on away from the target, or nothing moved for 20 frames on a keyframed copy (240 without keyframes), the seek is dropped. The controller then aims again from where the world is. A settle step that lands outside its range is dropped the same way.
- One command is in flight at a time, and the newest drag position always wins. `demo_pause` is a toggle, so a pause is only ever sent while the demo is playing.

**The fractional clock.** The world's demo time is `curtime × 64 − offset`. Paused on a whole tick, it reads exactly `tick + offset` (the demo's server start tick; 3173 in `ragdolls.dem`). Paused between ticks, it holds the fraction, and while playing it advances continuously. The offset is learned whenever the demo sits paused on a whole tick. MirvTime's `interpolation_amount` reads 0 in CS2, so it cannot be used. `mirv_filmmaker scrub` prints the raw clock.

The camera timeline sends its drag position with two decimals, so slow drags stay smooth instead of snapping tick by tick.

While a scrub is active, the playback loop stands down (`PlaybackLoop_RunFrame`). Otherwise the short forward bursts could trip its Out point and pull the world back to In. A render job owns the demo clock, so scrubbing is ignored while one runs.

## CS2's demo bar

The native bar (`huddemocontroller.js`) only updates its label while you drag (`SliderValueChanged`). It calls `GotoTick` once on `SliderReleased`. The injected observer adds its own handlers to the same `#Slider`:

- Drag events are forwarded **only while `slider.mousedown`**. The native `FrameUpdate` writes `slider.value` every frame when the mouse is up, and those writes fire `SliderValueChanged` too.
- Release forwards `floor(value)`, but only after a forwarded drag. A plain click is left to the native seek. A scrub started mid-seek would read `IsDemoPaused` while it blips false, and it would resume a paused demo. The native `GotoTick` still runs, but it is a no-op or a cheap forward skip once the world is already there.
- A `mirvscrub` panel attribute stops a re-injection from registering the handlers twice.

## Diagnosing

`mvm_debug start`, scrub, then read the `demo.scrub` lines in the log. They show `begin`, every `step`/`seek` with its source tick and target, `landing timed out` when the engine never paused on the expected tick, and `done` with the seek and step counts. The engine's own `Demo Skipping: skipping to demo tick N ... from full packet K` lines, logged as `console.engine`, show which snapshot a seek restored. On a keyframed copy, K is at most 32 ticks behind N.
