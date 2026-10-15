#pragma once

// HINT fragment of the easter-egg overlay (concatenated into kEasterEggOverlayJs, shares its
// scope). A rare hint -- or an egg switching on -- floats across the screen in the same "Gen X
// soft club" wordmark style as the CONFIG / CAMERA EDITOR titles (WorkspaceShellJs.h):
//   IN    the title streaks in from the left, smeared by a horizontal blur DERIVED FROM ITS
//         SPEED, echo trails fanning out behind it, then decelerates (expo) into a soft glow;
//   HOLD  a breathing bloom while a huge out-of-focus ghost word drifts behind; the clue types
//         itself out underneath, then a quiet caption fades in below it;
//   OUT   it whips off to the right, blurring again, and the banner is gone.
// Played ONCE per event rather than looped. Two weights:
//   faint -- the wordmark's own look: low opacity, the workspace gold/blue;
//   loud  -- full opacity, bigger, a pastel neon accent and a much stronger bloom.
//
// Panorama clips a panel's blur and text-shadow to its own box, so each animated label carries
// padding for the smear to fall off in and pulls its margins back by the same amount.

R"EGJS(
    var hintLayer = layer('MvmEggHints', '5');
    var HINT_IN = 1.25, HINT_OUT = 0.42, HINT_TRAIL_LAGS = [0.035, 0.075, 0.13];
    var HINT_PAD_H = 56, HINT_PAD_V = 26;
    var hint = null, hintSeen = 0;

    function hintRoom(l, h, v) {
      l.style.paddingLeft = h + 'px'; l.style.paddingRight = h + 'px';
      l.style.paddingTop = v + 'px'; l.style.paddingBottom = v + 'px';
    }

    function hintBuild(h, sz) {
      if (hint && hint.box && hint.box.IsValid()) hint.box.DeleteAsync(0);
      var loud = !!h.loud;
      var accent = loud ? pick([EGG.hot, EGG.sky, EGG.lav, EGG.mint, EGG.pink]) : pick([EGG.gold, EGG.blue]);
      var titleSize = loud ? 30 : 24;
      var boxW = Math.min(820, Math.max(420, sz.w * 0.42));
      var boxH = 170;
      var box = $.CreatePanel('Panel', hintLayer, '');
      box.hittest = false;
      box.style.width = boxW + 'px'; box.style.height = boxH + 'px';
      // Clip SIDEWAYS only: the title streaks in from beyond the left edge, but the soft blur and
      // the ghost word need room above and below, or their glow is sliced off flat.
      box.style.overflow = 'clip noclip';
      // Somewhere in the upper two thirds, left or right: never dead centre on the action.
      var x = Math.random() < 0.5 ? sz.w * 0.04 : sz.w * 0.96 - boxW;
      var y = sz.h * (0.1 + Math.random() * 0.38);
      box.style.position = Math.max(0, x).toFixed(0) + 'px ' + y.toFixed(0) + 'px 0px';
      // A whisper of a scrim behind JUST the words (loud only), sized each frame to the title and
      // the clue -- a box-wide band read as a UI bar, not a hint. It wipes in as the title lands.
      var scrim = null;
      if (loud) {
        scrim = $.CreatePanel('Panel', box, '');
        scrim.hittest = false;
        scrim.style.width = '0px'; scrim.style.height = (titleSize + 56) + 'px';
        scrim.style.horizontalAlign = 'left'; scrim.style.verticalAlign = 'top';
        scrim.style.marginLeft = '4px'; scrim.style.marginTop = '30px';
        scrim.style.borderRadius = '6px';
        try {
          scrim.style.backgroundColor = 'gradient( linear, 0% 0%, 100% 0%, from( #0a061200 ), color-stop( 0.12, #0a061270 ), color-stop( 0.8, #0a061255 ), to( #0a061200 ) )';
        } catch (eg) { scrim.style.backgroundColor = '#0a061255'; }
        scrim.style.opacity = '0';
      }

      var ghost = lbl(box, h.title || '', accent, loud ? 60 : 50);
      ghost.style.fontWeight = 'bold'; ghost.style.letterSpacing = '7px';
      ghost.style.horizontalAlign = 'right'; ghost.style.verticalAlign = 'center';
      ghost.style.marginRight = '-40px'; hintRoom(ghost, 32, 20);
      ghost.style.opacity = '0';

      function titleLabel() {
        var l = lbl(box, h.title || '', accent, titleSize);
        l.style.fontWeight = 'bold'; l.style.letterSpacing = '6px';
        l.style.marginLeft = (24 - HINT_PAD_H) + 'px'; l.style.marginTop = (40 - HINT_PAD_V) + 'px';
        hintRoom(l, HINT_PAD_H, HINT_PAD_V);
        l.style.opacity = '0';
        return l;
      }
      var trails = [];
      for (var i = 0; i < HINT_TRAIL_LAGS.length; i++) trails.push(titleLabel());
      var title = titleLabel();

      var sub = lbl(box, '', loud ? EGG.cream : '#e8eef5', loud ? 14 : 13);
      sub.style.marginLeft = '26px'; sub.style.marginTop = (40 + titleSize + 14) + 'px';
      sub.style.maxWidth = (boxW - 60) + 'px';
      sub.style.textShadow = '0px 1px 4px 2.0 #000000cc';
      sub.style.opacity = '0';
      // The clue types itself out, so its width grows; the scrim sizes to this invisible full copy.
      var subFull = lbl(box, h.sub || '', '#ffffff', loud ? 14 : 13);
      subFull.style.marginLeft = '26px'; subFull.style.marginTop = (40 + titleSize + 14) + 'px';
      subFull.style.maxWidth = (boxW - 60) + 'px';
      subFull.style.opacity = '0';
      // The caption: plain, small, a step quieter than the clue.
      var code = lbl(box, h.code || '', loud ? EGG.cream : '#e8eef5', 12);
      code.style.marginLeft = '26px'; code.style.marginTop = (40 + titleSize + 14 + 22) + 'px';
      code.style.maxWidth = (boxW - 60) + 'px';
      code.style.textShadow = '0px 1px 4px 2.0 #000000cc';
      code.style.opacity = '0';

      hint = { box: box, ghost: ghost, code: code, trails: trails, title: title, sub: sub, scrim: scrim, subFull: subFull,
        loud: loud, accent: accent, start: now(), hold: loud ? 7.0 : 8.0, tw: 0, boxW: boxW,
        titleText: h.title || '', subText: h.sub || '', subShown: -1 };
    }

    function hintX(t, inDist, outDist) {
      if (t < 0) t = 0;
      if (t < HINT_IN) return -inDist * (1 - expoOut(t / HINT_IN));
      var hd = t - HINT_IN;
      if (hd < hint.hold) return Math.sin(hd * 0.9) * 1.5;
      var o = Math.min(1, (hd - hint.hold) / HINT_OUT);
      return outDist * o * o * o;
    }

    function hintFrame(t, sz) {
      var event = st.hint ? st.hint.id : 0;
      if (event <= (st.hintCancelledThrough || 0)) {
        if (hint && hint.box && hint.box.IsValid()) hint.box.DeleteAsync(0);
        hint = null;
        hintSeen = Math.max(hintSeen, event);
        return;
      }
      if (st.hint && st.hint.id !== hintSeen) {
        hintSeen = st.hint.id;
        hintBuild(st.hint, sz);
      }
      if (!hint) return;
      if (!hint.box.IsValid()) { hint = null; return; }
      var ht = t - hint.start;
      var total = HINT_IN + hint.hold + HINT_OUT + HINT_TRAIL_LAGS[HINT_TRAIL_LAGS.length - 1] + 0.05;
      if (ht > total) { hint.box.DeleteAsync(0); hint = null; return; }

      var sx = hint.box.actualuiscale_x || 1;
      var mtw = (hint.title.actuallayoutwidth || 0) / sx - 2 * HINT_PAD_H;
      if (mtw > 40 && mtw < 4000 && (!hint.tw || (ht >= HINT_IN && ht < HINT_IN + hint.hold))) hint.tw = mtw;
      var tw = hint.tw || 420;
      var spread = 22 * hint.titleText.length;
      var inDist = 24 + tw + spread + HINT_PAD_H + 24, outDist = Math.max(80, hint.boxW - 24 + HINT_PAD_H + 24);
      var dist = Math.max(inDist, outDist), fdt = 1 / 60;
      var x = hintX(ht, inDist, outDist);
      var streak = Math.min(1, Math.abs(hintX(ht + fdt, inDist, outDist) - x) / fdt / 2400);
      var inProg = clamp01(ht / HINT_IN), outProg = Math.max(0, (ht - HINT_IN - hint.hold) / HINT_OUT);
      var holding = ht >= HINT_IN && ht < HINT_IN + hint.hold;
      var peak = hint.loud ? 0.96 : 0.42;

      blur(hint.title, 1.1 + streak * 34, 0.6 + streak * 2.2);
      hint.title.style.transform = 'translate3d(' + x.toFixed(1) + 'px, 0px, 0px)';
      var spacing = (6 + 22 * (1 - expoOut(inProg)) + 18 * expoIn(Math.min(1, outProg))).toFixed(1) + 'px';
      hint.title.style.letterSpacing = spacing;
      hint.title.style.opacity = (peak * (1 - Math.min(1, outProg) * 0.5)).toFixed(3);
      var breathe = holding ? (0.5 + 0.5 * Math.sin((ht - HINT_IN) * 1.6)) : 0;
      var glow = (hint.loud ? 10 : 6) + streak * 22 + breathe * (hint.loud ? 14 : 8);
      hint.title.style.textShadow = '0px 0px ' + glow.toFixed(1) + 'px ' + (1.2 + streak * 1.6 + breathe * (hint.loud ? 1.2 : 0.6)).toFixed(2)
        + ' ' + hint.accent + 'aa';

      for (var i = 0; i < hint.trails.length; i++) {
        var lag = HINT_TRAIL_LAGS[i], tr = hint.trails[i];
        var tx = hintX(ht - lag, inDist, outDist), gap = Math.abs(x - tx);
        tr.style.transform = 'translate3d(' + tx.toFixed(1) + 'px, 0px, 0px)';
        tr.style.letterSpacing = spacing;
        var tstreak = Math.min(1, Math.abs(hintX(ht - lag + fdt, inDist, outDist) - tx) / fdt / 2400);
        blur(tr, 4 + tstreak * 30 + i * 6, 1 + i * 0.8);
        tr.style.opacity = (Math.min(hint.loud ? 0.5 : 0.26, gap / 260) * (1 - i * 0.25)).toFixed(3);
      }

      var gx = Math.sin(t * 0.35) * 22 - (Math.abs(x) / dist) * 40;
      hint.ghost.style.transform = 'translate3d(' + gx.toFixed(1) + 'px, 0px, 0px)';
      blur(hint.ghost, 3 + streak * 10, 3);
      var fadeAll = 1 - clamp01(outProg);
      hint.ghost.style.opacity = (((hint.loud ? 0.08 : 0.05) + streak * 0.05) * Math.min(1, inProg * 2) * fadeAll).toFixed(3);

      // The clue types itself out once the title has landed, and leaves with it.
      var typeProg = clamp01((ht - HINT_IN * 0.9) / Math.max(0.6, hint.subText.length * 0.022));
      var shown = Math.floor(typeProg * hint.subText.length);
      if (shown !== hint.subShown) {
        hint.subShown = shown;
        hint.sub.text = hint.subText.substring(0, shown) + (shown < hint.subText.length ? '_' : '');
      }
      hint.sub.style.opacity = ((hint.loud ? 0.95 : 0.78) * clamp01((ht - HINT_IN * 0.9) / 0.25) * fadeAll).toFixed(3);
      var subTransform = 'translate3d(' + (outProg > 0 ? outDist * 0.25 * outProg * outProg : 0).toFixed(1) + 'px, 0px, 0px)';
      hint.sub.style.transform = subTransform;
      // The caption fades in once the clue has finished typing, and leaves with it.
      var typeEnd = HINT_IN * 0.9 + Math.max(0.6, hint.subText.length * 0.022);
      hint.code.style.opacity = ((hint.loud ? 0.6 : 0.45) * clamp01((ht - typeEnd - 0.2) / 0.6) * fadeAll).toFixed(3);
      // Panorama's transform getter drops commas; that serialization is not accepted by its setter.
      hint.code.style.transform = subTransform;

      if (hint.scrim) {
        var subW = (hint.subFull.actuallayoutwidth || 0) / sx;
        var target = Math.min(hint.boxW - 8, Math.max(tw, subW) + 70);
        var wipe = expoOut(clamp01((ht - HINT_IN * 0.7) / 0.6));
        hint.scrim.style.width = (target * wipe).toFixed(1) + 'px';
        hint.scrim.style.opacity = (wipe * fadeAll).toFixed(3);
      }
    }
)EGJS"
