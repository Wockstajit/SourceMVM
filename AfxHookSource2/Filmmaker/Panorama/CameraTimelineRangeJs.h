#pragma once

// Render-range overlay for the REGULAR (native CS2) demo timeline.
//
// Panorama JS fragment concatenated (via #include, as an adjacent raw string literal) into
// kCameraTimelineJs in CameraTimelineJs.h. Not a standalone script -- it shares the enclosing
// IIFE's scope (ctx, S, mk, lbl, cmd, findNative, clamp01).
//
// WHAT IT DRAWS, over CS2's own demo bar, while the editor's bottom mode is Regular Timeline:
// a single selected-range highlight plus two draggable FLAGS (In / Out) that write the range
// back. Their native Slider children are input plumbing and must remain visually imperceptible;
// there is no slider track, field, or drag-time label.
//
// WHY ITS OWN ROOT. The previous attempt parented the highlight to #CamTimelineRoot, whose
// visibility is `st.open && !previewHidden` -- and Regular Timeline mode is precisely the mode
// where st.open is FALSE. The controls therefore own #CamTimelineRangeRoot instead.
//
// WHY DIRECT FLAGS WORK. CameraTimelineHud publishes the raw cursor and button state each frame,
// so the painted flag can own the hit target while that cursor pipe owns drag motion. Keeping the
// visual and the input owner on the same panel prevents clicks falling through to the native bar.
//
// WHY THE RAILS ONLY OCCUPY THE TOP OF THE BAR. The RoundMarkers container spans the marker strip
// AND the scrub groove beneath it. Full-height drag rails therefore sat on top of CS2's own
// scrubber and swallowed every click meant for it -- i.e. they would have broken demo seeking on
// the one bottom mode where the native bar IS the timeline. Only the rails are confined to the
// marker strip.

