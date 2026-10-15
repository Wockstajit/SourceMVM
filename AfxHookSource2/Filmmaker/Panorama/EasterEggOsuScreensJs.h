#pragma once

// osu! full-screen states over gameplay (part of EasterEggOsuJs.h; shares the overlay scope):
// loading (waiting for the song to open), pause, failed, results. Menus highlight the entry C++
// has selected (osuF.menu, arrow keys) and every entry is also clickable. No large text-shadow
// blurs anywhere: in game a big blur renders as a clipped ghost copy of the text.

R"EGJS(
    function osuScreenLayer() { var p = osuFull(osuScreens); p.visible = false; return p; }
    var osuLoadS = osuScreenLayer(), osuPauseS = osuScreenLayer(), osuFailS = osuScreenLayer(), osuResS = osuScreenLayer();

    // ---- loading ----
    var osuLoadCard = osuPanel(osuLoadS, '#0c0816e6');
    osuLoadCard.style.border = '1px solid #ffffff22';
    osuLoadCard.style.borderRadius = '18px';
    var osuLoadStar = osuCentered(osuLoadCard, '#000000', 16, '#ffffff');
    var osuLoadVer = osuText(osuLoadCard, '', '#ffffff', 20, 'bold');
    var osuLoadTitle = osuText(osuLoadCard, '', '#ffffff', 36, 'bold');
    var osuLoadArtist = osuText(osuLoadCard, '', '#ffffffd0', 22, 'bold');
    var osuLoadBy = osuText(osuLoadCard, '', '#ffffff8c', 16, 'bold');
    var osuLoadMods = osuPillPool(osuLoadCard, 8, 13);
    var osuLoadMsg = osuAligned(osuLoadCard, '#ff8fd8', 18, 'left', 'center');

    function osuInfoCard(card, star, ver, title, artist, by, mods, x, y, w, h) {
      osuBox(card, x, y, w, h);
      var s = osuF.stars || 0;
      osuPill(star, 26, 24, 86, 30, osuStarStr(s) + ' *', osuStarCol(s), osuStarInk(s));
      oT(ver, osuF.version || ''); osuSetLabel(ver, 126, 25, w - 150);
      oT(title, osuF.title || ''); osuSetLabel(title, 26, 66, w - 52);
      oT(artist, osuF.artist || ''); osuSetLabel(artist, 26, 112, w - 52);
      oT(by, 'mapped by ' + (osuF.mapper || '')); osuSetLabel(by, 26, 146, w - 52);
      osuModPills(mods, osuF.mods || [], 26, 182, 26, false);
    }

    function osuDrawLoading(t, sz) {
      var w = Math.min(760, sz.w * 0.7), x = (sz.w - w) / 2, y = sz.h * 0.5 - 115;
      osuInfoCard(osuLoadCard, osuLoadStar, osuLoadVer, osuLoadTitle, osuLoadArtist, osuLoadBy, osuLoadMods, x, y, w, 230);
      osuBox(osuLoadMsg.p, w - 220, 176, 200, 36);
      oT(osuLoadMsg.l, 'loading' + ['', '.', '..', '...'][Math.floor(t * 3) % 4]);
    }

    // ---- menus ----
    function osuMenu(parent, n) { var a = []; for (var i = 0; i < n; i++) a.push(osuMakeButton(parent, 24)); return a; }
    // ---- paused (UI pass frame 2d) / failed (frame 2c): hit-circle menus in the 1920x1080 design space ----
    // The selected entry is the active circle (fill, white rim, approach ring) and the bigger one;
    // diameters ease toward their target so moving the selection pops.
    var osuPauseShade = osuFull(osuPauseS);
    osuPauseShade.style.backgroundColor = '#05030acc';
    var osuPsSong = osuDLabel(osuPauseS, 'center'), osuPsTitle = osuDLabel(osuPauseS, 'center'), osuPsHint = osuDLabel(osuPauseS, 'center');
    var osuPsBtns = [osuHC(osuPauseS), osuHC(osuPauseS), osuHC(osuPauseS)];
    var OSU_PAUSE = [['continue', '#7ac943', 'continue', 'esc'], ['retry', '#f7a933', 'retry', 'R'], ['quit', '#e6455f', 'quit', 'Q']];

    var osuFailShade = osuFull(osuFailS);
    osuFailShade.style.backgroundColor = '#2a0610d6';
    var osuFlBig = osuDLabel(osuFailS, 'left');
    osuFlBig.l.style.overflow = 'noclip noclip';
    try { osuFlBig.l.style.textOverflow = 'clip'; } catch (e) {}
    var osuFlTrack = osuPanel(osuFailS, '#ffffff0f'), osuFlStub = osuPanel(osuFailS, '#ff4d6d');
    var osuFlWhy = osuDLabel(osuFailS), osuFlTitle = osuDLabel(osuFailS), osuFlSong = osuDLabel(osuFailS);
    var osuFlDied = osuDLabel(osuFailS), osuFlLen = osuDLabel(osuFailS, 'right');
    var osuFlTl = osuPanel(osuFailS, '#ffffff14'), osuFlTlFill = osuPanel(osuFailS, '#ffffff66');
    var osuFlMark = osuDisc(osuFailS, 24, '#ff4d6d', 4, '#1a0610');
    var osuFlStats = [];
    for (var ofs = 0; ofs < 3; ofs++) osuFlStats.push({ cap: osuDLabel(osuFailS), val: osuDLabel(osuFailS) });
    var osuFlBtns = [osuHC(osuFailS), osuHC(osuFailS)];
    var OSU_FAIL = [['retry', '#f7a933', 'retry', 'R', 1430, 44], ['quit', '#e6455f', 'quit', 'Q', 1720, 40]];
    var OSU_WHY = { health: 'your health ran out', 'sudden death': 'sudden death: one miss', perfect: 'perfect: anything less than 300' };

    // Hovering a circle selects it, exactly like the arrow keys: the pick is shown at once and C++ is
    // told (`osu menu <i>`) so enter / the next frame agree. Sent only when the hovered circle changes.
    var osuMenuHover = -1;
    function osuMenuPick(n, cxs, cy, ds) {
      var hov = -1;
      for (var i = 0; i < n; i++) {
        var r = ds[i] / 2;
        if (osuOver(dX(cxs[i] - r), dY(cy - r), dS(2 * r), dS(2 * r))) hov = i;
      }
      if (hov >= 0 && hov !== osuMenuHover && hov !== (osuF.menu || 0)) osuCmd('menu ' + hov);
      osuMenuHover = hov;
      return hov >= 0 ? hov : (osuF.menu || 0);
    }
    function osuEaseD(c, target, dt) { c.d = c.d > 0 ? c.d + (target - c.d) * Math.min(1, (dt || 0) * 14) : target; return c.d; }

    function osuDrawPause(t, dt, sz) {
      osuLay(sz);
      var song = (osuF.artist || '') + '  \u00b7  ' + (osuF.title || '') + '  [' + (osuF.version || '') + ']';
      osuDText(osuPsSong, 0, 150, 1920, 26, song.toUpperCase(), 20, '#ffffff80', 4);
      osuDText(osuPsTitle, 0, 172, 1920, 190, 'paused', 160, '#ffffff', -2);
      var sel = osuMenuPick(3, [660, 960, 1260], 640, [osuPsBtns[0].d || 210, osuPsBtns[1].d || 210, osuPsBtns[2].d || 210]);
      for (var i = 0; i < 3; i++) {
        var P = OSU_PAUSE[i], on = i === sel, d = osuEaseD(osuPsBtns[i], on ? 250 : 210, dt);
        osuHCDraw(osuPsBtns[i], t, { cx: 660 + i * 300, cy: 640, d: d, on: on, col: P[1], label: P[0], ls: on ? 40 : 36,
          key: P[3], kx: 'c', ky: d + 22, cmd: 'act ' + P[2] });
      }
      osuDText(osuPsHint, 0, 988, 1920, 26, 'left / right + enter, or click', 16, '#ffffff59', 0);
    }

    function osuDrawFail(t, dt, sz) {
      osuLay(sz);
      // The huge FAILED behind everything; clipping off screen is intended.
      osuDText(osuFlBig, -30, 590, 2800, 560, 'FAILED', 520, '#ff6b8112', -10);
      // The empty health bar across the top: a full-width track, a 22px red stub.
      osuBox(osuFlTrack, 0, 0, sz.w, Math.max(2, dS(10)));
      osuBox(osuFlStub, 0, 0, dS(22), Math.max(2, dS(10)));
      var song = (osuF.artist || '') + ' - ' + (osuF.title || '') + '  [' + (osuF.version || '') + ']';
      osuDText(osuFlWhy, 160, 220, 980, 26, (OSU_WHY[osuF.why] || 'failed').toUpperCase(), 20, '#ff8fa0', 4);
      osuDText(osuFlTitle, 154, 250, 980, 180, 'failed', 160, '#ff6b81', -2);
      osuDText(osuFlSong, 160, 426, 980, 32, song, 24, '#ffffffcc', 0);
      // Timeline: where in the song it ended.
      var len = Math.max(1, osuF.len || 1), died = Math.max(0, Math.min(len, osuF.tPlay || 0)), fr = died / len;
      osuDText(osuFlDied, 160, 512, 500, 22, 'died at ' + osuMmss(died / 1000), 16, '#ffffff80', 0);
      osuDText(osuFlLen, 640, 512, 500, 22, osuMmss(len / 1000), 16, '#ffffff80', 0);
      osuBox(osuFlTl, dX(160), dY(546), dS(980), dS(8)); oS(osuFlTl, 'borderRadius', oPx(dS(4)));
      osuBox(osuFlTlFill, dX(160), dY(546), dS(980 * fr), dS(8)); oS(osuFlTlFill, 'borderRadius', oPx(dS(4)));
      osuDiscAt(osuFlMark, dX(160 + 980 * fr), dY(550), dS(24));
      oS(osuFlMark, 'border', oPx(Math.max(1, dS(4))) + ' solid #1a0610');
      // Stats: score, accuracy, max combo in three columns.
      var stats = [['SCORE', osuScoreStr(osuF.score || 0)], ['ACCURACY', ((osuF.acc || 0) * 100).toFixed(2) + '%'], ['MAX COMBO', osuNum(osuF.maxCombo || 0) + 'x']];
      var cw = (980 - 48) / 3;
      for (var s2 = 0; s2 < 3; s2++) {
        osuDText(osuFlStats[s2].cap, 160 + s2 * (cw + 24), 607, cw, 20, stats[s2][0], 14, '#ffffff66', 2);
        osuDText(osuFlStats[s2].val, 160 + s2 * (cw + 24), 630, cw, 50, stats[s2][1], 40, '#ffffff', 0);
      }
      var sel = osuMenuPick(2, [1430, 1720], 560, [osuFlBtns[0].d || 200, osuFlBtns[1].d || 200]);
      for (var i = 0; i < 2; i++) {
        var F = OSU_FAIL[i], on = i === sel, d = osuEaseD(osuFlBtns[i], on ? 240 : 200, dt);
        osuHCDraw(osuFlBtns[i], t, { cx: F[4], cy: 560, d: d, on: on, col: F[1], label: F[0], ls: F[5],
          key: F[3], kx: 'c', ky: d + 22, cmd: 'act ' + F[2] });
      }
    }
)EGJS"
// Split: MSVC caps one raw string literal at ~16 KB (C2026); adjacent literals concatenate.
R"EGJS(
    // ---- results ----
    var osuResGrade = osuCentered(osuResS, '#ffd36b', 160, '#00000066');
    var osuResCard = osuPanel(osuResS, '#0c0816e6');
    osuResCard.style.border = '1px solid #ffffff22';
    osuResCard.style.borderRadius = '18px';
    var osuResStar = osuCentered(osuResCard, '#000000', 16, '#ffffff');
    var osuResVer = osuText(osuResCard, '', '#ffffff', 20, 'bold');
    var osuResTitle = osuText(osuResCard, '', '#ffffff', 36, 'bold');
    var osuResArtist = osuText(osuResCard, '', '#ffffffd0', 22, 'bold');
    var osuResBy = osuText(osuResCard, '', '#ffffff8c', 16, 'bold');
    var osuResMods = osuPillPool(osuResCard, 8, 13);
    var osuResScore = osuText(osuResCard, '', '#ffffff', 58, 'bold');
    osuResScore.style.letterSpacing = '3px';
    var osuResStats = [];
    for (var ors = 0; ors < 6; ors++) {
      var sp = osuPanel(osuResCard, '#ffffff10');
      sp.style.borderRadius = '12px';
      osuResStats.push({ p: sp, cap: osuText(sp, '', '#ffffff80', 14, 'bold'), val: osuText(sp, '', '#ffffff', 26, 'bold') });
    }
    var osuResBtns = osuMenu(osuResS, 2);
    var OSU_RES = [['retry', '#f7a933', 'retry'], ['back to song select', '#ff5fc8', 'quit']];
    var OSU_GRADE = { SS: ['SS', '#ffd36b'], SSH: ['SS', '#e9edf5'], S: ['S', '#ffd36b'], SH: ['S', '#e9edf5'],
      A: ['A', '#7ddf3a'], B: ['B', '#4fb6ff'], C: ['C', '#c38bff'], D: ['D', '#ff5a6e'] };

    function osuDrawResults(sz) {
      var W = sz.w, H = sz.h;
      var g = OSU_GRADE[osuF.grade] || OSU_GRADE.D;
      var gd = Math.min(H * 0.42, W * 0.28), gx = W * 0.25 - gd / 2, gy = H * 0.44 - gd / 2;
      osuBox(osuResGrade.p, gx, gy, gd, gd);
      oS(osuResGrade.p, 'borderRadius', oPx(gd / 2));
      oS(osuResGrade.p, 'border', oPx(Math.max(6, gd * 0.045)) + ' solid ' + g[1]);
      oS(osuResGrade.l, 'color', g[1]);
      oS(osuResGrade.l, 'fontSize', Math.round(gd * 0.48) + 'px');
      oT(osuResGrade.l, g[0]);
      var cw = Math.min(W * 0.46, 860), cx = W * 0.5, ch = 496, cy = Math.max(40, (H - ch - 86) / 2);
      osuInfoCard(osuResCard, osuResStar, osuResVer, osuResTitle, osuResArtist, osuResBy, osuResMods, cx, cy, cw, ch);
      oT(osuResScore, osuScoreStr(osuF.score || 0)); osuSetLabel(osuResScore, 26, 222, cw - 52);
      var stats = [
        ['accuracy', ((osuF.acc || 0) * 100).toFixed(2) + '%', '#ffffff'],
        ['max combo', osuNum(osuF.maxCombo || 0) + 'x', '#ffffff'],
        ['300', osuNum(osuF.n300 || 0), '#66ccff'],
        ['100', osuNum(osuF.n100 || 0), '#7dff8c'],
        ['50', osuNum(osuF.n50 || 0), '#ffd36b'],
        ['miss', osuNum(osuF.nMiss || 0), '#ff4d6d']];
      var sw = (cw - 52 - 2 * 12) / 3, sh = 70;
      for (var i = 0; i < stats.length; i++) {
        var S = osuResStats[i], sx = 26 + (i % 3) * (sw + 12), sy = 306 + Math.floor(i / 3) * (sh + 12);
        osuBox(S.p, sx, sy, sw, sh);
        oT(S.cap, stats[i][0]); osuSetLabel(S.cap, 14, 8, sw - 24);
        oT(S.val, stats[i][1]); osuSetLabel(S.val, 14, 28, sw - 24);
        oS(S.val, 'color', stats[i][2]);
      }
      var by = cy + ch + 24, bw = (cw - 20) / 2;
      for (var b = 0; b < 2; b++) {
        osuButton(osuResBtns[b], cx + b * (bw + 20), by, bw, 62, OSU_RES[b][0], OSU_RES[b][1], (osuF.menu || 0) === b, 'act ' + OSU_RES[b][2]);
      }
    }

    function osuDrawScreens(mode, t, dt, sz) {
      oV(osuLoadS, mode === 'loading');
      oV(osuPauseS, mode === 'pause');
      oV(osuFailS, mode === 'fail');
      oV(osuResS, mode === 'results');
      if (mode === 'loading') osuDrawLoading(t, sz);
      else if (mode === 'pause') osuDrawPause(t, dt, sz);
      else if (mode === 'fail') osuDrawFail(t, dt, sz);
      else if (mode === 'results') osuDrawResults(sz);
    }
)EGJS"
