#pragma once

// The workspace shell: the framed preview, the header, the right inspector column, and the bottom
// dock with its ribbon.
//
// THE TWO AXES. Getting these confused is what produced a panel with the timeline, the camera
// editor and the graph all stacked down the right-hand side, which is wrong twice over -- they are
// views of TIME, and time is drawn along the bottom under the picture it belongs to.
//
//   * WHICH WORKSPACE is open -- Camera / Config / Render. Chooses what the RIGHT column shows.
//     The column is an INSPECTOR: the properties of whatever is currently selected.
//   * WHICH EDITOR IS DOCKED -- Regular Timeline / Camera / Graph. Chooses what sits under the
//     preview. Owned by DeadlockDock.h, switched by the ribbon, and independent of the workspace.
//
//     +------------------------------------------+------------------+
//     |  HEADER                                  |  CAMERA CONFIG   |
//     +------------------------------------------+  RENDER      X   |
//     |  letterbox                               +------------------+
//     |      +--------------------------+        |                  |
//     |      |   the game, framed       |        |  inspector       |
//     |      +--------------------------+        |  cards           |
//     |  letterbox                               |                  |
//     +------------------------------------------+------------------+
//     |  [REGULAR TIMELINE] [CAMERA] [GRAPH]           meta      (*) |
//     +-------------------------------------------------------------+
//     |  the docked editor: Deadlock's own bar, our timeline, or the |
//     |  graph                                                       |
//     +-------------------------------------------------------------+
//
// WHERE THE NUMBERS COME FROM. Not from here. This fragment measures the screen and reports it to
// C++ when it changes; C++ runs the tested solver in DeadlockWorkspaceLayoutMath.h and hands back
// `st.layout`, which is also the rect the engine renders the world into. One owner for the geometry
// means the frame drawn around the preview and the viewport the world lands in cannot disagree -- a
// two-pixel disagreement shows as a seam of un-shrunk world along one edge, and chasing that from
// two copies of the arithmetic is a day nobody gets back.
//
// PANORAMA RULES THIS FRAGMENT LIVES BY. Every *Js.h shares one JavaScript scope, so everything
// here hangs off one namespaced object and there is not a single loose `var` at file scope.
// Clearing a colour with the empty string THROWS and takes the rest of the panel with it, so
// M.CLEAR is the only way to say "no colour". The in-game HUD context gets no mouse-move event at
// all, so anything draggable is a native Slider.

