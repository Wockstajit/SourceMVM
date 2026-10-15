#pragma once

// CLIPPY fragment of the easter-egg overlay (concatenated into kEasterEggOverlayJs, shares its
// scope). Leave the game untouched for a few minutes -- anywhere -- and the Office 97 assistant
// slides in with his yellow balloon, saying one of a rotating set of lines (C++ picks the line and
// sends its text + answers in st.clippy). He is drawn from panels -- two nested rounded wire loops, googly eyes,
// eyebrows -- because there is no image asset to ship and no glyph that looks like him.
//
// The balloon's answers and the close box are the only hittest panels on the whole overlay;
// they issue `mirv_filmmaker egg clippy <answer>` and C++ hides him and does the honours.

R"EGJS(
    var clippy = null, clippyShown = false, clippySeen = -1;

    function clippyBuild() {
      var c = $.CreatePanel('Panel', root, 'MvmEggClippy');
      c.hittest = false;
      c.style.zIndex = '30';
      c.style.width = '560px'; c.style.height = '300px';
      c.style.horizontalAlign = 'right'; c.style.verticalAlign = 'bottom';
      c.style.marginRight = '36px'; c.style.marginBottom = '230px';
      c.style.overflow = 'noclip noclip';
      c.visible = false;

      // --- the balloon ---
      var bal = $.CreatePanel('Panel', c, '');
      bal.hittest = true;
      bal.style.width = '400px'; bal.style.height = '250px';
      bal.style.horizontalAlign = 'left'; bal.style.verticalAlign = 'top';
      bal.style.flowChildren = 'down';
      bal.style.backgroundColor = '#ffffe1';
      bal.style.border = '1px solid #000000';
      bal.style.borderRadius = '10px';
      bal.style.padding = '15px';
      bal.style.boxShadow = '#00000088 3px 4px 10px 0px';
      function text(s, size, weight, mt) {
        var l = lbl(bal, s, '#000000', size);
        l.style.fontWeight = weight;
        l.style.width = '100%';
        l.style.marginTop = mt + 'px';
        return l;
      }
      var tTitle = text('', 19, 'bold', 0);
      var tBody = text('', 16, 'normal', 8);
      var tAside = text('', 12, 'normal', 6); tAside.style.color = '#555555';

      function answer(label, word) {
        var b = $.CreatePanel('Panel', answers, '');
        b.hittest = true;
        b.style.width = '100%'; b.style.height = '30px'; b.style.marginTop = '7px';
        b.style.flowChildren = 'right';
        var dot = $.CreatePanel('Panel', b, '');
        dot.hittest = false;
        dot.style.width = '11px'; dot.style.height = '11px'; dot.style.borderRadius = '6px';
        dot.style.backgroundColor = '#2c64c8'; dot.style.verticalAlign = 'center'; dot.style.marginRight = '9px';
        dot.style.boxShadow = '#00000066 1px 1px 2px 0px';
        var l = lbl(b, label, '#1a3e9a', 16);
        l.style.verticalAlign = 'center';
        b.SetPanelEvent('onmouseover', function () { l.style.color = '#c8327a'; });
        b.SetPanelEvent('onmouseout', function () { l.style.color = '#1a3e9a'; });
        b.SetPanelEvent('onactivate', function () { cmd('mirv_filmmaker egg clippy ' + word); });
        return b;
      }
      // The answer rows are rebuilt for each line (clippyFill).
      var answers = $.CreatePanel('Panel', bal, '');
      answers.hittest = false; answers.hittestchildren = true;
      answers.style.width = '100%'; answers.style.flowChildren = 'down';

      var close = $.CreatePanel('Panel', c, '');
      close.hittest = true;
      close.style.width = '22px'; close.style.height = '22px';
      close.style.horizontalAlign = 'left'; close.style.verticalAlign = 'top';
      close.style.marginLeft = '372px'; close.style.marginTop = '6px';
      var cx = lbl(close, 'x', '#555555', 15);
      cx.style.horizontalAlign = 'center'; cx.style.verticalAlign = 'center'; cx.style.fontWeight = 'bold';
      close.SetPanelEvent('onactivate', function () { cmd('mirv_filmmaker egg clippy hide'); });

      // Balloon tail pointing at him.
      var tail = $.CreatePanel('Panel', c, '');
      tail.hittest = false;
      tail.style.width = '18px'; tail.style.height = '18px';
      tail.style.backgroundColor = '#ffffe1';
      tail.style.horizontalAlign = 'left'; tail.style.verticalAlign = 'top';
      tail.style.marginLeft = '392px'; tail.style.marginTop = '176px';
      tail.style.transform = 'rotateZ(-45deg)';

      // --- Clippy himself ---
      var guy = $.CreatePanel('Panel', c, '');
      guy.hittest = false;
      guy.style.width = '110px'; guy.style.height = '190px';
      guy.style.horizontalAlign = 'right'; guy.style.verticalAlign = 'bottom';
      guy.style.overflow = 'noclip noclip';
      function loop(w, h, ml, mt) {
        var p = $.CreatePanel('Panel', guy, '');
        p.hittest = false;
        p.style.width = w + 'px'; p.style.height = h + 'px';
        p.style.horizontalAlign = 'left'; p.style.verticalAlign = 'top';
        p.style.marginLeft = ml + 'px'; p.style.marginTop = mt + 'px';
        p.style.border = '6px solid #b8c0c8';
        p.style.borderRadius = (w / 2) + 'px';
        p.style.boxShadow = '#00000077 2px 3px 5px 0px';
        return p;
      }
      loop(64, 176, 22, 8);
      loop(40, 128, 34, 46);
      loop(22, 74, 43, 18);
      function eye(ml) {
        var e = $.CreatePanel('Panel', guy, '');
        e.hittest = false;
        e.style.width = '26px'; e.style.height = '26px'; e.style.borderRadius = '13px';
        e.style.backgroundColor = '#ffffff'; e.style.border = '1px solid #333333';
        e.style.horizontalAlign = 'left'; e.style.verticalAlign = 'top';
        e.style.marginLeft = ml + 'px'; e.style.marginTop = '46px';
        var p = $.CreatePanel('Panel', e, '');
        p.hittest = false;
        p.style.width = '11px'; p.style.height = '11px'; p.style.borderRadius = '6px';
        p.style.backgroundColor = '#111111';
        p.style.horizontalAlign = 'center'; p.style.verticalAlign = 'center';
        var brow = $.CreatePanel('Panel', guy, '');
        brow.hittest = false;
        brow.style.width = '22px'; brow.style.height = '4px'; brow.style.borderRadius = '2px';
        brow.style.backgroundColor = '#222222';
        brow.style.horizontalAlign = 'left'; brow.style.verticalAlign = 'top';
        brow.style.marginLeft = (ml + 2) + 'px'; brow.style.marginTop = '34px';
        return { pupil: p, brow: brow };
      }
      var eyes = [eye(26), eye(58)];
      return { c: c, guy: guy, eyes: eyes, start: 0, title: tTitle, body: tBody, aside: tAside,
               answers: answers, answer: answer };
    }

    // Puts the line C++ picked into the balloon. answers: ["label|word", ...]
    function clippyFill(k, line) {
      k.title.text = line.title || '';
      k.body.text = line.body || '';
      k.aside.text = line.aside || '';
      k.answers.RemoveAndDeleteChildren();
      var list = line.answers || [];
      for (var i = 0; i < list.length; i++) {
        var bar = ('' + list[i]).indexOf('|');
        if (bar < 0) continue;
        k.answer(list[i].substring(0, bar), list[i].substring(bar + 1));
      }
    }

    function clippyFrame(t, sz) {
      var show = !!(st.clippy && st.clippy.show);
      if (show && !clippyShown) {
        if (!clippy) clippy = clippyBuild();
        clippy.start = t;
        if (st.clippy.id !== clippySeen) { clippySeen = st.clippy.id; clippyFill(clippy, st.clippy); }
        clippy.c.visible = true;
      }
      if (!show && clippyShown && clippy) clippy.c.visible = false;
      clippyShown = show;
      if (!show || !clippy) return;
      var ct = t - clippy.start;
      // Slides in, then bobs and looks around like he owns the place. No blur: a gaussian that
      // settles at zero width made the whole panel draw nothing in game.
      var inP = clamp01(ct / 0.9);
      var x = (1 - expoOut(inP)) * 640;
      clippy.c.style.transform = 'translate3d(' + x.toFixed(1) + 'px, 0px, 0px)';
      clippy.c.style.opacity = clamp01(ct / 0.3).toFixed(3);
      clippy.guy.style.transform = 'translate3d(0px, ' + (Math.sin(ct * 2.2) * 5).toFixed(1) + 'px, 0px) rotateZ('
        + (Math.sin(ct * 1.3) * 4).toFixed(1) + 'deg)';
      var look = Math.sin(ct * 0.7), lx = (look * 6).toFixed(1), ly = (Math.cos(ct * 1.1) * 3).toFixed(1);
      // Every few seconds the eyebrows go up. You know the look.
      var raise = (ct % 4.5) < 0.5 ? -6 * Math.sin(((ct % 4.5) / 0.5) * Math.PI) : 0;
      for (var i = 0; i < clippy.eyes.length; i++) {
        clippy.eyes[i].pupil.style.transform = 'translate3d(' + lx + 'px, ' + ly + 'px, 0px)';
        clippy.eyes[i].brow.style.transform = 'translate3d(0px, ' + raise.toFixed(1) + 'px, 0px) rotateZ(' + (i ? 8 : -8) + 'deg)';
      }
    }
)EGJS"
