#pragma once

// THE SPEED DIAL -- CS2's speed scrubber (Panorama/SpeedScrubberJs.h), ported one-to-one.
//
// One widget, three hosts, exactly as in CS2: the game's replay bar (DeadlockNativeHudJs.h), the
// camera timeline (DeadlockTimelineJs.h) and the graph (DeadlockGraphBodyJs.h). Each host builds
// one, calls pump() from its own render() with the frame's cursor, and calls setSpeed() with the
// engine's published speed. They share nothing but this factory.
//
// IT IS NOT A SLIDER, AND THAT IS THE WHOLE DIFFERENCE. The first Deadlock version was a Panorama
// Slider with the CS2 mapping bolted on, because this context has no mouse-move event. It moved,
// but it did not feel like the CS2 control: a Slider reports a value, not a gesture, so there was no
// hover state, no click-to-snap, no press-and-drag distinction, no momentum in the line field, and
// the Slider's own track and thumb drew through the dial. CS2 never needed the Slider because C++
// already publishes the raw OS cursor into the panel every frame (st.mx / st.my / st.lmb) -- and so
// does the Deadlock hook, for the graph's hit tests. This reads the same pipe.
//
// The interaction, unchanged from CS2: a LOGARITHMIC track, a drag that scrubs continuously and sets
// the line field flowing, a click (no travel) that snaps to one of five zones, a handle that grows
// and glows on hover and on drag, and a readout that hands the strip over to the zone labels while
// the pointer is on it.
//
// RANGE. Deadlock's own clamp, 0.1x..16x (DeadlockPlaybackSpeedMath.h), so no part of the track is
// dead. That range is NOT symmetric about 1x in log space -- 1x sits at 0.455 of the way along, not
// at the middle -- so nothing here may assume the centre means anything. Every landmark (the detent,
// the centre tick, the 1x caption, the colour pivot) is measured from sliderFor(1) instead, which is
// what stopped the captions sitting a tenth of a track away from the speeds they name.
//
// EVERY WRITE GOES THROUGH M.set. In the steady state nothing here writes at all; during a drag the
// line field, the handle and the readout are restyled, which is what a drag looks like. The
// vertex-buffer warning in DeadlockStyleJs.h applies to this file more than to any other.

