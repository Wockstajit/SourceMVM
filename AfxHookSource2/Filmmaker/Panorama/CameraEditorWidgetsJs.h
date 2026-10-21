#pragma once

// Shared UI widget builders (mk/lbl/btn/section/row) and the custom dropdown control.
// JS fragment concatenated (via #include, as an adjacent raw string literal) into the
// kCameraEditorJs script body assembled in CameraEditorJs.h. Not a standalone script --
// shares scope/closures with the root IIFE. See CameraEditorJs.h for the entry point.

R"EDJS(
    // ---- small builders -------------------------------------------------
    function mk(type, parent, props) { return $.CreatePanel(type, parent, '', props || {}); }
    function lbl(parent, text, color, size) {
      var l = mk('Label', parent); l.text = text || '';
      if (color) l.style.color = color; if (size) l.style.fontSize = size + 'px';
      l.style.fontFamily = S.font; return l;
    }
    // ---- interactive button state ---------------------------------------
    // Every btn() carries its own visual state (selected / hovered / pressed / disabled) and a
    // single resolver writes the final colors. This exists because there is no :hover here:
    // hover/press must come from onmouseover/onmouseout (proven in FilmmakerGuiJs.h) and a
    // scheduled release, and because render() runs EVERY frame -- code that assigned
    // style.backgroundColor directly would overwrite the hover fill before it was ever seen,
    // which is exactly why the UI read as unresponsive. Per-frame code must therefore go
    // through btnSel()/btnEnable(), never straight to style.
    // A press-flash is scheduled 110ms out. Closing the workspace deletes the button first, and
    // painting it then throws "Underlying panel is deleted", which aborts the rest of the script
    // (bug reports 2026-10-13: skin changer, camera editor, timeline).
    function panelAlive(b) {
      try {
        if (!b || (b.IsValid && !b.IsValid())) return false;
        if (!b.__lbl || (b.__lbl.IsValid && !b.__lbl.IsValid())) return false;
        return true;
      } catch (e) { return false; }
    }
    function btnVisual(b) {
      if (!panelAlive(b)) return;
      var bg = b.__sel ? b.__bgSel : b.__bgBase;
      var fg = b.__sel ? b.__fgSel : b.__fgBase;
      var br = b.__sel ? b.__brSel : b.__brBase;
      if (b.__off) {
        // A disabled control has to read as disabled from across the panel, not on close
        // inspection. Dimming only the LABEL left a full-strength button sitting there looking
        // clickable; the fill and the border go with it, and the whole thing drops to 45% so the
        // difference is obvious next to an enabled sibling.
        bg = S.clear; fg = S.dim; br = '#ffffff08';
        b.style.opacity = '0.45';
        b.style.backgroundColor = bg;
        b.style.border = '1px solid ' + br;
        b.__lbl.style.color = fg;
        // Disabling mid-press would otherwise strand the button at its pressed scale.
        if (b.__press) b.style.transform = 'scale3d(1,1,1)';
        if (b.__isTab) applyTabUnderline(b);
        return;
      }
      b.style.opacity = '1';
      if (b.__isTab) {
        // A tab never takes a fill or a side border in any state -- only its label colour and
        // its underline move. Leaving this to the generic hover/down branches below is what put
        // a box back on it the moment the pointer crossed it.
        b.style.backgroundColor = S.clear;
        b.style.border = '1px solid ' + S.clear;
        b.__lbl.style.color = b.__sel ? (b.__fgSel || b.__accent) : (b.__hot ? S.value : b.__fgBase);
        applyTabUnderline(b);
        return;
      }
      if (b.__down) {
        bg = b.__bgDown || S.btnDown; br = b.__accent;
        if (b.__fgDown) fg = b.__fgDown;
      } else if (b.__hot) {
        bg = b.__sel ? b.__bgSelHover : (b.__bgHover || S.btnHover);
        br = b.__accent; fg = b.__sel ? fg : (b.__fgHover || S.value);
      }
      b.style.backgroundColor = bg;
      b.style.border = '1px solid ' + br;
      b.__lbl.style.color = fg;
      // Scale-on-press, opt-in (see interactive()). Written here rather than from the onactivate
      // handler so it stays in the ONE resolver that owns this panel's paint -- the same reason the
      // fills do.
      if (b.__press) b.style.transform = b.__down ? 'scale3d(0.96,0.96,1)' : 'scale3d(1,1,1)';
      // The `border` shorthand above resets border-bottom, so a tab has to restate its underline
      // after every repaint (including the hover/press repaints this function is called for).
      if (b.__isTab) applyTabUnderline(b);
    }
    // Mark a button selected/unselected. Idempotent + cheap (only repaints on a real change),
    // so it is safe to call from render() every frame.
    function btnSel(b, on) {
      on = !!on;
      if (!b || b.__bgBase == null) return b; // not an interactive() panel -- never paint undefined
      if (b.__sel === on) return b;
      b.__sel = on; btnVisual(b); return b;
    }
    // Install the interactive state machine on a HAND-ROLLED panel (one that was not built by
    // btn(), e.g. the follow toggles and the launcher action buttons, which have bespoke
    // internals). `lblPanel` is the Label whose color tracks the state.
    function interactive(b, lblPanel, onClick, opts) {
      opts = opts || {};
      b.__lbl = lblPanel;
      b.__sel = false; b.__hot = false; b.__down = false; b.__off = false;
      b.__bgBase = opts.bg || S.btnBg;
      b.__fgBase = opts.fg || S.value;
      b.__brBase = opts.border || S.cardBorder;
      b.__accent = opts.accent || S.accent;
      b.__bgSel = opts.selBg || S.btnOn;
      b.__bgSelHover = opts.selHover || S.btnOnHover;
      b.__fgSel = opts.selFg || b.__accent;
      b.__brSel = opts.selBorder || b.__accent;
      // Per-button hover/press fills. Without these btnVisual sends every non-selected button to
      // S.btnHover / S.btnDown -- a translucent white that WASHES OUT a solid-fill button (the gold
      // APPLY turns pale grey on hover, i.e. the primary action looks like it went inert exactly
      // when you point at it). Default null keeps every existing caller on the old behaviour.
      b.__bgHover = opts.bgHover || null; b.__fgHover = opts.fgHover || null;
      b.__bgDown = opts.bgDown || null; b.__fgDown = opts.fgDown || null;
      // Scale-on-press is OPT-IN. Two existing callers already own style.transform for their own
      // animation and a second writer would fight them: capBtn (capApply, CameraEditorCustomizeJs.h)
      // and any segmented button given a shake/refusal treatment. Only pass
      // press:true on a button that does not animate its own transform.
      b.__press = !!opts.press;
      if (b.__press) {
        b.style.transformOrigin = '50% 50%';
        b.style.transitionProperty = 'background-color, transform';
        b.style.transitionDuration = '0.08s';
        b.style.transitionTimingFunction = 'ease-out';
      }
      b.__tip = opts.tip || null;
      b.hittest = true;
      b.SetPanelEvent('onmouseover', function () {
        b.__hot = true; btnVisual(b);
        if (b.__tip) showTip(b, typeof b.__tip === 'function' ? b.__tip() : b.__tip);
      });
      b.SetPanelEvent('onmouseout', function () {
        b.__hot = false; b.__down = false; btnVisual(b); hideTip();
      });
      b.SetPanelEvent('onactivate', function () {
        hideTip();
        b.__down = true; btnVisual(b);
        try { $.Schedule(0.11, function () { if (!panelAlive(b)) return; b.__down = false; btnVisual(b); }); }
        catch (e) { if (panelAlive(b)) { b.__down = false; btnVisual(b); } }
        if (onClick) onClick();
      });
      btnVisual(b);
      return b;
    }
    function btnEnable(b, on) {
      var off = !on;
      if (!b || b.__bgBase == null || b.__off === off) return b;
      b.__off = off; b.hittest = !off; btnVisual(b); return b;
    }
    // Give a button its selected-state palette (call once at build time).
    function btnAccent(b, accent, selBg, selHover) {
      b.__accent = accent || S.accent;
      b.__bgSel = selBg || S.btnOn;
      b.__bgSelHover = selHover || S.btnOnHover;
      b.__fgSel = b.__accent;
      b.__brSel = b.__accent;
      btnVisual(b); return b;
    }
    // `opts` (optional) is forwarded to interactive() -- press/bgHover/fgHover/bgDown/fgDown etc.
    // Omitting it keeps every existing caller on exactly the previous behaviour.
    function btn(parent, text, onClick, color, opts) {
      var b = mk('Panel', parent);
      b.style.borderRadius = UI.btn.radius + 'px';
      b.style.paddingTop = UI.btn.padV + 'px'; b.style.paddingBottom = UI.btn.padV + 'px';
      b.style.paddingLeft = UI.btn.padH + 'px'; b.style.paddingRight = UI.btn.padH + 'px';
      b.style.marginRight = UI.btn.gap + 'px'; b.style.verticalAlign = 'center';
      var l = lbl(b, text, color || S.value, UI.btn.font); l.style.fontWeight = 'bold';
      var o = { fg: color || S.value };
      if (opts) for (var k in opts) o[k] = opts[k];
      interactive(b, l, onClick, o);
      return b;
    }
    // Button hierarchy on top of btn() (which stays the SECONDARY default):
    //   btnPrimary  = gold fill + dark text -- at most ONE per row; "the thing to click".
    //   btnDanger   = red-tinted fill + red border/text for destructive actions.
    // These set the BASE palette (not style directly) so btnVisual keeps ownership of the
    // final paint and hover/press still work on top of the variant's fill.
    function btnPrimary(parent, text, onClick) {
      var b = btn(parent, text, onClick, S.accentText);
      b.__bgBase = S.accent; b.__brBase = S.accent; b.__fgBase = S.accentText;
      b.__accent = S.accent; b.__bgSel = S.accent; b.__bgSelHover = S.accent;
      b.__fgSel = S.accentText; b.__brSel = S.accentText;
      btnVisual(b); return b;
    }
    function btnDanger(parent, text, onClick) {
      var b = btn(parent, text, onClick, S.dangerText);
      b.__bgBase = S.dangerBg; b.__brBase = S.dangerBorder; b.__fgBase = S.dangerText;
      b.__accent = S.dangerBorder; b.__bgSel = S.dangerBg; b.__bgSelHover = S.dangerBg;
      b.__fgSel = S.dangerText; b.__brSel = S.dangerText;
      btnVisual(b); return b;
    }
    // Tab treatment: tabify() at build time, styleTab(b, on) each render.
    // TEXT ON A RULE, never a filled pill (Docs/panorama/panorama_ui_guide.md). The original
    // designs shared this tab: `padding:0 0 13px 0; border-bottom:2px solid (on ? accent
    // : transparent); colour: on ? accent : #8b95a3`, and called a pill here out by
    // name ("never pill buttons, which compete with the real buttons"). This used to set
    // `__bgSel = S.bgRaised`, which put a filled box behind the ACTIVE tab in both this
    // workspace and Render while CONFIG drew the same strip flat -- the same control looking
    // like two different things depending on which panel was open.
    function tabify(b) {
      b.style.paddingTop = '8px'; b.style.paddingBottom = '7px';
      b.__lbl.style.horizontalAlign = 'center';
      b.__isTab = true;
      b.__bgBase = S.clear; b.__brBase = S.clear;
      // #8b95a3 off, exactly as the original designs had it -- S.label (#c8d3df) made an inactive tab
      // read as loud as a live control.
      b.__fgBase = '#afb9c6ff';
      b.__bgSel = S.clear; b.__bgSelHover = S.clear; b.__brSel = S.clear;
      b.__bgHover = S.clear; b.__bgDown = S.clear;
      b.__fgHover = S.value;
      btnVisual(b); applyTabUnderline(b);
      return b;
    }
    // The gold underline is a border-bottom, which btnVisual's `border` shorthand would wipe,
    // so tabs re-apply it after every repaint.
    function applyTabUnderline(b) {
      b.style.borderBottom = '2px solid ' + (b.__sel ? b.__accent : (b.__hot ? b.__accent + '66' : S.clear));
    }
    // The ACTIVE tab's text is the value tier and only its underline carries the accent
    // (Docs/features/camera-editor-feature.md) -- the same as the Config panel's tabs.
    function styleTab(b, on, accent) {
      if (accent) b.__accent = accent;
      b.__fgSel = S.value;
      var changed = (b.__sel !== !!on);
      b.__sel = !!on;
      if (changed) btnVisual(b);
      applyTabUnderline(b);
    }
)EDJS"
R"EDJS(
    function section(parent, title) {
      var s = mk('Panel', parent); s.style.flowChildren = 'down'; s.style.width = '100%';
      s.style.marginTop = '12px'; s.style.paddingTop = '9px'; s.style.paddingBottom = '11px';
      s.style.paddingLeft = '11px'; s.style.paddingRight = '11px';
      s.style.backgroundColor = S.sectionBg; s.style.borderRadius = '6px';
      s.style.border = '1px solid ' + S.cardBorder;
      s.style.boxShadow = S.cardShadow;
      var hr = mk('Panel', s); hr.style.flowChildren = 'right'; hr.style.marginBottom = '7px';
      var chip = mk('Panel', hr); chip.hittest = false;
      chip.style.width = '3px'; chip.style.height = '10px'; chip.style.borderRadius = '1px';
      chip.style.backgroundColor = S.accent; chip.style.verticalAlign = 'center'; chip.style.marginRight = '7px';
      var t = lbl(hr, title, S.label, 11); t.style.fontWeight = 'bold'; t.style.letterSpacing = '2px';
      t.style.verticalAlign = 'center';
      return s;
    }
    // Layered slider: rounded track + a colored fill bar drawn UNDER a real native Slider.
    // The native Slider stays the drag mechanism (this HUD context has no mouse-move event,
    // so a hand-rolled draggable control is impossible); we only upgrade the visual layer.
    // Returns { wrap, sl, setFill(frac 0..1) }.
    // knobW (optional, default 12): the redesigned value rows use an 8px handle.
    function makeSlider(parent, height, fillColor, knobW) {
      var wrap = mk('Panel', parent);
      wrap.style.width = 'fill-parent-flow(1.0)'; wrap.style.height = (height || 16) + 'px';
      wrap.style.verticalAlign = 'center';
      var track = mk('Panel', wrap); track.hittest = false;
      track.style.width = '100%'; track.style.height = '4px'; track.style.borderRadius = '2px';
      track.style.verticalAlign = 'center'; track.style.backgroundColor = S.trackBg;
      var fill = mk('Panel', wrap); fill.hittest = false;
      fill.style.width = '0%'; fill.style.height = '4px'; fill.style.borderRadius = '2px';
      fill.style.verticalAlign = 'center'; fill.style.horizontalAlign = 'left';
      fill.style.backgroundColor = fillColor || S.accent;
      var sl = $.CreatePanel('Slider', wrap, '', { direction: 'horizontal' }); sl.AddClass('HorizontalSlider');
      sl.style.width = '100%'; sl.style.height = '100%'; sl.style.verticalAlign = 'center';

      // WE OWN THE HANDLE.
      //
      // The reported bug was: shift-drag a slider, the number and the coloured fill are right,
      // and the HANDLE sits somewhere else entirely. That is structural, not a rounding slip --
      // the fill is a panel we draw from our own value, while the handle was the native Slider's
      // own thumb drawn from ITS value, and the two are different numbers. The native control is
      // the hit target across the whole track, so for the entire gesture it kept mapping the
      // pointer's ABSOLUTE x into sl.value while the pump wrote a RELATIVE, gain-damped value
      // into the same property; with Shift those differ by up to 12.5x. Correcting sl.value once
      // on release was not enough, because a native drag whose mouseup never reached Panorama
      // keeps re-writing it every frame afterwards.
      //
      // So the handle stops being the native thumb. The Slider stays for value storage and for
      // the no-cursor-pipe fallback, but it is taken out of the hit path and made invisible, and
      // the handle below is positioned by setFill from the SAME fraction that draws the fill and
      // the readout. One value, one writer, three visuals that cannot disagree.
      sl.style.opacity = '0.001';
      sl.hittest = false; sl.hittestchildren = false;
      var KNOB_W = knobW || 12;
      var knob = mk('Panel', wrap); knob.hittest = false;
      knob.style.width = KNOB_W + 'px'; knob.style.height = ((height || 16) - 4) + 'px';
      knob.style.borderRadius = (KNOB_W < 12 ? 2 : 3) + 'px'; knob.style.verticalAlign = 'center';
      knob.style.horizontalAlign = 'left'; knob.style.backgroundColor = S.value;
      knob.style.position = '0px 0px 0px';

      function place(frac) {
        frac = clamp01(frac);
        var sx = wrap.actualuiscale_x || 1;
        var cssW = (wrap.actuallayoutwidth || 0) / sx;
        // Unlaid-out panels report FLT_MAX, not 0; fall back to the percentage form until a real
        // width exists, and let the next sync place it properly.
        if (!(cssW > 0) || cssW > 1e30) {
          fill.style.width = (frac * 100).toFixed(1) + '%';
          return;
        }
        var half = KNOB_W / 2;
        var travel = cssW - KNOB_W;
        if (travel < 0) travel = 0;
        var x = frac * travel;
        knob.style.position = x.toFixed(1) + 'px 0px 0px';
        // The fill ends on the handle's CENTRE, so it neither pokes past it nor falls short.
        fill.style.width = (half + x).toFixed(1) + 'px';
      }
      return { wrap: wrap, sl: sl, knob: knob, setFill: place };
    }
)EDJS"
// Split here: MSVC caps a single raw string literal at ~16380 bytes (C2026). Adjacent literals
// are concatenated by the compiler, so this stays one JS blob. Added when makeSlider grew its
// own handle (see the note there) and pushed the preceding literal over the cap.
R"EDJS(

    // ---- cursor-driven slider drag ------------------------------------------------------
    // WHY THE NATIVE SLIDER'S OWN DRAG IS NOT USED FOR VALUE EDITING:
    //   1) TOO COARSE. A native Slider maps the pointer's ABSOLUTE x across the track, so one
    //      full sweep always covers the entire range -- 360 degrees of Roll across a ~250px
    //      track is ~1.4 degrees per pixel. You can get near a value; you cannot place one.
    //   2) THE GESTURE DIES WHEN THE POINTER LEAVES THE PANEL. Sweeping left off the inspector
    //      and over the preview window dropped the drag mid-adjustment.
    // Both go away by driving the drag from the raw OS-cursor pipe C++ already publishes on the
    // editor state every frame (st.mx / st.my / st.lmb / st.shift -- the same pipe the Customize
    // preview pan and the demo-bar speed scrubber run on). An ordinary drag maps the pointer
    // directly onto the track. Holding Shift switches to a relative, gain-damped fine drag that
    // keeps tracking until the button is released no matter where the pointer travels.
    //
    // The native Slider stays: it is still the thumb the user sees and the hit target the press
    // lands on. We simply overwrite its value every frame while WE own the gesture.
    // A CLICK (press + release without travelling) still jumps to the position under the pointer,
    // so coarse reach costs one click; a DRAG refines from wherever the value already is. Losing
    // jump-to-click along with the absolute mapping would have traded one problem for another.
    var cursorSliders = [], cursorLastLmb = false, cursorPressDbg = null;
    var DRAG_GAIN_FINE = 0.08;  // Shift held: the same sweep, ~6x finer
    var DRAG_SLOP = 3;          // device px of travel that still reads as a click, not a drag
    // Visible itself AND with every ancestor visible.
    //
    // `p.visible` alone is not enough and checking it was never the point: nothing hides a slider
    // ROW directly. A collapsed section hides its own container (`advancedPanel.visible = false`
    // in the Follow inspector) and every row inside keeps `visible: true` plus the layout
    // rectangle it had when it was last shown -- which is what the pressInInspector note below
    // already observed.
    //
    // pressInInspector only stops a hidden row claiming a press that landed OUTSIDE the inspector.
    // It cannot stop a hidden row inside the column being driven alongside the visible one at the
    // same y, because the pump hit-tests every registered slider off the cursor pipe and never
    // consults Panorama's own hit testing. That is the "two sliders move together" bug.
    // The walk runs off the end of our own tree into CS2's native HUD panels, so it is guarded
    // and FAILS OPEN: if a property read ever throws we answer "on screen" and fall back to the
    // old behaviour, rather than making every slider in the inspector undraggable. Same trade the
    // hit-test sync below makes -- a desync beats a dead panel.
    function panelOnScreen(p) {
      try {
        // Cycle/orphan stop, not a real depth limit.
        for (var q = p, guard = 0; q && guard < 64; ++guard) {
          if (q.IsValid && !q.IsValid()) return false;
          if (!q.visible) return false;
          q = q.GetParent ? q.GetParent() : null;
        }
      } catch (e) { return true; }
      return true;
    }

    function overPanel(p, x, y) {
      if (!p || !panelOnScreen(p)) return false;
      var a = p.GetPositionWithinWindow ? p.GetPositionWithinWindow() : null;
      if (!a) return false;
      var w = p.actuallayoutwidth || 0, h = p.actuallayoutheight || 0;
      return w > 0 && h > 0 && x >= a.x && x <= a.x + w && y >= a.y && y <= a.y + h;
    }
    // rec must carry { sl, busy }. apply(v) does the work for one 0..1 value (fill + readout +
    // console command); begin/end bracket the gesture (editbegin / editend).
    function attachCursorDrag(rec, host, apply, begin, end) {
      rec.__host = host; rec.__apply = apply; rec.__begin = begin; rec.__end = end;
      rec.__held = false; rec.__anchorX = 0; rec.__baseV = 0; rec.__restV = 0;
      cursorSliders.push(rec);
      return rec;
    }
    // Restores native dragging when the OS-cursor pipe is not usable. Without this, taking the
    // Slider out of the hit path above would leave these controls completely undraggable in the
    // exact failure mode cursorDragFallback exists for (a CS2 update moving the WndProc capture).
    function cursorPipeUsable() { return !!(st && typeof st.mx === 'number'); }
    function syncSliderHitTest() {
      var live = cursorPipeUsable();
      // Game Mode must change ownership, not appearance. Keep the custom rail/knob visible in
      // both modes; expose the native Slider only as an emergency UI-mode fallback when the OS
      // cursor coordinates are unavailable.
      var nativeFallback = !!(st && st.cursor && !live);
      for (var i = 0; i < cursorSliders.length; i++) {
        var rec = cursorSliders[i];
        if (!rec.sl || (rec.sl.IsValid && !rec.sl.IsValid())) continue;
        if (rec.__nativeFallback === nativeFallback) continue;
        rec.__nativeFallback = nativeFallback;
        rec.sl.hittest = nativeFallback; rec.sl.hittestchildren = nativeFallback;
        rec.sl.style.opacity = nativeFallback ? '1' : '0.001';
        if (rec.knob) rec.knob.visible = !nativeFallback;
      }
    }
    function cursorSliderPump() {
      syncSliderHitTest();
      if (!st || !st.cursor) {           // GAME-mouse mode: the pointer flies the camera, not the UI
        if (cursorLastLmb) cursorLastLmb = false;
        for (var q = 0; q < cursorSliders.length; q++) {
          var r0 = cursorSliders[q];
          if (r0.__held) { r0.__held = false; if (r0.__end) r0.__end(); }
        }
        return;
      }
      var lmb = !!st.lmb, mx = st.mx, my = st.my;
      var fine = !!st.shift;
      // A press only claims a slider if it landed inside the inspector column. Hidden sections can
      // retain their last layout rectangle, so without this a slider in a collapsed section could
      // claim a click that visually landed somewhere else entirely.
      // The Render workspace hides the inspector and shows its own settings column in the same
      // place; its value rows (the depth range) are claimed the same way.
      var pressInInspector = (lmb && !cursorLastLmb)
        ? (overPanel(inspector, mx, my) || (typeof renderSettingsCol !== 'undefined' && overPanel(renderSettingsCol, mx, my)))
        : false;
      // The Customize modal (z230, a ctx child) covers the inspector, and this pump never consults
      // Panorama's hit testing: while it is open only the modal's own sliders (rec.__modal, the wear
      // bars) may claim a press, or a click on a wear bar also moved whatever inspector slider sat
      // underneath it.
      var modalOpen = typeof custOverlay !== 'undefined' && custOverlay && custOverlay.visible;
      var pressInModal = (lmb && !cursorLastLmb && modalOpen) ? overPanel(custWin, mx, my) : false;
      // What the last press resolved to ($.CamEditor.dbgCursorPress): which surface it landed on and
      // which slider, if any, claimed it.
      if (lmb && !cursorLastLmb)
        cursorPressDbg = { mx: mx, my: my, inspector: pressInInspector, modal: modalOpen, inModal: pressInModal, claimed: -1 };
      for (var i = 0; i < cursorSliders.length; i++) {
        var rec = cursorSliders[i], host = rec.__host;
        if (!host || (host.IsValid && !host.IsValid())) continue;
        // Remember the resting value while the button is UP. Read at press time instead, and the
        // native Slider may already have jumped its own value to the click position on that same
        // frame -- which would make every press nudge the value before the drag even started.
        if (!lmb && !rec.__held) rec.__restV = clamp01(rec.sl.value);
        var pressHere = rec.__modal ? pressInModal : (pressInInspector && !modalOpen);
        if (lmb && !cursorLastLmb && pressHere && overPanel(host, mx, my)) {
          if (cursorPressDbg) cursorPressDbg.claimed = i;
          rec.__held = true; rec.__moved = false; rec.__anchorX = mx; rec.__baseV = rec.__restV;
          rec.__fine = fine;
          if (rec.__tile) { rec.__tile.__held = true; rec.__tile.__down = true; edPaintTile(rec.__tile); }
          if (rec.__begin) rec.__begin();
        }
        // A redesigned value row drags against its BAR (rec.__span), not the whole row, so one
        // bar width sweeps the range and the handle stays under the pointer.
        var spanP = (rec.__span && !(rec.__span.IsValid && !rec.__span.IsValid())) ? rec.__span : host;
        var w = spanP.actuallayoutwidth || 0;
        if (rec.__held && w > 8) {
          if (!rec.__moved && Math.abs(mx - rec.__anchorX) > DRAG_SLOP) {
            rec.__moved = true;
            if (rec.__tile) { rec.__tile.__moved = true; edPaintTile(rec.__tile); }
          }
          if (rec.__moved) {
            // Entering Shift-fine mode re-anchors at the current value and pointer so the change
            // of scale cannot jump. Releasing Shift needs no re-anchor: normal mode immediately
            // resumes the expected absolute pointer-to-track mapping.
            // A RELATIVE rec (rec.__relative -- every redesigned value tile; see edValueTile in
            // CameraEditorCardsJs.h) re-anchors on BOTH edges of Shift, because for it both
            // directions are a change of scale. An ABSOLUTE rec only needs the entering edge:
            // leaving Shift there resumes the pointer-to-track mapping, which needs no anchor.
            if (rec.__fine !== fine) {
              rec.__fine = fine;
              if (fine || rec.__relative) {
                rec.__baseV = clamp01(rec.sl.value);
                rec.__anchorX = mx;
              }
            }
            var v;
            if (rec.__fine || rec.__relative) {
              v = clamp01(rec.__baseV + ((mx - rec.__anchorX) / w) * (rec.__fine ? DRAG_GAIN_FINE : 1));
            } else {
              var a = host.GetPositionWithinWindow ? host.GetPositionWithinWindow() : null;
              if (!a) continue;
              v = clamp01((mx - a.x) / w);
            }
            rec.busy = true; rec.sl.value = v; rec.busy = false;
            rec.__apply(v);
            rec.__lastV = v; // authoritative: what the readout and the stored value hold
          }
        }
        if (!lmb && rec.__held) {
          // Released without travelling: treat it as a click and jump to the press point.
          // Resolved from __anchorX, not the current pointer, so a couple of px of hand-shake
          // between press and release cannot move the answer.
          // Jump-to-click is the ABSOLUTE model's coarse-reach affordance. A value tile is
          // relative by contract (Docs/panorama/panorama_ui_guide.md: the value "never jumping to the click"), so a
          // press that did not travel leaves it exactly where it was.
          if (!rec.__moved && w > 8 && !rec.__relative) {
            var a = host.GetPositionWithinWindow ? host.GetPositionWithinWindow() : null;
            if (a) {
              var cv = clamp01((rec.__anchorX - a.x) / w);
              rec.busy = true; rec.sl.value = cv; rec.busy = false;
              rec.__apply(cv);
              rec.__lastV = cv;
            }
          }
          rec.__held = false;
          // HAND THE THUMB BACK IN A KNOWN STATE.
          //
          // Two writers own sl.value for the whole gesture. The native Slider is still the hit
          // target across the entire track (makeSlider gives it width/height 100% with no hittest
          // restriction) and keeps mapping the pointer's ABSOLUTE x onto its own value, while this
          // pump writes a RELATIVE, gain-damped value into the same property every frame. With Shift
          // held our gain is 0.08 against the native ~1.0, so the two disagree by up to 12.5x --
          // which is why the artefact reads as a Shift bug rather than a general one.
          //
          // The per-frame state->slider sync that would reconcile them is gated off while
          // (sl.mousedown || __held), and those two flags come from two UNSYNCHRONISED input paths:
          // __held from the raw WndProc cursor latch, sl.mousedown from Panorama's own dispatch. So
          // on release there is a window -- or, when the release lands off-panel, no end at all --
          // in which nothing may correct the thumb: the readout and the stored value hold the correct
          // fine value while the thumb sits at the absolute-x one. That is the reported "shift-drag
          // to 0, number says 0, release, handle jumps somewhere else".
          //
          // Fixed by re-asserting OUR value once, on the falling edge of the pipe's lmb, which is the
          // one signal that reliably clears. Deliberately NOT by relaxing the mousedown guard:
          // CameraTimelineRangeJs.h records that a programmatic write raises SliderValueChanged a
          // frame later, and accepting those commits phantom values. This also repairs the knock-on
          // bug where the next press re-seeded __restV from a desynced sl.value, so the following
          // drag started from the wrong base and the VALUE jumped too, not just the thumb.
          if (typeof rec.__lastV === 'number') {
            rec.busy = true; rec.sl.value = rec.__lastV; rec.busy = false;
            rec.__restV = rec.__lastV;
          }
          if (rec.__tile) {
            rec.__tile.__held = false; rec.__tile.__moved = false; rec.__tile.__down = false;
            edPaintTile(rec.__tile);
          }
          if (rec.__end) rec.__end();
        }
      }
      cursorLastLmb = lmb;
    }
)EDJS"
// Split here: MSVC caps a single raw string literal at ~16380 bytes (C2026). Adjacent
// literals are concatenated by the compiler, so this stays one JS blob.
R"EDJS(
    // Safety net for the case where the OS-cursor probe is unavailable (a CS2 update moving the
    // WndProc capture, say): fall back to the native slider's own drag so the control still works,
    // just at the old coarse gain. Ignores our own programmatic writes, and anything delivered a
    // frame late after a gesture ended -- both of which would otherwise commit phantom values.
    function cursorDragFallback(rec, v) {
      return !rec.busy && !rec.__held && !!rec.sl.mousedown;
    }
    // A labelled control row: fixed-width label on the left, control filling the rest.
    //
    // Every inspector section used to invent its own row layout -- ragged buttons here, a centred
    // label there, sliders with a different label width again -- which is what made the column
    // read as five unrelated mini-layouts stacked on top of each other rather than one form.
    // Everything that takes a label now goes through this, so the controls line up down the
    // whole inspector.
    // opts (optional): { font, gap } -- a section with taller controls needs a label that keeps up
    // with them and more air between rows. Omitting it keeps every existing caller identical.
    function fieldRow(parent, label, labelW, opts) {
      opts = opts || {};
      var r = mk('Panel', parent);
      r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginTop = (opts.gap != null ? opts.gap : 7) + 'px'; r.style.verticalAlign = 'middle';
      var l = lbl(r, label, S.label, opts.font || 12);
      l.style.width = (labelW || 62) + 'px'; l.style.verticalAlign = 'center';
      var host = mk('Panel', r);
      host.style.flowChildren = 'right'; host.style.width = 'fill-parent-flow(1.0)';
      host.style.verticalAlign = 'center';
      // Back-reference to the row: callers that conditionally show a control have to hide the
      // LABEL with it, and the host alone leaves an orphaned label sitting in the flow.
      host.__row = r;
      return host;
    }
    // Mutually-exclusive choices as one segmented strip: every option visible, one click, no
    // popup. Deliberately NOT a dropdown -- and it also replaces the old "cycle" buttons, which
    // hid the other options behind repeated clicking and never showed what was available.
    // Mark the active one with btnSel() from render().
    function segmented(parent, items, onPick) {
      var out = [];
      for (var i = 0; i < items.length; i++) {
        (function (idx) {
          var b = btn(parent, items[idx][1], function () { onPick(items[idx][0], idx); }, S.value);
          b.style.width = 'fill-parent-flow(1.0)';
          b.style.paddingLeft = '2px'; b.style.paddingRight = '2px';
          b.style.paddingTop = '5px'; b.style.paddingBottom = '5px';
          b.style.marginRight = (idx === items.length - 1) ? '0px' : '3px';
          b.__lbl.style.horizontalAlign = 'center';
          b.__lbl.style.fontSize = '11px';
          b.__lbl.style.whiteSpace = 'nowrap';
          out.push(b);
        })(i);
      }
      return out;
    }
    function row(parent) {
      var r = mk('Panel', parent); r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginTop = '4px'; r.style.verticalAlign = 'middle'; return r;
    }
    // One-shot enter animation: snap the panel to its "from" pose, then release it to rest on the
    // NEXT frame so the transition actually plays -- setting both in one frame is a no-op, because
    // Panorama only animates a value that changes across a layout pass. Longhand transition
    // properties throughout: mixing the `transition` shorthand with longhands here has bitten this
    // codebase before (see restoreHudViewport in CameraTimelineJs.h).
    function motionToken(panel) {
      var token = (panel.__motionToken || 0) + 1;
      panel.__motionToken = token;
      return token;
    }
    function motionCurrent(panel, token) {
      return !!panel && panel.__motionToken === token;
    }
    function animateIn(panel, fromY, dur) {
      if (!panel || (panel.IsValid && !panel.IsValid())) return;
      var token = motionToken(panel);
      try {
        panel.style.transitionProperty = 'transform, opacity';
        panel.style.transitionTimingFunction = MO_ARRIVE;   // one arrival curve, see moEnter
        panel.style.transitionDuration = '0s';
        panel.style.transform = 'translateY(' + fromY + 'px)';
        panel.style.opacity = '0';
      } catch (e) { return; }
      $.Schedule(0, function () {
        try {
          if (!motionCurrent(panel, token) || (panel.IsValid && !panel.IsValid())) return;
          panel.style.transitionDuration = (dur || 0.16) + 's';
          panel.style.transform = 'translateY(0px)';
          panel.style.opacity = '1';
        } catch (e2) {}
      });
    }

)EDJS"
// Split here: MSVC caps a single raw string literal at ~16380 bytes (C2026). Adjacent literals
// are concatenated by the compiler, so this stays one JS blob.
R"EDJS(
    // ---- motion ----------------------------------------------------------
    // ONE curve for arrivals. cubic-bezier(0.16,1,0.3,1) is a strong exponential ease-out: it
    // covers most of the distance immediately and settles, which reads as "already there" rather
    // than as a thing being flown in. Every panel entrance in this HUD uses it so the interface
    // has a single hand.
    var MO_ARRIVE = UI.mo.arrive;
    var MO_TAB = UI.mo.tab;    // routine state change: swapping a tab body
    var MO_PANEL = UI.mo.panel;  // overlay/view transition: a workspace or popup appearing
    //
    // THE PANORAMA RULE that governs all of this: a value only animates if it CHANGES across a
    // layout pass, so the "from" pose and the "to" pose cannot be written in the same frame --
    // hence the $.Schedule(0) release. And because render() repaints every frame, an entrance
    // must be fired ONCE on a real state change; re-applying it per frame restarts it forever
    // and the panel never settles.
    function moEnter(panel, fromY, dur, fromScale) {
      if (!panel || (panel.IsValid && !panel.IsValid())) return;
      var token = motionToken(panel);
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
          if (!motionCurrent(panel, token) || (panel.IsValid && !panel.IsValid())) return;
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
      var token = motionToken(panel);
      var d = dur || MO_PANEL;
      try {
        panel.style.transitionProperty = 'transform';
        panel.style.transitionTimingFunction = MO_ARRIVE;
        panel.style.transitionDuration = '0s';
        panel.style.transform = 'translateY(' + fromY + 'px)';
      } catch (e) { return; }
      $.Schedule(0, function () {
        try {
          if (!motionCurrent(panel, token) || (panel.IsValid && !panel.IsValid())) return;
          panel.style.transitionDuration = d + 's';
          panel.style.transform = 'translateY(0px)';
        } catch (e2) {}
      });
    }

    // A tab body arriving. Deliberately a SHORT rise (6px) and no scale: a tab swap is a change of
    // content inside a frame that did not move, so anything larger reads as the whole panel
    // relaunching. Direction is fixed rather than derived from tab order -- a left/right slide
    // implies a carousel this UI does not have.
    function moTabSwap(panel) { moEnter(panel, 6, MO_TAB); }

    function clamp01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
    function num(v) { return (typeof v === 'number' && isFinite(v)) ? v : 0; }
    function finitePositive(v) { return typeof v === 'number' && isFinite(v) && v > 1; }
)EDJS"
// Split here: MSVC caps a single raw string literal at ~16380 bytes (C2026). Adjacent
// literals are concatenated by the compiler, so this stays one JS blob.
R"EDJS(
    // ---- Custom dropdown ------------------------------------------------
    // The embedded HLAE Panorama host does NOT instantiate the native DropDown popup: the control
    // toggles its "DropDownMenuVisible" class but never creates a DropDownMenu panel (verified live
    // over netcon -- zero menus exist in the whole tree while "open"), so nothing ever shows. So a
    // tappable FIELD bar drives our OWN floating popup, parented to the editor root at high z-index
    // (above the opaque inspector, not clipped by its scroll-overflow). Only one popup open at once.
    // ---- tooltips for ellipsised text ------------------------------------
    // Anything with text-overflow: ellipsis is hiding content, and for item names the hidden part is
    // often the DISTINGUISHING part -- "Doppler Phase 2" vs "Phase 4", "Case Hardened" vs its Blue
    // Gem variant. Truncating there makes two different items look identical, so the full string has
    // to stay reachable. The native UiToolkitAPI tooltip pair hard-cuts and is clipped by the
    // inspector in this embedded context, so use one top-level floating panel instead. Its token
    // guard is important: a quick skim across several controls must not let an old delayed show
    // reopen a tooltip after the pointer has already left it.
    var CE_TIP_W = 280, CE_TIP_DELAY = 0.18, CE_TIP_FADE = 0.14;
    var ceTipPanel = null, ceTipLbl = null, ceTipToken = 0;
    function ensureTipPanel() {
      if (ceTipPanel && (!ceTipPanel.IsValid || ceTipPanel.IsValid())) return;
      ceTipPanel = mk('Panel', ctx);
      ceTipPanel.id = 'CamEditorTip';
      ceTipPanel.hittest = false; ceTipPanel.visible = false;
      ceTipPanel.style.ignoreParentFlow = 'true';
      ceTipPanel.style.zIndex = '500';
      ceTipPanel.style.height = 'fit-children';
      ceTipPanel.style.flowChildren = 'down'; ceTipPanel.style.width = CE_TIP_W + 'px';
      ceTipPanel.style.backgroundColor = '#10141af2';
      ceTipPanel.style.border = '1px solid #aeb4bc66';
      ceTipPanel.style.borderRadius = '4px';
      ceTipPanel.style.boxShadow = '#000000d0 0px 4px 14px 2px';
      ceTipPanel.style.paddingLeft = '10px'; ceTipPanel.style.paddingRight = '10px';
      ceTipPanel.style.paddingTop = '7px'; ceTipPanel.style.paddingBottom = '8px';
      ceTipPanel.style.opacity = '0';
      ceTipPanel.style.transitionProperty = 'opacity, transform';
      ceTipPanel.style.transitionTimingFunction = MO_ARRIVE;
      ceTipLbl = lbl(ceTipPanel, '', S.value, 12);
      ceTipLbl.style.whiteSpace = 'normal'; ceTipLbl.style.width = '100%';
    }
    // A tooltip on a label that is NOT truncated only repeats what the eye is already reading;
    // on one that IS, it is the only way to read the rest. This measures which case a label is
    // in, so a caller can gate its tip per row instead of choosing once for every row.
    //
    // Measured, not guessed from character count: the usable width depends on the modal scale
    // and the font boost, so any fixed threshold would be wrong at some sizes. Same idiom as the
    // search caret probe -- an opacity-0 (NOT visible=false: a hidden panel is dropped from
    // layout and measures 0) nowrap Label, parented at top level and taken out of flow so it can
    // never disturb the list it is measuring for.
    var ceClipProbe = null;
    function ensureClipProbe() {
      if (ceClipProbe && (!ceClipProbe.IsValid || ceClipProbe.IsValid())) return;
      ceClipProbe = lbl(ctx, '', S.value, 12);
      ceClipProbe.id = 'CamEditorClipProbe';
      ceClipProbe.hittest = false;
      ceClipProbe.style.ignoreParentFlow = 'true';
      ceClipProbe.style.opacity = '0';
      ceClipProbe.style.whiteSpace = 'nowrap';
      ceClipProbe.style.position = '0px 0px 0px';
    }

    // True when `text` cannot fit inside `panel` at its current width -- i.e. the label is
    // showing an ellipsis. Both widths are normalised by their own panel uiscale so a probe at
    // top level compares fairly against a label inside the scaled modal.
    function textIsClipped(panel, text, fontPx) {
      try {
        if (!panel || (panel.IsValid && !panel.IsValid()) || !text) return false;
        var avail = panel.actuallayoutwidth || 0;
        // An unlaid-out panel reports FLT_MAX, not 0. Either extreme means "cannot tell yet",
        // and showing the tip is the safe answer: a redundant tip beats an unreadable name.
        if (!(avail > 0) || avail > 1e30) return true;
        ensureClipProbe();
        ceClipProbe.style.fontSize = (fontPx > 0 ? fontPx : 12) + 'px';
        ceClipProbe.text = '' + text;
        var want = ceClipProbe.actuallayoutwidth || 0;
        if (!(want > 0) || want > 1e30) return true;
        var availCss = avail / (panel.actualuiscale_x || 1);
        var wantCss = want / (ceClipProbe.actualuiscale_x || 1);
        return wantCss > availCss + 1; // 1px of slack so an exact fit is not "clipped"
      } catch (e) {
        return true;
      }
    }

    // clipPanel/clipFontPx are optional: pass them and the tip only appears when that panel is
    // actually ellipsised. The test happens INSIDE the hover delay below, not at onmouseover,
    // because layout has settled by then -- measuring on the hover edge can read a stale width.
    function showTip(panel, text, clipPanel, clipFontPx) {
      if (!panel || !text) return;
      var token = ++ceTipToken;
      try {
        $.Schedule(CE_TIP_DELAY, function () {
          if (token !== ceTipToken) return;
          try {
            if (!panel || (panel.IsValid && !panel.IsValid())) return;
            if (clipPanel && !textIsClipped(clipPanel, text, clipFontPx)) return;
            ensureTipPanel(); ceTipLbl.text = '' + text;
            var sx = ctx.actualuiscale_x || 1, sy = ctx.actualuiscale_y || 1;
            var a = panel.GetPositionWithinWindow ? panel.GetPositionWithinWindow() : null;
            if (!a) return;
            var x = a.x / sx, y = a.y / sy, ah = (panel.actuallayoutheight || 0) / sy;
            var rootW = (ctx.actuallayoutwidth || 0) / sx, rootH = (ctx.actuallayoutheight || 0) / sy;
            if (rootW > 0 && x + CE_TIP_W > rootW - 8) x = Math.max(8, rootW - 8 - CE_TIP_W);
            var estH = Math.ceil(('' + text).length / 40) * 18 + 18;
            var below = y + ah + 6;
            y = (rootH > 0 && below + estH > rootH - 8) ? Math.max(8, y - estH - 6) : below;
            ceTipPanel.style.position = Math.round(x) + 'px ' + Math.round(y) + 'px 0px';
            ceTipPanel.style.transitionDuration = '0s';
            ceTipPanel.style.opacity = '0'; ceTipPanel.style.transform = 'translateY(4px)';
            ceTipPanel.visible = true;
            $.Schedule(0, function () {
              if (token !== ceTipToken) return;
              try {
                ceTipPanel.style.transitionDuration = CE_TIP_FADE + 's';
                ceTipPanel.style.opacity = '1'; ceTipPanel.style.transform = 'translateY(0px)';
              } catch (e2) {}
            });
          } catch (e) {}
        });
      } catch (e3) {}
    }
    function hideTip() {
      var token = ++ceTipToken;
      if (!ceTipPanel || (ceTipPanel.IsValid && !ceTipPanel.IsValid()) || !ceTipPanel.visible) return;
      try {
        ceTipPanel.style.transitionDuration = CE_TIP_FADE + 's';
        ceTipPanel.style.opacity = '0'; ceTipPanel.style.transform = 'translateY(4px)';
        $.Schedule(CE_TIP_FADE + 0.02, function () {
          if (token !== ceTipToken) return;
          try { ceTipPanel.visible = false; } catch (e2) {}
        });
      } catch (e) {}
    }
    var customDrops = [], openDrop = null;
    // Open/close indicator. ROTATING one glyph beats swapping '▾' for '▴': a text swap is a hard cut
    // with no sense of the panel unfolding, and it makes the state depend on two glyphs whose optical
    // weights have to match. rotateZ is native Panorama -- CS2's own stylesheets use rotateZ(180deg)
    // for exactly this. Call once at build time (resting) and again from open/close.
    function caretSpin(caret, open) {
      if (!caret || (caret.IsValid && !caret.IsValid())) return;
      caret.style.transformOrigin = '50% 50%';
      caret.style.transitionProperty = 'transform';
      caret.style.transitionDuration = '0.12s';
      caret.style.transitionTimingFunction = 'ease-out';
      caret.style.transform = open ? 'rotateZ(180deg)' : 'rotateZ(0deg)';
    }
    function closeAllDrops() {
      if (!openDrop) return;
      // hittest=false explicitly: `pop` is created with hittest=true ONCE and this embedded HUD
      // Panorama context has already shown it does not reliably exclude invisible panels from hit
      // testing (see the "native DropDown never instantiates" note above) -- leaving hittest=true
      // on a merely-invisible popup risks it silently eating clicks at its last on-screen rect for
      // whatever real control happens to sit there next (reported: dropdowns stop opening entirely
      // after closing a previous one).
      openDrop.pop.visible = false; openDrop.pop.hittest = false; caretSpin(openDrop.caret, false);
      openDrop.field.__open = false; fieldPaint(openDrop.field);
      openDrop = null;
    }
    // Bar styling: distinct fill + border so it reads as clickable. 'empty' = dim/disabled.
    //
    // The interaction state lives ON the panel and one resolver writes the final colors -- the
    // same contract btnVisual() uses, and for the same reason: update() calls fieldStyle() EVERY
    // frame, so a hover fill written straight to style.backgroundColor would be repainted over
    // before it was ever seen. That is exactly why these bars read as dead: pointing at one did
    // nothing, and the only feedback that a click had registered was the popup appearing.
    function fieldStyle(field, empty) {
      field.__empty = !!empty;
      fieldPaint(field);
    }
    // __bgBase/__brBase are the panel's RESTING colours, supplied by the owner. The rich Customize
    // pickers tint their border with the selected item's rarity colour, and that used to be written
    // straight to style.border from update() -- a second writer for a property this resolver owns,
    // with no defined precedence, so whichever ran last won. The owner hands its colour in now and
    // fieldPaint stays the single writer. Defaults preserve the previous palette for every caller
    // that sets neither.
    // __accentBase is the OWNING WORKSPACE's accent, not the editor's. The Camera Editor is gold
    // (S.accent) and the Render workspace is neutral steel (S.render); hardcoding S.accent here
    // made every Render dropdown flash gold on open, the one colour that workspace never uses.
    function fieldPaint(field) {
      var accent = field.__accentBase || S.accent;
      var bg = field.__empty ? '#11151b' : (field.__bgBase || '#1b2230');
      var br = field.__empty ? '#ffffff12' : (field.__brBase || '#ffffff2e');
      if (!field.__empty) {
        if (field.__down) { bg = '#2c374a'; br = accent; }
        else if (field.__open) { bg = '#242e3f'; br = accent; }
        else if (field.__hot) { bg = '#242e3f'; br = '#ffffff5c'; }
      }
      field.style.backgroundColor = bg;
      field.style.border = '1px solid ' + br;
    }
    // Place the popup directly under its field and show it. dbgAbs() (a hand-rolled walk of
    // actualxoffset/actualyoffset up to ctx) does NOT account for an ancestor's SCROLL position --
    // it gave the field's pre-scroll offset, so a popup opened after scrolling the Customize modal's
    // right-hand column rendered detached from the field. GetPositionWithinWindow() is CS2's own
    // native API for exactly this (native scripts use it to follow a scrolled/moving element, e.g.
    // reference/ scripts/common/add_major_tokens_anim.js, scripts/operation/operation_main.js) --
    // it resolves screen position correctly regardless of ancestor scrolling. Same physical-px /
    // uiscale convention as dbgAbs. The popup is a child of root (full-screen at 0,0), so
    // window-relative coords == root-relative.
    function showDropPopup(rec) {
      var sx = root.actualuiscale_x || ctx.actualuiscale_x || 1, sy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
      var a = rec.field.GetPositionWithinWindow ? rec.field.GetPositionWithinWindow() : dbgAbs(rec.field);
      var x = a.x / sx, y = a.y / sy;
      var w = (rec.field.actuallayoutwidth || 0) / sx, h = (rec.field.actuallayoutheight || 0) / sy;
      // Popups live at ROOT level (so nothing clips them), which means an owner panel's transform
      // does not reach them: a field inside a scaled container would drop a full-size popup under a
      // shrunken row. rec.uiScale (number or getter, default 1 = unscaled, every non-Customize
      // dropdown) is the owner's scale; the popup keeps the field's LAYOUT width and is scaled from
      // its top-left instead, so its rows shrink with it rather than just the box getting narrower.
      // GetPositionWithinWindow() is transform-aware, so x/y need no correction -- only the drop
      // BELOW the field does, since h is a layout height.
      var k = rec.uiScale ? ((typeof rec.uiScale === 'function') ? (rec.uiScale() || 1) : rec.uiScale) : 1;
      // rec.popWiden (default 1) lets a slot ask for a popup wider than its field. The model pickers
      // use it: their rows are tiles with a large thumbnail, a name and a count, and at exactly the
      // field's width the name had to ellipsis. Only ever WIDER than the field -- a popup narrower
      // than the control it belongs to reads as a different, unrelated panel.
      var widen = rec.popWiden > 1 ? rec.popWiden : 1;
      var popW = Math.round(w * widen);
      // Clamp so a widened popup cannot run off the right edge of the screen. The popup is scaled from
      // its own top-left, so its ON-SCREEN width is popW * k, and x is already a screen coordinate.
      var rootW = (root.actuallayoutwidth || 0) / sx;
      if (rootW > 0) {
        var overflow = (x + popW * k) - (rootW - 8);
        if (overflow > 0) x = Math.max(8, x - overflow);
      }
      rec.pop.style.position = Math.round(x) + 'px ' + Math.round(y + h * k + 2) + 'px 0px';
      rec.pop.style.width = popW + 'px';
      if (k !== 1) {
        rec.pop.style.transformOrigin = '0% 0%';
        rec.pop.style.transform = 'scale3d(' + k + ',' + k + ',1)';
      }
      var wasOpen = rec.pop.visible;
      rec.pop.visible = true; rec.pop.hittest = true; caretSpin(rec.caret, true); openDrop = rec;
      rec.field.__open = true; fieldPaint(rec.field);
      // Unfold from the bar. The caret already rotates on open; without this the list itself
      // hard-cut into existence and the two halves of the same gesture disagreed. Only on the
      // OPENING call -- repositionOpenDrop() re-runs this every frame while the popup is up, and
      // re-firing the entrance there would hold it permanently at its first frame.
      // The scale is applied to the popup's own transform, so it must not fight the uiScale
      // branch above: that path writes transform itself and opts out of the entrance.
      if (!wasOpen && k === 1) moEnter(rec.pop, -4, MO_PANEL, 0.985);
    }
    function toggleDrop(rec) {
      var wasOpen = (openDrop === rec);
      closeAllDrops();
      if (!wasOpen && rec.opts.length) showDropPopup(rec);
    }
)EDJS"
// Split here: MSVC caps a single raw string literal at ~16380 bytes (C2026). Adjacent literals
// are concatenated by the compiler, so this stays one JS blob.
R"EDJS(
    // Popups are positioned ONCE, at open time, from the field's current on-screen offset -- but a
    // field inside a scrolling container (e.g. the Customize modal's right-hand column) keeps moving
    // after that: dragging the scrollbar with a popup open (or opening one right after a scroll
    // settles) left the popup anchored to the field's PRE-scroll position, visibly detached from the
    // bar it belongs to. Re-run the same placement math every frame the popup is open (called from
    // render(), which already runs every frame) so it tracks the field precisely, including mid-drag.
    function repositionOpenDrop() {
      if (openDrop && openDrop.pop && openDrop.pop.visible) showDropPopup(openDrop);
    }
    function rebuildPopup(rec) {
      rec.pop.RemoveAndDeleteChildren();
      var accent = rec.accent || S.accent;
      var selBg = rec.accentOn || S.btnOn;
      for (var i = 0; i < rec.opts.length; i++) (function (o) {
        var meta = o[2] || {};
        var prow = mk('Panel', rec.pop); prow.hittest = !meta.header; prow.style.width = '100%';
        prow.style.paddingTop = meta.header ? '8px' : '6px'; prow.style.paddingBottom = meta.header ? '4px' : '6px';
        prow.style.paddingLeft = '12px'; prow.style.paddingRight = '12px';
        var restColor = meta.header ? S.clear : (meta.selected ? selBg : S.clear);
        prow.style.backgroundColor = restColor;
        prow.style.transitionProperty = 'background-color';
        prow.style.transitionDuration = '0.08s';
        var t = lbl(prow, o[0], meta.color || (meta.header ? S.hint : (meta.selected ? accent : '#e6eaef')), meta.header ? 10 : 14);
        t.hittest = false; t.style.whiteSpace = 'nowrap'; t.style.textOverflow = 'ellipsis'; t.style.width = '100%';
        if (meta.header) t.style.letterSpacing = '1px';
        if (meta.disabled) { prow.style.opacity = '0.55'; t.style.color = meta.color || '#8993a1'; }
        // Hover feedback. These rows had NONE: the list gave no sign of which entry the pointer
        // was actually on, so picking a target from a long list was guesswork and the whole
        // control read as unresponsive.
        prow.SetPanelEvent('onmouseover', function () {
          if (!meta.header && !meta.disabled)
            prow.style.backgroundColor = meta.selected ? (rec.accentOnHover || S.btnOnHover) : S.btnHover;
        });
        prow.SetPanelEvent('onmouseout', function () { prow.style.backgroundColor = restColor; });
        if (!meta.header) prow.SetPanelEvent('onactivate', function () {
          if (meta.disabled) { rec.onPick(o[1], meta); return; }
          prow.style.backgroundColor = S.btnDown;
          if (!rec.multi) closeAllDrops(); rec.onPick(o[1], meta);
        });
      })(rec.opts[i]);
    }
    // Build a custom dropdown. onPick(value) fires on selection. Same .update() contract as before:
    //   update(opts, selValue [, sigOverride] [, displayOverride])
    // opts items are [label, value] or [label, value, {selected, color, disabled, header}]. The collapsed bar text
    // updates every frame (cheap); the popup ROWS only rebuild when sig changes, so live values in
    // labels (e.g. distance) don't tear down an open popup each tick.
    // popHost (optional, defaults to `root`) is the panel the floating popup is parented to.
    // It exists because the popup must live in the SAME top-level root as its field: CamEditorRoot
    // is zIndex 53 and the Render workspace root is a SIBLING at 54, so a Render-modal dropdown
    // whose popup hung off `root` opened *behind* the modal and looked like it never opened at all.
    // z-order between siblings is decided at the top level; a child's zIndex 400 cannot escape it.
    // accent (optional) is the owning workspace's accent colour; see fieldPaint.
    function customDrop(parent, id, onPick, popHost, accent, tip, multi) {
      var field = mk('Panel', parent); field.id = id; field.hittest = true;
      field.__accentBase = accent || null;
      field.__tip = tip || null;
      field.style.width = 'fill-parent-flow(1.0)'; field.style.flowChildren = 'right';
      // Geometry belongs to the constructor. fieldPaint() is shared with the taller rich
      // Customize pickers, so putting height/radius in the state painter collapsed those rows
      // from 48px to 30px on their first hover/click/open/close repaint.
      field.style.height = '30px'; field.style.borderRadius = '4px';
      field.style.verticalAlign = 'center'; fieldStyle(field, true);
      var disp = lbl(field, '', '#e6eaef', 14); disp.hittest = false;
      disp.style.width = 'fill-parent-flow(1.0)'; disp.style.textAlign = 'center';
      disp.style.verticalAlign = 'center'; disp.style.whiteSpace = 'nowrap'; disp.style.textOverflow = 'ellipsis';
      disp.style.paddingLeft = '20px'; // balance the right-side caret so text reads centered
      var caret = lbl(field, '▾', '#e6eaef', 15); caret.hittest = false;
      caret.style.verticalAlign = 'center'; caret.style.marginRight = '10px'; caret.style.marginLeft = '4px';
      caretSpin(caret, false);   // resting pose; open/close rotate it (see caretSpin)
      var pop = mk('Panel', popHost || root); pop.visible = false; pop.hittest = true;
      pop.style.position = '0px 0px 0px'; pop.style.zIndex = '400'; pop.style.flowChildren = 'down';
      pop.style.backgroundColor = '#0e1620fa'; pop.style.border = '1px solid ' + S.accent;
      pop.style.borderRadius = '4px'; pop.style.paddingTop = '3px'; pop.style.paddingBottom = '3px';
      pop.style.overflow = 'squish scroll'; pop.style.maxHeight = '340px';
      pop.style.boxShadow = '#000000d0 0px 4px 14px 2px';
      var rec = { field: field, disp: disp, caret: caret, pop: pop, onPick: onPick, opts: [], sig: '', multi: !!multi,
                  accent: accent || null,
                  // Selected-row fill/hover derived from the accent so a non-gold workspace does
                  // not fall back to the gold S.btnOn wash.
                  accentOn: accent ? (accent.substring(0, 7) + '33') : null,
                  accentOnHover: accent ? (accent.substring(0, 7) + '4d') : null };
      if (accent) pop.style.border = '1px solid ' + accent;
      field.style.transitionProperty = 'background-color';
      field.style.transitionDuration = '0.08s';
      field.SetPanelEvent('onmouseover', function () {
        field.__hot = true; fieldPaint(field);
        if (field.__tip) showTip(field, typeof field.__tip === 'function' ? field.__tip() : field.__tip);
      });
      field.SetPanelEvent('onmouseout', function () {
        field.__hot = false; field.__down = false; fieldPaint(field); hideTip();
      });
      field.SetPanelEvent('onactivate', function () {
        hideTip();
        field.__down = true; fieldPaint(field);
        try { $.Schedule(0.11, function () { field.__down = false; fieldPaint(field); }); }
        catch (e) { field.__down = false; fieldPaint(field); }
        toggleDrop(rec);
      });
      customDrops.push(rec);
      return {
        dd: field,
        update: function (opts, selValue, sigOverride, displayOverride) {
          rec.opts = opts || [];
          fieldStyle(field, rec.opts.length === 0);
          var disptext = displayOverride;
          if (disptext == null) {
            disptext = rec.opts.length ? rec.opts[0][0] : '—';
            for (var d = 0; d < rec.opts.length; d++) if (rec.opts[d][1] === selValue) disptext = rec.opts[d][0];
          }
          rec.disp.text = disptext;
          rec.caret.style.color = rec.opts.length ? '#e6eaef' : '#7a828c';
          var sig = sigOverride;
          if (sig == null) { sig = ''; for (var k = 0; k < rec.opts.length; k++) sig += rec.opts[k][0] + '|'; sig += '#' + selValue; }
          if (sig !== rec.sig) { rec.sig = sig; rebuildPopup(rec); }
        }
      };
    }
)EDJS"
