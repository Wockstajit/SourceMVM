#pragma once

// THE GRAPH EDITOR: the dock's third tab, and the same instrument CS2's is.
//
// LAYOUT, from Panorama/GraphEditorJs.h:
//
//     +-----------------------------------------------------------------------+
//     | > | < | > [ speed ...... ] tick 1234   TITLE  [Sm][Li][Undo][Redo][X] |  header 58
//     +--------------+--------------------------------------------------------+
//     | CHANNELS     |  1200      1400      1600      1800                    |  ruler 22
//     | X   solo  12.4 o|                                                     |
//     | Y         -88.1 o|      ____                                         |
//     | Z           0.0 o|    _/    \__                                       |  canvas
//     | Pitch            |                                                    |
//     +--------------+--------------------------------------------------------+
//     | click name = solo · drag a key or handle · box-select · right-click.. |  footer 26
//     +-----------------------------------------------------------------------+
//
// Row order is [swatch+name][value][eye], left to right -- see buildLeft()'s own comment for why
// that specific order is load-bearing (DeadlockGraph.cpp's PickValueCell hit-tests the value cell
// assuming it).
//
// A CURVE IS DRAWN OUT OF ROTATED PANELS. Panorama has no canvas. Each segment is a panel as long
// as the span between two sampled points, one or two pixels tall, rotated about its left edge --
// see DeadlockDrawJs.h, which both this and the timeline's diamonds come from. The previous
// Deadlock graph drew 3px DOTS along the sampled curve, which is why it read as a scatter plot
// rather than as an animation curve.
//
// WHO OWNS WHAT, AND WHY IT IS SPLIT THAT WAY.
//
//   * C++ owns THE PLOT RECTANGLE and every hit test. This context gets no mouse-move event, so a
//     drag has to be driven from the OS-side cursor probe, which means C++ has to know where the
//     plot is on screen. Having JavaScript measure its own layout and report it back is a round
//     trip that is stale by definition.
//   * C++ owns THE CURVE. The polylines arrive already sampled and normalised into the plot, so
//     the curve drawn here is the curve the camera would actually follow -- one evaluator, not a
//     Bezier solve re-implemented in JavaScript and quietly drifting from it.
//   * C++ owns THE PLAYHEAD, because it belongs to the shot: the timeline and the graph are two
//     views of one thing, and a scrub in either has to be the same scrub.
//   * C++ owns EVERY GESTURE, not just the key drag: Bezier tangent handles, the rectangle
//     drag-select, the value cell's scrub, Ctrl+A / Delete, and the right-click Ease/Align/Delete
//     menu all live in DeadlockGraph.cpp's Graph_RunMainThreadFrame, reading the same OS cursor
//     probe. This fragment draws whatever that state feed says (a handle's two dots, a selection
//     rectangle, a menu at a given x/y) and never hit-tests any of it itself.
//
// So this fragment measures nothing and decides nothing. It places panels where it is told and
// sends the presses back.

