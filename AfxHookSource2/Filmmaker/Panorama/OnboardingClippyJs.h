#pragma once

// CLIPPY fragment of the onboarding panel (concatenated into kOnboardingJs, shares its scope):
// the clapperboard guide. Every part is a plain box -- slate, two striped bars, a hinge, eyes,
// brows, feet -- because there is no image asset to ship.
//
// Panorama does not anti-alias the edges of a rotated box, so nothing with a long straight edge
// is drawn rotated: the stripes are one diagonal gradient per bar, and the hearts are round lobes
// plus half-filled squares. Gradient colour stops and border-radius are drawn smoothly.
//
// Coordinates are in rig units (a 124 x 190 design, the same numbers as the browser preview) and
// pass through u() to size him for a 1080-tall screen.
//
// clippyBuild() makes the panels; clippyFrame() poses him every frame: idle bob, blink, pupils
// on the cursor, brows per expression, the clapper top while he talks, a hop, confetti.

R"OBJS(
    var RIG = 1.08;                                // rig units -> Panorama px
    function u(v) { return (v * RIG).toFixed(1) + 'px'; }
    function box(parent, x, y, w, h, bg) {
      var p = P(parent, '');
      p.style.marginLeft = u(x); p.style.marginTop = u(y);
      p.style.width = u(w); p.style.height = u(h);
      if (bg) p.style.backgroundColor = bg;
      return p;
    }
    // A soft-edged half-plane on a square panel: transparent before the edge, `color` after it.
    // The edge is perpendicular to (dx, dy) and sits `at` rig units from the top-left corner along
    // it. The two gradient points are half a pixel apart, so the edge is a sub-pixel ramp.
    //
    // No colour stops on purpose. A first draft drew all the stripes as one gradient with ~28
    // color-stop()s, and CS2 crashed in panorama.dll the first frame it was drawn. Nothing else in
    // the repo uses more than one, so every gradient here is a plain from() -> to().
    function halfPlane(parent, x, y, side, dx, dy, at, color, clear) {
      var q = box(parent, x, y, side, side, null);
      var n = Math.sqrt(dx * dx + dy * dy), ux = dx / n, uy = dy / n, half = 0.25 / RIG;
      function pt(d) { return (d * ux / side * 100).toFixed(3) + '% ' + (d * uy / side * 100).toFixed(3) + '%'; }
      q.style.backgroundColor = 'gradient(linear, ' + pt(at - half) + ', ' + pt(at + half)
        + ', from(' + clear + '), to(' + color + '))';
      return q;
    }

    // Stripes leaning 35 degrees, 27 units apart along the bar, 13 of them dark: for each stripe
    // an ink half-plane from its leading edge, then a cream one from its trailing edge. The squares
    // are bigger than the bar, which clips them; none of them is rotated.
    var STRIPE_SIDE = 124, LIGHT = '#f4f1e5', INK = '#1d2229';
    function stripedBar(parent, y, h, radius) {
      var b = box(parent, 2, y, 120, h, LIGHT);
      b.style.border = '1px solid #9da4ae';
      b.style.borderRadius = radius;
      b.style.overflow = 'clip';
      var top = -(STRIPE_SIDE - h) / 2, dx = 1, dy = 0.7;
      var period = 27 * Math.cos(35 * Math.PI / 180), dark = period * 13 / 27;
      var len = STRIPE_SIDE * Math.sqrt(dx * dx + dy * dy);
      for (var at = 4; at < len; at += period) {
        halfPlane(b, -2, top, STRIPE_SIDE, dx, dy, at, INK, INK + '00');
        if (at + dark < len) halfPlane(b, -2, top, STRIPE_SIDE, dx, dy, at + dark, LIGHT, LIGHT + '00');
      }
      return b;
    }

    // Heart eyes: two round lobes over a diamond built from four squares, each half filled along
    // its diagonal. `fill` names the filled half: 'br' bottom-right, 'bl', 'tr', 'tl'.
    var HEART = '#ff4d6d', HEART_CLEAR = '#ff4d6d00';
    function halfSquare(parent, x, y, side, fill) {
      var diag = side * Math.SQRT2 / 2; // distance from the start corner to the other diagonal
      if (fill === 'br') return halfPlane(parent, x, y, side, 1, 1, diag, HEART, HEART_CLEAR);
      if (fill === 'tl') {
        var q = halfPlane(parent, x, y, side, 1, 1, diag, HEART_CLEAR, HEART);
        return q;
      }
      // 'bl' / 'tr' run from the top-right corner, so the gradient is mirrored: flip the square.
      var m = halfPlane(parent, x, y, side, 1, 1, diag, fill === 'bl' ? HEART : HEART_CLEAR,
        fill === 'bl' ? HEART_CLEAR : HEART);
      m.style.transform = 'scale3d(-1, 1, 1)';
      return m;
    }
    // Classic heart in a 28 x 26 box: diamond half-diagonal H, lobes of radius H / sqrt(2) centred
    // on the diamond's upper sides.
    function heartShape(parent, x, y) {
      var H = 11.6, R = H / Math.SQRT2, cx = 14, cy = H / 2 + R;
      var hb = box(parent, x, y, 28, 26, null);
      hb.style.overflow = 'noclip';
      hb.style.transformOrigin = '50% 60%';
      halfSquare(hb, cx - H, cy - H, H, 'br');
      halfSquare(hb, cx, cy - H, H, 'bl');
      halfSquare(hb, cx - H, cy, H, 'tr');
      halfSquare(hb, cx, cy, H, 'tl');
      var lobeL = box(hb, cx - H / 2 - R, cy - H / 2 - R, 2 * R, 2 * R, HEART);
      var lobeR = box(hb, cx + H / 2 - R, cy - H / 2 - R, 2 * R, 2 * R, HEART);
      lobeL.style.borderRadius = u(R); lobeR.style.borderRadius = u(R);
      var glint = box(hb, 4, 4, 5, 4, '#ffd1da');
      glint.style.borderRadius = u(2.5);
      hb.visible = false;
      return hb;
    }

    function clippyBuild(parent) {
      var c = P(parent, 'MvmObClippy');
      c.hittest = true;
      c.style.width = u(128); c.style.height = u(194);
      c.style.overflow = 'noclip';
      var rig = box(c, 2, 4, 124, 190, null);
      rig.style.overflow = 'noclip';
      rig.style.transformOrigin = '50% 90%';

      function foot(x, deg) {
        var f = box(rig, x, 172, 20, 10, '#171c24');
        f.style.border = '1px solid #6b7480';
        f.style.borderRadius = u(2) + ' ' + u(2) + ' ' + u(6) + ' ' + u(6);
        f.style.transform = 'rotateZ(' + deg + 'deg)';
      }
      foot(24, 4); foot(80, -4);

      var slate = box(rig, 2, 80, 120, 96, '#252c36');
      slate.style.border = '2px solid #69717d';
      slate.style.borderRadius = '0px 0px ' + u(10) + ' ' + u(10);
      slate.style.boxShadow = '#00000066 3px 5px 8px 0px';
      box(rig, 10, 150, 104, 1, '#eceee755');
      var plate = box(rig, 2, 153, 120, 20, null);
      var name = P(plate, '');
      name.style.flowChildren = 'right';
      name.style.horizontalAlign = 'center'; name.style.verticalAlign = 'center';
      var n1 = L(name, 'SOURCE', '#f5f3e7', 11 * RIG), n2 = L(name, ':MVM', '#f0b323', 11 * RIG);
      n1.style.fontWeight = 'bold'; n2.style.fontWeight = 'bold';
      // Two labels so ":MVM" can be gold; the first label's box ends a little past its last
      // letter, which read as "SOURCE :MVM" in game.
      n2.style.marginLeft = u(-3);

      stripedBar(rig, 64, 18, u(2));
      var stick = stripedBar(rig, 44, 20, u(4) + ' ' + u(4) + ' ' + u(2) + ' ' + u(2));
      stick.style.transformOrigin = '7.5% 100%';
      stick.style.boxShadow = '#00000055 2px 3px 5px 0px';
      var hinge = box(rig, 5, 58, 12, 12, '#353c47');
      hinge.style.border = '2px solid #bec6cf';
      hinge.style.borderRadius = u(6);

      function eye(x) {
        var e = box(rig, x, 99, 30, 32, '#fffef5');
        e.style.borderRadius = u(15);
        e.style.overflow = 'clip';
        e.style.transformOrigin = '50% 50%';
        var p = box(e, 9, 9, 12, 13, '#17191d');
        p.style.borderRadius = u(6);
        var glint = box(p, 2, 2, 4, 4, '#ffffff');
        glint.style.borderRadius = u(2);
        return { eye: e, pupil: p };
      }
      var eyes = [eye(28), eye(68)];
      function brow(x) {
        var b = box(rig, x, 90, 22, 3, '#dce1e6');
        b.style.borderRadius = u(2);
        return b;
      }
      var brows = [brow(32), brow(72)];
      // Heart eyes for when he falls for your shot, in place of each eye. Hidden until his
      // expression is 'love' or he is falling.
      var hearts = [heartShape(rig, 29, 102), heartShape(rig, 69, 102)];
      var confetti = [];
      for (var k = 0; k < 4; k++) {
        var cf = box(c, 60, 40, 5, 10, '#f0b323');
        cf.style.opacity = '0';
        confetti.push(cf);
      }
      return { c: c, rig: rig, stick: stick, eyes: eyes, hearts: hearts, brows: brows, confetti: confetti,
               expr: 'curious', hopAt: -1e9, fallAt: -1e9, confettiAt: -1e9, lastStick: '', inLove: false };
    }

    // [left brow, right brow] as [lift px, tilt deg]: the inner ends sit a little high, so even
    // at rest he looks friendly rather than cross.
    var BROWS = { curious: [[-5, -12], [-2, 3]], happy: [[-5, -8], [-5, 8]], focused: [[-2, -4], [-2, 4]],
      love: [[-7, -14], [-7, 14]] };
    var CONFETTI = [[-60, -60], [45, -80], [-30, -95], [70, -40]];

    var FALL_MS = 2600;
    function clippyHop(k) { k.hopAt = Date.now(); }
    // Tips over onto his side, bounces once, lies there in love for a second, gets back up.
    function clippyFall(k, confetti) { k.fallAt = Date.now(); if (confetti) k.confettiAt = k.fallAt; }
    function clippyFallAngle(f) {
      if (f < 0.15) { var p = f / 0.15; return 84 * p * p; }
      if (f < 0.27) return 84 - 7 * Math.sin((f - 0.15) / 0.12 * Math.PI);
      if (f < 0.73) return 84;
      var q = (f - 0.73) / 0.27;
      return 84 * (1 - (1 - (1 - q) * (1 - q)));
    }

    // stickDeg: the clapper top's angle (-8 at rest, more negative is more open).
    // look: {x, y} cursor offset from his eyes in px, or null to glance idly.
    function clippyFrame(k, t, stickDeg, talking, look) {
      var bob = Math.sin(t / 1000 * Math.PI * 2 / 4.8);
      var rot = -0.5 + bob * 2.5, y = -2.5 - bob * 2.5;
      var h = (Date.now() - k.hopAt) / 750;
      if (h >= 0 && h < 1) {
        y += h < 0.25 ? 3 * (h / 0.25) : (h < 0.55 ? 3 - 21 * ((h - 0.25) / 0.3) : -18 + 18 * ((h - 0.55) / 0.45));
        rot += -5 * Math.sin(h * Math.PI * 2);
      }
      var f = (Date.now() - k.fallAt) / FALL_MS, falling = f >= 0 && f < 1;
      if (falling) rot += clippyFallAngle(f);
      k.rig.style.transform = 'translate3d(0px, ' + u(y) + ', 0px) rotateZ(' + rot.toFixed(2) + 'deg)';

      var s = 'rotateZ(' + stickDeg.toFixed(1) + 'deg)';
      if (s !== k.lastStick) { k.stick.style.transform = s; k.lastStick = s; }

      var blink = (t / 1000 % 6.7) / 6.7, sy = (blink > 0.43 && blink < 0.46) ? 0.1 : 1;
      var dx, dy;
      if (look) { dx = clamp(look.x / 70, -5, 5); dy = clamp(look.y / 90, -3, 3); }
      else { dx = Math.sin(t / 1000 * 0.7) * 3 - 1; dy = Math.cos(t / 1000 * 1.1) * 1.5; }
      var love = falling || k.expr === 'love';
      if (love !== k.inLove) {
        k.inLove = love;
        for (var e = 0; e < 2; e++) { k.eyes[e].eye.visible = !love; k.hearts[e].visible = love; }
      }
      if (love) {
        var beat = 1 + 0.09 * Math.max(0, Math.sin(t / 1000 * Math.PI * 2 / 0.8));
        for (var hh = 0; hh < 2; hh++) k.hearts[hh].style.transform = 'scale3d(' + beat.toFixed(3) + ', ' + beat.toFixed(3) + ', 1)';
      }
      for (var i = 0; i < 2; i++) {
        k.eyes[i].eye.style.transform = 'scale3d(1, ' + sy + ', 1)';
        k.eyes[i].pupil.style.transform = 'translate3d(' + u(dx) + ', ' + u(dy) + ', 0px)';
        var b = (BROWS[love ? 'love' : k.expr] || BROWS.curious)[i];
        var chat = talking ? Math.sin(t / 1000 * Math.PI * 2 / 0.6) * 1.5 - 0.5 : 0;
        k.brows[i].style.transform = 'translate3d(0px, ' + u(b[0] + chat) + ', 0px) rotateZ(' + b[1] + 'deg)';
      }
      for (var j = 0; j < 4; j++) {
        var cf = k.confetti[j], q = (Date.now() - k.confettiAt) / 1100;
        if (q >= 0 && q < 1) {
          cf.style.opacity = (q < 0.1 ? q * 10 : 1 - q).toFixed(2);
          cf.style.transform = 'translate3d(' + u(CONFETTI[j][0] * q) + ', ' + u(CONFETTI[j][1] * q) + ', 0px) rotateZ(' + (210 * q).toFixed(0) + 'deg)';
        } else if (cf.style.opacity !== '0') cf.style.opacity = '0';
      }
    }
)OBJS"
