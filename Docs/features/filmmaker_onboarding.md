# Onboarding tutorial (Clippy prototype)

Clippy is a film clapperboard who walks a new user through SOURCE:MVM inside CS2. He dims the
screen, lights up the control to use next, follows the user from the main menu into the practice
demo, jumps the demo to the moment a lesson needs, and moves on by himself when the user has done
the thing. He is a prototype: nothing opens him automatically.

## Open it

Type `mvm_onboarding` in the CS2 console, at the main menu or during a demo.

| Command | Effect |
| --- | --- |
| `mvm_onboarding` | Open the tutorial, or resume it where you left it |
| `mvm_onboarding restart` | Back to the opening choices |
| `mvm_onboarding close` | Ask whether to leave: Keep going, Stop here, or Start over. The bubble's close box does the same |
| `mvm_onboarding resume` | Answer "Keep going": back to the same step, with what it needs opened again |
| `mvm_onboarding off` | Hide everything |
| `mvm_onboarding chatter on\|off` | Chatter sound while he talks |
| `mvm_onboarding go <route> <step>` | Jump to a lesson step. One past the last step is the route's ending |
| `mvm_onboarding pos reset` | Put him back in his own spot after you dragged him |
| `mvm_onboarding status` | Print the current state |

`mirv_filmmaker onboarding ...` takes the same arguments; the panel's buttons use that form.
Routes: `cs2`, `app` (the two opening paths), `more` (the deeper-rundown menu), and
the topics `render`, `keys`, `graph`, `path`, `recoil`, `fov`, `config`. The panel also sends
`seek <tick>`, `playto <tick>`, `placekey <tick> <x> <y> <z> <pitch> <yaw>`, `lock ...`,
`clicks free|only <x0 y0 x1 y1>...` and `pos x y` itself.

## A clean slate that saves nothing

The tutorial runs as a practice session (`Onboarding/PracticeSession.cpp`). It starts the moment
the tutorial is switched on (`mvm_onboarding`, `go`, `restart`) and ends when it is switched off
(**Stop here**, **Explore on my own**, `mvm_onboarding off`). The close question keeps it running.

When it starts, the user's pending edits are written first (settings, scene keys, environment).
Then everything goes to its defaults in memory:

- every demo opens with no camera keys, shakes, loop or scene keys, the practice demo included;
- the CAM EDITOR layout and every stored UI key (inspector tab, graph mode, dock, CONFIG pages);
- CONFIG's camera feel, effects, ragdolls, environment, props and light choices, through the
  same reset lists the tab footers run;
- the render workspace (camera, range, passes, format). The render output folder is kept, so a
  take lands where the user's renders always do.

While it runs, no store writes to disk. `PracticeSession_Active()` (`Config/PracticeSessionFlag.h`,
header-only so the test builds can include it) is checked by the per-demo project store (a read
finds nothing, a write is reported done without writing), the settings file, `filmmaker_fx.json`,
the environment autosave (and its restore on map load is skipped), props, relight light choices
and the recently-played list. A crash or quit in the middle leaves the user's files as they were.

When it ends, everything is read back: the settings and effects files (defaults first, then the
file over them), the playing demo's keys and scene keys, the map's saved environment (restored the
way a level load does it, before anything is observed for saving) and its props, and the render
workspace as the user left it. Session-only state that was never saved anywhere (the lens, the
follow camera) stays at its defaults.

## The lessons

The opening offers two paths. Each gives the gist, then asks whether the user wants a deeper
rundown, which opens a menu of short topics.

| Path | Covers |
| --- | --- |
| Teach me CS2 filmmaking (`cs2`) | Movie Making, then the DOWNLOADED tab, repair, WATCH, then Weapon Recoil (switch on, T test shot, the pivot as the hinge, AK vs Glock) and Weapon Bob |
| Teach me SOURCE:MVM (`app`) | Movie Making, the DOWNLOADED tab, Add, Sort, repair, WATCH, the two workspaces, swapping to CONFIG mid-edit and back, placing two cameras, the CAMERA dock, selecting a key and easing it, playing the path, then the whole render (below) |
| Deeper rundown (`more`) | A menu of topics, each one a walk-through that takes you to the right place and moment |

