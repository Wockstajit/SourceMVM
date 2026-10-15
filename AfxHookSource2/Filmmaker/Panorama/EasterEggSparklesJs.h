#pragma once

// E-GIRL MODE fragment of the easter-egg overlay (concatenated into kEasterEggOverlayJs, shares
// its scope). Three parts:
//   * a pastel WASH on Source MVM's own panels (never CS2's -- see setWash). Only a wash this
//     script applied is ever cleared, and it is cleared to white (wash multiplies; white is no
//     tint), because style = '' throws in Panorama.
//   * a soft pink HAZE creeping in from the screen edges.
//   * a FLOWER that follows the pointer, a sparkle TRAIL it leaves behind as it moves, and an
//     explosion of sparkles and petals only when you click. Nothing shows while the mouse is
//     steering the camera (no pointer on screen). Built from panels, not glyphs -- the in-game
//     font's coverage of dingbats is not guaranteed.

R"EGJS(
    // The wash goes on Source MVM's own roots ONLY, never on CS2's panels: washing CS2's
    // HudDemoController (or the whole context) paints a flat band over the top scoreboard that
    // hides the player cards -- isolated in game by un-washing one panel at a time.
    // Roots the editor builds later are picked up by a re-sweep every second.
    var WASH = '#ffc6eeff', UNWASH = '#ffffffff';
    var WASH_ONLY = { 'CamTimelineRoot': 1, 'CamTimelineRangeRoot': 1, 'MarkerHudRoot': 1,
      'MovieHudRoot': 1, 'CamEditorRoot': 1, 'GraphExpRoot': 1, 'CamEditorConfirmRoot': 1, 'CamEditorSettingsRoot': 1,
      'CamEditorRenderRoot': 1, 'ConfigHudRoot': 1, 'CamEditorCustomizeRoot': 1, 'CamEditorCustomizeConfirmRoot': 1,
      'AimXhairRoot': 1, 'FilmmakerRoot': 1, 'MvmPlayRoot': 1 };
    var washOn = false, washed = [], washSweepAt = 0;
    function washSweep() {
      var n = ctx.GetChildCount ? ctx.GetChildCount() : 0;
      for (var i = 0; i < n; i++) {
        var c = ctx.GetChild(i);
        if (!c || !WASH_ONLY[c.id] || c.__eggWashed) continue;
        try { c.style.washColor = WASH; c.__eggWashed = true; washed.push(c); } catch (e) {}
      }
    }
    function setWash(on) {
      if (on && washOn && now() < washSweepAt) return;
      if (on) { washSweep(); washSweepAt = now() + 1.0; washOn = true; return; }
      if (!washOn) return;
      washOn = false;
      for (var i = 0; i < washed.length; i++) {
        var c = washed[i];
        try { if (c && (!c.IsValid || c.IsValid())) { c.style.washColor = UNWASH; c.__eggWashed = false; } } catch (e) {}
      }
      washed = [];
    }

    // A soft pink glow creeping in from the four edges. Four LINEAR strips, not one radial
    // gradient: in game the radial form rendered as a hard-edged band across the top that hid the
    // scoreboard (the browser drew it correctly, CS2 did not).
    var haze = layer('MvmEggHaze', '1');
    haze.visible = false;
    function hazeEdge(w, h, ha, va, x0, y0, x1, y1) {
      var p = $.CreatePanel('Panel', haze, '');
      p.hittest = false;
      p.style.width = w; p.style.height = h;
      p.style.horizontalAlign = ha; p.style.verticalAlign = va;
      try {
        // Eased fall-off (strong at the edge, a long soft tail) so no line marks where it ends.
        p.style.backgroundColor = 'gradient( linear, ' + x0 + ' ' + y0 + ', ' + x1 + ' ' + y1
          + ', from( #ff7fd048 ), color-stop( 0.35, #ff7fd01c ), color-stop( 0.7, #ff7fd008 ), to( #ff7fd000 ) )';
      } catch (eh) { p.style.backgroundColor = '#ff7fd010'; }
    }
    // Shallow: a thin, soft band at each edge (9% top/bottom, 7% sides); where two meet the corner
    // just deepens a little, like a lens vignette.
    hazeEdge('100%', '9%', 'left', 'top', '0%', '0%', '0%', '100%');
    hazeEdge('100%', '9%', 'left', 'bottom', '0%', '100%', '0%', '0%');
    hazeEdge('7%', '100%', 'left', 'top', '0%', '0%', '100%', '0%');
    hazeEdge('7%', '100%', 'right', 'top', '100%', '0%', '0%', '0%');
    // The beat (UwU's song, st.pulse 0..1) brightens these same edges -- no second set of strips,
    // whose inner edge showed as a hard rectangle on every kick.
    var hazeOp = -1;

    var sparkLayer = layer('MvmEggSparkles', '2');
    var SPARK_COLORS = [EGG.pink, EGG.lav, EGG.sky, EGG.cream, EGG.hot, EGG.mint];
    var PETAL_COLORS = [EGG.pink, EGG.hot, EGG.lav];
    var SPARK_MAX = 72;
    var sparks = [];
    // Every moving piece is anchored top-left and placed with style.position. A translate inside
    // `transform` is SCALED by the scale3d that follows it in Panorama, so a sparkle at half size
    // landed halfway to the top-left corner -- that was the "it all sits in the top left" bug.
    // `transform` now carries only rotation and scale, about the panel's own centre.
    function pin(p, w, h) {
      p.hittest = false;
      p.style.width = w + 'px'; p.style.height = h + 'px';
      p.style.horizontalAlign = 'left'; p.style.verticalAlign = 'top';
      p.style.transformOrigin = '50% 50%';
      p.style.overflow = 'noclip noclip';
    }
    function place(p, x, y, half) {
      p.style.position = (x - half).toFixed(1) + 'px ' + (y - half).toFixed(1) + 'px 0px';
    }
    function mkSpark() {
      var c = $.CreatePanel('Panel', sparkLayer, '');
      pin(c, 28, 28);
      c.visible = false;
      function bar(w, h) {
        var b = $.CreatePanel('Panel', c, '');
        b.hittest = false;
        b.style.width = w + 'px'; b.style.height = h + 'px';
        b.style.horizontalAlign = 'center'; b.style.verticalAlign = 'center';
        b.style.borderRadius = (Math.min(w, h) / 2) + 'px';
        return b;
      }
      // A four-point star, or (petal) a single soft round petal.
      var s = { p: c, h: bar(26, 3), v: bar(3, 26), d: bar(7, 7), alive: false, petal: null,
        x: 0, y: 0, vx: 0, vy: 0, grav: 0, age: 0, life: 1, rot: 0, vr: 0, size: 1, color: '', kind: '' };
      return s;
    }
    for (var si = 0; si < SPARK_MAX; si++) sparks.push(mkSpark());
    var sparkNext = 0;
    // kind: 'star' (sparkle) or 'petal'. grav pulls petals down as they drift.
    function spawn(x, y, vx, vy, life, size, kind) {
      var s = sparks[sparkNext];
      sparkNext = (sparkNext + 1) % SPARK_MAX;
      kind = kind || 'star';
      s.alive = true; s.x = x; s.y = y; s.vx = vx; s.vy = vy; s.age = 0; s.life = life;
      s.rot = Math.random() * 90; s.vr = (Math.random() - 0.5) * 240; s.size = size;
      s.grav = kind === 'petal' ? 160 : 0;
      var col = pick(kind === 'petal' ? PETAL_COLORS : SPARK_COLORS);
      if (col !== s.color || kind !== s.kind) {
        s.color = col; s.kind = kind;
        var star = kind === 'star';
        s.h.visible = star; s.v.visible = star;
        s.h.style.backgroundColor = col; s.v.style.backgroundColor = col;
        s.h.style.boxShadow = col + 'cc 0px 0px 6px 1px';
        s.v.style.boxShadow = col + 'cc 0px 0px 6px 1px';
        s.d.style.width = star ? '7px' : '14px'; s.d.style.height = star ? '7px' : '9px';
        s.d.style.borderRadius = star ? '4px' : '7px';
        s.d.style.backgroundColor = star ? EGG.cream : col;
        s.d.style.boxShadow = col + (star ? 'ff 0px 0px 8px 2px' : '99 0px 0px 4px 1px');
      }
      s.p.visible = true;
    }

    // ---- the flower that follows the pointer ----
    var flower = $.CreatePanel('Panel', sparkLayer, 'MvmEggFlower');
    pin(flower, 44, 44);
    flower.visible = false;
    var FLOWER_HALF = 22;
    for (var fp = 0; fp < 5; fp++) {
      var a = (fp / 5) * Math.PI * 2 - Math.PI / 2;
      var petal = $.CreatePanel('Panel', flower, '');
      petal.hittest = false;
      petal.style.width = '16px'; petal.style.height = '16px'; petal.style.borderRadius = '8px';
      petal.style.horizontalAlign = 'left'; petal.style.verticalAlign = 'top';
      petal.style.position = (22 - 8 + Math.cos(a) * 10).toFixed(1) + 'px ' + (22 - 8 + Math.sin(a) * 10).toFixed(1) + 'px 0px';
      petal.style.backgroundColor = fp % 2 ? EGG.pink : EGG.hot;
      petal.style.boxShadow = EGG.pink + 'aa 0px 0px 6px 1px';
    }
    var heart = $.CreatePanel('Panel', flower, '');
    heart.hittest = false;
    heart.style.width = '12px'; heart.style.height = '12px'; heart.style.borderRadius = '6px';
    heart.style.horizontalAlign = 'center'; heart.style.verticalAlign = 'center';
    heart.style.backgroundColor = '#ffe680';
    heart.style.boxShadow = '#fff4b0ff 0px 0px 6px 2px';
    // The flower rides just below-right of the pointer tip (never on top of what you click),
    // springing after it a little rather than glued to it.
    var FLOWER_OFF = 18;
    var fx = -1, fy = -1, fRot = 0, fPop = 0;

    var lastCx = -1, lastCy = -1, lastClick = -1, anySpark = false;
    function egirlFrame(t, dt, sz) {
      var on = !!st.egirl;
      setWash(on);
      haze.visible = on;
      // Pulse with the song: the edges brighten on each kick and settle back between.
      var beat = on ? Math.max(0, Math.min(1, st.pulse || 0)) : 0;
      var op = Math.round((0.55 + 0.45 * beat) * 50) / 50;
      if (op !== hazeOp) { hazeOp = op; haze.style.opacity = op.toFixed(2); }
      var showFlower = on && !!st.cvis;
      flower.visible = showFlower;
      if (on && st.cvis) {
        var px = st.cx * sz.w, py = st.cy * sz.h;
        var tx = px + FLOWER_OFF, ty = py + FLOWER_OFF;
        if (fx < 0) { fx = tx; fy = ty; }
        var k = 1 - Math.pow(0.0005, dt);   // ~fast spring, frame-rate independent
        fx += (tx - fx) * k; fy += (ty - fy) * k;
        // Trail: a sparkle left behind every ~12 px of travel. It stays where it was dropped and
        // twinkles out -- a trail, not a burst.
        var dx = fx - lastCx, dy = fy - lastCy, d = Math.sqrt(dx * dx + dy * dy);
        if (lastCx < 0 || d > 400) { lastCx = fx; lastCy = fy; }
        else if (d > 12) {
          var n = Math.min(3, Math.floor(d / 12));
          for (var j = 0; j < n; j++) {
            var f = (j + 1) / n;
            spawn(lastCx + dx * f + (Math.random() - 0.5) * 6, lastCy + dy * f + (Math.random() - 0.5) * 6,
              (Math.random() - 0.5) * 12, 6 + Math.random() * 10, 0.45 + Math.random() * 0.3, 0.3 + Math.random() * 0.3);
          }
          lastCx = fx; lastCy = fy;
        }
        // Click: the explosion -- sparkles and petals out of the flower in a ring.
        if (lastClick >= 0 && st.click !== lastClick) {
          for (var b = 0; b < 18; b++) {
            var ang = (b / 18) * Math.PI * 2 + Math.random() * 0.3, sp = 160 + Math.random() * 160;
            spawn(fx, fy, Math.cos(ang) * sp, Math.sin(ang) * sp, 0.6 + Math.random() * 0.4,
              0.6 + Math.random() * 0.5, b % 3 === 0 ? 'petal' : 'star');
          }
          fPop = 1;
        }
        fRot += dt * 40;
        fPop = Math.max(0, fPop - dt * 4);
        var fs = 1 + 0.35 * fPop + 0.04 * Math.sin(t * 3);
        place(flower, fx, fy, FLOWER_HALF);
        flower.style.transform = 'rotateZ(' + (fRot % 360).toFixed(1) + 'deg) scale3d(' + fs.toFixed(3) + ', ' + fs.toFixed(3) + ', 1)';
      } else {
        lastCx = -1; fx = -1;
      }
      lastClick = st.click;

      anySpark = false;
      for (var i = 0; i < SPARK_MAX; i++) {
        var s = sparks[i];
        if (!s.alive) continue;
        s.age += dt;
        if (s.age >= s.life) { s.alive = false; s.p.visible = false; continue; }
        anySpark = true;
        s.vx *= Math.pow(0.12, dt); s.vy = s.vy * Math.pow(0.2, dt) + (26 + s.grav) * dt;
        s.x += s.vx * dt; s.y += s.vy * dt; s.rot += s.vr * dt;
        var u = s.age / s.life;
        // Pop in, twinkle, shrink out.
        var scale = s.size * (u < 0.15 ? expoOut(u / 0.15) : 1 - Math.pow((u - 0.15) / 0.85, 2));
        scale *= 0.85 + 0.15 * Math.sin(s.age * 30);
        place(s.p, s.x, s.y, 14);
        s.p.style.transform = 'rotateZ(' + s.rot.toFixed(1) + 'deg) scale3d(' + scale.toFixed(3) + ', ' + scale.toFixed(3) + ', 1)';
        s.p.style.opacity = (1 - u * u).toFixed(3);
      }
    }
)EGJS"
