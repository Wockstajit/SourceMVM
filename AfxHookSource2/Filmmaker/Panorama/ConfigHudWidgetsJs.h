#pragma once

// Widget helpers for the CONFIG panel. JS fragment concatenated (via #include, as an adjacent
// raw string literal) into the kConfigHudJs script body assembled in ConfigHudJs.h -- not a
// standalone script. Shares scope with the root IIFE (S, ctx, cmd, ...).
//
// THIS FILE IS THE PANEL'S DESIGN SYSTEM. Every card, row and control in every CONFIG tab is
// built through these helpers, so the 2026-10 rail redesign (Docs/features/filmmaker_config_panel.md)
// lands here and the tab fragments keep every control they had. The grammar:
//
//   rail      one row per card under small group headings (CFG_RAIL), each with its own square
//             switch; only the SELECTED card shows in the pane
//   card      pane header (3x30 accent bar, 17px title, live summary, action / Reset / switch)
//             + body; sub-pages are TEXT TABS ON A RULE
//   value     ONE LINE: 130px label | bar | 66px number, dragged RELATIVELY
//   switch    a checkbox CHIP; consecutive chips wrap into one row
//   full row  segmented mode rows, option grids, swatches, dropdowns, notes, action rows
//
// Geometry comes from UI (PanoramaUiMetricsJs.h) so the Camera Editor uses the same numbers; the
// colours stay in each workspace's own `S` because the accent is identity.
//
// Kept local rather than importing CameraEditorWidgetsJs.h, on purpose: the Config panel is a
// deliberately standalone script (see ConfigHudJs.h's header comment).

