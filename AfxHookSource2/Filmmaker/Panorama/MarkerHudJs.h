#pragma once

// Panorama JS for the BO2-inspired camera-marker UI. Built ONCE into the HUD
// (CSGOHud) context by MarkerHud.cpp. It renders one of two things depending on
// the camera-path mode pushed in the "state" JSON:
//   * a centred SETTINGS CARD (hit-testable) while editing with the menu open;
//   * a PROMPT BANNER (click-through) during reposition / preview.
//   C++ -> JS : sets host attribute "state" (JSON) then runs $.MarkerHud.render().
//   JS  -> C++: buttons issue console commands (GameInterfaceAPI.ConsoleCommand
//               'mirv_filmmaker marker ...'), the same back-end the hotkeys use.
//
// VISUAL LANGUAGE: this is the F8 Movie Director card's language, deliberately and
// literally -- the same dark base + bluedots texture, the same gold accent-bar header,
// the same caption/label/value type ramp, the same separators, and the same soft bottom
// fade strip. See MovieHudJs.h; the token block below mirrors its `S` on purpose. Two
// popups that appear over the same demo, built from the same project, should not look
// like they came from different programs.
//
// INTERACTION: every control carries hover + press feedback through one resolver
// (paintCtl). The previous card had NO feedback of any kind -- pointing at a control did
// nothing and a click's only confirmation was the value changing -- which is what dated
// it more than any colour did. There is no :hover in this in-game Panorama context and no
// stylesheet of ours to hang classes on, so it comes from panel events.
//
// BANNER PLACEMENT: while the Camera Editor (or Config) owns the screen, the live game is
// rendered into a SUB-RECT of the window, not the whole window. A screen-centred banner
// therefore floated over the editor chrome rather than over the shot. The banner reads the
// "previewrect" attribute the editor JS publishes on #CamEditorRoot / #ConfigHudRoot and
// transforms itself into that rect -- scaled by the same factor as the game view, so it
// sits in the preview at the preview's scale. With no workspace open the rect is the whole
// window and it behaves exactly as before.

