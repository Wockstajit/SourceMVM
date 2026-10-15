#pragma once

// SONY VEGAS fragment of the easter-egg overlay (concatenated into kEasterEggOverlayJs, shares its
// scope). When a render starts while any Sony/VEGAS Pro or Movie Studio process is running, C++
// bumps st.vegas and this takes over the screen: the clip plays full screen with its own audio,
// "SONY VEGAS SUCKS" streaks in on top in the wordmark's motion-blur style, and when the clip ends
// the whole thing fades away. The render underneath keeps going -- this is UI only, and C++ never
// starts it while the job could capture our chrome (st.hidden would hide it anyway).
//
// The clip is ui_override/panorama/videos/mvm_egg_vegas.webm in the content mount (head-priority
// search path; see Assets/easter-eggs/README.md), so it resolves as {resources}/videos/.

R"EGJS(
    var VEGAS_MOVIE = 'file://{resources}/videos/mvm_egg_vegas.webm';
    var VEGAS_LEN = 17.6, VEGAS_FADE_IN = 0.35, VEGAS_FADE_OUT = 1.4;
    var vegas = null, vegasSeen = 0;

    function vegasBuild() {
      var L = layer('MvmEggVegas', '20');
      L.style.backgroundColor = '#000000ff';
      L.style.opacity = '0';
      L.visible = false;
      var mv = null;
      try {
        mv = $.CreatePanel('Movie', L, 'MvmEggVegasMovie', { controls: 'none', repeat: 'false', autoplay: 'false' });
        mv.hittest = false;
        // The clip keeps its 16:9 shape; the black layer letterboxes it on any other screen.
        mv.style.width = '100%'; mv.style.height = '100%';
        mv.style.horizontalAlign = 'center'; mv.style.verticalAlign = 'center';
      } catch (em) { $.Msg('[eggs] no Movie panel: ' + em + '\n'); mv = null; }
      var v = { L: L, mv: mv, start: 0, ended: false, endAt: 0, playing: false };
      if (mv) {
        try { $.RegisterEventHandler('MoviePlayerPlaybackEnded', mv, function () { v.ended = true; }); } catch (ee) {}
      }

      // The words: a full-size box with the label centred inside it (a Label positioned on its
      // own renders low and left in game).
      var tbox = $.CreatePanel('Panel', L, '');
      tbox.hittest = false;
      tbox.style.width = '100%'; tbox.style.height = '100%';
      tbox.style.overflow = 'noclip noclip';
      function big(color, opacity) {
        var l = lbl(tbox, 'SONY VEGAS SUCKS', color, 92);
        l.style.fontWeight = 'bold'; l.style.letterSpacing = '6px';
        l.style.horizontalAlign = 'center'; l.style.verticalAlign = 'center';
        // Room for the blur to fall off in, without forcing a wrap on a 4:3 window.
        l.style.paddingLeft = '50px'; l.style.paddingRight = '50px';
        l.style.paddingTop = '40px'; l.style.paddingBottom = '40px';
        l.style.opacity = opacity;
        return l;
      }
      v.trails = [big(EGG.hot, '0'), big(EGG.sky, '0')];
      v.word = big('#ffffff', '0');
      v.code = lbl(tbox, 'render intercepted', EGG.cream, 16);
      v.code.style.textShadow = '0px 1px 6px 2.0 #000000ee';
      v.code.style.horizontalAlign = 'center'; v.code.style.verticalAlign = 'center';
      v.code.style.marginTop = '-150px';
      v.sub = lbl(tbox, 'close it. you know what you did.', EGG.cream, 22);
      v.sub.style.horizontalAlign = 'center'; v.sub.style.verticalAlign = 'center';
      v.sub.style.marginTop = '140px';
      v.sub.style.textShadow = '0px 1px 6px 2.0 #000000ee';
      return v;
    }

    function vegasStart() {
      if (!vegas) vegas = vegasBuild();
      var v = vegas;
      if (v.mv) {
        var sz = cssSize(), w = sz.w, h = sz.w * 9 / 16;
        if (h > sz.h) { h = sz.h; w = sz.h * 16 / 9; }
        v.mv.style.width = w.toFixed(0) + 'px'; v.mv.style.height = h.toFixed(0) + 'px';
      }
      v.L.visible = true;
      v.start = now(); v.ended = false; v.endAt = 0; v.playing = true;
      if (v.mv) {
        try {
          v.mv.SetMovie(VEGAS_MOVIE);
          try { v.mv.UseAttachedAudioStream(true); } catch (ea) {}
          v.mv.Play();
        } catch (ep) { $.Msg('[eggs] movie play failed: ' + ep + '\n'); }
      }
    }

    function vegasStop() {
      var v = vegas;
      if (!v || !v.playing) return;
      v.playing = false;
      v.L.visible = false;
      v.L.style.opacity = '0';
      if (v.mv) {
        try { v.mv.Stop(); } catch (es) {}
        try { v.mv.SetMovie(''); } catch (ec) {}
      }
      $.Msg('[eggs] vegas stopped\n');
    }

    function vegasSync() {
      var event = st.vegas || 0;
      if (st.hidden || event <= (st.vegasCancelledThrough || 0)) {
        vegasSeen = event;
        vegasStop();
        return;
      }
      if (event !== vegasSeen) {
        vegasSeen = event;
        vegasStart();
      }
    }

    function vegasX(t, w) {
      // Streak in from the left, land, drift a hair while the clip plays.
      if (t < 0) t = 0;
      if (t < 1.1) return -w * (1 - expoOut(t / 1.1));
      return Math.sin((t - 1.1) * 0.8) * 3;
    }

    function vegasFrame(t, sz) {
      if (!vegas || !vegas.playing) return;
      var v = vegas, vt = t - v.start;
      if (!v.endAt && (v.ended || vt > VEGAS_LEN + 0.4)) v.endAt = vt;
      var fade = clamp01(vt / VEGAS_FADE_IN);
      if (v.endAt) {
        var o = clamp01((vt - v.endAt) / VEGAS_FADE_OUT);
        fade *= 1 - o;
        if (o >= 1) { vegasStop(); return; }
      }
      v.L.style.opacity = fade.toFixed(3);

      var wt = vt - 0.25, fdt = 1 / 60;
      var x = vegasX(wt, sz.w), streak = Math.min(1, Math.abs(vegasX(wt + fdt, sz.w) - x) / fdt / 2400);
      blur(v.word, 1 + streak * 40, 0.6 + streak * 3);
      v.word.style.transform = 'translate3d(' + x.toFixed(1) + 'px, 0px, 0px)';
      v.word.style.opacity = wt > 0 ? '1' : '0';
      var land = wt > 1.1 ? 0.5 + 0.5 * Math.sin((wt - 1.1) * 2.2) : 0;
      v.word.style.textShadow = '0px 0px ' + (18 + streak * 30 + land * 16).toFixed(1) + 'px ' + (2 + land).toFixed(2) + ' ' + EGG.hot + 'cc';
      for (var i = 0; i < v.trails.length; i++) {
        var lag = 0.05 + i * 0.07, tx = vegasX(wt - lag, sz.w);
        v.trails[i].style.transform = 'translate3d(' + (tx + (wt > 1.1 ? (i ? -6 : 6) * land : 0)).toFixed(1) + 'px, 0px, 0px)';
        blur(v.trails[i], 6 + streak * 30 + i * 6, 1.5 + i);
        v.trails[i].style.opacity = (wt > 0 ? Math.max(0.18, Math.min(0.55, Math.abs(x - tx) / 200)) : 0).toFixed(3);
      }
      v.code.style.opacity = clamp01((wt - 0.9) / 0.4).toFixed(3);
      v.sub.style.opacity = clamp01((wt - 1.6) / 0.5).toFixed(3);
    }
)EGJS"
