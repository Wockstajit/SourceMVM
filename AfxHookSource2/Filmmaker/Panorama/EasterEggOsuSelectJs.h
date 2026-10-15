#pragma once

// osu! song select + mod select (part of EasterEggOsuJs.h; shares the overlay scope).
// Laid out like osu!: beatmap info wedge top left, the carousel on the right (sets; the selected
// set expands into its difficulties, coloured by star rating), search across the top, footer with
// back / mods / random and the big play button. C++ owns the model (OsuGame.cpp BuildView) and
// sends only the window of rows around the selection, each with its offset `p` from it; rows ease
// between positions keyed by set:diff so the list glides instead of jumping.

R"EGJS(
    var OSU_TOP = 72, OSU_FOOT = 66, OSU_SET_H = 86, OSU_DIFF_H = 58, OSU_GAP = 8;
    var osuRowsLayer = osuFull(osuSelect);
    var osuChrome = osuFull(osuSelect);
    var osuModsLayer = osuFull(osuSelect);

    // ---- carousel rows ----
    var osuRows = [], osuRowY = {}; // see osuRowA below
    function osuRowRec(i) {
      while (osuRows.length <= i) {
        var p = osuPanel(osuRowsLayer, '#1e1430ee');
        var r = { p: p };
        r.stripe = osuPanel(p, '#ff8fd8');
        r.title = osuText(p, '', '#ffffff', 22, 'bold');
        r.sub = osuText(p, '', '#ffffffb3', 15, 'bold');
        r.pill = osuCentered(p, '#000000', 15, '#ffffff');
        r.pips = [];
        for (var k = 0; k < 18; k++) { var d = osuDisc(p, 12, '#ffffff', 0, ''); d.visible = false; r.pips.push(d); }
        r.p.visible = false;
        osuRows.push(r);
      }
      return osuRows[i];
    }
    // Row animation state, keyed "set:diff": eased y and opacity. A set's difficulties slide out
    // from under its title when it is selected (start at the set row, fade in) and slide back in
    // when it is left (kept as ghosts until they have folded away), like osu!'s carousel.
    var osuRowA = {}, osuRowLast = {}, osuGhosts = [];
    function osuPaintRow(r, R, y, h, alpha, W, H, mid, live) {
      var isSet = R.k === 'set';
      var dy = (y + h / 2 - mid) / (H * 0.5);
      var x = W * 0.47 + dy * dy * 110 + (isSet ? 0 : 34) - (R.sel ? 26 : 0) + (isSet ? 0 : (1 - alpha) * 60);
      var w = W - x + 40, visW = W - x;
      var over = live && osuOver(x, y, visW, h);
      osuBox(r.p, x, y, w, h);
      oS(r.p, 'zIndex', isSet ? '2' : '1'); // difficulties pass UNDER the set rows
      oS(r.p, 'opacity', alpha.toFixed(3));
      oS(r.p, 'borderRadius', '12px');
      osuBox(r.stripe, 0, 0, 7, h);
      oS(r.stripe, 'borderRadius', '12px 0px 0px 12px');
      if (isSet) {
        var top = R.st && R.st.length ? R.st[R.st.length - 1] : 0;
        oS(r.p, 'backgroundColor', R.sel ? '#3b1f4ef5' : (over ? '#2c1d42f0' : '#1c1330e8'));
        oS(r.p, 'border', R.sel ? '2px solid #ff8fd8ff' : '1px solid #ffffff1c');
        oS(r.stripe, 'backgroundColor', osuStarCol(top));
        oT(r.title, R.t || '');
        oS(r.title, 'marginLeft', '24px'); oS(r.title, 'marginTop', '9px'); oS(r.title, 'fontSize', '22px');
        oS(r.title, 'width', oPx(Math.max(60, visW - 48)));
        oT(r.sub, (R.a || '') + '  //  ' + (R.c || ''));
        oS(r.sub, 'marginLeft', '24px'); oS(r.sub, 'marginTop', '38px');
        oS(r.sub, 'width', oPx(Math.max(60, visW - 48)));
        oV(r.pill.p, false);
        var st = R.st || [];
        for (var k = 0; k < r.pips.length; k++) {
          var pd = r.pips[k];
          if (k >= st.length) { oV(pd, false); continue; }
          oS(pd, 'backgroundColor', osuStarCol(st[k]));
          osuAt(pd, 24 + k * 17, 64);
          oV(pd, true);
        }
        if (live) osuHit(x, y, visW, h, 'pick ' + R.s + ' -1');
      } else {
        var sc = osuStarCol(R.star);
        oS(r.p, 'backgroundColor', osuMix('#120c1c', sc, R.sel ? 0.42 : (over ? 0.3 : 0.2)) + 'f0');
        oS(r.p, 'border', R.sel ? '2px solid #ffffffff' : '1px solid ' + sc + '66');
        oS(r.stripe, 'backgroundColor', sc);
        osuPill(r.pill, 22, 15, 74, 28, osuStarStr(R.star), sc, osuStarInk(R.star));
        oT(r.title, R.v || '');
        oS(r.title, 'marginLeft', '110px'); oS(r.title, 'marginTop', '6px'); oS(r.title, 'fontSize', '20px');
        oS(r.title, 'width', oPx(Math.max(60, visW - 130)));
        oT(r.sub, 'mapped by ' + (R.c || ''));
        oS(r.sub, 'marginLeft', '110px'); oS(r.sub, 'marginTop', '32px');
        oS(r.sub, 'width', oPx(Math.max(60, visW - 130)));
        for (var q = 0; q < r.pips.length; q++) oV(r.pips[q], false);
        if (live && alpha > 0.5) osuHit(x, y, visW, h, 'pick ' + R.s + ' ' + R.d);
      }
      oV(r.p, true);
    }

    function osuDrawRows(t, dt, sz) {
      var rows = osuV.rows || [], W = sz.w, H = sz.h;
      var mid = (OSU_TOP + H - OSU_FOOT) * 0.5;
      var hs = [], ys = [], selAt = -1, setY = {};
      for (var i = 0; i < rows.length; i++) { hs.push(rows[i].k === 'set' ? OSU_SET_H : OSU_DIFF_H); if (rows[i].p === 0) selAt = i; }
      if (selAt < 0) selAt = 0;
      for (var a = 0; a < rows.length; a++) ys.push(0);
      if (rows.length) ys[selAt] = mid - hs[selAt] / 2;
      for (var b = selAt - 1; b >= 0; b--) ys[b] = ys[b + 1] - OSU_GAP - hs[b];
      for (var c = selAt + 1; c < rows.length; c++) ys[c] = ys[c - 1] + hs[c - 1] + OSU_GAP;
      var ease = Math.min(1, dt * 12), fade = Math.min(1, dt * 9), seen = {};
      // Set rows first: new difficulties start from where their set row is right now.
      for (var s0 = 0; s0 < rows.length; s0++) {
        if (rows[s0].k !== 'set') continue;
        var sk = rows[s0].s + ':-1';
        var sy = osuRowY[sk];
        setY[rows[s0].s] = sy === undefined ? ys[s0] : sy;
      }
      var used = 0;
      for (var j = 0; j < rows.length; j++) {
        var R = rows[j], key = R.s + ':' + R.d, isSet = R.k === 'set';
        seen[key] = 1;
        var y = osuRowY[key], al = osuRowA[key];
        if (y === undefined) {
          y = isSet ? ys[j] : (setY[R.s] !== undefined ? setY[R.s] : ys[j]);
          al = isSet ? 1 : 0;
        }
        y += (ys[j] - y) * ease;
        al += (1 - al) * fade;
        osuRowY[key] = y; osuRowA[key] = al; osuRowLast[key] = R;
        var h = hs[j];
        if (y + h < OSU_TOP - 20 || y > H - OSU_FOOT + 20) continue;
        osuPaintRow(osuRowRec(used++), R, y, h, al, W, H, mid, true);
      }
      // Difficulties that just left (their set was deselected): fold back up under the set.
      for (var kk in osuRowY) {
        if (seen[kk]) continue;
        var L = osuRowLast[kk];
        if (L && L.k === 'diff') osuGhosts.push({ R: L, y: osuRowY[kk], a: osuRowA[kk] || 1, s: L.s });
        delete osuRowY[kk]; delete osuRowA[kk]; delete osuRowLast[kk];
      }
      var keep = [];
      for (var g = 0; g < osuGhosts.length; g++) {
        var G = osuGhosts[g];
        var ty = osuRowY[G.s + ':-1'];
        if (ty !== undefined) G.y += (ty - G.y) * ease;
        G.a -= Math.min(G.a, dt * 6);
        if (G.a <= 0.02) continue;
        keep.push(G);
        var gr = Object.assign({}, G.R); gr.sel = false;
        if (G.y + OSU_DIFF_H < OSU_TOP - 20 || G.y > H - OSU_FOOT + 20) continue;
        osuPaintRow(osuRowRec(used++), gr, G.y, OSU_DIFF_H, G.a, W, H, mid, false);
      }
      osuGhosts = keep.length > 24 ? keep.slice(keep.length - 24) : keep;
      for (var z = used; z < osuRows.length; z++) oV(osuRows[z].p, false);
    }
)EGJS"
// Split: MSVC caps one raw string literal at ~16 KB (C2026); adjacent literals concatenate.
R"EGJS(
    // ---- info wedge ----
    var osuInfo = osuPanel(osuChrome, '#0c0816e0');
    osuInfo.style.border = '1px solid #ffffff1f';
    osuInfo.style.borderRadius = '16px';
    var osuInfoStar = osuCentered(osuInfo, '#000000', 16, '#ffffff');
    var osuInfoVer = osuText(osuInfo, '', '#ffffff', 20, 'bold');
    var osuInfoTitle = osuText(osuInfo, '', '#ffffff', 34, 'bold');
    var osuInfoArtist = osuText(osuInfo, '', '#ffffffd0', 22, 'bold');
    var osuInfoBy = osuText(osuInfo, '', '#ffffff8c', 16, 'bold');
    var osuInfoChips = [];
    for (var oci = 0; oci < 4; oci++) {
      var chip = osuPanel(osuInfo, '#ffffff10');
      chip.style.borderRadius = '10px';
      osuInfoChips.push({ p: chip, cap: osuText(chip, '', '#ffffff80', 13, 'bold'), val: osuText(chip, '', '#ffffff', 20, 'bold') });
    }
    var osuInfoBars = [];
    for (var obi = 0; obi < 4; obi++) {
      var cap = osuText(osuInfo, '', '#ffffffb3', 15, 'bold');
      var back = osuPanel(osuInfo, '#ffffff1a'); back.style.borderRadius = '4px';
      var fill = osuPanel(back, '#ff8fd8'); fill.style.borderRadius = '4px';
      var base = osuPanel(back, '#ffffff55'); base.style.borderRadius = '4px';
      var val = osuAligned(osuInfo, '#ffffff', 16, 'right', 'center');
      osuInfoBars.push({ cap: cap, back: back, fill: fill, base: base, val: val });
    }
    function osuSetLabel(l, x, y, w) { oS(l, 'marginLeft', oPx(x)); oS(l, 'marginTop', oPx(y)); if (w) oS(l, 'width', oPx(w)); }

    function osuDrawInfo(sz) {
      var I = osuV.info;
      oV(osuInfo, !!I);
      if (!I) return;
      var x = 26, y = OSU_TOP + 20, w = Math.min(sz.w * 0.42, 760), h = 386, iw = w - 44;
      osuBox(osuInfo, x, y, w, h);
      var sc = osuStarCol(I.stars);
      osuPill(osuInfoStar, 22, 20, 86, 30, osuStarStr(I.stars) + ' *', sc, osuStarInk(I.stars));
      oT(osuInfoVer, I.version || ''); osuSetLabel(osuInfoVer, 122, 21, iw - 110);
      oT(osuInfoTitle, I.title || ''); osuSetLabel(osuInfoTitle, 22, 62, iw);
      oT(osuInfoArtist, I.artist || ''); osuSetLabel(osuInfoArtist, 22, 106, iw);
      oT(osuInfoBy, 'mapped by ' + (I.mapper || '') + '   |   ' + (I.src || '') + (I.from ? '   |   ' + I.from : ''));
      osuSetLabel(osuInfoBy, 22, 140, iw);
      var bpm = Math.round(I.bpm || 0);
      var bpmStr = (Math.round(I.bpmMin) !== Math.round(I.bpmMax) && I.bpmMax > 0)
        ? Math.round(I.bpmMin) + '-' + Math.round(I.bpmMax) + ' (' + bpm + ')' : String(bpm);
      var chips = [['length', osuMmss(I.len || 0)], ['bpm', bpmStr], ['circles', osuNum(I.circles || 0)], ['sliders', osuNum(I.sliders || 0)]];
      var cw = (iw - 3 * 10) / 4;
      for (var i = 0; i < 4; i++) {
        var C = osuInfoChips[i];
        osuBox(C.p, 22 + i * (cw + 10), 178, cw, 54);
        oT(C.cap, chips[i][0]); osuSetLabel(C.cap, 12, 6, cw - 20);
        oT(C.val, chips[i][1]); osuSetLabel(C.val, 12, 24, cw - 20);
      }
      var attrs = [['CS', I.cs, I.cs0], ['HP', I.hp, I.hp0], ['OD', I.od, I.od0], ['AR', I.ar, I.ar0]];
      var bw = iw - 110;
      for (var k = 0; k < 4; k++) {
        var A = osuInfoBars[k], yy = 254 + k * 30, v = attrs[k][1] || 0, v0 = attrs[k][2] || 0;
        oT(A.cap, attrs[k][0]); osuSetLabel(A.cap, 22, yy, 44);
        osuBox(A.back, 72, yy + 7, bw, 8);
        var lo = Math.min(v, v0), hi = Math.max(v, v0);
        osuBox(A.base, 0, 0, bw * osuClamp(lo / 10), 8);
        osuBox(A.fill, 0, 0, bw * osuClamp(hi / 10), 8);
        // Mod-raised values show the gain in red, lowered ones the loss in green.
        oS(A.fill, 'backgroundColor', v > v0 + 0.01 ? '#ff6b6b' : (v < v0 - 0.01 ? '#7dff8c' : '#ffffffd9'));
        oS(A.base, 'backgroundColor', '#ffffffd9');
        osuBox(A.val.p, 72 + bw + 4, yy - 2, 30 + 4, 24);
        oS(A.val.l, 'color', v > v0 + 0.01 ? '#ff8f8f' : (v < v0 - 0.01 ? '#9dffaa' : '#ffffff'));
        oT(A.val.l, (Math.round(v * 10) / 10).toString());
      }
    }

    // ---- top bar + footer ----
    var osuTopBar = osuPanel(osuChrome, '#0a0612ff');
    osuTopBar.style.borderBottom = '2px solid #ff8fd866';
    var osuLogo = osuAligned(osuTopBar, '#ff5fc8', 34, 'left', 'center');
    var osuLogoSub = osuAligned(osuTopBar, '#ffffff99', 15, 'left', 'center');
    var osuSearch = osuPanel(osuTopBar, '#00000099');
    osuSearch.style.border = '1px solid #ffffff33';
    var osuSearchT = osuAligned(osuSearch, '#ffffff', 19, 'left', 'center');
    var osuSearchN = osuAligned(osuSearch, '#ffffff80', 14, 'right', 'center');
    var osuSortB = osuMakeButton(osuTopBar, 15);
    var osuScan = osuPanel(osuChrome, '#9fe6ff');
    var osuEmpty = osuCentered(osuChrome, '#ffffffd9', 20);
    // The footer (EasterEggOsuMenusJs.h osuDrawFooter) builds its widgets in osuChrome too.
)EGJS"
// Split: MSVC caps one raw string literal at ~16 KB (C2026); adjacent literals concatenate.
R"EGJS(
    function osuDrawChrome(t, sz) {
      var W = sz.w, H = sz.h;
      osuBox(osuTopBar, 0, 0, W, OSU_TOP);
      osuBox(osuLogo.p, 28, 0, 120, OSU_TOP); oT(osuLogo.l, 'osu!');
      osuBox(osuLogoSub.p, 112, 0, 260, OSU_TOP); oT(osuLogoSub.l, 'song select   //   ' + osuNum(osuV.sets || 0) + ' sets');
      var sx = W * 0.5, sw = W * 0.5 - 24;
      osuBox(osuSearch, sx, 14, sw, 44);
      oS(osuSearch, 'borderRadius', '22px');
      var q = osuV.search || '';
      var caret = Math.floor(t * 2) % 2 === 0 ? '_' : ' ';
      oS(osuSearchT.l, 'color', q ? '#ffffff' : '#ffffff66');
      osuBox(osuSearchT.p, 20, 0, sw - 190, 44);
      oT(osuSearchT.l, q ? q + caret : 'type to search');
      osuBox(osuSearchN.p, sw - 180, 0, 160, 44);
      oT(osuSearchN.l, osuNum(osuV.matched || 0) + ' matches');
      osuButton(osuSortB, sx - 196, 16, 180, 40, 'sort: ' + (osuV.sort || 'title'), '#c9a7ff', false, 'act sort');
      // Scan progress: a thin line under the top bar.
      var scanning = !!osuV.scanning && (osuV.total || 0) > 0;
      oV(osuScan, scanning);
      if (scanning) osuBox(osuScan, 0, OSU_TOP - 3, W * osuClamp((osuV.done || 0) / osuV.total), 3);
      // Nothing to show.
      var msg = '';
      if (!(osuV.sets > 0)) msg = osuV.scanning ? 'finding beatmaps...  ' + osuNum(osuV.done || 0) + ' / ' + osuNum(osuV.total || 0)
        : 'no beatmaps found.\ninstall osu! (stable or lazer), or drop .osz files into %APPDATA%\\HLAE\\osu';
      else if (!(osuV.matched > 0)) msg = 'no matches for "' + q + '"\nbackspace to edit, delete to clear';
      oV(osuEmpty.p, !!msg);
      if (msg) { osuBox(osuEmpty.p, W * 0.47, H * 0.42, W * 0.5, 90); oT(osuEmpty.l, msg); try { osuEmpty.l.style.whiteSpace = 'normal'; } catch (e) {} }

      osuDrawFooter(t, sz);
    }
    // Mods and options TAKE OVER the screen (UI pass): the carousel, info card, top bar and footer are
    // hidden -- not dimmed -- while either is open, so nothing shows through or competes for clicks.
    function osuDrawSelect(t, dt, sz) {
      var take = !!osuV.modsOpen || !!(osuV.opt && osuV.opt.open);
      oV(osuRowsLayer, !take);
      oV(osuChrome, !take);
      if (!take) {
        osuDrawRows(t, dt, sz);
        osuDrawInfo(sz);
        osuDrawChrome(t, sz);
      }
      osuDrawMods(t, sz);
      osuDrawOptions(t, sz);
    }
)EGJS"
