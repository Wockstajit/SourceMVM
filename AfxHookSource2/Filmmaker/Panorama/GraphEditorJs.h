#pragma once

// Panorama JS for the TIMELINE dock: ONE After-Effects-style timeline for everything that is keyed
// in a shot. Built ONCE into the in-game HUD (CSGOHud) context by GraphEditorExperimentHud.cpp and
// torn down when the dock leaves the TIMELINE tab.
//
// It replaces three surfaces that used to be separate dock tabs and modes -- the CAMERA timeline,
// the camera GRAPH and the SCENE GRAPH -- so camera keys and scene keys are edited side by side:
//
//   * LAYERS on the left, each with an expandable property tree. The camera path is one layer
//     (Transform > Position / Rotation / Field of view); every CONFIG card with keyed or armed
//     scene properties is another ("EFFECTS · LIGHTING", "ENVIRONMENT · FOG", ...).
//   * KEYFRAMES / GRAPH on the right. KEYFRAMES is a dope sheet: one glyph per key, a bar per
//     layer that slides all of its keys, summary marks on collapsed rows. GRAPH draws the curves of
//     the selected layer / group / property, each scaled to its own range, with Bezier handles.
//   * One ruler with the render range (work area), one overview navigator, one playhead.
//
// Two models stay authoritative in C++ and are addressed by domain: camera channels through
// `mirv_filmmaker grapheditor ...` (GraphExpModel, baked into the camera path) and scene
// properties through `mirv_filmmaker scenekeys graph ...` (SceneKeyframeTrack). A channel's id here
// is its domain letter plus its index: c0..c6 for the camera, s<catalog index> for the scene.
//
// Dragging is driven by the streamed mouse (state.mx/my/lmb/rmb from the WndProc): Panorama has no
// mouse-move event in this context, so a full-dock catcher swallows clicks and processInput() reads
// the raw cursor. Buttons, chevrons, switches and the key navigator use their own onactivate.
//
//   C++ -> JS : attribute "state", then $.GraphExp.render() every frame.
//   JS  -> C++: console commands.

namespace Filmmaker {

inline const char* kGraphEditorJs = R"GEJS(
(function () {
  try {
    var existing = $('#GraphExpRoot'); if (existing) existing.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    // Design tokens (Docs/features/camera-editor-feature.md, "The TIMELINE dock").
    var S = {
      panel: '#10141aff', panelHex: '#10141a', graphBg: '#0b0e12ff', border: '#ffffff1f', chip: '#ffffff1a',
      rowLine: '#ffffff08', layerBg: '#ffffff0a', layerSel: '#f0b32326', rowSel: '#f0b3231c',
      // dim = DISABLED controls only; hint = static supporting text; muted = meta and menu hints.
      label: '#c8d3dfff', value: '#eef2f6ff', hint: '#afb9c6ff', muted: '#8b95a3ff', dim: '#6b7480ff',
      inactive: '#afb9c6ff',
      accent: '#f0b323ff', cameraAccent: '#f0b323ff', renderAccent: '#aeb4bcff',
      dangerText: '#fc9994ff', dangerBg: '#c92a2a26', dangerBorder: '#c92a2a66', danger: '#ff6b6bff',
      sel: '#ffffffff', playhead: '#ff5a5aee', handle: '#ffffffcc', glyph: '#c8d3dfff', summary: '#ffffff8c',
      btnBg: '#ffffff14', btnOn: '#f0b32333', btnHover: '#ffffff26', btnDown: '#ffffff3d', btnOnHover: '#f0b32352',
      grid: '#ffffff0c', gridMid: '#ffffff20',
      ovTrack: 'rgba(255,255,255,0.05)', ovView: '#ffffff20', ovViewHot: '#ffffff33',
      ovViewBorder: '#ffffff4d', ovViewBorderHot: '#f0b323cc', ovKey: '#ffffff66',
      // Read by the shared colour popover (ConfigColorPickerJs.h, spliced in below).
      drawer: '#11151bff', clear: 'rgba(0,0,0,0)',
      fontMonodigit: 'Stratum2 Bold Monodigit, Stratum2, "Arial Unicode MS"',
      fontMono: 'Stratum2Mono, monospace',
      font: 'Stratum2, "Arial Unicode MS"'
    };
    var LAYER_COLORS = { cam: '#f0b323', effects: '#d4b36a', environment: '#7fbdc7', other: '#aeb4bc' };

    // The operator-dragged inspector width, published by C++ in st.inspW (InspectorWidth.h). The
    // dock fills the screen left of that column; INSPECTOR_W is only the value before the first
    // state arrives.
    var INSPECTOR_W = 640;
    var graphInspW = INSPECTOR_W;
    // Pushed in by the panel being dragged right now (api.setInspW). The stored width only moves on
    // release, so during the gesture this is the only current source.
    var graphInspWOverride = 0;
    function graphInspectorW(screenW) {
      var w = graphInspW > 0 ? graphInspW : INSPECTOR_W;
      // Same floor and screen cap as InspectorWidth.h.
      if (screenW > 1) w = Math.min(w, Math.max(600, screenW * 0.62));
      return Math.floor(Math.max(600, w));
    }

    // ---- geometry (layout px, dock-local) ---------------------------------
    // 440 tall under the shared ribbon. Toolbar on top; the left column header sits beside the
    // overview + ruler; rows scroll in the middle; the footer closes the dock.
    var DOCK_H = 440, HEADER_H = 72, OVERVIEW_H = 14, RULER_H = 26, ROWS_Y = 112, FOOTER_H = 26;
    var ROWS_H = DOCK_H - ROWS_Y - FOOTER_H;
    var LEFTW = 340, PAD = 12, SB_W = 12, ROW_H = 22, VAL_W = 120, NAV_W = 54;
    var GLYPH = 14, KF_HIT = 8, HANDLE_R = 7, KF_DRAG_PX = 3;
    var GRAPH_TOP = 30, GRAPH_BOTTOM = 26;   // value-axis margins inside the graph area
    var BOTTOM_LIFT = 0;                      // flush with the screen bottom, like every dock tab

    // ---- dock motion ------------------------------------------------------
    // The dock rises from below when the TIMELINE tab opens. TRANSFORM ONLY, no opacity: the dock is
    // opaque over the live game. moDockHideFrames starts above the debounce so a fresh build (the
    // script is torn down while hidden) plays the entrance.
    var moDockSeen = false, moDockHideFrames = 99;
    function moDockIn(panel, fromY) {
      if (!panel || (panel.IsValid && !panel.IsValid())) return;
      var y = (typeof fromY === 'number') ? fromY : 14;
      try {
        panel.style.transitionProperty = 'transform';
        panel.style.transitionTimingFunction = 'cubic-bezier(0.16,1,0.3,1)';
        panel.style.transitionDuration = '0s';
        panel.style.transform = 'translateY(' + y + 'px)';
      } catch (e) { return; }
      $.Schedule(0, function () {
        try {
          if (!panel || (panel.IsValid && !panel.IsValid())) return;
          panel.style.transitionDuration = '0.2s';
          panel.style.transform = 'translateY(0px)';
        } catch (e2) {}
      });
    }
    // Visibility follows the RAW answer every frame; only the entrance waits out a one-frame blink.
    function moDockOpenIsReal(raw) {
      var wasHiddenLongEnough = (moDockHideFrames > 2);
      if (raw) { moDockHideFrames = 0; return wasHiddenLongEnough; }
      moDockHideFrames++;
      return false;
    }

    function mk(type, parent, props) { return $.CreatePanel(type, parent, '', props || {}); }
    function lbl(parent, text, color, size) {
      var l = mk('Label', parent); l.text = text || '';
      if (color) l.style.color = color; if (size) l.style.fontSize = size + 'px';
      l.style.fontFamily = S.font; return l;
    }

    // ---- hover tips -------------------------------------------------------
    // ONE shared tip panel, repositioned per anchor. Instructions live on the control under the
    // pointer, never burned into the editing area.
    var tipPanel = null, tipLabel = null;
    function showTip(anchor, text) {
      if (!text) { hideTip(); return; }
      if (!G) return;
      if (!tipPanel) {
        tipPanel = mk('Panel', dock); tipPanel.hittest = false; tipPanel.visible = false;
        tipPanel.style.zIndex = '30'; tipPanel.style.padding = '4px 8px';
        tipPanel.style.backgroundColor = 'rgba(8,10,14,0.97)';
        tipPanel.style.border = '1px solid ' + S.border; tipPanel.style.borderRadius = '3px';
        tipLabel = lbl(tipPanel, '', S.value, 11);
        tipLabel.hittest = false; tipLabel.style.whiteSpace = 'nowrap';
      }
      tipLabel.text = text; tipPanel.visible = true;
      var pos = (anchor && anchor.GetPositionWithinWindow) ? anchor.GetPositionWithinWindow() : null;
      var w = tipPanel.actuallayoutwidth || 0, h = tipPanel.actuallayoutheight || 0;
      var ax = pos ? pos.x / (uiscale || 1) - dockScreenX : 10;
      var ay = pos ? pos.y / (uiscale || 1) - dockScreenY : 10;
      if (ax + w > G.rw) ax = G.rw - w - 2; if (ax < 2) ax = 2;
      var ty = ay - h - 4; if (ty < 2) ty = ay + 24;
      tipPanel.style.position = ax.toFixed(0) + 'px ' + ty.toFixed(0) + 'px 0px';
    }
    function hideTip() { if (tipPanel && (!tipPanel.IsValid || tipPanel.IsValid())) tipPanel.visible = false; }
    function attachTip(panel, text) {
      panel.SetPanelEvent('onmouseover', function () { showTip(panel, text); });
      panel.SetPanelEvent('onmouseout', function () { hideTip(); });
    }

    // One keyframe-navigator button (◀ ◇ ▶, 17x20). A SIBLING of the row's other targets, never a
    // child: Panorama bubbles onactivate.
    function navBtn(parent, glyph, tip, fn) {
      var b = mk('Panel', parent); b.hittest = true;
      b.style.width = '17px'; b.style.height = '20px'; b.style.verticalAlign = 'center';
      b.style.borderRadius = '3px';
      var l = lbl(b, glyph, S.hint, glyph === '◇' ? 11 : 9);
      l.hittest = false; l.style.horizontalAlign = 'center'; l.style.verticalAlign = 'center';
      b.__lbl = l;
      b.SetPanelEvent('onactivate', function () { hideTip(); fn(); });
      b.SetPanelEvent('onmouseover', function () { b.style.backgroundColor = S.btnHover; showTip(b, tip); });
      b.SetPanelEvent('onmouseout', function () { b.style.backgroundColor = 'rgba(0,0,0,0)'; hideTip(); });
      return b;
    }
)GEJS"
#include "PanoramaUiMetricsJs.h"
#include "SpeedScrubberJs.h"
R"GEJS(
    // Hover and a short press flash come from panel events (no :hover in this context); the
    // SELECTED state is owned by btnVisual so a per-frame render() cannot paint over the hover.
    function panelAlive(b) {
      try {
        if (!b || (b.IsValid && !b.IsValid())) return false;
        if (!b.__lbl || (b.__lbl.IsValid && !b.__lbl.IsValid())) return false;
        return true;
      } catch (e) { return false; }
    }
    function btnVisual(b) {
      if (!panelAlive(b)) return;
      // A TAB never takes a fill: only its label colour and its accent underline move.
      if (b.__isTab) {
        b.style.backgroundColor = 'rgba(0,0,0,0)';
        b.__lbl.style.color = b.__sel ? S.accent : (b.__hot ? S.value : S.muted);
        applyTabUnderline(b);
        return;
      }
      var bg = b.__sel ? b.__bgSel : b.__bgBase;
      var fg = b.__sel ? b.__fgSel : b.__fgBase;
      if (b.__down) bg = S.btnDown;
      else if (b.__hot) bg = b.__sel ? S.btnOnHover : S.btnHover;
      b.style.backgroundColor = bg;
      b.__lbl.style.color = fg;
    }
    function applyTabUnderline(b) {
      b.style.borderBottom = '2px solid ' + (b.__sel ? S.accent : (b.__hot ? S.accent.substring(0, 7) + '66' : 'rgba(0,0,0,0)'));
    }
    function tabify(b) { b.__isTab = true; b.style.borderRadius = '0px'; btnVisual(b); return b; }
    function styleTab(b, on) {
      var changed = (b.__sel !== !!on);
      b.__sel = !!on;
      if (changed) btnVisual(b); else applyTabUnderline(b);
      return b;
    }
    function btnSel(b, on) {
      on = !!on;
      if (!b || b.__bgBase == null || b.__sel === on) return b;
      b.__sel = on; btnVisual(b); return b;
    }
    // `tip` is integrated (SetPanelEvent REPLACES a handler, so a separate attachTip would overwrite
    // the hover/press feedback this button owns).
    function btn(parent, text, onClick, color, tip) {
      var b = mk('Panel', parent); b.hittest = true;
      b.style.borderRadius = UI.btn.radius + 'px';
      b.style.paddingTop = UI.btnCompact.padV + 'px'; b.style.paddingBottom = UI.btnCompact.padV + 'px';
      b.style.paddingLeft = UI.btnCompact.padH + 'px'; b.style.paddingRight = UI.btnCompact.padH + 'px';
      b.style.marginRight = UI.btn.gap + 'px'; b.style.verticalAlign = 'center';
      var l = lbl(b, text, color || S.value, UI.btnCompact.font); l.style.fontWeight = 'bold';
      l.style.whiteSpace = 'nowrap';
      b.__lbl = l;
      b.__sel = false; b.__hot = false; b.__down = false;
      b.__bgBase = S.btnBg; b.__fgBase = color || S.value;
      b.__bgSel = S.btnOn; b.__fgSel = S.accent;
      b.SetPanelEvent('onmouseover', function () { b.__hot = true; btnVisual(b); if (tip) showTip(b, tip); });
      b.SetPanelEvent('onmouseout', function () { b.__hot = false; b.__down = false; btnVisual(b); hideTip(); });
      b.SetPanelEvent('onactivate', function () {
        b.__down = true; btnVisual(b);
        try { $.Schedule(0.11, function () { if (!panelAlive(b)) return; b.__down = false; btnVisual(b); }); }
        catch (e) { if (panelAlive(b)) { b.__down = false; btnVisual(b); } }
        onClick();
      });
      btnVisual(b);
      return b;
    }
    function lineSeg(parent, x0, y0, x1, y1, color, thick) {
      var dx = x1 - x0, dy = y1 - y0; var len = Math.sqrt(dx * dx + dy * dy); if (len < 0.5) len = 0.5;
      var ang = Math.atan2(dy, dx) * 180 / Math.PI;
      var p = mk('Panel', parent); p.hittest = false;
      p.style.position = x0.toFixed(1) + 'px ' + (y0 - thick / 2).toFixed(1) + 'px 0px';
      p.style.width = len.toFixed(1) + 'px'; p.style.height = thick + 'px';
      p.style.transformOrigin = '0% 50%'; p.style.transform = 'rotateZ(' + ang.toFixed(2) + 'deg)';
      p.style.backgroundColor = color; return p;
    }
    function dot(parent, cx, cy, size, color, rot) {
      var p = mk('Panel', parent); p.hittest = false;
      p.style.position = (cx - size / 2).toFixed(1) + 'px ' + (cy - size / 2).toFixed(1) + 'px 0px';
      p.style.width = size + 'px'; p.style.height = size + 'px';
      p.style.backgroundColor = color; p.style.borderRadius = (size / 2) + 'px';
      if (rot) { p.style.transformOrigin = '50% 50%'; p.style.transform = 'rotateZ(45deg)'; p.style.borderRadius = '1px'; }
      return p;
    }
    function box(parent, x, y, w, h, color) {
      var p = mk('Panel', parent); p.hittest = false;
      p.style.position = x.toFixed(1) + 'px ' + y.toFixed(1) + 'px 0px';
      p.style.width = Math.max(0, w).toFixed(1) + 'px'; p.style.height = Math.max(0, h).toFixed(1) + 'px';
      if (color) p.style.backgroundColor = color;
      return p;
    }
    function clamp01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
    // ---- host-scope shims for the shared colour picker --------------------
    // ConfigColorPickerJs.h is a raw-string CHUNK that expects these from its host. A missing one is
    // a ReferenceError the first time a colour property is built (Automation/tests/scene-graph-
    // tests.mjs checks the list against the picker's own calls).
    function endTileLine(parent) { if (parent) { parent.__tileLine = null; parent.__chipLine = null; } }
    function paintBtn(b) { btnVisual(b); }
    function hoverable(p, paint) {
      p.hittest = true; p.__hot = false;
      p.SetPanelEvent('onmouseover', function () { p.__hot = true; paint(p); });
      p.SetPanelEvent('onmouseout', function () { p.__hot = false; paint(p); });
      paint(p);
      return p;
    }
    function hex2(n) {
      n = Math.round(n); if (n < 0) n = 0; if (n > 255) n = 255;
      var h = n.toString(16); return h.length < 2 ? '0' + h : h;
    }
    function rgbHex(c) { return '#' + hex2(c[0]) + hex2(c[1]) + hex2(c[2]) + 'ff'; }
    // Fails OPEN on a throw: the walk runs off our tree into CS2's native panels.
    function cfgPanelOnScreen(pnl) {
      try {
        for (var q = pnl, guard = 0; q && guard < 64; ++guard) {
          if (q.IsValid && !q.IsValid()) return false;
          if (!q.visible) return false;
          q = q.GetParent ? q.GetParent() : null;
        }
      } catch (e) { return true; }
      return true;
    }
    function num(v) { return (typeof v === 'number' && isFinite(v)) ? v : 0; }
    function rgbOnly(hex) { return ('' + hex).substring(0, 7); }

    // ---- bezier sampling (mirrors GraphExpModel) ------------------------
    function bez(a0, a1, a2, a3, s) { var u = 1 - s; return u * u * u * a0 + 3 * u * u * s * a1 + 3 * u * s * s * a2 + s * s * s * a3; }
    function segActive(a, b) { return (a.cr && a.cr.a) || (b.cl && b.cl.a); }
    function segPts(a, b) {
      var span = b.t - a.t; if (span <= 1e-9) span = 1e-9;
      var txR = clamp01(a.cr && a.cr.a ? a.cr.tx : 1 / 3), txL = clamp01(b.cl && b.cl.a ? b.cl.tx : 1 / 3);
      return {
        p0x: a.t, p0y: a.v, p1x: a.t + txR * span, p1y: a.v + (a.cr && a.cr.a ? a.cr.dv : 0),
        p2x: b.t - txL * span, p2y: b.v + (b.cl && b.cl.a ? b.cl.dv : 0), p3x: b.t, p3y: b.v
      };
    }
    function sampleSeg(a, b, t) {
      var P = segPts(a, b); var lo = 0, hi = 1, s = (t - a.t) / (b.t - a.t);
      for (var i = 0; i < 24; i++) { s = 0.5 * (lo + hi); var x = bez(P.p0x, P.p1x, P.p2x, P.p3x, s); if (x < t) lo = s; else hi = s; }
      return bez(P.p0y, P.p1y, P.p2y, P.p3y, s);
    }
    function sampleChan(chn, t) {
      var k = chn.keys; if (!k.length) return null;
      if (t <= k[0].t) return k[0].v; if (t >= k[k.length - 1].t) return k[k.length - 1].v;
      for (var i = 0; i + 1 < k.length; i++) if (t >= k[i].t && t <= k[i + 1].t) {
        if (segActive(k[i], k[i + 1])) return sampleSeg(k[i], k[i + 1], t);
        var sp = k[i + 1].t - k[i].t; var f = sp > 1e-9 ? (t - k[i].t) / sp : 0; return k[i].v + f * (k[i + 1].v - k[i].v);
      }
      return k[k.length - 1].v;
    }
)GEJS"
R"GEJS(
    // ---- keyframe glyphs ----------------------------------------------------
    // A key is drawn as two halves, each saying how the curve meets the key on that side:
    //   lin  half diamond           ease  half square with a notch (AE's Easy Ease hourglass)
    //   auto half disc (Smooth)     hold  half square (a stepped property)
    // Panorama has no clip-path, so a half is a clipped box holding a shape centred on the key.
    // A filled right triangle P1-P2-C (right angle at C), in parent px: a big panel whose top edge
    // lies on the hypotenuse P1->P2, clipped by the triangle's bounding box. Real geometry, so it
    // never shows a background colour through (a painted notch did, on any highlighted row).
    function triPiece(parent, p1, p2, c, color) {
      var bx = Math.min(p1[0], p2[0], c[0]), by = Math.min(p1[1], p2[1], c[1]);
      var bw = Math.max(p1[0], p2[0], c[0]) - bx, bh = Math.max(p1[1], p2[1], c[1]) - by;
      if (bw <= 0 || bh <= 0) return;
      var a = p1, b = p2, dx = b[0] - a[0], dy = b[1] - a[1];
      // The panel fills the clockwise side of its top edge: travel so that C is on it.
      if ((c[0] - a[0]) * -dy + (c[1] - a[1]) * dx < 0) { a = p2; b = p1; dx = -dx; dy = -dy; }
      var clip = mk('Panel', parent); clip.hittest = false; clip.style.overflow = 'clip clip';
      clip.style.position = bx.toFixed(2) + 'px ' + by.toFixed(2) + 'px 0px';
      clip.style.width = bw.toFixed(2) + 'px'; clip.style.height = bh.toFixed(2) + 'px';
      var L = (bw + bh) * 2, f = mk('Panel', clip); f.hittest = false; f.style.backgroundColor = color;
      f.style.width = L + 'px'; f.style.height = L + 'px';
      f.style.position = (a[0] - bx).toFixed(2) + 'px ' + (a[1] - by).toFixed(2) + 'px 0px';
      f.style.transformOrigin = '0% 0%';
      f.style.transform = 'rotateZ(' + (Math.atan2(dy, dx) * 180 / Math.PI).toFixed(3) + 'deg)';
    }
    function glyphHalf(parent, side, kind, size, color, bg) {
      var h = size / 2;
      var hb = mk('Panel', parent); hb.hittest = false;
      hb.style.width = h + 'px'; hb.style.height = size + 'px'; hb.style.overflow = 'clip clip';
      var f = mk('Panel', hb); f.hittest = false; f.style.backgroundColor = color;
      if (kind === 'lin') {
        var d = size / Math.SQRT2, cx = side < 0 ? h : 0;
        f.style.width = d.toFixed(2) + 'px'; f.style.height = d.toFixed(2) + 'px';
        f.style.position = (cx - d / 2).toFixed(2) + 'px ' + (h - d / 2).toFixed(2) + 'px 0px';
        f.style.transformOrigin = '50% 50%'; f.style.transform = 'rotateZ(45deg)';
      } else if (kind === 'auto') {
        f.style.width = size + 'px'; f.style.height = size + 'px'; f.style.borderRadius = h + 'px';
        f.style.position = (side < 0 ? 0 : -h) + 'px 0px 0px';
      } else if (kind === 'ease') {
        // The design's polygon: the half square minus a triangle on its outer edge, apex 62% in.
        // = a bar on the inner side plus a right triangle above and below the apex.
        var ax = side < 0 ? 0.62 * h : 0.38 * h, ox = side < 0 ? 0 : h;
        f.style.width = (side < 0 ? h - ax : ax).toFixed(2) + 'px'; f.style.height = size + 'px';
        f.style.position = (side < 0 ? ax : 0).toFixed(2) + 'px 0px 0px';
        triPiece(hb, [ox, 0], [ax, h], [ax, 0], color);
        triPiece(hb, [ox, size], [ax, h], [ax, size], color);
      } else {
        f.style.width = h + 'px'; f.style.height = size + 'px';
        f.style.position = '0px 0px 0px';
      }
      return hb;
    }
    // A whole glyph. With x/y it is placed centred on (x, y); without, it flows inline.
    function glyph(parent, sides, size, color, bg, x, y) {
      var g = mk('Panel', parent); g.hittest = false;
      g.style.width = size + 'px'; g.style.height = size + 'px'; g.style.flowChildren = 'right';
      if (typeof x === 'number') g.style.position = (x - size / 2).toFixed(1) + 'px ' + (y - size / 2).toFixed(1) + 'px 0px';
      glyphHalf(g, -1, sides[0], size, color, bg);
      glyphHalf(g, 1, sides[1], size, color, bg);
      return g;
    }
    var EASE_SIDES = { smooth: ['auto', 'auto'], linear: ['lin', 'lin'], 'in': ['ease', 'lin'], out: ['lin', 'ease'], inout: ['ease', 'ease'] };
    // The side's STORED kind, never a guess from its slope: a flat Smooth handle and an Easy Ease
    // are the same numbers, and a handle dragged in the graph must stop reading as Smooth. Auto
    // Bezier (Smooth, follows its neighbours) = disc; any other handle (Easy Ease, a hand-dragged
    // Bezier) = hourglass; no handle = linear.
    function sideOf(h) { if (h && h.auto) return 'auto'; return (h && h.a) ? 'ease' : 'lin'; }
    function stepped(ch) { return !!ch && (ch.type === 'bool' || ch.type === 'enum' || ch.type === 'asset'); }
    function keySides(ch, k) { return stepped(ch) ? ['hold', 'hold'] : [sideOf(k.cl), sideOf(k.cr)]; }
    function easeOfSides(s) {
      // An eased side names the key before an auto one does: Ease In on a Smooth key reads as In.
      if (s[0] === 'hold') return 'hold';
      if (s[0] === 'ease' && s[1] === 'ease') return 'inout';
      if (s[0] === 'ease') return 'in';
      if (s[1] === 'ease') return 'out';
      if (s[0] === 'auto' || s[1] === 'auto') return 'smooth';
      return 'linear';
    }

    // ---- root, dock, catcher -----------------------------------------------
    // Root is FULL-SCREEN, transparent and non-hittest: a reliable screen-size reference and a
    // pass-through outside the dock. The dock is the bottom-left band left of the inspector.
    var root = $.CreatePanel('Panel', ctx, 'GraphExpRoot', {});
    // z65: see the canonical z-layer map in PanoramaBridge.h.
    root.hittest = false; root.style.width = '100%'; root.style.height = '100%'; root.style.zIndex = '65';
    root.style.fontFamily = S.font;
    var dock = mk('Panel', root); dock.hittest = false;
    dock.style.horizontalAlign = 'left'; dock.style.verticalAlign = 'bottom';
    dock.style.backgroundColor = S.panel; dock.style.overflow = 'clip clip';
    var catcher = mk('Panel', dock); catcher.hittest = true; catcher.style.width = '100%'; catcher.style.height = '100%';
    catcher.style.position = '0px 0px 0px';
    catcher.SetPanelEvent('onactivate', function () { /* swallow stray clicks; the pipe owns logic */ });

    // ---- toolbar ------------------------------------------------------------
    // Timecode | KEYFRAMES / GRAPH + what you have hold of | transport + speed over ease, snap,
    // history and delete.
    var header = mk('Panel', dock); header.hittest = false;
    header.style.position = '0px 0px 0px'; header.style.height = HEADER_H + 'px';
    header.style.flowChildren = 'right'; header.style.paddingLeft = '14px'; header.style.paddingRight = '14px';
    header.style.borderBottom = '1px solid ' + S.border;
    var tcCol = mk('Panel', header); tcCol.hittest = false;
    tcCol.style.width = '150px'; tcCol.style.flowChildren = 'down'; tcCol.style.verticalAlign = 'center';
    tcCol.style.marginRight = '18px';
    var tcLbl = lbl(tcCol, '0:00.00', S.accent, 22); tcLbl.hittest = false;
    tcLbl.style.fontFamily = S.fontMonodigit; tcLbl.style.marginBottom = '4px';
    var tickLbl = lbl(tcCol, '', S.hint, 11); tickLbl.hittest = false;
    tickLbl.style.letterSpacing = '0.5px'; tickLbl.style.whiteSpace = 'nowrap';
    var midCol = mk('Panel', header); midCol.hittest = false;
    midCol.style.width = 'fill-parent-flow(1.0)'; midCol.style.flowChildren = 'down';
    midCol.style.verticalAlign = 'center'; midCol.style.marginRight = '18px';
    var tabRow = mk('Panel', midCol); tabRow.hittest = false; tabRow.style.flowChildren = 'right';
    tabRow.style.marginBottom = '6px';
    function viewTab(caption, mode, last) {
      var b = tabify(btn(tabRow, caption, function () { setView(mode); }, S.label,
        mode === 'graph' ? 'Curves of the selected layer or property' : 'Every key, one row per property'));
      b.style.width = 'fit-children';
      b.style.paddingLeft = '0px'; b.style.paddingRight = '0px';
      b.style.paddingTop = '4px'; b.style.paddingBottom = '6px';
      b.style.marginRight = last ? '0px' : '16px';
      b.__lbl.style.fontSize = '15px'; b.__lbl.style.letterSpacing = '1.4px';
      return b;
    }
    var keysTab = viewTab('KEYFRAMES', 'keys');
    var graphTab = viewTab('GRAPH', 'graph', true);
    var selInfo = lbl(midCol, '', S.hint, 11); selInfo.hittest = false;
    selInfo.style.letterSpacing = '0.5px'; selInfo.style.whiteSpace = 'nowrap';
    selInfo.style.textOverflow = 'ellipsis'; selInfo.style.width = '100%';
    var rightCol = mk('Panel', header); rightCol.hittest = false;
    rightCol.style.flowChildren = 'down'; rightCol.style.verticalAlign = 'center';
    var rowTop = mk('Panel', rightCol); rowTop.hittest = false; rowTop.style.flowChildren = 'right';
    rowTop.style.horizontalAlign = 'right'; rowTop.style.marginBottom = '5px';
    var prevKeyBtn = btn(rowTop, '|◀', function () { jumpKey(-1); }, S.value, 'Previous keyframe on the selected layer');
    var playBtn = btn(rowTop, '▶', function () { togglePlay(); }, S.value, 'Play / pause (Space)');
    playBtn.style.width = '46px'; playBtn.__lbl.style.horizontalAlign = 'center';
    var nextKeyBtn = btn(rowTop, '▶|', function () { jumpKey(1); }, S.value, 'Next keyframe on the selected layer');
    nextKeyBtn.style.marginRight = '11px';
    // Cache: keep the preview of every frame the world plays, so scrubbing back over it shows at
    // frame rate instead of restoring the demo on every move (Movie/ScrubFrameCache.h). Lit while
    // on; right before the speed it shares the transport with, as on the old camera timeline.
    var cacheBtn = btn(rowTop, 'Cache', function () {
      rawCmd('mirv_filmmaker scrub cache ' + (st && st.scrubCache ? 'off' : 'on'));
    }, S.value, 'Keep every frame you play, so scrubbing back over it is instant');
    cacheBtn.style.marginRight = '11px';
    // The SAME continuous speed scrubber the native demo bar carries.
    var speedScrub = buildSpeedScrubber(rowTop, { width: 250, height: 30, font: S.font });
    speedScrub.strip.style.marginRight = '0px';
    var rowBot = mk('Panel', rightCol); rowBot.hittest = false; rowBot.style.flowChildren = 'right';
    rowBot.style.horizontalAlign = 'right';
)GEJS"
R"GEJS(
    // The interpolation strip. With keys selected it shapes THOSE keys; with nothing selected Smooth /
    // Linear set the camera path's one interpolation (the eases need a selection).
    var EASE_ITEMS = [['smooth', 'Smooth'], ['linear', 'Linear'], ['in', 'Ease In'], ['out', 'Ease Out'], ['inout', 'In/Out']];
    var easeBar = mk('Panel', rowBot); easeBar.hittest = false; easeBar.style.flowChildren = 'right';
    easeBar.style.border = '1px solid ' + S.chip; easeBar.style.borderRadius = '3px';
    easeBar.style.marginRight = '9px'; easeBar.style.verticalAlign = 'center'; easeBar.style.overflow = 'clip clip';
    var easeBtns = {};
    EASE_ITEMS.forEach(function (it) {
      var b = mk('Panel', easeBar); b.hittest = true; b.style.flowChildren = 'right';
      b.style.paddingTop = '5px'; b.style.paddingBottom = '5px'; b.style.paddingLeft = '9px'; b.style.paddingRight = '9px';
      var gw = mk('Panel', b); gw.hittest = false; gw.style.width = '10px'; gw.style.height = '10px';
      gw.style.verticalAlign = 'center'; gw.style.marginRight = '6px';
      var l = lbl(b, it[1], S.label, 12); l.hittest = false; l.style.fontWeight = 'bold';
      l.style.whiteSpace = 'nowrap'; l.style.verticalAlign = 'center';
      b.__gw = gw; b.__lbl = l; b.__mode = it[0]; b.__on = null; b.__hot = false; b.__en = true;
      b.SetPanelEvent('onmouseover', function () { b.__hot = true; paintEase(b); showTip(b, easeTip(it[0])); });
      b.SetPanelEvent('onmouseout', function () { b.__hot = false; paintEase(b); hideTip(); });
      b.SetPanelEvent('onactivate', function () { applyEase(it[0]); });
      easeBtns[it[0]] = b;
    });
    function easeTip(m) {
      return { smooth: 'Smooth tangents (no selection: the whole camera path)', linear: 'Straight lines between keys (no selection: the whole camera path)',
        'in': 'Ease into the selected keys (Shift+F9)', out: 'Ease out of the selected keys (Ctrl+Shift+F9)', inout: 'Easy Ease the selected keys (F9)' }[m];
    }
    function paintEase(b) {
      var on = !!b.__on;
      b.style.backgroundColor = on ? '#f0b3232e' : (b.__hot && b.__en ? '#ffffff0d' : 'rgba(0,0,0,0)');
      b.style.opacity = b.__en ? '1' : '0.45';
      b.__lbl.style.color = on ? S.value : S.label;
      var sig = (on ? 1 : 0) + '';
      if (b.__gsig !== sig) {
        b.__gsig = sig; b.__gw.RemoveAndDeleteChildren();
        glyph(b.__gw, EASE_SIDES[b.__mode], 10, on ? S.accent : S.glyph, S.panelHex);
      }
    }
    var snapBtn = btn(rowBot, 'SNAP', function () { showSnapMenu(); }, S.value,
      'Time snap: quantise the playhead and key placement to a tick grid');
    var undoBtn = btn(rowBot, '↺ Undo', function () { undo(false); }, S.value, 'Undo (Ctrl+Z)');
    var redoBtn = btn(rowBot, '↻ Redo', function () { undo(true); }, S.value, 'Redo (Ctrl+Shift+Z)');
    var delBtn = btn(rowBot, '✕', function () { deleteSelection(); }, S.dangerText, 'Delete the selected keyframes (Del)');
    delBtn.style.marginRight = '0px';
    delBtn.style.backgroundColor = S.dangerBg; delBtn.style.border = '1px solid ' + S.dangerBorder;