#define DEADLOCK_GRAPH_BODY_JS R"CFJS(
(function () {
  if ($.MvmGraphBody) return;
  var M = $.MvmStyle;
  if (!M) return;
  var S = M.S;
  var D = $.MvmDraw;

  var wrap = null, header = null;
  var left = null, ruler = null, canvas = null, footer = null;
  var gridPool = null, curvePool = null, keyPool = null, rulerPool = null, valuePool = null;
  var handleLinePool = null, handleDotPool = null, selectRect = null;
  var playhead = null, phPin = null;
  var ctxMenu = null, ctxMenuLabels = null;
  var speedDial = null;
  var rows = [];
  var driveBtn = null, smoothBtn = null, linearBtn = null, undoBtn = null, redoBtn = null, deleteBtn = null;
  var hTick = null, hTitle = null, gPlayIco = null, fRange = null;
  var C_RULE = '#44444480';
  var geom = { x: 0, y: 0, w: 0, h: 0, canvasX: 0, canvasY: 0, canvasW: 0, canvasH: 0 };

  var CHANNEL_COUNT = 7;
  // CS2's graph geometry (Panorama/GraphEditorJs.h): a 64px header, a 236px channel column, a
  // 26px ruler, a 26px footer; rows 34px from RULER_H + 24, a 64px value cell and a 24px eye.
  // DeadlockGraph.cpp hit-tests the value cell against the SAME numbers (kHeaderHeight...).
  var HEADER_H = 64, LEFTW = 236, RULER_H = 26, FOOTER_H = 26;
  var ROW_H = 34, ROWS_Y0 = RULER_H + 24, VALW = 64, EYEW = 24;
  var KF_R = 9, KF_R_SEL = 12;

  function cmd(text) { M.cmd(text); }

  // The speed mapping lives in the dial ($.MvmSpeedDial), hosted here as in CS2's graph editor.

  // The dock's bevel, at the graph toolbar's size. Same family as the camera strip's buttons and
  // Deadlock's own .ButtonBevel: a vertical gradient inside a 1px rule, 8px radius, letter-spaced
  // caption over a hard black shadow. The flat 3px pill this used to draw belonged to no family at
  // all, which is why the toolbar read as a row of grey slabs.
  function grad(top, bottom) {
    return 'gradient( linear, 0% 0%, 0% 100%, from( ' + top + ' ), to( ' + bottom + ' ) )';
  }

  function bevel(parent, id, caption, onClick, opts) {
    opts = opts || {};
    var b = M.make(parent, 'Panel', id, {
      width: 'fit-children', padding: opts.small ? '3px 10px' : '7px 12px',
      borderRadius: opts.small ? '6px' : '8px',
      border: '1px solid ' + (opts.border || S.border),
      backgroundColor: grad(S.btnTop, S.btnBottom),
      verticalAlign: 'middle',
      marginRight: (opts.gap === undefined ? 6 : opts.gap) + 'px',
      marginLeft: (opts.gapL || 0) + 'px',
      transition: 'background-color 0.12s ease-out, border-color 0.12s ease-out'
    });
    if (!b) return null;
    var l = M.label(b, '', caption, {
      color: opts.color || S.text, fontSize: '12px', letterSpacing: '1.4px',
      textShadow: S.textShadowHard, whiteSpace: 'nowrap', verticalAlign: 'middle'
    });
    if (l) l.hittest = false;
    b.__lbl = l;
    if (onClick) {
      try {
        b.SetPanelEvent('onactivate', onClick);
        b.SetPanelEvent('onmouseover', function () {
          M.set(b, { backgroundColor: grad(S.btnTopHover, S.btnBottomHover) });
        });
        b.SetPanelEvent('onmouseout', function () {
          M.set(b, { backgroundColor: grad(S.btnTop, S.btnBottom) });
        });
      } catch (e) {}
    }
    return b;
  }

  // A 28px square of the same bevel carrying an icon instead of a caption.
  function iconBtn(parent, id, image, onClick, opts) {
    opts = opts || {};
    var b = M.make(parent, 'Panel', id, {
      width: '28px', height: '28px', borderRadius: '6px',
      border: '1px solid ' + S.border,
      backgroundColor: grad(S.btnTop, S.btnBottom),
      verticalAlign: 'middle',
      marginRight: (opts.gap === undefined ? 6 : opts.gap) + 'px',
      transition: 'background-color 0.12s ease-out'
    });
    if (!b) return null;
    var ico = M.make(b, 'Panel', '', {
      width: '14px', height: '14px',
      backgroundImage: 'url("' + image + '")',
      backgroundSize: 'contain', backgroundRepeat: 'no-repeat', backgroundPosition: 'center',
      horizontalAlign: 'center', verticalAlign: 'middle'
    });
    if (ico) ico.hittest = false;
    b.__ico = ico;
    if (onClick) {
      try {
        b.SetPanelEvent('onactivate', onClick);
        b.SetPanelEvent('onmouseover', function () {
          M.set(b, { backgroundColor: grad(S.btnTopHover, S.btnBottomHover) });
        });
        b.SetPanelEvent('onmouseout', function () {
          M.set(b, { backgroundColor: grad(S.btnTop, S.btnBottom) });
        });
      } catch (e) {}
    }
    return b;
  }

  // ---- header ---------------------------------------------------------------------------------
  // CS2's layout (GraphEditorJs.h ~255-292): the title and the live readout on the LEFT, a two-row
  // cluster anchored RIGHT -- the playback speed dial on top, the graph's own Smooth / Linear /
  // Undo / Redo / Delete Key below it. No transport (the dock's timeline and the replay bar own
  // playback), no Seed (the graph seeds itself from the path) and no Drive (it is decided when the
  // graph tab opens, from the cursor mode, as CS2's OnEnter does -- DeadlockGraph.cpp).
  function buildHeader() {
    header = M.make(wrap, 'Panel', 'MvmGraphHeader', {
      position: '0px 0px 0px', width: '100%', height: HEADER_H + 'px',
      flowChildren: 'right', padding: '0px 14px',
      backgroundColor: S.panelBgDeep, borderBottom: '1px solid ' + S.border
    });
    if (!header) return;

    hTitle = M.label(header, 'MvmGraphTitle', 'GRAPH EDITOR', {
      color: S.text, fontSize: '15px', fontWeight: 'bold', letterSpacing: '2px',
      verticalAlign: 'middle', marginRight: '18px', whiteSpace: 'nowrap'
    });
    if (hTitle) hTitle.hittest = false;
    hTick = M.label(header, 'MvmGraphTick', '', {
      color: S.textMuted, fontSize: '11px', letterSpacing: '0.5px',
      verticalAlign: 'middle', width: 'fill-parent-flow(1.0)', whiteSpace: 'nowrap'
    });
    if (hTick) hTick.hittest = false;

    var rightCol = M.make(header, 'Panel', '', { flowChildren: 'down', verticalAlign: 'middle' });
    var rowTop = M.make(rightCol, 'Panel', '', {
      flowChildren: 'right', horizontalAlign: 'right', marginBottom: '5px'
    });
    var rowBot = M.make(rightCol, 'Panel', '', { flowChildren: 'right', horizontalAlign: 'right' });

    // The one playback rate, as a VIEW: the same $.MvmSpeedDial module the timeline and the replay
    // bar host, at CS2's graph-header size (buildSpeedScrubber(rowTop, { width: 250, height: 30 })).
    speedDial = $.MvmSpeedDial ? $.MvmSpeedDial.build(rowTop, { width: 250, height: 30, id: 'MvmGraphSpeed' }) : null;
    if (speedDial && speedDial.strip) M.set(speedDial.strip, { marginRight: '0px' });

    // Plain captions: the wave glyph CS2 puts on Smooth is not in Deadlock's font.
    smoothBtn = bevel(rowBot, 'MvmGraphSmooth', 'SMOOTH', function () {
      cmd('mirv_filmmaker graph smooth');
    }, { small: true });
    linearBtn = bevel(rowBot, 'MvmGraphLinear', 'LINEAR', function () {
      cmd('mirv_filmmaker graph linear');
    }, { small: true });
    undoBtn = bevel(rowBot, 'MvmGraphUndo', 'UNDO', function () {
      cmd('mirv_filmmaker graph undo');
    }, { small: true });
    redoBtn = bevel(rowBot, 'MvmGraphRedo', 'REDO', function () {
      cmd('mirv_filmmaker graph redo');
    }, { small: true });
    // Dimmed in render() while nothing is selected, as CS2's reads st.selCount.
    deleteBtn = bevel(rowBot, 'MvmGraphDelete', 'DELETE KEY', function () {
      cmd('mirv_filmmaker graph delsel');
    }, { small: true, gap: 0 });
  }

)CFJS" R"CFJS(
  // ---- channel column -------------------------------------------------------------------------
  //
  // CS2's makeRow (GraphEditorJs.h ~297-334): each row at a FIXED y so its rect is known to the hit
  // tests, contents flowing right and centred -- swatch + name (the solo target), then the value
  // cell, then the eye. DeadlockGraph.cpp's PickValueCell assumes exactly this order and these
  // numbers (8px row padding, 6px value gap, 24px eye); change one file and not the other and the
  // drawn cell and the clickable one drift apart.
  function buildLeft() {
    left = M.make(wrap, 'Panel', 'MvmGraphChannels', {
      position: '0px ' + HEADER_H + 'px 0px', width: LEFTW + 'px',
      // S.panelBg, not panelBgDeep: the column docked above is panelBg, and CS2 reads as one
      // surface from that column straight through its channel list.
      backgroundColor: S.panelBg, borderRight: '1px solid ' + S.border
    });
    if (!left) return;
    left.hittest = false;

    // CS2's caption sits RULER_H + 4 down, level with the ruler band beside it, with no rule.
    var headBand = M.make(left, 'Panel', '', {
      position: '8px ' + (RULER_H + 4) + 'px 0px', width: (LEFTW - 16) + 'px', height: '16px'
    });
    if (headBand) headBand.hittest = false;
    var head = M.label(headBand, '', 'CHANNELS', {
      color: S.textMuted, fontSize: '11px', fontWeight: 'bold', letterSpacing: '2px',
      verticalAlign: 'middle'
    });
    if (head) head.hittest = false;

    for (var c = 0; c < CHANNEL_COUNT; ++c) {
      (function (index) {
        var y = ROWS_Y0 + index * ROW_H;
        var row = M.make(left, 'Panel', 'MvmGraphRow' + index, {
          position: '0px ' + y + 'px 0px', width: LEFTW + 'px', height: (ROW_H - 4) + 'px',
          flowChildren: 'right', padding: '0px 8px'
        });
        if (!row) return;
        row.hittest = false;

        var hot = M.make(row, 'Panel', '', {
          width: 'fill-parent-flow(1.0)', height: '100%', flowChildren: 'right', borderRadius: '3px'
        });
        if (hot) {
          try {
            hot.hittest = true;
            hot.SetPanelEvent('onactivate', function () {
              cmd('mirv_filmmaker graph solo ' + index);
            });
          } catch (e) {}
        }
        var sw = M.make(hot, 'Panel', '', {
          width: '12px', height: '12px', borderRadius: '3px',
          verticalAlign: 'middle', marginLeft: '2px', marginRight: '9px'
        });
        if (sw) sw.hittest = false;
        var nm = M.label(hot, '', '', {
          color: S.text, fontSize: '12px', verticalAlign: 'middle', whiteSpace: 'nowrap'
        });
        if (nm) nm.hittest = false;
        var val = M.label(row, '', '-', {
          color: S.text, fontFamily: S.fontMono, fontSize: '12px', fontWeight: 'bold',
          width: VALW + 'px', textAlign: 'right', verticalAlign: 'middle',
          whiteSpace: 'nowrap', marginRight: '6px'
        });
        if (val) val.hittest = false;

        // Show / hide is its own target; solo is the swatch + name. Two verbs, two targets.
        var eye = M.make(row, 'Panel', '', {
          width: EYEW + 'px', height: '20px', verticalAlign: 'middle', borderRadius: '3px',
          backgroundColor: '#ffffff0f'
        });
        var eyeL = null;
        if (eye) {
          eyeL = M.make(eye, 'Panel', '', {
            width: '8px', height: '8px', borderRadius: '4px',
            backgroundColor: S.textMuted,
            horizontalAlign: 'center', verticalAlign: 'middle'
          });
          if (eyeL) eyeL.hittest = false;
          try {
            eye.hittest = true;
            eye.SetPanelEvent('onactivate', function () {
              cmd('mirv_filmmaker graph channel ' + index);
            });
          } catch (e) {}
        }
        rows.push({ row: row, hot: hot, sw: sw, nm: nm, val: val, eye: eye, eyeL: eyeL });
      })(c);
    }
  }

)CFJS" R"CFJS(
  // ---- ruler, canvas, footer -------------------------------------------------------------------
  function buildCanvas() {
    ruler = M.make(wrap, 'Panel', 'MvmGraphRuler', {
      position: LEFTW + 'px ' + HEADER_H + 'px 0px', height: RULER_H + 'px',
      backgroundColor: S.offBlack + '66', borderBottom: '1px solid ' + C_RULE,
      overflow: 'clip'
    });
    if (ruler) ruler.hittest = false;
    rulerPool = D ? D.pool(ruler, 'Label', 32) : null;

    // CLIPPED. A curve is sampled across the whole shot and drawn as rotated segments, and a
    // segment whose far end lies past the plot is still a panel: unclipped, the curves ran out of
    // the canvas and across the inspector column beside it.
    canvas = M.make(wrap, 'Panel', 'MvmGraphCanvas', {
      position: LEFTW + 'px ' + (HEADER_H + RULER_H) + 'px 0px',
      backgroundColor: 'rgba(0,0,0,0.18)',
      overflow: 'clip'
    });
    if (!canvas) return;
    canvas.hittest = false;

    // Pools, drawn in this order so later layers sit over earlier ones. One pool per layer rather
    // than one per channel: a channel that is hidden this frame simply does not claim any panels,
    // and pool.done() collapses whatever it did not claim. Handles are claimed AFTER keys (both in
    // this creation order and in render()'s draw order) so a selected key's arms sit on top of its
    // own diamond and every other channel's curve, same as CS2's kfLayer-then-handleLayer stack.
    gridPool = D ? D.pool(canvas, 'Panel', 96) : null;
    curvePool = D ? D.pool(canvas, 'Panel', 768) : null;
    keyPool = D ? D.pool(canvas, 'Panel', 256) : null;
    handleLinePool = D ? D.pool(canvas, 'Panel', 32) : null;
    handleDotPool = D ? D.pool(canvas, 'Panel', 32) : null;
    valuePool = D ? D.pool(canvas, 'Label', 16) : null;

    // The rectangle drag-select. One reusable panel, not a pool -- only one can ever be active.
    // C++ (DeadlockGraph.cpp's SelectKeysInRect gesture) owns the hit-testing; this only draws
    // wherever the state feed's normalised graph.rect says the two corners currently are.
    selectRect = M.make(canvas, 'Panel', 'MvmGraphSelectRect', {
      position: '0px 0px 0px', backgroundColor: S.gold + '22', border: '1px solid ' + S.gold,
      zIndex: 5
    });
    if (selectRect) { selectRect.hittest = false; selectRect.visible = false; }

    playhead = M.make(canvas, 'Panel', 'MvmGraphPlayhead', {
      width: '2px', height: '100%', backgroundColor: S.warn, position: '0px 0px 0px', zIndex: 6
    });
    if (playhead) playhead.hittest = false;

    // A grab pin in the ruler above the line, so where to press to scrub is not a guess.
    phPin = M.make(wrap, 'Panel', 'MvmGraphPhPin', {
      width: '13px', height: '10px', backgroundColor: S.warn, borderRadius: '2px',
      position: '0px 0px 0px', zIndex: 7
    });
    if (phPin) phPin.hittest = false;

    buildContextMenu();

    footer = M.make(wrap, 'Panel', 'MvmGraphFooter', {
      position: '0px 0px 0px', width: '100%', height: FOOTER_H + 'px',
      flowChildren: 'right',
      backgroundColor: S.offBlack + '66', borderTop: '1px solid ' + C_RULE, padding: '0px 12px'
    });
    if (footer) {
      footer.hittest = false;
      // WHAT THE POINTER DOES HERE, on the left; WHAT THE AXIS MEANS, on the right. The old single
      // line listed five gestures separated by dashes and ran past the width it had. This one now
      // covers every gesture CS2's own footer hint does (GraphEditorJs.h's footerHintLbl) except
      // "click value = type": Deadlock's value cell scrubs but has no console command to commit a
      // typed number through (see DeadlockGraph.cpp's PickValueCell doc comment / this pass's
      // report for the exact gap).
      var f = M.label(footer, '', 'click name = solo \u00b7 eye = show/hide \u00b7 '
        + 'drag a key or handle \u00b7 drag value = scrub \u00b7 box-select \u00b7 ctrl+A = all '
        + '\u00b7 shift = straighten/lock \u00b7 right-click = ease/align', {
        color: S.textWarm, fontSize: '11px',
        verticalAlign: 'middle', whiteSpace: 'nowrap', textOverflow: 'ellipsis',
        width: 'fill-parent-flow(1.0)'
      });
      if (f) f.hittest = false;
      fRange = M.label(footer, 'MvmGraphRange', '', {
        color: S.textWarm, fontFamily: S.fontMono, fontSize: '11px',
        verticalAlign: 'middle', whiteSpace: 'nowrap'
      });
      if (fRange) fRange.hittest = false;
    }
  }

  // ---- right-click menu -------------------------------------------------------------------------
  //
  // Ease In / Ease Out / Ease In & Out / Align Keys / Delete, ported from CS2's own right-click
  // menu (GraphEditorJs.h's showEaseMenu, ~lines 545-568). $.MvmControls.contextMenuShell was
  // considered (it is this codebase's shared right-click shell) but is NOT used here: its place()
  // flips the panel above the anchor when opening below would run off screen, which is exactly
  // right for a menu whose OWN click handling lives in JavaScript, and exactly wrong for one whose
  // hit-testing lives in DeadlockGraph.cpp's ContextMenuHit -- C++ has no way to learn the shell
  // chose the flipped position, so a menu opened near the bottom of the dock would draw in one
  // place and answer clicks as if it were in another. This panel is instead drawn at EXACTLY the
  // x/y DeadlockGraph.cpp's OpenContextMenu already clamped to the dock, with no placement logic
  // of its own -- the same "JS draws where it is told" contract the rest of this file follows --
  // styled with the same tokens (panelBgDeep/border/boxShadowFloat) the shell itself uses.
  function buildContextMenu() {
    ctxMenu = M.make(wrap, 'Panel', 'MvmGraphMenu', {
      position: '0px 0px 0px', width: '152px', flowChildren: 'down',
      backgroundColor: S.panelBgDeep, border: '1px solid ' + S.gold, borderRadius: '8px',
      boxShadow: S.boxShadowFloat, zIndex: 20
    });
    if (!ctxMenu) return;
    ctxMenu.hittest = false;
    ctxMenu.visible = false;
    var head = M.make(ctxMenu, 'Panel', '', { width: '100%', height: '22px', padding: '0px 10px' });
    if (head) {
      head.hittest = false;
      var headL = M.label(head, '', 'SELECTION', {
        color: S.textMuted, fontSize: '10px', letterSpacing: '1.6px', verticalAlign: 'middle'
      });
      if (headL) headL.hittest = false;
    }
    // Same 5 items, same order as CS2: Ease In, Ease Out, Ease In & Out, Align Keys, Delete.
    var labels = ['Ease In', 'Ease Out', 'Ease In / Out', 'Align Keys', 'Delete'];
    ctxMenuLabels = [];
    for (var mi = 0; mi < labels.length; ++mi) {
      var mrow = M.make(ctxMenu, 'Panel', '', { width: '100%', height: '26px', padding: '0px 10px' });
      if (mrow) mrow.hittest = false;
      var mlbl = M.label(mrow, '', labels[mi], {
        color: (mi === 4 ? S.danger : S.text), fontSize: '13px', verticalAlign: 'middle'
      });
      if (mlbl) mlbl.hittest = false;
      ctxMenuLabels.push(mlbl);
    }
  }

  function build(parent) {
    wrap = M.make(parent, 'Panel', 'MvmGraphBody', {
      horizontalAlign: 'left', verticalAlign: 'top',
      backgroundColor: S.panelBgDeep
    });
    if (!wrap) return;
    wrap.visible = false;
    buildHeader();
    buildLeft();
    buildCanvas();
  }

  function setVisible(v) { M.vis(wrap, v); }

  // The dock hands over its rectangle; the canvas rect inside it is C++'s, published in the state,
  // so the panel and the hit tests cannot disagree about where the plot is.
  // THE DOCK'S WIDTH IS THE DOCK'S, NOT THE SCREEN'S. The camera strip and the graph stop where
  // the inspector column starts, and the render workspace's JOB column must NOT narrow them --
  // it sits above the dock, not beside it. Taking the screen width here drew the right-hand
  // controls underneath the column, where they still took clicks but could not be seen.
  function layout(g, cursorOn, bodyH, dockW) {
    if (!wrap || !g) return;
    geom.w = Math.floor((dockW > 0) ? dockW : g.screenW);
    geom.h = Math.floor(Math.max(0, bodyH || 0));
    M.set(wrap, {
      position: '0px 0px 0px', width: geom.w + 'px', height: geom.h + 'px'
    });
    try { wrap.hittest = !!cursorOn; } catch (e) {}

    var canvasH = Math.max(16, geom.h - HEADER_H - RULER_H - FOOTER_H);
    var canvasW = Math.max(16, geom.w - LEFTW);
    geom.canvasX = LEFTW;
    geom.canvasY = HEADER_H + RULER_H;
    geom.canvasW = canvasW;
    geom.canvasH = canvasH;

    M.set(left, { height: Math.max(0, geom.h - HEADER_H - FOOTER_H) + 'px' });
    M.set(ruler, { width: canvasW + 'px' });
    M.set(canvas, { width: canvasW + 'px', height: canvasH + 'px' });
    M.set(footer, { position: '0px ' + (geom.h - FOOTER_H) + 'px 0px' });
  }

  // ---- drawing ---------------------------------------------------------------------------------
  function drawGrid(st) {
    if (!gridPool || !rulerPool) return;
    var w = geom.canvasW, h = geom.canvasH;

    // Vertical time lines, labelled in the ruler above. The span comes from the shot, so the
    // numbers on the ruler are ticks you can type into a seek.
    var first = st.startTick || 0;
    var last = Math.max((st.endTick || 0), first + 1);
    var span = last - first;
    var step = D.niceStep(span / 8);
    for (var t = Math.ceil(first / step) * step; t <= last; t += step) {
      var x = (t - first) / span * w;
      if (x < 0 || x > w) continue;
      var gl = gridPool.next();
      if (gl) D.rect(gl, x, 0, 1, h, '#ffffff12');
      var rl = rulerPool.next({
        position: Math.floor(x + 2) + 'px 3px 0px',
        color: S.textMuted, fontSize: '10px', fontFamily: S.fontNumeric
      });
      M.txt(rl, '' + Math.round(t));
    }

    // Five horizontal rules, the middle one brighter: a curve's height is read against them.
    for (var i = 0; i <= 4; ++i) {
      var y = i / 4 * h;
      var hl = gridPool.next();
      if (hl) D.rect(hl, 0, y, w, 1, i === 2 ? '#ffffff22' : '#ffffff12');
    }
    gridPool.done();
    rulerPool.done();
  }
)CFJS" R"CFJS(  // The active channel's real units down the left edge of the plot, so the curve has a scale.
  function drawValueAxis(channels) {
    if (!valuePool) return;
    var active = null;
    for (var i = 0; i < channels.length; ++i) {
      if (channels[i] && channels[i].visible) { active = channels[i]; break; }
    }
    if (!active || typeof active.lo !== 'number' || typeof active.hi !== 'number') {
      valuePool.done();
      return;
    }
    for (var k = 0; k <= 4; ++k) {
      var y = k / 4 * geom.canvasH;
      // The plot's y runs downward and the value runs upward, so the top rule is the HIGH value.
      var v = active.hi + (active.lo - active.hi) * (k / 4);
      var l = valuePool.next({
        position: '3px ' + Math.floor(y + 1) + 'px 0px',
        color: S.textMuted, fontSize: '9px', fontFamily: S.fontNumeric
      });
      if (!l) break;
      M.txt(l, v.toFixed(1));
    }
    valuePool.done();
  }

  function drawChannel(spec) {
    var w = geom.canvasW, h = geom.canvasH;
    var pts = spec.pts || [];
    // pts is a flat [t0, v0, t1, v1, ...] pair list; flat is deliberate, it halves the JSON.
    var px = -1, py = -1;
    for (var i = 0; i + 1 < pts.length; i += 2) {
      var X = pts[i] * w, Y = pts[i + 1] * h;
      if (px >= 0) {
        var p = curvePool.next();
        if (!p) break;
        D.seg(p, px, py, X, Y, spec.color, 2);
      }
      px = X; py = Y;
    }
  }

  function drawKeys(spec) {
    var w = geom.canvasW, h = geom.canvasH;
    var keys = spec.keyPts || [];
    for (var k = 0; k < keys.length; ++k) {
      var p = keyPool.next();
      if (!p) break;
      var sel = !!keys[k].sel;
      D.diamond(p, keys[k].t * w, keys[k].v * h,
                sel ? KF_R_SEL : KF_R, sel ? S.text : spec.color, S.offBlack + 'ff');
    }
  }

  // Bezier tangent handles for the SELECTED keys only, ported from CS2's drawHandles
  // (GraphEditorJs.h ~lines 746-760): an arm to each side plus a small round dot, drawn OVER the
  // keyframe diamonds (see the pool creation order in buildCanvas). DeadlockGraph.cpp only emits
  // lt/lv/rt/rv for a selected key -- GraphExpModel's own control points, the same ones
  // SampleChannel bends the curve with, so a dragged handle and the curve it shapes are always the
  // same handle. Nothing is drawn (and nothing claims a pool slot) when the model has no active
  // handle to show, which is most keys, most of the time.
  function drawHandles(spec) {
    var w = geom.canvasW, h = geom.canvasH;
    var keys = spec.keyPts || [];
    for (var k = 0; k < keys.length; ++k) {
      var kp = keys[k];
      if (!kp.sel || kp.lt === undefined) continue;
      var kx = kp.t * w, ky = kp.v * h;
      var lx = kp.lt * w, ly = kp.lv * h;
      var rx = kp.rt * w, ry = kp.rv * h;
      var l1 = handleLinePool.next();
      if (l1) D.seg(l1, kx, ky, lx, ly, S.text, 1);
      var l2 = handleLinePool.next();
      if (l2) D.seg(l2, kx, ky, rx, ry, S.text, 1);
      var d1 = handleDotPool.next();
      if (d1) D.dot(d1, lx, ly, 7, S.text);
      var d2 = handleDotPool.next();
      if (d2) D.dot(d2, rx, ry, 7, S.text);
    }
  }

  function render(st) {
    if (!wrap || !wrap.visible) return;
    st = st || {};
    var g = st.graph;
    if (!g) return;

    var channels = g.channels || [];

    // The shot's playback speed, pumped every frame this body is visible -- same raw-cursor pipe
    // every other gesture in the workspace runs on (st.mx/my/lmb), since this HUD context has no
    // mouse-move event of its own to drive a drag with.
    if (speedDial) {
      $.MvmSpeedDial.pump(speedDial, st);
      if (typeof st.speed === 'number') $.MvmSpeedDial.setSpeed(speedDial, st.speed);
    }

    for (var i = 0; i < rows.length; ++i) {
      var spec = channels[i];
      var r = rows[i];
      if (!r || !spec) continue;
      // CS2 names the position channels "Pos X/Y/Z" (GraphEditorExperimentHud channel names); the
      // console keeps the short names C++ reports.
      M.txt(r.nm, ({ X: 'Pos X', Y: 'Pos Y', Z: 'Pos Z' })[spec.name] || spec.name);
      M.set(r.sw, { backgroundColor: spec.visible ? spec.color : 'rgba(255,255,255,0.16)' });
      // SOLO is its own accent, independent of shown/hidden (DeadlockGraph.cpp's Graph_SoloChannel
      // no longer reuses the eye flag to fake it -- see that file's comment). Matches CS2's own
      // `ch.solo ? S.accent : ...` row colouring.
      M.set(r.nm, { color: spec.solo ? S.gold : (spec.visible ? S.text : S.textMuted) });
      if (r.eyeL) M.set(r.eyeL, { backgroundColor: spec.visible ? spec.color : 'rgba(255,255,255,0.20)' });
      if (r.val) {
        try {
          r.val.text = (typeof spec.current === 'number') ? spec.current.toFixed(2) : '-';
        } catch (e) {}
      }
    }

    if (!(geom.canvasW > 0) || !(geom.canvasH > 0)) return;

    drawGrid(st);
    drawValueAxis(channels);

    if (curvePool && keyPool) {
      for (var c = 0; c < channels.length && c < CHANNEL_COUNT; ++c) {
        if (!channels[c] || !channels[c].visible) continue;
        drawChannel(channels[c]);
      }
      curvePool.done();
      for (var d = 0; d < channels.length && d < CHANNEL_COUNT; ++d) {
        if (!channels[d] || !channels[d].visible) continue;
        drawKeys(channels[d]);
      }
      keyPool.done();
      if (handleLinePool && handleDotPool) {
        for (var hIdx = 0; hIdx < channels.length && hIdx < CHANNEL_COUNT; ++hIdx) {
          if (!channels[hIdx] || !channels[hIdx].visible) continue;
          drawHandles(channels[hIdx]);
        }
        handleLinePool.done();
        handleDotPool.done();
      }
    }

    // The rectangle drag-select. graph.rect is already normalised into the plot exactly like a
    // curve's own pts, so it only ever needs multiplying by the measured canvas size.
    if (selectRect) {
      var rect = g.rect;
      if (rect && rect.active) {
        var rx0 = rect.x0 * geom.canvasW, ry0 = rect.y0 * geom.canvasH;
        var rx1 = rect.x1 * geom.canvasW, ry1 = rect.y1 * geom.canvasH;
        M.set(selectRect, {
          position: Math.floor(rx0) + 'px ' + Math.floor(ry0) + 'px 0px',
          width: Math.max(1, Math.floor(rx1 - rx0)) + 'px',
          height: Math.max(1, Math.floor(ry1 - ry0)) + 'px'
        });
        M.vis(selectRect, true);
      } else {
        M.vis(selectRect, false);
      }
    }

    // The right-click Ease/Align/Delete menu. graph.menu.x/y already arrive in this panel's own
    // local space (DeadlockGraph.cpp's Graph_AppendStateJson subtracts the dock's own origin), so
    // this draws at exactly the position C++ is hit-testing clicks against -- no placement logic
    // of its own (see buildContextMenu's doc comment for why that matters here).
    if (ctxMenu) {
      var menu = g.menu;
      if (menu && menu.open) {
        M.set(ctxMenu, { position: Math.floor(menu.x) + 'px ' + Math.floor(menu.y) + 'px 0px' });
        M.vis(ctxMenu, true);
      } else {
        M.vis(ctxMenu, false);
      }
    }

    var phx = (g.playhead || 0) * geom.canvasW;
    if (playhead) M.set(playhead, { position: Math.floor(phx - 1) + 'px 0px 0px' });
    if (phPin) {
      M.set(phPin, {
        position: Math.floor(geom.canvasX + phx - 6) + 'px ' + (HEADER_H + 4) + 'px 0px'
      });
    }

    // Smooth / Linear is the curve set's ONE interpolation mode, so the pair reads as a state the
    // way the timeline's does: whichever is in force takes the accent.
    var hasSel = (g.selection || 0) > 0;
    var sm = !!g.smooth;
    // A Smooth that cannot actually bend anything (too few keyframes -- see DeadlockGraph.cpp's
    // interpDegraded comment) sits DIM instead of lit, same idea as CS2's own st.interpDegraded
    // dimming its Smooth label: lit-but-inert reads as a bug, not as "working".
    var degraded = !!g.interpDegraded;
    if (smoothBtn) {
      M.set(smoothBtn, { border: '1px solid ' + (sm ? S.gold : S.border) });
      if (smoothBtn.__lbl) M.set(smoothBtn.__lbl, { color: sm ? (degraded ? S.textMuted : S.gold) : S.textMuted });
    }
    if (linearBtn) {
      M.set(linearBtn, { border: '1px solid ' + (sm ? S.border : S.gold) });
      if (linearBtn.__lbl) M.set(linearBtn.__lbl, { color: sm ? S.textMuted : S.gold });
    }
    // UNDO/REDO dim when there is nothing to act on -- a live disabled/enabled state neither button
    // had before (they always read as available even with an empty history).
    if (undoBtn && undoBtn.__lbl) M.set(undoBtn.__lbl, { color: g.canUndo ? S.text : S.textMuted });
    if (redoBtn && redoBtn.__lbl) M.set(redoBtn.__lbl, { color: g.canRedo ? S.text : S.textMuted });
    // Same dim-when-nothing-to-act-on rule, CS2 parity: GraphEditorJs.h's own Delete Key button
    // reads st.selCount the same way.
    if (deleteBtn && deleteBtn.__lbl) M.set(deleteBtn.__lbl, { color: hasSel ? S.danger : S.textMuted });

    var headTick = Math.round((st.playhead !== undefined) ? st.playhead : (st.tick || 0));
    // CS2's hInfo: the tick, and how many keys you have hold of (GraphEditorJs.h ~868).
    if (hTick) {
      M.txt(hTick, 'tick ' + headTick + (hasSel ? ('  \u00b7  ' + g.selection + ' selected') : ''));
    }
    // WHICH CHANNEL THE TITLE NAMES. The graph has no selected channel -- it has SOLO, and soloing
    // leaves exactly one visible. So when one channel is shown the title and the axis range speak
    // for it by name; when several are, they say how many, because a range quoted from one of seven
    // stacked curves is a number that belongs to a curve the reader cannot pick out.
    var shown = null, shownCount = 0;
    for (var v = 0; v < channels.length; ++v) {
      if (channels[v] && channels[v].visible) { shown = channels[v]; ++shownCount; }
    }
    if (fRange) {
      try {
        fRange.text = (shownCount === 1 && shown.lo !== undefined && shown.hi !== undefined)
          ? (shown.lo + ' \u2026 ' + shown.hi)
          : '';
      } catch (e) {}
    }
  }

  $.MvmGraphBody = {
    build: build,
    layout: layout,
    render: render,
    setVisible: setVisible
  };
})();
)CFJS"
