#pragma once

// osu! song-select footer + the mod select and options TAKEOVERS (part of EasterEggOsuJs.h; shares
// the overlay scope). Built to the "osu! UI pass" handoff, round 2 (frames 2a / 2b) and the round-1
// footer (frame 1j): menus are hit circles (osuHC), options are filling dials like the in-game volume
// meter. The takeovers use the handoff's 1920x1080 design space (osuLay / dX / dY / dS); song select
// itself stays in screen pixels. Clicks route exactly as before: `act ...`, `mod ...`, `set ...`.

R"EGJS(
    // ---- song-select footer (frame 1j) ----
    // 66px #0a0612f2 with a 1px top rule; 42px buttons, radius 10, 10px apart: [esc] back | [F1] mods
    // + the active mods + multiplier | [F2] random | options ... "arrows / wheel to browse"  [play enter].
    var osuFoot = osuPanel(osuChrome, '#0a0612f2');
    osuFoot.style.borderTop = '1px solid #ffffff14';
    function osuFootBtn() { var b = osuPanel(osuChrome, null); b.style.borderRadius = '10px'; return { p: b, lab: osuAligned(b, '#ffffff', 17, 'left', 'center'), cap: osuCentered(b, '#ffffffb3', 11, '#0a0612') }; }
    var osuFtBack = osuFootBtn(), osuFtMods = osuFootBtn(), osuFtRand = osuFootBtn(), osuFtOpt = osuFootBtn(), osuFtPlay = osuFootBtn();
    var osuFtDiv = osuPanel(osuChrome, '#ffffff1f');
    var osuFtPills = osuPillPool(osuChrome, 8, 11);
    var osuFtMult = osuAligned(osuChrome, '#c9a7ff', 13, 'left', 'center');
    var osuFtHint = osuAligned(osuChrome, '#ffffff66', 14, 'right', 'center');
    function osuFtCap(c, x, y, key, dark) {
      var w = Math.max(22, 10 + key.length * 7);
      osuBox(c.p, x, y, w, 20);
      oS(c.p, 'borderRadius', '5px');
      oS(c.p, 'backgroundColor', dark ? '#00000000' : '#0a0612');
      oS(c.p, 'border', '1px solid ' + (dark ? '#120a1c55' : '#ffffff40'));
      oS(c.p, 'borderBottom', '2px solid ' + (dark ? '#120a1c55' : '#ffffff40'));
      oS(c.l, 'color', dark ? '#120a1ccc' : '#ffffffb3');
      oT(c.l, key); oV(c.p, true);
      return w;
    }
    function osuFtDraw(B, x, y, w, bg, border, label, key, cmd) {
      var over = osuOver(x, y, w, 42);
      osuBox(B.p, x, y, w, 42);
      oS(B.p, 'backgroundColor', bg);
      oS(B.p, 'border', '1px solid ' + (over ? '#ffffff66' : border));
      var lx = 16;
      if (key) { osuFtCap(B.cap, 12, 11, key, false); lx = 12 + Math.max(22, 10 + key.length * 7) + 10; } else oV(B.cap.p, false);
      osuBox(B.lab.p, lx, 0, w - lx, 42); oT(B.lab.l, label);
      oV(B.p, true);
      osuHit(x, y, w, 42, cmd);
    }
    function osuDrawFooter(t, sz) {
      var W = sz.w, H = sz.h, y0 = H - OSU_FOOT, by = y0 + 12;
      osuBox(osuFoot, 0, y0, W, OSU_FOOT);
      osuFtDraw(osuFtBack, 18, by, 103, '#00000000', '#ffffff1f', 'back', 'esc', 'act back');
      osuBox(osuFtDiv, 134, by + 6, 1, 30);
      var mods = osuV.mods || [], n = Math.min(mods.length, osuFtPills.length);
      var mw = 92 + n * 42 + 50;
      osuFtDraw(osuFtMods, 150, by, mw, osuV.modsOpen ? '#c9a7ff40' : '#c9a7ff1f', '#c9a7ff59', 'mods', 'F1', 'act mods');
      for (var i = 0; i < osuFtPills.length; i++) {
        if (i >= n) { oV(osuFtPills[i].p, false); continue; }
        osuPill(osuFtPills[i], 150 + 92 + i * 42, by + 11, 38, 20, mods[i], OSU_MOD_COL[mods[i]] || '#ffffff', '#120a1c');
      }
      osuBox(osuFtMult.p, 150 + 92 + n * 42 + 4, by, 46, 42); oT(osuFtMult.l, (osuV.mult || 1).toFixed(2) + 'x');
      var rx = 150 + mw + 10;
      osuFtDraw(osuFtRand, rx, by, 117, '#9fe6ff14', '#9fe6ff40', 'random', 'F2', 'act random');
      var opt = osuV.opt || {};
      osuFtDraw(osuFtOpt, rx + 127, by, 86, opt.open ? '#ffd36b40' : '#ffd36b14', '#ffd36b40', 'options', '', 'act options');
      // Play: 200 x 42, pink, "play" with an [enter] cap on its right.
      var px = W - 18 - 200, can = osuV.matched > 0;
      oV(osuFtPlay.p, can);
      if (can) {
        var over = osuOver(px, by, 200, 42);
        osuBox(osuFtPlay.p, px, by, 200, 42);
        oS(osuFtPlay.p, 'backgroundColor', over ? '#ff7fd4' : '#ff5fc8');
        oS(osuFtPlay.p, 'border', '1px solid #ff5fc8');
        osuBox(osuFtPlay.lab.p, 22, 0, 120, 42); oS(osuFtPlay.lab.l, 'color', '#120a1c'); oS(osuFtPlay.lab.l, 'fontSize', '20px'); oT(osuFtPlay.lab.l, 'play');
        osuFtCap(osuFtPlay.cap, 200 - 14 - 38, 11, 'enter', true);
        osuHit(px, by, 200, 42, 'act play');
      }
      osuBox(osuFtHint.p, px - 20 - 300, y0, 300, OSU_FOOT); oT(osuFtHint.l, 'arrows / wheel to browse');
    }

    // ---- mod select takeover (frame 2a) ----
    function osuTakeover(parent) {
      var bg = osuFull(parent); bg.style.backgroundColor = '#07040ef2';
      var wash = osuFull(parent); wash.style.backgroundColor = 'gradient(linear, 0% 0%, 0% 30%, from(#ff8fd80d), to(#00000000))';
      return bg;
    }
    osuTakeover(osuModsLayer);
    var osuMdCap = osuDLabel(osuModsLayer), osuMdMult = osuFitLabel(osuModsLayer), osuMdMultCap = osuDLabel(osuModsLayer);
    var osuMdPills = osuPillPool(osuModsLayer, 8, 15);
    var OSU_MD_GROUPS = [['reduction', '#b2ff66'], ['increase', '#ff6b6b'], ['automation', '#66ccff']];
    var osuMdGroups = [];
    for (var omg = 0; omg < 3; omg++) { var gb = osuPanel(osuModsLayer, null); gb.style.borderRadius = '2px'; osuMdGroups.push({ bar: gb, name: osuDLabel(osuModsLayer) }); }
    var osuMdCircles = [];
    for (var omc = 0; omc < 12; omc++) osuMdCircles.push(osuHC(osuModsLayer));
    var osuMdHint = osuDLabel(osuModsLayer), osuMdBack = osuFitLabel(osuModsLayer), osuMdCapF1 = osuCap(osuModsLayer), osuMdCapEsc = osuCap(osuModsLayer);
    var osuMdOpenAt = -1, osuMdWas = false;

    function osuDrawMods(t, sz) {
      var open = !!osuV.modsOpen;
      oV(osuModsLayer, open);
      if (open && !osuMdWas) osuMdOpenAt = t;
      osuMdWas = open;
      if (!open) return;
      osuLay(sz);
      oS(osuModsLayer, 'opacity', osuClamp((t - osuMdOpenAt) / 0.12).toFixed(3));
      osuHit(0, 0, sz.w, sz.h, 'act mods'); // anywhere but a circle closes it
      // Header: MODS, the multiplier (150px), then "score multiplier" + the active mods beside it.
      osuDText(osuMdCap, 120, 64, 400, 24, 'MODS', 18, '#ff8fd8', 4);
      var mul = (osuV.mult || 1).toFixed(2) + 'x';
      osuAt(osuMdMult.p, dX(114), dY(78));
      osuFs(osuMdMult.l, 150); oS(osuMdMult.l, 'letterSpacing', oPx(dS(-2))); oT(osuMdMult.l, mul);
      var sx = 114 + osuFitWidth(osuMdMult, mul.length * 66) + 28;
      osuDText(osuMdMultCap, sx, 200, 420, 28, 'score multiplier', 20, '#ffffff80', 0);
      var mods = osuV.mods || [];
      for (var p = 0; p < osuMdPills.length; p++) {
        if (p >= mods.length) { oV(osuMdPills[p].p, false); continue; }
        osuPill(osuMdPills[p], dX(sx + p * 56), dY(232), dS(50), dS(28), mods[p], OSU_MOD_COL[mods[p]] || '#ffffff', '#120a1c');
        osuFs(osuMdPills[p].l, 15);
      }
      // Group rows (circle centres at y 372 / 612 / 852): a 40x4 bar and the name above-left.
      var ys = [372, 612, 852], cnt = [0, 0, 0], used = 0, tiles = osuV.modTable || [];
      for (var g = 0; g < 3; g++) {
        var G = osuMdGroups[g], col = OSU_MD_GROUPS[g][1];
        osuBox(G.bar, dX(120), dY(ys[g] - 34), dS(40), dS(4)); oS(G.bar, 'backgroundColor', col);
        osuDText(G.name, 120, ys[g] - 22, 330, 30, OSU_MD_GROUPS[g][0].toUpperCase(), 24, col, 2);
      }
      for (var i = 0; i < tiles.length && used < osuMdCircles.length; i++) {
        var M = tiles[i], gi = Math.max(0, Math.min(2, M.g)), k = cnt[gi]++;
        osuHCDraw(osuMdCircles[used++], t, { cx: 555 + k * 190, cy: ys[gi], d: 150, on: !!M.on, col: OSU_MD_GROUPS[gi][1],
          label: M.a, ls: 46, name: M.n, ns: 19, sub: M.m.toFixed(2) + 'x', key: M.k, cmd: 'mod ' + M.a });
      }
      for (; used < osuMdCircles.length; used++) osuHCHide(osuMdCircles[used]);
      // Footer row: the hint left, [F1] [esc] back to song select right (right edge 1800).
      osuDText(osuMdHint, 120, 1006, 600, 26, 'click a circle or hit its key', 16, '#ffffff73', 0);
      osuFs(osuMdBack.l, 16); oS(osuMdBack.l, 'color', '#ffffffcc'); oT(osuMdBack.l, 'back to song select');
      var bw = osuFitWidth(osuMdBack, 150), bx = 1800 - bw;
      osuAt(osuMdBack.p, dX(bx), dY(1008));
      var ew = osuCapDraw(osuMdCapEsc, bx - 10 - 34, 1005, 'esc', null, false);
      osuCapDraw(osuMdCapF1, bx - 10 - ew - 8 - 30, 1005, 'F1', null, false);
    }
)EGJS"
// Split: MSVC caps one raw string literal at ~16 KB (C2026); adjacent literals concatenate.
R"EGJS(
    // ---- options takeover (frame 2b) ----
    // Four dials (D 260, centres x 450 / 790 / 1130 / 1470, top 430): a #0c0814 ring with a 6px border
    // and a disc whose AREA is the value (D * 0.86 * sqrt(f)), like the in-game volume meter.
    var osuOptLayer = osuFull(osuSelect);
    osuTakeover(osuOptLayer);
    var OSU_OPTS = [
      ['sens', 'cursor sensitivity', 0.1, 6.0, 0.05, function (v) { return v.toFixed(2) + 'x'; }, '#9fe6ff'],
      ['master', 'master volume', 0, 1, 0.05, function (v) { return Math.round(v * 100) + '%'; }, '#ff8fd8'],
      ['music', 'music volume', 0, 1, 0.05, function (v) { return Math.round(v * 100) + '%'; }, '#ff8fd8'],
      ['effects', 'effect volume', 0, 1, 0.05, function (v) { return Math.round(v * 100) + '%'; }, '#ff8fd8']
    ];
    var osuOpCap = osuDLabel(osuOptLayer), osuOpTitle = osuDLabel(osuOptLayer);
    var osuOpSecs = [];
    for (var oos = 0; oos < 2; oos++) osuOpSecs.push({ l: osuDLabel(osuOptLayer), rule: osuPanel(osuOptLayer, null) });
    var osuOpDials = [];
    for (var ood = 0; ood < OSU_OPTS.length; ood++) {
      osuOpDials.push({ ring: osuDisc(osuOptLayer, 10, '#0c0814', 6, '#ffffff'), fill: osuDisc(osuOptLayer, 10, '#ffffff', 0, ''),
        val: osuCentered(osuOptLayer, '#ffffff', 60), name: osuCentered(osuOptLayer, '#ffffffd9', 22),
        minus: osuCentered(osuOptLayer, '#ffffffcc', 28), plus: osuCentered(osuOptLayer, '#ffffffcc', 28) });
    }
    var osuOpSkinCap = osuFitLabel(osuOptLayer), osuOpSkin = osuDLabel(osuOptLayer), osuOpHint = osuDLabel(osuOptLayer);
    var osuOpDone = { p: osuPanel(osuOptLayer, '#ff5fc8') };
    osuOpDone.l = osuAligned(osuOpDone.p, '#120a1c', 22, 'left', 'center');
    osuOpDone.cap = osuCap(osuOptLayer);
    var osuOpOpenAt = -1, osuOpWas = false;
    // Dial drag: press on a dial and drag -- up or right raises it, down or left lowers it, linearly
    // (400 design px sweep the whole range); shift is 5x finer. Per-frame deltas, so shift can change
    // mid-drag. The value is shown at once and sent (`set`) at most every 50 ms and on release.
    var osuDrag = { i: -1, v: 0, lx: 0, ly: 0, sent: null, sentAt: 0 }, osuLmbWas = false;

    function osuStepper(B, cx, cy, glyph, col, cmd) {
      var r = 27, over = osuOver(dX(cx - r), dY(cy - r), dS(54), dS(54));
      osuBox(B.p, dX(cx - r), dY(cy - r), dS(54), dS(54));
      oS(B.p, 'borderRadius', oPx(dS(27)));
      oS(B.p, 'border', oPx(Math.max(1, dS(2))) + ' solid ' + col + (over ? 'b3' : '59'));
      osuFs(B.l, 28); oT(B.l, glyph); oV(B.p, true);
      osuHit(dX(cx - r), dY(cy - r), dS(54), dS(54), cmd);
    }

    function osuDrawOptions(t, sz) {
      var opt = osuV.opt || {}, open = !!opt.open;
      oV(osuOptLayer, open);
      if (open && !osuOpWas) osuOpOpenAt = t;
      osuOpWas = open;
      if (!open) return;
      osuLay(sz);
      oS(osuOptLayer, 'opacity', osuClamp((t - osuOpOpenAt) / 0.12).toFixed(3));
      osuHit(0, 0, sz.w, sz.h, '');
      osuDText(osuOpCap, 120, 84, 400, 24, 'SETTINGS', 18, '#ff8fd8', 4);
      osuDText(osuOpTitle, 114, 102, 900, 160, 'options', 130, '#ffffff', -2);
      // Section labels: INPUT over the sensitivity dial, AUDIO over the three volumes.
      var secs = [['INPUT', 320, 260, '#9fe6ff'], ['AUDIO', 660, 940, '#ff8fd8']];
      for (var s = 0; s < 2; s++) {
        osuDText(osuOpSecs[s].l, secs[s][1], 350, secs[s][2], 20, secs[s][0], 16, secs[s][3], 3);
        osuBox(osuOpSecs[s].rule, dX(secs[s][1]), dY(380), dS(secs[s][2]), Math.max(1, dS(2)));
        oS(osuOpSecs[s].rule, 'backgroundColor', secs[s][3] + '40');
      }
      var cxs = [450, 790, 1130, 1470], D = 260, cy = 430 + D / 2;
      var lmb = !!osuF.lmb;
      if (lmb && !osuLmbWas) {
        // A press on a dial (inside its circle) starts a drag of that dial.
        for (var di = 0; di < OSU_OPTS.length; di++) {
          var ddx = osuMouse.x - dX(cxs[di]), ddy = osuMouse.y - dY(cy), rr = dS(D / 2);
          if (ddx * ddx + ddy * ddy <= rr * rr) {
            osuDrag = { i: di, v: opt[OSU_OPTS[di][0]] || 0, lx: osuMouse.x, ly: osuMouse.y, sent: null, sentAt: 0 };
          }
        }
      }
      if (osuDrag.i >= 0) {
        var DO = OSU_OPTS[osuDrag.i], range = DO[3] - DO[2];
        if (lmb) {
          var mv = ((osuMouse.x - osuDrag.lx) - (osuMouse.y - osuDrag.ly)) / Math.max(0.01, osuL.u);
          osuDrag.lx = osuMouse.x; osuDrag.ly = osuMouse.y;
          osuDrag.v = Math.max(DO[2], Math.min(DO[3], osuDrag.v + mv * range / 400 * (osuF.shift ? 0.2 : 1)));
        }
        var sv = (Math.round(osuDrag.v * 100) / 100).toFixed(2);
        if (sv !== osuDrag.sent && (!lmb || t - osuDrag.sentAt > 0.05)) {
          osuCmd('set ' + DO[0] + ' ' + sv); osuDrag.sent = sv; osuDrag.sentAt = t;
        }
        if (!lmb) osuDrag.i = -1;
      }
      osuLmbWas = lmb;
      for (var i = 0; i < OSU_OPTS.length; i++) {
        var O = OSU_OPTS[i], Q = osuOpDials[i], col = O[6], cx = cxs[i];
        var v = osuDrag.i === i ? osuDrag.v : (opt[O[0]] || 0); // the dial being dragged shows its live value
        var f = osuClamp((v - O[2]) / (O[3] - O[2]));
        osuDiscAt(Q.ring, dX(cx), dY(cy), dS(D));
        oS(Q.ring, 'border', oPx(Math.max(1, dS(6))) + ' solid ' + col + '59');
        osuDiscAt(Q.fill, dX(cx), dY(cy), Math.max(1, dS(D * 0.86 * Math.sqrt(f))));
        oS(Q.fill, 'backgroundColor', col + '8c');
        osuBox(Q.val.p, dX(cx - D / 2), dY(cy - 45), dS(D), dS(90)); osuFs(Q.val.l, 60); oT(Q.val.l, O[5](v));
        osuBox(Q.name.p, dX(cx - (D + 80) / 2), dY(430 + D + 22), dS(D + 80), dS(30)); osuFs(Q.name.l, 22); oT(Q.name.l, O[1]);
        osuHit(dX(cx - D / 2), dY(cy - D / 2), dS(D), dS(D), ''); // the dial drags; a click alone does nothing
        osuStepper(Q.minus, cx - 34, 430 + D + 64 + 27, '-', col, 'set ' + O[0] + ' ' + Math.max(O[2], v - O[4]).toFixed(2));
        osuStepper(Q.plus, cx + 34, 430 + D + 64 + 27, '+', col, 'set ' + O[0] + ' ' + Math.min(O[3], v + O[4]).toFixed(2));
      }
      // Bottom left: SKIN <name>, and the volume hint.
      osuFs(osuOpSkinCap.l, 14); oS(osuOpSkinCap.l, 'color', '#ffffff66'); oS(osuOpSkinCap.l, 'letterSpacing', oPx(dS(2))); oT(osuOpSkinCap.l, 'SKIN');
      osuAt(osuOpSkinCap.p, dX(120), dY(964));
      var kw = osuFitWidth(osuOpSkinCap, 40);
      osuDText(osuOpSkin, 120 + kw + 12, 958, 900, 30, opt.skin || 'built-in', 20, '#ffffff', 0);
      osuDText(osuOpHint, 120, 988, 1400, 24, 'Drag a dial up or right to raise it (shift for fine). In game, scroll for volume (hover a meter to pick it); in song select, alt + scroll.', 15, '#ffffff66', 0);
      // Done: a pink pill, right edge 1800, top 962, 140 x 56, with a dark [esc] cap.
      var dx0 = 1800 - 140, over = osuOver(dX(dx0), dY(962), dS(140), dS(56));
      osuBox(osuOpDone.p, dX(dx0), dY(962), dS(140), dS(56));
      oS(osuOpDone.p, 'borderRadius', oPx(dS(28)));
      oS(osuOpDone.p, 'backgroundColor', over ? '#ff7fd4' : '#ff5fc8');
      osuBox(osuOpDone.l.p, dS(30), 0, dS(80), dS(56)); osuFs(osuOpDone.l.l, 22); oT(osuOpDone.l.l, 'done');
      osuCapDraw(osuOpDone.cap, dx0 + 140 - 16 - 34, 962 + 14, 'esc', null, true);
      osuHit(dX(dx0), dY(962), dS(140), dS(56), 'act options');
    }
)EGJS"
