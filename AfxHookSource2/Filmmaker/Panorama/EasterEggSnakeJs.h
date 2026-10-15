#pragma once

// SNAKE fragment of the easter-egg overlay (concatenated into kEasterEggOverlayJs, shares its scope;
// uses layer()/lbl()/cmd() from EasterEggOverlayJs.h). Draws the long-render Snake whose rules run in
// C++ (EasterEggs/EasterEggSnake.cpp, SnakeMath.h). The "snake" attribute on #MvmEggRoot carries,
// every frame while it is up, either the prompt ("ask": true + the render's estimated time left) or
// the board: size, body cells (head first), food cell + CS2 icon name, score, best, state, and the
// render's estimated time left.
//
// The prompt is a small card in the lower third, no dimming: the user may ignore it and keep
// watching the render. Its buttons and the Y/N/Enter/Esc keys (C++ side) answer it.
//
// The board is sized from the screen: cell = the largest whole size that fits the grid into about
// 42% of the width and 46% of the height (a corner-of-the-eye game, not a takeover), so it is never
// cut off at any aspect ratio.

R"EGJS(
    var snkLayer = layer('MvmEggSnake', '45');
    snkLayer.visible = false;
    var snkCache = new Map();
    function snkS(p, k, v) { var c = snkCache.get(p); if (!c) { c = {}; snkCache.set(p, c); } if (c[k] !== v) { c[k] = v; p.style[k] = v; } }
    function snkV(p, on) { var c = snkCache.get(p); if (!c) { c = {}; snkCache.set(p, c); } if (c.$v !== on) { c.$v = on; p.visible = on; } }
    function snkT(l, s) { var c = snkCache.get(l); if (!c) { c = {}; snkCache.set(l, c); } if (c.$t !== s) { c.$t = s; l.text = s; } }
    function snkPx(v) { return v.toFixed(1) + 'px'; }
    function snkBox(p, x, y, w, h) { snkS(p, 'position', snkPx(x) + ' ' + snkPx(y) + ' 0px'); snkS(p, 'width', snkPx(w)); snkS(p, 'height', snkPx(h)); }
    function snkPanel(parent, bg) {
      var p = $.CreatePanel('Panel', parent, '');
      p.hittest = false;
      p.style.horizontalAlign = 'left'; p.style.verticalAlign = 'top';
      p.style.overflow = 'noclip noclip';
      if (bg) p.style.backgroundColor = bg;
      return p;
    }
    // A label centred in a sized box of its own (bare positioned Labels render low/left in game).
    function snkLabel(parent, color, size) {
      var b = snkPanel(parent, null);
      var l = lbl(b, '', color, size);
      l.style.fontWeight = 'bold';
      l.style.horizontalAlign = 'center'; l.style.verticalAlign = 'center';
      try { l.style.whiteSpace = 'nowrap'; } catch (e) {}
      return { p: b, l: l };
    }

    var snkDim = snkPanel(snkLayer, '#05030ad8');
    var snkCard = snkPanel(snkLayer, '#120c1e');
    snkCard.style.border = '2px solid #7dff8c66';
    snkCard.style.borderRadius = '12px';
    var snkTitle = snkLabel(snkCard, '#7dff8c', 22);
    var snkScore = snkLabel(snkCard, '#ffffff', 15);
    var snkEta = snkLabel(snkCard, '#ffffff99', 12);
    var snkBoard = snkPanel(snkCard, '#07050c');
    snkBoard.style.border = '1px solid #ffffff22';
    var snkFoodGlow = snkPanel(snkBoard, '#ffd36b33');
    var snkFood = null;
    try {
      snkFood = $.CreatePanel('Image', snkBoard, '', { scaling: 'stretch-to-fit-preserve-aspect' });
      snkFood.hittest = false;
      snkFood.style.horizontalAlign = 'left'; snkFood.style.verticalAlign = 'top';
    } catch (ei) { snkFood = null; }
    var snkSegs = [];
    var snkEyes = [snkPanel(snkBoard, '#ffffff'), snkPanel(snkBoard, '#ffffff')];
    var snkPupils = [snkPanel(snkBoard, '#101010'), snkPanel(snkBoard, '#101010')];
    var snkTongue = snkPanel(snkBoard, '#ff4d6d');
    // Segments are created as the snake grows, so later panels would paint over the face: lift it.
    snkEyes[0].style.zIndex = '5'; snkEyes[1].style.zIndex = '5';
    snkPupils[0].style.zIndex = '6'; snkPupils[1].style.zIndex = '6'; snkTongue.style.zIndex = '4';
    var snkMsg = snkLabel(snkCard, '#ffffff', 16);
    var snkHelp = snkLabel(snkCard, '#ffffff80', 11);
    var snkIcon = '', snkStr = '', snkS0 = {};

    // The "play Snake while you wait?" prompt.
    var askCard = snkPanel(snkLayer, '#120c1ef0');
    askCard.style.border = '2px solid #7dff8c66';
    askCard.style.borderRadius = '12px';
    askCard.style.boxShadow = '#000000aa 0px 6px 24px 0px';
    var askTitle = snkLabel(askCard, '#7dff8c', 20);
    var askBody = snkLabel(askCard, '#ffffffd0', 14);
    function askButton(text, word, bg, fg) {
      var b = snkPanel(askCard, bg);
      b.hittest = true;
      b.style.borderRadius = '8px';
      var l = lbl(b, text, fg, 14);
      l.style.fontWeight = 'bold';
      l.style.horizontalAlign = 'center'; l.style.verticalAlign = 'center';
      b.style.border = '2px solid #00000000';
      b.SetPanelEvent('onmouseover', function () { b.style.border = '2px solid #ffffffcc'; });
      b.SetPanelEvent('onmouseout', function () { b.style.border = '2px solid #00000000'; });
      b.SetPanelEvent('onactivate', function () { cmd('mirv_filmmaker egg snake ' + word); });
      return b;
    }
    var askYes = askButton('play   Y', 'yes', '#3fcf5a', '#07130a');
    var askNo = askButton('no thanks   N', 'no', '#ffffff1c', '#ffffffcc');

    function askFrame(S, W, H) {
      var cw = 420, ch = 150, cx = (W - cw) / 2, cy = H * 0.66 - ch / 2;
      snkBox(askCard, cx, cy, cw, ch);
      snkBox(askTitle.p, 0, 14, cw, 28);
      snkT(askTitle.l, 'this render is going to take a while');
      snkBox(askBody.p, 0, 46, cw, 22);
      snkT(askBody.l, (S.eta >= 0 ? 'about ' + snkMmss(S.eta) + ' left. ' : '') + 'play snake while you wait?');
      var bw = 170, bh = 38, gap = 16, by = ch - bh - 18, bx = (cw - 2 * bw - gap) / 2;
      snkBox(askYes, bx, by, bw, bh);
      snkBox(askNo, bx + bw + gap, by, bw, bh);
    }

    function snkSeg(i) {
      while (snkSegs.length <= i) {
        var p = snkPanel(snkBoard, '#3fcf5a');
        p.visible = false;
        snkSegs.push(p);
      }
      return snkSegs[i];
    }
    function snkMmss(s) { s = Math.max(0, Math.round(s)); return Math.floor(s / 60) + ':' + (s % 60 < 10 ? '0' : '') + (s % 60); }

    function snakeFrame(t, sz) {
      var str = root.GetAttributeString('snake', '');
      if (!str) { snkV(snkLayer, false); return; }
      if (str !== snkStr) { snkStr = str; try { snkS0 = JSON.parse(str); } catch (e) { return; } }
      var S = snkS0;
      snkV(snkLayer, !!(S.on || S.ask));
      snkV(askCard, !!S.ask);
      snkV(snkDim, !!S.on); snkV(snkCard, !!S.on);
      if (S.ask) { askFrame(S, sz.w, sz.h); return; }
      if (!S.on) return;
      var W = sz.w, H = sz.h, gw = S.w || 28, gh = S.h || 18;
      // Fit: header 62 px + footer 40 px inside the card; the card inside ~42% x 46% of the screen.
      var head = 62, foot = 40, pad = 14;
      var cell = Math.floor(Math.min((W * 0.42 - 2 * pad) / gw, (H * 0.46 - head - foot) / gh));
      cell = Math.max(6, cell);
      var bw = cell * gw, bh = cell * gh, cw = bw + 2 * pad, ch = bh + head + foot;
      var cx = (W - cw) / 2, cy = (H - ch) / 2;
      snkBox(snkDim, 0, 0, W, H);
      snkBox(snkCard, cx, cy, cw, ch);
      snkBox(snkTitle.p, 0, 6, cw, 30); snkT(snkTitle.l, 'snake');
      snkBox(snkScore.p, 0, 34, cw, 22); snkT(snkScore.l, 'score ' + (S.score || 0) + '      best ' + (S.best || 0));
      snkBox(snkEta.p, cw - 190, 10, 180, 20);
      snkV(snkEta.p, S.eta >= 0);
      if (S.eta >= 0) snkT(snkEta.l, 'render ~' + snkMmss(S.eta) + ' left');
      snkBox(snkBoard, pad, head, bw, bh);
      snkS(snkBoard, 'borderRadius', '8px');

      // Food: the game icon on a soft glow, gently pulsing.
      var f = S.food || [0, 0], pulse = 1 + 0.08 * Math.sin(t * 6);
      var fs = cell * 0.92 * pulse, fx = f[0] * cell + (cell - fs) / 2, fy = f[1] * cell + (cell - fs) / 2;
      snkBox(snkFoodGlow, f[0] * cell + cell * 0.05, f[1] * cell + cell * 0.05, cell * 0.9, cell * 0.9);
      snkS(snkFoodGlow, 'borderRadius', snkPx(cell * 0.45));
      if (snkFood) {
        if (S.icon !== snkIcon) { snkIcon = S.icon; try { snkFood.SetImage('file://{images}/icons/equipment/' + S.icon + '.svg'); } catch (e2) {} }
        snkBox(snkFood, fx, fy, fs, fs);
      }

      // The snake: rounded green segments, darker toward the tail, a head with eyes and a tongue.
      var b = S.body || [], n = b.length / 2, dead = S.state === 'dead';
      for (var i = 0; i < n; i++) {
        var p = snkSeg(i), k = n > 1 ? i / (n - 1) : 0;
        var inset = i === 0 ? cell * 0.02 : cell * (0.08 + 0.06 * k);
        snkBox(p, b[2 * i] * cell + inset, b[2 * i + 1] * cell + inset, cell - 2 * inset, cell - 2 * inset);
        snkS(p, 'borderRadius', snkPx(i === 0 ? cell * 0.42 : cell * 0.3));
        var g = Math.round(207 - 90 * k), r = Math.round(63 - 30 * k);
        var col = dead ? '#8a8a8a' : '#' + (r < 16 ? '0' : '') + r.toString(16) + (g < 16 ? '0' : '') + g.toString(16) + '5a';
        snkS(p, 'backgroundColor', col);
        snkV(p, true);
      }
      for (var z = n; z < snkSegs.length; z++) snkV(snkSegs[z], false);
      if (n > 0) {
        var hx = b[0] * cell, hy = b[1] * cell, d = S.dir || 0;
        // Eyes sit toward the direction of travel, side by side across it.
        var fwd = [[0, -1], [1, 0], [0, 1], [-1, 0]][d & 3], side = [-fwd[1], fwd[0]];
        var e = cell * 0.22, pr = cell * 0.11;
        for (var q = 0; q < 2; q++) {
          var sgn = q === 0 ? -1 : 1;
          var ex = hx + cell / 2 + fwd[0] * cell * 0.15 + side[0] * sgn * cell * 0.2;
          var ey = hy + cell / 2 + fwd[1] * cell * 0.15 + side[1] * sgn * cell * 0.2;
          snkBox(snkEyes[q], ex - e / 2, ey - e / 2, e, e); snkS(snkEyes[q], 'borderRadius', snkPx(e / 2));
          snkBox(snkPupils[q], ex - pr / 2 + fwd[0] * e * 0.2, ey - pr / 2 + fwd[1] * e * 0.2, pr, pr);
          snkS(snkPupils[q], 'borderRadius', snkPx(pr / 2));
          snkV(snkEyes[q], true); snkV(snkPupils[q], !dead);
        }
        // A flicking tongue out front.
        var flick = (Math.sin(t * 9) > 0.2) && !dead;
        var tl = cell * 0.32, tw = Math.max(2, cell * 0.08);
        var tx = hx + cell / 2 + fwd[0] * cell * 0.5, ty = hy + cell / 2 + fwd[1] * cell * 0.5;
        if (fwd[0] !== 0) snkBox(snkTongue, fwd[0] > 0 ? tx : tx - tl, ty - tw / 2, tl, tw);
        else snkBox(snkTongue, tx - tw / 2, fwd[1] > 0 ? ty : ty - tl, tw, tl);
        snkV(snkTongue, flick);
      }

      var msg = S.state === 'ready' ? 'press an arrow key or W A S D to play'
        : S.state === 'dead' ? 'game over  -  ' + (S.score || 0) + ' points  -  press a direction to play again'
        : S.state === 'won' ? 'you filled the board. legend.' : '';
      snkV(snkMsg.p, !!msg);
      if (msg) { snkBox(snkMsg.p, 0, head + bh * 0.22, cw, 30); snkT(snkMsg.l, msg); } // above the snake's start row
      snkBox(snkHelp.p, 0, head + bh + 8, cw, 26);
      snkT(snkHelp.l, 'arrows / W A S D move    esc closes    the render keeps going');
    }
)EGJS"