namespace Filmmaker {

inline const char* kMarkerHudJs = R"MKJS(
(function () {
  try {
    var existing = $('#MarkerHudRoot'); if (existing) existing.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    // ---- design tokens (mirrors MovieHudJs.h -- the F8 card) ----
    var S = {
      cardW: '384px',
      bg: 'rgba(0,0,0,0.9)',
      dots: 'url("s2r://panorama/images/backgrounds/bluedots_large_png.vtex")',
      fadeMask: 'url("s2r://panorama/images/masks/top-bottom-fade-4_png.vtex")',
      fadeH: '40px',
      accent: '#f0b323ff',
      accentText: '#151004ff',
      freeze: '#80bdffff',
      label: '#c8d3dfff',
      value: '#eef2f6ff',
      cap: '#a7b3beff',
      hint: '#afb9c6ff',   // static supporting text. Tier table: CameraEditorJs.h
      dim: '#6b7480ff',
      btnBg: '#ffffff14',
      btnHover: '#ffffff26',
      btnDown: '#ffffff3d',
      btnBorder: '#ffffff1a',
      // Selected-state fills are spelled out rather than built by appending an alpha suffix to
      // `accent`: these tokens already carry their own alpha byte, so '#f0b323ff' + '33' would
      // produce a 10-digit colour that Panorama rejects -- and a rejected style value throws.
      accentSel: '#f0b32333', accentSelHot: '#f0b32352',
      freezeSel: '#80bdff33', freezeSelHot: '#80bdff52',
      danger: '#ff928eff',      // Lc 60 on the 90%-black card; #ff6b6b measured 49
      dangerEdge: '#c92a2aff',
      sep: '#80808030',
      clear: 'rgba(0,0,0,0)',   // NEVER style.X = '' -- it throws and aborts the script
      font: 'Stratum2, "Arial Unicode MS"'
    };

    function cmd(c) {
      try { GameInterfaceAPI.ConsoleCommand(c); }
      catch (e) { $.Msg('[markerhud] cmd failed: ' + e + '\n'); }
    }
    function mk(type, parent) { return $.CreatePanel(type, parent, '', {}); }
    function lbl(parent, text, color, size) {
      var l = mk('Label', parent); l.text = text || '';
      if (color) l.style.color = color;
      if (size) l.style.fontSize = size + 'px';
      return l;
    }

    // ---- interaction state ----------------------------------------------
    // One resolver owns the final colours so hover/press can never be half-applied, and so
    // a control's SELECTED tint (Freeze blue, Auto-Snap gold) still shows a hover on top of
    // itself instead of being a dead block of colour.
    function paintCtl(b) {
      var bg = b.__sel ? b.__bgSel : S.btnBg;
      if (b.__down) bg = S.btnDown;
      else if (b.__hot) bg = b.__sel ? b.__bgSelHot : S.btnHover;
      b.style.backgroundColor = bg;
      b.style.border = '1px solid ' + ((b.__hot || b.__down) ? (b.__accent || '#ffffff40') : S.btnBorder);
    }
    function ctlSel(b, on, accent, selBg, selHot) {
      on = !!on;
      var restyled = false;
      if (accent && b.__accent !== accent) { b.__accent = accent; restyled = true; }
      if (selBg && b.__bgSel !== selBg) { b.__bgSel = selBg; restyled = true; }
      if (selHot && b.__bgSelHot !== selHot) { b.__bgSelHot = selHot; restyled = true; }
      if (b.__sel === on && !restyled) return b;
      b.__sel = on; paintCtl(b); return b;
    }
    function interactive(b, onClick, opts) {
      opts = opts || {};
      b.hittest = true;
      b.__sel = false; b.__hot = false; b.__down = false;
      b.__accent = opts.accent || S.accent;
      b.__bgSel = opts.selBg || S.accentSel;
      b.__bgSelHot = opts.selHot || S.accentSelHot;
      b.style.transitionProperty = 'background-color';
      b.style.transitionDuration = '0.08s';
      b.style.transitionTimingFunction = 'ease-out';
      b.SetPanelEvent('onmouseover', function () { b.__hot = true; paintCtl(b); });
      b.SetPanelEvent('onmouseout', function () { b.__hot = false; b.__down = false; paintCtl(b); });
      b.SetPanelEvent('onactivate', function () {
        b.__down = true; paintCtl(b);
        try { $.Schedule(0.11, function () { b.__down = false; paintCtl(b); }); }
        catch (e) { b.__down = false; paintCtl(b); }
        if (onClick) onClick();
      });
      paintCtl(b);
      return b;
    }

    // Full-screen, click-through container (only the settings card is hittable).
    var root = $.CreatePanel('Panel', ctx, 'MarkerHudRoot', {});
    root.hittest = false;
    root.style.width = '100%';
    root.style.height = '100%';
    root.style.zIndex = '60'; // z-layer map: PanoramaBridge.h

    // Full-screen hit-test CATCHER, shown only while the settings card is open. It sits
    // BEHIND the card (created first) and absorbs any click that misses the card, so CS2's
    // spectator can't switch players out from under the editor. The card's buttons are
    // later siblings, so they still receive their own clicks.
    var catcher = $.CreatePanel('Panel', root, '', {});
    catcher.hittest = true;
    catcher.style.width = '100%';
    catcher.style.height = '100%';
    catcher.visible = false;
    catcher.SetPanelEvent('onactivate', function () { /* swallow */ });

    // ============================ SETTINGS CARD ============================
    // Two stacked pieces, exactly like the F8 card: a SOLID content area carrying every row
    // (so text and controls always sit on an opaque, readable panel), then a fixed-height
    // FADE STRIP that starts at the same colour + texture and dissolves to nothing. That
    // gives the card a soft bottom edge without ever fading a control.
    var card = $.CreatePanel('Panel', root, '', {});
    card.hittest = true;
    card.style.width = S.cardW;
    card.style.horizontalAlign = 'center';
    card.style.verticalAlign = 'center';
    card.style.flowChildren = 'down';
    card.style.fontFamily = S.font;

    var body = mk('Panel', card);
    body.hittest = true;
    body.style.width = '100%';
    body.style.flowChildren = 'down';
    body.style.backgroundColor = S.bg;
    body.style.backgroundImage = S.dots;
    body.style.backgroundSize = '620px 620px';
    body.style.backgroundRepeat = 'repeat';
    body.style.backgroundImgOpacity = '0.03';
    body.style.borderTopLeftRadius = '5px';
    body.style.borderTopRightRadius = '5px';
    body.style.color = S.label;
    body.style.fontSize = '14px';

    var fade = mk('Panel', card);
    fade.hittest = false;
    fade.style.width = '100%';
    fade.style.height = S.fadeH;
    fade.style.backgroundColor = S.bg;
    fade.style.backgroundImage = S.dots;
    fade.style.backgroundSize = '620px 620px';
    fade.style.backgroundRepeat = 'repeat';
    fade.style.backgroundImgOpacity = '0.03';
    fade.style.opacityMask = S.fadeMask;

    function mkSep(parent) {
      var p = mk('Panel', parent);
      p.style.width = '100%'; p.style.height = '1px'; p.style.backgroundColor = S.sep;
      return p;
    }
    function mkSection(parent) {
      var p = mk('Panel', parent);
      p.style.flowChildren = 'down'; p.style.width = '100%';
      p.style.paddingTop = '9px'; p.style.paddingBottom = '9px';
      p.style.paddingLeft = '14px'; p.style.paddingRight = '14px';
      return p;
    }
    function mkCaption(parent, text) {
      var l = lbl(parent, text, S.cap, 11);
      l.style.fontWeight = 'bold'; l.style.letterSpacing = '1px'; l.style.marginBottom = '7px';
      return l;
    }

    // ---- header: accent bar + title, arrows either side -------------------
    var header = mk('Panel', body);
    header.style.flowChildren = 'right';
    header.style.width = '100%';
    header.style.paddingTop = '10px'; header.style.paddingBottom = '10px';
    header.style.paddingLeft = '12px'; header.style.paddingRight = '12px';

    function mkArrow(parent, glyph, command) {
      var a = mk('Panel', parent);
      a.style.width = '30px'; a.style.height = '26px';
      a.style.borderRadius = '4px'; a.style.verticalAlign = 'center';
      var t = lbl(a, glyph, S.accent, 16);
      t.hittest = false; t.style.fontWeight = 'bold';
      t.style.horizontalAlign = 'center'; t.style.verticalAlign = 'center';
      interactive(a, function () { cmd(command); });
      return a;
    }
    mkArrow(header, '◀', 'mirv_filmmaker marker prev');

    var titleWrap = mk('Panel', header);
    titleWrap.hittest = false;
    titleWrap.style.flowChildren = 'right';
    titleWrap.style.width = 'fill-parent-flow(1.0)';
    titleWrap.style.verticalAlign = 'center';
    titleWrap.style.marginLeft = '10px'; titleWrap.style.marginRight = '10px';
    var accentBar = mk('Panel', titleWrap);
    accentBar.style.width = '3px'; accentBar.style.height = '15px';
    accentBar.style.backgroundColor = S.accent; accentBar.style.borderRadius = '2px';
    accentBar.style.marginRight = '9px'; accentBar.style.verticalAlign = 'center';
    var title = lbl(titleWrap, 'CAMERA MARKER', S.accent, 14);
    title.style.fontWeight = 'bold'; title.style.letterSpacing = '1.5px';
    title.style.verticalAlign = 'center';
    title.style.width = 'fill-parent-flow(1.0)';
    title.style.whiteSpace = 'nowrap';

    mkArrow(header, '▶', 'mirv_filmmaker marker next');
    mkSep(body);
)MKJS"
R"MKJS(
    // ---- a "label ....... [ value ]" selector row -------------------------
    // The value is the button: one click cycles it. Fixed 132px so every chip in the column
    // lines up rather than each one sizing to its own text.
    function mkSelector(parent, labelText, command) {
      var r = mk('Panel', parent);
      r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginBottom = '6px'; r.style.minHeight = '28px';
      var k = lbl(r, labelText, S.label, 13);
      k.style.width = 'fill-parent-flow(1.0)'; k.style.verticalAlign = 'center';
      var btn = mk('Panel', r);
      btn.style.borderRadius = '4px'; btn.style.width = '132px';
      btn.style.paddingTop = '5px'; btn.style.paddingBottom = '5px';
      btn.style.verticalAlign = 'center';
      var v = lbl(btn, '', S.value, 13);
      v.hittest = false; v.style.fontWeight = 'bold';
      v.style.textAlign = 'center'; v.style.width = '100%';
      interactive(btn, function () { cmd(command); });
      return { row: r, btn: btn, val: v };
    }

    // Speed Mode and Path Mode are DELIBERATELY separate axes: Speed Mode decides
    // how long each segment takes; Path Mode (Linear/Bezier) decides the curve shape.
    var SPEED_STEPS = [0.2, 0.5, 0.8, 1.0];

    // A "− value +" stepper row that snaps the value to SPEED_STEPS and issues a
    // console command. getCur() reads the live value from the pushed state.
    function mkStepper(parent, labelText, getCur, cmdName) {
      var r = mk('Panel', parent);
      r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginBottom = '6px'; r.style.minHeight = '28px';
      var k = lbl(r, labelText, S.label, 13);
      k.style.width = 'fill-parent-flow(1.0)'; k.style.verticalAlign = 'center';
      function step(dir) {
        var cur = getCur();
        var idx = 0, best = 1e9;
        for (var i = 0; i < SPEED_STEPS.length; i++) {
          var d = Math.abs(SPEED_STEPS[i] - cur);
          if (d < best) { best = d; idx = i; }
        }
        idx += (dir > 0 ? 1 : -1);
        if (idx < 0) idx = 0; if (idx >= SPEED_STEPS.length) idx = SPEED_STEPS.length - 1;
        cmd(cmdName + ' ' + SPEED_STEPS[idx].toFixed(2));
      }
      var group = mk('Panel', r);
      group.hittest = false;
      group.style.flowChildren = 'right'; group.style.width = '132px'; group.style.verticalAlign = 'center';
      function mkB(glyph, dir) {
        var b = mk('Panel', group);
        b.style.width = '30px'; b.style.borderRadius = '4px';
        b.style.paddingTop = '5px'; b.style.paddingBottom = '5px';
        var t = lbl(b, glyph, S.accent, 15);
        t.hittest = false; t.style.fontWeight = 'bold';
        t.style.horizontalAlign = 'center'; t.style.verticalAlign = 'center';
        interactive(b, function () { step(dir); });
        return b;
      }
      mkB('−', -1);
      var vlab = lbl(group, '', S.value, 13);
      vlab.style.width = 'fill-parent-flow(1.0)'; vlab.style.textAlign = 'center';
      vlab.style.verticalAlign = 'center'; vlab.style.fontWeight = 'bold';
      mkB('+', +1);
      return { row: r, val: vlab };
    }

    var sec = mkSection(body);
    mkCaption(sec, 'PLAYBACK');
    var selSpeedMode = mkSelector(sec, 'Speed mode', 'mirv_filmmaker marker speedmode cycle');
    var selInterp = mkSelector(sec, 'Path curve', 'mirv_filmmaker marker interp cycle');
    var selTiming = mkSelector(sec, 'Timing (all cameras)', 'mirv_filmmaker marker timing toggle');
    var selAutoSnap = mkSelector(sec, 'Auto-snap to selected', 'mirv_filmmaker marker autosnap toggle');

    // Global Constant Speed (shown only when Speed Mode == Constant).
    var constStepper = mkStepper(sec, 'Constant speed',
      function () { return (st && st.constSpeed != null) ? st.constSpeed : 1.0; },
      'mirv_filmmaker marker constspeed');

    // Per-segment multiplier for this marker's OUTGOING segment (shown only when
    // Speed Mode == Per-Segment and this is NOT the last marker).
    var mulStepper = mkStepper(sec, 'Segment speed (to next)',
      function () { return (st && st.speedMul != null) ? st.speedMul : 1.0; },
      'mirv_filmmaker marker speedmul');

    // Per-mode explanation / "no outgoing segment" note.
    var expl = lbl(sec, '', S.hint, 11);
    expl.style.marginTop = '3px'; expl.style.whiteSpace = 'normal'; expl.style.width = '100%';

    mkSep(body);

    // ---- actions ---------------------------------------------------------
    // Reposition is the PRIMARY action and gets the solid gold fill; the two destructive
    // ones are secondary and red-tinted only on hover, so the card does not read as three
    // equally-weighted buttons stacked in a column.
    var asec = mkSection(body);
    mkCaption(asec, 'ACTIONS');
    function mkAction(parent, text, opts) {
      opts = opts || {};
      var b = mk('Panel', parent);
      b.style.width = '100%'; b.style.borderRadius = '4px';
      b.style.paddingTop = '9px'; b.style.paddingBottom = '9px';
      b.style.marginBottom = '5px';
      var l = lbl(b, text, opts.fg || S.value, 13);
      l.hittest = false; l.style.fontWeight = 'bold';
      l.style.horizontalAlign = 'center'; l.style.verticalAlign = 'center';
      interactive(b, opts.onClick, { accent: opts.accent });
      if (opts.primary) {
        // A primary fill has to survive paintCtl's rest state, so it owns the SELECTED slot
        // and is pinned on.
        ctlSel(b, true, S.accent, S.accent, S.accent);
        l.style.color = S.accentText;
      }
      b.__lbl = l;
      return b;
    }
    var repositionBtn = mkAction(asec, 'Reposition Camera', {
      primary: true,
      onClick: function () { cmd('mirv_filmmaker marker reposition'); }
    });
    var delBtn = mkAction(asec, 'Remove Marker', {
      accent: S.dangerEdge, fg: S.value,
      onClick: function () { if (st && st.selected >= 0) cmd('mirv_filmmaker marker delete ' + st.selected); }
    });
    var delAllConfirm = false;
    var delAllBtn = mkAction(asec, 'Remove All Markers', {
      accent: S.dangerEdge, fg: S.value,
      onClick: function () {
        if (!delAllConfirm) {
          delAllConfirm = true;
          delAllBtn.__lbl.text = 'Remove All — click again to confirm';
          delAllBtn.__lbl.style.color = S.danger;
          return;
        }
        resetDelAll();
        cmd('mirv_filmmaker marker deleteall confirm');
      }
    });
    delAllBtn.style.marginBottom = '0px';
    function resetDelAll() {
      delAllConfirm = false;
      delAllBtn.__lbl.text = 'Remove All Markers';
      delAllBtn.__lbl.style.color = S.value;
    }

    mkSep(body);
)MKJS"
R"MKJS(
    // ---- control hints, as F8-style [KEY] badge rows ----------------------
    var hsec = mkSection(body);
    mkCaption(hsec, 'CONTROLS');
    function mkControl(parent, keyText, desc) {
      var r = mk('Panel', parent);
      r.hittest = false;
      r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginBottom = '4px'; r.style.minHeight = '18px';
      var badge = mk('Panel', r);
      badge.hittest = false;
      badge.style.backgroundColor = S.btnBg; badge.style.borderRadius = '4px';
      badge.style.paddingTop = '2px'; badge.style.paddingBottom = '2px';
      badge.style.paddingLeft = '7px'; badge.style.paddingRight = '7px';
      badge.style.minWidth = '54px'; badge.style.marginRight = '9px';
      badge.style.verticalAlign = 'center';
      var bl = lbl(badge, keyText, '#d6dde4ff', 12);
      bl.style.fontWeight = 'bold'; bl.style.horizontalAlign = 'center';
      var d = lbl(r, desc, S.label, 12);
      d.style.width = 'fill-parent-flow(1.0)'; d.style.verticalAlign = 'center';
      return r;
    }
    mkControl(hsec, 'K', 'Place a camera marker');
    mkControl(hsec, 'J', 'Arm the path  →  Space plays it');
    mkControl(hsec, 'L', 'Delete the marker you are aiming at');
    mkControl(hsec, 'F', 'Open these settings (aim at a marker)');

    mkSep(body);

    // ---- footer ----------------------------------------------------------
    var foot = mk('Panel', body);
    foot.style.flowChildren = 'right'; foot.style.width = '100%';
    foot.style.paddingTop = '8px'; foot.style.paddingBottom = '10px';
    foot.style.paddingLeft = '14px'; foot.style.paddingRight = '14px';
    // Filler, not horizontalAlign:'right' -- inside a flow-right parent the alignment properties
    // do not reposition a flowed child, so the button would sit hard against the left padding.
    var footFill = mk('Panel', foot);
    footFill.hittest = false;
    footFill.style.width = 'fill-parent-flow(1.0)'; footFill.style.height = '1px';
    var doneBtn = mk('Panel', foot);
    doneBtn.style.borderRadius = '4px';
    doneBtn.style.paddingTop = '5px'; doneBtn.style.paddingBottom = '5px';
    doneBtn.style.paddingLeft = '14px'; doneBtn.style.paddingRight = '14px';
    var doneL = lbl(doneBtn, 'F / ESC   Done', S.accent, 13);
    doneL.hittest = false; doneL.style.fontWeight = 'bold'; doneL.style.letterSpacing = '1px';
    interactive(doneBtn, function () { cmd('mirv_filmmaker marker close'); });

    // ============================ PROMPT BANNER ============================
    // Click-through (hittest=false) so the user can free-look + left-click the world during
    // reposition, and so it never blocks the preview. Same card language as above at a
    // smaller scale: accent bar + gold title, dotted dark base, soft bottom fade.
    var BANNER_W = 360;
    var banner = $.CreatePanel('Panel', root, '', {});
    banner.hittest = false;
    var bs = banner.style;
    bs.flowChildren = 'down';
    // Anchored top-left and moved by transform: the banner has to land inside the PREVIEW
    // rect, which is not centred on the window while a workspace is open, and a transform is
    // also how it picks up the preview's scale. placeBanner() writes both every render.
    bs.horizontalAlign = 'left'; bs.verticalAlign = 'top';
    bs.marginTop = '0px'; bs.marginLeft = '0px';
    bs.transformOrigin = '0% 0%';
    // EXPLICIT width, not min-width + fit-children: the inner body is width:100%, and a
    // fit-children parent holding a 100%-wide child is a circular constraint. A fixed width
    // also makes the centring maths in placeBanner exact on the very first layout pass.
    bs.width = BANNER_W + 'px';
    bs.fontFamily = S.font;

    var bBody = mk('Panel', banner);
    bBody.hittest = false;
    bBody.style.width = '100%'; bBody.style.flowChildren = 'down';
    bBody.style.backgroundColor = S.bg;
    bBody.style.backgroundImage = S.dots;
    bBody.style.backgroundSize = '620px 620px';
    bBody.style.backgroundRepeat = 'repeat';
    bBody.style.backgroundImgOpacity = '0.03';
    bBody.style.borderTopLeftRadius = '5px'; bBody.style.borderTopRightRadius = '5px';
    bBody.style.paddingTop = '9px'; bBody.style.paddingBottom = '9px';
    bBody.style.paddingLeft = '16px'; bBody.style.paddingRight = '16px';

    var bHead = mk('Panel', bBody);
    bHead.hittest = false; bHead.style.flowChildren = 'right'; bHead.style.width = '100%';
    var bBar = mk('Panel', bHead);
    bBar.style.width = '3px'; bBar.style.height = '13px'; bBar.style.borderRadius = '2px';
    bBar.style.backgroundColor = S.accent; bBar.style.marginRight = '8px';
    bBar.style.verticalAlign = 'center';
    var bTitle = lbl(bHead, '', S.accent, 13);
    bTitle.style.fontWeight = 'bold'; bTitle.style.letterSpacing = '1.5px';
    bTitle.style.verticalAlign = 'center'; bTitle.style.whiteSpace = 'nowrap';
    var bSub = lbl(bBody, '', S.value, 12);
    bSub.style.marginTop = '6px'; bSub.style.whiteSpace = 'nowrap';
    var bKeys = lbl(bBody, '', S.label, 11);
    bKeys.style.marginTop = '4px'; bKeys.style.whiteSpace = 'nowrap';

    var bFade = mk('Panel', banner);
    bFade.hittest = false;
    bFade.style.width = '100%'; bFade.style.height = '18px';
    bFade.style.backgroundColor = S.bg;
    bFade.style.backgroundImage = S.dots;
    bFade.style.backgroundSize = '620px 620px';
    bFade.style.backgroundRepeat = 'repeat';
    bFade.style.backgroundImgOpacity = '0.03';
    bFade.style.opacityMask = S.fadeMask;

    // The preview rect (normalised window fractions) is published by whichever workspace owns
    // the screen. Read it from the DOM rather than adding a second C++ state field: both
    // roots are siblings of ours in the same HUD context, and the attribute is already
    // maintained every frame for the D3D blit.
    function previewRect() {
      var ids = ['CamEditorRoot', 'ConfigHudRoot'];
      for (var i = 0; i < ids.length; i++) {
        var p = ctx.FindChildTraverse && ctx.FindChildTraverse(ids[i]);
        if (!p || !p.visible || !p.GetAttributeString) continue;
        var raw = p.GetAttributeString('previewrect', '');
        if (!raw) continue;
        var f = raw.split(' ');
        if (f.length < 4) continue;
        var x0 = parseFloat(f[0]), y0 = parseFloat(f[1]), x1 = parseFloat(f[2]), y1 = parseFloat(f[3]);
        if (!(isFinite(x0) && isFinite(y0) && isFinite(x1) && isFinite(y1))) continue;
        if (!(x1 > x0 && y1 > y0)) continue;
        return [x0, y0, x1, y1];
      }
      return null;
    }
    // Map the banner into the preview rect. The scale factor is the preview's own, so the
    // banner reads at the same size relative to the shot as it does full-screen.
    // Panorama applies the listed translate BEFORE the scale on these HUD panels, so the
    // translate is itself scaled -- feed it pre-divided values to land on real screen px
    // (the same correction applyGameHudViewport uses in CameraTimelineJs.h).
    function placeBanner() {
      var sx = root.actualuiscale_x || ctx.actualuiscale_x || 1;
      var sy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
      var rw = (root.actuallayoutwidth || ctx.actuallayoutwidth || 0) / sx;
      var rh = (root.actuallayoutheight || ctx.actuallayoutheight || 0) / sy;
      if (!(rw > 1 && rh > 1)) return;
      var r = previewRect() || [0, 0, 1, 1];
      var s = r[3] - r[1];
      if (!(s > 0.05)) s = 1;
      var bw = BANNER_W;
      var cx = (r[0] + r[2]) * 0.5 * rw;
      var tx = cx - (bw * s) * 0.5;
      var ty = r[1] * rh + (r[3] - r[1]) * rh * 0.11;
      banner.style.transform = 'translate3d(' + (tx / s).toFixed(1) + 'px, ' + (ty / s).toFixed(1)
        + 'px, 0px) scale3d(' + s.toFixed(5) + ', ' + s.toFixed(5) + ', 1)';
    }
    // Re-place a couple of frames later too. C++ only calls render() when the state JSON
    // CHANGES (MarkerHud::RunFrame), so the banner would otherwise be placed exactly once --
    // on a frame where our fresh root can still report a zero layout size, and before the
    // workspace has published its preview rect.
    var bannerGen = 0;
    function settleBanner() {
      var myGen = ++bannerGen;
      function again() {
        if (myGen !== bannerGen) return;
        if (!banner || (banner.IsValid && !banner.IsValid())) return;
        if (!banner.visible) return;
        placeBanner();
      }
      try { $.Schedule(0.03, again); $.Schedule(0.20, again); } catch (e) {}
    }

    function showBanner(titleText, titleColor, subText, keysText) {
      bTitle.text = titleText; bTitle.style.color = titleColor;
      bBar.style.backgroundColor = titleColor;
      bSub.text = subText; bSub.visible = !!subText;
      bKeys.text = keysText; bKeys.visible = !!keysText;
      banner.visible = true; card.visible = false; catcher.visible = false;
      placeBanner();
      settleBanner();
    }
)MKJS"
R"MKJS(
    var st = null;
    var hostAttr = root;

