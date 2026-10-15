#pragma once

// The inspector column's control vocabulary: the card, its header, its sub-tabs, and the thirteen
// row kinds every card body is built out of.
//
// WHY THIS FILE EXISTS. See Docs/deadlock/ui-style.md and Docs/deadlock/README.md. The camera, config
// and render columns share the SAME shell with different contents -- one card grammar, one row
// grammar, one set of metrics. Three bodies each growing their own copy of a value tile is how the
// CS2 side ended up with three slightly different sliders, so the grammar lives here once and the
// bodies only describe what goes in it.
//
// WHAT A ROW IS. A row def is a plain object from one of the factory calls below; a card turns each
// into a widget with an `update()`. Structure is built ONCE -- a card's row list is static for a
// given sub-tab -- and every frame only writes text and colours, through M.set, which memoises. A
// panel rebuilt per frame is what saturates Panorama's vertex-buffer list (see DeadlockStyleJs.h).
//
// Value tiles use the same relative cursor gesture as CS2. The main-thread state feed already
// supplies mx/my/lmb/shift; no invisible native Slider or deferred value-change events are needed.
// One tile width covers its range, Shift is five times finer, with no step buttons.
//
// WRAPPING. Panorama has no flex-wrap. Tiles declare a pixel width and the card packs them into
// `line` panels, re-packing only when the column width changes.

