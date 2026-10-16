# Live demo scrubbing

Dragging any timeline playhead moves the demo **world** with it, forward and backward. Forward, the demo plays through the drag. Backward, it restores the dragged tick about 12 times a second. Over a shot that has been played through, drags in either direction and clicks are shown from the [scrub frame cache](#scrub-frame-cache) at full frame rate. Every demo is played from a **keyframed copy** that carries a full snapshot every 32 ticks, and every 2 ticks around the camera-path markers ([dense keyframes](#dense-keyframes-around-the-shot)). A release lands exactly on the tick under the playhead.

| Piece | File |
| --- | --- |
| Policy (engine-free, unit-tested) | `AfxHookSource2/Filmmaker/Movie/DemoScrubMath.h`, tests in `Automation/tests/demo-scrub-math-tests.cpp` (`DemoScrubMathTests`) |
| Scrub frame cache policy: takes, what is kept, shown and dropped (engine-free) | `Movie/ScrubFrameCacheMath.h`, tests in `Automation/tests/scrub-frame-cache-math-tests.cpp` (`ScrubFrameCacheMathTests`) |
| Scrub frame cache, main thread (frame identity, takes, invalidation, `scrub cache`) | `Movie/ScrubFrameCache.cpp` |
| Scrub frame cache, render thread (the frames, capture and show) | `RenderSystemDX11Hooks.cpp`, `AfxViewportScaler::ScrubCache_*` |
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
- **Dragging backward:** restore 2 ticks short of the playhead, then play those 2 ticks in with one quick `demo_step_tick` at up to 4× (never more than one tick per frame, so a slow frame cannot carry it past the goal; see below), while the preview holds its last settled frame (see [The preview hold](#the-preview-hold)). Then show the result for 2 frames before the next restore. The play-in always runs, even when a fast drag has already moved the playhead further back. Without it, a raw restore reached the screen, and a restore runs no animation, so players stood in bind pose or were missing (measured at 40 ticks/s: 25 restores, 0 play-ins). The shown frames never move forward. Gliding into the playhead after each restore had made every backward step a saw-tooth: back about 10 ticks, then forward one (`seek 5812 target 5813`, `seek 5806 target 5807`, …). Turning back to forward resumes the glide, and the release still lands exactly.
- **The play-in on a slow frame:** `demo_step_tick` plays at the timescale and only stops on a frame boundary. With heavy Environment effects a frame takes about 60 ms, and at 4× that is 15 ticks, so a 2-tick step ended 6 ticks on (`step to 5812 from 5810 lost: tick=5816`, 2026-10-16). The old code then stepped forward again, and the world ran forward behind the panels while the playhead moved back. The step's timescale is now capped at one tick per frame (`ScrubBackStepTimescale`), and a play-in that still runs long counts as played in.

### The preview hold

During a seek the engine builds no scene frame for many presents, but it keeps presenting the UI over the last world it rendered, at full size. The before-UI callback that runs the preview's scale blit, and its fallback, is never queued for those presents, so nothing can scale them. They showed the full-size world through the preview window, which looks like a zoom: measured at about 45% of presents while dragging backward, up to 2 s at a time. With heavy Environment effects it is most of them: a paused demo presented 16 scene frames out of 16 a second, while a backward drag presented 24 a second with only 8 scene frames among them (2026-10-16). The full-size world also shows through the translucent panels around the preview. That cannot be undone after the fact, because by present time the UI is already drawn over it.

- The render thread keeps the preview region of the last correctly scaled, settled frame (`Before_Present`, `RenderSystemDX11Hooks.cpp`). Outside a hold, that is the newest scaled scene frame, so it is the same world the engine re-shows behind the panels.
- A present whose blit was missed gets that region copied back over it, for as long as the misses last. It is the newest correct picture there is.
- A frame shown from the [scrub frame cache](#scrub-frame-cache) replaces the held region and becomes the frame held next.
- A frame whose own identity says it has not been played into since its restore is never kept as the frame to fall back on. The hold flag reaches the render thread at once, but a serve request travels with the frame's scale blit, a frame later; dropping the hold when serving began let the present in between show the restore and keep it, a bind-pose player on the first served frame.
- While a restore and its play-in are in flight (`DemoScrub` raises `SetHoldPreview`), the frames that did blit are held too, so a raw restore never shows. That hold gives up after 0.5 s, so a seek that never settles cannot freeze the preview.
- Only the preview rect is held. The panels around it, including the playhead under the drag, stay live.
- `vpscale.present` lines report `held=N` beside `MISSED=N`. A covered miss is not counted as a `vpscale.break`.

- **Release:** land on the nearest whole tick. From the glide, that is a `demo_step_tick` to it. From behind, it is a restore 8 ticks short and a played-in step, so the final frame has been played into. One tick past the goal is accepted only after that settle. Then the user's own speed (`DemoTimescale_Get()`) comes back, and playback resumes if the demo was playing when the drag began.
- **The playhead after a release:** `TimelinePlayhead` holds the released tick while the world travels there, and afterwards while the demo tick stays where it landed, within two ticks. The demo tick still reads the old position mid-seek and can land one past the goal, and the playhead used to show both. Playback, a step, or a scrub toward another tick (CS2's own bar) hands the playhead back to the demo tick. `mvm_debug` logs `playhead.hold` hold, landed and drop lines.
- **Other seeks:** CS2's own bar seeks on click and release, and other tools seek too. A seek in flight is judged every frame (`JudgeSeek`). If the world moved outside the range between where the seek started and its target, or the demo plays on away from the target, or nothing moved for 20 frames on a keyframed copy (240 without keyframes), the seek is dropped. The controller then aims again from where the world is. A settle step that lands outside its range is dropped the same way.
- One command is in flight at a time, and the newest drag position always wins. `demo_pause` is a toggle, so a pause is only ever sent while the demo is playing.

**The fractional clock.** The world's demo time is `curtime × 64 − offset`. Paused on a whole tick, it reads exactly `tick + offset` (the demo's server start tick; 3173 in `ragdolls.dem`). Paused between ticks, it holds the fraction, and while playing it advances continuously. The offset is learned whenever the demo sits paused on a whole tick. MirvTime's `interpolation_amount` reads 0 in CS2, so it cannot be used. `mirv_filmmaker scrub` prints the raw clock.

The camera timeline sends its drag position with two decimals, so slow drags stay smooth instead of snapping tick by tick.

While a scrub is active, the playback loop stands down (`PlaybackLoop_RunFrame`). Otherwise the short forward bursts could trip its Out point and pull the world back to In. A render job owns the demo clock, so scrubbing is ignored while one runs.

## Scrub frame cache

Without it, every backward update is an engine restore. On the 2-tick dense grid each restore replays at most one tick (measured: max 1, from about 1,940 without a keyframed copy), but the restore itself costs about 50 to 100 ms, so a backward drag showed roughly 8 to 13 new frames a second against forward's full frame rate. No keyframe density changes that, because the floor is the restore.

The cache keeps the scaled preview of every frame the world plays, and shows a backward drag over those ticks from the GPU at frame rate, with no restore. It is switched by **Cache** in the camera timeline's transport row, left of the speed scrubber (`mirv_filmmaker scrub cache on|off`, saved as the UI key `camtl.scrubCache`, on by default). Off, nothing is kept or shown and scrubbing works exactly as without it.

**Takes.** The demo replays deterministically, but the client does not. Ragdolls, death animations and particles are simulated on the client and come out differently every time the world is restored and played again. So a cached frame is only true for the **take** it was played in: one stretch of real forward play since the last restore. The cache holds **the recorded take** of the shot:

- A take that began at or before the shot's first key (+1 for a seek landing one past) **records** the shot (`ScrubCache::Recording`). Playing from the first key (camera timeline play), or clicking the first key and dragging forward through the shot, records it and replaces every cached frame of the shot.
- Any other take, such as the one a click in the middle starts (a release restores 8 ticks short and plays in), never replaces a cached frame. It only fills ticks nothing is cached for. Restores drop nothing.
- So dragging back and forth and clicking anywhere in the shot all show the same take, and it changes only when the shot is played through again from its start.
- Only played frames are kept. A restore that has not been played 2 ticks into (no animation, bind-pose players) is never cached. Within one take, the frame nearest the whole tick stays.
- A take starts when a seek of the scrub lands, or when the whole demo tick goes back or jumps more than 24 ticks (anyone's seek). It is judged on the whole tick: until the scrub has learned the clock offset, the fraction of a playing demo wraps back at tick boundaries, which once started a take every ~15 ticks of plain playback.

**Showing it** (`ScrubServeFromCache`). A playhead over a cached tick is shown from the cache, with no restore, while the world stays where it is:

- dragging behind the world, always;
- dragging forward, unless the world is recording (then it glides, and its frames re-record the shot);
- on a **release** (a click, or the end of a drag): the cached frame is shown at once while the world still lands there for real underneath, because play, keys and every other edit need the real world. After it lands, the preview stays **parked** on the cached frame while the world sits on that tick (`ScrubFrameCache_Park`). Play, a new drag, the world moving off the tick, the camera moving, or anything that invalidates the frame hands the preview back to the live world. Pressing play from a parked frame plays a fresh simulation from that tick, so ragdolls can differ slightly from the parked frame.

A frame is shown for a playhead within a tick of it, or anywhere between two frames of the same take at most 8 ticks apart, so a heavy scene that plays 2 to 4 ticks a frame still covers a shot. Off the cached span the controller restores or glides as before.

**A frame is a picture of one tick.** The camera timeline and graph scrubs fly the camera path. While the world chases a drag with the cache on, the path is flown at the **world's** own time, not the playhead's (`ScrubFrameCache_CameraTick`), so the camera and the world in every frame agree, exactly as a render of that tick would. A forward drag's preview therefore trails the playhead by the world's glide (about 0.12 s), as the world always did. After a release the camera flies the released tick while the world settles there, and the graph keeps driving it until the world lands. The camera timeline and the graph stop flying the camera once the world lands, and a camera left at the travelling world's time ended a jump to the first key a few ticks short of it.

Each frame's identity (demo time, take and where it began, played or not, the picture revision, the camera pose shown and the path's pose) is published by the main thread after the camera drivers run (`ScrubFrameCache_EndFrame`), handed to the render thread with that frame's own scale blit, and checked against the camera the engine actually rendered with (`EngineThread_TakeScrubFrame`). A frame whose camera does not match is shown but never kept (`poseMismatch` in the log).

**Path playback is a take too.** Live playback flies a low-passed camera on its own predicted clock, which starts up to ~13 ticks behind the world while it learns the tick rate. Its frames are kept under the world's time with the camera playback showed, and validated against the path's own pose at that time (`pathPose`). Measured on the 71-tick shot: playback kept 63 frames, and the drag back from the last key had 336 hits and 6 misses, with 320 presents shown from the cache.

**The end of path playback** lands on the last key the way a release does (`DemoScrub_Release`: restore 8 ticks short, play in, pause), and parks on the recorded frame there. It used to `demo_gototick` the key, which on a keyframed copy restores that tick's own snapshot and left players in bind pose on the final frame.

**Storage.** One texture per frame, the size of the preview rect (about 5.2 MB at 1564×826 RGBA8), created on first use and copied on the GPU at `Before_Present`, where the preview hold copies. The budget is 384 MB (about 74 frames; `scrub cache budget <MB>`). When it is full it thins first: the frame whose two same-take neighbours sit closest together goes, while the gap it leaves can still be bridged, so a shot longer than the budget stays covered end to end (a 70-tick pass into 62 slots used to evict its first ticks, fall off the bottom on the way back, and drop the rest with the restore that followed). Only then is the frame farthest from the playhead evicted. The frames are the preview region as presented, including any UI drawn over it.

**Invalidation.** Anything other than time that changes the picture drops the cache:

- the camera path (`CameraPath::PathRevision`: every edit, and any rebuild after which the poses at the keys and segment middles differ; entering a drag rebuilds an unchanged path, and counting that dropped the cache on every new drag); a shown frame is also checked against the pose the path flies at its tick now;
- persistent settings, the Config and camera-feel values (`FilmmakerSettings_PictureRevision`, UI-only keys excepted);
- scene key edits (`SceneKeys_Revision`), scene object edits (`g_sceneUndoClock`), and the Environment scene, including its lighting (`Environment::SceneGeneration`);
- the window, backbuffer format or preview rect, which re-sizes the cache.

`mirv_filmmaker scrub cache` and `mirv_filmmaker health` print the frames, VRAM, hits, misses, stores, shown frames, evictions and drops. `mvm_debug` logs `scrub.cache` lines: `present` once a second (stored, served, evicted, pose mismatches), `frame` once a second (hits, misses, frames the camera drivers noted and how many of them could be kept), `take N starts at T`, `revision changed: dropped`, `parked on the cached frame at` and `unparked from`, `sized`, and `evict`. `demo.scrub` logs `serving from cache at` and `cache serve ends at`.

## CS2's demo bar

The native bar (`huddemocontroller.js`) only updates its label while you drag (`SliderValueChanged`). It calls `GotoTick` once on `SliderReleased`. The injected observer adds its own handlers to the same `#Slider`:

- Drag events are forwarded **only while `slider.mousedown`**. The native `FrameUpdate` writes `slider.value` every frame when the mouse is up, and those writes fire `SliderValueChanged` too.
- Release forwards `floor(value)`, but only after a forwarded drag. A plain click is left to the native seek. A scrub started mid-seek would read `IsDemoPaused` while it blips false, and it would resume a paused demo. The native `GotoTick` still runs, but it is a no-op or a cheap forward skip once the world is already there.
- A `mirvscrub` panel attribute stops a re-injection from registering the handlers twice.

## Diagnosing

`mvm_debug start`, scrub, then read the `demo.scrub` and `scrub.cache` lines in the log. They show `begin`, every `step`/`seek` with its source tick and target, `landing timed out` when the engine never paused on the expected tick, and `done` with the seek and step counts. The engine's own `Demo Skipping: skipping to demo tick N ... from full packet K` lines, logged as `console.engine`, show which snapshot a seek restored. On a keyframed copy, K is at most 32 ticks behind N.
