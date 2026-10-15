#pragma once

// Panorama JS for the EXPERIMENTAL After-Effects-style camera GRAPH EDITOR. Built ONCE into
// the in-game HUD (CSGOHud) context by GraphEditorExperimentHud.cpp. Default OFF; appears only
// when the experiment is toggled on, and is fully torn down (DeleteAsync) when toggled off, so
// the regular timeline/editor are never touched.
//
// Layout (AE-like): LEFT channel list (per-channel show/solo + a scrubby/typed value field),
// TOP time ruler/scrubber, RIGHT graph canvas (grid + per-channel curves + keyframe diamonds +
// Bezier handles + drag-select rectangle + playhead).
//
// Dragging is driven by the streamed mouse (state.mx/my/lmb/seq from the WndProc) since Panorama
// has no mouse-move event: a full-screen catcher consumes the clicks, and this script interprets
// the raw cursor against the canvas to implement keyframe drag, handle drag, drag-select, ruler
// scrub, and AE scrubby number fields. All edits issue "mirv_filmmaker grapheditor ..." commands;
// C++ owns the authoritative model and pushes it back as "state".

namespace Filmmaker {

inline const char* kGraphEditorJs = R"GEJS(
(function () {
  try {
    var existing = $('#GraphExpRoot'); if (existing) existing.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    var S = {
      bg: 'rgba(8,10,14,0.55)', panel: 'rgba(16,20,26,0.98)', border: '#ffffff1f',
      grid: '#ffffff10', gridMid: '#ffffff20', label: '#c8d3dfff', value: '#eef2f6ff',
      // dim = DISABLED CONTROLS ONLY (del/undo/redo when there is nothing to act on).
      // inactive = de-emphasized STATE -- the channel rows below are the reason it exists: a
      // channel's name/value has to stay legible while it is NOT the edit target, because
      // reading it is how you pick the next one.
      // hint = STATIC supporting text: the CHANNELS/SELECTION headers, the shortcut hint line,
      // and the ruler/grid tick numbers. Tier table: CameraEditorJs.h.
      dim: '#6b7480ff', inactive: '#afb9c6ff', hint: '#afb9c6ff',
      accent: '#f0b323ff', cameraAccent: '#f0b323ff',
      // This editor draws the playhead AND hosts a destructive button. It used to paint the
      // Delete Key label with S.playhead, so one red meant both "you are here" and "this
      // deletes your keys" on a single surface. Destructive text gets its own token.
      dangerText: '#fc9994ff',
      renderAccent: '#aeb4bcff', sel: '#ffffffff', playhead: '#ff5a5aee',
      btnBg: '#ffffff14', btnOn: '#f0b32333', handle: '#ffffffcc',
      // Interaction tiers, matching CameraTimelineJs.h. This Panorama context has no :hover and
      // no stylesheet of ours, so every state is applied from panel events -- without these the
      // graph's buttons were literally inert: no hover, no press flash, no selected state, so a
      // click on Smooth/Linear gave zero feedback and you could not tell it had registered.
      btnHover: '#ffffff26', btnDown: '#ffffff3d', btnOnHover: '#f0b32352',
      dangerBg: '#c92a2a26', dangerBorder: '#c92a2a66', // destructive-button tint (matches CameraEditorJs)
      rowOn: '#ffffff0c',
      // Timeline overview / navigator (AE's navigation bar). The track is the WHOLE demo and the
      // viewport is the slice the ruler below is showing. Both stay quiet at rest -- a navigator
      // has to read as a map, not as one more control -- so the viewport only lifts on hover/drag.
      ovTrack: 'rgba(255,255,255,0.05)', ovView: '#ffffff20', ovViewHot: '#ffffff33',
      ovViewBorder: '#ffffff4d', ovViewBorderHot: '#f0b323cc', ovKey: '#ffffff66', ovPlay: '#ff5a5aee',
      // Tokens the SHARED colour popover reads (ConfigColorPickerJs.h, spliced in below) so this
      // dock's colour properties open the SAME wheel the Config panel's custom colours do.
      muted: '#8b95a3ff', danger: '#ff6b6bff',
      fontMonodigit: 'Stratum2 Bold Monodigit, Stratum2, "Arial Unicode MS"',
      font: 'Stratum2, "Arial Unicode MS"'
    };
    // LEFTW is the property / track column, and it is wide enough for the property name AND the
    // keyframe navigator (<< diamond >>) that now closes every row -- the AE arrangement, where the
    // navigator sits between the layer name and the timeline rather than floating in a toolbar.
    var LEFTW = 320, HEADER_H = 64, OVERVIEW_H = 30, RULER_H = 26, FOOTER_H = 30;
    var KF_R = 8, HANDLE_R = 7; // hit radii (layout px)
    // Scene-mode value axis: a fixed gutter down the left of the canvas so every strip's three
    // numbers share one right edge, and half a line height so each sits ON its rule.
    var AXIS_W = 46, AXIS_HALF = 6;
    // Docked layout: this editor lives in the bottom curve-editor zone -- left of the camera
    // editor's inspector and above the compact timeline scrub bar -- it is NOT full screen.
    // The operator-dragged inspector width, published by C++ in st.inspW (InspectorWidth.h).
    // INSPECTOR_W is only the value to use before the first state arrives; it used to be a
    // hand-synced copy of the editor's, and it is what sized the graph dock -- so dragging the
    // column left the graph at the old edge.
    var INSPECTOR_W = 640;
    var graphInspW = INSPECTOR_W;
    // Pushed in by the panel being dragged right now; see api.setInspW. The stored width only
    // moves on release, so during the gesture this is the only current source.
    var graphInspWOverride = 0;
    function graphInspectorW(screenW) {
      var w = graphInspW > 0 ? graphInspW : INSPECTOR_W;
      // Same floor and screen cap as InspectorWidth.h, so the graph dock, the column and the
      // camera timeline all agree on where the edge is.
      if (screenW > 1) w = Math.min(w, Math.max(600, screenW * 0.62));
      return Math.floor(Math.max(600, w));
    }
    var DOCK_H = 380;      // compact shared height (timeline is hidden while we're open);
                           // keep in sync with CameraEditorJs GRAPH_DOCK_H
    // 0 = FLUSH with the bottom of the screen. This used to be 28, to sit above CS2's bottom-left
    // build/version string -- but that reserved a 28px band of bare game render under the dock,
    // which reads as the editor not quite fitting rather than as deliberate clearance. The build
    // string is engine debug text; a movie-making workspace is entitled to cover it. Keep in sync
    // with CameraEditorJs BOTTOM_LIFT and CameraTimelineJs EDITOR_BOTTOM_LIFT -- all three docks
    // share this edge and a mismatch makes them jump as you switch between them.
    var BOTTOM_LIFT = 0;


    // ---- dock motion -----------------------------------------------------
    // This overlay is a bottom dock, and swapping between Regular Timeline / Camera / Graph used
    // to be a hard cut: one root's visible went false and another's went true in the same frame,
    // which reads as the screen glitching rather than as a panel changing. It rises from below --
    // the direction it actually lives in -- on the same arrival curve the rest of the HUD uses.
    //
    // TRANSFORM ONLY, no opacity: this dock is opaque and sits over the live game, so fading it
    // would show the match straight through the panel for the length of the transition.
    //
    // Fired once per visible->hidden->visible edge; render() runs every frame, so an unguarded
    // entrance would restart forever and never settle.
    //
    // moDockHideFrames starts ABOVE the debounce threshold, not at 0: this script is torn down on
    // exit (OnExit -> Teardown) and only pumped while the graph is enabled, so render() never runs
    // with the dock hidden. A fresh build IS the "was genuinely hidden" state -- starting at 0 made
    // moDockOpenIsReal always answer false and the rise never played.
    var moDockSeen = false, moDockHideFrames = 99;
    // fromY is a SIGNED offset, because the direction has to agree with what the dock is doing:
    //   growing  (Camera -> Graph): the top edge travels UP, so the panel rises  -> fromY = +14
    //   shrinking (Graph -> Camera): the top edge travels DOWN, so it descends   -> fromY = -14
    // Always rising made the shrink read backwards -- the dock got shorter while its content
    // moved the other way, which is the "weird" part.
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
    // Swapping workspaces (Cam Editor <-> Config) drives this dock's inputs through a frame or two
    // of disagreement, so the raw "should it show" answer blinks false and back, which re-fired the
    // entrance and made rapid Cam Editor / Config clicking stutter.
    //
    // The debounce is on the ANIMATION ONLY -- never on visibility. Delaying the hide was a real
    // bug: the C++ side STOPS pumping this script's render() once the bottom mode changes, so the
    // last render to run returned "still visible", root.visible stayed true, and the camera
    // timeline was left stranded on top of the Regular Timeline with nothing ever coming back to
    // clear it. Visibility must therefore be decided from the RAW answer every frame; only the
    // decision to replay the entrance gets to wait.
    //
    // moDockOpenIsReal(raw) is called once per render. It returns true when raw has just gone
    // false->true after being genuinely hidden (not a one-frame blink).
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

    // ---- hover tips ------------------------------------------------------
    // The old footer was one run-on line of every shortcut in the dock. That legend consumed a
    // strip of the editing area to describe controls that are now self-evident (a diamond adds a
    // key; - and + zoom), so it is gone and the explanation moved to the control under the
    // pointer. ONE shared tip panel, repositioned per anchor, because thirty dedicated label
    // panels would cost more than the single line this replaces.
    var tipPanel = null, tipLabel = null;
    function showTip(anchor, text) {
      if (!text) { hideTip(); return; }
      if (!G) return;   // hover can fire before the first valid layout pass
      if (!tipPanel) {
        tipPanel = mk('Panel', dock); tipPanel.hittest = false; tipPanel.visible = false;
        tipPanel.style.zIndex = '30'; tipPanel.style.padding = '4px 8px';
        tipPanel.style.backgroundColor = 'rgba(8,10,14,0.97)';
        tipPanel.style.border = '1px solid ' + S.border; tipPanel.style.borderRadius = '3px';
        tipLabel = lbl(tipPanel, '', S.value, 11);
        tipLabel.hittest = false; tipLabel.style.whiteSpace = 'nowrap';
      }
      tipLabel.text = text; tipPanel.visible = true;
      // Anchor in client px (same space the streamed cursor uses), converted to dock-local.
      var pos = (anchor && anchor.GetPositionWithinWindow) ? anchor.GetPositionWithinWindow() : null;
      var w = tipPanel.actuallayoutwidth || 0, h = tipPanel.actuallayoutheight || 0;
      var ax = pos ? pos.x / (uiscale || 1) - dockScreenX : 10;
      var ay = pos ? pos.y / (uiscale || 1) - dockScreenY : 10;
      if (ax + w > G.rw) ax = G.rw - w - 2; if (ax < 2) ax = 2;
      tipPanel.style.position = ax.toFixed(0) + 'px ' + Math.max(2, ay - h - 4).toFixed(0) + 'px 0px';
    }
    function hideTip() { if (tipPanel) tipPanel.visible = false; }
    function attachTip(panel, text) {
      panel.SetPanelEvent('onmouseover', function () { showTip(panel, text); });
      panel.SetPanelEvent('onmouseout', function () { hideTip(); });
    }

    // One keyframe-navigator button. Siblings of the row's solo target, never children of it --
    // Panorama bubbles onactivate, so a nav button nested inside the solo hit area would also
    // toggle solo every time you pressed the diamond.
    function navBtn(parent, glyph, tip, fn) {
      var b = mk('Panel', parent); b.hittest = true;
      b.style.width = '17px'; b.style.height = '20px'; b.style.verticalAlign = 'center';
      b.style.borderRadius = '3px'; b.style.marginLeft = '1px';
      var l = lbl(b, glyph, S.inactive, 11);
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
    // Same interaction contract as CameraTimelineJs.h's btn(): hover and a short press flash come
    // from panel events, and the SELECTED state is owned by btnVisual so a per-frame render()
    // cannot repaint over the hover fill. There is no :hover in this in-game Panorama context.
    function panelAlive(b) {
      try {
        if (!b || (b.IsValid && !b.IsValid())) return false;
        if (!b.__lbl || (b.__lbl.IsValid && !b.__lbl.IsValid())) return false;
        return true;
      } catch (e) { return false; }
    }
    function btnVisual(b) {
      if (!panelAlive(b)) return;
      // A TAB never takes a fill in any state -- only its label colour and its accent underline
      // move. Same treatment as the Config panel and the Camera Editor, so the dock's GRAPH /
      // SCENE GRAPH switch reads as navigation rather than as two more buttons in the header.
      if (b.__isTab) {
        b.style.backgroundColor = 'rgba(0,0,0,0)';
        b.__lbl.style.color = b.__sel ? S.accent : (b.__hot ? S.value : '#8b95a3ff');
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
      b.style.borderBottom = '2px solid ' + (b.__sel ? S.accent : (b.__hot ? S.accent + '66' : 'rgba(0,0,0,0)'));
    }
    function tabify(b) {
      b.__isTab = true; b.style.borderRadius = '0px'; btnVisual(b); return b;
    }
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
    // `tip` is integrated here rather than bolted on with attachTip(): SetPanelEvent REPLACES a
    // handler, so a separate attachTip would have silently overwritten the hover/press feedback
    // this button owns.
    function btn(parent, text, onClick, color, tip) {
      var b = mk('Panel', parent); b.hittest = true;
      b.style.borderRadius = UI.btn.radius + 'px';
      b.style.paddingTop = UI.btnCompact.padV + 'px'; b.style.paddingBottom = UI.btnCompact.padV + 'px';
      b.style.paddingLeft = UI.btnCompact.padH + 'px'; b.style.paddingRight = UI.btnCompact.padH + 'px';
      b.style.marginRight = UI.btn.gap + 'px'; b.style.verticalAlign = 'center';
      var l = lbl(b, text, color || S.value, UI.btnCompact.font); l.style.fontWeight = 'bold';
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
    function clamp01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
    // ---- host-scope shims for the shared colour popover -------------------
    // ConfigColorPickerJs.h is a raw-string CHUNK, not a standalone script: it expects
    // mk/lbl/btn/clamp01/root/S from whichever workspace splices it in, plus these three. Config
    // keeps them in ConfigHudWidgetsJs.h, which this dock does not include (it carries a much
    // smaller widget set), so the three the picker actually calls are restated here.
    function hex2(n) {
      n = Math.round(n); if (n < 0) n = 0; if (n > 255) n = 255;
      var h = n.toString(16); return h.length < 2 ? '0' + h : h;
    }
    function rgbHex(c) { return '#' + hex2(c[0]) + hex2(c[1]) + hex2(c[2]) + 'ff'; }
    // Fails OPEN on a throw: the walk runs off the end of our own tree into CS2's native panels,
    // and a placement desync beats a picker that refuses to appear.
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
)GEJS"
R"GEJS(
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

    // ---- root + catcher -------------------------------------------------
    // Root stays FULL-SCREEN + transparent + non-hittest: a reliable screen-size reference (other
    // HUDs read their full-screen root the same way) and a pass-through for clicks outside our dock.
    // The editor itself lives in `dock`: a BOTTOM-DOCKED child filling the width left of the camera
    // editor's inspector, lifted above the compact timeline bar. Size/offset are recomputed each
    // frame in computeGeom(); all editor children lay out relative to `dock`.
    var root = $.CreatePanel('Panel', ctx, 'GraphExpRoot', {});
    // z65: always-on-top experimental overlay. See the canonical z-layer map in PanoramaBridge.h.
    root.hittest = false; root.style.width = '100%'; root.style.height = '100%'; root.style.zIndex = '65';
    root.style.fontFamily = S.font;
    var dock = mk('Panel', root); dock.hittest = false;
    dock.style.horizontalAlign = 'left'; dock.style.verticalAlign = 'bottom';
    // No borderTop: the camera editor's tab bar sits directly above with its own accent
    // borderBottom, so a top border here drew a SECOND separator line 4px under it.
    dock.style.backgroundColor = S.panel;
    var catcher = mk('Panel', dock); catcher.hittest = true; catcher.style.width = '100%'; catcher.style.height = '100%';
    catcher.style.position = '0px 0px 0px';
    catcher.SetPanelEvent('onactivate', function () { /* swallow stray clicks; pipe owns logic */ });

    // ---- header ---------------------------------------------------------
    // Title + live info sit on the LEFT; a two-row control cluster anchors to the RIGHT. Top row
    // = switch-back-to-timeline + demo timescale; bottom row = the graph's own interp / history /
    // delete. (Requested: the Timeline + speed buttons sit ABOVE the Smooth/Linear stuff, right.)
    var header = mk('Panel', dock); header.style.width = '100%'; header.style.height = HEADER_H + 'px';
    header.style.flowChildren = 'right'; header.style.backgroundColor = S.panel;
    header.style.borderBottom = '1px solid ' + S.border; header.style.paddingLeft = '14px'; header.style.paddingRight = '14px';
    // The dock's OWN mode switch. Camera curves and scene strips are two models in one dock, and
    // which one is up must be a thing you can see and click -- it was previously implied by the
    // inspector tab, so the camera graph vanished the moment you opened SCENE KEYS.
    function setGraphMode(mode) {
      rawCmd('mirv_filmmaker settings ui editor.graph ' + mode);
      if (st) { st.graphMode = mode; lastSig = ''; sceneHeadSig = ''; }
    }
    var modeStrip = mk('Panel', header); modeStrip.style.flowChildren = 'right';
    modeStrip.style.verticalAlign = 'center'; modeStrip.style.marginRight = '18px';
    function gTab(caption, mode, last) {
      var b = tabify(btn(modeStrip, caption, function () { setGraphMode(mode); }, S.label));
      b.style.width = 'fit-children';
      b.style.paddingLeft = '0px'; b.style.paddingRight = '0px';
      b.style.paddingTop = '8px'; b.style.paddingBottom = '7px';
      b.style.marginRight = last ? '0px' : '16px';
      b.__lbl.style.fontSize = '15px'; b.__lbl.style.fontWeight = 'bold';
      b.__lbl.style.letterSpacing = '1.4px'; b.__lbl.style.whiteSpace = 'nowrap';
      return b;
    }
    var camModeTab = gTab('GRAPH', 'camera');
    var sceneModeTab = gTab('SCENE GRAPH', 'scene', true);
    var hInfo = lbl(header, '', S.hint, 11); hInfo.style.verticalAlign = 'center'; hInfo.style.width = 'fill-parent-flow(1.0)';
    hInfo.style.letterSpacing = '0.5px';

    // Right-anchored, two stacked rows.
    var rightCol = mk('Panel', header); rightCol.style.flowChildren = 'down'; rightCol.style.verticalAlign = 'center';
    var rowTop = mk('Panel', rightCol); rowTop.style.flowChildren = 'right'; rowTop.style.horizontalAlign = 'right'; rowTop.style.marginBottom = '5px';
    var rowBot = mk('Panel', rightCol); rowBot.style.flowChildren = 'right'; rowBot.style.horizontalAlign = 'right';

    // TOP row: demo speed. The SAME continuous scrubber the native demo bar and the camera timeline
    // carry -- six preset pills could not express 0.7x or 12x at all, and cost more width than the
    // control that can. (Bottom-panel switching lives in the Camera Editor's bottom-editor tab bar,
    // so there is no in-graph "Timeline" switch button. The graph editor always drives the camera
    // now -> no Drive toggle; the camera is seeded automatically -> no Reseed.)
    var speedScrub = buildSpeedScrubber(rowTop, { width: 250, height: 30, font: S.font });
    speedScrub.strip.style.marginRight = '0px';

    // BOTTOM row: Smooth / Linear. With no selection they set the whole graph AND the camera
    // path's interpolation (one mode, mirrored both ways -- the camera timeline shows the same
    // pair); with keys selected they shape only those keys' handles, graph-local.
    // TIME SNAP: one shared step for both graph modes (persisted as editor.graph.snap). The pill
    // shows the current value and lights when snapping; clicking opens the dropdown.
    var snapBtn = btn(rowBot, 'SNAP', function () { showSnapMenu(); }, S.value,
      'Time snap: quantise the playhead and key placement to a tick grid');
    snapBtn.style.verticalAlign = 'center';
    var smoothBtn = btn(rowBot, '∿ Smooth', function () { cmd('smooth' + (st && st.selCount > 0 ? ' sel' : '')); }, S.value,
      'Smooth interpolation for the selection (or the whole graph)');
    smoothBtn.style.verticalAlign = 'center';
    var linearBtn = btn(rowBot, '╱ Linear', function () { cmd('linear' + (st && st.selCount > 0 ? ' sel' : '')); }, S.value,
      'Linear interpolation for the selection (or the whole graph)');
    linearBtn.style.verticalAlign = 'center';
    var undoBtn = btn(rowBot, '↺ Undo', function () { cmd('undo'); }, S.value); undoBtn.style.verticalAlign = 'center';
    var redoBtn = btn(rowBot, '↻ Redo', function () { cmd('redo'); }, S.value); redoBtn.style.verticalAlign = 'center';
    // Destructive actions do not need to dominate the toolbar. Delete Key is an icon button with a
    // tooltip: it still turns red-tinted while there is a selection to act on, but at rest it is a
    // quiet glyph rather than the widest pill in the header.
    var delBtn = btn(rowBot, '✕', function () { if (st && st.selCount > 0) cmd('delsel'); }, S.dangerText,
      'Delete selected keyframes');
    delBtn.style.verticalAlign = 'center'; delBtn.style.marginRight = '0px';
    delBtn.style.paddingLeft = '9px'; delBtn.style.paddingRight = '9px';
    // Destructive treatment: red-tinted fill + border, not just red text on the same gray pill.
    delBtn.style.backgroundColor = S.dangerBg; delBtn.style.border = '1px solid ' + S.dangerBorder;
    // No "Exit Experiment" button: the graph editor is the default curve editor now, opened and
    // closed with the Camera Editor workspace (use the inspector's Exit). Standalone toggle is
    // still available via the `mirv_filmmaker grapheditor off` console command.

)GEJS"
R"GEJS(
    // ---- time-snap dropdown ---------------------------------------------
    // Real panels with onactivate (unlike the raw-pipe right-click ease menu) because Custom…
    // hosts a text field. It opens UNDER the SNAP pill, so it stays inside the dock.
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
      var pos = (snapBtn && snapBtn.GetPositionWithinWindow) ? snapBtn.GetPositionWithinWindow() : null;
      var ax = pos ? (pos.x / (uiscale || 1) - dockScreenX) : 100;
      var ay = pos ? (pos.y / (uiscale || 1) - dockScreenY) : 60;
      var aw = (snapBtn && snapBtn.actuallayoutwidth) || 0, ah = (snapBtn && snapBtn.actuallayoutheight) || 0;
      ax = Math.max(0, Math.min(ax + aw - SNAP_W, (G ? G.rw : 1920) - SNAP_W - 2));
      ay = ay + ah + 2;   // below the pill
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
      var t = lbl(head, 'TIME SNAP', S.hint, 10); t.hittest = false; t.style.verticalAlign = 'center';
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
      hideEaseMenu();
      snapCustomBox.visible = false;
      refreshSnapRows();
      layoutSnapMenu();
      snapMenu.visible = true;
    }

    // ---- left channel column -------------------------------------------
    // Each row sits at a FIXED Y (so the value cell's rect stays known for the mouse pipe) but its
    // contents FLOW horizontally + center vertically -- so the swatch / name / value / eye always
    // share one line. (Positioning each element by absolute Y inside the tall column drifted the
    // labels out of their rows.)
    // Rows start at the TOP of the editing area (aligned with the canvas/ruler top), not pushed
    // down by a ruler band that no longer exists: AE's layer list begins at the panel's top edge.
    var ROWS_Y0 = OVERVIEW_H, ROW_H = 34, VALW = 64, EYEW = 24;
    var EYE_X = LEFTW - 8 - EYEW, VAL_X = EYE_X - 6 - VALW;
    var left = mk('Panel', dock); left.style.position = '0px ' + HEADER_H + 'px 0px';
    left.style.width = LEFTW + 'px'; left.style.backgroundColor = S.panel;
    left.style.borderRight = '1px solid ' + S.border;
    // The property-column header sits at the very TOP of the column -- AE's layer panel starts at
    // the panel's top edge, it does not push its rows down to the ruler.
    var lhP = mk('Panel', left); lhP.hittest = false; lhP.style.position = '8px 4px 0px';
    lhP.style.width = (LEFTW - 16) + 'px'; lhP.style.height = '16px';
    var lh = lbl(lhP, 'PROPERTIES', S.hint, 11); lh.style.fontWeight = 'bold'; lh.style.letterSpacing = '2px'; lh.style.verticalAlign = 'center';

    var rows = []; // per-channel row controls + value-cell rect (dock-relative layout coords)
    function makeRow(c) {
      var rowY = ROWS_Y0 + c * ROW_H;
      var rowP = mk('Panel', left); rowP.hittest = false;
      rowP.style.position = '0px ' + rowY + 'px 0px'; rowP.style.width = LEFTW + 'px'; rowP.style.height = (ROW_H - 4) + 'px';
      rowP.style.flowChildren = 'right'; rowP.style.paddingLeft = '8px'; rowP.style.paddingRight = '8px';
      // swatch + name = solo TOGGLE target; fills the row left of the keyframe navigator. Then, in
      // order: << diamond >> (the navigator), the value cell the mouse pipe scrubs/types, and the
      // eye. Value and eye stay right-anchored at the same x as before, so the value-cell hit rect
      // (VAL_X) is unchanged.
      var hot = mk('Panel', rowP); hot.hittest = true; hot.style.flowChildren = 'right';
      hot.style.width = 'fill-parent-flow(1.0)'; hot.style.height = '100%'; hot.style.borderRadius = '3px';
      hot.SetPanelEvent('onactivate', function () { var m = chById(c); cmd('chan ' + c + ' ' + (m && m.solo ? 'unsolo' : 'solo')); });
      var sw = mk('Panel', hot); sw.hittest = false; sw.style.width = '12px'; sw.style.height = '12px';
      sw.style.verticalAlign = 'center'; sw.style.marginLeft = '2px'; sw.style.marginRight = '9px'; sw.style.borderRadius = '3px';
      var nm = lbl(hot, '', S.value, 12); nm.hittest = false; nm.style.verticalAlign = 'center';
      var nav = mk('Panel', rowP); nav.style.height = '100%'; nav.style.marginRight = '5px';
      nav.style.flowChildren = 'right'; nav.style.width = 'fit-children';
      var prev = navBtn(nav, '◀', 'Previous keyframe', function () { jumpChannelKey(c, -1); });
      var dia = navBtn(nav, '◇', 'Add or remove a keyframe at the playhead', function () { toggleChannelKey(c); });
      var next = navBtn(nav, '▶', 'Next keyframe', function () { jumpChannelKey(c, 1); });
      // The value lives in a fixed-size BOX so the in-cell TextEntry can be created inside it; the
      // box's rect is the value-cell hit rect the mouse pipe scrubs and the editor types into.
      var valBox = mk('Panel', rowP); valBox.hittest = false;
      valBox.style.width = VALW + 'px'; valBox.style.height = '100%'; valBox.style.marginRight = '6px';
      var val = lbl(valBox, '-', S.accent, 12); val.hittest = false; val.style.width = '100%';
      val.style.textAlign = 'right'; val.style.verticalAlign = 'center'; val.style.fontWeight = 'bold';
      var eye = mk('Panel', rowP); eye.hittest = true; eye.style.width = EYEW + 'px'; eye.style.height = '20px';
      eye.style.verticalAlign = 'center'; eye.style.borderRadius = '3px';
      var eyeL = lbl(eye, '◉', S.label, 12); eyeL.style.horizontalAlign = 'center'; eyeL.style.verticalAlign = 'center';
      eye.SetPanelEvent('onactivate', function () { var m = chById(c); cmd('chan ' + c + ' ' + (m && m.vis ? 'hide' : 'show')); });
      attachTip(eye, 'Show / hide this property');
      rows.push({ rowP: rowP, hot: hot, sw: sw, nm: nm, valBox: valBox, val: val, eye: eye, eyeL: eyeL,
                  prev: prev, dia: dia, next: next,
                  valRect: { x0: VAL_X, y0: HEADER_H + rowY, x1: VAL_X + VALW, y1: HEADER_H + rowY + (ROW_H - 4) } });
    }
    for (var c0 = 0; c0 < 7; c0++) makeRow(c0);
)GEJS"
// THE SAME COLOUR POPOVER THE CONFIG PANEL USES -- wheel, saturation, brightness and hex, one
// definition spliced into both workspaces (the pattern TimelineDockRibbonJs.h and
// EditorSettingsPopupJs.h already follow). A scene colour property is a colour, and it opens the
// control the rest of this UI opens for a colour; it does not get a second, worse editor that
// makes you type "255 180 140" to find out what it looks like.
#include "ConfigColorPickerJs.h"
R"GEJS(
    // ---- timeline overview / navigator ---------------------------------
    // AE's navigation bar. It sits directly above the ruler, spans the time area, and shows the
    // WHOLE demo as a track with a draggable viewport marking the slice the ruler below displays.
    // This is the primary time navigation -- drag the body to pan, either edge to zoom, and the
    // viewport literally is the answer to "what am I looking at". (The old header speed scrubber
    // was never a navigator; it stays where it is, as a speed control, and the map now exists.)
    var overview = mk('Panel', dock); overview.hittest = false;
    overview.style.backgroundColor = 'rgba(0,0,0,0.25)';
    overview.style.borderBottom = '1px solid ' + S.border;
    var ovTrack = mk('Panel', overview); ovTrack.hittest = false;
    ovTrack.style.position = '0px 0px 0px'; ovTrack.style.width = '100%'; ovTrack.style.height = '100%';
    ovTrack.style.backgroundColor = S.ovTrack;
    // Keyframe marks, so the navigator also answers "where are my keys in the whole demo".
    var ovKeyLayer = mk('Panel', overview); ovKeyLayer.hittest = false;
    ovKeyLayer.style.position = '0px 0px 0px'; ovKeyLayer.style.width = '100%'; ovKeyLayer.style.height = '100%';
    var ovView = mk('Panel', overview); ovView.hittest = false;
    ovView.style.backgroundColor = S.ovView; ovView.style.border = '1px solid ' + S.ovViewBorder;
    ovView.style.borderRadius = '2px';
    var ovPlay = mk('Panel', overview); ovPlay.hittest = false;
    ovPlay.style.width = '1px'; ovPlay.style.height = '100%'; ovPlay.style.backgroundColor = S.ovPlay;
    // Persistent edge grips sit above the playhead/key marks. Input still belongs to the
    // raw navigator hit regions; these panels only make those existing regions visible.
    function overviewGrip() {
      var grip = mk('Panel', overview); grip.hittest = false;
      grip.style.width = '6px'; grip.style.height = '22px';
      grip.style.borderRadius = '2px'; grip.style.backgroundColor = '#303740ff';
      grip.style.border = '1px solid #bac4d0ff';
      var line = mk('Panel', grip); line.hittest = false;
      line.style.width = '2px'; line.style.height = '12px';
      line.style.horizontalAlign = 'center'; line.style.verticalAlign = 'center';
      line.style.backgroundColor = '#e5ebf2ff';
      return grip;
    }
    var ovLeftGrip = overviewGrip(), ovRightGrip = overviewGrip();

    // ---- ruler + canvas layers -----------------------------------------
    var ruler = mk('Panel', dock); ruler.hittest = false; ruler.style.backgroundColor = 'rgba(0,0,0,0.25)';
    ruler.style.borderBottom = '1px solid ' + S.border;
    var rulerLayer = mk('Panel', ruler); rulerLayer.hittest = false; rulerLayer.style.width = '100%'; rulerLayer.style.height = '100%';

    var canvas = mk('Panel', dock); canvas.hittest = false; canvas.style.backgroundColor = 'rgba(0,0,0,0.18)';
    // Backstop for the scene strip stack, which scrolls: a half-visible band's contents must stop
    // at the canvas edge instead of drawing over the ruler above it or the bottom bar below.
    canvas.style.overflow = 'clip clip';
    // Scene-mode property bands sit UNDER the grid, so they are created first (Panorama draws
    // siblings in creation order and this canvas has no z-index of its own).
    var stripLayer = mk('Panel', canvas); stripLayer.hittest = false;
    var gridLayer = mk('Panel', canvas); gridLayer.hittest = false;
    var curveLayer = mk('Panel', canvas); curveLayer.hittest = false;
    var kfLayer = mk('Panel', canvas); kfLayer.hittest = false;
    var handleLayer = mk('Panel', canvas); handleLayer.hittest = false;
    var rectVis = mk('Panel', canvas); rectVis.hittest = false; rectVis.visible = false;
    rectVis.style.backgroundColor = '#f0b32322'; rectVis.style.border = '1px solid ' + S.accent;
    [stripLayer, gridLayer, curveLayer, kfLayer, handleLayer].forEach(function (L) { L.style.position = '0px 0px 0px'; L.style.width = '100%'; L.style.height = '100%'; });

    // The playhead is a property of the DOCK, not of the canvas: it runs from the top of the
    // ruler all the way down through the editing area (AE's playhead extends through the tracks),
    // so it is not clipped away by the canvas' own overflow as it crosses the ruler line.
    var phLine = mk('Panel', dock); phLine.hittest = false; phLine.style.zIndex = '5';
    phLine.style.width = '2px'; phLine.style.backgroundColor = S.playhead;

    // Playhead grab handle: a clear pin sitting in the ruler directly above the red line, so users
    // know where to click + drag to scrub. The red line itself is ALSO grabbable in the canvas.
    var phHandle = mk('Panel', dock); phHandle.hittest = false; phHandle.style.zIndex = '6';
    phHandle.style.width = '15px'; phHandle.style.height = '11px';
    phHandle.style.backgroundColor = S.playhead; phHandle.style.borderRadius = '3px';
    var phPin = mk('Panel', phHandle); phPin.hittest = false; phPin.style.width = '9px'; phPin.style.height = '9px';
    phPin.style.backgroundColor = S.playhead; phPin.style.transform = 'rotateZ(45deg)'; phPin.style.position = '3px 7px 0px';

    // ---- bottom bar: timeline scrollbar + zoom --------------------------
    // The old one-line shortcut legend lived here and is gone: instructions belong on the control
    // under the pointer, not burned into the editing area. In its place is real navigation -- a
    // scrollbar whose thumb WIDTH is the zoom level (wide = most of the demo is on screen, narrow
    // = you are inside a small range) and - / + buttons. Dragging the thumb pans; clicking the
    // trough re-centres there.
    var ZOOM_W = 26, SB_H = 14;
    var bottomBar = mk('Panel', dock); bottomBar.style.backgroundColor = S.panel;
    bottomBar.style.borderTop = '1px solid ' + S.border;
    function zoomBtn(glyph, tip, fn) {
      var b = mk('Panel', bottomBar); b.hittest = true;
      b.style.width = ZOOM_W + 'px'; b.style.borderRadius = '0px'; b.style.backgroundColor = S.btnBg;
      var l = lbl(b, glyph, S.value, 16); l.hittest = false;
      l.style.horizontalAlign = 'center'; l.style.verticalAlign = 'center'; l.style.fontWeight = 'bold';
      b.__lbl = l;
      b.SetPanelEvent('onactivate', function () { hideTip(); fn(); });
      b.SetPanelEvent('onmouseover', function () { b.style.backgroundColor = S.btnHover; showTip(b, tip); });
      b.SetPanelEvent('onmouseout', function () { b.style.backgroundColor = S.btnBg; hideTip(); });
      return b;
    }
    var zoomOut = zoomBtn('−', 'Zoom out the timeline (Ctrl+Wheel down)', function () { zoomStep(1.6); });
    var zoomIn = zoomBtn('+', 'Zoom in the timeline (Ctrl+Wheel up)', function () { zoomStep(1 / 1.6); });
    var sbTrack = mk('Panel', bottomBar); sbTrack.hittest = false;
    sbTrack.style.backgroundColor = 'rgba(0,0,0,0.45)'; sbTrack.style.borderRadius = '7px';
    var sbThumb = mk('Panel', sbTrack); sbThumb.hittest = false;
    sbThumb.style.backgroundColor = '#ffffff33'; sbThumb.style.borderRadius = '7px';
    sbThumb.style.border = '1px solid #ffffff2a';

    // ---- SCENE KEYS: stacked property strips ---------------------------
    // SCENE KEYS uses the Graph dock as a stacked property editor. Camera mode overlays seven
    // channels on ONE set of axes, which works because they are all metres/degrees on one camera.
    // Scene properties are not: exposure lives in -1..10, fog density in 0..1, sun yaw in 0..360
    // and a toggle in 0..1. Drawn on shared axes the small ranges flatten onto the baseline and
    // the curves cross each other meaninglessly. Each property therefore gets its OWN full-width
    // horizontal strip with its own vertical range, stacked down the canvas, and the left column
    // carries one header per strip, lined up with it.
    //
    // A strip exists for every property the inspector has turned ON, keyed or not.
    //
    // STRIPS ARE GROUPED. A shot can have a dozen properties under Lighting, Weather & Sky,
    // Atmosphere and Look, and the old flat list gave no way to tell them apart or to get a group
    // out of the way. Each group now gets a header BAR carrying its name, a disclosure triangle
    // and an eye (show/hide every curve in the group); its properties sit under it as compact
    // single-line rows. Collapsing a group removes its rows AND its canvas bands, so the whole
    // group -- not just its labels -- goes away.
    var SCENE_ROW_H = 26, SCENE_GROUP_H = 24;
    // The value cell's rect has to be known in DOCK coordinates because the mouse pipe owns it
    // (drag to scrub, click to type, right-click for its menu) -- exactly the camera column's
    // contract, which is the interaction people already know in this dock.
    var SH_VALW = 78, SH_ROWH = 20;
    var SH_VALX = LEFTW - 8 - EYEW - 6 - SH_VALW;
    var sceneStrips = [], sceneGroups = [], sceneScroll = 0, sceneContentH = 0, sceneCollapsed = {}, sceneCollapsedCh = {};
    var sceneHeadSig = '', sceneValues = {}, sceneCells = [], sceneNavs = [];
    var sceneHeadClip = mk('Panel', left); sceneHeadClip.visible = false; sceneHeadClip.hittest = false;
    sceneHeadClip.style.width = LEFTW + 'px'; sceneHeadClip.style.overflow = 'clip clip';
    var sceneHeads = mk('Panel', sceneHeadClip); sceneHeads.hittest = false;
    sceneHeads.style.position = '0px 0px 0px'; sceneHeads.style.width = '100%';
    var sceneEmpty = lbl(canvas, 'Turn properties on in SCENE KEYS to give them strips here.', S.hint, 14);
    sceneEmpty.style.horizontalAlign = 'center'; sceneEmpty.style.verticalAlign = 'center'; sceneEmpty.hittest = false;

    function stepped(ch) { return ch && (ch.type === 'bool' || ch.type === 'enum' || ch.type === 'asset'); }
    // Rounded per component: a colour arrives as three float channels and printing them raw put
    // '254.99999707116' in a 74px field, which wraps to three lines and hides the strip header.
    function sceneValueText(v) {
      if (v instanceof Array) return v.map(function (n) { return typeof n === 'number' ? String(Math.round(n * 1000) / 1000) : String(n); }).join(' ');
      return typeof v === 'number' ? String(Math.round(v * 1000) / 1000) : String(v || '');
    }
    function sceneOn() { return !!(st && st.graphMode === 'scene'); }
    function stripFor(c) { for (var i = 0; i < sceneStrips.length; i++) if (sceneStrips[i].ch === c) return sceneStrips[i]; return null; }
    function stripAt(cly) { for (var i = 0; i < sceneStrips.length; i++) { var s = sceneStrips[i]; if (cly >= s.y0 && cly < s.y0 + s.h) return s; } return null; }
    // A scene channel whose GROUP is collapsed is not drawn or hit-tested: collapsing a group means
    // "hide everything in it", and with the curves overlaid there is no band to remove. The list
    // rows are gone anyway; this is what takes the curves with them.
    function sceneStripped(ch) { return !sceneOn() || !sceneCollapsedCh[ch.ch]; }
    // Build the group list and place the compact rows. One pass, called before anything reads a
    // band. Groups come from the catalog's own family/group order (already sorted), so two
    // properties with the same name in different families cannot merge.
    function layoutSceneStrips() {
      sceneStrips = []; sceneGroups = []; sceneCollapsedCh = {};
      var n = model.length;
      if (!n || !G) { sceneScroll = 0; sceneContentH = 0; return; }
      var byId = {};
      for (var i = 0; i < n; i++) {
        var c = model[i], gid = (c.family || '') + '.' + (c.group || '');
        var g = byId[gid];
        if (!g) { g = byId[gid] = { id: gid, title: c.group || c.family || 'Properties', items: [] }; sceneGroups.push(g); }
        g.items.push(i);
      }
      var y = 0;
      for (var gi = 0; gi < sceneGroups.length; gi++) {
        var grp = sceneGroups[gi];
        grp.collapsed = !!sceneCollapsed[grp.id];
        grp.y0 = y; grp.h = SCENE_GROUP_H;
        if (grp.collapsed) {
          // Collapsed group: no rows, and its curves drop out of the shared canvas too.
          for (var h = 0; h < grp.items.length; h++) sceneCollapsedCh[model[grp.items[h]].ch] = true;
        }
        y += SCENE_GROUP_H;
        if (grp.collapsed) continue;
        for (var k = 0; k < grp.items.length; k++) {
          var ch = model[grp.items[k]];
          sceneStrips.push({ ch: ch.ch, fieldId: ch.fieldId, groupId: grp.id, y0: y, h: SCENE_ROW_H });
          y += SCENE_ROW_H;
        }
      }
      sceneContentH = y;
      var max = Math.max(0, sceneContentH - G.ch);
      if (sceneScroll > max) sceneScroll = max;
      if (sceneScroll < 0) sceneScroll = 0;
      for (var s = 0; s < sceneStrips.length; s++) sceneStrips[s].y0 -= sceneScroll;
      for (var g2 = 0; g2 < sceneGroups.length; g2++) sceneGroups[g2].y0 -= sceneScroll;
    }
)GEJS"
R"GEJS(
    function rebuildSceneHeads() {
      sceneHeads.RemoveAndDeleteChildren(); sceneValues = {}; sceneCells = []; sceneNavs = [];
      for (var gi = 0; gi < sceneGroups.length; gi++) (function (grp) {
        // ---- group header bar: disclosure + name + count + show/hide-all ----
        var allVis = false;
        for (var a = 0; a < grp.items.length; a++) if (model[grp.items[a]].vis) allVis = true;
        var gh = mk('Panel', sceneHeads); gh.hittest = false;
        gh.style.position = '0px ' + grp.y0 + 'px 0px';
        gh.style.width = LEFTW + 'px'; gh.style.height = grp.h + 'px';
        gh.style.flowChildren = 'right'; gh.style.paddingLeft = '8px'; gh.style.paddingRight = '8px';
        gh.style.backgroundColor = 'rgba(255,255,255,0.07)';
        gh.style.borderTop = '1px solid ' + S.border;
        // The disclosure+name is its OWN click target, a SIBLING of the eye. Panorama bubbles
        // onactivate, so putting the toggle on the whole bar would make the show/hide-all eye also
        // collapse the group every time you pressed it.
        var toggle = (function (gid) { return function () {
          sceneCollapsed[gid] = !sceneCollapsed[gid]; sceneHeadSig = ''; lastSig = ''; api.render();
        }; })(grp.id);
        var ghot = mk('Panel', gh); ghot.hittest = true; ghot.style.flowChildren = 'right';
        ghot.style.width = 'fill-parent-flow(1.0)'; ghot.style.height = '100%'; ghot.style.borderRadius = '3px';
        ghot.SetPanelEvent('onactivate', toggle);
        ghot.SetPanelEvent('onmouseover', function () { gh.style.backgroundColor = 'rgba(255,255,255,0.12)'; });
        ghot.SetPanelEvent('onmouseout', function () { gh.style.backgroundColor = 'rgba(255,255,255,0.07)'; });
        var gchev = lbl(ghot, grp.collapsed ? '▶' : '▼', S.inactive, 10);
        gchev.hittest = false; gchev.style.verticalAlign = 'center'; gchev.style.marginRight = '7px';
        var gname = lbl(ghot, grp.title, S.value, 11);
        gname.hittest = false; gname.style.verticalAlign = 'center'; gname.style.fontWeight = 'bold';
        gname.style.letterSpacing = '0.6px'; gname.style.whiteSpace = 'nowrap';
        gname.style.textOverflow = 'ellipsis'; gname.style.width = 'fill-parent-flow(1.0)';
        var gcount = lbl(gh, '' + grp.items.length, S.hint, 10);
        gcount.hittest = false; gcount.style.verticalAlign = 'center'; gcount.style.marginRight = '8px';
        var geye = btn(gh, allVis ? '◉' : '○', function () {
          for (var m = 0; m < grp.items.length; m++) cmd('chan ' + model[grp.items[m]].ch + (allVis ? ' hide' : ' show'));
        }, allVis ? S.accent : S.hint, allVis ? 'Hide every ' + grp.title + ' curve' : 'Show every ' + grp.title + ' curve');
        geye.style.width = '24px'; geye.style.height = '20px'; geye.style.verticalAlign = 'center';
        geye.style.paddingLeft = '0px'; geye.style.paddingRight = '0px'; geye.style.marginRight = '0px';
        if (grp.collapsed) return;

        // ---- one compact row per property ----
        // ONE CLOSURE PER ROW. `var ch` inside a plain for-loop is function-scoped, so every row's
        // solo / eye / keyframe handlers captured the SAME (last) property in the group: clicking
        // the eye on the first of three hid the third. The rows are built in an IIFE for the same
        // reason the groups above are.
        for (var k = 0; k < grp.items.length; k++) (function (ch, strip) {
          if (!strip) return;
          var row = mk('Panel', sceneHeads); row.hittest = false;
          row.style.position = '0px ' + strip.y0 + 'px 0px';
          row.style.width = LEFTW + 'px'; row.style.height = strip.h + 'px';
          row.style.flowChildren = 'right'; row.style.paddingLeft = '8px'; row.style.paddingRight = '8px';
          row.style.borderTop = '1px solid ' + S.border;
          row.style.backgroundColor = ch.solo ? S.rowOn : 'rgba(0,0,0,0)';
          var hot = mk('Panel', row); hot.hittest = true; hot.style.flowChildren = 'right';
          hot.style.width = 'fill-parent-flow(1.0)'; hot.style.height = '100%'; hot.style.borderRadius = '3px';
          hot.SetPanelEvent('onactivate', function () { cmd('chan ' + ch.ch + (ch.solo ? ' unsolo' : ' solo')); });
          var sw = mk('Panel', hot); sw.hittest = false; sw.style.width = '10px'; sw.style.height = '10px';
          sw.style.borderRadius = '2px'; sw.style.backgroundColor = ch.color;
          sw.style.verticalAlign = 'center'; sw.style.marginLeft = '2px'; sw.style.marginRight = '8px';
          var nm = lbl(hot, ch.muted ? ch.name + '  (off)' : ch.name, ch.muted ? S.hint : ch.solo ? S.accent : (ch.edit ? S.value : S.inactive), 12);
          nm.hittest = false; nm.style.verticalAlign = 'center';
          nm.style.whiteSpace = 'nowrap'; nm.style.textOverflow = 'ellipsis'; nm.style.width = 'fill-parent-flow(1.0)';
          var dnav = mk('Panel', row); dnav.style.height = '100%'; dnav.style.marginRight = '5px';
          dnav.style.flowChildren = 'right'; dnav.style.width = 'fit-children';
          var dprev = navBtn(dnav, '◀', 'Previous keyframe', function () { jumpSceneKey(ch, -1); });
          var ddia = navBtn(dnav, '◇', 'Add or remove a keyframe at the playhead', function () { toggleSceneKey(ch); });
          var dnext = navBtn(dnav, '▶', 'Next keyframe', function () { jumpSceneKey(ch, 1); });
          sceneNavs.push({ ch: ch.ch, fieldId: ch.fieldId, prev: dprev, dia: ddia, next: dnext });
          // A SINGLE NUMBER IS A SCRUBBY CELL, not a text box: drag it left/right to move the value
          // and watch the world change, click it to type an exact one (in the cell), right-click it
          // for this strip's own key menu. Colours, skies and enums keep a real TextEntry: there is
          // nothing to scrub along, and "255 180 140" or a sky name has to be typed.
          if (ch.type === 'colour') {
            // A COLOUR OPENS THE COLOUR WHEEL. The chip IS the swatch: it shows the value, and
            // clicking it drops the shared popover under the row. Writing goes through the same
            // `scenekeys set` verb the number cells use, so a colour edit keys exactly like any
            // other edit (and, at a keyed tick, re-keys in place).
            var swRgb = [255, 255, 255];
            var chip = mk('Panel', row); chip.hittest = true;
            chip.style.width = SH_VALW + 'px'; chip.style.height = SH_ROWH + 'px';
            chip.style.marginRight = '6px'; chip.style.verticalAlign = 'center';
            chip.style.borderRadius = '3px'; chip.style.border = '1px solid ' + S.border;
            var chipLbl = lbl(chip, '', S.value, 10);
            chipLbl.hittest = false; chipLbl.style.horizontalAlign = 'center';
            chipLbl.style.verticalAlign = 'center'; chipLbl.style.fontWeight = 'bold';
            chipLbl.style.whiteSpace = 'nowrap';
            var crec = { swatch: chip, swatchLbl: chipLbl, rgb: swRgb, editing: false, last: '' };
            function paintChip() {
              chip.style.backgroundColor = rgbHex(swRgb);
              // Dark text on a light swatch, light on a dark one -- the hex has to stay readable
              // against the colour it is describing.
              var lum = swRgb[0] * 0.2126 + swRgb[1] * 0.7152 + swRgb[2] * 0.0722;
              chipLbl.style.color = lum > 145 ? '#111820ff' : '#ffffffff';
              chipLbl.text = rgbHex(swRgb).substring(1, 7).toUpperCase();
            }
            crec.paint = paintChip;
            crec.picker = colorPicker(root, ch.name, swRgb, function (value) {
              paintChip();
              // show() fires onChange once to prime the preview; only a real change may write,
              // or merely OPENING the wheel would drop a keyframe.
              var sig = value[0] + ',' + value[1] + ',' + value[2];
              if (sig === crec.last) return;
              crec.last = sig;
              var live = chById(crec.ch), out = value.slice(0, 3);
              // Alpha (skyColor and friends are RGBA) is not the wheel's business: carry the
              // channel's current alpha through untouched rather than dropping it to opaque.
              if (live && live.value instanceof Array && live.value.length > 3) out.push(live.value[3]);
              rawCmd('mirv_filmmaker scenekeys set ' + ch.fieldId + ' ' + out.join(' '));
            }, 1);
            crec.ch = ch.ch;
            chip.SetPanelEvent('onactivate', function () { crec.picker.show(chip); });
            attachTip(chip, 'Pick a colour for ' + ch.name);
            paintChip();
            sceneValues[ch.fieldId] = crec;
          } else if (scrubbable(ch)) {
            var cell = mk('Panel', row); cell.hittest = false;
            cell.style.width = SH_VALW + 'px'; cell.style.height = SH_ROWH + 'px'; cell.style.marginRight = '6px';
            cell.style.verticalAlign = 'center'; cell.style.borderRadius = '3px';
            cell.style.backgroundColor = S.btnBg;
            var cl = lbl(cell, '-', S.accent, 12);
            cl.hittest = false; cl.style.horizontalAlign = 'right'; cl.style.verticalAlign = 'center';
            cl.style.fontWeight = 'bold'; cl.style.whiteSpace = 'nowrap'; cl.style.marginRight = '7px';
            var top0 = HEADER_H + OVERVIEW_H + strip.y0 + Math.round((strip.h - SH_ROWH) / 2);
            sceneCells.push({ ch: ch.ch, fieldId: ch.fieldId, box: cell, lbl: cl,
              rect: { x0: SH_VALX, y0: top0, x1: SH_VALX + SH_VALW, y1: top0 + SH_ROWH } });
            sceneValues[ch.fieldId] = { cell: cl, editing: false };
          } else {
            var val = $.CreatePanel('TextEntry', row, '', {});
            val.style.width = SH_VALW + 'px'; val.style.height = SH_ROWH + 'px'; val.style.marginRight = '6px';
            val.style.verticalAlign = 'center';
            val.style.fontSize = '11px'; val.style.color = S.value; val.style.backgroundColor = S.btnBg;
            val.style.border = '1px solid ' + S.border; val.style.borderRadius = '3px';
            var rec = { input: val, editing: false };
            val.SetPanelEvent('onfocus', function () { rec.editing = true; });
            val.SetPanelEvent('onblur', function () { rec.editing = false; });
            val.SetPanelEvent('oninputsubmit', function () { rawCmd('mirv_filmmaker scenekeys set ' + ch.fieldId + ' ' + val.text.replace(/,/g, ' ')); rec.editing = false; });
            sceneValues[ch.fieldId] = rec;
          }
          var eye = btn(row, ch.vis ? '◉' : '○', function () { cmd('chan ' + ch.ch + (ch.vis ? ' hide' : ' show')); }, ch.vis ? S.accent : S.hint);
          eye.style.width = '24px'; eye.style.height = '20px'; eye.style.verticalAlign = 'center';
          eye.style.paddingLeft = '0px'; eye.style.paddingRight = '0px'; eye.style.marginRight = '0px';
        })(model[grp.items[k]], stripFor(model[grp.items[k]].ch));
      })(sceneGroups[gi]);
    }
    function scrubbable(ch) { return !!ch && (ch.type === 'float' || ch.type === 'angle'); }
    function sceneCellHit(lx, ly) {
      if (!sceneOn()) return null;
      // The list is a clipped viewport (it scrolls), so a row that is scrolled out must not be
      // hit-testable through the clip.
      var top = G.ovH + HEADER_H;
      if (ly < top || ly > top + G.ch) return null;
      for (var i = 0; i < sceneCells.length; i++) {
        var r = sceneCells[i].rect;
        if (lx >= r.x0 && lx <= r.x1 && ly >= r.y0 && ly <= r.y1) return sceneCells[i];
      }
      return null;
    }
)GEJS"
R"GEJS(
    // The canvas is the SHARED graph in scene mode now -- the property bands are gone -- so there
    // is nothing to draw here; the layer is simply kept clear. (An empty strip used to carry a
    // "click to place the first key" instruction; empty space reads as empty space.)
    function drawSceneStrips() { stripLayer.RemoveAndDeleteChildren(); }

