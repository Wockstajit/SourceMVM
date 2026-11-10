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
      var hTitle = eggText(h.title), hSub = eggText(h.sub), hCode = eggText(h.code);
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

      var ghost = lbl(box, hTitle, accent, loud ? 60 : 50);
      ghost.style.fontWeight = 'bold'; ghost.style.letterSpacing = '7px';
      ghost.style.horizontalAlign = 'right'; ghost.style.verticalAlign = 'center';
      ghost.style.marginRight = '-40px'; hintRoom(ghost, 32, 20);
      ghost.style.opacity = '0';

      function titleLabel() {
        var l = lbl(box, hTitle, accent, titleSize);
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
      var subFull = lbl(box, hSub, '#ffffff', loud ? 14 : 13);
      subFull.style.marginLeft = '26px'; subFull.style.marginTop = (40 + titleSize + 14) + 'px';
      subFull.style.maxWidth = (boxW - 60) + 'px';
      subFull.style.opacity = '0';
      // The caption: plain, small, a step quieter than the clue.
      var code = lbl(box, hCode, loud ? EGG.cream : '#e8eef5', 12);
      code.style.marginLeft = '26px'; code.style.marginTop = (40 + titleSize + 14 + 22) + 'px';
      code.style.maxWidth = (boxW - 60) + 'px';
      code.style.textShadow = '0px 1px 4px 2.0 #000000cc';
      code.style.opacity = '0';

      hint = { box: box, ghost: ghost, code: code, trails: trails, title: title, sub: sub, scrim: scrim, subFull: subFull,
        loud: loud, accent: accent, start: now(), hold: loud ? 7.0 : 8.0, tw: 0, boxW: boxW,
        titleText: hTitle, subText: hSub, subShown: -1 };
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
// Split: MSVC caps one raw string literal at ~16 KB (C2026); adjacent literals concatenate.
R"EGJS(
    // The hints and the egg on/off banners C++ sends (kHints, kOnBanner, kOffBanner in
    // EasterEggs.cpp), by their exact English: title, code line, clue. References that are a name,
    // a file or a deliberate misspelling (UWU?, KAWAII.SYS, "are belong to us", peppy.wav) are not
    // listed, so they stay as written.
    (function () {
      var X = EGG_TEXT;
      // kHints
      X['whisper it'] = T('whisper it');
      X['the console gets lonely. be nice to it.'] = T('the console gets lonely. be nice to it.');
      X["IT'S DANGEROUS TO GO ALONE"] = T("IT'S DANGEROUS TO GO ALONE");
      X['take this'] = T('take this');
      X["three letters. very soft. you'll know."] = T("three letters. very soft. you'll know.");
      X["IT LOOKS LIKE YOU'RE..."] = T("IT LOOKS LIKE YOU'RE...");
      X['1997 called'] = T('1997 called');
      X["someone's waiting for you to stop clicking."] = T("someone's waiting for you to stop clicking.");
      X['HEY, LISTEN!'] = T('HEY, LISTEN!');
      X['idle hands'] = T('idle hands');
      X["sit still long enough and you'll get company."] = T("sit still long enough and you'll get company.");
      X['WOULD YOU KINDLY'] = T('WOULD YOU KINDLY');
      X['render responsibly'] = T('render responsibly');
      X['we can see your taskbar.'] = T('we can see your taskbar.');
      X['THIS IS FINE'] = T('THIS IS FINE');
      X["your render's fine. where it's headed... less so."] = T("your render's fine. where it's headed... less so.");
      X['THE CAKE IS A LIE'] = T('THE CAKE IS A LIE');
      X["the frames aren't, though."] = T("the frames aren't, though.");
      X['WAKE UP, NEO'] = T('WAKE UP, NEO');
      X['follow the sparkles'] = T('follow the sparkles');
      X['some words make the whole screen blush.'] = T('some words make the whole screen blush.');
      X['we mean all of them.'] = T('we mean all of them.');
      X['at least i have chicken'] = T('at least i have chicken');
      X['no plan. just vibes.'] = T('no plan. just vibes.');
      X['PRESS START'] = T('PRESS START');
      X['player 2'] = T('player 2');
      X['most people never see this one. hi.'] = T('most people never see this one. hi.');
      X['DO A BARREL ROLL'] = T('DO A BARREL ROLL');
      X["not that one. but you're close."] = T("not that one. but you're close.");
      X['a very old code still opens a very loud door.'] = T('a very old code still opens a very loud door.');
      X['THE EAGLE HAS LANDED'] = T('THE EAGLE HAS LANDED');
      X['some say it was staged. the bodies know better.'] = T('some say it was staged. the bodies know better.');
      X['four player split'] = T('four player split');
      X['remember when everyone was easier to hit?'] = T('remember when everyone was easier to hit?');
      X['WHY DID THE CHICKEN'] = T('WHY DID THE CHICKEN');
      X['cross the map'] = T('cross the map');
      X['forecast: cloudy, with a chance of poultry.'] = T('forecast: cloudy, with a chance of poultry.');
      X['MORE EXPLOSIONS'] = T('MORE EXPLOSIONS');
      X['budget: yes'] = T('budget: yes');
      X['every frag deserves a director who loves fire.'] = T('every frag deserves a director who loves fire.');
      X['PERFECTLY CENTERED'] = T('PERFECTLY CENTERED');
      X['chapter one'] = T('chapter one');
      X['a little yellow. very deliberate. ask nicely.'] = T('a little yellow. very deliberate. ask nicely.');
      X['CLICK THE CIRCLES'] = T('CLICK THE CIRCLES');
      X['three letters. one rhythm. the console keeps time.'] = T('three letters. one rhythm. the console keeps time.');
      X['DID YOU HEAR THAT'] = T('DID YOU HEAR THAT');
      X['probably nothing.'] = T('probably nothing.');
      X['SIGNAL LOST'] = T('SIGNAL LOST');
      X["we'll try again later."] = T("we'll try again later.");
      X['YOU WERE HERE'] = T('YOU WERE HERE');
      X['last session'] = T('last session');
      X['so were we.'] = T('so were we.');
      X['ARE YOU STILL WATCHING'] = T('ARE YOU STILL WATCHING');
      X['autoplay'] = T('autoplay');
      X['we are.'] = T('we are.');
      X['SLITHER'] = T('SLITHER');
      X['long exports'] = T('long exports');
      X['patience is rewarded. eventually.'] = T('patience is rewarded. eventually.');
      X['NOT YET'] = T('NOT YET');
      X['soon'] = T('soon');
      X['keep going.'] = T('keep going.');
      X['WE NOTICED'] = T('WE NOTICED');
      X["go to bed. or don't. we're not your mom."] = T("go to bed. or don't. we're not your mom.");
      X['SAY PLEASE'] = T('SAY PLEASE');
      X['the console has feelings too.'] = T('the console has feelings too.');
      // kOnBanner
      X['loaded'] = T('loaded');
      X["oh. it's pink now. say it again if you dare."] = T("oh. it's pink now. say it again if you dare.");
      X['the floor is yours.'] = T('the floor is yours.');
      X['ONE SMALL STEP'] = T('ONE SMALL STEP');
      X['gravity has left the chat.'] = T('gravity has left the chat.');
      X['BIGGER'] = T('BIGGER');
      X["easier to hit. you're welcome."] = T("easier to hit. you're welcome.");
      X['forecast: poultry'] = T('forecast: poultry');
      X["it's raining chickens. hallelujah."] = T("it's raining chickens. hallelujah.");
      X["every frag gets the director's cut."] = T("every frag gets the director's cut.");
      X['A FILM BY YOU'] = T('A FILM BY YOU');
      X['perfectly centered. a little yellow.'] = T('perfectly centered. a little yellow.');
      // kOffBanner
      X['BACK TO WORK'] = T('BACK TO WORK');
      X['kawaii.sys unloaded'] = T('kawaii.sys unloaded');
      X['the sparkles will remember this.'] = T('the sparkles will remember this.');
      X['LIGHTS UP'] = T('LIGHTS UP');
      X['last call'] = T('last call');
      X['everybody go home.'] = T('everybody go home.');
      X["we're back"] = T("we're back");
      X['gravity restored. mostly.'] = T('gravity restored. mostly.');
      X['HEADS DOWN'] = T('HEADS DOWN');
      X['back to scale'] = T('back to scale');
      X["everyone's ego returns to normal."] = T("everyone's ego returns to normal.");
      X['CLEAR SKIES'] = T('CLEAR SKIES');
      X['the flock moved on'] = T('the flock moved on');
      X['for now.'] = T('for now.');
      X['PRACTICAL EFFECTS'] = T('PRACTICAL EFFECTS');
      X['insurance called'] = T('insurance called');
      X['fine. no more fire.'] = T('fine. no more fire.');
      X['roll credits'] = T('roll credits');
      X['the end. or is it.'] = T('the end. or is it.');
    })();
)EGJS"