R"CFJS(
    // ---- motion ----------------------------------------------------------
    // ONE curve for arrivals. cubic-bezier(0.16,1,0.3,1) is a strong exponential ease-out: it
    // covers most of the distance immediately and settles, which reads as "already there" rather
    // than as a thing being flown in. Every panel entrance in this HUD uses it so the interface
    // has a single hand. Motion is <=160ms and only colour, opacity or a 4px entry offset.
    var MO_ARRIVE = UI.mo.arrive;
    var MO_TAB = UI.mo.tab;      // routine state change: swapping a tab body
    var MO_PANEL = UI.mo.panel;  // overlay/view transition: a workspace appearing
    //
    // THE PANORAMA RULE that governs all of this: a value only animates if it CHANGES across a
    // layout pass, so the "from" pose and the "to" pose cannot be written in the same frame --
    // hence the $.Schedule(0) release. And because render() repaints every frame, an entrance
    // must be fired ONCE on a real state change; re-applying it per frame restarts it forever
    // and the panel never settles.
    function moEnter(panel, fromY, dur, fromScale) {
      if (!panel || (panel.IsValid && !panel.IsValid())) return;
      var d = dur || MO_TAB;
      try {
        panel.style.transitionProperty = 'transform, opacity';
        panel.style.transitionTimingFunction = MO_ARRIVE;
        panel.style.transitionDuration = '0s';
        panel.style.transform = fromScale
          ? ('translateY(' + fromY + 'px) scale3d(' + fromScale + ',' + fromScale + ',1)')
          : ('translateY(' + fromY + 'px)');
        panel.style.opacity = '0';
      } catch (e) { return; }
      $.Schedule(0, function () {
        try {
          if (!panel || (panel.IsValid && !panel.IsValid())) return;
          panel.style.transitionDuration = d + 's';
          panel.style.transform = 'translateY(0px) scale3d(1,1,1)';
          panel.style.opacity = '1';
        } catch (e2) {}
      });
    }

    // An OPAQUE surface arriving. Transform only -- deliberately no opacity.
    //
    // Fading an opaque workspace column makes it see-through for the length of the transition, so
    // you watch the live game (and whatever panel is being replaced) straight through the panel
    // that is supposed to be covering it: the two surfaces' washes mix, which is where the blue
    // flash between Config and Camera came from. Opacity is for things that genuinely float
    // (popups, overlays); a slab that owns its pixels slides and never dips below full coverage.
    function moSlideIn(panel, fromY, dur) {
      if (!panel || (panel.IsValid && !panel.IsValid())) return;
      var d = dur || MO_PANEL;
      try {
        panel.style.transitionProperty = 'transform';
        panel.style.transitionTimingFunction = MO_ARRIVE;
        panel.style.transitionDuration = '0s';
        panel.style.transform = 'translateY(' + fromY + 'px)';
      } catch (e) { return; }
      $.Schedule(0, function () {
        try {
          if (!panel || (panel.IsValid && !panel.IsValid())) return;
          panel.style.transitionDuration = d + 's';
          panel.style.transform = 'translateY(0px)';
        } catch (e2) {}
      });
    }

    // A tab body arriving: the original design's 4px entry offset, with a fade. Deliberately small -- a
    // tab swap is a change of content inside a frame that did not move, so anything larger reads
    // as the whole panel relaunching.
    function moTabSwap(panel) { moEnter(panel, -UI.mo.rise, MO_TAB); }

    function mk(type, parent, props) { return $.CreatePanel(type, parent, '', props || {}); }
    function lbl(parent, text, color, size) {
      var l = mk('Label', parent); l.text = text || '';
      if (color) l.style.color = color; if (size) l.style.fontSize = size + 'px';
      l.style.fontFamily = S.font; return l;
    }
    function clamp01(v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
    function cfgValid(p) { return !!p && !(p.IsValid && !p.IsValid()); }
    function trans(p, props, dur) {
      p.style.transitionProperty = props;
      p.style.transitionDuration = (dur || UI.mo.hover) + 's';
      p.style.transitionTimingFunction = 'ease-out';
    }
    // Hover / press state on any panel, driven from panel events: this in-game HUD context has no
    // :hover and no stylesheet of ours to hang classes on.
    // ONE pair of hover handlers per panel, shared by the hover PAINT and the TOOLTIP.
    // Panorama's SetPanelEvent REPLACES the handler rather than adding one, so while hoverable()
    // and attachTip() each registered their own, whichever ran last silently won: a tip on a
    // tile killed that tile's hover highlight, and a highlight added later killed the tip. Both
    // now write their piece onto the panel and this binds the single handler that runs them.
    function cfgBindHover(p) {
      if (p.__hoverBound) return p;
      p.__hoverBound = true;
      p.SetPanelEvent('onmouseover', function () {
        p.__hot = true;
        if (p.__hoverPaint) p.__hoverPaint(p);
        if (p.__tipText) tipShow(p, p.__tipText);
      });
      p.SetPanelEvent('onmouseout', function () {
        p.__hot = false; p.__down = false;
        if (p.__hoverPaint) p.__hoverPaint(p);
        if (p.__tipText) tipHide();
      });
      return p;
    }
    function hoverable(p, paint) {
      p.hittest = true;
      p.__hot = false; p.__down = false;
      p.__hoverPaint = paint;
      cfgBindHover(p);
      paint(p);
      return p;
    }
    function pressFlash(p, paint) {
      p.__down = true; paint(p);
      try { $.Schedule(0.11, function () { if (cfgValid(p)) { p.__down = false; paint(p); } }); }
      catch (e) { p.__down = false; paint(p); }
    }
)CFJS"
R"CFJS(
    // ---- interaction state ----------------------------------------------------------------
    // One resolver owns every button's final colours. This panel had NO hover or press feedback
    // at all, and it could not have had any written the obvious way: setBtnOn() runs from the
    // per-frame render hooks, so anything written straight to style.backgroundColor by a
    // mouseover handler was repainted before it was ever seen. (Same contract, and the same
    // reason, as btnVisual() in CameraEditorWidgetsJs.h.)
    function paintBtn(b) {
      // Hard guard. A panel that never went through btn()/makeInteractive() has no __fgBase, and
      // assigning `undefined` to style.color THROWS -- which, from inside render(), aborts the
      // whole Config script and leaves the panel frozen. DESTROYED panels throw the same way, and
      // it is reachable: the press handler schedules a repaint 0.11s later, and a button can be
      // deleted in between (picking a WEAPON RECOIL scope rebuilds the key grid).
      if (!b || (b.IsValid && !b.IsValid()) || !b.__lbl) return;
      if (b.__lbl.IsValid && !b.__lbl.IsValid()) return;
      var accent = b.__accentOn || S.accent;
      if (b.__isTab) {
        // A tab is TEXT ON A RULE with an accent underline -- never a filled pill.
        var thot = b.__hot || b.__down;
        b.style.backgroundColor = S.clear;
        b.style.border = '1px solid ' + S.clear;
        // Active text is the value tier; the accent lives only in the underline (Docs/features/filmmaker_config_panel.md).
        b.__lbl.style.color = (b.__on || thot) ? S.value : S.muted;
        // The `border` shorthand above resets border-bottom, so the underline is restated last.
        b.style.borderBottom = UI.tab.underline + 'px solid '
          + (b.__on ? accent : (thot ? S.tabHoverEdge : S.clear));
        return;
      }
      // A CELL of a segmented control or an option grid. The selection is a tinted ground, never
      // an accent outline per cell: the group's one outline already says "this is one control".
      if (b.__seg || b.__grid) {
        var a7 = accent.substring(0, 7), shot = b.__hot || b.__down;
        b.style.opacity = b.__off ? '0.45' : '1';
        b.style.backgroundColor = b.__on ? (a7 + '2e') : (shot ? '#ffffff0d' : S.clear);
        if (b.__grid) b.style.border = '1px solid ' + (b.__on ? (a7 + '99') : (shot ? '#ffffff40' : '#ffffff1a'));
        b.__lbl.style.color = (b.__on || shot) ? S.value : S.label;
        return;
      }
      var bg = b.__on ? S.btnOn : (b.__bgBase || S.btnBg);
      var br = b.__on ? accent : (b.__brBase || S.btnBorder);
      var fg = b.__on ? (b.__fgOn || accent) : (b.__fgBase || S.label);
      if (b.__off) {
        // A disabled control has to read as disabled from across the panel, not on close
        // inspection: the fill and the border go with the label.
        bg = S.clear; br = '#ffffff08'; fg = S.dim;
        b.style.opacity = '0.45';
      } else {
        b.style.opacity = '1';
        if (b.__down) { bg = S.btnDown; br = accent; }
        else if (b.__hot) {
          bg = b.__on ? S.btnOnHover : S.btnHover;
          br = b.__on ? accent : S.btnBorderHover;
          if (!b.__on) fg = S.value;
        }
      }
      b.style.backgroundColor = bg;
      b.style.border = '1px solid ' + br;
      b.__lbl.style.color = fg;
    }
    // Stamp the interaction state + events onto ANY panel, including hand-rolled ones that have
    // their own geometry (the workspace action buttons). Anything that will later be passed to
    // setBtnOn() must go through here -- see the guard in paintBtn for what happens otherwise.
    function makeInteractive(b, l, onClick, opts) {
      opts = opts || {};
      b.hittest = true;
      b.__lbl = l;
      b.__on = false; b.__hot = false; b.__down = false; b.__off = false;
      b.__fgBase = opts.fg || S.label;
      b.__fgOn = opts.fgOn || S.accent;
      b.__accentOn = opts.accent || S.accent;
      if (opts.border) b.__brBase = opts.border;
      if (opts.bg) b.__bgBase = opts.bg;
      trans(b, 'background-color, border-color');
      b.SetPanelEvent('onmouseover', function () { b.__hot = true; paintBtn(b); });
      b.SetPanelEvent('onmouseout', function () { b.__hot = false; b.__down = false; paintBtn(b); });
      b.SetPanelEvent('onactivate', function () {
        if (b.__off) return;
        b.__down = true; paintBtn(b);
        try { $.Schedule(0.11, function () { b.__down = false; paintBtn(b); }); }
        catch (e) { b.__down = false; paintBtn(b); }
        if (onClick) onClick();
      });
      paintBtn(b);
      return b;
    }
    // The panel's plain button: 12/700 on a quiet fill -- the original design's secondary action.
    function btn(parent, text, onClick, color) {
      var b = mk('Panel', parent);
      b.style.borderRadius = UI.btn.radius + 'px';
      b.style.paddingTop = UI.chip.padV + 'px'; b.style.paddingBottom = UI.chip.padV + 'px';
      b.style.paddingLeft = '11px'; b.style.paddingRight = '11px';
      b.style.marginRight = '6px'; b.style.verticalAlign = 'center';
      var l = lbl(b, text, color || S.label, 12);
      l.hittest = false; l.style.fontWeight = 'bold'; l.style.whiteSpace = 'nowrap';
      return makeInteractive(b, l, onClick, { fg: color || S.label });
    }
    // Full-width button. An ACCENT one (color === S.accent) is a card's primary action: an accent
    // outline on a faint accent ground, so it reads as primary without competing with the
    // header's one solid fill. Any other colour is a plain full-width row (object lists, loads).
    function wideBtn(parent, text, onClick, color) {
      var b = btn(parent, text, onClick, color);
      b.style.width = '100%'; b.style.marginRight = '0px'; b.style.marginTop = '4px';
      b.style.paddingTop = '7px'; b.style.paddingBottom = '7px';
      b.__lbl.style.horizontalAlign = 'center';
      if (color === S.accent) {
        var a7 = S.accent.substring(0, 7);
        b.style.paddingTop = '8px'; b.style.paddingBottom = '8px';
        b.__lbl.style.letterSpacing = '1px';
        b.__bgBase = a7 + '14'; b.__brBase = a7 + '66';
        paintBtn(b);
      }
      return b;
    }
    // Card-level housekeeping (Reset, Auto, More...). Right-aligned and only as wide as its own
    // words -- deliberately NOT full width: an undo is secondary to the thing it undoes, and
    // seven full-width accent slabs is most of what made the EFFECTS tab read as a pile of
    // buttons. Always parent these to an actionRowEnd, which owns the right alignment.
    function actionBtn(parent, text, onClick) {
      var b = btn(parent, text, onClick, S.label);
      b.style.marginRight = '0px';
      b.style.paddingTop = UI.chip.padV + 'px'; b.style.paddingBottom = UI.chip.padV + 'px';
      b.style.paddingLeft = '12px'; b.style.paddingRight = '12px';
      b.__lbl.style.whiteSpace = 'nowrap';
      b.style.marginLeft = '6px';
      return b;
    }
    // A trailing row for one or more actionBtns, so a card's housekeeping sits on one baseline at
    // the card's end instead of stacking down the middle of the content.
    // width: fit-children, NOT 100%. horizontalAlign on a child is ignored along the flow axis, so
    // a full-width row cannot push its buttons right; a row that hugs its buttons and is itself
    // right-aligned can.
    function actionRowEnd(parent, topMargin) {
      var r = mk('Panel', parent);
      r.style.flowChildren = 'right'; r.style.width = 'fit-children';
      r.style.horizontalAlign = 'right';
      r.style.marginTop = (typeof topMargin === 'number' ? topMargin : UI.col.gap) + 'px';
      endTileLine(parent);
      return r;
    }
    // Selected state. Cheap and idempotent (only repaints on a real change), so the render hooks
    // can keep calling it every frame without stomping a live hover.
    function setBtnOn(b, on, onColor) {
      on = !!on;
      if (!b || b.__lbl == null) return b;
      var fgOn = onColor || S.accent;
      if (b.__on === on && b.__fgOn === fgOn) return b;
      if (!b || (b.IsValid && !b.IsValid())) return b;
      b.__on = on; b.__fgOn = fgOn; paintBtn(b); return b;
    }
    function setBtnDisabled(b, off) {
      off = !!off;
      if (!b || b.__lbl == null || b.__off === off) return b;
      b.__off = off; b.hittest = !off; b.hittestchildren = !off;
      b.__hot = false; b.__down = false;
      paintBtn(b); return b;
    }
    // ---- THE ONE RESET RUNNER ------------------------------------------------------------
    // A reset is three phases, in this order:
    //
    //   off      turn the thing OFF first. A backend that latches its settings when a switch
    //            changes will not notice a value rewritten underneath it, so a reset that only
    //            wrote defaults left the old state rendering. Going through off guarantees a
    //            change even when a key's default is the value it already held.
    //   cmds     put every key back to its shipped default.
    //   after    release any idle renderer after the defaults are in.
    //   local    clear UI-only state the state JSON does not carry back (an open picker, a name).
    //
    // Every RESET in this panel goes through here, and every one registers itself in cfgResets
    // so automation can run exactly what the button runs -- see api.reset.
    var cfgResets = {};
    function cfgRunReset(spec) {
      if (!spec) return false;
      var i;
      if (spec.off) for (i = 0; i < spec.off.length; i++) cmd(spec.off[i]);
      if (spec.cmds) for (i = 0; i < spec.cmds.length; i++) cmd(spec.cmds[i]);
      if (spec.after) {
        try { spec.after(); }
        catch (e) { $.Msg('[confighud] reset after-hook failed: ' + e + '\n'); }
      }
      if (spec.local) {
        try { spec.local(); }
        catch (e) { $.Msg('[confighud] reset local failed: ' + e + '\n'); }
      }
      return true;
    }
    // `spec` is either the array of default-writes (the old shape, still used by the camera
    // cards) or a full { off, cmds, after, local }.
    function cfgResetSpec(v) { return (v && v.length !== undefined) ? { cmds: v } : v; }
    function cfgRegisterReset(name, resolve) { if (name) cfgResets[name] = resolve; }
    // A footer RESET exactly as a click runs it (including its confirm), for automation.
    var cfgFooterPress = {};

    // (There is no third reset builder. Every RESET in this panel is either a card-header
    // chip -- body.__headerReset -- or a tab footer -- resetFooter. A per-effect effectReset()
    // used to live here with no call sites at all.)
)CFJS"
R"CFJS(
    // ---- line packing ------------------------------------------------------------------
    // A value is ONE LINE (label | bar | number), a switch is a CHECKBOX CHIP, and consecutive
    // chips share one wrapping row -- the rail design's main space saving (Docs/features/filmmaker_config_panel.md) over the old 64px
    // two-per-line tiles. Anything that is not a chip (a slider, a segmented row, a sub-head, a
    // note) ends the chip row, so a page still reads in the order its fragment created controls.
    function endTileLine(parent) { if (parent) { parent.__tileLine = null; parent.__chipLine = null; } }
    function cfgChipLine(parent) {
      var line = parent.__chipLine;
      // Only reuse the row while it is still the parent's LAST child: anything created in between
      // (even by a tab fragment that never called endTileLine) starts a fresh row below it.
      if (line && (!cfgValid(line) || parent.GetChild(parent.GetChildCount() - 1) !== line)) line = null;
      if (!line) {
        line = mk('Panel', parent); line.hittest = false; line.hittestchildren = true;
        line.style.flowChildren = 'right-wrap'; line.style.width = '100%';
        line.style.marginTop = (UI.line.gap + 1) + 'px';
        line.style.marginBottom = (-UI.line.gap) + 'px';
        parent.__chipLine = line;
      }
      return line;
    }
    // The one-line row's ground: #1b2129, lifting to #212833 under the pointer or while dragged.
    function paintTile(t) {
      var held = !!t.__held;
      t.style.backgroundColor = (t.__hot || held) ? S.rowHover : S.tileBg;
      t.style.border = '1px solid ' + ((held && t.__moved) ? (S.accent.substring(0, 7) + '80') : S.clear);
    }

    // ---- Shift-fine tile dragging -------------------------------------------------------
    //
    // The report asks for "Shift + Drag = slower / more precise" on EVERY value. It could not be
    // built from Panorama events: there is no mouse-move event here, so a RELATIVE drag is not
    // expressible, and a native Slider only ever maps the pointer's ABSOLUTE x onto its value.
    // The Camera Editor inspector solved this with the OS-cursor pipe; ConfigHud publishes the
    // same mx/my/lmb/shift, so the same approach serves these ~68 values.
    //
    // The drag is RELATIVE and NEVER JUMPS TO THE CLICK (Docs/panorama/panorama_ui_guide.md): the value moves with the
    // pointer from where it was, one tile width covers the range, Shift is a fifth of that.
    var CFG_DRAG_FINE = 0.2;   // Shift held: the same sweep, 5x finer
    var CFG_DRAG_SLOP = 2;     // px before a press counts as a drag rather than a click
    var cfgDragSliders = [];
    var cfgLastLmb = false;

    function cfgAttachDrag(rec, host, apply) {
      rec.__host = host; rec.__apply = apply;
      rec.__held = false; rec.__anchorX = 0; rec.__baseV = 0; rec.__restV = 0;
      cfgDragSliders.push(rec);
      return rec;
    }

    // Is this panel actually ON SCREEN -- visible itself, AND with every ancestor visible?
    //
    // `pnl.visible` is only the panel's OWN flag, and nothing hides a tile directly. Tab pages
    // hide by setting the PAGE's flag and a collapsed card hides its clipper; every tile inside
    // keeps reporting `visible: true` along with the layout rect it had when it was last shown.
    //
    // That is what made values on different tabs move together. The drag pump does its own hit
    // testing off the cursor pipe -- it has to, because this HUD context gets no mouse-move
    // events -- so it never sees Panorama's own hit test rejecting an off-screen panel.
    //
    // The walk runs off the end of our own tree into CS2's native HUD panels, so it FAILS OPEN:
    // if a property read ever throws we answer "on screen" rather than making every CONFIG value
    // undraggable. Same trade cfgSyncHitTest makes below -- a desync beats a dead panel.
    function cfgPanelOnScreen(pnl) {
      try {
        // The guard is a cycle/orphan stop, not a real depth limit.
        for (var p = pnl, guard = 0; p && guard < 64; ++guard) {
          if (p.IsValid && !p.IsValid()) return false;
          if (!p.visible) return false;
          p = p.GetParent ? p.GetParent() : null;
        }
      } catch (e) { return true; }
      return true;
    }
    function cfgOverPanel(pnl, x, y) {
      try {
        if (!pnl || !cfgPanelOnScreen(pnl)) return false;
        // A scrolled tile can sit BEHIND the pinned tab strip or footer and still be
        // 'visible', so a press up there would grab a control the operator cannot see.
        // Clip every hit test to the scroll viewport.
        if (scrollView) {
          var clip = scrollView.GetPositionWithinWindow ? scrollView.GetPositionWithinWindow() : null;
          if (!clip || x < clip.x || x >= clip.x + scrollView.actuallayoutwidth ||
              y < clip.y || y >= clip.y + scrollView.actuallayoutheight) return false;
        }
        var a = pnl.GetPositionWithinWindow ? pnl.GetPositionWithinWindow() : null;
        if (!a) return false;
        var w = pnl.actuallayoutwidth || 0, h = pnl.actuallayoutheight || 0;
        // An unlaid-out panel reports FLT_MAX, not 0 -- never let that swallow a click.
        if (!(w > 0) || !(h > 0) || w > 1e30 || h > 1e30) return false;
        return x >= a.x && x < a.x + w && y >= a.y && y < a.y + h;
      } catch (e) { return false; }
    }
    // Give the native control back if the OS-cursor pipe is not usable. Without this, taking the
    // Slider out of the hit path would leave every CONFIG value undraggable in exactly the
    // failure mode the pipe can hit (a CS2 update moving the WndProc capture) -- trading a
    // cosmetic desync for a dead panel.
    function cfgSyncHitTest(live) {
      for (var i = 0; i < cfgDragSliders.length; i++) {
        var rec = cfgDragSliders[i];
        if (!rec.sl || (rec.sl.IsValid && !rec.sl.IsValid())) continue;
        if (rec.__hitLive === live) continue; // only touch it on a real change
        rec.__hitLive = live;
        rec.sl.hittest = !live; rec.sl.hittestchildren = !live;
        rec.sl.style.opacity = live ? '0.001' : '1';
      }
    }
    function cfgCursorPump(st) {
      var live = !!(st && typeof st.mx === 'number');
      cfgSyncHitTest(live);
      if (!live) return;
      // Only while the PANEL owns the cursor: in game-mouse mode the button is aiming,
      // not dragging, and a tile under the crosshair would follow it.
      var lmb = !!st.lmb && !!st.cursor, mx = st.mx, my = st.my;
      // A colour drag owns the pointer; value rows still drag while a picker is merely open.
      if (cfgColorDrag) { cfgLastLmb = lmb; return; }
      var fine = !!st.shift;
      for (var i = 0; i < cfgDragSliders.length; i++) {
        var rec = cfgDragSliders[i], host = rec.__host;
        if (!host || (host.IsValid && !host.IsValid())) continue;
        // Read the resting value while the button is UP: at press time the native Slider may
        // already have jumped to the click position on the same frame.
        if (!lmb && !rec.__held) rec.__restV = clamp01(rec.sl.value);
        if (lmb && !cfgLastLmb && cfgOverPanel(host, mx, my)) {
          rec.__held = true; rec.__moved = false; rec.__anchorX = mx; rec.__baseV = rec.__restV;
          rec.__fine = fine;
          if (rec.__tile) { rec.__tile.__held = true; rec.__tile.__moved = false; paintTile(rec.__tile); }
        }
        // The drag scale is the BAR, not the whole row: one bar width sweeps the range, so the
        // knob stays under the pointer the way it looks like it should.
        var spanP = (rec.__span && cfgValid(rec.__span)) ? rec.__span : host;
        var w = spanP.actuallayoutwidth || 0;
        if (rec.__held && w > 8 && w < 1e30) {
          if (!rec.__moved && Math.abs(mx - rec.__anchorX) > CFG_DRAG_SLOP) {
            rec.__moved = true;
            if (rec.__tile) { rec.__tile.__moved = true; paintTile(rec.__tile); }
          }
          if (rec.__moved) {
            // Entering or leaving Shift-fine re-anchors at the current value and pointer, so the
            // change of scale cannot jump.
            if (rec.__fine !== fine) {
              rec.__fine = fine;
              rec.__baseV = clamp01(rec.sl.value);
              rec.__anchorX = mx;
            }
            var v = clamp01(rec.__baseV + ((mx - rec.__anchorX) / w) * (rec.__fine ? CFG_DRAG_FINE : 1));
            rec.busy = true; rec.sl.value = v; rec.busy = false;
            rec.__apply(v);
            rec.__lastV = v;
          }
        }
        if (!lmb && rec.__held) {
          rec.__held = false;
          // Hand the value back in a known state: the native Slider is still the fallback hit
          // target and may have written its own ABSOLUTE-x value during the gesture, so
          // re-assert ours once on the falling edge of the pipe's lmb.
          if (typeof rec.__lastV === 'number') {
            rec.busy = true; rec.sl.value = rec.__lastV; rec.busy = false;
            rec.__restV = rec.__lastV;
          }
          if (rec.__tile) { rec.__tile.__held = false; rec.__tile.__moved = false; paintTile(rec.__tile); }
        }
      }
      cfgLastLmb = lmb;
    }
    function cfgDragging() {
      for (var i = 0; i < cfgDragSliders.length; i++) if (cfgDragSliders[i].__held) return true;
      return false;
    }
    // Drop drag records whose row was deleted (search results are rebuilt per query).
    function cfgPruneDrags() {
      var keep = [];
      for (var i = 0; i < cfgDragSliders.length; i++) {
        var h = cfgDragSliders[i].__host;
        if (h && cfgValid(h) && !h.__dead) keep.push(cfgDragSliders[i]);
      }
      cfgDragSliders = keep;
    }

    // ---- the search index ----------------------------------------------------------------
    // Every value row, chip and mode row registers itself here AS IT IS BUILT, from the same
    // label string it shows -- there is no hand-maintained list to drift. Each entry can build a
    // live PROXY of its control (the same builder, the same command, the same handler) for the
    // global search results, and mirrors every sync()/set() onto the proxies that exist.
    var cfgSearchIndex = [];
    function cfgSearchAdd(kind, label, row, extra, make) {
      var e = { kind: kind, label: '' + label, row: row, make: make, proxies: [], has: false, last: null,
                words: (('' + label) + ' ' + (extra || '')).toLowerCase() };
      e.mirror = function (v) {
        e.last = v; e.has = true;
        for (var i = 0; i < e.proxies.length; i++) {
          var p = e.proxies[i];
          if (!p || !p.row || !cfgValid(p.row)) continue;
          if (p.sync) p.sync(v); else if (p.set) p.set(v);
        }
      };
      cfgSearchIndex.push(e);
      return e;
    }
    // NO WHEEL-TO-EDIT. A value tile is changed by dragging it and by nothing else: the wheel
    // belongs to the column, so scrolling past a slider cannot silently move it.
)CFJS"
R"CFJS(
    // ---- hover explanations ----------------------------------------------
    // Most controls in this panel are a name and a number, and a name like "Tilt" or "Variance"
    // does not tell you what moves when you drag it. Persistent hint lines under every control
    // turned the panel into a document, so the explanation lives on hover instead -- and it is
    // attached to the NAME only, never the whole control: hovering a value tile means you are
    // about to drag it, and a popup there sits in the way of the gesture.
    //
    // ONE floating tooltip panel, parented to the full-screen ConfigHudRoot (created lazily on
    // first show, because this fragment runs before `root` exists) so no card or scroll region
    // clips it. Position comes from GetPositionWithinWindow() (scroll- and transform-aware);
    // height is estimated from the text length only to decide above-vs-below.
    //
    // WRITING RULE for these strings: one sentence, present tense, says what the control changes
    // and -- where it is not obvious -- what higher means. No "this slider lets you...", no
    // restating the label, no implementation notes.
    var TIP_W = 300, TIP_DELAY = 0.18, TIP_FADE = 0.14;
    var tipPanel = null, tipLbl = null, tipToken = 0;
    function ensureTipPanel() {
      if (tipPanel && (!tipPanel.IsValid || tipPanel.IsValid())) return;
      tipPanel = mk('Panel', root);
      tipPanel.hittest = false;
      tipPanel.visible = false;
      tipPanel.style.zIndex = '260';
      tipPanel.style.flowChildren = 'down';
      tipPanel.style.width = TIP_W + 'px';
      tipPanel.style.backgroundColor = '#10141af2';
      tipPanel.style.border = '1px solid ' + S.accent.substring(0, 7) + '59';
      tipPanel.style.borderRadius = '5px';
      tipPanel.style.boxShadow = '#00000099 0px 4px 16px 0px';
      tipPanel.style.paddingLeft = '12px'; tipPanel.style.paddingRight = '12px';
      tipPanel.style.paddingTop = '9px'; tipPanel.style.paddingBottom = '10px';
      tipPanel.style.opacity = '0';
      tipPanel.style.transitionProperty = 'opacity, transform';
      tipPanel.style.transitionTimingFunction = MO_ARRIVE;
      tipLbl = lbl(tipPanel, '', S.value, 13);
      // hittest OFF on the label too, not only on the popup panel. A Label is hit-testable by
      // default, so the popup's text was a live click target floating at zIndex 260 over the
      // column: anything it covered could not be clicked while it was up.
      tipLbl.hittest = false;
      tipLbl.style.whiteSpace = 'normal';
      tipLbl.style.width = '100%';
    }
    function tipShow(anchor, text) {
      var token = ++tipToken;
      // Hover-intent delay: pointing across the panel should not fire a popup per name crossed.
      $.Schedule(TIP_DELAY, function () {
        if (token !== tipToken) return;
        try {
          // Never while a button is held: dragging a value across the panel would otherwise pop a
          // tooltip for every tile the pointer passes over.
          if (cfgDragging() || (st && st.lmb)) return;
          if (!anchor || (anchor.IsValid && !anchor.IsValid())) return;
          ensureTipPanel();
          tipLbl.text = '' + text;
          var sx = root.actualuiscale_x || ctx.actualuiscale_x || 1;
          var sy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
          var a = anchor.GetPositionWithinWindow ? anchor.GetPositionWithinWindow() : null;
          if (!a) return;
          var ax = a.x / sx, y = a.y / sy;
          var aw = (anchor.actuallayoutwidth || 0) / sx;
          var rootH = (root.actuallayoutheight || 0) / sy;
          // LEFT of the column, the way the original design placed it: the column sits against the screen's
          // right edge, so there is no room on that side. Flip back only if it would run off.
          var x = ax - TIP_W - 16;
          if (x < 8) x = Math.max(8, ax + aw + 12);
          var estH = Math.ceil(('' + text).length / 42) * 18 + 20;
          if (rootH > 0 && y + estH > rootH - 8) y = Math.max(8, rootH - 8 - estH);
          tipPanel.style.position = Math.round(x) + 'px ' + Math.round(y) + 'px 0px';
          // From-pose and to-pose must land on different layout passes (the Panorama rule at the
          // top of this file), hence the Schedule(0) release.
          tipPanel.style.transitionDuration = '0s';
          tipPanel.style.opacity = '0';
          tipPanel.style.transform = 'translateY(4px)';
          tipPanel.visible = true;
          $.Schedule(0, function () {
            if (token !== tipToken) return;
            try {
              tipPanel.style.transitionDuration = TIP_FADE + 's';
              tipPanel.style.opacity = '1';
              tipPanel.style.transform = 'translateY(0px)';
            } catch (e2) {}
          });
        } catch (e) {}
      });
    }
    function tipHide() {
      var token = ++tipToken; // cancels any pending or in-flight show
      if (!tipPanel || (tipPanel.IsValid && !tipPanel.IsValid()) || !tipPanel.visible) return;
      try {
        tipPanel.style.transitionDuration = TIP_FADE + 's';
        tipPanel.style.opacity = '0';
        tipPanel.style.transform = 'translateY(4px)';
        $.Schedule(TIP_FADE + 0.02, function () {
          if (token !== tipToken) return;
          try { tipPanel.visible = false; } catch (e2) {}
        });
      } catch (e) {}
    }
    // The tip belongs to the WHOLE control, not to its name. Hovering anywhere on a tile --
    // the number, the bar, the empty space -- explains it, because "hover the four letters of
    // the label" is not discoverable and is annoying once you know it.
    function attachTip(panel, text) {
      if (!panel || !text) return panel;
      panel.hittest = true;
      panel.__tipText = text;
      cfgBindHover(panel);
      return panel;
    }

    // ---- the hidden value store + the drawn bar ---------------------------------------
    // A native Slider, kept ONLY as value storage and as the no-cursor-pipe fallback: it is out
    // of the hit path and invisible. The bar (4px track, fill, 8x12 knob) is drawn from the same
    // fraction the readout uses. One value, one writer, two visuals that cannot disagree.
    //
    // opts.noFill / opts.centerTick / opts.knobColor serve the pivot drawer's bipolar sliders:
    // a centre tick instead of a fill, and the knob in the marker's colour.
    function makeSlider(parent, fillColor, opts) {
      opts = opts || {};
      var wrap = mk('Panel', parent); wrap.hittest = false;
      wrap.style.width = 'fill-parent-flow(1.0)'; wrap.style.height = '12px';
      wrap.style.verticalAlign = 'center';
      wrap.style.marginLeft = UI.line.colGap + 'px'; wrap.style.marginRight = UI.line.colGap + 'px';
      var track = mk('Panel', wrap); track.hittest = false;
      track.style.width = '100%'; track.style.height = '4px'; track.style.borderRadius = '1px';
      track.style.verticalAlign = 'center'; track.style.backgroundColor = S.trackBg;
      var fill = mk('Panel', track); fill.hittest = false;
      fill.style.width = '0%'; fill.style.height = '100%'; fill.style.borderRadius = '1px';
      fill.style.horizontalAlign = 'left';
      fill.style.backgroundColor = fillColor || S.accent;
      if (opts.noFill) fill.visible = false;
      if (opts.centerTick) {
        var tick = mk('Panel', wrap); tick.hittest = false;
        tick.style.width = '1px'; tick.style.height = '10px';
        tick.style.horizontalAlign = 'center'; tick.style.verticalAlign = 'center';
        tick.style.backgroundColor = '#ffffff40';
      }
      // The knob rides a flow row: a percentage spacer, then the knob pulled back by half its
      // width, so it centres on the value without any measured pixel positions.
      var knobLine = mk('Panel', wrap); knobLine.hittest = false;
      knobLine.style.flowChildren = 'right'; knobLine.style.width = '100%';
      knobLine.style.height = '12px'; knobLine.style.overflow = 'noclip';
      knobLine.style.verticalAlign = 'center';
      var spacer = mk('Panel', knobLine); spacer.hittest = false;
      spacer.style.width = '0%'; spacer.style.height = '1px';
      var knob = mk('Panel', knobLine); knob.hittest = false;
      knob.style.width = '8px'; knob.style.height = '12px'; knob.style.marginLeft = '-4px';
      knob.style.borderRadius = '2px'; knob.style.backgroundColor = opts.knobColor || '#eef2f6ff';
      var sl = $.CreatePanel('Slider', wrap, '', { direction: 'horizontal' });
      sl.AddClass('HorizontalSlider');
      sl.style.ignoreParentFlow = 'true';
      sl.style.width = '100%'; sl.style.height = '100%';
      sl.style.opacity = '0.001';
      sl.hittest = false; sl.hittestchildren = false;
      return { wrap: wrap, sl: sl, knob: knob, setFill: function (frac) {
        var pct = (clamp01(frac) * 100).toFixed(2) + '%';
        fill.style.width = pct; spacer.style.width = pct;
      } };
    }
)CFJS"
R"CFJS(
    // ---- the value row ----------------------------------------------------------------
    // ONE LINE, 32px: the name in a 130px column, the bar, the number in a 66px right-aligned
    // column. Drag anywhere on the row to change it -- relative, never jumping to the click.
    //
    // opts: { label, cmd, min, max, unit, decimals, offAtZero, tip, onChange, fill,
    //         noFill, centerTick, knobColor, search }
    //   cmd       console command prefix; the value and " quiet" are appended
    //   min/max   value range (default 0..150, the intensity-percent convention)
    //   unit      suffix on the readout, default '%'
    //   decimals  fractional digits kept in both the readout and the command argument
    //             (default 0). REQUIRED for any range spanning only a few units.
    //   offAtZero show "Off" instead of "0<unit>" at the minimum (only when min is 0)
    //   onChange  for a value whose command is not a fixed prefix plus the number
    //   search    extra words the global search should match (the pivot sliders add "pivot")
    //
    // FEEDBACK-LOOP GUARDS -- do not simplify these. Panorama delivers SliderValueChanged for
    // PROGRAMMATIC value writes too, after the writing scope has returned, so the handler below
    // swallows every event and only the drag pump commits; sync() only writes sl.value when it
    // differs beyond one slider quantum.
    function sliderRow(parent, opts) {
      var min = (typeof opts.min === 'number') ? opts.min : 0;
      var max = (typeof opts.max === 'number') ? opts.max : 150;
      var span = (max - min) || 1;
      var unit = (typeof opts.unit === 'string') ? opts.unit : '%';
      var decimals = (typeof opts.decimals === 'number') ? opts.decimals : 0;
      var mul = Math.pow(10, decimals);
      var quantum = 1 / mul; // smallest step the readout/command can express
      var offAtZero = !!opts.offAtZero;
      function quant(v) { return Math.round(v * mul) / mul; }
      function fmt(v) { return (offAtZero && v <= min && min === 0) ? 'Off' : (v.toFixed(decimals) + unit); }

      endTileLine(parent);
      var t = mk('Panel', parent);
      t.style.flowChildren = 'right'; t.style.width = '100%';
      t.style.height = UI.line.h + 'px'; t.style.marginTop = UI.line.gap + 'px';
      t.style.paddingLeft = UI.line.padH + 'px'; t.style.paddingRight = UI.line.padH + 'px';
      t.style.borderRadius = '3px';
      trans(t, 'background-color, border-color');
      var nl = lbl(t, opts.label, S.label, 13);
      nl.style.width = UI.line.label + 'px'; nl.style.verticalAlign = 'center';
      nl.style.whiteSpace = 'nowrap'; nl.style.textOverflow = 'ellipsis';
      nl.hittest = false;
      if (opts.tip) attachTip(t, opts.tip);
      var ms = makeSlider(t, opts.fill || S.accent, opts);
      var vl = lbl(t, fmt(min), S.value, 14);
      vl.hittest = false; vl.style.fontWeight = 'bold';
      vl.style.width = UI.line.value + 'px'; vl.style.textAlign = 'right';
      vl.style.verticalAlign = 'center'; vl.style.whiteSpace = 'nowrap';
      // Monospaced-DIGIT cut: a number that changes while you watch it must not shift as its
      // digits change width.
      vl.style.fontFamily = S.fontMonodigit;
      hoverable(t, paintTile);
      t.SetPanelEvent('onactivate', function () { /* the drag is cursor-driven */ });

      var lastVal = null;
      // PAINTING IS CACHED: render() runs every hook for every tab, and an identical Panorama
      // style/text assignment still dirties layout. Both the drag pump and sync() paint through
      // here, so the cache can never disagree with what is on screen.
      var lastFillFrac = null, lastText = null;
      function paintValue(val, frac) {
        if (lastFillFrac === null || Math.abs(frac - lastFillFrac) > 0.0002) {
          lastFillFrac = frac;
          ms.setFill(frac);
        }
        var txt = fmt(val);
        if (txt !== lastText) { lastText = txt; vl.text = txt; }
      }
      function commitFrac(frac) {
        var val = quant(min + clamp01(frac) * span);
        if (lastVal !== null && Math.abs(val - lastVal) < quantum / 2) return;
        lastVal = val;
        paintValue(val, (val - min) / span);
        if (opts.onChange) opts.onChange(val);
        else if (opts.cmd) cmd(opts.cmd + ' ' + val.toFixed(decimals) + ' quiet');
      }
      cfgAttachDrag(ms, t, commitFrac);
      ms.__tile = t; ms.__span = ms.wrap;
      // The Slider is invisible and out of the hit path; a SliderValueChanged is only ever
      // Panorama echoing back a sync() write. Committing that echo walked every value downward.
      $.RegisterEventHandler('SliderValueChanged', ms.sl, function () { return; });
      var handle = {
        row: t, slot: t, label: nl,
        sync: function (value) {
          handle.__last = value;
          if (handle.__search) handle.__search.mirror(value);
          var val = quant(typeof value === 'number' ? value : min);
          if (val < min) val = min; else if (val > max) val = max;
          if (ms.__held) return;                       // the pointer owns it
          var frac = clamp01((val - min) / span);
          if (lastVal === null || Math.abs(val - lastVal) >= quantum / 2) {
            lastVal = val;
            if (Math.abs((ms.sl.value || 0) - frac) > quantum / span / 2) {
              ms.busy = true; ms.sl.value = frac; ms.busy = false;
            }
          }
          paintValue(val, frac);
        }
      };
      if (!opts.__proxy) handle.__search = cfgSearchAdd('slider', opts.label, t, opts.search,
        function (host) {
          var o = {}; for (var k in opts) o[k] = opts[k];
          o.__proxy = true;
          return sliderRow(host, o);
        });
      return handle;
    }
)CFJS"
R"CFJS(
    // ---- the checkbox chip ---------------------------------------------------------------
    // On/off as a 28px chip: a 13px checkbox and a 12/700 name. Consecutive chips flow into ONE
    // wrapping row (cfgChipLine), which is what replaced a full 64px tile per switch.
    // Same { row, set(on) } contract the tile had, so no render hook changes.
    //
    // `hint` feeds the tooltip when no explicit `tip` is given (the parameter order is kept so
    // no caller changes).
    function togglePill(parent, label, hint, onClick, tip, __proxy) {
      var line = cfgChipLine(parent);
      var c = mk('Panel', line); c.hittest = true;
      c.style.flowChildren = 'right'; c.style.height = '28px';
      c.style.paddingLeft = '8px'; c.style.paddingRight = '11px';
      c.style.marginRight = '6px'; c.style.marginBottom = UI.line.gap + 'px';
      c.style.borderRadius = '3px';
      trans(c, 'background-color, border-color', 0.08);
      var box = mk('Panel', c); box.hittest = false;
      box.style.width = '13px'; box.style.height = '13px'; box.style.borderRadius = '2px';
      box.style.verticalAlign = 'center'; box.style.marginRight = '8px';
      var mark = lbl(box, '✔', '#0c0e12ff', 9); mark.hittest = false;
      mark.style.fontWeight = 'bold';
      mark.style.horizontalAlign = 'center'; mark.style.verticalAlign = 'center';
      var tl = lbl(c, label, S.hint, 12); tl.hittest = false;
      tl.style.fontWeight = 'bold'; tl.style.whiteSpace = 'nowrap'; tl.style.verticalAlign = 'center';
      var on = false;
      function paint() {
        var hot = c.__hot || c.__down;
        c.style.backgroundColor = on ? '#2bb24c1c' : '#ffffff08';
        c.style.border = '1px solid ' + (hot ? '#ffffff40' : (on ? '#2bb24c80' : '#ffffff1f'));
        box.style.backgroundColor = on ? '#35cc59ff' : S.clear;
        box.style.border = '1px solid ' + (on ? '#35cc59ff' : '#ffffff40');
        mark.visible = on;
        tl.style.color = on ? S.value : S.hint;
      }
      hoverable(c, paint);
      c.SetPanelEvent('onactivate', function () { pressFlash(c, paint); if (onClick) onClick(); });
      var tipText = tip || hint;
      if (tipText) attachTip(c, tipText);
      paint();
      var handle = { row: c, pill: box, knob: mark, label: tl, set: function (v) {
        v = !!v;
        handle.__last = v;
        if (handle.__search) handle.__search.mirror(v);
        if (on === v) return;
        on = v; paint();
      } };
      if (!__proxy) handle.__search = cfgSearchAdd('toggle', label, c, null, function (host) {
        return togglePill(host, label, hint, onClick, tip, true);
      });
      return handle;
    }
)CFJS"
R"CFJS(
    // ---- PICK ONE OF N, as a single divided control ------------------------------------
    //
    // `items` is [[key, caption], ...]; `width` is any Panorama length ('224px' for a fixed
    // column, '100%' for a full-width bar). Cells split it evenly, share one outline, and only
    // the outer corners round -- so it reads as one control with a current value, which is what
    // it is. N bordered pills read as N independent decisions.
    //
    // Prefer this over two on/off pills whenever the options are mutually exclusive: a pair of
    // pills lets the operator express "neither", which is usually a state the backend does not
    // have and the UI cannot explain.
    function segmented(parent, items, width, onPick, font) {
      var group = mk('Panel', parent);
      group.style.flowChildren = 'right'; group.style.verticalAlign = 'center';
      group.style.width = (typeof width === 'number') ? (width + 'px') : width;
      // ONE outline for the whole control; the cells carry only their selection ground.
      group.style.borderRadius = '3px'; group.style.border = '1px solid #ffffff1a';
      endTileLine(parent);
      var btns = {}, n = items.length;
      for (var i = 0; i < n; i++) (function (key, caption, isFirst, isLast) {
        var b = mk('Panel', group);
        b.style.width = 'fill-parent-flow(1.0)';
        b.style.paddingTop = '6px'; b.style.paddingBottom = '6px';
        b.style.paddingLeft = '4px'; b.style.paddingRight = '4px';
        b.style.marginRight = '0px';
        b.style.borderTopLeftRadius = isFirst ? '2px' : '0px';
        b.style.borderBottomLeftRadius = isFirst ? '2px' : '0px';
        b.style.borderTopRightRadius = isLast ? '2px' : '0px';
        b.style.borderBottomRightRadius = isLast ? '2px' : '0px';
        var l = lbl(b, caption, S.label, font || 12);
        l.hittest = false; l.style.fontWeight = 'bold';
        l.style.horizontalAlign = 'center'; l.style.whiteSpace = 'nowrap';
        b.__seg = true;
        makeInteractive(b, l, function () { onPick(key); }, { fg: S.label });
        btns[key] = b;
      })(items[i][0], items[i][1], i === 0, i === n - 1);
      return {
        panel: group,
        btns: btns,
        sync: function (key) { for (var k in btns) setBtnOn(btns[k], k === key, S.accent); }
      };
    }

    // A labelled MODE ROW: the name in the 130px label column, the segmented control filling the
    // rest -- the same column grid the value rows use, so a card's labels line up top to bottom.
    // Returns the same { panel, btns, sync } handle segmented() does, plus { row, label }.
    function modeRow(parent, label, items, width, onPick, tip, font, __proxy) {
      var r = mk('Panel', parent); r.hittest = false; r.hittestchildren = true;
      r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginTop = (UI.line.gap + 1) + 'px';
      r.style.paddingLeft = UI.line.padH + 'px';
      endTileLine(parent);
      var nl = lbl(r, label, S.label, 13);
      nl.style.width = UI.line.label + 'px'; nl.style.whiteSpace = 'nowrap';
      nl.style.textOverflow = 'ellipsis'; nl.style.verticalAlign = 'center';
      nl.hittest = false;
      if (tip) attachTip(r, tip);           // whole row, not just the name
      var seg = segmented(r, items, 'fill-parent-flow(1.0)', onPick, font);
      seg.panel.style.marginLeft = UI.line.colGap + 'px';
      seg.row = r; seg.label = nl;
      var rawSync = seg.sync;
      seg.sync = function (key) {
        seg.__last = key;
        if (seg.__search) seg.__search.mirror(key);
        rawSync(key);
      };
      if (!__proxy) seg.__search = cfgSearchAdd('seg', label, r, null, function (host) {
        return modeRow(host, label, items, width, onPick, tip, font, true);
      });
      return seg;
    }

    // Tab treatment: TEXT ON A RULE, uppercase and letterspaced, with an accent underline on the
    // active one. tabify() at build time, styleTab(b, on) each render.
    function tabify(b) {
      b.__isTab = true;
      b.style.backgroundColor = S.clear;
      b.style.border = '1px solid ' + S.clear;
      b.style.paddingTop = '6px'; b.style.paddingBottom = UI.tab.padBottom + 'px';
      b.style.paddingLeft = '3px'; b.style.paddingRight = '3px';
      b.style.marginRight = '0px';
      b.style.width = 'fill-parent-flow(1.0)';
      b.__lbl.style.horizontalAlign = 'center';
      b.__lbl.style.fontSize = UI.tab.font + 'px';
      b.__lbl.style.letterSpacing = UI.tab.ls + 'px';
      b.__lbl.style.whiteSpace = 'nowrap';
      b.__fgBase = S.muted;
      paintBtn(b);
      return b;
    }
    // A tab's underline and text both come from paintBtn, so hover survives the per-frame
    // styleTab() calls instead of being repainted away.
    function styleTab(b, on) { setBtnOn(b, on); }

    // ---- THE colour-swatch grid ---------------------------------------------------------
    // Smoke, sky tint, clouds and scene light all use this one, so chip size and selected-state
    // treatment are decided once instead of drifting per tab.
    //
    // SELECTION IS A RING PLUS A TICK, never a fill change -- the fill IS the colour being
    // chosen and has to stay truthful. The ring lives on the wrapper because a border painted
    // onto a saturated fill is unreadable, and the tick carries a shadow because a white glyph
    // vanishes on white and yellow.
    //
    // `defs` entries are [key, fillHex, payload]. Two keys are special:
    //   'default'/'off' -- draws a dark neutral chip with a dash. Deliberately NOT transparent:
    //                      over the card it used to read as white, beside the actual white chip.
    //   'custom'        -- appended when opts.custom is set; calls opts.onCustom.
    function swatchGrid(parent, defs, onPick, opts) {
      opts = opts || {};
      // 24x18 chips with a 2px ring for the selection (Docs/features/filmmaker_config_panel.md). They WRAP, so a long
      // palette never pushes the pane sideways.
      var W = opts.chipW || 24, H = opts.chipH || 18, GAP = 7;
      endTileLine(parent);
      var host = mk('Panel', parent); host.hittest = false; host.hittestchildren = true;
      host.style.flowChildren = 'right'; host.style.width = '100%';
      host.style.marginTop = opts.marginTop || (UI.line.gap + 2) + 'px';
      host.style.paddingLeft = UI.line.padH + 'px';
      if (opts.label) {
        var cap = lbl(host, opts.label, S.label, 13); cap.hittest = false;
        cap.style.width = UI.line.label + 'px'; cap.style.verticalAlign = 'center';
        cap.style.whiteSpace = 'nowrap'; cap.style.textOverflow = 'ellipsis';
        cap.style.marginRight = UI.line.colGap + 'px';
      }
      var chipsHost = mk('Panel', host); chipsHost.hittest = false; chipsHost.hittestchildren = true;
      chipsHost.style.flowChildren = 'right-wrap'; chipsHost.style.width = 'fill-parent-flow(1.0)';
      chipsHost.style.paddingTop = '3px';
      var chips = {}, idx = 0;
      var api = { chips: chips, selected: '', row: host };

      function addChip(key, fill, payload, isCustom) {
        var plain = (key === 'default' || key === 'off');
        var wrap = mk('Panel', chipsHost);
        wrap.style.width = (isCustom ? 54 : W) + 'px'; wrap.style.height = H + 'px';
        wrap.style.marginRight = GAP + 'px'; wrap.style.marginBottom = GAP + 'px';
        wrap.style.borderRadius = '2px';
        wrap.style.border = '2px solid ' + S.clear;
        wrap.style.backgroundColor = isCustom ? '#454b55ff' : (plain ? '#20242bff' : fill);
        wrap.style.boxShadow = 'inset 0px 0px 0px 1px #ffffff26';
        trans(wrap, 'border-color', 0.1);
        var mark = lbl(wrap, isCustom ? 'Custom' : (plain ? '—' : ''),
          isCustom ? S.value : S.dim, isCustom ? 10 : 11);
        mark.hittest = false; mark.style.fontWeight = 'bold';
        mark.style.horizontalAlign = 'center'; mark.style.verticalAlign = 'center';
        if (!plain && !isCustom) mark.visible = false;
        wrap.hittest = true;
        if (payload && payload.length === undefined && typeof payload === 'string') attachTip(wrap, payload);
        wrap.SetPanelEvent('onmouseover', function () { if (!wrap.__on) wrap.style.border = '2px solid #ffffff55'; });
        wrap.SetPanelEvent('onmouseout', function () { if (!wrap.__on) wrap.style.border = '2px solid ' + S.clear; });
        wrap.SetPanelEvent('onactivate', function () {
          select(key);
          if (isCustom) { cfgColorAnchor = wrap; if (opts.onCustom) opts.onCustom(); }
          else onPick(key, payload);
        });
        chips[key] = { wrap: wrap, inner: wrap, tick: mark, plain: plain, custom: isCustom };
        idx++;
      }
      function select(key) {
        api.selected = key;
        for (var k in chips) {
          var on = (k === key);
          chips[k].wrap.__on = on;
          chips[k].wrap.style.border = '2px solid ' + (on ? S.value : S.clear);
        }
      }
      for (var i = 0; i < defs.length; i++) addChip(defs[i][0], defs[i][1], defs[i][2], false);
      if (opts.custom) addChip('custom', '#454b55ff', null, true);
      api.select = select;
      // Keeps the Custom chip showing the colour in use, so it reads as a swatch rather than a
      // permanently grey button.
      api.paintCustom = function (rgb) {
        if (chips.custom) {
          chips.custom.inner.style.backgroundColor = rgbHex(rgb);
          var lum = 0, weights = [0.2126, 0.7152, 0.0722];
          for (var ci = 0; ci < 3; ci++) {
            var c = Math.max(0, Math.min(255, rgb[ci])) / 255;
            lum += weights[ci] * (c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4));
          }
          chips.custom.tick.style.color = lum > 0.179 ? '#000000ff' : '#ffffffff';
        }
      };
      return api;
    }

    // #rrggbbaa only. Panorama's rgba() takes a 0..1 alpha, so rgba(r,g,b,255) is INVALID, and an
    // invalid style assignment THROWS and aborts the rest of the script.
    function hex2(n) {
      n = Math.round(n); if (n < 0) n = 0; if (n > 255) n = 255;
      var s = n.toString(16);
      return s.length < 2 ? '0' + s : s;
    }
    function rgbHex(c) { return '#' + hex2(c[0]) + hex2(c[1]) + hex2(c[2]) + 'ff'; }

)CFJS"
R"CFJS(
    // ---- rows ------------------------------------------------------------------------
    function row(parent) {
      var r = mk('Panel', parent); r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginTop = UI.line.gap + 'px'; r.style.verticalAlign = 'middle';
      r.hittestchildren = true;
      endTileLine(parent);
      return r;
    }
    // Groups controls INSIDE a card: a small letterspaced caption. Typography and space separate
    // the group -- a card inside a card would say the inner group is a peer of the outer one.
    function subHead(parent, text) {
      var r = mk('Panel', parent); r.hittest = false;
      r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginTop = '10px'; r.style.marginBottom = '1px';
      var l = lbl(r, text, S.muted, 11);
      l.hittest = false; l.style.fontWeight = 'bold'; l.style.letterSpacing = '1.5px';
      l.style.verticalAlign = 'center'; l.style.whiteSpace = 'nowrap';
      l.style.textTransform = 'uppercase';   // casing in the style, copy in natural case
      endTileLine(parent);
      return r;
    }

    // ---- the square switch ----------------------------------------------------------------
    // The card header's (42x22) and the rail row's (34x18) on/off. Square, not a pill
    // (Docs/features/filmmaker_config_panel.md); the colours are the ones the pills always had.
    function cfgSquareSwitch(parent, m, onClick) {
      var p = mk('Panel', parent); p.hittest = true;
      p.style.width = m.w + 'px'; p.style.height = m.h + 'px';
      p.style.borderRadius = '3px'; p.style.verticalAlign = 'center';
      trans(p, 'background-color, border-color', 0.08);
      var knob = mk('Panel', p); knob.hittest = false;
      knob.style.width = m.knob + 'px'; knob.style.height = m.knob + 'px';
      knob.style.borderRadius = '2px'; knob.style.backgroundColor = '#dfe5ecff';
      knob.style.verticalAlign = 'center'; knob.style.horizontalAlign = 'left';
      knob.style.marginLeft = Math.floor((m.h - 2 - m.knob) / 2) + 'px';
      knob.style.transitionProperty = 'transform';
      knob.style.transitionDuration = '0.12s';
      knob.style.transitionTimingFunction = 'ease-out';
      var on = false, hot = false;
      function paint() {
        p.style.backgroundColor = on ? (hot ? '#35cc59ff' : '#2bb24cff') : (hot ? '#ffffff26' : '#00000088');
        p.style.border = '1px solid ' + (on ? '#1c8a3aff' : (hot ? '#ffffff44' : '#ffffff26'));
        knob.style.transform = 'translateX(' + (on ? m.travel : 0) + 'px)';
      }
      p.SetPanelEvent('onmouseover', function () { hot = true; paint(); });
      p.SetPanelEvent('onmouseout', function () { hot = false; paint(); });
      p.SetPanelEvent('onactivate', function () { if (onClick) onClick(); });
      paint();
      return { panel: p, knob: knob, set: function (v) { v = !!v; if (v === on) return; on = v; paint(); },
               get: function () { return on; } };
    }

    // ---- the rail ----------------------------------------------------------------------
    // Every tab is a RAIL (one row per card, under group headings, each with its own switch) and
    // a PANE that shows only the selected card. A card that is not selected is hidden by its own
    // `visible`, never torn down, so every render hook keeps writing to it.
    //
    // Groups and their order, by card persistKey. Cards are created in fragment order; the rail
    // places each into its group here, so the rail order does not depend on include order.
    var CFG_RAIL = {
      camera: [['Motion', ['config.section.smoothview', 'config.section.handheld']],
               ['Weapon', ['config.section.weaponrecoil', 'config.section.weaponbob', 'config.section.viewmodel']],
               ['Lens', ['config.section.dof', 'config.section.flare']]],
      effects: [['Particles', ['fx.packs']], ['World', ['light.card', 'atmos.card', 'weather.card']]],
      environment: [['Grade', ['env.looks']], ['Atmosphere', ['env.sky', 'env.light', 'env.fog', 'env.wx']],
                    ['Scene', ['env.scene']]],
      ragdolls: [['Bodies', ['ragdoll.model', 'ragdoll.physics']], ['Detail', ['ragdoll.weapon', 'acting']]]
    };
    var cfgRails = {};
    var cfgSections = {};
    var cfgCards = [];
    function cfgRail(tab) {
      var r = cfgRails[tab];
      if (r) return r;
      r = { tab: tab, groups: {}, items: {}, sel: '', host: mk('Panel', cfgRailHost) };
      r.host.hittest = false; r.host.hittestchildren = true;
      r.host.style.flowChildren = 'down'; r.host.style.width = '100%';
      var defs = CFG_RAIL[tab] || [];
      for (var i = 0; i < defs.length; i++) {
        var g = mk('Panel', r.host); g.hittest = false; g.hittestchildren = true;
        g.style.flowChildren = 'down'; g.style.width = '100%';
        if (i) g.style.marginTop = UI.rail.groupGap + 'px';
        var h = lbl(g, defs[i][0], S.group, 11); h.hittest = false;
        h.style.fontWeight = 'bold'; h.style.letterSpacing = '1.8px'; h.style.textTransform = 'uppercase';
        h.style.paddingLeft = '2px'; h.style.marginBottom = '2px';
        for (var j = 0; j < defs[i][1].length; j++) r.groups[defs[i][1][j]] = g;
      }
      cfgRails[tab] = r;
      r.host.visible = (typeof activeTab === 'undefined') || tab === activeTab;
      return r;
    }
    function cfgPaintRailRow(it) {
      var sel = it.rail.sel === it.key, hot = it.row.__hot;
      it.row.style.backgroundColor = sel ? S.railSel : (hot ? S.railHover : S.clear);
      it.row.style.border = '1px solid ' + (sel ? (S.accent.substring(0, 7) + '59') : S.clear);
      it.title.style.color = sel ? S.value : (it.on ? S.label : S.muted);
      it.edge.style.backgroundColor = it.on ? it.accent : S.edgeOff;
    }
    function cfgRailAdd(tab, key, title, accent, card) {
      var r = cfgRail(tab);
      var g = r.groups[key];
      if (!g) {
        // A card the table does not know still gets a row, under its own trailing group.
        g = r.groups.__other;
        if (!g) {
          g = mk('Panel', r.host); g.hittest = false; g.hittestchildren = true;
          g.style.flowChildren = 'down'; g.style.width = '100%'; g.style.marginTop = UI.rail.groupGap + 'px';
          r.groups.__other = g;
        }
      }
      var it = { rail: r, key: key, accent: accent, card: card, on: true };
      var rw = mk('Panel', g); rw.hittest = true;
      rw.style.flowChildren = 'right'; rw.style.width = '100%';
      rw.style.height = UI.rail.rowH + 'px'; rw.style.marginTop = UI.rail.rowGap + 'px';
      rw.style.borderRadius = '3px'; rw.style.overflow = 'clip';
      trans(rw, 'background-color, border-color', 0.08);
      var edge = mk('Panel', rw); edge.hittest = false;
      edge.style.width = UI.rail.edge + 'px'; edge.style.height = '100%';
      var col = mk('Panel', rw); col.hittest = false;
      col.style.flowChildren = 'down'; col.style.width = 'fill-parent-flow(1.0)';
      col.style.verticalAlign = 'center'; col.style.marginLeft = '9px'; col.style.marginRight = '8px';
      var t = lbl(col, title, S.label, 12); t.hittest = false;
      t.style.fontWeight = 'bold'; t.style.letterSpacing = '0.8px';
      t.style.whiteSpace = 'nowrap'; t.style.textOverflow = 'ellipsis'; t.style.width = '100%';
      var sum = lbl(col, '', S.muted, 11); sum.hittest = false;
      sum.style.marginTop = '2px'; sum.style.whiteSpace = 'nowrap';
      sum.style.textOverflow = 'ellipsis'; sum.style.width = '100%';
      var swSlot = mk('Panel', rw); swSlot.hittest = false; swSlot.hittestchildren = true;
      swSlot.style.flowChildren = 'right'; swSlot.style.width = 'fit-children';
      swSlot.style.verticalAlign = 'center'; swSlot.style.marginRight = '8px';
      it.row = rw; it.edge = edge; it.title = t; it.sum = sum; it.swSlot = swSlot;
      hoverable(rw, function () { cfgPaintRailRow(it); });
      // The title and summary are cut with an ellipsis; the tooltip carries them whole.
      attachTip(rw, title);
      rw.SetPanelEvent('onactivate', function () { cfgSelectCard(tab, key, true); });
      r.items[key] = it;
      if (!r.sel) cfgSelectCard(tab, key, false);
      else { card.visible = false; cfgPaintRailRow(it); }
      return it;
    }
    // The first card of a tab in RAIL order -- the default selection when none is stored.
    function cfgRailFirst(tab) {
      var defs = CFG_RAIL[tab] || [], r = cfgRails[tab];
      if (!r) return '';
      for (var i = 0; i < defs.length; i++)
        for (var j = 0; j < defs[i][1].length; j++) if (r.items[defs[i][1][j]]) return defs[i][1][j];
      for (var k in r.items) return k;
      return '';
    }
    function cfgSelectCard(tab, key, persist) {
      var r = cfgRails[tab];
      if (!r || !r.items[key]) return false;
      var changed = r.sel !== key;
      r.sel = key;
      for (var k in r.items) {
        r.items[k].card.visible = (k === key);
        cfgPaintRailRow(r.items[k]);
      }
      if (changed) { try { cfgScrollY = 0; } catch (e) {} }
      if (persist) cmd('mirv_filmmaker settings ui config.' + tab + '.sel ' + key);
      return true;
    }
)CFJS"
R"CFJS(
    // The tab's footer: "Keep between demos" and the tab-wide RESET, on ONE line pinned to the
    // bottom of the column. The caller still passes its page -- footerCard(pageEffects) -- and
    // this redirects into that page's holder inside the pinned band.
    var cfgFooterHosts = [];
    function footerCard(parent) {
      var host = parent;
      for (var i = 0; i < cfgFooterHosts.length; i++) {
        if (cfgFooterHosts[i][0] === parent) { host = cfgFooterHosts[i][1]; break; }
      }
      var f = mk('Panel', host); f.hittest = false; f.hittestchildren = true;
      f.style.flowChildren = 'right'; f.style.width = '100%';
      endTileLine(parent);
      return f;
    }

    // ---- the card ---------------------------------------------------------------------
    // A card is the PANE's content for one rail row: a header (3x30 accent bar, 17px title, the
    // live summary, then action / Reset / switch), the sub-page strip, and the body. The
    // signature is the old collapsible card's, so no tab fragment had to change how it builds a
    // card; `collapsible` / `startOpen` no longer mean anything, because only one card shows.
    //
    // The returned panel is the BODY, and `body.GetParent().GetParent()` is the card itself
    // (card > clipper > body) -- kept because older fragment code reaches for it.
    function section(parent, title, collapsible, startOpen, persistKey, edgeColor) {
      var tab = parent.__tabKey || '';
      var key = persistKey || ('card.' + title);
      var edgeOn = edgeColor || S.accent;
      var s = mk('Panel', parent); s.hittest = false; s.hittestchildren = true;
      s.style.flowChildren = 'down'; s.style.width = '100%';
      endTileLine(parent);

      var hr = mk('Panel', s); hr.hittest = false; hr.hittestchildren = true;
      hr.style.flowChildren = 'right'; hr.style.width = '100%';
      var edge = mk('Panel', hr); edge.hittest = false;
      edge.style.width = '3px'; edge.style.height = '30px';
      edge.style.borderRadius = '1px'; edge.style.backgroundColor = edgeOn;
      edge.style.verticalAlign = 'center'; edge.style.marginRight = '10px';
      var col = mk('Panel', hr); col.hittest = false;
      col.style.flowChildren = 'down'; col.style.width = 'fill-parent-flow(1.0)';
      col.style.verticalAlign = 'center';
      var t = lbl(col, title, S.value, 17);
      t.hittest = false; t.style.fontWeight = 'bold'; t.style.letterSpacing = '1.2px';
      t.style.whiteSpace = 'nowrap'; t.style.textOverflow = 'ellipsis'; t.style.width = '100%';
      var note = lbl(col, '', S.muted, 12);
      note.hittest = false;
      note.style.marginTop = '2px'; note.style.whiteSpace = 'nowrap';
      note.style.textOverflow = 'ellipsis'; note.style.width = '100%';
      // Header controls (action / Reset / switch), created before anything else so they stay at
      // the right of the title whatever order the fragment fills them in.
      var slots = mk('Panel', hr); slots.hittest = false; slots.hittestchildren = true;
      slots.style.flowChildren = 'right'; slots.style.width = 'fit-children';
      slots.style.verticalAlign = 'center';

      var clipper = mk('Panel', s); clipper.hittest = false; clipper.hittestchildren = true;
      clipper.style.flowChildren = 'down';
      clipper.style.width = 'fill-parent-flow(1.0)';
      clipper.style.marginTop = '6px';
      // The sub-page strip is created INSIDE the body by the first __page() call, so anything a
      // card builds before its pages (a chip row, the recoil scope line) sits ABOVE the strip --
      // the rail's "top" controls (Docs/features/filmmaker_config_panel.md). Hidden until a second page exists.
      var strip = null;
      var body = mk('Panel', clipper); body.hittest = false; body.hittestchildren = true;
      body.style.flowChildren = 'down'; body.style.width = '100%';
      s.__cfgCard = { tab: tab, key: key, title: title, accent: edgeOn, body: body };

      var item = tab ? cfgRailAdd(tab, key, title, edgeOn, s) : null;
)CFJS"
R"CFJS(
      // ---- sub-pages -----------------------------------------------------------------------
      //   var page = card.__page('Aim');   // returns the panel to fill; the tab appears with it
      // The strip only shows from the second page on, so a one-page card looks like a plain card.
      var pages = [], pageTabs = [], curPage = 0;
      function applyPages() {
        for (var i = 0; i < pages.length; i++) {
          pages[i].visible = (i === curPage);
          styleTab(pageTabs[i], i === curPage);
        }
      }
      body.__page = function (head) {
        var idx = pages.length;
        if (!strip) {
          endTileLine(body);
          strip = mk('Panel', body); strip.hittest = false; strip.hittestchildren = true;
          // right-wrap: a card with many pages wraps its strip to a second line instead of
          // clipping the last tab off the pane edge.
          strip.style.flowChildren = 'right-wrap'; strip.style.width = '100%';
          strip.style.marginTop = (body.GetChildCount() > 1 ? 8 : 2) + 'px'; strip.style.marginBottom = '3px';
          strip.style.borderBottom = '1px solid #ffffff10';
          strip.visible = false;
        }
        var pg = mk('Panel', body); pg.hittest = false; pg.hittestchildren = true;
        pg.style.flowChildren = 'down'; pg.style.width = '100%';
        // A page carries the same __note/__setOpen surface a card does, so a group that used to
        // be its own card can become a page without its render hook changing.
        pg.__note = function () {};
        pg.__setOpen = function () {};
        pg.__getOpen = function () { return true; };
        pg.__card = s;
        pg.__cfgPage = { idx: idx, name: ('' + head).toUpperCase(), body: body };
        pages.push(pg);
        var tb = tabify(btn(strip, ('' + head).toUpperCase(), function () {
          curPage = idx; applyPages();
          cmd('mirv_filmmaker settings ui ' + key + '.page ' + idx);
        }, S.label));
        tb.style.width = 'fit-children';
        tb.style.paddingLeft = '0px'; tb.style.paddingRight = '0px';
        tb.style.marginRight = UI.tab.gap + 'px';
        pageTabs.push(tb);
        strip.visible = pages.length > 1;
        applyPages();
        return pg;
      };
      body.__setPage = function (i) {
        i = parseInt(i, 10) || 0;
        if (i < 0 || i >= pages.length) i = 0;
        curPage = i; applyPages();
      };
      body.__pageCount = function () { return pages.length; };

      // The summary feeds BOTH the pane header and the rail row. Idempotent: the render hooks
      // call it every frame, and a Label text write that changes nothing still costs layout.
      var noteSeen = null;
      body.__note = function (text) {
        var v = (text === null || text === undefined) ? '' : ('' + text);
        if (v === noteSeen) return;
        noteSeen = v;
        note.text = v;
        note.visible = v.length > 0;
        // Both copies can be cut with an ellipsis, so both keep the full text on hover.
        if (v) attachTip(note, v); else note.__tipText = null;
        if (item) { item.sum.text = v; item.row.__tipText = v ? (title + ' — ' + v) : title; }
      };
      body.__card = s;
      body.__summary = function () { return noteSeen || ''; };

      // ---- header slots ------------------------------------------------------------------
      // Each returns the SAME shape its predecessor did, so render hooks keep calling .set() /
      // .setDisabled() exactly as before.
      body.__headerAction = function (cap, onClick) {
        var b = mk('Panel', slots);
        b.style.borderRadius = '3px'; b.style.verticalAlign = 'center'; b.style.marginLeft = '8px';
        b.style.paddingTop = '5px'; b.style.paddingBottom = '5px';
        b.style.paddingLeft = '11px'; b.style.paddingRight = '11px';
        var l = lbl(b, cap, '#0c0e12ff', 12);
        l.hittest = false; l.style.fontWeight = 'bold'; l.style.whiteSpace = 'nowrap';
        b.__lbl = l;
        // A solid accent fill: at most ONE per card, the thing the card is for.
        hoverable(b, function (p) {
          p.style.backgroundColor = p.__down ? (S.accent.substring(0, 7) + 'cc')
            : (p.__hot ? (S.accent.substring(0, 7) + 'e6') : S.accent);
          p.style.border = '1px solid ' + S.accent;
        });
        b.SetPanelEvent('onactivate', function () { pressFlash(b, function () {}); if (onClick) onClick(); });
        b.__headerSlot = true;
        return b;
      };
      body.__headerReset = function (cmds, resetName) {
        cfgRegisterReset(resetName, function () { return cfgResetSpec(cmds); });
        var b = mk('Panel', slots);
        b.style.borderRadius = '3px'; b.style.verticalAlign = 'center'; b.style.marginLeft = '8px';
        b.style.paddingTop = '5px'; b.style.paddingBottom = '5px';
        b.style.paddingLeft = '11px'; b.style.paddingRight = '11px';
        var l = lbl(b, 'Reset', S.label, 12);
        l.hittest = false; l.style.fontWeight = 'bold'; l.style.whiteSpace = 'nowrap';
        makeInteractive(b, l, function () {
          if (b.__resetDisabled) return;
          cfgRunReset(cfgResetSpec(cmds));
        }, { fg: S.label });
        b.__bgBase = S.clear; paintBtn(b);
        b.__resetDisabled = false;
        b.setDisabled = function (disabled) {
          disabled = !!disabled;
          if (disabled === b.__resetDisabled) return;
          b.__resetDisabled = disabled;
          setBtnDisabled(b, disabled);
        };
        b.__headerSlot = true;
        return b;
      };
      // The card's master on/off: a square switch in the header AND the same switch on the rail
      // row, so a card is switched without selecting it. The accent edges and the summary follow.
      body.__headerPill = function (onClick, tip) {
        var big = cfgSquareSwitch(slots, UI.sw, onClick);
        big.panel.style.marginLeft = '8px';
        if (tip) attachTip(big.panel, tip);
        var small = item ? cfgSquareSwitch(item.swSlot, UI.swSm, onClick) : null;
        var on = null;
        function apply(v) {
          big.set(v); if (small) small.set(v);
          edge.style.backgroundColor = v ? edgeOn : S.edgeOff;
          // Stays readable when off: the grey edge already says the card is off.
          note.style.color = S.muted;
          if (item) { item.on = v; cfgPaintRailRow(item); }
        }
        apply(false);
        big.panel.__headerSlot = true;
        return { row: big.panel, pill: big.panel, knob: big.knob, set: function (v) {
          v = !!v; if (v === on) return; on = v; apply(v);
        } };
      };

      // The header's right-hand slot row, for a card whose header control is its own (the Scene
      // Objects preset button).
      body.__headerHost = function () { return slots; };
      body.__setOpen = function (value) { if (value && tab) cfgSelectCard(tab, key, false); };
      body.__getOpen = function () { return !tab || (cfgRails[tab] && cfgRails[tab].sel === key); };
      cfgSections[key] = body;
      cfgCards.push(body);
      return body;
    }
)CFJS"
R"CFJS(
    // ---- THE PER-TAB "KEEP BETWEEN DEMOS" CHECKBOX, shared by every tab footer -------------
    //
    // `group` is the C++ side's key ('camera' | 'effects' | 'ragdolls'); `stateKey` is the field
    // the state JSON reports it under. It sits at the LEFT of the footer line, the tab's RESET at
    // the right (Docs/features/filmmaker_config_panel.md).
    function keepToggle(card, group, stateKey, whatItKeeps) {
      var ui = true, hot = false;
      var r = mk('Panel', card); r.hittest = true;
      r.style.flowChildren = 'right'; r.style.width = 'fit-children';
      r.style.verticalAlign = 'center';
      var box = mk('Panel', r); box.hittest = false;
      box.style.width = '15px'; box.style.height = '15px'; box.style.borderRadius = '2px';
      box.style.verticalAlign = 'center'; box.style.marginRight = '8px';
      var mark = lbl(box, '✔', '#0c0e12ff', 10); mark.hittest = false;
      mark.style.fontWeight = 'bold';
      mark.style.horizontalAlign = 'center'; mark.style.verticalAlign = 'center';
      var t = lbl(r, 'Keep between demos', S.value, 13); t.hittest = false;
      t.style.verticalAlign = 'center'; t.style.whiteSpace = 'nowrap';
      attachTip(r, 'Carries ' + whatItKeeps + ' into the next demo you load. Off, each new demo '
        + 'starts from the defaults.');
      function paint() {
        box.style.backgroundColor = ui ? '#35cc59ff' : S.clear;
        box.style.border = '1px solid ' + (ui ? '#35cc59ff' : (hot ? '#ffffff66' : '#ffffff40'));
        mark.visible = ui;
      }
      r.__hoverPaint = function () { hot = !!r.__hot; paint(); };
      r.SetPanelEvent('onactivate', function () {
        // Set locally first: the state snapshot is only rebuilt when it changes, so two quick
        // clicks must not both send against the same stale value.
        ui = !ui; paint();
        cmd('mirv_filmmaker effects keep ' + group + ' ' + (ui ? 'on' : 'off'));
      });
      paint();
      return { row: r, sync: function (st) {
        // DEMO PANEL ONLY: "the next demo you load" cannot happen from the live-match panel.
        var isDemo = !!st.demo;
        r.visible = isDemo;
        if (!isDemo) return;
        var v = st[stateKey] !== false;
        if (v !== ui) { ui = v; paint(); }
      } };
    }

    // ---- THE PER-TAB RESET, shared by every tab footer -----------------------------------
    // `resolve()` returns { label, cmds, local } for whatever the tab currently shows. A danger
    // OUTLINE at the right of the footer line; one click resets the whole tab.
    function resetFooter(card, resolve, resetName) {
      var sig = '', hot = false;
      cfgRegisterReset(resetName, function () { return cfgResetSpec(resolve()); });
      var spacer = mk('Panel', card); spacer.hittest = false;
      spacer.style.width = 'fill-parent-flow(1.0)'; spacer.style.height = '1px';
      var b = mk('Panel', card); b.hittest = true;
      b.style.width = 'fit-children'; b.style.verticalAlign = 'center';
      b.style.paddingTop = '7px'; b.style.paddingBottom = '7px';
      b.style.paddingLeft = '12px'; b.style.paddingRight = '12px';
      b.style.borderRadius = '3px';
      trans(b, 'background-color, border-color');
      var l = lbl(b, 'RESET', '#e8a0a0ff', 12); l.hittest = false;
      l.style.fontWeight = 'bold'; l.style.letterSpacing = '1px'; l.style.whiteSpace = 'nowrap';
      function disarm() {}
      // A reset that would also take away work the user placed asks first (spec.confirm returns
      // { title, text, ok } when there is something to lose, else null).
      function run() {
        var e = resolve(), q = e && e.confirm ? e.confirm() : null;
        if (q) { cfgConfirm(q.title, q.text, q.ok, function () { cfgRunReset(cfgResetSpec(resolve())); }); return; }
        cfgRunReset(cfgResetSpec(e));
      }
      // Repaint only on a real change; render() runs every frame.
      function paint() {
        var e = resolve();
        var name = (e && e.label) ? e.label : 'TAB';
        var next = name + (hot ? 'h' : '');
        if (next === sig) return;
        sig = next;
        l.text = 'RESET ' + name;
        b.style.backgroundColor = hot ? '#c92a2a33' : S.clear;
        b.style.border = '1px solid ' + S.danger;
      }
      b.SetPanelEvent('onmouseover', function () { hot = true; paint(); });
      b.SetPanelEvent('onmouseout', function () { hot = false; paint(); });
      b.SetPanelEvent('onactivate', function () { run(); });
      if (resetName) cfgFooterPress[resetName] = run;
      paint();
      return { paint: paint, disarm: disarm, btn: b };
    }

    // ---- the option grid -----------------------------------------------------------------
    // A pick-one where EVERY OPTION IS ON SCREEN: the name in the label column, the options as
    // wrapping cells beside it. items: [[key, caption], ...]. Returns { row, btns, sync(key) }.
    // `perRow` is kept for the signature; the cells wrap to whatever the pane has room for.
    function optionGrid(parent, label, items, perRow, tip, onPick, font) {
      var r = mk('Panel', parent); r.hittest = false; r.hittestchildren = true;
      r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginTop = (UI.line.gap + 1) + 'px';
      r.style.paddingLeft = UI.line.padH + 'px';
      endTileLine(parent);
      var cap = lbl(r, label, S.label, 13);
      cap.style.width = UI.line.label + 'px'; cap.style.whiteSpace = 'nowrap';
      cap.style.textOverflow = 'ellipsis'; cap.style.paddingTop = '5px';
      cap.hittest = false;
      if (tip) attachTip(r, tip);
      var cells = mk('Panel', r); cells.hittest = false; cells.hittestchildren = true;
      cells.style.flowChildren = 'right-wrap'; cells.style.width = 'fill-parent-flow(1.0)';
      cells.style.marginLeft = UI.line.colGap + 'px';
      var btns = {};
      for (var i = 0; i < items.length; i++) (function (key, caption, i) {
        var b = mk('Panel', cells);
        b.style.marginRight = '4px'; b.style.marginBottom = '4px';
        b.style.paddingTop = '5px'; b.style.paddingBottom = '5px';
        b.style.paddingLeft = '10px'; b.style.paddingRight = '10px';
        b.style.borderRadius = '3px';
        var l = lbl(b, caption, S.label, font || 12);
        l.hittest = false; l.style.fontWeight = 'bold'; l.style.whiteSpace = 'nowrap';
        b.__grid = true;
        makeInteractive(b, l, function () { onPick(key, i); }, { fg: S.label });
        btns[key] = b;
      })(items[i][0], items[i][1], i);
      return {
        row: r, btns: btns, label: cap,
        sync: function (key) { for (var k in btns) setBtnOn(btns[k], k === key, S.accent); }
      };
    }

    // ---- the action group ---------------------------------------------------------------
    // A named set of commands: the name in the label column and the buttons wrapping beside it,
    // or -- with no name -- the buttons on ONE right-aligned line. Buttons never stack under a
    // gutter. items: [[caption, onClick], ...]. Returns { row, btns, note }.
    function actionGroup(parent, label, items, perRow, note) {
      endTileLine(parent);
      var r = mk('Panel', parent); r.hittest = false; r.hittestchildren = true;
      r.style.flowChildren = 'down'; r.style.width = '100%';
      r.style.marginTop = (UI.line.gap + 3) + 'px';
      var line = mk('Panel', r); line.hittest = false; line.hittestchildren = true;
      line.style.flowChildren = 'right'; line.style.width = '100%';
      line.style.paddingLeft = label ? UI.line.padH + 'px' : '0px';
      var cells = line;
      if (label) {
        var cap = lbl(line, label, S.label, 13); cap.hittest = false;
        cap.style.width = UI.line.label + 'px'; cap.style.whiteSpace = 'nowrap';
        cap.style.textOverflow = 'ellipsis'; cap.style.paddingTop = '5px';
        cells = mk('Panel', line); cells.hittest = false; cells.hittestchildren = true;
        cells.style.flowChildren = 'right-wrap'; cells.style.width = 'fill-parent-flow(1.0)';
        cells.style.marginLeft = UI.line.colGap + 'px';
      } else {
        var push = mk('Panel', line); push.hittest = false;
        push.style.width = 'fill-parent-flow(1.0)'; push.style.height = '1px';
      }
      var btns = [];
      for (var i = 0; i < items.length; i++) (function (caption, onClick) {
        var b = mk('Panel', cells);
        b.style.marginRight = label ? '4px' : '0px'; b.style.marginLeft = label ? '0px' : '6px';
        b.style.marginBottom = label ? '4px' : '0px';
        b.style.paddingTop = '5px'; b.style.paddingBottom = '5px';
        b.style.paddingLeft = '11px'; b.style.paddingRight = '11px';
        b.style.borderRadius = '3px';
        var l = lbl(b, caption, S.label, 12);
        l.hittest = false; l.style.fontWeight = 'bold'; l.style.whiteSpace = 'nowrap';
        makeInteractive(b, l, onClick, { fg: S.label });
        btns.push(b);
      })(items[i][0], items[i][1]);
      var noteLbl = null;
      if (note) {
        noteLbl = lbl(r, note, S.hint, 12);
        noteLbl.hittest = false; noteLbl.style.whiteSpace = 'normal';
        noteLbl.style.width = '100%'; noteLbl.style.marginTop = '4px';
        noteLbl.style.paddingLeft = UI.line.padH + 'px'; noteLbl.style.paddingRight = UI.line.padH + 'px';
      }
      return { row: r, btns: btns, note: noteLbl };
    }
)CFJS"
R"CFJS(
    // ---- the dropdown ---------------------------------------------------------------------
    // A one-line field (current value + caret) whose option list FLOATS over the panel as a
    // popover anchored under the field -- it never pushes the rows below it down. The list lives
    // on one root-level layer (so the pane's clip cannot cut it) and is re-placed every frame
    // while open, so it follows the field when the pane scrolls; it flips above the field when
    // there is no room below. Only one is open at a time; a press anywhere else, the wheel, or
    // the field leaving the screen closes it (cfgDropdownPump / api.wheel).
    //
    // items: [[value, caption], ...]. opts: { button: [caption, fn], onDelete: fn(value),
    // placeholder }. Returns { row, field, list, sync(value), setItems(items), setText(text),
    // close() }.
    var cfgDropOpen = null;
    var cfgDropdowns = {};   // by label, for the $.ConfigHud.dropdown() automation helper
    function cfgDropdownClose() {
      if (!cfgDropOpen) return;
      var d = cfgDropOpen; cfgDropOpen = null;
      tipHide();
      try { d.__setOpen(false); } catch (e) {}
    }
    var cfgDropLayerPanel = null;
    function cfgDropLayer() {
      if (cfgDropLayerPanel && cfgValid(cfgDropLayerPanel)) return cfgDropLayerPanel;
      var l = mk('Panel', root); l.hittest = false; l.hittestchildren = true;
      l.style.width = '100%'; l.style.height = '100%'; l.style.zIndex = '250';
      cfgDropLayerPanel = l;
      return l;
    }
    function cfgDropPlace(d) {
      try {
        var f = d.field, list = d.list;
        var p = f.GetPositionWithinWindow(), rp = root.GetPositionWithinWindow();
        var sx = root.actualuiscale_x || 1, sy = root.actualuiscale_y || 1;
        var fw = (f.actuallayoutwidth || 0) / sx, fh = (f.actuallayoutheight || 0) / sy;
        if (!(fw > 0) || fw > 1e30) return;
        var x = (p.x - rp.x) / sx, y = (p.y - rp.y) / sy + fh + 4;
        var lh = (list.actuallayoutheight || 0) / sy, rh = (root.actuallayoutheight || 0) / sy;
        if (lh > 0 && lh < 1e30 && rh > 0 && y + lh > rh - 8) y = (p.y - rp.y) / sy - 4 - lh;
        var w = Math.round(fw) + 'px', pos = Math.round(x) + 'px ' + Math.round(y) + 'px 0px';
        if (d.__w !== w) { d.__w = w; list.style.width = w; }
        if (d.__pos !== pos) { d.__pos = pos; list.style.position = pos; }
      } catch (e) {}
    }
    // The last OS-cursor position (window pixels): where a right-click menu opens, since
    // `oncontextmenu` carries no coordinates.
    var cfgCursorX = 0, cfgCursorY = 0;
    function cfgDropdownPump(st) {
      if (st && typeof st.mx === 'number') { cfgCursorX = st.mx; cfgCursorY = st.my; }
      if (!cfgDropOpen) return;
      // A menu has no field to follow (and is not laid out on its first frame); it stays until a
      // press lands outside it.
      if (!cfgDropOpen.__menu && !cfgPanelOnScreen(cfgDropOpen.field)) { cfgDropdownClose(); return; }
      if (!cfgDropOpen.__menu) cfgDropPlace(cfgDropOpen);
      if (!st || typeof st.mx !== 'number') return;
      var lmb = !!st.lmb && !!st.cursor;
      var down = lmb && !cfgDropOpen.__lastLmb;
      cfgDropOpen.__lastLmb = lmb;
      if (!down) return;
      var d = cfgDropOpen;
      if (!d.__menu && !cfgPanelOnScreen(d.field)) { cfgDropdownClose(); return; }
      if (!cfgColorOverRect(d.field, st.mx, st.my) && !cfgColorOverRect(d.list, st.mx, st.my)) cfgDropdownClose();
    }
    // A right-click menu at the cursor: items are [caption, action, shortcut, danger]. It shares
    // the dropdowns' layer and their close-on-press-outside pump (one popover open at a time).
    function cfgMenu(items) {
      cfgDropdownClose();
      var list = mk('Panel', cfgDropLayer()); list.hittest = true; list.hittestchildren = true;
      list.style.flowChildren = 'down'; list.style.width = '210px';
      list.style.backgroundColor = '#11151bfa'; list.style.border = '1px solid #ffffff26';
      list.style.borderRadius = '4px'; list.style.padding = '4px';
      list.style.boxShadow = '#000000b3 0px 6px 18px 0px';
      list.SetPanelEvent('onactivate', function () {});
      var menu = { field: list, list: list, __menu: true, __lastLmb: true,
        __setOpen: function (open) { if (!open) { try { list.DeleteAsync(0); } catch (e) {} } } };
      items.forEach(function (it) {
        if (!it) { // a divider
          var hr = mk('Panel', list); hr.hittest = false; hr.style.width = '100%'; hr.style.height = '1px';
          hr.style.backgroundColor = '#ffffff14'; hr.style.margin = '3px 0px';
          return;
        }
        var o = mk('Panel', list); o.hittest = true;
        o.style.flowChildren = 'right'; o.style.width = '100%'; o.style.height = '28px';
        o.style.paddingLeft = '10px'; o.style.paddingRight = '8px'; o.style.borderRadius = '3px';
        var cap = lbl(o, it[0], it[3] ? '#ff8a8aff' : S.label, 13); cap.hittest = false;
        cap.style.width = 'fill-parent-flow(1.0)'; cap.style.verticalAlign = 'center';
        if (it[2]) {
          var key = lbl(o, it[2], S.muted, 11); key.hittest = false; key.style.verticalAlign = 'center';
        }
        o.SetPanelEvent('onmouseover', function () { o.style.backgroundColor = '#ffffff12'; });
        o.SetPanelEvent('onmouseout', function () { o.style.backgroundColor = S.clear; });
        o.SetPanelEvent('onactivate', function () { cfgDropdownClose(); it[1](); });
      });
      try {
        var rp = root.GetPositionWithinWindow();
        var sx = root.actualuiscale_x || 1, sy = root.actualuiscale_y || 1;
        var x = (cfgCursorX - rp.x) / sx + 2, y = (cfgCursorY - rp.y) / sy + 2;
        var rw = (root.actuallayoutwidth || 0) / sx, rh = (root.actuallayoutheight || 0) / sy;
        var mh = items.length * 28 + 12;
        // Kept inside the panel even when the cursor is not (automation, another monitor).
        if (rw > 0 && x + 214 > rw) x = rw - 214;
        if (rh > 0 && y + mh > rh) y = rh - mh;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        list.style.position = Math.round(x) + 'px ' + Math.round(y) + 'px 0px';
      } catch (e) {}
      cfgDropOpen = menu;
      return menu;
    }
    // A yes/no question over the panel: a dimmed backdrop that swallows clicks, and a box with the
    // question, a Cancel and a (red) confirm button. Clicking the backdrop cancels.
    var cfgConfirmPanel = null;
    function cfgConfirmClose() {
      if (cfgConfirmPanel) { try { cfgConfirmPanel.DeleteAsync(0); } catch (e) {} }
      cfgConfirmPanel = null;
    }
    function cfgConfirm(title, text, okLabel, onOk) {
      cfgConfirmClose();
      cfgDropdownClose();
      var shade = mk('Panel', cfgDropLayer()); shade.hittest = true;
      shade.style.width = '100%'; shade.style.height = '100%'; shade.style.backgroundColor = '#000000a0';
      shade.SetPanelEvent('onactivate', cfgConfirmClose);
      var box = mk('Panel', shade); box.hittest = true;
      box.SetPanelEvent('onactivate', function () {});
      box.style.width = '420px'; box.style.flowChildren = 'down';
      box.style.horizontalAlign = 'center'; box.style.verticalAlign = 'center';
      box.style.padding = '18px 20px 16px 20px'; box.style.backgroundColor = '#151a21fc';
      box.style.border = '1px solid #ffffff26'; box.style.borderRadius = '6px';
      box.style.boxShadow = '#000000cc 0px 10px 30px 0px';
      var t = lbl(box, title, S.value, 16); t.style.fontWeight = 'bold';
      var body = lbl(box, text, S.label, 13); body.style.marginTop = '10px'; body.style.width = '100%';
      var acts = mk('Panel', box);
      acts.style.flowChildren = 'right'; acts.style.horizontalAlign = 'right'; acts.style.marginTop = '18px';
      var no = btn(acts, 'Cancel', cfgConfirmClose, S.label);
      var yes = btn(acts, okLabel, function () { cfgConfirmClose(); onOk(); }, '#ff8a8aff');
      yes.style.marginRight = '0px';
      cfgConfirmPanel = shade;
      return shade;
    }
    function cfgColorOverRect(panel, x, y) {
      try {
        if (!panel || !cfgPanelOnScreen(panel)) return false;
        var p = panel.GetPositionWithinWindow();
        return x >= p.x && y >= p.y && x < p.x + panel.actuallayoutwidth && y < p.y + panel.actuallayoutheight;
      } catch (e) { return false; }
    }
    function dropdownRow(parent, label, items, onPick, opts) {
      opts = opts || {};
      endTileLine(parent);
      var r = mk('Panel', parent); r.hittest = false; r.hittestchildren = true;
      r.style.flowChildren = 'down'; r.style.width = '100%';
      r.style.marginTop = (UI.line.gap + 1) + 'px';
      var line = mk('Panel', r); line.hittest = false; line.hittestchildren = true;
      line.style.flowChildren = 'right'; line.style.width = '100%';
      line.style.paddingLeft = UI.line.padH + 'px';
      if (label) {
        var cap = lbl(line, label, S.label, 13); cap.hittest = false;
        cap.style.width = UI.line.label + 'px'; cap.style.whiteSpace = 'nowrap';
        cap.style.textOverflow = 'ellipsis'; cap.style.verticalAlign = 'center';
      }
      var field = mk('Panel', line); field.hittest = true;
      field.style.flowChildren = 'right'; field.style.width = 'fill-parent-flow(1.0)';
      field.style.height = '28px'; field.style.borderRadius = '3px';
      field.style.marginLeft = label ? UI.line.colGap + 'px' : '0px';
      field.style.paddingLeft = '10px'; field.style.paddingRight = '10px';
      field.style.backgroundColor = '#0c0e12ff';
      trans(field, 'border-color', 0.08);
      var txt = lbl(field, opts.placeholder || '', S.hint, 12); txt.hittest = false;
      txt.style.width = 'fill-parent-flow(1.0)'; txt.style.whiteSpace = 'nowrap';
      txt.style.textOverflow = 'ellipsis'; txt.style.verticalAlign = 'center';
      var caret = lbl(field, '▾', S.muted, 12); caret.hittest = false;
      caret.style.verticalAlign = 'center'; caret.style.marginLeft = '6px';
      caret.style.transformOrigin = '50% 50%';
      var extra = null;
      if (opts.button) {
        extra = btn(line, opts.button[0], opts.button[1], S.label);
        extra.style.marginLeft = '6px'; extra.style.marginRight = '0px';
        extra.style.paddingTop = '6px'; extra.style.paddingBottom = '6px';
        extra.style.paddingLeft = '12px'; extra.style.paddingRight = '12px';
      }
      // The popover: on the root-level layer, placed under the field by cfgDropPlace.
      var list = mk('Panel', cfgDropLayer()); list.hittest = true; list.hittestchildren = true;
      list.style.flowChildren = 'down'; list.style.width = '200px';
      list.style.backgroundColor = '#11151bfa'; list.style.border = '1px solid #ffffff26';
      list.style.borderRadius = '3px'; list.style.padding = '4px';
      list.style.boxShadow = '#000000b3 0px 6px 18px 0px';
      list.SetPanelEvent('onactivate', function () {});   // a press on the list's padding is not "outside"
      list.visible = false;
      var cur = null, open = false, rows = {};
      var api = { row: r, label: cap, field: field, list: list, extra: extra, caption: txt };
      function paintField() {
        var hot = field.__hot;
        field.style.border = '1px solid ' + (open ? (S.accent.substring(0, 7) + '99') : (hot ? '#ffffff40' : '#ffffff1f'));
        caret.style.transform = open ? 'rotateZ(180deg)' : 'rotateZ(0deg)';
      }
      api.__setOpen = function (v) {
        open = !!v; list.visible = open; if (open) cfgDropPlace(api); paintField();
      };
      function paintRows() {
        for (var k in rows) {
          var on = (k === String(cur));
          rows[k].lbl.style.color = on ? S.accent : (rows[k].__hot ? S.value : S.label);
          rows[k].style.backgroundColor = rows[k].__hot ? '#ffffff0d' : S.clear;
        }
      }
      api.setItems = function (next) {
        items = next || [];
        list.RemoveAndDeleteChildren(); rows = {};
        if (!items.length) {
          var empty = lbl(list, opts.empty || 'No options available.', S.muted, 12);
          empty.hittest = false; empty.style.margin = '5px 6px';
        }
        // An item may carry a third element: a one-sentence tooltip for that option.
        for (var i = 0; i < items.length; i++) (function (value, caption, tip) {
          var o = mk('Panel', list); o.hittest = true;
          o.style.flowChildren = 'right'; o.style.width = '100%'; o.style.height = '26px';
          o.style.paddingLeft = '8px'; o.style.paddingRight = '4px'; o.style.borderRadius = '2px';
          var ol = lbl(o, caption, S.label, 12); ol.hittest = false;
          ol.style.width = 'fill-parent-flow(1.0)'; ol.style.verticalAlign = 'center';
          ol.style.whiteSpace = 'nowrap'; ol.style.textOverflow = 'ellipsis';
          o.lbl = ol;
          hoverable(o, paintRows);
          if (tip) attachTip(o, tip);
          o.SetPanelEvent('onactivate', function () {
            cur = value; txt.text = caption; txt.style.color = S.value;
            field.__tipText = tip || null;
            cfgDropdownClose(); paintRows();
            if (onPick) onPick(value);
          });
          if (opts.onDelete) {
            var del = btn(o, '×', function () { opts.onDelete(value); }, S.muted);
            del.style.marginRight = '0px'; del.style.paddingTop = '1px'; del.style.paddingBottom = '1px';
            del.style.paddingLeft = '7px'; del.style.paddingRight = '7px';
            del.__bgBase = S.clear; del.__brBase = S.clear; paintBtn(del);
          }
          rows[String(value)] = o;
        })(items[i][0], items[i][1], items[i][2]);
        paintRows();
      };
      api.sync = function (value) {
        if (value === cur) return;
        for (var i = 0; i < items.length; i++) {
          if (items[i][0] === value) { cur = value; txt.text = items[i][1]; txt.style.color = S.value;
            field.__tipText = items[i][2] || null; paintRows(); return; }
        }
      };
      api.setText = function (text, muted) {
        if (txt.text !== text) txt.text = text;
        txt.style.color = muted ? S.muted : S.value;
      };
      api.close = function () { if (cfgDropOpen === api) cfgDropdownClose(); };
      hoverable(field, paintField);
      field.__activate = function () {
        if (open) { cfgDropdownClose(); return; }
        cfgDropdownClose();
        cfgDropOpen = api; api.__lastLmb = true;
        api.__setOpen(true);
      };
      field.SetPanelEvent('onactivate', field.__activate);
      api.setItems(items);
      api.toggle = function () { field.__activate(); };
      if (label) cfgDropdowns[label] = api;
      return api;
    }

    // A labelled pick from a list too long for a segmented strip: a dropdown that issues
    // `<cmdPrefix> <value> quiet`. sync(value) mirrors console-side changes back.
    function cycleRow(parent, label, cmdPrefix, items, tip) {
      var d = dropdownRow(parent, label, items, function (v) { cmd(cmdPrefix + ' ' + v + ' quiet'); });
      // The state label explains the assignment; the selected curve explains its motion.
      if (tip) attachTip(d.label || d.row, tip);
      if (items.length) d.sync(items[0][0]);
      return { row: d.row, sync: function (value) { if (value) d.sync(value); } };
    }
)CFJS"
R"CFJS(
    // ---- the scroll well --------------------------------------------------------------------
    // A list inside the column that scrolls on its own (the Scene Objects tree and asset results):
    // a clipped box at most `maxH` tall whose content is moved by marginTop, with NO scrollbar --
    // nothing reserves space for one, so the rows never shift sideways when the list starts or
    // stops scrolling. Panorama gets no wheel here, so api.wheel offers each notch to the well under
    // the cursor first (cfgWellWheel); one at its top or bottom passes the notch on to the column.
    //   var w = scrollWell(parent, 303);  fill w.inner;  w.onNearEnd = fn (within 40 px of the end)
    var cfgWells = [];
    function scrollWell(parent, maxH) {
      var clip = mk('Panel', parent); clip.hittest = false; clip.hittestchildren = true;
      clip.style.width = '100%'; clip.style.overflow = 'clip'; clip.style.flowChildren = 'down';
      // NEVER SQUISH the content: Panorama's default overflow shrinks flow children to fit their
      // parent, so once the clip was given a height (0 while the list was still empty) the rows
      // were squished into it and the list measured 0 for good -- the in-game "empty results".
      var inner = mk('Panel', clip); inner.hittest = false; inner.hittestchildren = true;
      inner.style.width = '100%'; inner.style.flowChildren = 'down';
      inner.style.height = 'fit-children'; inner.style.overflow = 'noclip noclip';
      endTileLine(parent);
      var w = { clip: clip, inner: inner, y: 0, maxH: maxH, onNearEnd: null, __h: -1, __m: null };
      // The content's height: the rows' own heights added up (each fixes its height), or the inner
      // panel's, whichever is larger -- the sum does not depend on the clip's current height.
      function contentH() {
        var s = inner.actualuiscale_y || 1, sum = 0, n = inner.GetChildCount ? inner.GetChildCount() : 0;
        for (var i = 0; i < n; i++) {
          var c = inner.GetChild(i);
          if (c && c.visible) sum += (c.actuallayoutheight || 0) / s + 1;
        }
        var h = Math.max((inner.actuallayoutheight || 0) / s, sum);
        return h > 0 && h < 1e6 ? h : 0;
      }
      w.apply = function () {
        var ch = contentH(), view = Math.min(w.maxH, ch), maxY = Math.max(0, ch - view);
        if (w.y > maxY) w.y = maxY;
        if (w.y < 0) w.y = 0;
        var h = Math.round(view);
        if (h !== w.__h) { w.__h = h; clip.style.height = h > 0 ? h + 'px' : 'fit-children'; }
        var m = (-Math.round(w.y)) + 'px';
        if (m !== w.__m) { w.__m = m; inner.style.marginTop = m; }
        // CLIPPED IS NOT UNCLICKABLE. Rows scrolled out of the box are only hidden by the clip:
        // Panorama still hit-tests them where they sit, so the results scrolled up out of view lay
        // invisibly over the OUTLINER tab and the Cancel placing button and took their clicks
        // (user report 2026-10-10). Only rows that overlap the visible window take the mouse.
        var s = inner.actualuiscale_y || 1, top = 0, n = inner.GetChildCount ? inner.GetChildCount() : 0;
        for (var i = 0; i < n; i++) {
          var c = inner.GetChild(i);
          if (!c || !c.visible) continue;
          var ch2 = (c.actuallayoutheight || 0) / s;
          var inView = top + ch2 > w.y + 1 && top < w.y + view - 1;
          if (c.__wellIn !== inView) {
            c.__wellIn = inView;
            if (c.__wellHit === undefined) c.__wellHit = c.hittest;
            c.hittest = inView ? c.__wellHit : false;
            c.hittestchildren = inView;
          }
          top += ch2 + 1;
        }
        if (w.onNearEnd && ch > view && w.y >= maxY - 40) w.onNearEnd();
      };
      // One notch = three rows. True when the well moved (the column then stays put).
      w.wheel = function (notches) {
        var before = w.y;
        w.y -= notches * 90;
        w.apply();
        return Math.abs(w.y - before) > 0.5;
      };
      w.reset = function () { w.y = 0; w.apply(); };
      // Registered with the first well: renderHooks is created after this fragment runs.
      if (!cfgWells.length) renderHooks.push(function () {
        for (var i = 0; i < cfgWells.length; i++) if (cfgValid(cfgWells[i].clip)) cfgWells[i].apply();
      });
      cfgWells.push(w);
      return w;
    }
    // The well under the cursor takes the notch, innermost first (the order they were made in is
    // not depth, so the smallest one under the cursor wins).
    function cfgWellWheel(notches) {
      var best = null, bestH = 1e9;
      for (var i = 0; i < cfgWells.length; i++) {
        var w = cfgWells[i];
        if (!cfgValid(w.clip) || !cfgPanelOnScreen(w.clip) || !cfgColorOverRect(w.clip, cfgCursorX, cfgCursorY)) continue;
        if (w.__h > 0 && w.__h < bestH) { best = w; bestH = w.__h; }
      }
      return !!best && best.wheel(notches);
    }
)CFJS"