#define DEADLOCK_CONTROLS_JS R"CFJS(
(function () {
  if ($.MvmControls) return;
  var M = $.MvmStyle;
  if (!M) return;
  var S = M.S;

  // Tokens that only the column uses. Everything shared already lives in S.
  var C = {
    tileBg:       '#1f1e19',
    actBg:        '#211f1a',
    cardOpen:     '#35332c',
    cardClosed:   '#2a2823',
    borderOpen:   '#5a5648',
    borderClosed: '#44444459',
    ruleFaint:    '#44444480',
    edgeOff:      '#3a3830',
    chevron:      '#6c6a5c',
    optOn:        '#70F8C1',
    tileHotBorder:'#FFED7959',
    tileHotBg:    '#3a3830',
    faint:        '#ffffff0d',
    faintMore:    '#ffffff0f'
  };

  // The per-axis fills on a value tile's 4px track. An axis colour says which way a number points
  // without spending a word on it.
  var AXIS = {
    x: S.danger, y: S.green, z: S.soul,
    pitch: S.gold, yaw: S.textWarm, roll: S.blue, fov: S.warn
  };

  var GAP = 10;          // between tiles on a line
  var TILE_MIN_H = '76px';

  function cmd(t) { M.cmd(t); }

  function txt(panel, t) {
    if (!panel) return;
    if (panel.__mvmText === t) return;
    try { panel.text = t; panel.__mvmText = t; } catch (e) {}
  }

  function show(panel, v) {
    if (!panel) return;
    var b = !!v;
    if (panel.__mvmShown === b) return;
    try { panel.visible = b; panel.__mvmShown = b; } catch (e) {}
  }

  function on(panel, ev, fn) {
    if (!panel || !fn) return;
    try { panel.SetPanelEvent(ev, fn); } catch (e) {}
  }

  // Deadlock's .ButtonBevel gradient, as a style string.
  function bevel(top, bottom) {
    return 'gradient( linear, 0% 0%, 0% 100%, from( ' + top + ' ), to( ' + bottom + ' ) )';
  }
  var BEVEL = bevel(S.btnTop, S.btnBottom);
  var BEVEL_HOT = bevel(S.btnTopHover, S.btnBottomHover);
  // The card header's action chip and the render footer use a warmer, gold-leaning bevel so they
  // read as the card's own button rather than as one more grey control.
  var BEVEL_ACCENT = bevel('#4a4a36', '#5c5a3c');

  function hoverBg(panel, base, hot) {
    on(panel, 'onmouseover', function () { M.set(panel, { backgroundColor: hot }); });
    on(panel, 'onmouseout', function () { M.set(panel, { backgroundColor: base }); });
  }

  // ---- tooltip layer --------------------------------------------------------------------------
  //
  // One panel for the whole column. It is positioned from the hovered row's own measured rect,
  // because a Panorama tooltip has no anchor of its own, and it is suppressed while a button is
  // held -- a tip that appears under the pointer mid-press is a tip you cannot read and cannot
  // dismiss.
  var tip = null, tipLabel = null, tipHost = null, tipHeld = false;
  var TIP_W = 326;   // the 300px box, its padding and border: the width before one is measured

  // Where `p` sits relative to `host`, in the virtual px a `position` style takes.
  //
  // WINDOW SPACE, NEVER actualxoffset. actualxoffset/actualyoffset are the offset from the panel's
  // own PARENT, so a tile nested card > group > row inside the scrolled column reports a few dozen
  // pixels, and a popup placed from that opened in the top-left corner of the screen instead of at
  // the control. GetPositionWithinWindow() is the drawn, scroll-aware window position; the host is
  // measured the same way so the subtraction stays in one space.
  function rectIn(p, host) {
    var sx = p.actualuiscale_x || 1, sy = p.actualuiscale_y || 1;
    var a = p.GetPositionWithinWindow();
    var o = (host && host.GetPositionWithinWindow) ? host.GetPositionWithinWindow() : null;
    return {
      x: (a.x - (o ? o.x : 0)) / sx, y: (a.y - (o ? o.y : 0)) / sy,
      w: (p.actuallayoutwidth || 0) / sx, h: (p.actuallayoutheight || 0) / sy,
      hostW: ((host && host.actuallayoutwidth) || 1920) / sx,
      hostH: ((host && host.actuallayoutheight) || 1080) / sy
    };
  }

  function buildTip(host) {
    tipHost = host;
    tip = M.make(host, 'Panel', 'MvmTip', {
      width: '300px',
      padding: '9px 12px 10px 12px',
      backgroundColor: 'rgba(46,44,39,0.95)',
      border: '1px solid ' + C.tileHotBorder,
      borderRadius: '6px',
      horizontalAlign: 'left',
      verticalAlign: 'top',
      zIndex: 260
    });
    if (!tip) return;
    tip.hittest = false;
    tip.visible = false;
    tipLabel = M.label(tip, '', '', { fontSize: '13px', color: S.text, width: '100%' });
  }

  function tipOff() { show(tip, false); }

  // CS2's placement: the tip's left edge on the hovered control, just below it; flipped above when
  // it would run off the bottom, pulled left when it would run off the right.
  function tipOn(anchor, text) {
    if (!tip || !text || tipHeld) return;
    txt(tipLabel, text);
    var r;
    try { r = rectIn(anchor, tipHost); } catch (e) { return; }
    var tipW = (tip.actuallayoutwidth || 0) / (anchor.actualuiscale_x || 1);
    if (!(tipW > 0)) tipW = TIP_W;
    // Estimated, not measured: the label has not laid the new text out yet, so a measured height
    // is the PREVIOUS tip's.
    var tipH = Math.ceil(String(text).length / 42) * 18 + 20;
    var x = Math.max(8, Math.min(r.x, r.hostW - 8 - tipW));
    var below = r.y + r.h + 6;
    var y = (below + tipH > r.hostH - 8) ? Math.max(8, r.y - tipH - 6) : below;
    M.set(tip, { position: Math.floor(x) + 'px ' + Math.floor(y) + 'px 0px' });
    show(tip, true);
  }

  function wireTip(panel, text) {
    if (!panel || !text) return;
    // The WHOLE tile, never only its label. A tip that fires from the label alone is a tip most
    // people never see, because the pointer lands on the number.
    on(panel, 'onmouseover', function () { tipOn(panel, text); });
    on(panel, 'onmouseout', tipOff);
  }

  function holdTip(v) { tipHeld = !!v; if (v) tipOff(); }

  // ---- row factories --------------------------------------------------------------------------
  //
  // Each returns a plain def. `kind` picks the builder; everything else is that kind's payload.

  function def(kind, o) {
    o = o || {};
    o.kind = kind;
    if (o.width === undefined) o.width = 0;      // 0 = full width
    return o;
  }

  // A value tile. min/max/dec/unit/axis/tip come from the caller's own table, so the ranges live
  // beside the controls that use them rather than in a second place that can disagree.
  var numberFocus = null, numberKeys = 0;
  function number(id, label, o) { return def('number', {id:id, label:label, min:o.min, max:o.max, onChange:o.onChange}); }
  function numberPump(st) {
    var keys = st.numberKeys || 0, pressed = keys & ~numberKeys; numberKeys = keys;
    if (!numberFocus) return;
    var rec = numberFocus;
    if (!st.uiMouse || st.workspace !== 'render' || (pressed & (1 << 12))) { numberFocus=null; rec.paint(); return; }
    if (pressed & (1 << 11)) {
      var v = Number(rec.buf);
      if (rec.buf !== '' && isFinite(v) && v >= rec.def.min && v <= rec.def.max) { rec.def.onChange(v); numberFocus=null; }
    } else if (pressed & (1 << 10)) rec.buf = rec.buf.slice(0,-1);
    else for(var i=0;i<10;i++) if ((pressed & (1<<i)) && rec.buf.length<6) rec.buf+=String(i);
    rec.paint();
  }
  function buildNumber(parent, d) {
    var root = M.make(parent, 'Panel', 'MvmNumber_' + d.id, {width:'100%', flowChildren:'down', marginBottom:'10px', backgroundColor:S.offBlack, padding:'6px'});
    M.label(root, '', d.label, {fontSize:'12px', color:S.textMuted, marginBottom:'4px'});
    var label = M.label(root, '', '', {width:'100%', height:'24px', color:S.text, fontSize:'16px'});
    var rec={def:d,buf:'',value:0,paint:function(){txt(label,numberFocus===rec ? rec.buf+'|' : String(rec.value));}};
    on(root,'onactivate',function(){var old=numberFocus;numberFocus=rec;rec.buf='';if(old)old.paint();rec.paint();});
    wireTip(root,'Click, type a number, then Enter to apply. Escape cancels.');
    return {root:root,set:function(v){rec.value=v;rec.paint();}};
  }
  function scrub(id, label, o) {
    o = o || {};
    return def('scrub', {
      id: id, label: label,
      min: o.min || 0, max: (o.max === undefined ? 1 : o.max),
      dec: o.dec || 0, unit: o.unit || '',
      fill: AXIS[o.axis] || S.gold,
      tip: o.tip || '', width: o.width || 213,
      onChange: o.onChange
    });
  }
  function sw(id, label, o) {
    o = o || {};
    return def('switch', { id: id, label: label, tip: o.tip || '', width: o.width || 213, onToggle: o.onToggle });
  }
  function mode(id, label, items, o) {
    o = o || {};
    return def('mode', { id: id, label: label, items: items, tip: o.tip || '', segW: o.segW || 236, onPick: o.onPick });
  }
  function grid(id, label, items, perRow, o) {
    o = o || {};
    return def('grid', { id: id, label: label, items: items, perRow: perRow || 3,
      tip: o.tip || '', font: o.font || 14, onPick: o.onPick });
  }
  function drop(id, label, o) {
    o = o || {};
    return def('drop', { id: id, label: label, note: o.note || '', onOpen: o.onOpen });
  }
  function act(label, items, perRow, o) {
    o = o || {};
    return def('act', { label: label, items: items, perRow: perRow || 3, note: o.note || '' });
  }
  // A LADDER, not a slider (CS2's CameraEditorPathInspectorJs.h Amount row, the path Speed
  // multiplier): the useful values are a short fixed list, so +/- steps through them by nearest
  // neighbour rather than dragging a continuous range between them.
  function stepper(id, label, steps, o) {
    o = o || {};
    return def('stepper', { id: id, label: label, steps: steps, tip: o.tip || '', onChange: o.onChange });
  }
  function read(label, axes) { return def('read', { label: label, axes: axes }); }
  function key(keys, what) { return def('key', { txt: keys, label: what }); }
  function nav(o) {
    o = o || {};
    return def('nav', { txt: o.txt || '', onPrev: o.onPrev, onNext: o.onNext, tag: o.tag });
  }
  function note(text, tag) { return def('note', { label: text, tag: tag }); }
  function cap(text) { return def('cap', { label: text }); }
  function opt(group, key2, title, sub, o) {
    o = o || {};
    return def('opt', { group: group, key: key2, label: title, sub: sub, onPick: o.onPick });
  }
  function big(text, tag) { return def('big', { txt: text, tag: tag }); }
)CFJS" R"CFJS(
  // ---- row builders ---------------------------------------------------------------------------

  // A FIXED BOX, not a minimum.
  //
  // With `min-height` the tile grew to whatever its label needed, so "Release on death" and "Hold
  // last position" stood a row taller than the value tiles above them and the group lost its grid.
  // The spec draws every tile at one height; a label that will not fit is a label to shorten, not a
  // tile to stretch.
  function tileShell(parent, id, widthPx, cursorStyle) {
    var t = M.make(parent, 'Panel', id, {
      flowChildren: 'down',
      height: TILE_MIN_H,
      padding: '11px 13px 12px 13px',
      border: '1px solid ' + S.border,
      borderRadius: '6px',
      backgroundColor: C.tileBg,
      width: widthPx ? (widthPx + 'px') : '100%',
      transition: 'background-color 0.12s ease-out, border-color 0.12s ease-out'
    });
    if (t) {
      on(t, 'onmouseover', function () {
        M.set(t, { borderColor: C.tileHotBorder, backgroundColor: C.tileHotBg });
      });
      on(t, 'onmouseout', function () {
        M.set(t, { borderColor: S.border, backgroundColor: C.tileBg });
      });
    }
    return t;
  }

  var scrubs = [], scrubLastLmb = false, scrubHeld = null, scrubScope = null;

  function panelRect(p) {
    try {
      var a = p.GetPositionWithinWindow(), w = p.actuallayoutwidth, h = p.actuallayoutheight;
      if (!a || !(w > 0 && h > 0 && w < 1e30 && h < 1e30)) return null;
      return { x: a.x, y: a.y, w: w, h: h };
    } catch (e) { return null; }
  }
  function inside(r, x, y) { return !!r && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
  function scrubOnScreen(rec, x, y) {
    try {
      var inScope = !scrubScope;
      for (var p = rec.root, n = 0; p && n < 64; ++n) {
        if (p === scrubScope) inScope = true;
        if ((p.IsValid && !p.IsValid()) || !p.visible) return false;
        if (x !== undefined && p.__mvmValueViewport && !inside(panelRect(p), x, y)) return false;
        p = p.GetParent ? p.GetParent() : null;
      }
      return inScope;
    } catch (e) { return false; }
  }
  function setScrubScope(root) { cancelScrub(); scrubScope = root; scrubLastLmb = true; }
  function cancelScrub() {
    if (scrubHeld) scrubHeld.held = false;
    scrubHeld = null;
    holdTip(false);
  }
  function cursorPump(st) {
    numberPump(st);
    var live = st && st.open && st.uiMouse && st.layout && st.layout.valid && typeof st.mx === 'number';
    var down = !!(st && st.lmb), fine = !!(st && st.shift);
    if (!live) { cancelScrub(); scrubLastLmb = down; return; }
    var mx = st.mx, my = st.my;
    if (scrubHeld && (!scrubOnScreen(scrubHeld) || (menu && menu.visible))) cancelScrub();
    if (down && !scrubLastLmb && !(menu && menu.visible)) {
      for (var i = 0; i < scrubs.length; ++i) {
        var rec = scrubs[i], r = panelRect(rec.root);
        if (!scrubOnScreen(rec, mx, my) || !inside(r, mx, my)) continue;
        scrubHeld = rec; rec.held = true; rec.anchor = mx; rec.base = rec.get();
        rec.fine = fine; rec.moved = false; rec.width = r.w;
        holdTip(true);
        break;
      }
    }
    var held = scrubHeld;
    if (held) {
      if (Math.abs(mx - held.anchor) > 2) held.moved = true;
      if (held.fine !== fine) {
        // Changing precision mid-gesture must not jump back towards the original press.
        held.fine = fine; held.base = held.get(); held.anchor = mx;
      }
      if (held.moved) held.commit(held.base + (mx - held.anchor) / held.width * held.span * (fine ? 0.2 : 1));
    }
    if (!down) cancelScrub();
    scrubLastLmb = down;
  }

  function buildScrub(parent, d) {
    var t = tileShell(parent, 'MvmValue_' + d.id, d.width);
    if (!t) return null;
    // Overlay layout keeps the track inside the fixed-height tile. The old down-flow stack plus
    // a 100%-height invisible Slider pushed the track outside the clipped content box.
    M.set(t, { flowChildren: 'none', padding: '8px 10px', cursor: 'sizewe' });
    var lab = M.label(t, '', d.label, {
      fontSize: '12px', height: '16px', color: S.textMuted, width: '100%', textOverflow: 'ellipsis'
    });
    var val = M.label(t, '', '', {
      fontFamily: S.fontMono, fontSize: '21px', color: S.text, marginTop: '16px', width: '100%'
    });
    lab.hittest = false; val.hittest = false;
    var row = M.make(t, 'Panel', '', {
      height: '14px', width: '100%', verticalAlign: 'bottom', flowChildren: 'right'
    });
    var track = M.make(row, 'Panel', 'MvmValueTrack_' + d.id, {
      height: '14px', width: '100%'
    });
    M.make(track, 'Panel', '', {
      height: '4px', width: '100%', verticalAlign: 'middle', backgroundColor: S.offBlack
    });
    var fill = M.make(track, 'Panel', '', {
      height: '4px', backgroundColor: d.fill, width: '0%', verticalAlign: 'middle'
    });
    // The thumb is aligned at the filled region's right edge, so both endpoints stay in bounds.
    var thumbHost = M.make(track, 'Panel', '', { height: '14px', width: '0%', minWidth: '7px' });
    M.make(thumbHost, 'Panel', 'MvmValueThumb_' + d.id, {
      height: '10px', width: '7px', borderRadius: '2px', backgroundColor: S.text,
      horizontalAlign: 'right', verticalAlign: 'middle'
    });
    track.hittest = false; track.hittestchildren = false;
    var span = (d.max - d.min) || 1;
    function paint(value) {
      var frac = (value - d.min) / span;
      if (frac < 0) frac = 0; else if (frac > 1) frac = 1;
      txt(val, value.toFixed(d.dec) + d.unit);
      M.set(fill, { width: (frac * 100).toFixed(2) + '%' });
      M.set(thumbHost, { width: (frac * 100).toFixed(2) + '%' });
    }
    return bindValue(t, d, paint);
  }

  // The CS2-shaped keyframe menu uses compact meters, with the same drag rules as value tiles.
  function buildMeter(parent, d) {
    var t = M.make(parent, 'Panel', 'MvmValue_' + d.id, {
      width: '100%', height: '34px', flowChildren: 'right', marginBottom: '8px', cursor: 'sizewe'
    });
    var lab = M.label(t, '', d.label, { width: '68px', fontSize: '12px', color: '#c8d3dfff', verticalAlign: 'center' });
    lab.hittest = false;
    var box = M.make(t, 'Panel', '', { width: '212px', marginLeft: '32px', marginRight: '32px',
      verticalAlign: 'center', flowChildren: 'down' });
    box.hittest = false; box.hittestchildren = false;
    var val = M.label(box, '', '', { width: '100%', textAlign: 'center', fontSize: '12px',
      fontWeight: 'bold', color: '#eef2f6ff', marginBottom: '6px' });
    var track = M.make(box, 'Panel', 'MvmValueTrack_' + d.id, { width: '100%', height: '6px', flowChildren: 'right' });
    var cells = [], empty = true, number = d.min;
    for (var i = 0; i < 10; ++i) cells.push(M.make(track, 'Panel', '', {
      width: 'fill-parent-flow(1.0)', height: '100%', marginRight: (i < 9 ? 3 : 0) + 'px', borderRadius: '1px'
    }));
    function paint(v) {
      number = v; txt(val, empty ? '–' : v.toFixed(d.dec));
      var filled = empty ? 0 : Math.round(v / d.max * 10);
      for (var i = 0; i < cells.length; ++i) M.set(cells[i], { backgroundColor: i < filled ? d.fill : '#ffffff14' });
    }
    var change = d.onChange; d.onChange = function (v) { empty = false; paint(v); if (change) change(v); };
    var widget = bindValue(t, d, paint);
    widget.setEmpty = function (v) { if (empty !== !!v) { empty = !!v; paint(number); } };
    return widget;
  }

  function bindValue(t, d, paint) {
    var value = d.min, mul = Math.pow(10, d.dec), span = (d.max - d.min) || 1;
    var rec = { root: t, held: false, span: span };
    function quant(v) { return Math.max(d.min, Math.min(d.max, Math.round(v * mul) / mul)); }
    function commit(v) {
      if (!isFinite(v)) return;
      v = quant(v);
      if (v === value) return;
      value = v;
      paint(value);
      if (d.onChange) { try { d.onChange(value); } catch (e) {} }
    }
    on(t, 'onactivate', function () {});
    wireTip(t, (d.tip ? d.tip + ' ' : '') + 'Drag to adjust; Shift for finer control.');
    rec.commit = commit; rec.get = function () { return value; };
    scrubs.push(rec);
    paint(value);
    return { root: t, set: function (v) {
               if (rec.held || typeof v !== 'number' || !isFinite(v)) return;
               v = quant(v); if (v !== value) { value = v; paint(value); }
             },
             get: function () { return value; } };
  }

  function pill(parent, w, h) {
    var track = M.make(parent, 'Panel', '', {
      width: w + 'px', height: h + 'px', borderRadius: (h / 2) + 'px',
      backgroundColor: S.offBlack, border: '1px solid ' + S.border,
      horizontalAlign: 'left'
    });
    var k = h - 6;
    var knob = M.make(track, 'Panel', '', {
      width: k + 'px', height: k + 'px', borderRadius: (k / 2) + 'px',
      backgroundColor: S.text, horizontalAlign: 'left', verticalAlign: 'middle', marginLeft: '3px'
    });
    if (knob) knob.hittest = false;
    return {
      root: track,
      set: function (v) {
        M.set(track, {
          backgroundColor: v ? S.green : S.offBlack,
          border: '1px solid ' + (v ? '#3fbe7e' : S.border)
        });
        // marginLeft, not a transform: a transform gives the knob its own composited layer, and a
        // layer per switch per frame is the vertex-buffer overflow this codebase already paid for.
        M.set(knob, { marginLeft: (v ? (w - k - 3) : 3) + 'px' });
      }
    };
  }

  function buildSwitch(parent, d) {
    var t = tileShell(parent, '', d.width);
    if (!t) return null;
    M.label(t, '', d.label, { fontSize: '12px', lineHeight: '1.3', color: S.text, width: '100%' });
    var spacer = M.make(t, 'Panel', '', { width: '100%', height: 'fill-parent-flow(1.0)' });
    if (spacer) spacer.hittest = false;
    var p = pill(t, 40, 22);
    var value = false;
    p.set(false);
    on(t, 'onactivate', function () {
      value = !value;
      p.set(value);
      if (d.onToggle) { try { d.onToggle(value); } catch (e) {} }
    });
    wireTip(t, d.tip);
    return { root: t, set: function (v) { v = !!v; if (v !== value) { value = v; p.set(v); } } };
  }

  // A segmented control. Cells share one rounded clip, so it reads as one control rather than as
  // a row of buttons that happen to touch.
  function segment(parent, items, segWpx, onPick, accent) {
    var box = M.make(parent, 'Panel', '', {
      flowChildren: 'right', borderRadius: '4px', overflow: 'noclip',
      width: segWpx ? (segWpx + 'px') : '100%', verticalAlign: 'middle'
    });
    var cells = [];
    for (var i = 0; i < items.length; ++i) {
      (function (item) {
        var c = M.make(box, 'Panel', '', {
          width: 'fill-parent-flow(1.0)', padding: '8px 4px', verticalAlign: 'middle',
          border: '1px solid ' + S.border, backgroundColor: C.faint,
          transition: 'background-color 0.12s ease-out'
        });
        // nowrap is load-bearing: a fill-parent-flow cell has no width when its label is measured,
        // so a caption with a break opportunity ("Per-Seg", "Lock-on") reserved a second text line
        // and that one cell stood taller than its neighbours (the text still drew on one line).
        // Fixed height too: an unconstrained label is sized to its glyph ink, so "Per-Seg" (a
        // descender) came out taller than "Manual" beside it. Same rule the value tiles use.
        var l = M.label(c, '', item[1], {
          fontSize: '12px', height: '16px', color: S.textMuted, horizontalAlign: 'center',
          verticalAlign: 'middle', whiteSpace: 'nowrap'
        });
        on(c, 'onactivate', function () { if (onPick) { try { onPick(item[0]); } catch (e) {} } });
        cells.push({ key: item[0], panel: c, label: l });
      })(items[i]);
    }
    var acc = accent || S.gold;
    function set(k) {
      for (var j = 0; j < cells.length; ++j) {
        var hot = cells[j].key === k;
        M.set(cells[j].panel, {
          border: '1px solid ' + (hot ? acc : S.border),
          backgroundColor: hot ? (acc + '26') : C.faint
        });
        M.set(cells[j].label, { color: hot ? acc : S.textMuted });
      }
    }
    return { root: box, set: set };
  }

  function buildMode(parent, d, accent) {
    var row = M.make(parent, 'Panel', '', {
      width: '100%', flowChildren: 'right', padding: '11px 13px',
      border: '1px solid ' + S.border, borderRadius: '6px', backgroundColor: C.tileBg
    });
    if (!row) return null;
    M.label(row, '', d.label, {
      width: 'fill-parent-flow(1.0)', fontSize: '14px', color: S.text, verticalAlign: 'middle'
    });
    var seg = segment(row, d.items, d.segW, d.onPick, accent);
    wireTip(row, d.tip);
    return { root: row, set: seg.set };
  }
)CFJS" R"CFJS(
  // Panorama has no flex-wrap, so a grid is built as explicit rows of `perRow` cells. The last row
  // is padded with invisible spacers, or its cells stretch and the grid stops being a grid.
  function buildGrid(parent, d, accent) {
    var box = M.make(parent, 'Panel', '', {
      width: '100%', flowChildren: 'down', padding: '11px 13px 13px 13px',
      border: '1px solid ' + S.border, borderRadius: '6px', backgroundColor: C.tileBg
    });
    if (!box) return null;
    M.label(box, '', d.label, { fontSize: '12px', color: S.textMuted, width: '100%' });
    var wrap = M.make(box, 'Panel', '', { width: '100%', flowChildren: 'down', marginTop: '10px' });
    var cells = [], line = null, acc = accent || S.gold;
    for (var i = 0; i < d.items.length; ++i) {
      if (i % d.perRow === 0) {
        line = M.make(wrap, 'Panel', '', {
          width: '100%', flowChildren: 'right', marginBottom: '9px'
        });
      }
      (function (item, last) {
        var c = M.make(line, 'Panel', '', {
          width: 'fill-parent-flow(1.0)', padding: '11px 6px', borderRadius: '4px',
          border: '1px solid ' + S.border, backgroundColor: C.faint,
          marginRight: last ? '0px' : '9px'
        });
        var l = M.label(c, '', item[1], {
          fontSize: d.font + 'px', color: S.text,
          horizontalAlign: 'center', verticalAlign: 'middle'
        });
        on(c, 'onactivate', function () { if (d.onPick) { try { d.onPick(item[0]); } catch (e) {} } });
        cells.push({ key: item[0], panel: c, label: l });
      })(d.items[i], (i % d.perRow) === (d.perRow - 1));
    }
    var pad = (d.perRow - (d.items.length % d.perRow)) % d.perRow;
    for (var p = 0; p < pad; ++p) {
      var sp = M.make(line, 'Panel', '', { width: 'fill-parent-flow(1.0)', marginRight: '9px' });
      if (sp) sp.hittest = false;
    }
    wireTip(box, d.tip);
    return { root: box, set: function (k) {
      for (var j = 0; j < cells.length; ++j) {
        var hot = cells[j].key === k;
        M.set(cells[j].panel, {
          border: '1px solid ' + (hot ? acc : S.border),
          backgroundColor: hot ? (acc + '26') : C.faint
        });
        M.set(cells[j].label, { color: hot ? acc : S.text });
      }
    } };
  }

  // A collapsed bar with a divided arrow cell, matching Deadlock's .DropDownNew. The MENU is not
  // built here: it is one overlay owned by the column, so only one can ever be open and it can
  // draw over the card that spawned it.
  function buildDrop(parent, d) {
    var box = M.make(parent, 'Panel', '', { width: '100%', flowChildren: 'down' });
    if (!box) return null;
    // AS AUTHORED. The spec writes these in sentence case -- "Hero", "Camera", "This camera",
    // "What to render" -- and letterspaces them small and muted. Upper-casing them turned a quiet
    // caption into a second heading competing with the card title above it.
    M.label(box, '', d.label, {
      fontSize: '11px', letterSpacing: '1.8px', color: S.textMuted, marginBottom: '6px', width: '100%'
    });
    var bar = M.make(box, 'Panel', '', {
      width: '100%', height: '40px', flowChildren: 'right', borderRadius: '6px',
      backgroundColor: S.offBlack, border: '1px solid ' + S.border,
      transition: 'border-color 0.12s ease-out'
    });
    var cur = M.label(bar, '', '', {
      width: 'fill-parent-flow(1.0)', padding: '0px 12px', fontSize: '14px', color: S.text,
      verticalAlign: 'middle', textOverflow: 'ellipsis'
    });
    var caret = M.make(bar, 'Panel', '', {
      width: '36px', height: '100%', backgroundColor: C.faintMore,
      borderLeft: '1px solid ' + S.border
    });
    var caretIco = M.make(caret, 'Panel', '', {
      width: '10px', height: '10px',
      backgroundImage: 'url("' + S.icoArrowDown + '")',
      backgroundSize: 'contain', backgroundRepeat: 'no-repeat', backgroundPosition: 'center',
      horizontalAlign: 'center', verticalAlign: 'middle'
    });
    if (caretIco) caretIco.hittest = false;
    var sub = M.label(box, '', d.note, {
      fontSize: '11px', color: S.textWarm, marginTop: '6px', width: '100%'
    });
    show(sub, !!d.note);
    on(bar, 'onactivate', function () { if (d.onOpen) { try { d.onOpen(bar); } catch (e) {} } });
    on(bar, 'onmouseover', function () { M.set(bar, { borderColor: '#FFED7980' }); });
    on(bar, 'onmouseout', function () { M.set(bar, { borderColor: S.border }); });
    return {
      root: box,
      set: function (label) { txt(cur, label || ''); },
      setOpen: function (v) { M.set(bar, { border: '1px solid ' + (v ? S.gold : S.border) }); },
      setNote: function (t) { txt(sub, t || ''); show(sub, !!t); }
    };
  }

  // A named, bordered set of commands. A bare row of buttons cannot say what its buttons have in
  // common; the caption does.
  function buildAct(parent, d) {
    var box = M.make(parent, 'Panel', '', {
      width: '100%', flowChildren: 'down', padding: '11px 13px 13px 13px',
      border: '1px solid ' + S.border, borderRadius: '6px', backgroundColor: C.actBg
    });
    if (!box) return null;
    M.label(box, '', d.label, {
      fontSize: '11px', letterSpacing: '1.8px', color: S.textMuted, width: '100%'
    });
    var wrap = M.make(box, 'Panel', '', { width: '100%', flowChildren: 'down', marginTop: '10px' });
    var line = null;
    // Kept so set() below can show/hide ONE button without touching its neighbours -- CS2 parity
    // (CameraEditorFollowInspectorJs.h's followPlace/followClear.visible) needs Place and Clear to
    // hide together while Reposition, in the SAME row, stays put. Panorama collapses an invisible
    // flow child's width, so hiding one still lets the others re-flow rather than leaving a gap.
    var btns = [];
    for (var i = 0; i < d.items.length; ++i) {
      if (i % d.perRow === 0) {
        line = M.make(wrap, 'Panel', '', { width: '100%', flowChildren: 'right', marginBottom: '9px' });
      }
      (function (item, last) {
        var b = M.make(line, 'Panel', '', {
          width: 'fill-parent-flow(1.0)', padding: '10px 6px', borderRadius: '8px',
          border: '1px solid ' + S.border, backgroundColor: BEVEL,
          marginRight: last ? '0px' : '9px'
        });
        M.label(b, '', item.cap, {
          fontSize: '13px', letterSpacing: '1.2px', textTransform: 'uppercase', color: S.text,
          textShadow: '2px 2px 0px 1.0 #000000', horizontalAlign: 'center', verticalAlign: 'middle',
          width: '100%'
        });
        on(b, 'onactivate', function () {
          holdTip(false);
          if (item.run) { try { item.run(); } catch (e) {} }
          else if (item.cmd) cmd(item.cmd);
        });
        on(b, 'onmousedown', function () { holdTip(true); });
        hoverBg(b, BEVEL, BEVEL_HOT);
        btns.push(b);
      })(d.items[i], (i % d.perRow) === (d.perRow - 1));
    }
    var pad = (d.perRow - (d.items.length % d.perRow)) % d.perRow;
    for (var p = 0; p < pad; ++p) {
      var sp = M.make(line, 'Panel', '', { width: 'fill-parent-flow(1.0)', marginRight: '9px' });
      if (sp) sp.hittest = false;
    }
    var note2 = M.label(box, '', d.note, {
      fontSize: '11px', color: S.textWarm, marginTop: '9px', width: '100%'
    });
    show(note2, !!d.note);
    return {
      root: box,
      // Optional per-button visibility: set([true, false, true]) hides just item 1 (0-based).
      // Omitted or called with nothing keeps every button as built -- existing callers that never
      // pass an array are unaffected.
      set: function (visibleFlags) {
        if (!visibleFlags) return;
        for (var k = 0; k < btns.length; ++k) show(btns[k], visibleFlags[k] !== false);
      }
    };
  }

  // Label + [-] value [+], all in one bordered row. Steps to the nearest value in d.steps rather
  // than dragging, per CS2's own reasoning (CameraEditorPathInspectorJs.h's stepMul): a handful of
  // useful values do not deserve a continuous range to hunt through.
  function buildStepper(parent, d) {
    var box = M.make(parent, 'Panel', '', {
      width: '100%', flowChildren: 'right', padding: '11px 13px',
      border: '1px solid ' + S.border, borderRadius: '6px', backgroundColor: C.actBg,
      verticalAlign: 'middle'
    });
    if (!box) return null;
    var lab = M.label(box, '', d.label, {
      fontSize: '13px', color: S.text, verticalAlign: 'middle', width: 'fill-parent-flow(1.0)'
    });
    lab.hittest = false;
    var steps = d.steps && d.steps.length ? d.steps : [1];
    var value = steps[0];
    function nearestIndex(v) {
      var idx = 0, best = 1e9;
      for (var i = 0; i < steps.length; ++i) {
        var diff = Math.abs(steps[i] - v);
        if (diff < best) { best = diff; idx = i; }
      }
      return idx;
    }
    // CS2's geometry (CameraEditorPathInspectorJs.h pathMulRow): 44px buttons, a 150px bold readout
    // between them, 8px apart. `enabled` greys the readout and makes the buttons inert, the way CS2
    // does in Manual where there is no multiplier to change.
    var enabled = true;
    function makeBtn(caption, delta) {
      var b = M.make(box, 'Panel', '', {
        width: '44px', padding: '7px 0px', borderRadius: '6px', verticalAlign: 'middle',
        border: '1px solid ' + S.border, backgroundColor: BEVEL
      });
      M.label(b, '', caption, {
        fontSize: '16px', height: '20px', color: S.text, horizontalAlign: 'center', textAlign: 'center',
        verticalAlign: 'middle', width: '100%'
      });
      on(b, 'onactivate', function () {
        if (!enabled) return;
        var idx = nearestIndex(value) + delta;
        if (idx < 0) idx = 0; else if (idx >= steps.length) idx = steps.length - 1;
        if (d.onChange) { try { d.onChange(steps[idx]); } catch (e) {} }
      });
      hoverBg(b, BEVEL, BEVEL_HOT);
      return b;
    }
    var minusBtn = makeBtn('−', -1);
    var val = M.label(box, '', '', {
      fontFamily: S.fontMono, fontSize: '17px', fontWeight: 'bold', color: S.text, width: '150px',
      textAlign: 'center', verticalAlign: 'middle', marginLeft: '8px', marginRight: '8px', whiteSpace: 'nowrap'
    });
    val.hittest = false;
    var plusBtn = makeBtn('+', +1);
    wireTip(box, d.tip || '');
    // `text` replaces the default "x1.00" readout (CS2 writes "Constant  x1.00", "Segment  x1.00",
    // "not used in Manual"); `en` defaults to true.
    function paint(v, text, en) {
      value = v;
      enabled = (en === undefined) ? true : !!en;
      txt(val, text !== undefined ? text : ('x' + v.toFixed(2)));
      M.set(val, { color: enabled ? S.text : S.textMuted });
      if (minusBtn) M.set(minusBtn, { opacity: enabled ? '1' : '0.45' });
      if (plusBtn) M.set(plusBtn, { opacity: enabled ? '1' : '0.45' });
    }
    paint(value);
    return { root: box, set: function (v, text, en) { if (typeof v === 'number' && isFinite(v)) paint(v, text, en); } };
  }

  function buildRead(parent, d) {
    var row = M.make(parent, 'Panel', '', {
      width: '100%', flowChildren: 'right', padding: '11px 13px',
      border: '1px solid ' + S.border, borderRadius: '6px', backgroundColor: S.offBlack
    });
    if (!row) return null;
    M.label(row, '', d.label, {
      width: '46px', fontSize: '10px', letterSpacing: '1.6px', color: S.textMuted, verticalAlign: 'middle'
    });
    var vals = [];
    for (var i = 0; i < d.axes.length; ++i) {
      var cell = M.make(row, 'Panel', '', {
        width: 'fill-parent-flow(1.0)', flowChildren: 'right', marginLeft: '11px'
      });
      M.label(cell, '', d.axes[i], { fontSize: '10px', color: S.textMuted, verticalAlign: 'bottom' });
      vals.push(M.label(cell, '', '0', {
        fontFamily: S.fontMono, fontSize: '15px', color: S.text, marginLeft: '6px', verticalAlign: 'bottom'
      }));
    }
    return { root: row, set: function (list) {
      for (var j = 0; j < vals.length; ++j) txt(vals[j], (list && list[j] !== undefined) ? String(list[j]) : '-');
    } };
  }

  function buildKey(parent, d) {
    var row = M.make(parent, 'Panel', '', { width: '100%', flowChildren: 'right' });
    if (!row) return null;
    var capp = M.make(row, 'Panel', '', {
      width: '96px', padding: '7px 0px', backgroundColor: S.offBlack,
      border: '1px solid ' + S.border, borderRadius: '4px'
    });
    M.label(capp, '', d.txt, {
      fontFamily: S.fontMono, fontSize: '13px', color: S.text,
      horizontalAlign: 'center', width: '100%'
    });
    M.label(row, '', d.label, {
      width: 'fill-parent-flow(1.0)', fontSize: '13px', color: S.textWarm,
      marginLeft: '12px', verticalAlign: 'middle'
    });
    return { root: row, set: function () {} };
  }

  function buildNav(parent, d) {
    var row = M.make(parent, 'Panel', '', { width: '100%', flowChildren: 'right' });
    if (!row) return null;
    function arrow(glyph, fn) {
      var b = M.make(row, 'Panel', '', {
        width: '60px', padding: '9px 0px', backgroundColor: BEVEL,
        border: '1px solid ' + S.border, borderRadius: '8px'
      });
      var ico = M.make(b, 'Panel', '', {
        width: '12px', height: '12px',
        backgroundImage: 'url("' + glyph + '")',
        backgroundSize: 'contain', backgroundRepeat: 'no-repeat', backgroundPosition: 'center',
        horizontalAlign: 'center', verticalAlign: 'middle'
      });
      if (ico) ico.hittest = false;
      on(b, 'onactivate', function () { if (fn) { try { fn(); } catch (e) {} } });
      hoverBg(b, BEVEL, BEVEL_HOT);
      return b;
    }
    arrow(S.icoArrowLeft, d.onPrev);
    var mid = M.label(row, '', d.txt, {
      width: 'fill-parent-flow(1.0)', fontFamily: S.fontMono, fontSize: '15px', color: S.text,
      horizontalAlign: 'center', verticalAlign: 'middle', textOverflow: 'ellipsis',
      marginLeft: '8px', marginRight: '8px'
    });
    arrow(S.icoArrowRight, d.onNext);
    return { root: row, set: function (t) { txt(mid, t); } };
  }

  function buildNote(parent, d) {
    var l = M.label(parent, '', d.label, { width: '100%', fontSize: '12px', color: S.textWarm });
    return { root: l, set: function (t) { txt(l, t); } };
  }

  function buildCap(parent, d) {
    var box = M.make(parent, 'Panel', '', { width: '100%', marginTop: '14px', marginBottom: '8px' });
    M.label(box, '', d.label, {
      fontSize: '11px', letterSpacing: '1.8px', color: S.textMuted, width: '100%'
    });
    return { root: box, set: function () {} };
  }

  function buildBig(parent, d) {
    var box = M.make(parent, 'Panel', '', { width: '100%', marginTop: '14px', marginBottom: '4px' });
    var l = M.label(box, '', d.txt, { fontFamily: S.fontMono, fontSize: '20px', color: S.text, width: '100%' });
    return { root: box, set: function (t) { txt(l, t); } };
  }
)CFJS" R"CFJS(
  // An option row: the choice and the CONSEQUENCE of the choice, one under the other. The spec is
  // explicit that Range, Camera and HUD are option rows rather than dropdowns, because a dropdown
  // hides exactly the sentence that makes the choice decidable.
  //
  // A blocked option keeps its row and states the reason in the row's own sub-line; it is not
  // removed, because a missing option reads as a bug rather than as a prerequisite.
  function buildOpt(parent, d) {
    var row = M.make(parent, 'Panel', '', {
      width: '100%', flowChildren: 'right', minHeight: '58px', padding: '10px 14px',
      borderRadius: '6px', border: '1px solid ' + S.border, backgroundColor: C.tileBg,
      transition: 'background-color 0.12s ease-out, border-color 0.12s ease-out'
    });
    if (!row) return null;
    var col = M.make(row, 'Panel', '', {
      width: 'fill-parent-flow(1.0)', flowChildren: 'down', verticalAlign: 'middle'
    });
    var title = M.label(col, '', d.label, { fontSize: '14px', color: S.text, width: '100%' });
    var sub = M.label(col, '', d.sub, { fontSize: '11px', color: S.textMuted, marginTop: '2px', width: '100%' });
    var mark = M.make(row, 'Panel', '', {
      width: '3px', height: '20px', borderRadius: '2px', backgroundColor: M.CLEAR,
      verticalAlign: 'middle', marginLeft: '12px'
    });
    var selected = false, blocked = '';
    function paint() {
      M.set(row, {
        border: '1px solid ' + (blocked ? S.border : (selected ? C.optOn : S.border)),
        backgroundColor: blocked ? '#191814' : (selected ? (C.optOn + '1a') : C.tileBg)
      });
      M.set(title, { color: blocked ? '#7d7a6c' : (selected ? C.optOn : S.text) });
      M.set(sub, { color: blocked ? S.warn : S.textMuted });
      M.set(mark, { backgroundColor: selected && !blocked ? C.optOn : M.CLEAR });
      txt(sub, blocked || d.sub);
    }
    on(row, 'onactivate', function () {
      if (blocked) return;
      if (d.onPick) { try { d.onPick(d.key); } catch (e) {} }
    });
    on(row, 'onmouseover', function () { if (!blocked) M.set(row, { borderColor: '#70F8C180' }); });
    on(row, 'onmouseout', function () { paint(); });
    paint();
    return {
      root: row, key: d.key, group: d.group,
      set: function (sel) { if (sel !== selected) { selected = !!sel; paint(); } },
      setBlocked: function (why) { why = why || ''; if (why !== blocked) { blocked = why; paint(); } }
    };
  }

  var BUILDERS = {
    number: buildNumber, scrub: buildScrub, 'switch': buildSwitch, mode: buildMode, grid: buildGrid,
    drop: buildDrop, act: buildAct, read: buildRead, key: buildKey, nav: buildNav,
    note: buildNote, cap: buildCap, big: buildBig, opt: buildOpt, stepper: buildStepper
  };

  // Full-width kinds never share a line. Only the two tile kinds pack.
  function packs(kind) { return kind === 'scrub' || kind === 'switch'; }

  // ---- the card -------------------------------------------------------------------------------
  //
  // A header row and a body. The body carries its group tabs -- text on a rule, never pills, and
  // never more than four -- and the rows of the selected group.
  // A FLAT card has no chrome and no header: it is a group-tabbed page rather than a box. The
  // render column is one of these, because there is only ever one card there and a box round the
  // whole column is a box round nothing.
  function buildCard(parent, spec, accent) {
    var flat = !!spec.flat;
    var root = M.make(parent, 'Panel', '', flat
      ? { width: '100%', flowChildren: 'down' }
      : {
          width: '100%', flowChildren: 'down', padding: '14px 16px', borderRadius: '8px',
          backgroundColor: C.cardClosed, border: '1px solid ' + C.borderClosed, marginBottom: '10px'
        });
    if (!root) return null;

    var head = M.make(root, 'Panel', '', { width: '100%', flowChildren: 'right' });
    if (flat && head) head.visible = false;
    var edge = M.make(head, 'Panel', '', {
      width: '3px', height: '32px', borderRadius: '2px', backgroundColor: spec.edge || S.gold,
      verticalAlign: 'middle'
    });
    var titleCol = M.make(head, 'Panel', '', {
      width: 'fill-parent-flow(1.0)', flowChildren: 'down', marginLeft: '13px', verticalAlign: 'middle'
    });
    var title = M.label(titleCol, '', spec.title || '', {
      fontFamily: S.fontSerif, fontSize: '16px', letterSpacing: '1.2px', color: S.text, width: '100%'
    });
    var sub = M.label(titleCol, '', '', {
      fontFamily: S.fontMono, fontSize: '12px', color: S.textMuted, marginTop: '3px',
      width: '100%', textOverflow: 'ellipsis'
    });
    show(title, !!spec.title);

    var action = null;
    if (spec.action) {
      action = M.make(head, 'Panel', '', {
        padding: '7px 13px', borderRadius: '8px', backgroundColor: BEVEL_ACCENT,
        border: '1px solid ' + accent, marginLeft: '13px', verticalAlign: 'middle'
      });
      M.label(action, '', spec.action, {
        fontSize: '12px', letterSpacing: '1.6px', textTransform: 'uppercase', color: accent,
        textShadow: '2px 2px 0px 1.0 #000000'
      });
      on(action, 'onactivate', function () { if (spec.onAction) { try { spec.onAction(); } catch (e) {} } });
    }

    var headPill = null;
    if (spec.pill) {
      headPill = pill(head, 44, 24);
      M.set(headPill.root, { marginLeft: '13px', verticalAlign: 'middle' });
      on(headPill.root, 'onactivate', function () {
        if (spec.onPill) { try { spec.onPill(); } catch (e) {} }
      });
    }

    // THE DISCLOSURE MARK IS AN IMAGE, not a character. '⌄' and '⌃' are in Unicode but not in
    // every font that ships with the game, and a missing glyph draws a box -- the one failure that
    // is invisible in a browser mock-up and obvious on screen. Deadlock's own solid arrows are
    // already in the VPK, so the mark is drawn rather than typed.
    var chev = M.make(head, 'Panel', '', {
      width: '12px', height: '12px', marginLeft: '13px', verticalAlign: 'middle',
      backgroundImage: 'url("' + S.icoArrowDown + '")',
      backgroundSize: 'contain', backgroundRepeat: 'no-repeat', backgroundPosition: 'center'
    });
    if (chev) chev.hittest = false;
    show(chev, !spec.always);

    var body = M.make(root, 'Panel', '', { width: '100%', flowChildren: 'down' });
    if (body) body.visible = false;

    // group tabs
    var tabRow = M.make(body, 'Panel', '', {
      width: '100%', flowChildren: 'right', marginTop: '14px',
      borderBottom: '1px solid ' + C.ruleFaint
    });
    var tabs = [];
    for (var i = 0; i < spec.groups.length; ++i) {
      (function (g, idx) {
        var t = M.make(tabRow, 'Panel', '', {
          padding: '0px 0px 10px 0px', marginRight: '22px',
          borderBottom: '2px solid ' + M.CLEAR
        });
        var l = M.label(t, '', g.head.toUpperCase(), {
          fontSize: '12px', letterSpacing: '1.5px', color: S.textMuted
        });
        on(t, 'onactivate', function () { selectGroup(idx); });
        tabs.push({ panel: t, label: l });
      })(spec.groups[i], i);
    }
    // spec.hideTabs: the column's own tab strip drives selectGroup instead (the render column,
    // whose RANGE / CAMERA / HUD / OUTPUT sit under its title exactly as CS2's do).
    show(tabRow, spec.groups.length > 1 && !spec.hideTabs);

    var rowWrap = M.make(body, 'Panel', '', {
      width: '100%', flowChildren: 'down', marginTop: spec.hideTabs ? '2px' : '14px'
    });

    // Rows are built for EVERY group up front and hidden per group, so switching a group tab is a
    // visibility change rather than a rebuild. A rebuild here would delete the panel the pointer
    // is inside, which Panorama does not enjoy.
    var widgets = {}, groupRoots = [];
    function addRows(host, defs) {
      var line = null, lineLeft = 0, inner = spec.innerW || 444;
      for (var r = 0; r < defs.length; ++r) {
        var dd = defs[r];
        if (!dd) continue;
        var b = BUILDERS[dd.kind];
        if (!b) continue;
        var host2 = host;
        if (packs(dd.kind)) {
          var wNeed = dd.width + (line ? GAP : 0);
          if (!line || lineLeft < wNeed) {
            line = M.make(host, 'Panel', '', { width: '100%', flowChildren: 'right', marginBottom: GAP + 'px' });
            lineLeft = inner;
          } else {
            lineLeft -= GAP;
          }
          lineLeft -= dd.width;
          host2 = line;
        } else {
          line = null;
          host2 = M.make(host, 'Panel', '', { width: '100%', flowChildren: 'down', marginBottom: GAP + 'px' });
        }
        var w = b(host2, dd, accent);
        if (!w) continue;
        if (packs(dd.kind) && host2 !== host) M.set(w.root, { marginRight: GAP + 'px' });
        if (dd.id) widgets[dd.id] = w;
        if (dd.kind === 'opt') {
          widgets['opt:' + dd.group + ':' + dd.key] = w;
        }
        if (dd.tag) widgets[dd.tag] = w;
      }
    }

    for (var g2 = 0; g2 < spec.groups.length; ++g2) {
      var host = M.make(rowWrap, 'Panel', '', { width: '100%', flowChildren: 'down' });
      groupRoots.push(host);
      addRows(host, (spec.persist || []).concat(spec.groups[g2].rows));
      if (host) host.visible = (g2 === 0);
    }

    var open = !!spec.always, groupIndex = 0;

    function paint() {
      if (!flat) {
        M.set(root, {
          backgroundColor: open ? C.cardOpen : C.cardClosed,
          border: '1px solid ' + (open ? C.borderOpen : C.borderClosed)
        });
      }
      M.set(title, { color: open ? S.text : S.textWarm });
      show(body, open);
      show(action, open && !!spec.action);
      // A closed card's mark points down, an open one up. An image swap rather than a rotation:
      // a rotation is a transform, and a transform allocates a composited layer per card.
      M.set(chev, { backgroundImage: 'url("' + (open ? S.icoArrowUp : S.icoArrowDown) + '")' });
    }

    function selectGroup(i) {
      if (i === groupIndex) return;
      groupIndex = i;
      for (var j = 0; j < groupRoots.length; ++j) show(groupRoots[j], j === i);
      paintTabs();
      if (spec.onGroup) { try { spec.onGroup(i); } catch (e) {} }
    }

    function paintTabs() {
      for (var j = 0; j < tabs.length; ++j) {
        var hot = j === groupIndex;
        M.set(tabs[j].panel, { borderBottom: '2px solid ' + (hot ? accent : M.CLEAR) });
        M.set(tabs[j].label, { color: hot ? accent : S.textMuted });
      }
    }

    if (!spec.always) {
      on(head, 'onactivate', function () {
        open = !open;
        paint();
        if (spec.onToggle) { try { spec.onToggle(open); } catch (e) {} }
      });
    }
    paintTabs();
    paint();

    return {
      key: spec.key,
      root: root,
      w: widgets,
      // `always` cards are open by definition and are not counted as "a card the user opened",
      // which is what decides whether Collapse all has anything to do.
      always: !!spec.always,
      isOpen: function () { return open; },
      setOpen: function (v) { v = !!v || !!spec.always; if (v !== open) { open = v; paint(); } },
      setSub: function (t) { txt(sub, t || ''); },
      setEdge: function (c) { M.set(edge, { backgroundColor: c }); },
      setPill: function (v) { if (headPill) headPill.set(!!v); },
      selectGroup: selectGroup
    };
  }
)CFJS" R"CFJS(
  // ---- the column shell -----------------------------------------------------------------------
  //
  // Title, the collapse-all rule, the sub-tab strip, the card area and the footer. The three
  // workspaces differ only in what they put in it, which is why it is built here once.
  function buildColumn(parent, o) {
    var root = M.make(parent, 'Panel', o.id || '', { width: '100%', height: '100%', flowChildren: 'down' });
    if (!root) return null;
    root.visible = false;

    var titleRow = M.make(root, 'Panel', '', {
      width: '100%', flowChildren: 'right', padding: '22px 22px 0px 22px'
    });
    // CS2's header (CameraEditorJs.h + PanoramaUiMetricsJs.h): title 18px, chips 13px semibold
    // with UI.chip padding 8/13. The display face and accent colour stay Deadlock's own.
    var name = M.label(titleRow, '', o.name, {
      width: 'fill-parent-flow(1.0)', fontFamily: S.fontDisplay, fontSize: '18px',
      letterSpacing: '2.5px', color: o.accent, verticalAlign: 'middle', whiteSpace: 'nowrap'
    });
    function smallBtn(text2, run) {
      var b = M.make(titleRow, 'Panel', '', {
        padding: '8px 13px', border: '1px solid ' + S.border, borderRadius: '6px',
        marginLeft: '14px', verticalAlign: 'middle'
      });
      // nowrap: "MOUSE: GAME · J" was wrapping at a space and standing taller than "ESC EXIT".
      var l = M.label(b, '', text2, {
        fontSize: '13px', height: '18px', fontWeight: 'semibold', letterSpacing: '1px',
        color: S.textWarm, whiteSpace: 'nowrap', verticalAlign: 'middle'
      });
      on(b, 'onactivate', function () { if (run) { try { run(); } catch (e) {} } });
      on(b, 'onmouseover', function () { M.set(b, { borderColor: S.gold }); });
      on(b, 'onmouseout', function () { M.set(b, { borderColor: S.border }); });
      return { panel: b, label: l };
    }
    // Says which mode the mouse is in and how to change it, because the single most common way to
    // get stuck in this tool is not knowing the cursor is owned by the game.
    // o.backOnly: CS2's render panel carries ONE header chip, '✕ Back' (CameraEditorRenderJs.h
    // RndBack), and neither the mouse chip nor ESC EXIT -- J and Esc still work from the keyboard.
    var mouseBtn = o.backOnly ? null
      : smallBtn('MOUSE: GAME · J', function () { cmd('mirv_filmmaker ui cursor toggle'); });
    // o.midChip: a stateful chip between MOUSE and ESC EXIT -- CS2's CONFIG header puts
    // "GAME UI: SHOW ALL" exactly there (ConfigHudJs.h hudCycle).
    var midBtn = (o.midChip && !o.backOnly) ? smallBtn(o.midChip.label, o.midChip.onActivate) : null;
    if (!o.backOnly) smallBtn('ESC EXIT', function () { cmd('mirv_filmmaker workspace off'); });
    // CS2 parity for the Render workspace only (o.extraChip is omitted by every other caller):
    // CS2 pairs its RENDER title with a "✕ Back" chip on this SAME row (CameraEditorRenderJs.h),
    // because CS2's render is a modal OVER the Camera Editor and ESC EXIT-equivalent there means
    // "close render, keep the editor". Deadlock's render is a peer workspace instead, so its own
    // ESC EXIT closes everything -- this chip is the specific "go back to the editor instead"
    // affordance CS2 needs no extra button for. Previously lived in the JOB column (this file's
    // own render body), which put it on the opposite side of the screen from CS2's; moved here so
    // the position matches, not just the existence.
    if (o.extraChip) smallBtn(o.extraChip.label, o.extraChip.onActivate);

    // TEXT TABS ON A RULE, and never pill buttons -- the spec is explicit about it, and the same
    // treatment carries the group tabs inside a card, so the two levels read as one idea. Two
    // filled half-width boxes read as a segmented control, which is a choice between values rather
    // than a move between pages.
    //
    // ONE rule, under the tabs, exactly as CS2 draws it (CameraEditorJs.h `modeRow`: marginTop 20,
    // borderBottom 1px, UI.tabTop = font 15 / ls 1.4 / gap 20 / padBottom 13). There used to be a
    // second rule between the title and this strip, carrying nothing but the hidden "Collapse all"
    // label; CS2 has no such line and parks "Collapse all" at the far right of THIS row instead.
    var subStrip = M.make(root, 'Panel', '', {
      width: '100%', flowChildren: 'right', padding: '20px 22px 0px 22px',
      borderBottom: '1px solid ' + C.ruleFaint
    });
    var subTabs = [];
    for (var i = 0; i < (o.subTabs || []).length; ++i) {
      (function (t, idx) {
        // The tab hugs its label. A tab stretched to half the column puts its underline under
        // empty space, which is the one thing the underline is supposed to point at.
        // o.tabFont / o.tabLs / o.tabGap: CS2's render tabs are 14px / 1.6 / 26px
        // (CameraEditorRenderJs.h renderTabRow) where the editor's are 15px / 1.4 / 20px.
        var p = M.make(subStrip, 'Panel', '', {
          width: 'fit-children', padding: '2px 0px 13px 0px', marginRight: (o.tabGap || 20) + 'px',
          marginBottom: '-1px', borderBottom: '2px solid ' + M.CLEAR
        });
        var l = M.label(p, '', t[1].toUpperCase(), {
          fontSize: (o.tabFont || 15) + 'px', height: '20px', letterSpacing: (o.tabLs || 1.4) + 'px',
          color: S.textMuted, whiteSpace: 'nowrap'
        });
        on(p, 'onactivate', function () { if (o.onSubTab) o.onSubTab(t[0], idx); });
        subTabs.push({ key: t[0], panel: p, label: l });
      })(o.subTabs[i], i);
    }
    var tabSpacer = M.make(subStrip, 'Panel', '', { width: 'fill-parent-flow(1.0)', height: '1px' });
    if (tabSpacer) tabSpacer.hittest = false;
    var collapse = M.label(subStrip, '', 'Collapse all', {
      paddingBottom: '13px', fontSize: '11px', fontWeight: 'bold', letterSpacing: '1px',
      color: S.textMuted, verticalAlign: 'bottom', whiteSpace: 'nowrap'
    });
    on(collapse, 'onmouseover', function () { M.set(collapse, { color: S.text }); });
    on(collapse, 'onmouseout', function () { M.set(collapse, { color: S.textMuted }); });
    on(collapse, 'onactivate', function () { if (o.onCollapseAll) o.onCollapseAll(); });
    show(collapse, false);
    // Always shown: with no tabs (Config, Render) the strip is just the one rule under the title,
    // which is also all CS2 draws there.

    var cards = M.make(root, 'Panel', '', {
      width: '100%', height: 'fill-parent-flow(1.0)',
      flowChildren: 'down', padding: '14px 22px 18px 22px', overflow: 'squish scroll'
    });
    if (cards) cards.__mvmValueViewport = true;

    // The empty state. "Nothing here yet" is the honest answer for Config, and saying it plainly is
    // better than a tab that opens onto nothing.
    var empty = M.make(cards, 'Panel', '', {
      width: '100%', height: '100%', flowChildren: 'down'
    });
    if (empty) {
      empty.visible = false;
      var pad1 = M.make(empty, 'Panel', '', { width: '100%', height: 'fill-parent-flow(1.0)' });
      M.label(empty, '', 'NOTHING HERE YET', {
        fontFamily: S.fontSerif, fontSize: '15px', letterSpacing: '1.6px', color: S.textWarm,
        horizontalAlign: 'center', width: '100%', textAlign: 'center'
      });
      M.label(empty, '', o.emptyText || '', {
        width: '330px', fontSize: '13px', color: S.textMuted, marginTop: '10px',
        horizontalAlign: 'center', textAlign: 'center'
      });
      M.make(empty, 'Panel', '', { width: '100%', height: 'fill-parent-flow(1.0)' });
    }

    var footer = M.make(root, 'Panel', '', {
      width: '100%', flowChildren: 'down', padding: '14px 22px 18px 22px',
      borderTop: '1px solid ' + C.ruleFaint
    });
    if (footer) footer.visible = false;
    var footLabel = M.label(footer, '', '', {
      fontSize: '11px', letterSpacing: '1.8px', color: S.textMuted, width: '100%'
    });
    var footRow = M.make(footer, 'Panel', '', { width: '100%', flowChildren: 'right', marginTop: '10px' });

    function footButtons(list) {
      try { footRow.RemoveAndDeleteChildren(); } catch (e) {}
      for (var j = 0; j < list.length; ++j) {
        (function (f, last) {
          var b = M.make(footRow, 'Panel', '', {
            width: 'fill-parent-flow(1.0)', minHeight: '46px', borderRadius: '8px',
            backgroundColor: BEVEL_ACCENT, border: '1px solid ' + (f.colour || S.border),
            marginRight: last ? '0px' : '8px'
          });
          M.label(b, '', f.cap, {
            fontSize: '13px', letterSpacing: '1.6px', textTransform: 'uppercase',
            color: f.colour || S.textWarm, textShadow: '2px 2px 0px 1.0 #000000',
            horizontalAlign: 'center', verticalAlign: 'middle', width: '100%'
          });
          on(b, 'onactivate', function () { if (f.run) { try { f.run(); } catch (e) {} } });
          f.panel = b;
        })(list[j], j === list.length - 1);
      }
      show(footer, list.length > 0);
    }

    function setSubTab(k) {
      for (var j = 0; j < subTabs.length; ++j) {
        var hot = subTabs[j].key === k;
        M.set(subTabs[j].panel, { borderBottom: '2px solid ' + (hot ? o.accent : M.CLEAR) });
        M.set(subTabs[j].label, { color: hot ? o.accent : S.textMuted });
      }
    }
    if (subTabs.length) setSubTab(subTabs[0].key);

    return {
      root: root, cards: cards, empty: empty,
      setVisible: function (v) { show(root, v); },
      setSubTab: setSubTab,
      setEmpty: function (v) { show(empty, v); },
      setCollapseVisible: function (v) { show(collapse, v); },
      setMidChip: function (text) { if (midBtn) txt(midBtn.label, text); },
      setMouseMode: function (ui) {
        if (!mouseBtn) return;
        txt(mouseBtn.label, ui ? 'MOUSE: UI · J' : 'MOUSE: GAME · J');
        M.set(mouseBtn.label, { color: ui ? S.gold : S.textWarm });
      },
      // An empty label collapses, so a caption-less footer (CS2's render Set In / Set Out / Clear)
      // starts at its rule rather than under a blank line.
      footer: { set: function (label, list) {
                  txt(footLabel, label); show(footLabel, !!label);
                  M.set(footRow, { marginTop: label ? '10px' : '0px' });
                  footButtons(list);
                },
                show: function (v) { show(footer, v); } }
    };
  }

  // ---- the dropdown overlay -------------------------------------------------------------------
  //
  // ONE menu for the whole column, parented at the root rather than inside a card, so it draws over
  // everything and so two of them can never be open at once.
  var menu = null, menuList = null, menuNote = null, menuScrim = null, menuRows = [], menuHost = null;
  var menuOnPick = null, menuOnClose = null;

  function buildMenu(host) {
    menuHost = host;
    menuScrim = M.make(host, 'Panel', 'MvmMenuScrim', {
      width: '100%', height: '100%', position: '0px 0px 0px',
      horizontalAlign: 'left', verticalAlign: 'top', zIndex: 240, backgroundColor: M.CLEAR
    });
    if (menuScrim) {
      menuScrim.visible = false;
      on(menuScrim, 'onactivate', function () { closeMenu(); });
    }
    menu = M.make(host, 'Panel', 'MvmMenu', {
      backgroundColor: S.panelBg, border: '1px solid ' + S.border, borderRadius: '8px',
      padding: '5px', flowChildren: 'down', horizontalAlign: 'left', verticalAlign: 'top',
      zIndex: 250
    });
    if (!menu) return;
    menu.visible = false;
    menuList = M.make(menu, 'Panel', '', { width: '100%', flowChildren: 'down', overflow: 'squish scroll' });
    menuNote = M.label(menu, '', '', {
      fontSize: '11px', color: S.textWarm, padding: '8px 11px 4px 11px', width: '100%'
    });
  }

  function closeMenu() {
    show(menu, false);
    show(menuScrim, false);
    if (menuOnClose) { try { menuOnClose(); } catch (e) {} }
    menuOnClose = null;
  }

  // `anchor` is the collapsed bar. The menu opens below it, or above when below would run off the
  // bottom -- the same flip Deadlock's own menus do.
  function openMenu(anchor, src, current, onPick, onClose) {
    if (!menu) return;
    menuOnPick = onPick;
    menuOnClose = onClose;

    var options = src.options || [];
    while (menuRows.length < options.length) {
      (function () {
        var r = M.make(menuList, 'Panel', '', {
          width: '100%', flowChildren: 'right', padding: '11px 10px', borderRadius: '4px'
        });
        var mark = M.make(r, 'Panel', '', {
          width: '2px', height: '18px', borderRadius: '1px', backgroundColor: M.CLEAR,
          verticalAlign: 'middle'
        });
        var capL = M.label(r, '', '', {
          width: 'fill-parent-flow(1.0)', fontSize: '14px', color: S.text, marginLeft: '10px',
          verticalAlign: 'middle', textOverflow: 'ellipsis'
        });
        var metaL = M.label(r, '', '', {
          fontSize: '12px', color: S.textMuted, marginLeft: '10px', verticalAlign: 'middle'
        });
        var item = { row: r, mark: mark, cap: capL, meta: metaL, key: '' };
        on(r, 'onactivate', function () {
          var k = item.key;
          closeMenu();
          if (menuOnPick) { try { menuOnPick(k); } catch (e) {} }
        });
        on(r, 'onmouseover', function () { M.set(r, { backgroundColor: '#ffffff14' }); });
        on(r, 'onmouseout', function () {
          M.set(r, { backgroundColor: item.key === current ? (S.gold + '1f') : M.CLEAR });
        });
        menuRows.push(item);
      })();
    }
    for (var i = 0; i < menuRows.length; ++i) {
      var it = menuRows[i];
      if (i >= options.length) { show(it.row, false); continue; }
      var o2 = options[i];
      it.key = o2[0];
      show(it.row, true);
      txt(it.cap, o2[1]);
      txt(it.meta, o2[2] || '');
      var hot = o2[0] === current;
      M.set(it.row, { backgroundColor: hot ? (S.gold + '1f') : M.CLEAR });
      M.set(it.mark, { backgroundColor: hot ? S.gold : M.CLEAR });
      M.set(it.cap, { color: hot ? S.gold : S.text });
    }
    txt(menuNote, src.note || '');
    show(menuNote, !!src.note);

    // As wide as the bar, in window space (see rectIn), and pulled left rather than clipped when the
    // bar sits at the screen's right edge.
    var x = 0, y = 0, w = 400;
    try {
      var r = rectIn(anchor, menuHost);
      w = r.w;
      x = Math.max(8, Math.min(r.x, r.hostW - 8 - w));
      var h = Math.min(400, options.length * 44 + 10) + (src.note ? 34 : 0);
      var below = r.y + r.h + 6;
      y = (below + h > r.hostH - 16) ? Math.max(16, r.y - h - 6) : below;
    } catch (e) {}
    M.set(menu, {
      position: Math.floor(x) + 'px ' + Math.floor(y) + 'px 0px',
      width: Math.floor(w) + 'px'
    });
    M.set(menuList, { maxHeight: '400px' });
    show(menuScrim, true);
    show(menu, true);
  }
)CFJS" R"CFJS(
  // ---- shared overlay primitives (P8a) ----------------------------------------------------------
  //
  // Four building blocks any Deadlock panel can use instead of growing its own copy of CS2's literal
  // styling. NONE of these are wired into a real call site yet -- that is a later phase's job, once a
  // caller actually needs one -- so nothing that ships today changes behaviour.

  // A transient, auto-expiring banner. CS2 reference: MarkerHudJs.h's showBanner (~line 544) and its
  // `s.notice` pattern (~line 599) -- shown over whatever surface raised it, for as long as the state
  // feed keeps handing over a message. CS2's own expiry (CameraPath::Notify / m_noticeExpireSec,
  // Movie/CameraPath.cpp line 178) is a wall-clock deadline C++ keeps, so that JS side never runs a
  // timer at all -- it only draws whatever `s.notice` is truthy that frame. This adds an OPTIONAL
  // JS-side timer for a caller with no C++ deadline of its own, through $.Schedule -- the same
  // delayed-call primitive DeadlockSpeedDialJs.h and DeadlockWorkspaceJs.h already use, not an
  // invented one.
  function buildNotice(host) {
    var root = M.make(host, 'Panel', '', {
      flowChildren: 'right', padding: '11px 14px', borderRadius: '6px',
      backgroundColor: S.panelBgDeep, border: '1px solid ' + S.border,
      horizontalAlign: 'center', verticalAlign: 'top'
    });
    if (!root) return null;
    root.hittest = false;
    root.visible = false;
    var bar = M.make(root, 'Panel', '', {
      width: '3px', height: '100%', borderRadius: '2px', backgroundColor: S.gold, verticalAlign: 'middle'
    });
    if (bar) bar.hittest = false;
    var label = M.label(root, '', '', {
      fontSize: '13px', color: S.text, marginLeft: '11px', verticalAlign: 'middle',
      maxWidth: '420px', textOverflow: 'ellipsis'
    });
    if (label) label.hittest = false;
    var gen = 0;
    function doHide() { show(root, false); }
    function doShow(text, ms) {
      txt(label, text || '');
      show(root, true);
      var myGen = ++gen;
      if (ms > 0) {
        try { $.Schedule(ms / 1000, function () { if (myGen === gen) doHide(); }); } catch (e) {}
      }
    }
    return { root: root, show: doShow, hide: doHide };
  }

  // An arm-then-click-again button. CS2 reference: CameraTimelineJs.h's clearBtn/disarmClear
  // (~lines 142-154, 840-844) and MarkerHudJs.h's own "Remove All Markers" -> "Remove All -- click
  // again to confirm" (~lines 361-379) -- disarmed by clicking any OTHER tracked button, never by a
  // timer. The CALLER tracks its own button group and calls .disarm() on every other button's click;
  // this does not try to auto-track "other buttons" globally, which is exactly the kind of hidden
  // state the scrub-scope comments earlier in this file warn against.
  function armedButton(parent, restLabel, armedLabel, onConfirm) {
    var armed = false;
    var b = M.make(parent, 'Panel', '', {
      padding: '10px 14px', borderRadius: '8px', border: '1px solid ' + S.border,
      backgroundColor: BEVEL, verticalAlign: 'middle'
    });
    if (!b) return null;
    var l = M.label(b, '', restLabel, {
      fontSize: '13px', letterSpacing: '1.2px', textTransform: 'uppercase', color: S.text,
      textShadow: S.textShadowHard, horizontalAlign: 'center', verticalAlign: 'middle', width: '100%'
    });
    function paint() {
      txt(l, armed ? armedLabel : restLabel);
      M.set(l, { color: armed ? S.danger : S.text });
      M.set(b, { border: '1px solid ' + (armed ? S.danger : S.border) });
    }
    function disarm() { if (armed) { armed = false; paint(); } }
    on(b, 'onactivate', function () {
      if (!armed) { armed = true; paint(); return; }
      disarm();
      if (onConfirm) { try { onConfirm(); } catch (e) {} }
    });
    hoverBg(b, BEVEL, BEVEL_HOT);
    return { root: b, disarm: disarm, isArmed: function () { return armed; } };
  }

  // A full-screen modal shell for a destructive confirm. CS2 reference: CameraEditorJs.h's
  // confirmOverlay/confirmBox/confirmTitle/confirmBody/confirmActions/confirmNo/confirmYes
  // (~lines 159-190): a dimming scrim, a centred card, and a No/Yes pair. There the overlay is a
  // child of `ctx` -- the editor's own top level -- because a child of a lower-z sibling root draws
  // BEHIND a higher-z one (the Graph, the timeline). Deadlock's own dropdown menu and tooltip solve
  // the same problem by parenting on the workspace root at build time (buildOverlays(root), below);
  // `rootHost` here is that same true top level -- a caller passes $.MvmWorkspace.root() (or whatever
  // other panel is genuinely the top of its own stack), never a card or column body.
  function buildConfirmModal(rootHost, opts) {
    opts = opts || {};
    var scrim = M.make(rootHost, 'Panel', '', {
      width: '100%', height: '100%', position: '0px 0px 0px',
      zIndex: opts.zIndex || 280, backgroundColor: S.scrimDim
    });
    if (!scrim) return null;
    scrim.visible = false;
    var box = M.make(scrim, 'Panel', '', {
      width: (opts.width || 480) + 'px', horizontalAlign: 'center', verticalAlign: 'center',
      backgroundColor: S.panelBgDeep, border: '1px solid ' + S.danger, borderRadius: '8px',
      boxShadow: S.boxShadowFloat, flowChildren: 'down', padding: '24px 26px'
    });
    if (!box) return null;
    var title = M.label(box, '', '', {
      fontFamily: S.fontSerif, fontSize: '18px', letterSpacing: '1.5px', color: S.text,
      horizontalAlign: 'center', textAlign: 'center', width: '100%', marginBottom: '12px'
    });
    var body = M.label(box, '', '', {
      fontSize: '13px', color: S.textMuted, horizontalAlign: 'center', textAlign: 'center',
      width: '100%', marginBottom: '20px', whiteSpace: 'normal'
    });
    var actions = M.make(box, 'Panel', '', { width: '100%', flowChildren: 'right', horizontalAlign: 'center' });
    function doHide() { show(scrim, false); }
    on(box, 'onactivate', function () {});      // swallow clicks that land inside the card
    on(scrim, 'onactivate', doHide);            // outside click cancels, like CS2's confirmOverlay
    function mkAction(danger, label2, run) {
      var a = M.make(actions, 'Panel', '', {
        padding: '10px 20px', borderRadius: '8px',
        border: '1px solid ' + (danger ? S.danger : S.border),
        backgroundColor: BEVEL, marginRight: danger ? '0px' : '10px'
      });
      M.label(a, '', label2, {
        fontSize: '12px', letterSpacing: '1.4px', textTransform: 'uppercase',
        color: danger ? S.danger : S.text, textShadow: S.textShadowHard,
        horizontalAlign: 'center', verticalAlign: 'middle', width: '100%'
      });
      on(a, 'onactivate', run);
      hoverBg(a, BEVEL, BEVEL_HOT);
      return a;
    }
    function doShow(o) {
      o = o || {};
      txt(title, o.title || '');
      txt(body, o.body || '');
      try { actions.RemoveAndDeleteChildren(); } catch (e) {}
      mkAction(false, o.noLabel || 'Cancel', doHide);
      mkAction(true, o.yesLabel || 'Confirm', function () {
        doHide();
        if (o.onYes) { try { o.onYes(); } catch (e) {} }
      });
      show(scrim, true);
    }
    return { root: scrim, show: doShow, hide: doHide };
  }

  // A shared shell for a per-element right-click menu: the outer panel, border, shadow, scrim and
  // dismiss-on-outside-click machinery, so a caller only supplies CONTENT. CS2-shaped styling is
  // exactly the complaint this exists to fix: DeadlockTimelineJs.h's own per-keyframe "shake" popup
  // (shakeScrim/shakeMenu, ~lines 508-521) is a literal 390px / rgba(16,20,26,0.99) / boxShadow card
  // rather than Deadlock's own tokens. This shell is NOT migrated into that file here -- it is a
  // separate phase's file and this change must not touch its behaviour -- so a future call site gets
  // Deadlock's own chrome for free and only has to write buildContentFn.
  function buildContextMenuShell(host) {
    var scrim = null, panel = null, body = null, built = false, gen = 0;
    function build() {
      if (built || !host) return;
      built = true;
      scrim = M.make(host, 'Panel', '', {
        width: '100%', height: '100%', position: '0px 0px 0px', zIndex: 245, backgroundColor: M.CLEAR
      });
      if (scrim) {
        scrim.visible = false;
        on(scrim, 'onactivate', doClose);
        on(scrim, 'oncontextmenu', doClose);
      }
      panel = M.make(host, 'Panel', '', {
        backgroundColor: S.panelBgDeep, border: '1px solid ' + S.border, borderRadius: '8px',
        padding: '10px', boxShadow: S.boxShadowFloat, flowChildren: 'down',
        horizontalAlign: 'left', verticalAlign: 'top', zIndex: 246, overflow: 'squish scroll'
      });
      if (!panel) return;
      panel.visible = false;
      on(panel, 'onactivate', function () {});
      body = M.make(panel, 'Panel', '', { width: '100%', flowChildren: 'down' });
    }
    function doClose() {
      show(scrim, false);
      show(panel, false);
    }
    function place(anchor) {
      if (!panel || !anchor) return;
      var x = 0, y = 0, w = 320;
      try {
        var r = rectIn(anchor, host);
        var sx = anchor.actualuiscale_x || 1, sy = anchor.actualuiscale_y || 1;
        w = Math.max(220, (panel.actuallayoutwidth || 0) / sx || 320);
        x = Math.max(8, Math.min(r.x, r.hostW - 8 - w));
        var h = (panel.actuallayoutheight || 0) / sy;
        var below = r.y + r.h + 6;
        y = (h > 0 && below + h > r.hostH - 8) ? Math.max(8, r.y - h - 6) : below;
      } catch (e) {}
      M.set(panel, { position: Math.floor(x) + 'px ' + Math.floor(y) + 'px 0px' });
    }
    function doOpen(anchor, buildContentFn) {
      build();
      if (!panel || !body || !anchor) return;
      try { body.RemoveAndDeleteChildren(); } catch (e) {}
      if (buildContentFn) { try { buildContentFn(body); } catch (e) {} }
      show(scrim, true);
      show(panel, true);
      place(anchor);
      // The body was just (re)built, so its first measured size is the PREVIOUS content's; settle
      // once more next frame, the same guarded re-place MarkerHudJs.h's settleBanner uses.
      var myGen = ++gen;
      try {
        $.Schedule(0.03, function () {
          if (myGen !== gen || !panel || !panel.visible) return;
          place(anchor);
        });
      } catch (e) {}
    }
    return { open: doOpen, close: doClose };
  }

  // A Deadlock-styled equivalent of CS2's EditorSettingsPopupJs.h fly-out: the open/close/toggle
  // state machine and a toggle-row factory, with NO toggles pre-wired. CS2's four rows (X-Ray, True
  // View, Include DOA actions, Allow demo mismatch versions) read spec_show_xray, cl_demo_predict and
  // cl_trueview_show_doa_predictions -- demo-playback cvars that do not exist in Deadlock: grepped
  // across AfxHookSource2/Deadlock and every Deadlock *Js.h fragment, which also has no cvar-read
  // helper like CS2's rdInt to reflect one live even if it did. A future call site supplies its own
  // rows through .row() and drives them from its own state feed, same as every other Deadlock control
  // in this file.
  function buildSettingsPopup(host, opts) {
    opts = opts || {};
    var accent = opts.accent || S.blue;
    var z = opts.zIndex || 265;
    var overlay = M.make(host, 'Panel', opts.id || '', {
      width: '100%', height: '100%', position: '0px 0px 0px', zIndex: z, backgroundColor: M.CLEAR
    });
    if (!overlay) return null;
    overlay.visible = false;
    var card = M.make(overlay, 'Panel', '', {
      width: (opts.width || 320) + 'px', horizontalAlign: 'right', verticalAlign: 'bottom',
      marginRight: (opts.marginRight == null ? 16 : opts.marginRight) + 'px',
      marginBottom: (opts.marginBottom == null ? 16 : opts.marginBottom) + 'px',
      backgroundColor: S.panelBgDeep, border: '1px solid ' + accent, borderRadius: '8px',
      boxShadow: S.boxShadowFloat, flowChildren: 'down', padding: '14px 16px', zIndex: z + 1
    });
    if (!card) return null;
    on(overlay, 'onactivate', function () { doClose('bgClick'); });
    on(card, 'onactivate', function () {});
    M.label(card, '', opts.title || 'SETTINGS', {
      fontSize: '12px', letterSpacing: '2px', color: accent, marginBottom: '4px', fontWeight: 'bold'
    });

    // One toggle row: label (+ optional explanation) on the left, a pill switch on the right. This
    // shell owns no state at all -- the caller reads its own feed and calls .set()/.setEnabled().
    function settingRow(label2, explain) {
      var row = M.make(card, 'Panel', '', {
        width: '100%', flowChildren: 'right', marginTop: '12px', verticalAlign: 'middle'
      });
      var col = M.make(row, 'Panel', '', {
        width: 'fill-parent-flow(1.0)', flowChildren: 'down', verticalAlign: 'center', paddingRight: '12px'
      });
      M.label(col, '', label2, { fontSize: '14px', color: S.text, width: '100%' });
      if (explain) M.label(col, '', explain, { fontSize: '10px', color: S.textMuted, marginTop: '2px', width: '100%' });
      var p = pill(row, 46, 24);
      var enabled = true, onClick = null;
      on(p.root, 'onactivate', function () { if (enabled && onClick) { try { onClick(); } catch (e) {} } });
      return {
        row: row, set: function (v) { p.set(!!v); },
        setEnabled: function (v) { enabled = !!v; M.set(row, { opacity: enabled ? '1.0' : '0.4' }); p.root.hittest = enabled; },
        onClick: function (fn) { onClick = fn; }
      };
    }

    var openState = false;
    function doRefresh() { if (opts.onRefresh) { try { opts.onRefresh(); } catch (e) {} } }
    function doOpen() {
      if (opts.beforeOpen) { try { opts.beforeOpen(); } catch (e) {} }
      doRefresh();
      openState = true;
      show(overlay, true);
      if (opts.onOpen) { try { opts.onOpen(); } catch (e) {} }
    }
    function doClose(why) {
      openState = false;
      show(overlay, false);
      if (opts.onClose) { try { opts.onClose(why); } catch (e) {} }
    }
    function doToggle() { if (openState) doClose('reclick'); else doOpen(); }
    return {
      overlay: overlay, card: card, accent: accent, row: settingRow,
      open: doOpen, close: doClose, toggle: doToggle, refresh: doRefresh,
      isOpen: function () { return openState; }
    };
  }

  $.MvmControls = {
    C: C, AXIS: AXIS,
    bevel: bevel, BEVEL: BEVEL, BEVEL_HOT: BEVEL_HOT, BEVEL_ACCENT: BEVEL_ACCENT,
    txt: txt, show: show, on: on, hoverBg: hoverBg,
    buildMeter: buildMeter, number: number, scrub: scrub, sw: sw, mode: mode, grid: grid, drop: drop, act: act,
    read: read, key: key, nav: nav, note: note, cap: cap, opt: opt, big: big, stepper: stepper,
    card: buildCard, column: buildColumn,
    buildOverlays: function (host) { buildMenu(host); buildTip(host); },
    openMenu: openMenu, closeMenu: closeMenu,
    setScrubScope: setScrubScope, menuOpen: function () { return !!(menu && menu.visible); },
    tipOff: tipOff, holdTip: holdTip, cursorPump: cursorPump, cancelScrub: cancelScrub,
    notice: buildNotice, armedButton: armedButton, confirmModal: buildConfirmModal,
    contextMenuShell: buildContextMenuShell, settingsPopup: buildSettingsPopup
  };
})();
)CFJS"
