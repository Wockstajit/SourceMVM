#pragma once

// OSU fragment of the easter-egg overlay (concatenated into kEasterEggOverlayJs, shares its scope;
// uses lbl()/layer()/cmd() from EasterEggOverlayJs.h). Draws the osu!-style rhythm game whose rules
// run in C++ (Filmmaker/EasterEggs/Osu/OsuGame.cpp). Three attributes on #MvmEggRoot feed it:
//   osuframe  every frame: mode, song time, cursor, score/combo/acc/hp, popups, spinner, clicks
//   osuobjs   the hit objects in view (+ slider paths, x10 ints), only when that set changes
//   osulist   the song-select view (carousel window, info, mods), only when it changes
// Clicks never reach these panels (the game owns the mouse); C++ forwards each click as a
// normalised point and this script hit-tests the rectangles it drew last frame (osuHits).
// Coordinates are osu! pixels on the 512x384 playfield, placed exactly like OsuMath.h's
// PlayfieldFor(): 80% of the screen height, centred, nudged down 2%. Slider path lookup mirrors
// OsuMath.h's PathAt() (even 6 px steps, a shorter last step) so the ball sits where C++ judges it.
//
// Fragments: this core + EasterEggOsuPlayJs.h (gameplay) + EasterEggOsuSelectJs.h (song select) +
// EasterEggOsuMenusJs.h (song-select footer, mod select + options takeovers) +
// EasterEggOsuScreensJs.h (loading, pause, fail, results).

