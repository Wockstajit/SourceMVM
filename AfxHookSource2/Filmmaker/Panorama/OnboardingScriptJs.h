#pragma once

// SCRIPT fragment of the onboarding panel (concatenated into kOnboardingJs, shares its scope):
// every lesson step. The routes that string them together are in OnboardingRoutesJs.h. A step can
// carry:
//
//   title, body, expr   what Clippy says, and his face ('curious', 'happy', 'focused', 'love')
//   ctx                 'menu' or 'hud': where the step happens. Off-context, the panel offers to
//                       load the practice demo or to skip the menu part (bridgeLine, Routes).
//   target              what to spotlight: a panel id, 'Parent>Child' (an id inside another),
//                       'tour:<name>' (a control registered in $.MvmTour), 'cfg:<card>:<part>'
//                       ($.ConfigHud.cardPanel), a function returning a panel, or a list.
//   enter()             runs once when the step starts in its context: seek the demo, open a card.
//   done(a)             a = the game state C++ sends (OnboardingHud.cpp). When it turns true the
//                       step moves on by itself. Already true when the step comes up (the user
//                       came back, or set it earlier), it only points: a Next button shows.
//   doneBody            what he says instead in that case.
//   act()               what the step asked the user to do, done for them: Skip runs it before
//                       moving on, so the next step starts where it expects.
//   needs               what has to be open for the step: { editor, config, render, renderTab,
//                       dock, insp, tlView, freecam, pov, camKeys, sel, graphSel, card, reveal,
//                       menu }. dock: 'native' or 'timeline'. tlView: the TIMELINE dock's 'keys'
//                       (KEYFRAMES) or 'graph'. graphSel: camera keys selected on the timeline.
//                       pov: first person through the practice demo's player.
//                       camKeys: at least that many camera keys (the first fight's two are placed
//                       when there are fewer). sel: a key selected. card: the CONFIG card the step
//                       points into. reveal: { panel, shows }, a card opened by its own button.
//                       menu: 'watch' or 'downloaded', the main-menu page. Needs are put in place
//                       once; after that the user may change them (that is often the step's job).
//                       Entering the step opens them, so a step never depends on what the user
//                       happened to leave open.
//   markers             points in the map to fly to, one at a time (see OnboardingLessonJs.h).
//   startPose           { pos, ang }: where the free camera starts, a short flight from them.
//   fitGraph            a graph step: the graph frames the keys once its needs are in place.
//   showKey             about one camera key: it is selected again on entry, so the demo and the
//                       view jump to it.
//   action              the main button's label. Without one the step waits for done().
//   options             buttons instead of the main button: { label, go: 'route step' | 'finale' }.
//   keys                the director hotkeys this step lets through (SPACE, LEFT, RIGHT, J, K, L, F,
//                       T, ESC, F8, DELETE, Z). Every other one does nothing while the step is up.
//   clickArea           a wider target spec whose area also takes clicks (the whole TIMELINE dock
//                       on its steps: the toolbar, FIT and the right-click menu).
//   click               'target' (default with a target: only the lit control and the bubble take
//                       clicks), 'none' (default without one: only the bubble), or 'free'.
//
// Every instruction has to be true of the real UI: labels, keys and ids were checked against the
// panels they name. Demo ticks are onboarding.dem's (Docs/features/filmmaker_onboarding.md).
//
// Plain ASCII on purpose. Non-ASCII punctuation is written as a JS \u escape.

