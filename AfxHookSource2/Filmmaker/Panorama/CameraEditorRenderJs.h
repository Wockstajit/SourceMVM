#pragma once

// Panorama JS fragment for the inline RENDER workspace -- concatenated (via #include, as adjacent
// raw string literals) into the kCameraEditorJs script body assembled in CameraEditorJs.h.
// Not a standalone script -- shares scope/closures with the root IIFE and uses the S palette and
// widget factories (btn, lbl, mk, tabify, styleTab, btnSel, btnEnable) from CameraEditorWidgetsJs.h.
//
// Architecture:
//   * CamEditorRenderRoot is a TOP-LEVEL sibling of CamEditorRoot, NOT a child.
//   * z-index 54: above editor chrome, below the bottom timeline/graph roots.
//   * No backdrop and no click-outside dismissal; the live preview remains visible.
//   * CameraEditorRenderLayoutJs.h owns the three-column geometry and the click shields; this
//     file only fills renderSettingsCol (right) and renderJobCol (left).
//   * All state from st.render (built by Render_BuildStateJson, pushed by CameraEditorHud).
//   * All actions issue "mirv_filmmaker render ..." commands.
//
// Why the JOB column is persistent and separate from the settings tabs: with tabbed settings
// alone there was nowhere on screen that answered "so what happens if I press go" -- the range,
// the frame count, the destination and the reason Start is refusing all lived in different tabs
// or nowhere at all.
//
// Honesty rule (plan section 10): a control that cannot do its job yet says why, in its own words,
// sourced from C++ (`formatNote`, the validation entries, the `avail` block). The JS never invents
// a reason and never presents an unavailable option as available -- a camera source whose
// preconditions are missing shakes and explains instead of firing a command that would only warn
// to a console the user does not have open.

