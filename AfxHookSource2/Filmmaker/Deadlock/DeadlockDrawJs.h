#pragma once

// Vector primitives for Panorama: line segments, dots and diamonds, drawn out of ordinary panels.
//
// WHY THIS FILE EXISTS. The camera timeline draws keyframe diamonds and a playhead; the graph
// draws seven curves, their Bezier handles and a grid. Both need the same three shapes, and on the
// CS2 side both grew their own private copies of them -- so a fix to the way a segment is rotated
// had to be made twice, and was once made only once. Here they are one component.
//
// HOW A LINE IS DRAWN. Panorama has no canvas and no line element. A segment is a panel `len` wide
// and `thick` tall, anchored at the start point and rotated about its left edge:
//
//     transform-origin: 0% 50%;  transform: rotateZ(<angle>deg);
//
// The y is offset by half the thickness first, so the segment is CENTRED on the line rather than
// hanging below it. `transform-origin` must be a two-value percentage -- a pixel origin or a
// three-value one throws, and a Panorama style throw is silent and takes the rest of the script
// with it, so the panel simply stops drawing from that point down.
//
// WHY POOLS RATHER THAN RemoveAndDeleteChildren. The CS2 graph rebuilds its layers every frame by
// deleting every child and recreating them. That is several hundred panel allocations per frame,
// and Deadlock's HUD context is the same V8 the game's own HUD runs in. A pool keeps the panels
// and moves them: `p = pool.next()` hands back the next panel, creating one only the first time,
// and `pool.done()` collapses whatever was not claimed this frame. Steady state allocates nothing.
//
// A pool is capped. A curve that somehow asked for ten thousand segments would otherwise take the
// game down rather than draw badly, and drawing badly is the better failure.

#define DEADLOCK_DRAW_JS R"CFJS(
(function () {
  if ($.MvmDraw) return;
  var M = $.MvmStyle;
  if (!M) return;

  // A recycling child pool. `type` is the Panorama panel type; `cap` bounds it.
  function pool(parent, type, cap) {
    var items = [], used = 0;
    var limit = cap || 512;
    return {
      next: function (props) {
        if (used >= limit) return null;
        var p = items[used];
        if (!p) {
          p = M.make(parent, type || 'Panel', '', null);
          if (!p) return null;
          try { p.hittest = false; } catch (e) {}
          items[used] = p;
        }
        used++;
        // Only on a real change. In the graph this loop runs over hundreds of panels every frame,
        // and a redundant visible write still moves the panel through Panorama's draw list.
        if (p.__mvmShown !== true) {
          try { p.visible = true; p.__mvmShown = true; } catch (e) {}
        }
        if (props) M.set(p, props);
        return p;
      },
      // Hide everything this frame did not claim, then arm for the next frame.
      done: function () {
        for (var i = used; i < items.length; ++i) {
          if (items[i].__mvmShown === false) continue;
          try { items[i].visible = false; items[i].__mvmShown = false; } catch (e) {}
        }
        used = 0;
      },
      reset: function () { used = 0; },
      count: function () { return used; }
    };
  }

  // A segment from (x0,y0) to (x1,y1), `thick` px wide, in the parent's local coordinates.
  function seg(p, x0, y0, x1, y1, color, thick) {
    if (!p) return null;
    var t = thick || 1;
    var dx = x1 - x0, dy = y1 - y0;
    var len = Math.sqrt(dx * dx + dy * dy);
    if (len < 0.5) len = 0.5;
    var ang = Math.atan2(dy, dx) * 180 / Math.PI;
    M.set(p, {
      position: x0.toFixed(1) + 'px ' + (y0 - t / 2).toFixed(1) + 'px 0px',
      width: len.toFixed(1) + 'px',
      height: t + 'px',
      borderRadius: '0px',
      transformOrigin: '0% 50%',
      transform: 'rotateZ(' + ang.toFixed(2) + 'deg)',
      backgroundColor: color
    });
    return p;
  }

  // A round dot centred on (cx,cy).
  function dot(p, cx, cy, size, color) {
    if (!p) return null;
    M.set(p, {
      position: (cx - size / 2).toFixed(1) + 'px ' + (cy - size / 2).toFixed(1) + 'px 0px',
      width: size + 'px',
      height: size + 'px',
      borderRadius: (size / 2) + 'px',
      transform: 'none',
      backgroundColor: color,
      border: '0px solid ' + M.CLEAR
    });
    return p;
  }

  // A keyframe diamond: a square rotated 45 degrees about its own centre. `size` is the SQUARE's
  // edge, so the shape's visible diagonal is size * 1.414 -- the row that holds it has to be that
  // tall plus the border, or every diamond's top point is clipped.
  function diamond(p, cx, cy, size, color, borderColor) {
    if (!p) return null;
    M.set(p, {
      position: (cx - size / 2).toFixed(1) + 'px ' + (cy - size / 2).toFixed(1) + 'px 0px',
      width: size + 'px',
      height: size + 'px',
      borderRadius: '1px',
      transformOrigin: '50% 50%',
      transform: 'rotateZ(45deg)',
      backgroundColor: color,
      border: '1px solid ' + (borderColor || M.CLEAR)
    });
    return p;
  }

  // An axis-aligned rectangle, for grids, shading and rails.
  function rect(p, x, y, w, h, color) {
    if (!p) return null;
    M.set(p, {
      position: Math.floor(x) + 'px ' + Math.floor(y) + 'px 0px',
      width: Math.max(0, Math.floor(w)) + 'px',
      height: Math.max(0, Math.floor(h)) + 'px',
      transform: 'none',
      borderRadius: '0px',
      backgroundColor: color
    });
    return p;
  }

  // "1, 2, 5 times a power of ten" -- the step that puts a readable number of gridlines across a
  // span without the labels turning into 3.7 / 7.4 / 11.1.
  function niceStep(raw) {
    var r = Math.max(1, raw);
    var p10 = Math.pow(10, Math.floor(Math.log(r) / Math.LN10));
    var n = r / p10;
    var m = n < 1.5 ? 1 : (n < 3 ? 2 : (n < 7 ? 5 : 10));
    return Math.max(1, m * p10);
  }

  $.MvmDraw = {
    pool: pool,
    seg: seg,
    dot: dot,
    diamond: diamond,
    rect: rect,
    niceStep: niceStep
  };
})();
)CFJS"