    // ---- time-snap dropdown ---------------------------------------------
    // Real panels with onactivate because Custom… hosts a text field. It opens UNDER the SNAP pill.
    var snapMenu = null, snapRows = [], snapCustomBox = null, snapCustomEntry = null, snapRect = null;
    var SNAP_W = 168, SNAP_HEAD = 22, SNAP_ITEM = 26, SNAP_PAD = 5;
    function snapMenuOpen() { return !!(snapMenu && snapMenu.visible); }
    function hideSnapMenu() { if (snapMenu) { snapMenu.visible = false; if (snapCustomBox) snapCustomBox.visible = false; } }
    function applySnap(v) { setSnap(v); hideSnapMenu(); }
    function refreshSnapRows() {
      var cur = snapSetting();
      for (var i = 0; i < snapRows.length; i++) {
        var row = snapRows[i], on = (row.__mode === cur);
        row.__lbl.style.color = on ? S.accent : S.value;
        row.style.backgroundColor = on ? S.btnOn : 'rgba(0,0,0,0)';
      }
    }
    function layoutSnapMenu() {
      if (!snapMenu) return;
      var h = SNAP_PAD * 2 + SNAP_HEAD + snapRows.length * SNAP_ITEM + (snapCustomBox && snapCustomBox.visible ? SNAP_ITEM : 0);
      snapMenu.style.width = SNAP_W + 'px'; snapMenu.style.height = h + 'px';
      var r = panelRect(snapBtn);
      var ax = r ? r.x1 - SNAP_W : 100, ay = r ? r.y1 + 2 : 60;
      ax = Math.max(0, Math.min(ax, (G ? G.rw : 1280) - SNAP_W - 2));
      snapMenu.style.position = ax.toFixed(0) + 'px ' + ay.toFixed(0) + 'px 0px';
      snapRect = { x0: ax, y0: ay, x1: ax + SNAP_W, y1: ay + h };
    }
    function buildSnapMenu() {
      if (snapMenu) return;
      var items = [['auto', 'Auto'], ['1', '1'], ['10', '10'], ['100', '100'], ['1000', '1000'], ['custom', 'Custom…'], ['off', 'Off']];
      snapMenu = mk('Panel', dock); snapMenu.hittest = false; snapMenu.visible = false;
      snapMenu.style.zIndex = '22'; snapMenu.style.flowChildren = 'down';
      snapMenu.style.paddingTop = SNAP_PAD + 'px'; snapMenu.style.paddingBottom = SNAP_PAD + 'px';
      snapMenu.style.backgroundColor = 'rgba(16,20,26,0.99)';
      snapMenu.style.border = '1px solid ' + S.border; snapMenu.style.borderRadius = '5px';
      snapMenu.style.boxShadow = '#000000aa 0px 6px 18px 0px';
      var head = mk('Panel', snapMenu); head.hittest = false;
      head.style.width = '100%'; head.style.height = SNAP_HEAD + 'px'; head.style.paddingLeft = '11px';
      var t = lbl(head, 'TIME SNAP (TICKS)', S.hint, 10); t.hittest = false; t.style.verticalAlign = 'center';
      t.style.fontWeight = 'bold'; t.style.letterSpacing = '1.4px';
      snapRows = [];
      for (var i = 0; i < items.length; i++) (function (mode, label) {
        var row = mk('Panel', snapMenu); row.hittest = true;
        row.style.width = (SNAP_W - 2 * SNAP_PAD) + 'px'; row.style.height = SNAP_ITEM + 'px';
        row.style.horizontalAlign = 'center'; row.style.borderRadius = '3px'; row.style.paddingLeft = '11px';
        var l = lbl(row, label, S.value, 13); l.hittest = false; l.style.verticalAlign = 'center'; l.style.whiteSpace = 'nowrap';
        row.__lbl = l; row.__mode = mode;
        row.SetPanelEvent('onactivate', function () {
          if (mode === 'custom') {
            if (snapCustomBox) {
              snapCustomBox.visible = true; layoutSnapMenu();
              if (snapCustomEntry) { snapCustomEntry.SetFocus(); try { snapCustomEntry.RaiseSelectAll && snapCustomEntry.RaiseSelectAll(); } catch (e) {} }
            }
            return;
          }
          applySnap(mode);
        });
        row.SetPanelEvent('onmouseover', function () { if (row.__mode !== snapSetting()) row.style.backgroundColor = S.btnHover; });
        row.SetPanelEvent('onmouseout', function () { if (row.__mode !== snapSetting()) row.style.backgroundColor = 'rgba(0,0,0,0)'; });
        snapRows.push(row);
      })(items[i][0], items[i][1]);
      snapCustomBox = mk('Panel', snapMenu); snapCustomBox.visible = false; snapCustomBox.hittest = true;
      snapCustomBox.style.width = (SNAP_W - 2 * SNAP_PAD) + 'px'; snapCustomBox.style.height = SNAP_ITEM + 'px';
      snapCustomBox.style.horizontalAlign = 'center';
      snapCustomEntry = $.CreatePanel('TextEntry', snapCustomBox, 'SnapCustomTick', {});
      snapCustomEntry.style.width = (SNAP_W - 2 * SNAP_PAD - 24) + 'px'; snapCustomEntry.style.height = '20px';
      snapCustomEntry.style.verticalAlign = 'center'; snapCustomEntry.style.fontSize = '12px'; snapCustomEntry.style.color = S.value;
      snapCustomEntry.style.backgroundColor = 'rgba(0,0,0,0.6)'; snapCustomEntry.style.border = '1px solid ' + S.border;
      snapCustomEntry.SetPanelEvent('oninputsubmit', function () {
        var n = parseFloat(snapCustomEntry.text);
        if (isFinite(n) && n >= 1) applySnap('' + Math.round(n)); else hideSnapMenu();
      });
    }
    function showSnapMenu() {
      buildSnapMenu();
      closeMenu();
      snapCustomBox.visible = false;
      refreshSnapRows();
      layoutSnapMenu();
      snapMenu.visible = true;
    }
)GEJS"
R"GEJS(
    // ---- column header, overview, ruler ------------------------------------
    var colHead = mk('Panel', dock); colHead.hittest = false;
    colHead.style.position = '0px ' + HEADER_H + 'px 0px';
    colHead.style.width = LEFTW + 'px'; colHead.style.height = (ROWS_Y - HEADER_H) + 'px';
    colHead.style.flowChildren = 'right'; colHead.style.paddingLeft = '10px'; colHead.style.paddingRight = '8px';
    colHead.style.borderRight = '1px solid ' + S.border; colHead.style.borderBottom = '1px solid ' + S.border;
    function headCap(text, w, align) {
      var l = lbl(colHead, text, S.hint, 11); l.hittest = false;
      l.style.fontWeight = 'bold'; l.style.letterSpacing = '2px'; l.style.verticalAlign = 'center';
      if (w) { l.style.width = w + 'px'; l.style.textAlign = align; l.style.marginLeft = '6px'; }
      else l.style.width = 'fill-parent-flow(1.0)';
      return l;
    }
    headCap('LAYERS'); headCap('VALUE', VAL_W, 'right'); headCap('KEY', NAV_W, 'center');

    // The overview maps the WHOLE demo: key marks, the render range, the playhead and the view box.
    // Drag the box to scroll, an end to zoom, empty track to jump.
    var overview = mk('Panel', dock); overview.hittest = false;
    overview.style.backgroundColor = 'rgba(0,0,0,0.25)'; overview.style.overflow = 'clip clip';
    var ovTrack = mk('Panel', overview); ovTrack.hittest = false; ovTrack.style.backgroundColor = S.ovTrack;
    var ovKeyLayer = mk('Panel', overview); ovKeyLayer.hittest = false;
    ovKeyLayer.style.position = '0px 0px 0px'; ovKeyLayer.style.width = '100%'; ovKeyLayer.style.height = '100%';
    var ovRange = mk('Panel', overview); ovRange.hittest = false; ovRange.style.backgroundColor = '#f0b32340';
    var ovPlay = mk('Panel', overview); ovPlay.hittest = false;
    ovPlay.style.width = '1px'; ovPlay.style.height = '100%'; ovPlay.style.backgroundColor = S.playhead;
    var ovView = mk('Panel', overview); ovView.hittest = false;
    ovView.style.backgroundColor = S.ovView; ovView.style.border = '1px solid ' + S.ovViewBorder;
    ovView.style.borderRadius = '2px';
    function ovGrip() {
      var g = mk('Panel', overview); g.hittest = false;
      g.style.width = '6px'; g.style.height = '12px'; g.style.borderRadius = '2px'; g.style.backgroundColor = '#303740ff';
      return g;
    }
    var ovLeftGrip = ovGrip(), ovRightGrip = ovGrip();

    // The ruler: ticks that adapt to the zoom, the render range (work area) as a gold bar with end
    // handles along its top, and a scrub zone everywhere else.
    var ruler = mk('Panel', dock); ruler.hittest = false; ruler.style.backgroundColor = '#00000040';
    ruler.style.borderBottom = '1px solid ' + S.border; ruler.style.overflow = 'clip clip';
    var rangeFill = mk('Panel', ruler); rangeFill.hittest = false; rangeFill.style.backgroundColor = '#f0b32310';
    rangeFill.style.borderLeft = '1px solid #f0b32399'; rangeFill.style.borderRight = '1px solid #f0b32399';
    var rulerLayer = mk('Panel', ruler); rulerLayer.hittest = false;
    rulerLayer.style.position = '0px 0px 0px'; rulerLayer.style.width = '100%'; rulerLayer.style.height = '100%';
    var rangeBar = mk('Panel', ruler); rangeBar.hittest = false; rangeBar.style.backgroundColor = '#f0b32366';
    var rangeL = mk('Panel', ruler); rangeL.hittest = false; rangeL.style.backgroundColor = S.accent;
    rangeL.style.borderRadius = '1px'; rangeL.style.width = '7px'; rangeL.style.height = '8px';
    var rangeR = mk('Panel', ruler); rangeR.hittest = false; rangeR.style.backgroundColor = S.accent;
    rangeR.style.borderRadius = '1px'; rangeR.style.width = '7px'; rangeR.style.height = '8px';

    // ---- rows ---------------------------------------------------------------
    var rowsView = mk('Panel', dock); rowsView.hittest = false; rowsView.style.overflow = 'clip clip';
    var rowsInner = mk('Panel', rowsView); rowsInner.hittest = false;
    var rowsSbThumb = mk('Panel', dock); rowsSbThumb.hittest = false; rowsSbThumb.visible = false;
    rowsSbThumb.style.width = '6px'; rowsSbThumb.style.borderRadius = '3px'; rowsSbThumb.style.backgroundColor = '#ffffff33';
    var emptyHint = lbl(dock, '', S.hint, 13); emptyHint.hittest = false; emptyHint.visible = false;

    // ---- graph area (replaces the track side when GRAPH is up) -------------
    var graph = mk('Panel', dock); graph.hittest = false; graph.visible = false;
    graph.style.backgroundColor = S.graphBg; graph.style.overflow = 'clip clip';
    var gridLayer = mk('Panel', graph); gridLayer.hittest = false;
    var curveLayer = mk('Panel', graph); curveLayer.hittest = false;
    var kfLayer = mk('Panel', graph); kfLayer.hittest = false;
    var handleLayer = mk('Panel', graph); handleLayer.hittest = false;
    [gridLayer, curveLayer, kfLayer, handleLayer].forEach(function (L) { L.style.position = '0px 0px 0px'; L.style.width = '100%'; L.style.height = '100%'; });
    var legend = mk('Panel', graph); legend.hittest = false; legend.style.position = '12px 10px 0px';
    legend.style.flowChildren = 'right'; legend.style.padding = '5px 10px 5px 10px';
    legend.style.backgroundColor = 'rgba(8,10,14,0.9)'; legend.style.border = '1px solid ' + S.border;
    legend.style.borderRadius = '3px';
    var graphEmpty = lbl(graph, '', S.hint, 13); graphEmpty.hittest = false;
    graphEmpty.style.horizontalAlign = 'center'; graphEmpty.style.verticalAlign = 'center';

    // Box selection, drawn over whichever view is up.
    var selOverlay = mk('Panel', dock); selOverlay.hittest = false; selOverlay.style.zIndex = '4';
    var rectVis = mk('Panel', selOverlay); rectVis.hittest = false; rectVis.visible = false;
    rectVis.style.backgroundColor = '#f0b32322'; rectVis.style.border = '1px solid ' + S.accent;

    // The playhead runs from just under the render-range bar down through the rows.
    var phLine = mk('Panel', dock); phLine.hittest = false; phLine.style.zIndex = '5';
    phLine.style.width = '2px'; phLine.style.backgroundColor = S.playhead;
    var phHandle = mk('Panel', dock); phHandle.hittest = false; phHandle.style.zIndex = '6';
    phHandle.style.width = '12px'; phHandle.style.height = '10px';
    phHandle.style.backgroundColor = S.playhead; phHandle.style.borderRadius = '3px';

    // ---- footer: render range, the visible span, FIT, zoom and the scrollbar ----
    var footer = mk('Panel', dock); footer.hittest = false;
    footer.style.flowChildren = 'right'; footer.style.borderTop = '1px solid ' + S.border;
    var footLeft = mk('Panel', footer); footLeft.hittest = false;
    footLeft.style.width = LEFTW + 'px'; footLeft.style.height = '100%'; footLeft.style.borderRight = '1px solid ' + S.border;
    var rangeTxt = lbl(footer, '', S.hint, 11); rangeTxt.hittest = false;
    rangeTxt.style.width = 'fill-parent-flow(1.0)'; rangeTxt.style.paddingLeft = '12px';
    rangeTxt.style.verticalAlign = 'center'; rangeTxt.style.whiteSpace = 'nowrap';
    var viewTxt = lbl(footer, '', S.hint, 11); viewTxt.hittest = false;
    viewTxt.style.verticalAlign = 'center'; viewTxt.style.whiteSpace = 'nowrap'; viewTxt.style.marginRight = '6px';
    function footBtn(glyphText, tip, fn, w) {
      var b = mk('Panel', footer); b.hittest = true;
      b.style.width = (w || 24) + 'px'; b.style.height = '18px'; b.style.verticalAlign = 'center';
      b.style.backgroundColor = S.btnBg; b.style.marginRight = '6px';
      var l = lbl(b, glyphText, S.value, glyphText.length > 1 ? 10 : 13); l.hittest = false;
      l.style.horizontalAlign = 'center'; l.style.verticalAlign = 'center'; l.style.fontWeight = 'bold';
      if (glyphText.length > 1) l.style.letterSpacing = '1px';
      b.__lbl = l;
      b.SetPanelEvent('onactivate', function () { hideTip(); fn(); });
      b.SetPanelEvent('onmouseover', function () { b.style.backgroundColor = S.btnHover; showTip(b, tip); });
      b.SetPanelEvent('onmouseout', function () { b.style.backgroundColor = S.btnBg; hideTip(); });
      return b;
    }
    // FIT frames every key (and refits every curve's height in GRAPH): the navigator and the zoom
    // buttons only move the window from where it is.
    var fitBtn = footBtn('FIT', 'Fit every key in the timeline', function () { fitAll(); }, 34);
    var zoomOut = footBtn('−', 'Zoom out (Ctrl+Wheel)', function () { zoomStep(1.5); });
    var sbTrack = mk('Panel', footer); sbTrack.hittest = false;
    sbTrack.style.width = '220px'; sbTrack.style.height = '12px'; sbTrack.style.verticalAlign = 'center';
    sbTrack.style.backgroundColor = 'rgba(0,0,0,0.45)'; sbTrack.style.borderRadius = '7px'; sbTrack.style.marginRight = '6px';
    var sbThumb = mk('Panel', sbTrack); sbThumb.hittest = false;
    sbThumb.style.height = '12px'; sbThumb.style.backgroundColor = '#ffffff33'; sbThumb.style.borderRadius = '7px';
    var zoomIn = footBtn('+', 'Zoom in (Ctrl+Wheel)', function () { zoomStep(1 / 1.5); });
    zoomIn.style.marginRight = '14px';
)GEJS"
// THE SAME COLOUR POPOVER THE CONFIG PANEL USES -- one definition spliced into both workspaces. A
// scene colour property opens the control the rest of this UI opens for a colour.
#include "ConfigColorPickerJs.h"
R"GEJS(
    // THE COLOUR PICKER'S TRAY. The shared picker opens inline under its parent; this dock has no
    // flowing column for it, so colour chips parent their picker here: one floating panel beside the
    // layer list, placed next to the chip that opened it.
    var PICK_W = 392;
    var pickTray = mk('Panel', dock); pickTray.hittest = false; pickTray.hittestchildren = true;
    pickTray.visible = false; pickTray.style.zIndex = '24'; pickTray.style.width = PICK_W + 'px';
    pickTray.style.flowChildren = 'down'; pickTray.style.padding = '8px 10px 10px 10px';
    pickTray.style.backgroundColor = 'rgba(16,20,26,0.99)'; pickTray.style.borderRadius = '5px';
    pickTray.style.border = '1px solid ' + S.border; pickTray.style.boxShadow = '#000000aa 0px 6px 18px 0px';
    var pickHead = lbl(pickTray, '', S.hint, 10); pickHead.hittest = false;
    pickHead.style.fontWeight = 'bold'; pickHead.style.letterSpacing = '1.4px';
    var pickBody = mk('Panel', pickTray); pickBody.hittest = false; pickBody.hittestchildren = true;
    pickBody.style.width = '100%'; pickBody.style.flowChildren = 'down';
    var pickAnchor = null, pickTitle = '', pickPos = '';
    function panelRect(p) {
      if (!p || (p.IsValid && !p.IsValid()) || !p.GetPositionWithinWindow) return null;
      var pos = p.GetPositionWithinWindow(), s = uiscale || 1;
      var x = pos.x / s - dockScreenX, y = pos.y / s - dockScreenY;
      return { x0: x, y0: y, x1: x + (p.actuallayoutwidth || 0) / s, y1: y + (p.actuallayoutheight || 0) / s };
    }
    function overPanel(p, lx, ly) { var r = panelRect(p); return !!r && lx >= r.x0 && lx <= r.x1 && ly >= r.y0 && ly <= r.y1; }
    function placePickTray(anchor, title) {
      if (anchor !== pickAnchor || title !== pickTitle) { pickAnchor = anchor; pickTitle = title; pickHead.text = ('' + title).toUpperCase(); }
      var r = panelRect(anchor), h = (pickTray.actuallayoutheight || 0) / (uiscale || 1) || 250;
      var y = r ? r.y0 - 12 : ROWS_Y;
      y = Math.max(HEADER_H + 4, Math.min(DOCK_H - h - 4, y));
      var pos = (LEFTW + 10) + 'px ' + Math.round(y) + 'px 0px';
      if (pos !== pickPos) { pickPos = pos; pickTray.style.position = pos; }
      pickTray.visible = true;
    }
    function closePickTray() { if (cfgColorOwner) cfgCloseColor(); pickTray.visible = false; pickAnchor = null; }

    // ---- state + interaction vars (closure) -----------------------------
    var st = null, G = null, uiscale = 1;
    var dockScreenX = 0, dockScreenY = 0;     // dock top-left in screen layout px (mouse mapping)
    var camCh = [], scCh = [], chMap = {};
    var tree = [], rows = [], treeSig = '', rowSig = '', trackSig = '', graphSig = '';
    var openState = {}, splitState = {};
    var selNode = '';                          // the selected row's node id (what GRAPH draws)
    var viewMode = 'keys', viewHold = 0;        // viewHold: frames a local view pick wins over state
    var lastDom = 'cam';                        // the domain of the last edit (Undo goes there first)
    var prevLmb = false, prevRmb = false, gesture = null;
    var selCache = {};
    var rowScroll = 0, rowsContentH = 0;

    function rawCmd(s) { try { GameInterfaceAPI.ConsoleCommand(s); } catch (e) {} }
    function camCmd(s) { rawCmd('mirv_filmmaker grapheditor ' + s); }
    function sceneCmd(s) { rawCmd('mirv_filmmaker scenekeys graph ' + s); }
    function domCmd(dom, s) { if (dom === 'scene') sceneCmd(s); else camCmd(s); }
    function edit(dom) { lastDom = dom; }
    // THE PLAYHEAD HAS ONE VERB. `grapheditor playhead` moves the shared cursor AND drags the demo
    // world with it (DemoScrub), for camera and scene alike.
    function playheadTo(t) { rawCmd('mirv_filmmaker grapheditor playhead ' + (+t).toFixed(2)); }
    // Land the world on a whole tick and hand the playhead back. The playhead holds that tick while
    // the demo travels there (Movie/TimelinePlayhead.h).
    function releasePlayhead(t) {
      t = Math.max(0, Math.round(t));
      playheadTo(t);
      rawCmd('mirv_filmmaker scrub release ' + t);
      rawCmd('mirv_filmmaker grapheditor playhead release');
    }

    function chU(uid) { return chMap[uid] || null; }
    function domOf(uid) { return ('' + uid).charAt(0) === 's' ? 'scene' : 'cam'; }
    function chNum(uid) { return parseInt(('' + uid).substring(1), 10); }
    function tps() { var r = num(st && st.tickRate); return r > 1 ? r : 64; }
    function fmtTime(t) {
      var s = Math.max(0, t) / tps(), m = Math.floor(s / 60); s -= m * 60;
      return m + ':' + (s < 10 ? '0' : '') + s.toFixed(2);
    }
    function fmtTimeShort(t) { return fmtTime(t).replace(/\.00$/, ''); }
    function keyAtTick(ch, t) {
      if (!ch) return null;
      var want = Math.round(t);
      for (var i = 0; i < ch.keys.length; i++) if (Math.round(ch.keys[i].t) === want) return ch.keys[i];
      return null;
    }
    function isSelectedId(uid, id) { return !!selCache[uid + ':' + id]; }
    function selCountDom(dom) { return dom === 'scene' ? num(st.scene && st.scene.selCount) : num(st.selCount); }
    function totalSel() { return selCountDom('cam') + selCountDom('scene'); }
)GEJS"
R"GEJS(
    // ---- the visible time window (owned HERE) ---------------------------
    // C++ publishes viewT0/viewT1 as a SUGGESTED fit plus a viewEpoch that advances when that
    // suggestion should be re-applied (dock enter, reseed, clear, demo change). After the seed the JS
    // owns pan and zoom: moving the playhead NEVER moves the view by itself; only the navigator, the
    // scrollbar, the zoom buttons, FIT, Ctrl/Shift+wheel and playback paging do.
    var view = { t0: 0, t1: 1, mode: '', epoch: null };
    var MIN_SPAN = 24;
    function domainRange() {
      var d0 = num(st.domainT0), d1 = num(st.domainT1);
      if (!(d1 > d0)) {
        d0 = Math.min(view.t0, num(st.playhead)); d1 = Math.max(view.t1, num(st.playhead));
        if (!(d1 > d0)) { d0 = 0; d1 = 2000; }
      }
      return { t0: d0, t1: d1 };
    }
    function seedView(mode) {
      var a = num(st.viewT0), b = num(st.viewT1);
      if (!(b > a + 0.5)) { var p = num(st.playhead); a = p - 1000; b = p + 1000; }
      view.t0 = a; view.t1 = b; view.mode = mode; view.epoch = (st.viewEpoch == null ? null : st.viewEpoch);
      normalizeView();
    }
    function normalizeView() {
      var d = domainRange(), domSpan = Math.max(1, d.t1 - d.t0);
      var minS = Math.min(MIN_SPAN, domSpan), maxS = domSpan;
      var span = view.t1 - view.t0;
      if (!(span > 0)) span = minS;
      if (span > maxS) span = maxS;
      if (span < minS) span = minS;
      var c = (view.t0 + view.t1) * 0.5;
      view.t0 = c - span / 2; view.t1 = view.t0 + span;
      // HARD CLAMP to the demo: no ticks before its start or past its end.
      if (view.t0 < d.t0) { view.t1 += d.t0 - view.t0; view.t0 = d.t0; }
      if (view.t1 > d.t1) { view.t0 -= view.t1 - d.t1; view.t1 = d.t1; }
      if (view.t0 < d.t0) view.t0 = d.t0;
    }
    function afterViewChange(automatic) {
      trackSig = ''; graphSig = '';
      if (!automatic) {
        // Manual navigation owns the window until playback is restarted.
        manualView = true;
        wasPlaying = !!st.playing;
      }
    }
    function zoomAt(f, anchorTick) {
      var span = view.t1 - view.t0;
      var ns = span * f;
      view.t0 = anchorTick - (anchorTick - view.t0) * (ns / span);
      view.t1 = view.t0 + ns;
      normalizeView(); afterViewChange();
    }
    function zoomStep(f) {
      var p = num(st.playhead);
      var anchor = (p >= view.t0 && p <= view.t1) ? p : (view.t0 + view.t1) * 0.5;
      zoomAt(f, anchor);
    }
    function panTicks(dt) { view.t0 += dt; view.t1 += dt; normalizeView(); afterViewChange(); }
    function panByFrac(frac) { panTicks(frac * (view.t1 - view.t0)); }
    // A JUMP (prev/next key, a new key off screen): if the target is off screen, shift the view so
    // it lands 2% inside. Never a zoom.
    function revealTicks(a, b) {
      if (!(b >= a)) return;
      var span = view.t1 - view.t0, m = span * 0.02;
      if (a >= view.t0 && b <= view.t1) return;
      if (b - a > span - 2 * m) { frameTicks(a, b); return; }
      if (a < view.t0) { view.t0 = a - m; view.t1 = view.t0 + span; }
      else { view.t1 = b + m; view.t0 = view.t1 - span; }
      normalizeView(); afterViewChange(true);
    }
    function frameTicks(a, b) {
      var pad = Math.max(32, (b - a) * 0.08);
      view.t0 = a - pad; view.t1 = b + pad;
      if (view.t1 - view.t0 < MIN_SPAN * 4) { var c = (a + b) / 2; view.t0 = c - MIN_SPAN * 2; view.t1 = c + MIN_SPAN * 2; }
      normalizeView(); afterViewChange(true);
    }
    // FIT: every key of every layer on screen, and every curve refitted to its own height.
    function fitAll() {
      var a = Infinity, b = -Infinity, all = camCh.concat(scCh);
      for (var i = 0; i < all.length; i++)
        for (var j = 0; j < all[i].keys.length; j++) { var t = all[i].keys[j].t; if (t < a) a = t; if (t > b) b = t; }
      if (!(b >= a)) { var p = num(st.playhead); a = p - 500; b = p + 500; }
      frameTicks(a, b);
      ranges = {}; graphSig = '';
    }
    // Keys that were not in the model last frame and sit outside the window are revealed: the only
    // way to create one off screen is from a playhead you cannot see.
    var knownKeys = null;
    function revealNewKeys() {
      var seen = {}, a = Infinity, b = -Infinity, fresh = !!knownKeys, all = camCh.concat(scCh);
      for (var i = 0; i < all.length; i++) {
        var ch = all[i];
        for (var j = 0; j < ch.keys.length; j++) {
          var k = ch.keys[j], id = ch.uid + ':' + k.id; seen[id] = 1;
          if (fresh && !knownKeys[id] && (k.t < view.t0 || k.t > view.t1)) { if (k.t < a) a = k.t; if (k.t > b) b = k.t; }
        }
      }
      knownKeys = seen;
      if (b >= a) revealTicks(a, b);
    }

    // ---- time snap --------------------------------------------------------
    // ONE shared step (editor.graph.snap) that quantises where things LAND -- the playhead, key
    // placement and key drags. Alt bypasses it for one gesture.
    function snapSetting() { return (st && st.snap) ? ('' + st.snap) : 'off'; }
    function niceSnap(v) {
      var p = Math.pow(10, Math.floor(Math.log(Math.max(1, v)) / Math.LN10));
      var n = v / p;
      var m = n < 1.5 ? 1 : n < 3.5 ? 2 : n < 7.5 ? 5 : 10;
      return Math.max(1, Math.round(m * p));
    }
    function snapStep() {
      var s = snapSetting();
      if (s === 'off' || s === '0') return 0;
      if (s === 'auto') return niceSnap((view.t1 - view.t0) / 40);
      var n = parseFloat(s);
      return (isFinite(n) && n >= 1) ? Math.max(1, Math.round(n)) : 0;
    }
    function snapTick(t) {
      var step = snapStep();
      if (step <= 0 || (st && st.alt)) return t;   // Alt = fine placement
      return Math.max(0, Math.round(t / step) * step);
    }
    // One place a scrub asks "what tick is under the cursor", so the snap is never forgotten.
    function scrubTick(tx) { return snapTick(xToTick(tx)); }
    function snapLabel() {
      var s = snapSetting();
      if (s === 'off' || s === '0') return 'SNAP';
      if (s === 'auto') return 'SNAP · Auto';
      return 'SNAP · ' + s;
    }
    function setSnap(v) {
      v = ('' + v);
      rawCmd('mirv_filmmaker settings ui editor.graph.snap ' + v);
      if (st) st.snap = v;
      trackSig = ''; graphSig = ''; navSig = '';
    }

    // While the demo PLAYS the window PAGES: when the playhead passes the right edge the next page
    // starts at the playhead (AE). Manual pan/zoom suspends this until playback restarts.
    var playWasInView = false, wasPlaying = false, manualView = false;
    function followPlayback() {
      var p = num(st.playhead), inside = (p >= view.t0 && p <= view.t1);
      if (!st.playing || st.scrubbing) { playWasInView = inside; wasPlaying = false; return; }
      var started = !wasPlaying; wasPlaying = true;
      if (started) manualView = false;
      if (manualView) { playWasInView = inside; return; }
      if (inside) { playWasInView = true; return; }
      if (!started && !playWasInView) return;
      var span = view.t1 - view.t0;
      view.t0 = (p > view.t1) ? p : (p - span * 0.02);
      view.t1 = view.t0 + span;
      normalizeView(); afterViewChange(true);
      playWasInView = true;
    }

    // ---- coordinate maps -----------------------------------------------------
    // Track-local x: 0 is the left edge of the track side (the column border), the time span runs
    // from PAD to PAD + G.cw.
    function tickToX(t) { return PAD + (t - view.t0) / (view.t1 - view.t0) * G.cw; }
    function xToTick(x) { return view.t0 + ((x - PAD) / G.cw) * (view.t1 - view.t0); }
    // The overview maps the WHOLE demo (domain) across the track width.
    function domToX(t) { var d = domainRange(); return (t - d.t0) / Math.max(1, d.t1 - d.t0) * G.tw; }
    function xToDom(x) { var d = domainRange(); return d.t0 + (x / G.tw) * Math.max(1, d.t1 - d.t0); }
    function ovViewRect() {
      var x0 = domToX(view.t0), x1 = domToX(view.t1);
      if (x0 < 0) x0 = 0; if (x1 > G.tw) x1 = G.tw;
      if (x1 < x0 + 8) x1 = Math.min(G.tw, x0 + 8);
      return { x0: x0, x1: x1 };
    }
)GEJS"
R"GEJS(
    // ---- the layer tree ---------------------------------------------------------
    // Rebuilt only when the SET of scene properties or a split changes; the rows flatten it by the
    // open state. Nodes carry channel ids, never channel objects (those are replaced every frame).
    var CAM_PROPS = [
      { id: 'cam.pos', name: 'Position', chans: [0, 1, 2], comps: ['X', 'Y', 'Z'], axn: ['X Position', 'Y Position', 'Z Position'], dec: 0, unit: '' },
      { id: 'cam.rot', name: 'Rotation', chans: [3, 4, 5], comps: ['Pitch', 'Yaw', 'Roll'], axn: ['Pitch', 'Yaw', 'Roll'], dec: 1, unit: '°' },
      { id: 'cam.fov', name: 'Field of view', chans: [6], comps: ['FOV'], axn: ['Field of view'], dec: 1, unit: '' }
    ];
    // A layer names the CONFIG tab as well as the card: both tabs have a Lighting card.
    function sceneLayerTitle(c) {
      return (c.family === 'effects' ? 'EFFECTS' : c.family === 'environment' ? 'ENVIRONMENT' : 'SCENE')
        + (c.group ? ' · ' + ('' + c.group).toUpperCase() : '');
    }
    function buildTree() {
      var out = [], nodes = {};
      var cam = { id: 'cam', kind: 'layer', dom: 'cam', name: 'CAMERA PATH', color: LAYER_COLORS.cam, children: [] };
      var tr = { id: 'cam.tr', kind: 'group', dom: 'cam', name: 'Transform', layer: cam, children: [] };
      cam.children.push(tr);
      CAM_PROPS.forEach(function (p) {
        var n = { id: p.id, kind: 'prop', dom: 'cam', name: p.name, layer: cam, chans: p.chans.map(function (c) { return 'c' + c; }),
                  comps: p.comps, dec: p.dec, unit: p.unit, splittable: p.chans.length > 1, children: [] };
        if (n.splittable && splitState[n.id])
          n.children = n.chans.map(function (u, i) {
            return { id: n.id + '.' + i, kind: 'prop', dom: 'cam', name: p.axn[i], layer: cam, parentProp: n,
                     chans: [u], comps: [p.comps[i]], dec: p.dec, unit: p.unit, children: [] };
          });
        tr.children.push(n);
      });
      // Path > Camera shake: the GTA V shake each camera key carries (CamPathShake). Not a curve:
      // its marks follow the camera keys, and its menu sets style, strength and speed.
      var pg = { id: 'cam.path', kind: 'group', dom: 'cam', name: 'Path', layer: cam, children: [] };
      var shk = { id: 'cam.shake', kind: 'shake', dom: 'cam', name: 'Camera shake', layer: cam, chans: [], comps: [], children: [] };
      // Once a key carries a shake the row twirls open to that shake's own values: the shake that
      // runs under the playhead (from the camera key at or before it), like any property's value.
      if ((st.shakes || []).length) shk.children = [
        { id: 'cam.shake.n', kind: 'shakep', param: 'n', verb: 'intensity', name: 'Intensity', min: 1, max: 10, dom: 'cam', layer: cam, chans: [], comps: [''], children: [] },
        { id: 'cam.shake.p', kind: 'shakep', param: 'p', verb: 'speed', name: 'Speed', min: 10, max: 100, dom: 'cam', layer: cam, chans: [], comps: [''], children: [] }];
      pg.children.push(shk);
      cam.children.push(pg);
      out.push(cam);
      var byGroup = {};
      scCh.forEach(function (c) {
        var gid = c.groupId || ((c.family || '') + '.' + (c.group || ''));
        var L = byGroup[gid];
        if (!L) {
          L = byGroup[gid] = { id: 's:' + gid, kind: 'layer', dom: 'scene', groupId: gid, name: sceneLayerTitle(c),
                               color: LAYER_COLORS[c.family] || LAYER_COLORS.other, children: [] };
          out.push(L);
        }
        L.children.push({ id: 's:' + c.fieldId, kind: 'prop', dom: 'scene', name: c.name, layer: L, chans: [c.uid],
                          comps: [''], fieldId: c.fieldId, type: c.type, children: [] });
      });
      function index(n) { nodes[n.id] = n; n.children.forEach(index); }
      out.forEach(index);
      tree = out; treeNodes = nodes;
    }
    var treeNodes = {};
    function nodeById(id) { return treeNodes[id] || null; }
    function isSplit(n) { return n.kind === 'prop' && !!splitState[n.id] && n.children.length > 0; }
    function isOpen(n) {
      if (n.kind === 'prop') return isSplit(n) && openState[n.id] !== false;
      if (n.kind === 'shake') return n.children.length > 0 && openState[n.id] !== false;
      return openState[n.id] !== false;   // layers and groups start open
    }
    function expandable(n) { return n.kind === 'layer' || n.kind === 'group' || isSplit(n) || (n.kind === 'shake' && n.children.length > 0); }
    // Leaf properties under a node (a split property's dimension rows instead of the property).
    function propsOf(n) {
      if (!n || n.kind === 'shake' || n.kind === 'shakep') return [];
      if (n.kind === 'prop') return isSplit(n) ? n.children : [n];
      var out = []; n.children.forEach(function (c) { out = out.concat(propsOf(c)); }); return out;
    }
    function chansOf(n) {
      var u = [];
      propsOf(n).forEach(function (p) { p.chans.forEach(function (x) { if (u.indexOf(x) < 0) u.push(x); }); });
      return u;
    }
    // Keys of several channels merged by time: one entry per tick, carrying every member key. This is
    // what a Position row (X, Y and Z) and a collapsed layer show.
    function unionKeys(uids) {
      var m = {}, out = [];
      uids.forEach(function (u) {
        var c = chU(u); if (!c) return;
        c.keys.forEach(function (k) {
          var tk = Math.round(k.t * 2) / 2, e = m[tk];
          if (!e) { e = m[tk] = { t: k.t, members: [], sel: false }; out.push(e); }
          e.members.push({ uid: u, id: k.id, k: k });
          if (k.sel) e.sel = true;
        });
      });
      out.sort(function (a, b) { return a.t - b.t; });
      return out;
    }
    function allTimes(n) { return unionKeys(chansOf(n)).map(function (e) { return e.t; }); }
    function layerOn(L) {
      if (L.dom === 'cam') return !!st.drive;
      var any = false;
      for (var i = 0; i < L.children.length; i++) { var c = chU(L.children[i].chans[0]); if (c && !c.muted) any = true; }
      return any;
    }
    function propMuted(p) { var c = p.dom === 'scene' ? chU(p.chans[0]) : null; return !!(c && c.muted); }
    function flatten() {
      var out = [];
      function walk(n, depth) {
        out.push({ node: n, depth: depth });
        if (expandable(n) && isOpen(n)) n.children.forEach(function (c) { walk(c, depth + 1); });
      }
      tree.forEach(function (L) { walk(L, 0); });
      return out;
    }
    function rowPad(r) {
      var n = r.node;
      if (n.kind === 'layer') return 6;
      if (n.kind === 'group') return 24;
      return 10 + r.depth * 18;
    }
    function rowAt(ly) {
      var i = Math.floor((ly - ROWS_Y + rowScroll) / ROW_H);
      return (ly >= ROWS_Y && ly < ROWS_Y + ROWS_H && i >= 0 && i < rows.length) ? rows[i] : null;
    }
    function rowY(r) { return ROWS_Y + r.index * ROW_H - rowScroll; }
)GEJS"
R"GEJS(
    // ---- building the rows ------------------------------------------------------
    // One full-width panel per visible row: the left cell (chevron, swatch, name, value, key
    // navigator or meta + switch) and the track cell where the glyphs are drawn. Rebuilt when the
    // flattened list changes; values and states are refreshed every frame from cached refs.
    function setOpen(n, open) { openState[n.id] = open; rowSig = ''; }
    function selectNode(id) { if (selNode !== id) { selNode = id; graphSig = ''; trackSig = ''; } }
    function rebuildRows() {
      closePickTray(); pickBody.RemoveAndDeleteChildren();
      if (editState) cancelEdit();
      rowsInner.RemoveAndDeleteChildren();
      var flat = flatten();
      rows = [];
      for (var i = 0; i < flat.length; i++) {
        try { rows.push(buildRow(flat[i], i)); }
        catch (rowErr) {
          // One row that cannot build must not take the rows below it with it.
          $.Msg('[timeline] row ' + (flat[i].node && flat[i].node.id) + ' failed: ' + rowErr + String.fromCharCode(10));
          rows.push({ node: flat[i].node, depth: flat[i].depth, index: i, broken: true });
        }
      }
      rowsContentH = rows.length * ROW_H;
      rowsInner.style.width = G.rw + 'px'; rowsInner.style.height = Math.max(ROWS_H, rowsContentH) + 'px';
      clampRowScroll();
      trackSig = '';
    }
    function buildRow(f, index) {
      var n = f.node, r = { node: n, depth: f.depth, index: index };
      var rowP = mk('Panel', rowsInner); rowP.hittest = false;
      rowP.style.position = '0px ' + (index * ROW_H) + 'px 0px';
      rowP.style.width = G.rw + 'px'; rowP.style.height = ROW_H + 'px';
      rowP.style.borderBottom = '1px solid ' + S.rowLine;
      r.rowP = rowP;
      var left = mk('Panel', rowP); left.hittest = false;
      left.style.width = LEFTW + 'px'; left.style.height = ROW_H + 'px'; left.style.flowChildren = 'right';
      left.style.paddingLeft = rowPad(r) + 'px'; left.style.paddingRight = '8px';
      left.style.borderRight = '1px solid ' + S.border;
      r.left = left;
      var chev = mk('Panel', left); chev.style.width = '14px'; chev.style.height = '100%'; chev.style.marginRight = '6px';
      var chevL = lbl(chev, expandable(n) ? (isOpen(n) ? '▾' : '▸') : '', S.hint, 10);
      chevL.hittest = false; chevL.style.horizontalAlign = 'center'; chevL.style.verticalAlign = 'center';
      if (expandable(n)) {
        chev.hittest = true;
        chev.SetPanelEvent('onactivate', function () { setOpen(n, !isOpen(n)); });
      } else chev.hittest = false;
      if (n.kind === 'layer') {
        var sw = mk('Panel', left); sw.hittest = false; sw.style.width = '10px'; sw.style.height = '10px';
        sw.style.borderRadius = '2px'; sw.style.backgroundColor = n.color; sw.style.verticalAlign = 'center';
        sw.style.marginRight = '6px';
      }
      var hot = mk('Panel', left); hot.hittest = true; hot.style.height = '100%';
      hot.style.width = 'fill-parent-flow(1.0)'; hot.style.marginRight = '6px';
      var nm = lbl(hot, n.name, S.value, 12); nm.hittest = false; nm.style.verticalAlign = 'center';
      nm.style.whiteSpace = 'nowrap'; nm.style.textOverflow = 'ellipsis'; nm.style.width = '100%';
      if (n.kind === 'layer') { nm.style.fontWeight = 'bold'; nm.style.letterSpacing = '1px'; }
      else if (n.kind === 'group') { nm.style.fontWeight = 'bold'; nm.style.letterSpacing = '0.5px'; nm.style.color = S.label; }
      hot.SetPanelEvent('onactivate', function () { selectNode(n.id); });
      hot.SetPanelEvent('onmouseover', function () { if (n.name.length > 18) showTip(hot, n.name); });
      hot.SetPanelEvent('onmouseout', function () { hideTip(); });
      r.nm = nm;
      if (n.kind === 'layer') {
        var meta = lbl(left, '', S.muted, 11); meta.hittest = false;
        meta.style.fontFamily = S.fontMono; meta.style.width = '60px'; meta.style.textAlign = 'right';
        meta.style.verticalAlign = 'center';
        r.meta = meta;
        var swc = mk('Panel', left); swc.hittest = true;
        swc.style.width = '34px'; swc.style.height = '18px'; swc.style.verticalAlign = 'center';
        swc.style.marginLeft = '6px'; swc.style.marginRight = '4px'; swc.style.borderRadius = '2px';
        var knob = mk('Panel', swc); knob.hittest = false;
        knob.style.width = '12px'; knob.style.height = '12px'; knob.style.borderRadius = '1px';
        swc.SetPanelEvent('onactivate', function () { toggleLayer(n); });
        swc.SetPanelEvent('onmouseover', function () { showTip(swc, n.dom === 'cam'
          ? 'Camera path on: scrubbing and Space preview the path. Off: the free cam stays yours.'
          : 'Layer on: its keys apply to the world. Off: the keys are kept but not applied.'); });
        swc.SetPanelEvent('onmouseout', function () { hideTip(); });
        r.sw = swc; r.knob = knob; r.swOn = null;
      }
      if (n.kind === 'prop') buildPropCells(r, left);
      else if (n.kind === 'shake') buildShakeCells(r, left);
      else if (n.kind === 'shakep') buildShakeParamCells(r, left);
      var trk = mk('Panel', rowP); trk.hittest = false;
      trk.style.position = LEFTW + 'px 0px 0px';
      trk.style.width = G.tw + 'px'; trk.style.height = ROW_H + 'px'; trk.style.overflow = 'clip clip';
      r.trk = trk;
      r.bg = null;
      return r;
    }
)GEJS"
R"GEJS(
    // The VALUE cell and the << ◇ >> navigator of a property row. A number is scrubby (drag it) and
    // typeable (click it) through the mouse pipe; a colour opens the shared colour wheel; a switch,
    // a list or a sky is typed.
    function buildPropCells(r, left) {
      var n = r.node;
      // A fixed 120px column with its numbers right-aligned inside it (they may run a little past
      // its left edge for three wide components rather than wrap).
      var vcol = mk('Panel', left); vcol.hittest = false;
      vcol.style.width = VAL_W + 'px'; vcol.style.height = '100%'; vcol.style.marginRight = '6px';
      vcol.style.overflow = 'noclip noclip';
      var vb = mk('Panel', vcol); vb.hittest = false;
      vb.style.width = 'fit-children'; vb.style.height = '100%';
      vb.style.flowChildren = 'right'; vb.style.horizontalAlign = 'right';
      r.valBox = vb; r.comps = [];
      var c0 = chU(n.chans[0]);
      if (n.dom === 'scene' && c0 && c0.type === 'colour') {
        var swRgb = [255, 255, 255];
        var chip = mk('Panel', vb); chip.hittest = true;
        chip.style.width = '66px'; chip.style.height = '16px'; chip.style.verticalAlign = 'center';
        chip.style.borderRadius = '3px'; chip.style.border = '1px solid ' + S.border;
        var chipLbl = lbl(chip, '', S.value, 10);
        chipLbl.hittest = false; chipLbl.style.horizontalAlign = 'center'; chipLbl.style.verticalAlign = 'center';
        chipLbl.style.fontWeight = 'bold'; chipLbl.style.fontFamily = S.fontMonodigit;
        var crec = { swatch: chip, swatchLbl: chipLbl, rgb: swRgb, last: '', uid: n.chans[0] };
        crec.paint = function () {
          chip.style.backgroundColor = rgbHex(swRgb);
          var lum = swRgb[0] * 0.2126 + swRgb[1] * 0.7152 + swRgb[2] * 0.0722;
          chipLbl.style.color = lum > 145 ? '#111820ff' : '#ffffffff';
          chipLbl.text = rgbHex(swRgb).substring(1, 7).toUpperCase();
        };
        crec.picker = colorPicker(pickBody, n.name, swRgb, function (value) {
          crec.paint();
          // show() fires onChange once to prime the preview; only a real change may write, or
          // merely OPENING the wheel would drop a keyframe.
          var sig = value[0] + ',' + value[1] + ',' + value[2];
          if (sig === crec.last) return;
          crec.last = sig;
          var live = chU(crec.uid), out = value.slice(0, 3);
          // Alpha is not the wheel's business: carry the property's current alpha through.
          if (live && live.value instanceof Array && live.value.length > 3) out.push(live.value[3]);
          edit('scene');
          rawCmd('mirv_filmmaker scenekeys set ' + n.fieldId + ' ' + out.join(' '));
        }, 1);
        crec.picker.onOpen = function () { placePickTray(chip, n.name); };
        crec.picker.owner.onClose = function () { pickTray.visible = false; pickAnchor = null; };
        chip.SetPanelEvent('onactivate', function () { crec.picker.show(); });
        attachTip(chip, 'Pick a colour for ' + n.name);
        crec.paint();
        r.colour = crec;
      } else {
        for (var i = 0; i < n.comps.length; i++) {
          var wrap = mk('Panel', vb); wrap.hittest = false;
          wrap.style.height = '100%'; wrap.style.width = 'fit-children';
          if (i) wrap.style.marginLeft = '5px';
          var v = lbl(wrap, '-', S.value, n.comps.length > 2 ? 11 : 12); v.hittest = false;
          v.style.fontWeight = 'bold'; v.style.fontFamily = S.fontMonodigit; v.style.verticalAlign = 'center';
          v.style.whiteSpace = 'nowrap'; v.style.borderBottom = '1px dotted #ffffff33';
          r.comps.push({ wrap: wrap, lbl: v, uid: n.chans[Math.min(i, n.chans.length - 1)], text: null });
        }
      }
      var nav = mk('Panel', left); nav.hittest = false;
      nav.style.width = NAV_W + 'px'; nav.style.height = '100%'; nav.style.flowChildren = 'right';
      var showNav = !isSplit(n);
      if (showNav) {
        var navIn = mk('Panel', nav); navIn.hittest = false; navIn.style.flowChildren = 'right';
        navIn.style.horizontalAlign = 'center'; navIn.style.height = '100%';
        r.prev = navBtn(navIn, '◀', 'Previous keyframe', function () { selectNode(n.id); jumpOn(n, -1); });
        r.dia = navBtn(navIn, '◇', 'Add or remove a keyframe at the playhead', function () { selectNode(n.id); toggleKeyAt(n); });
        r.next = navBtn(navIn, '▶', 'Next keyframe', function () { selectNode(n.id); jumpOn(n, 1); });
      }
    }

    // The Camera shake row: the style on the camera key under the playhead, and ◀ ◇ ▶ over the
    // keys that carry one (◇ adds a shake to the key under the playhead, or removes it).
    function buildShakeCells(r, left) {
      var vcol = mk('Panel', left); vcol.hittest = false;
      vcol.style.width = VAL_W + 'px'; vcol.style.height = '100%'; vcol.style.marginRight = '6px';
      var v = lbl(vcol, 'Off', S.value, 12); v.hittest = false;
      v.style.horizontalAlign = 'right'; v.style.verticalAlign = 'center'; v.style.whiteSpace = 'nowrap';
      r.shakeLbl = v;
      var nav = mk('Panel', left); nav.hittest = false;
      nav.style.width = NAV_W + 'px'; nav.style.height = '100%';
      var navIn = mk('Panel', nav); navIn.hittest = false; navIn.style.flowChildren = 'right';
      navIn.style.horizontalAlign = 'center'; navIn.style.height = '100%';
      var times = function () { return (st.shakes || []).map(function (s) { return s.t; }); };
      r.prev = navBtn(navIn, '◀', 'Previous shaken key', function () { var t = nearestTime(times(), st.playhead, -1); if (t != null) seekTo(t); });
      r.dia = navBtn(navIn, '◇', 'Add or remove a shake on the camera key at the playhead', function () {
        var i = pathIndexAt(st.playhead); if (i < 0) return;
        shakeCmd(shakeAt(i) ? 'clear ' + i : 'style ' + i + ' next');
      });
      r.next = navBtn(navIn, '▶', 'Next shaken key', function () { var t = nearestTime(times(), st.playhead, 1); if (t != null) seekTo(t); });
    }
    // Intensity / Speed under Camera shake: scrubbable and typeable like any number; ◀ ▶ walk the
    // shaken keys. They show (and set) the shake running under the playhead.
    function buildShakeParamCells(r, left) {
      var vcol = mk('Panel', left); vcol.hittest = false;
      vcol.style.width = VAL_W + 'px'; vcol.style.height = '100%'; vcol.style.marginRight = '6px';
      var vb = mk('Panel', vcol); vb.hittest = false;
      vb.style.width = 'fit-children'; vb.style.height = '100%'; vb.style.horizontalAlign = 'right';
      var wrap = mk('Panel', vb); wrap.hittest = false; wrap.style.height = '100%'; wrap.style.width = 'fit-children';
      var v = lbl(wrap, '-', S.value, 12); v.hittest = false;
      v.style.fontWeight = 'bold'; v.style.fontFamily = S.fontMonodigit; v.style.verticalAlign = 'center';
      v.style.whiteSpace = 'nowrap'; v.style.borderBottom = '1px dotted #ffffff33';
      r.comps = [{ wrap: wrap, lbl: v, uid: null, text: null }];
      var nav = mk('Panel', left); nav.hittest = false;
      nav.style.width = NAV_W + 'px'; nav.style.height = '100%';
      var navIn = mk('Panel', nav); navIn.hittest = false; navIn.style.flowChildren = 'right';
      navIn.style.horizontalAlign = 'center'; navIn.style.height = '100%';
      var times = function () { return (st.shakes || []).map(function (s) { return s.t; }); };
      r.prev = navBtn(navIn, '◀', 'Previous shaken key', function () { var t = nearestTime(times(), st.playhead, -1); if (t != null) seekTo(t); });
      r.dia = navBtn(navIn, '◇', 'Add a shake key here (it continues the shake running here), or remove it', function () {
        var i = pathIndexAt(st.playhead); if (i < 0) return;
        if (shakeAt(i)) { shakeCmd('clear ' + i); return; }
        shakeKeyFrom(i, null, 0);
      });
      r.next = navBtn(navIn, '▶', 'Next shaken key', function () { var t = nearestTime(times(), st.playhead, 1); if (t != null) seekTo(t); });
    }
)GEJS"
R"GEJS(
    // The camera key whose shake runs at tick t: the last one at or before it (-1 before the first).
    function shakeKeyAt(t) {
      var k = st.pathKeys || [], at = -1;
      for (var i = 0; i < k.length; i++) if (k[i] <= t + 0.5) at = i;
      return at;
    }
    // Shake runs, as CamPathShake_EvalSnapshot plays them: a shake key and the next shake key with
    // the same style are one run across any camera keys between them (a one-shot, o, never
    // chains), and the values glide along CamPathShakeRampMath.h's eased ramp. A shake key with no
    // same-style successor runs to the next camera key and ends there.
    function shakeKeysSorted() { return (st.shakes || []).slice().sort(function (a, b) { return a.i - b.i; }); }
    function shakeRun(i) {
      var ks = shakeKeysSorted();
      for (var q = 0; q < ks.length; q++) if (ks[q].i === i) { var nx = ks[q + 1]; return nx && nx.s === ks[q].s && !ks[q].o ? nx : null; }
      return null;
    }
    function shakeEase(u) { u = Math.max(0, Math.min(1, u)); return u * u * (3 - 2 * u); }
    // The shake key whose run covers tick t, or null.
    function shakeRunningAt(t) {
      var ks = shakeKeysSorted(), pk = st.pathKeys || [], a = null;
      for (var q = 0; q < ks.length; q++) if (ks[q].t <= t + 0.5) a = ks[q];
      if (!a) return null;
      if (shakeRun(a.i)) return a;
      var end = pk[a.i + 1];
      return (end == null || t < end - 0.5) ? a : null;
    }
    function shakeValueAt(param, t) {
      var a = shakeRunningAt(t); if (!a) return null;
      var b = shakeRun(a.i);
      if (!b || !(b.t > a.t)) return a[param];
      return a[param] + (b[param] - a[param]) * shakeEase((t - a.t) / (b.t - a.t));
    }
    // Put a shake on camera key idx that continues the shake running there (or the menu's first),
    // at the values it reads there, with `param` (if given) set to v.
    function shakeKeyFrom(idx, param, v) {
      // Read just before the key: the shake arriving there (key idx itself carries none yet).
      var t = (st.pathKeys || [])[idx] - 1, run = shakeRunningAt(t), styles = st.shakeStyles || [];
      var style = run ? run.s : (styles[0] ? styles[0][0] : -1); if (style < 0) return;
      var n = Math.round(num(shakeValueAt('n', t)) || SHAKE_N0), p = Math.round(num(shakeValueAt('p', t)) || SHAKE_P0);
      if (param === 'n') n = v; else if (param === 'p') p = v;
      shakeCmd('set ' + idx + ' ' + style + ' ' + n + ' ' + p);
    }
    // An edit lands on the camera key under the playhead (keying it if it has no shake yet), else on
    // the key whose shake runs here.
    function shakeEditKey() { var i = pathIndexAt(st.playhead), a = shakeRunningAt(st.playhead); return i >= 0 ? i : (a ? a.i : -1); }
    function shakeParamSet(node, idx, v) {
      v = Math.round(Math.max(node.min, Math.min(node.max, v)));
      if (!isFinite(v) || idx < 0) return;
      if (shakeAt(idx)) shakeCmd(node.verb + ' ' + idx + ' ' + v);
      else shakeKeyFrom(idx, node.param, v);
    }
    function fmtShake(v) { return v == null ? '-' : String(Math.round(v * 10) / 10); }
    function pathIndexAt(t) {
      var k = st.pathKeys || [], want = Math.round(t);
      for (var i = 0; i < k.length; i++) if (Math.round(k[i]) === want) return i;
      return -1;
    }
    function shakeAt(i) { var s = st.shakes || []; for (var j = 0; j < s.length; j++) if (s[j].i === i) return s[j]; return null; }
    function shakeCmd(tail) { rawCmd('mirv_filmmaker pathshake ' + tail); }

    // Per-frame refresh of what the rows SAY: values, the diamond, switches, counts, selection.
    function sceneValueText(c) {
      var v = c.value;
      if (c.type === 'bool') return v ? 'On' : 'Off';
      if (c.type === 'asset') return (v === '' || v == null) ? 'Default' : ('' + v);
      if (v instanceof Array) return v.map(function (x) { return typeof x === 'number' ? String(Math.round(x * 1000) / 1000) : String(x); }).join(' ');
      return typeof v === 'number' ? String(Math.round(v * 1000) / 1000) : String(v || '');
    }
    function camValue(c) { var v = c.keys.length ? sampleChan(c, st.playhead) : c.cur; return num(v); }
    function fmtNum(v, dec) { v = num(v); if (Math.abs(v) < Math.pow(10, -dec) / 2) v = 0; return v.toFixed(dec); }
    function setText(l, t) { if (l && l.text !== t) l.text = t; }
    function setColor(l, c) { if (l && l.__c !== c) { l.__c = c; l.style.color = c; } }
    function updateRows() {
      for (var i = 0; i < rows.length; i++) {
        var r = rows[i], n = r.node; if (r.broken) continue;
        var sel = selNode === n.id;
        var bg = n.kind === 'layer' ? (sel ? S.layerSel : S.layerBg) : (sel ? S.rowSel : 'rgba(0,0,0,0)');
        if (r.bg !== bg) { r.bg = bg; r.rowP.style.backgroundColor = bg; }
        if (n.kind === 'layer') {
          var on = layerOn(n);
          var nk = unionKeys(chansOf(n)).length;
          setText(r.meta, nk + (nk === 1 ? ' key' : ' keys'));
          setColor(r.nm, on ? S.value : S.muted);
          if (r.swOn !== on) {
            r.swOn = on;
            r.sw.style.backgroundColor = on ? S.btnOn : S.btnBg;
            r.sw.style.border = '1px solid ' + (on ? S.accent : S.chip);
            r.knob.style.backgroundColor = on ? S.accent : S.dim;
            r.knob.style.position = (on ? 18 : 2) + 'px 2px 0px';
          }
          continue;
        }
        if (n.kind === 'shake') {
          var pi = pathIndexAt(st.playhead), sk = pi >= 0 ? shakeAt(pi) : null, anyShake = (st.shakes || []).length > 0;
          var running = shakeRunningAt(st.playhead);
          setText(r.shakeLbl, running ? running.l : 'Off');
          setColor(r.shakeLbl, running ? S.value : S.muted);
          setText(r.dia.__lbl, sk ? '◆' : '◇');
          setColor(r.dia.__lbl, sk ? S.accent : (pi >= 0 ? S.hint : S.dim));
          setColor(r.prev.__lbl, anyShake ? S.hint : S.dim); setColor(r.next.__lbl, anyShake ? S.hint : S.dim);
          continue;
        }
        if (n.kind === 'shakep') {
          var spv = shakeValueAt(n.param, st.playhead), anyS = (st.shakes || []).length > 0;
          var onKey = pathIndexAt(st.playhead), keyed = onKey >= 0 && !!shakeAt(onKey);
          if (!(editState && editState.node === n)) setText(r.comps[0].lbl, fmtShake(spv));
          setColor(r.comps[0].lbl, spv != null ? S.value : S.muted); setColor(r.nm, spv != null ? S.value : S.muted);
          setText(r.dia.__lbl, keyed ? '◆' : '◇');
          setColor(r.dia.__lbl, keyed ? S.accent : (onKey >= 0 ? S.hint : S.dim));
          setColor(r.prev.__lbl, anyS ? S.hint : S.dim); setColor(r.next.__lbl, anyS ? S.hint : S.dim);
          continue;
        }
        if (n.kind !== 'prop') continue;
        var muted = propMuted(n);
        setColor(r.nm, muted ? S.muted : S.value);
        if (r.colour) {
          var cc = chU(r.colour.uid), cv = cc && cc.value;
          if (cv instanceof Array && !r.colour.picker.visible()) {
            var moved = false;
            for (var ci = 0; ci < 3; ci++) {
              var nn = Math.max(0, Math.min(255, Math.round(cv[ci])));
              if (r.colour.rgb[ci] !== nn) { r.colour.rgb[ci] = nn; moved = true; }
            }
            // Always re-prime, not only on a move: a chip that never moved would keep last '' and
            // the picker's priming onChange would key the colour just for opening.
            r.colour.last = r.colour.rgb.join(',');
            if (moved) r.colour.paint();
          }
          r.colour.picker.sync();
        }
        for (var k = 0; k < r.comps.length; k++) {
          var cp = r.comps[k], ch = chU(cp.uid); if (!ch) continue;
          if (editState && editState.comp === cp) continue;
          var txt = n.dom === 'cam' ? fmtNum(camValue(ch), n.dec) + n.unit : sceneValueText(ch);
          setText(cp.lbl, txt);
          setColor(cp.lbl, muted ? S.muted : (n.dom === 'cam' ? ch.color : S.value));
        }
        if (r.dia) {
          var at = false, any = false;
          for (var u = 0; u < n.chans.length; u++) { var c2 = chU(n.chans[u]); if (!c2) continue; if (c2.keys.length) any = true; if (keyAtTick(c2, st.playhead)) at = true; }
          setText(r.dia.__lbl, at ? '◆' : '◇');
          setColor(r.dia.__lbl, at ? S.accent : S.hint);
          setColor(r.prev.__lbl, any ? S.hint : S.dim); setColor(r.next.__lbl, any ? S.hint : S.dim);
        }
      }
    }
)GEJS"
R"GEJS(
    // ---- drawing the track side (KEYFRAMES) ------------------------------------
    // Layer rows: the bar from first to last key (drag = slide every key of the layer). Collapsed
    // rows: summary marks. Property rows: one glyph per key time; a key shared by X/Y/Z is one glyph.
    function drawTrack() {
      for (var i = 0; i < rows.length; i++) {
        var r = rows[i], n = r.node; if (r.broken) continue;
        r.trk.RemoveAndDeleteChildren();
        r.hits = []; r.bar = null;
        var y = ROW_H / 2;
        if (n.kind === 'layer') {
          var ks = unionKeys(chansOf(n));
          if (ks.length) {
            var on = layerOn(n), col = rgbOnly(n.color);
            var x0 = tickToX(ks[0].t), x1 = tickToX(ks[ks.length - 1].t);
            r.bar = { x0: x0, x1: Math.max(x0 + 4, x1) };
            // Clamped to the track: Panorama does not reliably clip a child many times wider than
            // its clipping parent, and an off-screen key span drew its bar across the inspector.
            var bx0 = Math.max(-6, x0), bx1 = Math.min(G.tw + 6, Math.max(x0 + 4, x1));
            if (bx1 > bx0) {
              var bar = box(r.trk, bx0, 4, bx1 - bx0, 13, col + (on ? '40' : '18'));
              bar.style.borderRadius = '3px'; bar.style.border = '1px solid ' + col + (on ? 'aa' : '44');
            }
            if (!isOpen(n)) drawSummary(r, ks);
          }
          continue;
        }
        if (n.kind === 'group' || isSplit(n)) { if (!isOpen(n)) drawSummary(r, unionKeys(chansOf(n))); continue; }
        if (n.kind === 'shake') {
          // One mark per shaken camera key, shaped as a hold (the shake runs from that key to the
          // next); clicking one goes to it.
          // Under the marks, a bar for how far each shake runs: to the next shake key of its run, or
          // to the next camera key where it ends.
          var spk = st.pathKeys || [];
          (st.shakes || []).forEach(function (s) {
            var nxs = shakeRun(s.i), endT = nxs ? nxs.t : spk[s.i + 1];
            if (endT == null) endT = view.t1;
            var bx0 = Math.max(-6, tickToX(s.t)), bx1 = Math.min(G.tw + 6, tickToX(endT));
            if (bx1 > bx0) box(r.trk, bx0, y - 2, bx1 - bx0, 4, '#80bdff40');
          });
          (st.shakes || []).forEach(function (s) {
            var sx = tickToX(s.t); if (sx < -GLYPH || sx > G.tw + GLYPH) return;
            glyph(r.trk, ['hold', 'hold'], 10, shakeSel[s.i] ? S.accent : '#80bdffff', S.panelHex, sx, y);
            r.hits.push({ x: sx, entry: { t: s.t, members: [], shake: s } });
          });
          continue;
        }
        if (n.kind === 'shakep') {
          // The value as a line inside the row (min at the bottom), eased between the keys of a run
          // exactly as the camera plays it; a marker on each shake key with its number beside it.
          var vy = function (v) { return ROW_H - 4 - (v - n.min) / (n.max - n.min) * (ROW_H - 8); };
          var pk = st.pathKeys || [];
          (st.shakes || []).forEach(function (s) {
            var nx = shakeRun(s.i);
            if (!nx || !(nx.t > s.t)) return;
            var t0 = s.t, t1 = nx.t, prev = null;
            for (var q = 0; q <= 16; q++) {
              var tq = t0 + (t1 - t0) * q / 16, pt = { x: tickToX(tq), y: vy(s[n.param] + (nx[n.param] - s[n.param]) * shakeEase(q / 16)) };
              if (prev) gSeg(r.trk, prev.x, prev.y, pt.x, pt.y, '#80bdff99', 2);
              prev = pt;
            }
          });
          (st.shakes || []).forEach(function (s) {
            var sx = tickToX(s.t); if (sx < -24 || sx > G.tw + 24) return;
            glyph(r.trk, ['ease', 'ease'], 10, shakeSel[s.i] ? S.accent : '#80bdffff', S.panelHex, sx, vy(s[n.param]));
            var tv = lbl(r.trk, String(s[n.param]), '#80bdffcc', 10); tv.hittest = false;
            tv.style.fontFamily = S.fontMonodigit; tv.style.fontWeight = 'bold';
            tv.style.position = (sx + 7).toFixed(0) + 'px 2px 0px';
            r.hits.push({ x: sx, entry: { t: s.t, members: [], shake: s } });
          });
          continue;
        }
        var keys = unionKeys(n.chans), c0 = chU(n.chans[0]);
        var bg = selNode === n.id ? '#2a271b' : S.panelHex;
        for (var j = 0; j < keys.length; j++) {
          var e = keys[j], x = tickToX(e.t);
          if (x < -GLYPH || x > G.tw + GLYPH) continue;
          var m0 = e.members[0], mc = chU(m0.uid) || c0;
          glyph(r.trk, keySides(mc, m0.k), GLYPH, e.sel ? S.accent : S.glyph, bg, x, y);
          r.hits.push({ x: x, entry: e });
        }
      }
    }
    function drawSummary(r, ks) {
      for (var j = 0; j < ks.length; j++) {
        var x = tickToX(ks[j].t);
        if (x < -8 || x > G.tw + 8) continue;
        dot(r.trk, x, ROW_H / 2, 7, ks[j].sel ? S.accent : S.summary, true);
      }
    }
    function hitTrackKey(r, tx) {
      if (!r || !r.hits) return null;
      var best = null, bd = KF_HIT;
      for (var i = 0; i < r.hits.length; i++) { var d = Math.abs(r.hits[i].x - tx); if (d <= bd) { bd = d; best = r.hits[i].entry; } }
      return best;
    }
    function membersInRect(g) {
      var x0 = Math.min(g.sx, g.cx), x1 = Math.max(g.sx, g.cx), y0 = Math.min(g.sy, g.cy), y1 = Math.max(g.sy, g.cy);
      var out = [];
      for (var i = 0; i < rows.length; i++) {
        var r = rows[i]; if (r.broken || r.node.kind !== 'prop' || isSplit(r.node)) continue;
        var ry0 = rowY(r) - ROWS_Y, ry1 = ry0 + ROW_H;
        if (ry1 < y0 || ry0 > y1) continue;
        var keys = unionKeys(r.node.chans);
        for (var j = 0; j < keys.length; j++) {
          var x = tickToX(keys[j].t);
          if (x >= x0 - 4 && x <= x1 + 4) out = out.concat(keys[j].members);
        }
      }
      return out;
    }
)GEJS"
R"GEJS(
    // ---- the GRAPH view -----------------------------------------------------------
    // The curves of the selected layer, group or property, each scaled to its own range across the
    // whole height. Camera channels keep their C++ fit as a floor; an eased bulge and the selected
    // handles widen it. Scene curves fit what they draw inside their coded range. Ranges refit
    // between gestures and stay FROZEN during one, or the axis would move under the pointer.
    var ranges = {};
    function graphChans() {
      var n = nodeById(selNode) || tree[0];
      var props = propsOf(n).filter(function (p) { return p.chans.some(function (u) { var c = chU(u); return c && c.keys.length; }); });
      if (!props.length) props = propsOf(n).slice(0, 1);
      var u = [];
      props.forEach(function (p) { p.chans.forEach(function (x) { if (u.indexOf(x) < 0 && u.length < 8) u.push(x); }); });
      return u.map(chU).filter(function (c) { return !!c; });
    }
    function band() { return { y0: GRAPH_TOP, h: Math.max(20, ROWS_H - GRAPH_TOP - GRAPH_BOTTOM) }; }
    function rng(c) { return ranges[c.uid] || { lo: c.min, hi: c.max }; }
    function fitCamRange(c) {
      var lo = c.min, hi = c.max, a = lo, b = hi;
      function take(v) { if (isFinite(v)) { if (v < a) a = v; if (v > b) b = v; } }
      var k = c.keys;
      for (var i = 0; i < k.length; i++) {
        if (k[i].sel) {
          if (k[i].cl && k[i].cl.a) take(k[i].v + k[i].cl.dv);
          if (k[i].cr && k[i].cr.a) take(k[i].v + k[i].cr.dv);
        }
        if (i + 1 < k.length && segActive(k[i], k[i + 1])) {
          var P = segPts(k[i], k[i + 1]);
          for (var s = 1; s < 16; s++) take(bez(P.p0y, P.p1y, P.p2y, P.p3y, s / 16));
        }
      }
      if (a >= lo && b <= hi) return { lo: lo, hi: hi };
      var pad = (b - a) * 0.08;
      return { lo: a < lo ? a - pad : lo, hi: b > hi ? b + pad : hi };
    }
    function fitSceneRange(c) {
      var lo = c.min, hi = c.max, full = hi - lo;
      if (!(full > 0) || stepped(c) || c.type === 'colour' || !c.keys.length) return { lo: lo, hi: hi };
      var a = Infinity, b = -Infinity;
      function take(v) { if (!isFinite(v)) return; v = Math.max(lo, Math.min(hi, v)); if (v < a) a = v; if (v > b) b = v; }
      var k = c.keys;
      for (var i = 0; i < k.length; i++) {
        take(k[i].v);
        if (k[i].sel) { take(handleEndpoint(c, k[i], -1).v); take(handleEndpoint(c, k[i], 1).v); }
        if (i + 1 < k.length) { var pts = scenePolyline(c, k[i], k[i + 1]); for (var p = 0; p < pts.length; p++) take(pts[p].v); }
      }
      var span = Math.max(b - a, full * 0.04), pad = span * 0.15, mid = (a + b) / 2;
      var vlo = mid - span / 2 - pad, vhi = mid + span / 2 + pad;
      if (vhi - vlo >= full) return { lo: lo, hi: hi };
      if (vlo < lo) { vhi += lo - vlo; vlo = lo; }
      if (vhi > hi) { vlo -= vhi - hi; vhi = hi; }
      return { lo: vlo, hi: vhi };
    }
    function refitRanges(list) {
      var out = {};
      list.forEach(function (c) { out[c.uid] = c.dom === 'scene' ? fitSceneRange(c) : fitCamRange(c); });
      ranges = out;
    }
    function valToY(v, c) { var r = rng(c), d = r.hi - r.lo; if (Math.abs(d) < 1e-9) d = 1; var b = band(); return b.y0 + (1 - (v - r.lo) / d) * b.h; }
    function yToVal(y, c) { var r = rng(c), d = r.hi - r.lo; if (Math.abs(d) < 1e-9) d = 1; var b = band(); return r.lo + (1 - (y - b.y0) / b.h) * d; }
    function rngSpan(c) { var r = rng(c); return (r.hi - r.lo) || 1; }
    function tickDelta(dx) { return dx / G.cw * (view.t1 - view.t0); }
    function valDelta(dy, c) { return -(dy / band().h) * rngSpan(c); }
    // What a scene segment really evaluates to (mirrors SceneKeyframeTrack's Sample()): a float is
    // clamped to its range, an ANGLE goes the way the key's direction says and wraps.
    function scenePolyline(c, a, b) {
      var steps = c.type === 'angle' ? 24 : 14, out = [], P;
      var delta = b.v - a.v;
      if (c.type === 'angle') {
        delta = ((b.v - a.v) % 360 + 360) % 360;
        if (a.direction === 2 && delta > 0) delta -= 360;
        else if (!a.direction && delta > 180) delta -= 360;
      }
      var bb = { t: b.t, v: a.v + delta, cl: b.cl };
      P = segPts(a, bb);
      var active = segActive(a, b), prev = null;
      for (var s = 0; s <= steps; s++) {
        var u = s / steps;
        var t = active ? bez(P.p0x, P.p1x, P.p2x, P.p3x, u) : a.t + (b.t - a.t) * u;
        var v = active ? bez(P.p0y, P.p1y, P.p2y, P.p3y, u) : a.v + delta * u;
        if (c.type === 'angle') { while (v > c.max) v -= 360; while (v < c.min) v += 360; }
        else v = Math.max(c.min, Math.min(c.max, v));
        out.push({ t: t, v: v, brk: c.type === 'angle' && prev !== null && Math.abs(v - prev) > 180 });
        prev = v;
      }
      return out;
    }
    function keyIndex(c, id) { for (var i = 0; i < c.keys.length; i++) if (c.keys[i].id === id) return i; return -1; }
    function keyById(c, id) { var i = keyIndex(c, id); return i >= 0 ? c.keys[i] : null; }
    function handleEndpoint(c, k, side) {
      var idx = keyIndex(c, k.id);
      var defSpan = (view.t1 - view.t0) * 0.06;
      if (side > 0) {
        var nx = (idx + 1 < c.keys.length) ? c.keys[idx + 1] : null;
        var span = nx ? (nx.t - k.t) : defSpan; var tx = (k.cr && k.cr.a) ? k.cr.tx : 1 / 3;
        var dv = (k.cr && k.cr.a) ? k.cr.dv : 0;
        return { t: k.t + clamp01(tx) * span, v: k.v + dv, span: span };
      }
      var pv = (idx - 1 >= 0) ? c.keys[idx - 1] : null;
      var span2 = pv ? (k.t - pv.t) : defSpan; var tx2 = (k.cl && k.cl.a) ? k.cl.tx : 1 / 3;
      var dv2 = (k.cl && k.cl.a) ? k.cl.dv : 0;
      return { t: k.t - clamp01(tx2) * span2, v: k.v + dv2, span: span2 };
    }
)GEJS"
R"GEJS(
    // A graph line clipped to the visible track first (same reason as the layer bar: Panorama does
    // not reliably clip a child far outside its parent).
    function gSeg(parent, x0, y0, x1, y1, color, thick) {
      var lo = -2, hi = G.tw + 2;
      if ((x0 < lo && x1 < lo) || (x0 > hi && x1 > hi)) return null;
      if (x1 !== x0) {
        var ax = x0, ay = y0, bx = x1, by = y1;
        var f = function (x) { return ay + (by - ay) * (x - ax) / (bx - ax); };
        if (x0 < lo) { y0 = f(lo); x0 = lo; } else if (x0 > hi) { y0 = f(hi); x0 = hi; }
        if (x1 < lo) { y1 = f(lo); x1 = lo; } else if (x1 > hi) { y1 = f(hi); x1 = hi; }
      }
      return lineSeg(parent, x0, y0, x1, y1, color, thick);
    }
    function niceStep(raw) { var p = Math.pow(10, Math.floor(Math.log(Math.max(1, raw)) / Math.LN10)); var n = raw / p; var m = n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10; return Math.max(1, m * p); }
    function drawGraph() {
      gridLayer.RemoveAndDeleteChildren(); curveLayer.RemoveAndDeleteChildren();
      kfLayer.RemoveAndDeleteChildren(); handleLayer.RemoveAndDeleteChildren(); legend.RemoveAndDeleteChildren();
      var list = graphChans(), bd = band();
      graphList = list;
      // Grid: the ruler's seconds as verticals, the snap step when it is coarse enough to see, five
      // horizontals with the first curve's values along the left edge.
      var tk = rulerSteps(), snap = snapStep(), pxPerTick = G.cw / Math.max(1e-9, view.t1 - view.t0);
      // Minor lines AT THE SNAP STEP while snapping, so the grid shows exactly where a key will
      // stick; skipped when they would be denser than a few px.
      if (snap > 0 && snap * pxPerTick >= 4 && (view.t1 - view.t0) / snap <= 140) {
        for (var sm = Math.ceil(view.t0 / snap) * snap; sm <= view.t1; sm += snap) {
          var smx = tickToX(sm); if (smx >= 0 && smx <= G.tw) lineSeg(gridLayer, smx, 0, smx, ROWS_H, S.grid, 1);
        }
      }
      for (var s = Math.ceil(view.t0 / tk.minor) * tk.minor; s <= view.t1; s += tk.minor) {
        var gx = tickToX(s); if (gx < 0 || gx > G.tw) continue;
        var major = Math.abs(s / tk.major - Math.round(s / tk.major)) < 1e-6;
        lineSeg(gridLayer, gx, 0, gx, ROWS_H, major ? S.gridMid : S.grid, 1);
      }
      for (var h = 0; h <= 4; h++) {
        var hy = bd.y0 + h * bd.h / 4;
        lineSeg(gridLayer, 0, hy, G.tw, hy, h === 2 ? S.gridMid : '#ffffff10', 1);
        if (list.length) {
          var ax = list[0], axSpan = Math.abs(rngSpan(ax));
          var dec = axSpan >= 100 ? 0 : axSpan >= 10 ? 1 : axSpan >= 1 ? 2 : 3;
          var vl = lbl(gridLayer, yToVal(hy, ax).toFixed(dec), S.hint, 9); vl.hittest = false;
          vl.style.position = '3px ' + (hy - 12).toFixed(0) + 'px 0px';
        }
      }
      graphEmpty.visible = !list.some(function (c) { return c.keys.length; });
      graphEmpty.text = 'No keys on ' + ((nodeById(selNode) || tree[0] || { name: 'this layer' }).name) + ' yet. Press ◇ on a property to add one at the playhead.';
      for (var i = 0; i < list.length; i++) {
        var c = list[i], col = c.color, k = c.keys;
        var item = mk('Panel', legend); item.hittest = false; item.style.flowChildren = 'right'; item.style.marginRight = '14px';
        var sw = mk('Panel', item); sw.hittest = false; sw.style.width = '10px'; sw.style.height = '10px';
        sw.style.borderRadius = '2px'; sw.style.backgroundColor = col; sw.style.verticalAlign = 'center'; sw.style.marginRight = '6px';
        var ln = lbl(item, legendName(c), S.value, 11); ln.hittest = false; ln.style.whiteSpace = 'nowrap';
        if (!k.length) continue;
        var fy = valToY(k[0].v, c); gSeg(curveLayer, 0, fy, tickToX(k[0].t), fy, col + '55', 1);
        var ly = valToY(k[k.length - 1].v, c); gSeg(curveLayer, tickToX(k[k.length - 1].t), ly, G.tw, ly, col + '55', 1);
        for (var j = 0; j + 1 < k.length; j++) {
          var a = k[j], b = k[j + 1];
          if (stepped(c)) {
            gSeg(curveLayer, tickToX(a.t), valToY(a.v, c), tickToX(b.t), valToY(a.v, c), col, 2);
            gSeg(curveLayer, tickToX(b.t), valToY(a.v, c), tickToX(b.t), valToY(b.v, c), col, 2);
          } else if (c.dom === 'scene' && (c.type === 'float' || c.type === 'angle')) {
            var pts = scenePolyline(c, a, b);
            for (var q = 1; q < pts.length; q++) {
              if (pts[q].brk) continue;
              gSeg(curveLayer, tickToX(pts[q - 1].t), valToY(pts[q - 1].v, c), tickToX(pts[q].t), valToY(pts[q].v, c), col, 2);
            }
          } else if (segActive(a, b)) {
            var P = segPts(a, b), px = null, py = null;
            for (var st2 = 0; st2 <= 14; st2++) {
              var u = st2 / 14, X = tickToX(bez(P.p0x, P.p1x, P.p2x, P.p3x, u)), Y = valToY(bez(P.p0y, P.p1y, P.p2y, P.p3y, u), c);
              if (px !== null) gSeg(curveLayer, px, py, X, Y, col, 2);
              px = X; py = Y;
            }
          } else gSeg(curveLayer, tickToX(a.t), valToY(a.v, c), tickToX(b.t), valToY(b.v, c), col, 2);
        }
        for (var m = 0; m < k.length; m++) {
          var kx = tickToX(k[m].t), ky = valToY(k[m].v, c);
          if (kx < -10 || kx > G.tw + 10) continue;
          var d = dot(kfLayer, kx, ky, 9, k[m].swatch || (k[m].sel ? S.accent : col), true);
          d.style.border = '1px solid ' + S.graphBg;
          if (k[m].sel && !stepped(c)) {
            [-1, 1].forEach(function (side) {
              var e = handleEndpoint(c, k[m], side), hx = tickToX(e.t), hy2 = valToY(e.v, c);
              gSeg(handleLayer, kx, ky, hx, hy2, S.handle, 1);
              dot(handleLayer, hx, hy2, 7, S.handle, false);
            });
          }
        }
      }
    }
    var graphList = [];
    function legendName(c) {
      if (c.dom === 'scene') return c.name;
      var p = CAM_PROPS.filter(function (x) { return x.chans.indexOf(chNum(c.uid)) >= 0; })[0];
      return p ? p.axn[p.chans.indexOf(chNum(c.uid))] : c.name;
    }
    function hitGraphKey(tx, gy) {
      var best = null, bd = KF_HIT * KF_HIT;
      for (var i = 0; i < graphList.length; i++) {
        var c = graphList[i];
        for (var j = 0; j < c.keys.length; j++) {
          var k = c.keys[j], dx = tickToX(k.t) - tx, dy = valToY(k.v, c) - gy, d = dx * dx + dy * dy;
          if (d <= bd) { bd = d; best = { uid: c.uid, id: k.id, t: k.t, k: k }; }
        }
      }
      return best;
    }
    function hitGraphHandle(tx, gy) {
      var best = null, bd = HANDLE_R * HANDLE_R;
      for (var i = 0; i < graphList.length; i++) {
        var c = graphList[i]; if (stepped(c)) continue;
        for (var j = 0; j < c.keys.length; j++) {
          var k = c.keys[j]; if (!k.sel) continue;
          for (var side = -1; side <= 1; side += 2) {
            var e = handleEndpoint(c, k, side), dx = tickToX(e.t) - tx, dy = valToY(e.v, c) - gy, d = dx * dx + dy * dy;
            if (d <= bd) { bd = d; best = { uid: c.uid, id: k.id, side: side, span: e.span }; }
          }
        }
      }
      return best;
    }
    function graphMembersInRect(g) {
      var x0 = Math.min(g.sx, g.cx), x1 = Math.max(g.sx, g.cx), y0 = Math.min(g.sy, g.cy), y1 = Math.max(g.sy, g.cy), out = [];
      graphList.forEach(function (c) {
        c.keys.forEach(function (k) {
          var x = tickToX(k.t), y = valToY(k.v, c);
          if (x >= x0 && x <= x1 && y >= y0 && y <= y1) out.push({ uid: c.uid, id: k.id, k: k });
        });
      });
      return out;
    }
)GEJS"
R"GEJS(
    // ---- selection, keys and edits --------------------------------------------
    // Each model keeps its own selection. Selecting in one domain clears the other unless the
    // gesture adds (Shift), so Delete / ease / drag always mean the keys you can see lit.
    function selectMembers(members, add) {
      var by = { cam: [], scene: [] };
      members.forEach(function (m) { by[domOf(m.uid)].push(m); });
      ['cam', 'scene'].forEach(function (dom) {
        var list = by[dom];
        if (!list.length) { if (!add) domCmd(dom, 'selclear'); return; }
        // One selset is atomic, but a console line has a length limit: a big box selection goes out
        // as a clear plus one additive select per key.
        if (!add && list.length <= 16) { domCmd(dom, 'selset ' + list.map(function (m) { return chNum(m.uid) + ' ' + m.id; }).join(' ')); return; }
        if (!add) domCmd(dom, 'selclear');
        list.forEach(function (m) { domCmd(dom, 'select ' + chNum(m.uid) + ' ' + m.id + ' add'); });
      });
      members.forEach(function (m) { selCache[m.uid + ':' + m.id] = 1; });
      if (!add) setShakeSel([], false);
    }
    function clearSelection() { camCmd('selclear'); sceneCmd('selclear'); selCache = {}; setShakeSel([], false); }
    // Shake keys are selected here, by camera-key index: they live on the camera path's markers,
    // not in either key model. Click, Shift+click and box select them like keys; Delete removes
    // them; dragging an Intensity / Speed marker up or down sets its value.
    var shakeSel = {};
    function setShakeSel(list, add) {
      if (!add) shakeSel = {};
      list.forEach(function (i) { shakeSel[i] = 1; });
      trackSig = '';
    }
    function shakeSelCount() { var n = 0; (st.shakes || []).forEach(function (s) { if (shakeSel[s.i]) n++; }); return n; }
    function shakesInRect(g) {
      var x0 = Math.min(g.sx, g.cx), x1 = Math.max(g.sx, g.cx), y0 = Math.min(g.sy, g.cy), y1 = Math.max(g.sy, g.cy), out = [];
      rows.forEach(function (r) {
        if (r.broken || (r.node.kind !== 'shake' && r.node.kind !== 'shakep')) return;
        var ry0 = rowY(r) - ROWS_Y, ry1 = ry0 + ROW_H;
        if (ry1 < y0 || ry0 > y1) return;
        (st.shakes || []).forEach(function (s) { var x = tickToX(s.t); if (x >= x0 - 4 && x <= x1 + 4 && out.indexOf(s.i) < 0) out.push(s.i); });
      });
      return out;
    }
    function selectedDoms() {
      var d = [];
      if (selCountDom('cam') > 0) d.push('cam');
      if (selCountDom('scene') > 0) d.push('scene');
      return d;
    }
    function selectedDomsOf(members) {
      var d = [];
      members.forEach(function (m) { var x = domOf(m.uid); if (d.indexOf(x) < 0) d.push(x); });
      return d;
    }
    function deleteSelection() {
      selectedDoms().forEach(function (dom) { edit(dom); domCmd(dom, 'delsel'); });
      (st.shakes || []).forEach(function (s) { if (shakeSel[s.i]) shakeCmd('clear ' + s.i); });
      setShakeSel([], false);
    }
    function undo(redo) {
      var can = function (dom) { return dom === 'scene' ? !!(st.scene && (redo ? st.scene.canRedo : st.scene.canUndo)) : !!(redo ? st.canRedo : st.canUndo); };
      var dom = can(lastDom) ? lastDom : (can(lastDom === 'cam' ? 'scene' : 'cam') ? (lastDom === 'cam' ? 'scene' : 'cam') : null);
      if (dom) domCmd(dom, redo ? 'redo' : 'undo');
    }
    // The interpolation strip and the Keyframe Assistant: per domain, on that domain's selection.
    function applyEase(mode) {
      var doms = selectedDoms();
      if (!doms.length) {
        if (mode === 'smooth' || mode === 'linear') { edit('cam'); camCmd(mode); }
        return;
      }
      doms.forEach(function (dom) {
        edit(dom);
        if (mode === 'smooth' || mode === 'linear') domCmd(dom, dom === 'cam' ? mode + ' sel' : mode);
        else domCmd(dom, 'ease ' + mode);
      });
    }
    function selectAll() { camCmd('selall'); sceneCmd('selall'); }
    function seekTo(t) {
      t = Math.max(0, Math.round(t));
      releasePlayhead(t);
      revealTicks(t, t);
    }
    function nearestTime(times, t, dir) {
      var best = null;
      for (var i = 0; i < times.length; i++) {
        var kt = times[i];
        if (dir < 0) { if (kt < t - 0.25 && (best === null || kt > best)) best = kt; }
        else { if (kt > t + 0.25 && (best === null || kt < best)) best = kt; }
      }
      return best;
    }
    function jumpOn(n, dir) {
      var t = nearestTime(allTimes(n), st.playhead, dir);
      if (t != null) seekTo(t);
    }
    // |◀ ▶| on the toolbar: the selected row's keys, or every key when nothing is selected.
    function jumpKey(dir) {
      var n = nodeById(selNode);
      var times = n ? allTimes(n) : unionKeys(camCh.map(function (c) { return c.uid; }).concat(scCh.map(function (c) { return c.uid; }))).map(function (e) { return e.t; });
      var t = nearestTime(times, st.playhead, dir);
      if (t != null) seekTo(t);
    }
    // A camera key is the whole camera: placing the first key at a tick keys every channel there,
    // at the value it already has, exactly like "+ Add". Later edits at that tick re-key one channel.
    function camKeyAt(uid, tick, v) {
      var anyAt = camCh.some(function (c) { return !!keyAtTick(c, tick); });
      if (anyAt) { camCmd('addkey ' + chNum(uid) + ' ' + tick.toFixed(2) + ' ' + v.toFixed(3)); return; }
      camCh.forEach(function (c) {
        var val = c.uid === uid ? v : (c.keys.length ? sampleChan(c, tick) : c.cur);
        camCmd('addkey ' + chNum(c.uid) + ' ' + tick.toFixed(2) + ' ' + num(val).toFixed(3));
      });
    }
    function clampToSpec(ch, v) {
      if (!ch || !isFinite(v)) return v;
      if (typeof ch.min === 'number' && v < ch.min) v = ch.min;
      if (typeof ch.max === 'number' && v > ch.max) v = ch.max;
      return v;
    }
    // inGesture: a scrub already opened the camera edit (one undo step for the whole drag).
    function setPropValue(n, uid, v, inGesture) {
      if (n.dom === 'cam') {
        var tick = snapTick(st.playhead);
        edit('cam'); if (!inGesture) camCmd('editbegin');
        camKeyAt(uid, tick, v);
        if (Math.abs(tick - st.playhead) > 0.25) releasePlayhead(tick);
      } else {
        var c = chU(uid);
        edit('scene');
        rawCmd('mirv_filmmaker scenekeys set ' + n.fieldId + ' ' + (typeof v === 'number' ? clampToSpec(c, v).toFixed(3) : v));
      }
    }
    // ◇ on a row: remove the keys under the playhead, or key every channel of the property with the
    // value you are looking at (the live camera, or the property's own editable value).
    function toggleKeyAt(n) {
      var tick = Math.round(st.playhead), hits = [];
      n.chans.forEach(function (u) { var c = chU(u), k = keyAtTick(c, tick); if (k) hits.push({ uid: u, id: k.id }); });
      if (hits.length) {
        // Select exactly those keys and delete them in one step (one undo entry per domain).
        selectMembers(hits, false);
        selectedDomsOf(hits).forEach(function (dom) { edit(dom); domCmd(dom, 'delsel'); });
        return;
      }
      addKeyAt(n);
    }
    function addKeyAt(n) {
      if (n.dom === 'scene') { edit('scene'); rawCmd('mirv_filmmaker scenekeys graphadd ' + n.fieldId + ' ' + Math.round(snapTick(st.playhead))); return; }
      var tick = snapTick(st.playhead);
      edit('cam'); camCmd('editbegin');
      n.chans.forEach(function (u) { var c = chU(u); var v = (typeof c.cur === 'number' && isFinite(c.cur)) ? c.cur : sampleChan(c, tick); camCmd('addkey ' + chNum(u) + ' ' + tick.toFixed(2) + ' ' + num(v).toFixed(3)); });
    }
    function toggleLayer(L) {
      var on = layerOn(L);
      if (L.dom === 'cam') camCmd('drive ' + (on ? 'off' : 'on'));
      else { edit('scene'); rawCmd('mirv_filmmaker scenekeys mute ' + L.groupId + (on ? ' on' : ' off')); }
    }
    function selectLayerKeys(L) {
      var members = [];
      unionKeys(chansOf(L)).forEach(function (e) { members = members.concat(e.members); });
      if (!members.length) return 0;
      // The camera layer IS every camera key: one selall instead of a select per key.
      if (L.dom === 'cam') { camCmd('selall'); sceneCmd('selclear'); members.forEach(function (m) { selCache[m.uid + ':' + m.id] = 1; }); }
      else selectMembers(members, false);
      return members.length;
    }
    function togglePlay() {
      var camKeys = unionKeys(camCh.map(function (c) { return c.uid; })).length;
      if (st.drive && camKeys >= 2) rawCmd('mirv_filmmaker camtl ' + (st.pathPlaying ? 'pause' : 'playtest'));
      else rawCmd(st.playing ? 'demo_pause' : 'demo_resume');
    }
    function setView(mode) {
      if (mode !== 'keys' && mode !== 'graph') return;
      viewMode = mode; viewHold = 30;
      rawCmd('mirv_filmmaker settings ui editor.timeline.view ' + mode);
      trackSig = ''; graphSig = ''; closeMenu();
    }
)GEJS"
R"GEJS(
    // ---- typing a value ---------------------------------------------------------
    // The number you clicked is where you type: a TextEntry is created INSIDE that number's cell and
    // destroyed on commit, which is also what drops the keyboard focus (a hidden focused entry
    // swallowed Space). Enter or a click away commits.
    var editState = null;   // { entry, comp, node, done }
    function beginCellEdit(r, comp, initial) {
      if (editState) commitEdit();
      var e = $.CreatePanel('TextEntry', comp.wrap, 'TimelineType', {});
      e.hittest = true;
      e.style.width = '52px'; e.style.height = '18px'; e.style.verticalAlign = 'center';
      e.style.fontSize = '12px'; e.style.textAlign = 'right'; e.style.fontWeight = 'bold';
      e.style.fontFamily = S.fontMonodigit;
      e.style.color = S.value; e.style.backgroundColor = '#05070aff';
      e.style.border = '1px solid ' + S.accent; e.style.borderRadius = '2px';
      e.text = initial;
      comp.lbl.visible = false;
      editState = { entry: e, comp: comp, node: r.node, done: false };
      e.SetPanelEvent('oninputsubmit', function () { commitEdit(); });
      e.SetPanelEvent('onblur', function () { commitEdit(); });
      e.SetFocus(); try { e.RaiseSelectAll && e.RaiseSelectAll(); } catch (err) {}
    }
    function finishEdit(apply) {
      if (!editState || editState.done) return;
      var es = editState; es.done = true; editState = null;
      var text = es.entry.text;
      try { if (es.comp.lbl && es.comp.lbl.IsValid && es.comp.lbl.IsValid()) es.comp.lbl.visible = true; } catch (e0) {}
      try { es.entry.DeleteAsync(0); } catch (err) {}   // deletion is what drops the focus grab
      hideTip();
      if (!apply) return;
      if (es.node.kind === 'shakep') {
        var sv = parseFloat(('' + text).replace(',', '.')), si = shakeEditKey();
        if (isFinite(sv) && si >= 0 && shakeValueAt(es.node.param, st.playhead) != null) shakeParamSet(es.node, si, sv);
        return;
      }
      var c = chU(es.comp.uid);
      if (es.node.dom === 'scene' && c && (stepped(c) || c.type === 'colour')) {
        var t = ('' + text).replace(/,/g, ' ').replace(/^\s+|\s+$/g, '');
        if (c.type === 'bool') t = /^(on|true|yes|1)$/i.test(t) ? '1' : '0';
        if (t !== '') setPropValue(es.node, es.comp.uid, t);
        return;
      }
      var v = parseFloat(('' + text).replace(',', '.'));
      if (isFinite(v)) setPropValue(es.node, es.comp.uid, es.node.dom === 'scene' ? v : clampToSpec(c, v));
    }
    function commitEdit() { finishEdit(true); }
    function cancelEdit() { finishEdit(false); }
    function editHit(lx, ly) { return !!editState && overPanel(editState.entry, lx, ly); }
    function openType(r, comp) {
      var c = chU(comp.uid); if (!c) return;
      var init = r.node.dom === 'cam' ? fmtNum(camValue(c), Math.max(1, r.node.dec + 1)) : sceneValueText(c);
      if (r.node.dom === 'scene' && c.type === 'bool') init = c.value ? 'On' : 'Off';
      beginCellEdit(r, comp, init);
    }
    // Which value cell (and which number in it) is under the cursor.
    function valueHit(lx, ly) {
      if (lx >= LEFTW || ly < ROWS_Y || ly >= ROWS_Y + ROWS_H) return null;
      var r = rowAt(ly); if (!r || r.broken || (r.node.kind !== 'prop' && r.node.kind !== 'shakep') || !r.comps || !r.comps.length) return null;
      for (var i = 0; i < r.comps.length; i++) {
        var rc = panelRect(r.comps[i].wrap);
        if (rc && lx >= rc.x0 - 3 && lx <= rc.x1 + 3) return { row: r, comp: r.comps[i] };
      }
      return null;
    }

    // ---- menus --------------------------------------------------------------------
    // One menu engine for the property, layer and keyframe menus, driven off the raw cursor pipe
    // (hover and click are hit-tested here). Items: {kind:'title'|'head'|'sep'} or {id, label,
    // hint (under), sc (shortcut, right), check, danger, disabled, sub:[...], subW, pick, keep,
    // key}. Submenus nest to any depth (Camera Shake > a shake > its strength). An item with a sub
    // opens it on hover; with pick it is ALSO an action on click. key names an item across a
    // rebuild (a shake's id carries its values, which change as you step them).
    var menu = null;   // { levels: [panel per depth], open: [the item at depth i whose sub is levels[i+1]], onPick, rebuild }
    var M_PAD = 5, M_ITEM = 27, M_HINT = 40, M_TITLE = 22, M_HEAD = 44, M_SEP = 9;
    function itemH(it) { return it.kind === 'sep' ? M_SEP : it.kind === 'title' ? M_TITLE : it.kind === 'head' ? M_HEAD : (it.hint ? M_HINT : M_ITEM); }
    function menuOpen() { return !!menu; }
    function dropLevels(from) {
      while (menu.levels.length > from) { var m = menu.levels.pop(); try { m.panel.DeleteAsync(0); } catch (e) {} }
      menu.open.length = Math.max(0, from - 1);
    }
    function closeMenu() {
      if (!menu) return;
      dropLevels(0);
      menu = null;
    }
    function buildMenuPanel(x, y, w, items, level) {
      var p = mk('Panel', dock); p.hittest = false; p.style.zIndex = '' + (26 + level);
      p.style.flowChildren = 'down'; p.style.paddingTop = M_PAD + 'px'; p.style.paddingBottom = M_PAD + 'px';
      p.style.backgroundColor = 'rgba(16,20,26,0.99)'; p.style.border = '1px solid ' + S.border;
      p.style.borderRadius = '5px'; p.style.boxShadow = '#000000aa 0px 6px 18px 0px';
      var h = M_PAD * 2;
      items.forEach(function (it) { h += itemH(it); });
      if (x + w > G.rw - 2) x = G.rw - w - 2; if (x < 2) x = 2;
      if (y + h > DOCK_H - 2) y = DOCK_H - h - 2; if (y < 2) y = 2;
      p.style.position = x.toFixed(0) + 'px ' + y.toFixed(0) + 'px 0px';
      p.style.width = w + 'px'; p.style.height = h + 'px';
      var hasCheck = items.some(function (it) { return it.check != null; });
      var cy = y + M_PAD, rects = [];
      items.forEach(function (it) {
        var ih = itemH(it);
        if (it.kind === 'sep') {
          var sp = mk('Panel', p); sp.hittest = false; sp.style.width = '100%'; sp.style.height = '1px';
          sp.style.marginTop = '4px'; sp.style.marginBottom = '4px'; sp.style.backgroundColor = '#ffffff1a';
        } else if (it.kind === 'title') {
          var th = mk('Panel', p); th.hittest = false; th.style.width = '100%'; th.style.height = M_TITLE + 'px'; th.style.paddingLeft = '11px';
          var tl = lbl(th, it.label, S.hint, 10); tl.hittest = false; tl.style.verticalAlign = 'center';
          tl.style.fontWeight = 'bold'; tl.style.letterSpacing = '1.4px'; tl.style.whiteSpace = 'nowrap';
        } else if (it.kind === 'head') {
          var hd = mk('Panel', p); hd.hittest = false; hd.style.width = (w - 2 * M_PAD) + 'px'; hd.style.height = M_HEAD + 'px';
          hd.style.horizontalAlign = 'center'; hd.style.flowChildren = 'down'; hd.style.padding = '6px 8px 6px 8px';
          var hv = lbl(hd, it.value, S.value, 13); hv.hittest = false; hv.style.fontFamily = S.fontMonodigit; hv.style.whiteSpace = 'nowrap';
          hv.style.marginBottom = '2px';
          var ht = lbl(hd, it.title, S.muted, 11); ht.hittest = false; ht.style.whiteSpace = 'nowrap';
        } else {
          var row = mk('Panel', p); row.hittest = false;
          row.style.width = (w - 2 * M_PAD) + 'px'; row.style.height = ih + 'px'; row.style.horizontalAlign = 'center';
          row.style.borderRadius = '3px'; row.style.flowChildren = 'right'; row.style.paddingLeft = '8px'; row.style.paddingRight = '8px';
          if (hasCheck) {
            var ck = lbl(row, it.check ? '✓' : '', S.accent, 11); ck.hittest = false; ck.style.width = '12px';
            ck.style.verticalAlign = 'center'; ck.style.marginRight = '8px';
          }
          var col = mk('Panel', row); col.hittest = false; col.style.width = 'fill-parent-flow(1.0)';
          col.style.flowChildren = 'down'; col.style.verticalAlign = 'center';
          var il = lbl(col, it.label, it.disabled ? S.dim : (it.danger ? S.dangerText : S.value), 13); il.hittest = false; il.style.whiteSpace = 'nowrap';
          if (it.hint) { var ih2 = lbl(col, it.hint, S.muted, 11); ih2.hittest = false; ih2.style.whiteSpace = 'nowrap'; ih2.style.marginTop = '2px'; }
          var right = it.sub ? '›' : (it.sc || '');
          if (right) { var rl = lbl(row, right, it.sub ? S.hint : S.muted, it.sub ? 12 : 11); rl.hittest = false; rl.style.verticalAlign = 'center'; rl.style.marginLeft = '8px'; }
          rects.push({ x0: x + M_PAD, x1: x + w - M_PAD, y0: cy, y1: cy + ih, it: it, panel: row });
        }
        cy += ih;
      });
      return { panel: p, rects: rects, x: x, y: y, w: w, h: h };
    }
    // `rebuild` (optional) re-lists the items: a pick on an item marked `keep` applies, then the
    // menu reopens in place a few frames later with the new values, down the same submenus
    // (stepping a shake's strength leaves you on that shake).
    var reopenMenu = null;
    function showMenu(lx, ly, w, items, onPick, rebuild) {
      closeMenu(); hideSnapMenu(); hideTip();
      menu = { levels: [buildMenuPanel(lx, ly, w, items, 0)], open: [], onPick: onPick, rebuild: rebuild || null };
    }
    function itemKey(it) { return it.key || it.id; }
    function pumpMenuReopen() {
      if (!reopenMenu || --reopenMenu.frames > 0) return;
      var r = reopenMenu; reopenMenu = null;
      showMenu(r.x, r.y, r.w, r.rebuild(), r.onPick, r.rebuild);
      for (var L = 0; L < r.path.length && menu; L++) {
        var m = menu.levels[L], hit = null;
        for (var i = 0; m && i < m.rects.length; i++) if (itemKey(m.rects[i].it) === r.path[L]) { hit = m.rects[i]; break; }
        if (!hit || !hit.it.sub) break;
        openSub(L, hit);
      }
    }
    function menuRectAt(m, lx, ly) {
      if (!m) return null;
      for (var i = 0; i < m.rects.length; i++) { var r = m.rects[i]; if (lx >= r.x0 && lx <= r.x1 && ly >= r.y0 && ly <= r.y1) return r; }
      return null;
    }
    function inMenuPanel(m, lx, ly) { return !!m && lx >= m.x && lx <= m.x + m.w && ly >= m.y && ly <= m.y + m.h; }
    // Open item r's submenu (r sits in level L), closing anything deeper first.
    function openSub(L, r) {
      if (menu.open[L] === r && menu.levels.length > L + 1) return;
      dropLevels(L + 1);
      var W = r.it.subW || 224, x = r.x1 + 2;
      if (x + W > G.rw - 2) x = r.x0 - W - 2;   // flip left near the edge
      menu.levels.push(buildMenuPanel(x, r.y0 - 6, W, r.it.sub, L + 1));
      menu.open[L] = r;
    }
    // The deepest open menu under the cursor (a submenu draws over its parent) and the item there;
    // r is null over a separator or the padding, which still counts as inside that menu.
    function menuHit(lx, ly) {
      for (var L = menu.levels.length - 1; L >= 0; L--)
        if (inMenuPanel(menu.levels[L], lx, ly)) return { L: L, r: menuRectAt(menu.levels[L], lx, ly) };
      return null;
    }
    function menuHover(lx, ly) {
      if (!menu) return;
      var h = menuHit(lx, ly);
      if (h && h.r) {
        if (h.r.it.sub && !h.r.it.disabled) openSub(h.L, h.r);
        else dropLevels(h.L + 1);
      }
      menu.levels.forEach(function (m, L) {
        m.rects.forEach(function (r) {
          var on = ((h && r === h.r) || menu.open[L] === r) && !r.it.disabled;
          var bg = on ? (r.it.danger ? S.dangerBg : S.btnOn) : 'rgba(0,0,0,0)';
          if (r.__bg !== bg) { r.__bg = bg; r.panel.style.backgroundColor = bg; }
        });
      });
    }
    // A press while a menu is open is consumed: it picks an item, opens a submenu, or (outside
    // every menu) closes it.
    function menuClick(lx, ly) {
      var h = menuHit(lx, ly);
      if (h && !h.r) return;
      var r = h ? h.r : null;
      if (r && r.it.sub && !r.it.pick) { if (!r.it.disabled) openSub(h.L, r); return; }
      var pick = menu.onPick, m = menu, path = [];
      if (r) { for (var i = 0; i < h.L; i++) path.push(itemKey(m.open[i].it)); if (r.it.sub) path.push(itemKey(r.it)); }
      var x = m.levels[0].x, y = m.levels[0].y, w = m.levels[0].w;
      closeMenu();
      if (r && !r.it.disabled && pick) {
        pick(r.it.id);
        if (r.it.keep && m.rebuild) reopenMenu = { frames: 3, x: x, y: y, w: w, rebuild: m.rebuild, onPick: pick, path: path };
      }
    }
)GEJS"
R"GEJS(
    // ---- the three menus ------------------------------------------------------------
    function fmtKeyValue(m) {
      var c = chU(m.uid); if (!c) return '';
      if (m.k.swatch) return ('' + m.k.swatch).toUpperCase();
      if (c.dom === 'cam') { var p = CAM_PROPS.filter(function (x) { return x.chans.indexOf(chNum(m.uid)) >= 0; })[0]; return fmtNum(m.k.v, p ? p.dec : 1) + (p ? p.unit : ''); }
      if (c.type === 'bool') return m.k.v ? 'On' : 'Off';
      return String(Math.round(m.k.v * 1000) / 1000);
    }
    // The shake menu for camera-path key `idx`: every shake on the menu (the Rockstar Editor five,
    // the rest under More shakes), each its own submenu with strength and speed. Clicking a shake
    // puts it on the key; the steps put THAT shake on with the stepped value and keep the menu
    // open on it. Ids are `shk:<pathshake verb + args>`.
    var SHAKE_N0 = 5, SHAKE_P0 = 50;   // CamPathShakeSetting's defaults
    function shakeStyleItem(idx, s, style) {
      var si = style[0], cur = !!s && s.s === si, n = s ? s.n : SHAKE_N0, p = s ? s.p : SHAKE_P0;
      var set = function (nn, pp) { return 'shk:set ' + idx + ' ' + si + ' ' + nn + ' ' + pp; };
      return { id: set(n, p), key: 'style:' + si, label: style[1], check: cur, pick: true, keep: true, subW: 236, sub: [
        { kind: 'head', value: style[1], title: cur ? ('Strength ' + n + ' of 10 · speed ' + p) : 'Click to put it on this key' },
        { kind: 'sep' },
        { id: set(Math.min(10, n + 1), p), label: 'Stronger', hint: 'strength ' + Math.min(10, n + 1) + ' of 10', disabled: cur && n >= 10, keep: true },
        { id: set(Math.max(1, n - 1), p), label: 'Weaker', hint: 'strength ' + Math.max(1, n - 1) + ' of 10', disabled: cur && n <= 1, keep: true },
        { kind: 'sep' },
        { id: set(n, Math.min(100, p + 10)), label: 'Faster', hint: 'speed ' + Math.min(100, p + 10), disabled: cur && p >= 100, keep: true },
        { id: set(n, Math.max(10, p - 10)), label: 'Slower', hint: 'speed ' + Math.max(10, p - 10), disabled: cur && p <= 10, keep: true }
      ] };
    }
    function shakeItems(idx) {
      var s = shakeAt(idx), styles = st.shakeStyles || [], ess = Math.min(styles.length, st.shakeEssentials || 5);
      var items = [
        { kind: 'head', value: s ? s.l : 'No shake', title: s ? ('Strength ' + s.n + ' of 10 · speed ' + s.p) : ('Camera key ' + (idx + 1)) },
        { kind: 'sep' }
      ];
      styles.slice(0, ess).forEach(function (x) { items.push(shakeStyleItem(idx, s, x)); });
      var more = styles.slice(ess);
      if (more.length) items.push({ id: 'shkmore', label: 'More shakes', subW: 210,
        check: !!s && more.some(function (x) { return x[0] === s.s; }),
        sub: more.map(function (x) { return shakeStyleItem(idx, s, x); }) });
      items.push({ kind: 'sep' },
        { id: 'shk:applyall ' + idx, label: 'Use on every camera key', disabled: !s },
        { id: 'shk:clear ' + idx, label: 'Remove shake', danger: true, disabled: !s });
      return items;
    }
    function pickShake(id) { if (('' + id).indexOf('shk:') === 0) { shakeCmd(id.substring(4)); return true; } return false; }
    function showShakeMenu(lx, ly, idx) {
      showMenu(lx, ly, 236, shakeItems(idx), function (id) { pickShake(id); }, function () { return shakeItems(idx); });
    }
    function showKeyMenu(lx, ly, n, entry) {
      var list = unionKeys(n.chans), idx = 0;
      for (var i = 0; i < list.length; i++) if (Math.abs(list[i].t - entry.t) < 0.26) idx = i + 1;
      var m0 = entry.members[0], c0 = chU(m0.uid), cur = easeOfSides(keySides(c0, m0.k)), stepOnly = stepped(c0);
      // Rebuilt after a keep-open shake step so the menu shows the new strength.
      var build = function () {
      var items = [
        { kind: 'head', value: entry.members.map(fmtKeyValue).join(', '),
          title: n.name + ' · keyframe ' + idx + ' of ' + list.length + ' · ' + fmtTime(entry.t) },
        { kind: 'sep' },
        { id: 'delete', label: 'Delete Keyframe', sc: 'Del' },
        { id: 'align', label: 'Align selected keys here', disabled: totalSel() < 2 },
        { kind: 'sep' },
        { id: 'assist', label: 'Keyframe Assistant', disabled: stepOnly, sub: stepOnly ? null : [
          { id: 'inout', label: 'Easy Ease', sc: 'F9', check: cur === 'inout' },
          { id: 'in', label: 'Easy Ease In', sc: 'Shift+F9', check: cur === 'in' },
          { id: 'out', label: 'Easy Ease Out', sc: 'Ctrl+Shift+F9', check: cur === 'out' },
          { kind: 'sep' },
          { id: 'smooth', label: 'Smooth', check: cur === 'smooth' },
          { id: 'linear', label: 'Linear', check: cur === 'linear' }
        ] }
      ];
      // A camera key is a camera-path marker, and a marker can carry a shake.
      var pidx = domOf(m0.uid) === 'cam' ? pathIndexAt(entry.t) : -1;
      if (pidx >= 0) items.push({ id: 'shake', label: 'Camera Shake', subW: 236, sub: shakeItems(pidx) });
      return items;
      };
      showMenu(lx, ly, 230, build(), function (id) {
        if (pickShake(id)) return;
        if (id === 'delete') deleteSelection();
        else if (id === 'align') { edit(domOf(m0.uid)); domCmd(domOf(m0.uid), 'align ' + chNum(m0.uid) + ' ' + m0.id); }
        else applyEase(id);
      }, build);
    }
    function showRowMenu(lx, ly, r) {
      var n = r.node, items = [{ kind: 'title', label: n.name.toUpperCase() }];
      if (n.kind === 'prop') {
        var parent = n.parentProp;
        if (isSplit(n)) items.push({ id: 'join', label: 'Join Dimensions', hint: 'back to one ' + n.name + ' row' });
        else if (parent) items.push({ id: 'joinparent', label: 'Join Dimensions', hint: 'merge into ' + parent.name });
        else if (n.splittable) items.push({ id: 'separate', label: 'Separate Dimensions', hint: n.comps.join(' / ') + ' on their own rows' });
        if (viewMode !== 'graph') items.push({ id: 'graph', label: 'Show in Graph' });
        var at = n.chans.some(function (u) { return !!keyAtTick(chU(u), st.playhead); });
        if (!isSplit(n)) items.push({ id: 'addkey', label: at ? 'Re-key at playhead' : 'Add key at playhead',
          hint: n.dom === 'cam' ? 'the live camera\'s ' + n.name.toLowerCase() : 'the value it has now' });
        if (n.dom === 'scene') {
          var muted = propMuted(n);
          items.push({ kind: 'sep' });
          items.push({ id: 'mute', label: muted ? 'Enable property' : 'Disable property', hint: muted ? 'apply its keys again' : 'keep the keys, stop applying them' });
          items.push({ id: 'clear', label: 'Delete property', danger: true, hint: 'removes it and its keys (undoable)' });
        }
      } else {
        items.push({ id: 'selall', label: 'Select all keys', disabled: !allTimes(n).length });
        if (viewMode !== 'graph') items.push({ id: 'graph', label: 'Show in Graph' });
        if (n.kind === 'layer') {
          var on = layerOn(n);
          items.push({ id: 'toggle', label: on ? 'Turn layer off' : 'Turn layer on',
            hint: n.dom === 'cam' ? (on ? 'scrubbing leaves the free cam alone' : 'scrubbing previews the path') : (on ? 'keys kept, nothing applied' : 'apply its keys again') });
          if (n.dom === 'scene') items.push({ kind: 'sep' }, { id: 'clear', label: 'Delete layer', danger: true, hint: 'removes every property in it (undoable)' });
        }
      }
      showMenu(lx, ly, 236, items, function (id) {
        if (id === 'separate') { splitState[n.id] = true; openState[n.id] = true; treeSig = ''; }
        else if (id === 'join') { delete splitState[n.id]; treeSig = ''; selectNode(n.id); }
        else if (id === 'joinparent') { delete splitState[n.parentProp.id]; treeSig = ''; selectNode(n.parentProp.id); }
        else if (id === 'graph') { selectNode(n.id); setView('graph'); }
        else if (id === 'addkey') addKeyAt(n);
        else if (id === 'mute') { edit('scene'); rawCmd('mirv_filmmaker scenekeys mute ' + n.fieldId + (propMuted(n) ? ' off' : ' on')); }
        else if (id === 'clear') { edit('scene'); rawCmd('mirv_filmmaker scenekeys clear ' + (n.kind === 'layer' ? n.groupId : n.fieldId)); }
        else if (id === 'selall') selectLayerKeys(n);
        else if (id === 'toggle') toggleLayer(n);
      });
    }
    // Keyboard, forwarded by MovieMode while the dock is up (`grapheditor key <name>`).
    function onKey(name) {
      if (name === 'escape') { if (menu) closeMenu(); else if (snapMenuOpen()) hideSnapMenu(); else if (editState) cancelEdit(); return; }
      if (editState) return;   // the text field owns the keyboard
      if (name === 'delete') deleteSelection();
      else if (name === 'undo') undo(false);
      else if (name === 'redo') undo(true);
      else if (name === 'selall') selectAll();
      else if (name === 'easeinout') applyEase('inout');
      else if (name === 'easein') applyEase('in');
      else if (name === 'easeout') applyEase('out');
    }
)GEJS"
R"GEJS(
    // ---- the mouse-driven interaction engine ----------------------------------------
    function curCursor() {
      var lx = (st.mx || 0) / uiscale - dockScreenX, ly = (st.my || 0) / uiscale - dockScreenY;
      return { lx: lx, ly: ly, tx: lx - LEFTW, gy: ly - ROWS_Y };
    }
    function inTrackX(tx) { return tx >= 0 && tx <= G.tw; }
    function rangeTicks() {
      if (gesture && gesture.r0 != null) return { a: gesture.r0, b: gesture.r1 };
      if (!st.rangeActive) return null;
      var a = num(st.rangeIn), b = num(st.rangeOut);
      return b > a ? { a: a, b: b } : null;
    }
    // Hover dressing for the navigator, cached so a per-frame call does not rewrite styles.
    var ovVis = '';
    function setOvVisual(hot, edge) {
      if (gesture && gesture.type === 'ovL') edge = -1; else if (gesture && gesture.type === 'ovR') edge = 1;
      var sig = (hot ? 1 : 0) + '' + edge;
      if (sig === ovVis) return;
      ovVis = sig;
      ovView.style.border = '1px solid ' + (hot || edge ? S.ovViewBorderHot : S.ovViewBorder);
      ovView.style.backgroundColor = hot || edge ? S.ovViewHot : S.ovView;
      ovLeftGrip.style.backgroundColor = edge === -1 ? S.accent : '#303740ff';
      ovRightGrip.style.backgroundColor = edge === 1 ? S.accent : '#303740ff';
    }
    function pressRuler(cc) {
      var rt = rangeTicks();
      if (rt && cc.ly < HEADER_H + OVERVIEW_H + 10) {
        var xa = tickToX(rt.a), xb = tickToX(rt.b);
        if (Math.abs(cc.tx - xa) <= 6) { gesture = { type: 'rangeL', r0: rt.a, r1: rt.b }; return; }
        if (Math.abs(cc.tx - xb) <= 6) { gesture = { type: 'rangeR', r0: rt.a, r1: rt.b }; return; }
        if (cc.tx > xa && cc.tx < xb) { gesture = { type: 'rangeMove', r0: rt.a, r1: rt.b, sx: cc.tx, a0: rt.a, b0: rt.b }; return; }
      }
      gesture = { type: 'ruler', lastTx: cc.tx }; playheadTo(scrubTick(Math.max(PAD, Math.min(PAD + G.cw, cc.tx))));
    }
    function pressTrackKeys(cc) {
      var r = rowAt(cc.ly);
      if (r && !r.broken && (r.node.kind === 'shake' || r.node.kind === 'shakep')) {
        var se = hitTrackKey(r, cc.tx);
        selectNode(r.node.id);
        if (se) {
          var si = se.shake.i;
          if (st.shift) { if (shakeSel[si]) { delete shakeSel[si]; trackSig = ''; } else setShakeSel([si], true); }
          else if (!shakeSel[si]) { camCmd('selclear'); sceneCmd('selclear'); selCache = {}; setShakeSel([si], false); }
          seekTo(se.t);
          if (r.node.kind === 'shakep') gesture = { type: 'shakeKey', node: r.node, i: si, sy: cc.ly, v0: se.shake[r.node.param], last: se.shake[r.node.param], moved: false };
          return;
        }
      }
      if (r && !r.broken && r.node.kind === 'prop' && !isSplit(r.node)) {
        var e = hitTrackKey(r, cc.tx);
        if (e) { pressKeyEntry(cc, r.node, e.members, e.t); return; }
      }
      if (r && !r.broken && r.node.kind === 'layer' && r.bar && cc.tx >= r.bar.x0 - 3 && cc.tx <= r.bar.x1 + 3) {
        selectNode(r.node.id);
        var n = selectLayerKeys(r.node);
        if (n) startKeyDrag(cc, [r.node.dom], allTimes(r.node)[0], false);
        return;
      }
      if (Math.abs(cc.tx - tickToX(st.playhead)) <= 4) { gesture = { type: 'ruler', lastTx: cc.tx }; return; }
      if (r) selectNode(r.node.id);
      gesture = { type: 'rect', sx: cc.tx, sy: cc.gy, cx: cc.tx, cy: cc.gy, graph: false };
    }
    function pressKeyEntry(cc, node, members, t) {
      selectNode(node.id);
      var already = members.every(function (m) { return isSelectedId(m.uid, m.id); });
      if (!already) selectMembers(members, !!st.shift);
      var doms = already ? selectedDoms() : [];
      members.forEach(function (m) { var d = domOf(m.uid); if (doms.indexOf(d) < 0) doms.push(d); });
      startKeyDrag(cc, doms, t, domOf(members[0].uid) === 'scene', members[0]);
      // Pressing a key that is already part of a selection keeps the whole selection for a drag;
      // a plain click (no drag, no Shift) narrows it to this key on release, as in AE.
      if (already && !st.shift) gesture.narrow = members;
    }
    // The edit (one undo step per domain) opens when the drag really starts, not on the press:
    // a click that only selects must not leave an empty step in the history.
    function startKeyDrag(cc, doms, t, seek, lead) {
      gesture ={ type: 'kf', sx: cc.lx, sy: cc.ly, ldt: 0, ldv: 0, doms: doms, leadT0: t, lead: lead || null, graph: viewMode === 'graph' };
      // SCENE KEYS: clicking a key goes to it, and a dragged key carries the playhead with it -- the
      // world is what a scene key changes, so the only way to judge one is to stand on it.
      if (seek) { gesture.seek = true; gesture.ph = Math.round(t); playheadTo(gesture.ph); }
    }
    function pressGraph(cc) {
      var h = hitGraphHandle(cc.tx, cc.gy);
      if (h) { edit(domOf(h.uid)); domCmd(domOf(h.uid), 'editbegin'); gesture = { type: 'handle', uid: h.uid, id: h.id, side: h.side }; return; }
      var k = hitGraphKey(cc.tx, cc.gy);
      if (k) { pressKeyEntry(cc, nodeById(selNode) || tree[0], [{ uid: k.uid, id: k.id, k: k.k }], k.t); return; }
      if (Math.abs(cc.tx - tickToX(st.playhead)) <= 4) { gesture = { type: 'ruler', lastTx: cc.tx }; return; }
      gesture = { type: 'rect', sx: cc.tx, sy: cc.gy, cx: cc.tx, cy: cc.gy, graph: true };
    }
    function updateRect(g) {
      var x0 = Math.min(g.sx, g.cx), x1 = Math.max(g.sx, g.cx), y0 = Math.min(g.sy, g.cy), y1 = Math.max(g.sy, g.cy);
      rectVis.visible = true; rectVis.style.position = x0.toFixed(0) + 'px ' + y0.toFixed(0) + 'px 0px';
      rectVis.style.width = (x1 - x0).toFixed(0) + 'px'; rectVis.style.height = (y1 - y0).toFixed(0) + 'px';
    }
    function rightClick(cc) {
      hideSnapMenu();
      if (cc.ly >= ROWS_Y && cc.ly < ROWS_Y + ROWS_H && cc.lx >= 0 && cc.lx < LEFTW + G.tw) {
        if (cc.lx >= LEFTW && viewMode === 'graph') {
          var gk = hitGraphKey(cc.tx, cc.gy);
          if (gk) {
            if (!isSelectedId(gk.uid, gk.id)) selectMembers([{ uid: gk.uid, id: gk.id }], false);
            var gn = nodeOfChan(gk.uid);
            showKeyMenu(cc.lx, cc.ly, gn, { t: gk.t, members: [{ uid: gk.uid, id: gk.id, k: gk.k }] });
          } else closeMenu();
          return;
        }
        var r = rowAt(cc.ly); if (!r || r.broken) { closeMenu(); return; }
        if (r.node.kind === 'shake' || r.node.kind === 'shakep') {
          // On a mark: that key's shake. Anywhere else on the row: the camera key under the
          // playhead, else the one whose shake runs here.
          var se = cc.lx >= LEFTW ? hitTrackKey(r, cc.tx) : null;
          var sidx = se ? se.shake.i : (pathIndexAt(st.playhead) >= 0 ? pathIndexAt(st.playhead) : shakeKeyAt(st.playhead));
          selectNode(r.node.id);
          if (sidx >= 0) showShakeMenu(cc.lx, cc.ly, sidx);
          else showMenu(cc.lx, cc.ly, 236, [{ kind: 'title', label: 'CAMERA SHAKE' },
            { id: 'none', label: 'Add a camera key first', hint: 'a shake runs from a camera key', disabled: true }], function () {});
          return;
        }
        if (cc.lx >= LEFTW && r.node.kind === 'prop' && !isSplit(r.node)) {
          var e = hitTrackKey(r, cc.tx);
          if (e) {
            selectNode(r.node.id);
            if (!e.members.every(function (m) { return isSelectedId(m.uid, m.id); })) selectMembers(e.members, false);
            showKeyMenu(cc.lx, cc.ly, r.node, e); return;
          }
        }
        selectNode(r.node.id);
        showRowMenu(cc.lx, cc.ly, r);
        return;
      }
      closeMenu();
    }
    function nodeOfChan(uid) {
      var out = null;
      tree.forEach(function (L) { propsOf(L).forEach(function (p) { if (!out && p.chans.length === 1 && p.chans[0] === uid) out = p; }); });
      if (!out) tree.forEach(function (L) { propsOf(L).forEach(function (p) { if (!out && p.chans.indexOf(uid) >= 0) out = p; }); });
      return out || tree[0];
    }
)GEJS"
R"GEJS(
    function processInput() {
      var lmb = !!st.lmb, rmb = !!st.rmb, cc = curCursor();
      var inDock = cc.lx >= 0 && cc.lx <= G.rw && cc.ly >= 0 && cc.ly <= DOCK_H;
      var overOverview = inTrackX(cc.tx) && cc.ly >= HEADER_H && cc.ly < HEADER_H + OVERVIEW_H;
      var overRuler = inTrackX(cc.tx) && cc.ly >= HEADER_H + OVERVIEW_H && cc.ly < ROWS_Y;
      var overRows = cc.ly >= ROWS_Y && cc.ly < ROWS_Y + ROWS_H;
      var overTrack = overRows && inTrackX(cc.tx);
      var ovEdge = 0;
      if (overOverview) { var vr0 = ovViewRect(); if (Math.abs(cc.tx - vr0.x0) <= 6) ovEdge = -1; else if (Math.abs(cc.tx - vr0.x1) <= 6) ovEdge = 1; }
      setOvVisual(overOverview && !ovEdge, ovEdge);
      if (menu) menuHover(cc.lx, cc.ly);
      if (rmb && !prevRmb && inDock && !cfgColorOwner) rightClick(cc);
      prevRmb = rmb;

      if (lmb && !prevLmb) {
        // The colour picker owns the pointer while open; a press outside the tray closes it.
        if (cfgColorOwner) {
          if (!overPanel(pickTray, cc.lx, cc.ly) && !overPanel(pickAnchor, cc.lx, cc.ly)) closePickTray();
          prevLmb = lmb; return;
        }
        if (snapMenuOpen()) {
          if (!(snapRect && cc.lx >= snapRect.x0 && cc.lx <= snapRect.x1 && cc.ly >= snapRect.y0 && cc.ly <= snapRect.y1)) hideSnapMenu();
          prevLmb = lmb; return;
        }
        if (menu) { menuClick(cc.lx, cc.ly); prevLmb = lmb; return; }
        if (editState) { if (editHit(cc.lx, cc.ly)) { prevLmb = lmb; return; } commitEdit(); }
        if (!inDock) { prevLmb = lmb; return; }
        if (overOverview) {
          var vr = ovViewRect(), span = view.t1 - view.t0;
          if (Math.abs(cc.tx - vr.x0) <= 6) gesture = { type: 'ovL' };
          else if (Math.abs(cc.tx - vr.x1) <= 6) gesture = { type: 'ovR' };
          else if (cc.tx > vr.x0 && cc.tx < vr.x1) gesture = { type: 'ovMove', sx: cc.tx, t0: view.t0, t1: view.t1 };
          else {
            var ct = xToDom(cc.tx);
            view.t0 = ct - span / 2; view.t1 = view.t0 + span; normalizeView(); afterViewChange();
            gesture = { type: 'ovMove', sx: cc.tx, t0: view.t0, t1: view.t1 };
          }
        } else if (overRuler) pressRuler(cc);
        else if (overPanel(sbTrack, cc.lx, cc.ly)) {
          var sr = panelRect(sbTrack), d = domainRange(), w = view.t1 - view.t0;
          if (!(cc.lx >= sbThumbX0 && cc.lx <= sbThumbX0 + sbThumbW)) {
            var c2 = d.t0 + (cc.lx - sr.x0) / Math.max(1, sr.x1 - sr.x0) * (d.t1 - d.t0);
            view.t0 = c2 - w / 2; view.t1 = view.t0 + w; normalizeView(); afterViewChange();
          }
          gesture = { type: 'sb', sx: cc.lx, t0: view.t0, t1: view.t1, scale: (d.t1 - d.t0) / Math.max(1, sr.x1 - sr.x0) };
        } else if (overRows && cc.lx >= LEFTW + G.tw && rowsContentH > ROWS_H) {
          gesture = { type: 'rsb', sy: cc.ly, s0: rowScroll };
        } else if (overTrack) {
          if (viewMode === 'graph') pressGraph(cc); else pressTrackKeys(cc);
        } else if (overRows && cc.lx < LEFTW) {
          var vh = valueHit(cc.lx, cc.ly);
          if (vh && vh.row.node.kind === 'shakep') {
            var sp0 = shakeValueAt(vh.row.node.param, st.playhead), sk0 = shakeEditKey();
            selectNode(vh.row.node.id);
            if (sp0 != null && sk0 >= 0) gesture = { type: 'shakeNum', row: vh.row, comp: vh.comp, smx: st.mx, moved: false, idx: sk0,
                                 sv: sp0, last: Math.round(sp0) };
          } else if (vh) {
            var vc = chU(vh.comp.uid), scrub = vc && (vh.row.node.dom === 'cam' || vc.type === 'float' || vc.type === 'angle');
            selectNode(vh.row.node.id);
            gesture = { type: 'num', row: vh.row, comp: vh.comp, smx: st.mx, moved: false, scrub: !!scrub,
                        sv: vc ? (vh.row.node.dom === 'cam' ? camValue(vc) : num(vc.value)) : 0 };
          }
        }
      } else if (lmb && prevLmb && gesture) {
        dragGesture(cc);
      } else if (!lmb && prevLmb && gesture) {
        releaseGesture(cc);
      }
      prevLmb = lmb;
    }
)GEJS"
R"GEJS(
    function dragGesture(cc) {
      var g = gesture;
      if (g.type === 'ovL') { view.t0 = xToDom(cc.tx); normalizeView(); afterViewChange(); }
      else if (g.type === 'ovR') { view.t1 = xToDom(cc.tx); normalizeView(); afterViewChange(); }
      else if (g.type === 'ovMove') {
        var dt0 = xToDom(cc.tx) - xToDom(g.sx);
        view.t0 = g.t0 + dt0; view.t1 = g.t1 + dt0; normalizeView(); afterViewChange();
      } else if (g.type === 'sb') {
        var ds = (cc.lx - g.sx) * g.scale;
        view.t0 = g.t0 + ds; view.t1 = g.t1 + ds; normalizeView(); afterViewChange();
      } else if (g.type === 'rsb') {
        var ratio = rowsContentH / ROWS_H;
        rowScroll = g.s0 + (cc.ly - g.sy) * ratio; clampRowScroll();
      } else if (g.type === 'ruler') {
        // No auto-scroll inside the track. Only past its edge does the view creep, faster the further
        // out (capped), with the playhead pinned to the edge.
        var over = cc.tx > PAD + G.cw ? cc.tx - (PAD + G.cw) : (cc.tx < PAD ? cc.tx - PAD : 0);
        if (over) {
          var w = view.t1 - view.t0, step = (over > 0 ? 1 : -1) * w * Math.min(0.012, Math.abs(over) * 0.00012) * 0.6;
          view.t0 += step; view.t1 += step; normalizeView(); afterViewChange(true);
          playheadTo(snapTick(over > 0 ? view.t1 : view.t0));
        } else playheadTo(scrubTick(cc.tx));
      } else if (g.type === 'rangeL' || g.type === 'rangeR' || g.type === 'rangeMove') {
        var d = domainRange(), t = Math.round(xToTick(cc.tx));
        if (g.type === 'rangeL') g.r0 = Math.max(d.t0, Math.min(g.r1 - 32, t));
        else if (g.type === 'rangeR') g.r1 = Math.min(d.t1, Math.max(g.r0 + 32, t));
        else {
          var dt = Math.round(xToTick(cc.tx) - xToTick(g.sx)), len = g.b0 - g.a0;
          g.r0 = Math.max(d.t0, Math.min(d.t1 - len, g.a0 + dt)); g.r1 = g.r0 + len;
        }
        navSig = '';
      } else if (g.type === 'rect') {
        g.cx = cc.tx; g.cy = cc.gy; updateRect(g);
      } else if (g.type === 'handle') {
        var c = chU(g.uid), k = c ? keyById(c, g.id) : null;
        if (k) {
          var e = handleEndpoint(c, k, g.side), th = xToTick(cc.tx), vh = yToVal(cc.gy, c);
          var span = e.span > 1e-9 ? e.span : 1;
          var tx = clamp01(g.side > 0 ? (th - k.t) / span : (k.t - th) / span), dv = vh - k.v;
          // Shift = STRAIGHTEN: the handle stays level with its key.
          if (st.shift) dv = 0;
          // Keep the handle OUTSIDE the key glyph, or it is swallowed and cannot be grabbed again.
          var spanPx = span / (view.t1 - view.t0) * G.cw, hpx = tx * spanPx, vpx = dv / rngSpan(c) * band().h;
          var dist = Math.sqrt(hpx * hpx + vpx * vpx), MIN = KF_HIT + 8;
          if (dist < MIN) {
            if (dist < 1e-3) { tx = clamp01(MIN / (spanPx || 1)); dv = 0; }
            else { var sc = MIN / dist; tx = clamp01(tx * sc); dv = dv * sc; }
          }
          var side = g.side > 0 ? 'right' : 'left';
          if (domOf(g.uid) === 'cam') camCmd('handle ' + chNum(g.uid) + ' ' + g.id + ' ' + side + ' ' + tx.toFixed(3) + ' ' + dv.toFixed(3) + ' 0');
          else sceneCmd('handle ' + chNum(g.uid) + ' ' + g.id + ' ' + side + ' ' + tx.toFixed(3) + ' ' + dv.toFixed(3));
        }
      } else if (g.type === 'kf') dragKeys(cc);
      else if (g.type === 'shakeKey') {
        var kdy = g.sy - cc.ly, kn = g.node;
        if (!g.moved && Math.abs(kdy) > 3) g.moved = true;
        if (g.moved) {
          var kv = Math.round(Math.max(kn.min, Math.min(kn.max, g.v0 + kdy * (kn.max - kn.min) / 150 * (st.shift ? 0.25 : 1))));
          if (kv !== g.last) { g.last = kv; shakeCmd(kn.verb + ' ' + g.i + ' ' + kv); }
        }
      }
      else if (g.type === 'shakeNum') {
        var sdx = st.mx - g.smx, pn = g.row.node;
        if (!g.moved && Math.abs(sdx) > 4) g.moved = true;
        if (g.moved) {
          var snv = Math.round(Math.max(pn.min, Math.min(pn.max, g.sv + sdx * (pn.max - pn.min) / 200 * (st.shift ? 0.2 : 1))));
          if (snv !== g.last) { g.last = snv; shakeParamSet(pn, g.idx, snv); }
        }
      }
      else if (g.type === 'num') {
        var dpx = st.mx - g.smx;
        if (!g.moved && Math.abs(dpx) > 4) {
          g.moved = true;
          // One undo step for the whole scrub, opened when it really starts.
          if (g.scrub && g.row.node.dom === 'cam') { edit('cam'); camCmd('editbegin'); }
        }
        if (g.moved && g.scrub) {
          var c3 = chU(g.comp.uid); if (!c3) return;
          var sens = ((c3.max - c3.min) / (g.row.node.dom === 'cam' ? 300 : 320)) * (st.shift ? 0.2 : 1);
          var nv = g.sv + dpx * sens;
          if (g.row.node.dom === 'scene') nv = Math.max(c3.min, Math.min(c3.max, nv));
          setPropValue(g.row.node, g.comp.uid, nv, true);
        }
      }
    }
    // A key drag. A CLICK IS NOT A DRAG: until the pointer leaves a few px around the press nothing
    // moves (every camera move bakes the path). Then the LEAD key snaps to the grid and the rest of
    // the selection moves by the same delta; GRAPH also moves values. Camera keys take per-frame
    // steps; scene keys take the TOTAL offset since the press (SceneKeyframeTrack measures from the
    // edit's start), so a step refused at a collision is not lost.
    function dragKeys(cc) {
      var g = gesture;
      if (!g.dragging) {
        var pdx = cc.lx - g.sx, pdy = cc.ly - g.sy;
        if (pdx * pdx + pdy * pdy < KF_DRAG_PX * KF_DRAG_PX) return;
        g.dragging = true;
        g.doms.forEach(function (d) { edit(d); domCmd(d, 'editbegin'); });
      }
      var dt = tickDelta(cc.lx - g.sx), dv = 0;
      var lc = g.graph && g.lead ? chU(g.lead.uid) : null;
      if (lc) {
        dv = valDelta(cc.ly - g.sy, lc);
        // Shift = timing only: freeze the value delta where it was.
        if (st.shift) { if (!g.shiftLock) { g.shiftLock = true; g.lockDv = g.ldv; } dv = g.lockDv; }
        else if (g.shiftLock) { g.shiftLock = false; g.sy = cc.ly + g.lockDv * band().h / rngSpan(lc); dv = g.lockDv; }
      }
      if (g.leadT0 != null && snapStep() > 0) dt = snapTick(g.leadT0 + dt) - g.leadT0;
      if (g.doms.indexOf('scene') >= 0) dt = Math.round(dt);
      if (dt === g.ldt && dv === g.ldv) return;
      g.doms.forEach(function (d) {
        var dvd = (lc && domOf(g.lead.uid) === d) ? dv : 0, ldvd = (lc && domOf(g.lead.uid) === d) ? g.ldv : 0;
        if (d === 'scene') sceneCmd('movesel ' + dt.toFixed(3) + ' ' + dvd.toFixed(3));
        else camCmd('movesel ' + (dt - g.ldt).toFixed(3) + ' ' + (dvd - ldvd).toFixed(3));
      });
      g.ldt = dt; g.ldv = dv;
      if (g.seek && g.lead) {
        var lk = keyById(chU(g.lead.uid) || { keys: [] }, g.lead.id);
        var ph = Math.max(0, Math.round(lk ? lk.t : g.leadT0 + dt));
        if (ph !== g.ph) { g.ph = ph; playheadTo(ph); }
      }
    }
    function releaseGesture(cc) {
      var g = gesture;
      if (g.type === 'rect') {
        rectVis.visible = false;
        var picked = g.graph ? graphMembersInRect(g) : membersInRect(g), pickedShakes = g.graph ? [] : shakesInRect(g);
        if (Math.abs(g.cx - g.sx) < 3 && Math.abs(g.cy - g.sy) < 3) { if (!st.shift) clearSelection(); }
        else if (picked.length || pickedShakes.length) {
          if (picked.length) selectMembers(picked, !!st.shift);
          else if (!st.shift) { camCmd('selclear'); sceneCmd('selclear'); selCache = {}; }
          setShakeSel(pickedShakes, !!st.shift || !!picked.length);
        } else if (!st.shift) clearSelection();
      } else if (g.type === 'ruler') {
        var tx = Math.max(PAD, Math.min(PAD + G.cw, cc.tx));
        releasePlayhead(snapTick(xToTick(tx)));
      } else if (g.type === 'rangeL' || g.type === 'rangeR' || g.type === 'rangeMove') {
        rawCmd('mirv_filmmaker render range custom ' + Math.round(g.r0) + ' ' + Math.round(g.r1));
      } else if (g.type === 'kf') {
        if (g.seek) {
          var lk = g.lead ? keyById(chU(g.lead.uid) || { keys: [] }, g.lead.id) : null;
          releasePlayhead(lk ? lk.t : g.ph);
        }
        if (g.dragging && g.doms.indexOf('scene') >= 0) sceneCmd('editend');
        if (!g.dragging && g.narrow) selectMembers(g.narrow, false);
      } else if (g.type === 'handle') {
        if (domOf(g.uid) === 'scene') sceneCmd('editend');
      } else if (g.type === 'shakeNum') {
        if (!g.moved) beginCellEdit(g.row, g.comp, String(Math.round(g.sv)));
      } else if (g.type === 'num') {
        if (!g.moved) {
          if (g.row.node.dom === 'scene' && !g.scrub) { var c = chU(g.comp.uid); if (c && c.type !== 'colour') openType(g.row, g.comp); }
          else openType(g.row, g.comp);
        }
      }
      gesture = null;
      graphSig = ''; trackSig = '';
    }
    function clampRowScroll() {
      var max = Math.max(0, rowsContentH - ROWS_H);
      if (rowScroll > max) rowScroll = max; if (rowScroll < 0) rowScroll = 0;
    }
)GEJS"
R"GEJS(
    // ---- per-frame layout of the ruler, overview, footer and playhead -------------
    // Ruler ticks adapt to the span: label steps from 1/16 s up to 20 min, roughly 70 px apart.
    var SECOND_STEPS = [0.0625, 0.125, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200];
    function rulerSteps() {
      var r = tps(), pxPerSec = G.cw / Math.max(1e-9, (view.t1 - view.t0) / r), major = SECOND_STEPS[SECOND_STEPS.length - 1];
      for (var i = 0; i < SECOND_STEPS.length; i++) if (SECOND_STEPS[i] * pxPerSec >= 70) { major = SECOND_STEPS[i]; break; }
      var minor = major >= 10 ? major / 5 : (major >= 1 ? major / (major === 2 ? 2 : 5) : major / 2);
      return { major: major * r, minor: Math.max(1, minor * r) };
    }
    function drawRuler() {
      rulerLayer.RemoveAndDeleteChildren();
      var s = rulerSteps();
      for (var t = Math.ceil(view.t0 / s.minor) * s.minor; t <= view.t1; t += s.minor) {
        var x = tickToX(t); if (x < 0 || x > G.tw) continue;
        var major = Math.abs(t / s.major - Math.round(t / s.major)) < 1e-6;
        box(rulerLayer, x, RULER_H - (major ? 10 : 5), 1, major ? 10 : 5, major ? '#ffffff55' : '#ffffff26');
        if (major) {
          var l = lbl(rulerLayer, fmtTimeShort(t), S.hint, 11); l.hittest = false;
          l.style.fontFamily = S.fontMonodigit; l.style.position = (x + 4).toFixed(0) + 'px 9px 0px';
        }
      }
    }
    function drawOverviewKeys() {
      ovKeyLayer.RemoveAndDeleteChildren();
      var e = unionKeys(camCh.map(function (c) { return c.uid; }).concat(scCh.map(function (c) { return c.uid; })));
      for (var i = 0; i < e.length; i++) {
        var x = domToX(e[i].t); if (x < 0 || x > G.tw) continue;
        box(ovKeyLayer, x, 5, 1, 4, e[i].sel ? S.sel : S.ovKey);
      }
    }
    var navSig = '', sbThumbX0 = 0, sbThumbW = 10;
    function layoutNav() {
      var d = domainRange(), domSpan = Math.max(1, d.t1 - d.t0), vr = ovViewRect(), px = domToX(num(st.playhead));
      var rt = rangeTicks();
      var trackW = 220, thumbW = Math.max(10, Math.min(trackW, (view.t1 - view.t0) / domSpan * trackW));
      var frac = Math.max(0, Math.min(1, (view.t0 - d.t0) / domSpan)), thumbX = Math.min(trackW - thumbW, frac * trackW);
      var sbr = panelRect(sbTrack);
      if (sbr) { sbThumbX0 = sbr.x0 + thumbX; sbThumbW = thumbW; }
      var sig = [vr.x0.toFixed(1), vr.x1.toFixed(1), px.toFixed(1), thumbX.toFixed(1), thumbW.toFixed(1), G.tw,
                 rt ? rt.a + '-' + rt.b : '-', view.t0.toFixed(2), view.t1.toFixed(2)].join(',');
      if (sig === navSig) return;
      navSig = sig;
      ovLeftGrip.style.position = Math.max(0, Math.min(G.tw - 6, vr.x0 - 1)).toFixed(1) + 'px 1px 0px';
      ovRightGrip.style.position = Math.max(0, Math.min(G.tw - 6, vr.x1 - 5)).toFixed(1) + 'px 1px 0px';
      ovView.style.position = vr.x0.toFixed(1) + 'px 1px 0px';
      ovView.style.width = Math.max(3, vr.x1 - vr.x0).toFixed(1) + 'px'; ovView.style.height = '12px';
      if (px >= 0 && px <= G.tw) { ovPlay.visible = true; ovPlay.style.position = px.toFixed(1) + 'px 0px 0px'; }
      else ovPlay.visible = false;
      sbThumb.style.position = thumbX.toFixed(1) + 'px 0px 0px'; sbThumb.style.width = thumbW.toFixed(1) + 'px';
      var on = !!rt;
      [ovRange, rangeFill, rangeBar, rangeL, rangeR].forEach(function (p) { p.visible = on; });
      if (on) {
        var ra = domToX(rt.a), rb = domToX(rt.b);
        ovRange.style.position = ra.toFixed(1) + 'px 3px 0px'; ovRange.style.width = Math.max(1, rb - ra).toFixed(1) + 'px'; ovRange.style.height = '8px';
        var xa = tickToX(rt.a), xb = tickToX(rt.b), w = Math.max(2, xb - xa);
        rangeFill.style.position = xa.toFixed(1) + 'px 0px 0px'; rangeFill.style.width = w.toFixed(1) + 'px'; rangeFill.style.height = (RULER_H - 1) + 'px';
        rangeBar.style.position = xa.toFixed(1) + 'px 0px 0px'; rangeBar.style.width = w.toFixed(1) + 'px'; rangeBar.style.height = '8px';
        rangeL.style.position = (xa - 3).toFixed(1) + 'px 0px 0px';
        rangeR.style.position = (xb - 4).toFixed(1) + 'px 0px 0px';
        rangeTxt.text = 'RENDER RANGE  ' + fmtTime(rt.a) + ' – ' + fmtTime(rt.b);
      } else rangeTxt.text = 'RENDER RANGE  whole demo';
      setText(viewTxt, 'Showing ' + fmtTimeShort(view.t0) + ' – ' + fmtTimeShort(view.t1) + ' of ' + fmtTimeShort(d.t1));
      // The ruler's ticks only depend on the window and the width, not on the playhead: during
      // playback this block runs every frame, and rebuilding the ticks there is the expensive part.
      var rsig = view.t0.toFixed(2) + ',' + view.t1.toFixed(2) + ',' + G.tw + ',' + tps();
      if (rsig !== rulerSig) { rulerSig = rsig; drawRuler(); }
    }
    var rulerSig = '';
    function layoutPlayhead() {
      var x = tickToX(num(st.playhead));
      if (x >= 0 && x <= G.tw) {
        phLine.visible = true; phHandle.visible = true;
        phLine.style.position = (LEFTW + x - 1).toFixed(1) + 'px ' + (HEADER_H + OVERVIEW_H + 8) + 'px 0px';
        phHandle.style.position = (LEFTW + x - 6).toFixed(1) + 'px ' + (HEADER_H + OVERVIEW_H + 8) + 'px 0px';
      } else { phLine.visible = false; phHandle.visible = false; }
    }
    function layoutRowsScroll() {
      rowsInner.style.position = '0px ' + (-rowScroll).toFixed(0) + 'px 0px';
      var need = rowsContentH > ROWS_H;
      rowsSbThumb.visible = need;
      if (need) {
        var h = Math.max(24, ROWS_H * ROWS_H / rowsContentH), y = ROWS_Y + (ROWS_H - h) * (rowScroll / (rowsContentH - ROWS_H));
        rowsSbThumb.style.height = h.toFixed(0) + 'px';
        rowsSbThumb.style.position = (G.rw - SB_W + 3) + 'px ' + y.toFixed(0) + 'px 0px';
      }
    }
)GEJS"
R"GEJS(
    function computeGeom() {
      // Measure from the ALREADY-laid-out HUD context first: a fresh root reports 0 for the first
      // layout passes after a build or a resolution switch.
      uiscale = root.actualuiscale_x || ctx.actualuiscale_x || 1;
      var rawW = root.actuallayoutwidth || 0, rawH = root.actuallayoutheight || 0;
      if (rawW < 16) { rawW = ctx.actuallayoutwidth || 0; rawH = ctx.actuallayoutheight || 0; }
      if (rawW < 16) return G != null;
      var scrW = rawW / uiscale;
      var scrH = (rawH > 16 ? rawH : rawW * 0.5625) / uiscale;
      var dockW = Math.floor(Math.max(560, (st && st.renderOpen) ? scrW : scrW - graphInspectorW(scrW)));
      dockScreenX = 0; dockScreenY = Math.max(0, scrH - DOCK_H - BOTTOM_LIFT);
      var tw = Math.max(120, dockW - LEFTW - SB_W);
      var changed = !G || G.rw !== dockW;
      G = { rw: dockW, tw: tw, cw: Math.max(60, tw - 2 * PAD) };
      if (!changed) return true;
      dock.style.width = dockW + 'px'; dock.style.height = DOCK_H + 'px'; dock.style.marginBottom = BOTTOM_LIFT + 'px';
      header.style.width = dockW + 'px';
      overview.style.position = LEFTW + 'px ' + HEADER_H + 'px 0px'; overview.style.width = tw + 'px'; overview.style.height = OVERVIEW_H + 'px';
      ovTrack.style.position = '0px 3px 0px'; ovTrack.style.width = tw + 'px'; ovTrack.style.height = '8px';
      ruler.style.position = LEFTW + 'px ' + (HEADER_H + OVERVIEW_H) + 'px 0px'; ruler.style.width = tw + 'px'; ruler.style.height = RULER_H + 'px';
      rowsView.style.position = '0px ' + ROWS_Y + 'px 0px'; rowsView.style.width = dockW + 'px'; rowsView.style.height = ROWS_H + 'px';
      graph.style.position = LEFTW + 'px ' + ROWS_Y + 'px 0px'; graph.style.width = tw + 'px'; graph.style.height = ROWS_H + 'px';
      selOverlay.style.position = LEFTW + 'px ' + ROWS_Y + 'px 0px'; selOverlay.style.width = tw + 'px'; selOverlay.style.height = ROWS_H + 'px';
      selOverlay.style.overflow = 'clip clip';
      phLine.style.height = (DOCK_H - FOOTER_H - (HEADER_H + OVERVIEW_H + 8)) + 'px';
      footer.style.position = '0px ' + (DOCK_H - FOOTER_H) + 'px 0px'; footer.style.width = dockW + 'px'; footer.style.height = FOOTER_H + 'px';
      emptyHint.style.position = (LEFTW + 24) + 'px ' + (ROWS_Y + 60) + 'px 0px';
      // A narrow dock (a wide inspector) keeps the ease strip's glyphs and drops its words.
      var narrow = dockW < 1180;
      EASE_ITEMS.forEach(function (it) { easeBtns[it[0]].__lbl.visible = !narrow; easeBtns[it[0]].__gw.style.marginRight = narrow ? '0px' : '6px'; });
      rowSig = ''; trackSig = ''; graphSig = ''; navSig = '';
      return true;
    }

    // ---- the frame ----------------------------------------------------------------
    var api = {};
    // Wheel, forwarded by CameraEditorJs's api.wheel while the cursor is over this dock. Over the
    // dock the wheel is ours and consumed: Ctrl = zoom about the cursor, Shift = pan time, plain =
    // scroll the layers.
    api.wheel = function (notches) {
      var n = parseInt(notches, 10);
      if (!isFinite(n) || !n || !st || !G || !st.enabled) return false;
      var c = curCursor();
      if (c.lx < 0 || c.lx > G.rw || c.ly < 0 || c.ly > DOCK_H) return false;
      if (st.ctrl) {
        var anchor = inTrackX(c.tx) ? xToTick(c.tx) : (view.t0 + view.t1) * 0.5;
        zoomAt(n > 0 ? (1 / 1.25) : 1.25, anchor);
        return true;
      }
      if (st.shift) { panByFrac(n > 0 ? -0.1 : 0.1); return true; }
      var was = rowScroll;
      rowScroll -= n * ROW_H * 3; clampRowScroll();
      if (rowScroll !== was) trackSig = '';
      return true;   // consumed: over the dock the wheel is ours even when there is nothing to scroll
    };
    // THE COLUMN IS BEING DRAGGED RIGHT NOW, at this width (CONFIG's and the CAMERA EDITOR's grip).
    api.setInspW = function (w) {
      var n = parseInt(w, 10);
      graphInspWOverride = (isFinite(n) && n > 0) ? n : 0;
    };
    api.key = function (name) { if (st && st.enabled) onKey('' + name); };
    // What the shared ribbon says on the right while TIMELINE is up.
    api.ribbonMeta = function () {
      if (!st || !tree.length) return '';
      var keys = unionKeys(camCh.map(function (c) { return c.uid; })).length;
      scCh.forEach(function (c) { keys += c.keys.length; });
      return tree.length + (tree.length === 1 ? ' layer' : ' layers') + ' · ' + keys + ' keys';
    };
)GEJS"
R"GEJS(
    api.render = function () {
      var raw = root.GetAttributeString('state', ''); if (!raw) { root.visible = false; return; }
      try { st = JSON.parse(raw); } catch (e) { return; }
      // A live drag outranks the published width, and hands back once state has caught up.
      if (graphInspWOverride > 0) {
        graphInspW = graphInspWOverride;
        if (typeof st.inspW === 'number' && Math.abs(st.inspW - graphInspWOverride) < 1) graphInspWOverride = 0;
      } else if (typeof st.inspW === 'number' && st.inspW > 0) {
        graphInspW = st.inspW;
      }
      S.accent = st.renderOpen ? S.renderAccent : S.cameraAccent;
      S.btnOn = st.renderOpen ? '#aeb4bc26' : '#f0b32333';
      var shown = !!st.enabled;   // RAW: never delay a hide, see moDockOpenIsReal
      var realOpen = moDockOpenIsReal(shown);
      root.visible = shown;
      if (shown !== moDockSeen) { moDockSeen = shown; if (shown && realOpen) moDockIn(root, 14); }
      if (!st.enabled) return;

      // The two models, tagged with their domain and id.
      camCh = st.channels || []; scCh = (st.scene && st.scene.channels) || []; chMap = {}; selCache = {};
      camCh.forEach(function (c) { c.dom = 'cam'; c.uid = 'c' + c.ch; chMap[c.uid] = c; });
      scCh.forEach(function (c) { c.dom = 'scene'; c.uid = 's' + c.ch; chMap[c.uid] = c; });
      camCh.concat(scCh).forEach(function (c) { c.keys.forEach(function (k) { if (k.sel) selCache[c.uid + ':' + k.id] = 1; }); });

      if (!computeGeom() || !G) { root.visible = false; return; }
      if (view.mode !== 'timeline' || (st.viewEpoch != null && view.epoch !== st.viewEpoch)) { seedView('timeline'); ranges = {}; }
      revealNewKeys();
      if (viewHold > 0) viewHold--;
      else if (st.timelineView === 'graph' || st.timelineView === 'keys') { if (viewMode !== st.timelineView) { viewMode = st.timelineView; trackSig = ''; graphSig = ''; } }

      // Structure: the tree follows the SET of scene properties and the splits; the rows follow the
      // tree and the open state.
      var tsig = scCh.map(function (c) { return c.fieldId; }).join('|') + '#' + Object.keys(splitState).join(',')
        + '#' + ((st.shakes || []).length ? 'shake' : '');
      if (tsig !== treeSig) { treeSig = tsig; buildTree(); rowSig = ''; if (!nodeById(selNode)) selNode = ''; }
      var rsig = treeSig + '@' + flatten().map(function (f) { return f.node.id; }).join(',') + '@' + G.rw;
      if (rsig !== rowSig) { rowSig = rsig; rebuildRows(); }

      // The colour popover runs off the same raw cursor pipe; one pump per frame drags the wheel.
      try { cfgColorPump({ cursor: true, lmb: !!st.lmb, mx: st.mx, my: st.my }); }
      catch (ecp) { $.Msg('[timeline] colour pump failed: ' + ecp + String.fromCharCode(10)); }
      if (pickTray.visible && pickAnchor) placePickTray(pickAnchor, pickTitle);
      processInput();
      pumpMenuReopen();
      followPlayback();
      if (typeof st.timescale === 'number') speedScrub.setSpeed(st.timescale);
      speedScrub.pump(st.mx || 0, st.my || 0, !!st.lmb);

      // ---- toolbar ----
      setText(tcLbl, fmtTime(num(st.playhead)));
      setText(tickLbl, 'tick ' + Math.round(num(st.playhead)) + ' · ' + Math.round(tps()) + ' tps');
      styleTab(keysTab, viewMode === 'keys'); styleTab(graphTab, viewMode === 'graph');
      var playingNow = (st.drive && st.pathPlaying) || st.playing;
      setText(playBtn.__lbl, playingNow ? 'II' : '▶'); btnSel(playBtn, playingNow);
      btnSel(cacheBtn, !!st.scrubCache);
      var nSel = totalSel(), lead = null;
      for (var li = 0; li < camCh.length + scCh.length && !lead; li++) {
        var lc = li < camCh.length ? camCh[li] : scCh[li - camCh.length];
        for (var lk = 0; lk < lc.keys.length; lk++) if (lc.keys[lk].sel) { lead = { c: lc, k: lc.keys[lk] }; break; }
      }
      var cur = lead ? easeOfSides(keySides(lead.c, lead.k)) : (st.interp === 'Linear' ? 'linear' : 'smooth');
      EASE_ITEMS.forEach(function (it) {
        var b = easeBtns[it[0]], en = nSel > 0 ? !(lead && stepped(lead.c)) : (it[0] === 'smooth' || it[0] === 'linear');
        var on = en && it[0] === cur;
        if (b.__on !== on || b.__en !== en) { b.__on = on; b.__en = en; paintEase(b); }
      });
      var nDel = nSel + shakeSelCount();
      if (nDel !== delBtn.__n) {
        delBtn.__n = nDel;
        delBtn.__lbl.style.color = nDel ? S.dangerText : S.dim;
        delBtn.style.backgroundColor = nDel ? S.dangerBg : S.btnBg;
        delBtn.style.border = '1px solid ' + (nDel ? S.dangerBorder : S.border);
      }
      var cu = !!(st.canUndo || (st.scene && st.scene.canUndo)), cr = !!(st.canRedo || (st.scene && st.scene.canRedo));
      setColor(undoBtn.__lbl, cu ? S.value : S.dim); setColor(redoBtn.__lbl, cr ? S.value : S.dim);
      btnSel(snapBtn, snapSetting() !== 'off'); setText(snapBtn.__lbl, snapLabel());
      if (snapMenuOpen()) refreshSnapRows();
      setText(selInfo, selectionInfo(nSel, lead));

      updateRows();
      var hasRows = rows.length > 1 || scCh.length > 0;
      emptyHint.visible = !hasRows && viewMode === 'keys';
      emptyHint.text = 'Turn properties on in SCENE KEYS and they get their own layers here.';

      // The expensive drawing only when something it reads changed.
      graph.visible = viewMode === 'graph';
      var ksig = [st.rev, st.scene ? st.scene.rev : 0, view.t0, view.t1, G.rw, selNode, rowSig, viewMode,
                  (st.shakes || []).map(function (z) { return z.i + '.' + z.s + '.' + z.n + '.' + z.p + (shakeSel[z.i] ? '*' : ''); }).join(',')].join(':');
      if (viewMode === 'keys' && ksig !== trackSig) { trackSig = ksig; drawTrack(); }
      var gsig = ksig + ':' + snapSetting();
      if (viewMode === 'graph' && gsig !== graphSig) {
        graphSig = gsig;
        if (!gesture) refitRanges(graphChans());
        drawGraph();
      }
      var osig = [st.rev, st.scene ? st.scene.rev : 0, G.tw, num(st.domainT0), num(st.domainT1)].join(':');
      if (osig !== ovSig) { ovSig = osig; drawOverviewKeys(); }
      layoutNav();
      layoutRowsScroll();
      layoutPlayhead();
    };
    var ovSig = '';
    function selectionInfo(nSel, lead) {
      if (lead && nSel === 1) {
        var n = nodeOfChan(lead.c.uid), list = unionKeys(n.chans), idx = 0;
        for (var i = 0; i < list.length; i++) if (Math.abs(list[i].t - lead.k.t) < 0.26) idx = i + 1;
        return n.layer.name + '  ›  ' + n.name + '  ·  key ' + idx + ' of ' + list.length + '  ·  ' + fmtTime(lead.k.t);
      }
      if (nSel > 1) return nSel + ' keys selected  ·  drag to move  ·  F9 eases  ·  Del deletes';
      var nShk = shakeSelCount();
      if (nShk) return nShk + (nShk === 1 ? ' shake key' : ' shake keys') + ' selected  ·  drag Intensity / Speed up or down  ·  Del removes';
      var sn = nodeById(selNode);
      if (sn) return (sn.layer ? sn.layer.name + '  ›  ' : '') + sn.name + '  ·  click a key to select it, right-click for options';
      return 'Click a key to select it  ·  drag to move  ·  right-click for options';
    }

    $.GraphExp = api;
    api.render();
    $.Msg('[timeline] unified timeline built.\n');
  } catch (err) {
    $.Msg('[timeline] gui error: ' + err + '\n');
  }
})();
)GEJS";

} // namespace Filmmaker
