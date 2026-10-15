#pragma once

// THE DEMO SPEED SCRUBBER, as a reusable widget.
//
// JS fragment concatenated (via #include, as an adjacent raw string literal) into a host script --
// currently the camera timeline dock (CameraTimelineJs.h) and the graph editor dock
// (GraphEditorJs.h). Not a standalone script: it defines one builder and nothing else, and shares
// the host's IIFE scope.
//
// WHY IT EXISTS SEPARATELY FROM DemoBarButtonsJs.h. That file owns the control on CS2's NATIVE demo
// bar: it replaces #TimeScale inside #SpeedControls, publishes its own cursor attribute from
// DemoBarButtons.cpp, and drives itself from a $.Schedule loop with a generation token because it
// can be re-injected at any time. None of that applies inside our own docks, which already render
// every frame and already carry the raw cursor on their state. Bolting the dock version onto that
// file would have meant one script serving two hosts with two lifetimes and two input paths; this
// is the widget, and each host supplies the cursor and calls pump() from its own render().
//
// The interaction is deliberately identical to the native-bar control, because it IS the same
// control in a second place: a LOGARITHMIC 0.05x..20x track, a drag that scrubs continuously, a
// click that snaps to one of five zones, and a readout that hands the strip over to the zone labels
// while the pointer is on it.
//
//   var sp = buildSpeedScrubber(parent, { width: 250, height: 30, font: S.font });
//   sp.pump(st.mx, st.my, st.lmb);   // every frame, from render()
//   sp.setSpeed(demoTimeScale);      // whenever the engine's authoritative value is read
//
// Coordinates are CLIENT device px -- the space GetPositionWithinWindow() reports in and the space
// CameraBridge_GetUiCursor() publishes.

