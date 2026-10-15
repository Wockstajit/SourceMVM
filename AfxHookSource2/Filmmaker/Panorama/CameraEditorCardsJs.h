#pragma once

// THE CARD GRAMMAR for the CAMERA EDITOR inspector (Docs/features/camera-editor-feature.md).
// JS fragment concatenated (via #include, as an adjacent raw string literal) into the
// kCameraEditorJs body assembled in CameraEditorJs.h. MUST follow CameraEditorWidgetsJs.h -- it
// builds on that fragment's mk/lbl/btnVisual/makeSlider/attachCursorDrag/showTip.
//
// The same grammar as the CONFIG panel (ConfigHudWidgetsJs.h), only the accent differing:
//   rail     one row per card under small group headings; a row shows a BADGE (key count, a gold
//            count of animated properties, or LIVE) instead of Config's switch
//   pane     only the selected card: 3x30 accent bar, 17px title, live summary, header action /
//            Reset; sub-pages as text tabs on a rule
//   rows     a value is ONE LINE (label 130 | bar | value 66); a switch is a checkbox chip and
//            consecutive chips wrap into one row; a mode row is label + segmented control
//
// Why a separate set from CameraEditorWidgetsJs.h: that file's section()/fieldRow()/segmented()
// serve the Customize modal, the debug overlay and the render rows, which were not redesigned.
// Every name here carries an `ed` prefix because all editor fragments share ONE function scope.