R"OBJS(
    var OPENING = { title: "It looks like you're making a movie.", expr: 'curious',
      body: "Need help with that? I'm Clippy. I quit the paperclip business for a hinge and a career in film. Where should we start?" };
    var CHOICES = [
      { label: 'Teach me CS2 filmmaking', route: 'cs2' },
      { label: 'Teach me SOURCE:MVM', route: 'app' }
    ];

    // ---- onboarding.dem, Italy. Ticks are the demo's own (64 per second).
    var T_AK = 150, T_GLOCK = 600, T_KNIFE_WALK = 2740, T_SCENE_A = 300, T_SCENE_B = 1300;
    // Fight 1: he walks north up the alley into three bots. Both points sit on his own path, so
    // they are open air. Placed one at a time: Camera 1 at 5960, then the demo plays on to 6060 and
    // pauses for Camera 2, so the move has 100 ticks to happen. `ang` is the pitch / yaw a skipped
    // step places them with, looking up the alley toward the bots.
    var FIGHT1_CAMS = [
      { name: 'Camera 1', pos: [-430, -900, -30], ang: [14, 72], tick: 5960 },
      { name: 'Camera 2', pos: [-330, -470, -70], ang: [8, 60], tick: 6060 }
    ];

    // ---- helpers the steps share
    function placeFight1Keys() {
      for (var i = 0; i < FIGHT1_CAMS.length; i++) {
        var m = FIGHT1_CAMS[i];
        cmd('placekey ' + m.tick + ' ' + m.pos.join(' ') + ' ' + m.ang.join(' '));
      }
    }
    // A lesson that needs a camera path gets one: the two fight keys, when there are fewer than two.
    function ensureKeys() { if (((st.app || {}).keys || 0) < 2) placeFight1Keys(); }
    // Clicks a menu button the way the mouse would (its own Activated event).
    function activate(panel) {
      if (!panel) return;
      try { panel.checked = true; } catch (e) {}
      try { $.DispatchEvent('Activated', panel, 'mouse'); } catch (e) {}
    }
    // CAM EDITOR, CONFIG and the swap button sit on CS2's demo bar, which is only on screen while
    // the dock shows it (`dock: 'native'`), never under the Render workspace.
    function openEditorStep(title, body) {
      return { ctx: 'hud', title: title, expr: 'focused', target: ['CamEditorBtn', 'ModeSwapBtn'], body: body,
        needs: { dock: 'native' },
        done: function (a) { return a.editor; }, act: function () { raw('mirv_filmmaker editor on'); } };
    }
    function openConfigStep(title, body) {
      return { ctx: 'hud', title: title, expr: 'focused', target: ['ConfigBtn', 'ModeSwapBtn'], body: body,
        needs: { dock: 'native' },
        done: function (a) { return a.config; }, act: function () { raw('mirv_filmmaker config on'); } };
    }

    // ---- main menu
    var navClicked = null;
    var DEMOS_TAB = { ctx: 'menu', title: 'Your demos live here.', expr: 'focused', target: 'MainMenuNavBarWatch',
      body: "CS2 saves every match you play as a .dem file. SOURCE:MVM keeps yours under Movie Making. Click it. While I'm showing you around, only what I light up works.",
      enter: function () { navClicked = watchClicks('MainMenuNavBarWatch'); },
      act: function () { activate(menuPanel('MainMenuNavBarWatch')); },
      done: function () { return (navClicked && navClicked()) || downloadedOpen(); } };
    var DOWNLOADED_TAB = { ctx: 'menu', title: 'Now click DOWNLOADED.', expr: 'happy', target: downloadedTab, needs: { menu: 'watch' },
      body: "Your Matches is CS2's own list of recent games. DOWNLOADED is every demo on your PC, the practice one included.",
      done: function () { return downloadedOpen(); }, act: function () { activate(downloadedTab()); } };
    var ADD = { ctx: 'menu', title: 'Add your demos.', expr: 'curious', action: 'Next', target: 'FmOrgAddBtn', needs: { menu: 'downloaded' },
      body: 'Your CS2 folder is scanned already. Add pulls in a demo file or a whole folder from anywhere else, like Downloads.' };
    var SORT = { ctx: 'menu', title: 'Sort them.', expr: 'focused', action: 'Next', target: 'FmSortBtn', needs: { menu: 'downloaded' },
      body: 'Newest first, by map, or by name. Organizing demos is the closest most of us get to cleaning our room.' };
    var REPAIR = { ctx: 'menu', title: 'Broken demos get fixed here.', expr: 'curious', action: 'Next',
      target: ['FmRepairStatus', 'FmDownloadedBody>id-mi-watch'], needs: { menu: 'downloaded' },
      enter: function () { try { $.Filmmaker.selectFile('onboarding'); } catch (e) {} },
      body: "I picked the practice demo for you. When a CS2 update stops an old demo from playing, selecting it repairs a copy right here. Your original file is never touched." };
    var WATCH = { ctx: 'menu', title: 'Press WATCH.', expr: 'happy', target: 'FmDownloadedBody>id-mi-watch', needs: { menu: 'downloaded' },
      enter: function () { try { $.Filmmaker.selectFile('onboarding'); } catch (e) {} },
      body: "That's the practice demo: one short round on Italy. Click WATCH and I'll meet you inside.",
      anyCtx: true, done: function () { return st.ctx === 'hud'; }, act: function () { raw('playdemo onboarding'); } };

    // ---- the demo and the camera
    var EDITOR_OPEN = openEditorStep('Open CAM EDITOR.', 'Click CAM EDITOR. It opens the workspace where you build camera moves.');
    var PLACE_CAMS = { ctx: 'hud', title: 'Put the camera where the action is.', expr: 'focused', markers: FIGHT1_CAMS,
      keys: ['K', 'L'], click: 'free', needs: { freecam: true },
      startPose: { pos: [-482, -1062, 5], ang: [12, 72] }, // behind Camera 1, facing it
      enter: function () { seek(FIGHT1_CAMS[0].tick); freeCam(); },
      act: function () {
        for (var i = 0; i < run.marks.length; i++) {
          var m = run.marks[i];
          if (!m.done) cmd('placekey ' + m.tick + ' ' + m.pos.join(' ') + ' ' + m.ang.join(' '));
        }
      },
      body: "You're on the free camera now. Fly to Camera 1 with WASD, hold the mouse to look around, and press K. I'll roll the fight forward a bit, then you place Camera 2." };
    var DOCK_CAMERA = { fitGraph: true, ctx: 'hud', title: 'Your keys live down here.', expr: 'focused', target: 'tour:dock.timeline',
      needs: { camKeys: 2, editor: true, dock: 'native' },
      body: 'Click TIMELINE. The CAMERA PATH layer holds every key you placed, at the moment you placed it. Scene keys get layers right under it.',
      done: function (a) { return a.dock === 'timeline'; }, act: function () { raw('mirv_filmmaker dock timeline'); } };
    var PLAY_PATH = { fitGraph: true, ctx: 'hud', title: 'Roll it.', expr: 'happy', target: 'tour:tl.play', keys: ['J', 'SPACE'],
      needs: { camKeys: 2, editor: true, dock: 'timeline' },
      body: 'Press J, or play up here, and watch it to the end. The camera glides from key to key while the fight plays out. Click anywhere to get your mouse back.',
      done: function (a) { if (a.pathPlaying) run.sawPlay = true; return !!run.sawPlay && !a.pathPlaying; },
      act: function () { raw('mirv_filmmaker camtl play'); } };
)OBJS"
R"OBJS(
    // ---- one camera key up close
    var KEYS_READY = { ctx: 'hud', title: 'Two keys to work with.', expr: 'curious', action: 'Next',
      showKey: true, enter: function () { ensureKeys(); },
      body: "If you don't have camera keys yet, I just dropped two in at the first fight. Now let's make that move better." };
    var SELECT_KEY = { fitGraph: true, ctx: 'hud', title: 'Click the first key.', expr: 'focused', target: 'tour:tl.row.cam', clickArea: 'tour:tl.dock',
      needs: { camKeys: 2, editor: true, dock: 'timeline', tlView: 'keys' },
      body: 'Each diamond on the CAMERA PATH row is a key. Click the first one. Shift adds more.',
      done: function (a) { return (a.graphSel || 0) > 0; }, act: function () { raw('mirv_filmmaker grapheditor selall'); } };
    var PATH_TAB = { showKey: true, ctx: 'hud', title: 'PATH CAMERA.', expr: 'focused', target: 'tour:editor.tab.path',
      needs: { camKeys: 2, editor: true, dock: 'timeline' },
      body: "The selected key's settings are under PATH CAMERA. Click it if it isn't open.",
      done: function (a) { return a.insp === 'path'; },
      act: function () { try { $.CamEditor.gotoInspector('path'); } catch (e) {} } };
    var KEY_FOV = { showKey: true, ctx: 'hud', title: 'Zoom on one key.', expr: 'curious', target: 'tour:editor.fov',
      needs: { camKeys: 2, sel: true, editor: true, dock: 'timeline', insp: 'path' },
      body: 'Drag FOV to the left to zoom in on this key. The path zooms between keys, so the camera pushes in while it flies.',
      // Measured from the key that is selected once the step has shown it, so the selection itself
      // does not count as a change.
      done: function (a) {
        if (!(a.sel >= 0)) return false;
        if (run.fovSel !== a.sel) { run.fovSel = a.sel; run.fov0 = a.selFov; return false; }
        return Math.abs((a.selFov || 0) - (run.fov0 || 0)) > 0.5;
      },
      act: function () { var a = st.app || {}; if (a.sel >= 0) raw('mirv_filmmaker camtl setval ' + a.sel + ' 6 60'); } };
    var KEY_EASE = { fitGraph: true, showKey: true, ctx: 'hud', title: 'Ease in, ease out.', expr: 'happy', target: 'tour:tl.ease.inout', clickArea: 'tour:tl.dock',
      needs: { camKeys: 2, editor: true, dock: 'timeline', graphSel: true },
      enter: function () { run.eased0 = (st.app || {}).easedSides || 0; },
      body: 'With keys selected, In/Out eases them: the camera starts gently and settles at the next key instead of lurching. F9 does the same, and right-clicking a key opens the Keyframe Assistant.',
      done: function (a) { return (a.easedSides || 0) > (run.eased0 || 0); },
      act: function () { raw('mirv_filmmaker grapheditor ease inout'); } };
    var KEY_SMOOTH = { fitGraph: true, showKey: true, ctx: 'hud', title: 'Curves need three keys.', expr: 'focused', action: 'Next', target: 'tour:tl.ease.smooth', clickArea: 'tour:tl.dock',
      needs: { camKeys: 2, editor: true, dock: 'timeline' },
      body: "Smooth bends the path through your keys. With two it's still a straight line. Add a third key with K after the tour and you get a curve." };
    var KEY_REPOS = { showKey: true, ctx: 'hud', title: 'Move a key.', expr: 'curious', action: 'Next', target: 'tour:editor.repos', click: 'none',
      needs: { camKeys: 2, sel: true, editor: true, dock: 'timeline', insp: 'path' },
      body: 'Reposition picks the selected key up: you fly the free camera somewhere better and click to put it down. Save that one for after the tour.' };

    // ---- rendering the camera path
    var RENDER_STEPS = [
      { ctx: 'hud', title: 'Rendering films your path.', expr: 'focused', action: 'Next', enter: function () { ensureKeys(); },
        body: "Render films your camera path frame by frame into a video. If you had no keys yet, I just placed two at the first fight." },
      openEditorStep('Open CAM EDITOR.', 'Render lives inside the editor.'),
      { ctx: 'hud', title: 'Click RENDER.', expr: 'happy', target: 'tour:editor.render', needs: { camKeys: 2, editor: true },
        body: 'The render workspace opens on top of the editor.',
        done: function (a) { return a.render; }, act: function () { raw('mirv_filmmaker editor render'); } },
      { ctx: 'hud', title: 'RANGE & CAMERA first.', expr: 'focused', action: 'Next', target: 'tour:render.tab.range',
        needs: { camKeys: 2, render: true, renderTab: 0 },
        body: 'This tab decides which camera films the shot and for how long. Have a look, then hit Next.',
        act: function () { raw('mirv_filmmaker render tab 0'); } },
      { ctx: 'hud', title: 'Film your camera path.', expr: 'curious', target: 'tour:render.cam.path', needs: { camKeys: 2, render: true, renderTab: 0 },
        body: 'Current Viewer would film whatever is on screen. Click Camera Path: it films your keys, and the range snaps to your first and last key.',
        done: function (a) { return a.renderCam === 4; }, act: function () { raw('mirv_filmmaker render camera path'); } },
      { ctx: 'hud', title: 'The range came with it.', expr: 'happy', action: 'Next', target: 'tour:render.range', needs: { camKeys: 2, render: true, renderTab: 0 },
        body: 'Camera Path is lit up here too. The render starts on your first key and stops on the last, and the line underneath says how long that is.' },
      { ctx: 'hud', title: 'Now OUTPUT.', expr: 'focused', target: 'tour:render.tab.output', needs: { render: true },
        body: 'This is where the file goes and what kind of file it is.',
        done: function (a) { return a.renderTab === 3; }, act: function () { raw('mirv_filmmaker render tab 3'); } },
      { ctx: 'hud', title: 'Choose a folder.', expr: 'curious', target: 'tour:render.folder', needs: { render: true, renderTab: 3 },
        body: "Click Choose folder\u2026 and pick where renders should go. You only do this once, it's remembered. No folder, no render.",
        doneBody: "You already picked one, it's in the box up here. Choose folder\u2026 changes it. Otherwise hit Next.",
        done: function (a) { return a.renderFolder; } },
      { ctx: 'hud', title: 'MP4 is fine.', expr: 'happy', action: 'Next', target: 'tour:render.format', needs: { render: true, renderTab: 3 },
        body: 'MP4 plays anywhere. EXR, TGA and PNG are image sequences, for when you grade or edit in After Effects or Premiere.' },
      { ctx: 'hud', title: 'START RENDER.', expr: 'focused', target: 'tour:render.start', needs: { render: true },
        body: "It films frame by frame, so it's slower than real time. Hopefully you read the editing book first, or else Tiago will be mad.",
        done: function (a) { return a.renderJob || a.renderState >= 11; }, act: function () { raw('mirv_filmmaker render start'); } },
      { ctx: 'hud', title: 'Your take is cooking.', expr: 'love', action: 'Got it', target: 'tour:render.start',
        body: "Esc won't stop it, CANCEL RENDER will. It lands in your folder, in its own dated folder, as beauty\\everything.mp4. The JOB column shows the path when it's done." }
    ];
)OBJS"
R"OBJS(
    // ---- scene keys: the look over time
    var TOD_CARD = { panel: 'tour:scene.rail.tod', shows: 'tour:scene.tod.day' };
    // Open Graph is on the KEYS & HISTORY card, not on TIME OF DAY, which the steps before leave open.
    var KEYS_CARD = { panel: 'tour:scene.rail.keys', shows: 'tour:scene.opengraph' };
    var SCENE_STEPS = [
      openEditorStep('Open CAM EDITOR.', 'Scene keys live in the editor too.'),
      { ctx: 'hud', title: 'Open SCENE KEYS.', expr: 'focused', target: 'tour:editor.tab.scene', needs: { editor: true },
        body: 'Camera keys move the camera. Scene keys change the look over time: exposure, sun, sky, fog, weather.',
        done: function (a) { return a.insp === 'scene'; },
        act: function () { try { $.CamEditor.gotoInspector('scene'); } catch (e) {} } },
      { ctx: 'hud', title: 'Click TIME OF DAY.', expr: 'curious', target: 'tour:scene.rail.tod', needs: { editor: true, insp: 'scene' },
        body: "This card keys a whole look in one click. It's the fastest way to see what scene keys do.",
        done: function () { return guideVisible('tour:scene.tod.day'); },
        act: function () { activate(resolve('tour:scene.rail.tod', cssSize(), Date.now())); } },
      { ctx: 'hud', title: 'Key a daytime look.', expr: 'happy', target: 'tour:scene.tod.day', needs: { editor: true, insp: 'scene', reveal: TOD_CARD },
        enter: function () { seek(T_SCENE_A); run.sk0 = (st.app || {}).skKeys || 0; },
        body: 'I moved the demo to the start of the round. Click Day: it keys the sun, exposure, sky and fog right here.',
        done: function (a) { return (a.skKeys || 0) > (run.sk0 || 0); },
        act: function () { raw('mirv_filmmaker scenekeys macro day'); } },
      { ctx: 'hud', title: 'Now key the evening.', expr: 'curious', target: 'tour:scene.tod.dusk', needs: { editor: true, insp: 'scene', reveal: TOD_CARD },
        enter: function () { seek(T_SCENE_B); run.sk0 = (st.app || {}).skKeys || 0; },
        body: 'I jumped about fifteen seconds later. Click Dusk to key the evening look here.',
        done: function (a) { return (a.skKeys || 0) > (run.sk0 || 0); },
        act: function () { raw('mirv_filmmaker scenekeys macro dusk'); } },
      { ctx: 'hud', title: 'Watch it change.', expr: 'love', action: 'I see it', keys: ['SPACE'], needs: { editor: true },
        enter: function () { seek(T_SCENE_A); },
        body: 'Back at the start. Press Space and watch the light slide from day to dusk between your two keys.' },
      { ctx: 'hud', title: 'See it on the timeline.', expr: 'focused', target: 'tour:scene.opengraph', needs: { editor: true, insp: 'scene', reveal: KEYS_CARD },
        body: 'Open Timeline puts every property you keyed on its CONFIG card\'s layer, right under the camera path.',
        done: function (a) { return a.dock === 'timeline'; },
        act: function () { raw('mirv_filmmaker dock timeline'); } },
      { ctx: 'hud', title: 'Set one by hand.', fitGraph: true, expr: 'curious', target: ['tour:tl.row.s:exposure', 'tour:tl.rows'], clickArea: 'tour:tl.dock',
        needs: { editor: true, dock: 'timeline', tlView: 'keys' },
        enter: function () { run.sk0 = (st.app || {}).skKeys || 0; },
        body: 'Each row is one property. Drag its number left or right to change it at the playhead, and that drops a key. Try Exposure.',
        done: function (a) { return (a.skKeys || 0) > (run.sk0 || 0); } },
      { ctx: 'hud', title: 'Two keys make a change.', fitGraph: true, expr: 'happy', action: 'Got it', target: 'tour:tl.rows', clickArea: 'tour:tl.dock', keys: ['Z'],
        needs: { editor: true, dock: 'timeline', tlView: 'keys' },
        body: 'One key just holds a value; two keys make it change between them. Drag a diamond sideways to change when. Ctrl+Z undoes.' }
    ];

    // ---- the camera graph
    var GRAPH_STEPS = [
      openEditorStep('Open CAM EDITOR.', 'The graph draws the camera keys you place in the editor.'),
      KEYS_READY,
      { ctx: 'hud', title: 'Open TIMELINE.', expr: 'focused', target: 'tour:dock.timeline', needs: { camKeys: 2, editor: true, dock: 'native' },
        body: 'Camera keys and scene keys share one dock. Click TIMELINE.',
        done: function (a) { return a.dock === 'timeline'; }, act: function () { raw('mirv_filmmaker dock timeline'); } },
      { ctx: 'hud', title: 'Switch to GRAPH.', fitGraph: true, showKey: true, expr: 'focused', target: 'tour:tl.view.graph',
        needs: { camKeys: 2, editor: true, dock: 'timeline', tlView: 'keys' },
        body: 'KEYFRAMES shows when each key happens. GRAPH shows how the values move between them. Click GRAPH.',
        done: function (a) { return a.tlView === 'graph'; },
        act: function () { raw('mirv_filmmaker settings ui editor.timeline.view graph'); } },
      { ctx: 'hud', title: 'Pick what to draw.', fitGraph: true, showKey: true, expr: 'curious', action: 'Next', target: 'tour:tl.row.cam.rot', clickArea: 'tour:tl.dock',
        needs: { camKeys: 2, editor: true, dock: 'timeline', tlView: 'graph' },
        body: 'GRAPH draws the curves of the row you pick on the left. Click Rotation to see pitch, yaw and roll.' },
      { ctx: 'hud', title: 'Click a key.', fitGraph: true, showKey: true, expr: 'focused', target: 'tour:tl.graph', clickArea: 'tour:tl.dock',
        needs: { camKeys: 2, editor: true, dock: 'timeline', tlView: 'graph' },
        body: 'Click a key on a curve. Handles appear on each side: they set how the camera eases into and out of it.',
        done: function (a) { return (a.graphSel || 0) > 0; } },
      { ctx: 'hud', title: 'Drag it.', fitGraph: true, showKey: true, expr: 'curious', target: 'tour:tl.graph', clickArea: 'tour:tl.dock', keys: ['Z'],
        needs: { camKeys: 2, editor: true, dock: 'timeline', tlView: 'graph' },
        body: 'Up or down turns the camera more or less, sideways changes when. Hold Shift to change only the timing.',
        done: function (a) { return a.graphUndo; } },
      { ctx: 'hud', title: 'Read the slope.', fitGraph: true, showKey: true, expr: 'happy', action: 'Got it', target: 'tour:tl.graph', clickArea: 'tour:tl.dock', keys: ['Z', 'DELETE'],
        needs: { camKeys: 2, editor: true, dock: 'timeline', tlView: 'graph' },
        body: 'Steep means fast, flat means still. Right-click a key for the Keyframe Assistant: Easy Ease (F9) eases both sides, Easy Ease In the side arriving, Easy Ease Out the side leaving. Ctrl+Z undoes anything.' }
    ];
)OBJS"
