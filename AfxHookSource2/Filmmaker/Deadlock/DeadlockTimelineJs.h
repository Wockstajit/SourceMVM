#pragma once

// THE CAMERA TIMELINE: the dock's second tab, and the surface the shot is actually built on.
//
// GEOMETRY IS CS2's, VERBATIM, because the two tools are meant to be the same tool. From
// Panorama/CameraTimelineJs.h:
//
//   header    15px bold title, 2px tracking; a hint line; Clear and close, right
//   transport a 38px accent PLAY; a joined 44+1+44 step pair; Linear | Smooth; a mono readout;
//             the speed control right-anchored at 250px
//   track     42px tall. An 8px rail inset 26px from the top of a FULL-HEIGHT wrapper; a 22px
//             band of keyframe diamonds above it; a playhead running the whole 42px with a 10px
//             knob at y=22; the Slider banded to y 18..42 so it does not swallow diamond clicks
//
// THE THREE RULES THAT LOOK LIKE FUSSINESS AND ARE NOT
//
//   * THE RAIL NEEDS A FULL-HEIGHT WRAPPER. Positioning the 8px rail itself at `0 26 0` takes it
//     out of the row's stack and only its top border survives, so the rail draws as a hairline.
//   * THE DIAMOND BAND IS 22px, NOT 18. A 14px square rotated 45 degrees is 19.8px across its
//     diagonal, plus its border. An 18px band clips every diamond's top point.
//   * THE SLIDER KEEPS ITS CLASS. `HorizontalSlider` is not skin: the drag maps the pointer onto
//     the TRACK's laid-out geometry, and a classless Slider has no track to map onto -- it draws
//     and cannot be dragged.
//
// WHAT DEADLOCK CHANGES. CS2's speed control is a cursor-driven scrubber, and this context has no
// mouse-move event at all, so the drag has nothing to listen to. It is a native Slider here with
// the same logarithmic mapping and the same detent at 1x -- same control, different mechanism.
// Everything else that drags is a Slider for the same reason.
//
// THE SCRUB CONTRACT, carried over intact:
//   * not scrubbing -> the playhead IS the demo tick, so the timeline and the graph agree for free
//   * scrubbing     -> the playhead is the dragged tick and the world has not moved
//   * release       -> ends the scrub AND seeks, so the idle fallback is immediately right again
// A release that does not seek is how the timeline and the graph end up reporting different ticks
// while both look correct.