| Topic | Walks through |
| --- | --- |
| Rendering (`render`) | Keys placed for you if you have none, RENDER, RANGE & CAMERA, the Camera Path camera tile (the range follows), OUTPUT, Choose folder, format, START RENDER, where the take lands (`<folder>\<project>_<date>\beauty\everything.mp4`) |
| Scene keys (`keys`) | SCENE KEYS, the TIME OF DAY card, Day keyed at the start of the round, Dusk fifteen seconds later, Space to watch the light change, Open Graph, setting one property by hand |
| The graph (`graph`) | GRAPH (camera, not scene), solo Yaw, select a key, drag it (an edit is what moves the lesson on), ease and undo |
| Camera keys (`path`) | The CAMERA dock, selecting a key, PATH CAMERA, FOV on one key, Ease In/Out, why Smooth needs three keys, Reposition, playing it |
| Recoil and weapon bob (`recoil`) | Weapon Recoil on, T test shot, the pivot drawer as the hinge, AK vs Glock, Weapon Bob on the knife walk |
| Field of view (`fov`) | The VIEWMODEL card's FOV and Weapon FOV, then per-key FOV in the editor |
| CONFIG while you edit (`config`) | Swapping to CONFIG and back without losing the keys |

Skip does the step: each interactive step has an `act()` (open the editor, click the tab, switch
the effect on, place the keys, open Render, start it) that Skip runs before moving on, so the next
step starts where it expects.

A step moves on by itself only when the user does its job while the step is up. A step that is
already done when it comes up in its own context only points (WATCH, whose job is the switch to
the demo, still moves on when the panel is rebuilt there): the user came back with Back, set it earlier (a render
folder chosen last week), or the step's `needs` opened the thing it shows. It gets a Next button,
and `doneBody`, when the step has one, replaces the instruction. RANGE & CAMERA always waits for
Next: the tab is open as soon as Render is.

The camera step asks for CS2's roaming free camera (`spec_mode 4`, the `freecam` need, asked again
until it sticks, because the seek lands in whatever camera the demo had) so WASD and hold-click
look work. Once the seek has landed it moves that camera a short flight behind Camera 1, facing it
(`startPose`, sent as CS2's `spec_goto`; a MirvInput pose override does not move the roaming
camera). It shows the markers one at a time: after Camera 1 is placed the demo plays from 5960 to
6060 (`playto`) and pauses for Camera 2. Every later step that uses the path needs two camera keys
(`camKeys`) and places the first fight's two when there are fewer, so Skip, Back and a resumed
tutorial still have a path to work with.

While a camera path plays (J), a click anywhere gives the mouse back to the UI instead of
capturing it for free look (`MovieMode::OnUiClick`), and W, A, S or D stops the path and flies the
free camera from where it was. That holds outside the tutorial too.

Footer: **Back** goes one step back (from a path's first step to the opening; from a topic's
first step to the topic menu). **Skip this step** moves on. **Explore on my own** on the opening
and the endings hides him completely. The close box asks first, in the bubble: **Keep going**
returns to the same step, **Stop here** hides him and keeps the place for the next
`mvm_onboarding`, **Start over** goes back to the opening choices. There is no Restart button in
the footer.

The practice demo is `onboarding.dem` in the CS2 `game/csgo` folder, played with
`playdemo onboarding`. Lessons jump to its moments by tick (64 per second): the AK spray at 150,
the Glock at 600, the knife walk at 2740, and the first fight at 5960, where the camera markers
sit on the player's own path so they are open air.

The tutorial runs on a paused demo. When the panel is built in a demo (the demo loaded under the
tutorial, or the tutorial opened mid-demo) and after every lesson seek, the driver pauses it once
the demo has settled: playing steadily for 300 ms, no seek in flight, nothing moved for 400 ms. `demo_pause` toggles,
and measured runs showed a seek finishing paused and the engine's own start of playback resuming
it, so a reading of "paused" only counts after it has held for 1.5 s. Seeks go through
`mvm_onboarding seek <tick>`, which uses `DemoScrub_Release(tick, false)` to land on the tick. The
panel driver holds a seek until the demo is running (past tick 8, or 4 s): sent while the demo was
still loading, the seek landed and the engine's own start of playback rolled it on (5960 became
5977).
A step that needs motion says "press Space".

