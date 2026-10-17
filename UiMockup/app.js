// Mockup driver: ONE persistent Panorama context that boots the way the game does after a demo
// loads -- nothing of ours open, CS2's demo bar with CAM EDITOR / CONFIG / LOOP -- and is then
// walked by clicking inside the UI, exactly as in game.
//
// The shape mirrors Filmmaker::RunMainThreadFrame:
//
//   * The always-alive HUDs (MovieHud, MarkerHud, CameraTimeline, DemoBarButtons, AimCrosshair,
//     FxDebug) are evaluated once at boot, in the engine's pump order, and rendered every frame.
//   * The workspaces (ConfigHud, CameraEditor, GraphEditor) are BUILT when their flag turns on and
//     TORN DOWN when it turns off -- BuildIfNeeded() / Teardown() -- with the same delete-by-id list
//     the C++ runs (read out of the driver's Teardown() by extract-ui.mjs). Nothing is hidden and
//     kept; a closed workspace is gone, as in game.
//   * The dock (Movie/TimelineDock.cpp) is claimed, released and applied per frame by whichever
//     workspace hosts it, so the CAMERA / GRAPH / REGULAR ribbon tabs, and every quirk of who gets
//     the dock when, come out the way the engine does them.
//   * Every control writes a console command; shim/commands.js turns it into model state; the next
//     frame's applyWorkspaces() turns the model into built and torn-down panels. The sidebar never
//     reaches into a panel: its buttons issue the same commands and call the same api entry points a
//     person or the C++ side would.

