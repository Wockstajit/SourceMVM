#pragma once

// CS2 ASSETS in the view: the in-world editor over the Config preview (Filmmaker/Props).
// Placing: the model follows the surface under the cursor; a click drops it. Editing: the selected
// prop carries move arrows and three turn rings (turn, tilt, roll); drag one, or click another prop
// to select it. While Scene Objects is open a click selects a prop even when none is selected. The
// right button held in the view looks around (MovieMode, UiClick::RightPressLooks); a quick
// right-click (MovieMode tells them apart) selects what is under it and opens its menu, the
// outliner's ($.ConfigHud.viewMenu, propsMenuRequest). Geometry (`st.props.gizmo`) is
// projected by C++ from the rendered camera, in coordinates normalized over the full frame, which
// the scaled preview shows whole. Input is the OS-cursor feed the environment gizmo uses; its
// envDrawLine/envAxisHit/gizmoTag helpers are shared (same script scope).
R"CFJS(
    var propsOverlay = $.CreatePanel('Panel', root, 'PropsEditOverlay');
    propsOverlay.style.overflow = 'clip';
    propsOverlay.hittest = false; propsOverlay.hittestchildren = false;
    propsOverlay.SetPanelEvent('onactivate', function () {});
    var PROPS_PLACE = '#7fe0d0cc', PROPS_BOX = '#f2d39488', PROPS_RING_POINTS = 32;
    // Turn about the vertical (blue, like Z), tilt about the prop's side (green, Y), roll about its
    // forward (red, X): each ring wears the colour of the axis it turns about.
    var PROPS_RINGS = ['turn', 'tilt', 'roll'], PROPS_RING_NAMES = ['Turning', 'Tilting', 'Rolling'];
    var PROPS_RING_COLORS = ['#65aaffdd', '#6be699dd', '#ff6767dd'];
    var propsEdges = [], propsRingLines = [[], [], []], propsAxes = [];
    for (var pe = 0; pe < 12; pe++) { var pEdge = mk('Panel', propsOverlay); pEdge.hittest = false; propsEdges.push(pEdge); }
    for (var prn = 0; prn < 3; prn++) {
      for (var pr = 0; pr < PROPS_RING_POINTS; pr++) {
        var pRing = mk('Panel', propsOverlay); pRing.hittest = false;
        pRing.style.backgroundColor = PROPS_RING_COLORS[prn]; propsRingLines[prn].push(pRing);
      }
    }
    for (var pa = 0; pa < 3; pa++) {
      var pShaft = mk('Panel', propsOverlay); pShaft.hittest = false;
      pShaft.style.backgroundColor = envAxisColors[pa];
      propsAxes.push({ shaft: pShaft, name: gizmoTag(propsOverlay, envAxisNames[pa], envAxisColors[pa]) });
    }
    // The paint brush outline (teal; red while erasing).
    var propsBrushLines = [];
    for (var pb = 0; pb < PROPS_RING_POINTS; pb++) { var pBrush = mk('Panel', propsOverlay); pBrush.hittest = false; propsBrushLines.push(pBrush); }
    // Where the next copies will land (C++ previews the stroke's own scatter).
    var propsBrushDots = [];
    for (var pd = 0; pd < 6; pd++) {
      var pDot = mk('Panel', propsOverlay); pDot.hittest = false;
      pDot.style.width = '9px'; pDot.style.height = '9px'; pDot.style.borderRadius = '50%';
      pDot.style.backgroundColor = '#7fe0d0ee'; pDot.style.border = '1px solid #0b2b26ff';
      propsBrushDots.push(pDot);
    }
    var propsPaintDown = false;
    // Every placed light and mesh carries an icon over the view: a bulb with rays (point light), a
    // cone (spot), a glowing panel (area), a cube (your mesh), drawn in the light's own colour on a
    // dark disc ringed in its kind's colour (the in-world markers' palette, sent as `accent`), with
    // its type beside it in that colour. A click on the icon selects that object (props pick ranks
    // an icon above anything behind it). The glyphs are drawn from line panels -- the same
    // technique as the gizmo arrows -- because CS2 ships no such icons and the earlier circle /
    // diamond shapes all read as the same square in game.
    var PROPS_MARKERS = 48, PROPS_MARK = 26, propsMarkers = [];
    var PROPS_MARKER_TAGS = { point: 'POINT', spot: 'SPOT', area: 'AREA', mesh: 'MESH', camera: 'CAMERA' };
    function propsSeg(host, x1, y1, x2, y2, color, th) {
      var l = mk('Panel', host); l.hittest = false;
      var dx = x2 - x1, dy = y2 - y1;
      l.style.position = x1.toFixed(1) + 'px ' + (y1 - th / 2).toFixed(1) + 'px 0px';
      l.style.width = Math.sqrt(dx * dx + dy * dy).toFixed(1) + 'px'; l.style.height = th + 'px';
      l.style.transformOrigin = '0% 50%'; l.style.backgroundColor = color;
      l.style.transform = 'rotateZ(' + (Math.atan2(dy, dx) * 180 / Math.PI).toFixed(1) + 'deg)';
    }
    function propsRing(host, cx, cy, r, color, th) {
      var c = mk('Panel', host); c.hittest = false;
      c.style.position = (cx - r) + 'px ' + (cy - r) + 'px 0px';
      c.style.width = (2 * r) + 'px'; c.style.height = (2 * r) + 'px'; c.style.borderRadius = r + 'px';
      c.style.border = th + 'px solid ' + color;
    }
    // The glyph for one kind, centred in the 26px disc.
    function propsGlyph(host, kind, color) {
      host.RemoveAndDeleteChildren();
      var c = 13;
      if (kind === 'point') {
        propsRing(host, c, c, 4, color, 2);
        for (var i = 0; i < 8; i++) {
          var a = i * Math.PI / 4, ca = Math.cos(a), sa = Math.sin(a);
          propsSeg(host, c + ca * 7, c + sa * 7, c + ca * 10, c + sa * 10, color, 2);
        }
      } else if (kind === 'spot') {
        propsRing(host, c, 7, 2, color, 2);
        propsSeg(host, c, 8, 7, 19, color, 2); propsSeg(host, c, 8, 19, 19, color, 2);
        propsSeg(host, 7, 19, 19, 19, color, 2);
      } else if (kind === 'camera') {
        // A camera object (scene-object-rigs.md): a body and the lens cone in front of it.
        propsSeg(host, 5, 9, 15, 9, color, 2); propsSeg(host, 5, 17, 15, 17, color, 2);
        propsSeg(host, 5, 9, 5, 17, color, 2); propsSeg(host, 15, 9, 15, 17, color, 2);
        propsSeg(host, 15, 11, 21, 8, color, 2); propsSeg(host, 15, 15, 21, 18, color, 2);
        propsSeg(host, 21, 8, 21, 18, color, 2);
      } else if (kind === 'area') {
        propsSeg(host, 6, 7, 20, 7, color, 2); propsSeg(host, 6, 12, 20, 12, color, 2);
        propsSeg(host, 6, 6, 6, 13, color, 2); propsSeg(host, 20, 6, 20, 13, color, 2);
        propsSeg(host, 9, 15, 8, 20, color, 2); propsSeg(host, c, 15, c, 20, color, 2); propsSeg(host, 17, 15, 18, 20, color, 2);
      } else {
        // An isometric cube: the outline, then the three edges meeting at the near corner.
        var P = [[13, 5], [20, 9], [20, 17], [13, 21], [6, 17], [6, 9]];
        for (var k = 0; k < 6; k++) propsSeg(host, P[k][0], P[k][1], P[(k + 1) % 6][0], P[(k + 1) % 6][1], color, 1.5);
        propsSeg(host, 13, 13, 6, 9, color, 1.5); propsSeg(host, 13, 13, 20, 9, color, 1.5); propsSeg(host, 13, 13, 13, 21, color, 1.5);
      }
    }
    for (var pm = 0; pm < PROPS_MARKERS; pm++) {
      var mBox = mk('Panel', propsOverlay); mBox.hittest = false;
      mBox.style.width = PROPS_MARK + 'px'; mBox.style.height = PROPS_MARK + 'px';
      var mDisc = mk('Panel', mBox); mDisc.hittest = false;
      mDisc.style.width = PROPS_MARK + 'px'; mDisc.style.height = PROPS_MARK + 'px'; mDisc.style.borderRadius = (PROPS_MARK / 2) + 'px';
      mDisc.style.backgroundColor = '#101318e6';
      var mGlyph = mk('Panel', mBox); mGlyph.hittest = false;
      mGlyph.style.width = PROPS_MARK + 'px'; mGlyph.style.height = PROPS_MARK + 'px';
      // A bare positioned Label sits low in game: each tag is a positioned Panel holding its label.
      var mTag = mk('Panel', propsOverlay); mTag.hittest = false;
      mTag.style.backgroundColor = '#101318cc'; mTag.style.padding = '1px 4px'; mTag.style.borderRadius = '3px';
      var mText = lbl(mTag, '', '#e8ecf2', 11); mText.hittest = false; mText.style.fontWeight = 'bold';
      propsMarkers.push({ box: mBox, disc: mDisc, glyph: mGlyph, tag: mTag, text: mText, sig: '' });
    }
    // What the cursor is over: its outline and a name tag beside the cursor.
    var propsHoverEdges = [];
    for (var ph = 0; ph < 12; ph++) {
      var hEdge = mk('Panel', propsOverlay); hEdge.hittest = false;
      hEdge.style.backgroundColor = '#ffffffcc'; propsHoverEdges.push(hEdge);
    }
    var propsHoverTag = mk('Panel', propsOverlay);
    propsHoverTag.hittest = false;
    var propsHoverText = lbl(propsHoverTag, '', '#ffffff', 13); propsHoverText.hittest = false;
    propsHoverTag.style.backgroundColor = '#101318ee'; propsHoverTag.style.padding = '3px 8px';
    propsHoverTag.style.borderRadius = '4px'; propsHoverTag.style.border = '1px solid #ffffff44';
    var propsHoverSent = false;
    function propsDrawMarkers(p, w, h, hover) {
      var marks = p.markers || [];
      for (var mi = 0; mi < PROPS_MARKERS; mi++) {
        var m = propsMarkers[mi], at = marks[mi] && marks[mi].at;
        var on = !!(at && at[2]);
        m.box.visible = on; m.tag.visible = on;
        if (!on) continue;
        var mark = marks[mi], mesh = mark.kind === 'mesh';
        var hot = hover && hover.name === mark.name && hover.at &&
          Math.abs(hover.at[0] - at[0]) < 0.0001 && Math.abs(hover.at[1] - at[1]) < 0.0001;
        var color = mesh ? '#b9a3ffff' : (mark.color || '#ffffff');
        var sig = mark.kind + '|' + color + '|' + (mark.accent || '');
        if (sig !== m.sig) {
          m.sig = sig; propsGlyph(m.glyph, mark.kind, color);
          m.text.style.color = mark.accent || '#e8ecf2';
        }
        m.disc.style.border = (mark.selected ? '2px solid #ffffff' : hot ? '2px solid ' + ENV_ACCENT
          : mark.accent ? '2px solid ' + mark.accent : '1px solid #ffffff40');
        m.disc.style.backgroundColor = mark.selected ? '#2a3442f0' : hot ? '#1c2530f0' : '#101318e6';
        m.box.style.position = (at[0] * w - PROPS_MARK / 2).toFixed(1) + 'px ' + (at[1] * h - PROPS_MARK / 2).toFixed(1) + 'px 0px';
        m.text.text = mesh ? mark.name : (PROPS_MARKER_TAGS[mark.kind] || 'LIGHT');
        m.tag.style.position = (at[0] * w + PROPS_MARK / 2 + 4).toFixed(1) + 'px ' + (at[1] * h - 8).toFixed(1) + 'px 0px';
      }
    }
    function propsDrawHover(hover, w, h, mx, my, x, y) {
      var bounds = hover && hover.bounds, n = 0;
      for (var corner = 0; corner < 8 && bounds; corner++) {
        for (var bit = 1; bit <= 4; bit *= 2) {
          if (corner & bit) continue;
          envDrawLine(propsHoverEdges[n++], bounds[corner], bounds[corner | bit], w, h, 2);
        }
      }
      for (; n < 12; n++) propsHoverEdges[n].visible = false;
      propsHoverTag.visible = !!(hover && hover.name);
      if (!propsHoverTag.visible) return;
      propsHoverText.text = hover.name + (hover.kind ? '   ·   ' + hover.kind : '');
      propsHoverTag.style.position = (mx - x + 16).toFixed(1) + 'px ' + (my - y + 18).toFixed(1) + 'px 0px';
    }
    function propsHideHandles() {
      var all = propsEdges.concat(propsBrushLines);
      for (var i = 0; i < all.length; i++) all[i].visible = false;
      for (var r = 0; r < 3; r++) for (var k = 0; k < propsRingLines[r].length; k++) propsRingLines[r][k].visible = false;
      for (var a = 0; a < 3; a++) { propsAxes[a].shaft.visible = false; propsAxes[a].name.visible = false; }
      for (var d = 0; d < propsBrushDots.length; d++) propsBrushDots[d].visible = false;
      propsHint.visible = false;
    }
)CFJS" R"CFJS(
    var propsHint = lbl(propsOverlay, '', S.label, 15);
    propsHint.hittest = false;
    propsHint.style.horizontalAlign = 'center'; propsHint.style.verticalAlign = 'top'; propsHint.style.marginTop = '24px';
    propsHint.style.backgroundColor = '#101318e8'; propsHint.style.padding = '8px 14px';
    propsHint.style.borderRadius = '5px';
    var propsPointerDown = false, propsGrabbing = false;
    // A quick right-click in the view: pick there, then open the picked object's outliner menu once
    // the pick lands in a later state. A new selection opens at once; the same one (it was already
    // selected) after a state or two; nothing under the pointer drops the menu.
    var propsMenuRequest = false, propsMenuPending = null;
    function propsOpenMenu(key) { var it = olFind(key); if (!it) return; olSelect(key); olMenu(it); }
    var propsLastU = -1, propsLastV = -1, propsLastSnap = false;
    // The ring nearest (u, v) within reach: 0 turn, 1 tilt, 2 roll, -1 none. Aspect-corrected.
    function propsRingHit(g, u, v, aspect) {
      var best = -1, reach = 0.02, rings = g.rings || [];
      for (var r = 0; r < rings.length; r++) {
        var ring = rings[r];
        for (var i = 0; i < ring.length; i++) {
          var a = ring[i], b = ring[(i + 1) % ring.length];
          if (!a[2] || !b[2]) continue;
          var dx = (b[0] - a[0]) * aspect, dy = b[1] - a[1], len = dx * dx + dy * dy;
          if (len < 0.0000001) continue;
          var px = (u - a[0]) * aspect, py = v - a[1];
          var t = Math.max(0, Math.min(1, (px * dx + py * dy) / len));
          var d = Math.sqrt(Math.pow(px - t * dx, 2) + Math.pow(py - t * dy, 2));
          if (d < reach) { reach = d; best = r; }
        }
      }
      return best;
    }
    renderHooks.push(function (s) {
      var p = s.props, down = !!s.lmb;
      var placing = !!(p && p.placing), g = p && p.gizmo, painting = !!(p && p.paint && p.paint.active);
      // Leaving the tab or closing Config ends a placement. Looking around (the right button held
      // hands the mouse to the camera) only pauses the input: the placement waits for the cursor.
      var shown = activeTab === 'environment' && !!s.enabled && !!p;
      if (!shown && (placing || painting)) propsCommand('cancel');
      if ((!shown || !s.cursor) && propsPaintDown) { propsCommand('paint release'); propsPaintDown = false; }
      if ((!shown || !s.cursor) && propsGrabbing) { propsCommand('release'); propsGrabbing = false; }
      // On the Environment tab a click in the view always picks: a CS2 asset, a placed mesh or a
      // light (props pick ranks them all). A light or mesh being moved owns the press (its own
      // gizmo hook, which picks again when a click misses its arrows).
      // The light dots and the hover outline show on the whole tab, also while a light or mesh is
      // being moved (its own gizmo then owns the clicks).
      var editing = shown && !s.envEditing;
      propsOverlay.visible = shown;
      if (!shown) {
        if (propsHoverSent) { propsCommand('hoverat -1 -1'); propsHoverSent = false; }
        propsPointerDown = down; return;
      }
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
      propsOverlay.style.position = x + 'px ' + y + 'px 0px';
      propsOverlay.style.width = clipW + 'px'; propsOverlay.style.height = h + 'px';
      // The overlay takes the clicks over the view while it edits, so they never reach the game.
      propsOverlay.hittest = editing && !!s.cursor;
      var origin = root.GetPositionWithinWindow(), mx = (s.mx - origin.x) / scale, my = (s.my - origin.y) / scale;
      var u = (mx - x) / w, v = (my - y) / h, aspect = w / h;
      var inside = mx >= x && mx < x + clipW && my >= y && my < y + h;
      // A popup, slider drag, open menu or resize grip owns its press, even over the preview.
      var free = !!s.cursor && inside && !settingsPopup.isOpen() && !cfgDragging() && !cfgDropOpen;
      var pressed = down && !propsPointerDown, moved = Math.abs(u - propsLastU) + Math.abs(v - propsLastV) > 0.0015;
      // Hover: what a click here would pick, asked for as the cursor moves (not mid-drag, placing
      // or painting, where the cursor is busy with something else).
      var hovering = free && !down && !placing && !painting && !propsGrabbing;
      if (hovering && (moved || !propsHoverSent)) { propsCommand('hoverat ' + u.toFixed(4) + ' ' + v.toFixed(4)); propsHoverSent = true; }
      else if (!hovering && propsHoverSent) { propsCommand('hoverat -1 -1'); propsHoverSent = false; }
      var hover = propsHoverSent ? p.hover : null;
      if (propsMenuRequest) {
        propsMenuRequest = false;
        if (inside && !placing && !painting && !propsGrabbing) {
          propsCommand('pick ' + u.toFixed(4) + ' ' + v.toFixed(4));
          propsMenuPending = { was: p.selected || 0, wasEnv: s.envSelected || 0, at: Date.now() };
        }
      } else if (propsMenuPending) {
        // The pick is a queued console command; its result arrives a few states later. A changed
        // selection is the answer at once; an unchanged one counts after 250 ms; 600 ms of nothing
        // (empty ground deselects) drops the menu.
        var pm = propsMenuPending, envSel = s.envSelected || 0, age = Date.now() - pm.at;
        if (p.selected && (p.selected !== pm.was || age >= 250)) {
          propsMenuPending = null; propsOpenMenu('p' + p.selected);
        } else if (!p.selected && envSel && (envSel !== pm.wasEnv || age >= 250)) {
          propsMenuPending = null; propsOpenMenu('e' + envSel);
        } else if (age >= 600) propsMenuPending = null;
      }
      propsDrawMarkers(p, w, h, hover);
      propsDrawHover(hover, w, h, mx, my, x, y);
      if (!editing) {
        propsHideHandles();
        // A light or mesh being moved owns the clicks, but its hint sits where every other hint in
        // the view does (the environment gizmo's own label rendered elsewhere in game).
        if (s.envEditing && s.cursor) {
          propsHint.visible = true;
          propsHint.text = 'Drag an arrow to move   ·   release to keep   ·   Esc: cancel   ·   click elsewhere to deselect';
        }
        if (moved) { propsLastU = u; propsLastV = v; }
        propsPointerDown = down; return;
      }
      var axis = -1, ring = -1, selected = !!p.selected;
      // A press owns its eventual release before the queued pick reaches the backend. It only
      // describes a move after the backend confirms a grab: empty space and locked assets do not.
      var moving = propsGrabbing && selected && !!p.grab;
      // The idle "click to select" line belongs to SCENE OBJECTS: on Looks, Sky, Fog... it sat over
      // the view unasked (user report 2026-10-13). Work in progress (placing, painting, a drag, a
      // selection) keeps its hint on any card, since it describes what the mouse is doing now.
      propsHint.visible = !!(cfgRails.environment && cfgRails.environment.sel === 'env.scene') || placing || painting || moving || selected;
      // Shift snaps a drag: the arrows keep the prop on the floor, the rings turn in 15-degree steps.
      var snap = !!s.shift, snapArg = snap ? ' 1' : '';
      if (painting) {
        var erasing = snap || !!p.paint.erasing, uv = u.toFixed(4) + ' ' + v.toFixed(4);
        propsHint.text = (erasing ? 'Erasing ' : 'Painting ') + (p.paint.name || '') + '   ·   hold the mouse   ·   Shift: erase   ·   hold right-click: look around   ·   Esc: done';
        if (free && down) { propsCommand('paint ' + (erasing ? 'erase ' : 'stroke ') + uv); propsPaintDown = true; }
        else if (free && moved) propsCommand('paint hover ' + uv);
        if (!down && propsPaintDown) { propsCommand('paint release'); propsPaintDown = false; }
      } else if (placing) {
        propsHint.text = 'Click to drop ' + (p.placingName || 'it') + '   ·   hold right-click: look around   ·   Esc: cancel';
        if (free && moved) propsCommand('hover ' + u.toFixed(4) + ' ' + v.toFixed(4));
        if (free && pressed) propsCommand('drop');
      } else {
        var grabbed = PROPS_RINGS.indexOf(p.grab);
        propsHint.text = moving
          ? (grabbed >= 0 ? PROPS_RING_NAMES[grabbed] + (snap ? ' in 15° steps' : '   ·   Shift: 15° steps') : p.grab === 'move' ? 'Moving over the ground' : 'Moving' + (snap ? ' on the floor' : '   ·   Shift: snap to the floor'))
            + '   ·   Esc: put it back'
          : selected ? 'Drag it to move it over the ground   ·   an arrow for one axis   ·   a ring to turn   ·   right-click: its menu   ·   hold right-click: look around'
          : 'Click an asset, or a light\'s or mesh\'s icon, to select it   ·   right-click: its menu   ·   hold right-click: look around';
        if (!propsGrabbing && free && g && selected) {
          axis = envAxisHit(g, u, v, aspect);
          ring = axis < 0 ? propsRingHit(g, u, v, aspect) : -1;
        }
        if (free && pressed) {
          if (axis >= 0 || ring >= 0) {
            propsCommand('grab ' + p.selected + ' ' + (ring >= 0 ? PROPS_RINGS[ring] : ['x', 'y', 'z'][axis]) + ' ' + u.toFixed(4) + ' ' + v.toFixed(4));
            propsGrabbing = true;
          } else {
            // A click on an asset picks it AND grabs it: dragging slides it along whatever surface
            // is under the cursor (close at the bottom of the view, far toward the horizon). The
            // arrows and rings stay for exact moves. A light, a mesh or empty ground just picks.
            propsCommand('pickgrab ' + u.toFixed(4) + ' ' + v.toFixed(4));
            propsGrabbing = true;
          }
        } else if (propsGrabbing && down && (moved || snap !== propsLastSnap)) {
          propsCommand('drag ' + u.toFixed(4) + ' ' + v.toFixed(4) + snapArg);
        } else if (propsGrabbing && !down) {
          propsCommand('drag ' + u.toFixed(4) + ' ' + v.toFixed(4) + snapArg);
          propsCommand('release'); propsGrabbing = false;
        }
      }
      if (moved) { propsLastU = u; propsLastV = v; }
      propsLastSnap = snap;
      propsPointerDown = down;
      // Esc (handled natively) ends a grab; the state then reports no grab.
      if (propsGrabbing && p.grab === '' && !down) propsGrabbing = false;

      var bounds = g && g.bounds, n = 0;
      for (var corner = 0; corner < 8 && bounds; corner++) {
        for (var bit = 1; bit <= 4; bit *= 2) {
          if (corner & bit) continue;
          var edge = propsEdges[n++];
          edge.style.backgroundColor = placing ? PROPS_PLACE : PROPS_BOX;
          envDrawLine(edge, bounds[corner], bounds[corner | bit], w, h, placing ? 2 : 1);
        }
      }
      for (; n < 12; n++) propsEdges[n].visible = false;
      var brush = painting && p.paint.ring;
      for (var bi = 0; bi < propsBrushLines.length; bi++) {
        var bl = propsBrushLines[bi];
        if (!brush || bi >= brush.length) { bl.visible = false; continue; }
        bl.style.backgroundColor = (snap || p.paint.erasing) ? '#ff6767dd' : '#7fe0d0dd';
        envDrawLine(bl, brush[bi], brush[(bi + 1) % brush.length], w, h, 2);
      }
      var dots = painting && !snap && p.paint.dots || [];
      for (var di = 0; di < propsBrushDots.length; di++) {
        var dot = propsBrushDots[di], at = dots[di];
        dot.visible = !!(at && at[2]);
        if (dot.visible) dot.style.position = (at[0] * w - 4.5).toFixed(1) + 'px ' + (at[1] * h - 4.5).toFixed(1) + 'px 0px';
      }
      var showHandles = !placing && !!g;
      var grabbedRing = PROPS_RINGS.indexOf(p.grab);
      for (var rn = 0; rn < 3; rn++) {
        var rp = (g && g.rings && g.rings[rn]) || [], lines = propsRingLines[rn];
        // While one ring is dragged the others step back, so the one being used is unmistakable.
        var hot = ring === rn || grabbedRing === rn, faded = grabbedRing >= 0 && grabbedRing !== rn;
        for (var ri = 0; ri < lines.length; ri++) {
          var line = lines[ri];
          if (!showHandles || faded || ri >= rp.length) { line.visible = false; continue; }
          envDrawLine(line, rp[ri], rp[(ri + 1) % rp.length], w, h, hot ? 4 : 2);
        }
      }
      for (var ai = 0; ai < 3; ai++) {
        var ax = propsAxes[ai], tip = g && g.axes[ai];
        if (!showHandles || !tip || grabbedRing >= 0) { ax.shaft.visible = false; ax.name.visible = false; continue; }
        envDrawLine(ax.shaft, g.origin, tip, w, h, axis === ai ? 5 : 3);
        gizmoTagAt(ax.name, g.origin, tip, w, h);
      }
    });
)CFJS"