#define DEADLOCK_TIMELINE_JS R"CFJS(
(function () {
  if ($.MvmTimeline) return;
  var M = $.MvmStyle;
  if (!M) return;
  var S = M.S;
  var D = $.MvmDraw;
  var X = $.MvmControls;

  var dock = null, transport = null, trackRow = null;
  var scrub = null, scrubApplying = false, scrubbing = false;
  var railFill = null, headLine = null, headKnob = null;
  var diamondLayer = null, diamondPool = null, shakeDotPool = null;
  var playBtn = null, playLbl = null, freeBtn = null;
  var easeBtn = null, easeLbl = null, easeIndex = 0;
  var speedBtns = [];
  // CS2 footer extras (CameraTimelineJs.h): the per-segment speed stepper, Live / Freeze, and the
  // transport's speed scrubber.
  var segMinus = null, segLbl = null, segPlus = null, liveCell = null, freezeCell = null, speedDial = null;
  var EASE_NAMES = ['none', 'in', 'out', 'inout'];
  var interpLin = null, interpSmooth = null;
  var readout = null, rulerLayer = null, rulerPool = null, rulerTickPool = null;
  var geom = { x: 0, w: 0, first: 0, last: 1 };
  var last = null, lastRmb = false;
  var shakeHost = null, shakeShell = null, shakeMenuPanel = null, shakeRefs = {}, shakeList = null;
  var shakeListOpen = false, shakeListRows = [], shakeListSignature = '';
  var shakeRemoveAllArmed = false;
  var Q = { accent: '#f0b323ff', value: '#eef2f6ff', label: '#c8d3dfff', hint: '#9aa5b3ff',
    border: '#ffffff1f', button: '#ffffff14', hover: '#ffffff26', on: '#f0b32333' };
  var shakeKey = -1, shakeTick = -1, shakeWorkspace = '', shakeLevel = '';
  var shakeAnchor = null;

  // The original design's camera strip, top to bottom: a transport row, the ruler, the scrub track, and an
  // action row -- 10 + 30 + 9 + 20 + 9 + 44 + 9 + 30 + 10 = 171, inside the 172px the 208px dock
  // leaves after its ribbon. There is no header row: the card above already says which editor this
  // is, and a second title inside the dock was saying it twice.
  // CS2's CameraTimelineJs.h rows AS DOCKED IN THE EDITOR: transport, track (42), key footer, 8px
  // apart. No header row -- CS2 builds one but hides it whenever the timeline is hosted by the
  // editor (`header.visible = !hosted`) -- and no ruler row, which CS2 never had. Top padding and
  // the transport height are CS2's measured positions under the ribbon (captures at 1080p):
  // 18 + 38 + 8 + 42 + 8 + 30 + 18 = 162, inside the 172px editor band DeadlockDock.cpp reserves.
  var TRANSPORT_H = 38, RULER_H = 20, TRACK_H = 42, ACTIONS_H = 30;
  var PAD_X = 14, PAD_Y = 18, GAP = 8;
  var DIAMOND = 14;
  var C_OFF = '#ffffff0d';   // the unselected cell ground, from the spec

  function cmd(text) { M.cmd(text); }

  // No speed mapping of its own any more. The dial ($.MvmSpeedDial) owns the log track, the 1x
  // detent and the readout, and this dock hosts one exactly as the bar and the graph do -- three
  // copies of the mapping disagreed on where 1x sat, which is the kind of thing a user feels before
  // anyone can name it.

  function tickToX(tick) {
    if (!(geom.last > geom.first) || !(geom.w > 0)) return 0;
    var t = (tick - geom.first) / (geom.last - geom.first);
    if (t < 0) t = 0; else if (t > 1) t = 1;
    return t * geom.w;
  }

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

  // ---- the dock's two button families -----------------------------------------------------------
  //
  // The spec draws exactly two, and the difference between them is not decoration: a BEVEL performs
  // an action, a CELL selects one of a set. Drawing both as the same flat rectangle -- which is what
  // this dock used to do -- is what made the strip read as a row of undifferentiated grey boxes with
  // the text sitting slightly wrong inside each one.
  //
  // Both size to their caption. A fixed pixel width has to be guessed per string, and every guess
  // that comes up short crops the caption while every one that runs long leaves a button that is
  // mostly empty; padding measures the text for us and is what the spec actually specifies.

  var BEVEL_TOP = S.btnTop, BEVEL_BOT = S.btnBottom;
  var BEVEL_HOT_TOP = S.btnTopHover, BEVEL_HOT_BOT = S.btnBottomHover;
  // The accent bevel is a warmer ground under a coloured border, not a tint of the grey one.
  var ACCENT_TOP = '#4a4a36', ACCENT_BOT = '#5c5a3c';

  function grad(top, bottom) {
    return 'gradient( linear, 0% 0%, 0% 100%, from( ' + top + ' ), to( ' + bottom + ' ) )';
  }

  // opts: { accent, gap, gapL }  -- accent colours the border and the caption and warms the ground.
  function bevel(parent, id, caption, onClick, opts) {
    opts = opts || {};
    var acc = opts.accent || null;
    var b = M.make(parent, 'Panel', id, {
      width: 'fit-children',
      padding: '7px 13px',
      borderRadius: '8px',
      border: '1px solid ' + (acc || S.border),
      backgroundColor: acc ? grad(ACCENT_TOP, ACCENT_BOT) : grad(BEVEL_TOP, BEVEL_BOT),
      verticalAlign: 'middle',
      marginRight: (opts.gap === undefined ? 6 : opts.gap) + 'px',
      marginLeft: (opts.gapL || 0) + 'px',
      transition: 'background-color 0.12s ease-out, border-color 0.12s ease-out'
    });
    if (!b) return null;
    var l = M.label(b, '', caption, {
      color: acc || S.text,
      fontSize: '12px',
      letterSpacing: '2px',
      textTransform: 'uppercase',
      textShadow: '2px 2px 0px 1.0 #000000',
      whiteSpace: 'nowrap',
      verticalAlign: 'middle'
    });
    if (l) l.hittest = false;
    b.__lbl = l;
    b.__accent = acc;
    function ground(hot) {
      if (b.__accent) return grad(ACCENT_TOP, ACCENT_BOT);
      return hot ? grad(BEVEL_HOT_TOP, BEVEL_HOT_BOT) : grad(BEVEL_TOP, BEVEL_BOT);
    }
    b.__paint = function (accent) {
      b.__accent = accent || null;
      M.set(b, {
        border: '1px solid ' + (b.__accent || S.border),
        backgroundColor: ground(false)
      });
      if (b.__lbl) M.set(b.__lbl, { color: b.__accent || S.text });
    };
    if (onClick) {
      try {
        b.SetPanelEvent('onactivate', onClick);
        b.SetPanelEvent('onmouseover', function () { M.set(b, { backgroundColor: ground(true) }); });
        b.SetPanelEvent('onmouseout', function () { M.set(b, { backgroundColor: ground(false) }); });
      } catch (e) {}
    }
    return b;
  }

  // One cell of a segmented set. The set's own wrapper clips the corners, so a cell carries no
  // radius of its own and the group reads as one control rather than as buttons that happen to touch.
  function cell(parent, id, caption, onClick) {
    var c = M.make(parent, 'Panel', id, {
      width: 'fit-children', padding: '8px 13px',
      border: '1px solid ' + S.border, backgroundColor: C_OFF,
      verticalAlign: 'middle',
      transition: 'background-color 0.12s ease-out, border-color 0.12s ease-out'
    });
    if (!c) return null;
    // Fixed height: an unconstrained label is sized to its glyph ink, so "PER-SEG" (a descender)
    // stood taller than "MANUAL" beside it. Same rule DeadlockControlsJs.h's segment() uses.
    var l = M.label(c, '', caption, {
      color: S.textMuted, fontSize: '12px', height: '16px', letterSpacing: '1px',
      textTransform: 'uppercase', whiteSpace: 'nowrap', verticalAlign: 'middle'
    });
    if (l) l.hittest = false;
    c.__lbl = l;
    c.__set = function (on, accent) {
      var acc = accent || S.gold;
      M.set(c, {
        border: '1px solid ' + (on ? acc : S.border),
        backgroundColor: on ? (acc + '2e') : C_OFF
      });
      if (c.__lbl) M.set(c.__lbl, { color: on ? acc : S.textMuted });
    };
    if (onClick) { try { c.SetPanelEvent('onactivate', onClick); } catch (e) {} }
    return c;
  }

  // The wrapper that makes a row of cells one control.
  function segBox(parent) {
    return M.make(parent, 'Panel', '', {
      flowChildren: 'right', width: 'fit-children', verticalAlign: 'middle',
      borderRadius: '4px', overflow: 'clip'
    });
  }

  // ---- the transport row (CS2 "SHARED TRANSPORT") -------------------------------------------------
  //
  // Play (a fixed 38px glyph button), the joined single-tick stepper, Linear / Smooth, the readout
  // taking the slack, and the speed scrubber right-anchored -- CS2's order and sizes
  // (CameraTimelineJs.h 852-931). FREE CAM is not on this row in CS2 and is gone from here.
  function buildTransport() {
    transport = M.make(dock, 'Panel', 'MvmTlTransport', {
      width: '100%', height: TRANSPORT_H + 'px', flowChildren: 'right', marginBottom: GAP + 'px'
    });
    if (!transport) return;

    playBtn = bevel(transport, 'MvmTlShot', '▶', function () {
      cmd('mirv_filmmaker path play');
    }, { accent: S.gold, gap: 0 });
    if (playBtn) {
      M.set(playBtn, { width: '38px', padding: '7px 0px', borderRadius: '3px' });
      if (playBtn.__lbl) M.set(playBtn.__lbl, { horizontalAlign: 'center', width: '100%', letterSpacing: '0px' });
    }
    playLbl = playBtn ? playBtn.__lbl : null;

    // The stepper: two 44px halves inside ONE hairline border with a divider, not two buttons.
    var step = M.make(transport, 'Panel', 'MvmTlStep', {
      flowChildren: 'right', width: 'fit-children', verticalAlign: 'middle', marginLeft: '11px',
      border: '1px solid #ffffff1f', borderRadius: '3px', overflow: 'clip'
    });
    function half(id, caption, delta) {
      var h = M.make(step, 'Panel', id, { width: '44px', padding: '10px 0px', backgroundColor: C_OFF });
      if (!h) return null;
      var l = M.label(h, '', caption, {
        color: S.text, fontSize: '12px', fontWeight: 'bold', horizontalAlign: 'center',
        verticalAlign: 'middle', width: '100%', whiteSpace: 'nowrap'
      });
      if (l) l.hittest = false;
      try {
        h.SetPanelEvent('onactivate', function () { cmd('mirv_filmmaker scrubtick ' + delta); });
        h.SetPanelEvent('onmouseover', function () { M.set(h, { backgroundColor: '#ffffff1f' }); });
        h.SetPanelEvent('onmouseout', function () { M.set(h, { backgroundColor: C_OFF }); });
      } catch (e) {}
      return h;
    }
    half('MvmTlStepBack', '◀ 1', -1);
    var sdiv = M.make(step, 'Panel', '', { width: '1px', height: '100%', backgroundColor: '#ffffff1f' });
    if (sdiv) sdiv.hittest = false;
    half('MvmTlStepFwd', '1 ▶', 1);

    // The path's ONE interpolation mode. The graph's own Linear / Smooth drives the same setting,
    // so the two surfaces cannot disagree about the curve. (No wave glyph: U+223F is not in the
    // face Deadlock ships.)
    var interpBox = segBox(transport);
    if (interpBox) M.set(interpBox, { marginLeft: '12px' });
    interpLin = cell(interpBox, 'MvmTlLinear', 'LINEAR', function () {
      cmd('mirv_filmmaker path interp linear');
    });
    interpSmooth = cell(interpBox, 'MvmTlSmooth', 'SMOOTH', function () {
      cmd('mirv_filmmaker path interp cubic');
    });

    readout = M.label(transport, 'MvmTlReadout', '', {
      color: S.text, fontFamily: S.fontMono, fontSize: '15px', fontWeight: 'bold', marginLeft: '14px',
      verticalAlign: 'middle', width: 'fill-parent-flow(1.0)',
      whiteSpace: 'nowrap', textOverflow: 'ellipsis'
    });
    if (readout) readout.hittest = false;

    // CS2's speed scrubber, 250x30, right-anchored. The same dial the playback bar and the graph
    // carry; all three show one number because all three are pumped from the same state.
    speedDial = $.MvmSpeedDial ? $.MvmSpeedDial.build(transport, { width: 250, height: 30, id: 'MvmTlSpeed' }) : null;
    if (speedDial) M.set(speedDial, { marginLeft: '12px' });
  }

)CFJS" R"CFJS(
  // ---- the track ------------------------------------------------------------------------------
  function buildTrack() {
    trackRow = M.make(dock, 'Panel', 'MvmTlTrack', {
      width: '100%', height: TRACK_H + 'px'
    });
    if (!trackRow) return;

    // THE RAIL, inside a full-height wrapper. See the header note: positioning the 8px panel
    // directly leaves only its top border.
    var railWrap = M.make(trackRow, 'Panel', '', {
      width: '100%', height: TRACK_H + 'px', position: '0px 0px 0px'
    });
    if (railWrap) railWrap.hittest = false;
    var rail = M.make(railWrap, 'Panel', 'MvmTlRail', {
      width: '100%', height: '8px', marginTop: '26px', borderRadius: '4px',
      backgroundColor: '#10130Dff', border: '1px solid ' + S.border, overflow: 'clip'
    });
    if (rail) {
      rail.hittest = false;
      railFill = M.make(rail, 'Panel', '', {
        width: '0px', height: '100%', backgroundColor: S.gold
      });
      if (railFill) railFill.hittest = false;
    }

    // NO LOOP REGION HERE. The loop is a property of playback, not of the shot, so it is drawn
    // on the Regular Timeline -- the game's own replay bar -- and only there. Two views that both
    // draw the loop are two places it can be wrong.

    // The 22px diamond band. A 14px square rotated 45 degrees needs every one of those pixels.
    diamondLayer = M.make(trackRow, 'Panel', 'MvmTlDiamonds', {
      position: '0px 0px 0px', width: '100%', height: '22px'
    });
    if (diamondLayer) diamondLayer.hittest = false;
    diamondPool = D ? D.pool(diamondLayer, 'Panel', 192) : null;
    // The shake indicator dot (CS2: CameraTimelineJs.h ~1155-1166) is its own pool on the same
    // layer: D.diamond() writes cx/cy/rotate straight onto the pooled panel it is given, so a dot
    // must be a SEPARATE, unrotated sibling rather than a child squeezed inside the rotated square.
    shakeDotPool = D ? D.pool(diamondLayer, 'Panel', 192) : null;

    var headWrap = M.make(trackRow, 'Panel', '', {
      position: '0px 0px 0px', width: '100%', height: TRACK_H + 'px'
    });
    if (headWrap) headWrap.hittest = false;
    headLine = M.make(headWrap, 'Panel', 'MvmTlHead', {
      width: '2px', height: TRACK_H + 'px', backgroundColor: S.warn, position: '0px 0px 0px'
    });
    if (headLine) {
      headLine.hittest = false;
      headKnob = M.make(headLine, 'Panel', '', {
        width: '10px', height: '10px', borderRadius: '2px',
        backgroundColor: S.warn, position: '-4px 22px 0px'
      });
      if (headKnob) headKnob.hittest = false;
    }

    // BANDED TO THE LOWER PART OF THE TRACK, y 18..42. A full-height Slider sits on top of the
    // diamond band in hit order and swallows every diamond press.
    // INVISIBLE. The slider exists only because this Panorama context has no mouse-move event and a
    // hand-rolled drag would have nothing to listen to; what you are meant to see is the rail, the
    // diamonds and the red playhead. Left visible, it drew Deadlock's own round grip on top of the
    // track, parked at the left end and looking like a marker nobody placed.
    scrub = M.make(trackRow, 'Slider', 'MvmTlScrub', {
      position: '0px 18px 0px', width: '100%', height: '24px', opacity: '0.0'
    });
    if (scrub) {
      try { scrub.AddClass('HorizontalSlider'); } catch (e) {}
      try {
        scrub.min = 0; scrub.max = 1; scrub.value = 0;
        scrub.SetPanelEvent('onvaluechanged', function () {
          if (scrubApplying) return;
          scrubbing = true;
          cmd('mirv_filmmaker scrub ' + scrub.value.toFixed(3));
        });
        scrub.SetPanelEvent('onmouseactivate', function () { scrubbing = true; });
        scrub.SetPanelEvent('onmouseup', function () {
          if (!scrubbing) return;
          scrubbing = false;
          cmd('mirv_filmmaker scrubend');
        });
        scrub.SetPanelEvent('onblur', function () {
          if (!scrubbing) return;
          scrubbing = false;
          cmd('mirv_filmmaker scrubend');
        });
      } catch (e) {}
    }
  }

  // ---- ruler ----------------------------------------------------------------------------------
  function buildRuler() {
    rulerLayer = M.make(dock, 'Panel', 'MvmTlRuler', {
      width: '100%', height: RULER_H + 'px'
    });
    if (rulerLayer) rulerLayer.hittest = false;
    // TWO POOLS, DRAWN IN THIS ORDER: the hairlines first so the labels sit on top of them. A label
    // alone leaves the eye to guess which pixel of the track its time refers to, and at a 1292px
    // band that guess is worth several seconds of footage.
    rulerTickPool = D ? D.pool(rulerLayer, 'Panel', 24) : null;
    rulerPool = D ? D.pool(rulerLayer, 'Label', 24) : null;
  }

  // ---- actions row ----------------------------------------------------------------------------
  //
  // WHAT IS NOT HERE: the loop. This row used to open with LOOP / LOOP OFF / SET IN / SET OUT /
  // CLEAR, and that was the wrong home for them. Looping is a property of PLAYBACK, so it belongs on
  // the playback bar -- the Regular Timeline, where DeadlockNativeHudJs.h puts the Loop button beside
  // the speed dial. This dock is about the SHOT.
  //
  // The rule is the same one CS2 settled on, and it is not tidiness: a loop that can be set from two
  // views is a loop that can disagree with itself, and the user finds out by setting one here and
  // watching playback ignore it.
  function buildActionsRow() {
    var row = M.make(dock, 'Panel', 'MvmTlActionsRow', {
      width: '100%', height: ACTIONS_H + 'px', flowChildren: 'right', marginTop: GAP + 'px'
    });
    if (!row) return;

    // CS2's keyFooter (CameraTimelineJs.h 1026-1067), left to right: KEYFRAME caption, + Add,
    // - Delete, Tick->Here, Ease, the per-segment speed stepper, then on the right the speed-mode
    // set and Live / Freeze.
    M.label(row, '', 'KEYFRAME', {
      fontSize: '12px', fontWeight: 'bold', letterSpacing: '1.6px', color: S.textMuted,
      verticalAlign: 'middle', marginRight: '10px', whiteSpace: 'nowrap'
    });

    bevel(row, 'MvmTlAdd', '+ ADD', function () {
      cmd('mirv_filmmaker marker add');
    }, { accent: S.gold });
    bevel(row, 'MvmTlDel', '− DELETE', function () {
      cmd('mirv_filmmaker marker delete');
    }, {});
    // A RETIME (CS2's camtl movekey): the selected key moves to the current tick, pose untouched.
    bevel(row, 'MvmTlTickHere', 'TICK→HERE', function () {
      cmd('mirv_filmmaker marker tickhere');
    }, {});

    // EASE IS PER MARKER, and this is one of its three routes -- the PATH card and the marker's own
    // right-click menu write the same value. The button cycles rather than opening a menu, because
    // four states in a dock row is a cycle, not a list.
    easeBtn = bevel(row, 'MvmTlEase', 'EASE: NONE', function () {
      var next = EASE_NAMES[(easeIndex + 1) % EASE_NAMES.length];
      cmd('mirv_filmmaker marker ease ' + next);
    }, {});
    easeLbl = easeBtn ? easeBtn.__lbl : null;

    // The selected key's OUTGOING segment speed: - / Seg x1.00 / +, stepping the same 0.2 / 0.5 /
    // 0.8 / 1.0 ladder the PATH card's Amount row does, via the same `marker speedmul`.
    var LADDER = [0.2, 0.5, 0.8, 1.0];
    function stepSeg(dir) {
      var cur = (last && last.cam && typeof last.cam.speedMul === 'number') ? last.cam.speedMul : 1;
      var idx = 0, best = 1e9;
      for (var i = 0; i < LADDER.length; ++i) { var d = Math.abs(LADDER[i] - cur); if (d < best) { best = d; idx = i; } }
      idx = Math.max(0, Math.min(LADDER.length - 1, idx + dir));
      cmd('mirv_filmmaker marker speedmul ' + LADDER[idx].toFixed(2));
    }
    segMinus = bevel(row, 'MvmTlSegMinus', '−', function () { stepSeg(-1); }, { gap: 0 });
    segLbl = M.label(row, 'MvmTlSegLbl', 'SEG x1.00', {
      color: S.text, fontSize: '13px', width: '92px', textAlign: 'center', verticalAlign: 'middle',
      whiteSpace: 'nowrap', fontFamily: S.fontMono
    });
    if (segLbl) segLbl.hittest = false;
    segPlus = bevel(row, 'MvmTlSegPlus', '+', function () { stepSeg(1); }, {});

    var spacer = M.make(row, 'Panel', '', { width: 'fill-parent-flow(1.0)', height: '1px' });
    if (spacer) spacer.hittest = false;

    // How the segments are timed, joined on the right the way the curve pair is on the row above.
    var speedBox = segBox(row);
    var modes = [['manual', 'MANUAL'], ['perseg', 'PER-SEG'], ['constant', 'CONSTANT']];
    speedBtns = [];
    for (var m = 0; m < modes.length; ++m) {
      (function (mode) {
        var c = cell(speedBox, 'MvmTlSpeed_' + mode[0], mode[1], function () {
          cmd('mirv_filmmaker path speedmode ' + mode[0]);
        });
        speedBtns.push({ key: mode[0], panel: c });
      })(modes[m]);
    }

    // Live plays the demo along the path; Freeze holds the tick (`path freeze`). The PATH card's
    // Timing row writes the same flag.
    var timingBox = segBox(row);
    if (timingBox) M.set(timingBox, { marginLeft: '8px' });
    liveCell = cell(timingBox, 'MvmTlLive', 'LIVE', function () { cmd('mirv_filmmaker path freeze 0'); });
    freezeCell = cell(timingBox, 'MvmTlFreeze', 'FREEZE', function () { cmd('mirv_filmmaker path freeze 1'); });
  }

)CFJS" R"CFJS(

  function build(parent) {
    // A root sibling of the dock, so the popup can extend above the timeline without clipping.
    shakeHost = parent.GetParent();
    dock = M.make(parent, 'Panel', 'MvmTimelineDock', {
      horizontalAlign: 'left', verticalAlign: 'top',
      backgroundColor: S.panelBgDeep,
      flowChildren: 'down',
      padding: PAD_Y + 'px ' + PAD_X + 'px'
    });
    if (!dock) return;
    dock.visible = false;

    // The original design's order: the controls, then the ruler that labels the track, then the track, then
    // the marker row. The ruler sits ABOVE the track it belongs to, which is the one arrangement
    // that lets a time label point at the tick underneath it.
    buildTransport();
    // No ruler row: CS2's camera timeline has none (its tick/time readout lives on the transport).
    buildTrack();
    buildActionsRow();
  }

  function setVisible(v) {
    M.vis(dock, v);
    if (!v) { closeShake(); lastRmb = true; }
  }

  // Per-camera shake lives only on a timeline diamond's context menu. Reuse the shared controls;
  // build the popup once, then hide it. The clicked key owns edits independently of selection.
  function validShakeKey() {
    return shakeKey >= 0 && last && last.markerList && last.markerList[shakeKey] &&
      last.markerList[shakeKey].tick === shakeTick;
  }
  function shakeForKey() {
    var list = (last && last.shakes) || [];
    for (var i = 0; i < list.length; ++i) if (list[i].i === shakeKey) return list[i];
    return null;
  }
  function closeShake() {
    if (shakeKey < 0) return;
    shakeKey = -1; shakeListOpen = false; shakeRemoveAllArmed = false;
    X.closeMenu(); X.setScrubScope(null);
    if (shakeShell) shakeShell.close();
  }
  function shakeCommand(tail) {
    if (!validShakeKey()) { closeShake(); return; }
    cmd('mirv_filmmaker pathshake ' + tail);
  }
  function setShake(style, intensity, speed) {
    var styles = (last && last.shakeStyles) || [];
    if (style < 0) { if (!styles.length) return; style = styles[0][0]; }
    shakeCommand('set ' + shakeKey + ' ' + style + ' ' + intensity + ' ' + speed);
  }
  // Match CS2's CameraTimelineShakeMenuJs.h: 390px, compact header and ease, stacked properties,
  // segmented meters, and a single action row. Only the numeric steppers are omitted by request.
  function shakeLabel(parent, text, size, color, props) {
    var p = M.label(parent, '', text, { fontSize: size + 'px', color: color || Q.value,
      verticalAlign: 'center', whiteSpace: 'nowrap' });
    if (props) M.set(p, props);
    p.hittest = false;
    return p;
  }
  function shakeButton(parent, id, text, run, color) {
    var p = M.make(parent, 'Panel', id, { padding: '4px 9px', borderRadius: '3px',
      marginRight: '5px', verticalAlign: 'center', backgroundColor: Q.button });
    p.__lbl = shakeLabel(p, text, 13, color, { fontWeight: 'bold' });
    X.on(p, 'onactivate', run);
    X.on(p, 'onmouseover', function () { M.set(p, { backgroundColor: Q.hover }); });
    X.on(p, 'onmouseout', function () { M.set(p, { backgroundColor: p.__selected ? Q.on : Q.button }); });
    return p;
  }
  function shakeSpacer(parent) {
    M.make(parent, 'Panel', '', { width: 'fill-parent-flow(1.0)', height: '1px' }).hittest = false;
  }
  function shakeRule(parent, margin) {
    return M.make(parent, 'Panel', '', { width: '100%', height: '1px',
      backgroundColor: Q.border, marginBottom: margin + 'px' });
  }
  function chooseShake(style) {
    var sh = shakeForKey(); setShake(style, sh ? sh.n : 5, sh ? sh.p : 50);
    shakeListOpen = false; renderShake();
  }
  function stepShakeStyle(delta) {
    var styles = (last && last.shakeStyles) || [], sh = shakeForKey(), i = -1;
    if (!styles.length) return;
    for (var k = 0; sh && k < styles.length; ++k) if (Number(styles[k][0]) === sh.s) i = k;
    if (i < 0) i = delta > 0 ? 0 : styles.length - 1;
    else i = (i + delta + styles.length) % styles.length;
    chooseShake(Number(styles[i][0]));
  }
  // Deadlock's own chrome, via the shared context-menu shell (DeadlockControlsJs.h's
  // X.contextMenuShell) instead of a hand-rolled scrim + 'rgba(16,20,26,0.99)' card: the shell
  // supplies the scrim, the bordered/shadowed panel and outside-click dismissal from S tokens, and
  // this file only supplies CONTENT. The shell REBUILDS its content on every .open() call (it has
  // no persistent-widget concept of its own), so the shell itself is built once here, and
  // buildShakeContent() -- called once per open, not per frame -- is what (re)builds shakeRefs.
  function buildShake() {
    if (shakeShell || !shakeHost || !X) return;
    shakeShell = X.contextMenuShell(shakeHost);
  }
  // `body` is the shell's own content panel; its parent is the shell's bordered card. Grabbing that
  // parent is the only way to reach it at all -- contextMenuShell returns just {open, close} -- and
  // it is what the CS2-matched anchor math in renderShake() sizes/positions every frame, and what
  // scrub-scoping and the scroll-viewport clip (below) need a real panel reference for.
  function buildShakeContent(body) {
    shakeMenuPanel = body.GetParent();
    // 390px, per CameraTimelineShakeMenuJs.h's own shakeMenu.style.width -- everything else here
    // (background, border, radius, shadow, padding, scrolling) now comes from the shell.
    M.set(shakeMenuPanel, { width: '390px', fontFamily: S.fontSans });
    shakeMenuPanel.__mvmValueViewport = true;

    var head = M.make(body, 'Panel', '', { width: '100%', flowChildren: 'right', marginBottom: '14px' });
    shakeLabel(head, 'KEYFRAME', 13, Q.value, { fontWeight: 'bold', letterSpacing: '1.5px' });
    var chip = M.make(head, 'Panel', '', { height: '18px', borderRadius: '9px', padding: '0px 9px',
      marginLeft: '10px', verticalAlign: 'center', backgroundColor: Q.on });
    shakeRefs.key = shakeLabel(chip, '', 11, Q.accent, { fontWeight: 'bold' });
    shakeSpacer(head);
    // Deadlock's bundled font has no cross glyph; an ASCII X keeps the close control visible.
    var close = shakeButton(head, 'MvmShakeClose', 'X', closeShake, Q.label);
    M.set(close, { marginRight: '0px' });

    var easeHead = M.make(body, 'Panel', '', { width: '100%', flowChildren: 'right', marginBottom: '7px' });
    shakeLabel(easeHead, 'EASE', 10, Q.hint, { fontWeight: 'bold', letterSpacing: '1.6px' });
    shakeSpacer(easeHead); shakeLabel(easeHead, 'this key only', 10, Q.hint);
    var easeRow = M.make(body, 'Panel', '', { width: '100%', flowChildren: 'right', marginBottom: '13px' });
    shakeRefs.ease = [];
    for (var ei = 0; ei < 4; ++ei) (function (i) {
      var p = shakeButton(easeRow, 'MvmShakeEase_' + i, ['None', 'In', 'Out', 'In-Out'][i], function () {
        if (validShakeKey()) cmd('mirv_filmmaker marker ease ' + EASE_NAMES[i] + ' ' + shakeKey);
      });
      M.set(p, { marginRight: '2px' }); shakeRefs.ease.push(p);
    })(ei);
    shakeRule(body, 13);

    var styleRow = M.make(body, 'Panel', '', { width: '100%', height: '30px',
      flowChildren: 'right', marginBottom: '8px' });
    shakeLabel(styleRow, 'Style', 12, Q.label, { width: '68px' });
    var prev = shakeButton(styleRow, 'MvmShakeStylePrev', '◀', function () { stepShakeStyle(-1); }, Q.accent);
    M.set(prev, { width: '26px', paddingLeft: '0px', paddingRight: '0px', marginRight: '0px' });
    M.set(prev.__lbl, { width: '100%', textAlign: 'center' });
    var styleBox = M.make(styleRow, 'Panel', 'MvmShakeStyle', { width: '212px', height: '26px',
      marginLeft: '6px', marginRight: '6px', verticalAlign: 'center', borderRadius: '3px', backgroundColor: Q.button });
    shakeRefs.style = shakeLabel(styleBox, '', 12, Q.value, { width: 'fit-children', maxWidth: '212px', horizontalAlign: 'center',
      fontWeight: 'bold', textOverflow: 'shrink' });
    X.on(styleBox, 'onactivate', function () { shakeListOpen = !shakeListOpen; renderShake(); });
    X.hoverBg(styleBox, Q.button, Q.hover);
    var next = shakeButton(styleRow, 'MvmShakeStyleNext', '▶', function () { stepShakeStyle(1); }, Q.accent);
    M.set(next, { width: '26px', paddingLeft: '0px', paddingRight: '0px', marginRight: '0px' });
    M.set(next.__lbl, { width: '100%', textAlign: 'center' });
    shakeRefs.intensity = X.buildMeter(body, { id: 'shakeIntensity', label: 'Intensity',
      min: 1, max: 10, dec: 0, fill: Q.accent, onChange: function (v) {
        var sh = shakeForKey(); setShake(sh ? sh.s : -1, v, sh ? sh.p : 50);
      } });
    shakeRefs.speed = X.buildMeter(body, { id: 'shakeSpeed', label: 'Speed',
      min: 10, max: 100, dec: 0, fill: '#80bdffff', onChange: function (v) {
        var sh = shakeForKey(); setShake(sh ? sh.s : -1, sh ? sh.n : 5, v);
      } });

    // Same inline list as CS2; Deadlock's native scroll viewport also supports its scrollbar.
    shakeList = M.make(body, 'Panel', 'MvmShakeStyles', { width: '100%', height: '250px',
      flowChildren: 'down', overflow: 'squish scroll', marginBottom: '10px',
      backgroundColor: '#00000055', borderRadius: '4px' });
    M.vis(shakeList, false);
    // The shell rebuilds `body` from scratch on every open (see buildShake's own comment), so
    // `shakeList` above is a BRAND NEW, empty panel each time -- but shakeListSignature is a
    // module-level cache that would otherwise survive the rebuild. Left stale, renderShakeList()
    // would see "signature unchanged" on the next open and skip repopulating this fresh empty
    // panel, leaving the style list permanently blank after the first close/reopen. Reset both here,
    // once per (re)build, so the first renderShakeList() after any open always repopulates it.
    shakeListSignature = ''; shakeListRows = [];
    shakeRefs.rule = shakeRule(body, 10);
    var foot = M.make(body, 'Panel', '', { width: '100%', flowChildren: 'right' });
    shakeRefs.applyAll = shakeButton(foot, 'MvmShakeApplyAll', 'Apply to All', function () {
      shakeCommand('applyall ' + shakeKey);
    }, Q.accent);
    shakeRefs.applyAll.__selected = true; M.set(shakeRefs.applyAll, { backgroundColor: Q.on });
    shakeRefs.remove = shakeButton(foot, 'MvmShakeRemove', 'Remove', function () {
      shakeCommand('clear ' + shakeKey); closeShake();
    });
    shakeSpacer(foot);
    // Arm-then-click-again, matching CS2's own removeAllArmed exactly -- already the SAME pattern
    // X.armedButton exists to share, but that helper's label is fixed at construction time with no
    // way to update it afterwards, and this button's label carries a LIVE count ('Remove All (3)')
    // that both CS2 and this control read every frame from state that changes independently of the
    // click (shakes can be added/removed while the popup sits open). Adopting X.armedButton here
    // would either lose that live count or require reaching into its internal label panel by child
    // index -- so the existing, already-equivalent hand-rolled toggle stays; see the shared
    // primitive's own comment for the gap.
    shakeRefs.removeAll = shakeButton(foot, 'MvmShakeRemoveAll', 'Remove All', function () {
      if (!shakeRemoveAllArmed) { shakeRemoveAllArmed = true; renderShake(); return; }
      shakeCommand('clearall'); closeShake();
    }, '#ff8a8aff');
    M.set(shakeRefs.removeAll, { marginRight: '0px' });
  }
)CFJS" R"CFJS(
  function renderShakeList(styles, selected) {
    var signature = JSON.stringify(styles) + ':' + last.shakeEssentialsCount;
    if (signature !== shakeListSignature) {
      shakeListSignature = signature;
      // The style catalogue changes only on load. Never recreate panels during a resting frame.
      shakeList.RemoveAndDeleteChildren(); shakeListRows = [];
      for (var i = 0; i < styles.length; ++i) {
        if (i === 0 || i === last.shakeEssentialsCount) {
          var head = M.make(shakeList, 'Panel', '', { width: '100%', height: '24px',
            paddingLeft: '12px', marginTop: (i === 0 ? 4 : 10) + 'px' });
          shakeLabel(head, i === 0 ? 'ESSENTIALS' : 'MORE CAMERA SHAKES', 10, Q.hint,
            { fontWeight: 'bold', letterSpacing: '1.2px' });
        }
        (function (style) {
          var row = M.make(shakeList, 'Panel', '', { width: '100%', height: '26px', padding: '0px 12px' });
          var text = shakeLabel(row, style[1], 12, Q.label, { textOverflow: 'shrink' });
          X.on(row, 'onactivate', function () { chooseShake(Number(style[0])); });
          X.hoverBg(row, M.CLEAR, Q.hover);
          shakeListRows.push({ text: text, style: Number(style[0]) });
        })(styles[i]);
      }
    }
    for (var j = 0; j < shakeListRows.length; ++j)
      M.set(shakeListRows[j].text, { color: shakeListRows[j].style === selected ? Q.accent : Q.label });
  }
  function renderShake() {
    if (shakeKey < 0) return;
    // The shell dismisses itself on an outside click/right-click (its own scrim, wired inside
    // X.contextMenuShell) without telling this file, so a frame where the panel has gone invisible
    // out from under us is the only signal that happened. Catch it here -- this runs every frame --
    // and run the SAME cleanup closeShake() does (reset shakeKey, drop the scrub scope), or the
    // scrub scope stays pinned to a hidden popup forever.
    if (shakeMenuPanel && !shakeMenuPanel.visible) { closeShake(); return; }
    if (!validShakeKey() || !last.open || !last.demo || !last.uiMouse || !last.layout ||
        !last.layout.valid || last.workspace !== shakeWorkspace || last.level !== shakeLevel) {
      closeShake(); return;
    }
    var sh = shakeForKey(), styles = last.shakeStyles || [], label = sh ? '#' + sh.s : 'None — choose style';
    for (var i = 0; sh && i < styles.length; ++i) if (Number(styles[i][0]) === sh.s) label = styles[i][1];
    M.txt(shakeRefs.key, 'KEY ' + (shakeKey + 1));
    var ease = last.markerList[shakeKey].ease || 0;
    for (var e = 0; e < 4; ++e) {
      var p = shakeRefs.ease[e]; p.__selected = e === ease;
      M.set(p, { backgroundColor: p.__selected ? Q.on : Q.button });
      M.set(p.__lbl, { color: p.__selected ? Q.accent : Q.value });
    }
    M.txt(shakeRefs.style, label);
    shakeRefs.intensity.set(sh ? sh.n : 5); shakeRefs.intensity.setEmpty(!sh);
    shakeRefs.speed.set(sh ? sh.p : 50); shakeRefs.speed.setEmpty(!sh);
    M.vis(shakeRefs.applyAll, !!sh); M.vis(shakeRefs.remove, !!sh);
    var count = (last.shakes || []).length;
    M.vis(shakeRefs.removeAll, count > 0); M.vis(shakeRefs.rule, !!sh || count > 0);
    if (!count) shakeRemoveAllArmed = false;
    M.txt(shakeRefs.removeAll.__lbl, shakeRemoveAllArmed ? 'Remove All — sure?' : 'Remove All (' + count + ')');
    M.set(shakeRefs.removeAll, { backgroundColor: shakeRemoveAllArmed ? '#c92a2a33' : Q.button });
    M.vis(shakeList, shakeListOpen && styles.length > 0);
    if (shakeListOpen) renderShakeList(styles, sh ? sh.s : -1);
    // Anchor above the entire dock and centre on its diamond, just as CS2 does. Anchor geometry
    // never comes from the popup itself; memoized styling settles after its measured height arrives.
    try {
      var a = shakeAnchor.GetPositionWithinWindow(), h = shakeHost.GetPositionWithinWindow();
      var sx = shakeHost.actualuiscale_x || 1, sy = shakeHost.actualuiscale_y || 1;
      var sw = last.layout.screenW / sx, shh = last.layout.screenH / sy;
      var mh = shakeMenuPanel.actuallayoutheight / sy || 300;
      var dockTop = (last.layout.screenH - last.layout.dockH - h.y) / sy;
      M.set(shakeMenuPanel, { maxHeight: Math.max(100, shh - 24) + 'px',
        position: Math.floor(Math.max(8, Math.min(sw - 398, (a.x - h.x) / sx + 7 - 195))) + 'px ' +
          Math.floor(Math.max(8, dockTop - mh - 10)) + 'px 0px' });
    } catch (e) {}
  }
  function openShake(index, anchor) {
    if (!last || !last.open || !last.uiMouse || !dock.visible || !last.demo ||
        !last.markerList || !last.markerList[index] || X.menuOpen()) return;
    if (shakeKey === index && shakeMenuPanel && shakeMenuPanel.visible) return;
    buildShake();
    if (!shakeShell) return;
    X.closeMenu();
    shakeKey = index; shakeTick = last.markerList[index].tick;
    shakeWorkspace = last.workspace; shakeLevel = last.level; shakeAnchor = anchor;
    shakeListOpen = false; shakeRemoveAllArmed = false;
    // .open() rebuilds shakeRefs fresh every time (the shell owns no persistent widgets of its
    // own) and does its own one-shot placement from a stale measured size; renderShake() below
    // immediately overwrites that with the CS2-matched anchor math above the dock, and keeps doing
    // so every subsequent frame -- the shell's own settle callback ~30ms later can only ever draw
    // one intermediate frame before this file's next render() call puts the popup back.
    shakeShell.open(anchor, buildShakeContent);
    X.setScrubScope(shakeMenuPanel);
    renderShake();
  }

)CFJS" R"CFJS(

  // THE DOCK'S WIDTH IS THE DOCK'S, NOT THE SCREEN'S. The camera strip and the graph stop where
  // the inspector column starts, and the render workspace's JOB column must NOT narrow them --
  // it sits above the dock, not beside it. Taking the screen width here drew the right-hand
  // controls underneath the column, where they still took clicks but could not be seen.
  function layout(g, cursorOn, bodyH, dockW) {
    if (!dock || !g) return;
    var w = Math.floor((dockW > 0) ? dockW : g.screenW);
    M.set(dock, {
      position: '0px 0px 0px',
      width: w + 'px',
      height: Math.floor(Math.max(0, bodyH || 0)) + 'px'
    });
    try { dock.hittest = !!cursorOn; } catch (e) {}
    geom.w = Math.max(1, w - PAD_X * 2);
  }

  // ---- render ---------------------------------------------------------------------------------
  function renderDiamonds(list, selected) {
    if (!diamondPool) return;
    list = list || [];
    var shakes = (last && last.shakes) || [];
    for (var i = 0; i < list.length; ++i) {
      var p = diamondPool.next();
      if (!p) break;
      p.__mvmMarkerIndex = i;
      if (!p.__mvmDiamondBound) {
        p.__mvmDiamondBound = true; p.hittest = true;
        (function (key) {
          // Left-click GOES TO the camera. CS2's diamond sends `camtl select <i>`, which is
          // CameraPath::SelectForEditor: free cam on, demo paused, seek to the key's tick, its pose
          // pushed. Deadlock's equivalent is `marker goto <i>` (Shot_SelectForEditor). Right-click
          // keeps opening this key's shake menu.
          X.on(key, 'onactivate', function () { cmd('mirv_filmmaker marker goto ' + key.__mvmMarkerIndex); });
          X.on(key, 'oncontextmenu', function () { openShake(key.__mvmMarkerIndex, key); });
        })(p);
      }
      var sel = !!list[i].selected;
      D.diamond(p, tickToX(list[i].tick), 11, DIAMOND,
                sel ? S.text : S.gold, sel ? S.text : '#10130Dff');
      // A keyframe carrying a shake gets a dot in the middle of its diamond, so where a shot shakes
      // is readable off the bar instead of only by right-clicking every key in turn (CS2:
      // CameraTimelineJs.h ~1155-1166). Dark, not the freeze blue: it has to read on both diamond
      // colours (gold idle, white selected).
      if (shakeDotPool) {
        var hasShake = false;
        for (var k = 0; k < shakes.length; ++k) { if (shakes[k].i === i) { hasShake = true; break; } }
        if (hasShake) {
          var dotP = shakeDotPool.next();
          if (dotP) D.dot(dotP, tickToX(list[i].tick), 11, 4, S.offBlack);
        }
      }
      // The same cursor samples used by value drags also resolve right-button presses. This
      // covers HUD contexts that consume native context-menu events before they reach a child.
      if (last && last.rmb && !lastRmb && shakeKey < 0 && last.uiMouse) {
        try {
          var a = p.GetPositionWithinWindow(), s = p.actualuiscale_x || 1;
          var cx = a.x + p.actuallayoutwidth / 2, cy = a.y + p.actuallayoutheight / 2;
          if (Math.abs(last.mx - cx) + Math.abs(last.my - cy) <= DIAMOND * s * 0.72)
            openShake(i, p);
        } catch (e) {}
      }
    }
    diamondPool.done();
    if (shakeDotPool) shakeDotPool.done();
  }

  function renderRuler() {
    if (!rulerPool) return;
    var span = geom.last - geom.first;
    if (!(span > 0) || !(geom.w > 0)) {
      rulerPool.done();
      if (rulerTickPool) rulerTickPool.done();
      return;
    }
    var step = D.niceStep(span / 8);
    var t0 = Math.ceil(geom.first / step) * step;
    for (var t = t0; t <= geom.last; t += step) {
      var x = tickToX(t);
      if (x < 0 || x > geom.w) continue;
      if (rulerTickPool) {
        rulerTickPool.next({
          position: Math.floor(x) + 'px 0px 0px',
          width: '1px', height: (RULER_H - 6) + 'px',
          backgroundColor: 'rgba(68,68,68,0.6)'
        });
      }
      var l = rulerPool.next({
        position: Math.floor(x + 2) + 'px 4px 0px',
        color: 'rgba(198,198,198,0.67)', fontSize: '10px', fontFamily: S.fontMono
      });
      if (!l) break;
      // CLOCK TIME, NOT A TICK NUMBER. A ruler exists to be read against the footage, and six
      // digits of tick tell you nothing about where in the replay you are. The ticks are still the
      // unit everything else speaks -- the readout above carries one -- but the scale is time.
      try {
        var sec = Math.max(0, (t - geom.first) / 64);
        var mm = Math.floor(sec / 60), ss = sec - mm * 60;
        M.txt(l, mm + ':' + (ss < 10 ? '0' : '') + ss.toFixed(1));
      } catch (e) {}
    }
    rulerPool.done();
    if (rulerTickPool) rulerTickPool.done();
  }

  function render(st) {
    if (!dock || !st) return;
    last = st;
    renderShake();

    geom.first = st.startTick || 0;
    geom.last = Math.max((st.endTick || 0), geom.first + 1);

    var head = (st.playhead !== undefined) ? st.playhead : (st.tick || 0);
    var x = tickToX(head);

    if (scrub && st.demo && !scrubbing) {
      scrubApplying = true;
      try {
        scrub.min = geom.first;
        scrub.max = geom.last;
        scrub.value = head;
      } catch (e) {}
      scrubApplying = false;
    }

    if (railFill) M.set(railFill, { width: Math.floor(x) + 'px' });
    if (headLine) M.set(headLine, { position: Math.floor(x - 1) + 'px 0px 0px' });

    renderDiamonds(st.markerList, st.selected);
    lastRmb = !!st.rmb;
    renderRuler();

    if (playLbl) {
      try {
        // CS2's play glyph swaps to a pause mark while the shot runs (the button is 38px, a word
        // does not fit). Space with the timeline open plays the SHOT; closed, it is a demo pause.
        M.txt(playLbl, st.playing ? 'II' : '▶');
      } catch (e) {}
    }
    if (speedDial && $.MvmSpeedDial) {
      try {
        $.MvmSpeedDial.pump(speedDial, st);
        if (typeof st.speed === 'number') $.MvmSpeedDial.setSpeed(speedDial, st.speed);
      } catch (e) {}
    }
    var camState = st.cam || {};
    if (segLbl) M.txt(segLbl, 'SEG x' + (typeof camState.speedMul === 'number' ? camState.speedMul : 1).toFixed(2));
    // CS2 shows the Seg stepper only in Per-Segment mode with a key selected that HAS an outgoing
    // segment (`perSeg && has && !st.selIsLast`).
    var keyCount = (st.markerList || []).length;
    var segSel = (st.selected === undefined) ? -1 : st.selected;
    var segShown = (camState.speedMode | 0) === 2 && segSel >= 0 && segSel < keyCount - 1;
    if (segMinus) X.show(segMinus, segShown);
    if (segLbl) X.show(segLbl, segShown);
    if (segPlus) X.show(segPlus, segShown);
    if (liveCell && liveCell.__set) liveCell.__set(!camState.freeze, S.gold);
    if (freezeCell && freezeCell.__set) freezeCell.__set(!!camState.freeze, S.gold);
    // GREEN MEANS THE BUTTON OWNS THE VIEW RIGHT NOW; gold means it is the primary action waiting
    // to be taken. So the shot button turns green while it is playing, free cam turns green while it
    // holds the camera, and neither is ever both.
    if (playBtn && playBtn.__paint) playBtn.__paint(st.playing ? S.green : S.gold);
    if (freeBtn && freeBtn.__paint) freeBtn.__paint(st.freecam ? S.green : null);

    var cam = st.cam || {};
    easeIndex = Math.max(0, Math.min(EASE_NAMES.length - 1, cam.ease || 0));
    if (easeLbl) {
      M.txt(easeLbl, 'EASE: ' + EASE_NAMES[easeIndex].toUpperCase());
    }

    var SPEED_ORDER = ['manual', 'constant', 'perseg'];
    var mode = SPEED_ORDER[cam.speedMode || 0] || 'manual';
    for (var sb = 0; sb < speedBtns.length; ++sb) {
      var p = speedBtns[sb].panel;
      if (p && p.__set) p.__set(speedBtns[sb].key === mode, S.gold);
    }

    var smooth = !!st.pathSmooth;
    if (interpSmooth && interpSmooth.__set) interpSmooth.__set(smooth, S.gold);
    if (interpLin && interpLin.__set) interpLin.__set(!smooth, S.gold);
    if (readout) {
      try {
        // CS2's reading: `tick N   time X.XXs` (demo seconds, tick / 64), or the two-key prompt.
        var keysForReadout = (st.markerList || []).length;
        M.txt(readout, !st.demo ? '\u2014'
          : keysForReadout < 2 ? 'Add 2 camera keys (K)'
          : ('tick ' + Math.round(head) + '   time ' + (Math.round(head) / 64).toFixed(2) + 's'));
      } catch (e) {}
    }
  }

  $.MvmTimeline = {
    build: build,
    layout: layout,
    render: render,
    setVisible: setVisible
  };
})();
)CFJS"
