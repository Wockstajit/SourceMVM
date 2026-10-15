#pragma once

// osu! gameplay drawing (part of EasterEggOsuJs.h; shares the overlay scope).
// Slider bodies are concentric chains of OPAQUE discs, one chain per colour band (white rim, then
// darker-to-lighter combo colour toward the middle): overlapping opaque discs of one colour merge
// into a smooth tube, so no individual circles show, and the bands read as a gradient across it.
// Discs are spaced by distance along the path so each band's edge ripple stays under a pixel.

R"EGJS(
    var osuFpLayer = osuFull(osuPlay); // follow points sit under everything, as in osu!
    var osuSliderLayer = osuFull(osuPlay), osuCircleLayer = osuFull(osuPlay), osuApproachLayer = osuFull(osuPlay);
    var osuBallLayer = osuFull(osuPlay), osuFxLayer = osuFull(osuPlay), osuFlLayer = osuFull(osuPlay);
    var osuHud = osuFull(osuPlay);
    // [diameter fraction, darken toward black]; index 0 is the white rim.
    var OSU_BODY = [[1.0, -1], [0.87, 0.62], [0.7, 0.45], [0.5, 0.28], [0.3, 0.12]];
    // A skin's legacy body (lazer LegacySliderBody / stable MmSliderRendererGL): SliderBorder from the
    // shadow edge (1 - 59/64 of the radius) to 0.1875 in, then the track colour (SliderTrackOverride,
    // else the combo colour) from darkened 10% at the border to lightened toward the middle, at 0.7
    // alpha. [diameter fraction, 0..1 from outer to inner colour]; index 0 is the border.
    var OSU_BODY_SKIN = [[0.922, -1], [0.8125, 0.0], [0.6, 0.35], [0.4, 0.65], [0.2, 0.95]];
    function osuLegacyLighten(h, amount) {
      var c = osuRgb(h); amount *= 0.5;
      return '#' + osuHex2(Math.min(255, c[0] * (1 + 0.5 * amount) + 255 * amount)) + osuHex2(Math.min(255, c[1] * (1 + 0.5 * amount) + 255 * amount))
        + osuHex2(Math.min(255, c[2] * (1 + 0.5 * amount) + 255 * amount));
    }
    function osuLegacyDarken(h, amount) { var c = osuRgb(h); return '#' + osuHex2(c[0] / (1 + amount)) + osuHex2(c[1] / (1 + amount)) + osuHex2(c[2] / (1 + amount)); }

    // A reverse arrow: two flat bars meeting at a tip (no nested transforms -- a rotated box with
    // positioned rotated children drew as an X in game). Each bar is placed in screen space and
    // turned on its own centre.
    function osuArrow(parent) {
      var r = { b1: osuPanel(parent, '#ffffff'), b2: osuPanel(parent, '#ffffff') };
      r.b1.style.transformOrigin = '50% 50%'; r.b2.style.transformOrigin = '50% 50%';
      return r;
    }
    function osuArrowBar(bar, tx, ty, ang, L, th) {
      var cx = tx + Math.cos(ang) * L / 2, cy = ty + Math.sin(ang) * L / 2;
      osuBox(bar, cx - L / 2, cy - th / 2, L, th);
      oS(bar, 'borderRadius', oPx(th / 2));
      // rotateZ is clockwise on screen, as in CSS (measured: a negated angle mirrored the chevron).
      oS(bar, 'transform', 'rotateZ(' + (ang * 57.29578).toFixed(1) + 'deg)');
      oV(bar, true);
    }
    // Chevron centred near (x, y) pointing along `ang` (radians, screen space).
    function osuArrowDraw(r, x, y, ang, A) {
      var L = A * 0.55, th = Math.max(3, A * 0.14);
      var tx = x + Math.cos(ang) * A * 0.2, ty = y + Math.sin(ang) * A * 0.2;
      osuArrowBar(r.b1, tx, ty, ang + Math.PI * 0.75, L, th);
      osuArrowBar(r.b2, tx, ty, ang - Math.PI * 0.75, L, th);
    }
    function osuArrowHide(r) { oV(r.b1, false); oV(r.b2, false); }

    var osuSliders = [];
    function osuSliderRec(i) {
      while (osuSliders.length <= i) {
        var g = { root: osuFull(osuSliderLayer), lay: [], discs: [], key: '', D: 0 };
        // The rim stays sharp; the colour bands sit in one group blurred just enough to melt the
        // steps between them into a smooth gradient.
        g.lay.push(osuFull(g.root)); g.discs.push([]);
        g.inner = osuFull(g.root);
        g.inner.style.opacity = '0.375'; // the colour bands, mostly see-through; the rim stays solid
        for (var k = 1; k < OSU_BODY.length; k++) { g.lay.push(osuFull(g.inner)); g.discs.push([]); }
        g.tickLayer = osuFull(g.root); g.ticks = [];
        g.arrEnd = osuArrow(g.root); g.arrHead = osuArrow(g.root);
        g.arrImgE = osuImg(g.root); g.arrImgH = osuImg(g.root); g.tickImgs = [];
        osuSliders.push(g);
      }
      return osuSliders[i];
    }
    function osuBuildSlider(g, o, D, col, sk) {
      var bands = sk ? OSU_BODY_SKIN : OSU_BODY;
      var outer = '', inner = '';
      if (sk) {
        var track = sk.track || col;
        outer = osuLegacyDarken(track, 0.1); inner = osuLegacyLighten(track, 0.5);
      }
      var pts = o.p, n = pts.length / 2;
      var xs = [], ys = [], cum = [0];
      for (var k = 0; k < n; k++) {
        xs.push(osuX(pts[2 * k] / 10)); ys.push(osuY(pts[2 * k + 1] / 10));
        if (k) { var dx = xs[k] - xs[k - 1], dy = ys[k] - ys[k - 1]; cum.push(cum[k - 1] + Math.sqrt(dx * dx + dy * dy)); }
      }
      if (n === 0) { xs.push(osuX(o.x)); ys.push(osuY(o.y)); n = 1; }
      var total = cum[n - 1] || 0;
      for (var L = 0; L < bands.length; L++) {
        var d = D * bands[L][0];
        // Edge ripple of a disc chain ~ spacing^2 / (8 r): keep it under 0.8 px.
        var sp = Math.max(2, Math.sqrt(8 * (d / 2) * 0.8));
        var cnt = Math.min(220, Math.max(1, Math.ceil(total / sp)) + 1);
        var fill = bands[L][1] < 0 ? (sk ? sk.border : '#ffffff') : osuMix(col, '#000000', bands[L][1]);
        // The skin's track is 0.7 alpha over a black playfield: composited here so the bands can stay
        // opaque (a see-through band would show the border disc beneath it).
        if (sk && bands[L][1] >= 0) fill = osuMix(osuMix(outer, inner, bands[L][1]), '#000000', 0.3);
        var list = g.discs[L], seg = 0;
        for (var j = 0; j < cnt; j++) {
          var dist = cnt > 1 ? total * j / (cnt - 1) : 0, x = xs[0], y = ys[0];
          if (n > 1) {
            while (seg < n - 2 && cum[seg + 1] < dist) seg++;
            var sl = cum[seg + 1] - cum[seg], u = sl > 1e-6 ? osuClamp((dist - cum[seg]) / sl) : 0;
            x = xs[seg] + (xs[seg + 1] - xs[seg]) * u; y = ys[seg] + (ys[seg + 1] - ys[seg]) * u;
          }
          if (list.length <= j) list.push(osuDisc(g.lay[L], 10, '#ffffff', 0, ''));
          oS(list[j], 'backgroundColor', fill);
          osuDiscAt(list[j], x, y, d);
          oV(list[j], true);
        }
        for (var q = cnt; q < list.length; q++) oV(list[q], false);
      }
      // Reverse arrows point back INTO the body from each end.
      var back = function (fromEnd) {
        var want = Math.min(total, D * 0.3);
        if (fromEnd) { var e = n - 1, b = e; while (b > 0 && cum[e] - cum[b] < want) b--; return Math.atan2(ys[b] - ys[e], xs[b] - xs[e]); }
        var f = 0; while (f < n - 1 && cum[f] < want) f++; return Math.atan2(ys[f] - ys[0], xs[f] - xs[0]);
      };
      g.ex = xs[n - 1]; g.ey = ys[n - 1]; g.hx = xs[0]; g.hy = ys[0];
      g.endAng = back(true); g.headAng = back(false);
      var bl = Math.max(0.6, D * 0.005);
      oS(g.inner, 'blur', 'gaussian(' + bl.toFixed(1) + ', ' + bl.toFixed(1) + ', 1)');
      oS(g.inner, 'opacity', sk ? '1' : '0.375');
      g.key = o.i + ':' + o.t + (sk ? ':s' : ''); g.D = D;
    }

    // ---- follow points (osu.Game.Rulesets.Osu FollowPointConnection) ----
    // Between consecutive objects of one combo (never into a new combo or across a spinner): a mark
    // every 32 osu px from 48 px out to 32 px short of the next object. Each fades in 800 ms (less at
    // AR above 9) before the time the cursor should pass it, sliding in from 10% back and shrinking
    // from 1.5x over 400 ms, and fades out over 400 ms from that time.
    var osuFps = [], osuFpImgs = [];
    function osuFpImg(i) { while (osuFpImgs.length <= i) osuFpImgs.push(osuImg(osuFpLayer)); return osuFpImgs[i]; }
    function osuFpRec(i) {
      while (osuFps.length <= i) {
        var p = osuPanel(osuFpLayer, '#ffffff');
        p.style.transformOrigin = '50% 50%';
        p.visible = false;
        osuFps.push(p);
      }
      return osuFps[i];
    }
    function osuEndPos(o) {
      if ((o.k & 2) && o.p && o.p.length >= 4) {
        var n = o.p.length / 2, k = o.s % 2 === 1 ? n - 1 : 0;
        return [o.p[2 * k] / 10, o.p[2 * k + 1] / 10];
      }
      return [o.x, o.y];
    }
    function osuDrawFollowPoints(t) {
      var objs = osuO.o || [], pre = osuF.pre || 600, used = 0, usedImg = 0;
      // lazer FollowPointConnection: each point starts at 1.5x the OBJECT scale (radius / 64, i.e. circle
      // size) and eases to 1x -- not the bare playfield scale, which drew CS4 points ~1.75x too big.
      var objScale = (osuF.r || 36) / 64;
      // Skin follow points: followpoint-0..N animated over one second (legacy default framerate), else
      // followpoint; a 1x1 frame draws nothing.
      var sk = osuSkin(), frames = null;
      if (sk) {
        if (sk.fp && sk.fp.length) { frames = []; for (var fi0 = 0; fi0 < sk.fp.length; fi0++) frames.push(sk.fp[fi0].i || null); }
        else if (osuSkinEl('followpoint')) frames = [osuSkinEl('followpoint')];
      }
      var lead = 800 * Math.min(1, pre / 450);
      for (var i = 0; i + 1 < objs.length; i++) {
        var a = objs[i], b = objs[i + 1];
        if (b.i !== a.i + 1 || b.n === 1 || (a.k & 8) || (b.k & 8)) continue;
        var sp = osuEndPos(a), st = (a.k & 2) ? a.e : a.t;
        var dx = b.x - sp[0], dy = b.y - sp[1], dist = Math.floor(Math.sqrt(dx * dx + dy * dy));
        var ang = Math.atan2(dy, dx), dur = b.t - st;
        for (var d = 48; d < dist - 32; d += 32) {
          var fr = d / dist, fo = st + fr * dur, fi = fo - lead;
          if (t < fi || t > fo + 400) continue;
          var u = osuClamp((t - fi) / 400), e = 1 - (1 - u) * (1 - u);
          var al = t <= fo ? u : osuClamp(1 - (t - fo) / 400);
          var f2 = fr - 0.1 * (1 - e);
          var x = osuX(sp[0] + f2 * dx), y = osuY(sp[1] + f2 * dy);
          var sc = (1.5 - 0.5 * e) * objScale, len = 14 * osuPF.s * sc, th = Math.max(2, 2.6 * osuPF.s * sc);
          if (frames) {
            var fe = frames[Math.floor(Math.max(0, t - fi) / 1000 * frames.length) % frames.length];
            if (!osuElVis(fe)) continue;
            var im = osuFpImg(usedImg++);
            osuImgAt(im, fe, x, y, osuPF.s * sc);
            oS(im, 'transform', 'rotateZ(' + (ang * 57.29578).toFixed(1) + 'deg)');
            oS(im, 'opacity', al.toFixed(3));
            continue;
          }
          var fp = osuFpRec(used++);
          osuBox(fp, x - len / 2, y - th / 2, len, th);
          oS(fp, 'borderRadius', oPx(th / 2));
          oS(fp, 'transform', 'rotateZ(' + (ang * 57.29578).toFixed(1) + 'deg)');
          oS(fp, 'opacity', (al * 0.7).toFixed(3));
          oV(fp, true);
        }
      }
      for (var h = used; h < osuFps.length; h++) oV(osuFps[h], false);
      for (var h4 = usedImg; h4 < osuFpImgs.length; h4++) oV(osuFpImgs[h4], false);
    }

    var osuCircles = [];
    function osuCircleRec(i) {
      while (osuCircles.length <= i) {
        var c = { box: osuPanel(osuCircleLayer, null), col: '' };
        c.ring = osuDisc(c.box, 10, '#ffffff', 0, '');
        c.core = osuDisc(c.box, 10, '#000000', 0, '');
        c.ring.style.horizontalAlign = 'center'; c.ring.style.verticalAlign = 'center';
        c.core.style.horizontalAlign = 'center'; c.core.style.verticalAlign = 'center';
        c.core.visible = false;
        c.num = osuText(c.box, '1', '#ffffff', 30, 'bold');
        c.num.style.horizontalAlign = 'center'; c.num.style.verticalAlign = 'center';
        c.num.style.textAlign = 'center';
        c.num.style.textShadow = '0px 1px 3px 1.0 #000000cc';
        c.app = osuDisc(osuApproachLayer, 10, null, 3, '#ffffffff');
        // Skin sprites: circle (tinted), overlay, number glyphs, approach circle (tinted).
        c.skBase = osuImg(c.box); c.skBase.style.zIndex = '1';
        c.skOv = osuImg(c.box); c.skOv.style.zIndex = '3';
        c.skNum = [osuImg(c.box), osuImg(c.box), osuImg(c.box), osuImg(c.box)];
        c.skApp = osuImg(osuApproachLayer);
        osuCircles.push(c);
      }
      return osuCircles[i];
    }
    var osuBalls = [];
    function osuBallRec(i) {
      while (osuBalls.length <= i) {
        var b = { follow: osuDisc(osuBallLayer, 10, null, 3, '#ffffffd0'), ball: osuDisc(osuBallLayer, 10, '#ffffff', 3, '#ffffff') };
        b.skFollow = osuImg(osuBallLayer); b.skBall = osuImg(osuBallLayer);
        osuBalls.push(b);
      }
      return osuBalls[i];
    }
    var osuPops = [];
    for (var opi = 0; opi < 24; opi++) {
      var pp0 = osuCentered(osuFxLayer, '#ffffff', 30);
      pp0.l.style.textShadow = '0px 1px 3px 1.0 #000000cc';
      pp0.p.visible = false;
      osuPops.push(pp0);
    }
    var OSU_POP = { 300: ['', '#66ccff'], 100: ['100', '#7dff8c'], 50: ['50', '#ffd36b'], 0: ['miss', '#ff4d6d'] };
    var osuPopImgs = [];
    for (var opj = 0; opj < 24; opj++) osuPopImgs.push(osuImg(osuFxLayer));

    // Spinner
    var osuSpin = osuPanel(osuFxLayer, null);
    var osuSpinBack = osuDisc(osuSpin, 10, '#0a061299', 0, '');
    var osuSpinFill = osuDisc(osuSpin, 10, '#ff8fd855', 0, '');
    var osuSpinRing = osuDisc(osuSpin, 10, null, 6, '#ffffffd0');
    var osuSpinApp = osuDisc(osuSpin, 10, null, 4, '#9fe6ffd0');
    var osuSpinArm = osuPanel(osuSpin, null);
    osuSpinArm.style.transformOrigin = '50% 50%';
    var osuSpinDot = osuDisc(osuSpinArm, 10, '#ffffff', 0, '');
    var osuSpinTxt = osuCentered(osuSpin, '#ffffff', 34);
    osuSpin.visible = false;
)EGJS"
// Split: MSVC caps one raw string literal at ~16 KB (C2026); adjacent literals concatenate.
R"EGJS(
    // HUD
    var osuHpBack = osuPanel(osuHud, '#00000099');
    osuHpBack.style.border = '1px solid #ffffff30';
    var osuHpFill = osuPanel(osuHpBack, null);
    var osuScoreT = osuAligned(osuHud, '#ffffff', 50, 'right', 'top');
    osuScoreT.l.style.letterSpacing = '2px';
    osuScoreT.l.style.textShadow = '0px 1px 3px 1.0 #000000aa';
    var osuAccT = osuAligned(osuHud, '#ffffffd9', 24, 'right', 'top');
    var osuProgBack = osuPanel(osuHud, '#ffffff26');
    var osuProgFill = osuPanel(osuProgBack, '#ffffffd9');
    var osuHudMods = osuPillPool(osuHud, 8, 14);
    var osuComboT = osuAligned(osuHud, '#ffffff', 64, 'left', 'bottom');
    osuComboT.l.style.textShadow = '0px 1px 4px 1.0 #000000aa';
    osuComboT.l.style.transformOrigin = '0% 100%';
    var osuSkip = osuCentered(osuHud, '#120a1c', 22, '#ff8fd8');
    var osuNotice = osuCentered(osuHud, '#ffffffcc', 18);
    var osuComboLast = 0, osuComboPop = 0;

    // Flashlight: a huge disc whose border is the darkness and whose hole follows the cursor.
    var osuFl = osuPanel(osuFlLayer, null), osuFlSoft = osuPanel(osuFlLayer, null);

    function osuHdAlpha(o, t, pre) {
      var t0 = o.t - pre, fi = pre * 0.4;
      if (t < t0 + fi) return osuClamp((t - t0) / fi);
      return osuClamp(1 - (t - t0 - fi) / (pre * 0.3));
    }

    function osuDrawPlay(t, dt, sz) {
      var pre = osuF.pre || 600, fade = osuF.fade || 400, r = osuF.r || 36, hd = !!osuF.hd;
      var D = 2 * r * osuPF.s;
      var objs = osuO.o || [], cols = osuO.cols && osuO.cols.length ? osuO.cols : ['#ff8fd8'];
      var ci = 0, si = 0, bi = 0;
      var sk = osuSkin(), os = osuPF.s * r / 64; // skin sprite scale: screen px per sprite px (radius / 64)
      osuDrawFollowPoints(t);
      for (var i = 0; i < objs.length; i++) {
        var o = objs[i];
        var isSlider = (o.k & 2) !== 0, isSpinner = (o.k & 8) !== 0;
        if (isSpinner) continue;
        var appear = o.t - pre;
        if (t < appear) continue;
        var alpha = hd ? osuHdAlpha(o, t, pre) : osuClamp((t - appear) / Math.max(1, fade));
        var col = cols[o.c % cols.length];
        var x = osuX(o.x), y = osuY(o.y);
        if (isSlider) {
          var g = osuSliderRec(si++);
          if (g.key !== o.i + ':' + o.t + (sk ? ':s' : '') || Math.abs(g.D - D) > 0.5) osuBuildSlider(g, o, D, col, sk);
          var gone = t > o.e ? osuClamp((t - o.e) / 200) : 0;
          var bodyA = osuClamp((t - appear) / Math.max(1, fade));
          if (hd) { var fo = appear + pre * 0.4; bodyA = t < fo ? osuClamp((t - appear) / (pre * 0.4)) : osuClamp(1 - (t - fo) / Math.max(1, o.e - fo)); }
          oV(g.root, gone < 1);
          oS(g.root, 'opacity', (bodyA * 0.92 * (1 - gone)).toFixed(3));
          // Reverse arrows for the turns still to come (turn j happens at the end when j is even).
          var pass = Math.max(0, Math.min(o.s - 1, Math.floor((t - o.t) / Math.max(1, o.sp))));
          var lastTurn = o.s - 2;
          var endTurn = pass % 2 === 0 ? pass : pass + 1, headTurn = pass % 2 === 1 ? pass : pass + 1;
          var A = D * 0.62 * (1 + 0.08 * Math.sin(t / 90));
          var showEnd = o.s > 1 && endTurn <= lastTurn, showHead = o.s > 1 && headTurn <= lastTurn && t >= o.t;
          var ra = sk ? osuSkinEl('reversearrow') : null;
          if (ra) {
            // The skin's arrow (drawn pointing right) turned to point back into the body.
            osuArrowHide(g.arrEnd); osuArrowHide(g.arrHead);
            var rp = 1 + 0.08 * Math.sin(t / 90);
            if (showEnd && osuElVis(ra)) { osuImgAt(g.arrImgE, ra, g.ex, g.ey, os * rp); oS(g.arrImgE, 'transform', 'rotateZ(' + (g.endAng * 57.29578).toFixed(1) + 'deg)'); } else oV(g.arrImgE, false);
            if (showHead && osuElVis(ra)) { osuImgAt(g.arrImgH, ra, g.hx, g.hy, os * rp); oS(g.arrImgH, 'transform', 'rotateZ(' + (g.headAng * 57.29578).toFixed(1) + 'deg)'); } else oV(g.arrImgH, false);
          } else {
            oV(g.arrImgE, false); oV(g.arrImgH, false);
            if (showEnd) osuArrowDraw(g.arrEnd, g.ex, g.ey, g.endAng, A); else osuArrowHide(g.arrEnd);
            if (showHead) osuArrowDraw(g.arrHead, g.hx, g.hy, g.headAng, A); else osuArrowHide(g.arrHead);
          }
          // Slider ticks of the pass in progress that the ball has not reached yet.
          var tks = o.tk || [], nt = 0, p0 = o.t + pass * o.sp, p1 = p0 + o.sp, td = Math.max(6, D * 0.14);
          var tkE = sk ? osuSkinEl('sliderscorepoint') : null;
          for (var q = 0; q < tks.length; q++) {
            var tt = tks[q];
            if (tt < p0 || tt > p1 || tt <= t) continue;
            if (g.ticks.length <= nt) g.ticks.push(osuDisc(g.tickLayer, 10, '#ffffff', 0, ''));
            if (g.tickImgs.length <= nt) g.tickImgs.push(osuImg(g.tickLayer));
            var tp = osuPathAt(o.p, osuProgress(o, tt));
            if (tkE) {
              oV(g.ticks[nt], false);
              if (osuElVis(tkE)) osuImgAt(g.tickImgs[nt], tkE, osuX(tp[0]), osuY(tp[1]), os); else oV(g.tickImgs[nt], false);
            } else {
              oV(g.tickImgs[nt], false);
              osuDiscAt(g.ticks[nt], osuX(tp[0]), osuY(tp[1]), td);
              oV(g.ticks[nt], true);
            }
            nt++;
          }
          for (var q2 = nt; q2 < g.ticks.length; q2++) { oV(g.ticks[q2], false); oV(g.tickImgs[q2], false); }
          if (t >= o.t && t <= o.e) {
            var b = osuBallRec(bi++);
            var pos = osuPathAt(o.p, osuProgress(o, t));
            var bx = osuX(pos[0]), by = osuY(pos[1]);
            var sbE = sk ? (osuSkinEl('sliderb') || osuSkinEl('sliderb0')) : null, sfE = sk ? osuSkinEl('sliderfollowcircle') : null;
            if (sbE || sfE) {
              // Skin ball (untinted unless AllowSliderBallTint) and follow circle (only while held).
              oV(b.ball, false); oV(b.follow, false);
              if (osuElVis(sbE)) { osuImgAt(b.skBall, sbE, bx, by, os); osuTint(b.skBall, sk.ballTint ? col : ''); } else oV(b.skBall, false);
              if (osuElVis(sfE) && osuF.held) osuImgAt(b.skFollow, sfE, bx, by, os); else oV(b.skFollow, false);
            } else {
              oV(b.skBall, false); oV(b.skFollow, false);
              oS(b.ball, 'backgroundColor', col);
              oS(b.ball, 'border', oPx(Math.max(2, D * 0.07)) + ' solid #ffffff');
              osuDiscAt(b.ball, bx, by, D * 0.86); oV(b.ball, true);
              var fd = D * (osuF.held ? 2.4 : 1.15);
              oS(b.follow, 'border', oPx(Math.max(2, D * 0.05)) + ' solid #ffffffd0');
              osuDiscAt(b.follow, bx, by, fd); oV(b.follow, !!osuF.held);
            }
          }
        }
        // Hit circle (or slider head)
        var headDone = o.head !== -1;
        var since = headDone ? Math.max(0, t - o.headAt) : -1;
        if (headDone && since > 240) continue;
        var c = osuCircleRec(ci++);
        oV(c.box, true);
        // The skin's circle: sliderstartcircle for slider heads when the skin has one (its overlay
        // only from the same prefix, as lazer LegacyMainCirclePiece), else hitcircle.
        var cName = isSlider && sk && osuSkinEl('sliderstartcircle') ? 'sliderstartcircle' : 'hitcircle';
        var cE = sk ? osuSkinEl(cName) : null;
        if (cE) {
          var B2 = D * 1.7, h2 = B2 / 2;
          osuBox(c.box, x - h2, y - h2, B2, B2);
          oS(c.box, 'zIndex', String(100000 - o.i));
          oV(c.ring, false); oV(c.core, false); oV(c.num, false); oV(c.app, false);
          var a2 = alpha, sc2 = 1, numA = 1;
          if (headDone) {
            // Legacy hit: circle + overlay grow to 1.4x and fade over 240 ms; the number goes in 60
            // (skin version 2.0+) or with them. A miss just fades.
            var u2 = Math.min(1, since / 240);
            a2 = o.head > 0 ? 1 - u2 : 1 - Math.min(1, since / 120);
            sc2 = o.head > 0 ? 1 + 0.4 * u2 : 1;
            numA = (sk.version || 1) >= 2 ? Math.max(0, 1 - since / 60) : 1;
            if (hd && o.head > 0) a2 = 0;
          }
          oS(c.box, 'opacity', a2.toFixed(3));
          if (osuElVis(cE)) { osuImgAt(c.skBase, cE, h2, h2, os * sc2); osuTint(c.skBase, col); } else oV(c.skBase, false);
          var ovE = osuSkinEl(cName + 'overlay');
          if (osuElVis(ovE)) osuImgAt(c.skOv, ovE, h2, h2, os * sc2); else oV(c.skOv, false);
          // The number: HitCirclePrefix glyphs at 0.8x, advancing by width - HitCircleOverlap.
          var digits = String(o.n), ns = os * 0.8, ov = sk.overlap || 0, glyphs = [], tw = 0, okNum = true;
          for (var di = 0; di < digits.length && di < c.skNum.length; di++) {
            var gE = osuSkinEl('n' + digits.charAt(di));
            if (!gE) { okNum = false; break; }
            glyphs.push(gE);
          }
          for (var dj = 0; dj < c.skNum.length; dj++) oV(c.skNum[dj], false);
          if (okNum && glyphs.length) {
            var xs2 = [], cur = 0;
            for (var dk = 0; dk < glyphs.length; dk++) {
              xs2.push(cur);
              var gw = glyphs[dk][1] / glyphs[dk][3];
              tw = cur + gw;
              cur += gw - ov;
            }
            for (var dl = 0; dl < glyphs.length; dl++) {
              var gx = h2 + (xs2[dl] + glyphs[dl][1] / glyphs[dl][3] / 2 - tw / 2) * ns;
              osuImgAt(c.skNum[dl], glyphs[dl], gx, h2, ns);
              oS(c.skNum[dl], 'zIndex', sk.overlayAbove ? '2' : '4');
              oS(c.skNum[dl], 'opacity', numA.toFixed(3));
            }
          } else if (!headDone) {
            oV(c.num, true); oT(c.num, digits);
          }
          var apE = osuSkinEl('approachcircle');
          var showApp2 = !headDone && t < o.t && !hd && osuElVis(apE);
          if (showApp2) {
            osuImgAt(c.skApp, apE, x, y, os * (1 + 3 * Math.max(0, (o.t - t) / pre)));
            osuTint(c.skApp, col);
            oS(c.skApp, 'opacity', (alpha * 0.9).toFixed(3));
          } else oV(c.skApp, false);
          continue;
        }
        oV(c.skBase, false); oV(c.skOv, false); oV(c.skApp, false);
        for (var dm = 0; dm < c.skNum.length; dm++) oV(c.skNum[dm], false);
        oV(c.ring, true);
        // The box has room for the hit burst (up to 1.5x): scaling the panel with a transform
        // instead got it clipped flat to its own box in game.
        var B = D * 1.7;
        osuBox(c.box, x - B / 2, y - B / 2, B, B);
        if (c.col !== col) {
          c.col = col;
          oS(c.ring, 'backgroundColor', col + '1d'); // the combo colour at ~11%
          oS(c.app, 'border', oPx(Math.max(2, D * 0.045)) + ' solid ' + col);
        }
        oT(c.num, String(o.n));
        var fs = Math.max(12, D * 0.4);
        oS(c.num, 'fontSize', fs.toFixed(0) + 'px');
        // A centred Label's box carries the font's descender room, so digits sit high: nudge down.
        oS(c.num, 'transform', 'translate3d(0px, ' + (fs * 0.17).toFixed(1) + 'px, 0px)');
        oS(c.box, 'zIndex', String(100000 - o.i));
        var a = alpha, sc = 1;
        if (headDone) {
          var u = Math.min(1, since / 240);
          a = o.head > 0 ? 1 - u : 1 - Math.min(1, since / 120);
          sc = o.head > 0 ? 1 + 0.5 * u : 1;
          if (hd && o.head > 0) a = 0; // Hidden: no burst
        }
        var rd = D * sc;
        osuRound(c.ring, rd);
        oS(c.ring, 'border', oPx(Math.max(1.5, rd * 0.045)) + ' solid #ffffff'); // thin rim, same size
        oV(c.num, !headDone);
        oS(c.box, 'opacity', a.toFixed(3));
        // Approach circle: 4x the circle at appearance down to 1x at the hit time. None in Hidden.
        var showApp = !headDone && t < o.t && !hd;
        oV(c.app, showApp);
        if (showApp) {
          osuDiscAt(c.app, x, y, D * (1 + 3 * Math.max(0, (o.t - t) / pre)));
          oS(c.app, 'opacity', (alpha * 0.9).toFixed(3));
        }
      }
      for (var h1 = ci; h1 < osuCircles.length; h1++) { oV(osuCircles[h1].box, false); oV(osuCircles[h1].app, false); oV(osuCircles[h1].skApp, false); }
      for (var h2 = si; h2 < osuSliders.length; h2++) oV(osuSliders[h2].root, false);
      for (var h3 = bi; h3 < osuBalls.length; h3++) { oV(osuBalls[h3].ball, false); oV(osuBalls[h3].follow, false); oV(osuBalls[h3].skBall, false); oV(osuBalls[h3].skFollow, false); }

      osuDrawSpinner(t);
      osuDrawPops();
      osuDrawFlashlight(sz);
      osuDrawHud(t, dt, sz);
    }
)EGJS"
// Split: MSVC caps one raw string literal at ~16 KB (C2026); adjacent literals concatenate.
R"EGJS(
    var osuSpinBonus = osuCentered(osuFxLayer, '#ffffff', 46);
    osuSpinBonus.l.style.textShadow = '0px 2px 6px 1.0 #000000cc';
    var osuSkSpin = osuFull(osuFxLayer);
    var osuSkSpinBg = osuImg(osuSkSpin), osuSkSpinCircle = osuImg(osuSkSpin), osuSkSpinText = osuImg(osuSkSpin);
    function osuDrawSpinner(t) {
      var sp = osuF.spin;
      // Bonus spins: the running total, under the centre (stable shows it there in the score font).
      var bonus = sp ? (sp.bonus || 0) : 0;
      oV(osuSpinBonus.p, bonus > 0);
      if (bonus > 0) { osuBox(osuSpinBonus.p, osuX(256) - 200, osuY(192) + 60 * osuPF.s, 400, 70); oT(osuSpinBonus.l, String(bonus)); }
      var scE = osuSkin() ? osuSkinEl('spinner-circle') : null;
      oV(osuSkSpin, !!sp && !!scE);
      if (sp && scE) {
        // Old-style legacy spinner: background, the turning circle, and spin / clear text, in stable's
        // 1024x768 screen units (playfield scale / 1.6).
        oV(osuSpin, false);
        var u = osuPF.s / 1.6, cx = osuX(256), cy = osuY(192);
        var bgE = osuSkinEl('spinner-background');
        if (osuElVis(bgE)) osuImgAt(osuSkSpinBg, bgE, cx, cy, u); else oV(osuSkSpinBg, false);
        if (osuElVis(scE)) {
          osuImgAt(osuSkSpinCircle, scE, cx, cy, u);
          oS(osuSkSpinCircle, 'transform', 'rotateZ(' + (sp.rot * 57.2958 % 360).toFixed(1) + 'deg)');
        } else oV(osuSkSpinCircle, false);
        var txE = osuSkinEl(sp.prog >= 1 ? 'spinner-clear' : 'spinner-spin');
        if (osuElVis(txE)) osuImgAt(osuSkSpinText, txE, cx, cy + 0.36 * 384 * osuPF.s, u); else oV(osuSkSpinText, false);
        return;
      }
      oV(osuSpin, !!sp);
      if (!sp) return;
      var sd = 384 * osuPF.s * 0.84, c = sd / 2;
      osuBox(osuSpin, osuX(256) - c, osuY(192) - c, sd, sd);
      osuDiscAt(osuSpinBack, c, c, sd);
      osuDiscAt(osuSpinRing, c, c, sd);
      osuDiscAt(osuSpinFill, c, c, Math.max(4, sd * Math.min(1, sp.prog)));
      oS(osuSpinFill, 'backgroundColor', sp.prog >= 1 ? '#7dff8c66' : '#ff8fd855');
      osuDiscAt(osuSpinApp, c, c, Math.max(8, sd * sp.left));
      // The arm: a dot riding the rim, turned by the spin so far.
      osuBox(osuSpinArm, 0, 0, sd, sd);
      osuDiscAt(osuSpinDot, c, 18, 26);
      oS(osuSpinArm, 'transform', 'rotateZ(' + (sp.rot * 57.2958 % 360).toFixed(1) + 'deg)');
      osuBox(osuSpinTxt.p, 0, c - 30, sd, 60);
      oT(osuSpinTxt.l, sp.prog >= 1 ? 'clear!' : 'spin!');
    }

    function osuDrawPops() {
      var pops = osuF.pops || [], n = 0, ni = 0, sk = osuSkin();
      for (var pi = 0; pi < pops.length && n < osuPops.length; pi++) {
        var pp = pops[pi], spec = OSU_POP[pp[3]] || OSU_POP[0];
        var jE = sk ? osuSkinEl('hit' + (OSU_POP[pp[3]] ? pp[3] : 0)) : null;
        if (jE) {
          // Legacy judgement: pops in (0.6x -> 1.1x -> 1x over 200 ms) and fades out by 800 ms; a miss
          // instead drops a little and tilts. A 1x1 sprite (often hit300) shows nothing.
          if (!osuElVis(jE) || ni >= osuPopImgs.length) continue;
          var ag = pp[4], Q = osuPopImgs[ni++], miss = pp[3] === 0;
          var jsc = miss ? 1.0 : (ag < 100 ? 0.6 + 0.5 * (ag / 100) : 1.1 - 0.1 * osuClamp((ag - 100) / 100));
          var jy = osuY(pp[2]) + (miss ? 20 * osuPF.s / 1.6 * osuClamp(ag / 800) : 0);
          osuImgAt(Q, jE, osuX(pp[1]), jy, osuPF.s / 1.6 * jsc);
          oS(Q, 'transform', miss ? 'rotateZ(' + (8 * osuClamp(ag / 800)).toFixed(1) + 'deg)' : 'none');
          oS(Q, 'opacity', (ag < 600 ? Math.min(1, ag / 40) : Math.max(0, 1 - (ag - 600) / 200)).toFixed(3));
          continue;
        }
        if (!spec[0]) continue; // 300s stay quiet, like lazer
        var P = osuPops[n++], age = pp[4];
        oT(P.l, spec[0]); oS(P.l, 'color', spec[1]);
        osuBox(P.p, osuX(pp[1]) - 70, osuY(pp[2]) - 24 - osuClamp(age / 600) * 22, 140, 48);
        oS(P.p, 'opacity', Math.max(0, 1 - age / 800).toFixed(3));
        oV(P.p, true);
      }
      for (; n < osuPops.length; n++) oV(osuPops[n].p, false);
      for (; ni < osuPopImgs.length; ni++) oV(osuPopImgs[ni], false);
    }

    function osuDrawFlashlight(sz) {
      var on = !!osuF.fl && osuF.m === 'play';
      oV(osuFl, on); oV(osuFlSoft, on);
      if (!on) return;
      var combo = osuF.combo || 0;
      var scale = combo >= 200 ? 0.625 : (combo >= 100 ? 0.8125 : 1);
      var hole = 2 * 125 * 0.9 * scale * osuPF.s;
      var Bd = Math.max(sz.w, sz.h) * 1.6, S = hole + 2 * Bd;
      var cx = osuX(osuF.cx), cy = osuY(osuF.cy);
      osuBox(osuFl, cx - S / 2, cy - S / 2, S, S);
      oS(osuFl, 'borderRadius', oPx(S / 2));
      oS(osuFl, 'border', oPx(Bd) + ' solid #000000');
      osuBox(osuFlSoft, cx - hole / 2, cy - hole / 2, hole, hole);
      oS(osuFlSoft, 'borderRadius', oPx(hole / 2));
      oS(osuFlSoft, 'border', oPx(hole * 0.14) + ' solid #00000080');
    }

    function osuDrawHud(t, dt, sz) {
      // The paused / failed screens carry their own title, health and stats (UI pass 2c / 2d).
      oV(osuHud, osuF.m === 'play');
      if (osuF.m !== 'play') return;
      var W = sz.w, H = sz.h;
      // Health: top left.
      var hw = W * 0.34;
      osuBox(osuHpBack, 24, 22, hw, 14);
      oS(osuHpBack, 'borderRadius', '7px');
      var hp = osuClamp(osuF.hp || 0);
      osuBox(osuHpFill, 0, 0, Math.max(0, (hw - 2) * hp), 12);
      oS(osuHpFill, 'borderRadius', '6px');
      oS(osuHpFill, 'backgroundColor', hp < 0.25
        ? 'gradient(linear, 0% 0%, 100% 0%, from(#ff3b5c), to(#ff8fa0))'
        : 'gradient(linear, 0% 0%, 100% 0%, from(#ff5fc8), to(#ffe3f6))');
      // Score, accuracy, progress, mods: top right.
      osuBox(osuScoreT.p, W - 524, 10, 500, 64);
      oT(osuScoreT.l, osuScoreStr(osuF.score || 0));
      osuBox(osuAccT.p, W - 524, 70, 500, 34);
      oT(osuAccT.l, ((osuF.acc || 0) * 100).toFixed(2) + '%');
      osuBox(osuProgBack, W - 24 - 220, 108, 220, 5);
      oS(osuProgBack, 'borderRadius', '2px');
      osuBox(osuProgFill, 0, 0, 220 * osuClamp(osuF.prog || 0), 5);
      osuModPills(osuHudMods, osuF.mods || [], W - 24, 124, 24, true);
      // Combo: bottom left, with a pop on every increase.
      var combo = osuF.combo || 0;
      if (combo > osuComboLast) osuComboPop = 1;
      osuComboLast = combo;
      osuComboPop = Math.max(0, osuComboPop - dt * 6);
      var cs = 1 + 0.18 * osuComboPop;
      osuBox(osuComboT.p, 26, H - 110, 400, 92);
      oS(osuComboT.l, 'transform', 'scale3d(' + cs.toFixed(3) + ', ' + cs.toFixed(3) + ', 1)');
      oT(osuComboT.l, combo + 'x');
      // Skip the intro: bottom right.
      var skip = !!osuF.skip && osuF.m === 'play';
      oV(osuSkip.p, skip);
      if (skip) {
        var pulse = 0.75 + 0.25 * Math.sin(t / 160);
        osuBox(osuSkip.p, W - 260, H - 110, 220, 62);
        oS(osuSkip.p, 'borderRadius', '12px');
        oS(osuSkip.p, 'opacity', pulse.toFixed(3));
        oT(osuSkip.l, 'skip  >>   space');
      }
      var note = osuF.auto ? 'autoplay' : (osuF.silent ? 'no audio for this map - playing silently' : '');
      oV(osuNotice.p, !!note);
      if (note) { osuBox(osuNotice.p, W * 0.5 - 300, 18, 600, 32); oT(osuNotice.l, note); }
    }

    // Cursor + trail (the OS pointer is hidden while playing).
    var osuCur = osuDisc(osuCursorLayer, 34, '#ff8fd840', 4, '#fff4fbff');
    var osuCurDot = osuDisc(osuCursorLayer, 8, '#ffffff', 0, '');
    var osuTrail = [], osuTrailPos = [];
    for (var oti = 0; oti < 10; oti++) {
      osuTrail.push(osuDisc(osuCursorLayer, 14, '#ff8fd8', 0, ''));
      osuTrailPos.push([0, 0]);
    }
    // Skin cursor + lazer's disjoint legacy trail (LegacyCursorTrail): a trail sprite is laid at the
    // cursor every 1000/60 ms and each fades out linearly over 150 ms. The cursor is in stable's
    // 1024x768 screen units (playfield scale / 1.6) times the player's cursor size; the trail texture
    // is shrunk a further 1.6x (its ScaleAdjust *= STABLE_MAGIC_SCALE_FACTOR).
    var osuSkTrail = [], osuSkTrailPts = [], osuSkClock = 0, osuSkLastSpawn = -1;
    for (var ost = 0; ost < 16; ost++) osuSkTrail.push(osuImg(osuCursorLayer));
    var osuSkCur = osuImg(osuCursorLayer), osuSkCurMid = osuImg(osuCursorLayer);
    function osuDrawSkinCursor(cx, cy, dt, cE) {
      var sk = osuSkin(), u = osuPF.s / 1.6 * (sk.cursorSize || 1), ut = u / 1.6;
      osuSkClock += Math.max(0, Math.min(0.1, dt || 0)) * 1000;
      var tE = osuSkinEl('cursortrail');
      if (osuElVis(tE)) {
        if (osuSkLastSpawn < 0 || osuSkClock - osuSkLastSpawn >= 1000 / 60) {
          osuSkLastSpawn = osuSkClock;
          osuSkTrailPts.push([cx, cy, osuSkClock]);
        }
        while (osuSkTrailPts.length && osuSkClock - osuSkTrailPts[0][2] > 150) osuSkTrailPts.shift();
        while (osuSkTrailPts.length > osuSkTrail.length) osuSkTrailPts.shift();
        for (var i = 0; i < osuSkTrail.length; i++) {
          var tp = osuSkTrailPts[i];
          if (!tp) { oV(osuSkTrail[i], false); continue; }
          osuImgAt(osuSkTrail[i], tE, tp[0], tp[1], ut);
          oS(osuSkTrail[i], 'opacity', osuClamp(1 - (osuSkClock - tp[2]) / 150).toFixed(3));
        }
      } else for (var j = 0; j < osuSkTrail.length; j++) oV(osuSkTrail[j], false);
      // CursorExpand: 1.2x while a key is held (legacy pressed scale); CursorCentre off anchors the
      // sprite's top-left at the pointer.
      var cs = (sk.cursorExpand && osuF.held) ? 1.2 : 1;
      if (osuElVis(cE)) {
        var w = cE[1] / cE[3] * u * cs, h = cE[2] / cE[3] * u * cs;
        osuImgAt(osuSkCur, cE, sk.cursorCentre ? cx : cx + w / 2, sk.cursorCentre ? cy : cy + h / 2, u * cs);
      } else oV(osuSkCur, false);
      var mE = osuSkinEl('cursormiddle');
      if (osuElVis(mE)) osuImgAt(osuSkCurMid, mE, cx, cy, u); else oV(osuSkCurMid, false);
    }
    function osuDrawCursor(mode, dt, sz) {
      var on = mode === 'play';
      oV(osuCursorLayer, on);
      if (!on) return;
      var cx = osuX(osuF.cx), cy = osuY(osuF.cy);
      var cE = osuSkin() ? osuSkinEl('cursor') : null;
      if (cE) {
        oV(osuCur, false); oV(osuCurDot, false);
        for (var hk = 0; hk < osuTrail.length; hk++) oV(osuTrail[hk], false);
        osuDrawSkinCursor(cx, cy, dt, cE);
        return;
      }
      oV(osuSkCur, false); oV(osuSkCurMid, false);
      for (var hj = 0; hj < osuSkTrail.length; hj++) oV(osuSkTrail[hj], false);
      oV(osuCur, true); oV(osuCurDot, true);
      var d = osuF.held ? 44 : 36;
      osuDiscAt(osuCur, cx, cy, d);
      osuDiscAt(osuCurDot, cx, cy, 8);
      osuTrailPos.unshift([cx, cy]); osuTrailPos.pop();
      for (var ti = 0; ti < osuTrail.length; ti++) {
        oV(osuTrail[ti], true);
        var tp = osuTrailPos[ti], k = 1 - ti / osuTrail.length;
        osuDiscAt(osuTrail[ti], tp[0], tp[1], 6 + 10 * k);
        oS(osuTrail[ti], 'opacity', (0.45 * k).toFixed(3));
      }
    }
)EGJS"
