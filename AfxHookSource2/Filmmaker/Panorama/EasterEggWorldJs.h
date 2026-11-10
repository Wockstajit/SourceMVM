#pragma once

// WORLD-EGG fragment of the easter-egg overlay (concatenated into kEasterEggOverlayJs, shares its
// scope). The screen-side halves of the world eggs; their in-world halves are C++
// (Filmmaker/EasterEggs/EasterEggWorld.h):
//   DISCO        sweeping coloured light beams from the top of the screen, a strobe on every beat
//                (120 BPM, the same tempo the C++ side recolours the sun to) and mirror-ball glints.
//   MICHAEL BAY  a warm flash over the screen for every explosion (st.bay counts them).
//   WES ANDERSON letterbox bars to 2:1, a peach wash, and a pastel chapter card each round.
// Display-only and hittest=false like everything else here.

R"EGJS(
    var worldLayer = layer('MvmEggWorld', '3');
    var BEAT = 60 / 120;
    var DISCO_COLORS = ['#ff28c8', '#28e6ff', '#ffe628', '#78ff3c', '#ff7814', '#a046ff'];

    // ---- disco ----
    var discoLayer = $.CreatePanel('Panel', worldLayer, '');
    discoLayer.hittest = false;
    discoLayer.style.width = '100%'; discoLayer.style.height = '100%';
    discoLayer.style.overflow = 'noclip noclip';
    discoLayer.visible = false;
    var strobe = $.CreatePanel('Panel', discoLayer, '');
    strobe.hittest = false;
    strobe.style.width = '100%'; strobe.style.height = '100%';
    strobe.style.opacity = '0';
    var beams = [];
    for (var bi = 0; bi < 6; bi++) {
      var bm = $.CreatePanel('Panel', discoLayer, '');
      bm.hittest = false;
      bm.style.width = '150px'; bm.style.height = '1600px';
      bm.style.horizontalAlign = 'left'; bm.style.verticalAlign = 'top';
      bm.style.transformOrigin = '50% 0%';
      bm.style.opacity = '0.22';
      beams.push({ p: bm, color: '', phase: bi * 1.1, speed: 0.6 + bi * 0.17, anchor: (bi + 0.5) / 6 });
    }
    var discoStart = 0, discoBeat = -1, glintAt = 0;
    function beamPaint(b, col) {
      if (b.color === col) return;
      b.color = col;
      try {
        b.p.style.backgroundColor = 'gradient( linear, 0% 0%, 0% 100%, from( ' + col + 'cc ), color-stop( 0.45, ' + col + '44 ), to( ' + col + '00 ) )';
      } catch (e) { b.p.style.backgroundColor = col + '33'; }
    }
    function discoFrame(t, sz) {
      var on = !!st.disco;
      discoLayer.visible = on;
      if (!on) { discoStart = 0; return; }
      if (!discoStart) { discoStart = t; discoBeat = -1; }
      var dt = t - discoStart, beat = Math.floor(dt / BEAT), inBeat = (dt % BEAT) / BEAT;
      if (beat !== discoBeat) {
        discoBeat = beat;
        strobe.style.backgroundColor = DISCO_COLORS[beat % DISCO_COLORS.length] + '33';
      }
      strobe.style.opacity = (Math.pow(1 - inBeat, 3)).toFixed(3);
      for (var i = 0; i < beams.length; i++) {
        var b = beams[i];
        beamPaint(b, DISCO_COLORS[(beat + i) % DISCO_COLORS.length]);
        var ang = Math.sin(dt * b.speed + b.phase) * 38;
        b.p.style.transform = 'translate3d(' + (b.anchor * sz.w - 75).toFixed(1) + 'px, -40px, 0px) rotateZ(' + ang.toFixed(1) + 'deg)';
        b.p.style.opacity = (0.14 + 0.12 * Math.pow(1 - inBeat, 2)).toFixed(3);
      }
      // Mirror-ball glints, through the sparkle pool (E-girl's update loop animates them).
      if (t >= glintAt) {
        glintAt = t + 0.06;
        spawn(Math.random() * sz.w, Math.random() * sz.h * 0.8, 0, 0, 0.35 + Math.random() * 0.3, 0.3 + Math.random() * 0.5);
      }
    }

    // ---- michael bay ----
    var bayFlash = $.CreatePanel('Panel', worldLayer, '');
    bayFlash.hittest = false;
    bayFlash.style.width = '100%'; bayFlash.style.height = '100%';
    // Flat, not a radial gradient: CS2 draws radial gradients as a hard-edged band.
    bayFlash.style.backgroundColor = '#ffa64a';
    bayFlash.style.opacity = '0';
    bayFlash.visible = false;
    var baySeen = 0, bayAt = -10;
    function bayFrame(t) {
      if ((st.bay || 0) !== baySeen) { baySeen = st.bay || 0; bayAt = t; }
      var k = (t - bayAt) / 0.55;
      bayFlash.visible = k < 1;
      if (k < 1) bayFlash.style.opacity = (0.45 * Math.pow(1 - k, 2)).toFixed(3);
    }

    // ---- wes anderson ----
    var wesLayer = $.CreatePanel('Panel', worldLayer, '');
    wesLayer.hittest = false;
    wesLayer.style.width = '100%'; wesLayer.style.height = '100%';
    wesLayer.visible = false;
    var wesWash = $.CreatePanel('Panel', wesLayer, '');
    wesWash.hittest = false;
    wesWash.style.width = '100%'; wesWash.style.height = '100%';
    wesWash.style.backgroundColor = '#ffcf9a16';
    function wesBar(valign) {
      var p = $.CreatePanel('Panel', wesLayer, '');
      p.hittest = false;
      p.style.width = '100%'; p.style.height = '0px';
      p.style.horizontalAlign = 'left'; p.style.verticalAlign = valign;
      p.style.backgroundColor = '#1b1410';
      return p;
    }
    var wesTop = wesBar('top'), wesBottom = wesBar('bottom');
    var card = $.CreatePanel('Panel', wesLayer, '');
    card.hittest = false;
    card.style.width = '100%'; card.style.height = '100%';
    card.style.backgroundColor = '#f4dca6';
    card.style.opacity = '0';
    card.visible = false;
    var cardKicker = lbl(card, '', '#a33b2b', 18);
    cardKicker.style.horizontalAlign = 'center'; cardKicker.style.verticalAlign = 'center';
    cardKicker.style.marginTop = '-70px'; cardKicker.style.letterSpacing = '9px'; cardKicker.style.fontWeight = 'bold';
    var cardTitle = lbl(card, '', '#3b2a1e', 52);
    cardTitle.style.horizontalAlign = 'center'; cardTitle.style.verticalAlign = 'center';
    cardTitle.style.marginTop = '10px'; cardTitle.style.letterSpacing = '3px'; cardTitle.style.fontWeight = 'bold';
    var cardRule = $.CreatePanel('Panel', card, '');
    cardRule.hittest = false;
    cardRule.style.width = '120px'; cardRule.style.height = '2px';
    cardRule.style.horizontalAlign = 'center'; cardRule.style.verticalAlign = 'center';
    cardRule.style.marginTop = '82px'; cardRule.style.backgroundColor = '#a33b2b';
    var chapterSeen = 0, cardAt = -10;
    var NUMBERS = [T('ONE'), T('TWO'), T('THREE'), T('FOUR'), T('FIVE'), T('SIX'), T('SEVEN'), T('EIGHT'), T('NINE'), T('TEN'), T('ELEVEN'), T('TWELVE')];
    // The card titles C++ sends (kChapters in EasterEggs.cpp, Title() in EasterEggWesMath.h).
    (function () {
      var X = EGG_TEXT;
      X['A Rather Unfortunate Rush'] = T('A Rather Unfortunate Rush');
      X['Concerning the Bomb'] = T('Concerning the Bomb');
      X['An Unexpected Flank'] = T('An Unexpected Flank');
      X['Smoke, Mirrors, and a Molotov'] = T('Smoke, Mirrors, and a Molotov');
      X['The Clutch (Abridged)'] = T('The Clutch (Abridged)');
      X['Several Grenades, Badly Thrown'] = T('Several Grenades, Badly Thrown');
      X['Overtime, Probably'] = T('Overtime, Probably');
      X['In Which Somebody Peeks'] = T('In Which Somebody Peeks');
      X['In Which Everyone Brings a Knife'] = T('In Which Everyone Brings a Knife');
      X['The Pistol Round'] = T('The Pistol Round');
      X['Everybody Buys Everything'] = T('Everybody Buys Everything');
      X['Nobody Buys Anything'] = T('Nobody Buys Anything');
      X['Two Force Buys, Badly Planned'] = T('Two Force Buys, Badly Planned');
      X['The Terrorists Count Their Pennies'] = T('The Terrorists Count Their Pennies');
      X['The Counter-Terrorists Count Their Pennies'] = T('The Counter-Terrorists Count Their Pennies');
      X['The Teams Count Their Pennies'] = T('The Teams Count Their Pennies');
      X['The Terrorists Force-Buy, Against All Advice'] = T('The Terrorists Force-Buy, Against All Advice');
      X['The Counter-Terrorists Force-Buy, Against All Advice'] = T('The Counter-Terrorists Force-Buy, Against All Advice');
      X['The Teams Force-Buy, Against All Advice'] = T('The Teams Force-Buy, Against All Advice');
      X['The Terrorists Spend Everything Anyway'] = T('The Terrorists Spend Everything Anyway');
      X['The Counter-Terrorists Spend Everything Anyway'] = T('The Counter-Terrorists Spend Everything Anyway');
      X['The Teams Spend Everything Anyway'] = T('The Teams Spend Everything Anyway');
    })();
    function wesFrame(t, sz) {
      var w = st.wes || {};
      wesLayer.visible = !!w.on;
      if (!w.on) { card.visible = false; return; }
      // Letterbox to 2:1 -- never taller than the screen already is.
      var barH = Math.max(0, (sz.h - sz.w / 2.0) / 2);
      wesTop.style.height = barH.toFixed(0) + 'px';
      wesBottom.style.height = barH.toFixed(0) + 'px';
      if ((w.chapter || 0) !== chapterSeen) {
        chapterSeen = w.chapter || 0;
        cardAt = t;
        var n = chapterSeen - 1;
        cardKicker.text = T('CHAPTER {0}', n >= 0 && n < NUMBERS.length ? NUMBERS[n] : String(chapterSeen));
        cardTitle.text = eggText(w.title);
      }
      var ct = t - cardAt, total = 2.7;
      card.visible = ct < total;
      if (ct < total) {
        var a = ct < 0.25 ? ct / 0.25 : (ct > total - 0.4 ? (total - ct) / 0.4 : 1);
        card.style.opacity = clamp01(a).toFixed(3);
      }
    }

    function worldFrame(t, sz) {
      discoFrame(t, sz);
      bayFrame(t);
      wesFrame(t, sz);
    }
)EGJS"