    // Hide sibling HUD panels while preserving this preview root. During armed preview,
    // Tab toggles this so the Cam Path Preview control remains available. During playback,
    // siblings stay hidden and this root hides too, producing a completely clean frame.
    var hiddenPanels = null;
    function setOtherUiHidden(h) {
      if (h) {
        if (!hiddenPanels) hiddenPanels = [];
        var count = ctx.GetChildCount ? ctx.GetChildCount() : 0;
        for (var i = 0; i < count; i++) {
          var p = ctx.GetChild(i);
          if (!p || p === root || p.id === 'MarkerHudRoot') continue;
          if (p.visible) {
            hiddenPanels.push(p);
            try { p.visible = false; } catch (e) {}
          }
        }
      } else if (hiddenPanels) {
        for (var j = 0; j < hiddenPanels.length; j++) {
          try { hiddenPanels[j].visible = true; } catch (e2) {}
        }
        hiddenPanels = null;
      }
    }

    var api = {};
    api.render = function () {
      var raw = hostAttr.GetAttributeString('state', '');
      if (!raw) { root.visible = false; setOtherUiHidden(false); return; }
      var s; try { s = JSON.parse(raw); } catch (e) { return; }
      st = s;

      // Capture chrome gate: while a render job is capturing with a before-present pass, this
      // card and its prompt banner must vanish or they get baked into the recorded frame -- the
      // banner in particular sits inside the preview rect, i.e. directly over the shot.
      if (s.captureHide) { root.visible = false; return; }

      var mode = s.mode || 'editing';
      var freeze = (s.timing === 'Freeze');
      setOtherUiHidden(((mode === 'previewPlaying' && !s.editorPlay) || mode === 'previewArmed')
        && !!s.hudHidden);

      // Transient notice (e.g. "Need at least 2 camera markers to play path.").
      // Shown over everything EXCEPT the open settings card (which has its own
      // inline explanation), so pressing J with too few markers gives clear feedback.
      if (s.notice && !(mode === 'editing' && s.menuOpen)) {
        showBanner('CAMERA PATH', S.accent, s.notice, '');
        root.visible = true; return;
      }

      if (mode === 'reposition') {
        var n = (s.selected != null && s.selected >= 0) ? (s.selected + 1) : 0;
        showBanner('REPOSITIONING CAMERA #' + n, S.accent,
          'Move to the new position, then left-click to place.',
          'LEFT CLICK  place        ESC  cancel');
        root.visible = true; return;
      }
      if (mode === 'previewArmed') {
        root.visible = false; return;
      }
      if (mode === 'previewPlaying') {
        root.visible = false; return;
      }

      // editing
      if (!s.menuOpen) { root.visible = false; return; }
      banner.visible = false; card.visible = true; catcher.visible = true; root.visible = true;

      var nn = (s.selected != null && s.selected >= 0) ? (s.selected + 1) : 0;
      title.text = 'CAMERA MARKER #' + nn + '  ·  ' + s.count + ' TOTAL';
      selSpeedMode.val.text = s.speedMode || '-';
      selInterp.val.text = s.interp || '-';
      selTiming.val.text = freeze ? 'FREEZE' : 'LIVE';
      selTiming.val.style.color = freeze ? S.freeze : S.accent;
      ctlSel(selTiming.btn, true,
        freeze ? S.freeze : S.accent,
        freeze ? S.freezeSel : S.accentSel,
        freeze ? S.freezeSelHot : S.accentSelHot);
      selAutoSnap.val.text = s.autoSnap ? 'ON' : 'OFF';
      selAutoSnap.val.style.color = s.autoSnap ? S.accent : S.label;
      ctlSel(selAutoSnap.btn, !!s.autoSnap, S.accent, S.accentSel, S.accentSelHot);
      constStepper.val.text = ((s.constSpeed != null) ? s.constSpeed : 1).toFixed(1) + 'x';
      mulStepper.val.text = ((s.speedMul != null) ? s.speedMul : 1).toFixed(1) + 'x';

      // Path interpolation and speed are independent; the speed controls shown
      // depend ONLY on the speed mode (not the path mode).
      var sm = s.speedMode || 'Manual';
      constStepper.row.visible = (sm === 'Constant');
      var perSeg = (sm === 'Per-Segment');
      mulStepper.row.visible = (perSeg && !s.isLast);

      if (!freeze) {
        expl.text = 'LIVE: the camera follows the replay — markers are hit at their tick/time. Use the speed scrubber for pace. The speed modes shape FREEZE playback.';
      } else if (sm === 'Manual') {
        expl.text = 'FREEZE · Manual: each segment’s duration is the tick/time gap to the next marker.';
      } else if (sm === 'Constant') {
        expl.text = 'FREEZE · Constant: the whole path glides at one shared speed.';
      } else if (s.isLast) {
        expl.text = 'No outgoing segment — the last marker ends the path.';
      } else {
        expl.text = 'FREEZE · Per-Segment: this marker controls the move to the next marker.';
      }

      resetDelAll();
    };
    $.MarkerHud = api;
    api.render();
    $.Msg('[markerhud] panel built.\n');
  } catch (err) {
    $.Msg('[markerhud] gui error: ' + err + '\n');
  }
})();
)MKJS";

} // namespace Filmmaker
