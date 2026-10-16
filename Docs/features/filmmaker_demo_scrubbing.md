# Live demo scrubbing

Dragging any timeline playhead moves the demo **world** with it, forward and backward. Forward, the demo plays through the drag. Backward, it restores the dragged tick about 12 times a second. Every demo is played from a **keyframed copy** that carries a full snapshot every 32 ticks, and every 2 ticks around the camera-path markers ([dense keyframes](#dense-keyframes-around-the-shot)). A release lands exactly on the tick under the playhead.

| Piece | File |
| --- | --- |
| Policy (engine-free, unit-tested) | `AfxHookSource2/Filmmaker/Movie/DemoScrubMath.h`, tests in `Automation/tests/demo-scrub-math-tests.cpp` (`DemoScrubMathTests`) |
| Driver (issues engine commands, main thread) | `AfxHookSource2/Filmmaker/Movie/DemoScrub.cpp` |
| Keyframed copies (build, cache, play) | `AfxHookSource2/Filmmaker/Demo/DemoKeyframes.cpp`, helper `FilmmakerDemoKeyframesGo/` |
| Camera timeline slider | `CameraPath.cpp` `ScrubToTick` (drag → `DemoScrub_Drag`, release → `DemoScrub_Release`) |
| Graph editor ruler, key jumps and key drags (GRAPH and SCENE GRAPH) | `GraphEditorExperimentHud.cpp` `CmdPlayhead`, `GraphEditorJs.h` `playheadTo` / `releasePlayhead` |
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
- The live state is compared with **each of Valve's full packets**. The entity list, classes and serials must match, and at most 1% of fields may differ. Some difference is legitimate. Valve's snapshot is the server's view, while a keyframe must reproduce what normal playback holds in the client. That includes stale weapon timing fields, per-round stats, and values the server changed back without sending a delta (an `engine2` comment describes that bug).
- The known-legitimate kinds are listed but do not count toward the 1%: the scoreboard bookkeeping (`m_perRoundStats`, the `m_pActionTrackingServices` totals, `m_pDamageServices`) and stale `m_flAnimTime` / `m_flSimulationTime`. The per-round stats grow with every round played, so counting them failed long matches outright. A 10-player de_cache match failed at tick 65281 with 350 of 27805 fields (1.26%), every one of them of those kinds, and had no copy at all. With the split it builds: 25 of Valve's packets checked, 0 of 939979 other fields differ, 4783 scoreboard and stale-time fields listed.

**Measured:**

| Demo | Ticks | Size | Keyframes | Time | Backward seek |
| --- | --- | --- | --- | --- | --- |
| `ragdolls.dem` (cs_office) | 6.4k | 3.7 → 13.1 MB | 197 | 7 s | 42 to 47 ms |
| match, de_inferno | 116k | 230 → 563 MB | 3631 | 68 s | 65 to 80 ms, any distance |
| match, de_mirage (legacy protocol) | 118k | 300 → 522 MB | 3656 | 51 s | (client refuses the protocol) |

**In the product.** The first Watch of a demo plays the normal file (or its repaired copy) at once. In the background, at below-normal priority, it builds `keyframed.dem` in the demo's `source_mvm` project folder. Selecting a demo in the browser starts the build too. Every later play uses the copy. `keyframed.dem.src` stores the size and mtime of the original and of the helper, and the copy is rebuilt when either changes. The copy is not listed as a demo. Camera paths, markers and other sidecars stay keyed to the original.
When a build fails, the console warning and the crash-report history carry the helper's own last line
(`FAILED: <reason>`), read from `keyframed.dem.log` beside the copy. The log is deleted after each run.

**One copy.** Only the demo you have open keeps a keyframed copy. Opening a demo deletes every other demo's copy, along with its marker, log and unfinished build. That happens whether or not the opened demo needs a build. A copy that is open in another game instance stays until it is free. `repaired.dem` copies are never pruned.

## Dense keyframes around the shot

Scrubbing between cameras is a backward seek most of the time, and every backward seek is a restore followed by a replay ([How CS2 seeks](#how-cs2-seeks)). The replay is what puts effects back on screen in a burst. Every shot, impact, blood splash and grenade event between the restored snapshot and the target fires again within a frame or two, all at age zero. On the 32-tick grid, a release lands by restoring at most 31 ticks before `goal − 8` and playing in. Up to 39 ticks of events fire again on every jump between cameras. During a backward drag, each restore replays up to 31 ticks.

Denser keyframes shrink that window, but they cost about **45 KB each** (measured: 2941 keyframes added 132 MB to a 173 MB, 10-player match). A 2-tick grid over a whole 120k-tick match would add about 2.7 GB. So the copy gets a dense grid **only where the shot is**:

- **Windows.** They come from the demo's camera-path markers. Markers less than 8 s apart form one cluster, and a window covers each cluster plus 4 s of padding on both sides. Inside a window a keyframe sits on every 2nd tick. Everywhere else the 32-tick grid stays, still aligned with Valve's own packets.
- **Effect on seeks.** Inside a window, a release replays at most 9 ticks (the 8-tick play-in plus one), and a drag restore replays at most one. That is about a quarter of the replayed events on a release, and a 30th on a drag.
- **Size.** A 3 s shot gets an 11 s window, which is 352 keyframes, or about 16 MB. The windows are capped at 6144 dense ticks in total (about 140 MB). Beyond that the dense step widens to 4 ticks, then 8, until the windows fit.
- **Helper.** `-dense <a-b,c-d,...>` names the windows and `-dense-every N` sets their step. Without `-dense`, the copy is exactly the old 32-tick one.
- **Marker file.** `keyframed.dem.src` also records the windows a copy was built with: `<demo stamp>|<helper stamp>|<windows>`. A copy is reused while its stamps match, whatever its windows.
- **Rebuilding as keys are added.** When camera markers exist outside every window of the copy, a rebuild with the new windows starts in the background. It starts when the demo opens, and also during a session, 5 s after the last marker change, never during a render job. The engine holds the copy it is playing open, so a rebuild during playback writes the second slot, `keyframed-b.dem`. The next play of the demo uses whichever slot is newer and covers its markers, and deletes the other.
- **Switching onto a finished copy.** A copy that finishes while its demo is open is switched to automatically, at the first quiet moment: the demo paused, no render job, no scrub or path playback in flight. The switch reopens the demo on the copy, plays to the tick it was on, and pauses there. It is a level load of a few seconds, once per build. Without it, the session that built the copy kept scrubbing backward from Valve's snapshot a minute back, about 0.75 s per step on a 10-player match. The marker HUD shows a notice when a switch is queued, and `mvm_debug` logs `keyframes.switch` lines (switching, seeking, done, or why it gave up).
- **Mapping.** Both slot names count as project copies (`IsProjectDemoCopyFileName`), so the demo browser hides them, the scrub driver detects a keyframed copy by either name, and `project.mvm` maps both back to the original.

## Effects across a restore

What the engine does, and what can and cannot be kept:

- A restore destroys every live client particle. Entity-bound effects (smoke volumes, inferno flames, grenade trails) are rebuilt from the snapshot's entity state. Event effects (tracers, impacts, blood, muzzle flashes) are gone unless their event lies inside the replay window. Surface decals are not particles and survive. A restore cannot keep a particle that already exists. A backward move needs the particle as it was at an earlier age, and a particle cannot be simulated backward.
- What this design changes: fewer events replay (see above), so a jump between cameras no longer re-fires a burst of every effect from the last half-second.
- What it does not change: a restored frame still starts each effect at the age the replay gives it. A smoke that bloomed 3 s ago is rebuilt from its entity state, and an impact from 2 ticks ago replays at age zero.
- **The tick-perfect fix is a re-created effect at the right age:** a particle system re-created by a replayed event is simulated forward by `(target tick − event tick) × interval` before its first frame. The hook already sees every particle system the client creates (`ParticleFxHook.cpp`). What it still needs is engine-side: the replayed event's own tick at creation, and a way to advance a new particle collection by a given time (CS2's definitions carry a pre-simulation time). That needs reverse engineering in `particles.dll` and `client.dll`, and it is tracked as its own piece of work.

Diagnostics: `KF_CENSUS=1` prints a per-command message census instead of writing. `KF_TRACE=<field name substring>` prints every write to matching fields.

## Scrub policy

A controller runs every frame and steers the world toward the playhead. The playhead is fractional, and so is the world. A paused demo can rest between two ticks. Play at a low `demo_timescale`, pause, and the whole world is interpolated to that fraction and stays there. Measured: 0.51 ticks of 0.02× playback left the camera 0.52 of the way between two ticks.

- **Ahead by up to 16 ticks** (0.25 s): keep the demo playing toward the playhead, at a speed proportional to the gap. It arrives in about 0.12 s and slows as it gets close, clamped to 0.02 to 16×. It pauses the frame it would reach the playhead, at whatever fraction of a tick that is. A slow drag glides. A burst of 90 captured frames during a 6-tick drag changed a little on every frame (median 0.06, largest 0.09 mean pixel difference), with no frozen-then-jump pattern.
- **Behind by more than a tick, or far ahead:** pause, restore the whole tick before the playhead, then glide in. On a keyframed copy, a restore is one ~80 ms frame, so a backward drag updates about 12 times a second. Less than a tick ahead of the playhead, the world holds, because a restore is not worth doing for a fraction of a tick.
- **Dragging backward:** restore 2 ticks short of the playhead, then play those 2 ticks in with one quick `demo_step_tick` at 4×, while the preview holds its last settled frame (see [The preview hold](#the-preview-hold)). Then show the result for 2 frames before the next restore. The play-in always runs, even when a fast drag has already moved the playhead further back. Without it, a raw restore reached the screen, and a restore runs no animation, so players stood in bind pose or were missing (measured at 40 ticks/s: 25 restores, 0 play-ins). The shown frames never move forward. Gliding into the playhead after each restore had made every backward step a saw-tooth: back about 10 ticks, then forward one (`seek 5812 target 5813`, `seek 5806 target 5807`, …). Turning back to forward resumes the glide, and the release still lands exactly.

### The preview hold

During a seek the engine builds no scene frame for many presents, but it keeps presenting the UI over the last world it had. The before-UI callback that runs the preview's scale blit, and its fallback, is never queued for those presents, so nothing can scale them. They showed the full-size world through the preview window, which looks like a zoom: measured at about 45% of presents while dragging backward, up to 2 s at a time.

- The render thread keeps the preview region of the last correctly scaled, settled frame (`Before_Present`, `RenderSystemDX11Hooks.cpp`).
- A present whose blit was missed gets that region copied back over it, for as long as the misses last. It is the newest correct picture there is.
- While a restore and its play-in are in flight (`DemoScrub` raises `SetHoldPreview`), the frames that did blit are held too, so a raw restore never shows. That hold gives up after 0.5 s, so a seek that never settles cannot freeze the preview.
- Only the preview rect is held. The panels around it, including the playhead under the drag, stay live.
- `vpscale.present` lines report `held=N` beside `MISSED=N`. A covered miss is not counted as a `vpscale.break`.

### Why backward is still slower than forward

Forward, the world plays, so every rendered frame is new. Backward, every update is an engine restore. On the 2-tick dense grid each restore replays at most one tick (measured: max 1, from about 1,940 without a keyframed copy), but the restore itself costs about 50–80 ms. With the play-in and the settle frames, a backward drag shows roughly 8–13 new frames a second against forward's full frame rate. No keyframe density changes that, because the floor is the restore.

The way past it is a **scrub frame cache**: keep the scaled preview of each tick the world has been played through (keyed by tick and camera pose), and serve a backward drag over cached ticks from the cache, with a single real restore when the drag stops. Dragging back and forth over a shot, which is how a shot is tuned, would then match forward after the first pass. This is planned as its own change. It needs a frame identity carried from the engine thread to the present, a VRAM budget (about 5 MB per cached preview frame at 1564×826), and invalidation when anything other than time changes the picture.
- **Release:** land on the nearest whole tick. From the glide, that is a `demo_step_tick` to it. From behind, it is a restore 8 ticks short and a played-in step, so the final frame has been played into. One tick past the goal is accepted only after that settle. Then the user's own speed (`DemoTimescale_Get()`) comes back, and playback resumes if the demo was playing when the drag began.
- **The playhead after a release:** `TimelinePlayhead` holds the released tick while the world travels there, and afterwards while the demo tick stays where it landed, within two ticks. The demo tick still reads the old position mid-seek and can land one past the goal, and the playhead used to show both. Playback, a step, or a scrub toward another tick (CS2's own bar) hands the playhead back to the demo tick. `mvm_debug` logs `playhead.hold` hold, landed and drop lines.
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