#define DEADLOCK_SPEED_DIAL_JS R"CFJS(
(function () {
  if ($.MvmSpeedDial) return;
  var M = $.MvmStyle;
  if (!M) return;
  var S = M.S;

  // Deadlock's own clamp (DeadlockPlaybackSpeedMath.h), so no part of the track is dead.
  var MIN_SPEED = 0.1, MAX_SPEED = 16, RATIO = MAX_SPEED / MIN_SPEED;
  var SNAP = 0.02;            // half-width of the "this is exactly 1x" detent, in track fraction
  var CLICK_SLOP = 4;         // device px of travel that still reads as a click, not a drag
  var SP_SLOW = 26, SP_FAST = 8;   // line spacing at each end of the track
  // Blue at the slow end through neutral at 1x to gold at the fast end: Deadlock's own accents.
  var COL_SLOW = [26, 159, 255], COL_MID = [238, 242, 246], COL_FAST = [255, 237, 121];
  // The five click-to-snap presets, from Deadlock's own speed dropdown.
  var ZONES = [0.1, 0.25, 1, 4, 16];
  var ZONE_LBL = ['0.1x', '0.25x', '1x', '4x', '16x'];

  function c01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
  function lerp(a, b, t) { return a + (b - a) * t; }
  function sliderFor(sp) { return (sp > 0) ? c01(Math.log(sp / MIN_SPEED) / Math.log(RATIO)) : 0.5; }
  // WHERE 1x ACTUALLY IS. Not 0.5: the track is only centred on 1x when the range happens to
  // be symmetric about it in log space, and Deadlock's 0.1..16 is not. The detent, the centre
  // tick and the 1x caption all read this, so they cannot drift apart from each other or from
  // the handle.
  var ONE_AT = Math.log(1 / MIN_SPEED) / Math.log(RATIO);
  function speedFor(v) { v = c01(v); return (Math.abs(v - ONE_AT) < SNAP) ? 1 : MIN_SPEED * Math.pow(RATIO, v); }
  function fmtSpeed(sp) {
    if (sp >= 10) return sp.toFixed(1) + 'x';
    if (sp >= 1) return (Math.abs(sp - Math.round(sp)) < 0.005 ? sp.toFixed(0) : sp.toFixed(2)) + 'x';
    if (sp >= 0.1) return sp.toFixed(2) + 'x';
    return sp.toFixed(3) + 'x';
  }
  function rgba(c, a) { return 'rgba(' + (c[0] | 0) + ',' + (c[1] | 0) + ',' + (c[2] | 0) + ',' + a + ')'; }
  // Neutral AT 1x, not at the middle of the track. The colour is the fastest way to read which side
  // of normal speed you are on, so it has to pivot on the same point every other landmark does --
  // otherwise the handle goes gold slightly before it passes 1x.
  function tintFor(v) {
    var a, b, t;
    if (v < ONE_AT) { a = COL_SLOW; b = COL_MID; t = ONE_AT > 0 ? v / ONE_AT : 1; }
    else { a = COL_MID; b = COL_FAST; t = ONE_AT < 1 ? (v - ONE_AT) / (1 - ONE_AT) : 0; }
    return [lerp(a[0], b[0], c01(t)), lerp(a[1], b[1], c01(t)), lerp(a[2], b[2], c01(t))];
  }
  // ZONE GEOMETRY, DERIVED FROM THE SPEEDS RATHER THAN FROM EVEN FIFTHS.
  //
  // The captions used to be spaced at (i + 0.5)/5 while the handle is placed logarithmically,
  // so a caption sat over its own speed only by coincidence. CS2 buys that coincidence with a
  // range wider than its zone speeds; Deadlock's clamp does not allow it, so the end speeds
  // ARE the ends of the track and their captions sat a tenth of the track away from them.
  //
  // Each caption is centred on its speed, and the boundary between two zones is the midpoint
  // between them -- which is also the honest definition of "nearest preset".
  var ZONE_AT = [], ZONE_EDGE = [0];
  (function () {
    for (var i = 0; i < ZONES.length; ++i) ZONE_AT.push(sliderFor(ZONES[i]));
    for (var j = 1; j < ZONE_AT.length; ++j) ZONE_EDGE.push((ZONE_AT[j - 1] + ZONE_AT[j]) / 2);
    ZONE_EDGE.push(1);
  })();
  function zoneAt(f) {
    f = c01(f);
    for (var i = ZONES.length - 1; i > 0; --i) if (f >= ZONE_EDGE[i]) return i;
    return 0;
  }

  // Where the strip is, in WINDOW pixels -- the space the cursor is published in. Panorama's
  // GetPositionWithinWindow answers that directly; if this context's panel type lacks it, the
  // layout offsets are summed up the parent chain instead. Either way the result is compared to
  // st.mx / st.my, and dbg() exposes both so a mismatch can be seen rather than guessed at.
  function stripRect(d) {
    var p = d.strip;
    try {
      var sx = p.actualuiscale_x || 1, sy = p.actualuiscale_y || 1;
      var w = p.actuallayoutwidth || 0, h = p.actuallayoutheight || 0;
      var x = 0, y = 0;
      if (p.GetPositionWithinWindow) {
        var a = p.GetPositionWithinWindow();
        x = a.x; y = a.y;
      } else {
        var q = p;
        for (var g = 0; q && g < 32; ++g) {
          x += (q.actualxoffset || 0); y += (q.actualyoffset || 0);
          q = q.GetParent ? q.GetParent() : null;
        }
      }
      return { x: x, y: y, w: w, h: h, sx: sx, sy: sy };
    } catch (e) { return null; }
  }

  function build(parent, opts) {
    opts = opts || {};
    var W = opts.width || 250, H = opts.height || 30;
    var FONT = opts.font || S.fontSans;
    var POOL = Math.ceil(W / SP_FAST) + 3;

    var strip = M.make(parent, 'Panel', opts.id || '', {
      width: W + 'px', height: H + 'px',
      verticalAlign: 'center',
      borderRadius: '4px',
      border: '1px solid ' + S.border,
      backgroundColor: 'gradient( linear, 0% 0%, 100% 0%, from( #05070ae8 ),'
                     + ' color-stop( 0.5, #0b0f14b8 ), to( #05070ae8 ) )'
    });
    if (!strip) return null;
    strip.hittest = true;
    // Swallow the click: the gesture is cursor-driven, and a click that fell through would land on
    // whatever the host put underneath -- on the replay bar, that is the game's own scrubber.
    try { strip.SetPanelEvent('onactivate', function () {}); } catch (e) {}

    // Clipper for the scrolling line field. The handle's halo lives OUTSIDE it so the glow bleeds.
    var clip = M.make(strip, 'Panel', '', { width: '100%', height: '100%', overflow: 'clip' });
    if (clip) clip.hittest = false;
    var field = M.make(clip, 'Panel', '', {
      width: '100%', height: '100%',
      transitionProperty: 'opacity', transitionDuration: '0.10s', transitionTimingFunction: 'ease-out'
    });
    if (field) field.hittest = false;

    var lines = [];
    for (var i = 0; i < POOL; i++) {
      var p = M.make(field, 'Panel', '', {
        position: '0px 0px 0px', width: '2px', height: '100%', verticalAlign: 'center',
        transitionProperty: 'transform, width, height, background-color',
        transitionDuration: '0.13s', transitionTimingFunction: 'ease-out'
      });
      if (!p) break;
      p.hittest = false;
      lines.push(p);
    }

    var centreTick = M.make(clip, 'Panel', '', {
      width: '1px', height: '100%',
      position: (ONE_AT * W).toFixed(1) + 'px 0px 0px', backgroundColor: '#ffffff24'
    });
    if (centreTick) centreTick.hittest = false;

    var handle = M.make(strip, 'Panel', '', {
      width: '4px', height: '70%', verticalAlign: 'center', position: '0px 0px 0px',
      borderRadius: '2px',
      border: '1px solid #05070acc',   // keeps it from dissolving into the field
      transitionProperty: 'width, height, background-color, box-shadow',
      transitionDuration: '0.09s', transitionTimingFunction: 'ease-out'
    });
    if (handle) handle.hittest = false;

    // Readout. Centring is horizontalAlign/verticalAlign ONLY: a Label given width:100% anchors to
    // its parent's top-left and the alignment properties stop applying.
    var pill = M.make(strip, 'Panel', '', {
      width: '84px', height: (H - 10) + 'px',
      horizontalAlign: 'center', verticalAlign: 'center',
      backgroundColor: '#05070aff', borderRadius: '10px', border: '1px solid #ffffff22',
      transitionProperty: 'opacity', transitionDuration: '0.12s', transitionTimingFunction: 'ease-out'
    });
    if (pill) pill.hittest = false;
    var readout = M.label(pill, '', '1x', {
      horizontalAlign: 'center', verticalAlign: 'center',
      fontSize: '13px', fontFamily: FONT, color: '#eef2f6ff'
    });
    if (readout) readout.hittest = false;
)CFJS" R"CFJS(
    // Zone chrome: four dividers + five labels, INVISIBLE until the pointer is over the strip. At
    // rest this control is a readout and must stay one -- five permanent labels on a 250px strip is
    // noise -- so the affordance only appears when it is actionable. Created AFTER the pill so they
    // draw over it while it is faded back.
    var zoneTicks = [], zoneLabels = [], zw = W / ZONES.length, ly = Math.round((H - 14) / 2);
    for (var zt = 1; zt < ZONES.length; zt++) {
      var zdiv = M.make(strip, 'Panel', '', {
        position: (ZONE_EDGE[zt] * W).toFixed(1) + 'px 0px 0px', width: '1px', height: '100%',
        backgroundColor: '#ffffff3d', opacity: '0',
        transitionProperty: 'opacity', transitionDuration: '0.12s', transitionTimingFunction: 'ease-out'
      });
      if (zdiv) { zdiv.hittest = false; zoneTicks.push(zdiv); }
    }
    for (var zi = 0; zi < ZONES.length; zi++) {
      // CENTRED ON ITS OWN SPEED -- except at the ends, where a centred caption would hang half
      // off the strip. Clamping the box moved the text half a caption inward instead (measured:
      // "0.1x" drawn at 0.100 of the track against a speed at 0.000), so the end captions keep a
      // box at the edge and align their TEXT to it. The text then sits as close to its own speed
      // as the strip allows, and reads as the end of the range.
      var atEnd = (zi === 0) ? 'left' : ((zi === ZONES.length - 1) ? 'right' : 'center');
      var lx = (zi === 0) ? 0
             : ((zi === ZONES.length - 1) ? (W - zw)
             : (ZONE_AT[zi] * W - zw / 2));
      var zlab = M.label(strip, '', ZONE_LBL[zi], {
        // Explicit width + textAlign, NOT width:100% / an align property -- see the readout note.
        position: lx.toFixed(1) + 'px ' + ly + 'px 0px',
        width: zw.toFixed(1) + 'px',
        textAlign: atEnd, fontSize: '11px', fontWeight: 'bold', fontFamily: FONT,
        color: '#c6cedaff', opacity: '0',
        transitionProperty: 'opacity, color', transitionDuration: '0.12s', transitionTimingFunction: 'ease-out'
      });
      if (zlab) { zlab.hittest = false; zoneLabels.push(zlab); }
    }

    var d = {
      strip: strip, field: field, lines: lines, centreTick: centreTick, handle: handle,
      pill: pill, readout: readout, zoneTicks: zoneTicks, zoneLabels: zoneLabels, W: W, H: H,
      curV: 0.5, hot: false, dragging: false, pressX: 0, pressMoved: false,
      hotZone: -1, lastPaintedZone: -2,
      scroll: 0, vel: 0, flow: 0, smear: 0, lastSmear: -1,
      lastSpacing: -1, lastTintKey: '', lineEaseFast: false, lastLmb: false, lastSent: null,
      pendingSpeed: null, flushArmed: false,
      dbgLast: { mx: 0, my: 0, ax: -1, ay: -1, sw: 0, sh: 0, over: false, pumped: 0 }
    };
    layout(d, d.curV); paintChrome(d, d.curV);
    return d;
  }

  function setLineEase(d, fast) {
    if (d.lineEaseFast === fast) return;   // restyling the pool every frame would cost more than it saves
    d.lineEaseFast = fast;
    var dur = fast ? '0.02s' : '0.13s';
    for (var i = 0; i < d.lines.length; i++) M.set(d.lines[i], { transitionDuration: dur });
  }
  function setLineSmear(d, sm) {
    var w = Math.max(1, lerp(4.5, 1.5, d.curV)) * (1 + 2.2 * sm);
    for (var i = 0; i < d.lines.length; i++) {
      if (d.lines[i].__mvmOff) continue;
      M.set(d.lines[i], { width: w.toFixed(2) + 'px' });
    }
  }
  // Only writes when the (hover, hovered-zone) pair actually changes.
  function paintZones(d) {
    var z = (d.hot && !(d.dragging && d.pressMoved)) ? d.hotZone : -1;
    if (z === d.lastPaintedZone) return;
    d.lastPaintedZone = z;
    var on = (z >= 0);
    for (var i = 0; i < d.zoneTicks.length; i++) M.set(d.zoneTicks[i], { opacity: on ? '0.55' : '0' });
    for (var j = 0; j < d.zoneLabels.length; j++) {
      M.set(d.zoneLabels[j], {
        opacity: on ? (j === z ? '1' : '0.45') : '0',
        color: (on && j === z) ? rgba(tintFor(sliderFor(ZONES[j])), 1) : '#c6cedaff'
      });
    }
    if (d.pill) M.set(d.pill, { opacity: on ? '0.25' : '1' });
  }
  function layout(d, v) {
    var spacing = lerp(SP_SLOW, SP_FAST, v);
    var w = Math.max(1, lerp(4.5, 1.5, v));
    var tint = tintFor(v);
    var key = (tint[0] | 0) + ',' + (tint[1] | 0) + ',' + (tint[2] | 0);
    if (Math.abs(spacing - d.lastSpacing) < 0.05 && key === d.lastTintKey && d.smear === 0) return;
    d.lastSpacing = spacing; d.lastTintKey = key;
    for (var i = 0; i < d.lines.length; i++) {
      var p = d.lines[i], x = i * spacing;
      if (x > d.W + spacing * 2) { M.set(p, { opacity: '0' }); p.__mvmOff = true; continue; }
      var dist = Math.abs((x - d.W / 2) / (d.W / 2)); if (dist > 1) dist = 1;
      p.__mvmOff = false;
      M.set(p, {
        transform: 'translateX(' + x.toFixed(2) + 'px)',
        width: w.toFixed(2) + 'px',
        height: (92 - 46 * dist * dist).toFixed(1) + '%',
        opacity: '1',
        backgroundColor: rgba(tint, (0.26 + 0.5 * (1 - dist)).toFixed(3))
      });
    }
  }
  function paintChrome(d, v) {
    var tint = tintFor(v);
    var hw = d.dragging ? 9 : (d.hot ? 7 : 4);
    if (d.handle) {
      M.set(d.handle, {
        width: hw + 'px',
        height: (d.dragging ? 100 : (d.hot ? 88 : 70)) + '%',
        transform: 'translateX(' + (v * (d.W - hw)).toFixed(2) + 'px)',
        backgroundColor: rgba(tint, d.dragging ? 1 : (d.hot ? 0.98 : 0.88)),
        boxShadow: d.dragging ? (rgba(tint, 0.5) + ' 0px 0px 10px 1px')
                 : (d.hot ? (rgba(tint, 0.3) + ' 0px 0px 7px 0px')
                          : (rgba(tint, 0) + ' 0px 0px 0px 0px'))
      });
    }
    if (d.readout) {
      var t = fmtSpeed(speedFor(v));
      try { if (d.readout.text !== t) d.readout.text = t; } catch (e) {}
      M.set(d.readout, { color: rgba(tint, 1) });
    }
    if (d.centreTick) M.set(d.centreTick, { backgroundColor: (Math.abs(v - ONE_AT) < SNAP) ? '#ffffff99' : '#ffffff24' });
  }

  // THE ONLY DIFFERENCE FROM CS2'S applySpeed: the command is debounced. Here a console command
  // rides the marked-$.Msg bridge, a bounded 64-entry queue drained on the main thread, and a drag
  // that issued one per frame queued more than the game could retire -- the queue dropped its oldest
  // entries and the dial visibly lagged the pointer. The paint is immediate; the send is coalesced,
  // and the LAST value always lands.
  function applySpeed(d, v, force) {
    var sp = speedFor(v);
    // Relative guard: an absolute epsilon is meaningless at 10x and far too coarse at 0.1x.
    if (!force && d.lastSent !== null && Math.abs(sp - d.lastSent) < d.lastSent * 0.008) return;
    d.lastSent = sp;
    d.pendingSpeed = sp;
    if (d.flushArmed) return;
    d.flushArmed = true;
    $.Schedule(0.05, function () {
      d.flushArmed = false;
      if (typeof d.pendingSpeed !== 'number') return;
      var out = d.pendingSpeed; d.pendingSpeed = null;
      M.cmd('mirv_filmmaker speed ' + out.toFixed(4));
    });
  }
  // A click (no travel) is a preset, not a scrub. force=true because the relative guard would
  // otherwise swallow a small step such as 4x -> 10x mid-flick.
  function snapToZone(d, f) {
    d.curV = sliderFor(ZONES[zoneAt(f)]);
    d.vel = 0; d.flow = 0; setLineEase(d, false);
    layout(d, d.curV); paintChrome(d, d.curV); applySpeed(d, d.curV, true);
  }
)CFJS" R"CFJS(
  // One frame of input + motion. The host calls this from its own render(), so there is no
  // $.Schedule loop to retire. `st` is the frame's state: the cursor in window px, and uiMouse --
  // in game-mouse mode the button is aiming, so the dial must not take it.
  function pump(d, st) {
    if (!d || !st) return;
    try {
      if (d.strip.IsValid && !d.strip.IsValid()) return;
      var mx = st.mx || 0, my = st.my || 0, lmb = !!st.lmb && !!st.uiMouse;
      var a = stripRect(d);
      var dl = d.dbgLast;
      dl.mx = mx; dl.my = my; dl.pumped++;
      dl.ax = a ? a.x : -1; dl.ay = a ? a.y : -1; dl.sw = a ? a.w : 0; dl.sh = a ? a.h : 0;
      if (a && a.w > 0) {
        var over = (mx >= a.x && mx <= a.x + a.w && my >= a.y && my <= a.y + a.h);
        dl.over = over;
        if (over !== d.hot) { d.hot = over; paintChrome(d, d.curV); }
        d.hotZone = zoneAt((mx - a.x) / a.w);
        // The press must LAND on the strip to start a gesture; once started it keeps tracking even
        // if the pointer leaves, so a fast flick past the edge does not drop the gesture.
        if (lmb && !d.lastLmb && over) { d.dragging = true; d.pressX = mx; d.pressMoved = false; }
        if (!lmb) {
          // Released without travelling = a CLICK on a zone. Resolved from where the press LANDED,
          // not where the pointer ended up, so hand-shake cannot move the answer.
          if (d.dragging && !d.pressMoved) snapToZone(d, (d.pressX - a.x) / a.w);
          if (d.dragging) { d.dragging = false; paintChrome(d, d.curV); }
        }
        if (d.dragging) {
          if (!d.pressMoved && Math.abs(mx - d.pressX) > CLICK_SLOP) d.pressMoved = true;
          if (d.pressMoved) {
            var nv = c01((mx - a.x) / a.w);
            d.vel = nv - d.curV;
            if (Math.abs(d.vel) < 0.0004) d.vel = 0;   // held still mid-drag: stop smearing
            if (d.vel !== 0) {
              d.curV = nv;
              setLineEase(d, Math.abs(d.vel) > 0.012); // stop easing while the pointer is really moving
              layout(d, d.curV); paintChrome(d, d.curV); applySpeed(d, d.curV, false);
            }
          }
        } else if (d.lineEaseFast) {
          setLineEase(d, false); layout(d, d.curV);       // settle: restore the morph easing
        }
        paintZones(d);
        d.lastLmb = lmb;
      }

      // Flow is a RESPONSE to dragging, not a permanent animation: spin up from drag velocity, then
      // coast to a full stop. A strip that keeps sliding while untouched reads as broken.
      var spacing = lerp(SP_SLOW, SP_FAST, d.curV);
      d.flow += ((d.dragging ? d.vel * 900 : 0) - d.flow) * 0.18;
      if (Math.abs(d.flow) < 0.05) d.flow = 0;
      if (d.flow !== 0) {
        d.scroll -= d.flow * 0.016;
        if (d.scroll <= -spacing) d.scroll += spacing;
        else if (d.scroll > 0) d.scroll -= spacing;
        if (d.field) M.set(d.field, { transform: 'translateX(' + d.scroll.toFixed(2) + 'px)' });
      }

      // Smear: width + alpha, never a blur filter. A gaussian rewritten per frame on this
      // container is what took CS2 down when its native-bar version tried it.
      var target = d.dragging ? Math.min(1, Math.abs(d.vel) * 26) : 0;
      d.smear += (target - d.smear) * 0.3;
      if (d.smear < 0.01) d.smear = 0;
      d.vel *= 0.80;
      if (Math.abs(d.smear - d.lastSmear) > 0.04) {
        d.lastSmear = d.smear;
        if (d.field) M.set(d.field, { opacity: (1 - 0.35 * d.smear).toFixed(3) });
        setLineSmear(d, d.smear);
      }
    } catch (e) {}
  }

  // The engine's authoritative speed. Ignored while the user owns the gesture, so reading a value
  // the engine has not applied yet cannot yank the handle out from under the drag.
  function setSpeed(d, sp) {
    if (!d || d.dragging || !(sp > 0)) return;
    var v = sliderFor(sp);
    if (Math.abs(v - d.curV) < 0.0005) return;
    d.curV = v; d.lastSent = sp;
    layout(d, d.curV); paintChrome(d, d.curV);
  }

  // Last frame's raw input and the rect it was tested against. If the cursor's space and the
  // strip's ever stop agreeing the control goes silently dead, and this is the only way to see
  // which side moved:  mirv_filmmaker ui_eval "$.Msg($.MvmSpeedDial.dbg($.MvmNativeHud.dial()))"
  function dbg(d) { return d ? JSON.stringify(d.dbgLast) : 'no dial'; }

  $.MvmSpeedDial = {
    build: build, pump: pump, setSpeed: setSpeed, sync: setSpeed, dbg: dbg,
    speedFor: speedFor, sliderFor: sliderFor, fmt: fmtSpeed
  };
})();
)CFJS"
