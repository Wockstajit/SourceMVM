#pragma once

// World selection/translation overlay. Geometry comes from the render camera; input uses the
// same OS-cursor feed as Config sliders. No camera command is issued by this editor.
R"CFJS(
    var envOverlay = $.CreatePanel('Panel', root, 'EnvPositionOverlay');
    envOverlay.style.overflow = 'clip';
    envOverlay.hittest = false; envOverlay.hittestchildren = false;
    envOverlay.SetPanelEvent('onactivate', function () {});
    var envEdges = [], envAxes = [], envAxisNames = ['X', 'Y', 'Z'];
    var envAxisColors = ['#ff6767ff', '#6be699ff', '#65aaffff'];
    // An axis letter at a handle's tip. A Label positioned directly renders low, left and clipped
    // in game, away from its arrow, so the letter is centred in a positioned box instead; the box
    // is centred on the point just past the tip.
    var GIZMO_TAG = 24;
    function gizmoTag(parent, text, color) {
      var box = mk('Panel', parent); box.hittest = false;
      box.style.width = GIZMO_TAG + 'px'; box.style.height = GIZMO_TAG + 'px';
      var t = lbl(box, text, color, 17); t.hittest = false; t.style.fontWeight = 'bold';
      t.style.horizontalAlign = 'center'; t.style.verticalAlign = 'center';
      return box;
    }
    // Place a tag just beyond `tip`, along the handle from `origin` (both normalized points).
    function gizmoTagAt(tag, origin, tip, w, h) {
      tag.visible = !!(tip && tip[2]);
      if (!tag.visible) return;
      var dx = (tip[0] - origin[0]) * w, dy = (tip[1] - origin[1]) * h, len = Math.sqrt(dx * dx + dy * dy) || 1;
      var x = tip[0] * w + dx / len * 14 - GIZMO_TAG / 2, y = tip[1] * h + dy / len * 14 - GIZMO_TAG / 2;
      tag.style.position = x.toFixed(1) + 'px ' + y.toFixed(1) + 'px 0px';
    }
    for (var ei = 0; ei < 12; ei++) {
      var edge = mk('Panel', envOverlay); edge.hittest = false;
      edge.style.backgroundColor = '#f2d394bb'; envEdges.push(edge);
    }
    for (var ai = 0; ai < 3; ai++) {
      var shaft = mk('Panel', envOverlay); shaft.hittest = false;
      shaft.style.backgroundColor = envAxisColors[ai];
      envAxes.push({ shaft: shaft, name: gizmoTag(envOverlay, envAxisNames[ai], envAxisColors[ai]) });
    }
    var envMoveHint = lbl(envOverlay, 'Drag an axis · release to keep · Esc to cancel', S.label, 15);
    envMoveHint.hittest = false;
    envMoveHint.style.horizontalAlign = 'center'; envMoveHint.style.marginTop = '24px';
    envMoveHint.style.backgroundColor = '#101318e8'; envMoveHint.style.padding = '8px 14px';
    envMoveHint.style.borderRadius = '5px';
    var envPointerDown = false, envDragLocal = false, envDragRevision = 0, envDragAck = false;
    function envDrawLine(p, a, b, w, h, thickness) {
      p.visible = !!(a && b && a[2] && b[2]);
      if (!p.visible) return;
      var x = a[0] * w, y = a[1] * h, dx = (b[0] - a[0]) * w, dy = (b[1] - a[1]) * h;
      p.style.position = x.toFixed(2) + 'px ' + y.toFixed(2) + 'px 0px';
      p.style.width = Math.sqrt(dx * dx + dy * dy).toFixed(2) + 'px';
      p.style.height = thickness + 'px'; p.style.transformOrigin = '0% 50%';
      p.style.transform = 'rotateZ(' + (Math.atan2(dy, dx) * 180 / Math.PI).toFixed(2) + 'deg)';
    }
    function envAxisHit(g, u, v, aspect) {
      var best = -1, distance = 0.025;
      if (!g || !g.origin[2]) return best;
      for (var i = 0; i < 3; i++) {
        var a = g.origin, b = g.axes[i];
        if (!b[2]) continue;
        var dx = (b[0] - a[0]) * aspect, dy = b[1] - a[1], len = dx * dx + dy * dy;
        if (len < 0.000025) continue;
        var px = (u - a[0]) * aspect, py = v - a[1];
        var t = Math.max(0, Math.min(1, (px * dx + py * dy) / len));
        var d = Math.sqrt(Math.pow(px - t * dx, 2) + Math.pow(py - t * dy, 2));
        if (d < distance) { distance = d; best = i; }
      }
      return best;
    }
    renderHooks.push(function (s) {
      var down = !!s.lmb, editing = !!s.envEditing;
      var available = activeTab === 'environment' && !!s.cursor && !!s.enabled;
      if (!available && editing) envCommand('edit stop');
      envOverlay.visible = available && !!s.envGizmo;
      if (!available) { envDragLocal = false; envPointerDown = down; return; }
      var scale = root.actualuiscale_x || 1;
      var rw = root.actuallayoutwidth / scale, rh = root.actuallayoutheight / scale;
      var rect = String(root.GetAttributeString('previewrect', '')).split(/\s+/);
      var x = 0, y = 0, w = rw, h = rh, clipW = rw - INSPECTOR_W;
      if (rect.length === 4) {
        x = parseFloat(rect[0]) * rw; y = parseFloat(rect[1]) * rh;
        w = (parseFloat(rect[2]) - parseFloat(rect[0])) * rw;
        h = (parseFloat(rect[3]) - parseFloat(rect[1])) * rh; clipW = w;
      }
      if (!(w > 0 && h > 0)) return;
      envOverlay.style.position = x + 'px ' + y + 'px 0px';
      envOverlay.style.width = clipW + 'px'; envOverlay.style.height = h + 'px';
      envOverlay.hittest = editing;
      envMoveHint.visible = false; // shown by the props overlay's hint, at the top with the rest
      var origin = root.GetPositionWithinWindow(), mx = (s.mx - origin.x) / scale, my = (s.my - origin.y) / scale;
      var u = (mx - x) / w, v = (my - y) / h, aspect = w / h, g = s.envGizmo;
      var inside = mx >= x && mx < x + clipW && my >= y && my < y + h;
      // A popup, slider drag or resize grip owns its press, even if it overlaps the preview.
      var hit = editing && inside && !settingsPopup.isOpen() && !cfgDragging()
        ? envAxisHit(g, u, v, aspect) : -1;
      if (envDragLocal) {
        if (s.envDragging) envDragAck = true;
        if (!editing || (s.envEditRevision > envDragRevision && !s.envDragging)) envDragLocal = false;
      }
      if (editing && down && !envPointerDown && hit >= 0) {
        envDragLocal = true; envDragAck = false; envDragRevision = s.envEditRevision || 0;
        envCommand('edit begin ' + ['x', 'y', 'z'][hit] + ' ' + u + ' ' + v + ' ' + aspect);
      } else if (editing && down && !envPointerDown && hit < 0 && inside && !envDragLocal &&
                 !settingsPopup.isOpen() && !cfgDragging()) {
        // A click off the arrows picks again: another object, or nothing (deselects).
        cmd('mirv_filmmaker props pick ' + u.toFixed(4) + ' ' + v.toFixed(4));
      } else if (envDragLocal && down && envDragAck) {
        envCommand('edit drag ' + u + ' ' + v);
      } else if (envDragLocal && !down) {
        envCommand('edit drag ' + u + ' ' + v);
        envCommand('edit commit'); envDragLocal = false;
      }
      envPointerDown = down;
      if (!g) return;
      var n = 0;
      for (var corner = 0; corner < 8; corner++) {
        for (var bit = 1; bit <= 4; bit *= 2) {
          if (!(corner & bit)) envDrawLine(envEdges[n++], g.bounds[corner], g.bounds[corner | bit], w, h, 1);
        }
      }
      for (var i = 0; i < 3; i++) {
        var axis = envAxes[i], tip = g.axes[i];
        envDrawLine(axis.shaft, g.origin, tip, w, h, hit === i ? 5 : 3);
        axis.shaft.visible = editing && axis.shaft.visible;
        gizmoTagAt(axis.name, g.origin, tip, w, h);
        axis.name.visible = editing && axis.name.visible;
      }
    });
)CFJS"