// SPLIT ACROSS TWO ADJACENT RAW LITERALS. MSVC caps one string literal at 16380 bytes and
// SILENTLY TRUNCATES past it -- a failure that ships as "the panel stops halfway down" rather
// than as a build error. Adjacent literals concatenate, so the body closes and reopens once in
// the middle; the seam has to sit on a single line, because a newline outside a literal would
// The Panorama build test extracts every raw block in this file, in order, and joins
// them, so it sees exactly what RunScript receives.
#define DEADLOCK_WORKSPACE_JS R"CFJS(
(function () {
  if ($.MvmWorkspace) return;
  var M = $.MvmStyle;
  if (!M) return;
  var S = M.S;

  var ctx = null;
  var root = null;
  var header = null;
  var column = null, body = null;
  var grip = null;             // the column's drag edge (DeadlockInspectorGripJs.h)
  var gripSendArmed = false, gripPendingW = 0;
  var dockBody = null;

  var chrome = null;           // every absolutely-placed decoration, built once
  var lastWorkspace = '';
  var lastScreenW = 0, lastScreenH = 0;
  var lastDockKey = '';
  // The state this was last rendered with, kept for the probes. RunScript has no return value and
  // the state is a call ARGUMENT, so without this a live capture of "what the game is showing" needs
  // the render wrapped from outside -- which Automation/livejs/deadlock-stash-state.js still does
  // for builds that predate this line.
  var lastState = null;


  function cmd(text) { M.cmd(text); }

  // Absolute placement. Panorama wants a three-component position and an explicit size, and the
  // panel must be top-left anchored or the position is measured from somewhere else entirely.
  function place(panel, x, y, w, h) {
    if (!panel) return;
    M.set(panel, {
      position: Math.floor(x) + 'px ' + Math.floor(y) + 'px 0px',
      width: Math.floor(Math.max(0, w)) + 'px',
      height: Math.floor(Math.max(0, h)) + 'px',
      horizontalAlign: 'left',
      verticalAlign: 'top'
    });
  }

  // Change-gated, like every style write (see M.set). A redundant visible write moves the panel
  // through Panorama's draw list for nothing, and this runs over the whole chrome every frame.
  function show(panel, visible) {
    if (!panel) return;
    var v = !!visible;
    if (panel.__mvmShown === v) return;
    try { panel.visible = v; panel.__mvmShown = v; } catch (e) {}
  }

  function hit(panel, on) {
    if (!panel) return;
    var v = !!on;
    if (panel.__mvmHit === v) return;
    try { panel.hittest = v; panel.__mvmHit = v; } catch (e) {}
  }

  // ---- chrome: letterbox masks, the accent frame, the tag, the click catchers ----------------
  //
  // The catchers RING the preview rather than covering it. A click on empty chrome is swallowed,
  // but a click INSIDE the preview belongs to the game, which is how Deadlock's own spectator
  // controls keep working while the editor is open.
  function buildChrome() {
    function panel(id, props) {
      var p = M.make(root, 'Panel', id, props || {});
      if (p) { p.hittest = false; p.visible = false; }
      return p;
    }
    function mask(id) {
      // Deadlock's grounds are warm and very dark rather than black; a pure-black letterbox reads
      // as a rendering fault next to them.
      return panel(id, { backgroundColor: S.offBlack, horizontalAlign: 'left', verticalAlign: 'top' });
    }
    function edge(id) {
      return panel(id, { backgroundColor: S.gold, horizontalAlign: 'left', verticalAlign: 'top' });
    }
    function catcher(id) {
      var c = M.make(root, 'Panel', id, { horizontalAlign: 'left', verticalAlign: 'top' });
      if (c) {
        c.visible = false;
        c.hittest = false;
        try { c.SetPanelEvent('onactivate', function () { /* swallow */ }); } catch (e) {}
      }
      return c;
    }

    chrome = {
      maskLeft: mask('MvmMaskL'), maskRight: mask('MvmMaskR'),
      maskTop: mask('MvmMaskT'), maskBottom: mask('MvmMaskB'),
      // The two rules that close the bands, and the lower band itself. `frameTop` is the rule under
      // the header band; `frameBottom` is the rule above the dock band.
      frameTop: edge('MvmFrameT'), frameBottom: edge('MvmFrameB'),
      bandBottom: panel('MvmBandB', {
        backgroundColor: 'rgba(16,19,13,0.9)', horizontalAlign: 'left', verticalAlign: 'top'
      }),
      catchLeft: catcher('MvmCatchL'), catchRight: catcher('MvmCatchR'),
      catchTop: catcher('MvmCatchT'), catchBottom: catcher('MvmCatchB')
    };

    chrome.tag = M.make(root, 'Panel', 'MvmPreviewTag', {
      flowChildren: 'right',
      height: '22px',
      backgroundColor: 'rgba(16,19,13,0.72)',
      borderRadius: '3px',
      padding: '0px 10px 0px 8px',
      horizontalAlign: 'left',
      verticalAlign: 'top'
    });
    if (chrome.tag) {
      chrome.tag.hittest = false;
      chrome.tag.visible = false;
      chrome.dot = M.make(chrome.tag, 'Panel', '', {
        width: '7px', height: '7px', borderRadius: '4px',
        backgroundColor: S.warn, verticalAlign: 'middle', marginRight: '7px'
      });
      chrome.tagLabel = M.label(chrome.tag, '', 'PREVIEW', {
        fontSize: '11px', color: S.text, letterSpacing: '2px', verticalAlign: 'middle'
      });
    }
  }

  function hideChrome() {
    if (!chrome) return;
    for (var k in chrome) show(chrome[k], false);
  }

  var WORKSPACE_TAG = {
    camera: { text: 'CAMERA EDITOR', accent: 'gold' },
    config: { text: 'CONFIG', accent: 'blue' },
    render: { text: 'RENDER', accent: 'soul' }
  };

  function layoutChrome(g, cursorOn, live, workspace) {
    if (!chrome) return;

    var px = g.previewX, py = g.previewY, pw = g.previewW, ph = g.previewH;
    var right = px + pw, bottom = py + ph;

    // Masks fill the band around the preview, gated on real clearance so a zero-height strip is
    // not left behind as a one-pixel line of the wrong colour.
    place(chrome.maskLeft, g.areaX, g.areaY, px - g.areaX, g.areaH);
    show(chrome.maskLeft, (px - g.areaX) > 1);
    place(chrome.maskRight, right, g.areaY, (g.areaX + g.areaW) - right, g.areaH);
    show(chrome.maskRight, ((g.areaX + g.areaW) - right) > 1);
    place(chrome.maskTop, px, g.areaY, pw, py - g.areaY);
    show(chrome.maskTop, (py - g.areaY) > 1);
    place(chrome.maskBottom, px, bottom, pw, (g.areaY + g.areaH) - bottom);
    show(chrome.maskBottom, ((g.areaY + g.areaH) - bottom) > 1);

    // THE TWO RULES. Gold while the editor's own camera drives the preview, green while it is
    // showing the game's live view, so which one you are looking at is never a guess.
    //
    // THEY COME OUT OF THE SOLVER. They used to be placed at literal 44/46px offsets from areaY
    // and from screenH - dockH, neither of which the solver reserved -- and the dockH published
    // here was not even the one it used. On a 1920x1080 native tab that put the top rule at y=88
    // while the picture started at 168, and the bottom rule plus its 44px band across live world
    // that ran on to 955. Now the solver reserves the band and hands back where all three go, so
    // the frame cannot be anywhere but around the render.
    // THE RULES TRACE THE PICTURE. Not the band, not an offset from the dock.
    //
    // They used to span the band at fixed offsets, on the reasoning that a rule which stays put as
    // the aspect changes reads as a frame. It does not: the solver centres the picture in the band,
    // so the two only coincide when the letterbox happens to be exactly 46px, and every other time
    // the "frame" is a line across the render or a line floating in the letterbox. Drawn on the
    // picture's own edges it is a frame by construction, and a misalignment becomes impossible
    // rather than merely unlikely.
    var frameColor = live ? S.green : S.gold;
    var areaBottom = g.areaY + g.areaH;
    var bandTopY = (typeof g.bandBottomY === 'number') ? g.bandBottomY : areaBottom;
    place(chrome.frameTop, px, Math.max(0, py - 2), pw, 2);
    place(chrome.frameBottom, px, bottom, pw, 2);
    place(chrome.bandBottom, g.areaX, bandTopY, g.areaW, Math.max(0, areaBottom - bandTopY));
    var edges = [chrome.frameTop, chrome.frameBottom];
    for (var i = 0; i < edges.length; ++i) {
      M.set(edges[i], { backgroundColor: frameColor });
      show(edges[i], true);
    }
    show(chrome.bandBottom, (areaBottom - bandTopY) > 1);

    place(chrome.catchTop, 0, 0, g.screenW, py);
    // THE BOTTOM CATCHER STOPS AT THE DOCK, and that clamp is the whole fix for "I cannot click
    // the regular timeline in UI mode".
    //
    // It used to run from the preview's lower edge all the way to the bottom of the screen, which
    // is straight over the dock band -- and the catchers only hit-test in UI mode, so in game mode
    // the clicks fell through to Deadlock's replay bar and worked, while turning the cursor ON
    // covered the bar with an invisible panel whose entire job is to swallow clicks. The mode that
    // exists to let you use the UI was the mode that made the UI unusable.
    //
    // Below this line the dock owns hit-testing: the ribbon takes its own clicks, and the dock body
    // deliberately does not hit-test on the native tab so the game's bar can have them.
    var dockTop = g.screenH - (g.dockH || 0);
    place(chrome.catchBottom, 0, bottom, g.screenW, Math.max(0, dockTop - bottom));
    place(chrome.catchLeft, 0, py, px, ph);
    place(chrome.catchRight, right, py, Math.max(0, g.screenW - right), ph);
    var catchers = [chrome.catchTop, chrome.catchBottom, chrome.catchLeft, chrome.catchRight];
    for (var j = 0; j < catchers.length; ++j) {
      show(catchers[j], true);
      hit(catchers[j], cursorOn);
    }

    // The tag names the WORKSPACE, in that workspace's accent, and sits just under the top band.
    // Which camera owns the frame is already said by the rules' colour, so the tag does not say it
    // twice; it answers the other question, which is what this column is.
    if (chrome.tag) {
      var tag = WORKSPACE_TAG[workspace] || WORKSPACE_TAG.camera;
      M.set(chrome.tag, {
        position: Math.floor(g.areaX + 18) + 'px ' + Math.floor(g.areaY + 14) + 'px 0px'
      });
      show(chrome.tag, true);
      try { chrome.tagLabel.text = tag.text; } catch (e) {}
      M.set(chrome.tagLabel, { letterSpacing: '2.2px' });
      M.set(chrome.dot, { backgroundColor: S[tag.accent] });
    }
  }

  // ---- header ---------------------------------------------------------------------------------
  // THE PREVIEW FRAME IS TWO BANDS, NOT A BOX ROUND THE PICTURE.
  //
  // A 44px translucent band across the top of the preview area and another above the dock, each
  // closed by a 2px gold rule. That is what tells you where the render ends, and it is drawn on
  // every dock tab -- it matters most on the game's own bar, where the render runs behind it.
  //
  // It replaces a strip of readouts (SOURCE:MVM, the tick, a mouse-mode button). Each of those now
  // lives where it belongs: the tick on the timeline, the mouse mode in the column's own title row.
  // Two places showing the same tick is two places that can disagree about it.
  function buildHeader() {
    header = M.make(root, 'Panel', 'MvmHeader', {
      backgroundColor: 'rgba(16,19,13,0.9)',
      horizontalAlign: 'left',
      verticalAlign: 'top'
    });
    if (!header) return;
    header.hittest = false;
    header.visible = false;
  }

  // ---- inspector column ------------------------------------------------------------------------
  // The column's width is C++'s to own (it solves the layout the blit follows), so the grip
  // SENDS the width rather than applying it. Coalesced: one command per flush, not one per
  // frame -- the bridge is a bounded queue drained on the main thread, and a drag that flooded
  // it is exactly how the first speed dial came to lag the pointer.
  function sendInspectorW(w, final) {
    gripPendingW = w;
    if (final) {
      gripSendArmed = false;
      cmd('mirv_filmmaker ui inspectorw ' + Math.floor(w));
      return;
    }
    if (gripSendArmed) return;
    gripSendArmed = true;
    $.Schedule(0.05, function () {
      if (!gripSendArmed) return;
      gripSendArmed = false;
      cmd('mirv_filmmaker ui inspectorw ' + Math.floor(gripPendingW));
    });
  }

)CFJS" R"CFJS(  function buildGrip() {
    if (!$.MvmInspectorGrip || !root) return;
    grip = $.MvmInspectorGrip.build({
      root: root,
      colour: S.gold,
      screenW: function () { return lastScreenW > 0 ? lastScreenW : 0; },
      onWidth: sendInspectorW
    });
  }

  function buildColumn() {
    column = M.make(root, 'Panel', 'MvmColumn', {
      backgroundColor: 'rgba(26,26,26,0.94)',
      borderLeft: '1px solid ' + S.border,
      flowChildren: 'down',
      horizontalAlign: 'left',
      verticalAlign: 'top'
    });
    if (!column) return;
    column.visible = false;

    // NO TAB STRIP HERE. The workspaces are reached from the REPLAY BAR -- Cam Editor, Config,
    // Render, Loop -- and the button for the workspace you are already in is hidden. A second set
    // of tabs inside the column would be a second answer to "which workspace am I in", and the
    // column already answers it in its own title.
    //
    // So the body is the whole column, with no padding of its own: each body builds the full
    // column chrome (title, collapse rule, sub-tabs, cards, footer) out of $.MvmControls.column,
    // which is what keeps the three of them identical where the spec says they are identical.
    body = M.make(column, 'Panel', 'MvmBody', {
      width: '100%',
      height: '100%',
      flowChildren: 'down'
    });
  }

  function build() {
    ctx = $.GetContextPanel();
    if (!ctx) return false;

    root = M.make(ctx, 'Panel', 'MvmWorkspaceRoot', {
      // Blitz creates its weapon/card selection layers after the replay HUD. A low HUD-style
      // z-index lets those new siblings cover the whole dock, making the timeline disappear.
      // This workspace is user-invoked tooling and must remain above transient game overlays.
      width: '100%', height: '100%', zIndex: 10000
    });
    if (!root) return false;
    root.hittest = false;
    root.visible = false;

    buildChrome();
    buildHeader();
    buildColumn();

    buildGrip();
    // The INSPECTOR bodies live in the column.
    $.MvmCameraBody && $.MvmCameraBody.build(body);
    $.MvmConfigBody && $.MvmConfigBody.build(body);
    $.MvmRenderBody && $.MvmRenderBody.build(body);

    // The DOCK: a ribbon, then a body that hosts whichever editor is selected. Its own container
    // at a higher z than the column, so a marker flag near the right edge of the timeline draws
    // OVER the column's left border instead of being clipped by it.
    dockBody = M.make(root, 'Panel', 'MvmDockBody', {
      horizontalAlign: 'left', verticalAlign: 'top', zIndex: 60
    });
    if (dockBody) dockBody.visible = false;

    $.MvmDockRibbon && $.MvmDockRibbon.build(root, function () {
      cmd('mirv_filmmaker dock settings');
    });
    if ($.MvmDockRibbon && $.MvmDockRibbon.root()) {
      try { $.MvmDockRibbon.root().style.zIndex = 61; } catch (e) {}
    }

    // All THREE docked editors build into the dock body. Only one is visible at a time, and the
    // first of them is the Regular Timeline -- the game's own replay bar is a 272px widget that
    // ships hidden, so the tab that is supposed to show a timeline has to draw one.
    $.MvmRegular && $.MvmRegular.build(dockBody);
    $.MvmTimeline && $.MvmTimeline.build(dockBody);
    $.MvmGraphBody && $.MvmGraphBody.build(dockBody);

    // The dropdown menu and the tooltip are built LAST and parented at the root, not inside a
    // card. One of each for the whole workspace: a menu inside the card that spawned it is clipped
    // by that card, and two of them can be open at once.
    // The render workspace's JOB column is stage chrome, not part of the inspector: it is pinned
    // to the LEFT of the band and the layout solver insets the preview by its width. So it is built
    // into the root and placed from the solved layout, like the letterbox masks.
    $.MvmRenderBody && $.MvmRenderBody.buildJob && $.MvmRenderBody.buildJob(root);

    $.MvmControls && $.MvmControls.buildOverlays(root);

    return true;
  }

  function showWorkspace(id) {
    if (id === lastWorkspace) return;
    lastWorkspace = id;

    $.MvmCameraBody && $.MvmCameraBody.setVisible(id === 'camera');
    $.MvmConfigBody && $.MvmConfigBody.setVisible(id === 'config');
    $.MvmRenderBody && $.MvmRenderBody.setVisible(id === 'render');
  }

  // Screen size in VIRTUAL pixels -- what style.width accepts. actuallayout* is in device pixels
  // and actualuiscale_* is the factor between them, so dividing once here keeps every placement
  // below honest on a scaled display.
  //
  // Measure from the HUD context panel when our own root still reports zero: a fresh root reads 0
  // for the first layout passes after a build or a resolution change, and trusting that squeezes
  // the whole workspace into the top-left corner for about half a second.
  function measureScreen() {
    var sx = (root && root.actualuiscale_x) || (ctx && ctx.actualuiscale_x) || 1;
    var sy = (root && root.actualuiscale_y) || (ctx && ctx.actualuiscale_y) || 1;
    var w = (root && root.actuallayoutwidth) || 0;
    var h = (root && root.actuallayoutheight) || 0;
    if (w < 16) { w = (ctx && ctx.actuallayoutwidth) || 0; h = (ctx && ctx.actuallayoutheight) || 0; }
    return { w: Math.round(w / sx), h: Math.round(h / sy), sy: sy };
  }

  function reportScreen(s) {
    if (s.w < 16 || s.h < 16) return;
    if (s.w === lastScreenW && s.h === lastScreenH) return;
    lastScreenW = s.w;
    lastScreenH = s.h;
    // Change-gated on purpose. C++ owns the layout arithmetic and only needs telling when the
    // screen actually changes; a console command per frame would be a round trip per frame for a
    // number that moves when someone resizes a window.
    cmd('mirv_filmmaker ui screen ' + s.w + ' ' + s.h);
  }

  function hideAll() {
    if (grip && grip.panel) show(grip.panel, false);
    hideChrome();
    show(header, false);
    show(column, false);
    show(dockBody, false);
    $.MvmDockRibbon && $.MvmDockRibbon.setVisible(false);
    $.MvmRegular && $.MvmRegular.setVisible(false);
    $.MvmTimeline && $.MvmTimeline.setVisible(false);
    $.MvmGraphBody && $.MvmGraphBody.setVisible(false);
  }

)CFJS" R"CFJS(
  function render(st) {
    if (!root) return;
    if (!st) st = {};

    lastState = st;

    // The native HUD driver runs whether or not the workspace is open: it is what restores the
    // game's own panels when the workspace closes, and (F8) what clears them outright on blackout.
    // TEMPORARY bisect flags: $.MvmSkip = { hud, hideall, body, dock } via `mirv_filmmaker ui_eval`.
    var S = ($.MvmSkip = $.MvmSkip || {});
    if (!S.hud) $.MvmNativeHud && $.MvmNativeHud.render(st);

    // The director card runs whether or not the workspace is open too -- it is FOR the moment the
    // workspace is closed (see DeadlockMovieHudJs.h) -- and it reads its own visibility out of
    // st.director rather than st.open/st.blackout, so no gating belongs here.
    $.MvmDirectorHud && $.MvmDirectorHud.render(st);

    // F8: every workspace panel disappears with the rest of the screen, exactly like `!st.open`
    // below, but WITHOUT forgetting what was actually open -- st.open itself is untouched, so the
    // instant blackout lifts this picks the same workspace back up.
    var openNow = st.open && !st.blackout;
    show(root, openNow);
    if (!openNow) {
      $.MvmControls && $.MvmControls.cursorPump(st);
      if (!S.hideall) hideAll();
      lastWorkspace = '';
      return;
    }

    var screen = measureScreen();
    reportScreen(screen);

    var g = st.layout;
    // No layout yet: C++ has not been told the screen size, or the solver rejected it. Draw nothing
    // rather than guessing -- a guessed rect is how a preview ends up framing the wrong part of the
    // screen with no way to tell whether the maths or the measurement is at fault.
    if (!g || !g.valid) {
      $.MvmControls && $.MvmControls.cursorPump(st);
      hideAll();
      return;
    }

    showWorkspace(st.workspace || 'camera');

    var cursorOn = !!st.uiMouse;
    if (S.body) return;
    layoutChrome(g, cursorOn, !!st.live, st.workspace || 'camera');

    // The JOB column only exists in the render workspace, and only the solver knows how wide the
    // band let it be, so it is placed from `g` rather than from its own constant.
    if ($.MvmRenderBody && $.MvmRenderBody.placeJob) {
      $.MvmRenderBody.placeJob(g, (st.workspace === 'render') && (g.leftW || 0) > 8);
    }

    // THE HEADER GOES IN THE STRIP THE SOLVER RESERVED FOR IT, which is ABOVE areaY.
    //
    // WorkspaceLayout_Solve sets areaY = headerH, i.e. the area already begins below the header.
    // Drawing the header AT areaY therefore put it one band too low and left 0..headerH painted by
    // nothing at all -- the bare backbuffer our own blit had just cleared to black, measured as a
    // 44px black strip above the workspace's own ground.
    place(header, g.areaX, Math.max(0, g.areaY - g.headerH), g.areaW, g.headerH);
    show(header, g.headerH > 8);
    hit(header, cursorOn);

    // FULL HEIGHT. The column used to stop at the dock's top edge, which was right while the dock
    // ran the full width of the screen and would have covered anything below. The dock stops at the
    // column's left edge now, so that strip belongs to nothing and showed the bare window through
    // it -- the right-hand side visibly not reaching the bottom.
    //
    // EXCEPT IN RENDER. CS2 runs the dock the whole width there (GraphEditorJs.h:414, `renderOpen
    // ? scrW`; CameraEditorJs.h tabBar `renderWorkspaceOpen ? 0 : inspectorW`), so both the JOB
    // column and the RENDER column end at the dock's top edge instead of beside it.
    // It ends at the RIBBON's top: CS2's Set In / Set Out / Clear sit above its tab strip, and the
    // strip beside the tabs stays empty (measured on 22-cs2-render-graph.png at true scale).
    var renderWs = (st.workspace === 'render');
    var columnH = renderWs ? Math.max(0, g.screenH - (g.dockH || 0)) : g.screenH;
    place(column, g.screenW - g.inspectorW, 0, g.inspectorW, columnH);
    show(column, g.inspectorW > 8);
    hit(column, cursorOn);

    // The drag edge sits on the SOLVED width, so it follows whatever C++ decided, including the
    // clamp; and it is pumped with this frame's cursor, like the dial.
    if (grip && $.MvmInspectorGrip) {
      // As tall as the column: a full-height grip drew its hairline down through the render
      // workspace's full-width dock.
      $.MvmInspectorGrip.place(grip, g.inspectorW, columnH);
      show(grip.panel, g.inspectorW > 8);
      hit(grip.panel, cursorOn);
      $.MvmInspectorGrip.pump(grip, st);
    }

    $.MvmCameraBody && $.MvmCameraBody.render(st);
    $.MvmConfigBody && $.MvmConfigBody.render(st);
    $.MvmRenderBody && $.MvmRenderBody.render(st);
    $.MvmControls && $.MvmControls.cursorPump(st);

    // ---- the dock -------------------------------------------------------------------------------
    if (S.dock) return;
    var d = st.dock || { tab: 'native', ribbonH: 36, editorH: 120, meta: '' };
    var ribbonY = g.screenH - g.dockH;
    var bodyY = ribbonY + d.ribbonH;
    var bodyH = Math.max(0, g.screenH - bodyY);

    // THE DOCK RUNS FROM THE LEFT EDGE TO THE INSPECTOR, ON EVERY TAB AND IN EVERY WORKSPACE.
    //
    // It is never drawn over the inspector column -- that put the Regular Timeline's right-hand end
    // over the card stack, and in the render workspace over the CAPTURE footer. It is NOT inset by
    // the render workspace's JOB column either: that column stops at the dock's top edge instead,
    // which is what the original design did (`uiH: 1080 - dockH`). Insetting the dock as well cost the
    // transport 340px it needs -- the speed dial and the workspace buttons ran off the right.
    // Render is the one workspace whose dock spans the screen (see the column above): CS2's render
    // columns both stop at the dock, so nothing is under it.
    var dockX = 0;
    var dockW = Math.max(0, Math.floor(g.screenW - (renderWs ? 0 : (g.inspectorW || 0))));
    // The ribbon is exactly as wide as the dock body it heads.
    var ribbonW = dockW;

    if ($.MvmDockRibbon) {
      $.MvmDockRibbon.place(dockX, ribbonY, ribbonW, d.ribbonH);
      $.MvmDockRibbon.sync(d.tab, d.meta || '');
      $.MvmDockRibbon.setVisible(true);
      $.MvmDockRibbon.setHittest(cursorOn);
    }

    // Every tab draws something of ours now, so the body is always live. Only the active editor
    // is laid out and rendered: an editor that is not on screen still costs a few hundred style
    // writes a frame if it is asked to draw itself anyway.
    // THE NATIVE TAB HOSTS DEADLOCK'S OWN BAR, so nothing of ours goes in that band.
    //
    // The body panel is the ground our two editors draw on; on the native tab it would be an opaque
    // rectangle over the bar we are trying to show. Hidden rather than made transparent, because a
    // hit-testing panel of ours over the bar also eats every press it was supposed to get -- its
    // play button, its skips, its scrubber -- while still looking exactly right.
    var hostsNativeBar = (d.tab === 'native');
    place(dockBody, dockX, bodyY, dockW, bodyH);
    show(dockBody, !hostsNativeBar);
    hit(dockBody, cursorOn && !hostsNativeBar);

    // Our own drawn transport is retired: the Regular Timeline IS the game's bar now, with our
    // speed dial and workspace buttons in it. Keeping both would put two transports, two speed
    // controls and two sets of workspace buttons in front of the reviewer, and they could disagree.
    if ($.MvmRegular) $.MvmRegular.setVisible(false);
    if ($.MvmTimeline) {
      $.MvmTimeline.setVisible(d.tab === 'camera');
      if (d.tab === 'camera') {
        $.MvmTimeline.layout(g, cursorOn, bodyH, dockW);
        $.MvmTimeline.render(st);
      }
    }
    if ($.MvmGraphBody) {
      $.MvmGraphBody.setVisible(d.tab === 'graph');
      if (d.tab === 'graph') {
        $.MvmGraphBody.layout(g, cursorOn, bodyH, dockW);
        $.MvmGraphBody.render(st);
      }
    }

    // THE DOCK'S RECTANGLE, REPORTED ONCE PER CHANGE. C++ converts cursor pixels into ticks and
    // values through it, so it has to be the same rectangle the graph just drew into -- and the
    // graph cannot be the one to measure it, because a rectangle round-tripped through a console
    // command is a frame stale by construction. Reporting the rectangle the LAYOUT produced, from
    // the one place that produced it, keeps the two the same number.
    var dockKey = dockX + ':' + Math.floor(bodyY) + ':' + Math.floor(dockW) + ':' + Math.floor(bodyH);
    if (dockKey !== lastDockKey) {
      lastDockKey = dockKey;
      cmd('mirv_filmmaker dock rect ' + dockX + ' ' + Math.floor(bodyY) + ' '
          + Math.floor(dockW) + ' ' + Math.floor(bodyH));
    }
  }

  $.MvmWorkspace = {
    build: build,
    render: render,
    root: function () { return root; },
    // For the probes: the state object of the last frame, verbatim.
    state: function () { return lastState; }
  };
})();
)CFJS"