    // (The click-to-type number editor is created on demand inside its own cell; see
    // beginCellEdit / commitEdit in the number-field-typing block.)

    // ---- state + interaction vars (closure) -----------------------------
    var st = null, model = [], G = null, uiscale = 1;
    var lastSig = '', activeCh = 0;
    var prevLmb = false, prevRmb = false, gesture = null;
    var dockScreenX = 0, dockScreenY = 0; // dock top-left in screen layout px (for mouse mapping)
    var didInitTimeline = false;          // one-shot: force the host timeline to its compact view
    var easeMenu = null, easeItemPanels = [], easeRects = []; // right-click ease menu (dock-relative rects)

    function cmd(s) { try { GameInterfaceAPI.ConsoleCommand('mirv_filmmaker ' + (sceneOn() ? 'scenekeys graph ' : 'grapheditor ') + s); } catch (e) {} }
    function rawCmd(s) { try { GameInterfaceAPI.ConsoleCommand(s); } catch (e) {} }

)GEJS"
R"GEJS(
    // ---- the visible time window (owned HERE) ---------------------------
    // C++ publishes viewT0/viewT1 as a SUGGESTED fit (the key span, or the playhead when there are
    // no keys yet) plus a viewEpoch that advances whenever that suggestion should be re-applied
    // (dock enter, reseed, clear). After the seed, the JS owns pan and zoom outright. That split is
    // the whole point: the old window was anchored to the playhead, so scrubbing away from a key
    // dragged the view out from under it and made existing keyframes impossible to find. Now
    // moving the playhead NEVER moves the view; only the navigator, the scrollbar, the zoom
    // buttons and Ctrl/Shift+wheel do.
    var view = { t0: 0, t1: 1, mode: '', epoch: null };
    var MIN_SPAN = 24;
    function domainRange() {
      var d0 = num(st.domainT0), d1 = num(st.domainT1);
      if (!(d1 > d0)) {
        // No published domain (early frame / standalone): the view plus the playhead is enough to
        // draw a navigator that is not obviously wrong.
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
      // HARD CLAMP to the demo. There are no ticks before 0 and none past the last one, so the
      // ruler must never be allowed to show "-5000" or run off the end of the demo. The old
      // code allowed 5% of travel past each end, which is where the negative ticks came from.
      if (view.t0 < d.t0) { view.t1 += d.t0 - view.t0; view.t0 = d.t0; }
      if (view.t1 > d.t1) { view.t0 -= view.t1 - d.t1; view.t1 = d.t1; }
      if (view.t0 < d.t0) view.t0 = d.t0;
    }
    function afterViewChange(automatic) {
      lastSig = '';
      if (!automatic) {
        // Manual navigation owns the window until playback is restarted. Updating the
        // playback latch also covers a drag on the very first playing frame.
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
    // While the demo PLAYS, page the window so the playhead stays on screen. Scrubbing never does
    // this -- that is the whole point of the seed/epoch split above -- but during playback a
    // playhead that has left the window makes the graph look frozen, which is exactly "when I
    // play, nothing happens".
    //
    // It does NOT yank the window to a playhead that was never in view: opening the graph on a
    // keyed shot while the playhead sits somewhere else keeps the keys visible. It pages on the
    // rising edge of PLAY (starting playback should show the playhead) and whenever the playhead
    // crosses out of the window during playback. Manual pan/zoom suspends this following
    // until the next playback start, so a navigator drag can leave the playhead off screen.
    // ---- time snap --------------------------------------------------------
    // ONE shared step for both graph modes, persisted by the settings store as
    // editor.graph.snap. It quantises where things LAND -- the playhead, key placement and key
    // drags -- and nothing else. Alt bypasses it for one gesture (Shift is axis-lock/straighten
    // and Ctrl is wheel-zoom in this dock, so Alt is the free modifier).
    function snapSetting() { return (st && st.snap) ? ('' + st.snap) : 'off'; }
    function niceSnap(v) {
      // 1-2-5 decade ladder, so an Auto step is always a round number.
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
    // One place the ruler/canvas scrub asks "what tick is under the cursor" so the snap can never
    // be forgotten on one path but not another.
    function scrubTick(clx) { return snapTick(xToTick(clx)); }
    function snapLabel() {
      var s = snapSetting();
      if (s === 'off' || s === '0') return 'SNAP';
      if (s === 'auto') return 'SNAP AUTO';
      return 'SNAP ' + s;
    }
    function setSnap(v) {
      v = ('' + v);
      rawCmd('mirv_filmmaker settings ui editor.graph.snap ' + v);
      if (st) st.snap = v;   // show it now; C++ echoes the same value next frame
      lastSig = ''; navSig = '';
    }

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
      view.t0 = (p > view.t1) ? (p - span * 0.12) : (p - span * 0.88);
      view.t1 = view.t0 + span;
      normalizeView(); afterViewChange(true);
      playWasInView = true;
    }

    // ---- coordinate maps (canvas-local layout px) -----------------------
    function tickToX(t) { return (t - view.t0) / (view.t1 - view.t0) * G.cw; }
    function xToTick(x) { return view.t0 + (x / G.cw) * (view.t1 - view.t0); }
    // The overview maps the WHOLE demo (domain), not the visible window.
    function domToX(t) { var d = domainRange(); return (t - d.t0) / Math.max(1, d.t1 - d.t0) * G.cw; }
    function xToDom(x) { var d = domainRange(); return d.t0 + (x / G.cw) * Math.max(1, d.t1 - d.t0); }
    function ovViewRect() {
      var x0 = domToX(view.t0), x1 = domToX(view.t1);
      if (x0 < 0) x0 = 0; if (x1 > G.cw) x1 = G.cw;
      // A floor of a few px: a deep zoom on a long demo maps the viewport to ~1px, which reads as
      // an empty navigator rather than as "you are looking at a tiny slice".
      if (x1 < x0 + 8) x1 = Math.min(G.cw, x0 + 8);
      return { x0: x0, x1: x1 };
    }

    // ONE set of axes for the whole canvas, in BOTH modes. Scene properties used to get their own
    // stacked band with its own tiny axis, which squashed every curve into ~20px and read as a
    // pile of separate little graphs. They are overlaid exactly like the camera channels now: each
    // curve is scaled to its own [min,max] across the FULL height, so every property is visible at
    // once and fills the area -- solo one (click its name) to see only it.
    function chBand(ch) { return { y0: 0, h: G.ch }; }
    function valToY(v, ch) { var d = ch.max - ch.min; if (Math.abs(d) < 1e-9) d = 1; var b = chBand(ch); return b.y0 + (1 - (v - ch.min) / d) * b.h; }
    function yToVal(y, ch) { var d = ch.max - ch.min; if (Math.abs(d) < 1e-9) d = 1; var b = chBand(ch); return ch.min + (1 - (y - b.y0) / b.h) * d; }
    function tickDelta(dxLayout) { return dxLayout / G.cw * (view.t1 - view.t0); }
    function valDelta(dyLayout, ch) { var d = ch.max - ch.min; if (Math.abs(d) < 1e-9) d = 1; return -(dyLayout / chBand(ch).h) * d; }

    function computeGeom() {
      // Measure the screen from the ALREADY-laid-out HUD context panel first. Our own root is
      // created fresh on each (re)build and reports actuallayoutwidth==0 until the next layout pass
      // -- and a resolution switch re-lays-out the whole tree -- so for ~half a second root was 0
      // and the old hardcoded 1280/720 fallback squished the editor into the top-left until it
      // settled. ctx already has a valid full-screen size on frame one, killing that transient.
      uiscale = root.actualuiscale_x || ctx.actualuiscale_x || 1;
      var rawW = root.actuallayoutwidth || 0, rawH = root.actuallayoutheight || 0;
      if (rawW < 16) { rawW = ctx.actuallayoutwidth || 0; rawH = ctx.actuallayoutheight || 0; }
      if (rawW < 16) return G != null; // no valid size yet: keep last-good geom, retry next frame
      var scrW = rawW / uiscale;
      var scrH = (rawH > 16 ? rawH : rawW * 0.5625) / uiscale;
      // The graph editor REPLACES the camera timeline (the host hides it while we're open), so we
      // sit flush at the bottom and fill the whole bottom panel -- no timeline bar to sit above.
      var dockW = Math.max(360, (st && st.renderOpen) ? scrW : scrW - graphInspectorW(scrW));
      dockScreenX = 0; dockScreenY = Math.max(0, scrH - DOCK_H - BOTTOM_LIFT);
      // Size + place the dock within the full-screen root (bottom-left, flush to the screen bottom).
      dock.style.width = dockW + 'px'; dock.style.height = DOCK_H + 'px';
      dock.style.marginBottom = BOTTOM_LIFT + 'px';
      // Vertical stack, top to bottom: header, overview navigator, then the editing area with the
      // time ruler OVERLAID on its top edge, then the bottom scrollbar.
      //
      // The ruler used to be its own band above the canvas, and because rows line up with their
      // bands the left column reserved that band too -- a 26px hole above the first property in
      // both modes. Overlaying the ruler on the canvas (AE does the same: the ruler sits on the
      // top of the graph, it is not a row of its own) puts the first property row directly under
      // the header and hands the reclaimed 26px to the editing area.
      var ovY = HEADER_H, rulerY = HEADER_H + OVERVIEW_H, canvasY = HEADER_H + OVERVIEW_H;
      G = { rw: dockW, rh: DOCK_H, headerH: HEADER_H, rulerH: RULER_H, ovH: OVERVIEW_H,
            ovY: ovY, rulerY: rulerY,
            cx: LEFTW, cy: canvasY,
            cw: Math.max(120, dockW - LEFTW),
            ch: Math.max(80, DOCK_H - HEADER_H - OVERVIEW_H - FOOTER_H) };
      left.style.height = (DOCK_H - HEADER_H) + 'px';
      overview.style.position = LEFTW + 'px ' + ovY + 'px 0px';
      overview.style.width = (dockW - LEFTW) + 'px'; overview.style.height = OVERVIEW_H + 'px';
      ruler.style.position = LEFTW + 'px ' + rulerY + 'px 0px';
      ruler.style.width = (dockW - LEFTW) + 'px'; ruler.style.height = RULER_H + 'px';
      ruler.style.zIndex = '4';   // over the canvas it now shares its top edge with
      canvas.style.position = G.cx + 'px ' + G.cy + 'px 0px';
      canvas.style.width = G.cw + 'px'; canvas.style.height = G.ch + 'px';
      // The playhead runs the full editing area; it draws above the ruler (z 5 > 4) so it reads as
      // one cursor across both.
      phLine.style.height = G.ch + 'px';
      bottomBar.style.position = G.cx + 'px ' + (DOCK_H - FOOTER_H) + 'px 0px';
      bottomBar.style.width = G.cw + 'px'; bottomBar.style.height = FOOTER_H + 'px';
      zoomOut.style.position = '0px 0px 0px'; zoomOut.style.height = FOOTER_H + 'px';
      zoomIn.style.position = (G.cw - ZOOM_W) + 'px 0px 0px'; zoomIn.style.height = FOOTER_H + 'px';
      sbTrack.style.position = (ZOOM_W + 10) + 'px ' + Math.round((FOOTER_H - SB_H) / 2) + 'px 0px';
      sbTrack.style.width = Math.max(40, G.cw - 2 * (ZOOM_W + 10)) + 'px'; sbTrack.style.height = SB_H + 'px';
      return true;
    }

    function curCursor() {
      // Streamed cursor is screen-space; make it dock-relative so it lines up with the dock-
      // relative panel layout (the dock is no longer pinned to the screen origin).
      var lx = (st.mx || 0) / uiscale - dockScreenX, ly = (st.my || 0) / uiscale - dockScreenY;
      return { lx: lx, ly: ly, clx: lx - G.cx, cly: ly - G.cy };
    }
    function chById(c) { for (var i = 0; i < model.length; i++) if (model[i].ch === c) return model[i]; return null; }
    function keyById(ch, id) { for (var i = 0; i < ch.keys.length; i++) if (ch.keys[i].id === id) return ch.keys[i]; return null; }
    function keyIndex(ch, id) { for (var i = 0; i < ch.keys.length; i++) if (ch.keys[i].id === id) return i; return -1; }

    // ---- keyframe navigation -------------------------------------------------
    // The AE workflow, per property: move the playhead, set a value, press the diamond; the
    // diamond is filled when the playhead is already on a key, and << / >> step key to key. None
    // of it depends on clicking empty canvas space to create the first key.
    function seekTo(t) {
      // Move the SHARED playhead and the demo with it, then hand the playhead back to the demo
      // clock. Same three-command shape as a ruler release, so a jump-to-key behaves exactly like
      // a scrub that ended on that tick.
      t = Math.max(0, Math.round(t));
      cmd('playhead ' + t);
      rawCmd('mirv_filmmaker scrub release ' + t);
      cmd('playhead release');
    }
    function nearestKeyTick(keys, t, dir) {
      var best = null;
      for (var i = 0; i < keys.length; i++) {
        var kt = keys[i].t;
        if (dir < 0) { if (kt < t - 0.25 && (best === null || kt > best)) best = kt; }
        else { if (kt > t + 0.25 && (best === null || kt < best)) best = kt; }
      }
      if (best === null && keys.length) best = (dir < 0) ? keys[keys.length - 1].t : keys[0].t;
      return best;
    }
    function toggleChannelKey(c) {
      var ch = chById(c); if (!ch) return;
      var at = keyAtTick(ch, st.playhead);
      if (at) { cmd('delkey ' + c + ' ' + at.id); return; }
      var v = (typeof ch.cur === 'number' && isFinite(ch.cur)) ? ch.cur : sampleChan(ch, st.playhead);
      if (v == null || !isFinite(v)) v = 0;
      cmd('addkey ' + c + ' ' + snapTick(st.playhead).toFixed(2) + ' ' + v.toFixed(3));
    }
    function jumpChannelKey(c, dir) {
      var ch = chById(c); if (!ch || !ch.keys.length) return;
      var t = nearestKeyTick(ch.keys, st.playhead, dir);
      if (t != null) seekTo(t);
    }
    // Scene strips have no channel index the graph knows: the diamond keys the strip's own field.
    // graphadd samples the current editable value, so it is the same "key using the value you are
    // looking at" contract as the camera channels' addkey.
    function toggleSceneKey(ch) {
      if (!ch) return;
      var at = keyAtTick(ch, st.playhead);
      if (at) { cmd('select ' + ch.ch + ' ' + at.id); cmd('delsel'); return; }
      rawCmd('mirv_filmmaker scenekeys graphadd ' + ch.fieldId + ' ' + Math.round(snapTick(st.playhead)));
    }
    function jumpSceneKey(ch, dir) {
      if (!ch || !ch.keys.length) return;
      var t = nearestKeyTick(ch.keys, st.playhead, dir);
      if (t != null) seekTo(t);
    }
)GEJS"
R"GEJS(
    // ---- hit-testing ----------------------------------------------------
    // Region tests run on the DOCK-relative cursor: the overview, the ruler and the bottom bar all
    // start at G.cx and share its width, so their canvas-local x is the same number.
    function inOverview(clx, cly) { return clx >= 0 && clx <= G.cw && cly >= G.ovY && cly <= G.ovY + OVERVIEW_H; }
    function inBottomBar(lx, ly) { return lx >= G.cx && lx <= G.cx + G.cw && ly >= DOCK_H - FOOTER_H && ly <= DOCK_H; }
    // Scrollbar track in DOCK coordinates, plus the thumb's current rect (set by layoutNav).
    function sbTrackRect() {
      var x0 = G.cx + ZOOM_W + 10, x1 = G.cx + G.cw - ZOOM_W - 10;
      return { x0: x0, x1: Math.max(x0 + 40, x1) };
    }
    var sbThumbX0 = 0, sbThumbW = 40;
    function hitKeyframe(clx, cly) {
      var best = null, bd = KF_R * KF_R;
      for (var i = 0; i < model.length; i++) {
        var ch = model[i]; if (!ch.vis || !sceneStripped(ch)) continue;
        for (var j = 0; j < ch.keys.length; j++) {
          var k = ch.keys[j]; var x = tickToX(k.t), y = valToY(k.v, ch);
          var dx = x - clx, dy = y - cly, d = dx * dx + dy * dy;
          if (d <= bd) { bd = d; best = { ch: ch.ch, id: k.id }; }
        }
      }
      return best;
    }
    function handleEndpoint(ch, k, side) {
      var idx = keyIndex(ch, k.id);
      var defSpan = (view.t1 - view.t0) * 0.06;
      if (side > 0) {
        var nx = (idx + 1 < ch.keys.length) ? ch.keys[idx + 1] : null;
        var span = nx ? (nx.t - k.t) : defSpan; var tx = (k.cr && k.cr.a) ? k.cr.tx : 1 / 3;
        var dv = (k.cr && k.cr.a) ? k.cr.dv : 0;
        return { t: k.t + clamp01(tx) * span, v: k.v + dv, span: span };
      } else {
        var pv = (idx - 1 >= 0) ? ch.keys[idx - 1] : null;
        var span2 = pv ? (k.t - pv.t) : defSpan; var tx2 = (k.cl && k.cl.a) ? k.cl.tx : 1 / 3;
        var dv2 = (k.cl && k.cl.a) ? k.cl.dv : 0;
        return { t: k.t - clamp01(tx2) * span2, v: k.v + dv2, span: span2 };
      }
    }
    function hitHandle(clx, cly) {
      var best = null, bd = HANDLE_R * HANDLE_R;
      for (var i = 0; i < model.length; i++) {
        var ch = model[i]; if (!ch.vis || stepped(ch) || !sceneStripped(ch)) continue;
        for (var j = 0; j < ch.keys.length; j++) {
          var k = ch.keys[j]; if (!k.sel) continue;
          [-1, 1].forEach(function (side) {
            var e = handleEndpoint(ch, k, side); var x = tickToX(e.t), y = valToY(e.v, ch);
            var dx = x - clx, dy = y - cly, d = dx * dx + dy * dy;
            if (d <= bd) { bd = d; best = { ch: ch.ch, id: k.id, side: side, span: e.span }; }
          });
        }
      }
      return best;
    }
    function keysInRect(g) {
      var x0 = Math.min(g.sx, g.cx), x1 = Math.max(g.sx, g.cx);
      var y0 = Math.min(g.sy, g.cy), y1 = Math.max(g.sy, g.cy);
      var out = [];
      for (var i = 0; i < model.length; i++) {
        var ch = model[i]; if (!ch.vis || !sceneStripped(ch)) continue;
        for (var j = 0; j < ch.keys.length; j++) {
          var k = ch.keys[j]; var x = tickToX(k.t), y = valToY(k.v, ch);
          // space-separated "ch id" so the flat token list survives the console tokenizer
          // (a single "ch:id,ch:id" token gets split on the comma and loses all but the first).
          if (x >= x0 && x <= x1 && y >= y0 && y <= y1) out.push(ch.ch + ' ' + k.id);
        }
      }
      return out;
    }

    // ---- number field typing -------------------------------------------
    // Hit-test the left-column value cells by their known absolute rects (layout px). No reliance
    // on onmouseover (which the in-game HUD context does not reliably deliver to created panels).
    function numFieldHit(lx, ly) {
      if (sceneOn()) return -1;
      for (var c = 0; c < rows.length; c++) {
        var r = rows[c].valRect;
        if (lx >= r.x0 && lx <= r.x1 && ly >= r.y0 && ly <= r.y1) return c;
      }
      return -1;
    }
    function fieldValue(c) { var ch = chById(c); if (!ch) return 0; var v = sampleChan(ch, st.playhead); return v == null ? 0 : v; }
    function fieldSens(c) { var ch = chById(c); if (!ch) return 0.1; return (ch.max - ch.min) / 300; }

    // The number you clicked is where you type. The TextEntry is created INSIDE the value cell and
    // destroyed on commit, so it can never land anywhere but on that number -- the old floating
    // box was positioned in dock space and, in game, appeared at the bottom of the dock -- and
    // destroying it releases keyboard focus. A hidden-but-still-focused TextEntry was swallowing
    // Space, which is why the demo would not play after a value had been typed.
    var edit = null; // { entry, label, rect, onCommit, done }
    function clampToSpec(ch, v) {
      if (!ch || !isFinite(v)) return v;
      if (typeof ch.min === 'number' && v < ch.min) v = ch.min;
      if (typeof ch.max === 'number' && v > ch.max) v = ch.max;
      return v;
    }
    function beginCellEdit(host, label, initial, rect, onCommit) {
      if (edit) commitEdit();
      if (!host) return;
      var e = $.CreatePanel('TextEntry', host, 'GraphExpType', {});
      e.hittest = true;
      e.style.width = '100%'; e.style.height = '100%';
      e.style.fontSize = '12px'; e.style.textAlign = 'right';
      e.style.color = S.accent; e.style.backgroundColor = 'rgba(0,0,0,0.65)';
      e.style.border = '1px solid ' + S.accent; e.style.borderRadius = '3px';
      e.text = initial;
      edit = { entry: e, label: label, rect: rect, onCommit: onCommit, done: false };
      if (label) label.visible = false;
      e.SetPanelEvent('oninputsubmit', function () { commitEdit(); });
      e.SetPanelEvent('onblur', function () { commitEdit(); });
      e.SetFocus(); try { e.RaiseSelectAll && e.RaiseSelectAll(); } catch (err) {}
    }
    function commitEdit() {
      if (!edit || edit.done) return;
      edit.done = true;
      var e = edit.entry, cb = edit.onCommit, label = edit.label, v = parseFloat(e.text);
      edit = null;
      if (label) label.visible = true;
      try { e.DeleteAsync(0); } catch (err) {}   // deletion is what actually drops the focus grab
      hideTip();
      if (isFinite(v) && cb) cb(v);
    }
    function editHit(lx, ly) {
      if (!edit || !edit.rect) return false;
      var r = edit.rect;
      return lx >= r.x0 - 2 && lx <= r.x1 + 2 && ly >= r.y0 - 2 && ly <= r.y1 + 2;
    }
    function openSceneType(cell) {
      var ch = chById(cell.ch);
      var rec = sceneValues[cell.fieldId];
      var initial = (ch && typeof ch.value === 'number') ? String(Math.round(ch.value * 1000) / 1000) : '0';
      beginCellEdit(cell.box, rec && rec.cell, initial, cell.rect, function (v) {
        rawCmd('mirv_filmmaker scenekeys set ' + cell.fieldId + ' ' + clampToSpec(ch, v).toFixed(3));
      });
    }
    function openType(c) {
      var ch = chById(c);
      beginCellEdit(rows[c].valBox, rows[c].val, fieldValue(c).toFixed(2), rows[c].valRect, function (v) {
        v = clampToSpec(ch, v);
        cmd('editbegin'); cmd('addkey ' + c + ' ' + snapTick(st.playhead).toFixed(2) + ' ' + v.toFixed(3)); if (sceneOn()) cmd('editend');
      });
    }

    // ---- right-click selection menu (applies to the WHOLE selection) ----
    // "Align Keys" snaps every selected key to the right-clicked key's tick (vertical sync
    // across channels); the ease items shape the selection's tangents; Delete removes it.
    var easeAlignCh = -1, easeAlignId = -1; // key the menu was opened on (the Align target)
    // ONE flowed column, not five absolutely-placed rows over a floating caption. The old build
    // offset each row with style.position inside a non-flowing parent, which put the SELECTION
    // caption on top of the third item -- a context menu that reads as a rendering fault.
    // EM_* are shared by the layout and by the hit rects, so the two can never drift.
    var EM_W = 214, EM_PAD = 5, EM_HEAD = 22, EM_ITEM = 27, EM_SEP = 7;
    var EASE_ITEMS = [['in', 'Ease In'], ['out', 'Ease Out'], ['inout', 'Ease In / Out'],
                      ['align', 'Align Keys'], ['delete', 'Delete Key', 1]];
    var menuItems = EASE_ITEMS, menuPick = null;
    function menuItemY(i) {
      var y = EM_PAD + EM_HEAD;
      for (var j = 0; j < i; j++) y += EM_ITEM + (menuItems[j + 1] && menuItems[j + 1][2] ? EM_SEP : 0);
      return y;
    }
    function menuH() { return menuItemY(menuItems.length - 1) + EM_ITEM + EM_PAD; }
    // ONE menu panel, rebuilt for whichever list is being shown. Two hand-built menus would drift
    // apart in a week, and a context menu that does not match the one next to it looks broken.
    function buildMenu(title) {
      if (!easeMenu) {
        easeMenu = mk('Panel', dock); easeMenu.hittest = false; easeMenu.style.zIndex = '20';
        easeMenu.style.flowChildren = 'down';
        easeMenu.style.paddingTop = EM_PAD + 'px'; easeMenu.style.paddingBottom = EM_PAD + 'px';
        easeMenu.style.backgroundColor = 'rgba(16,20,26,0.99)';
        easeMenu.style.border = '1px solid ' + S.border;
        easeMenu.style.borderRadius = '5px';
        easeMenu.style.boxShadow = '#000000aa 0px 6px 18px 0px';
      }
      easeMenu.RemoveAndDeleteChildren();
      easeMenu.style.width = EM_W + 'px'; easeMenu.style.height = menuH() + 'px';
      var head = mk('Panel', easeMenu); head.hittest = false;
      head.style.width = '100%'; head.style.height = EM_HEAD + 'px'; head.style.paddingLeft = '11px';
      var t = lbl(head, title, S.hint, 10);
      t.hittest = false; t.style.verticalAlign = 'center'; t.style.whiteSpace = 'nowrap';
      t.style.fontWeight = 'bold'; t.style.letterSpacing = '1.4px';
      easeItemPanels = [];
      for (var i = 0; i < menuItems.length; i++) {
        var danger = !!menuItems[i][2];
        if (danger) {
          var sep = mk('Panel', easeMenu); sep.hittest = false;
          sep.style.width = '100%'; sep.style.height = '1px';
          sep.style.marginTop = '3px'; sep.style.marginBottom = '3px';
          sep.style.backgroundColor = S.border;
        }
        var it = mk('Panel', easeMenu); it.hittest = false;
        it.style.width = (EM_W - 2 * EM_PAD) + 'px'; it.style.height = EM_ITEM + 'px';
        it.style.horizontalAlign = 'center'; it.style.borderRadius = '3px'; it.style.paddingLeft = '11px';
        var l = lbl(it, menuItems[i][1], danger ? S.dangerText : S.value, 13);
        l.hittest = false; l.style.verticalAlign = 'center'; l.style.whiteSpace = 'nowrap';
        easeItemPanels.push(it);
      }
    }
)GEJS"
R"GEJS(
    function showMenu(lx, ly, title, items, onPick) {
      menuItems = items; menuPick = onPick;
      buildMenu(title);
      var W = EM_W, H = menuH();
      if (lx + W > G.rw) lx = G.rw - W - 2; if (lx < 0) lx = 0;
      if (ly + H > G.rh) ly = G.rh - H - 2; if (ly < 0) ly = 0;
      easeMenu.style.position = lx + 'px ' + ly + 'px 0px'; easeMenu.visible = true;
      easeRects = [];
      for (var i = 0; i < items.length; i++) {
        var y = ly + menuItemY(i);
        easeRects.push({ x0: lx + EM_PAD, x1: lx + W - EM_PAD, y0: y, y1: y + EM_ITEM, mode: items[i][0] });
      }
    }
    // The strip's own menu: what you can do to THIS property at THIS tick, reachable where you are
    // already pointing instead of only from a keyframe you may not have yet.
    function showStripMenu(lx, ly, cell) {
      var ch = chById(cell.ch), at = keyAtTick(ch, st.playhead);
      var items = [['stripkey', at ? 'Re-key at playhead' : 'Key at playhead']];
      if (at) items.push(['stripdel', 'Delete key at playhead']);
      items.push(['stripoff', 'Turn this strip off', 1]);
      showMenu(lx, ly, (ch ? ch.name.toUpperCase() : 'STRIP'), items, function (mode) {
        if (mode === 'stripkey') rawCmd('mirv_filmmaker scenekeys graphadd ' + cell.fieldId + ' ' + Math.round(snapTick(st.playhead)));
        else if (mode === 'stripdel' && at) { cmd('select ' + cell.ch + ' ' + at.id); cmd('delsel'); }
        else if (mode === 'stripoff') rawCmd('mirv_filmmaker scenekeys clear ' + cell.fieldId);
      });
    }
    // Right-click on the property LIST (left of the curves): the whole property, or with a group
    // header the whole group. Disable keeps the keys but stops applying them (`scenekeys mute`);
    // Delete removes the keys and the strip (`scenekeys clear`, undoable like every scene edit).
    function sceneRowHit(lx, ly) {
      if (!sceneOn() || lx < 0 || lx > LEFTW) return null;
      var top = G.ovH + HEADER_H, y = ly - top;
      if (ly < top || ly > top + G.ch) return null;
      for (var g = 0; g < sceneGroups.length; g++) {
        var grp = sceneGroups[g];
        if (y >= grp.y0 && y < grp.y0 + grp.h) return { group: grp };
      }
      for (var i = 0; i < sceneStrips.length; i++) {
        var sp = sceneStrips[i];
        if (y >= sp.y0 && y < sp.y0 + sp.h) return { strip: sp };
      }
      return null;
    }
    function showSceneRowMenu(lx, ly, hit) {
      var target, title, allMuted = true, groupWord = '';
      if (hit.group) {
        var g = hit.group, first = g.items.length ? model[g.items[0]] : null;
        if (!first) return;
        target = first.groupId; title = g.title.toUpperCase(); groupWord = ' group';
        for (var i = 0; i < g.items.length; i++) if (!model[g.items[i]].muted) allMuted = false;
      } else {
        var ch = chById(hit.strip.ch);
        if (!ch) return;
        target = ch.fieldId; title = ch.name.toUpperCase(); allMuted = !!ch.muted;
      }
      var items = [['mute', allMuted ? 'Enable' + groupWord : 'Disable' + groupWord], ['delete', 'Delete' + groupWord, 1]];
      showMenu(lx, ly, title, items, function (mode) {
        if (mode === 'mute') rawCmd('mirv_filmmaker scenekeys mute ' + target + (allMuted ? ' off' : ' on'));
        else if (mode === 'delete') rawCmd('mirv_filmmaker scenekeys clear ' + target);
      });
    }
    function keyAtTick(ch, t) {
      if (!ch) return null;
      var want = Math.round(t);
      for (var i = 0; i < ch.keys.length; i++) if (Math.round(ch.keys[i].t) === want) return ch.keys[i];
      return null;
    }
    function showEaseMenu(lx, ly, ch, id) {
      easeAlignCh = ch; easeAlignId = id;
      showMenu(lx, ly, 'SELECTED KEYS', EASE_ITEMS, function (em) {
        if (em === 'delete') cmd('delsel');
        else if (em === 'align') cmd('align ' + easeAlignCh + ' ' + easeAlignId);
        else cmd('ease ' + em);
      });
    }
    function hideEaseMenu() { if (easeMenu) { easeMenu.visible = false; easeRects = []; } }
    function easeMenuOpen() { return easeMenu && easeMenu.visible; }
    function easeHover(lx, ly) {
      for (var i = 0; i < easeRects.length; i++) {
        var inIt = (lx >= easeRects[i].x0 && lx <= easeRects[i].x1 && ly >= easeRects[i].y0 && ly <= easeRects[i].y1);
        if (easeItemPanels[i]) easeItemPanels[i].style.backgroundColor =
          inIt ? (easeRects[i].mode === 'delete' ? S.dangerBg : S.btnOn) : 'rgba(0,0,0,0)';
      }
    }
    function easeHit(lx, ly) {
      for (var i = 0; i < easeRects.length; i++)
        if (lx >= easeRects[i].x0 && lx <= easeRects[i].x1 && ly >= easeRects[i].y0 && ly <= easeRects[i].y1) return easeRects[i].mode;
      return null;
    }

)GEJS"
R"GEJS(
    // ---- the mouse-driven interaction engine ----------------------------
    // Hover dressing for the navigator panels, cached so a per-frame call does not rewrite styles.
    var ovVis = '', sbVis = 0;
    function setOvVisual(hot, edge) {
      if (gesture && gesture.type === 'ovL') edge = -1;
      else if (gesture && gesture.type === 'ovR') edge = 1;
      var sig = (hot ? 1 : 0) + '' + edge;
      if (sig === ovVis) return;
      ovVis = sig;
      var lit = hot || edge !== 0;
      ovView.style.border = '1px solid ' + (lit ? S.ovViewBorderHot : S.ovViewBorder);
      ovView.style.backgroundColor = lit ? S.ovViewHot : S.ovView;
      var activeEdge = edge;
      ovLeftGrip.style.backgroundColor = activeEdge === -1 ? S.accent : '#303740ff';
      ovRightGrip.style.backgroundColor = activeEdge === 1 ? S.accent : '#303740ff';
      ovLeftGrip.style.border = '1px solid ' + (activeEdge === -1 ? S.accent : '#bac4d0ff');
      ovRightGrip.style.border = '1px solid ' + (activeEdge === 1 ? S.accent : '#bac4d0ff');
    }
    function setSbVisual(hot) {
      var v = hot ? 1 : 0;
      if (v === sbVis) return; sbVis = v;
      sbThumb.style.backgroundColor = hot ? '#ffffff59' : '#ffffff33';
    }

    function processInput() {
      var lmb = !!st.lmb, rmb = !!st.rmb; var cc = curCursor();
      var inCanvas = (cc.clx >= 0 && cc.clx <= G.cw && cc.cly >= 0 && cc.cly <= G.ch);
      var inRuler = (cc.lx >= G.cx && cc.lx <= G.cx + G.cw && cc.ly >= G.rulerY && cc.ly <= G.rulerY + G.rulerH);
      var overOverview = inOverview(cc.clx, cc.ly);
      var overBar = inBottomBar(cc.lx, cc.ly);
      var nearPlayhead = inCanvas && Math.abs(cc.clx - tickToX(st.playhead)) <= 6;

      // Navigator hover dressing. The viewport lights as a whole when the pointer is anywhere over
      // the map; an edge that would resize it gets the accent so the grab is discoverable.
      var ovEdge = 0;
      if (overOverview) {
        var vr0 = ovViewRect();
        if (Math.abs(cc.clx - vr0.x0) <= 7) ovEdge = -1;
        else if (Math.abs(cc.clx - vr0.x1) <= 7) ovEdge = 1;
      }
      setOvVisual(overOverview, ovEdge);
      var sbHot = false;
      if (overBar) {
        var sr0 = sbTrackRect();
        if (cc.lx >= sr0.x0 - 4 && cc.lx <= sr0.x1 + 4) sbHot = true;
      }
      setSbVisual(sbHot || (gesture && (gesture.type === 'sbL' || gesture.type === 'sbR' || gesture.type === 'sbMove')));

      // Right-click opens the ease menu on a keyframe; it applies to the whole selection (the
      // clicked key is added to the selection if it wasn't already part of it).
      if (rmb && !prevRmb) {
        hideSnapMenu();
        var rk = inCanvas ? hitKeyframe(cc.clx, cc.cly) : null;
        var rcell = rk ? null : sceneCellHit(cc.lx, cc.ly);
        var rrow = (rk || rcell) ? null : sceneRowHit(cc.lx, cc.ly);
        if (rk) { if (!isSelectedId(rk.ch, rk.id)) cmd('select ' + rk.ch + ' ' + rk.id); showEaseMenu(cc.lx, cc.ly, rk.ch, rk.id); }
        else if (rcell) showStripMenu(cc.lx, cc.ly, rcell);
        else if (rrow) showSceneRowMenu(cc.lx, cc.ly, rrow);
        else hideEaseMenu();
      }
      if (easeMenuOpen()) easeHover(cc.lx, cc.ly);
      prevRmb = rmb;

      if (lmb && !prevLmb) {
        // The colour wheel owns the pointer while it is open: it is a real Panorama popover with
        // its own drag pump, so the raw pipe must not also start a box-select or grab a keyframe
        // under it. cfgColorPump closes it on a genuine click-away.
        if (cfgColorOwner) { prevLmb = lmb; return; }
        // The snap dropdown's rows own their own clicks; a press anywhere else closes it. Its
        // Custom… field is a real TextEntry, so the raw pipe must not start a gesture over it.
        if (snapMenuOpen()) {
          if (!(snapRect && cc.lx >= snapRect.x0 && cc.lx <= snapRect.x1 && cc.ly >= snapRect.y0 && cc.ly <= snapRect.y1)) hideSnapMenu();
          prevLmb = lmb; return;
        }
        // While the ease menu is open a click picks an item (or closes it), and is consumed.
        if (easeMenuOpen()) {
          var em = easeHit(cc.lx, cc.ly);
          if (em && menuPick) menuPick(em);
          hideEaseMenu(); prevLmb = lmb; return;
        }
        // An open in-cell editor owns its own clicks; a press anywhere else commits it first, and
        // the raw pipe starts no gesture while the editor still holds the click.
        if (edit) { if (editHit(cc.lx, cc.ly)) { prevLmb = lmb; return; } commitEdit(); }
        // ---- timeline overview navigator: body pans, a grip resizes, empty track jumps.
        if (overOverview) {
          var vr = ovViewRect(), span = view.t1 - view.t0;
          if (Math.abs(cc.clx - vr.x0) <= 7) gesture = { type: 'ovL' };
          else if (Math.abs(cc.clx - vr.x1) <= 7) gesture = { type: 'ovR' };
          else if (cc.clx > vr.x0 && cc.clx < vr.x1) gesture = { type: 'ovMove', startX: cc.clx, startT0: view.t0, startT1: view.t1 };
          else {
            var ct = xToDom(cc.clx);
            view.t0 = ct - span / 2; view.t1 = view.t0 + span; normalizeView(); afterViewChange();
            gesture = { type: 'ovMove', startX: cc.clx, startT0: view.t0, startT1: view.t1 };
          }
          prevLmb = lmb; return;
        }
        // ---- bottom scrollbar: the same grammar through the whole demo (thumb width = zoom).
        if (overBar) {
          var sr = sbTrackRect();
          if (cc.lx >= sr.x0 - 4 && cc.lx <= sr.x1 + 4) {
            if (Math.abs(cc.lx - sbThumbX0) <= 6) gesture = { type: 'sbL' };
            else if (Math.abs(cc.lx - (sbThumbX0 + sbThumbW)) <= 6) gesture = { type: 'sbR' };
            else if (cc.lx > sbThumbX0 && cc.lx < sbThumbX0 + sbThumbW) gesture = { type: 'sbMove', startX: cc.clx, startT0: view.t0, startT1: view.t1 };
            else {
              var span2 = view.t1 - view.t0, ct2 = xToDom(cc.clx);
              view.t0 = ct2 - span2 / 2; view.t1 = view.t0 + span2; normalizeView(); afterViewChange();
              gesture = { type: 'sbMove', startX: cc.clx, startT0: view.t0, startT1: view.t1 };
            }
          }
          prevLmb = lmb; return;
        }
        if (inCanvas) {
          var h = hitHandle(cc.clx, cc.cly);
          if (h) { cmd('editbegin'); gesture = { type: 'handle', ch: h.ch, id: h.id, side: h.side }; }
          else {
            var k = hitKeyframe(cc.clx, cc.cly);
            if (k) {
              if (!isSelectedId(k.ch, k.id)) cmd('select ' + k.ch + ' ' + k.id);
              // leadT0 is the CLICKED key's original tick: the snap grid is anchored to it, so a
              // multi-key drag keeps its spacing instead of collapsing every key onto one tick.
              cmd('editbegin'); gesture = { type: 'kf', sx: cc.lx, sy: cc.ly, ldt: 0, ldv: 0, ach: k.ch, leadT0: k.t };
            } else if (inRuler) {
              // The ruler overlays the top of the canvas, so it is checked AFTER the keys/handles
              // that may sit under it -- the ruler is a scrub zone only where nothing is drawn.
              gesture = { type: 'ruler' }; cmd('playhead ' + scrubTick(cc.clx).toFixed(2));
            } else if (nearPlayhead) {
              // grab the red playhead LINE in the canvas to scrub (same as the ruler handle).
              gesture = { type: 'ruler' }; cmd('playhead ' + scrubTick(cc.clx).toFixed(2));
            } else {
              // Empty canvas is box-select. It used to be a create target ("click the strip to
              // place the first key"), which scattered keys wherever the pointer happened to land
              // and made key placement depend on finding empty space. Keys now come from the
              // property row's diamond at the playhead; a drag out here only ever selects.
              gesture = { type: 'rect', sx: cc.clx, sy: cc.cly, cx: cc.clx, cy: cc.cly };
            }
          }
        } else if (inRuler) { gesture = { type: 'ruler' }; cmd('playhead ' + scrubTick(cc.clx).toFixed(2)); }
        else {
          var scell = sceneCellHit(cc.lx, cc.ly);
          if (scell) {
            var sch = chById(scell.ch);
            gesture = { type: 'snum', cell: scell, smx: st.mx, moved: false,
                        sv: (sch && typeof sch.value === 'number') ? sch.value : 0 };
            cmd('editbegin');
          } else { var nf = numFieldHit(cc.lx, cc.ly); if (nf >= 0) { gesture = { type: 'num', ch: nf, smx: st.mx, sv: fieldValue(nf), moved: false }; cmd('editbegin'); } }
        }
      } else if (lmb && prevLmb && gesture) {
        if (gesture.type === 'ovL') { view.t0 = xToDom(cc.clx); normalizeView(); afterViewChange(); }
        else if (gesture.type === 'ovR') { view.t1 = xToDom(cc.clx); normalizeView(); afterViewChange(); }
        else if (gesture.type === 'ovMove') {
          var ovdt = xToDom(cc.clx) - xToDom(gesture.startX);
          view.t0 = gesture.startT0 + ovdt; view.t1 = gesture.startT1 + ovdt; normalizeView(); afterViewChange();
        } else if (gesture.type === 'sbL') { view.t0 = xToDom(cc.clx); normalizeView(); afterViewChange(); }
        else if (gesture.type === 'sbR') { view.t1 = xToDom(cc.clx); normalizeView(); afterViewChange(); }
        else if (gesture.type === 'sbMove') {
          var sbdt = xToDom(cc.clx) - xToDom(gesture.startX);
          view.t0 = gesture.startT0 + sbdt; view.t1 = gesture.startT1 + sbdt; normalizeView(); afterViewChange();
        } else if (gesture.type === 'handle') {
          var chh = chById(gesture.ch); var kk = chh ? keyById(chh, gesture.id) : null; if (kk) {
            var e = handleEndpoint(chh, kk, gesture.side);
            var th = xToTick(cc.clx), vh = yToVal(cc.cly, chh);
            var span = e.span > 1e-9 ? e.span : 1;
            var tx = clamp01(gesture.side > 0 ? (th - kk.t) / span : (kk.t - th) / span);
            var dv = vh - kk.v;
            // Shift = STRAIGHTEN: lock the handle flat (level with the key, dv=0) so the curve runs
            // straight through the keyframe; you still set the arm LENGTH (left/right), just not slope.
            if (st.shift) dv = 0;
            // Keep the handle OUTSIDE the diamond: enforce a minimum pixel distance from the key so
            // the arm/dot can't get swallowed into the diamond and become ungrabbable.
            var spanPx = span / (view.t1 - view.t0) * G.cw, dd = (chh.max - chh.min) || 1;
            var hpx = tx * spanPx, vpx = dv / dd * chBand(chh).h, dist = Math.sqrt(hpx * hpx + vpx * vpx), MIN = KF_R + 8;
            if (dist < MIN) {
              if (dist < 1e-3) { tx = clamp01(MIN / (spanPx || 1)); dv = 0; }
              else { var sc = MIN / dist; tx = clamp01(tx * sc); dv = dv * sc; }
            }
            // No vertical clamp: drag the handle as high/low as you want -- the auto-fit zooms the
            // view out to follow it (and back in when it returns), so you are never walled in.
            // reflect=0: left/right tangents are INDEPENDENT (dragging one never moves the other).
            cmd('handle ' + gesture.ch + ' ' + gesture.id + ' ' + (gesture.side > 0 ? 'right' : 'left') + ' ' + tx.toFixed(3) + ' ' + dv.toFixed(3) + ' 0');
          }
        } else if (gesture.type === 'kf') {
          var ach = chById(gesture.ach) || model[0];
          var dt = tickDelta(cc.lx - gesture.sx), dv2 = valDelta(cc.ly - gesture.sy, ach);
          // Shift = lock to the horizontal axis (timing only): freeze the value delta where it was
          // when Shift went down; rebaseline on release so it resumes without a jump.
          if (st.shift) {
            if (!gesture.shiftLock) { gesture.shiftLock = true; gesture.lockDv = gesture.ldv; }
            dv2 = gesture.lockDv;
          } else if (gesture.shiftLock) {
            gesture.shiftLock = false;
            var dd3 = (ach.max - ach.min) || 1; gesture.sy = cc.ly + gesture.lockDv * chBand(ach).h / dd3; dv2 = gesture.lockDv;
          }
          // Snap the LEAD key to the grid; every other selected key moves by the same delta. With
          // snapping off, scene stays on whole ticks and the camera stays continuous.
          if (gesture.leadT0 != null && snapStep() > 0) dt = snapTick(gesture.leadT0 + dt) - gesture.leadT0;
          else if (sceneOn()) dt = Math.round(dt);
          cmd('movesel ' + (dt - gesture.ldt).toFixed(3) + ' ' + (dv2 - gesture.ldv).toFixed(3));
          gesture.ldt = dt; gesture.ldv = dv2;
        } else if (gesture.type === 'rect') { gesture.cx = cc.clx; gesture.cy = cc.cly; updateRect(gesture); }
        else if (gesture.type === 'ruler') { cmd('playhead ' + scrubTick(cc.clx).toFixed(2)); }
        else if (gesture.type === 'num') {
          var dpx = st.mx - gesture.smx; if (Math.abs(dpx) > 5) gesture.moved = true;
          if (gesture.moved) { var nv = gesture.sv + dpx * fieldSens(gesture.ch); cmd('addkey ' + gesture.ch + ' ' + snapTick(st.playhead).toFixed(2) + ' ' + nv.toFixed(3)); }
        } else if (gesture.type === 'snum') {
          var sdpx = st.mx - gesture.smx; if (Math.abs(sdpx) > 4) gesture.moved = true;
          if (gesture.moved) {
            var sch2 = chById(gesture.cell.ch);
            // Shift = fine, a fifth of the gain, same as every other scrubby control in this UI.
            var sens = ((sch2 ? sch2.max - sch2.min : 1) / 320) * (st.shift ? 0.2 : 1);
            var sv2 = gesture.sv + sdpx * sens;
            if (sch2) sv2 = Math.max(sch2.min, Math.min(sch2.max, sv2));
            rawCmd('mirv_filmmaker scenekeys set ' + gesture.cell.fieldId + ' ' + sv2.toFixed(3));
          }
        }
      } else if (!lmb && prevLmb && gesture) {
        if (gesture.type === 'rect') {
          var sel = keysInRect(gesture); if (sel.length) cmd('selset ' + sel.join(' ')); else cmd('selclear');
          rectVis.visible = false;
        } else if (gesture.type === 'ruler') {
          rawCmd('mirv_filmmaker scrub release ' + Math.round(snapTick(xToTick(curCursor().clx)))); cmd('playhead release');
        } else if (gesture.type === 'num') { if (!gesture.moved) openType(gesture.ch); }
        else if (gesture.type === 'snum') { if (!gesture.moved) openSceneType(gesture.cell); }
        if (sceneOn()) cmd('editend');
        gesture = null;
      }
      prevLmb = lmb;
    }
    var selCache = {};
    function isSelectedId(c, id) { return !!selCache[c + ':' + id]; }
    function updateRect(g) {
      var x0 = Math.min(g.sx, g.cx), x1 = Math.max(g.sx, g.cx), y0 = Math.min(g.sy, g.cy), y1 = Math.max(g.sy, g.cy);
      rectVis.visible = true; rectVis.style.position = x0.toFixed(0) + 'px ' + y0.toFixed(0) + 'px 0px';
      rectVis.style.width = (x1 - x0).toFixed(0) + 'px'; rectVis.style.height = (y1 - y0).toFixed(0) + 'px';
    }
)GEJS"
R"GEJS(
    // ---- drawing --------------------------------------------------------
    function drawGrid() {
      gridLayer.RemoveAndDeleteChildren();
      var ach = chById(activeCh) || model[0];
      // vertical time lines + labels
      var span = view.t1 - view.t0;
      rulerLayer.RemoveAndDeleteChildren();
      var pxPerTick = G.cw / Math.max(1e-9, span);
      var snap = snapStep();
      var labelStep = niceStep(span / 8);
      // Minor lines AT THE SNAP STEP, so the grid shows exactly where a key will stick. Skipped
      // when they would be denser than a few px (or more than ~140 of them); the labelled majors
      // are then the only lines.
      if (snap > 0 && snap * pxPerTick >= 4 && span / snap <= 140) {
        var m0 = Math.ceil(view.t0 / snap) * snap;
        for (var tm = m0; tm <= view.t1; tm += snap) {
          var xm = tickToX(tm); if (xm < 0 || xm > G.cw) continue;
          lineSeg(gridLayer, xm, 0, xm, G.ch, S.grid, 1);
        }
      }
      // Labelled majors: multiples of the snap step when snapping, otherwise the automatic step.
      var major = snap > 0 ? snap * Math.max(1, Math.round(labelStep / snap)) : labelStep;
      var b0 = Math.ceil(view.t0 / major) * major;
      for (var t = b0; t <= view.t1; t += major) {
        var x = tickToX(t); if (x < 0 || x > G.cw) continue;
        lineSeg(gridLayer, x, 0, x, G.ch, S.gridMid, 1);
        var tl = lbl(rulerLayer, '' + Math.round(t), S.hint, 10);
        tl.style.position = (x + 2).toFixed(0) + 'px 6px 0px';
      }
      // Horizontal value lines: ONE shared axis, labelling the ACTIVE property's range across the
      // whole canvas. This is the same in both modes now -- scene curves overlay each other and
      // each is scaled to its own range, exactly like the camera channels.
      if (ach) {
        var lo = typeof ach.min === 'number' ? ach.min : 0, hi = typeof ach.max === 'number' ? ach.max : 1;
        var coarse = Math.abs(hi - lo) >= 100;
        for (var gi = 0; gi <= 4; gi++) {
        var y = gi / 4 * G.ch; lineSeg(gridLayer, 0, y, G.cw, y, gi === 2 ? S.gridMid : S.grid, 1);
          // CENTRED ON ITS OWN LINE, in a fixed right-aligned gutter. Positioning a Label by its
          // TOP left every number sitting a half-line below the rule it belongs to; a sized box
          // with the label aligned inside it is what keeps the numbers on their rules.
          var box = mk('Panel', gridLayer); box.hittest = false;
          box.style.position = '0px ' + (y - AXIS_HALF).toFixed(0) + 'px 0px';
          box.style.width = AXIS_W + 'px'; box.style.height = (AXIS_HALF * 2) + 'px';
          var v = yToVal(y, ach), svl = lbl(box, v.toFixed(coarse ? 0 : 2), S.hint, 9);
          svl.hittest = false; svl.style.horizontalAlign = 'right'; svl.style.verticalAlign = 'center';
          svl.style.whiteSpace = 'nowrap'; svl.style.marginRight = '5px';
        }
      }
    }
    function niceStep(raw) { var p = Math.pow(10, Math.floor(Math.log(Math.max(1, raw)) / Math.LN10)); var n = raw / p; var m = n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10; return Math.max(1, m * p); }

    function drawCurves() {
      curveLayer.RemoveAndDeleteChildren();
      for (var i = 0; i < model.length; i++) {
        var ch = model[i]; if (!ch.vis || ch.keys.length === 0 || !sceneStripped(ch)) continue;
        var col = ch.edit ? ch.color : (ch.color + '66');
        var k = ch.keys;
        // Flat extrapolation before the first / after the last key, in every mode now that a curve
        // is scaled to the whole canvas rather than confined to its own band.
        var fy = valToY(k[0].v, ch); lineSeg(curveLayer, 0, fy, tickToX(k[0].t), fy, ch.color + '55', 1);
        var ly = valToY(k[k.length - 1].v, ch); lineSeg(curveLayer, tickToX(k[k.length - 1].t), ly, G.cw, ly, ch.color + '55', 1);
        for (var j = 0; j + 1 < k.length; j++) {
          var a = k[j], b = k[j + 1];
          if (stepped(ch)) {
            lineSeg(curveLayer, tickToX(a.t), valToY(a.v, ch), tickToX(b.t), valToY(a.v, ch), col, 2);
            lineSeg(curveLayer, tickToX(b.t), valToY(a.v, ch), tickToX(b.t), valToY(b.v, ch), col, 2);
          } else if (segActive(a, b)) {
            var P = segPts(a, b); var steps = 14; var px = -1, py = -1;
            for (var s = 0; s <= steps; s++) {
              var u = s / steps; var xv = bez(P.p0x, P.p1x, P.p2x, P.p3x, u), yv = bez(P.p0y, P.p1y, P.p2y, P.p3y, u);
              var X = tickToX(xv), Y = valToY(yv, ch);
              if (px >= 0) lineSeg(curveLayer, px, py, X, Y, col, 2);
              px = X; py = Y;
            }
          } else { lineSeg(curveLayer, tickToX(a.t), valToY(a.v, ch), tickToX(b.t), valToY(b.v, ch), col, 2); }
        }
      }
    }
    function drawKeyframes() {
      kfLayer.RemoveAndDeleteChildren();
      for (var i = 0; i < model.length; i++) {
        var ch = model[i]; if (!ch.vis || !sceneStripped(ch)) continue;
        for (var j = 0; j < ch.keys.length; j++) {
          var k = ch.keys[j]; var x = tickToX(k.t), y = valToY(k.v, ch);
          if (x < -10 || x > G.cw + 10 || y < -10 || y > G.ch + 10) continue;
          dot(kfLayer, x, y, k.sel ? 12 : 9, k.swatch || (k.sel ? S.sel : ch.color), true);
        }
      }
    }
    function drawHandles() {
      handleLayer.RemoveAndDeleteChildren();
      for (var i = 0; i < model.length; i++) {
        var ch = model[i]; if (!ch.vis || stepped(ch) || !sceneStripped(ch)) continue;
        for (var j = 0; j < ch.keys.length; j++) {
          var k = ch.keys[j]; if (!k.sel) continue;
          var kx = tickToX(k.t), ky = valToY(k.v, ch);
          [-1, 1].forEach(function (side) {
            var e = handleEndpoint(ch, k, side); var hx = tickToX(e.t), hy = valToY(e.v, ch);
            lineSeg(handleLayer, kx, ky, hx, hy, S.handle, 1);
            dot(handleLayer, hx, hy, 7, S.handle, false);
          });
        }
      }
    }
)GEJS"
R"GEJS(
    // The navigator's keyframe marks: one tick of light per key across the WHOLE demo, drawn from
    // every visible channel. Rebuilt with the curves (not per frame) -- it only changes when the
    // model, visibility or the domain changes.
    function drawOverviewKeys() {
      ovKeyLayer.RemoveAndDeleteChildren();
      for (var i = 0; i < model.length; i++) {
        var ch = model[i]; if (!ch.vis) continue;
        for (var j = 0; j < ch.keys.length; j++) {
          var k = ch.keys[j]; var x = domToX(k.t);
          if (x < 0 || x > G.cw) continue;
          var m = mk('Panel', ovKeyLayer); m.hittest = false;
          m.style.position = x.toFixed(1) + 'px 0px 0px';
          m.style.width = '1px'; m.style.height = '42%'; m.style.verticalAlign = 'center';
          m.style.backgroundColor = k.sel ? S.sel : S.ovKey;
        }
      }
    }

    function updateChannelRows() {
      for (var c = 0; c < rows.length; c++) {
        var ch = chById(c); var r = rows[c]; if (!ch) continue;
        r.sw.style.backgroundColor = ch.color;
        r.nm.text = ch.name; r.nm.style.color = ch.solo ? S.accent : (ch.edit ? S.value : S.inactive);
        var v = sampleChan(ch, st.playhead);
        r.val.text = (v == null) ? '-' : v.toFixed(2);
        r.val.style.color = ch.edit ? S.accent : S.inactive;
        r.eye.style.backgroundColor = ch.vis ? S.btnOn : S.btnBg;
        r.eyeL.style.color = ch.vis ? S.accent : S.inactive;
        r.hot.style.backgroundColor = (c === activeCh) ? S.rowOn : 'rgba(0,0,0,0)';
        // The diamond's whole job is to say "the playhead is on a key" -- filled when it is, so it
        // doubles as the remove button without a separate delete control on the row.
        var at = keyAtTick(ch, st.playhead);
        r.dia.__lbl.text = at ? '◆' : '◇';
        r.dia.__lbl.style.color = at ? S.accent : S.inactive;
        var dim = ch.keys.length ? S.inactive : S.dim;
        r.prev.__lbl.style.color = dim; r.next.__lbl.style.color = dim;
      }
    }
    // The same per-frame state for the scene strips' diamonds (their headers are rebuilt only when
    // the strip set moves, so the glyphs cannot be read back out of the tree).
    function updateSceneNavs() {
      for (var i = 0; i < sceneNavs.length; i++) {
        var n = sceneNavs[i], ch = chById(n.ch); if (!ch) continue;
        var at = keyAtTick(ch, st.playhead);
        n.dia.__lbl.text = at ? '◆' : '◇';
        n.dia.__lbl.style.color = at ? S.accent : S.inactive;
        var dim = ch.keys.length ? S.inactive : S.dim;
        n.prev.__lbl.style.color = dim; n.next.__lbl.style.color = dim;
      }
    }

    // Per-frame placement of the navigator chrome: the overview viewport, its playhead marker and
    // the bottom scrollbar thumb. Style writes are cheap but not free -- Panorama re-lays-out on
    // each one -- so the whole block is gated behind a signature and does nothing at rest.
    var navSig = '';
)GEJS"
R"GEJS(
    function layoutNav() {
      var d = domainRange(), domSpan = Math.max(1, d.t1 - d.t0);
      var vr = ovViewRect();
      var px = domToX(num(st.playhead));
      var trackX0 = G.cx + ZOOM_W + 10;
      var trackW = Math.max(40, G.cw - 2 * (ZOOM_W + 10));
      var thumbW = Math.max(22, Math.min(trackW, (view.t1 - view.t0) / domSpan * trackW));
      var frac = (view.t0 - d.t0) / domSpan;
      if (frac < 0) frac = 0; if (frac > 1) frac = 1;
      var thumbX = trackX0 + frac * trackW;
      if (thumbX + thumbW > trackX0 + trackW) thumbX = trackX0 + trackW - thumbW;
      sbThumbX0 = thumbX; sbThumbW = thumbW;
      var sig = vr.x0.toFixed(1) + ',' + vr.x1.toFixed(1) + ',' + px.toFixed(1) + ','
        + thumbX.toFixed(1) + ',' + thumbW.toFixed(1) + ',' + G.cw;
      if (sig === navSig) return;
      navSig = sig;
      ovLeftGrip.style.position = Math.max(0, Math.min(G.cw - 6, vr.x0 - 3)).toFixed(1) + 'px 4px 0px';
      ovRightGrip.style.position = Math.max(0, Math.min(G.cw - 6, vr.x1 - 3)).toFixed(1) + 'px 4px 0px';
      ovView.style.position = vr.x0.toFixed(1) + 'px 0px 0px';
      ovView.style.width = Math.max(3, vr.x1 - vr.x0).toFixed(1) + 'px';
      ovView.style.height = '100%';
      if (px >= 0 && px <= G.cw) { ovPlay.visible = true; ovPlay.style.position = px.toFixed(1) + 'px 0px 0px'; }
      else ovPlay.visible = false;
      sbThumb.style.position = (thumbX - trackX0).toFixed(1) + 'px 0px 0px';
      sbThumb.style.width = thumbW.toFixed(1) + 'px';
      sbThumb.style.height = SB_H + 'px';
    }

    var api = {};
    // Wheel, forwarded by CameraEditorJs's api.wheel when the cursor is over this dock. The wheel
    // is CONTEXTUAL: over the dock it belongs to the dock (and is consumed, so it can never fall
    // through to the game or scroll the inspector), and only over the inspector does it return
    // false so the column keeps its own scroll.
    //
    //   Ctrl+Wheel  = zoom the time axis about the cursor
    //   Shift+Wheel = pan the time axis
    //   plain Wheel = scroll the track list (scene mode stacks one strip per property)
    //
    // Ctrl+Wheel must never nudge the free-cam FOV while the graph is up; MovieMode.cpp stops the
    // modifier notches at this dock before its own FOV branch. (Panorama never sees the wheel in
    // this HUD context -- the notches come off the WndProc pipe through CameraEditorHud.)
    api.wheel = function (notches) {
      var n = parseInt(notches, 10);
      if (!isFinite(n) || !n || !st || !G || !st.enabled) return false;
      var c = curCursor();
      if (c.lx < 0 || c.lx > G.rw || c.ly < 0 || c.ly > G.rh) return false;
      if (st.ctrl) {
        var anchor = (c.clx >= 0 && c.clx <= G.cw) ? xToTick(c.clx) : (view.t0 + view.t1) * 0.5;
        zoomAt(n > 0 ? (1 / 1.25) : 1.25, anchor);
        return true;
      }
      if (st.shift) { panByFrac(n > 0 ? -0.1 : 0.1); return true; }
      if (sceneOn()) {
        var max = Math.max(0, sceneContentH - G.ch);
        if (max > 0) {
          var was = sceneScroll;
          sceneScroll = Math.max(0, Math.min(max, sceneScroll - n * 48));
          if (sceneScroll !== was) { sceneHeadSig = ''; lastSig = ''; api.render(); }
        }
      }
      return true;   // consumed: over the dock the wheel is ours even when there is nothing to scroll
    };
    // THE COLUMN IS BEING DRAGGED RIGHT NOW, at this width -- called straight from CONFIG's and
    // the CAMERA EDITOR's grip (one shared Panorama script context), so the graph follows the
    // edge DURING the drag rather than jumping to it on release.
    api.setInspW = function (w) {
      var n = parseInt(w, 10);
      graphInspWOverride = (isFinite(n) && n > 0) ? n : 0;
    };
    api.render = function () {
      var raw = root.GetAttributeString('state', ''); if (!raw) { root.visible = false; return; }
      try { st = JSON.parse(raw); } catch (e) { return; }
      // A live drag outranks the published width, and hands back once the published value has
      // caught up -- same contract as the camera timeline's, for the same reason.
      if (graphInspWOverride > 0) {
        graphInspW = graphInspWOverride;
        if (typeof st.inspW === 'number' && Math.abs(st.inspW - graphInspWOverride) < 1) graphInspWOverride = 0;
      } else if (typeof st.inspW === 'number' && st.inspW > 0) {
        graphInspW = st.inspW;
      }
      S.accent = st.renderOpen ? S.renderAccent : S.cameraAccent;
      S.btnOn = st.renderOpen ? '#aeb4bc26' : '#f0b32333';
      rectVis.style.backgroundColor = st.renderOpen ? '#aeb4bc1f' : '#f0b32322';
      rectVis.style.border = '1px solid ' + S.accent;
      // easeMenu is built LAZILY, on the first right-click (showEaseMenu -> buildEaseMenu), so it
      // is null for the entire session until then. Dereferencing it unconditionally here threw
      // every frame, and a Panorama exception ABORTS THE REST OF THE FUNCTION -- taking out the
      // `root.visible` assignment on the next line and every draw call below it, which is why the
      // graph editor rendered nothing at all. Only restyle it once it exists.
      if (easeMenu) easeMenu.style.border = '1px solid ' + S.accent;
      model = st.channels || [];
      // The graph is the TALLER dock, so arriving here is always a grow: it rises.
      var graphShown = !!st.enabled;   // RAW: never delay a hide, see moDockOpenIsReal
      var graphRealOpen = moDockOpenIsReal(graphShown);
      root.visible = graphShown;
      if (graphShown !== moDockSeen) {
        moDockSeen = graphShown;
        // The graph is the TALLER dock, so arriving here is always a grow: it rises.
        if (graphShown && graphRealOpen) moDockIn(root, 14);
      }
      if (!st.enabled) return;

      // Put the host timeline in its compact scrub view (once) so the bottom bar stays short and
      // this dock can sit above it. The timeline also self-suppresses its curve toggle while we're on.
      if (!didInitTimeline) { didInitTimeline = true; rawCmd('mirv_filmmaker camtl view timeline'); }

      // selection cache (used by hit-test press path before C++ echoes back)
      selCache = {};
      for (var i = 0; i < model.length; i++) for (var j = 0; j < model[i].keys.length; j++)
        if (model[i].keys[j].sel) selCache[model[i].ch + ':' + model[i].keys[j].id] = 1;

      activeCh = (st.selCh != null && st.selCh >= 0) ? st.selCh : 0;
      // Until we have a valid screen measurement (root reports 0 for the first layout passes after
      // a build / resolution switch), skip the layout-dependent draw rather than render squished.
      if (!computeGeom() || !G) { root.visible = false; return; }
      // Seed the visible window ONCE per mode (and whenever C++ advances viewEpoch: dock enter,
      // reseed, clear). Everything after this line reads `view`, never the state's suggested
      // window, which is what makes scrubbing the playhead leave the window alone.
      var gmk = sceneOn() ? 'scene' : 'camera';
      if (view.mode !== gmk || (st.viewEpoch != null && view.epoch !== st.viewEpoch)) seedView(gmk);
      // One scene-mode signal across editor, timeline and graph. C++ publishes the
      // persisted inspector mode to each HUD state feed.
      var sceneMode = sceneOn();
      styleTab(camModeTab, !sceneMode);
      styleTab(sceneModeTab, sceneMode);
      lhP.visible = !sceneMode;
      rows.forEach(function (r) { r.rowP.visible = !sceneMode; });
      sceneHeadClip.visible = sceneMode;
      stripLayer.visible = sceneMode;
      sceneEmpty.visible = sceneMode && !model.length;
      if (sceneMode) {
        layoutSceneStrips();
        sceneHeadClip.style.position = '0px ' + G.ovH + 'px 0px';
        sceneHeadClip.style.height = G.ch + 'px';
        // The headers are rebuilt when the strip SET, a group's collapse, the visibility or the
        // scroll offset moves -- all change where every row sits, and there is no cheaper way to
        // move an absolutely placed column.
        var headSig = model.map(function (c) { return c.fieldId + ':' + c.vis + ':' + c.solo + ':' + c.muted; }).join('|')
          + '@' + sceneScroll + 'x' + sceneContentH
          + '@' + sceneGroups.map(function (g) { return g.id + (g.collapsed ? '1' : '0'); }).join(',');
        if (headSig !== sceneHeadSig) { sceneHeadSig = headSig; rebuildSceneHeads(); }
        model.forEach(function (c) {
          var r = sceneValues[c.fieldId]; if (!r || r.editing) return;
          if (r.swatch) {
            // A colour row mirrors the MODEL into the chip (and into the open wheel), except
            // while the wheel is the thing driving the value -- otherwise every frame would
            // paint the last committed colour back over the one being dragged.
            var v = c.value;
            if (v instanceof Array && !r.picker.visible()) {
              var moved = false;
              for (var ci = 0; ci < 3; ci++) {
                var n = Math.max(0, Math.min(255, Math.round(v[ci])));
                if (r.rgb[ci] !== n) { r.rgb[ci] = n; moved = true; }
              }
              if (moved) { r.last = r.rgb.join(','); r.paint(); }
            }
            r.picker.sync();
            return;
          }
          var text = sceneValueText(c.value);
          if (r.cell) { if (r.cell.text !== text) r.cell.text = text; }
          else r.input.text = text;
        });
        updateSceneNavs();
      }
      // The colour popover runs off the SAME raw cursor pipe as everything else in this dock
      // (Panorama delivers no mouse-move here), so it needs one pump per frame to drag the wheel
      // and the two sliders, and to close itself on a click-away.
      try { cfgColorPump({ cursor: true, lmb: !!st.lmb, mx: st.mx, my: st.my }); }
      catch (ecp) { $.Msg('[grapheditor] colour pump failed: ' + ecp + String.fromCharCode(10)); }
      processInput();
      followPlayback();
      // ONE SPEED, THREE BARS -- st.timescale is the engine's demo_timescale, pushed by C++.
      // ctx.GetDemoControllerState() does not exist on this HUD context panel; see the same note
      // in CameraTimelineJs.h and Movie/DemoTimescale.h.
      if (typeof st.timescale === 'number') speedScrub.setSpeed(st.timescale);
      // Same raw-cursor pipe every other gesture in this editor runs on (no mouse-move event exists
      // in this HUD context), pumped once per frame.
      speedScrub.pump(st.mx || 0, st.my || 0, !!st.lmb);

      // Smooth / Linear show which mode the shot is actually in. st.interp is the camera path's
      // InterpName(), i.e. the SAME value the camera timeline's pair reads -- setting it in
      // either panel lights the matching button in both.
      btnSel(smoothBtn, st.interp !== 'Linear');
      btnSel(linearBtn, st.interp === 'Linear');
      // Reflect the path's reported interpolation state; cubic supports three markers.
      smoothBtn.__lbl.text = '∿ Smooth';
      smoothBtn.__lbl.style.color = st.interpDegraded ? S.dim
        : (smoothBtn.__sel ? S.accent : S.value);

      // The destructive FILL tracks enablement, not just the label: a dimmed label on a live
      // red pill reads as an enabled button whose text went missing. Dropping to the neutral
      // pill is what makes S.dim legible AS "disabled" (same pairing as custApplyBtn). Latched
      // on the flag so this stays off the per-frame style-write path once settled.
      var delOn = st.selCount > 0;
      if (delOn !== delBtn.__delOn) {
        delBtn.__delOn = delOn;
        delBtn.__lbl.style.color = delOn ? S.dangerText : S.dim;
        delBtn.style.backgroundColor = delOn ? S.dangerBg : S.btnBg;
        delBtn.style.border = '1px solid ' + (delOn ? S.dangerBorder : S.border);
      }
      undoBtn.__lbl.style.color = st.canUndo ? S.value : S.dim;
      redoBtn.__lbl.style.color = st.canRedo ? S.value : S.dim;
      // Time snap: the pill reports the current value and lights while snapping is on.
      btnSel(snapBtn, snapSetting() !== 'off');
      snapBtn.__lbl.text = snapLabel();
      if (snapMenuOpen()) refreshSnapRows();
      // Deliberately SHORT. This line used to run the tick, the selection count, a live/SCRUBBING
      // word and four shortcut hints across the whole header -- a wall of small text where the
      // shortcuts belong in the quiet hint block under the channel list (they are there now) and
      // "live" said nothing the moving playhead did not. Tick, and what you have hold of.
      hInfo.text = 'tick ' + Math.round(st.playhead)
        + (st.selCount > 0 ? ('  ·  ' + st.selCount + ' selected') : '');

      if (!sceneMode) updateChannelRows();

      // Rebuild the (expensive) grid/curves/keyframes/handles only when the model, view, size
      // or active channel changed. The model rev bumps on every edit, so an active drag rebuilds
      // each frame; idle frames only move the playhead.
      // The view span is part of the signature, so a pan/zoom rebuilds the curves; the overview
      // keyframe marks only need the model + domain, but rebuilding them here keeps one code path.
      var dom = domainRange();
      var sig = (sceneMode ? 'scene:' : 'camera:') + st.rev + ':' + view.t0 + ':' + view.t1 + ':' + G.cw + ':' + G.ch + ':' + activeCh
        + ':' + dom.t0 + ':' + dom.t1 + ':' + snapSetting()
        + (sceneMode ? ':' + sceneScroll + ':' + model.length : '');
      if (sig !== lastSig) {
        lastSig = sig;
        if (sceneMode) drawSceneStrips(); else stripLayer.RemoveAndDeleteChildren();
        drawGrid(); drawCurves(); drawKeyframes(); drawHandles(); drawOverviewKeys();
      }
      layoutNav();

      {
      var phx = tickToX(st.playhead);
        // Dock-relative and full height: the playhead runs from the ruler down through the tracks.
        // It is HIDDEN once it leaves the visible window -- pinned to the left edge it read as "the
        // playhead is stuck there" instead of "the playhead is outside this view".
        if (phx >= 0 && phx <= G.cw) {
          phLine.visible = true;
          phLine.style.position = (G.cx + phx).toFixed(1) + 'px ' + G.cy + 'px 0px';
        phHandle.visible = true;
          // The ruler now overlays the top of the canvas, so the grab pin lives IN the ruler at
          // the canvas top rather than 13px above it.
          phHandle.style.position = (G.cx + phx - 7).toFixed(1) + 'px ' + (G.cy + 2) + 'px 0px';
        } else {
          phLine.visible = false; phHandle.visible = false;
        }
      }
    };

    $.GraphExp = api;
    api.render();
    $.Msg('[grapheditor] experimental graph editor built.\n');
  } catch (err) {
    $.Msg('[grapheditor] gui error: ' + err + '\n');
  }
})();
)GEJS";

} // namespace Filmmaker
