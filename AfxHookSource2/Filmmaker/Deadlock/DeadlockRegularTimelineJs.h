#pragma once

// THE REGULAR TIMELINE: the replay transport, drawn by us, driven by the game.
//
// WHY THIS DRAWS THE TRANSPORT AGAIN.
//
// The previous version drew nothing. Its whole job was to host Deadlock's own `#hud_replay` in the
// dock band -- un-hide it, widen it, measure it -- on the principle that "Regular Timeline" should
// mean the game's timeline rather than a lookalike. That principle is right. The implementation
// could not work, and here is the measurement that settled it, taken in game on a loaded replay:
//
//   * the bar and every control in it report `visible`, with no `Hidden` class anywhere on the
//     chain, no opacity, and no transform on the bar or any ancestor;
//   * with the bar widened, `#PlaybackControls` reports its box as 0,0 1920x60 -- the full band;
//   * a test background painted onto that panel appears ONLY across its right-hand ~500px, which
//     is exactly the extent of the button group we append to it;
//   * `overflow: noclip` on the bar and all three ancestors changes nothing;
//   * suppressing the preview blit entirely (`mirv_filmmaker blit off`) changes nothing, so it is
//     not the scaled preview painting over it;
//   * with the editor closed and the game left alone, the bar is not on screen either.
//
// A panel that reports a 1920px box and paints 500px of its own background is not being hidden or
// covered. `CitadelHudReplay` is an engine C++ panel, and it renders from something our inline
// width does not move. Every remaining avenue is a guess at Valve's internals.
//
// So: draw the transport, and DELEGATE THE ACTIONS TO THE GAME. Each button dispatches `Activated`
// on the game's own corresponding button, which runs `CitadelReplaySkipByTime` / `CitadelReplay-
// NextMarker` in the panel context those functions are actually bound in -- our Panorama context
// has no such bindings at all. When a native button is missing, the console command takes over, so
// the control still does something rather than going quietly dead.
//
// This follows the original dock design; see Docs/deadlock/ui-style.md and Docs/deadlock/README.md.
// Its Regular Timeline tab was a scrub strip and our own controls, describing the game's bar as
// "JS-reachable, so speed, skips and marker stepping call its functions rather than
// ours". Calling its functions is not the same as hosting its panel.
//
// The native bar stays hidden, so there is never a second transport underneath this one.