R"SPSJ(
    // ---- speed scrubber -------------------------------------------------
    function buildSpeedScrubber(parent, opts) {
      opts = opts || {};
      var W = opts.width || 250, H = opts.height || 30;
      var FONT = opts.font || 'Stratum2, "Arial Unicode MS"';
      var MIN_SPEED = 0.05, MAX_SPEED = 20, RATIO = MAX_SPEED / MIN_SPEED;
      var SNAP = 0.02;            // half-width of the "this is exactly 1x" detent, in track frac
      var CLICK_SLOP = 4;         // device px of travel that still reads as a click, not a drag
      var SP_SLOW = 26, SP_FAST = 8;   // line spacing at each end of the track
      var COL_SLOW = [78, 150, 255], COL_MID = [236, 241, 247], COL_FAST = [240, 179, 35];
      // Five zones, so the middle one is dead centre -- and the centre of the track is already
      // exactly where 1x lives. These five are picked so each speed's real handle position falls
      // essentially ON its zone's centre (0.116 / 0.269 / 0.5 / 0.731 / 0.884 vs centres
      // 0.1 / 0.3 / 0.5 / 0.7 / 0.9); 0.25/0.5/1/2/4 would all crowd the middle 46% of a log track.
      var ZONES = [0.1, 0.25, 1, 4, 10];
      var ZONE_LBL = ['0.1x', '0.25x', '1x', '4x', '10x'];
      var POOL = Math.ceil(W / SP_FAST) + 3;

      function c01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
      function lerp(a, b, t) { return a + (b - a) * t; }
      function speedFor(v) { v = c01(v); return (Math.abs(v - 0.5) < SNAP) ? 1 : MIN_SPEED * Math.pow(RATIO, v); }
      function sliderFor(sp) { return (sp > 0) ? c01(Math.log(sp / MIN_SPEED) / Math.log(RATIO)) : 0.5; }
      function fmtSpeed(sp) {
        if (sp >= 10) return sp.toFixed(1) + 'x';
        if (sp >= 1) return (Math.abs(sp - Math.round(sp)) < 0.005 ? sp.toFixed(0) : sp.toFixed(2)) + 'x';
        if (sp >= 0.1) return sp.toFixed(2) + 'x';
        return sp.toFixed(3) + 'x';
      }
      function rgba(c, a) { return 'rgba(' + (c[0] | 0) + ',' + (c[1] | 0) + ',' + (c[2] | 0) + ',' + a + ')'; }
      function tintFor(v) {
        var a, b, t;
        if (v < 0.5) { a = COL_SLOW; b = COL_MID; t = v / 0.5; }
        else { a = COL_MID; b = COL_FAST; t = (v - 0.5) / 0.5; }
        return [lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t)];
      }
      function P(host, id) { return $.CreatePanel('Panel', host, id || '', {}); }

      var strip = P(parent);
      strip.style.width = W + 'px'; strip.style.height = H + 'px';
      strip.style.verticalAlign = 'center';
      strip.hittest = true;
      strip.SetPanelEvent('onactivate', function () { /* swallow: the drag is cursor-driven */ });
      strip.style.borderRadius = '4px';
      strip.style.border = '1px solid #ffffff1a';
      strip.style.backgroundColor =
        'gradient( linear, 0% 0%, 100% 0%, from( #05070ae8 ), color-stop( 0.5, #0b0f14b8 ), to( #05070ae8 ) )';

      // Clipper for the scrolling line field. The handle's halo lives OUTSIDE it so the glow bleeds.
      var clip = P(strip); clip.hittest = false;
      clip.style.width = '100%'; clip.style.height = '100%'; clip.style.overflow = 'clip';
      var field = P(clip); field.hittest = false;
      field.style.width = '100%'; field.style.height = '100%';
      field.style.transitionProperty = 'opacity';
      field.style.transitionDuration = '0.10s';
      field.style.transitionTimingFunction = 'ease-out';

      var lines = [];
      for (var i = 0; i < POOL; i++) {
        var p = P(field); p.hittest = false;
        p.style.position = '0px 0px 0px';
        p.style.width = '2px'; p.style.height = '100%';
        p.style.verticalAlign = 'center';
        p.style.transitionProperty = 'transform, width, height, background-color';
        p.style.transitionDuration = '0.13s';
        p.style.transitionTimingFunction = 'ease-out';
        lines.push(p);
      }

      var centreTick = P(clip); centreTick.hittest = false;
      centreTick.style.width = '1px'; centreTick.style.height = '100%';
      centreTick.style.position = (W / 2) + 'px 0px 0px';
      centreTick.style.backgroundColor = '#ffffff24';

      var handle = P(strip); handle.hittest = false;
      handle.style.width = '4px'; handle.style.height = '70%';
      handle.style.verticalAlign = 'center'; handle.style.position = '0px 0px 0px';
      handle.style.borderRadius = '2px';
      handle.style.border = '1px solid #05070acc';   // keeps it from dissolving into the field
      handle.style.transitionProperty = 'width, height, background-color, box-shadow';
      handle.style.transitionDuration = '0.09s';
      handle.style.transitionTimingFunction = 'ease-out';

      // Readout. Centring is horizontalAlign/verticalAlign ONLY: a Label given width:100% anchors
      // to its parent's top-left and the alignment properties stop applying.
      var pill = P(strip); pill.hittest = false;
      pill.style.width = '84px'; pill.style.height = (H - 10) + 'px';
      pill.style.horizontalAlign = 'center'; pill.style.verticalAlign = 'center';
      pill.style.backgroundColor = '#05070aff';
      pill.style.borderRadius = '10px';
      pill.style.border = '1px solid #ffffff22';
      pill.style.transitionProperty = 'opacity';
      pill.style.transitionDuration = '0.12s';
      pill.style.transitionTimingFunction = 'ease-out';
      var readout = $.CreatePanel('Label', pill, '', {});
      readout.hittest = false; readout.text = '1x';
      readout.style.horizontalAlign = 'center'; readout.style.verticalAlign = 'center';
      readout.style.fontSize = '13px'; readout.style.fontFamily = FONT;
      readout.style.color = '#eef2f6ff';
)SPSJ"
R"SPSJ(
      // Zone chrome: four dividers + five labels, INVISIBLE until the pointer is over the strip. At
      // rest this control is a readout and must stay one -- five permanent labels on a 250px strip
      // is noise -- so the affordance only appears when it is actionable. Created AFTER the pill so
      // they draw over it while it is faded back.
      var zoneTicks = [], zoneLabels = [], zw = W / ZONES.length, ly = Math.round((H - 14) / 2);
      for (var zt = 1; zt < ZONES.length; zt++) {
        var zdiv = P(strip); zdiv.hittest = false;
        zdiv.style.position = (zt * zw).toFixed(1) + 'px 0px 0px';
        zdiv.style.width = '1px'; zdiv.style.height = '100%';
        zdiv.style.backgroundColor = '#ffffff3d'; zdiv.style.opacity = '0';
        zdiv.style.transitionProperty = 'opacity';
        zdiv.style.transitionDuration = '0.12s';
        zdiv.style.transitionTimingFunction = 'ease-out';
        zoneTicks.push(zdiv);
      }
      for (var zi = 0; zi < ZONES.length; zi++) {
        var zlab = $.CreatePanel('Label', strip, '', {});
        zlab.hittest = false; zlab.text = ZONE_LBL[zi];
        // Explicit width + textAlign, NOT width:100% / an align property -- see the readout note.
        zlab.style.position = (zi * zw).toFixed(1) + 'px ' + ly + 'px 0px';
        zlab.style.width = zw.toFixed(1) + 'px';
        zlab.style.textAlign = 'center';
        zlab.style.fontSize = '11px'; zlab.style.fontWeight = 'bold'; zlab.style.fontFamily = FONT;
        zlab.style.color = '#c6cedaff'; zlab.style.opacity = '0';
        zlab.style.transitionProperty = 'opacity, color';
        zlab.style.transitionDuration = '0.12s';
        zlab.style.transitionTimingFunction = 'ease-out';
        zoneLabels.push(zlab);
      }

      var curV = 0.5, hot = false, dragging = false;
      var pressX = 0, pressMoved = false, hotZone = -1, lastPaintedZone = -2;
      var scroll = 0, vel = 0, flow = 0, smear = 0, lastSmear = -1;
      var lastSpacing = -1, lastTintKey = '', lineEaseFast = false;
      var lastLmb = false, lastSent = null;

      function setLineEase(fast) {
        if (lineEaseFast === fast) return;   // restyling the pool every frame would cost more than it saves
        lineEaseFast = fast;
        var d = fast ? '0.02s' : '0.13s';
        for (var i = 0; i < POOL; i++) lines[i].style.transitionDuration = d;
      }
      function setLineSmear(sm) {
        var w = Math.max(1, lerp(4.5, 1.5, curV)) * (1 + 2.2 * sm);
        for (var i = 0; i < POOL; i++) {
          if (lines[i].style.opacity === '0') continue;
          lines[i].style.width = w.toFixed(2) + 'px';
        }
      }
      function zoneAt(f) {
        var i = Math.floor(c01(f) * ZONES.length);
        return i < 0 ? 0 : (i >= ZONES.length ? ZONES.length - 1 : i);
      }
      // Only writes when the (hover, hovered-zone) pair actually changes.
      function paintZones() {
        var z = (hot && !(dragging && pressMoved)) ? hotZone : -1;
        if (z === lastPaintedZone) return;
        lastPaintedZone = z;
        var on = (z >= 0);
        for (var i = 0; i < zoneTicks.length; i++) zoneTicks[i].style.opacity = on ? '0.55' : '0';
        for (var j = 0; j < zoneLabels.length; j++) {
          zoneLabels[j].style.opacity = on ? (j === z ? '1' : '0.45') : '0';
          zoneLabels[j].style.color = (on && j === z) ? rgba(tintFor(sliderFor(ZONES[j])), 1) : '#c6cedaff';
        }
        pill.style.opacity = on ? '0.25' : '1';
      }
      function layout(v) {
        var spacing = lerp(SP_SLOW, SP_FAST, v);
        var w = Math.max(1, lerp(4.5, 1.5, v));
        var tint = tintFor(v);
        var key = (tint[0] | 0) + ',' + (tint[1] | 0) + ',' + (tint[2] | 0);
        if (Math.abs(spacing - lastSpacing) < 0.05 && key === lastTintKey && smear === 0) return;
        lastSpacing = spacing; lastTintKey = key;
        for (var i = 0; i < POOL; i++) {
          var p = lines[i], x = i * spacing;
          if (x > W + spacing * 2) { p.style.opacity = '0'; continue; }
          var d = Math.abs((x - W / 2) / (W / 2)); if (d > 1) d = 1;
          p.style.transform = 'translateX(' + x.toFixed(2) + 'px)';
          p.style.width = w.toFixed(2) + 'px';
          p.style.height = (92 - 46 * d * d).toFixed(1) + '%';
          p.style.opacity = '1';
          p.style.backgroundColor = rgba(tint, (0.26 + 0.5 * (1 - d)).toFixed(3));
        }
      }
      function paintChrome(v) {
        var tint = tintFor(v);
        var hw = dragging ? 9 : (hot ? 7 : 4);
        handle.style.width = hw + 'px';
        handle.style.height = (dragging ? 100 : (hot ? 88 : 70)) + '%';
        handle.style.transform = 'translateX(' + (v * (W - hw)).toFixed(2) + 'px)';
        handle.style.backgroundColor = rgba(tint, dragging ? 1 : (hot ? 0.98 : 0.88));
        handle.style.boxShadow = dragging ? (rgba(tint, 0.5) + ' 0px 0px 10px 1px')
                               : (hot ? (rgba(tint, 0.3) + ' 0px 0px 7px 0px')
                                      : (rgba(tint, 0) + ' 0px 0px 0px 0px'));
        readout.text = fmtSpeed(speedFor(v));
        readout.style.color = rgba(tint, 1);
        centreTick.style.backgroundColor = (Math.abs(v - 0.5) < SNAP) ? '#ffffff99' : '#ffffff24';
      }
      function applySpeed(v, force) {
        var sp = speedFor(v);
        // Relative guard: an absolute epsilon is meaningless at 20x and far too coarse at 0.05x.
        if (!force && lastSent !== null && Math.abs(sp - lastSent) < lastSent * 0.008) return;
        lastSent = sp;
        // `mirv_filmmaker timescale`, NOT `demo_timescale` -- it forwards to the engine AND
        // records the value, which is the only way the other two bars carrying this same control
        // learn about it (Movie/DemoTimescale.h: demo_timescale is a command, not a convar, and
        // Panorama's GetDemoControllerState() is not available on this context panel).
        try { GameInterfaceAPI.ConsoleCommand('mirv_filmmaker timescale ' + sp.toFixed(4)); } catch (e) {}
      }
      // A click (no travel) is a preset, not a scrub. force=true because the relative guard would
      // otherwise swallow a small step such as 4x -> 10x mid-flick.
      function snapToZone(f) {
        curV = sliderFor(ZONES[zoneAt(f)]);
        vel = 0; flow = 0; setLineEase(false);
        layout(curV); paintChrome(curV); applySpeed(curV, true);
      }
)SPSJ"
R"SPSJ(
      // One frame of input + motion. The host calls this from its own render(), so there is no
      // $.Schedule loop to retire and no generation token to police.
      // Last frame's raw input and the rect it was tested against, for
      //   mirv_filmmaker camtl eval $.Msg($.CamTimeline.dbgSpeed()+String.fromCharCode(10));
      // The cursor arrives as CLIENT device px and the rect comes from GetPositionWithinWindow;
      // if those two ever stop agreeing, the control goes silently dead and this is the only way
      // to see which side moved. (Console eval cannot carry string literals through the CS2
      // tokenizer, so a probe like this has to exist in the script, not in the command.)
      var dbgLast = { mx: 0, my: 0, ax: -1, ay: -1, sw: 0, sh: 0, over: false, pumped: 0 };
      function pump(mx, my, lmb) {
        try {
          if (strip.IsValid && !strip.IsValid()) return;
          var a = strip.GetPositionWithinWindow ? strip.GetPositionWithinWindow() : null;
          var sw = strip.actuallayoutwidth || 0, sh = strip.actuallayoutheight || 0;
          dbgLast.mx = mx; dbgLast.my = my; dbgLast.sw = sw; dbgLast.sh = sh; dbgLast.pumped++;
          dbgLast.ax = a ? a.x : -1; dbgLast.ay = a ? a.y : -1;
          dbgLast.over = !!(a && sw > 0 && mx >= a.x && mx <= a.x + sw && my >= a.y && my <= a.y + sh);
          if (a && sw > 0) {
            var over = (mx >= a.x && mx <= a.x + sw && my >= a.y && my <= a.y + sh);
            if (over !== hot) { hot = over; paintChrome(curV); }
            hotZone = zoneAt((mx - a.x) / sw);
            // The press must LAND on the strip to start a gesture; once started it keeps tracking
            // even if the pointer leaves, so a fast flick past the edge does not drop the gesture.
            if (lmb && !lastLmb && over) { dragging = true; pressX = mx; pressMoved = false; }
            if (!lmb) {
              // Released without travelling = a CLICK on a zone. Resolved from where the press
              // LANDED, not where the pointer ended up, so hand-shake cannot move the answer.
              if (dragging && !pressMoved) snapToZone((pressX - a.x) / sw);
              dragging = false;
            }
            if (dragging) {
              if (!pressMoved && Math.abs(mx - pressX) > CLICK_SLOP) pressMoved = true;
              if (pressMoved) {
                var nv = c01((mx - a.x) / sw);
                vel = nv - curV;
                if (Math.abs(vel) < 0.0004) vel = 0;   // held still mid-drag: stop smearing
                if (vel !== 0) {
                  curV = nv;
                  setLineEase(Math.abs(vel) > 0.012); // stop easing while the pointer is really moving
                  layout(curV); paintChrome(curV); applySpeed(curV, false);
                }
              }
            } else if (lineEaseFast) {
              setLineEase(false); layout(curV);       // settle: restore the morph easing
            }
            paintZones();
            lastLmb = lmb;
          }

          // Flow is a RESPONSE to dragging, not a permanent animation: spin up from drag velocity,
          // then coast to a full stop. A strip that keeps sliding while untouched reads as broken.
          var spacing = lerp(SP_SLOW, SP_FAST, curV);
          flow += ((dragging ? vel * 900 : 0) - flow) * 0.18;
          if (Math.abs(flow) < 0.05) flow = 0;
          if (flow !== 0) {
            scroll -= flow * 0.016;
            if (scroll <= -spacing) scroll += spacing;
            else if (scroll > 0) scroll -= spacing;
            field.style.transform = 'translateX(' + scroll.toFixed(2) + 'px)';
          }

          // Smear: width + alpha, never a blur filter. A gaussian rewritten per frame on this
          // container is what took the game down when the native-bar version tried it.
          var target = dragging ? Math.min(1, Math.abs(vel) * 26) : 0;
          smear += (target - smear) * 0.3;
          if (smear < 0.01) smear = 0;
          vel *= 0.80;
          if (Math.abs(smear - lastSmear) > 0.04) {
            lastSmear = smear;
            field.style.opacity = (1 - 0.35 * smear).toFixed(3);
            setLineSmear(smear);
          }
        } catch (e) {}
      }
      // The engine's authoritative timescale. Ignored while the user owns the gesture, so reading
      // a value the engine has not applied yet cannot yank the handle out from under the drag.
      function setSpeed(sp) {
        if (dragging || !(sp > 0)) return;
        var v = sliderFor(sp);
        if (Math.abs(v - curV) < 0.0005) return;
        curV = v; lastSent = sp;
        layout(curV); paintChrome(curV);
      }

      layout(curV); paintChrome(curV);
      return { strip: strip, pump: pump, setSpeed: setSpeed,
               dbg: function () { return JSON.stringify(dbgLast); } };
    }
)SPSJ"