(function () {
  'use strict';

  var state = null;
  var manifest = [];
  var running = false;
  var lastFrame = 0;
  var loopHandle = 0;

  var hostEl = document.getElementById('hostlayer');
  var stageEl = document.getElementById('stage');
  var errorEl = document.getElementById('errorbox');

  // Set from generated/state-snapshot.json when one has been captured; null means run on the
  // built-in stand-in. ?live=0 forces the stand-in.
  var liveState = null;

  var Mockup = window.MvmMockup = {
    stageScale: 1,
    onLog: function (s) { pushDev('log', s); }
  };

  // ---------------------------------------------------------------- dev panel

  var devBuf = { log: [], cmd: [], issues: [] };
  function pushDev(kind, line) {
    var b = devBuf[kind];
    b.push(line);
    if (b.length > 300) b.shift();
    var pane = document.getElementById('pane-' + kind);
    if (pane && pane.classList.contains('active')) renderDev(kind);
  }
  function renderDev(kind) {
    var pane = document.getElementById('pane-' + kind);
    if (!pane) return;
    if (kind === 'edits') {
      pane.textContent = window.MvmCompare && window.MvmCompare.exportEdits
        ? window.MvmCompare.exportEdits()
        : 'compare.js is not loaded';
      return;
    }
    if (kind === 'issues') {
      var unknown = window.PanoramaShim.unknownStyleProps();
      var unh = window.MvmCommands.unhandled();
      var out = ['UNMAPPED STYLE PROPERTIES', '-------------------------'];
      var keys = Object.keys(unknown);
      out.push(keys.length ? keys.map(function (k) { return '  ' + k + '  x' + unknown[k]; }).join('\n') : '  (none)');
      out.push('', 'UNROUTED CONSOLE COMMANDS', '-------------------------');
      var ck = Object.keys(unh);
      out.push(ck.length ? ck.map(function (k) { return '  ' + k + '  x' + unh[k]; }).join('\n') : '  (none)');
      pane.textContent = out.join('\n');
      return;
    }
    pane.innerHTML = devBuf[kind].slice(-200).map(function (l) {
      return '<div class="' + (kind === 'cmd' ? 'cmd' : '') + '">' + escapeHtml(l) + '</div>';
    }).join('');
    pane.scrollTop = pane.scrollHeight;
  }
  function escapeHtml(s) {
    return String(s).replace(/[&<>]/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]; });
  }

  function showError(msg) {
    errorEl.style.display = 'block';
    errorEl.textContent = msg;
  }
  function clearError() { errorEl.style.display = 'none'; errorEl.textContent = ''; }

  // ---------------------------------------------------------------- stage sizing

  // 'fit' scales the 1920x1080 stage down to the pane; a fixed zoom keeps it at native (or 2x)
  // and lets the pane scroll, which is what makes a like-for-like comparison against a game
  // screenshot possible instead of judging a downscaled thumbnail.
  var zoomMode = 'fit';
  var stageWidth = 1920;
  function fitStage() {
    var vpEl = document.getElementById('viewport');
    var vp = vpEl.getBoundingClientRect();
    var k = (zoomMode === 'fit') ? Math.min(vp.width / stageWidth, vp.height / 1080) : zoomMode;
    Mockup.stageScale = k;
    stageEl.style.transform = 'scale(' + k + ')';
    var wrap = document.getElementById('stagewrap');
    stageEl.style.width = stageWidth + 'px';
    wrap.style.width = (stageWidth * k) + 'px';
    wrap.style.height = (1080 * k) + 'px';
    // The game-render plate sits outside .stage (it must not be clipped by the stage's overflow),
    // so the scale transform never reaches it and its own rect has to follow k by hand.
    syncBackdrop(true);
    vpEl.classList.toggle('zoomed', zoomMode !== 'fit');
  }
  function setZoom(mode, btnId) {
    zoomMode = mode;
    ['zoom-fit', 'zoom-1', 'zoom-2'].forEach(function (id) {
      document.getElementById(id).classList.toggle('on', id === btnId);
    });
    fitStage();
  }
  window.addEventListener('resize', fitStage);
  // A page opened in a background tab measures a 0x0 viewport, so the stage was fitted to scale
  // 0 and stayed collapsed after the tab came forward -- every panel reported hidden and the
  // editor's own layout guards fired. Re-fit whenever the tab becomes visible, and from the
  // frame loop while the scale is still 0.
  document.addEventListener('visibilitychange', function () { if (!document.hidden) fitStage(); });

  // ---------------------------------------------------------------- the scaled world
  // THE PLATE FOLLOWS `previewrect`, exactly as the game's blit does.
  //
  // In game the world is rendered full-screen and a pre-UI D3D blit scales it into the rect the
  // workspace publishes on its root as `previewrect` (normalised x0 y0 x1 y1); everything outside
  // that rect is our own chrome, over black. The mockup used to paint the capture across the whole
  // stage, which showed world in two places the game never does: the letterbox bands around the
  // preview, and the hairline seams between two chrome panels that appear once the stage is scaled
  // to fit (the panels are pixel-exact at 1:1 -- the seam is the browser's sub-pixel rounding, not
  // a hole in the layout). Following the rect also means the plate RESIZES the moment the layout
  // does: switching the dock tab or dragging the column edge rescales the world here like in game.
  var backdropEl = document.getElementById('backdrop');
  var lastBackdropKey = '';
  function previewRectNow() {
    // Whichever workspace owns the screen publishes it; with none open the game render is
    // full-screen, which is also what the panels publish as they close.
    var ids = ['CamEditorRoot', 'ConfigHudRoot'];
    for (var i = 0; i < ids.length; i++) {
      var p = window.PanoramaShim.findById(ids[i]);
      if (!p || !p.GetAttributeString) continue;
      var raw = p.GetAttributeString('previewrect', '');
      if (!raw) continue;
      var n = String(raw).trim().split(/\s+/).map(Number);
      if (n.length === 4 && n.every(isFinite) && n[2] > n[0] && n[3] > n[1]) return n;
    }
    return [0, 0, 1, 1];
  }
  function syncBackdrop(force) {
    if (!backdropEl) return;
    var r = previewRectNow();
    var k = Mockup.stageScale || 1;
    var key = r.join(' ') + '@' + k;
    if (!force && key === lastBackdropKey) return;
    lastBackdropKey = key;
    backdropEl.style.left = (r[0] * 1920 * k) + 'px';
    backdropEl.style.top = (r[1] * 1080 * k) + 'px';
    backdropEl.style.width = ((r[2] - r[0]) * 1920 * k) + 'px';
    backdropEl.style.height = ((r[3] - r[1]) * 1080 * k) + 'px';
  }

  // ---------------------------------------------------------------- mouse pipe
  // The Config panel's sliders read the raw cursor out of the state feed (Panorama has no
  // mouse-move event), so the browser cursor is published the same way the engine publishes it.
  var mseq = 0;
  function wireMouse() {
    stageEl.addEventListener('mousemove', function (e) {
      if (!state) return;
      var r = stageEl.getBoundingClientRect();
      state.mx = Math.round((e.clientX - r.left) / Mockup.stageScale);
      state.my = Math.round((e.clientY - r.top) / Mockup.stageScale);
      state.shift = e.shiftKey;
      state.mseq = ++mseq;
    });
    stageEl.addEventListener('mousedown', function (e) {
      if (!state) return;
      if (e.button === 0) { state.lmb = true; state.mclick = (state.mclick || 0) + 1; }
      if (e.button === 2) state.rmb = true;
      state.shift = e.shiftKey; state.mseq = ++mseq;
    });
    window.addEventListener('mouseup', function (e) {
      if (!state) return;
      if (e.button === 0) state.lmb = false;
      if (e.button === 2) state.rmb = false;
      state.mseq = ++mseq;
    });
  }

  // ---------------------------------------------------------------- hotkeys
  // MovieMode::OnKey (Movie/MovieMode.cpp) is a hand-written if-chain, so this is a hand-written
  // table that follows its branches for the director surface. Each line names the branch it
  // mirrors; the mode-dependent ones read the same model flags the C++ reads.
  //   F8      -> HudBlackout_Toggle while a demo is active        (MovieMode.cpp:423)
  //   Esc     -> marker close if the marker card is up            (:415); else the workspaces'
  //              own ESC EXIT (CameraEditorJs.h / ConfigHudJs.h send editor close / config close)
  //   Space   -> camtl playtest when a path editor overlay is up and the path is idle (:442),
  //              else demo_resume / demo_pause                     (:452)
  //   Left/Right -> skip 15s (camtl scrub inside the path range when the timeline is up) (:465)
  //   G       -> camtl cursor toggle                               (:338)
  //   K       -> marker place (editing mode)                       (:524)
  //   L       -> marker delete (the aimed-at marker; here: the selected one) (:530)
  //   J       -> marker previewstop / play / preview by mode       (:370)
  //   Ctrl+Z / Ctrl+Shift+Z -> grapheditor or camtl undo / redo   (:305)
  var keyDownSet = {};
  function wireKeys() {
    document.addEventListener('keydown', function (e) {
      if (!state || state.nav.context !== 'ingame' || !state.demo) return;
      var tag = (e.target && e.target.tagName) || '';
      if (/^(INPUT|TEXTAREA|SELECT)$/.test(tag) || (e.target && e.target.isContentEditable)) return;
      var key = e.key;
      if (keyDownSet[key] && key !== 'ArrowLeft' && key !== 'ArrowRight') return; // no auto-repeat
      keyDownSet[key] = true;
      var cmd = null;
      var pathOverlay = state.dock.tlVisible || state.dock.graphEnabled;
      var mode = state.marker.mode;
      if (key === 'F8') cmd = 'mirv_filmmaker blackout toggle';
      else if (key === 'Escape') {
        if (state.envEditing) cmd = 'mirv_filmmaker environment edit cancel';
        else if (state.marker.menuOpen) cmd = 'mirv_filmmaker marker close';
        else if (mode === 'previewArmed' || mode === 'previewPlaying') cmd = 'mirv_filmmaker marker previewstop';
        else if (mode === 'reposition') cmd = 'mirv_filmmaker marker repositioncancel';
        else if (state.nav.renderOpen) cmd = 'mirv_filmmaker render close';
        else if (state.nav.editorOpen) cmd = 'mirv_filmmaker editor close';
        else if (state.nav.configOpen) cmd = 'mirv_filmmaker config close';
      } else if (key === ' ') {
        if (mode === 'previewArmed') cmd = 'mirv_filmmaker marker play';
        else if (mode === 'previewPlaying') cmd = 'mirv_filmmaker camtl pause';
        else if (pathOverlay && mode === 'editing') cmd = 'mirv_filmmaker camtl playtest';
        else cmd = state.paused ? 'demo_resume' : 'demo_pause';
      } else if (key === 'ArrowLeft' || key === 'ArrowRight') {
        if (mode === 'editing') {
          var delta = 15 * window.MvmState.TICK_RATE * (key === 'ArrowRight' ? 1 : -1);
          var target = Math.max(0, state.tick + delta);
          cmd = state.dock.tlVisible ? 'mirv_filmmaker camtl scrub ' + target : 'demo_gototick ' + target;
        }
      } else if (key === 'g' || key === 'G') cmd = 'mirv_filmmaker camtl cursor toggle';
      else if ((key === 'k' || key === 'K') && mode === 'editing') cmd = 'mirv_filmmaker marker place';
      else if ((key === 'l' || key === 'L') && mode === 'editing') cmd = 'mirv_filmmaker marker delete';
      else if (key === 'j' || key === 'J') {
        if (mode === 'previewPlaying') cmd = 'mirv_filmmaker marker previewstop';
        else if (mode === 'previewArmed') cmd = 'mirv_filmmaker marker play';
        else if (mode === 'editing') cmd = 'mirv_filmmaker marker preview';
      } else if ((key === 'z' || key === 'Z') && (e.ctrlKey || e.metaKey)) {
        var redo = e.shiftKey;
        if (state.dock.graphEnabled) cmd = 'mirv_filmmaker grapheditor ' + (redo ? 'redo' : 'undo');
        else if (state.dock.tlVisible) cmd = 'mirv_filmmaker camtl ' + (redo ? 'redo' : 'undo');
      }
      if (!cmd) return;
      e.preventDefault();
      pushDev('log', '[key] ' + key + ' -> ' + cmd);
      window.PanoramaShim.onCommand(cmd);
    });
    document.addEventListener('keyup', function (e) { delete keyDownSet[e.key]; });
    window.addEventListener('blur', function () { keyDownSet = {}; });
  }

  // ---------------------------------------------------------------- the native CS2 demo bar

  // CS2's own demo playback controller, rebuilt from the decompiled native layout
  // (reference/cs2 refs/layout/hud/huddemocontroller.xml + styles/hud/huddemocontroller.css).
  //
  // This is not decoration. Several Source MVM surfaces do not own their chrome at all -- the demo
  // bar buttons and the speed scrubber dock INTO this panel, found by id, and the camera timeline
  // restyles it. Without the real ids and the real geometry they build nothing, and any docking
  // bug would be invisible. Ids, sizes, margins and icons below are Valve's values.
  // CS2's own demo bar reads the engine; here it reads the same state feed the Source MVM panels
  // do, so the clock, the round and the play glyph agree with the rest of the mockup instead of
  // sitting on the placeholder strings this was first built with.
  var nativeRefs = {};
  var TICKRATE = 64;
  function clock(sec, withTenths) {
    if (!isFinite(sec) || sec < 0) sec = 0;
    var m = Math.floor(sec / 60);
    var s = sec - m * 60;
    return m + ':' + (s < 10 ? '0' : '') + (withTenths ? s.toFixed(1) : String(Math.floor(s)));
  }
  // DemoBarButtons.cpp publishes the raw cursor onto #MirvSpeedRow every frame as 'mirvcursor'
  // ("x y lmb seq speed", client device px) -- the speed dial reads that, not the state feed,
  // so without it the dial could be seen but not dragged. The stage is 1920x1080 at scale 1, so
  // stage px are device px.
  function publishSpeedCursor(st) {
    if (!st) return;
    var row = window.PanoramaShim.findById('MirvSpeedRow');
    if (!row || !row.SetAttributeString) return;
    var speed = (typeof st.speed === 'number' && st.speed > 0) ? st.speed : 1;
    row.SetAttributeString('mirvcursor', (st.mx | 0) + ' ' + (st.my | 0) + ' ' + (st.lmb ? 1 : 0) + ' ' + (st.mseq | 0) + ' ' + speed);
  }

  function syncNativeChrome(st) {
    if (!st) return;
    if (nativeRefs.time) {
      var total = (st.demoEndTick || 0) / TICKRATE;
      nativeRefs.time.text = clock(st.time || 0, true) + ' / ' + clock(total, false);
    }
    if (nativeRefs.round) {
      // CS2 feeds its own bar {d:round_number} from the demo's interval list, which never
      // reaches the Source MVM state feed -- so this is derived from the playhead against the
      // same 24 intervals RoundMarkers draws. It moves correctly as you scrub; it is not the
      // demo's real round number.
      var total = st.demoEndTick || 0;
      var n = total > 0 ? Math.floor((st.tick || 0) / total * 24) + 1 : 1;
      nativeRefs.round.text = String(st.round != null ? st.round : Math.max(1, Math.min(24, n)));
    }
    if (nativeRefs.speed) nativeRefs.speed.text = (st.speed == null ? 1 : st.speed) + 'x';
    if (nativeRefs.playIcon) {
      // The button shows the action, not the status: paused offers play.
      var glyph = st.paused ? 'play' : 'pause';
      nativeRefs.playIcon.style.backgroundImage = 'url("s2r://panorama/images/icons/ui/' + glyph + '.vsvg")';
    }
  }

  function buildNativeChrome(ctx) {
    function P(type, parent, id) { return $.CreatePanel(type, parent, id, {}); }

    nativeRefs = {};
    var host = P('Panel', ctx, 'CSGOHudDemoController');
    host.hittest = false;
    host.style.width = '100%'; host.style.height = '100%';

    var root = P('Panel', host, 'Root');
    root.style.width = '100%'; root.style.height = 'fit-children';
    root.style.flowChildren = 'down'; root.style.verticalAlign = 'bottom';

    // The gear fly-out (X-Ray / True View / DOA / version mismatch). Hidden at rest, exactly as in
    // game -- this is the menu the Camera Editor's own gear button drives.
    var settings = P('Panel', root, 'Settings');
    settings.hittest = true; settings.visible = false;
    settings.style.horizontalAlign = 'right'; settings.style.flowChildren = 'down';
    settings.style.backgroundColor = 'rgba(20,23,28,0.94)';
    settings.style.padding = '10px'; settings.style.marginRight = '10px';
    [['XRayCheckbox', 'X-Ray', ''],
     ['TrueViewCheckBox', 'True View', 'Show what the player actually saw.'],
     ['TrueViewDOACheckBox', 'Include DOA actions', 'Predict dead-or-alive actions.'],
     ['TrueViewWrongVersionCheckBox', 'Allow mismatched versions', 'Demo recorded on another build.']
    ].forEach(function (c) {
      var box = P('Panel', settings, c[0]);
      box.style.flowChildren = 'right'; box.style.marginBottom = '6px';
      var tog = P('ToggleButton', box, c[0].replace('CheckBox', 'ToggleButton').replace('Checkbox', 'ToggleButton'));
      tog.style.width = '18px'; tog.style.height = '18px';
      tog.style.border = '1px solid #ffffff40'; tog.style.marginRight = '8px';
      var body = P('Panel', box, ''); body.style.flowChildren = 'down';
      var t = P('Label', body, ''); t.text = c[1];
      t.style.color = '#e3e8ee'; t.style.fontSize = '15px';
      if (c[2]) { var e = P('Label', body, ''); e.text = c[2]; e.style.color = '#8b95a1'; e.style.fontSize = '12px'; }
    });

    // #Contents is 96px tall in the full controller (45px in the minimal one) since the
    // 2026-10-14 CS2 build.
    var contents = P('Panel', root, 'Contents');
    contents.hittest = true;
    contents.style.width = '100%'; contents.style.height = '96px';
    contents.style.flowChildren = 'down';
    contents.style.backgroundColor = 'rgba(22,26,32,0.86)';
    contents.style.boxShadow = '#000000 0px 0px 2px 0px';

    var sliderRow = P('Panel', contents, 'SliderRow');
    sliderRow.style.width = '100%'; sliderRow.style.height = '50px';

    var roundMarkers = P('Panel', sliderRow, 'RoundMarkers');
    roundMarkers.style.width = '100%'; roundMarkers.style.height = '100%';
    roundMarkers.style.margin = '0px 10px'; roundMarkers.style.verticalAlign = 'center';
    roundMarkers.style.flowChildren = 'right';
    for (var r = 0; r < 24; r++) {
      var seg = P('Panel', roundMarkers, '');
      seg.style.height = '8px'; seg.style.marginTop = '30px';
      seg.style.width = 'fill-parent-flow(1.0)'; seg.style.marginRight = '2px';
      seg.style.backgroundColor = (r % 2) ? '#ffffff50' : '#ffffff20';
    }
    P('Panel', sliderRow, 'HighlightMarkers');

    // The kill / death / bullet pips CS2 lays over the scrub bar. The engine builds these from the
    // demo's own event list, which this mockup does not have, so the SPREAD is invented -- but the
    // icons, sizes, wash colour and shadow are the real ones out of huddemocontroller.css, because
    // an empty bar here reads as a missing feature rather than as missing data.
    var hlIcons = P('Panel', sliderRow, 'HighlightIcons');
    hlIcons.style.width = '100%'; hlIcons.style.height = '100%';
    hlIcons.style.margin = '0px 10px'; hlIcons.style.zIndex = '1';
    // huddemocontroller.css lifts this strip by 10px, but that is relative to a native row this
    // mockup does not reproduce exactly, and here it just pushed the icons out of the bar and
    // they came back sliced in half. Sit them at the top of the row, where the game draws them.
    var PIPS = [3, 5.5, 6.4, 7.4, 11, 16.4, 17.4, 21, 26.4, 30, 31, 34.5, 39.4, 40.4, 43.5,
                47, 48.4, 50, 54, 58, 59.5, 63, 64.4, 66, 70, 74.4, 75.4, 78, 82, 86,
                87.4, 90, 91.4, 94, 96];
    var PIP_KIND = ['kill', 'death', 'kill', 'death', 'death', 'kill', 'death', 'death'];
    PIPS.forEach(function (pct, n) {
      var kind = PIP_KIND[n % PIP_KIND.length];
      var pip = P('Panel', hlIcons, '');
      pip.style.width = '22px'; pip.style.height = '22px';
      // Percent, not px: this strip is 1490 wide when the editor docks it and 1900 when it does
      // not, and a px span sized for one ran the last pips off the end of the other.
      pip.style.horizontalAlign = 'left';
      pip.style.marginLeft = pct + '%';
      pip.style.marginTop = '2px';
      pip.style.backgroundImage = 'url("s2r://panorama/images/icons/ui/'
        + (kind === 'kill' ? 'crosshair_circle' : 'kill') + '.vsvg")';
      pip.style.backgroundSize = 'contain'; pip.style.backgroundRepeat = 'no-repeat';
      pip.style.washColor = '#707070ff';
      pip.style.brightness = '0.8';
    });

    var slider = P('Panel', sliderRow, 'Slider');
    slider.style.width = '100%'; slider.style.height = '100%';
    slider.style.margin = '0px 10px';
    var thumb = P('Panel', slider, 'SliderThumb');
    thumb.style.width = '4px'; thumb.style.height = '16px';
    thumb.style.backgroundColor = '#ffffff'; thumb.style.marginTop = '26px';

    var controlRow = P('Panel', contents, 'ControlRow');
    controlRow.style.width = '100%'; controlRow.style.height = '41px';
    controlRow.style.margin = '0px 5px 5px 5px'; controlRow.style.flowChildren = 'right';

    function icon(parent, id, name) {
      var im = P('Image', parent, id);
      im.style.height = '36px'; im.style.padding = '5px'; im.style.verticalAlign = 'center';
      im.style.width = '36px';
      im.style.backgroundImage = 'url("s2r://panorama/images/icons/ui/' + name + '.vsvg")';
      im.style.backgroundSize = 'contain'; im.style.backgroundRepeat = 'no-repeat';
      im.style.backgroundPosition = 'center';
      return im;
    }
    var playBtn = P('Panel', controlRow, 'PlayButton');
    playBtn.style.height = '100%'; playBtn.style.width = '41px';
    playBtn.style.padding = '5px 0px'; playBtn.style.borderRadius = '3px';
    nativeRefs.playIcon = icon(playBtn, 'PauseIcon', 'pause');
    // The game's own play/pause button toggles demo_pause; here it goes through the router so the
    // clock, the glyph and every Source MVM panel agree.
    playBtn.SetPanelEvent('onactivate', function () {
      window.PanoramaShim.onCommand(state && state.paused ? 'demo_resume' : 'demo_pause');
    });
    function group(id) {
      var g = P('Panel', controlRow, id);
      g.style.marginLeft = '30px'; g.style.height = 'fit-children';
      g.style.flowChildren = 'right'; g.style.verticalAlign = 'center';
      return g;
    }
    function label(parent, id, text) {
      var l = P('Label', parent, id); l.text = text;
      l.style.padding = '5px'; l.style.color = '#d6dde4'; l.style.verticalAlign = 'center';
      return l;
    }
    // The row as of the 2026-10-14 CS2 build: play | speed | time | -/+ step | round | player |
    // next event | spacer | camera mode | highlights | gear | end playback.
    var speed = group('SpeedControls');
    speed.style.marginLeft = '15px';
    // CS2's own stepped speed Slider. The Source MVM speed dial hides it and takes its place.
    var speedSlider = P('Panel', speed, 'SpeedSliderContainer');
    speedSlider.style.width = '140px'; speedSlider.style.height = '36px';
    var ts = label(speed, 'TimeScale', '1x');
    nativeRefs.speed = ts;
    ts.style.width = '70px';

    var timeLabel = P('Panel', controlRow, 'TimeLabel');
    timeLabel.style.marginLeft = '20px'; timeLabel.style.flowChildren = 'right';
    timeLabel.style.verticalAlign = 'center';
    label(timeLabel, 'TimeTitle', 'Time:');
    nativeRefs.time = label(timeLabel, 'TimeValue', '');
    nativeRefs.time.style.fontSize = '15px';

    var timeCtl = group('TimeControls');
    label(timeCtl, 'TimeStepBackward', '-5s');
    icon(timeCtl, 'TimeIcon', 'clock').style.padding = '2px';
    label(timeCtl, 'TimeStepForward', '+5s');

    var roundCtl = group('RoundControls');
    icon(roundCtl, 'RoundPrev', 'skip').style.transform = 'rotateZ(180deg)';
    var restart = P('Panel', roundCtl, 'RoundRestart');
    restart.style.height = '100%'; restart.style.flowChildren = 'right';
    label(restart, 'IntervalLabel', 'Round: ');
    var rn = label(restart, 'RoundNumber', '');
    nativeRefs.round = rn;
    rn.style.width = '22px'; rn.style.padding = '5px 0px'; rn.style.color = '#ffffff';
    icon(roundCtl, 'RoundNext', 'skip');

    var playerCtl = group('PlayerControls');
    icon(playerCtl, 'PlayerPrev', 'message_arrow').style.width = '32px';
    var pname = label(playerCtl, 'PlayerName', 'Player');
    pname.style.width = '190px'; pname.style.textAlign = 'center';
    icon(playerCtl, 'PlayerNext', 'message_arrow').style.width = '32px';

    // In game this is double_arrows + the domination glyph out of images/hud; skip stands in.
    var nextEvent = icon(controlRow, 'NextEventButton', 'skip');
    nextEvent.style.marginLeft = '10px';

    var spacer = P('Panel', controlRow, 'ControlRowSpacer');
    spacer.style.width = 'fill-parent-flow(1.0)';

    var cam = P('Panel', controlRow, 'CameraButton');
    cam.style.marginLeft = '30px'; cam.style.flowChildren = 'right'; cam.style.verticalAlign = 'center';
    icon(cam, '', 'camera');
    label(cam, 'CameraModeLabel', 'First Person');

    icon(controlRow, 'HighlightsButton', 'film');

    var setBtn = P('Panel', controlRow, 'SettingsButton');
    setBtn.style.height = '36px'; setBtn.style.verticalAlign = 'center';
    icon(setBtn, 'SettingsIcon', 'settings');
    setBtn.SetPanelEvent('onactivate', function () { settings.visible = !settings.visible; });

    var end = P('Label', controlRow, 'EndPlayback');
    end.text = 'End Playback';
    end.style.verticalAlign = 'center';
    end.style.padding = '5px'; end.style.marginRight = '10px'; end.style.height = '36px';
    end.style.backgroundColor = '#ffffff08'; end.style.borderRadius = '2px';
    end.style.color = '#d6dde4';

    return host;
  }

  // ---------------------------------------------------------------- the context
  // Which scripts are alive, in the engine's pump order (Filmmaker.cpp RunMainThreadFrame).
  // The always-alive ones are evaluated when the context is entered; the on-demand ones (a
  // `teardown` entry in the manifest) are built by applyWorkspaces() when their flag turns on.
  var CONTEXTS = {
    'ingame': {
      label: 'In-game demo',
      note: 'What you see after a demo loads. Nothing open; CAM EDITOR / CONFIG / LOOP on the demo bar.',
      always: ['moviehud', 'markerhud', 'cameratimeline', 'demobar', 'aimcrosshair', 'fxdebug', 'eastereggs', 'onboarding'],
      order: ['moviehud', 'markerhud', 'cameratimeline', 'aimcrosshair', 'confighud', 'cameraeditor', 'grapheditor', 'fxdebug', 'eastereggs', 'onboarding'],
      native: true, demo: true
    },
    'menu-demos': {
      label: 'Main menu: Demos tab',
      note: 'The main-menu demo library. A different Panorama context in game, so it swaps the whole stage.',
      always: ['filmmakergui', 'onboarding'], order: ['filmmakergui', 'onboarding'], native: false, demo: false
    },
    'menu-workshop': {
      label: 'Main menu: Workshop Maps', note: 'Installed maps, native shell, shared filters and keyword editor.',
      always: ['workshop'], order: [], native: false, demo: false
    },
    'menu-video': {
      label: 'Settings: custom resolution',
      note: 'The dialog injected into CS2 Video settings. Menu context, not in-game.',
      always: ['videoresolution'], order: ['videoresolution'], native: false, demo: false
    }
  };

  var sources = {};      // id -> script text, fetched once per page life (watchSources refetches)
  var built = {};        // id -> true while its script is evaluated in the current context
  var live = { config: false, editor: false, graph: false }; // the HUD drivers' m_wasEnabled

  function byId(id) { return (manifest || []).filter(function (m) { return m.id === id; })[0] || null; }

  function evalSurface(id) {
    var m = byId(id);
    if (!m || !sources[id]) { pushDev('log', '[mockup] no script for ' + id); return false; }
    try {
      (0, eval)(sources[id]);
    } catch (e) {
      showError('Script threw while building ' + id + ':' + String.fromCharCode(10) + (e && e.stack || e));
      return false;
    }
    built[id] = true;
    if (m.api && !window.$[m.api]) pushDev('log', '[mockup] $.' + m.api + ' was never registered by ' + id);
    return true;
  }

  // BuildIfNeeded(): the script is evaluated when its api is absent -- after a teardown, or the
  // first time. Its build registers the api and creates its root under the context panel.
  function build(id) {
    var m = byId(id);
    if (!m) return false;
    if (m.api && window.$[m.api]) return true;
    pushDev('log', '[mockup] build ' + id);
    return evalSurface(id);
  }

  // Teardown(): the driver's own delete-by-id list, then $.Api = null. Read out of the C++ by
  // extract-ui.mjs so the browser and the game tear down the same panels.
  function teardown(id) {
    var m = byId(id);
    if (!m || !m.teardown) return;
    pushDev('log', '[mockup] teardown ' + id);
    // Every panel carrying the id, not the first: a DeleteAsync is a timer, and a rebuild that
    // lands before it fires leaves two roots with one name -- the stale one would be deleted and
    // the live one kept.
    m.teardown.roots.forEach(function (rid) {
      [].forEach.call(document.querySelectorAll('#hostlayer [data-pid="' + rid + '"]'), function (el) {
        var p = el.__panel;
        if (p && p.DeleteAsync) { try { p.DeleteAsync(0); } catch (e) { pushDev('log', '[teardown] ' + e); } }
      });
    });
    window.$[m.teardown.api] = null;
    built[id] = false;
  }

  function enterContext(name) {
    var ctxDef = CONTEXTS[name];
    if (!ctxDef) return;
    clearError();
    devBuf.log.length = 0; devBuf.cmd.length = 0;
    state.nav.context = name;
    state.demo = ctxDef.demo;
    built = {};
    live = { config: false, editor: false, graph: false };
    // reset() removes the old panels, but $ outlives the context. Drop their APIs too:
    // otherwise build() sees the old registration and skips evaluating the new source.
    (manifest || []).forEach(function (m) { if (m.api) delete window.$[m.api]; });
    var ctx = window.PanoramaShim.reset(hostEl);
    var isMenu = name === 'menu-demos' || name === 'menu-workshop';
    stageWidth = isMenu ? Number(document.getElementById('menu-aspect').value) : 1920;
    document.getElementById('stage-dimensions').textContent = stageWidth + '×1080';
    hostEl.style.width = stageWidth + 'px'; ctx.style.width = stageWidth + 'px';
    document.getElementById('backdrop').style.visibility = isMenu ? 'hidden' : '';
    if (isMenu) window.MvmNativeMenu.build(ctx, name === 'menu-workshop' ? 'workshop' : 'downloaded');
    if (ctxDef.native && document.getElementById('opt-native').checked) buildNativeChrome(ctx);
    window.PanoramaShim.stateJson = JSON.stringify(state);
    ctxDef.always.forEach(evalSurface);
    if (isMenu) window.MvmNativeMenu.attach();
    fitStage();
    // A rebuild with a workspace already open (Reload, a source edit on disk) must come back with
    // it open, not re-enter it: the enter side effects (hiding the director card, claiming the
    // dock) already happened, so only the panels are missing.
    if (name === 'ingame') {
      if (state.nav.configOpen) { build('confighud'); live.config = true; }
      if (state.nav.editorOpen) { build('cameraeditor'); live.editor = true; }
      if (state.dock.graphEnabled) { build('grapheditor'); live.graph = true; }
    }
    [].forEach.call(document.querySelectorAll('.ctx-btn'), function (b) {
      b.classList.toggle('active', b.dataset.ctx === name);
    });
    renderDev('issues');
  }

  // ---------------------------------------------------------------- the dock
  // Movie/TimelineDock.cpp, verbatim. Rank: editor 2, config 1. Claim succeeds for the current
  // host or a host of at least equal rank; Release only by the host, and it parks the dock;
  // Apply only by the host, and it writes the tab into the two overlays.
  function rank(h) { return h === 'editor' ? 2 : (h === 'config' ? 1 : 0); }
  function dockClaim(h) {
    var d = state.dock;
    if (d.host === h) return true;
    if (rank(h) < rank(d.host)) return false;
    d.host = h;
    return true;
  }
  function dockRelease(h) {
    var d = state.dock;
    if (d.host !== h) return;
    d.host = 'none';
    d.hosted = false; d.tlVisible = false; d.graphEnabled = false;
  }
  function dockApply(h) {
    var d = state.dock;
    if (d.host !== h || h === 'none') return;
    d.hosted = true;
    d.tlVisible = false;   // the camera timeline is never docked: TIMELINE is the unified dock
    d.graphEnabled = (d.tab === 'timeline');
  }

  // ---------------------------------------------------------------- the workspaces
  // Once per frame, in the engine's order: ConfigHud::RunFrame, then CameraEditorHud::RunFrame,
  // then GraphEditorExperimentHud::RunFrame. The enter and exit bodies are the C++ OnEnter /
  // OnExit, so who hides the director card, who owns the dock and what is torn down come out the
  // same. Both workspaces RE-CLAIM every frame, so a claim that loses the open-from-editor race
  // self-heals on the next frame instead of leaving Config with a dead ribbon.
  var customizePending = false;
  function applyWorkspaces() {
    if (state.nav.context !== 'ingame') return;
    var nav = state.nav, dock = state.dock, movie = state.movie;

    // ---- ConfigHud::RunFrame -------------------------------------------------
    // Mutual exclusion: the editor is the bigger workspace; if it is up, Config yields.
    if (nav.configOpen && nav.editorOpen) nav.configOpen = false;
    if (nav.configOpen && !live.config) {
      // OnEnter: latch the cursor on, claim the dock, hide the director card.
      state.cursor = true;
      dockClaim('config');
      movie.prevConfig = movie.visible;
      movie.visible = false;
      live.config = true;
      build('confighud');
    } else if (!nav.configOpen && live.config) {
      // OnExit: park the dock, put the card back, tear down.
      dockRelease('config');
      movie.visible = movie.prevConfig;
      live.config = false;
      teardown('confighud');
    }
    // Re-claim per frame (ConfigHud.cpp does the same): the OnEnter claim can lose the race when
    // Config opens FROM the editor, and without this the dock is never hosted again.
    if (nav.configOpen) { dockClaim('config'); dockApply('config'); }

    // ---- CameraEditorHud::RunFrame -------------------------------------------
    if (nav.editorOpen && !live.editor) {
      // OnEnter: hide the card, host the timeline (visible later via Apply), cursor on, claim,
      // graph follows the remembered tab, pre-select a key for the inspector (no teleport).
      movie.prevEditor = movie.visible;
      movie.visible = false;
      dock.hosted = true; dock.tlVisible = false;
      state.cursor = true;
      dockClaim('editor');
      dock.graphEnabled = (dock.tab === 'timeline');
      if ((state.count || 0) > 0 && state.selected < 0) state.selected = 0;
      live.editor = true;
      build('cameraeditor');
    } else if (!nav.editorOpen && live.editor) {
      // OnExit: release the dock, drop the render modal and any customize request, restore the
      // card only when Config is not taking over, tear down.
      dockRelease('editor');
      dock.hosted = false; dock.tlVisible = false; dock.graphEnabled = false;
      nav.renderOpen = false; state.render.open = false;
      nav.customizeOpen = false; customizePending = false;
      if (!nav.configOpen) movie.visible = movie.prevEditor;
      live.editor = false;
      teardown('cameraeditor');
    }
    if (nav.editorOpen) { dockClaim('editor'); dockApply('editor'); }

    // The Customize modal: CameraEditorHud honours m_openCustomizeRequested once the panel has
    // built. The request is consumed here; whether the modal is UP is read off the DOM after that.
    if (nav.customizeOpen && !customizePending) {
      customizePending = true;
      retry(function () {
        var api = window.$.CamEditor;
        if (!api || !api.openCustomize) return false;
        api.openCustomize();
        return customizeVisible();
      }, function () { customizePending = false; nav.customizeOpen = false; });
    }

    // ---- GraphEditorExperimentHud::RunFrame ----------------------------------
    if (dock.graphEnabled && !live.graph) { live.graph = true; build('grapheditor'); }
    else if (!dock.graphEnabled && live.graph) { live.graph = false; teardown('grapheditor'); }
  }

  // Retries a step until it reports done, as CameraEditorHud does for its opening requests: the
  // editor needs a few render frames before its modal will lay out.
  function retry(step, then, tries) {
    var n = 0, max = tries || 12;
    (function attempt() {
      var done = false;
      try { done = step(); } catch (e) { pushDev('log', '[ready] ' + e); }
      if (done || ++n > max) { if (then) then(done); return; }
      setTimeout(attempt, 250);
    })();
  }
  function customizeVisible() {
    // The modal is the full-screen overlay at z-index 230 (PanoramaBridge.h z-layer map).
    return [].some.call(document.querySelectorAll('#hostlayer .pnl'), function (e) {
      return e.__panel && e.__panel._visible && e.__panel._sty.zIndex === '230';
    });
  }

  // ---------------------------------------------------------------- per-owner projection
  // The real HUD drivers do not publish one shared JSON object: each builds its own, and several
  // keys mean different things to different owners. The model keeps the colliding ones under the
  // owner's namespace (shim/state.js) and this hands each script the feed it would get from its
  // own BuildStateJson.
  var NAMESPACES = ['nav', 'dock', 'movie', 'marker', 'aim', 'fxdbg', 'eggs', 'onboarding'];
  function baseFeed() {
    var feed = Object.assign({}, state);
    NAMESPACES.forEach(function (k) { delete feed[k]; });
    return feed;
  }
  function stateFor(id) {
    var feed = baseFeed();
    var nav = state.nav, dock = state.dock;
    var dockH = { native: 126, timeline: 476 }[dock.tab] || 126;
    switch (id) {
      case 'moviehud':
        // MovieHud.cpp: visible = m_visible && !CameraPath_PreviewHudHidden && !captureHide,
        // and the whole card goes on F8.
        feed.visible = state.movie.visible && !state.previewHudHidden && !state.render.captureHide && !nav.blackout;
        feed.mode = state.movie.mode;
        feed.playing = !!state.demo;
        break;
      case 'markerhud':
        feed.mode = state.marker.mode;
        feed.menuOpen = state.marker.menuOpen;
        feed.notice = state.marker.notice;
        feed.captureHide = state.render.captureHide;
        break;
      case 'cameratimeline':
        feed.hosted = dock.hosted;
        feed.open = dock.tlVisible;
        feed.graphExp = dock.graphEnabled;
        feed.configOpen = nav.configOpen;
        feed.renderOpen = nav.renderOpen;
        feed.blackout = nav.blackout;
        feed.timelineView = 'timeline';
        break;
      case 'grapheditor':
        // GraphEditorExperimentHud::BuildStateJson: the camera channels plus the scene keys under
        // "scene" (SceneKeys_AppendTimelineState) -- ONE timeline for both. The visible window is
        // the JS's to own (it seeds from viewT0/viewT1 once), so this feed supplies the suggestion
        // plus the DOMAIN the overview maps -- the demo's own bounds.
        feed.domainT0 = isFinite(state.demoStartTick) ? state.demoStartTick : 0;
        feed.domainT1 = isFinite(state.demoEndTick) ? state.demoEndTick : 115200;
        var sk = state.sceneKeys; window.MvmSceneKeys.sync(sk, state.scrubbing ? state.playhead : state.tick);
        feed.scene = { rev: sk.rev, selCount: sk.selected.length, selCh: sk.activeCh == null ? -1 : sk.activeCh,
                       canUndo: sk.canUndo, canRedo: sk.canRedo, channels: sk.sceneChannels };
        var camSel = 0;
        (state.channels || []).forEach(function (c) { c.keys.forEach(function (k) { if (k.sel) camSel++; }); });
        feed.selCount = camSel;
        feed.rev = state.camRev || 0;
        feed.canUndo = !!(state.camUndo && state.camUndo.length); feed.canRedo = !!(state.camRedo && state.camRedo.length);
        feed.drive = !!state.graphDrive;
        feed.pathPlaying = false;
        feed.scrubCache = !!state.scrubCache;
        feed.tickRate = window.MvmState.TICK_RATE;
        feed.interp = state.interp === false ? 'Linear' : 'Smooth';
        feed.timelineView = (state.ui && state.ui['editor.timeline.view'] === 'graph') ? 'graph' : 'keys';
        var rr = state.render || {};
        feed.rangeActive = !!rr.resOk; feed.rangeIn = rr.resStart || 0; feed.rangeOut = rr.resEnd || 0;
        // The camera path's marker ticks and each marker's shake (CamPathShake), for the
        // CAMERA PATH layer's "Camera shake" row.
        feed.pathKeys = (state.markers || []).map(function (m) { return m.tick; });
        feed.shakes = (state.shakes || []).filter(function (s) { return state.markers && state.markers[s.i]; }).map(function (s) {
          return { i: s.i, t: state.markers[s.i].tick, s: s.s, l: s.l || 'Hand', n: s.n || 5, p: s.p || 50 };
        });
        feed.shakeStyles = window.MvmShakeMenu || [];
        feed.shakeEssentials = 5;
        feed.enabled = dock.graphEnabled;
        feed.renderOpen = nav.renderOpen;
        break;
      case 'confighud':
        feed.enabled = nav.configOpen && !nav.blackout;
        feed.dockTab = dock.tab;
        feed.dockH = dockH;
        feed.renderOpen = nav.renderOpen;
        feed.renderJobActive = !!state.renderJobActive;
        feed.daPlayers = window.MvmCommands.deathAnimPlayers(state);
        break;
      case 'cameraeditor':
        feed.enabled = nav.editorOpen && !nav.blackout;
        feed.bottomMode = dock.tab;
        feed.timelineView = 'timeline';
        feed.graphExp = dock.graphEnabled;
        feed.dockH = dockH;
        feed.renderOpen = nav.renderOpen;
        feed.render = Object.assign({}, state.render, { open: nav.renderOpen });
        break;
      case 'aimcrosshair':
        Object.assign(feed, state.aim);
        feed.on = state.aim.on && !nav.blackout;
        break;
      case 'fxdebug':
        Object.assign(feed, state.fxdbg);
        break;
      case 'eastereggs':
        // EasterEggs_BuildOverlayJson: the cursor in client fractions, from the same mouse pipe.
        Object.assign(feed, state.eggs);
        feed.cx = (state.mx || 960) / 1920; feed.cy = (state.my || 540) / 1080;
        feed.click = state.mclick || 0; // the C++ side counts button-downs only, not moves
        break;
      case 'onboarding':
        // OnboardingHud::RunFrame: the tutorial state (Onboarding.cpp), the cursor in client
        // fractions, the held left button, and the bottom band in the dock's 1080 units.
        Object.assign(feed, state.onboarding);
        var menuCtx = state.nav.context !== 'ingame';
        feed.ctx = menuCtx ? 'menu' : 'hud';
        feed.hidden = !!state.render.captureHide;
        feed.dockH = menuCtx ? 0 : (dock.hosted ? dockH : 126);
        feed.cx = (state.mx == null ? -1920 : state.mx) / 1920; feed.cy = (state.my == null ? -1080 : state.my) / 1080;
        feed.cvis = state.mx != null && feed.cx >= 0 && feed.cx <= 1 && feed.cy >= 0 && feed.cy <= 1;
        feed.lmb = !!state.lmb;
        // The game state a lesson step waits on. The mockup has no rendered camera, so the
        // world markers (vp / eye) never draw here.
        if (!menuCtx) feed.app = {
          editor: !!nav.editorOpen, config: !!nav.configOpen, render: !!nav.renderOpen,
          graph: !!dock.graphEnabled, dock: dock.tab, insp: state.uiEditorInspector || 'path',
          keys: (state.markers || []).length, pathPlaying: !!state.playing,
          sceneRev: state.sceneKeys ? state.sceneKeys.rev : 0, recoil: !!state.wrOn, bob: !!state.wbOn,
          weapon: 'ak47', demo: !!state.demo, paused: !state.demoPlaying, tick: state.tick || 0
        };
        break;
      case 'filmmakergui':
        feed.enabled = true; feed.demo = false;
        break;
      case 'videoresolution':
        feed.demo = false;
        break;
    }
    return feed;
  }

  // ---------------------------------------------------------------- the frame
  var renderErrorShown = false;
  function frame(now, oneShot) {
    if (!running) return;
    if (!(Mockup.stageScale > 0)) fitStage();
    var dt = Math.min(0.1, (now - lastFrame) / 1000);
    lastFrame = now;

    window.MvmState.advance(state, dt);
    window.MvmCommands.syncEnvironment(state);
    applyWorkspaces();
    syncNativeChrome(state);
    publishSpeedCursor(state);
    window.PanoramaShim.tickVideos(dt);

    // Every alive HUD's render(), in pump order. The api is looked up EVERY frame: a teardown
    // nulls it, a build registers it again.
    var order = CONTEXTS[state.nav.context].order;
    if (state.nav.context === 'menu-demos' || state.nav.context === 'menu-workshop') window.MvmNativeMenu.tick();
    var threw = null;
    for (var i = 0; i < order.length; i++) {
      var m = byId(order[i]);
      var api = m && m.api ? window.$[m.api] : null;
      if (!api || typeof api.render !== 'function') continue;
      window.PanoramaShim.stateJson = JSON.stringify(stateFor(order[i]));
      try { api.render(); } catch (e) { threw = threw || (order[i] + ': ' + (e && e.stack || e)); }
    }
    if (threw) {
      if (!renderErrorShown) { renderErrorShown = true; showError('render() threw:\n\n' + threw); }
    } else if (renderErrorShown) { clearError(); renderErrorShown = false; }
    // Comparison drivers and Dev inspect the persistent model, not whichever per-root projection
    // happened to render last.
    window.PanoramaShim.stateJson = JSON.stringify(state);
    // AFTER render(): the panels publish previewrect as they lay out, so reading it here is the
    // same frame's answer rather than the previous one's.
    syncBackdrop(false);
    syncSidebar();
    if (!oneShot) schedule();
  }

  // requestAnimationFrame is suspended while the tab (or the embedding pane) is hidden, which
  // would freeze the state feed and make every control look dead on the next look. Fall back to a
  // timer when hidden so the panel keeps rendering and state stays live either way.
  function schedule() {
    if (!running) return;
    if (document.hidden) loopHandle = setTimeout(function () { frame(performance.now()); }, 33);
    else loopHandle = requestAnimationFrame(frame);
  }

  // ---------------------------------------------------------------- where am I
  // The legacy surface ids, derived from the model. compare.js and the verify scripts key on
  // these, and the Jump list highlights them.
  var lastJump = null;
  function deriveId() {
    var nav = state.nav;
    if (nav.context === 'menu-demos') return 'filmmakergui';
    if (nav.context === 'menu-workshop') return 'workshop';
    if (nav.context === 'menu-video') return 'videoresolution';
    if (nav.configOpen) return 'confighud';
    if (nav.editorOpen && customizeVisible()) return 'customize';
    if (nav.editorOpen && nav.renderOpen) return 'render';
    if (nav.editorOpen) return 'cameraeditor';
    return 'demobar';
  }
  function currentId() {
    if (lastJump && JUMPS[lastJump] && JUMPS[lastJump].holds(state)) return lastJump;
    return deriveId();
  }
  Mockup.current = function () {
    // Nothing to report until boot has fetched the scripts and wired the router: the harness
    // waits on this before driving, and an early answer let it send commands into a page that
    // could not route them yet.
    if (!running) return null;
    var id = currentId();
    var m = byId(id) || { id: id, label: id, api: null };
    return { id: m.id, label: m.label, api: m.api, apiObj: m.api ? window.$[m.api] : null, src: m.src };
  };
  Mockup.model = function () { return state; };
  Mockup.surfaces = function () { return manifest || []; };
  Mockup.host = function () { return hostEl; };
  Mockup.stage = function () { return stageEl; };
  Mockup.zoom = function (mode) { setZoom(mode, mode === 'fit' ? 'zoom-fit' : (mode === 2 ? 'zoom-2' : 'zoom-1')); };
  // One frame, now, without rescheduling the loop. Headless Chrome never fires
  // requestAnimationFrame when it is not producing frames (--dump-dom), so a driven command would
  // mutate state that nothing ever published or rendered; the harness steps the loop by hand.
  Mockup.tick = function (n) { for (var i = 0; i < (n || 1); i++) frame(performance.now(), true); };
  Mockup.context = function (name) { if (name) enterContext(name); return state.nav.context; };

  var lastSidebarId = null;
  function syncSidebar() {
    var id = currentId();
    if (id === lastSidebarId) return;
    lastSidebarId = id;
    [].forEach.call(document.querySelectorAll('.surface-btn'), function (b) {
      b.classList.toggle('active', !!(b.__entry && b.__entry.id === id));
    });
    var m = byId(id);
    document.getElementById('surfacetitle').textContent = (m ? m.label : id) + (liveState ? '  ·  live state @ tick ' + liveState.tick : '');
  }

  // ---------------------------------------------------------------- jumping
  // A legacy surface id is a PLACE, reached the way a person reaches it: by the console commands
  // the UI's own buttons send (a context switch for the two menu rows), then an api call for the
  // views that need one. `holds` says whether the model is still there, so current() keeps
  // reporting the id you asked for while it is true and derives one once you have moved on.
  function cmds(list) { list.forEach(function (c) { window.PanoramaShim.onCommand('mirv_filmmaker ' + c); }); }
  var AIM_ON = { on: true, valid: true, front: true, sx: 0.5, sy: 0.5, size: 7, gap: 4, thick: 2,
                 col: 'rgba(0,255,120,1)', dot: true, centre: false, view: 0, debugbg: false };
  var JUMPS = {
    demobar: { cmds: ['editor off', 'config off'],
      holds: function (s) { return !s.nav.configOpen && !s.nav.editorOpen; } },
    confighud: { cmds: ['config on'], holds: function (s) { return s.nav.configOpen; } },
    cameraeditor: { cmds: ['editor on', 'render close'],
      holds: function (s) { return s.nav.editorOpen && !s.nav.renderOpen && !customizeVisible(); } },
    render: { cmds: ['editor on', 'render open'],
      holds: function (s) { return s.nav.editorOpen && s.nav.renderOpen; } },
    keyeditor: { cmds: ['editor on', 'render close', 'settings ui editor.inspector path'],
      ready: function (api) {
        if (!api || !api.setInspectorMode) return false;
        api.setInspectorMode('path');
        // Confirmed by the inspector actually showing the key sections.
        return [].some.call(document.querySelectorAll('#hostlayer .pnl'), function (e) {
          var p = e.__panel;
          return p && p._visible && p._text && String(p._text).toUpperCase().indexOf('SELECTED CAMERA') >= 0;
        });
      },
      holds: function (s) { return s.nav.editorOpen && !s.nav.renderOpen && s.uiEditorInspector === 'path'; } },
    customize: { cmds: ['editor on', 'render close', 'editor customize'],
      holds: function (s) { return s.nav.editorOpen && (s.nav.customizeOpen || customizeVisible()); } },
    // The camera timeline is never docked any more (the TIMELINE tab is the unified dock); its own
    // surface is the floating bar `camtl open` puts over the HUD.
    cameratimeline: { cmds: ['editor off', 'config off', 'camtl open'],
      holds: function (s) { return !s.nav.editorOpen && !s.nav.configOpen && s.dock.tlVisible; } },
    grapheditor: { cmds: ['editor on', 'render close', 'dock timeline'],
      holds: function (s) { return s.nav.editorOpen && !s.nav.renderOpen && s.dock.tab === 'timeline'; } },
    markerhud: { cmds: ['editor off', 'config off'], patch: { marker: { mode: 'editing', menuOpen: true, notice: '' } },
      holds: function (s) { return !s.nav.configOpen && !s.nav.editorOpen && s.marker.menuOpen; } },
    moviehud: { cmds: ['editor off', 'config off', 'hud on'],
      holds: function (s) { return !s.nav.configOpen && !s.nav.editorOpen && s.movie.visible; } },
    aimcrosshair: { cmds: [], patch: { aim: AIM_ON }, holds: function (s) { return s.aim.on; } },
    fxdebug: { cmds: ['fx debughud on'], patch: { dbg: true, debug: true }, holds: function (s) { return s.fxdbg.enabled; } },
    filmmakergui: { context: 'menu-demos', cmds: [],
      ready: function (api) { if (!api || !api.gotoTab) return false; api.gotoTab('demos'); return true; },
      holds: function (s) { return s.nav.context === 'menu-demos'; } },
    workshop: { context: 'menu-workshop', cmds: [], holds: function (s) { return s.nav.context === 'menu-workshop'; } },
    videoresolution: { context: 'menu-video', cmds: [], holds: function (s) { return s.nav.context === 'menu-video'; } }
  };
  function jumpTo(id, then) {
    var j = JUMPS[id];
    var m = byId(id);
    if (!j || !m) return false;
    var ctx = j.context || 'ingame';
    if (state.nav.context !== ctx) enterContext(ctx);
    cmds(j.cmds);
    if (j.patch) mergePatch(state, j.patch);
    // A jump to another editor view leaves the Customize modal behind if it is up -- the way a
    // person would press Cancel before going elsewhere.
    if (id !== 'customize' && window.$.CamEditor && window.$.CamEditor.closeCustomize && customizeVisible()) {
      window.$.CamEditor.closeCustomize();
    }
    lastJump = id;
    Mockup.tick(2);
    var run = then || null;
    if (j.ready) {
      retry(function () { return j.ready(m.api ? window.$[m.api] : null); }, function () {
        if (run) setTimeout(run, 350);
      });
    } else if (run) {
      setTimeout(run, 350);
    }
    return true;
  }
  Mockup.select = function (id, then) { return jumpTo(id, then); };

  // ---------------------------------------------------------------- reaching the popups
  // Most dialogs are opened by an internal function with no api entry point, so they are reached
  // the way a user reaches them: find the real control by its label and fire its own handler.
  // That exercises the actual code path rather than forcing a panel visible.
  function livePanels() {
    return [].map.call(document.querySelectorAll('#hostlayer .pnl'), function (e) { return e.__panel; })
      .filter(function (p) { return p && p._visible; });
  }
  function activateByText(needle) {
    var want = needle.toUpperCase();
    var hits = livePanels().filter(function (p) {
      return p._text && String(p._text).toUpperCase().indexOf(want) >= 0;
    });
    for (var i = 0; i < hits.length; i++) {
      var p = hits[i];
      while (p && !(p._events && p._events['onactivate'])) p = p.parent;
      if (p) { p._events['onactivate'](); return true; }
    }
    pushDev('log', '[action] no activatable control labelled: ' + needle);
    return false;
  }
  // `rootId` narrows the search to one root: several always-alive HUDs carry the same events
  // (the marker HUD has a right-click of its own), and the first match is not the one wanted.
  function rootOfPanel(p) {
    for (var n = p; n; n = n.parent) { if (n.id && /Root$/.test(n.id)) return n.id; }
    return '';
  }
  function fireEventOn(evt, index, rootId) {
    var hits = livePanels().filter(function (p) {
      return p._events && p._events[evt] && (!rootId || rootOfPanel(p) === rootId);
    });
    var p = hits[index || 0];
    if (!p) { pushDev('log', '[action] no panel handles ' + evt + (rootId ? ' under ' + rootId : '')); return false; }
    p._events[evt]();
    return true;
  }
  function after(ms, fn) { setTimeout(fn, ms); }

  // Patches merge ONE LEVEL into nested objects. `render` in particular carries a whole output
  // settings object (fps, codec, resolution, folder), and a scenario that only wants to flip
  // `open` must not replace it wholesale -- doing that is what blanked the render workspace's
  // readouts with "undefined fps".
  function mergePatch(target, patch) {
    Object.keys(patch).forEach(function (k) {
      var v = patch[k];
      var isPlainObject = v && typeof v === 'object' && !Array.isArray(v);
      if (isPlainObject && target[k] && typeof target[k] === 'object' && !Array.isArray(target[k])) {
        Object.keys(v).forEach(function (kk) { target[k][kk] = v[kk]; });
      } else {
        target[k] = v;
      }
    });
  }
  // The inverse of a patch against the current state: the same keys, with the values they hold
  // now, one level deep to match mergePatch. Deep-cloned so a later edit cannot change history.
  function snapshotFor(cur, patch) {
    var undo = {};
    Object.keys(patch).forEach(function (k) {
      var v = patch[k], curv = cur[k];
      if (v && typeof v === 'object' && !Array.isArray(v) && curv && typeof curv === 'object' && !Array.isArray(curv)) {
        var sub = {};
        Object.keys(v).forEach(function (kk) { sub[kk] = JSON.parse(JSON.stringify(curv[kk] === undefined ? null : curv[kk])); });
        undo[k] = sub;
      } else {
        undo[k] = JSON.parse(JSON.stringify(curv === undefined ? null : curv));
      }
    });
    return undo;
  }

  // Overlays that open from code rather than from a control that is visible at rest. These are the
  // "what does it look like when it pops up" cases, exposed as buttons so they can be inspected.
  // `needs` is the place the popup lives, reached by the same commands the UI sends; `run` then
  // does what the person would do there, with the owning script's api in hand.
  var IN_EDITOR = ['editor on', 'render close'];
  var ACTIONS = [
    { owner: 'Camera Editor', api: 'CamEditor', items: [
      ['Customize Player', IN_EDITOR, function (a) { a.openCustomize(); }],
      ['Close Customize', IN_EDITOR, function (a) { a.closeCustomize(); }],
      ['Editor Settings (gear)', IN_EDITOR, function (a) { a.toggleSettings(); }],
      ['Skin picker', IN_EDITOR, function (a) { a.openCustomize(); after(400, function () { a.customizeOpenPicker('primary'); }); }],
      // Agent, not wear. There is no wear PICKER to open -- wear is a slider on the row
      // (wearSliderBlock in CameraEditorCustomizeJs.h), and api.customizeOpenWear only
      // repopulates, so that button was indistinguishable from 'Customize Player'.
      ['Agent picker', IN_EDITOR, function (a) { a.openCustomize(); after(400, function () { a.customizeOpenPicker('agent'); }); }],
      ['Confirm: Clear all paths', IN_EDITOR, function () { activateByText('Clear all'); }],
      ['Confirm: Customize reset', IN_EDITOR, function (a) { a.openCustomize(); after(450, function () { activateByText('RESET TO DEFAULT'); }); }],
      // The RENDER button just issues this command; the workspace opens off st.render.open in the
      // next state frame. Issuing the command is the real path.
      ['Render workspace', [], function () { window.PanoramaShim.onCommand('mirv_filmmaker editor render'); }],
      ['Render tab: RANGE & CAMERA', ['editor render'], function () { window.PanoramaShim.onCommand('mirv_filmmaker render tab 0'); }],
      ['Render tab: PASSES', ['editor render'], function () { window.PanoramaShim.onCommand('mirv_filmmaker render tab 2'); }],
      ['Render tab: OUTPUT', ['editor render'], function () { window.PanoramaShim.onCommand('mirv_filmmaker render tab 3'); }],
      ['Inspector: Path', IN_EDITOR, function (a) { a.setInspectorMode('path'); }],
      ['Inspector: Follow', IN_EDITOR, function (a) { a.setInspectorMode('follow'); }],
      ['Inspector: Scene Keys', IN_EDITOR, function (a) { a.gotoInspector('scene'); }],
      ['Attachment menu', IN_EDITOR, function (a) { a.setAttachmentMenuOpen(true); }],
      ['Dock: REGULAR TIMELINE', ['editor on'], function () { window.PanoramaShim.onCommand('mirv_filmmaker dock native'); }],
      ['Dock: TIMELINE', ['editor on'], function () { window.PanoramaShim.onCommand('mirv_filmmaker dock timeline'); }],
      ['Editor: ESC EXIT', [], function () { window.PanoramaShim.onCommand('mirv_filmmaker editor close'); }]
    ] },
    { owner: 'Config', api: 'ConfigHud', items: [
      // 'RESET ' matched "Reset Pivots" -- the Weapon Recoil / Weapon Bob button, which sits
      // ABOVE the footer in the panel list, so both of these pressed it and actually reset the
      // pivots while the footer stayed idle. The footer reads 'RESET ' + section name
      // (ConfigHudWidgetsJs.h), so name the section to land on it.
      // ONE click, like every other Reset in the panel (ConfigHudWidgetsJs.h resetFooter: the
      // arm-then-confirm idiom was dropped because the second click kept landing after the
      // auto-disarm). Firing it runs the reset; there is no armed state to photograph any more.
      ['Reset footer (RESET CAMERA)', ['config on'], function () { activateByText('RESET CAMERA'); }],
      // RAGDOLLS > Advanced Ragdolls > PER-PLAYER ANIMATIONS, the button's own handler. The clip
      // case selects the first listed clip once the catalog has loaded, as a click on its row does.
      ['Per-player death animations', ['config on'], function (a) {
        a.gotoTab('ragdolls'); after(300, function () { activateByText('PER-PLAYER ANIMATIONS'); });
      }],
      ['Death clip preview', ['config on'], function (a) {
        a.gotoTab('ragdolls');
        after(300, function () {
          activateByText('PER-PLAYER ANIMATIONS');
          window.MvmPreview3D.loadPacks().then(function (lib) {
            after(200, function () { if (window.$.ConfigHudDap) window.$.ConfigHudDap.select(lib.clips[0].name); });
          });
        });
      }],
      ['Config: ESC EXIT', [], function () { window.PanoramaShim.onCommand('mirv_filmmaker config close'); }]
    ] },
    { owner: 'Camera Timeline', api: 'CamTimeline', items: [
      ['Shake menu (right-click a key)', ['editor off', 'config off', 'camtl open'], function () { return fireEventOn('oncontextmenu', 0, 'CamTimelineRoot'); }],
      ['Clear path', ['editor off', 'config off', 'camtl open'], function () { activateByText('Clear'); }],
      // The floating bar over the HUD with no editor around it: `camtl open` from the console.
      ['Floating timeline (no editor)', ['editor off', 'config off', 'camtl open'], null]
    ] },
    { owner: 'HUD', api: null, items: [
      ['Director card (hud toggle)', [], function () { window.PanoramaShim.onCommand('mirv_filmmaker hud toggle'); }],
      ['Blackout (F8)', [], function () { window.PanoramaShim.onCommand('mirv_filmmaker blackout toggle'); }],
      ['Aim crosshair', [], function () { state.aim.on = !state.aim.on; }],
      ['FX debug squares', [], function () { window.PanoramaShim.onCommand('mirv_filmmaker fx debughud ' + (state.fxdbg.enabled ? 'off' : 'on')); }]
    ] },
    { owner: 'Easter eggs', api: 'MvmEggs', items: [
      ['Hint (faint)', [], function () { window.PanoramaShim.onCommand('mirv_filmmaker egg hint 0 faint'); }],
      ['Hint (loud)', [], function () { window.PanoramaShim.onCommand('mirv_filmmaker egg hint 1 loud'); }],
      ['E-girl mode', [], function () { window.PanoramaShim.onCommand('mirv_filmmaker egg egirl toggle'); }],
      ['Sony Vegas intervention', [], function () { window.PanoramaShim.onCommand('mirv_filmmaker egg vegas test'); }],
      ['Clippy', [], function () { window.PanoramaShim.onCommand('mirv_filmmaker egg clippy show'); }],
      ['Onboarding tutorial (Clippy)', [], function () { window.PanoramaShim.onCommand('mirv_filmmaker onboarding restart'); }],
      ['Disco inferno', [], function () { window.PanoramaShim.onCommand('mirv_filmmaker egg disco toggle'); }],
      ['Michael Bay blast', [], function () { window.PanoramaShim.onCommand('mirv_filmmaker egg michaelbay'); }],
      ['Wes Anderson (+ next chapter)', [], function () { window.PanoramaShim.onCommand(state.eggs.wes.on ? 'mirv_filmmaker egg chapter' : 'mirv_filmmaker egg wes on'); }]
    ] },
    { owner: 'Menu context', api: null, items: [
      ['Demos tab', [], function () { jumpTo('filmmakergui'); }],
      ['Open dialog: Custom Resolution', [], function () { jumpTo('videoresolution', function () { var a = window.$.MvmResolutionSettings; if (a && a.open) a.open(); }); }]
    ] }
  ];

  // State presets. Some overlays are not opened by a control at all -- they are a STATE the C++
  // side reports (a transient notice, a reposition prompt, a running render job), so they are
  // reproduced by patching the state feed exactly the way the game would. Keys are the model's
  // namespaces, so each lands on the owner that reads it.
  var SCENARIOS = [
    { owner: 'Marker HUD', items: [
      ['Notice: Need at least 2 markers', { marker: { mode: 'editing', menuOpen: false, notice: 'Need at least 2 camera markers to play path.' } }],
      ['Notice: path cleared', { marker: { mode: 'editing', menuOpen: false, notice: 'Camera path cleared.' } }],
      ['Banner: repositioning', { marker: { mode: 'reposition', notice: '' }, selected: 2 }],
      ['Card: editing (default)', { marker: { mode: 'editing', menuOpen: true, notice: '' } }],
      ['Preview armed (card hides)', { marker: { mode: 'previewArmed', notice: '' } }]
    ] },
    { owner: 'Movie HUD', items: [
      ['Mode: editing', { movie: { mode: 'editing', visible: true } }],
      ['Mode: reposition', { movie: { mode: 'reposition', visible: true } }],
      ['Hidden', { movie: { visible: false } }]
    ] },
    { owner: 'Camera Editor', items: [
      ['Render job RUNNING', { renderJobActive: true, render: { captureHide: false, jobActive: true } }],
      ['Render job idle', { renderJobActive: false, render: { captureHide: false, jobActive: false } }],
      ['Capture: chrome hidden', { render: { captureHide: true, jobActive: true } }],
      ['No POV (free roam)', { povReady: false }],
      ['Mouse: GAME', { cursor: false }]
    ] },
    { owner: 'Config', items: [
      ['Effects still loading', { fxOn: true, fxReady: false }],
      ['Effects master OFF', { fxOn: false }],
      ['Live map (not a demo)', { demo: false }]
    ] },
    { owner: 'Camera Timeline', items: [
      ['Loop off', { loopOn: false }],
      ['No camera keys', { markers: [], count: 0, selCount: 0, selected: -1 }],
      ['Path playing live', { pathLive: true, playing: true }]
    ] }
  ];

  var stateUndo = {};
  function buildActions() {
    var host = document.getElementById('actions');
    host.innerHTML = '';
    function label(text, cls) {
      var h = document.createElement('div');
      h.className = cls || 'group-label';
      h.textContent = text;
      host.appendChild(h);
    }
    label('Popups / overlays');
    ACTIONS.forEach(function (grp) {
      label(grp.owner, 'action-owner');
      grp.items.forEach(function (a) {
        var b = document.createElement('button');
        b.className = 'action-btn';
        b.textContent = a[0];
        b.onclick = function () {
          b.blur();
          try {
            cmds(a[1] || []);
            Mockup.tick(2);
            if (!a[2]) return;
            var apiName = grp.api;
            // The owning script may have just been built: wait for its api, then act.
            retry(function () {
              var api = apiName ? window.$[apiName] : null;
              if (apiName && !api) return false;
              a[2](api);
              return true;
            }, null, 8);
          } catch (e) { pushDev('log', '[action] ' + e); }
        };
        host.appendChild(b);
      });
    });
    // States are cumulative and each one is a toggle: a second click restores exactly the fields
    // it changed, and Reset restores all of them. They used to merge silently, which read as one
    // state undoing another whenever two touched the same field.
    label('States  (combine; click again to undo)');
    stateUndo = {};
    SCENARIOS.forEach(function (grp) {
      label(grp.owner, 'action-owner');
      grp.items.forEach(function (a) {
        var b = document.createElement('button');
        b.className = 'action-btn'; b.textContent = a[0];
        b.onclick = function () {
          b.blur();
          if (!state) return;
          if (stateUndo[a[0]]) { mergePatch(state, stateUndo[a[0]]); delete stateUndo[a[0]]; b.classList.remove('on'); return; }
          stateUndo[a[0]] = snapshotFor(state, a[1]);
          mergePatch(state, a[1]); b.classList.add('on');
        };
        host.appendChild(b);
      });
    });
    var r = document.createElement('button');
    r.className = 'action-btn'; r.textContent = 'Reset states';
    r.onclick = function () {
      r.blur();
      Object.keys(stateUndo).reverse().forEach(function (k) { mergePatch(state, stateUndo[k]); });
      stateUndo = {};
      [].forEach.call(host.querySelectorAll('.action-btn.on'), function (x) { x.classList.remove('on'); });
    };
    host.appendChild(r);
  }

  // ---------------------------------------------------------------- sidebar

  function buildSidebar() {
    var ctxHost = document.getElementById('contexts');
    ctxHost.innerHTML = '';
    var h = document.createElement('div');
    h.className = 'group-label'; h.textContent = 'Context';
    ctxHost.appendChild(h);
    Object.keys(CONTEXTS).forEach(function (name) {
      var c = CONTEXTS[name];
      var b = document.createElement('button');
      b.className = 'ctx-btn'; b.dataset.ctx = name;
      b.innerHTML = escapeHtml(c.label) + '<span class="note">' + escapeHtml(c.note) + '</span>';
      b.onclick = function () { b.blur(); lastJump = null; enterContext(name); };
      ctxHost.appendChild(b);
    });

    buildActions();

    var list = document.getElementById('surfacelist');
    list.innerHTML = '';
    var groups = [['ingame', 'In-game'], ['menu', 'Menu context']];
    groups.forEach(function (g) {
      var items = manifest.filter(function (m) { return m.group === g[0]; });
      if (!items.length) return;
      var gh = document.createElement('div');
      gh.className = 'action-owner';
      gh.textContent = g[1];
      list.appendChild(gh);
      items.forEach(function (m) {
        var b = document.createElement('button');
        b.className = 'surface-btn';
        b.innerHTML = '<span class="lines">' + m.lines + '</span>' + escapeHtml(m.label) +
          '<span class="note">' + escapeHtml(m.note || '') + '</span>';
        b.onclick = function () { b.blur(); jumpTo(m.id); };
        b.__entry = m;
        list.appendChild(b);
      });
    });
  }

  // ---------------------------------------------------------------- controls

  function wireControls() {
    document.getElementById('btn-play').onclick = function () {
      if (!state) return;
      window.PanoramaShim.onCommand(state.paused ? 'demo_resume' : 'demo_pause');
      this.classList.toggle('on', !state.paused);
      this.textContent = state.paused ? 'Play' : 'Pause';
    };
    document.getElementById('btn-reload').onclick = function () { reloadContext(); };
    document.getElementById('btn-dev').onclick = function () {
      var p = document.getElementById('devpanel');
      p.classList.toggle('open');
      this.classList.toggle('on', p.classList.contains('open'));
      fitStage();
    };
    document.getElementById('zoom-fit').onclick = function () { setZoom('fit', 'zoom-fit'); };
    document.getElementById('zoom-1').onclick = function () { setZoom(1, 'zoom-1'); };
    document.getElementById('zoom-2').onclick = function () { setZoom(2, 'zoom-2'); };
    document.getElementById('opt-native').onchange = function () { reloadContext(); };
    document.getElementById('opt-cursor').onchange = function () { if (state) state.cursor = this.checked; };
    document.getElementById('opt-demo').onchange = function () { if (state) state.demo = this.checked; };
    document.getElementById('opt-grid').onchange = function () {
      document.getElementById('backdrop').style.opacity = this.checked ? '1' : '0.25';
    };

    [].forEach.call(document.querySelectorAll('#devtabs button'), function (b) {
      b.onclick = function () {
        [].forEach.call(document.querySelectorAll('#devtabs button'), function (x) { x.classList.remove('active'); });
        [].forEach.call(document.querySelectorAll('.devpane'), function (x) { x.classList.remove('active'); });
        b.classList.add('active');
        document.getElementById('pane-' + b.dataset.pane).classList.add('active');
        renderDev(b.dataset.pane);
      };
    });
  }

  // Rebuild the current context from the sources on disk, keeping the model (so an open workspace
  // comes back open, a shut section stays shut).
  async function reloadContext() {
    manifest = await (await fetch('generated/manifest.json?t=' + Date.now())).json();
    window.MvmCommands.load(await (await fetch('generated/bindings.json?t=' + Date.now())).json());
    var ids = Object.keys(sources);
    for (var i = 0; i < ids.length; i++) await fetchSource(ids[i]);
    enterContext(state.nav.context);
  }
  Mockup.reload = reloadContext;

  async function fetchSource(id) {
    var m = byId(id);
    if (!m || !m.src) return false;
    var res = await fetch(m.src + '?t=' + Date.now());
    if (!res.ok) throw new Error(m.src + ': ' + res.status + ' ' + res.statusText);
    sources[id] = await res.text();
    return true;
  }

  // ---------------------------------------------------------------- source watch
  // Editing generated/<surface>.js is the round trip's first half (apply-ui.mjs is the second), so
  // the page rebuilds the context when one of its scripts changes on disk. Polling HEAD is
  // enough: the dev server stamps Last-Modified, and a 1.5s cadence against a local file is free.
  // ?watch=0 turns it off, which the headless capture does not need but a debugger might.
  var watchStamp = {};
  function watchSources() {
    if (/[?&]watch=0/.test(location.search)) return;
    setInterval(function () {
      var srcs = Object.keys(sources).map(function (id) { return byId(id).src; });
      srcs.push('generated/manifest.json', 'generated/bindings.json');
      var pending = srcs.length, changed = false;
      srcs.forEach(function (src) {
        fetch(src, { method: 'HEAD', cache: 'no-store' }).then(function (r) {
          var lm = r.headers.get('etag') || r.headers.get('last-modified') || '';
          if (watchStamp[src] && lm && watchStamp[src] !== lm) changed = true;
          if (lm) watchStamp[src] = lm;
        }).catch(function () {}).then(function () {
          if (--pending === 0 && changed) {
            reloadContext().then(function () { pushDev('log', '[watch] source changed on disk -- context rebuilt'); });
          }
        });
      });
    }, 1500);
  }

  // Browsers block autoplay while a tab is hidden or before the page has been interacted with, so
  // the animated panel backgrounds can load and then sit on their first frame. Nudge them whenever
  // the page becomes visible or the user clicks, which covers both cases.
  function nudgeVideos() {
    [].forEach.call(document.querySelectorAll('#hostlayer video'), function (v) {
      if (!v.paused) return;
      var p = v.play();
      if (p && p.catch) p.catch(function () { /* still blocked; will retry on the next event */ });
    });
  }
  document.addEventListener('visibilitychange', function () { if (!document.hidden) nudgeVideos(); });
  window.addEventListener('pointerdown', nudgeVideos);
  setInterval(function () { if (!document.hidden) nudgeVideos(); }, 2000);

  // ---------------------------------------------------------------- snapshot seeding
  // A real state feed captured out of a running demo (UiMockup/import-state.mjs) beats the
  // hand-written stand-in on every field it covers: slider values, section open/shut, the
  // selected key, the player list. The capture merges every root's feed into one object, so the
  // colliding keys are routed back to the owner they came from -- and nothing in it decides what
  // is OPEN at boot: the mockup boots closed, like the game.
  var MARKER_MODES = { editing: 1, reposition: 1, previewArmed: 1, previewPlaying: 1 };
  var AIM_KEYS = ['on', 'valid', 'front', 'sx', 'sy', 'size', 'gap', 'thick', 'col', 'dot', 'centre', 'view', 'debugbg'];
  var SKIP_KEYS = { enabled: 1, hosted: 1, configOpen: 1, blackout: 1, open: 1, visible: 1, graphExp: 1,
                    renderOpen: 1, timelineView: 1, mode: 1, menuOpen: 1, notice: 1, bottomMode: 1, dbg: 1 };
  function seedFromSnapshot(model, snap) {
    Object.keys(snap).forEach(function (k) {
      if (SKIP_KEYS[k] || AIM_KEYS.indexOf(k) >= 0 || NAMESPACES.indexOf(k) >= 0) return;
      if (k === 'render' && snap.render && typeof snap.render === 'object') {
        mergePatch(model, { render: Object.assign({}, snap.render, { open: false }) });
        return;
      }
      model[k] = snap[k];
    });
    if (typeof snap.mode === 'string') {
      if (MARKER_MODES[snap.mode]) model.marker.mode = snap.mode; else model.movie.mode = snap.mode;
    }
    if (typeof snap.menuOpen === 'boolean') model.marker.menuOpen = snap.menuOpen;
    if (typeof snap.notice === 'string') model.marker.notice = snap.notice;
    // The dock tab is NOT taken from the capture: it is a moment mid-session, and the page boots
    // the way a fresh launch does -- Regular Timeline until a tab is picked.
    AIM_KEYS.forEach(function (k) { if (snap[k] !== undefined && k !== 'on') model.aim[k] = snap[k]; });
    // Whatever the capture had open, the model's own render/tab state must be consistent.
    model.render.open = false;
  }

  // ---------------------------------------------------------------- boot

  (async function boot() {
    await window.MvmNativeMenu.ready();
    document.getElementById('menu-workshop').onclick = function () { enterContext('menu-workshop'); };
    document.getElementById('menu-downloaded').onclick = function () { enterContext('menu-demos'); };
    document.getElementById('menu-reset').onclick = function () { window.MvmNativeMenu.reset(); reloadContext(); };
    document.getElementById('menu-aspect').onchange = function () { if (state.nav) enterContext(state.nav.context); };
    try {
      manifest = await (await fetch('generated/manifest.json?t=' + Date.now())).json();
      var bindings = await (await fetch('generated/bindings.json?t=' + Date.now())).json();
      window.MvmCommands.load(bindings);
    } catch (e) {
      showError('Could not load generated/ files. Run:\n\n  node UiMockup/extract-ui.mjs\n  node UiMockup/derive-bindings.mjs\n\n' + e);
      return;
    }
    if (!/[?&]live=0\b/.test(location.search)) {
      try {
        var snapRes = await fetch('generated/state-snapshot.json?t=' + Date.now());
        if (snapRes.ok) liveState = await snapRes.json();
      } catch (e) { /* no capture yet -- the stand-in state is the normal case */ }
    }
    // A real CS2 frame behind the panels, if one has been captured. It matters for more than
    // looks: every surface in this UI is translucent over the game, so judging a colour or a
    // scrim against a flat gradient gives the wrong answer.
    try {
      var shot = await fetch('assets/backdrop.jpg', { method: 'HEAD' });
      if (shot.ok) {
        var bd = document.getElementById('backdrop');
        // A url() inside a custom property is resolved against the STYLESHEET, not the document,
        // so a relative path here becomes /shim/assets/... and 404s. Hand it an absolute one.
        var abs = new URL('assets/backdrop.jpg', location.href).href;
        bd.style.setProperty('--shot', 'url("' + abs + '")');
        bd.classList.add('has-image');
      }
    } catch (e) { /* gradient stand-in */ }

    state = window.MvmState.make();
    if (liveState) seedFromSnapshot(state, liveState);

    // Every script once, up front, so a workspace builds synchronously the frame its flag turns
    // on -- the way the C++ has the header string at hand.
    var ids = manifest.filter(function (m) { return m.src && !m.variantOf; }).map(function (m) { return m.id; });
    try {
      await Promise.all(ids.map(fetchSource));
    } catch (e) {
      showError('Failed to fetch a surface script' + String.fromCharCode(10) + e);
      return;
    }

    window.PanoramaShim.onCommand = function (c) {
      pushDev('cmd', c);
      var ok = window.MvmCommands.run(state, c);
      if (!ok) pushDev('cmd', '   (unrouted)');
      // A navigation verb moves you: the place you jumped to no longer names where you are, and
      // current() derives it from the model again (a jump re-sets it after its own commands).
      if (/^mirv_filmmaker (config|editor|render|dock|hud|blackout|grapheditor (on|off)|camtl (open|close|toggle))\b/.test(c)) lastJump = null;
    };

    buildSidebar();
    wireControls();
    wireMouse();
    wireKeys();
    watchSources();
    fitStage();
    // Boot the way the game does after a demo loads: the in-game context, nothing open.
    enterContext('ingame');
    running = true;
    lastFrame = performance.now();
    schedule();
  })();
})();