R"TLJS(
    // ---- render-range overlay on the native demo bar ---------------------
    var RANGE_RAIL_H = 18;    // height of ONE drag rail
    var RANGE_RAIL_GAP = 2;
    var RANGE_FLAG_W = 24;
    // Vertical space the two stacked rails occupy inside the native marker strip. The output row
    // remains below the input row, but neither row draws its own track or field.
    //
    // Keeping the rails inside the marker strip is important: moving them into the editor's
    // Regular Timeline / Camera / Graph tab bar makes the render controls look detached from the
    // timeline and steals hover/clicks from those tabs.
    var RANGE_RAIL_STACK = 2 * RANGE_RAIL_H + RANGE_RAIL_GAP;

    var rangeRoot = $.CreatePanel('Panel', ctx, 'CamTimelineRangeRoot', {});
    rangeRoot.hittest = false; rangeRoot.hittestchildren = true; rangeRoot.visible = false;
    rangeRoot.style.width = '100%'; rangeRoot.style.height = '100%';
    // z59: above CamTimelineRoot (z55) AND above the native demo bar, which CameraTimelineJs lifts
    // to z58 while docked in a workspace (dockPopups). At z56 the loop flags drew under the docked
    // bar's opaque fill. See the z-layer map in PanoramaBridge.h.
    rangeRoot.style.zIndex = '59';

    // Absolutely-positioned handle container placed over the native bar each frame.
    var rangeBox = mk('Panel', rangeRoot); rangeBox.hittest = false; rangeBox.hittestchildren = true;
    rangeBox.style.position = '0px 0px 0px';

    // The selected range is the only bar-shaped visual. It is not a Slider and never receives
    // input; the transparent native Slider panels below provide the drag mechanics.
    var rangeBandEl = mk('Panel', rangeBox); rangeBandEl.hittest = false;
    rangeBandEl.visible = false;
    // The shared token is intentionally recessive elsewhere; this full selection needs enough
    // contrast to remain readable over CS2's alternating round-marker cells.
    rangeBandEl.style.backgroundColor = '#59d1b538';
    rangeBandEl.style.borderLeft = '2px solid ' + S.rangeEdge;
    rangeBandEl.style.borderRight = '2px solid ' + S.rangeEdge;

    var rangeGeom = { w: 0, h: 0, bandY: 0, bandH: RANGE_RAIL_STACK, t0: 0, t1: 1 };
    function rangeTickAt(frac) {
      return Math.round(rangeGeom.t0 + clamp01(frac) * (rangeGeom.t1 - rangeGeom.t0));
    }

    // A direct, visible hit target. Dragging is driven from the raw cursor state published by
    // CameraTimelineHud; routing through an invisible Slider made Panorama discard the child thumb
    // because its Slider parent was non-hit-testable, so clicks fell through to CS2's scrubber.
    function makeRangeRail(side, onCommit) {
      var rail = mk('Panel', rangeBox); rail.hittest = false; rail.hittestchildren = true;
      rail.style.position = '0px 0px 0px';
      var flag = mk('Panel', rail); flag.hittest = true;
      flag.style.position = '0px 0px 0px';
      flag.style.width = RANGE_FLAG_W + 'px'; flag.style.height = RANGE_RAIL_H + 'px';
      flag.style.zIndex = '4';
      flag.style.backgroundColor = S.rangeEdge;
      flag.style.borderRadius = side < 0 ? '2px 0px 0px 2px' : '0px 2px 2px 0px';
      var tag = lbl(flag, side < 0 ? 'I' : 'O', '#0b1016ff', 11);
      tag.hittest = false; tag.style.fontWeight = 'bold';
      tag.style.horizontalAlign = 'center'; tag.style.verticalAlign = 'center';
      // Consume Panorama's click at exactly the painted flag. Raw cursor state below owns motion.
      flag.SetPanelEvent('onactivate', function () {});
      var rec = { rail: rail, flag: flag, side: side, held: false,
        frac: side < 0 ? 0 : 1, onCommit: onCommit };
      return rec;
    }
    function placeRangeFlag(rec) {
      var x = clamp01(rec.frac) * rangeGeom.w - RANGE_FLAG_W / 2;
      if (x < 0) x = 0;
      var maxX = Math.max(0, rangeGeom.w - RANGE_FLAG_W);
      if (x > maxX) x = maxX;
      rec.flag.style.position = x.toFixed(1) + 'px 0px 0px';
    }

    // BOTH flags commit through one command built from the two current positions, ordered.
    // Committing them independently ("render in" / "render out") meant dragging I past O left an
    // inverted range that the C++ side then had to clamp -- which pinned the two flags on top of
    // each other and looked frozen. Ordering here makes crossing SWAP their roles instead: drag I
    // past O and the flag you are holding becomes the Out flag, exactly like a video editor.
    // Out is EXCLUSIVE in the render model, so the later tick is the last INCLUDED frame and the
    // stored end is one past it.
    var rangeOverlayKind = 'render';
    var rangePending = null;
    function commitRangeFromRails() {
      var a = rangeTickAt(railIn.frac), b = rangeTickAt(railOut.frac);
      var lo = Math.min(a, b), hi = Math.max(a, b);
      if (hi <= lo) hi = lo + 1;
      // Keep the locally-dragged result authoritative until C++ echoes the command back. Without
      // this, the next render frame restored the old engine ticks and the flags/band snapped back
      // briefly before finally jumping to the committed range.
      var lf = Math.min(railIn.frac, railOut.frac), hf = Math.max(railIn.frac, railOut.frac);
      railIn.frac = lf; railOut.frac = hf;
      placeRangeFlag(railIn); placeRangeFlag(railOut); redrawRangeBandFromRails();
      rangePending = { kind: rangeOverlayKind, lo: lo, hi: hi, until: (new Date()).getTime() + 1500 };
      if (rangeOverlayKind === 'loop')
        cmd('mirv_filmmaker loop range ' + lo + ' ' + hi + ' quiet');
      else
        cmd('mirv_filmmaker render range custom ' + lo + ' ' + (hi + 1));
    }
    var railIn = makeRangeRail(-1, commitRangeFromRails);
    var railOut = makeRangeRail(1, commitRangeFromRails);
    var rangeLastLmb = false;

    function rangeOverFlag(rec, mx, my) {
      var a = rec.flag.GetPositionWithinWindow ? rec.flag.GetPositionWithinWindow() : null;
      if (!a) return false;
      var w = rec.flag.actuallayoutwidth || 0, h = rec.flag.actuallayoutheight || 0;
      return w > 0 && h > 0 && mx >= a.x && mx <= a.x + w && my >= a.y && my <= a.y + h;
    }
    function pumpRangeFlags() {
      if (!st || typeof st.mx !== 'number') return;
      var lmb = !!st.lmb, mx = st.mx, my = st.my;
      // Do not gate this on CameraTimelineHud's `cursor` bit. In Regular Timeline and Config the
      // editor/config HUD owns UI-mouse mode, so this sibling HUD can report cursor=false even
      // while the OS pointer is visibly clicking Panorama. The exact painted-flag hit test is the
      // authority here and cannot claim an unrelated game click.
      if (lmb && !rangeLastLmb) {
        var hit = rangeOverFlag(railIn, mx, my) ? railIn
          : (rangeOverFlag(railOut, mx, my) ? railOut : null);
        if (hit) hit.held = true;
      }
      var active = railIn.held ? railIn : (railOut.held ? railOut : null);
      if (active && lmb) {
        var a = rangeBox.GetPositionWithinWindow ? rangeBox.GetPositionWithinWindow() : null;
        var w = rangeBox.actuallayoutwidth || 0;
        if (a && w > 1) {
          active.frac = clamp01((mx - a.x) / w);
          placeRangeFlag(active); redrawRangeBandFromRails();
        }
      }
      if (!lmb && active) {
        active.held = false; redrawRangeBandFromRails();
        active.onCommit();
      }
      rangeLastLmb = lmb;
    }

    function redrawRangeBandFromRails() {
      var f0 = railIn.frac, f1 = railOut.frac;
      var x0 = Math.min(f0, f1) * rangeGeom.w;
      var x1 = Math.max(f0, f1) * rangeGeom.w;
      if (x1 - x0 < 2) { rangeBandEl.visible = false; return; }
      rangeBandEl.style.position = x0.toFixed(1) + 'px ' + rangeGeom.bandY.toFixed(1) + 'px 0px';
      rangeBandEl.style.width = (x1 - x0).toFixed(1) + 'px';
      rangeBandEl.style.height = rangeGeom.bandH.toFixed(1) + 'px';
      rangeBandEl.visible = true;
    }

    // Current round span within the native round-marker container (container px): the current
    // round is the LAST marker starting at/left of the playhead; it ends where the next round
    // starts (or at the container end). CS2's own script places those markers as
    // demo-fraction * container width, so a px span converts straight back to ticks.
    function currentRoundSpan(rm, playFrac) {
      var n = rm.GetChildCount ? rm.GetChildCount() : 0;
      if (!n) return null;
      var rmW = rm.actuallayoutwidth || 0;
      if (rmW < 8) return null;
      var playX = playFrac * rmW, bestL = -1;
      for (var i = 0; i < n; i++) {
        var L = rm.GetChild(i).actualxoffset || 0;
        if (L <= playX + 2 && L > bestL) bestL = L;
      }
      if (bestL < 0) return null;
      var x1 = rmW;
      for (var j = 0; j < n; j++) {
        var L2 = rm.GetChild(j).actualxoffset || 0;
        if (L2 > bestL + 2 && L2 < x1) x1 = L2;
      }
      return { x0: bestL, x1: x1, w: rmW };
    }

    var lastRoundPushStart = -1, lastRoundPushEnd = -1;
    function hideRangeOverlay() {
      if (rangeRoot.visible) rangeRoot.visible = false;
      railIn.held = false; railOut.held = false; rangePending = null;
      rangeLastLmb = !!(st && st.lmb);
    }
)TLJS"
R"TLJS(
    // Per-frame update. `nativeVisible` means CS2's bar is actually visible: plain playback,
    // Config, or the editor's Regular Timeline. Round resolution runs in EVERY mode, because the
    // Range tab's "Current Round" needs its ticks pushed back whether the bar is on screen or not.
    function updateRangeOverlay(nativeVisible) {
      var rmPanel = findNative('RoundMarkers');
      var host = rmPanel || findNative('SliderRow');
      var mode = st ? st.renderRangeMode : null;
      var haveBounds = !!(st && st.demoStartTick >= 0 && st.demoEndTick > st.demoStartTick);
      if (!host || mode == null || !haveBounds) { hideRangeOverlay(); return; }

      var span = st.demoEndTick - st.demoStartTick;

      // -- round resolution + pushback (mode-independent) --
      var round = null;
      if (rmPanel) {
        var rs = currentRoundSpan(rmPanel, (st.tick - st.demoStartTick) / span);
        if (rs && rs.w > 0) {
          round = {
            f0: rs.x0 / rs.w, f1: rs.x1 / rs.w,
            startTick: Math.round(st.demoStartTick + rs.x0 / rs.w * span),
            endTick: Math.round(st.demoStartTick + rs.x1 / rs.w * span) + 1
          };
          if (mode === 1
            && (Math.abs(round.startTick - lastRoundPushStart) > 2
              || Math.abs(round.endTick - lastRoundPushEnd) > 2)) {
            lastRoundPushStart = round.startTick; lastRoundPushEnd = round.endTick;
            cmd('mirv_filmmaker render setrange ' + round.startTick + ' ' + round.endTick);
          }
        }
      }

      // Nothing is DRAWN unless CS2's own bar is genuinely the visible timeline. Besides the
      // camera/graph overlays (which cover the bar), the Config panel and a clean preview frame
      // both leave the bar hidden or repositioned -- and an overlay painted at the bar's last
      // known rectangle then sits on top of whatever replaced it. Hit-testable drag rails over a
      // hidden bar would also eat clicks meant for the panel that IS on screen.
      // Render owns the rails while its workspace is open. Everywhere else the same I/O rails
      // represent playback Loop, and disappear when Loop is off while preserving its ticks.
      var loopMode = !st.renderOpen && !!(st.loopOn && st.loopReady);
      if (!nativeVisible || (!st.renderOpen && !loopMode) || st.previewHudHidden) { hideRangeOverlay(); return; }
      rangeOverlayKind = loopMode ? 'loop' : 'render';

      var uis = host.actualuiscale_x || ctx.actualuiscale_x || 1;
      var hw = (host.actuallayoutwidth || 0) / uis;
      var a = host.GetPositionWithinWindow ? host.GetPositionWithinWindow() : null;
      if (!a || !(hw > 8)) { hideRangeOverlay(); return; }
      // Keep the handle container compact and anchored at the live marker-strip top. RoundMarkers
      // also owns the native scrub groove, so the hit-tested rails must never reach the scrubber
      // or the editor tabs.
      var RANGE_BOX_MAX_H = 2 * RANGE_RAIL_H + RANGE_RAIL_GAP + 6;
      var hh = (host.actuallayoutheight || 0) / uis;
      if (!(hh > RANGE_RAIL_STACK + 4)) hh = RANGE_BOX_MAX_H;
      if (hh > RANGE_BOX_MAX_H) hh = RANGE_BOX_MAX_H;

      // Fill the same fixed-height two-row area occupied by the handles. The previous 6px strip
      // sat on the marker field's bottom edge and was visually indistinguishable from a border.
      rangeGeom.w = hw; rangeGeom.h = hh; rangeGeom.bandY = 0;
      rangeGeom.bandH = RANGE_RAIL_STACK;
      rangeGeom.t0 = st.demoStartTick; rangeGeom.t1 = st.demoEndTick;

      // The handle container starts at the marker strip's real top. This keeps the I/O controls below
      // the Regular Timeline / Camera / Graph tabs while keeping both rails inside their parent.
      rangeBox.style.position = (a.x / uis).toFixed(1) + 'px '
        + (a.y / uis).toFixed(1) + 'px 0px';
      rangeBox.style.width = hw.toFixed(1) + 'px';
      rangeBox.style.height = hh.toFixed(1) + 'px';

      var active = loopMode
        ? !!(st.loopReady && st.loopOut > st.loopIn)
        : !!(st.renderRangeActive && st.renderOutTickExclusive > st.renderInTick);
      var inTick = loopMode ? st.loopIn : st.renderInTick;
      var outTick = loopMode ? st.loopOut : st.renderOutTickExclusive - 1;
      if (rangePending) {
        var confirmed = rangePending.kind === rangeOverlayKind
          && Math.abs(inTick - rangePending.lo) <= 2 && Math.abs(outTick - rangePending.hi) <= 2;
        if (confirmed || rangePending.kind !== rangeOverlayKind
          || (new Date()).getTime() > rangePending.until) rangePending = null;
      }
      var edge = loopMode ? '#7be08eff' : S.rangeEdge;
      railIn.flag.style.backgroundColor = edge; railOut.flag.style.backgroundColor = edge;
      rangeBandEl.style.backgroundColor = loopMode ? '#7be08e38' : '#59d1b538';
      rangeBandEl.style.borderLeft = '2px solid ' + edge;
      rangeBandEl.style.borderRight = '2px solid ' + edge;
      function syncRail(rec, frac) {
        if (rec.held || rangePending) return;
        rec.frac = clamp01(frac);
        placeRangeFlag(rec);
      }
      syncRail(railIn, active ? (inTick - st.demoStartTick) / span : 0);
      syncRail(railOut, active ? (outTick - st.demoStartTick) / span : 1);

      if (active || railIn.held || railOut.held)
        redrawRangeBandFromRails();
      else rangeBandEl.visible = false;

      // Rails sit inside the marker strip at its top. They must NOT reach down over RoundMarkers'
      // full height: that container runs past the scrub groove and over CS2's control row, so
      // full-height rails would swallow every click meant for CS2's own scrubber -- i.e. break
      // demo seeking in the one bottom mode where the native bar IS the timeline.
      function placeRail(rec, y) {
        rec.rail.style.position = '0px ' + y + 'px 0px';
        rec.rail.style.width = hw.toFixed(1) + 'px';
        rec.rail.style.height = RANGE_RAIL_H + 'px';
      }
      placeRail(railIn, 0);
      placeRail(railOut, RANGE_RAIL_H + RANGE_RAIL_GAP);
      placeRangeFlag(railIn); placeRangeFlag(railOut);

      rangeRoot.visible = true;
      pumpRangeFlags();
    }
    // Live diagnosis hook for the In/Out drag (this is UI state that only exists at runtime, so
    // reading the code cannot answer "is the rail where I think it is, and is it being hit?"):
    //   mirv_filmmaker camtl eval $.Msg($.CamTimeline.dbgRange()+String.fromCharCode(10));
    function dbgRangeState() {
      function railInfo(rec, name) {
        var a = rec.rail.GetPositionWithinWindow ? rec.rail.GetPositionWithinWindow() : null;
        return {
          n: name, frac: Number(rec.frac.toFixed(4)), held: !!rec.held,
          fht: !!rec.flag.hittest,
          x: a ? Math.round(a.x) : -1, y: a ? Math.round(a.y) : -1,
          w: Math.round(rec.rail.actuallayoutwidth || 0), h: Math.round(rec.rail.actuallayoutheight || 0)
        };
      }
      return JSON.stringify({
        vis: !!rangeRoot.visible,
        cursor: !!(st && st.cursor), lmb: !!(st && st.lmb),
        box: { w: Math.round(rangeGeom.w), h: Math.round(rangeGeom.h) },
        band: { vis: !!rangeBandEl.visible, x: Math.round(rangeBandEl.actualxoffset || 0),
          w: Math.round(rangeBandEl.actuallayoutwidth || 0), h: Math.round(rangeBandEl.actuallayoutheight || 0) },
        t0: rangeGeom.t0, t1: rangeGeom.t1,
        rails: [railInfo(railIn, 'in'), railInfo(railOut, 'out')]
      });
    }
)TLJS"
