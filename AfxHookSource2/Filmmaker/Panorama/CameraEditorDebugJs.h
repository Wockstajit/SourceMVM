#pragma once

// VIEWPORT DEBUG OVERLAY: readout comparing the Panorama preview rect to the render-layer blit rect.
// JS fragment concatenated (via #include, as an adjacent raw string literal) into the
// kCameraEditorJs script body assembled in CameraEditorJs.h. Not a standalone script --
// shares scope/closures with the root IIFE. See CameraEditorJs.h for the entry point.

R"EDJS(
    // ===================== VIEWPORT DEBUG OVERLAY =======================
    // Hidden unless state.debug (mirv_filmmaker editor debug). Read-only readout that compares the
    // Panorama preview rect against the render-layer world-blit rect and publishes per-panel HUD
    // rectangles for automation.
    var dbgPanel = $.CreatePanel('Panel', ctx, 'CamEditorDebugRoot', {});
    dbgPanel.hittest = false; dbgPanel.visible = false;
    dbgPanel.style.zIndex = '150'; dbgPanel.style.position = '8px 40px 0px';
    dbgPanel.style.width = '600px'; dbgPanel.style.flowChildren = 'down'; dbgPanel.style.height = 'fit-children';
    dbgPanel.style.backgroundColor = 'rgba(6,10,8,0.88)'; dbgPanel.style.border = '1px solid #5effa066';
    dbgPanel.style.borderRadius = '4px';
    dbgPanel.style.paddingTop = '8px'; dbgPanel.style.paddingBottom = '8px';
    dbgPanel.style.paddingLeft = '10px'; dbgPanel.style.paddingRight = '10px';
    var dbgText = lbl(dbgPanel, '', '#9effa0ff', 12);
    dbgText.style.fontFamily = 'Consolas, "Courier New", monospace'; dbgText.style.width = '100%';

    function dbgR(v) { v = Number(v || 0); return Math.round(v * 100) / 100; }
    function dbgRectObj(x, y, w, h) { return { x: dbgR(x), y: dbgR(y), w: dbgR(w), h: dbgR(h) }; }
    function dbgArea(r) { return dbgR(Math.max(0, r.w) * Math.max(0, r.h)); }
    function dbgIntersect(a, b) {
      if (!a || !b) return 0;
      var x0 = Math.max(a.x, b.x), y0 = Math.max(a.y, b.y);
      var x1 = Math.min(a.x + a.w, b.x + b.w), y1 = Math.min(a.y + a.h, b.y + b.h);
      return Math.max(0, x1 - x0) * Math.max(0, y1 - y0);
    }
    function dbgCropPct(r, bounds) {
      var area = Math.max(0, r.w) * Math.max(0, r.h);
      if (area <= 0) return 0;
      var inside = dbgIntersect(r, bounds);
      return dbgR(Math.max(0, Math.min(100, 100 - (inside / area) * 100)));
    }
    function dbgOverlapPct(r, blockers) {
      var area = Math.max(0, r.w) * Math.max(0, r.h);
      if (area <= 0) return 0;
      var overlap = 0;
      for (var i = 0; i < blockers.length; i++) overlap += dbgIntersect(r, blockers[i]);
      return dbgR(Math.max(0, Math.min(100, (overlap / area) * 100)));
    }

    // Absolute position of panel p relative to ctx (screen origin), in ACTUAL px.
    function dbgAbs(p) {
      var x = 0, y = 0, g = 0;
      try {
        while (p && g++ < 64) {
          x += (p.actualxoffset || 0); y += (p.actualyoffset || 0);
          if (p === ctx) break;
          p = p.GetParent ? p.GetParent() : null;
        }
      } catch (e) {}
      return { x: x, y: y };
    }
    function dbgFindByClass(rootp, cls, maxDepth) {
      var stack = [{ p: rootp, d: 0 }];
      while (stack.length) {
        var it = stack.pop(), p = it.p;
        if (!p) continue;
        try { if (p !== rootp && p.BHasClass && p.BHasClass(cls)) return p; } catch (e) {}
        if (it.d < maxDepth) {
          var n = p.GetChildCount ? p.GetChildCount() : 0;
          for (var i = 0; i < n; i++) { try { stack.push({ p: p.GetChild(i), d: it.d + 1 }); } catch (e2) {} }
        }
      }
      return null;
    }
    function dbgFindPanel(desc) {
      var p = null;
      try {
        if (desc.panel) return desc.panel();
        if (desc.id) p = ctx.FindChildTraverse && ctx.FindChildTraverse(desc.id);
        if (!p && desc.contentOf) p = ctx.FindChildTraverse && ctx.FindChildTraverse(desc.contentOf);
        if (!p && desc.ids) for (var i = 0; i < desc.ids.length && !p; i++) p = ctx.FindChildTraverse && ctx.FindChildTraverse(desc.ids[i]);
        if (!p && desc.cls) p = dbgFindByClass(ctx, desc.cls, 16);
      } catch (e) {}
      return p;
    }
    function dbgVisibleContentRect(rootp, sx, sy) {
      if (!rootp) return null;
      var rootAbs = dbgAbs(rootp);
      var rootRect = dbgRectObj(rootAbs.x / sx, rootAbs.y / sy,
        (rootp.actuallayoutwidth || 0) / sx, (rootp.actuallayoutheight || 0) / sy);
      var minX = 999999, minY = 999999, maxX = -999999, maxY = -999999, hits = 0;
      var stack = [{ p: rootp, d: 0 }];
      while (stack.length) {
        var it = stack.pop(), p = it.p;
        if (!p) continue;
        try {
          if (p !== rootp && p.visible !== false) {
            var a = dbgAbs(p), w = (p.actuallayoutwidth || 0) / sx, h = (p.actuallayoutheight || 0) / sy;
            var r = dbgRectObj(a.x / sx, a.y / sy, w, h);
            var almostRoot = dbgRectHasArea(rootRect) &&
              Math.abs(r.x - rootRect.x) <= 1 && Math.abs(r.y - rootRect.y) <= 1 &&
              Math.abs(r.w - rootRect.w) <= 1 && Math.abs(r.h - rootRect.h) <= 1;
            if (!almostRoot && dbgRectHasArea(r)) {
              minX = Math.min(minX, r.x); minY = Math.min(minY, r.y);
              maxX = Math.max(maxX, r.x + r.w); maxY = Math.max(maxY, r.y + r.h);
              hits++;
            }
          }
          if (it.d < 10) {
            var n = p.GetChildCount ? p.GetChildCount() : 0;
            for (var i = 0; i < n; i++) stack.push({ p: p.GetChild(i), d: it.d + 1 });
          }
        } catch (e) {}
      }
      if (!hits) return rootRect;
      // Children inside CS2 HUD roots are often intentionally laid out beyond the clipped parent.
      // Clamp the union back to the root so this records live visible pixels, not hidden overflow.
      minX = Math.max(minX, rootRect.x); minY = Math.max(minY, rootRect.y);
      maxX = Math.min(maxX, rootRect.x + rootRect.w); maxY = Math.min(maxY, rootRect.y + rootRect.h);
      return dbgRectObj(minX, minY, Math.max(0, maxX - minX), Math.max(0, maxY - minY));
    }
    function dbgFindAdjustedAncestor(p) {
      var cur = p, guard = 0;
      try {
        while (cur && guard++ < 32) {
          if (cur.__camViewportAdjusted) return cur;
          if (cur === ctx) break;
          cur = cur.GetParent ? cur.GetParent() : null;
        }
      } catch (e) {}
      return null;
    }
    function dbgRectFinite(r) {
      return !!(r && isFinite(r.x) && isFinite(r.y) && isFinite(r.w) && isFinite(r.h) &&
        Math.abs(r.x) < 100000 && Math.abs(r.y) < 100000 &&
        Math.abs(r.w) < 100000 && Math.abs(r.h) < 100000);
    }
    function dbgRectHasArea(r) {
      return dbgRectFinite(r) && r.w > 0.5 && r.h > 0.5;
    }
    function dbgPanelRecord(desc, sx, sy, g, blockers) {
      var p = dbgFindPanel(desc);
      var parent = null, id = '', source = dbgRectObj(0, 0, 0, 0), final = dbgRectObj(0, 0, 0, 0);
      var missing = true, inactive = false, adjusted = false, scaleX = 1, scaleY = 1, tx = 0, ty = 0;
      if (p && !(p.IsValid && !p.IsValid())) {
        missing = false;
        id = p.id || desc.id || '';
        parent = p.GetParent ? p.GetParent() : null;
        var a = dbgAbs(p), w = (p.actuallayoutwidth || 0), h = (p.actuallayoutheight || 0);
        source = desc.contentOf ? dbgVisibleContentRect(p, sx, sy) : dbgRectObj(a.x / sx, a.y / sy, w / sx, h / sy);
        if (!dbgRectHasArea(source)) {
          inactive = true;
          source = dbgRectObj(0, 0, 0, 0);
          final = source;
        } else {
          var anc = dbgFindAdjustedAncestor(p);
          adjusted = !!anc;
          if (g && anc) {
            scaleX = g.x1 - g.x0; scaleY = g.y1 - g.y0;
            var ox = g.x0 * ((root.actuallayoutwidth || ctx.actuallayoutwidth || 0) / sx);
            var oy = g.y0 * ((root.actuallayoutheight || ctx.actuallayoutheight || 0) / sy);
            final = dbgRectObj(ox + source.x * scaleX, oy + source.y * scaleY, source.w * scaleX, source.h * scaleY);
            tx = ox; ty = oy;
          } else {
            final = source;
          }
        }
      }
      var rw = ((root.actuallayoutwidth || ctx.actuallayoutwidth || 0) / sx);
      var rh = ((root.actuallayoutheight || ctx.actuallayoutheight || 0) / sy);
      var vp = g ? dbgRectObj(g.ox, g.oy, g.pw, g.ph) : dbgRectObj(0, 0, rw, rh);
      return {
        name: desc.name, id: id || desc.id || '', parent: (parent && parent.id) || '',
        missing: missing, inactive: inactive, adjusted: adjusted,
        source: source, viewport: vp, final: final,
        sourceArea: dbgArea(source), finalArea: dbgArea(final),
        cropPct: dbgCropPct(final, vp),
        editorOverlapPct: dbgOverlapPct(final, blockers),
        scale: { x: dbgR(scaleX), y: dbgR(scaleY) },
        offset: { x: dbgR(tx), y: dbgR(ty) }
      };
    }
    function dbgFmtRect(r) {
      if (!dbgRectFinite(r)) return 'invalid';
      return 'x=' + r.x.toFixed(0) + ' y=' + r.y.toFixed(0) + ' w=' + r.w.toFixed(0) + ' h=' + r.h.toFixed(0);
    }
    function dbgFmtPanel(p, sx, sy) {
      if (!p || (p.IsValid && !p.IsValid())) return 'n/a';
      var a = dbgAbs(p), w = (p.actuallayoutwidth || 0), h = (p.actuallayoutheight || 0);
      return 'x=' + (a.x / sx).toFixed(0) + ' y=' + (a.y / sy).toFixed(0) +
             ' w=' + (w / sx).toFixed(0) + ' h=' + (h / sy).toFixed(0);
    }
    var dbgDescriptors = [
      { name: 'chat', id: 'HudChat' },
      { name: 'money', id: 'HudMoney' },
      { name: 'health', id: 'HealthBar' },
      { name: 'armor', cls: 'hud-HA-armor' },
      { name: 'ammo', id: 'hud-WPN-main' },
      { name: 'weapon-panel', id: 'HudWeaponSelection' },
      { name: 'death-notices', id: 'HudDeathNotice' },
      { name: 'minimap', id: 'HudRadar' },
      { name: 'minimap-content', contentOf: 'HudRadar' },
      { name: 'score-timer', id: 'ScoreAndTimeAndBomb' },
      { name: 'player-cards', id: 'HudTeamCounter' },
      { name: 'player-cards-content', contentOf: 'HudTeamCounter' },
      { name: 'weapon-panel-content', contentOf: 'HudWeaponSelection' },
      { name: 'round-win-panel', id: 'HudWinPanel' },
      { name: 'round-win-container', cls: 'WinPanelBasicContainer' },
      { name: 'round-win-result', cls: 'WinPanel__Result' },
      { name: 'round-win-mvp-section', cls: 'MVP_section' },
      { name: 'round-win-mvp', id: 'MVP' },
      { name: 'hud-alerts', id: 'HudAlerts' },
      { name: 'progress-bar', id: 'HudProgressBar' },
      { name: 'vote', id: 'HudVote' },
      { name: 'instructor', id: 'HudInstructor' },
      { name: 'reticle', id: 'HudReticle' },
      { name: 'damage-indicator', id: 'HudDamageIndicator' },
      { name: 'radio', id: 'HudRadio' },
      { name: 'trueview-active', ids: ['TrueViewCheckBox', 'TrueViewToggleButton', 'Settings', 'Contents'] },
      { name: 'camera-editor-panel', panel: function () { return inspector; } },
      { name: 'graph-editor-panel', id: 'GraphExpRoot' },
      { name: 'camera-timeline-panel', id: 'CamTimelineBar' }
    ];
    function updateDebug(st) {
      if (!st || !st.debug) { dbgPanel.visible = false; return; }
      dbgPanel.visible = true;
      var rsx = root.actualuiscale_x || ctx.actualuiscale_x || 1, rsy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
      var rawW = root.actuallayoutwidth || ctx.actuallayoutwidth || 0, rawH = root.actuallayoutheight || ctx.actuallayoutheight || 0;
      var rw = rawW / rsx, rh = rawH / rsy;
      var L = [];
      L.push('=== CAMERA EDITOR VIEWPORT DEBUG ===');
      L.push('window phys   ' + rawW.toFixed(0) + ' x ' + rawH.toFixed(0) + '   (= render target)');
      L.push('uiscale       ' + rsx.toFixed(4) + ' x ' + rsy.toFixed(4));
      L.push('window virt   ' + rw.toFixed(1) + ' x ' + rh.toFixed(1));
      L.push('aspect        ' + (rh > 0 ? (rw / rh).toFixed(4) : '0'));
      var g = dbgGeom;
      if (g) {
        L.push('-- preview rect (panorama) --');
        L.push('frac          x0=' + g.x0.toFixed(4) + ' y0=' + g.y0.toFixed(4) + ' x1=' + g.x1.toFixed(4) + ' y1=' + g.y1.toFixed(4));
        L.push('px virt       x=' + g.ox.toFixed(0) + ' y=' + g.oy.toFixed(0) + ' w=' + g.pw.toFixed(0) + ' h=' + g.ph.toFixed(0));
        L.push('scale sx,sy   ' + (g.x1 - g.x0).toFixed(4) + ' , ' + (g.y1 - g.y0).toFixed(4) + '  (insets x=0 top=0)');
        L.push('offset tx,ty  ' + (g.x0 * rw).toFixed(1) + ' , ' + (g.y0 * rh).toFixed(1));
        L.push('expected px   x=' + (g.x0 * rawW).toFixed(0) + ' y=' + (g.y0 * rawH).toFixed(0) +
               ' w=' + ((g.x1 - g.x0) * rawW).toFixed(0) + ' h=' + ((g.y1 - g.y0) * rawH).toFixed(0));
      } else { L.push('-- preview rect: not laid out yet --'); }
      var d = st.dbg || {};
      L.push('-- world blit (render layer) --');
      L.push('blitRan=' + (d.blitRan ? 'YES' : 'no') + '  scaleReq=' + (d.scaleReq ? 'on' : 'off') + '  rectValid=' + (d.previewValid ? 'yes' : 'no'));
      L.push('render target ' + (d.bbW || 0) + ' x ' + (d.bbH || 0));
      L.push('actual px     x=' + Number(d.vx || 0).toFixed(0) + ' y=' + Number(d.vy || 0).toFixed(0) +
             ' w=' + Number(d.vw || 0).toFixed(0) + ' h=' + Number(d.vh || 0).toFixed(0));
      if (g && d.blitRan) {
        var ex = g.x0 * rawW, ey = g.y0 * rawH, ew = (g.x1 - g.x0) * rawW, eh = (g.y1 - g.y0) * rawH, tol = 2.0;
        var match = Math.abs(ex - d.vx) <= tol && Math.abs(ey - d.vy) <= tol && Math.abs(ew - d.vw) <= tol && Math.abs(eh - d.vh) <= tol;
        L.push('blit rect match ' + (match ? 'YES (world only)' : 'NO  <-- expected vs actual differ'));
        L.push('HUD alignment is per-panel below; active crop/overlap means NOT locked');
      }
      var blockers = [];
      var inspAbs = dbgAbs(inspector);
      if (inspector.visible) blockers.push(dbgRectObj(inspAbs.x / rsx, inspAbs.y / rsy, (inspector.actuallayoutwidth || 0) / rsx, (inspector.actuallayoutheight || 0) / rsy));
      if (curBottomMode === 'graph' || curBottomMode === 'camera') blockers.push(dbgRectObj(0, rh - (curBottomMode === 'graph' ? GRAPH_DOCK_H + BOTTOM_LIFT : 180), Math.max(0, rw - INSPECTOR_W), curBottomMode === 'graph' ? GRAPH_DOCK_H + BOTTOM_LIFT : 180));
      L.push('-- panels (screen px) --');
      L.push('hud root      ' + dbgFmtPanel(ctx, rsx, rsy));
      var recs = [];
      for (var di = 0; di < dbgDescriptors.length; di++) {
        var rec = dbgPanelRecord(dbgDescriptors[di], rsx, rsy, g, blockers);
        recs.push(rec);
        L.push(rec.name + ' <' + (rec.id || '?') + '> src ' + dbgFmtRect(rec.source) +
          ' final ' + dbgFmtRect(rec.final) + ' area=' + rec.finalArea.toFixed(0) +
          ' crop=' + rec.cropPct.toFixed(1) + '% overlap=' +
          rec.editorOverlapPct.toFixed(1) + '% parent=<' + (rec.parent || '?') + '>' +
          (rec.missing ? ' MISSING' : (rec.inactive ? ' INACTIVE' : '')));
      }
      try { root.SetAttributeString('debugpanels', JSON.stringify(recs)); } catch (eJson) { root.SetAttributeString('debugpanels', '[]'); }
      dbgText.text = L.join('\n');
    }
)EDJS"