R"EDJS(
    // ---- palette additions (ADDITIVE: the keys in CameraEditorJs.h are load-bearing) ----------
    S.cardBg = '#131820ff'; S.cardBgOpen = '#161b21ff'; S.cardBorderOpen = '#ffffff1f';
    S.tileBg = '#1b2129ff'; S.tileHover = '#212833ff';
    S.tileBorder = '#ffffff14'; S.tileBorderHover = '#ffffff40';
    S.muted = '#afb9c6ff'; S.rowHover = '#ffffff0e';
    S.railSel = '#1a2029ff'; S.railHover = '#1b2027ff'; S.group = '#afb9c6ff'; S.drawer = '#11151bff';

    var edCards = [];

    function edTrans(p, props, dur) {
      p.style.transitionProperty = props;
      p.style.transitionDuration = (dur || UI.mo.hover) + 's';
      p.style.transitionTimingFunction = 'ease-out';
    }
    // Hover/press state on a panel that is not a btn(): the paint function decides what the
    // state looks like, this only owns when it changes. Same contract as ConfigHudWidgetsJs.h.
    function edHoverable(p, paint) {
      p.hittest = true;
      p.__hot = false; p.__down = false;
      p.SetPanelEvent('onmouseover', function () { p.__hot = true; paint(p); if (p.__tipText) showTip(p, p.__tipText); });
      p.SetPanelEvent('onmouseout', function () { p.__hot = false; p.__down = false; paint(p); if (p.__tipText) hideTip(); });
      paint(p);
      return p;
    }
    function edPressFlash(p, paint) {
      p.__down = true; paint(p);
      try { $.Schedule(0.11, function () {
        if (!p || (p.IsValid && !p.IsValid())) return;
        p.__down = false; paint(p);
      }); }
      catch (e) {
        if (p && !(p.IsValid && !p.IsValid())) { p.__down = false; paint(p); }
      }
    }
    // Explanations live on hover. On a panel that already has hover paint (edHoverable), the tip
    // rides that handler (p.__tipText) instead of replacing it.
    function edTip(panel, text) {
      if (!text) { panel.hittest = false; return panel; }
      panel.hittest = true;
      panel.__tipText = text;
      panel.SetPanelEvent('onmouseover', function () { showTip(panel, text); });
      panel.SetPanelEvent('onmouseout', function () { hideTip(); });
      return panel;
    }
    function edNote(parent, text) {
      var l = lbl(parent, text, S.hint, 12);
      l.hittest = false; l.style.whiteSpace = 'normal';
      l.style.width = '100%'; l.style.marginTop = '6px';
      l.style.paddingLeft = UI.line.padH + 'px'; l.style.paddingRight = UI.line.padH + 'px';
      edEndTileLine(parent);
      return l;
    }
    // Groups controls INSIDE a card: a small letterspaced caption.
    function edSubHead(parent, text) {
      var r = mk('Panel', parent); r.hittest = false;
      r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginTop = '10px'; r.style.marginBottom = '1px';
      var l = lbl(r, text, S.muted, 11);
      l.hittest = false; l.style.fontWeight = 'bold'; l.style.letterSpacing = '1.5px';
      l.style.verticalAlign = 'center'; l.style.whiteSpace = 'nowrap';
      l.style.textTransform = 'uppercase';   // casing in the style, copy in natural case
      edEndTileLine(parent);
      return r;
    }

    // ---- line packing -----------------------------------------------------------------------
    // Consecutive chips share one wrapping row; anything else ends it.
    function edEndTileLine(parent) { if (parent) { parent.__tileLine = null; parent.__chipLine = null; } }
    function edChipLine(parent) {
      var line = parent.__chipLine;
      if (line && parent.GetChild(parent.GetChildCount() - 1) !== line) line = null;
      if (!line) {
        line = mk('Panel', parent); line.hittest = false; line.hittestchildren = true;
        line.style.flowChildren = 'right-wrap'; line.style.width = '100%';
        line.style.marginTop = (UI.line.gap + 1) + 'px';
        line.style.marginBottom = (-UI.line.gap) + 'px';
        parent.__chipLine = line;
      }
      return line;
    }
    // The one-line row's ground. The cursor pump repaints it for the whole gesture.
    function edPaintTile(t) {
      var held = !!(t.__held || t.__down);
      t.style.backgroundColor = (t.__hot || held) ? S.tileHover : S.tileBg;
      t.style.border = '1px solid ' + ((held && t.__moved) ? (S.accent.substring(0, 7) + '80') : S.clear);
    }

    // A value ROW: label | bar | number. Relative drag off the OS-cursor pipe: the value moves
    // with the pointer FROM WHERE IT WAS and never jumps to the click; Shift is fine control.
    //
    // opts: { label, min, max, decimals, unit, fill, tip, offAtZero, onChange(v), onBegin, onEnd }
    // Returns { sync(value), tile, row, rec }.
    function edValueTile(parent, opts) {
      var min = (typeof opts.min === 'number') ? opts.min : 0;
      var max = (typeof opts.max === 'number') ? opts.max : 100;
      var span = (max - min) || 1;
      var unit = (typeof opts.unit === 'string') ? opts.unit : '';
      var decimals = (typeof opts.decimals === 'number') ? opts.decimals : 0;
      var mul = Math.pow(10, decimals), quantum = 1 / mul;
      function quant(v) { return Math.round(v * mul) / mul; }
      function fmt(v) {
        return (opts.offAtZero && v <= min && min === 0) ? 'Off' : (v.toFixed(decimals) + unit);
      }
      edEndTileLine(parent);
      var t = mk('Panel', parent);
      t.style.flowChildren = 'right'; t.style.width = '100%';
      t.style.height = UI.line.h + 'px'; t.style.marginTop = UI.line.gap + 'px';
      t.style.paddingLeft = UI.line.padH + 'px'; t.style.paddingRight = UI.line.padH + 'px';
      t.style.borderRadius = '3px';
      edTrans(t, 'background-color, border-color');
      var nl = lbl(t, opts.label, S.label, 13);
      nl.hittest = false; nl.style.width = UI.line.label + 'px'; nl.style.verticalAlign = 'center';
      nl.style.whiteSpace = 'nowrap'; nl.style.textOverflow = 'ellipsis';
      var mid = mk('Panel', t); mid.hittest = false;
      mid.style.flowChildren = 'right'; mid.style.width = 'fill-parent-flow(1.0)';
      mid.style.height = '16px'; mid.style.verticalAlign = 'center';
      mid.style.marginLeft = UI.line.colGap + 'px'; mid.style.marginRight = UI.line.colGap + 'px';
      var ms = makeSlider(mid, 16, opts.fill || S.accent, 8);
      var vl = lbl(t, fmt(min), S.value, 14);
      vl.hittest = false; vl.style.fontWeight = 'bold';
      vl.style.width = UI.line.value + 'px'; vl.style.textAlign = 'right';
      vl.style.verticalAlign = 'center'; vl.style.whiteSpace = 'nowrap';
      vl.style.fontFamily = S.fontMonodigit;
      if (opts.tip) t.__tipText = opts.tip;
      edHoverable(t, edPaintTile);
      t.SetPanelEvent('onactivate', function () { /* the drag is cursor-driven */ });

      var lastVal = null;
      var rec = { sl: ms.sl, knob: ms.knob, setFill: ms.setFill, drag: false, busy: false,
                  __tile: t, __relative: true, __span: ms.wrap };
      function commitFrac(frac) {
        var val = quant(min + (frac < 0 ? 0 : frac > 1 ? 1 : frac) * span);
        if (lastVal !== null && Math.abs(val - lastVal) < quantum / 2) return;
        lastVal = val;
        ms.setFill((val - min) / span);
        vl.text = fmt(val);
        if (opts.onChange) opts.onChange(val);
      }
      function beginV() { if (rec.drag) return; rec.drag = true; if (opts.onBegin) opts.onBegin(); }
      function endV() { if (!rec.drag) return; rec.drag = false; if (opts.onEnd) opts.onEnd(); }
      attachCursorDrag(rec, t, commitFrac, beginV, endV);
      $.RegisterEventHandler('SliderValueChanged', ms.sl, function (p, v) {
        if (!cursorDragFallback(rec, v)) return;   // the pipe is live -> the pump handled it
        beginV(); commitFrac(v);
      });
      $.RegisterEventHandler('SliderReleased', ms.sl, function () { endV(); });

      return {
        tile: t, row: t, rec: rec,
        sync: function (value) {
          var val = quant(typeof value === 'number' ? value : min);
          if (val < min) val = min; else if (val > max) val = max;
          if (rec.__held || rec.drag || rec.sl.mousedown) return;   // the pointer owns it
          var frac = (val - min) / span;
          frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
          if (lastVal === null || Math.abs(val - lastVal) >= quantum / 2) {
            lastVal = val;
            rec.sl.value = frac;
          }
          ms.setFill(frac);
          var txt = fmt(val);
          if (vl.text !== txt) vl.text = txt;
        }
      };
    }
)EDJS"
R"EDJS(
    // A checkbox CHIP: 28px, a 13px box and a 12/700 name. Consecutive chips wrap into one row.
    // Returns { row, set(on) }.
    function edSwitchTile(parent, label, tip, onClick) {
      var line = edChipLine(parent);
      var c = mk('Panel', line); c.hittest = true;
      c.style.flowChildren = 'right'; c.style.height = '28px';
      c.style.paddingLeft = '8px'; c.style.paddingRight = '11px';
      c.style.marginRight = '6px'; c.style.marginBottom = UI.line.gap + 'px';
      c.style.borderRadius = '3px';
      edTrans(c, 'background-color, border-color', 0.08);
      var box = mk('Panel', c); box.hittest = false;
      box.style.width = '13px'; box.style.height = '13px'; box.style.borderRadius = '2px';
      box.style.verticalAlign = 'center'; box.style.marginRight = '8px';
      var mark = lbl(box, '✔', '#0c0e12ff', 9); mark.hittest = false;
      mark.style.fontWeight = 'bold'; mark.style.horizontalAlign = 'center'; mark.style.verticalAlign = 'center';
      var tl = lbl(c, label, S.hint, 12); tl.hittest = false;
      tl.style.fontWeight = 'bold'; tl.style.whiteSpace = 'nowrap'; tl.style.verticalAlign = 'center';
      var on = false;
      function paint() {
        var hot = c.__hot || c.__down;
        c.style.backgroundColor = on ? '#2bb24c1c' : '#ffffff08';
        c.style.border = '1px solid ' + (hot ? '#ffffff40' : (on ? '#2bb24c80' : '#ffffff1f'));
        box.style.backgroundColor = on ? '#35cc59ff' : S.clear;
        box.style.border = '1px solid ' + (on ? '#35cc59ff' : '#ffffff40');
        mark.visible = on;
        tl.style.color = on ? S.value : S.hint;
      }
      if (tip) c.__tipText = tip;
      edHoverable(c, paint);
      c.SetPanelEvent('onactivate', function () { edPressFlash(c, paint); if (onClick) onClick(); });
      return { row: c, pill: box, set: function (v) { v = !!v; if (v === on) return; on = v; paint(); } };
    }

    // ---- pick-one strips -------------------------------------------------------------------
    // ONE outline around the whole control; a cell shows its selection as a tinted ground.
    function edSegCell(b) {
      b.__bgBase = S.clear; b.__brBase = S.clear;
      b.__bgSel = S.accent.substring(0, 7) + '2e'; b.__bgSelHover = S.accent.substring(0, 7) + '40';
      b.__brSel = S.clear; b.__fgSel = S.value; b.__fgBase = S.label;
      b.__bgHover = '#ffffff0d'; b.__bgDown = '#ffffff14'; b.__accent = S.clear;
      btnVisual(b);
      return b;
    }
    function edSegmented(parent, items, width, onPick, font) {
      var group = mk('Panel', parent);
      group.style.flowChildren = 'right'; group.style.verticalAlign = 'center';
      group.style.width = width || 'fill-parent-flow(1.0)'; group.style.borderRadius = '3px';
      group.style.border = '1px solid #ffffff1a';
      edEndTileLine(parent);
      var btns = {}, n = items.length;
      for (var i = 0; i < n; i++) (function (key, caption, isFirst, isLast) {
        var b = btn(group, caption, function () { onPick(key); }, S.label);
        b.style.width = 'fill-parent-flow(1.0)';
        b.style.paddingTop = '6px'; b.style.paddingBottom = '6px';
        b.style.paddingLeft = '4px'; b.style.paddingRight = '4px';
        b.style.marginRight = '0px';
        b.style.borderRadius = '0px';
        b.__lbl.style.fontSize = (font || 12) + 'px';
        b.__lbl.style.fontWeight = 'bold';
        b.__lbl.style.horizontalAlign = 'center';
        b.__lbl.style.whiteSpace = 'nowrap';
        edSegCell(b);
        btns[key] = b;
      })(items[i][0], items[i][1], i === 0, i === n - 1);
      return {
        panel: group, btns: btns,
        sync: function (key) { for (var k in btns) btnSel(btns[k], k === key); }
      };
    }
    // A labelled MODE ROW: the 130px label column, then the segmented control filling the rest.
    function edModeRow(parent, label, items, width, onPick, tip, font) {
      var r = mk('Panel', parent); r.hittest = false; r.hittestchildren = true;
      r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginTop = (UI.line.gap + 1) + 'px';
      r.style.paddingLeft = UI.line.padH + 'px';
      edEndTileLine(parent);
      var nl = lbl(r, label, S.label, 13);
      nl.style.width = UI.line.label + 'px'; nl.style.whiteSpace = 'nowrap';
      nl.style.textOverflow = 'ellipsis'; nl.style.verticalAlign = 'center';
      edTip(nl, tip);
      var seg = edSegmented(r, items, 'fill-parent-flow(1.0)', onPick, font && font < 13 ? font : 12);
      seg.panel.style.marginLeft = UI.line.colGap + 'px';
      seg.row = r; seg.label = nl;
      return seg;
    }
    // A pick-one with every option on screen: label column + wrapping cells.
    function edOptionGrid(parent, label, items, perRow, tip, onPick, font) {
      var r = mk('Panel', parent); r.hittest = false; r.hittestchildren = true;
      r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginTop = (UI.line.gap + 1) + 'px';
      r.style.paddingLeft = UI.line.padH + 'px';
      edEndTileLine(parent);
      var cap = lbl(r, label, S.label, 13);
      cap.style.width = UI.line.label + 'px'; cap.style.whiteSpace = 'nowrap'; cap.style.paddingTop = '5px';
      edTip(cap, tip);
      var cells = mk('Panel', r); cells.hittest = false; cells.hittestchildren = true;
      cells.style.flowChildren = 'right-wrap'; cells.style.width = 'fill-parent-flow(1.0)';
      cells.style.marginLeft = UI.line.colGap + 'px';
      var btns = {};
      for (var i = 0; i < items.length; i++) (function (key, caption, idx) {
        var b = btn(cells, caption, function () { onPick(key, idx); }, S.label);
        b.style.marginRight = '4px'; b.style.marginBottom = '4px';
        b.style.paddingTop = '5px'; b.style.paddingBottom = '5px';
        b.style.paddingLeft = '10px'; b.style.paddingRight = '10px';
        b.__lbl.style.fontSize = '12px'; b.__lbl.style.fontWeight = 'bold'; b.__lbl.style.whiteSpace = 'nowrap';
        edSegCell(b);
        b.__brBase = '#ffffff1a'; b.__brSel = S.accent.substring(0, 7) + '99'; btnVisual(b);
        btns[key] = b;
      })(items[i][0], items[i][1], i);
      return {
        row: r, btns: btns, label: cap,
        sync: function (key) { for (var k in btns) btnSel(btns[k], k === key); }
      };
    }
)EDJS"
R"EDJS(
    // A named set of commands. With a name: the label column and the buttons wrapping beside it.
    // With no name: the buttons on ONE right-aligned line. Buttons never stack under a gutter.
    // items: [[caption, onClick, variant]], variant 'primary' | 'danger' | undefined.
    function edButton(parent, caption, onClick, variant) {
      var b = (variant === 'primary') ? btn(parent, caption, onClick, S.accent)
            : (variant === 'danger') ? btnDanger(parent, caption, onClick)
            : btn(parent, caption, onClick, S.label);
      if (variant === 'primary') {
        // Outlined primary (Docs/features/camera-editor-feature.md): gold text on a faint gold ground, so the header's one
        // solid fill stays the only solid accent in the pane.
        b.__bgBase = S.accent.substring(0, 7) + '1f'; b.__brBase = S.accent.substring(0, 7) + '80';
        b.__fgBase = S.accent; b.__bgHover = S.accent.substring(0, 7) + '33'; b.__fgHover = S.accent;
        btnVisual(b);
      }
      b.style.paddingTop = '5px'; b.style.paddingBottom = '5px';
      b.style.paddingLeft = '11px'; b.style.paddingRight = '11px';
      b.__lbl.style.fontSize = '12px'; b.__lbl.style.fontWeight = 'bold';
      b.__lbl.style.whiteSpace = 'nowrap'; b.__lbl.style.horizontalAlign = 'center';
      return b;
    }
    function edActionGroup(parent, label, items, perRow, note) {
      edEndTileLine(parent);
      var r = mk('Panel', parent); r.hittest = false; r.hittestchildren = true;
      r.style.flowChildren = 'down'; r.style.width = '100%';
      r.style.marginTop = (UI.line.gap + 3) + 'px';
      var line = mk('Panel', r); line.hittest = false; line.hittestchildren = true;
      line.style.flowChildren = 'right'; line.style.width = '100%';
      line.style.paddingLeft = label ? UI.line.padH + 'px' : '0px';
      // The name sits in a box as tall as one button row and is centred in it: a padded label
      // landed a few px off the buttons' centre line (the Placement row read as misaligned).
      var capBox = mk('Panel', line); capBox.hittest = false;
      capBox.style.width = UI.line.label + 'px'; capBox.style.height = '32px';
      var cap = lbl(capBox, label || '', S.label, 13); cap.hittest = false;
      cap.style.width = '100%'; cap.style.whiteSpace = 'nowrap';
      cap.style.textOverflow = 'ellipsis'; cap.style.verticalAlign = 'center';
      var cells = line;
      if (label) {
        // Explicit rows of `perRow` buttons. 'right-wrap' put the wrapped row half on top of the
        // first in game (Time of Day's Night / Moonlit over Dawn / Day), so the rows are laid out
        // here rather than left to the wrap.
        cells = mk('Panel', line); cells.hittest = false; cells.hittestchildren = true;
        cells.style.flowChildren = 'down'; cells.style.width = 'fill-parent-flow(1.0)';
        cells.style.marginLeft = UI.line.colGap + 'px';
      } else {
        capBox.visible = false;
        var push = mk('Panel', line); push.hittest = false;
        push.style.width = 'fill-parent-flow(1.0)'; push.style.height = '1px';
      }
      var btns = [], rowPanel = cells;
      var rowSize = (label && perRow > 0) ? perRow : items.length;
      for (var i = 0; i < items.length; i++) (function (caption, onClick, variant, index) {
        if (label && index % rowSize === 0) {
          rowPanel = mk('Panel', cells); rowPanel.hittest = false; rowPanel.hittestchildren = true;
          rowPanel.style.flowChildren = 'right'; rowPanel.style.width = '100%';
        }
        var b = edButton(rowPanel, caption, onClick, variant);
        // One button height across the inspector: the same 32px as the THIS CAMERA row.
        b.style.height = '32px'; b.style.paddingTop = '0px'; b.style.paddingBottom = '0px';
        b.__lbl.style.verticalAlign = 'center';
        b.style.marginRight = label ? '4px' : '0px'; b.style.marginLeft = label ? '0px' : '6px';
        b.style.marginBottom = label ? '4px' : '0px';
        btns.push(b);
      })(items[i][0], items[i][1], items[i][2], i);
      // `note` is created for an EMPTY string too: several callers pass '' and fill it later.
      var noteLbl = null;
      if (note !== null && note !== undefined) {
        noteLbl = lbl(r, note, S.hint, 12);
        noteLbl.hittest = false; noteLbl.style.whiteSpace = 'normal';
        noteLbl.style.width = '100%'; noteLbl.style.marginTop = '4px';
        noteLbl.style.paddingLeft = UI.line.padH + 'px'; noteLbl.style.paddingRight = UI.line.padH + 'px';
      }
      return { row: r, btns: btns, note: noteLbl, cap: cap };
    }
    // "THIS CAMERA" (Docs/features/camera-editor-feature.md; original rail fix): a small heading, then ONE row of equal-width 32px buttons.
    function edEvenActions(parent, heading, items) {
      edEndTileLine(parent);
      var r = mk('Panel', parent); r.hittest = false; r.hittestchildren = true;
      r.style.flowChildren = 'down'; r.style.width = '100%'; r.style.marginTop = '10px';
      var h = lbl(r, heading, S.muted, 11); h.hittest = false;
      h.style.fontWeight = 'bold'; h.style.letterSpacing = '1.5px'; h.style.marginBottom = '6px';
      h.style.textTransform = 'uppercase';
      var line = mk('Panel', r); line.hittest = false; line.hittestchildren = true;
      line.style.flowChildren = 'right'; line.style.width = '100%';
      var btns = [];
      for (var i = 0; i < items.length; i++) (function (caption, onClick, variant, last) {
        var b = edButton(line, caption, onClick, variant);
        b.style.width = 'fill-parent-flow(1.0)'; b.style.height = '32px';
        b.style.paddingTop = '0px'; b.style.paddingBottom = '0px';
        b.__lbl.style.verticalAlign = 'center';
        b.style.marginRight = last ? '0px' : '6px';
        btns.push(b);
      })(items[i][0], items[i][1], items[i][2], i === items.length - 1);
      return { row: r, btns: btns, head: h };
    }
)EDJS"
R"EDJS(
    // ---- the rail ----------------------------------------------------------------------------
    // Each inspector tab is a RAIL (one row per card, under group headings) and a PANE that shows
    // only the selected card. A card that is not selected is hidden by its own `visible`, never
    // torn down, so every sync keeps writing to it. Groups appear in first-use order.
    var edRails = {};
    var edActiveTab = '';   // the inspector tab whose rail and card are showing
    function edRail(tab) {
      var r = edRails[tab];
      if (r) return r;
      r = { tab: tab, groups: {}, groupOrder: [], items: {}, order: [], sel: '', host: mk('Panel', edRailHost) };
      r.host.hittest = false; r.host.hittestchildren = true;
      r.host.style.flowChildren = 'down'; r.host.style.width = '100%';
      r.host.visible = false;
      edRails[tab] = r;
      return r;
    }
    function edRailGroup(r, name) {
      var g = r.groups[name];
      if (g && g.IsValid && !g.IsValid()) g = null;
      if (g) return g;
      g = mk('Panel', r.host); g.hittest = false; g.hittestchildren = true;
      g.style.flowChildren = 'down'; g.style.width = '100%';
      if (r.groupOrder.length) g.style.marginTop = UI.rail.groupGap + 'px';
      var h = lbl(g, name, S.group, 11); h.hittest = false;
      h.style.fontWeight = 'bold'; h.style.letterSpacing = '1.8px'; h.style.textTransform = 'uppercase';
      h.style.paddingLeft = '2px'; h.style.marginBottom = '2px';
      g.__count = 0;
      r.groups[name] = g; r.groupOrder.push(name);
      return g;
    }
    function edPaintRailRow(it) {
      var sel = it.rail.sel === it.key, hot = it.row.__hot;
      it.row.style.backgroundColor = sel ? S.railSel : (hot ? S.railHover : S.clear);
      it.row.style.border = '1px solid ' + (sel ? (S.accent.substring(0, 7) + '59') : S.clear);
      it.title.style.color = sel ? S.value : S.label;
    }
    function edRailAdd(tab, key, group, title, accent, card) {
      var r = edRail(tab), g = edRailGroup(r, group || 'CARDS');
      var it = { rail: r, key: key, card: card, group: group };
      var rw = mk('Panel', g); rw.hittest = true;
      rw.style.flowChildren = 'right'; rw.style.width = '100%';
      rw.style.height = UI.rail.rowH + 'px'; rw.style.marginTop = UI.rail.rowGap + 'px';
      rw.style.borderRadius = '3px'; rw.style.overflow = 'clip';
      edTrans(rw, 'background-color, border-color', 0.08);
      var edge = mk('Panel', rw); edge.hittest = false;
      edge.style.width = UI.rail.edge + 'px'; edge.style.height = '100%';
      edge.style.backgroundColor = accent;
      var col = mk('Panel', rw); col.hittest = false;
      col.style.flowChildren = 'down'; col.style.width = 'fill-parent-flow(1.0)';
      col.style.verticalAlign = 'center'; col.style.marginLeft = '9px'; col.style.marginRight = '6px';
      var t = lbl(col, title, S.label, 12); t.hittest = false;
      t.style.fontWeight = 'bold'; t.style.letterSpacing = '0.8px';
      t.style.whiteSpace = 'nowrap'; t.style.textOverflow = 'ellipsis'; t.style.width = '100%';
      var sum = lbl(col, '', S.muted, 11); sum.hittest = false;
      sum.style.marginTop = '2px'; sum.style.whiteSpace = 'nowrap';
      sum.style.textOverflow = 'ellipsis'; sum.style.width = '100%';
      var badge = mk('Panel', rw); badge.hittest = false; badge.visible = false;
      badge.style.verticalAlign = 'center'; badge.style.marginRight = '8px';
      badge.style.borderRadius = '3px'; badge.style.paddingLeft = '6px'; badge.style.paddingRight = '6px';
      badge.style.height = '18px';
      var bl = lbl(badge, '', S.label, 11); bl.hittest = false;
      bl.style.fontWeight = 'bold'; bl.style.letterSpacing = '0.6px';
      bl.style.verticalAlign = 'center'; bl.style.fontFamily = S.fontMonodigit;
      it.row = rw; it.edge = edge; it.title = t; it.sum = sum; it.badge = badge; it.badgeLbl = bl;
      // The title and summary are cut with an ellipsis; the tooltip carries them whole.
      rw.__tipText = title;
      edHoverable(rw, function () { edPaintRailRow(it); });
      rw.SetPanelEvent('onactivate', function () { edSelectCard(tab, key, true); });
      r.items[key] = it; r.order.push(key);
      g.__count++;
      r.host.visible = (tab === edActiveTab);
      if (!r.sel) edSelectCard(tab, key, false);
      else { card.visible = false; edPaintRailRow(it); }
      return it;
    }
    // Drop a card's rail row (the Scene Keys catalog rebuilds its cards); an emptied group goes too.
    function edRailRemove(tab, key) {
      var r = edRails[tab]; if (!r || !r.items[key]) return;
      var it = r.items[key];
      try { it.row.DeleteAsync(0); } catch (e) {}
      delete r.items[key];
      var oi = r.order.indexOf(key); if (oi >= 0) r.order.splice(oi, 1);
      var g = r.groups[it.group || 'CARDS'];
      if (g && --g.__count <= 0) {
        try { g.DeleteAsync(0); } catch (e2) {}
        delete r.groups[it.group || 'CARDS'];
        var gi = r.groupOrder.indexOf(it.group || 'CARDS'); if (gi >= 0) r.groupOrder.splice(gi, 1);
      }
      if (r.sel === key) { r.sel = ''; if (r.order.length) edSelectCard(tab, r.order[0], false); }
    }
    function edSelectCard(tab, key, persist) {
      var r = edRails[tab];
      if (!r || !r.items[key]) return false;
      var changed = r.sel !== key;
      r.sel = key;
      for (var k in r.items) {
        r.items[k].card.visible = (k === key) && (tab === edActiveTab);
        edPaintRailRow(r.items[k]);
      }
      if (changed) { try { edScrollY = 0; } catch (e) {} }
      if (persist) cmd('mirv_filmmaker settings ui editor.' + tab + '.sel ' + key);
      return true;
    }
    // Which rail is showing: the inspector tab. Called from render() every frame.
    function edApplyRails(tab) {
      if (tab === edActiveTab) return;
      edActiveTab = tab;
      for (var k in edRails) {
        var r = edRails[k];
        r.host.visible = (k === tab);
        for (var key in r.items) r.items[key].card.visible = (k === tab) && (r.sel === key);
      }
    }
)EDJS"
R"EDJS(
    // ---- the card ---------------------------------------------------------------------------
    // opts: { edge, rail: { tab, key, group } }
    // Returns the BODY panel, carrying __page / __setPage / __pageCount / __note / __badge /
    // __headerAction / __headerReset / __headerPill / __setOpen / __getOpen / __card.
    function edCard(parent, title, opts) {
      opts = opts || {};
      var edgeOn = opts.edge || S.accent;
      var s = mk('Panel', parent); s.hittest = false; s.hittestchildren = true;
      s.style.flowChildren = 'down'; s.style.width = '100%';
      edEndTileLine(parent);

      var hr = mk('Panel', s); hr.hittest = false; hr.hittestchildren = true;
      hr.style.flowChildren = 'right'; hr.style.width = '100%';
      var edge = mk('Panel', hr); edge.hittest = false;
      edge.style.width = '3px'; edge.style.height = '30px';
      edge.style.borderRadius = '1px'; edge.style.backgroundColor = edgeOn;
      edge.style.verticalAlign = 'center'; edge.style.marginRight = '10px';
      var col = mk('Panel', hr); col.hittest = false;
      col.style.flowChildren = 'down'; col.style.width = 'fill-parent-flow(1.0)';
      col.style.verticalAlign = 'center';
      var t = lbl(col, title, S.value, 17);
      t.hittest = false; t.style.fontWeight = 'bold'; t.style.letterSpacing = '1.2px';
      t.style.whiteSpace = 'nowrap'; t.style.textOverflow = 'ellipsis'; t.style.width = '100%';
      var note = lbl(col, '', S.muted, 12);
      note.hittest = false; note.style.marginTop = '2px'; note.style.whiteSpace = 'nowrap';
      note.style.textOverflow = 'ellipsis'; note.style.width = '100%';
      var slots = mk('Panel', hr); slots.hittest = false; slots.hittestchildren = true;
      slots.style.flowChildren = 'right'; slots.style.width = 'fit-children';
      slots.style.verticalAlign = 'center';

      var clipper = mk('Panel', s); clipper.hittest = false; clipper.hittestchildren = true;
      clipper.style.flowChildren = 'down';
      clipper.style.width = 'fill-parent-flow(1.0)'; clipper.style.marginTop = '6px';
      // The page strip is created INSIDE the body by the first __page() call, so controls a card
      // builds before its pages sit above the strip (the rail's "top" controls (Docs/features/camera-editor-feature.md)).
      var strip = null;
      var body = mk('Panel', clipper); body.hittest = false; body.hittestchildren = true;
      body.style.flowChildren = 'down'; body.style.width = '100%';

      var item = opts.rail ? edRailAdd(opts.rail.tab, opts.rail.key, opts.rail.group, title, edgeOn, s) : null;

      var pages = [], pageTabs = [], curPage = 0;
      var pageKey = opts.rail ? ('editor.' + opts.rail.key + '.page') : '';
      function applyPages() {
        for (var i = 0; i < pages.length; i++) {
          pages[i].visible = (i === curPage);
          styleTab(pageTabs[i], i === curPage, edgeOn);
        }
      }
      body.__page = function (head) {
        var idx = pages.length;
        if (!strip) {
          edEndTileLine(body);
          strip = mk('Panel', body); strip.hittest = false; strip.hittestchildren = true;
          strip.style.flowChildren = 'right'; strip.style.width = '100%';
          strip.style.marginTop = (body.GetChildCount() > 1 ? 8 : 2) + 'px'; strip.style.marginBottom = '3px';
          strip.style.borderBottom = '1px solid #ffffff10';
          strip.visible = false;
        }
        var pg = mk('Panel', body); pg.hittest = false; pg.hittestchildren = true;
        pg.style.flowChildren = 'down'; pg.style.width = '100%';
        pg.__note = function () {};
        pg.__card = s;
        pages.push(pg);
        var tb = tabify(btn(strip, ('' + head).toUpperCase(), function () {
          curPage = idx; applyPages();
          if (pageKey) cmd('mirv_filmmaker settings ui ' + pageKey + ' ' + idx);
        }, S.label));
        tb.style.width = 'fit-children';
        tb.style.paddingLeft = '0px'; tb.style.paddingRight = '0px';
        tb.style.paddingTop = '6px'; tb.style.paddingBottom = UI.tab.padBottom + 'px';
        tb.style.marginRight = UI.tab.gap + 'px';
        tb.__lbl.style.fontSize = UI.tab.font + 'px';
        tb.__lbl.style.letterSpacing = UI.tab.ls + 'px';
        // Active page text is the value tier; the accent is only the underline.
        tb.__fgSel = S.value;
        pageTabs.push(tb);
        strip.visible = pages.length > 1;
        applyPages();
        return pg;
      };
      body.__setPage = function (i) {
        i = parseInt(i, 10) || 0;
        if (i < 0 || i >= pages.length) i = 0;
        curPage = i; applyPages();
      };
      body.__pageCount = function () { return pages.length; };
      body.__pageKey = pageKey;

      var noteSeen = null;
      body.__note = function (text) {
        var v = (text === null || text === undefined) ? '' : ('' + text);
        if (v === noteSeen) return;
        noteSeen = v;
        note.text = v;
        note.visible = v.length > 0;
        // Both copies can be cut with an ellipsis, so both keep the full text on hover.
        note.__tipText = v || null;
        if (v && !note.__tipBound) {
          note.__tipBound = true; note.hittest = true;
          note.SetPanelEvent('onmouseover', function () { if (note.__tipText) showTip(note, note.__tipText); });
          note.SetPanelEvent('onmouseout', function () { hideTip(); });
        }
        if (item) { item.sum.text = v; item.row.__tipText = v ? (title + ' — ' + v) : title; }
      };
      // The rail row's badge: kind 'count' (neutral), 'gold' (animated), 'live' (green).
      var badgeSeen = null;
      body.__badge = function (text, kind) {
        if (!item) return;
        var sig = (text || '') + '|' + (kind || '');
        if (sig === badgeSeen) return;
        badgeSeen = sig;
        item.badge.visible = !!text;
        if (!text) return;
        item.badgeLbl.text = '' + text;
        var bg = kind === 'gold' ? S.accent : (kind === 'live' ? '#4cd96dff' : '#ffffff12');
        var fg = (kind === 'gold' || kind === 'live') ? '#0c0e12ff' : S.label;
        item.badge.style.backgroundColor = bg; item.badgeLbl.style.color = fg;
      };
      body.__card = s;
      body.__title = title;   // api.card() / api.cardPage() address a card by its title

      body.__headerAction = function (cap, onClick) {
        var b = btnPrimary(slots, cap, onClick);
        b.style.marginLeft = '8px'; b.style.marginRight = '0px';
        b.style.verticalAlign = 'center';
        b.style.paddingTop = '5px'; b.style.paddingBottom = '5px';
        b.style.paddingLeft = '11px'; b.style.paddingRight = '11px';
        b.__lbl.style.fontSize = '12px'; b.__lbl.style.whiteSpace = 'nowrap';
        return b;
      };
      body.__headerReset = function (onClick) {
        var b = btn(slots, 'Reset', onClick, S.label);
        b.__bgBase = S.clear; b.__brBase = '#ffffff1f'; btnVisual(b);
        b.style.marginLeft = '8px'; b.style.marginRight = '0px';
        b.style.verticalAlign = 'center';
        b.style.paddingTop = '5px'; b.style.paddingBottom = '5px';
        b.style.paddingLeft = '11px'; b.style.paddingRight = '11px';
        b.__lbl.style.fontSize = '12px'; b.__lbl.style.whiteSpace = 'nowrap';
        return b;
      };
      // Square switch (42x22), same colours as Config's.
      body.__headerPill = function (onClick, tip) {
        var p = mk('Panel', slots); p.hittest = true;
        p.style.width = UI.sw.w + 'px'; p.style.height = UI.sw.h + 'px';
        p.style.borderRadius = '3px';
        p.style.verticalAlign = 'center'; p.style.marginLeft = '8px';
        edTrans(p, 'background-color, border-color', 0.08);
        var knob = mk('Panel', p); knob.hittest = false;
        knob.style.width = UI.sw.knob + 'px'; knob.style.height = UI.sw.knob + 'px';
        knob.style.borderRadius = '2px'; knob.style.backgroundColor = '#dfe5ecff';
        knob.style.verticalAlign = 'center'; knob.style.horizontalAlign = 'left';
        knob.style.marginLeft = '3px';
        knob.style.transitionProperty = 'transform';
        knob.style.transitionDuration = '0.12s';
        knob.style.transitionTimingFunction = 'ease-out';
        var on = false, hot = false;
        function paint() {
          p.style.backgroundColor = on ? (hot ? '#35cc59ff' : '#2bb24cff') : (hot ? '#ffffff26' : '#00000088');
          p.style.border = '1px solid ' + (on ? '#1c8a3aff' : (hot ? '#ffffff44' : '#ffffff26'));
          knob.style.transform = 'translateX(' + (on ? UI.sw.travel : 0) + 'px)';
          edge.style.backgroundColor = on ? edgeOn : '#2a3037ff';
          note.style.color = S.muted;
        }
        p.SetPanelEvent('onmouseover', function () { hot = true; paint(); });
        p.SetPanelEvent('onmouseout', function () { hot = false; paint(); });
        p.SetPanelEvent('onactivate', function () { if (onClick) onClick(); });
        if (tip) edTip(p, tip);
        paint();
        return { row: p, pill: p, knob: knob,
                 set: function (v) { v = !!v; if (v === on) return; on = v; paint(); } };
      };

      // With the rail there is no "shut": opening SELECTS the card (api.card keeps working).
      body.__setOpen = function (v) { if (v && opts.rail) edSelectCard(opts.rail.tab, opts.rail.key, false); };
      body.__getOpen = function () { return !opts.rail || (edRails[opts.rail.tab] && edRails[opts.rail.tab].sel === opts.rail.key); };
      body.__rail = opts.rail || null;
      edCards.push(body);
      return body;
    }
)EDJS"
