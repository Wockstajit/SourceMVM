#pragma once

// The DRAG GRIP that resizes the right-hand inspector column, shared by CONFIG and the CAMERA
// EDITOR so both columns resize with the same gesture, the same bounds and the same feel.
//
// WHY IT IS NOT A SLIDER. Every other draggable control in this HUD is a Panorama `Slider`,
// because a Slider is the only panel type that reports a drag when the context gives us no
// mouse-move event. This one does not need that trick: C++ already publishes the raw OS cursor
// (st.mx / st.my / st.lmb / st.cursor) into the panel every frame for the value-tile drag pump,
// and an edge grip wants ABSOLUTE pointer-x, not a Slider's 0..1 fraction of its own width --
// which is a quantity that changes as you drag it. So it reads the same cursor pipe directly.
//
// WHY IT PERSISTS ON RELEASE, NOT PER FRAME. The width is stored through the console
// (`mirv_filmmaker settings ui inspectorW <px>`), and this HUD writes console output
// synchronously: a command per frame during a drag is the console-spam stall that the value
// tiles' feedback-loop guards exist to prevent. The layout follows the pointer live; only the
// final width is written down.

R"IRJS(
    // TELL THE OTHER PANELS, NOW. The docked timeline / camera / graph each live in their own
    // Panorama panel with their own state, and the width is only persisted when the drag ends --
    // so during the gesture a direct call is the only thing that is current. Every panel here
    // shares one script context, which is what makes `$.CamTimeline` reachable from CONFIG.
    // Each target is optional: the graph panel does not exist until the graph is opened once.
    function mvmPublishInspW(w) {
      try { if ($.CamTimeline && $.CamTimeline.setInspW) $.CamTimeline.setInspW(w); } catch (e) {}
      try { if ($.GraphExp && $.GraphExp.setInspW) $.GraphExp.setInspW(w); } catch (e) {}
    }

    // opts: {
    //   root      the panel the grip is parented to (full-screen overlay root)
    //   width()   current applied width, css px
    //   apply(px) lay the column out at this width -- called live during the drag
    //   commit(px)persist it -- called once, on release
    //   screenW() the surface width in css px, for the same cap C++ applies
    //   colour    accent for the active state (Config blue / editor amber)
    // }
    function mvmInspectorGrip(opts) {
      var GRIP_W = 7;          // the hit target
      // The painted rule is 2px and never changes width -- see place() and paint() below. The
      // scaled preview draws its own 2px frame edge ending exactly on the column's edge, so a
      // narrower or differently-placed rule showed as the line changing thickness and shifting
      // sideways at the top and bottom of the preview.
      var RULE_W = 2;
      var MIN_W = 600;         // matches InspectorWidth.h kMin
      var MAX_W = 1100;        // matches InspectorWidth.h kMax
      var SCREEN_FRAC = 0.62;  // matches InspectorWidth.h kMaxScreenFraction

      var grip = $.CreatePanel('Panel', opts.root, '', {});
      grip.hittest = true;
      grip.style.horizontalAlign = 'right';
      grip.style.verticalAlign = 'top';
      grip.style.width = GRIP_W + 'px';
      grip.style.height = '100%';
      grip.style.zIndex = '90';   // above the column's own chrome, or the edge is unhittable
      // The painted rule: 1px, on the OUTER edge of the 7px target. A grip you cannot see is a
      // grip nobody finds, but it has to be the column's edge and nothing else.
      //
      // LEFT-ALIGNED, NOT RIGHT. The target hangs inside the column (marginRight = w - 6), so a
      // right-aligned rule painted 6px IN from the column's edge -- while the scaled preview's
      // own frame paints its bright edge ON that edge, and only across the preview's height. The
      // result was two parallel vertical lines 6px apart, one of them stopping short: a resize bar
      // that reads as split in two rather than as one straight edge. Aligning the rule left puts
      // it flush against the frame's edge so the two read as a single line, in CONFIG and in the
      // CAMERA EDITOR alike (they share this control).
      var rule = $.CreatePanel('Panel', grip, '', {});
      rule.hittest = false;
      rule.style.width = RULE_W + 'px'; rule.style.height = '100%';
      rule.style.horizontalAlign = 'left';
      rule.style.backgroundColor = '#ffffff14';
      rule.style.transitionProperty = 'background-color';
      rule.style.transitionDuration = '0.12s';

      var hot = false, held = false, anchorX = 0, baseW = 0, moved = false, lastLmb = false;
      var liveW = 0;

      function paint() {
        // COLOUR ONLY. Growing the rule on hover moved its edge by a pixel, which is the same
        // "the line gets thicker then thinner" fault the width mismatch caused, just on hover.
        rule.style.backgroundColor = (held || hot)
          ? (opts.colour || '#4aa3ff')
          : '#ffffff14';
      }
      grip.SetPanelEvent('onmouseover', function () { hot = true; paint(); });
      grip.SetPanelEvent('onmouseout', function () { hot = false; paint(); });
      paint();

      function capFor(screenW) {
        var cap = MAX_W;
        if (screenW > 1) cap = Math.min(cap, Math.max(MIN_W, screenW * SCREEN_FRAC));
        return cap;
      }

      // Is the pointer on the grip? Its own rect, in window coordinates -- the same hit test the
      // value tiles do, and for the same reason: no mouse-move events, so Panorama's hit test
      // never runs for a drag.
      function over(x, y) {
        try {
          if (!grip.visible) return false;
          var r = grip.GetPositionWithinWindow ? grip.GetPositionWithinWindow() : null;
          if (!r) return false;
          var sx = grip.actualuiscale_x || 1, sy = grip.actualuiscale_y || 1;
          var w = (grip.actuallayoutwidth || 0), h = (grip.actuallayoutheight || 0);
          // Widen the target either side of the painted rule: a 7px edge is a hard thing to hit
          // on a 4K screen, and there is nothing else along this line to catch by mistake.
          var pad = 6 * sx;
          return x >= r.x - pad && x < r.x + w + pad && y >= r.y && y < r.y + h;
        } catch (e) { return false; }
      }

      return {
        panel: grip,
        // Keep the grip sitting on the column's edge.
        place: function (w) {
          liveW = w;
          // Park the target so its LEFT-aligned rule ends exactly on the column's edge, which is
          // where the preview's own frame edge ends: the two then paint the same 2px line at the
          // same x, and the edge reads as one straight rule for the full height of the screen
          // instead of stepping sideways and changing weight where the preview stops.
          grip.style.marginRight = Math.floor(w - (GRIP_W - RULE_W)) + 'px';
        },
        dragging: function () { return held && moved; },
        // Called once per render with the panel state. Returns true while it owns the pointer,
        // so the caller can keep other drag pumps off it.
        pump: function (st) {
          if (!st || typeof st.mx !== 'number') { held = false; lastLmb = false; return false; }
          // Only while the PANEL owns the cursor: in game-mouse mode the button is aiming.
          var lmb = !!st.lmb && !!st.cursor;
          var mx = st.mx;
          if (lmb && !lastLmb && over(mx, st.my)) {
            held = true; moved = false;
            anchorX = mx;
            baseW = liveW;
          }
          if (held) {
            var sx = grip.actualuiscale_x || 1;
            // The column is on the RIGHT, so dragging LEFT makes it wider: the delta is negated.
            var dx = (anchorX - mx) / sx;
            if (!moved && Math.abs(dx) > 2) moved = true;
            if (moved) {
              var w = baseW + dx;
              var cap = capFor(opts.screenW ? opts.screenW() : 0);
              if (w < MIN_W) w = MIN_W;
              if (w > cap) w = cap;
              w = Math.floor(w);
              if (w !== liveW) { liveW = w; opts.apply(w); }
            }
            if (!lmb) {
              // Released. Persist once -- never per frame; see the note at the top of this file.
              held = false;
              if (moved && opts.commit) opts.commit(liveW);
              moved = false;
              paint();
            }
          }
          lastLmb = lmb;
          if (held) paint();
          return held;
        }
      };
    }
)IRJS"
