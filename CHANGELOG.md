# Changelog

What changed in each SOURCE:MVM release, newest first. The in-game What's new popup reads this file,
so every release heading keeps the form `## <version> - <date>` (a hotfix is `## 0.3.1 Hotfix 1 - <date>`)
and every change is one `- ` bullet under a `### ` group. Releases older than 0.2.0 are on the
[releases page](https://github.com/Wockstajit/SourceMVM/releases).

## Unreleased

### Version and changes

- A WHAT'S NEW button in the top bar, left of INVENTORY, shows which version you have, on the main
  menu and in the pause menu. After an update, CS2's news popup shows what changed, once. The
  button opens it again.

## 0.3.1 Hotfix 1 - 2026-10-21

### Fixed

- Tooltips and dropdown lists in CONFIG draw on top of the demo bar instead of behind it.
- Skins stay on when a loop jumps back to its start, and a render has them from its first frame.
- Skin picks are saved with each demo and come back when you open that demo again, even after
  restarting CS2.
- Clicking the wear bar in Customize sets the wear where you click instead of jumping to Factory New.
- A skin change, a picked-up gun or a newly drawn skinned gun pauses the demo for a much shorter
  moment, about a tenth of a second for a drawn gun.
- With Keep between demos on, Environment keeps its sky, lighting, fog and weather settings for the
  next demo. Those effects still start off, and switching one back on brings its settings back.
  With Keep between demos off, opening a demo no longer overwrites the map's saved Environment
  settings.

## 0.3.1 - 2026-10-20

### Updates are now up to you

- The launcher no longer checks for updates on its own. Turn checks on under ADVANCED > UPDATES if
  you want them; the UPDATES button still checks once whenever you press it.
- When a check finds a new version, the launcher asks first. UPDATE downloads, installs and
  restarts; NOT NOW keeps the version you have, and the button reads DOWNLOAD UPDATE until you want
  it.
- GO works without internet. If the update server cannot be reached, the launcher says so once and
  starts CS2 with what is installed. Steam still has to be open and signed in.

### Cleaner sidebar in Config and the Camera Editor

- Every control is one row in the same box, switches are square, and a card is one page with group
  headings.
- Nothing is cut off any more. Option rows that do not fit beside their label drop under it, and
  the rows refit when you drag the inspector wider or narrower.
- The Config rail and the camera-key rail scroll when the window is short, so every entry stays
  reachable at 480p and 720p.
- The outliner has its own icons for eye, kind, render, lock, folder and chevron.
- The Nervous System switch in Death Animations only arms when Death Animations is on.
- The custom colour mark in the colour picker follows the inspector width.

### Lens flare stays off the green screen

- On a Players only or Gun only green screen frame, live preview or render job, the lens flare now
  sits the frame out, so the chroma background and the subject's edges stay clean for keying. Depth
  of field keeps working, and the flare comes back on every other frame.

## 0.3.0 - 2026-10-20

### Round heat map

- CONFIG > ENVIRONMENT > Scene Objects has a new Round heat map switch. It paints the floor red
  wherever players walked during the round you're watching, around the camera, so you can see where
  a placed object won't get in anyone's way.
- It covers the whole round, read from the demo file, so it's complete even at the start of a round.
- Each strip is as wide as a player, and floor nobody stood on stays untouched. Brighter red means
  more people went there. Upper floors keep their own paths.
- Area sets how far around the camera it paints. Players picks everyone, T or CT. Recordings never
  show it.
- The status line shows the match round, counted the way the scoreboard counts it. During warmup it
  says so.

### One TIMELINE dock

- A single TIMELINE dock replaces the separate camera timeline, camera graph and Scene Graph. The
  camera path and every keyed Config card sit side by side as layers, with KEYFRAMES and GRAPH
  views, a ruler that shows the render range, an overview strip, FIT and zoom, eases, SNAP, and undo
  and redo.
- Key icons show their ease. Smooth is a disc. Easy Ease, or a handle you dragged in GRAPH, is an
  hourglass.
- Camera shake has its own keys. Open Camera Shake to see Intensity and Speed rows, select and drag
  the keys, and the shake ramps smoothly from one key to the next.
- The shake menu is grouped by style, with Stronger, Weaker, Faster and Slower steps that keep the
  menu open.
- The frame Cache switch is in the transport, next to the speed control.

### Scene objects you can attach and key

- Cameras, lights and meshes can ride a player at a body point, a held or dropped weapon, a thrown
  grenade or another object, and you can key them on the TIMELINE dock.
- You can look through a camera object, give it its own camera shake, and jump to it with Go to
  camera.
- Every scene camera and light has its own layer with all its settings. Right-click > Add places a
  new one, and Delete removes it.
- Scene cameras, lights, camera paths and the follow camera are solid shapes in one set of colours
  instead of wireframes, with a dot where the camera is at the playhead.
- Space plays the demo and J plays the camera path. AUTO on the camera layer follows the path while
  the timeline plays.

### Clippy tutorial and practice mode

- Clippy walks you through the basics inside CS2's own menus. He dims the screen, lights up the
  control to use next, takes you into a practice demo, and moves on when you do each step. Start it
  with `mvm_onboarding`.
- The tutorial runs as a practice session that saves nothing. Your settings, demo projects and
  scenes are left exactly as they were.

### Scrubbing and the camera path

- Clicking a tick on the demo bar or the timeline now lands on it, still paused if you were paused.
  It used to play on, pause, jump back and play forward again, and sometimes stopped short of where
  you clicked.
- Dragging back over a shot you've already played shows it from a frame cache at full frame rate.
- Each demo gets a fast-seek copy with extra keyframes around your camera keys, so backward drags
  keep up.
- The camera graph and the path stay in step. The graph's curves drive the camera, the path line
  follows them, and markers no longer get merged together.
- Leaving camera path playback always works, and your field of view comes back afterwards.
- Scene keys stay with the original demo when the fast-seek copy is playing.
- The demo no longer reloads itself every time a new fast-seek copy finishes.
- A rebuild of the fast-seek copy no longer wipes your camera path, and a camera key with a broken
  angle no longer freezes CS2.
- At the end of a demo, playback holds about a second after the final round and stays there.

### Depth of field and lens flare

- Smoke is blurred at its own distance, so the blurred background no longer shows through it, and
  autofocus can focus on a smoke.
- New depth of field controls: Sharp band, Focus speed for Auto, bent-in aperture blades, a soft
  centre-weighted disc, and a Zebra view that shows what's sharp.
- The first-person gun no longer gets a dark ragged halo with depth of field on.
- The lens flare now fires on the sun, and has a Sun control.

### Lighting and Environment

- Lighting is faster. In a heavy molotov fight on de_ancient the typical frame went from about 26 to
  30 FPS, and the stutters of 20 ms or more are gone.
- Each light stops where it can no longer change the picture, and lamp shadows cost less, which
  helps busy views.
- A placed light at night no longer turns the floor into a brown, banded crust.
- On the ENVIRONMENT tab, left click selects and moves objects and never turns the camera. Hold right
  click in the view to look around.
- The scroll wheel scrolls the panel under the pointer instead of switching the camera.
- The bright square under a placed lamp with Dynamic shadows on is gone.
- After you close Config, clicks in the game view work again.
- Painting with several models no longer hitches. A brush can mix up to 16 models and go up to 400%
  density.

### Other fixes

- Money on headshot works on heads without a helmet too.
- Weapon recoil catches every shot of the player you're watching, and look sway no longer stutters
  in fast turns.
- Cameras riding a gun no longer pick up Smooth View's breathing and sway.
- The Gun only render pass no longer has green patches where the gun is as bright as the background.
- The follow camera's target box disappears 5 seconds after you pick a target.
- Clicks and look-around keep working after a click during camera path playback. A click there also
  gives you the mouse back, and W, A, S or D stops the path.
- Graph value drags move a steady amount per pixel, clicking a layer's name selects all its keys,
  and the wheel scrolls the open developer console.
- Turning on an Environment scene key switches the Environment on.

## 0.2.1 - 2026-10-16

### Cameras and mouse

- The scroll wheel now cycles CS2's own demo cameras. Click and drag on the game to look around, and
  click a panel to use the UI. The G key and the MOUSE button are gone.
- Shift or Ctrl with the wheel, and holding X or Z, still change fly speed, FOV and roll, even while
  you drag.
- The overview map is hidden.
- Camera keyframe diamonds can be clicked on every tab, and they're easier to hit.

### Scene Keys and the Scene Graph

- Every property shows up in the Scene Graph list. A colour property used to hide everything listed
  after it.
- Clicking a key moves the playhead to it. Dragging a key takes the playhead with it, so you see the
  lighting at that key while you move it.
- Changing a value right after jumping to a key changes that key. Before, it could add a second key
  one tick away.
- Curves fill the graph and stay on screen. The new FIT button frames all your keys, and a key you
  add or jump to off screen scrolls into view.
- Colour properties open the same colour picker as Config.
- Names match Config. The procedural sun is "Sun azimuth" and "Sun elevation" now, and the groups
  follow Config's tabs and cards.

### Lighting

- Lights placed with Scene Objects work again under Environment lighting.
- Placed CS2 props cast shadows again.

### Startup and stability

- Startup and map loading are faster. Lighting setup for CS2's materials now runs in the background.
  It used to run on CS2's own loading threads, where it could add 20 to 50 seconds.
- Effects stay off at the main menu. They turn on once a demo or practice map loads.
- Fixed the game being forced fullscreen and hanging for a while at launch on some PCs.
- A normal quit after a slow start is no longer reported as a crash.

### Launcher

- Upgrading over an older install no longer fails with "Failed to uninstall old application files:
  2". Some bundled files had paths too long for the old uninstaller.
- If Windows blocks CS2 from starting, or CS2 closes right after GO, the launcher now says so and
  links its log. Before, it reported a successful launch.

## 0.2.0 - 2026-10-15

### Open source

- SOURCE:MVM is open source now. The full source code is in this repository.

### CS2 update

- Works with the CS2 update from October 14, which changed the demo controls.

### Demo bar

- The speed control now sits in the middle of the bar. CS2's own speed slider is hidden so there
  aren't two.
- -5s / +5s now skip 5 seconds, and so do the Left and Right arrow keys.
- CS2's new player picker, next-kill button and camera-mode button stay on the bar.
- Our buttons are smaller and LOOP is now an icon. It turns green while a loop is on.
- The bar stays readable over bright scenes. Before, opening and closing Config or the Camera Editor
  could leave it see-through.
- Time, tick and the player name are smaller to make room for the speed control.
- In the Camera Editor, Config and Render, the bar fits with a gap at the edge. The player list and
  round menu open on top of the editor, at the right size, and the loop markers stay visible.

### Rendering

- Render jobs start again. Some jobs failed with "the recorder failed to start".
- The Render panel shows one button that switches between START RENDER and CANCEL RENDER.
- mirv_streams: a stream now puts back every setting it changes when it finishes, so a matte pass no
  longer turns decals or overlays back on, and grouped streams no longer undo each other's settings.
- Fixed a possible crash in the mirv_streams scene picker while recording.

### Lighting and Environment

- New auto exposure: the image adjusts its brightness as you move between dark and bright areas,
  like your eyes do. Turn it on in Environment > Lighting > Exposure, where you can also set its
  speed and compensation.
- Player bodies no longer flicker or shadow themselves from the sun, and their shadow edges no
  longer crawl.
- Less speckle on bumpy stone in sunlight.
- Players near the camera keep their sun shadow in first person.
- Fog now sits behind the sniper scope. Before, it was drawn over the scope.
- Fog on its own no longer runs the full shadow system, so it costs less.

### Fixes

- Fixed a crash when quitting the game.
- Fixed a crash when Discord, Steam or another overlay was running.
- Fixed a crash at startup.
- Space and the other director keys no longer do anything while the console is open.
- In a demo, Escape closes Render, the Camera Editor and Config without opening CS2's pause menu.
- Skins on older weapon models now show on third-person world models.
- The window border can't be dragged by accident while the RenoDX add-on is loaded.
- The launcher finds NVIDIA graphics cards more reliably for the DLSS option. Some RTX cards were
  reported as "No NVIDIA GPU was found".
