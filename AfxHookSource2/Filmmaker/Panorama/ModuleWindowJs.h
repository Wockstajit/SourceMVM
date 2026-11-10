#pragma once

// THE MODULE WINDOW -- the frame a workspace column sits in when the UI layout is MODULAR.
//
// Two layouts share one set of panels (Docs/panorama/modular-layout.md):
//   WORKSPACE  the column is docked on the right of a scaled game preview (the original layout).
//   MODULAR    the game stays full screen and each column floats in a window you can move, scale
//              and close, opened from the header bar along the top (WorkspaceHeaderJs.h).
//
// HOW IT KEEPS THE LOOK. The column is not rebuilt for the window. CONFIG and the Camera Editor
// build their column (ground, inspector, fades, scrollbar, RENDER / CUSTOMIZE PLAYER) inside
// `win.host` instead of directly inside their root. In WORKSPACE layout the host is the whole
// screen, so every right / top / bottom anchor lands exactly where it always did. In MODULAR
// layout the host is the window's body, so the same anchors land inside the window.
//
// SCALING, NOT RESIZING. The corner handle makes the WHOLE window bigger or smaller -- title bar,
// column, text and all -- with its proportions locked, so the layout inside never changes, it is
// only drawn at another size (the owner's request, 2026-11-07). It is Panorama's own `ui-scale`
// (CS2 shrinks its Rush HUD panel with `ui-scale: 40%`), not a transform: ui-scale lays the panel
// out smaller, so actuallayoutwidth and actualuiscale report what is drawn and every hit test in
// the column that compares GetPositionWithinWindow() + actuallayoutwidth against the cursor stays
// right. A transform would have drawn smaller while those numbers stayed full size.
//
// WHY THE HOST AND NOT THE ROOT. Root stays full screen at 0,0 in both layouts. Dropdowns, the
// death-animation picker and every other popup placed from GetPositionWithinWindow() are root
// children positioned in screen coordinates (CameraEditorWidgetsJs.h: "window-relative == root-
// relative"). Moving the root into the window would have shifted every one of them by the
// window's offset. Anything the column places INSIDE the host from a screen position (its
// scrollbar thumb, its top fade) converts through win.hostTop().
//
// NOT a standalone script: a raw-string chunk concatenated into a host IIFE. Needs `mk`, `lbl`,
// `cmd` and `S` (.value .hint .clear) in the host's scope.
//
//   var win = mvmModuleWindow(root, { key: 'config', title: 'CONFIG', accent: S.accent, z: 52,
//                                     side: 'left', onClose: function () { cmd('...'); } });
//   ...build the column inside win.host...
//   win.layout(st, { modular, rw, rh, w })   every frame; returns the host's rect (see below)
//   win.pump(st)                              title-bar move + corner scale, before any other
//                                             pointer consumer; true while it owns the press
//   win.over(mx, my)                          client-px point inside the window (wheel routing)
//   win.front()                               this window was pressed last (z order)
//   win.zIndex()                              the root's z: opts.z, or 54 while it is the front one
//   win.hostTop(panel)                        a panel's top edge in the host's own px
//   win.publish(root)                         the rect and front flag for C++ (UiLayout.h)
R"MWJS(
    // Shared by every window and by the header bar. headerH is the bar along the top of the screen
    // in MODULAR layout (WorkspaceHeaderJs.h); the window's design height leaves room for it and
    // for CS2's own demo bar (126, TimelineDock.h) at 100% scale.
    var MVM_WIN = { headerH: 44, titleH: 34, margin: 10, nativeDockH: 126, keepOnScreen: 140,
                    minScale: 0.45, maxScale: 1.25, gripSize: 18, cornerSize: 16, frontZ: 54 };

    // How tall the bar docked along the bottom is, for a window's bottomClear: the TIMELINE dock's
    // body (its live height while its top edge is being dragged, $.MvmDockBodyH), or CS2's demo
    // bar. In the MODULAR layout there is no ribbon above either.
    function mvmWinDockClear(tab, dockH) {
      if (tab === 'timeline') {
        if ($.MvmDockBodyH > 0) return $.MvmDockBodyH;
        return (dockH > 36) ? dockH - 36 : 440;
      }
      return 96;
    }

    // Every window, by key, so a press goes only to the one drawn on top where windows overlap:
    // the front one, else the higher root z (CONFIG 52 under the Camera Editor 53). Each window
    // reads the same raw cursor, and without this both took the press and the one pumped last
    // came to the front (Codex review on #137, 2026-11-07). A closed panel's panels are deleted,
    // so its entry stops answering; a reopened panel replaces it.
    if (!$.MvmWins) $.MvmWins = {};
    function mvmWinTopAt(px, py) {
      var best = null, bestZ = -1;
      for (var k in $.MvmWins) {
        var e = $.MvmWins[k], hit = false;
        try { hit = e.hit(px, py); } catch (eHit) { hit = false; }
        if (!hit) continue;
        var z = ($.MvmWinFront === k) ? MVM_WIN.frontZ : e.z;
        if (z > bestZ) { bestZ = z; best = k; }
      }
      return best;
    }

    // Surfaces drawn over every window: the TIMELINE dock, the demo bar, Customize Player, Editor
    // Settings, a confirm. Each registers a test of a client-px point under its own name in
    // $.MvmWinCovers, from its own script. A press that lands on one belongs to it, not to the
    // title bar, corner or close button of a window lying underneath: all of them read the same
    // raw cursor, so without this a drag in the dock rescaled the window below it.
    if (!$.MvmWinCovers) $.MvmWinCovers = {};
    function mvmWinCovered(px, py) {
      for (var k in $.MvmWinCovers) {
        var hit = false;
        // A cover whose panels were deleted throws or answers false: either way it covers nothing.
        try { hit = !!$.MvmWinCovers[k](px, py); } catch (eCover) { hit = false; }
        if (hit) return true;
      }
      return false;
    }

    function mvmModuleWindow(root, opts) {
      opts = opts || {};
      var key = opts.key || 'window';
      var accent = opts.accent || '#80bdffff';
      var TITLE_H = MVM_WIN.titleH, M = MVM_WIN.margin, GRIP = MVM_WIN.gripSize;

      // ---- panels -----------------------------------------------------------------------------
      // frame   unscaled, placed on the screen at the window's DRAWN size (design size x scale)
      //  win    the window at its design size, drawn at `scale` by ui-scale
      //   bar   the title bar
      //   host  the column's body
      //   grip  the corner scale handle, over the body's bottom-right corner
      // In WORKSPACE layout frame, win and host are all the full screen and nothing else shows.
      var frame = mk('Panel', root); frame.hittest = false;
      frame.style.horizontalAlign = 'left'; frame.style.verticalAlign = 'top';
      frame.style.position = '0px 0px 0px';
      frame.style.width = '100%'; frame.style.height = '100%';

      var win = mk('Panel', frame); win.hittest = false;
      win.style.horizontalAlign = 'left'; win.style.verticalAlign = 'top';
      win.style.width = '100%'; win.style.height = '100%';

      // Title bar: the workspace's accent as a 2px rule along the top (the preview frame's weight),
      // then the PREVIEW tag's grammar -- a dot and a bold, tracked caption -- and the wordmark's
      // code line on the right. Quiet on purpose: the window holds the column's own header chips.
      var bar = mk('Panel', win); bar.hittest = true; bar.visible = false;
      bar.style.width = '100%'; bar.style.height = TITLE_H + 'px';
      bar.style.flowChildren = 'right';
      bar.style.backgroundColor = '#0c0e12ff';
      bar.style.borderTop = '2px solid ' + accent;
      bar.style.borderBottom = '1px solid #ffffff14';
      // The right padding leaves the top-right corner to its scale handle (see corners below).
      bar.style.paddingLeft = '12px'; bar.style.paddingRight = (MVM_WIN.cornerSize + 2) + 'px';
      var dot = mk('Panel', bar); dot.hittest = false;
      dot.style.width = '7px'; dot.style.height = '7px'; dot.style.borderRadius = '4px';
      dot.style.backgroundColor = accent; dot.style.verticalAlign = 'center'; dot.style.marginRight = '9px';
      var title = lbl(bar, opts.title || '', S.value, 12); title.hittest = false;
      title.style.fontWeight = 'bold'; title.style.letterSpacing = '2px';
      title.style.verticalAlign = 'center'; title.style.whiteSpace = 'nowrap';
      var spacer = mk('Panel', bar); spacer.hittest = false;
      spacer.style.width = 'fill-parent-flow(1.0)'; spacer.style.height = '100%';
      // The scale, while it is not 100%: the one thing the corner handle changes.
      var scaleLbl = lbl(bar, '', S.hint, 11); scaleLbl.hittest = false;
      scaleLbl.style.fontFamily = 'Stratum2 Bold Monodigit, Stratum2, "Arial Unicode MS"';
      scaleLbl.style.verticalAlign = 'center'; scaleLbl.style.marginRight = '12px';
      var code = lbl(bar, opts.code || 'SOURCE // MVM', accent, 10); code.hittest = false;
      code.style.fontWeight = 'bold'; code.style.letterSpacing = '3px'; code.style.opacity = '0.7';
      code.style.verticalAlign = 'center'; code.style.whiteSpace = 'nowrap'; code.style.marginRight = '10px';
      // Close. A real button with its own hover, not part of the drag target: pump() leaves a
      // press that starts on it alone, so a click here never also moves the window.
      var close = mk('Panel', bar); close.hittest = true;
      close.style.width = '30px'; close.style.height = '26px'; close.style.verticalAlign = 'center';
      close.style.borderRadius = '3px';
      close.style.transitionProperty = 'background-color'; close.style.transitionDuration = '0.08s';
      var closeX = lbl(close, '✕', S.hint, 13); closeX.hittest = false;
      closeX.style.horizontalAlign = 'center'; closeX.style.verticalAlign = 'center';
      close.SetPanelEvent('onmouseover', function () { close.style.backgroundColor = '#ffffff1a'; closeX.style.color = S.value; });
      close.SetPanelEvent('onmouseout', function () { close.style.backgroundColor = S.clear; closeX.style.color = S.hint; });
      // The click itself is read from the cursor pipe in pump() (press and release both on the
      // button), like the title-bar drag. Panorama's onactivate never fired here in CS2: the
      // owner's clicks on CONFIG's close reached the panel (input.uiclick, 2026-11-07) and nothing
      // closed. The event is kept only to swallow the click.
      close.SetPanelEvent('onactivate', function () {});

      // The column's body.
      var host = mk('Panel', win); host.hittest = false;
      host.style.horizontalAlign = 'left'; host.style.verticalAlign = 'top';
      host.style.position = '0px 0px 0px';
      host.style.width = '100%'; host.style.height = '100%';
      host.style.overflow = 'clip';

      // Corner handles: drag any corner to scale the window, and the OPPOSITE corner stays where it
      // is. The bottom-right one is always drawn (three short diagonal rules, the familiar resize
      // mark); the other three show an L bracket in the accent while the pointer is on them. Each
      // sits in a spot that holds no control: the top two in the title bar's padding (the close
      // button stops short of the top-right one), the bottom two inside the column's own inset
      // around RENDER / CUSTOMIZE PLAYER (UI.action.inset).
      var CORNER = MVM_WIN.cornerSize;
      var corners = [];
      function mkCorner(h, v) {
        var size = (h === 'right' && v === 'bottom') ? GRIP : CORNER;
        var c = mk('Panel', win); c.hittest = true; c.visible = false;
        c.style.width = size + 'px'; c.style.height = size + 'px';
        c.style.horizontalAlign = h; c.style.verticalAlign = v;
        c.style.zIndex = '210';
        var rec = { panel: c, sx: (h === 'left') ? -1 : 1, sy: (v === 'top') ? -1 : 1, hot: false, marks: [], always: false };
        if (h === 'right' && v === 'bottom') {
          rec.always = true;
          c.style.marginRight = '2px'; c.style.marginBottom = '2px';
          for (var gi = 0; gi < 3; gi++) {
            var gm = mk('Panel', c); gm.hittest = false;
            var len = 6 + gi * 5;
            gm.style.width = len + 'px'; gm.style.height = '2px'; gm.style.borderRadius = '1px';
            gm.style.horizontalAlign = 'right'; gm.style.verticalAlign = 'bottom';
            gm.style.marginRight = '1px'; gm.style.marginBottom = (2 + gi * 4) + 'px';
            gm.style.transformOrigin = '100% 50%';
            gm.style.transform = 'rotateZ(-45deg)';
            rec.marks.push(gm);
          }
        } else {
          // An L bracket along the corner's two outer edges, 3px in from them.
          var a = mk('Panel', c), b = mk('Panel', c);
          a.hittest = false; b.hittest = false;
          a.style.width = '10px'; a.style.height = '2px'; b.style.width = '2px'; b.style.height = '10px';
          var mh = (h === 'left') ? 'marginLeft' : 'marginRight', mv = (v === 'top') ? 'marginTop' : 'marginBottom';
          [a, b].forEach(function (m) {
            m.style.horizontalAlign = h; m.style.verticalAlign = v;
            m.style[mh] = '3px'; m.style[mv] = '3px';
            rec.marks.push(m);
          });
        }
        c.SetPanelEvent('onmouseover', function () { rec.hot = true; paintGrip(); });
        c.SetPanelEvent('onmouseout', function () { rec.hot = false; paintGrip(); });
        c.SetPanelEvent('onactivate', function () { /* the pipe owns the drag */ });
        corners.push(rec);
        return rec;
      }
      mkCorner('left', 'top'); mkCorner('right', 'top'); mkCorner('left', 'bottom');
      var grip = mkCorner('right', 'bottom').panel;
      function showCorners(on) { for (var i = 0; i < corners.length; i++) corners[i].panel.visible = on; }
      function hitCorners(on) { for (var i = 0; i < corners.length; i++) corners[i].panel.hittest = on; }

)MWJS"
// Split here: MSVC caps a single raw string literal at ~16380 bytes (C2026). Adjacent literals are
// concatenated by the compiler, so this stays one JS blob.
R"MWJS(
      // ---- geometry ---------------------------------------------------------------------------
      // x, y: the window's top-left on screen (root layout px). scale: drawn size / design size.
      // The design size is the column's width (the operator's inspector width, shared by both
      // windows) by the height that fits the screen under the header bar above CS2's demo bar.
      var x = null, y = null, scale = 1, lastW = 0, storedSeen = null;
      // The value persist() just wrote, until the state hands it back. The write is a console
      // command, which runs a frame or more after it is issued; until then the state still carries
      // the PREVIOUS stored rect, and applying that as "new" is what snapped a window back to where
      // the drag began and then forward again on release (the owner's report, 2026-11-07).
      var pendingStore = null, pendingFrames = 0;
      var drag = null, lastLmb = false, modular = false, appliedSig = '', appliedScale = 1, pressInside = false;
      var lastPubRect = null, lastPubFront = null, lastScaleText = null, appliedHit = null;
      // The root's z while this is not the front window (the host's root.style.zIndex).
      var baseZ = (typeof opts.z === 'number') ? opts.z : 52;
      var designW = 0, designH = 0;

      function paintGrip() {
        for (var i = 0; i < corners.length; i++) {
          var c = corners[i];
          var on = c.hot || (drag && drag.kind === 'scale' && drag.c === c);
          var col = on ? accent : (c.always ? '#ffffff40' : S.clear);
          for (var j = 0; j < c.marks.length; j++) c.marks[j].style.backgroundColor = col;
        }
      }
      function clampScale(k, rh) {
        var maxK = MVM_WIN.maxScale;
        // Never taller than the screen under the header bar.
        if (designH > 0 && rh > 0) maxK = Math.min(maxK, Math.max(MVM_WIN.minScale, (rh - MVM_WIN.headerH - M) / designH));
        return Math.min(maxK, Math.max(MVM_WIN.minScale, k));
      }
      // Panorama's ui-scale. A panel that refuses the property falls back to a top-left transform,
      // which draws the same size but leaves hit tests at full size -- logged once, so a CS2
      // update that drops ui-scale shows up in the console rather than as clicks going astray.
      var uiScaleOk = true;
      function applyScale(k) {
        if (k === appliedScale) return;
        appliedScale = k;
        var pct = (Math.round(k * 1000) / 10) + '%';
        if (uiScaleOk) {
          // x, y and z, all three. Measured in CS2 2026-11-07: a bare number throws "Invalid value
          // for property 'ui-scale'", and ONE percentage is accepted but lays the panel out with no
          // height (actuallayoutheight 0), so the window would simply vanish. "50% 50% 100%" halves
          // the panel on both axes and its actualuiscale with it.
          try { win.style.uiScale = pct + ' ' + pct + ' 100%'; return; }
          catch (e) { uiScaleOk = false; $.Msg('[window] ui-scale refused (' + e + '), using a transform\n'); }
        }
        win.style.transformOrigin = '0% 0%';
        win.style.transform = (k === 1) ? 'none' : ('scale3d(' + k + ',' + k + ',1)');
      }
      function readStored(st) {
        var ui = (st && st.ui) || {};
        var v = ui['win.' + key];
        if (!v) return null;
        var p = ('' + v).split('_');
        if (p.length < 3) return null;
        var sx_ = parseFloat(p[0]), sy_ = parseFloat(p[1]), sk_ = parseFloat(p[2]);
        if (!isFinite(sx_) || !isFinite(sy_) || !isFinite(sk_) || sk_ <= 0) return null;
        return { x: sx_, y: sy_, k: sk_ / 100, raw: '' + v };
      }
      function persist() {
        // One key, underscores between the numbers (CS2's console tokenizer splits on commas):
        // x, y and the scale in whole percent.
        if (lastRw > 0) clampPos(lastRw, lastRh, lastBottomClear);
        var v = Math.round(x) + '_' + Math.round(y) + '_' + Math.round(scale * 100);
        storedSeen = v;
        pendingStore = v; pendingFrames = 0;
        cmd('mirv_filmmaker settings ui win.' + key + ' ' + v);
      }
      // bottomClear: the height of whatever is docked along the bottom (the TIMELINE dock or CS2's
      // demo bar), which draws over the windows. The title bar never goes below its top edge, so a
      // window cannot be lost under the dock (the owner's request, 2026-11-07): open the dock or
      // make it taller and a window under it moves up.
      var lastRw = 0, lastRh = 0, lastBottomClear = 0;
      function clampPos(rw, rh, bottomClear) {
        var dw = designW * scale;
        var keep = Math.min(MVM_WIN.keepOnScreen, dw);
        if (x > rw - keep) x = rw - keep;
        if (x < keep - dw) x = keep - dw;
        var floorY = rh - Math.max(M, bottomClear || 0) - TITLE_H * scale;
        if (y > floorY) y = floorY;
        if (y < MVM_WIN.headerH) y = MVM_WIN.headerH;
      }
      function placeDefault(rw) {
        y = MVM_WIN.headerH + M;
        x = (opts.side === 'left') ? M : (rw - designW * scale - M);
      }

      function rectOf(p) {
        if (!p || !p.visible) return null;
        var r = p.GetPositionWithinWindow ? p.GetPositionWithinWindow() : null;
        if (!r) return null;
        return { x: r.x, y: r.y, w: p.actuallayoutwidth || 0, h: p.actuallayoutheight || 0 };
      }
      function inRect(r, px, py) { return !!r && px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h; }
      function setWorkspace() {
        if (appliedSig === 'full') return;
        appliedSig = 'full';
        applyScale(1);
        frame.style.position = '0px 0px 0px'; frame.style.width = '100%'; frame.style.height = '100%';
        win.style.width = '100%'; win.style.height = '100%';
        win.style.backgroundColor = S.clear; win.style.border = '0px solid #00000000';
        win.style.borderRadius = '0px'; win.style.boxShadow = 'none';
        host.style.position = '0px 0px 0px'; host.style.width = '100%'; host.style.height = '100%';
        bar.visible = false; showCorners(false);
      }

)MWJS"
// Split here: MSVC caps a single raw string literal at ~16380 bytes (C2026). Adjacent literals are
// concatenated by the compiler, so this stays one JS blob.
R"MWJS(
      var api = {
        host: host, frame: frame, bar: bar, title: title,
        modular: function () { return modular; },
        scale: function () { return modular ? scale : 1; },
        front: function () { return $.MvmWinFront === key; },
        zIndex: function () { return '' + ((modular && $.MvmWinFront === key) ? MVM_WIN.frontZ : baseZ); },
        focus: function () { $.MvmWinFront = key; },
        over: function (px, py) { return modular && inRect(rectOf(frame), px, py); },
        setTitle: function (t) { if (title.text !== t) title.text = t; },
        // A panel's top edge in the host's own px: the scrollbar thumb and the top fade are host
        // children placed from where the scroll view is on screen.
        hostTop: function (panel) {
          var p = panel && panel.GetPositionWithinWindow ? panel.GetPositionWithinWindow() : null;
          if (!p || !isFinite(p.y)) return 0;
          var h = host.GetPositionWithinWindow ? host.GetPositionWithinWindow() : null;
          var s = host.actualuiscale_y || root.actualuiscale_y || 1;
          return (p.y - ((h && isFinite(h.y)) ? h.y : 0)) / s;
        },
        hide: function () {
          modular = false;
          drag = null;
          setWorkspace();
        },
        // Lay the window out for this frame. o: { modular, rw, rh, w }. Returns the host's rect:
        // x / y on screen and dw / dh its drawn size (root px), w / h its size in its OWN px --
        // what the column lays itself out against -- and k the scale.
        layout: function (st, o) {
          modular = !!o.modular;
          if (!modular) {
            drag = null;
            setWorkspace();
            return { modular: false, x: 0, y: 0, w: o.rw, h: o.rh, dw: o.rw, dh: o.rh, k: 1 };
          }
          var rw = o.rw, rh = o.rh, w = Math.floor(o.w);
          designW = w;
          designH = Math.max(320, Math.floor(rh - MVM_WIN.headerH - MVM_WIN.nativeDockH - M * 2));
          // A stored rect wins when it is new to us (first open, or written by another session);
          // never in the middle of our own drag.
          if (!drag) {
            var s = readStored(st);
            if (pendingStore !== null) {
              // Our own write is in flight: whatever the state says until it arrives is older.
              // Two seconds of frames without it and the write is given up on.
              if (s && s.raw === pendingStore) { pendingStore = null; storedSeen = s.raw; }
              else if (++pendingFrames > 240) pendingStore = null;
            } else if (s && s.raw !== storedSeen) { storedSeen = s.raw; x = s.x; y = s.y; scale = s.k; }
          }
          scale = clampScale(scale, rh);
          if (x === null) placeDefault(rw);
          // A new column width (set in the docked layout, or restored) keeps the window's RIGHT
          // edge where it is, so a right-hand window does not grow off the screen. A left-hand
          // window keeps its LEFT edge: shifted the same way, CONFIG slid off the left of the
          // screen by however much its column was widened in the docked layout.
          if (lastW > 0 && w !== lastW && opts.side !== 'left') x -= (w - lastW) * scale;
          lastW = w;
          lastRw = rw; lastRh = rh; lastBottomClear = o.bottomClear || 0;
          clampPos(rw, rh, lastBottomClear);
          var dw = Math.round(designW * scale), dh = Math.round(designH * scale);
          var hostH = designH - TITLE_H;
          var sig = Math.round(x) + ',' + Math.round(y) + ',' + designW + ',' + designH + ',' + scale.toFixed(4);
          if (sig !== appliedSig) {
            appliedSig = sig;
            frame.style.position = Math.round(x) + 'px ' + Math.round(y) + 'px 0px';
            frame.style.width = dw + 'px'; frame.style.height = dh + 'px';
            win.style.width = designW + 'px'; win.style.height = designH + 'px';
            // The column ground's base (WorkspaceShellJs.h MVM_COLUMN_BASE): the frame shows only
            // where the column does not, so it has to be the same ground or the window reads as two.
            win.style.backgroundColor = 'rgba(15,17,20,1)';
            win.style.border = '1px solid #ffffff1f';
            win.style.borderRadius = '4px';
            win.style.boxShadow = '#000000d9 0px 10px 32px 0px';
            host.style.position = '0px ' + TITLE_H + 'px 0px';
            host.style.width = designW + 'px'; host.style.height = hostH + 'px';
            bar.visible = true; showCorners(true);
            applyScale(scale);
          }
          var pctText = (Math.abs(scale - 1) < 0.005) ? '' : (Math.round(scale * 100) + '%');
          if (pctText !== lastScaleText) { lastScaleText = pctText; scaleLbl.text = pctText; }
          // Only on a change: a hittest write every frame on the button under the pointer is the
          // one thing the close button did that no other button in these panels does.
          var cur = !!(st && st.cursor);
          if (cur !== appliedHit) { appliedHit = cur; bar.hittest = cur; hitCorners(cur); close.hittest = cur; }
          return { modular: true, x: Math.round(x), y: Math.round(y + TITLE_H * scale),
                   w: designW, h: hostH, dw: dw, dh: Math.round(hostH * scale), k: scale };
        },
        // Title-bar move and corner scale from the OS-cursor pipe (st.mx / st.my / st.lmb): HUD JS
        // has no mouse-move event. Returns true while this window owns the press.
        pump: function (st) {
          if (!modular || !st || typeof st.mx !== 'number') { drag = null; lastLmb = false; return false; }
          var lmb = !!st.lmb && !!st.cursor;
          var mx = st.mx, my = st.my;
          var sx = frame.actualuiscale_x || 1, sy = frame.actualuiscale_y || 1;
          // Only the window drawn on top where the press lands takes it: its title bar, corners,
          // close button and the raise on release. A window under another one ignores the press,
          // and so does one under a surface drawn over the windows (mvmWinCovered).
          var pressed = lmb && !lastLmb;
          var onTop = pressed && !mvmWinCovered(mx, my) && mvmWinTopAt(mx, my) === key;
          if (onTop) {
            var cornerHit = null;
            for (var ci = 0; ci < corners.length && !cornerHit; ci++)
              if (inRect(rectOf(corners[ci].panel), mx, my)) cornerHit = corners[ci];
            if (inRect(rectOf(close), mx, my)) {
              drag = { kind: 'close', mx: mx, my: my, moved: false };
            } else if (cornerHit) {
              drag = { kind: 'scale', c: cornerHit, mx: mx, my: my, k: scale, x: x, y: y, moved: false };
              paintGrip();
            } else if (inRect(rectOf(bar), mx, my)) {
              drag = { kind: 'move', mx: mx, my: my, x: x, y: y, moved: false };
            }
            // Moving or scaling a window brings it to the front at once: nothing under the press
            // is a Panorama button that a z change could upset (those raise on release, below).
            if (drag && drag.kind !== 'close') $.MvmWinFront = key;
          }
          if (drag) {
            var dx = (mx - drag.mx) / sx, dy = (my - drag.my) / sy;
            if (!drag.moved && (Math.abs(mx - drag.mx) > 2 || Math.abs(my - drag.my) > 2)) drag.moved = true;
            if (drag.moved) {
              if (drag.kind === 'move') { x = drag.x + dx; y = drag.y + dy; }
              else if (drag.kind === 'scale' && designW > 0 && designH > 0) {
                // The corner follows the pointer along the window's diagonal: the average of how
                // far it moved outward across and outward down, each as a share of the design size
                // (sx / sy are -1 on the left / top). Whole percent, so the stored number and the
                // drawn size agree exactly.
                var c = drag.c;
                var k = drag.k + (c.sx * dx / designW + c.sy * dy / designH) / 2;
                var rh = (root.actuallayoutheight || 0) / (root.actualuiscale_y || 1);
                k = clampScale(k, rh);
                // A top corner moves the top edge: it stops at the header bar, not past it.
                if (c.sy < 0) k = Math.min(k, (drag.y + designH * drag.k - MVM_WIN.headerH) / designH);
                k = Math.round(Math.max(MVM_WIN.minScale, k) * 100) / 100;
                scale = k;
                // The opposite corner stays put.
                x = (c.sx < 0) ? drag.x + designW * (drag.k - k) : drag.x;
                y = (c.sy < 0) ? drag.y + designH * (drag.k - k) : drag.y;
              }
            }
            if (!lmb) {
              var was = drag; drag = null; paintGrip();
              if (was.kind === 'close') { if (inRect(rectOf(close), mx, my) && opts.onClose) opts.onClose(); }
              else if (was.moved) persist();
            }
          }
          // The window pressed last draws on top. Raised on the RELEASE, not the press: the root's
          // z order changing under a held button is not something a click should have to survive.
          if (!lmb && lastLmb && pressInside) $.MvmWinFront = key;
          if (pressed) pressInside = onTop;
          else if (!lmb) pressInside = false;
          lastLmb = lmb;
          return !!drag;
        },
        dragging: function () { return !!drag && drag.moved; },
        // Tell C++ where this window is, in client px ("x y w h", '' while it is not a window),
        // and whether it is the front one: MovieMode sends a click or a wheel notch to the window
        // under the pointer (Panorama/UiLayout.h). Written only when it changes.
        publish: function (rootPanel) {
          var s = '';
          if (modular) {
            var r = rectOf(frame);
            if (r && r.w > 0 && r.h > 0) s = Math.round(r.x) + ' ' + Math.round(r.y) + ' ' + Math.round(r.w) + ' ' + Math.round(r.h);
          }
          if (s !== lastPubRect) { lastPubRect = s; rootPanel.SetAttributeString('winrect', s); }
          var f = (modular && $.MvmWinFront === key) ? '1' : '0';
          if (f !== lastPubFront) { lastPubFront = f; rootPanel.SetAttributeString('winfront', f); }
        }
      };
      $.MvmWins[key] = {
        z: baseZ,
        hit: function (px, py) {
          if (!modular || (frame.IsValid && !frame.IsValid())) return false;
          return inRect(rectOf(frame), px, py);
        }
      };
      return api;
    }
)MWJS"
