#pragma once

// Scaled 16:9 preview frame, native CS2 HUD viewport alignment, 3D gizmos, track badges,
// and prop overlay for Scene Editor. Split across two raw literals to stay under MSVC C2026.
// Strict ES5 only. Shares the root IIFE scope of SceneEditorJs.h.

namespace Filmmaker {

inline const char *const kSceneEditorGizmoJsA = R"SEJS(
    // ---- Native CS2 HUD Scaling into Scene Editor Preview Window ----
    var SE_OURS = {
      'CamTimelineRoot': 1, 'CamTimelineRangeRoot': 1, 'MarkerHudRoot': 1, 'MovieHudRoot': 1,
      'CamEditorRoot': 1, 'GraphExpRoot': 1, 'CamEditorConfirmRoot': 1, 'CamEditorSettingsRoot': 1,
      'CamEditorRenderRoot': 1, 'ConfigHudRoot': 1, 'ConfigHudOverlayRoot': 1,
      'ConfigEditorSettingsRoot': 1, 'CamEditorDebugRoot': 1,
      'CamEditorCustomizeRoot': 1, 'CamEditorCustomizeConfirmRoot': 1, 'MvmBarTipRoot': 1,
      'MvmEggRoot': 1, 'MvmOnboardingRoot': 1, 'AimXhairRoot': 1, 'SceneEditorRoot': 1
    };
    var seViewportHudPanels = [];
    var seHudLoggedOnce = false;
    function seIsNativeDemoBar(p) {
      return !!(p && p.FindChildTraverse && (p.FindChildTraverse('SliderRow') || p.FindChildTraverse('ControlRow')));
    }
    function seRememberHudPanel(p) {
      for (var i = 0; i < seViewportHudPanels.length; i++) if (seViewportHudPanels[i] === p) return;
      seViewportHudPanels.push(p);
    }
    function seClearPanelViewport(p) {
      if (!p) return;
      try {
        if (p.IsValid && !p.IsValid()) return;
        p.style.transition = 'none';
        p.style.transform = 'none';
        p.__seViewportAdjusted = false;
        p.__seViewportCss = '';
        (function (panel) {
          $.Schedule(0, function () {
            try {
              if (!panel || (panel.IsValid && !panel.IsValid())) return;
              if (panel.__seViewportAdjusted) return;
              panel.style.transformOrigin = null;
              panel.style.transform = null;
              panel.style.transition = null;
            } catch (e3) {}
          });
        })(p);
      } catch (e) {}
    }
    function seRestoreHudViewport() {
      seHudLoggedOnce = false;
      for (var i = 0; i < seViewportHudPanels.length; i++) {
        seClearPanelViewport(seViewportHudPanels[i]);
      }
      seViewportHudPanels = [];
      var n = ctx.GetChildCount ? ctx.GetChildCount() : 0;
      for (var j = 0; j < n; j++) {
        var c = ctx.GetChild(j);
        if (c && c.__seViewportAdjusted) seClearPanelViewport(c);
      }
    }
    function seApplyHudViewport(active, rect, rsx, rsy, sw, sh) {
      if (!active || !rect || rect.length < 4 || !(sw > 1 && sh > 1)) {
        seRestoreHudViewport();
        return;
      }
      var x0 = Number(rect[0]), y0 = Number(rect[1]), x1 = Number(rect[2]), y1 = Number(rect[3]);
      if (!(isFinite(x0) && isFinite(y0) && isFinite(x1) && isFinite(y1) && x1 > x0 && y1 > y0)) {
        seRestoreHudViewport();
        return;
      }
      var sx = x1 - x0, sy = y1 - y0;
      var tx = x0 * sw, ty = y0 * sh;
      var n = ctx.GetChildCount ? ctx.GetChildCount() : 0;
      var dump = seHudLoggedOnce ? null : [];
      for (var i = 0; i < n; i++) {
        var c = ctx.GetChild(i);
        if (!c || SE_OURS[c.id] || seIsNativeDemoBar(c)) continue;
        seRememberHudPanel(c);
        try {
          var px = (c.actualxoffset || 0) / rsx, py = (c.actualyoffset || 0) / rsy;
          var ptx = tx - (1 - sx) * px, pty = ty - (1 - sy) * py;
          var cssTx = ptx / sx, cssTy = pty / sy;
          var css = 'translate3d(' + cssTx.toFixed(1) + 'px, ' + cssTy.toFixed(1) + 'px, 0px) scale3d('
            + sx.toFixed(5) + ', ' + sy.toFixed(5) + ', 1)';
          if (dump) {
            dump.push((c.id || '(no-id)') + '@(' + Math.round(px) + ',' + Math.round(py) + ','
              + Math.round((c.actuallayoutwidth || 0) / rsx) + 'x' + Math.round((c.actuallayoutheight || 0) / rsy) + ')');
          }
          c.__seViewportAdjusted = true;
          if (c.__seViewportCss === css) continue;
          c.__seViewportCss = css;
          c.style.transition = 'none';
          c.style.transformOrigin = '0% 0%';
          c.style.transform = css;
        } catch (e2) {}
      }
      if (dump) {
        seHudLoggedOnce = true;
        var ret = null;
        try { ret = ctx.FindChildTraverse ? ctx.FindChildTraverse('HudReticle') : null; } catch (er) {}
        var retDesc = 'missing';
        if (ret) {
          var rp = ret.GetParent ? ret.GetParent() : null;
          retDesc = 'parent=' + (rp ? rp.id : '?') + ' xy=(' + Math.round((ret.actualxoffset || 0) / rsx)
            + ',' + Math.round((ret.actualyoffset || 0) / rsy) + ') sz='
            + Math.round((ret.actuallayoutwidth || 0) / rsx) + 'x' + Math.round((ret.actuallayoutheight || 0) / rsy);
        }
        $.Msg('[sceneeditor] scaled HUD roots (' + dump.length + '): ' + dump.join(' ') + ' | HudReticle=' + retDesc + '\n');
      }
    }

