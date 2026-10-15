#pragma once

// THE DRAG GRIP that resizes the right-hand inspector column -- CS2's (Panorama/InspectorResizeJs.h),
// ported one-to-one. Same gesture, same bounds, same feel: a 7px hit target on the column's left
// edge with a 1px rule painted inside it, which brightens on hover and while held; drag LEFT to widen.
//
// WHY IT IS NOT A SLIDER. Every other draggable control in this HUD is a Panorama `Slider`, because
// a Slider is the only panel type that reports a drag when the context gives us no mouse-move event.
// This one does not need that trick: C++ already publishes the raw OS cursor (st.mx / st.my /
// st.lmb) into the panel every frame, and an edge grip wants ABSOLUTE pointer-x, not a Slider's
// 0..1 fraction of its own width -- a quantity that changes as you drag it.
//
// WHO OWNS THE WIDTH. Not this panel. C++ solves the layout (DeadlockWorkspaceLayoutMath.h) from a
// stored inspector width, and the blit that shrinks the world reads the same answer -- so the column
// cannot be laid out here and reported later, or the frame and the world would disagree for the
// length of the drag. Instead the grip SENDS the width (`mirv_filmmaker ui inspectorw <px>`),
// coalesced to one command per flush rather than one per frame, and the next frame's layout comes
// back solved. The final value is sent once more on release so the last pixel lands.
//
// THE FLOOR IS CS2's, NOT THE SOLVER's. CS2's InspectorWidth.h kMin is 360 (Panorama/InspectorWidth.cpp:
// below it the column's two-per-row value tiles stop fitting side by side), and this grip now enforces
// the same 360 for parity with the CS2 gesture. Deadlock's own WorkspaceLayout_ClampInspector floor is
// a looser 320 -- 360 sits comfortably above it, so nothing this grip lets you reach gets clamped back
// by the solver; a width the solver would additionally accept (320..359) is simply never reachable by
// dragging, the same one-sided relationship CS2 itself has against its own screen-fraction cap.

