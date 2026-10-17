#pragma once

// GUIDE fragment of the onboarding panel (concatenated into kOnboardingJs, shares its scope): the
// parts that point at things.
//
//   * Spotlight. A step can name a target: one or more panel ids, searched in this Panorama
//     context. While the target is on screen everything else is dimmed (four dark panels around
//     a hole) and a gold ring pulses around it. Nothing here takes clicks, so the user clicks the
//     real control through the hole.
//   * World markers. A step can name points in the map (the onboarding demo's fights). C++ sends
//     the rendered camera's world-to-clip matrix and eye position; each point is projected here,
//     drawn as a ring with its name and distance, and pinned to the screen edge with an arrow when
//     it is behind or off screen. Close enough, the ring turns gold and says "Press K".
//
// Panel rects are measured in window space (GetPositionWithinWindow) against the root, never with
// actualxoffset, which is relative to each panel's own parent.

R"OBJS(
    function layer(id, z) {
      var l = P(root, id);
      l.style.width = '100%'; l.style.height = '100%';
      l.style.overflow = 'noclip';
      l.style.zIndex = z;
      return l;
    }
    // Linear mode's click blocker: on for steps where nothing is lit, so only the bubble works.
    var blocker = layer('MvmObBlock', '1');
    var guide = layer('MvmObSpot', '1');     // the dim and the ring, faded together
    var marks = layer('MvmObMarks', '2');    // world markers, never dimmed
    var dims = [];
    for (var di = 0; di < 4; di++) {
      var dp = P(guide, '');
      dp.style.backgroundColor = '#05070acc';
      dp.visible = false;
      dims.push(dp);
    }
    var ring = P(guide, 'MvmObRing');
    ring.style.border = '2px solid ' + C.gold;
    ring.style.borderRadius = '9px';
    ring.style.boxShadow = '#f0b32366 0px 0px 14px 2px';
    ring.visible = false;
    var dimOpacity = 0, spotRect = null;

    // On-screen rect of a panel in this context, in css px, or null when it is missing, hidden,
    // not laid out yet (positions near 3.4e38) or off screen.
    function rectOf(p, sz) {
      if (!p || !p.IsValid || !p.IsValid()) return null;
      var inBar = false;
      for (var q = p; q; q = q.GetParent()) {
        if (q.visible === false) return null;
        if (q.id === 'ControlRow') inBar = true;
        if (q === ctx) break;
      }
      var a = p.GetPositionWithinWindow(), o = root.GetPositionWithinWindow();
      if (!a || !(Math.abs(a.x) < 1e30 && Math.abs(a.y) < 1e30)) return null;
      // A docked demo bar is drawn scaled down by $.MirvBarFitK (CameraTimelineJs.h). The window
      // position already includes that; the layout size does not.
      var fit = inBar && typeof $.MirvBarFitK === 'number' && $.MirvBarFitK > 0.05 && $.MirvBarFitK <= 1 ? $.MirvBarFitK : 1;
      var w = (p.actuallayoutwidth || 0) * fit / sz.sx, h = (p.actuallayoutheight || 0) * fit / sz.sy;
      if (!(w > 2 && h > 2)) return null;
      var r = { x: (a.x - o.x) / sz.sx, y: (a.y - o.y) / sz.sy, w: w, h: h };
      // Off screen counts as not found: a ring around nothing is worse than no ring.
      if (r.x + r.w < 0 || r.y + r.h < 0 || r.x > sz.w || r.y > sz.h) return null;
      return r;
    }

    // Which clicks get through this frame. 'target': only the lit hole (the dims take the rest);
    // 'none': only the bubble; 'free': everything.
    var clickMode = 'free';
    function setClicks(mode, haveSpot) {
      var dimsCatch = mode === 'target' && haveSpot;
      var block = mode === 'none' || (mode === 'target' && !haveSpot);
      if (blocker.hittest !== block) blocker.hittest = block;
      for (var i = 0; i < 4; i++) if (dims[i].hittest !== dimsCatch) dims[i].hittest = dimsCatch;
      clickMode = mode;
    }
    // The same rule for the window procedure, which sees every click before CS2 and Panorama do
    // (Onboarding_BlocksClick): the zones a click may land in, in client fractions. Panorama's own
    // hit testing is not enough on its own: some workspace panels took clicks through the dim.
    var clicksSent = '', asstSent = '';
    var clickArea = null; // the step's clickArea this frame (css px), or null
    function zoneSpec(x0, y0, x1, y1, sz, pad) {
      function q(v) { return (Math.round(v * 200) / 200).toFixed(3); }
      return q((x0 - pad) / sz.w) + ' ' + q((y0 - pad) / sz.h) + ' ' + q((x1 + pad) / sz.w) + ' ' + q((y1 + pad) / sz.h);
    }
    // asstParts: the bubble's rect and Clippy's (not the box around both, whose empty corner
    // can sit over a control).
    function sendClickZones(sz, asstParts) {
      var spec = 'free';
      if (clickMode !== 'free' && asstParts) {
        spec = 'only';
        for (var i = 0; i < asstParts.length; i++)
          spec += ' ' + zoneSpec(asstParts[i].x0, asstParts[i].y0, asstParts[i].x1, asstParts[i].y1, sz, 8);
        if (clickMode === 'target' && spotRect && dimOpacity > 0.5)
          spec += ' ' + zoneSpec(spotRect.x, spotRect.y, spotRect.x + spotRect.w, spotRect.y + spotRect.h, sz, 6);
        if (clickMode === 'target' && clickArea)
          spec += ' ' + zoneSpec(clickArea.x, clickArea.y, clickArea.x + clickArea.w, clickArea.y + clickArea.h, sz, 4);
      }
      if (spec !== clicksSent) { clicksSent = spec; cmd('clicks ' + spec); }
      // Where he is, on every step: a press there is the bubble's, never the preview's.
      var asst = 'none';
      if (asstParts) {
        asst = '';
        for (var k = 0; k < asstParts.length; k++)
          asst += (k ? ' ' : '') + zoneSpec(asstParts[k].x0, asstParts[k].y0, asstParts[k].x1, asstParts[k].y1, sz, 4);
      }
      if (asst !== asstSent) { asstSent = asst; cmd('asst ' + asst); }
    }
    function place4(p, x, y, w, h) {
      p.style.marginLeft = Math.round(x) + 'px'; p.style.marginTop = Math.round(y) + 'px';
      p.style.width = Math.max(0, Math.round(w)) + 'px'; p.style.height = Math.max(0, Math.round(h)) + 'px';
    }
    // rect: what to light, or null. Pads it a little so the ring does not sit on the control.
    function spotlight(rect, sz, dt, now) {
      var want = rect ? 1 : 0;
      dimOpacity += (want - dimOpacity) * Math.min(1, dt * 8);
      if (Math.abs(want - dimOpacity) < 0.01) dimOpacity = want;
      if (rect) spotRect = rect;
      var show = dimOpacity > 0 && !!spotRect;
      for (var i = 0; i < 4; i++) dims[i].visible = show;
      ring.visible = show;
      if (!show) return;
      var pad = 6, x0 = spotRect.x - pad, y0 = spotRect.y - pad;
      var x1 = spotRect.x + spotRect.w + pad, y1 = spotRect.y + spotRect.h + pad;
      place4(dims[0], 0, 0, sz.w, y0);
      place4(dims[1], 0, y1, sz.w, sz.h - y1);
      place4(dims[2], 0, y0, x0, y1 - y0);
      place4(dims[3], x1, y0, sz.w - x1, y1 - y0);
      place4(ring, x0 - 2, y0 - 2, x1 - x0 + 4, y1 - y0 + 4);
      var pulse = 0.65 + 0.35 * (0.5 + 0.5 * Math.sin(now / 1000 * Math.PI * 2 / 1.4));
      guide.style.opacity = dimOpacity.toFixed(3);
      ring.style.opacity = pulse.toFixed(3);
    }

    // ---------------------------------------------------------------- world markers
    var NEAR = 56;  // units: close enough to drop the camera key here
    var markers = [];
    function markerBuild() {
      var m = P(marks, '');
      m.style.width = '160px'; m.style.height = '86px';
      m.style.overflow = 'noclip';
      var dot = P(m, '');
      dot.style.width = '34px'; dot.style.height = '34px'; dot.style.borderRadius = '17px';
      dot.style.horizontalAlign = 'center';
      dot.style.border = '3px solid ' + C.ink;
      dot.style.boxShadow = '#000000aa 0px 2px 8px 0px';
      var core = P(dot, '');
      core.style.width = '8px'; core.style.height = '8px'; core.style.borderRadius = '4px';
      core.style.horizontalAlign = 'center'; core.style.verticalAlign = 'center';
      core.style.backgroundColor = C.ink;
      var name = L(m, '', C.ink, 14);
      name.style.horizontalAlign = 'center'; name.style.marginTop = '40px'; name.style.fontWeight = 'bold';
      var sub = L(m, '', C.muted, 12);
      sub.style.horizontalAlign = 'center'; sub.style.marginTop = '60px';
      m.visible = false;
      return { m: m, dot: dot, core: core, name: name, sub: sub, near: null };
    }
    function project(vp, p) {
      var w = vp[12] * p[0] + vp[13] * p[1] + vp[14] * p[2] + vp[15];
      if (!(w > 0.001)) return { behind: true, x: 0, y: 0 };
      var nx = (vp[0] * p[0] + vp[1] * p[1] + vp[2] * p[2] + vp[3]) / w;
      var ny = (vp[4] * p[0] + vp[5] * p[1] + vp[6] * p[2] + vp[7]) / w;
      return { behind: false, x: (nx + 1) * 0.5, y: (1 - ny) * 0.5 };
    }
    // list: [{ name, pos: [x, y, z], done: bool }]. Returns the index of the marker the camera
    // is close to, or -1.
    function worldMarkers(list, sz) {
      var nearIndex = -1;
      var vp = st.vp, eye = st.eye;
      // The world is drawn into the workspace's preview box when one is open (screen fractions).
      var pv = st.pv && st.pv.length === 4 ? st.pv : [0, 0, 1, 1];
      var bx0 = pv[0] * sz.w, by0 = pv[1] * sz.h, bw = (pv[2] - pv[0]) * sz.w, bh = (pv[3] - pv[1]) * sz.h;
      for (var i = 0; i < Math.max(list.length, markers.length); i++) {
        if (i >= markers.length) markers.push(markerBuild());
        var mk = markers[i], item = list[i];
        if (!item || !vp || vp.length !== 16 || !eye) { mk.m.visible = false; continue; }
        var dx = item.pos[0] - eye[0], dy = item.pos[1] - eye[1], dz = item.pos[2] - eye[2];
        var dist = Math.sqrt(dx * dx + dy * dy + dz * dz), near = dist < NEAR && !item.done;
        if (near && nearIndex < 0) nearIndex = i;
        var pr = project(vp, item.pos), x = bx0 + pr.x * bw, y = by0 + pr.y * bh;
        var edge = 34, onScreen = !pr.behind && x > bx0 + edge && x < bx0 + bw - edge && y > by0 + edge && y < by0 + bh - edge;
        if (!onScreen) {
          // Pin to the edge of the visible world on the side the point is on.
          var cx = bx0 + bw / 2, cy = by0 + bh / 2;
          var ax = pr.behind ? 2 * cx - x : x, ay = pr.behind ? 2 * cy - y : y, vx = ax - cx, vy = ay - cy;
          var s = Math.min((bw / 2 - edge) / Math.max(1, Math.abs(vx)), (bh / 2 - edge) / Math.max(1, Math.abs(vy)));
          x = cx + vx * s; y = cy + vy * s;
        }
        mk.m.visible = true;
        mk.m.style.marginLeft = Math.round(x - 80) + 'px';
        mk.m.style.marginTop = Math.round(y - 17) + 'px';
        var col = item.done ? '#7fd88f' : (near ? C.gold : C.ink);
        if (mk.near !== col) {
          mk.near = col;
          mk.dot.style.border = '3px solid ' + col;
          mk.core.style.backgroundColor = col;
          mk.dot.style.backgroundColor = near ? '#f0b32333' : 'none';
        }
        mk.name.text = item.name;
        mk.sub.text = item.done ? 'Placed' : (near ? 'Press K' : (onScreen ? Math.round(dist) + ' units' : 'This way'));
        mk.m.style.opacity = onScreen || item.done ? '1' : '0.8';
      }
      return nearIndex;
    }
    function hideMarkers() { for (var i = 0; i < markers.length; i++) markers[i].m.visible = false; }
)OBJS"