    // ---- Scaled 16:9 Preview Frame + 3D Viewport Gizmo & Track Marker Overlay ----
    function mkMatte() {
      var m = mk('Panel', root); m.hittest = true; m.visible = false;
      m.style.backgroundColor = '#080a0eff';
      m.SetPanelEvent('onactivate', function () {});
      return m;
    }
    var seMatteLeft = mkMatte(), seMatteRight = mkMatte(), seMatteTop = mkMatte(), seMatteBottom = mkMatte();
    var sePreviewFrame = mk('Panel', root);
    sePreviewFrame.hittest = false; sePreviewFrame.hittestchildren = false; sePreviewFrame.visible = false;
    sePreviewFrame.style.border = '1px solid #ffffff26';

    var vpOverlay = mk('Panel', root);
    vpOverlay.hittest = false; vpOverlay.hittestchildren = false;
    vpOverlay.style.overflow = 'clip';
    vpOverlay.SetPanelEvent('onactivate', function () {});
    var previewRect = { x: 0, y: TOPBAR_H, w: 640, h: 360 };
    function layoutPreview(sw, sh) {
      var areaW = Math.max(160, sw - SIDEBAR_W);
      var areaH = Math.max(120, sh - TOPBAR_H - DOCK_H);
      var aspect = sw / Math.max(1, sh);
      var pw = areaW, ph = pw / aspect;
      if (ph > areaH) { ph = areaH; pw = ph * aspect; }
      pw = Math.max(64, Math.floor(pw));
      ph = Math.max(64, Math.floor(ph));
      var ox = Math.max(0, Math.floor((areaW - pw) / 2));
      var oy = TOPBAR_H + Math.max(0, Math.floor((areaH - ph) / 2));
      var right = ox + pw, bottom = oy + ph;

      function setBox(p, show, bx, by, bw, bh) {
        p.visible = !!show && bw > 0 && bh > 0;
        if (!p.visible) return;
        p.style.position = bx.toFixed(0) + 'px ' + by.toFixed(0) + 'px 0px';
        p.style.width = bw.toFixed(0) + 'px';
        p.style.height = bh.toFixed(0) + 'px';
      }
      setBox(seMatteLeft, ox > 0, 0, TOPBAR_H, ox, areaH);
      setBox(seMatteRight, areaW - right > 0, right, TOPBAR_H, areaW - right, areaH);
      setBox(seMatteTop, oy > TOPBAR_H, ox, TOPBAR_H, pw, oy - TOPBAR_H);
      setBox(seMatteBottom, TOPBAR_H + areaH - bottom > 0, ox, bottom, pw, TOPBAR_H + areaH - bottom);

      sePreviewFrame.visible = true;
      sePreviewFrame.style.position = ox + 'px ' + oy + 'px 0px';
      sePreviewFrame.style.width = pw + 'px';
      sePreviewFrame.style.height = ph + 'px';

      root.SetAttributeString('previewrect',
        (ox / sw).toFixed(5) + ' ' + (oy / sh).toFixed(5) + ' ' +
        (right / sw).toFixed(5) + ' ' + (bottom / sh).toFixed(5));
      previewRect = { x: ox, y: oy, w: pw, h: ph };
      return previewRect;
    }
    var SE_AXIS_COLORS = ['#ff6767ff', '#6be699ff', '#65aaffff'];
    var SE_AXIS_NAMES = ['X', 'Y', 'Z'];
    var seAxes = [];
    for (var sai = 0; sai < 3; sai++) {
      var shaft = mk('Panel', vpOverlay); shaft.hittest = false;
      shaft.style.backgroundColor = SE_AXIS_COLORS[sai];
      var tag = mk('Panel', vpOverlay); tag.hittest = false;
      tag.style.width = '22px'; tag.style.height = '22px';
      var tLbl = lbl(tag, SE_AXIS_NAMES[sai], SE_AXIS_COLORS[sai], 16);
      tLbl.hittest = false; tLbl.style.fontWeight = 'bold';
      tLbl.style.horizontalAlign = 'center'; tLbl.style.verticalAlign = 'center';
      seAxes.push({ shaft: shaft, tag: tag });
    }
    var SE_RING_N = 32;
    var seYawRing = [];
    for (var sri = 0; sri < SE_RING_N; sri++) {
      var rl = mk('Panel', vpOverlay); rl.hittest = false;
      rl.style.backgroundColor = '#65aaffdd';
      seYawRing.push(rl);
    }
    // Up to 24 track name badges + 24 scrub dots projected over 3D world paths
    var SE_MAX_BADGES = 24, seBadges = [], seScrubDots = [];
    for (var bi = 0; bi < SE_MAX_BADGES; bi++) {
      var bBox = mk('Panel', vpOverlay); bBox.hittest = false; bBox.visible = false;
      bBox.style.backgroundColor = '#10141ae8'; bBox.style.borderRadius = '3px';
      bBox.style.padding = '2px 7px'; bBox.style.border = '1px solid #ffffff33';
      var bText = lbl(bBox, '', S.value, 12); bText.hittest = false; bText.style.fontWeight = 'bold';
      seBadges.push({ box: bBox, text: bText });

      var dot = mk('Panel', vpOverlay); dot.hittest = false; dot.visible = false;
      dot.style.width = '11px'; dot.style.height = '11px'; dot.style.borderRadius = '6px';
      dot.style.border = '2px solid #ffffffff'; dot.style.backgroundColor = '#ff5a5aee';
      seScrubDots.push(dot);
    }
    // Placed prop bounding box & axes when a prop is selected or being placed inside Scene Editor
    var sePropBoxEdges = [], sePropAxes = [];
    for (var pe = 0; pe < 12; pe++) {
      var pl = mk('Panel', vpOverlay); pl.hittest = false; pl.visible = false;
      pl.style.backgroundColor = '#7fe0d0bb';
      sePropBoxEdges.push(pl);
    }
    for (var pai = 0; pai < 3; pai++) {
      var ps = mk('Panel', vpOverlay); ps.hittest = false; ps.visible = false;
      ps.style.backgroundColor = SE_AXIS_COLORS[pai];
      sePropAxes.push(ps);
    }
    var seVpHint = lbl(vpOverlay, '', S.label, 13);
    seVpHint.hittest = false;
    seVpHint.style.horizontalAlign = 'center'; seVpHint.style.verticalAlign = 'top';
    seVpHint.style.marginTop = '12px'; seVpHint.style.padding = '5px 12px';
    seVpHint.style.backgroundColor = '#10141add'; seVpHint.style.borderRadius = '4px';
    seVpHint.style.border = '1px solid #ffffff1a';
)SEJS";