On top of that, a locked step that allows neither Space nor J holds the demo paused: if anything
starts it (CS2's own play button, playback picking up after a load), the driver pauses it again
after a quarter second, at most once a second. Seeks, a playing camera path, a render and a lesson
roll (`playto`, which resumes and pauses on its target tick) are left alone.

## Linear mode

While a lesson step is up only what it asks for works.

- **Keys.** Each step names the director hotkeys it lets through (`keys` in the script); the panel
  sends `onboarding lock on <keys>` and `MovieMode::OnKey` swallows the others: Space, Left/Right,
  J, K, L, F, T, Esc, F8, Delete and Z. Movement and mouse look are never blocked. The camera step
  allows K and L, the recoil steps T and Space, playing the path J and Space.
- **Clicks.** With a target, only the lit control and the bubble respond. Without one, only the
  bubble does. The camera and scene-key steps are `click: 'free'`, because they need the world and
  the property list. A step that only points at something for later is `click: 'none'`: Reposition
  needs a click in the world to put the key down, which the tour would block. The lines say so: the
  first step of each path tells the user that only what is lit works, and no line asks for a click
  or key its step does not allow. The rule is enforced twice. The panel's dims and a transparent blocker take
  the clicks inside Panorama, but some workspace panels took clicks through layers drawn above
  them (a dimmed CUSTOMIZE PLAYER opened), so the panel also sends the allowed zones (Clippy's
  bubble, Clippy, the lit control, in client fractions) as `onboarding clicks only ...`. The window
  procedure checks every press against them before CS2 or Panorama sees it
  (`Onboarding_BlocksClick`, `OnboardingGateClick` in `WindowInputHooks.cpp`) and drops a press
  outside them together with its release. While a camera path plays every click gets through,
  because that click is how the user gets the mouse back and the director swallows it.
- A press on Clippy (bubble or clapper) belongs to Panorama alone on every step: the panel sends his
  rectangles as `onboarding asst ...` and the window procedure keeps such a press from the
  preview's click-to-steer, the free cam's click swallow and CS2. A Next button over the preview
  used to grab the camera instead.
- He never lets a click fall through him on a locked step, even while faded, and he keeps clear
  of START RENDER and RENDER, so a click meant for the bubble cannot start a render.
- The opening, the endings, the bridges and the close question are unlocked. `off` clears the
  lock and the click zones.

## How a step works

Each step in `OnboardingScriptJs.h` can name:

- **ctx**, `menu` or `hud`. A step shown in the other context is replaced by a bridge: in the menu
  it offers to load the practice demo; in a demo it offers to skip the menu-only part.
- **target**, what to spotlight. A panel id, `Parent>Child` for an id inside another panel,
  `tour:<name>` for a control registered in `$.MvmTour`, or `cfg:<card>:<part>` for a Config card
  part from `$.ConfigHud.cardPanel()`. Everything outside it is dimmed and a gold ring pulses
  around it. Nothing in the overlay takes clicks, so the real control works through the hole. The
  bubble moves next to the target unless the user has dragged it.
- **enter()**, run once when the step starts: seek the demo (`demo_resume` first, because a forward
  `demo_gototick` is ignored while paused), open a Config card, select the practice demo.
- **needs**, what the step is taught in: `editor`, `config`, `render`, `renderTab`, `dock`, `insp`,
  `graphMode`, `freecam`, `pov` (first person through the practice demo's player, for the weapon
  lessons: `onboarding pov` sets it the way the demo bar does, dropping our free cam), `camKeys`,
  `sel` (a key selected), `card` (the CONFIG card it points into), `reveal` (a card opened by its
  own rail button: TIME OF DAY for Day and Dusk, KEYS & HISTORY for Open Graph), `menu` (`watch` or
  `downloaded`). A step about one camera key (`showKey`) selects it again once its needs are in
  place, which jumps the demo and the view to that key whatever camera was up.
  A graph step (`fitGraph`) frames the keys with the graph's own FIT once the demo is paused:
  while the demo plays, the graph pages its window to the playhead, and right after a load that
  left the keys off screen and the canvas empty. Graph steps also let clicks into the whole dock
  (`clickArea: 'tour:graph.dock'`), so FIT, the toolbar and the right-click ease menu work.
  Entering the step puts the game there, whatever the user had open (retried every 600 ms for up to
  8 s, because the editor takes a frame to exist). Once they are in place the step stops enforcing
  them, because changing them is often the step's job (clicking RENDER). A step whose needs do not
  include Render closes the Render workspace. CAM EDITOR, CONFIG and the swap button are on CS2's
  demo bar, so the steps that point at them need `dock: 'native'`: with the CAMERA or GRAPH dock up
  the bar is not on screen.
- **Never trapped.** A step whose lit control is still missing 3 s after its needs settled lets
  every click through and logs `[onboarding] step <route> <n>: its control is not on screen`.
  Skip always does the step.
- **done(app)**, checked every frame against the game state C++ sends (in the menu: whether a
  button was clicked, through an extra listener on its own `Activated` event, or whether the
  Downloaded list is on screen): editor or Config open, dock
  tab, inspector tab, camera-key count, path playing, scene-key revision, recoil and bob on.
  When it turns true, Clippy hops and the lesson moves on.
- **markers**, points in the map. Each is drawn as a ring with its name and distance, pinned to the
  edge of the visible world when off screen, and turns gold with "Press K" within 56 units of the
  camera. A key dropped there ticks it off; the step moves on when all are placed. With CAM EDITOR
  or CONFIG open the world is drawn shrunk into the workspace's preview box, so markers are
  projected into that box (`WorkspacePreviewRef().Active`), not onto the full screen.

Most editor controls are created without an id, so the scripts that own the ones a lesson points
at register them by name in `$.MvmTour`: the dock tabs (`dock.native`, `dock.camera`,
`dock.graph`), the inspector tabs (`editor.tab.path|follow|scene`), `editor.addkey`, `editor.fov`,
`editor.render`, `camtl.play`, `camtl.addkey`, `camtl.keys`, `scene.keyall`, `scene.opengraph` and
`graph.canvas`. Config cards answer `$.ConfigHud.cardPanel(key, part)` with `header`, `rail`,
`test` (Weapon Recoil's Test Shot) or `pivot` (the hinge drawer). The demo bar buttons already have
ids: `CamEditorBtn`, `ConfigBtn` and `ModeSwapBtn`. A docked demo bar is drawn scaled by
`$.MirvBarFitK`, which the spotlight applies to anything inside `ControlRow`.

## Look and feel

- Text types out quickly with short pauses after punctuation. The clapper top opens and closes
  on the syllables, and the optional chatter plays CS2's `UIPanorama.rename_teletype` UI sound
  on the same beats. Chatter starts on; the bubble's Chatter toggle or `mvm_onboarding chatter off`
  turns it off. Clicking the text shows the whole line, or replays a finished one.
- The bubble is anchored at its bottom edge and reserves the full line's height, so the buttons
  stay put while a line types.
- With nothing spotlit, resting the cursor just outside him for half a second moves him to the
  other spot on the left, at most once every two seconds. Holding the mouse on anything else fades
  him to 20% until about a second after release. During a demo he also fades while the camera is
  flown with no cursor.
- Drag the bubble's header row (the strip with the grip, not Chatter or the close box) to move
  him anywhere. The spot is kept as screen fractions in C++, so it survives the switch from the
  menu to a demo. While he is placed by hand he stays put.
- At the end of a path he falls over with heart eyes. Poking him three times quickly does the same.
- Panorama does not anti-alias the edges of rotated boxes, so the stripes and the hearts are
  drawn with gradients on unrotated squares instead. Every gradient is a plain `from()` to `to()`:
  a first draft with about 28 `color-stop()`s crashed CS2 in `panorama.dll` the first frame it drew.
- He hides during a render capture that hides UI, and stays up when the editor hides the game HUD
  (`MvmOnboardingRoot` is in that list in `CameraTimelineJs.h`).

## Where it lives

| Part | File |
| --- | --- |
| Tutorial state and the console command | `AfxHookSource2/Filmmaker/Onboarding/Onboarding.cpp` |
| Practice session (defaults in, nothing saved, read back) | `AfxHookSource2/Filmmaker/Onboarding/PracticeSession.cpp`, `AfxHookSource2/Filmmaker/Config/PracticeSessionFlag.h` |
| Panel driver (context, cursor, game state, rendered camera) | `AfxHookSource2/Filmmaker/Panorama/OnboardingHud.cpp` |
| Bubble, buttons, placement, speech timing | `AfxHookSource2/Filmmaker/Panorama/OnboardingJs.h` |
| The clapperboard rig and its animation | `AfxHookSource2/Filmmaker/Panorama/OnboardingClippyJs.h` |
| Every lesson | `AfxHookSource2/Filmmaker/Panorama/OnboardingScriptJs.h` |
| Spotlight and world markers | `AfxHookSource2/Filmmaker/Panorama/OnboardingGuideJs.h` |
| Step runtime | `AfxHookSource2/Filmmaker/Panorama/OnboardingLessonJs.h` |

The state lives in C++ so it survives the switch from the main menu to a loaded demo; the panel
is rebuilt in the new context and says its current line again. Like the easter-egg overlay, the
panel follows whichever Panorama context is up. It costs nothing until it is first opened.

The browser mockup shows it as **Onboarding (Clippy)** under the easter-egg actions, in both the
in-game and main-menu contexts (see the [mockup-first workflow](../panorama/mockup-first-workflow.md)).
The mockup has no rendered camera, so world markers only draw in game.

## Debugging

`mvm_debug start` logs every state change under `[onboarding]`, the practice session's start and
end under `[practice]`, a skipped environment restore under `[environment.autosave]`, the context
switches and panel builds, and the first chatter sound the panel plays (or why it failed). A failing step `enter()`
logs `[onboarding] step enter: ...` to the console. `perf.ui` lines report its per-frame cost as
`onboarding=`.
