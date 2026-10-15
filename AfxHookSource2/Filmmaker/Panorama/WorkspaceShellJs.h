#pragma once

// THE WORKSPACE SHELL -- ONE definition of everything that frames the live preview.
//
// Every workspace that shows the game inside a preview builds the same furniture around it:
// four click-catchers ringing the preview, the "● PREVIEW" tag, four letterbox masks carrying
// the animated line-map wash, the 2px accent frame, and the wordmark decor (rule, code line,
// ghost title, title) that lands in whichever letterbox region is largest.
//
// CONFIG and the Camera Editor each carried their own copy of all of it, and Render painted its
// own line-map a third time. The copies were line-for-line the same apart from the accent and
// the wordmark strings -- which is exactly what the user called out: "all it's supposed to do
// is change colours". Two copies of the same 120 lines also meant a fix landing in one panel
// and not the other, which is how the two workspaces kept breaking one at a time.
//
// NOT a standalone script. It is a raw-string chunk concatenated into a host IIFE and needs
// these already in the host's scope: `mk`, `lbl`, `S` (theme tokens: .bg .value .danger).
//
//   var shell = mvmWorkspaceShell(root, {
//     accent: S.accent, title: 'CONFIG / SYSTEM', code: 'SOURCE // MVM FILMMAKER',
//     onCatcher: function () { /* swallow, or close dropdowns */ }
//   });
//   shell.layout({ rw, rh, areaX0, areaW, areaH, ox, oy, pw, ph, catcherBottom, cursorOn,
//                  accent, title, code, frameColor })   -- every frame the workspace is up
//   shell.hide()                                        -- when it is not
//   shell.pulse(true|false)                             -- the tag's recording dot (editor)
//   shell.tag / shell.backdrop are exposed for hosts that position the footer themselves.
R"SHELLJS(
    // THE COLUMN GROUND -- the Render workspace's column background, for the inspector columns:
    // rgba(15,17,20) with CS2's own GO-button line map (an animated video) at a whisper of
    // opacity, tinted with the workspace's accent through washColor. Render's columns build it in
    // CameraEditorRenderLayoutJs.h (grey); the Camera Editor (gold) and CONFIG (blue) get the
    // identical ground here.
    //
    // ONE root-level panel spanning the column's whole height, top of the screen to the bottom,
    // created BEFORE the inspector so it paints under it. The inspector (shortened per frame to
    // end above RENDER / CUSTOMIZE PLAYER) and the footer band behind those buttons are both
    // transparent over it, so the texture runs unbroken under the buttons -- washing each piece
    // separately left a seam and a flat band where the inspector ended.
    //
    // The texture is a CHILD layer, never a property of the ground: washColor multiplies a panel's
    // whole content. sync(width, visible) follows the user-resizable column every frame.
    var MVM_COLUMN_BASE = 'rgba(15,17,20,1)';
    function mvmColumnGround(root, washColor, width) {
      var g = mk('Panel', root); g.hittest = false;
      g.style.horizontalAlign = 'right'; g.style.verticalAlign = 'top';
      g.style.width = width + 'px'; g.style.height = '100%';
      g.style.backgroundColor = MVM_COLUMN_BASE;
      g.style.borderLeft = '1px solid #ffffff14';
      var tex = mk('Panel', g); tex.hittest = false;
      tex.style.width = '100%'; tex.style.height = '100%';
      tex.style.backgroundImage = 'url("file://{resources}/videos/linemap.webm")';
      tex.style.backgroundRepeat = 'repeat'; tex.style.backgroundPosition = '20% 60%';
      tex.style.backgroundSize = 'auto auto';
      // About a quarter of the original 7.5%: in the side columns the line map is texture, not decoration.
      tex.style.backgroundImgOpacity = '0.02';
      tex.style.washColor = washColor; tex.style.brightness = '0.48';
      var lastW = width, lastVis = true;
      return {
        panel: g, texture: tex,
        sync: function (w, visible) {
          visible = !!visible;
          if (visible !== lastVis) { lastVis = visible; g.visible = visible; }
          w = Math.round(w);
          if (w > 0 && w !== lastW) { lastW = w; g.style.width = w + 'px'; }
        }
      };
    }

    function mvmWorkspaceShell(root, opts) {
      opts = opts || {};
      var accent = opts.accent || S.accent;

      // Catcher frame: rings the preview so a click on empty space is swallowed instead of
      // reaching the game (which would switch the spectated player). The preview itself stays
      // bare so LMB/RMB there still reach the game and cycle targets natively.
      function mkCatcher() {
        var c = mk('Panel', root); c.visible = false; c.hittest = false;
        c.SetPanelEvent('onactivate', function () { if (opts.onCatcher) opts.onCatcher(); });
        return c;
      }
      var catcherTop = mkCatcher(), catcherBottom = mkCatcher();
      var catcherLeft = mkCatcher(), catcherRight = mkCatcher();

      // "● PREVIEW" tag pinned to the preview's top-left.
      var tag = mk('Panel', root); tag.visible = false; tag.hittest = false;
      tag.style.flowChildren = 'right'; tag.style.height = '22px';
      tag.style.backgroundColor = 'rgba(12,14,18,0.65)'; tag.style.borderRadius = '3px';
      tag.style.paddingLeft = '8px'; tag.style.paddingRight = '10px';
      var dot = mk('Panel', tag); dot.style.width = '7px'; dot.style.height = '7px';
      dot.style.backgroundColor = S.danger; dot.style.borderRadius = '4px';
      dot.style.verticalAlign = 'center'; dot.style.marginRight = '7px';
      var tagLbl = lbl(tag, 'PREVIEW', S.value, 11);
      tagLbl.style.fontWeight = 'bold'; tagLbl.style.letterSpacing = '2px';
      tagLbl.style.verticalAlign = 'center';
      // Recording-style pulse on the dot. Only opacity moves, so the tag never reflows.
      var pulsing = false, bright = true;
      function pulseTick() {
        if (!pulsing) return;
        if (!dot || (dot.IsValid && !dot.IsValid())) return;
        bright = !bright;
        dot.style.opacity = bright ? '1.0' : '0.22';
        $.Schedule(bright ? 0.75 : 0.30, pulseTick);
      }
      function pulse(on) {
        on = !!on;
        if (on === pulsing) return;
        pulsing = on;
        if (on) $.Schedule(0.75, pulseTick);
        else dot.style.opacity = '1.0';
      }

      // Footer backdrop under the preview area, behind the dock.
      var backdrop = mk('Panel', root); backdrop.visible = false; backdrop.hittest = false;
      backdrop.style.horizontalAlign = 'left'; backdrop.style.verticalAlign = 'top';
      backdrop.style.backgroundColor = S.bg; backdrop.style.borderTop = '1px solid #ffffff14';

      // Letterbox masks: black, with CS2's matchmaking-button line-map washed in the accent.
      function mask() {
        var p = mk('Panel', root); p.visible = false; p.hittest = false;
        p.style.backgroundColor = '#05070aff';
        p.style.backgroundImage = 'url("file://{resources}/videos/linemap.webm")';
        p.style.backgroundRepeat = 'repeat'; p.style.backgroundPosition = '20% 60%';
        p.style.backgroundSize = 'auto auto'; p.style.backgroundImgOpacity = '0.11';
        p.style.brightness = '0.55'; p.style.washColor = accent;
        p.style.horizontalAlign = 'left'; p.style.verticalAlign = 'top';
        return p;
      }
      var barLeft = mask(), barRight = mask(), barTop = mask(), barBottom = mask();
      var masks = [barLeft, barRight, barTop, barBottom];

      // 2px accent frame around the preview.
      function edge(prop) {
        var p = mk('Panel', root); p.visible = false; p.hittest = false;
        p.style[prop] = '2px'; p.style.backgroundColor = accent;
        p.style.horizontalAlign = 'left'; p.style.verticalAlign = 'top';
        return p;
      }
      var frameL = edge('width'), frameR = edge('width'), frameT = edge('height'), frameB = edge('height');
      var frames = [frameL, frameR, frameT, frameB];

      // Wordmark decor, placed in the largest letterbox region that can carry it.
      var decor = mk('Panel', root); decor.hittest = false; decor.visible = false;
      decor.style.overflow = 'clip';
      var decorRule = mk('Panel', decor); decorRule.style.width = '96px'; decorRule.style.height = '1px';
      decorRule.style.marginLeft = '24px'; decorRule.style.marginTop = '18px'; decorRule.style.backgroundColor = accent;
      var decorCode = lbl(decor, opts.code || '', accent, 10);
      decorCode.style.marginLeft = '132px'; decorCode.style.marginTop = '11px';
      decorCode.style.fontWeight = 'bold'; decorCode.style.letterSpacing = '3px';
      // Panorama clips a panel's blur and text-shadow to the panel's OWN box, so a label that
      // is exactly as big as its text has its smear and glow cut off flat at the edges. Each
      // animated label gets room for its blur to fall off in, and pulls its margins in by the same
      // amount so the text itself sits exactly where it did.
      var WM_PAD_H = 56, WM_PAD_V = 26;
      function wmRoom(l, padH, padV) {
        l.style.paddingLeft = padH + 'px'; l.style.paddingRight = padH + 'px';
        l.style.paddingTop = padV + 'px'; l.style.paddingBottom = padV + 'px';
      }
      var decorGhost = lbl(decor, opts.title || '', accent, 42);
      decorGhost.style.horizontalAlign = 'right'; decorGhost.style.verticalAlign = 'center';
      decorGhost.style.marginRight = (18 - 32) + 'px'; decorGhost.style.fontWeight = 'bold';
      wmRoom(decorGhost, 32, 20);
      decorGhost.style.letterSpacing = '7px'; decorGhost.style.opacity = '0.055';
      // Motion trail: echoes of the title drawn BEHIND it (built first, so they paint first), each
      // sampling the title's path a few ms in the past. While the title streaks they fan out behind
      // it; as it decelerates they collapse onto it and fade -- the early-2000s long-exposure smear.
      var DECOR_TRAIL_LAGS = [0.035, 0.075, 0.13];
      var decorTrails = [];
      for (var dti = 0; dti < DECOR_TRAIL_LAGS.length; dti++) {
        var tr = lbl(decor, opts.title || '', accent, 28);
        tr.style.marginLeft = (24 - WM_PAD_H) + 'px'; tr.style.marginBottom = (14 - WM_PAD_V) + 'px';
        wmRoom(tr, WM_PAD_H, WM_PAD_V);
        tr.style.verticalAlign = 'bottom'; tr.style.fontWeight = 'bold';
        tr.style.letterSpacing = '6px'; tr.style.opacity = '0';
        decorTrails.push(tr);
      }
      var decorTitle = lbl(decor, opts.title || '', accent, 28);
      decorTitle.style.marginLeft = (24 - WM_PAD_H) + 'px'; decorTitle.style.marginBottom = (14 - WM_PAD_V) + 'px';
      wmRoom(decorTitle, WM_PAD_H, WM_PAD_V);
      decorTitle.style.verticalAlign = 'bottom'; decorTitle.style.fontWeight = 'bold';
      decorTitle.style.letterSpacing = '6px'; decorTitle.style.opacity = '0.34';

      var curAccent = accent, curTitle = opts.title || '', curCode = opts.code || '';
      function restyle(a, title, code) {
        a = a || accent;
        if (a !== curAccent) {
          curAccent = a;
          for (var i = 0; i < 4; i++) { masks[i].style.washColor = a; }
          decorRule.style.backgroundColor = a; decorCode.style.color = a;
          decorGhost.style.color = a; decorTitle.style.color = a;
          for (var t = 0; t < decorTrails.length; t++) decorTrails[t].style.color = a;
        }
        if (title !== undefined && title !== curTitle) {
          curTitle = title; decorGhost.text = title; decorTitle.text = title;
          for (var u = 0; u < decorTrails.length; u++) decorTrails[u].text = title;
          wmRestart();   // a new title (Editor -> Render) sweeps in fresh
        }
        if (code !== undefined && code !== curCode) { curCode = code; decorCode.text = code; wmRestart(); }
      }

)SHELLJS"
// Split here: MSVC caps a single raw string literal at ~16380 bytes (C2026). Adjacent literals are
// concatenated by the compiler, so this stays one JS blob (mid-function is fine).
R"SHELLJS(
      // ---- the wordmark animation ("Gen X soft club") -------------------------------------
      // A loop, driven per frame by $.Schedule while the decor is on screen:
      //   IN    the title streaks in from the left -- fast, wide-tracked, smeared by a horizontal
      //         motion blur that is DERIVED FROM ITS SPEED -- then decelerates hard (expo ease-out)
      //         until the blur resolves into a soft glow and the word is readable;
      //   HOLD  it rests with a breathing bloom while the ghost word drifts behind it;
      //   OUT   it whips off to the right, blurring again, and the loop starts over.
      // The loop is SEAMLESS: the next sweep starts the instant the last one -- title, every trail
      // echo, and the blur/glow room around them -- has cleared the box, from just outside its left
      // edge, so motion never stops and nothing is ever cut off mid-screen. WM_TAIL is the exit
      // running on after the title is gone until the slowest echo is out too. It used to rest in an empty band for
      // 0.3s plus a near-motionless exponential wind-up (measured in game: 0.31-0.40s blank every
      // 8.3s), which read as the animation pausing on every loop.
      // As the title lands, the code line decodes like a club flyer and the accent rule draws on.
      // Only paint properties move (transform, blur, glow, opacity, letter-spacing on a bottom-
      // anchored label in a clipped box), so nothing outside the decor ever reflows.
      var WM_IN = 1.25, WM_HOLD = 6.5, WM_OUT = 0.42;
      var WM_TAIL = DECOR_TRAIL_LAGS[DECOR_TRAIL_LAGS.length - 1] + 0.02;
      var WM_CYCLE = WM_IN + WM_HOLD + WM_OUT + WM_TAIL;
      var WM_GLYPHS = '01/\\|_-=+<>:;.#%*';
      var wmStart = 0, wmRunning = false, wmCodeDone = '', wmTw = 0;
      // Frozen loop time (s) for deterministic captures, or null while it runs. Once a frozen frame
      // has been painted the loop stops writing, so the page goes quiet for the capture tools.
      var wmFreeze = null, wmFrozenPainted = false;
      function wmNow() { return Date.now() / 1000; }
      function wmRestart() { wmStart = wmNow(); wmCodeDone = ''; }
      function wmExpoOut(t) { return t >= 1 ? 1 : 1 - Math.pow(2, -11 * t); }
      function wmExpoIn(t) { return t <= 0 ? 0 : Math.pow(2, 11 * (t - 1)); }
      function wmAlpha(c, a) { c = '' + c; return (c.charAt(0) === '#' && c.length >= 7) ? ('#' + c.substring(1, 7) + a) : c; }
      // Title x offset (px) at loop time t. inDist = how far left the title starts to be fully out
      // of the box, outDist = how far right it must travel to be fully out of it. t < 0 is a trail
      // sampling before this loop began: the previous exit (tail included) is already off screen,
      // so it waits at the entry point with the title and fans out behind it.
      function wmX(t, inDist, outDist) {
        if (t < 0) t = 0;
        if (t < WM_IN) return -inDist * (1 - wmExpoOut(t / WM_IN));
        var h = t - WM_IN;
        if (h < WM_HOLD) return Math.sin(h * 0.9) * 1.5;   // a breath of float, not a wobble
        var o = Math.min(1, (h - WM_HOLD) / WM_OUT);
        // Cubic ease-in: under way from the first frame (an exponential ease-in sat almost still
        // for most of the exit), still a whip at the end.
        return outDist * o * o * o;
      }
      function wmBlur(p, h, v) { p.style.blur = 'gaussian(' + h.toFixed(1) + ', ' + v.toFixed(1) + ', 1)'; }
      function wmScramble(text, prog) {
        var out = '';
        for (var i = 0; i < text.length; i++) {
          var ch = text.charAt(i);
          if (ch === ' ' || i < prog * text.length) out += ch;
          else out += WM_GLYPHS.charAt(Math.floor(Math.random() * WM_GLYPHS.length));
        }
        return out;
      }
      // The workspace can be torn down between two scheduled frames, and touching ANY property of a
      // deleted panel throws "Underlying panel is deleted!" -- so validity is checked first, before
      // `visible`, and the whole frame is guarded: a throw ends the loop instead of the script.
      function wmFrame() {
        if (!wmRunning) return;
        try {
          if (!decor || !decor.IsValid || !decor.IsValid() || !decor.visible) { wmRunning = false; return; }
          wmPaint();
        } catch (e) { wmRunning = false; return; }
        $.Schedule(wmFreeze !== null && wmFrozenPainted ? 1 / 30 : 1 / 60, wmFrame);
      }
      function wmPaint() {
        if (wmFreeze !== null && wmFrozenPainted) return;   // a frozen frame is painted once
        var now = wmNow(), t = (wmFreeze !== null) ? wmFreeze : (now - wmStart) % WM_CYCLE;
        if (wmFreeze !== null) { now = wmStart + wmFreeze; wmFrozenPainted = true; }
        var sx = decor.actualuiscale_x || 1;
        var w = (decor.actuallayoutwidth || 0) / sx;
        if (!(w > 0 && w < 1e30)) w = 680;
        // Title width without the blur padding (see wmRoom), re-measured only while it rests (the
        // sweep widens its letter-spacing) so the travel distances never change mid-sweep.
        var mtw = (decorTitle.actuallayoutwidth || 0) / sx - 2 * WM_PAD_H;
        if (mtw > 40 && mtw < 4000 && (!wmTw || (t >= WM_IN && t < WM_IN + WM_HOLD))) wmTw = mtw;
        var tw = wmTw || 340;
        // Fully out = the text AND its blur/glow room (WM_PAD_H) past the clip edge, with the entry's
        // extra letter-spacing (up to +22px a glyph) accounted for on the way in.
        var spread = 22 * (curTitle ? curTitle.length : 12);
        var inDist = 24 + tw + spread + WM_PAD_H + 24, outDist = Math.max(80, w - 24 + WM_PAD_H + 24);
        var dist = Math.max(inDist, outDist);
        var x = wmX(t, inDist, outDist), dt = 1 / 60;
        var vel = Math.abs(wmX(t + dt, inDist, outDist) - x) / dt;   // px/s -- the blur follows it
        var streak = Math.min(1, vel / 2400);                     // 0 at rest, 1 at full whip
        var inProg = Math.min(1, Math.max(0, t / WM_IN));
        var outProg = Math.max(0, (t - WM_IN - WM_HOLD) / WM_OUT);
        // The very first sweep after the workspace opens draws the rule on; later loops keep it.
        var firstLoop = (wmFreeze === null) && (now - wmStart) < WM_CYCLE;
        var hold = t >= WM_IN && t < WM_IN + WM_HOLD;

        // The title: horizontal smear while moving, a soft residual focus at rest.
        var bh = 1.1 + streak * 34, bv = 0.6 + streak * 2.2;
        wmBlur(decorTitle, bh, bv);
        decorTitle.style.transform = 'translate3d(' + x.toFixed(1) + 'px, 0px, 0px)';
        decorTitle.style.letterSpacing = (6 + 22 * (1 - wmExpoOut(inProg)) + 18 * wmExpoIn(Math.min(1, outProg))).toFixed(1) + 'px';
        // No fade at the wrap: the title enters from outside the clipped box, so it is never seen
        // popping in, and a fade there was part of the visible dead time between loops.
        var fadeOut = 1 - Math.min(1, outProg) * 0.5;
        decorTitle.style.opacity = (0.42 * fadeOut).toFixed(3);
        // Bloom: wide and bright while streaking, then a slow breathe while it holds.
        var breathe = hold ? (0.5 + 0.5 * Math.sin((t - WM_IN) * 1.6)) : 0;
        var glow = 6 + streak * 22 + breathe * 8;
        decorTitle.style.textShadow = '0px 0px ' + glow.toFixed(1) + 'px ' + (1.2 + streak * 1.6 + breathe * 0.6).toFixed(2) + ' ' + wmAlpha(curAccent, 'aa');

        // The trail: each echo is the title a little in the past, fainter and blurrier.
        for (var i = 0; i < decorTrails.length; i++) {
          var lag = DECOR_TRAIL_LAGS[i];
          var tx = wmX(t - lag, inDist, outDist), gap = Math.abs(x - tx);
          var tr = decorTrails[i];
          tr.style.transform = 'translate3d(' + tx.toFixed(1) + 'px, 0px, 0px)';
          tr.style.letterSpacing = decorTitle.style.letterSpacing;
          var tstreak = Math.min(1, Math.abs(wmX(t - lag + dt, inDist, outDist) - tx) / dt / 2400);   // its own speed: it is still whipping out after the title has gone
          wmBlur(tr, 4 + tstreak * 30 + i * 6, 1 + i * 0.8);
          tr.style.opacity = (Math.min(0.26, gap / 260) * (1 - i * 0.25)).toFixed(3);
        }

        // The ghost word: always out of focus, drifting slowly; it kicks against the sweep.
        // The kick is by distance from rest, the same at the end of one sweep and the start of the
        // next, so the ghost never jumps at the wrap.
        var gx = Math.sin(now * 0.35) * 22 - (Math.abs(x) / dist) * 40;
        decorGhost.style.transform = 'translate3d(' + gx.toFixed(1) + 'px, 0px, 0px)';
        wmBlur(decorGhost, 3 + streak * 10, 3);
        decorGhost.style.opacity = (0.05 + streak * 0.05).toFixed(3);

        // The rule draws on with the landing; the code line decodes once the title is close.
        decorRule.style.width = (firstLoop ? 96 * wmExpoOut(Math.min(1, inProg * 1.3)) : 96).toFixed(1) + 'px';
        var codeProg = Math.min(1, Math.max(0, (t - WM_IN * 0.45) / 0.55));
        if (codeProg < 1) { decorCode.text = wmScramble(curCode, codeProg); wmCodeDone = ''; }
        else if (wmCodeDone !== curCode) { decorCode.text = curCode; wmCodeDone = curCode; }
        decorCode.style.opacity = (0.35 + 0.65 * codeProg * (1 - Math.min(1, outProg))).toFixed(3);
      }
      function wmEnsure() {
        if (wmRunning) return;
        wmRunning = true;
        if (!wmStart) wmRestart();
        $.Schedule(0, wmFrame);
      }

      function placeDecor(rects) {
        var best = null, bestArea = 0;
        for (var i = 0; i < rects.length; i++) {
          var r = rects[i], area = Math.max(0, r.w) * Math.max(0, r.h);
          if (r.w >= 180 && r.h >= 58 && area > bestArea) { best = r; bestArea = area; }
        }
        if (!best) { decor.visible = false; return; }
        decor.style.position = Math.floor(best.x) + 'px ' + Math.floor(best.y) + 'px 0px';
        decor.style.width = Math.floor(best.w) + 'px'; decor.style.height = Math.floor(best.h) + 'px';
        // The code row is top-anchored (~14px at marginTop 11) and the title bottom-anchored
        // (28px + marginBottom 14): below ~85px of height they overlap. Gate on real clearance.
        decorTitle.visible = best.h >= 88;
        for (var i = 0; i < decorTrails.length; i++) decorTrails[i].visible = decorTitle.visible;
        decorGhost.visible = best.h >= 110;
        // Every time the workspace opens, the wordmark sweeps in fresh.
        if (!decor.visible) wmRestart();
        decor.visible = true;
        wmEnsure();
      }

      // g: { rw, rh, areaX0, areaW, areaH, ox, oy, pw, ph, catcherBottom, cursorOn,
      //      accent?, title?, code?, frameColor? }  -- all in root css px.
      function layout(g) {
        var ax0 = g.areaX0 || 0;
        var right = g.ox + g.pw, bottom = g.oy + g.ph;
        var areaRight = ax0 + g.areaW;
        restyle(g.accent, g.title, g.code);

        var cb = (g.catcherBottom !== undefined) ? g.catcherBottom : g.rh;
        catcherTop.style.position = '0px 0px 0px';
        catcherTop.style.width = Math.floor(g.rw) + 'px'; catcherTop.style.height = Math.floor(g.oy) + 'px';
        catcherBottom.style.position = '0px ' + Math.floor(bottom) + 'px 0px';
        catcherBottom.style.width = Math.floor(g.rw) + 'px';
        catcherBottom.style.height = Math.floor(Math.max(0, cb - bottom)) + 'px';
        catcherLeft.style.position = '0px ' + Math.floor(g.oy) + 'px 0px';
        catcherLeft.style.width = Math.floor(g.ox) + 'px'; catcherLeft.style.height = Math.floor(g.ph) + 'px';
        catcherRight.style.position = Math.floor(right) + 'px ' + Math.floor(g.oy) + 'px 0px';
        catcherRight.style.width = Math.floor(Math.max(0, g.rw - right)) + 'px';
        catcherRight.style.height = Math.floor(g.ph) + 'px';
        var on = !!g.cursorOn;
        catcherTop.visible = catcherBottom.visible = catcherLeft.visible = catcherRight.visible = true;
        catcherTop.hittest = catcherBottom.hittest = catcherLeft.hittest = catcherRight.hittest = on;

        barLeft.style.position = ax0 + 'px 0px 0px';
        barLeft.style.width = (g.ox - ax0) + 'px'; barLeft.style.height = g.areaH + 'px';
        barLeft.visible = (g.ox - ax0) > 1;
        barRight.style.position = right + 'px 0px 0px';
        barRight.style.width = (areaRight - right) + 'px'; barRight.style.height = g.areaH + 'px';
        barRight.visible = (areaRight - right) > 1;
        barTop.style.position = g.ox + 'px 0px 0px';
        barTop.style.width = g.pw + 'px'; barTop.style.height = g.oy + 'px';
        barTop.visible = g.oy > 1;
        barBottom.style.position = g.ox + 'px ' + bottom + 'px 0px';
        barBottom.style.width = g.pw + 'px'; barBottom.style.height = (g.areaH - bottom) + 'px';
        barBottom.visible = (g.areaH - bottom) > 1;

        placeDecor([
          { x: ax0, y: 0, w: g.ox - ax0, h: g.areaH },
          { x: right, y: 0, w: areaRight - right, h: g.areaH },
          { x: g.ox, y: 0, w: g.pw, h: g.oy },
          { x: g.ox, y: bottom, w: g.pw, h: g.areaH - bottom }
        ]);

        var fc = g.frameColor || curAccent;
        for (var i = 0; i < 4; i++) frames[i].style.backgroundColor = fc;
        frameL.style.position = g.ox + 'px ' + g.oy + 'px 0px'; frameL.style.height = g.ph + 'px';
        frameR.style.position = (right - 2) + 'px ' + g.oy + 'px 0px'; frameR.style.height = g.ph + 'px';
        frameT.style.position = g.ox + 'px ' + g.oy + 'px 0px'; frameT.style.width = g.pw + 'px';
        frameB.style.position = g.ox + 'px ' + (bottom - 2) + 'px 0px'; frameB.style.width = g.pw + 'px';
        frameL.visible = frameR.visible = frameT.visible = frameB.visible = true;

        tag.visible = true; tag.style.position = (g.ox + 18) + 'px ' + (g.oy + 14) + 'px 0px';
      }

      function hide() {
        catcherTop.visible = catcherBottom.visible = catcherLeft.visible = catcherRight.visible = false;
        catcherTop.hittest = catcherBottom.hittest = catcherLeft.hittest = catcherRight.hittest = false;
        backdrop.visible = false; tag.visible = false; decor.visible = false;
        for (var i = 0; i < 4; i++) { masks[i].visible = false; frames[i].visible = false; }
      }

      return {
        layout: layout, hide: hide, pulse: pulse, restyle: restyle,
        // Restart the wordmark sweep now (automation / netcon: $.ConfigHud.wordmark(), $.CamEditor.wordmark()).
        // With a number, freezes the loop at that time instead (captures): 0.12 = mid-streak.
        replay: function (at) {
          var f = parseFloat(at);
          if (isFinite(f)) { wmFreeze = Math.max(0, Math.min(WM_CYCLE - 0.001, f)); wmFrozenPainted = false; }
          else { wmFreeze = null; wmRestart(); }
        },
        // tagLbl is exposed because the Camera Editor retitles it per frame ('LIVE' vs
        // 'PREVIEW') depending on whether the preview is showing the live view.
        tag: tag, tagLbl: tagLbl, backdrop: backdrop, decor: decor,
        catchers: { top: catcherTop, bottom: catcherBottom, left: catcherLeft, right: catcherRight },
        masks: { left: barLeft, right: barRight, top: barTop, bottom: barBottom },
        frames: { left: frameL, right: frameR, top: frameT, bottom: frameB }
      };
    }
)SHELLJS"