#include "CameraEditorRenderLayoutJs.h"
R"EDJS(
    var RENDER_ERROR_TEXT = '#fc9994ff';
    var RANGE_MODE_LABELS = ['Full Demo', 'Round', 'Camera Path', 'Segment', 'Custom In/Out'];
    var RANGE_MODE_TOKENS = ['full', 'round', 'path', 'segment', 'custom'];
    var CAM_SOURCES = [
      ['current', 'Current Viewer'], ['pov', 'First-Person POV'], ['thirdperson', 'Third-Person'],
      ['free', 'Free Camera'], ['path', 'Camera Path'], ['follow', 'Follow Camera']
    ];
    var CAM_SOURCE_HINTS = [
      'Whatever is on screen right now. Nothing is switched.',
      'Switches to first-person (in-eye) on the spectated player.',
      'Switches to the third-person orbit camera on the spectated player.',
      'Switches to the free fly camera (WASD + mouse in GAME-mouse mode).',
      'Plays the authored camera path. Needs 2+ camera keys.',
      'Tracks the follow target. Pick a target in the editor Follow tab first.'
    ];
    var FPS_CHOICES = [24, 30, 50, 60, 120];
    var CRF_CHOICES = [[14, 'Highest'], [18, 'High'], [23, 'Medium'], [28, 'Small file']];
    var DEMO_TICKRATE = 64; // CS2 demos; readouts label themselves with it rather than hiding it

    // ---- typed number fields ---------------------------------------------
    // CS2 does not give an in-game HUD Panorama TextEntry real keyboard focus, so a field here is
    // a plain panel that we focus ourselves; the characters arrive from C++ (WM_CHAR -> the same
    // queue the Customize search boxes use) and are dispatched by renderFieldChar below. The
    // "textcapture" attribute tells C++ to route them here AND to stop the same keystroke from
    // also driving the demo (see CameraEditorHud::TextCaptureActive).
    var renderFocusField = null;
    var renderPressDown = false;
    var renderLastTab = -1;   // drives the close-popups-on-tab-change guard in renderRender()
    function renderPublishTextCapture() {
      try { root.SetAttributeString('textcapture', renderFocusField ? '1' : '0'); } catch (e) {}
    }
    // A real blinking caret. The static '_' the focused field used to show gave no sign the box
    // was live and taking keys -- which is exactly what "I click it, I type, nothing happens"
    // looks like even when the characters ARE arriving.
    var renderCaretOn = true, renderCaretGen = 0;
    function renderCaretTick(gen) {
      if (gen !== renderCaretGen || !renderFocusField) return;
      renderCaretOn = !renderCaretOn;
      renderFieldPaint(renderFocusField);
      try { $.Schedule(0.5, function () { renderCaretTick(gen); }); } catch (e) {}
    }
    function renderFieldPaint(rec) {
      var focused = (renderFocusField === rec);
      if (focused) {
        rec.lbl.text = rec.buf + (renderCaretOn ? '|' : ' ');
      } else {
        rec.lbl.text = rec.value() || rec.placeholder;
      }
      rec.lbl.style.color = focused ? S.render : (rec.hot ? S.value : S.value);
      rec.panel.style.border = '1px solid ' + (focused ? S.render : (rec.hot ? '#ffffff55' : S.cardBorder));
      rec.panel.style.backgroundColor = focused ? S.renderOn : (rec.hot ? S.btnHover : S.btnBg);
    }
    function renderFieldBlur() {
      var prev = renderFocusField;
      renderFocusField = null;
      renderCaretGen++;   // retire the blink loop
      if (prev) renderFieldPaint(prev);
      renderPublishTextCapture();
    }
    function renderFieldFocus(rec) {
      var prev = renderFocusField;
      renderFocusField = rec; rec.buf = '';
      renderCaretOn = true; renderCaretGen++;
      (function (g) { try { $.Schedule(0.5, function () { renderCaretTick(g); }); } catch (e) {} })(renderCaretGen);
      if (prev && prev !== rec) renderFieldPaint(prev);
      renderFieldPaint(rec);
      renderPublishTextCapture();
    }
    // Returns 'ok' when the character was consumed here, so the Customize modal's own handler is
    // not also fed the same keystroke.
    function renderFieldChar(code) {
      var rec = renderFocusField;
      if (!rec) return false;
      if (code === 13) {                       // Enter -- commit
        var text = rec.buf;
        renderFieldBlur();
        if (text.length) rec.commit(text);
        return true;
      }
      if (code === 27) { renderFieldBlur(); return true; }   // Esc -- abandon
      if (code === 8) { rec.buf = rec.buf.slice(0, -1); renderFieldPaint(rec); return true; }
      if (code >= 48 && code <= 57) {          // digits
        if (rec.buf.length < 9) rec.buf += String.fromCharCode(code);
        renderFieldPaint(rec); return true;
      }
      if (rec.pair && (code === 120 || code === 88 || code === 32 || code === 44)) {
        // x / X / space / comma all separate WIDTH from HEIGHT, so any habit works.
        if (rec.buf.indexOf('x') < 0 && rec.buf.length) rec.buf += 'x';
        renderFieldPaint(rec); return true;
      }
      return true; // swallow everything else while focused rather than letting it leak
    }
    // `getValue` renders the CURRENT committed value; `commit` receives the typed string.
    function renderNumberField(parent, placeholder, pair, getValue, commit) {
      var f = mk('Panel', parent);
      f.style.borderRadius = '3px'; f.style.width = 'fill-parent-flow(1.0)';
      f.style.paddingTop = '6px'; f.style.paddingBottom = '6px';
      f.style.paddingLeft = '9px'; f.style.paddingRight = '9px';
      var l = lbl(f, '', S.value, 12); l.hittest = false;
      l.style.fontWeight = 'bold'; l.style.whiteSpace = 'nowrap';
      var rec = { panel: f, lbl: l, buf: '', hot: false, placeholder: placeholder, pair: !!pair, value: getValue, commit: commit };
      f.hittest = true;
      f.SetPanelEvent('onactivate', function () { renderFieldFocus(rec); });
      f.SetPanelEvent('onmouseover', function () { rec.hot = true; renderFieldPaint(rec); });
      f.SetPanelEvent('onmouseout', function () { rec.hot = false; renderFieldPaint(rec); });
      renderFieldPaint(rec);
      return rec;
    }

    // Console-testable focus (see api.renderFocus): the typed path has no keyboard-free way to be
    // exercised otherwise, because focusing normally requires a real click on the field.
    function renderFocusFieldByName(which) {
      // Trim: "mirv_filmmaker editor eval" rejoins its argv with spaces, so a quoted argument
      // arrives padded (" fps ") and an untrimmed compare silently fell through to blur --
      // which looked exactly like the typed-field path being broken.
      which = ('' + (which || '')).replace(/^\s+|\s+$/g, '');
      if (which === 'fps') renderFieldFocus(fpsField);
      else if (which === 'bitrate') renderFieldFocus(bitrateField);
      else renderFieldBlur();
    }

)EDJS"
R"EDJS(
    // ================= RIGHT COLUMN: settings inspector =================
    // Design v2 ("RENDER — v2, NO RAIL", original frames 6a-6c; current contract:
    // Docs/features/filmmaker_render_passes.md): three full-width tabs, each ONE page of headed sections that fits
    // the column, so nothing hides behind a popup and nothing needs much scrolling.
    //   RANGE & CAMERA  the range-mode strip, the resolved span as a read-only readout (the span
    //                   is changed from the playhead bar pinned at the column's foot), then the
    //                   camera sources as a grid of radio tiles
    //   PASSES          the pass matrix with a View control for showing a pass live in camera,
    //                   KEYING & DEPTH (greyed until a pass uses it), GAME HUD and the extras
    //   OUTPUT          DESTINATION (folder + format grid), PICTURE (size + frame rate) and
    //                   ENCODING (MP4 only)
    // The C++ modal keeps its four tab slots (`render tab 0..3`, Render_GetModalTab): RANGE &
    // CAMERA is slot 0 and also answers slot 1, so every control sends the command it always did.
    var RS_AC = '#aeb4bc', RS_GOLD = '#f0b323';
    var RS_MUTED = '#8b95a3ff', RS_DIM = '#6b7480ff';
    var RS_TILE = '#1b2129ff', RS_TILE_HOVER = '#212833ff';
    // A blocked option is drawn in FULL-OPACITY colours, never dimmed as a whole: the one line
    // that has to stay readable on a blocked option is the reason it is blocked.
    var RS_BLOCK_TEXT = '#f4a259ff', RS_BLOCK_BORDER = '#c96a3aff', RS_BLOCK_BG = '#171b21ff';
    var RENDER_VIEWS = [['RANGE & CAMERA', 0], ['PASSES', 2], ['OUTPUT', 3]];
    function renderViewOf(tab) { return tab === 2 ? 1 : (tab === 3 ? 2 : 0); }

    var renderHead = mk('Panel', renderSettingsCol); renderHead.id = 'RndHead';
    renderHead.style.flowChildren = 'right'; renderHead.style.width = '100%';
    renderHead.style.paddingLeft = UI.col.pad + 'px'; renderHead.style.paddingRight = UI.col.pad + 'px';
    // The inspector's own paddingTop (CameraEditorJs.h / ConfigHudJs.h), so Back sits on the SAME
    // baseline as Exit in the other two workspaces.
    renderHead.style.paddingTop = UI.col.pad + 'px'; renderHead.style.paddingBottom = '0px';
    var renderTitle = lbl(renderHead, 'RENDER', S.render, 18);
    renderTitle.style.fontWeight = 'bold'; renderTitle.style.letterSpacing = '2.5px';
    renderTitle.style.verticalAlign = 'center'; renderTitle.style.width = 'fill-parent-flow(1.0)';
    // The chime when a job ends (RenderDoneSound.h). Like every header chip it says its own state;
    // the volume is in OUTPUT > WHEN THE RENDER ENDS.
    var renderSoundChip = mvmChromeChip(renderHead,
      function (parent, t, f) {
        return btn(parent, t, f, S.label, { tip: 'Play a sound when a render finishes, a lower one if it fails. Volume: OUTPUT tab.' });
      },
      btnVisual, 'SOUND: ON', function () { cmd('mirv_filmmaker render sound toggle'); });
    renderSoundChip.id = 'RndSound';
    var renderBackBtn = mvmChromeChip(renderHead,
      function (parent, t, f) { return btn(parent, t, f, S.label, { tip: 'Return to the Camera Editor.' }); },
      btnVisual, '✕ Back', function () { closeRender(); });
    renderBackBtn.id = 'RndBack';

    // Text tabs on a rule, the workspace's grey accent underlining the active one -- the grammar
    // the Config panel and the Camera Editor use.
    var renderTabs = [];
    var renderTabRow = mk('Panel', renderSettingsCol);
    renderTabRow.style.flowChildren = 'right'; renderTabRow.style.width = '100%';
    renderTabRow.style.paddingLeft = UI.col.pad + 'px'; renderTabRow.style.paddingRight = UI.col.pad + 'px';
    renderTabRow.style.marginTop = '18px';
    renderTabRow.style.borderBottom = '1px solid #ffffff12';
    for (var ti = 0; ti < RENDER_VIEWS.length; ti++) {
      (function (idx) {
        var tb = tabify(btn(renderTabRow, RENDER_VIEWS[idx][0], function () {
          cmd('mirv_filmmaker render tab ' + RENDER_VIEWS[idx][1]);
        }, S.value, { tip: 'Open the ' + RENDER_VIEWS[idx][0].toLowerCase() + ' render settings.' }));
        tb.style.width = 'fit-children';
        tb.style.paddingLeft = '0px'; tb.style.paddingRight = '0px';
        tb.style.paddingTop = '2px'; tb.style.paddingBottom = UI.tabTop.padBottom + 'px';
        tb.style.marginRight = (idx === RENDER_VIEWS.length - 1) ? '0px' : '24px';
        tb.__lbl.style.fontSize = UI.tabTop.font + 'px';
        tb.__lbl.style.letterSpacing = UI.tabTop.ls + 'px';
        renderTabs.push(tb);
        mvmTourAdd(['render.tab.range', 'render.tab.passes', 'render.tab.output'][idx], tb);
      })(ti);
    }

    var renderBody = mk('Panel', renderSettingsCol);
    renderBody.style.flowChildren = 'down'; renderBody.style.width = '100%';
    renderBody.style.height = 'fill-parent-flow(1.0)';
    renderBody.style.paddingTop = '18px'; renderBody.style.paddingBottom = '4px';
    renderBody.style.paddingLeft = UI.col.pad + 'px'; renderBody.style.paddingRight = UI.col.pad + 'px';
    renderBody.style.overflow = 'squish scroll';

    // Pinned footer, a SIBLING of the scroll viewport: a "push to the bottom" spacer inside an
    // overflow:scroll body put the playhead actions past its edge. Shown on every tab, and since the
    // body above fills the remaining height, it sits at the same spot whichever tab is open.
    var renderFooter = mk('Panel', renderSettingsCol);
    renderFooter.style.flowChildren = 'right'; renderFooter.style.width = '100%';
    renderFooter.style.height = 'fit-children';
    renderFooter.style.paddingLeft = UI.col.pad + 'px'; renderFooter.style.paddingRight = UI.col.pad + 'px';
    renderFooter.style.paddingTop = '10px'; renderFooter.style.paddingBottom = '14px';
    renderFooter.style.borderTop = '1px solid #ffffff12';

    // ---- builders ----------------------------------------------------------------------------
    function rsNum(v) { return String(Math.round(v)).replace(/\B(?=(\d{3})+(?!\d))/g, ','); }
    function rsClock(ticks) {
      var s = Math.max(0, Math.round(ticks / DEMO_TICKRATE));
      var m = Math.floor(s / 60), r = s % 60;
      return m + ':' + (r < 10 ? '0' : '') + r;
    }
    // A section: a 3x14 accent bar and a 12px letterspaced title. Anything added to __head after
    // the title sits on the same line (an aside, a readout, the View button).
    function rsSection(parent, title) {
      var s = mk('Panel', parent); s.hittest = false; s.hittestchildren = true;
      s.style.flowChildren = 'down'; s.style.width = '100%'; s.style.marginBottom = '22px';
      var h = mk('Panel', s); h.hittest = false; h.hittestchildren = true;
      h.style.flowChildren = 'right'; h.style.width = '100%'; h.style.height = '28px';
      var bar = mk('Panel', h); bar.hittest = false;
      bar.style.width = '3px'; bar.style.height = '14px'; bar.style.borderRadius = '1px';
      bar.style.backgroundColor = RS_AC; bar.style.verticalAlign = 'center'; bar.style.marginRight = '9px';
      var t = lbl(h, title, S.value, 12); t.hittest = false;
      t.style.fontWeight = 'bold'; t.style.letterSpacing = '1.6px'; t.style.verticalAlign = 'center';
      t.style.whiteSpace = 'nowrap';
      s.__head = h; s.__title = t;
      return s;
    }
    function rsFill(parent) {
      var f = mk('Panel', parent); f.hittest = false;
      f.style.width = 'fill-parent-flow(1.0)'; f.style.height = '1px';
      return f;
    }
    function rsNote(parent, text, top) {
      var l = lbl(parent, text, RS_MUTED, 12); l.hittest = false;
      l.style.whiteSpace = 'normal'; l.style.width = '100%';
      l.style.marginTop = (top == null ? 8 : top) + 'px';
      l.style.paddingLeft = '2px'; l.style.paddingRight = '2px';
      return l;
    }
    function rsCaption(parent, text) {
      var l = lbl(parent, text, RS_MUTED, 11); l.hittest = false;
      l.style.fontWeight = 'bold'; l.style.letterSpacing = '1.5px'; l.style.marginBottom = '6px';
      return l;
    }
    // A label column then a control filling the rest: the Output rows and the path-extension row.
    function rsLabeledRow(parent, text, labelW, top) {
      var r = mk('Panel', parent); r.hittest = false; r.hittestchildren = true;
      r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginTop = (top == null ? 8 : top) + 'px';
      var l = lbl(r, text, S.label, 13); l.hittest = false;
      l.style.width = labelW + 'px'; l.style.verticalAlign = 'center'; l.style.whiteSpace = 'normal';
      l.style.marginRight = '12px';
      r.__lbl = l;
      return r;
    }
    // The orange line a blocked choice answers with. It stays up until the state it complains
    // about changes (the per-frame code clears it), so a refusal is not gone before it is read.
    function rsBlockBox(parent) {
      var b = mk('Panel', parent); b.hittest = false;
      b.style.width = '100%'; b.style.marginTop = '8px'; b.style.borderRadius = '3px';
      b.style.paddingTop = '8px'; b.style.paddingBottom = '8px';
      b.style.paddingLeft = '12px'; b.style.paddingRight = '12px';
      b.style.backgroundColor = RS_BLOCK_BG; b.style.border = '1px solid ' + RS_BLOCK_BORDER;
      var l = lbl(b, '', RS_BLOCK_TEXT, 12); l.hittest = false;
      l.style.fontWeight = 'bold'; l.style.whiteSpace = 'normal'; l.style.width = '100%';
      b.visible = false; b.__err = false; b.__lbl = l;
      return b;
    }
    function rsBlockSay(box, text) { box.__lbl.text = text; box.__err = true; box.visible = true; }
    function rsBlockClear(box) { box.__err = false; box.visible = false; }
)EDJS"
R"EDJS(
    // A pick-one strip: ONE outline, a cell shows its selection as a tinted ground. items are
    // [key, caption, sub?, tip?]; a sub line is the small mono caption under the name (CRF 14).
    // flag(i, 'dim'|'blocked', on) marks a cell unavailable (35% / orange) without hiding it.
    function rsSeg(parent, items, onPick, opts) {
      opts = opts || {};
      var g = mk('Panel', parent); g.hittest = false; g.hittestchildren = true;
      g.style.flowChildren = 'right'; g.style.width = opts.width || '100%';
      g.style.border = '1px solid #ffffff1a'; g.style.borderRadius = '3px';
      g.style.verticalAlign = 'center';
      if (opts.id) g.id = opts.id;
      var cells = [];
      for (var i = 0; i < items.length; i++) (function (it, idx) {
        var c = mk('Panel', g);
        c.style.width = 'fill-parent-flow(1.0)'; c.style.flowChildren = 'down';
        c.style.paddingTop = (opts.padV || 8) + 'px'; c.style.paddingBottom = (opts.padV || 8) + 'px';
        c.style.paddingLeft = '2px'; c.style.paddingRight = '2px';
        var l = lbl(c, it[1], S.label, 12); l.hittest = false;
        l.style.fontWeight = 'bold'; l.style.horizontalAlign = 'center'; l.style.whiteSpace = 'nowrap';
        var sub = null;
        if (it[2]) {
          sub = lbl(c, it[2], RS_MUTED, 10); sub.hittest = false;
          sub.style.fontFamily = S.fontMonodigit; sub.style.horizontalAlign = 'center';
          sub.style.marginTop = '1px'; sub.style.whiteSpace = 'nowrap';
        }
        c.__key = it[0]; c.__sel = false; c.__dim = false; c.__blocked = false;
        c.__paint = function (p) {
          p.style.backgroundColor = p.__sel ? (RS_AC + '33') : ((p.__hot || p.__down) ? '#ffffff0d' : S.clear);
          l.style.color = p.__blocked ? RS_BLOCK_TEXT : ((p.__sel || p.__hot) ? S.value : S.label);
          p.style.opacity = p.__dim ? '0.35' : '1';
        };
        if (it[3]) c.__tipText = it[3];
        edHoverable(c, c.__paint);
        c.SetPanelEvent('onactivate', function () { edPressFlash(c, c.__paint); onPick(it[0], idx); });
        cells.push(c);
      })(items[i], i);
      return {
        panel: g, cells: cells,
        sync: function (key) {
          for (var j = 0; j < cells.length; j++) {
            var on = cells[j].__key === key;
            if (cells[j].__sel !== on) { cells[j].__sel = on; cells[j].__paint(cells[j]); }
          }
        },
        flag: function (idx, name, v) {
          var c = cells[idx]; v = !!v;
          if (c['__' + name] === v) return;
          c['__' + name] = v; c.__paint(c);
        },
        tip: function (idx, text) { cells[idx].__tipText = text; }
      };
    }
    // A radio TILE: square radio, a bold name and a line saying what it resolves to. Used where
    // every alternative deserves to be visible at once (camera source, game HUD). __set(on, sub,
    // blocked): a blocked tile stays clickable -- the click is how the reason gets said.
    function rsRadioTile(parent, title, onPick, tip, last) {
      var t = mk('Panel', parent);
      // ONE fixed height for every tile, sized for a two-line sub: tiles grew to their own text
      // before, so a row of three came out three different heights.
      t.style.width = 'fill-parent-flow(1.0)'; t.style.height = '68px';
      t.style.flowChildren = 'right'; t.style.borderRadius = '3px';
      t.style.paddingTop = '9px'; t.style.paddingBottom = '9px';
      t.style.paddingLeft = '10px'; t.style.paddingRight = '10px';
      t.style.marginRight = last ? '0px' : '6px';
      var box = mk('Panel', t); box.hittest = false;
      box.style.width = '14px'; box.style.height = '14px'; box.style.borderRadius = '2px';
      box.style.marginTop = '1px'; box.style.marginRight = '9px';
      var dot = mk('Panel', box); dot.hittest = false;
      dot.style.width = '6px'; dot.style.height = '6px'; dot.style.borderRadius = '1px';
      dot.style.horizontalAlign = 'center'; dot.style.verticalAlign = 'center';
      var col = mk('Panel', t); col.hittest = false;
      col.style.flowChildren = 'down'; col.style.width = 'fill-parent-flow(1.0)';
      var tl = lbl(col, title, S.label, 13); tl.hittest = false;
      tl.style.fontWeight = 'bold'; tl.style.whiteSpace = 'nowrap'; tl.style.textOverflow = 'ellipsis';
      var sub = lbl(col, '', RS_MUTED, 11); sub.hittest = false;
      sub.style.whiteSpace = 'normal'; sub.style.marginTop = '3px'; sub.style.width = '100%';
      t.__on = false; t.__blocked = false;
      function paint(p) {
        var on = p.__on;
        p.style.backgroundColor = on ? (RS_AC + '1f') : ((p.__hot || p.__down) ? RS_TILE_HOVER : RS_TILE);
        p.style.border = '1px solid ' + (on ? (RS_AC + '80') : S.clear);
        box.style.border = '1px solid ' + (on ? RS_AC : '#ffffff40');
        dot.style.backgroundColor = on ? RS_AC : S.clear;
        tl.style.color = on ? S.value : S.label;
        sub.style.color = p.__blocked ? RS_BLOCK_TEXT : RS_MUTED;
      }
      if (tip) t.__tipText = tip;
      edHoverable(t, paint);
      t.SetPanelEvent('onactivate', function () { edPressFlash(t, paint); if (onPick) onPick(); });
      t.__set = function (on, subText, blocked) {
        on = !!on; blocked = !!blocked;
        if (subText != null && sub.text !== subText) sub.text = subText;
        sub.visible = !!(sub.text && sub.text.length);
        if (t.__on === on && t.__blocked === blocked) return;
        t.__on = on; t.__blocked = blocked; paint(t);
      };
      return t;
    }
    function rsTileRow(parent, top) {
      var r = mk('Panel', parent); r.hittest = false; r.hittestchildren = true;
      r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginTop = (top == null ? 6 : top) + 'px';
      return r;
    }
    // An outlined action in the workspace accent (Set In, Choose folder…). `quiet` is the grey
    // secondary (Clear).
    function rsOutlineBtn(parent, label, onClick, tip, quiet) {
      var b = btn(parent, label, onClick, quiet ? S.label : S.value, { tip: tip });
      b.__bgBase = quiet ? '#ffffff0d' : (RS_AC + '26'); b.__brBase = quiet ? '#ffffff1f' : (RS_AC + '80');
      b.__bgHover = quiet ? '#ffffff1f' : (RS_AC + '44'); b.__fgHover = S.value; b.__accent = quiet ? '#ffffff33' : (RS_AC + 'aa');
      b.style.marginRight = '0px'; b.style.borderRadius = '3px';
      b.style.paddingTop = '0px'; b.style.paddingBottom = '0px';
      b.style.paddingLeft = '14px'; b.style.paddingRight = '14px';
      b.__lbl.style.fontSize = '13px'; b.__lbl.style.horizontalAlign = 'center';
      b.__lbl.style.verticalAlign = 'center'; b.__lbl.style.whiteSpace = 'nowrap';
      btnVisual(b);
      return b;
    }
)EDJS"
R"EDJS(
    // ================= RANGE & CAMERA (tab slots 0 and 1) =================
    var shotPanel = mk('Panel', renderBody);
    shotPanel.style.flowChildren = 'down'; shotPanel.style.width = '100%'; shotPanel.style.height = 'fit-children';

    var RANGE_MODE_TIPS = [
      'Render the entire demo from its first to last tick.',
      'Render only the round containing the playhead.',
      'Render from the first camera key through the last camera key.',
      'Render from the selected camera key to the next key.',
      'Render between the In and Out flags on the timeline.'
    ];
    var RANGE_BLOCK_REASONS = [null, null,
      'Camera Path needs 2+ camera keys. Place them with K.',
      'Segment needs a camera key selected that has a NEXT key.', null];
    var rangeSec = rsSection(shotPanel, 'RANGE');
    var rangeItems = [];
    for (var rmi = 0; rmi < RANGE_MODE_LABELS.length; rmi++) rangeItems.push([rmi, RANGE_MODE_LABELS[rmi], null, RANGE_MODE_TIPS[rmi]]);
    var rangeSeg = rsSeg(rangeSec, rangeItems, function (idx) {
      var av = (st && st.render && st.render.avail) || {};
      if (idx === 2 && (av.pathKeys || 0) < 2) { rsBlockSay(rangeErr, RANGE_BLOCK_REASONS[2]); return; }
      if (idx === 3 && ((av.pathSel == null || av.pathSel < 0) || (av.pathSel + 1 >= (av.pathKeys || 0)))) { rsBlockSay(rangeErr, RANGE_BLOCK_REASONS[3]); return; }
      cmd('mirv_filmmaker render range ' + RANGE_MODE_TOKENS[idx]);
    }, { padV: 9, id: 'RsRangeSeg' });
    var rangeErr = rsBlockBox(rangeSec);
    mvmTourAdd('render.range', rangeSeg.panel); mvmTourAdd('render.range.path', rangeSeg.cells[2]);

    // The resolved span: what this mode actually renders right now.
    var rangeSpan = mk('Panel', rangeSec); rangeSpan.hittest = false;
    rangeSpan.style.flowChildren = 'down'; rangeSpan.style.width = '100%'; rangeSpan.style.marginTop = '8px';
    rangeSpan.style.paddingTop = '14px'; rangeSpan.style.paddingBottom = '14px';
    rangeSpan.style.paddingLeft = '14px'; rangeSpan.style.paddingRight = '14px';
    rangeSpan.style.borderRadius = '3px'; rangeSpan.style.backgroundColor = RS_TILE;
    rangeSpan.style.border = '1px solid #ffffff14';
    var spanTop = mk('Panel', rangeSpan); spanTop.hittest = false;
    spanTop.style.flowChildren = 'right'; spanTop.style.width = '100%';
    function spanCell(cap, w, align, left) {
      var c = mk('Panel', spanTop); c.hittest = false;
      c.style.flowChildren = 'down'; c.style.width = w; c.style.verticalAlign = 'bottom';
      if (left) c.style.marginLeft = left + 'px';
      var k = lbl(c, cap, RS_DIM, 10); k.hittest = false;
      k.style.fontWeight = 'bold'; k.style.letterSpacing = '1.8px'; k.style.horizontalAlign = align;
      k.style.whiteSpace = 'nowrap';
      var v = lbl(c, '', S.value, 24); v.hittest = false;
      v.style.fontFamily = S.fontMonodigit; v.style.fontWeight = 'bold';
      v.style.marginTop = '3px'; v.style.horizontalAlign = align; v.style.whiteSpace = 'nowrap';
      return v;
    }
    var spanIn = spanCell('IN', 'fill-parent-flow(1.0)', 'left');
    var spanArrow = lbl(spanTop, '→', RS_DIM, 18); spanArrow.hittest = false;
    spanArrow.style.verticalAlign = 'bottom'; spanArrow.style.marginLeft = '14px';
    spanArrow.style.marginRight = '14px'; spanArrow.style.marginBottom = '4px';
    var spanLast = spanCell('LAST FRAME', 'fill-parent-flow(1.0)', 'center');
    var spanRule = mk('Panel', spanTop); spanRule.hittest = false;
    spanRule.style.width = '1px'; spanRule.style.height = '48px'; spanRule.style.backgroundColor = '#ffffff14';
    spanRule.style.marginLeft = '14px'; spanRule.style.verticalAlign = 'bottom';
    var spanLen = spanCell('LENGTH', '86px', 'right', 14);
    var spanTicks = spanCell('TICKS @ ' + DEMO_TICKRATE, '96px', 'right', 14);
    // One-tick steps under IN and LAST FRAME. A flag on the timeline covers about 45 ticks per pixel
    // on a 20-minute demo, so the last few ticks are set here (tester report). Each step sends the
    // whole span, so it works from whatever range mode resolved it, and switches to Custom. A step
    // never leaves the demo (demoStart to demoEnd, the last tick it plays): a last frame past the end
    // would have the job wait on frames the demo's end hold never plays.
    var rsSpanStart = 0, rsSpanEnd = 0, rsDemoStart = -1, rsDemoEnd = -1;
    function rsSetSpan(a, b) {
      if (rsDemoStart >= 0 && rsDemoEnd > rsDemoStart) {
        a = Math.max(a, rsDemoStart);
        b = Math.min(b, rsDemoEnd + 1);
      } else {
        a = Math.max(a, 0);
      }
      if (!(b > a) || (a === rsSpanStart && b === rsSpanEnd)) return;
      cmd('mirv_filmmaker render range custom ' + a + ' ' + b);
    }
    function spanNudges(valueLbl, align, what, step) {
      var row = mk('Panel', valueLbl.GetParent()); row.hittest = false; row.hittestchildren = true;
      row.style.flowChildren = 'right'; row.style.marginTop = '6px'; row.style.horizontalAlign = align;
      function nudge(text, d, last) {
        var b = rsOutlineBtn(row, text, function () { step(d); },
          'Move ' + what + ' one tick ' + (d < 0 ? 'earlier' : 'later') + '.', true);
        b.style.height = '22px'; b.style.paddingLeft = '8px'; b.style.paddingRight = '8px';
        b.style.marginRight = last ? '0px' : '4px';
        b.__lbl.style.fontSize = '12px';
        return b;
      }
      // ASCII minus: the HUD font is not guaranteed to draw U+2212.
      nudge('-1', -1, false); nudge('+1', 1, true);
      return row;
    }
    spanNudges(spanIn, 'left', 'In', function (d) { rsSetSpan(rsSpanStart + d, rsSpanEnd); });
    spanNudges(spanLast, 'center', 'the last frame', function (d) { rsSetSpan(rsSpanStart, rsSpanEnd + d); });
    // The span on the whole demo, with the playhead. Read-only: the span is moved from the
    // playhead bar at the foot of the column, or the I / O flags on the timeline.
    var spanBar = mk('Panel', rangeSpan); spanBar.hittest = false;
    spanBar.style.width = '100%'; spanBar.style.height = '18px'; spanBar.style.marginTop = '10px';
    var spanTrack = mk('Panel', spanBar); spanTrack.hittest = false;
    spanTrack.style.width = '100%'; spanTrack.style.height = '10px'; spanTrack.style.borderRadius = '2px';
    spanTrack.style.verticalAlign = 'center'; spanTrack.style.backgroundColor = '#ffffff0f';
    var spanFill = mk('Panel', spanBar); spanFill.hittest = false;
    spanFill.style.height = '10px'; spanFill.style.width = '3px'; spanFill.style.borderRadius = '2px';
    spanFill.style.verticalAlign = 'center'; spanFill.style.backgroundColor = RS_AC;
    spanFill.style.position = '0px 0px 0px';
    var spanHead = mk('Panel', spanBar); spanHead.hittest = false;
    spanHead.style.width = '2px'; spanHead.style.height = '18px'; spanHead.style.backgroundColor = RS_GOLD;
    spanHead.style.position = '0px 0px 0px';
    var spanEnds = mk('Panel', rangeSpan); spanEnds.hittest = false;
    spanEnds.style.flowChildren = 'right'; spanEnds.style.width = '100%'; spanEnds.style.marginTop = '1px';
    var spanZero = lbl(spanEnds, '0:00', RS_DIM, 11); spanZero.hittest = false; spanZero.style.fontFamily = S.fontMonodigit;
    rsFill(spanEnds);
    var spanEnd = lbl(spanEnds, '', RS_DIM, 11); spanEnd.hittest = false; spanEnd.style.fontFamily = S.fontMonodigit;
    function rsPlaceSpan(lo, hi, a, b, ph) {
      var sx = spanBar.actualuiscale_x || 1;
      var w = (spanBar.actuallayoutwidth || 0) / sx;
      if (!(w > 0) || w > 1e30) return;
      var span = (hi > lo) ? (hi - lo) : 1;
      function x(t) { return Math.max(0, Math.min(w, (t - lo) / span * w)); }
      var fx = x(a), fw = Math.max(3, x(b) - fx);
      spanFill.style.position = fx.toFixed(1) + 'px 0px 0px';
      spanFill.style.width = fw.toFixed(1) + 'px';
      spanHead.style.position = Math.max(0, x(ph) - 1).toFixed(1) + 'px 0px 0px';
    }
    var rangeEmpty = mk('Panel', rangeSec); rangeEmpty.hittest = false;
    rangeEmpty.style.width = '100%'; rangeEmpty.style.marginTop = '8px'; rangeEmpty.style.borderRadius = '3px';
    rangeEmpty.style.paddingTop = '14px'; rangeEmpty.style.paddingBottom = '14px';
    rangeEmpty.style.paddingLeft = '14px'; rangeEmpty.style.paddingRight = '14px';
    rangeEmpty.style.backgroundColor = RS_TILE; rangeEmpty.style.border = '1px solid #fc999466';
    var rangeEmptyL = lbl(rangeEmpty, '', RENDER_ERROR_TEXT, 13); rangeEmptyL.hittest = false;
    rangeEmptyL.style.whiteSpace = 'normal'; rangeEmptyL.style.width = '100%';
    var rangeTip = rsNote(rangeSec, '');
)EDJS"
R"EDJS(
    // Every source states its own precondition. `avail` is published by C++ each frame
    // (Render_BuildStateJson) so the check is against live engine state, not a guess.
    //
    // `spectating` is sampled from the live pawn index and legitimately drops for a frame or two
    // whenever the spectated pawn is being rebuilt (round transitions, player switches, the moment
    // a demo seek lands). Reading it raw made POV / Third-Person strobe "unavailable" while the
    // user was plainly watching a player. So it is latched: available instantly, unavailable only
    // after a full second of consecutive misses, and permanently reachable once this demo has ever
    // had a spectated pawn -- picking the source is itself what makes the game switch to it.
    //
    // camSpecTick advances the latch EXACTLY once per render pass; camSpectatingStable() only
    // reads it, because blockedReason runs once per tile (six times a frame).
    var camSpecLatch = false, camSpecMissFrames = 0, camSpecEverSeen = false;
    function camSpecTick(raw) {
      if (raw) { camSpecLatch = true; camSpecEverSeen = true; camSpecMissFrames = 0; }
      else if (camSpecLatch && ++camSpecMissFrames > 64) { camSpecLatch = false; }
    }
    function camSpectatingStable() { return camSpecLatch || camSpecEverSeen; }
    function camSourceBlockedReason(idx) {
      var av = (st && st.render && st.render.avail) || {};
      var spec = camSpectatingStable();
      if (idx === 1 && !spec) return 'Needs a spectated player — click one in the preview.';
      if (idx === 2 && !spec) return 'Needs a spectated player — click one in the preview.';
      if (idx === 4 && (av.pathKeys || 0) < 2) return 'Needs 2+ camera keys. Place them with K.';
      if (idx === 5 && !av.followReady) return 'No follow camera placed yet — set one up in the editor’s Follow tab.';
      return null;
    }
    // The tile's own line: what the source resolves to, or (blocked) the short form of why not.
    var CAM_SUBS = ['What’s on screen now', 'In-eye on the spectated player', 'Orbit on the spectated player',
                    'Free fly (WASD + mouse)', 'Plays the path', 'Tracks the follow target'];
    var CAM_BLOCK_SUBS = [null, 'Needs a spectated player', 'Needs a spectated player', null,
                          'Needs 2+ camera keys', 'Place a follow camera first'];

    var camSec = rsSection(shotPanel, 'CAMERA');
    var camTiles = [];
    var camGrid = mk('Panel', camSec); camGrid.id = 'RsCamGrid'; camGrid.hittest = false; camGrid.hittestchildren = true;
    camGrid.style.flowChildren = 'down'; camGrid.style.width = '100%';
    for (var csr = 0; csr < 2; csr++) {
      var camRow = rsTileRow(camGrid, csr === 0 ? 0 : 6);
      for (var csc = 0; csc < 3; csc++) (function (idx) {
        camTiles.push(rsRadioTile(camRow, CAM_SOURCES[idx][1], function () {
          var why = camSourceBlockedReason(idx);
          if (why) { rsBlockSay(camErr, why); return; }
          cmd('mirv_filmmaker render camera ' + CAM_SOURCES[idx][0]);
        }, CAM_SOURCE_HINTS[idx], csc === 2));
      })(csr * 3 + csc);
    }
    var camErr = rsBlockBox(camSec);
    mvmTourAdd('render.cams', camGrid); mvmTourAdd('render.cam.path', camTiles[4]);
    // Camera Path's own setting, shown while it is the source.
    var EXT_ROWS = [['fail', 'Fail (safe)'], ['holdfirst', 'Hold first'], ['holdlast', 'Hold last'], ['clamp', 'Clamp range']];
    var extRow = rsLabeledRow(camSec, 'If the path is shorter than the range', 200, 12);
    var extSeg = rsSeg(extRow, EXT_ROWS, function (token) { cmd('mirv_filmmaker render extension ' + token); },
      { padV: 7, width: 'fill-parent-flow(1.0)', id: 'RsExtSeg' });
    var extNote = rsNote(camSec, 'Default is Fail. Silent clamping renders the wrong clip without telling you.');

    // The pinned footer: the playhead and the three actions that act on it.
    var phCol = mk('Panel', renderFooter); phCol.hittest = false;
    phCol.style.flowChildren = 'down'; phCol.style.width = '150px'; phCol.style.verticalAlign = 'center';
    var phCap = lbl(phCol, 'PLAYHEAD', RS_DIM, 10); phCap.hittest = false;
    phCap.style.fontWeight = 'bold'; phCap.style.letterSpacing = '1.8px';
    var phVal = lbl(phCol, '', RS_GOLD, 15); phVal.hittest = false;
    phVal.style.fontFamily = S.fontMonodigit; phVal.style.fontWeight = 'bold'; phVal.style.marginTop = '2px';
    function footBtn(label, onClick, tip, quiet, last) {
      var b = rsOutlineBtn(renderFooter, label, onClick, tip, quiet);
      b.style.width = 'fill-parent-flow(1.0)'; b.style.height = '36px';
      b.style.marginLeft = '6px';
      return b;
    }
    var setInBtn = footBtn('Set In', function () {
      if (st && typeof st.tick === 'number') cmd('mirv_filmmaker render in ' + (st.tick | 0));
    }, 'Set the render In flag at the current playhead tick.');
    var setOutBtn = footBtn('Set Out', function () {
      if (st && typeof st.tick === 'number') cmd('mirv_filmmaker render out ' + ((st.tick | 0) + 1));
    }, 'Set the render Out flag just after the current playhead tick.');
    var clearRangeBtn = footBtn('Clear', function () {
      cmd('mirv_filmmaker render clearrange');
    }, 'Clear the custom In and Out range.', true);
)EDJS"
R"EDJS(
    // ================= PASSES (tab slot 2) =================
    // What the job records: one output per pass, a pass being an output TYPE (beauty, depth,
    // normal) of a VISIBILITY group. Every pass of a frame is rendered from the same moment of the
    // demo, so the outputs line up frame for frame.
    var passesPanel = mk('Panel', renderBody);
    passesPanel.style.flowChildren = 'down'; passesPanel.style.width = '100%'; passesPanel.style.height = 'fit-children';
    passesPanel.visible = false;
    function rpPass() { return (st && st.render && st.render.pass) ? st.render.pass : null; }

    var RP_TYPES = [['beauty', 'Beauty'], ['depth', 'Depth'], ['normal', 'Normal'], ['key', 'Key']];
    var RP_ELEMENTS = [
      ['all', 'Everything', 'The scene as the game draws it.'],
      ['worldplayers', 'World + players', 'Everything except the first-person gun and its effects.'],
      ['world', 'World only', 'The map: no players, gear, weapons or viewmodel.'],
      ['players', 'Players only', 'Players and their gear over the background colour.'],
      ['gun', 'Gun only', 'The first-person gun and its effects over the background colour.']
    ];
    // Mirrors PassAllowed (RenderPassPlan.h): the isolated groups (Players only, Gun only) have a
    // background to key out, so their colour plate is Key; the groups that fill the frame take
    // Beauty. Depth and Normal exist for every group.
    function rpAllowed(type, el) {
      var isolated = el === 'players' || el === 'gun';
      return type === 'key' ? isolated : (type === 'beauty' ? !isolated : true);
    }
    function rpLabelOf(rows, key) {
      for (var i = 0; i < rows.length; i++) if (rows[i][0] === key) return rows[i][1];
      return key;
    }
    // The pass the camera view shows live (`render pass preview <type> <elements>`), or null for
    // the normal scene (Beauty + Everything, `render pass preview off`).
    function rpPreviewOf(q) {
      var pv = q && q.preview;
      if (!pv || typeof pv !== 'object' || (pv.type === 'beauty' && pv.elements === 'all')) return null;
      return pv;
    }
    // The first enabled pass, in grid order: every pass has a live form (Gun only shows the gun
    // itself live; the job adds its muzzle flash from a second render).
    function rpFirstViewable(q) {
      var list = (q && q.list) || [];
      for (var e = 0; e < RP_ELEMENTS.length; e++) {
        for (var t = 0; t < RP_TYPES.length; t++)
          for (var i = 0; i < list.length; i++)
            if (list[i].type === RP_TYPES[t][0] && list[i].elements === RP_ELEMENTS[e][0]) return list[i];
      }
      return null;
    }
    var rpSec = rsSection(passesPanel, 'PASSES');
    rsFill(rpSec.__head);
    // View: shows one of the ENABLED passes live in the camera view -- its type as well as its
    // visibility group, so Normal of World only is the normals of the map. Turning it off is
    // `render pass preview off` -- the camera view back to the full scene.
    var rpViewOn = false;
    var rpViewBtn = mk('Panel', rpSec.__head);
    rpViewBtn.style.flowChildren = 'right'; rpViewBtn.style.height = '28px'; rpViewBtn.style.verticalAlign = 'center';
    rpViewBtn.style.paddingLeft = '11px'; rpViewBtn.style.paddingRight = '11px'; rpViewBtn.style.borderRadius = '3px';
    var rpViewEye = lbl(rpViewBtn, '◉', S.label, 12); rpViewEye.hittest = false;
    rpViewEye.style.verticalAlign = 'center'; rpViewEye.style.marginRight = '7px';
    var rpViewL = lbl(rpViewBtn, 'View', S.label, 12); rpViewL.hittest = false;
    rpViewL.style.fontWeight = 'bold'; rpViewL.style.verticalAlign = 'center'; rpViewL.style.whiteSpace = 'nowrap';
    function rpPaintView(p) {
      var on = rpViewOn, c = on ? RS_GOLD : (p.__hot ? S.value : S.label);
      p.style.backgroundColor = on ? (RS_GOLD + '26') : (p.__hot ? '#ffffff1f' : '#ffffff0d');
      p.style.border = '1px solid ' + (on ? (RS_GOLD + '99') : '#ffffff1f');
      rpViewEye.style.color = c; rpViewL.style.color = c;
      rpViewL.text = on ? 'Viewing live' : 'View';
    }
    rpViewBtn.__tipText = 'Show a pass live in the camera view.';
    edHoverable(rpViewBtn, rpPaintView);
    rpViewBtn.SetPanelEvent('onactivate', function () {
      edPressFlash(rpViewBtn, rpPaintView);
      var q = rpPass();
      if (rpViewOn) { rpViewOn = false; if (rpPreviewOf(q)) cmd('mirv_filmmaker render pass preview off'); }
      else {
        rpViewOn = true;
        // Straight to the first pass that can be shown, so View always shows something.
        var first = rpFirstViewable(q);
        if (first && !rpPreviewOf(q)) cmd('mirv_filmmaker render pass preview ' + first.type + ' ' + first.elements);
      }
      rpPaintView(rpViewBtn);
    });

    var rpViewStrip = mk('Panel', rpSec); rpViewStrip.hittest = false; rpViewStrip.hittestchildren = true;
    rpViewStrip.style.flowChildren = 'down'; rpViewStrip.style.width = '100%'; rpViewStrip.style.marginTop = '6px';
    rpViewStrip.style.paddingTop = '8px'; rpViewStrip.style.paddingBottom = '4px';
    rpViewStrip.style.paddingLeft = '10px'; rpViewStrip.style.paddingRight = '6px';
    rpViewStrip.style.borderRadius = '3px'; rpViewStrip.style.backgroundColor = RS_GOLD + '12';
    rpViewStrip.style.border = '1px solid ' + RS_GOLD + '40';
    rpViewStrip.visible = false;
    var rpViewCap = lbl(rpViewStrip, 'LIVE IN CAMERA', RS_GOLD, 10); rpViewCap.hittest = false;
    rpViewCap.style.fontWeight = 'bold'; rpViewCap.style.letterSpacing = '1.6px';
    // The caption is a line of its own: inline in the wrapping strip it sank to the bottom of
    // whichever row the chips left it on.
    rpViewCap.style.width = '100%'; rpViewCap.style.marginBottom = '8px';
    // The chips wrap in their own row under the caption (a full-width caption inside a wrapping
    // flow did not take a line of its own in game: it drew underneath the first chip).
    var rpViewRow = mk('Panel', rpViewStrip); rpViewRow.hittest = false; rpViewRow.hittestchildren = true;
    rpViewRow.style.flowChildren = 'right-wrap'; rpViewRow.style.width = '100%';
    // One chip per ENABLED pass ("Normal · World only"), pooled and relabelled by the sync as the
    // grid changes. A chip's key is "<type> <elements>", exactly the preview command's arguments.
    var rpViewChips = [];
    function rpViewChip() {
      var c = mk('Panel', rpViewRow);
      c.style.height = '24px'; c.style.paddingLeft = '9px'; c.style.paddingRight = '9px';
      c.style.marginRight = '6px'; c.style.marginBottom = '4px'; c.style.borderRadius = '3px';
      var l = lbl(c, '', S.label, 12); l.hittest = false;
      l.style.fontWeight = 'bold'; l.style.verticalAlign = 'center'; l.style.whiteSpace = 'nowrap';
      c.__key = ''; c.__on = false; c.__inert = false;
      c.__paint = function (p) {
        p.style.backgroundColor = p.__on ? RS_GOLD : (p.__hot ? '#ffffff12' : S.clear);
        p.style.border = '1px solid ' + (p.__on ? RS_GOLD : (p.__hot ? '#ffffff40' : '#ffffff26'));
        l.style.color = p.__on ? '#0c0e12ff' : S.label;
        p.style.opacity = p.__inert ? '0.4' : '1';
      };
      c.__set = function (key, caption, inert) {
        if (!c.visible) c.visible = true;
        if (c.__key === key) return;
        c.__key = key; c.__inert = inert; l.text = caption;
        c.__tipText = 'Show ' + caption + ' in the camera view.'
          + (key.indexOf(' gun') > 0 ? ' The render also keeps the muzzle flash, which the live view leaves out.' : '');
        c.__paint(c);
      };
      edHoverable(c, c.__paint);
      c.SetPanelEvent('onactivate', function () {
        if (c.__inert || !c.__key) return;
        edPressFlash(c, c.__paint);
        cmd('mirv_filmmaker render pass preview ' + c.__key);
      });
      rpViewChips.push(c);
      return c;
    }
    var rpViewEmpty = lbl(rpViewRow, 'Turn on a pass to view it live.', S.label, 12); rpViewEmpty.hittest = false;
    rpViewEmpty.style.verticalAlign = 'center'; rpViewEmpty.style.marginBottom = '4px'; rpViewEmpty.visible = false;
)EDJS"
R"EDJS(
    // The matrix: a row per visibility group, a checkbox cell per output type.
    var rpHead = mk('Panel', rpSec); rpHead.hittest = false;
    rpHead.style.flowChildren = 'right'; rpHead.style.width = '100%'; rpHead.style.marginTop = '8px';
    rsFill(rpHead);
    for (var rph = 0; rph < RP_TYPES.length; rph++) {
      var rpHl = lbl(rpHead, RP_TYPES[rph][1].toUpperCase(), RS_MUTED, 10); rpHl.hittest = false;
      rpHl.style.width = '64px'; rpHl.style.marginLeft = '6px'; rpHl.style.textAlign = 'center';
      rpHl.style.fontWeight = 'bold'; rpHl.style.letterSpacing = '1px';
    }
    function rpCell(parent, onClick, tip) {
      var c = mk('Panel', parent);
      c.style.width = '64px'; c.style.height = '32px'; c.style.marginLeft = '6px';
      c.style.borderRadius = '3px'; c.style.verticalAlign = 'center';
      var box = mk('Panel', c); box.hittest = false;
      box.style.width = '14px'; box.style.height = '14px'; box.style.borderRadius = '2px';
      box.style.horizontalAlign = 'center'; box.style.verticalAlign = 'center';
      var mark = lbl(box, '✔', '#0c0e12ff', 10); mark.hittest = false;
      mark.style.fontWeight = 'bold'; mark.style.horizontalAlign = 'center'; mark.style.verticalAlign = 'center';
      var eye = lbl(c, '◉', RS_GOLD, 11); eye.hittest = false;
      eye.style.horizontalAlign = 'right'; eye.style.verticalAlign = 'center'; eye.style.marginRight = '6px';
      c.__on = false; c.__live = false;
      function paint(p) {
        p.style.backgroundColor = p.__on ? (RS_AC + '40') : '#ffffff08';
        p.style.border = '1px solid ' + (p.__live ? RS_GOLD : (p.__hot ? '#ffffff40' : (p.__on ? (RS_AC + '99') : '#ffffff1a')));
        box.style.backgroundColor = p.__on ? S.value : S.clear;
        box.style.border = '1px solid ' + (p.__on ? S.value : '#ffffff40');
        mark.visible = p.__on; eye.visible = p.__live;
      }
      c.__tipText = tip;
      edHoverable(c, paint);
      c.SetPanelEvent('onactivate', function () { edPressFlash(c, paint); onClick(); });
      c.__setCell = function (on, live) {
        on = !!on; live = !!live;
        if (c.__on === on && c.__live === live) return;
        c.__on = on; c.__live = live; paint(c);
      };
      return c;
    }
    var rpRows = [], rpCells = [];
    for (var rpe = 0; rpe < RP_ELEMENTS.length; rpe++) (function (ei) {
      var row = mk('Panel', rpSec); row.hittest = false; row.hittestchildren = true;
      row.style.flowChildren = 'right'; row.style.width = '100%'; row.style.minHeight = '42px';
      row.style.marginTop = '6px'; row.style.paddingLeft = '10px'; row.style.borderRadius = '3px';
      row.style.paddingTop = '4px'; row.style.paddingBottom = '4px';
      var txt = mk('Panel', row); txt.hittest = false;
      txt.style.flowChildren = 'down'; txt.style.width = 'fill-parent-flow(1.0)'; txt.style.verticalAlign = 'center';
      var name = lbl(txt, RP_ELEMENTS[ei][1], S.label, 13); name.hittest = false;
      name.style.fontWeight = 'bold'; name.style.whiteSpace = 'nowrap';
      var desc = lbl(txt, RP_ELEMENTS[ei][2], RS_MUTED, 11); desc.hittest = false;
      desc.style.marginTop = '2px'; desc.style.whiteSpace = 'nowrap'; desc.style.textOverflow = 'ellipsis';
      desc.style.width = '100%';
      var cells = [];
      for (var ti = 0; ti < RP_TYPES.length; ti++) (function (tj) {
        if (!rpAllowed(RP_TYPES[tj][0], RP_ELEMENTS[ei][0])) {
          // No such pass (Key of a group with no background): an empty slot keeps the columns.
          var gap = mk('Panel', row); gap.hittest = false;
          gap.style.width = '64px'; gap.style.height = '32px'; gap.style.marginLeft = '6px';
          gap.__setCell = function () {};
          cells.push(gap);
          return;
        }
        cells.push(rpCell(row, function () {
          cmd('mirv_filmmaker render pass toggle ' + RP_TYPES[tj][0] + ' ' + RP_ELEMENTS[ei][0]);
        }, RP_TYPES[tj][1] + ' of ' + RP_ELEMENTS[ei][1].toLowerCase() + ': ' + RP_ELEMENTS[ei][2]));
      })(ti);
      row.__name = name;
      rpRows.push(row); rpCells.push(cells);
    })(rpe);
    var rpSummary = rsNote(rpSec, '', 6);

    // KEYING & DEPTH: each half greys out until a pass that uses it is on.
    var rkSec = rsSection(passesPanel, 'KEYING & DEPTH');
    var rkCols = mk('Panel', rkSec); rkCols.hittest = false; rkCols.hittestchildren = true;
    rkCols.style.flowChildren = 'right'; rkCols.style.width = '100%';
    var rkKeyCol = mk('Panel', rkCols); rkKeyCol.hittest = false; rkKeyCol.hittestchildren = true;
    rkKeyCol.style.flowChildren = 'down'; rkKeyCol.style.width = 'fill-parent-flow(1.0)'; rkKeyCol.style.marginRight = '16px';
    var rkDepthCol = mk('Panel', rkCols); rkDepthCol.hittest = false; rkDepthCol.hittestchildren = true;
    rkDepthCol.style.flowChildren = 'down'; rkDepthCol.style.width = 'fill-parent-flow(1.0)';
    rsCaption(rkKeyCol, 'BACKGROUND COLOUR');
    var RP_KEYS = [['Green', [0, 255, 0]], ['Blue', [0, 0, 255]], ['Magenta', [255, 0, 255]], ['Black', [0, 0, 0]]];
    var rpKeyGroup = mk('Panel', rkKeyCol); rpKeyGroup.hittest = false; rpKeyGroup.hittestchildren = true;
    rpKeyGroup.style.flowChildren = 'down'; rpKeyGroup.style.width = '100%';
    var rpKeyBtns = [], rpKeyRow = null;
    for (var rpk = 0; rpk < RP_KEYS.length; rpk++) (function (ki) {
      if (ki % 2 === 0) rpKeyRow = rsTileRow(rpKeyGroup, ki === 0 ? 0 : 6);
      var rgb = RP_KEYS[ki][1];
      var b = mk('Panel', rpKeyRow);
      b.style.width = 'fill-parent-flow(1.0)'; b.style.height = '32px'; b.style.flowChildren = 'right';
      b.style.paddingLeft = '10px'; b.style.paddingRight = '10px'; b.style.borderRadius = '3px';
      b.style.marginRight = (ki % 2 === 0) ? '6px' : '0px';
      var sw = mk('Panel', b); sw.hittest = false;
      sw.style.width = '14px'; sw.style.height = '14px'; sw.style.borderRadius = '2px'; sw.style.verticalAlign = 'center';
      sw.style.backgroundColor = 'rgb(' + rgb[0] + ',' + rgb[1] + ',' + rgb[2] + ')'; sw.style.border = '1px solid #ffffff26';
      sw.style.marginRight = '8px';
      var l = lbl(b, RP_KEYS[ki][0], S.label, 12); l.hittest = false;
      l.style.fontWeight = 'bold'; l.style.verticalAlign = 'center'; l.style.whiteSpace = 'nowrap';
      b.__on = false;
      b.__paint = function (p) {
        p.style.backgroundColor = p.__on ? (RS_AC + '33') : '#ffffff08';
        p.style.border = '1px solid ' + (p.__hot ? '#ffffff40' : (p.__on ? (RS_AC + '99') : '#ffffff1f'));
        l.style.color = p.__on ? S.value : S.label;
      };
      b.__tipText = 'Fill the background of Players only and Gun only with ' + RP_KEYS[ki][0].toLowerCase() + '.';
      edHoverable(b, b.__paint);
      b.SetPanelEvent('onactivate', function () {
        edPressFlash(b, b.__paint);
        cmd('mirv_filmmaker render pass key ' + rgb[0] + ' ' + rgb[1] + ' ' + rgb[2]);
      });
      rpKeyBtns.push(b);
    })(rpk);
    var rpKeyNote = rsNote(rkKeyCol, '', 6);
    rpKeyNote.style.fontSize = '11px';
)EDJS"
R"EDJS(
    rsCaption(rkDepthCol, 'DEPTH RANGE');
    var rpDepthGroup = mk('Panel', rkDepthCol); rpDepthGroup.hittest = false; rpDepthGroup.hittestchildren = true;
    rpDepthGroup.style.flowChildren = 'down'; rpDepthGroup.style.width = '100%'; rpDepthGroup.style.marginTop = '-5px';
    // One-line value rows (the editor's edValueTile, cursor-driven), tightened to the half column.
    function rpDepthTile(label, min, max, fill, onChange) {
      var v = edValueTile(rpDepthGroup, { label: label, min: min, max: max, decimals: 0, unit: 'u', fill: fill,
        tip: label === 'Near' ? 'Units from the camera that render white.' : 'Units from the camera that render black.',
        onChange: onChange });
      var nl = v.tile.GetChild(0), vl = v.tile.GetChild(2);
      if (nl) nl.style.width = '34px';
      if (vl) { vl.style.width = '54px'; vl.style.fontSize = '13px'; }
      return v;
    }
    var rpNearTile = rpDepthTile('Near', 0, 1024, S.value, function (v) {
      var q = rpPass();
      if (q && isFinite(v) && v >= 0 && v < q.depthFar) cmd('mirv_filmmaker render pass depth ' + v + ' ' + q.depthFar);
    });
    var rpFarTile = rpDepthTile('Far', 64, 16384, '#5a6270ff', function (v) {
      var q = rpPass();
      if (q && isFinite(v) && v > q.depthNear) cmd('mirv_filmmaker render pass depth ' + q.depthNear + ' ' + v);
    });
    var rpDepthNote = rsNote(rkDepthCol, '', 6);
    rpDepthNote.style.fontSize = '11px';

    // GAME HUD, which only applies to Beauty · Everything, and the job-wide extras.
    var HUD_MODES = [['none', 'No HUD'], ['game', 'In-game'], ['full', 'Full HUD']];
    var HUD_SUBS = ['Clean plate — nothing but the world.',
                    'Health, ammo, radar.',
                    'Scoreboard and killfeed included.'];
    var hudSec = rsSection(passesPanel, 'GAME HUD');
    var hudAside = lbl(hudSec.__head, 'Beauty · Everything', RS_MUTED, 11); hudAside.hittest = false;
    hudAside.style.verticalAlign = 'center'; hudAside.style.marginLeft = '9px';
    var hudRow = rsTileRow(hudSec, 0);
    var hudTiles = [];
    for (var hmi = 0; hmi < HUD_MODES.length; hmi++) (function (idx) {
      hudTiles.push(rsRadioTile(hudRow, HUD_MODES[idx][1], function () {
        cmd('mirv_filmmaker render pass hud ' + HUD_MODES[idx][0]);
      }, 'Game HUD in the Beauty · Everything pass: ' + HUD_MODES[idx][1] + '.', idx === HUD_MODES.length - 1));
    })(hmi);
    var hudExtras = mk('Panel', hudSec); hudExtras.hittest = false; hudExtras.hittestchildren = true;
    hudExtras.style.flowChildren = 'down'; hudExtras.style.width = '100%'; hudExtras.style.marginTop = '4px';
    var camDataTog = edSwitchTile(hudExtras, 'Camera metadata sidecar', 'Write camera transform metadata alongside the rendered frames.', function () {
      cmd('mirv_filmmaker render pass cameradata ' + ((st && st.render && st.render.pass && st.render.pass.cameraData) ? 0 : 1));
    });
    var bloodTog = edSwitchTile(hudExtras, 'Blood', 'Off: no blood appears during the render, in any pass.', function () {
      cmd('mirv_filmmaker render pass blood ' + ((st && st.render && st.render.pass && st.render.pass.blood === false) ? 1 : 0));
    });
)EDJS"
R"EDJS(
    // ================= OUTPUT (tab slot 3) =================
    var outputPanel = mk('Panel', renderBody);
    outputPanel.style.flowChildren = 'down'; outputPanel.style.width = '100%'; outputPanel.style.height = 'fit-children';
    outputPanel.visible = false;
    function renderOut() { return (st && st.render && st.render.out) ? st.render.out : null; }

    // DESTINATION: where the take lands and what it is written as.
    var destSec = rsSection(outputPanel, 'DESTINATION');
    var destRow = mk('Panel', destSec); destRow.hittest = false; destRow.hittestchildren = true;
    destRow.style.flowChildren = 'right'; destRow.style.width = '100%';
    var folderBox = mk('Panel', destRow); folderBox.hittest = false;
    folderBox.style.width = 'fill-parent-flow(1.0)'; folderBox.style.height = '34px';
    folderBox.style.paddingLeft = '10px'; folderBox.style.paddingRight = '10px';
    folderBox.style.borderRadius = '3px'; folderBox.style.backgroundColor = S.bg;
    var folderLbl = lbl(folderBox, '', S.value, 12); folderLbl.hittest = false;
    folderLbl.style.verticalAlign = 'center'; folderLbl.style.whiteSpace = 'nowrap';
    folderLbl.style.textOverflow = 'ellipsis'; folderLbl.style.width = '100%';
    var folderBtn = rsOutlineBtn(destRow, 'Choose folder…', function () {
      cmd('mirv_filmmaker render output folder');
    }, 'Choose the folder where the render files will be written.');
    folderBtn.style.height = '34px'; folderBtn.style.marginLeft = '6px';
    folderBtn.__lbl.style.fontSize = '12px';
    mvmTourAdd('render.folder', folderBtn);
    var projectLbl = rsNote(destSec, '', 8);
    var FORMATS = [['mp4', 'MP4', 'VIDEO'], ['avi', 'AVI', 'VIDEO'], ['gif', 'GIF', 'VIDEO'], ['exr', 'EXR', 'SEQ'],
                   ['tga', 'TGA', 'SEQ'], ['bmp', 'BMP', 'SEQ'], ['png', 'PNG', 'SEQ'], ['tiff', 'TIFF', 'SEQ']];
    var fmtRow = rsTileRow(destSec, 10); fmtRow.id = 'RsFormatGrid';
    mvmTourAdd('render.format', fmtRow);
    var fmtCells = [];
    for (var fmi = 0; fmi < FORMATS.length; fmi++) (function (f, last) {
      var c = mk('Panel', fmtRow);
      c.style.width = 'fill-parent-flow(1.0)'; c.style.height = '40px'; c.style.flowChildren = 'down';
      c.style.borderRadius = '3px'; c.style.marginRight = last ? '0px' : '5px';
      var inner = mk('Panel', c); inner.hittest = false;
      inner.style.flowChildren = 'down'; inner.style.horizontalAlign = 'center'; inner.style.verticalAlign = 'center';
      var l = lbl(inner, f[1], S.label, 13); l.hittest = false;
      l.style.fontWeight = 'bold'; l.style.horizontalAlign = 'center';
      var k = lbl(inner, f[2], RS_MUTED, 9); k.hittest = false;
      k.style.letterSpacing = '1px'; k.style.horizontalAlign = 'center'; k.style.marginTop = '1px';
      c.__key = f[0]; c.__on = false;
      c.__paint = function (p) {
        p.style.backgroundColor = p.__on ? (RS_AC + '33') : '#ffffff08';
        p.style.border = '1px solid ' + (p.__hot ? '#ffffff40' : (p.__on ? (RS_AC + '99') : '#ffffff1f'));
        l.style.color = p.__on ? S.value : S.label;
      };
      c.__tipText = (f[2] === 'SEQ' ? 'One ' + f[1] + ' image per frame.' : f[1] + ' video file.');
      edHoverable(c, c.__paint);
      c.SetPanelEvent('onactivate', function () {
        edPressFlash(c, c.__paint);
        cmd('mirv_filmmaker render output format ' + f[0]);
      });
      fmtCells.push(c);
    })(FORMATS[fmi], fmi === FORMATS.length - 1);
    var fmtNote = rsNote(destSec, '');

    // PICTURE: frame rate, under a read-only readout of the frame size. A render is always the
    // game resolution: the scene cannot render larger than the game, and a smaller size would only
    // be an FFmpeg resample, so the size is changed in CS2's own video settings, not here.
    var picSec = rsSection(outputPanel, 'PICTURE');
    rsFill(picSec.__head);
    var picDims = lbl(picSec.__head, '', S.value, 13); picDims.hittest = false;
    picDims.style.fontFamily = S.fontMonodigit; picDims.style.fontWeight = 'bold'; picDims.style.verticalAlign = 'center';
    var picTag = lbl(picSec.__head, 'GAME RESOLUTION', '#35cc59ff', 11); picTag.hittest = false;
    picTag.style.fontWeight = 'bold'; picTag.style.letterSpacing = '1px';
    picTag.style.verticalAlign = 'center'; picTag.style.marginLeft = '9px';
)EDJS"
R"EDJS(
    var fpsCustomOpen = false;
    var fpsRow = rsLabeledRow(picSec, 'Frame rate', 96, 8);
    var fpsItems = [];
    for (var fpi = 0; fpi < FPS_CHOICES.length; fpi++) fpsItems.push(['' + FPS_CHOICES[fpi], '' + FPS_CHOICES[fpi]]);
    fpsItems.push(['custom', 'Custom']);
    var fpsSeg = rsSeg(fpsRow, fpsItems, function (token) {
      if (token === 'custom') { fpsCustomOpen = true; renderFieldFocus(fpsField); return; }
      fpsCustomOpen = false;
      cmd('mirv_filmmaker render output fps ' + token);
    }, { width: 'fill-parent-flow(1.0)', id: 'RsFpsSeg' });
    var fpsFieldRow = mk('Panel', picSec);
    fpsFieldRow.style.flowChildren = 'right'; fpsFieldRow.style.width = '100%';
    fpsFieldRow.style.marginTop = '8px'; fpsFieldRow.style.paddingLeft = '108px';
    var fpsField = renderNumberField(fpsFieldRow, 'type a number, Enter', false,
      function () { var o = renderOut(); return o ? (o.fps + ' fps') : ''; },
      function (text) {
        var v = parseInt(text, 10);
        if (isFinite(v) && v > 0) cmd('mirv_filmmaker render output fps ' + v);
      });
    rsNote(picSec, 'Render sets host_framerate to the frame rate automatically.');

    // ENCODING: drives MP4 only, so the whole block greys out for the other formats.
    var encSec = rsSection(outputPanel, 'ENCODING');
    rsFill(encSec.__head);
    var encAside = lbl(encSec.__head, '', RS_MUTED, 11); encAside.hittest = false;
    encAside.style.fontWeight = 'bold'; encAside.style.letterSpacing = '1px'; encAside.style.verticalAlign = 'center';
    var encBody = mk('Panel', encSec); encBody.hittest = false; encBody.hittestchildren = true;
    encBody.style.flowChildren = 'down'; encBody.style.width = '100%';
    var qualCustomOpen = false;
    var qualRow = rsLabeledRow(encBody, 'Quality', 96, 0);
    var qualItems = [];
    for (var qci = 0; qci < CRF_CHOICES.length; qci++) qualItems.push(['' + CRF_CHOICES[qci][0], CRF_CHOICES[qci][1], 'CRF ' + CRF_CHOICES[qci][0]]);
    qualItems.push(['bitrate', 'Bitrate', 'kbps']);
    var qualSeg = rsSeg(qualRow, qualItems, function (token) {
      if (token === 'bitrate') {
        // Entering bitrate mode is the field appearing; the value itself commits on Enter.
        qualCustomOpen = true;
        if (!((renderOut() || {}).bitrateKbps > 0)) cmd('mirv_filmmaker render output bitrate 8000');
        renderFieldFocus(bitrateField);
        return;
      }
      qualCustomOpen = false;
      cmd('mirv_filmmaker render output bitrate 0');
      cmd('mirv_filmmaker render output quality ' + token);
    }, { padV: 5, width: 'fill-parent-flow(1.0)', id: 'RsQualitySeg' });
    var bitrateRow = mk('Panel', encBody);
    bitrateRow.style.flowChildren = 'right'; bitrateRow.style.width = '100%';
    bitrateRow.style.marginTop = '8px'; bitrateRow.style.paddingLeft = '108px';
    var bitrateField = renderNumberField(bitrateRow, 'kbps', false,
      function () { var o = renderOut(); return (o && o.bitrateKbps > 0) ? (o.bitrateKbps + ' kbps') : ''; },
      function (text) {
        var v = parseInt(text, 10);
        if (isFinite(v) && v >= 0) cmd('mirv_filmmaker render output bitrate ' + v);
      });
    // Encoder and speed. Auto is the default and the right answer for almost everyone: a GPU encode
    // never holds a frame up, while x264 can (measured 2026-10-18). The design puts both
    // on one line; they get a line each because the column is narrower on a 4:3 game window
    // (34% of 1440) and "Balanced" clipped there.
    var encRow = rsLabeledRow(encBody, 'Encoder', 96, 8);
    var encSeg = rsSeg(encRow, [['auto', 'Auto'], ['cpu', 'CPU'], ['gpu', 'GPU']], function (token) {
      if (token === 'gpu' && encSeg.cells[2].__dim) return;   // NVENC unavailable: the note says why
      cmd('mirv_filmmaker render output encoder ' + token);
    }, { width: 'fill-parent-flow(1.0)' });
    var speedRow = rsLabeledRow(encBody, 'Speed', 96, 8);
    var speedSeg = rsSeg(speedRow, [['quality', 'Best'], ['balanced', 'Balanced'], ['fast', 'Fastest']], function (token) {
      cmd('mirv_filmmaker render output speed ' + token);
    }, { width: 'fill-parent-flow(1.0)' });
    var encNote = rsNote(encBody, '');

    // WHEN THE RENDER ENDS: the chime (RenderDoneSound.h), also switched by the header's SOUND chip.
    var doneSec = rsSection(outputPanel, 'WHEN THE RENDER ENDS');
    var soundTog = edSwitchTile(doneSec, 'Play a sound',
      'A chime when the render finishes, a lower one if it fails. Cancelling stays silent.', function () {
        cmd('mirv_filmmaker render sound toggle');
      });
    var soundVol = edValueTile(doneSec, { label: 'Volume', min: 0, max: 100, decimals: 0, unit: '%', fill: S.value,
      tip: 'How loud the chime is. It plays through Windows, so the game volume does not change it.',
      onChange: function (v) { cmd('mirv_filmmaker render sound volume ' + Math.round(v)); },
      onEnd: function () { cmd('mirv_filmmaker render sound test'); } });
    var soundTestRow = mk('Panel', doneSec); soundTestRow.hittest = false; soundTestRow.hittestchildren = true;
    soundTestRow.style.flowChildren = 'right'; soundTestRow.style.width = '100%'; soundTestRow.style.marginTop = '8px';
    var soundTestBtn = rsOutlineBtn(soundTestRow, 'Play the sound', function () {
      cmd('mirv_filmmaker render sound test');
    }, 'Hear the chime at this volume.', true);
    soundTestBtn.style.height = '30px';

    // Live game resolution in PHYSICAL pixels (actuallayout* is device px; uiscale only affects
    // how style px map onto it). Read every render, so the readout follows a resolution change.
    function renderGameRes() {
      var w = root.actuallayoutwidth || ctx.actuallayoutwidth || 0;
      var h = root.actuallayoutheight || ctx.actuallayoutheight || 0;
      if (!(w > 16 && h > 16)) return [1920, 1080];
      return [Math.round(w), Math.round(h)];
    }

    // One panel per view; RANGE & CAMERA answers both slot 0 and slot 1.
    var RENDER_TAB_PANELS = [shotPanel, passesPanel, outputPanel];
)EDJS"
R"EDJS(
    // ================= LEFT COLUMN: the job =================
    var jobHead = mk('Panel', renderJobCol);
    jobHead.style.width = '100%';
    jobHead.style.paddingLeft = '18px'; jobHead.style.paddingRight = '18px';
    jobHead.style.paddingTop = '13px'; jobHead.style.paddingBottom = '11px';
    var jobTitle = lbl(jobHead, 'JOB', S.render, 16);
    jobTitle.style.fontWeight = 'bold'; jobTitle.style.letterSpacing = '2px';
    var jobRule = mk('Panel', renderJobCol); jobRule.hittest = false;
    jobRule.style.width = '100%'; jobRule.style.height = '1px'; jobRule.style.backgroundColor = S.panelBorder;

    var jobBody = mk('Panel', renderJobCol);
    jobBody.style.flowChildren = 'down'; jobBody.style.width = '100%';
    jobBody.style.height = 'fill-parent-flow(1.0)';
    jobBody.style.paddingLeft = '18px'; jobBody.style.paddingRight = '18px';
    jobBody.style.paddingTop = '12px'; jobBody.style.paddingBottom = '12px';

    // Deliberately generous: this column is the answer to "what will happen if I press go", so it
    // gets real type sizes and breathing room rather than a dense readout.
    function jobRow(label) {
      var r = mk('Panel', jobBody);
      r.style.flowChildren = 'down'; r.style.width = '100%'; r.style.marginBottom = '13px';
      var k = lbl(r, label.toUpperCase(), S.hint, 10);
      k.style.letterSpacing = '1px'; k.style.marginBottom = '2px';
      var v = lbl(r, '', S.value, 16); v.style.width = '100%'; v.style.fontWeight = 'bold';
      return v;
    }
    var jobRange = jobRow('Range');
    var jobLength = jobRow('Length');
    var jobFrames = jobRow('Frames');
    var jobCamera = jobRow('Camera');
    var jobHud = jobRow('Passes');
    var jobFormat = jobRow('Output');
    var jobFolder = jobRow('Folder');
    jobFolder.style.fontSize = '13px'; // paths are long; keep them on one readable line

    var jobDivider = mk('Panel', jobBody); jobDivider.hittest = false;
    jobDivider.style.width = '100%'; jobDivider.style.height = '1px';
    jobDivider.style.backgroundColor = S.panelBorder;
    jobDivider.style.marginTop = '4px'; jobDivider.style.marginBottom = '14px';

    var jobStatus = lbl(jobBody, '', S.value, 15);
    jobStatus.style.fontWeight = 'bold'; jobStatus.style.marginBottom = '6px';
    var jobProblem = lbl(jobBody, '', RENDER_ERROR_TEXT, 14);
    jobProblem.style.whiteSpace = 'normal';
    jobProblem.style.width = '100%';

    // PREVIOUS RENDERS: this demo's earlier takes (RenderTakeHistory.h). Use starts the next job
    // from one -- the same range, camera, passes, frame rate and format -- to re-render a shot with
    // one change, or to add a pass for exactly the same In / Out.
    var PREV_ROWS = 3;
    var prevSec = mk('Panel', jobBody); prevSec.hittest = false; prevSec.hittestchildren = true;
    prevSec.style.flowChildren = 'down'; prevSec.style.width = '100%'; prevSec.style.marginTop = '16px';
    prevSec.visible = false;
    var prevCap = lbl(prevSec, 'PREVIOUS RENDERS', S.hint, 10); prevCap.hittest = false;
    prevCap.style.letterSpacing = '1px'; prevCap.style.marginBottom = '6px';
    var prevRows = [];
    for (var pri = 0; pri < PREV_ROWS; pri++) (function (idx) {
      var row = mk('Panel', prevSec); row.hittest = false; row.hittestchildren = true;
      row.style.flowChildren = 'right'; row.style.width = '100%'; row.style.marginBottom = '8px';
      var txt = mk('Panel', row); txt.hittest = false;
      txt.style.flowChildren = 'down'; txt.style.width = 'fill-parent-flow(1.0)'; txt.style.verticalAlign = 'center';
      var l1 = lbl(txt, '', S.value, 13); l1.hittest = false; l1.style.fontWeight = 'bold';
      l1.style.whiteSpace = 'normal'; l1.style.width = '100%';
      var l2 = lbl(txt, '', S.hint, 11); l2.hittest = false; l2.style.whiteSpace = 'normal'; l2.style.width = '100%';
      var use = rsOutlineBtn(row, 'Use', function () {
        cmd('mirv_filmmaker render import ' + idx);
        use.__lbl.text = 'Loaded'; use.__loadedUntil = (new Date()).getTime() + 1500;
      }, 'Start from this take: its range, camera, passes, frame rate and format.', true);
      use.style.height = '28px'; use.style.marginLeft = '8px'; use.style.verticalAlign = 'center';
      use.__lbl.style.fontSize = '12px';
      prevRows.push({ row: row, l1: l1, l2: l2, use: use });
    })(pri);
    var PREV_MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
    function prevWhen(iso) {
      // "2026-10-24T11:17:16" -> "Oct 24, 11:17"
      var m = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})/.exec(iso || '');
      if (!m) return iso || '';
      return PREV_MONTHS[(parseInt(m[2], 10) - 1) % 12] + ' ' + parseInt(m[3], 10) + ', ' + m[4] + ':' + m[5];
    }
    function renderPrevious(takes) {
      takes = takes || [];
      prevSec.visible = takes.length > 0;
      for (var i = 0; i < prevRows.length; i++) {
        var p = prevRows[i], t = takes[i];
        p.row.visible = !!t;
        if (!t) continue;
        var l1 = prevWhen(t.when) + (t.status && t.status !== 'completed' ? '  ·  ' + t.status : '');
        var n = (t.passes || []).length;
        var l2 = rsNum(t.start) + ' → ' + rsNum(t.end - 1) + '  ·  ' + rsClock(t.end - t.start)
          + '  ·  ' + n + (n === 1 ? ' pass' : ' passes') + '  ·  ' + t.fps + ' fps ' + ('' + t.format).toUpperCase();
        if (p.l1.text !== l1) p.l1.text = l1;
        if (p.l2.text !== l2) p.l2.text = l2;
        if (p.use.__loadedUntil && (new Date()).getTime() > p.use.__loadedUntil) {
          p.use.__loadedUntil = 0; p.use.__lbl.text = 'Use';
        }
      }
    }

    // jobBody flows DOWN, so `verticalAlign: bottom` on the action row would do nothing -- a
    // filling spacer is what actually pushes it to the bottom of the column.
    var jobSpacer = mk('Panel', jobBody); jobSpacer.hittest = false;
    jobSpacer.style.width = '100%'; jobSpacer.style.height = 'fill-parent-flow(1.0)';

    var jobActionRow = mk('Panel', jobBody);
    jobActionRow.style.flowChildren = 'right'; jobActionRow.style.width = '100%';
    // ONE button: START RENDER while idle, CANCEL RENDER while a job runs.
    var jobBtnRunning = false;
    var jobBtn = btnPrimary(jobActionRow, 'START RENDER', function () {
      cmd(jobBtnRunning ? 'mirv_filmmaker render cancel' : 'mirv_filmmaker render start');
    });
    jobBtn.style.width = 'fill-parent-flow(1.0)'; jobBtn.style.marginRight = '0px';
    jobBtn.style.minHeight = '52px';
    jobBtn.style.paddingTop = '8px'; jobBtn.style.paddingBottom = '8px';
    jobBtn.__lbl.style.horizontalAlign = 'center'; jobBtn.__lbl.style.fontSize = '14px';
    mvmTourAdd('render.start', jobBtn);
    function setJobBtnRunning(running) {
      if (jobBtnRunning === running) return;
      jobBtnRunning = running;
      var bg = running ? S.dangerBg : S.accent, br = running ? S.dangerBorder : S.accent;
      var fg = running ? S.dangerText : S.accentText;
      jobBtn.__bgBase = bg; jobBtn.__brBase = br; jobBtn.__fgBase = fg;
      jobBtn.__accent = br; jobBtn.__bgSel = bg; jobBtn.__bgSelHover = bg;
      jobBtn.__fgSel = fg; jobBtn.__brSel = fg;
      jobBtn.__lbl.text = running ? 'CANCEL RENDER' : 'START RENDER';
      btnVisual(jobBtn);
    }

    // ---- open / close ----
    function openRender() { cmd('mirv_filmmaker render open'); }
    // closeAllDrops() matters here: the dropdown popups are parented to renderRoot, and a popup
    // left visible when the modal closes would hang over the editor with nothing behind it.
    function closeRender() { renderFieldBlur(); closeAllDrops(); cmd('mirv_filmmaker render close'); }

    function renderTicksToSeconds(ticks) { return ticks / DEMO_TICKRATE; }
    function fmtDuration(seconds) {
      if (!isFinite(seconds) || seconds < 0) return '—';
      var m = Math.floor(seconds / 60), s = seconds - m * 60;
      return (m > 0 ? (m + 'm ') : '') + s.toFixed(1) + 's';
    }
    function fmtFolder(path) {
      if (!path) return 'not chosen';
      if (path.length <= 38) return path;
      return '…' + path.substring(path.length - 37);
    }
)EDJS"
R"EDJS(
    // ---- render (called each frame) ----
    function renderRender() {
      if (!st || !st.render) {
        renderRoot.visible = false;
        root.SetAttributeString('renderworkspace', '0');
        if (renderFocusField) renderFieldBlur();
        return;
      }

      var r = st.render;
      var isOpen = !!r.open;
      // Idempotent state projection: rebuilding the Panorama tree while open no longer loses the
      // transition edge and leaves the workspace hidden.
      renderRoot.visible = isOpen;
      root.SetAttributeString('renderworkspace', isOpen ? '1' : '0');
      if (!isOpen) {
        // A field left focused after the workspace closed would keep swallowing every keystroke.
        if (renderFocusField) renderFieldBlur();
        return;
      }
      renderPublishTextCapture(); // safety net: the attribute must never go stale while open
      // A press anywhere but the focused field hands the keyboard back (abandoning the edit, as Esc
      // does), so G and the other keys work again without Enter or Esc.
      var pressDown = !!st.lmb;
      if (pressDown && !renderPressDown && renderFocusField) {
        var fp = renderFocusField.panel, fq = fp && fp.GetPositionWithinWindow ? fp.GetPositionWithinWindow() : null;
        var onField = !!fq && st.mx >= fq.x && st.mx < fq.x + fp.actuallayoutwidth
          && st.my >= fq.y && st.my < fq.y + fp.actuallayoutheight;
        if (!onField) renderFieldBlur();
      }
      renderPressDown = pressDown;

      var out = r.out || {}, pass = r.pass || {};

      // -- tabs --
      var view = renderViewOf(r.tab || 0);
      // A typed field must lose focus on a tab change: its caret would keep blinking on a hidden row.
      var tabChanged = (renderLastTab !== view);
      if (tabChanged) {
        renderLastTab = view;
        closeAllDrops();
        renderFieldBlur();
      }
      for (var ti = 0; ti < renderTabs.length; ti++) styleTab(renderTabs[ti], ti === view, S.render);
      for (var tp = 0; tp < RENDER_TAB_PANELS.length; tp++) RENDER_TAB_PANELS[tp].visible = (tp === view);
      // Fired only on a real change, and AFTER the panel is visible -- an entrance written to a
      // hidden panel has no layout pass to animate against.
      if (tabChanged) moTabSwap(RENDER_TAB_PANELS[view]);

      // -- RANGE --
      var rm = r.rangeMode || 0;
      var av = r.avail || {};
      var resOk = !!r.resOk;
      var rangeBlocked = [
        false, false,
        (av.pathKeys || 0) < 2,
        (av.pathSel == null || av.pathSel < 0) || (av.pathSel + 1 >= (av.pathKeys || 0)),
        false
      ];
      rangeSeg.sync(rm);
      for (var rob = 0; rob < rangeSeg.cells.length; rob++) {
        rangeSeg.flag(rob, 'blocked', rangeBlocked[rob] && rob !== rm);
        rangeSeg.tip(rob, rangeBlocked[rob] ? RANGE_BLOCK_REASONS[rob] : RANGE_MODE_TIPS[rob]);
      }
      // A refusal stays up until the mode changes.
      if (rangeErr.__err && rm !== rangeErr.__lastMode) rsBlockClear(rangeErr);
      rangeErr.__lastMode = rm;

      // The RESOLVED span is what each mode actually means right now: Full Demo / Path Bounds /
      // Segment resolve in C++, Current Round is pushed back from the native round markers, and
      // Custom is already concrete. Showing the raw stored ticks instead reads as "0 to 0".
      var start = resOk ? (r.resStart || 0) : (r.startTick || 0);
      var endExcl = resOk ? (r.resEnd || 0) : (r.endTickExclusive || 0);
      var ticks = (resOk && endExcl > start) ? (endExcl - start) : 0;
      var nowTick = (st && typeof st.tick === 'number') ? (st.tick | 0) : start;
      rangeSpan.visible = ticks > 0;
      rangeEmpty.visible = !(ticks > 0);
      if (ticks > 0) {
        rsSpanStart = start; rsSpanEnd = endExcl;
        rsDemoStart = typeof r.demoStart === 'number' ? r.demoStart : -1;
        rsDemoEnd = typeof r.demoEnd === 'number' ? r.demoEnd : -1;
        spanIn.text = rsNum(start);
        spanLast.text = rsNum(endExcl - 1);
        spanLen.text = rsClock(ticks);
        spanTicks.text = rsNum(ticks);
        var lo = (r.demoStart >= 0) ? r.demoStart : 0;
        var hi = (r.demoEnd > lo) ? r.demoEnd : Math.max(endExcl, nowTick, lo + 1);
        spanEnd.text = rsClock(hi - lo);
        rsPlaceSpan(lo, hi, start, endExcl, nowTick);
      } else {
        rangeEmptyL.text = rm === 4 ? 'No In / Out set. Move the playhead and use Set In and Set Out below.'
          : rm === 0 ? 'Demo bounds unavailable.'
          : rm === 1 ? 'No round markers on the timeline yet.'
          : (RANGE_BLOCK_REASONS[rm] || 'Nothing to render in this range.');
      }
      rangeTip.text = RANGE_MODE_TIPS[rm] + ' The Out frame is excluded. Change the span from the playhead bar below.';
      phVal.text = rsNum(nowTick);

      // -- CAMERA --
      var camSrc = r.cameraSource || 0;
      camSpecTick(!!(r.avail && r.avail.spectating));
      for (var cob = 0; cob < camTiles.length; cob++) {
        var why = camSourceBlockedReason(cob);
        var sub = why ? CAM_BLOCK_SUBS[cob] : (cob === 4 ? ((av.pathKeys || 0) + ' keys · plays the path') : CAM_SUBS[cob]);
        camTiles[cob].__set(cob === camSrc, sub, !!why);
      }
      if (camErr.__err && camSrc !== camErr.__lastSrc) rsBlockClear(camErr);
      camErr.__lastSrc = camSrc;
      // Path-extension policy only applies to the authored camera path.
      extRow.visible = extNote.visible = (camSrc === 4);
      extSeg.sync((EXT_ROWS[r.extension || 0] || EXT_ROWS[0])[0]);
)EDJS"
R"EDJS(
      // -- PASSES --
      var hudMode = pass.hud || 0;
      for (var hb = 0; hb < hudTiles.length; hb++) hudTiles[hb].__set(hb === hudMode, HUD_SUBS[hb], false);
      camDataTog.set(!!pass.cameraData);
      bloodTog.set(pass.blood !== false);
      var rpList = pass.list || [];
      function rpHas(t, e) {
        for (var i = 0; i < rpList.length; i++) if (rpList[i].type === t && rpList[i].elements === e) return true;
        return false;
      }
      // A preview other than the full scene means View is on, whoever set it.
      var rpPv = rpPreviewOf(pass);
      if (rpPv) rpViewOn = true;
      rpPaintView(rpViewBtn);
      rpViewStrip.visible = rpViewOn;
      // The pass on screen, as "<type> <elements>": View on with no preview is the normal scene,
      // i.e. Beauty · Everything.
      var rpLive = rpViewOn ? (rpPv ? rpPv.type + ' ' + rpPv.elements : 'beauty all') : '';
      // The strip lists exactly the passes the job renders, in grid order (design v2).
      var rpViewable = 0, rpChip = 0;
      for (var rve = 0; rve < RP_ELEMENTS.length; rve++)
        for (var rvt = 0; rvt < RP_TYPES.length; rvt++) {
          if (!rpHas(RP_TYPES[rvt][0], RP_ELEMENTS[rve][0])) continue;
          var vkey = RP_TYPES[rvt][0] + ' ' + RP_ELEMENTS[rve][0], vinert = false;
          var vc = rpViewChips[rpChip] || rpViewChip();
          rpChip++;
          vc.__set(vkey, RP_TYPES[rvt][1] + ' · ' + RP_ELEMENTS[rve][1], vinert);
          if (!vinert) rpViewable++;
          var von = vkey === rpLive;
          if (vc.__on !== von) { vc.__on = von; vc.__paint(vc); }
        }
      for (; rpChip < rpViewChips.length; rpChip++) if (rpViewChips[rpChip].visible) rpViewChips[rpChip].visible = false;
      rpViewEmpty.visible = rpViewOn && !rpViewable;
      // The eye marks the cell the camera view is showing.
      for (var rei = 0; rei < RP_ELEMENTS.length; rei++) {
        var anyOn = false;
        for (var rti = 0; rti < RP_TYPES.length; rti++) {
          var on = rpHas(RP_TYPES[rti][0], RP_ELEMENTS[rei][0]);
          var live = on && rpLive === RP_TYPES[rti][0] + ' ' + RP_ELEMENTS[rei][0];
          anyOn = anyOn || on;
          rpCells[rei][rti].__setCell(on, live);
        }
        rpRows[rei].style.backgroundColor = anyOn ? '#ffffff06' : S.clear;
        rpRows[rei].__name.style.color = anyOn ? S.value : S.label;
      }
      var rpRenders = pass.rendersPerFrame || 0;
      rpSummary.text = rpList.length === 0
        ? 'Nothing to render — turn on at least one pass.'
        : (rpList.length + (rpList.length === 1 ? ' output' : ' outputs') + ' · '
           + rpRenders + (rpRenders === 1 ? ' render' : ' renders') + ' per frame');
      rpSummary.style.color = rpList.length === 0 ? RENDER_ERROR_TEXT : RS_MUTED;
      var rpKey = pass.key || [0, 255, 0];
      var rpKeyUsed = rpHas('key', 'players') || rpHas('key', 'gun');
      for (var rkb = 0; rkb < rpKeyBtns.length; rkb++) {
        var kc = RP_KEYS[rkb][1], kon = kc[0] === rpKey[0] && kc[1] === rpKey[1] && kc[2] === rpKey[2];
        if (rpKeyBtns[rkb].__on !== kon) { rpKeyBtns[rkb].__on = kon; rpKeyBtns[rkb].__paint(rpKeyBtns[rkb]); }
      }
      rpKeyGroup.style.opacity = rpKeyUsed ? '1.0' : '0.35';
      rpKeyGroup.hittestchildren = rpKeyUsed;
      rpKeyNote.text = rpKeyUsed ? 'The background of the Key passes.' : 'Turn on Key for Players only or Gun only to use this.';
      rpKeyNote.style.color = rpKeyUsed ? RS_MUTED : RS_DIM;
      var rpDepthUsed = false;
      for (var rdi = 0; rdi < rpList.length; rdi++) rpDepthUsed = rpDepthUsed || rpList[rdi].type === 'depth';
      rpNearTile.sync(pass.depthNear || 0);
      rpFarTile.sync(pass.depthFar || 0);
      rpDepthGroup.style.opacity = rpDepthUsed ? '1.0' : '0.35';
      rpDepthGroup.hittestchildren = rpDepthUsed;
      rpDepthNote.text = rpDepthUsed ? 'White at near, black at far. Normals are view space.' : 'Turn on a Depth pass to use this.';
      rpDepthNote.style.color = rpDepthUsed ? RS_MUTED : RS_DIM;

      // -- OUTPUT: destination --
      folderLbl.text = out.folderPending ? 'Choosing… (a folder dialog is open)' : (out.folder || 'No folder chosen yet.');
      folderLbl.style.color = out.folder ? S.value : S.dangerText;
      folderBox.style.border = '1px solid ' + (out.folder ? '#ffffff1f' : S.dangerBorder);
      for (var fc = 0; fc < fmtCells.length; fc++) {
        var fon = fmtCells[fc].__key === out.format;
        if (fmtCells[fc].__on !== fon) { fmtCells[fc].__on = fon; fmtCells[fc].__paint(fmtCells[fc]); }
      }
      fmtNote.text = out.formatNote || '';
      fmtNote.style.color = out.formatOk ? RS_MUTED : S.dangerText;
      // The take name is what lands on disk.
      projectLbl.text = 'Take: ' + (out.project || 'render') + '_<timestamp>';

      // -- OUTPUT: picture --
      var gameRes = renderGameRes();
      picDims.text = gameRes[0] + ' × ' + gameRes[1];
      var fpsCustom = fpsCustomOpen || FPS_CHOICES.indexOf(out.fps) < 0;
      fpsSeg.sync(fpsCustom ? 'custom' : ('' + out.fps));
      // The typed box only exists while it is the live answer: picking a preset hides it again.
      fpsFieldRow.visible = fpsCustom;

      // -- OUTPUT: encoding --
      var isMp4 = out.format === 'mp4';
      // qualCustomOpen keeps bitrate marked even if the command entering it is still in flight.
      var inBitrateMode = qualCustomOpen || (out.bitrateKbps || 0) > 0;
      qualSeg.sync(inBitrateMode ? 'bitrate' : ('' + out.crf));
      bitrateRow.visible = inBitrateMode;
      var enc = (typeof out.encoder === 'number') ? out.encoder : 2;
      encSeg.sync(enc === 2 ? 'auto' : (enc === 0 ? 'cpu' : 'gpu'));
      encSeg.flag(2, 'dim', out.nvenc === 3);
      encNote.text = (enc === 2 && out.nvenc === 2)
        ? 'Auto → GPU (NVENC). GPU encoding never slows a render down; CPU encoding can.'
        : (out.nvencNote || '');
      encNote.style.color = (out.encoder === 1 && out.nvenc === 3) ? S.dangerText : RS_MUTED;
      speedSeg.sync(['quality', 'balanced', 'fast'][out.speed || 0] || 'quality');
      // The speed presets change the CPU encoder's time per frame; on the GPU the three measured
      // the same (2026-10-18), so the row only shows when the job will encode on the CPU.
      speedRow.visible = !(enc === 1 || (enc === 2 && out.nvenc === 2));
      encBody.style.opacity = isMp4 ? '1.0' : '0.35';
      encBody.hittestchildren = isMp4;
      encAside.visible = !isMp4;
      if (!isMp4) {
        var fmtShort = out.format ? ('' + out.format).toUpperCase() : '?';
        encAside.text = 'MP4 ONLY · NOW ' + fmtShort;
      }
      if (renderFocusField !== bitrateField) renderFieldPaint(bitrateField);
)EDJS"
R"EDJS(
      if (renderFocusField !== fpsField) renderFieldPaint(fpsField);

      // -- the chime: header chip and OUTPUT > WHEN THE RENDER ENDS --
      var snd = r.sound || {};
      var soundOn = snd.on !== false;
      var soundText = soundOn ? 'SOUND: ON' : 'SOUND: OFF';
      if (renderSoundChip.__lbl.text !== soundText) renderSoundChip.__lbl.text = soundText;
      soundTog.set(soundOn);
      soundVol.sync(typeof snd.volume === 'number' ? snd.volume : 60);
      soundVol.tile.style.opacity = soundOn ? '1.0' : '0.45';

      // -- Job column --
      renderPrevious(r.takes);
      jobRange.text = ticks > 0 ? (start + '  →  ' + (endExcl - 1)) : 'not set';
      jobRange.style.color = ticks > 0 ? S.value : RENDER_ERROR_TEXT;
      jobLength.text = ticks > 0 ? (fmtDuration(renderTicksToSeconds(ticks)) + '   ·   ' + ticks + ' ticks') : '—';
      // Recording FPS is both mux and sample rate. Dial is preview-only (forced to 1 on render).
      var sampleFps = (out.captureFps > 0) ? out.captureFps : out.fps;
      var frames = (ticks > 0 && sampleFps > 0) ? Math.round(renderTicksToSeconds(ticks) * sampleFps) : 0;
      jobFrames.text = frames > 0 ? (frames + '   ·   ' + out.fps + ' fps') : '—';
      jobCamera.text = (CAM_SOURCES[camSrc] || ['', '?'])[1];
      var HUD_LABELS = ['No HUD', 'In-game HUD', 'Full HUD'];
      var rpNames = [];
      for (var rjn = 0; rjn < rpList.length; rjn++) rpNames.push(rpList[rjn].stream);
      jobHud.text = (rpNames.length ? rpNames.join(', ') : 'none')
        + '   ·   ' + (HUD_LABELS[hudMode] || 'No HUD')
        + '   ·   ' + (pass.cameraData ? 'camera metadata' : 'no camera metadata')
        + (pass.blood === false ? '   ·   no blood' : '');
      jobHud.style.color = rpNames.length ? S.value : RENDER_ERROR_TEXT;
      jobFormat.text = (out.formatName || '?') + '   ·   '
        + gameRes[0] + '×' + gameRes[1];
      jobFormat.style.color = out.formatOk ? S.value : RENDER_ERROR_TEXT;
      jobFolder.text = fmtFolder(out.folder);
      jobFolder.style.color = out.folder ? S.value : RENDER_ERROR_TEXT;

      var jobActive = !!r.jobActive;
      var fatal = r.fatalCount || 0;
      var pg = r.prog || {};
      if (jobActive) {
        jobStatus.text = 'RENDERING  ' + (pg.percent != null ? (pg.percent + '%') : '');
        jobStatus.style.color = S.render;
        jobProblem.text = (pg.phase || '') +
          '   ·   frame ' + (pg.framesDone || 0) + ' / ' + (pg.framesTotal || 0) +
          ((pg.rendersPerFrame || 0) > 1 ? ('   ·   ' + pg.passes + ' outputs, ' + pg.rendersPerFrame + ' renders/frame') : '') +
          '   ·   tick ' + (pg.curTick || 0);
        jobProblem.style.color = S.hint;
      } else if (r.sessionState === 12 && pg.lastError) {
        // Failed: keep the reason on screen until the next start/validation change.
        jobStatus.text = 'Render FAILED';
        jobStatus.style.color = RENDER_ERROR_TEXT;
        jobProblem.text = pg.lastError;
        jobProblem.style.color = RENDER_ERROR_TEXT;
      } else if (fatal > 0) {
        jobStatus.text = fatal + ' problem' + (fatal === 1 ? '' : 's') + ' — cannot start';
        jobStatus.style.color = RENDER_ERROR_TEXT;
        jobProblem.text = !resOk ? 'Choose a range in RANGE, or set In and Out at the playhead.'
          : (!out.folder ? 'Choose an output folder in OUTPUT.' : (r.firstFatal || ''));
        if (!resOk && !out.folder) jobProblem.text += '\nChoose an output folder in OUTPUT.';
        jobProblem.style.color = RENDER_ERROR_TEXT;
      } else {
        var done = (r.sessionState === 11 && pg.framesDone > 0)
          ? ('Last render: ' + pg.framesDone + ' frame(s) → ' + fmtFolder(pg.takeDir || '') + '.   ')
          : '';
        jobStatus.text = 'Ready to render';
        jobStatus.style.color = '#7be07bff';
        jobProblem.text = done + ((r.warnCount > 0) ? (r.warnCount + ' warning(s) — see: mirv_filmmaker render validate') : '');
        jobProblem.style.color = S.hint;
      }

      // A refused Start stays greyed with the reason right above it; Cancel is always clickable.
      setJobBtnRunning(jobActive);
      btnEnable(jobBtn, jobActive || fatal === 0);

      // -- title badge --
      renderTitle.text = jobActive ? 'RENDER · RUNNING' : 'RENDER';
      renderTitle.style.color = jobActive ? '#4e9fff' : S.render;
    }
)EDJS"