#define DEADLOCK_INSPECTOR_GRIP_JS R"CFJS(
(function () {
  if ($.MvmInspectorGrip) return;
  var M = $.MvmStyle;
  if (!M) return;
  var S = M.S;

  var GRIP_W = 7;            // the hit target; the painted rule inside it is 1px
  var MIN_W = 360;           // CS2 parity: InspectorWidth.h kMin (InspectorResizeJs.h's own MIN_W).
                             // Stricter than WorkspaceLayout_ClampInspector's own 320 floor -- see
                             // the header comment above.
  var MAX_W = 1100;
  var SCREEN_FRAC = 0.62;    // WorkspaceLayout_ClampInspector's cap

  function capFor(screenW) {
    var cap = MAX_W;
    if (screenW > 1) cap = Math.min(cap, Math.max(MIN_W, screenW * SCREEN_FRAC));
    return cap;
  }

  // The grip's own rect in WINDOW pixels, the space the cursor is published in.
  function gripRect(g) {
    var p = g.panel;
    try {
      var w = p.actuallayoutwidth || 0, h = p.actuallayoutheight || 0;
      var x = 0, y = 0;
      if (p.GetPositionWithinWindow) {
        var a = p.GetPositionWithinWindow(); x = a.x; y = a.y;
      } else {
        var q = p;
        for (var i = 0; q && i < 32; ++i) {
          x += (q.actualxoffset || 0); y += (q.actualyoffset || 0);
          q = q.GetParent ? q.GetParent() : null;
        }
      }
      return { x: x, y: y, w: w, h: h, sx: p.actualuiscale_x || 1 };
    } catch (e) { return null; }
  }

  // opts: { root, colour, screenW(), onWidth(px) }  -- onWidth is called with every live width
  // and again on release; the host decides how to send it.
  function build(opts) {
    var grip = M.make(opts.root, 'Panel', 'MvmInspectorGrip', {
      horizontalAlign: 'right', verticalAlign: 'top',
      width: GRIP_W + 'px', height: '100%',
      zIndex: 90   // above the column's own chrome, or the edge is unhittable
    });
    if (!grip) return null;
    grip.hittest = true;
    try { grip.SetPanelEvent('onactivate', function () {}); } catch (e) {}
    // The painted rule: 1px, centred in the 7px target. A grip you cannot see is a grip nobody
    // finds, but the column already has a border here, so this only brightens it.
    var rule = M.make(grip, 'Panel', '', {
      width: '1px', height: '100%', horizontalAlign: 'right',
      backgroundColor: '#ffffff14',
      transitionProperty: 'background-color', transitionDuration: '0.12s'
    });
    if (rule) rule.hittest = false;

    var g = {
      panel: grip, rule: rule, opts: opts,
      hot: false, held: false, anchorX: 0, baseW: 0, moved: false, lastLmb: false, liveW: 0
    };
    try {
      grip.SetPanelEvent('onmouseover', function () { g.hot = true; paint(g); });
      grip.SetPanelEvent('onmouseout', function () { g.hot = false; paint(g); });
    } catch (e) {}
    paint(g);
    return g;
  }

  function paint(g) {
    if (!g.rule) return;
    M.set(g.rule, {
      backgroundColor: (g.held || g.hot) ? (g.opts.colour || S.gold) : '#ffffff14',
      width: (g.held || g.hot) ? '2px' : '1px'
    });
  }

  // Keep the grip sitting on the column's edge. Called every frame with the SOLVED width.
  // h: the column's height (the render workspace ends it at the dock); omitted = full height.
  function place(g, w, h) {
    if (!g) return;
    g.liveW = w;
    M.set(g.panel, {
      marginRight: Math.floor(w - (GRIP_W - 1)) + 'px',
      height: (h > 0) ? (Math.floor(h) + 'px') : '100%'
    });
  }

  // Is the pointer on the grip? Widened either side of the painted rule: a 7px edge is a hard
  // thing to hit, and there is nothing else along this line to catch by mistake.
  function over(g, x, y) {
    var r = gripRect(g);
    if (!r || !g.panel.visible) return false;
    var pad = 6 * r.sx;
    return x >= r.x - pad && x < r.x + r.w + pad && y >= r.y && y < r.y + r.h;
  }

  // Once per render with the frame's state. Returns true while it owns the pointer, so the host
  // can keep other drag pumps off it.
  function pump(g, st) {
    if (!g || !st || typeof st.mx !== 'number') { if (g) { g.held = false; g.lastLmb = false; } return false; }
    // Only while the PANEL owns the cursor: in game-mouse mode the button is aiming.
    var lmb = !!st.lmb && !!st.uiMouse;
    var mx = st.mx;
    if (lmb && !g.lastLmb && over(g, mx, st.my)) {
      g.held = true; g.moved = false; g.anchorX = mx; g.baseW = g.liveW;
    }
    if (g.held) {
      var r = gripRect(g);
      var sx = (r && r.sx) || 1;
      // The column is on the RIGHT, so dragging LEFT makes it wider: the delta is negated.
      var dx = (g.anchorX - mx) / sx;
      if (!g.moved && Math.abs(dx) > 2) g.moved = true;
      if (g.moved) {
        var w = g.baseW + dx;
        var cap = capFor(g.opts.screenW ? g.opts.screenW() : 0);
        if (w < MIN_W) w = MIN_W;
        if (w > cap) w = cap;
        w = Math.floor(w);
        if (w !== g.liveW) { g.liveW = w; if (g.opts.onWidth) g.opts.onWidth(w, false); }
      }
      if (!lmb) {
        g.held = false;
        if (g.moved && g.opts.onWidth) g.opts.onWidth(g.liveW, true);
        g.moved = false;
        paint(g);
      }
    }
    g.lastLmb = lmb;
    if (g.held) paint(g);
    return g.held;
  }

  function dragging(g) { return !!(g && g.held && g.moved); }

  $.MvmInspectorGrip = { build: build, place: place, pump: pump, dragging: dragging };
})();
)CFJS"