R"EGJS(
    // ================= osu! core =================
    var osuLayer = layer('MvmEggOsu', '35');
    osuLayer.visible = false;
    var OSU_STEP = 6.0;
    // Style writes are cached per panel: the whole UI is re-laid every frame.
    var osuCache = new Map();
    function osuC(p) { var c = osuCache.get(p); if (!c) { c = {}; osuCache.set(p, c); } return c; }
    function oS(p, k, v) { var c = osuC(p); if (c[k] !== v) { c[k] = v; p.style[k] = v; } }
    function oT(l, s) { var c = osuC(l); if (c.$t !== s) { c.$t = s; l.text = s; } }
    function oV(p, on) { var c = osuC(p); if (c.$v !== on) { c.$v = on; p.visible = on; } }
    function oPx(v) { return v.toFixed(1) + 'px'; }
    function osuClamp(v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
    function osuAt(p, x, y) { oS(p, 'position', oPx(x) + ' ' + oPx(y) + ' 0px'); }
    function osuBox(p, x, y, w, h) { osuAt(p, x, y); oS(p, 'width', oPx(w)); oS(p, 'height', oPx(h)); }
    function osuRound(p, d) { oS(p, 'width', oPx(d)); oS(p, 'height', oPx(d)); oS(p, 'borderRadius', oPx(d / 2)); }
    function osuPanel(parent, bg) {
      var p = $.CreatePanel('Panel', parent, '');
      p.hittest = false;
      p.style.horizontalAlign = 'left'; p.style.verticalAlign = 'top';
      p.style.overflow = 'noclip noclip';
      if (bg) p.style.backgroundColor = bg;
      return p;
    }
    // Every drawing layer is FULL SCREEN: a small container with overflow noclip still has its
    // out-of-bounds children culled in game.
    function osuFull(parent) { var p = osuPanel(parent, null); p.style.width = '100%'; p.style.height = '100%'; return p; }
    function osuText(parent, text, color, size, weight) {
      var l = lbl(parent, text, color, size);
      l.style.fontWeight = weight || 'bold';
      try { l.style.whiteSpace = 'nowrap'; l.style.textOverflow = 'ellipsis'; } catch (e) {}
      return l;
    }
    // A label centred in a sized box of its own: positioned bare Labels render low/left in game.
    function osuCentered(parent, color, size, bg) {
      var b = osuPanel(parent, bg || null);
      var l = osuText(b, '', color, size, 'bold');
      l.style.horizontalAlign = 'center'; l.style.verticalAlign = 'center';
      return { p: b, l: l };
    }
    function osuAligned(parent, color, size, h, v) {
      var b = osuPanel(parent, null);
      var l = osuText(b, '', color, size, 'bold');
      l.style.horizontalAlign = h; l.style.verticalAlign = v;
      return { p: b, l: l };
    }
    function osuDisc(parent, d, fill, border, borderCol) {
      var p = osuPanel(parent, fill);
      p.style.width = d + 'px'; p.style.height = d + 'px'; p.style.borderRadius = (d / 2) + 'px';
      p.style.transformOrigin = '50% 50%';
      if (border) p.style.border = border + 'px solid ' + borderCol;
      return p;
    }
    function osuDiscAt(p, x, y, d) { osuRound(p, d); osuAt(p, x - d / 2, y - d / 2); }

    // ---- colour ----
    function osuHex2(v) { v = Math.max(0, Math.min(255, Math.round(v))); return (v < 16 ? '0' : '') + v.toString(16); }
    function osuRgb(h) { return [parseInt(h.substr(1, 2), 16), parseInt(h.substr(3, 2), 16), parseInt(h.substr(5, 2), 16)]; }
    function osuMix(a, b, u) {
      var A = osuRgb(a), B = osuRgb(b);
      return '#' + osuHex2(A[0] + (B[0] - A[0]) * u) + osuHex2(A[1] + (B[1] - A[1]) * u) + osuHex2(A[2] + (B[2] - A[2]) * u);
    }
    // osu!'s star-rating spectrum (osu.Game OsuColour.ForStarDifficulty), sampled linearly.
    var OSU_SPEC = [[0.1, '#aaaaaa'], [0.1, '#4290fb'], [1.25, '#4fc0ff'], [2.0, '#4fffd5'], [2.5, '#7cff4f'],
      [3.3, '#f6f05c'], [4.2, '#ff8068'], [4.9, '#ff4e6f'], [5.8, '#c645b8'], [6.7, '#6563de'], [7.7, '#18158e'], [9.0, '#000000']];
    function osuStarCol(s) {
      if (!(s > 0)) return '#aaaaaa';
      s = Math.round(s * 100) / 100;
      if (s < 0.1) return '#aaaaaa';
      for (var i = 1; i < OSU_SPEC.length; i++) {
        if (s <= OSU_SPEC[i][0]) {
          var a = OSU_SPEC[i - 1], b = OSU_SPEC[i];
          return osuMix(a[1], b[1], b[0] > a[0] ? (s - a[0]) / (b[0] - a[0]) : 1);
        }
      }
      return '#000000';
    }
    function osuStarInk(s) { return s >= 6.5 ? '#ffd966' : '#000000c8'; }
    function osuStarStr(s) { return s > 0 ? s.toFixed(2) : '?'; }

    // ---- numbers ----
    function osuPad(n, w) { var s = '' + n; while (s.length < w) s = '0' + s; return s; }
    function osuMmss(sec) { sec = Math.max(0, Math.round(sec)); return Math.floor(sec / 60) + ':' + osuPad(sec % 60, 2); }
    function osuNum(n) { var s = '' + Math.round(n), o = ''; while (s.length > 3) { o = ',' + s.substr(s.length - 3) + o; s = s.substr(0, s.length - 3); } return s + o; }
    function osuScoreStr(n) { var s = osuPad(Math.max(0, Math.round(n)), 8); return s.substr(0, 2) + ',' + s.substr(2, 3) + ',' + s.substr(5, 3); }

    // ---- state ----
    var osuF = {}, osuO = { cols: [], o: [] }, osuV = { rows: [], mods: [], modTable: [] };
    var osuFrameStr = '', osuObjsStr = '', osuViewStr = '';
    var osuPF = { s: 1, ox: 0, oy: 0 };
    var osuMouse = { x: -1, y: -1 }, osuHits = [], osuClickSeen = -1, osuWasMode = 'closed';
    function osuOver(x, y, w, h) { return osuMouse.x >= x && osuMouse.x <= x + w && osuMouse.y >= y && osuMouse.y <= y + h; }
    // A clickable rectangle for this frame; later ones win. '' swallows the click.
    function osuHit(x, y, w, h, c) { osuHits.push([x, y, x + w, y + h, c]); }
    function osuCmd(c) { cmd('mirv_filmmaker osu ' + c); }

    // OsuMath.h PlayfieldFor() (osu!'s OsuPlayfieldAdjustmentContainer): 80% of the largest 4:3
    // area, centred, shifted down 8 osu px. C++ maps the cursor with the same numbers.
    function osuPlace(sz) {
      var s = Math.min(sz.w, sz.h * 4 / 3) * 0.8 / 512;
      osuPF = { s: s, ox: (sz.w - 512 * s) * 0.5, oy: (sz.h - 384 * s) * 0.5 + 8 * s };
    }
    function osuX(x) { return osuPF.ox + x * osuPF.s; }
    function osuY(y) { return osuPF.oy + y * osuPF.s; }

    // OsuMath.h PathAt(): points every OSU_STEP px except a shorter last segment.
    function osuPathAt(p, frac) {
      var n = p.length / 2;
      if (n <= 0) return [0, 0];
      if (n === 1) return [p[0] / 10, p[1] / 10];
      frac = osuClamp(frac);
      var lx = (p[2 * n - 2] - p[2 * n - 4]) / 10, ly = (p[2 * n - 1] - p[2 * n - 3]) / 10;
      var last = Math.sqrt(lx * lx + ly * ly);
      var total = (n - 2) * OSU_STEP + last;
      var d = frac * total;
      var i = Math.floor(d / OSU_STEP);
      if (i >= n - 1) i = n - 2;
      var seg = i === n - 2 ? last : OSU_STEP;
      var u = seg > 1e-9 ? osuClamp((d - i * OSU_STEP) / seg) : 0;
      return [(p[2 * i] * (1 - u) + p[2 * i + 2] * u) / 10, (p[2 * i + 1] * (1 - u) + p[2 * i + 3] * u) / 10];
    }
    function osuProgress(o, t) {
      if (!(o.sp > 0)) return 0;
      var p = Math.max(0, Math.min(o.s, (t - o.t) / o.sp));
      var pass = Math.floor(p); if (pass >= o.s) pass = o.s - 1;
      var f = p - pass;
      return pass % 2 === 1 ? 1 - f : f;
    }

    // ---- the stack (creation order = paint order) ----
    var osuBack = osuFull(osuLayer);
    osuBack.style.backgroundColor = 'gradient(linear, 0% 0%, 100% 100%, from(#2a1340), to(#07040e))';
    var osuDim = osuFull(osuLayer);
    var osuPlay = osuFull(osuLayer), osuSelect = osuFull(osuLayer), osuScreens = osuFull(osuLayer);
    var osuCursorLayer = osuFull(osuLayer);

    // A button: a sized box with its label centred inside.
    function osuMakeButton(parent, size) { return osuCentered(parent, '#ffffff', size || 20, '#ffffff14'); }
    function osuButton(b, x, y, w, h, text, col, active, c) {
      var over = osuOver(x, y, w, h);
      osuBox(b.p, x, y, w, h);
      oS(b.p, 'borderRadius', oPx(Math.min(h / 2, 12)));
      oS(b.p, 'backgroundColor', active ? col + 'ff' : (over ? osuMix(col, '#000000', 0.35) + 'f2' : osuMix(col, '#0a0612', 0.72) + 'e6'));
      oS(b.p, 'border', active || over ? '2px solid #ffffffd9' : '2px solid ' + col + '99');
      oS(b.l, 'color', active ? '#120a1c' : '#ffffff');
      oT(b.l, text);
      oV(b.p, true);
      if (c) osuHit(x, y, w, h, c);
    }
    // A pill (stars, mods): coloured box with dark or light ink.
    function osuPill(b, x, y, w, h, text, bg, ink) {
      osuBox(b.p, x, y, w, h);
      oS(b.p, 'borderRadius', oPx(h / 2));
      oS(b.p, 'backgroundColor', bg);
      oS(b.l, 'color', ink);
      oT(b.l, text);
      oV(b.p, true);
    }
    var OSU_MOD_COL = { EZ: '#b2ff66', NF: '#b2ff66', HT: '#b2ff66', HR: '#ff6b6b', SD: '#ff6b6b', PF: '#ff6b6b',
      DT: '#ff6b6b', NC: '#ff6b6b', HD: '#ff6b6b', FL: '#ff6b6b', AT: '#66ccff' };
    function osuModPills(pool, mods, x, y, h, rightAlign) {
      var w = h * 1.9, gap = 6, n = Math.min(mods.length, pool.length);
      var x0 = rightAlign ? x - n * (w + gap) + gap : x;
      for (var i = 0; i < pool.length; i++) {
        if (i >= n) { oV(pool[i].p, false); continue; }
        osuPill(pool[i], x0 + i * (w + gap), y, w, h, mods[i], OSU_MOD_COL[mods[i]] || '#ffffff', '#120a1c');
      }
    }
    function osuPillPool(parent, n, size) { var a = []; for (var i = 0; i < n; i++) { var b = osuCentered(parent, '#120a1c', size, '#ffffff'); b.p.visible = false; a.push(b); } return a; }
)EGJS"
R"EGJS(
    // ---- the player's skin (OsuSkin.cpp) ----
    // osuO.skin.el[name] = [url, width, height, ratio]: the image served over 127.0.0.1 (Panorama will
    // not show a loose PNG, but it loads http images), its pixel size and 2 for an @2x file. Sizes
    // follow lazer's LegacySkin: a sprite is its pixels / ratio in osu! units, times the object
    // scale (radius / 64) for hit objects. A 1x1 image is a skin deliberately hiding that element.
    function osuSkin() { return osuO.skin || null; }
    function osuSkinEl(k) { var s = osuO.skin; return s && s.el ? (s.el[k] || null) : null; }
    function osuElVis(e) { return !!e && e[1] > 1 && e[2] > 1; }
    function osuImg(parent) {
      var p = $.CreatePanel('Image', parent, '', { scaling: 'stretch-to-fit-preserve-aspect' });
      p.hittest = false;
      p.style.horizontalAlign = 'left'; p.style.verticalAlign = 'top';
      p.style.overflow = 'noclip noclip';
      p.style.transformOrigin = '50% 50%';
      p.visible = false;
      return p;
    }
    function osuImgSrc(p, e) { var c = osuC(p); if (c.$src !== e[0]) { c.$src = e[0]; p.SetImage(e[0]); } }
    // Draws element e centred on (x, y) at `scale` screen px per osu! unit; returns its size.
    function osuImgAt(p, e, x, y, scale) {
      var w = e[1] / e[3] * scale, h = e[2] / e[3] * scale;
      osuImgSrc(p, e);
      osuBox(p, x - w / 2, y - h / 2, w, h);
      oV(p, true);
      return [w, h];
    }
    // Multiplies the image by a colour (osu! sprite Colour): '' = untinted.
    function osuTint(p, col) { oS(p, 'washColor', col || '#ffffff'); }

    // ---- the volume overlay (osu!'s scroll-for-volume) ----
    // Same layout as OsuGame.cpp VolLayout (the wheel picks the hovered meter there).
    var osuVolLayer = osuFull(osuLayer);
    osuVolLayer.style.zIndex = '50';
    var osuVolM = [];
    for (var ovi = 0; ovi < 3; ovi++) {
      var vm = { ring: osuDisc(osuVolLayer, 10, '#0c0814e6', 4, '#ffffff40'), fill: osuDisc(osuVolLayer, 10, '#ff8fd8', 0, '') };
      vm.val = osuCentered(osuVolLayer, '#ffffff', 26);
      vm.name = osuCentered(osuVolLayer, '#ffffffcc', 15);
      osuVolM.push(vm);
    }
    var OSU_VOL_NAMES = ['master', 'music', 'effects'];
    function osuDrawVolume(sz) {
      var v = osuF.vol;
      oV(osuVolLayer, !!v);
      if (!v) return;
      var W = sz.w, H = sz.h, cx = W - 0.13 * H;
      var lay = [[0.5 * H, 0.085 * H], [0.3 * H, 0.06 * H], [0.7 * H, 0.06 * H]];
      var vals = [v.m || 0, v.mu || 0, v.e || 0];
      oS(osuVolLayer, 'opacity', osuClamp(v.a).toFixed(3));
      for (var i = 0; i < 3; i++) {
        var M = osuVolM[i], r = lay[i][1], sel = v.sel === i;
        osuDiscAt(M.ring, cx, lay[i][0], 2 * r);
        oS(M.ring, 'border', oPx(Math.max(2, r * 0.07)) + ' solid ' + (sel ? '#ff8fd8' : '#ffffff40'));
        // The level as a filled disc growing from the centre: area follows the value.
        var fd = 2 * r * 0.86 * Math.sqrt(osuClamp(vals[i]));
        osuDiscAt(M.fill, cx, lay[i][0], Math.max(1, fd));
        oS(M.fill, 'backgroundColor', sel ? '#ff8fd8b0' : '#ffffff40');
        osuBox(M.val.p, cx - r, lay[i][0] - r, 2 * r, 2 * r);
        oS(M.val.l, 'fontSize', Math.max(14, r * 0.42).toFixed(0) + 'px');
        oT(M.val.l, Math.round(vals[i] * 100) + '%');
        osuBox(M.name.p, cx - r * 1.6, lay[i][0] + r + 2, r * 3.2, 24);
        oT(M.name.l, OSU_VOL_NAMES[i]);
      }
    }
)EGJS"
R"EGJS(
    // ---- design space (the UI pass handoff: 1920x1080 reference, scaled to fit and centred) ----
    // Every value in the mod / options / failed / paused screens is a 1920x1080 pixel: u scales it
    // and (ox, oy) centre the 16:9 area when the window has another aspect.
    var osuL = { u: 1, ox: 0, oy: 0 };
    function osuLay(sz) { var u = Math.min(sz.w / 1920, sz.h / 1080); osuL = { u: u, ox: (sz.w - 1920 * u) / 2, oy: (sz.h - 1080 * u) / 2 }; }
    function dX(v) { return osuL.ox + v * osuL.u; }
    function dY(v) { return osuL.oy + v * osuL.u; }
    function dS(v) { return v * osuL.u; }
    function osuFs(l, px) { oS(l, 'fontSize', Math.max(1, Math.round(px * osuL.u)) + 'px'); }
    // A label in a box of its own (design units); align 'left' | 'center' | 'right'.
    function osuDLabel(parent, align) {
      var b = osuPanel(parent, null);
      var l = osuText(b, '', '#ffffff', 16, 'bold');
      l.style.horizontalAlign = align || 'left'; l.style.verticalAlign = 'center';
      return { p: b, l: l };
    }
    function osuDText(L, x, y, w, h, text, px, col, ls) {
      osuBox(L.p, dX(x), dY(y), dS(w), dS(h));
      osuFs(L.l, px);
      oS(L.l, 'color', col);
      oS(L.l, 'letterSpacing', oPx(dS(ls || 0)));
      oT(L.l, text);
      oV(L.p, true);
    }
    // A label whose box hugs its text, so the next item can follow it (Panorama measures it after
    // layout: the width read here is last frame's). est = the fallback width in design units.
    function osuFitLabel(parent) {
      var b = osuPanel(parent, null);
      b.style.width = 'fit-children'; b.style.height = 'fit-children';
      var l = osuText(b, '', '#ffffff', 16, 'bold');
      return { p: b, l: l };
    }
    function osuFitWidth(F, est) {
      try { var w = F.p.actuallayoutwidth / (F.p.actualuiscale_x || 1); if (w > 0) return w / osuL.u; } catch (e) {}
      return est;
    }
    // Key cap: #0a0612, 1px border with a 3px bottom, radius 6, min 30x28, 14px. `col` = active.
    function osuCap(parent) { var c = osuCentered(parent, '#ffffffb3', 14, '#0a0612'); return c; }
    function osuCapDraw(c, x, y, key, col, dark) {
      var w = Math.max(30, 14 + key.length * 9), h = 28;
      osuBox(c.p, dX(x), dY(y), dS(w), dS(h));
      oS(c.p, 'borderRadius', oPx(dS(6)));
      var bc = dark ? '#120a1c55' : (col || '#ffffff40');
      oS(c.p, 'backgroundColor', dark ? '#00000000' : '#0a0612');
      oS(c.p, 'border', oPx(Math.max(1, dS(1))) + ' solid ' + bc);
      oS(c.p, 'borderBottom', oPx(Math.max(2, dS(3))) + ' solid ' + bc);
      osuFs(c.l, 14);
      oS(c.l, 'color', dark ? '#120a1ccc' : (col || '#ffffffb3'));
      oT(c.l, key);
      oV(c.p, true);
      return w;
    }
    // The hit-circle button (mods, failed, paused). o: cx, cy, d, on, col, label, ls (label px),
    // name, ns, sub, key, kx, ky (cap offset from the circle's top-left), cmd. Active = radial fill,
    // white rim and an approach ring that eases in from 1.6d (with the body popping 0.92 -> 1).
    function osuHC(parent) {
      return { ring: osuDisc(parent, 10, '#00000000', 3, '#ffffff'), body: osuDisc(parent, 10, '#120a1c', 4, '#ffffff'),
        lab: osuCentered(parent, '#ffffff', 40), name: osuCentered(parent, '#ffffff99', 19), sub: osuCentered(parent, '#ffffff59', 15),
        cap: osuCap(parent), onAt: -1, was: false, d: 0 };
    }
    function osuHCHide(c) { oV(c.ring, false); oV(c.body, false); oV(c.lab.p, false); oV(c.name.p, false); oV(c.sub.p, false); oV(c.cap.p, false); }
    function osuHCDraw(c, t, o) {
      var d = o.d, x0 = o.cx - d / 2, y0 = o.cy - d / 2, col = o.col;
      var over = !o.on && osuOver(dX(x0), dY(y0), dS(d), dS(d));
      if (o.on && !c.was) c.onAt = t;
      c.was = o.on;
      var k = o.on && c.onAt >= 0 ? osuClamp((t - c.onAt) / 0.18) : 1, e = 1 - (1 - k) * (1 - k);
      oS(c.body, 'backgroundColor', o.on
        ? 'gradient(radial, 50% 38%, 0% 0%, 50% 50%, from(' + osuMix(col, '#ffffff', 0.4) + '), color-stop(0.72, ' + col + '), to(' + col + '))'
        : '#120a1c');
      oS(c.body, 'border', oPx(Math.max(1, dS(Math.round(d * (o.on ? 0.055 : 0.04))))) + ' solid ' + (o.on ? '#ffffff' : col + (over ? 'b3' : '59')));
      osuDiscAt(c.body, dX(o.cx), dY(o.cy), dS(d * (o.on ? 0.92 + 0.08 * e : 1)));
      oV(c.body, true);
      oV(c.ring, o.on);
      if (o.on) {
        osuDiscAt(c.ring, dX(o.cx), dY(o.cy), dS(d * (1.6 - 0.26 * e)));
        oS(c.ring, 'border', oPx(dS(Math.max(3, Math.round(d * 0.02)))) + ' solid ' + col + '8c');
        oS(c.ring, 'opacity', e.toFixed(3));
      }
      osuBox(c.lab.p, dX(x0), dY(y0), dS(d), dS(d));
      osuFs(c.lab.l, o.ls); oS(c.lab.l, 'color', o.on ? '#120a1c' : col); oT(c.lab.l, o.label); oV(c.lab.p, true);
      if (o.name) {
        osuBox(c.name.p, dX(o.cx - (d + 120) / 2), dY(y0 + d + 20), dS(d + 120), dS(26));
        osuFs(c.name.l, o.ns || 19); oS(c.name.l, 'color', o.on || over ? '#ffffff' : '#ffffff99'); oT(c.name.l, o.name); oV(c.name.p, true);
        osuBox(c.sub.p, dX(o.cx - (d + 120) / 2), dY(y0 + d + 46), dS(d + 120), dS(20));
        osuFs(c.sub.l, 15); oS(c.sub.l, 'color', o.on ? col : '#ffffff59'); oT(c.sub.l, o.sub || ''); oV(c.sub.p, !!o.sub);
      } else { oV(c.name.p, false); oV(c.sub.p, false); }
      if (o.key) {
        var kw = Math.max(30, 14 + o.key.length * 9);
        osuCapDraw(c.cap, x0 + (o.kx === undefined ? d * 0.74 : (o.kx === 'c' ? d / 2 - kw / 2 : o.kx)), y0 + (o.ky === undefined ? -6 : o.ky), o.key, o.on ? col : null, false);
      } else oV(c.cap.p, false);
      if (o.cmd !== undefined) osuHit(dX(x0), dY(y0), dS(d), dS(d), o.cmd);
    }
)EGJS"
#include "EasterEggOsuPlayJs.h"
#include "EasterEggOsuSelectJs.h"
#include "EasterEggOsuMenusJs.h"
#include "EasterEggOsuScreensJs.h"
R"EGJS(
    function osuHandleClick(sz) {
      var k = osuF.clk;
      if (!k) return;
      if (osuClickSeen < 0) { osuClickSeen = k[0]; return; }
      if (k[0] === osuClickSeen) return;
      osuClickSeen = k[0];
      var x = k[1] * sz.w, y = k[2] * sz.h;
      for (var i = osuHits.length - 1; i >= 0; i--) {
        var h = osuHits[i];
        if (x >= h[0] && x <= h[2] && y >= h[1] && y <= h[3]) { if (h[4]) osuCmd(h[4]); return; }
      }
    }

    function osuFrame(t, dt, sz) {
      var fs = root.GetAttributeString('osuframe', '');
      if (!fs) { oV(osuLayer, false); return; }
      if (fs !== osuFrameStr) { osuFrameStr = fs; try { osuF = JSON.parse(fs); } catch (e) { return; } }
      var mode = osuF.m || 'closed';
      oV(osuLayer, mode !== 'closed');
      if (mode === 'closed') {
        if (osuWasMode !== 'closed') osuClickSeen = -1;
        osuWasMode = mode;
        return;
      }
      var os = root.GetAttributeString('osuobjs', '');
      if (os && os !== osuObjsStr) { osuObjsStr = os; try { osuO = JSON.parse(os); } catch (e2) {} }
      var vs = root.GetAttributeString('osulist', '');
      if (vs && vs !== osuViewStr) { osuViewStr = vs; try { osuV = JSON.parse(vs); } catch (e3) {} }
      osuPlace(sz);
      osuMouse.x = (osuF.mx || 0) * sz.w; osuMouse.y = (osuF.my || 0) * sz.h;
      // Clicks hit what was on screen when they happened: last frame's rectangles.
      osuHandleClick(sz);
      osuHits = [];

      // The CS2 frame always shows faintly through (osu!'s background dim over the "background"): the
      // menus put the purple backdrop over it at 94%, gameplay a 97% black dim (the game at ~3%).
      var game = mode === 'play' || mode === 'pause' || mode === 'fail';
      var dims = { select: '#00000000', loading: '#00000040', play: '#000000f7', pause: '#000000f7', fail: '#000000f7', results: '#00000060' };
      oS(osuDim, 'backgroundColor', dims[mode] || '#000000d0');
      oV(osuBack, !game);
      oS(osuBack, 'opacity', '0.94');
      oV(osuPlay, game);
      oV(osuSelect, mode === 'select');
      oV(osuScreens, mode === 'loading' || mode === 'pause' || mode === 'fail' || mode === 'results');
      if (mode === 'select') osuDrawSelect(t, dt, sz);
      if (game) osuDrawPlay(osuF.t, dt, sz);
      osuDrawScreens(mode, t, dt, sz);
      osuDrawCursor(mode, dt, sz);
      osuDrawVolume(sz);
      osuWasMode = mode;
    }
)EGJS"