#define DEADLOCK_REGULAR_TIMELINE_JS R"CFJS(
(function () {
  if ($.MvmRegular) return;
  var M = $.MvmStyle;
  if (!M) return;
  var S = M.S;

  // Dock metrics, from the original design: the Regular Timeline band is 130px total and the ribbon takes
  // 36 of it. Ours now, not the game bar's measured height -- we draw the contents, so we are the
  // only thing that knows how tall they are.
  var BAND_H = 94;

  // The four workspace buttons' own fill. From the spec, and deliberately not S.border / the bevel
  // gradient: see wsButton.
  var WS_BG = '#576886bb', WS_BG_HOT = '#6a7d9ecc', WS_BG_LOOP = '#3f7a5fcc';

  var wrap = null, notice = null, row = null;
  var scrubRow = null, scrubSlider = null, track = null, fill = null, head = null;
  var loopBand = null, flagIn = null, flagOut = null;
  var pips = null;
  var playBtn = null, timeLabel = null;
  var markerBar = null, markerLabel = null;
  var speedDial = null;
  var geom = { x: 0, y: 0, w: 0, h: 0 };
  var shown = false;
  var lastReportedH = 0;
  var scrubbing = false;
  // Panorama fires onvaluechanged for programmatic writes too. Without this guard, the per-frame
  // playhead sync below emitted a demo seek every frame; playback then fought those seeks and
  // visibly jumped backward and forward as soon as the editor opened.
  var scrubApplying = false;

  // The scrubber's own span, so a click maps to the tick the label is showing.
  var spanFirst = 0, spanLast = 1;

  function cmd(text) { M.cmd(text); }

  // Looked up by id every time rather than cached. The replay HUD is rebuilt on level load and on
  // a Panorama reload, and a cached handle to a deleted panel reads as "the bar stopped working
  // after I loaded a second replay".
  function nat(id) {
    try {
      var ctx = $.GetContextPanel();
      if (!ctx) return null;
      var p = ctx.FindChildTraverse(id);
      if (!p) return null;
      if (p.IsValid && !p.IsValid()) return null;
      return p;
    } catch (e) { return null; }
  }

  // THE GAME'S OWN HANDLER, RUN IN THE GAME'S OWN CONTEXT.
  //
  // `CitadelReplaySkipByTime` and friends are bound per-XML-context by the panel that declares
  // them and are undefined as globals here -- measured by enumerating the global object from a
  // live RunScript. Dispatching the activation onto the game's button runs the expression the
  // game attached to it, in the context it was attached in, which is the only route there is.
  //
  // The third argument is not optional: without 'mouse' the event is constructed but onactivate
  // never fires, and the control reads as dead while looking perfectly alive.
  function activateNative(id) {
    var p = nat(id);
    if (!p) return false;
    try { $.DispatchEvent('Activated', p, 'mouse'); return true; } catch (e) { return false; }
  }

  // THE REPLAY'S SPAN, READ OFF THE GAME'S OWN SCRUBBER.
  //
  // Nothing in the state feed carries it: `startTick`/`endTick` there are the camera SHOT's span,
  // which is a different thing and is usually a single tick. The engine exposes no replay length
  // either. But the game's own `#TimeScrubSlider` is calibrated to the whole replay, and it is in
  // the tree next to the marker list we already read -- so the strip is scaled by the same number
  // the game scales its own by, rather than by a guess that would drift as the replay ran on.
  function nativeSpan() {
    var sl = nat('TimeScrubSlider');
    if (!sl) return null;
    try {
      var lo = sl.min, hi = sl.max;
      if (typeof lo !== 'number' || typeof hi !== 'number' || !(hi > lo)) return null;
      return { first: lo, last: hi };
    } catch (e) { return null; }
  }

  // Each control names the native button it prefers and the command it falls back to.
  function delegate(nativeId, fallbackCmd) {
    if (activateNative(nativeId)) return;
    if (fallbackCmd) cmd(fallbackCmd);
  }
)CFJS" R"CFJS(
  // ---- build ----------------------------------------------------------------------------------

  // DEADLOCK'S .ButtonBevel, to the spec's numbers: 8px radius, a vertical gradient, letter-spaced
  // uppercase, and a hard 2px black text shadow. The 6px radius and 1px tracking this used to carry
  // were close enough to look like the same button family and far enough to look wrong beside the
  // game's own chrome.
  function bevelButton(parent, id, text, widthPx, onActivate) {
    var b = M.make(parent, 'Panel', id, {
      backgroundColor: 'gradient( linear, 0% 0%, 0% 100%, from( ' + S.btnTop +
                       ' ), to( ' + S.btnBottom + ' ) )',
      border: '1px solid ' + S.border,
      borderRadius: '8px',
      padding: '7px 10px',
      marginRight: '5px',
      verticalAlign: 'middle'
    });
    if (!b) return null;
    if (widthPx) M.set(b, { width: widthPx + 'px', padding: '7px 0px' });
    // THE LABEL ONLY FILLS THE BUTTON WHEN THE BUTTON HAS A WIDTH OF ITS OWN.
    //
    // A button with no explicit width sizes to its children, so a child asking for 100% of it
    // resolves against a parent that is waiting on the child -- and Panorama settles that at zero.
    // The buttons came out as empty grey boxes: the right size, the right border, no text.
    var l = M.label(b, '', text, {
      fontSize: '12px', letterSpacing: '1px', color: S.text,
      textTransform: 'uppercase', textShadow: '2px 2px 0px 1.0 #000000',
      whiteSpace: 'nowrap',
      horizontalAlign: 'center', verticalAlign: 'middle'
    });
    if (widthPx && l) M.set(l, { width: '100%' });
    b.__lbl = l;
    try {
      b.SetPanelEvent('onactivate', onActivate);
      b.SetPanelEvent('onmouseover', function () {
        M.set(b, { backgroundColor: 'gradient( linear, 0% 0%, 0% 100%, from( ' +
                 S.btnTopHover + ' ), to( ' + S.btnBottomHover + ' ) )' });
      });
      b.SetPanelEvent('onmouseout', function () {
        M.set(b, { backgroundColor: 'gradient( linear, 0% 0%, 0% 100%, from( ' +
                 S.btnTop + ' ), to( ' + S.btnBottom + ' ) )' });
      });
    } catch (e) {}
    return b;
  }

  // AN ICON BUTTON, not a glyph in a label.
  //
  // Panorama draws an icon as a panel's background image; there is no <img> and no icon font. The
  // image is sized with `contain` so a 18px source and a 24px one land the same, and the panel
  // keeps its own box so the row's rhythm does not depend on a glyph's side bearings.
  function iconButton(parent2, id, image, widthPx, onActivate) {
    var b = bevelButton(parent2, id, '', widthPx, onActivate);
    if (!b) return null;
    if (b.__lbl) { try { b.__lbl.visible = false; } catch (e) {} }
    var ico = M.make(b, 'Panel', '', {
      width: '20px', height: '20px',
      backgroundImage: 'url("' + image + '")',
      backgroundSize: 'contain',
      backgroundRepeat: 'no-repeat',
      backgroundPosition: 'center',
      horizontalAlign: 'center', verticalAlign: 'middle'
    });
    if (ico) ico.hittest = false;
    b.__ico = ico;
    return b;
  }

  function buildScrub(parent) {
    scrubRow = M.make(parent, 'Panel', 'MvmRegScrub', { width: '100%', height: '26px' });
    if (!scrubRow) return;

    // The track, its progress fill and the loop region, bottom-aligned so the flags and pips have
    // room above them -- the same stack the original design drew.
    track = M.make(scrubRow, 'Panel', '', {
      width: '100%', height: '6px', borderRadius: '3px',
      backgroundColor: S.offBlack, border: '1px solid ' + S.border,
      horizontalAlign: 'left', verticalAlign: 'bottom', marginBottom: '6px'
    });
    if (track) track.hittest = false;
    fill = M.make(track, 'Panel', '', {
      height: '100%', width: '0%', borderRadius: '3px',
      backgroundColor: S.green, horizontalAlign: 'left'
    });
    if (fill) fill.hittest = false;

    loopBand = M.make(scrubRow, 'Panel', '', {
      height: '6px', backgroundColor: 'rgba(95,230,158,0.24)',
      horizontalAlign: 'left', verticalAlign: 'bottom', marginBottom: '6px'
    });
    if (loopBand) { loopBand.hittest = false; loopBand.visible = false; }

    // Event pips. The replay's marker list never reaches our state feed -- but the game's own bar
    // has it, as one child per event named `<type>_event_<tick>`, and that panel is still in the
    // tree even though it does not paint. So the ticks are READ from there rather than invented,
    // and drawn here where they can be seen.
    pips = [];

    function flag(colour, cap) {
      var f = M.make(scrubRow, 'Panel', '', {
        width: '16px', height: '24px', horizontalAlign: 'left', verticalAlign: 'bottom'
      });
      if (!f) return null;
      f.hittest = false;
      f.visible = false;
      M.make(f, 'Panel', '', {
        width: '2px', height: '24px', backgroundColor: colour,
        horizontalAlign: 'left', verticalAlign: 'bottom', marginLeft: '7px'
      });
      M.make(f, 'Panel', '', {
        width: '8px', height: '8px', backgroundColor: colour,
        horizontalAlign: 'left', verticalAlign: 'bottom', marginBottom: '16px'
      });
      M.label(f, '', cap, {
        fontSize: '10px', letterSpacing: '1px', color: colour,
        horizontalAlign: 'left', verticalAlign: 'bottom'
      });
      return f;
    }
    flagIn = flag(S.gold, 'IN');
    flagOut = flag(S.gold, 'OUT');

    head = M.make(scrubRow, 'Panel', '', {
      width: '4px', height: '14px', backgroundColor: S.text,
      horizontalAlign: 'left', verticalAlign: 'bottom', marginBottom: '2px'
    });
    if (head) head.hittest = false;

    // THE DRAG IS A SLIDER, because this Panorama context has no mouse-move event at all and a
    // hand-rolled drag would have nothing to listen to. It is stretched over the whole strip and
    // left invisible; the track above is what you see.
    scrubSlider = M.make(scrubRow, 'Slider', 'MvmRegScrubSlider', {
      width: '100%', height: '100%', opacity: '0.0',
      position: '0px 0px 0px', horizontalAlign: 'left', verticalAlign: 'top'
    });
    if (scrubSlider) {
      try {
        scrubSlider.min = 0; scrubSlider.max = 1; scrubSlider.value = 0;
        scrubSlider.SetPanelEvent('onvaluechanged', function () {
          if (scrubApplying || !scrubbing) return;
          var t = Math.round(spanFirst + scrubSlider.value * (spanLast - spanFirst));
          cmd('mirv_filmmaker seek ' + t);
        });
        scrubSlider.SetPanelEvent('onmouseactivate', function () { scrubbing = true; });
        scrubSlider.SetPanelEvent('onmouseup', function () { scrubbing = false; });
        scrubSlider.SetPanelEvent('onblur', function () { scrubbing = false; });
      } catch (e) {}
    }
  }
)CFJS" R"CFJS(
  function buildControls(parent) {
    row = M.make(parent, 'Panel', 'MvmRegControls', {
      width: '100%', flowChildren: 'right', marginTop: '7px'
    });
    if (!row) return;

    playBtn = iconButton(row, 'MvmRegPlay', S.icoReplayPlay, 46, function () {
      // The game's own play button first: it knows whether the replay is paused, and it keeps the
      // bar's own state in step. `mirv_filmmaker play` is the fallback and does the same thing
      // through the demo player.
      delegate('PlayButton', 'mirv_filmmaker play');
    });

    // The skips keep their numerals. They are a measurement, not an icon -- the game labels its own
    // the same way, on hover -- and ±10 / ±60 is the whole point of the control.
    bevelButton(row, 'MvmRegBack60', '-60', 0, function () {
      delegate('SkipBackward2Button', 'mirv_filmmaker seekrel -3840');
    });
    bevelButton(row, 'MvmRegBack10', '-10', 0, function () {
      delegate('SkipBackward1Button', 'mirv_filmmaker seekrel -640');
    });
    bevelButton(row, 'MvmRegFwd10', '+10', 0, function () {
      delegate('SkipForward1Button', 'mirv_filmmaker seekrel 640');
    });
    bevelButton(row, 'MvmRegFwd60', '+60', 0, function () {
      delegate('SkipForward2Button', 'mirv_filmmaker seekrel 3840');
    });

    // POSITION OVER LENGTH, AS A CLOCK -- and no tick number.
    //
    // The tick belongs to the camera dock, where you place markers on it and the number is the thing
    // you act on. On the playback bar it is a second reading of the same instant in a unit nobody
    // scrubs by, and carrying it here forced the field out to 216px of mostly-empty right margin.
    timeLabel = M.label(row, 'MvmRegTime', '', {
      width: '140px', fontFamily: S.fontMono, fontSize: '15px', color: S.text,
      textAlign: 'right', verticalAlign: 'middle', marginRight: '16px',
      whiteSpace: 'nowrap', textOverflow: 'ellipsis'
    });

    // The marker stepper, with the game's own marker-type dropdown named but not reimplemented:
    // the list belongs to the replay and we have no route to its contents.
    iconButton(row, 'MvmRegPrevMarker', S.icoArrowLeft, 34, function () {
      delegate('PrevMarkerButton', null);
    });
    markerBar = M.make(row, 'Panel', 'MvmRegMarkerType', {
      width: '132px', height: '28px', flowChildren: 'right', padding: '0px 10px',
      borderRadius: '6px', backgroundColor: S.offBlack, border: '1px solid ' + S.border,
      marginRight: '5px', verticalAlign: 'middle'
    });
    markerLabel = M.label(markerBar, '', 'KILL & DEATH', {
      width: 'fill-parent-flow(1.0)', fontSize: '12px', letterSpacing: '1px',
      color: S.text, verticalAlign: 'middle'
    });
    M.make(markerBar, 'Panel', '', {
      width: '10px', height: '10px',
      backgroundImage: 'url("' + S.icoArrowDown + '")',
      backgroundSize: 'contain', backgroundRepeat: 'no-repeat', backgroundPosition: 'center',
      verticalAlign: 'middle'
    });
    try {
      markerBar.SetPanelEvent('onactivate', function () {
        // The game's own dropdown, opened in its own context.
        activateNative('MarkerTypeDropdown');
      });
    } catch (e) {}
    iconButton(row, 'MvmRegNextMarker', S.icoArrowRight, 34, function () {
      delegate('NextMarkerButton', null);
    });

    // THE SPEED DIAL, in place of Deadlock's own stepper. The game's stepper is hidden by the
    // native-HUD driver; this is the CS2-style continuous dial that replaces it.
    var dialHost = M.make(row, 'Panel', '', {
      width: '250px', height: '30px', marginLeft: '16px', marginRight: '16px', verticalAlign: 'middle'
    });
    speedDial = $.MvmSpeedDial ? $.MvmSpeedDial.build(dialHost, { width: 250, height: 30, id: 'MvmRegSpeed' }) : null;

    // THE WORKSPACES ARE REACHED FROM HERE, and the button for the workspace you are already in is
    // hidden -- what CS2 calls the ModeSwapBtn. Loop lives here too, because looping is a property
    // of playback and this is the playback bar.
    // THESE FOUR ARE NOT OUR CHROME, and the spec says so by giving them their own fill: a flat
    // slate blue with no border, where every button in the dock above is a bevelled gradient inside
    // a 1px rule. They sit ON Deadlock's bar, so they are drawn as something added to the game's
    // furniture rather than as a piece of ours that happens to be parked there. Three coloured
    // borders, one per workspace, was our idea and it made the right-hand end of the bar read as a
    // legend for colours nothing else on screen uses.
    function wsButton(id, caption, wide, onClick) {
      var b = bevelButton(row, id, caption, wide, onClick);
      if (!b) return null;
      M.set(b, {
        height: '32px', borderRadius: '6px', padding: '0px',
        border: '1px solid ' + M.CLEAR, backgroundColor: WS_BG
      });
      if (b.__lbl) M.set(b.__lbl, { letterSpacing: '1.2px' });
      b.__wsBg = WS_BG;
      try {
        b.SetPanelEvent('onmouseover', function () { M.set(b, { backgroundColor: WS_BG_HOT }); });
        b.SetPanelEvent('onmouseout', function () { M.set(b, { backgroundColor: b.__wsBg }); });
      } catch (e) {}
      return b;
    }
    var camBtn = wsButton('MvmBarCam', 'CAM EDITOR', 122, function () {
      cmd('mirv_filmmaker workspace camera');
    });
    var configBtn = wsButton('MvmBarConfig', 'CONFIG', 96, function () {
      cmd('mirv_filmmaker workspace config');
    });
    var renderBtn = wsButton('MvmBarRender', 'RENDER', 96, function () {
      cmd('mirv_filmmaker workspace render');
    });
    var loopBtn = wsButton('MvmBarLoop', 'LOOP', 84, function () {
      cmd('mirv_filmmaker loop toggle');
    });
    row.__ws = { camera: camBtn, config: configBtn, render: renderBtn, loop: loopBtn };
  }

  function build(parent) {
    // OPAQUE NOW, AND THAT IS THE CHANGE.
    //
    // It used to be transparent on purpose, so the game's bar could show through from underneath.
    // Nothing shows through -- see the header -- and a transparent band let the unscaled world
    // bleed into the dock. This brings the bar's own ground with it instead.
    wrap = M.make(parent, 'Panel', 'MvmRegularTimeline', {
      horizontalAlign: 'left', verticalAlign: 'top',
      // FULLY OPAQUE. At 0.85 the game's own replay bar -- which stays laid out underneath us so
      // the dock can delegate to its buttons -- showed straight through: its transport icons, its
      // marker dropdown and the tail of its scrubber all ghosted across our own transport, which
      // reads as a second, broken set of controls. The band is ours; it paints its own ground.
      backgroundColor: 'rgb(46,44,39)',
      flowChildren: 'down', padding: '9px 12px 8px 12px'
    });
    if (!wrap) return;
    wrap.visible = false;

    buildScrub(wrap);
    buildControls(wrap);

    // Kept for the case the transport has nothing to drive: no replay loaded, or a context where
    // the demo player is not answering. An empty band with no explanation is the state that was
    // reported as "the regular timeline isn't there".
    notice = M.label(wrap, 'MvmRegNotice', '', {
      color: S.textMuted, fontSize: '12px', letterSpacing: '1px',
      horizontalAlign: 'center', width: '100%'
    });
    if (notice) { notice.hittest = false; notice.visible = false; }
  }

  function setVisible(v) {
    shown = !!v;
    M.vis(wrap, shown);
  }

  function layout(x, y, w, h, cursorOn) {
    if (!wrap) return;
    geom.x = Math.floor(x);
    geom.y = Math.floor(y);
    geom.w = Math.floor(Math.max(0, w));
    geom.h = Math.floor(Math.max(0, h));
    M.set(wrap, {
      position: geom.x + 'px ' + geom.y + 'px 0px',
      width: geom.w + 'px',
      height: geom.h + 'px'
    });
    try { wrap.hittest = !!cursorOn; } catch (e) {}
  }
)CFJS" R"CFJS(
  // ---- render ---------------------------------------------------------------------------------

  function clock(sec) {
    if (!(sec > 0)) sec = 0;
    var m = Math.floor(sec / 60), s2 = Math.floor(sec - m * 60);
    return m + ':' + (s2 < 10 ? '0' : '') + s2;
  }

  function pct(tick) {
    var span = (spanLast - spanFirst) || 1;
    var f = (tick - spanFirst) / span;
    if (f < 0) f = 0; else if (f > 1) f = 1;
    return f;
  }

  // The replay's own events, read off the game's bar. Ours are drawn from the same ticks, in the
  // scrubber's own tints -- kill white, death red, anything else green -- and clicking one seeks a
  // lead-in EARLY, because clicking a kill is a request to watch it rather than to land on it.
  var EVENT_TINT = { kill: '#ffffff', death: 'rgb(225,75,75)' };
  var LEAD_IN = 320;   // five seconds at Deadlock's 64 ticks a second

  function nativeEvents() {
    var box = nat('MarkersContainer');
    if (!box) return [];
    var out = [], n = 0;
    try { n = box.GetChildCount ? box.GetChildCount() : 0; } catch (e) { n = 0; }
    for (var i = 0; i < n; ++i) {
      var c = null;
      try { c = box.GetChild(i); } catch (e) {}
      if (!c || !c.id) continue;
      var m = /^([a-z_]+)_event_(\d+)$/.exec(c.id);
      if (!m) continue;
      out.push({ kind: m[1], tick: parseInt(m[2], 10) });
    }
    return out;
  }

  function renderEvents() {
    var list = nativeEvents();
    var innerW = Math.max(0, geom.w - 24);
    while (pips.length < list.length) {
      (function () {
        var pip = M.make(scrubRow, 'Panel', '', {
          width: '3px', height: '15px', borderRadius: '1px',
          horizontalAlign: 'left', verticalAlign: 'bottom', marginBottom: '4px'
        });
        if (!pip) return;
        var item = { panel: pip, tick: -1 };
        // Each handler closes over ITS OWN item, never over the loop variable. Sharing one
        // variable is how every marker once seeked to the last marker's tick.
        try {
          pip.SetPanelEvent('onactivate', function () {
            cmd('mirv_filmmaker seek ' + Math.max(0, item.tick - LEAD_IN));
          });
        } catch (e) {}
        pips.push(item);
      })();
    }
    for (var i = 0; i < pips.length; ++i) {
      var it = pips[i];
      if (i >= list.length) {
        if (it.panel.__mvmShown !== false) {
          try { it.panel.visible = false; it.panel.__mvmShown = false; } catch (e) {}
        }
        continue;
      }
      var e = list[i];
      it.tick = e.tick;
      // Named after the tick it stands for, the way the game names its own, so what a pip means is
      // readable from the tree rather than only from the closure that drew it.
      if (it.panel.__mvmEventId !== e.tick) {
        try { it.panel.id = 'MvmRegEvent_' + e.tick; it.panel.__mvmEventId = e.tick; } catch (er) {}
      }
      if (it.panel.__mvmShown !== true) {
        try { it.panel.visible = true; it.panel.__mvmShown = true; } catch (e) {}
      }
      M.set(it.panel, {
        marginLeft: Math.round(pct(e.tick) * innerW) + 'px',
        backgroundColor: EVENT_TINT[e.kind] || '#ace15d'
      });
    }
  }

  function place(panel, fraction, widthPx) {
    if (!panel) return;
    var x = Math.round(fraction * Math.max(0, geom.w - 24));
    M.set(panel, { marginLeft: Math.max(0, x - Math.floor((widthPx || 0) / 2)) + 'px' });
  }

  function render(st) {
    if (!wrap || !shown) return;

    // The replay's own span. With no replay the transport has nothing to seek within, so it says
    // so rather than offering a scrubber that maps to nothing.
    var haveDemo = !!st.demo;
    if (notice) {
      M.set(notice, { color: S.textMuted });
      M.txt(notice, haveDemo ? '' : 'No replay loaded.');
      M.vis(notice, !haveDemo);
    }
    M.vis(scrubRow, haveDemo);
    M.vis(row, haveDemo);
    if (!haveDemo) return;

    var span = nativeSpan();
    if (span) {
      spanFirst = span.first;
      spanLast = span.last;
    } else {
      spanFirst = (typeof st.demoStartTick === 'number') ? st.demoStartTick : 0;
      spanLast = (typeof st.demoEndTick === 'number') ? st.demoEndTick : (st.tick || 1);
      // No scrubber to read and no span in the feed: grow the end to whatever we have seen, so the
      // playhead walks the strip instead of sitting pinned at the right.
      if (spanLast < (st.tick || 0)) spanLast = st.tick;
    }
    if (!(spanLast > spanFirst)) spanLast = spanFirst + 1;

    var tick = st.tick || 0;
    var f = pct(tick);

    M.set(fill, { width: (f * 100).toFixed(2) + '%' });
    place(head, f, 4);
    if (scrubSlider && !scrubbing) {
      scrubApplying = true;
      try { scrubSlider.value = f; } catch (e) {}
      scrubApplying = false;
    }

    // The loop, drawn from the same in/out the replay bar's flags carry. Dimmed but in place when
    // the loop is off, so arming it does not make a region appear from nowhere.
    var haveLoop = (st.loopIn >= 0 && st.loopOut > st.loopIn);
    if (loopBand) {
      if (haveLoop) {
        var a = pct(st.loopIn), b = pct(st.loopOut);
        var innerW = Math.max(0, geom.w - 24);
        M.set(loopBand, {
          marginLeft: Math.round(a * innerW) + 'px',
          width: Math.max(1, Math.round((b - a) * innerW)) + 'px',
          backgroundColor: st.loopOn ? 'rgba(95,230,158,0.24)' : 'rgba(95,230,158,0.10)'
        });
      }
      M.vis(loopBand, haveLoop);
    }
    if (flagIn) {
      if (haveLoop || st.loopIn >= 0) place(flagIn, pct(st.loopIn), 16);
      M.vis(flagIn, st.loopIn >= 0);
    }
    if (flagOut) {
      if (st.loopOut >= 0) place(flagOut, pct(st.loopOut), 16);
      M.vis(flagOut, st.loopOut >= 0);
    }

    renderEvents();

    // Play and pause are the same control, so it swaps its icon rather than its label.
    if (playBtn && playBtn.__ico) {
      M.set(playBtn.__ico, {
        backgroundImage: 'url("' + (st.paused ? S.icoReplayPlay : S.icoReplayPause) + '")'
      });
    }
    if (timeLabel) {
      try {
        M.txt(timeLabel, clock((tick - spanFirst) / 64) + ' / ' + clock((spanLast - spanFirst) / 64));
      } catch (e) {}
    }

    // The dial is cursor-driven, exactly as the CS2 one is: pumped with this frame's pointer, then
    // seeded from the engine's published speed, which setSpeed ignores while a drag owns the handle.
    if (speedDial && $.MvmSpeedDial) {
      $.MvmSpeedDial.pump(speedDial, st);
      if (typeof st.speed === 'number') $.MvmSpeedDial.setSpeed(speedDial, st.speed);
    }

    // Hide the button that names the workspace you are already in.
    if (row && row.__ws) {
      var open = !!st.open, ws = st.workspace || '';
      var showFor = function (key, btn) {
        if (!btn) return;
        var hideIt = open && ws === key;
        if (btn.__mvmShown !== !hideIt) {
          try { btn.visible = !hideIt; btn.__mvmShown = !hideIt; } catch (e) {}
        }
      };
      showFor('camera', row.__ws.camera);
      showFor('config', row.__ws.config);
      showFor('render', row.__ws.render);
      // Loop is the one of the four that has a STATE, so it is the one that changes colour: a green
      // fill inside a green rule while it is on, and the same slate as its neighbours while it is off.
      var lb = row.__ws.loop;
      if (lb) {
        if (lb.__lbl) {
          try { lb.__lbl.text = st.loopOn ? 'LOOP ON' : 'LOOP'; } catch (e) {}
          M.set(lb.__lbl, { color: st.loopOn ? S.green : S.text });
        }
        lb.__wsBg = st.loopOn ? WS_BG_LOOP : WS_BG;
        M.set(lb, {
          border: '1px solid ' + (st.loopOn ? S.green : M.CLEAR),
          backgroundColor: lb.__wsBg
        });
      }
    }

    // Our own band height, reported once on change. We draw the contents, so we are the only thing
    // that knows how tall they are -- the game bar's measured height is no longer the answer.
    if (BAND_H !== lastReportedH) {
      lastReportedH = BAND_H;
      cmd('mirv_filmmaker dock nativeheight ' + BAND_H);
    }
  }

  $.MvmRegular = {
    build: build,
    setVisible: setVisible,
    layout: layout,
    render: render,
    height: function () { return BAND_H; }
  };
})();
)CFJS"