inline const char *const kSceneEditorGizmoJsB = R"SEJS(
    function seDrawSeg(panel, a, b, w, h, thick) {
      panel.visible = !!(a && b && a[2] && b[2]);
      if (!panel.visible) return;
      var x = a[0] * w, y = a[1] * h - thick * 0.5;
      var dx = (b[0] - a[0]) * w, dy = (b[1] - a[1]) * h;
      var len = Math.sqrt(dx * dx + dy * dy);
      if (len < 1) { panel.visible = false; return; }
      panel.style.position = x.toFixed(1) + 'px ' + y.toFixed(1) + 'px 0px';
      panel.style.width = len.toFixed(1) + 'px';
      panel.style.height = thick + 'px';
      panel.style.transformOrigin = '0% 50%';
      panel.style.transform = 'rotateZ(' + (Math.atan2(dy, dx) * 180 / Math.PI).toFixed(2) + 'deg)';
    }
    function seHitAxis(g, u, v, aspect) {
      if (!g || !g.origin || !g.origin[2]) return -1;
      var best = -1, bestD = 0.028;
      for (var i = 0; i < 3; i++) {
        var a = g.origin, b = g.axes && g.axes[i];
        if (!b || !b[2]) continue;
        var dx = (b[0] - a[0]) * aspect, dy = b[1] - a[1], len = dx * dx + dy * dy;
        if (len < 0.000025) continue;
        var px = (u - a[0]) * aspect, py = v - a[1];
        var t = Math.max(0, Math.min(1, (px * dx + py * dy) / len));
        var d = Math.sqrt(Math.pow(px - t * dx, 2) + Math.pow(py - t * dy, 2));
        if (d < bestD) { bestD = d; best = i; }
      }
      return best;
    }
    function seHitRing(ringPts, u, v, aspect) {
      if (!ringPts || !ringPts.length) return false;
      var bestD = 0.025;
      for (var i = 0; i < ringPts.length; i++) {
        var a = ringPts[i], b = ringPts[(i + 1) % ringPts.length];
        if (!a || !b || !a[2] || !b[2]) continue;
        var dx = (b[0] - a[0]) * aspect, dy = b[1] - a[1], len = dx * dx + dy * dy;
        if (len < 0.00001) continue;
        var px = (u - a[0]) * aspect, py = v - a[1];
        var t = Math.max(0, Math.min(1, (px * dx + py * dy) / len));
        var d = Math.sqrt(Math.pow(px - t * dx, 2) + Math.pow(py - t * dy, 2));
        if (d < bestD) return true;
      }
      return false;
    }

    var vpLmbWas = false, vpGrabbingTrack = false, vpGrabbingProp = false;
    var vpLastU = -1, vpLastV = -1, vpLastSnap = false;

    function renderViewportOverlay(s, sw, sh, rsx, rsy) {
      var ox = 0, oy = 0, pw = sw, ph = sh;
      if (s.streamRecording) {
        seMatteLeft.visible = seMatteRight.visible = seMatteTop.visible = seMatteBottom.visible = false;
        sePreviewFrame.visible = false;
        root.SetAttributeString('previewrect', '');
        seApplyHudViewport(false, null, rsx, rsy, sw, sh);
      } else {
        var pr = layoutPreview(sw, sh);
        ox = pr.x; oy = pr.y; pw = pr.w; ph = pr.h;
        // Scale all native CS2 HUD roots (HudInWorld 3D nameplates, HudReticle, radar, etc.) into the exact
        // same preview rect used by AfxViewportScaler so 3D overlays, crosshair, and HUD align 1:1.
        seApplyHudViewport(true, [ox / sw, oy / sh, (ox + pw) / sw, (oy + ph) / sh], rsx, rsy, sw, sh);
      }
      vpOverlay.style.position = ox + 'px ' + oy + 'px 0px';
      vpOverlay.style.width = pw + 'px';
      vpOverlay.style.height = ph + 'px';
      vpOverlay.hittest = !!(s.cursor && !modalBlocking());

      // Preview-normalized coordinates [0,1]x[0,1] matching AfxViewportScaler's scaled world frame
      var origin = root.GetPositionWithinWindow ? root.GetPositionWithinWindow() : { x: 0, y: 0 };
      var mx = ((s.mx || 0) - origin.x) / rsx;
      var my = ((s.my || 0) - origin.y) / rsy;
      var u = (mx - ox) / Math.max(1, pw), v = (my - oy) / Math.max(1, ph), aspect = pw / Math.max(1, ph);
      var insideVp = !!(s.cursor && mx >= ox && mx < ox + pw && my >= oy && my < oy + ph && !modalBlocking());
      var down = !!s.lmb, pressed = down && !vpLmbWas;
      var moved = Math.abs(u - vpLastU) + Math.abs(v - vpLastV) > 0.0012;
      var snap = !!s.shift;

      // Track badges + scrub position dots
      var tracks = s.tracks || [];
      for (var i = 0; i < SE_MAX_BADGES; i++) {
        var bd = seBadges[i], dt = seScrubDots[i], tk = tracks[i];
        if (!tk || !tk.enabled) { bd.box.visible = false; dt.visible = false; continue; }
        var stPt = tk.startScreen, scPt = tk.scrubScreen;
        var sel = (s.selKind === 'track' && s.selTrack === i);
        var accent = tk.placedActor ? '#6eebafff' : (tk.team === 2 ? '#ffb044ff' : '#60c8ffff');
        bd.box.visible = !!(stPt && stPt[2]);
        if (bd.box.visible) {
          bd.box.style.border = sel ? '2px solid #ffffffff' : ('1px solid ' + accent);
          bd.box.style.backgroundColor = sel ? '#1e2838f0' : '#10141ae0';
          bd.text.text = tk.name + (Math.abs(tk.offsetYaw || 0) > 0.5 ? ('  ' + Math.round(tk.offsetYaw) + '°') : '');
          bd.text.style.color = sel ? '#ffffffff' : accent;
          bd.box.style.position = (stPt[0] * pw - 28).toFixed(1) + 'px ' + (stPt[1] * ph - 26).toFixed(1) + 'px 0px';
        }
        dt.visible = !!(scPt && scPt[2]);
        if (dt.visible) {
          dt.style.backgroundColor = sel ? '#ffffffff' : accent;
          dt.style.position = (scPt[0] * pw - 5.5).toFixed(1) + 'px ' + (scPt[1] * ph - 5.5).toFixed(1) + 'px 0px';
        }
      }

      // Track transform gizmo (Move arrows / Rotate yaw ring)
      var tg = s.trackGizmo;
      var showTrackGizmo = !!(s.selKind === 'track' && tg && tg.origin && tg.origin[2]);
      var hotAxis = (!vpGrabbingTrack && !vpGrabbingProp && insideVp && showTrackGizmo && s.gizmoMode === 'move') ? seHitAxis(tg, u, v, aspect) : -1;
      var hotRing = (!vpGrabbingTrack && !vpGrabbingProp && insideVp && showTrackGizmo && s.gizmoMode === 'rotate') ? seHitRing(tg.yawRing, u, v, aspect) : false;

      for (var ai = 0; ai < 3; ai++) {
        var ax = seAxes[ai];
        var showAx = showTrackGizmo && s.gizmoMode === 'move' && tg.axes && tg.axes[ai] && tg.axes[ai][2];
        ax.shaft.visible = showAx; ax.tag.visible = showAx;
        if (showAx) {
          var tip = tg.axes[ai], org = tg.origin;
          seDrawSeg(ax.shaft, org, tip, pw, ph, (hotAxis === ai) ? 5 : 3);
          var dx = (tip[0] - org[0]) * pw, dy = (tip[1] - org[1]) * ph, l = Math.sqrt(dx * dx + dy * dy) || 1;
          ax.tag.style.position = (tip[0] * pw + dx / l * 13 - 11).toFixed(1) + 'px ' + (tip[1] * ph + dy / l * 13 - 11).toFixed(1) + 'px 0px';
        }
      }
      var ringPts = (showTrackGizmo && s.gizmoMode === 'rotate') ? (tg.yawRing || []) : [];
      for (var ri = 0; ri < SE_RING_N; ri++) {
        var rSeg = seYawRing[ri];
        if (ri >= ringPts.length || !ringPts[ri][2] || !ringPts[(ri + 1) % ringPts.length][2]) {
          rSeg.visible = false; continue;
        }
        var pa = ringPts[ri], pb = ringPts[(ri + 1) % ringPts.length];
        seDrawSeg(rSeg, pa, pb, pw, ph, (hotRing || (s.dragging && s.dragKind === 'turn')) ? 4 : 2);
      }

      // Prop placement / selection bounds
      var p = s.props || null, pg = p && p.gizmo, placingProp = !!(p && p.placing);
      var showProp = !!(pg && (placingProp || s.selKind === 'prop'));
      var pHotAxis = (!vpGrabbingTrack && !vpGrabbingProp && insideVp && !placingProp && showProp) ? seHitAxis(pg, u, v, aspect) : -1;
      var bounds = showProp && pg.bounds, en = 0;
      for (var c = 0; c < 8 && bounds; c++) {
        for (var bt = 1; bt <= 4; bt *= 2) {
          if (c & bt) continue;
          var peL = sePropBoxEdges[en++];
          seDrawSeg(peL, bounds[c], bounds[c | bt], pw, ph, placingProp ? 2 : 1);
        }
      }
      for (; en < 12; en++) sePropBoxEdges[en].visible = false;
      for (var pai2 = 0; pai2 < 3; pai2++) {
        var ps2 = sePropAxes[pai2], pTip = showProp && !placingProp && pg.axes && pg.axes[pai2];
        if (!pTip || !pTip[2] || !pg.origin || !pg.origin[2]) { ps2.visible = false; continue; }
        seDrawSeg(ps2, pg.origin, pTip, pw, ph, pHotAxis === pai2 ? 5 : 3);
      }

      // Viewport mouse interactions
      if (placingProp) {
        seVpHint.text = T('Click to place model   ·   Hold Right-Click + WASD: fly   ·   Esc: cancel');
        if (insideVp && moved) cmd('mirv_filmmaker props hover ' + u.toFixed(4) + ' ' + v.toFixed(4));
        if (insideVp && pressed) cmd('mirv_filmmaker props drop');
      } else {
        seVpHint.text = vpGrabbingTrack
          ? (s.gizmoMode === 'rotate' ? T('Rotating yaw   ·   Shift: 15° snap   ·   Esc: revert') : T('Moving path   ·   Shift: 8u snap   ·   Esc: revert'))
          : (!s.cursor
            ? T('Fly Cam active (WASD + Mouse)   ·   Release Right-Click or press G for cursor')
            : T('Left-Click actor/path to select   ·   Drag arrows/ring   ·   Hold Right-Click + WASD: fly around   ·   G: toggle fly'));
        if (insideVp && pressed) {
          if (hotAxis >= 0) {
            cmd('mirv_filmmaker sceneeditor grab ' + ['x', 'y', 'z'][hotAxis] + ' ' + u.toFixed(4) + ' ' + v.toFixed(4));
            vpGrabbingTrack = true;
          } else if (hotRing) {
            cmd('mirv_filmmaker sceneeditor grab turn ' + u.toFixed(4) + ' ' + v.toFixed(4));
            vpGrabbingTrack = true;
          } else if (pHotAxis >= 0 && p && p.selected) {
            cmd('mirv_filmmaker props grab ' + p.selected + ' ' + ['x', 'y', 'z'][pHotAxis] + ' ' + u.toFixed(4) + ' ' + v.toFixed(4));
            vpGrabbingProp = true;
          } else {
            cmd('mirv_filmmaker sceneeditor pick ' + u.toFixed(4) + ' ' + v.toFixed(4));
          }
        } else if (vpGrabbingTrack && down && (moved || snap !== vpLastSnap)) {
          cmd('mirv_filmmaker sceneeditor drag ' + u.toFixed(4) + ' ' + v.toFixed(4) + (snap ? ' 1' : ' 0'));
        } else if (vpGrabbingTrack && !down) {
          cmd('mirv_filmmaker sceneeditor drag ' + u.toFixed(4) + ' ' + v.toFixed(4) + (snap ? ' 1' : ' 0'));
          cmd('mirv_filmmaker sceneeditor release');
          vpGrabbingTrack = false;
        } else if (vpGrabbingProp && down && (moved || snap !== vpLastSnap)) {
          cmd('mirv_filmmaker props drag ' + u.toFixed(4) + ' ' + v.toFixed(4) + (snap ? ' 1' : ''));
        } else if (vpGrabbingProp && !down) {
          cmd('mirv_filmmaker props release');
          vpGrabbingProp = false;
        }
      }
      if (!s.cursor) { vpGrabbingTrack = false; vpGrabbingProp = false; }
      if (moved) { vpLastU = u; vpLastV = v; }
      vpLastSnap = snap;
      vpLmbWas = down;
    }
)SEJS";

} // namespace Filmmaker
