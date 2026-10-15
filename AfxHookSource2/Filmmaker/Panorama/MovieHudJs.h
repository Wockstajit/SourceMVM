#pragma once

// Panorama JS for the in-game movie-director HUD. Built ONCE into the HUD
// (CSGOHud) context via PanoramaBridge::RunScript. It is DISPLAY-ONLY:
//   C++ -> JS : sets host attribute "state" (JSON) then runs $.MovieHud.render().
//   JS  -> C++: none (show/hide is a console command; see MovieHud.cpp).
//
// LAYOUT: a single full-screen, transparent, hittest=false container (#MovieHudRoot
// -- the id C++ sets "state" on and tears down) holds two anchored cards:
//   * #MovieHudCardRight -- top-right, its top edge level with the native top
//     scoreboard (team counter): MOVIE DIRECTOR header + live status + CAMERA readout.
//   * #MovieHudCardLeft  -- top-left, sitting just under the radar/minimap: CONTROLS.
// Both cards are hittest=false so they never block game/demo input.
//
// FADE: each card is a SOLID content area (bg color + bluedots) that holds all the
// text, followed by a fixed-height gradient FADE STRIP that begins at the same bg
// colour and dissolves to transparent. This gives a soft bottom edge (no hard
// rectangle) while keeping every text row on a fully opaque, readable background.
// (Masking the whole card faded the bottom rows of text into the scene.)
//
// DOM-building and styling are kept in separate sections below so the structure
// stays readable even though our injection model builds panels in JS rather than
// shipping XML/CSS into the VPK.

namespace Filmmaker {

inline const char* kMovieHudJs = R"MHJS(
(function () {
  try {
    var existing = $('#MovieHudRoot'); if (existing) existing.DeleteAsync(0);
    var existingToast = $('#MvmCamToast'); if (existingToast) existingToast.DeleteAsync(0);

    // Build directly under the context panel (the in-game HUD) so C++'s
    // FindChildById(ctx, "MovieHudRoot") locates it and we don't rebuild every
    // frame. Do NOT attach to the absolute top root: that can overlay (and
    // swallow input from) the main menu.
    var ctx = $.GetContextPanel();

    // ---- design tokens (matches the native CS2 scoreboard look) ----
    var S = {
      panelW: '278px',            // right card (director + camera)
      ctrlW:  '252px',            // left card (controls)
      bg: 'rgba(0,0,0,0.88)',     // scoreboard base
      dots: 'url("s2r://panorama/images/backgrounds/bluedots_large_png.vtex")',
      accent: '#f0b323ff',        // CS gold
      label: '#c8d3dfff',
      value: '#eef2f6ff',
      on: '#7ac368ff',
      // `off` is the other half of a binary state pair, so it has to be as readable as `on`
      // (Lc 60), not 31 Lc weaker -- at #6b7480 an OFF badge read as an absent one. `on` is
      // unchanged: it already measures Lc 60 and it doubles as a badge FILL at :317.
      off: '#a8b2bfff',
      cap: '#a7b3beff',           // section captions -- 11px on a 88%-black card
      badgeBg: '#ffffff14',
      badgeText: '#d6dde4ff',
      sep: '#80808030',
      font: 'Stratum2, "Arial Unicode MS"',
      fadeH: '52px', // height of the soft fade strip at the bottom of each card
      // Linear top->bottom alpha ramp (255 at top -> 0 at bottom). Applied to the
      // fade strip so BOTH the dark colour AND the dotted texture dissolve together.
      fadeMask: 'url("s2r://panorama/images/masks/top-bottom-fade-4_png.vtex")',
      scale: 0.8,    // cards rendered smaller (extra ~10% shrink per request)
      // Horizontal placement: the left (CONTROLS) card aligns to the live minimap; the
      // right (DIRECTOR) card mirrors that SAME inset from the right edge so both sides
      // have even spacing. Bump radarNudgeX if the minimap's drawn box sits right of the
      // radar panel's origin and you want the controls card slid under it.
      minSideInset: 16, // never let the controls card touch the window edge
      radarNudgeX: 0    // extra px to push the controls card right toward the minimap
    };

    // Full-screen, transparent, click-through container. This is the panel C++
    // sets the "state" attribute on (#MovieHudRoot) and deletes on teardown; the
    // two visible cards are anchored children of it.
    var root = $.CreatePanel('Panel', ctx, 'MovieHudRoot', {});
    root.hittest = false;
    root.style.width = '100%';
    root.style.height = '100%';
    root.style.zIndex = '50'; // z-layer map: PanoramaBridge.h

    // Card factory: a scoreboard-look panel with a soft bottom edge. Position
    // (align/margins) is set by the caller. `origin` is the transform-origin the
    // 10% shrink scales toward, so the card stays pinned to its anchored corner
    // (e.g. '100% 0%' = top-right, '0% 0%' = top-left).
    //
    // The card flows DOWN as two stacked pieces:
    //   * a SOLID content area (bg color + bluedots) that holds ALL the text, so
    //     text always sits on an opaque panel and stays fully readable.
    //   * a fixed-height FADE STRIP below it: a gradient that begins at the exact
    //     bg colour (seamless with the content) and dissolves to transparent, so
    //     the card has a soft bottom edge -- no hard rectangle -- WITHOUT ever
    //     fading any text. (Previously a single opacity-mask faded the whole card,
    //     which washed out the bottom rows of text.)
    // Callers append their rows to `card.contentPanel` (returned on the card).
    function mkCard(id, w, origin) {
      var c = $.CreatePanel('Panel', root, id, {});
      c.hittest = false;
      var st = c.style;
      st.width = w;
      st.flowChildren = 'down';        // content area, then fade strip
      st.transformOrigin = origin;     // pin the shrink to the anchored corner
      st.transform = 'scale3d(' + S.scale + ', ' + S.scale + ', 1)';

      // --- solid content area (holds all text) ---
      // Dotted bluedots texture over the base color, like the native scoreboard
      // (.sb-main). Rounded top corners; bottom is squared and met seamlessly by
      // the fade strip.
      var content = $.CreatePanel('Panel', c, '', {});
      content.hittest = false;
      var cs = content.style;
      cs.width = '100%';
      cs.flowChildren = 'down';
      cs.backgroundColor = S.bg;
      cs.backgroundImage = S.dots;
      cs.backgroundSize = '620px 620px'; // larger tile -> sparser, less noisy dots
      cs.backgroundRepeat = 'repeat';
      cs.backgroundImgOpacity = '0.03';  // fainter so the texture stays subtle
      cs.borderTopLeftRadius = '5px';
      cs.borderTopRightRadius = '5px';
      cs.color = S.label;
      cs.fontFamily = S.font;
      cs.fontSize = '14px';

      // --- fade strip (soft bottom edge, no text) ---
      // Continues the SAME solid colour + dotted texture as the content above (so
      // there is no seam where the dots stop), then an opacity-mask dissolves the
      // whole strip -- colour and dots together -- smoothly to transparent.
      var fade = $.CreatePanel('Panel', c, '', {});
      fade.hittest = false;
      var fs = fade.style;
      fs.width = '100%';
      fs.height = S.fadeH;
      fs.backgroundColor = S.bg;
      fs.backgroundImage = S.dots;
      fs.backgroundSize = '620px 620px';
      fs.backgroundRepeat = 'repeat';
      fs.backgroundImgOpacity = '0.03';
      fs.opacityMask = S.fadeMask;     // dissolves colour + dots to nothing

      c.contentPanel = content;        // append rows here, not to the card itself
      return c;
    }

    // --- small builders -------------------------------------------------
    function mkLabel(parent, color, size) {
      var l = $.CreatePanel('Label', parent, '', {});
      if (color) l.style.color = color;
      if (size) l.style.fontSize = size;
      return l;
    }
    // A padded vertical section.
    function mkSection(parent) {
      var p = $.CreatePanel('Panel', parent, '', {});
      p.style.flowChildren = 'down';
      p.style.width = '100%';
      p.style.paddingTop = '8px'; p.style.paddingBottom = '8px';
      p.style.paddingLeft = '14px'; p.style.paddingRight = '14px';
      return p;
    }
    function mkSep(parent) {
      var p = $.CreatePanel('Panel', parent, '', {});
      p.style.width = '100%'; p.style.height = '1px';
      p.style.backgroundColor = S.sep;
      return p;
    }
    function mkCaption(parent, text) {
      var l = mkLabel(parent, S.cap, '11px');
      l.text = text;
      l.style.fontWeight = 'bold';
      l.style.letterSpacing = '1px';
      l.style.marginBottom = '7px';
      return l;
    }
    // "label .......... value" row; returns the value Label for live updates.
    function mkKV(parent, keyText) {
      var row = $.CreatePanel('Panel', parent, '', {});
      row.style.flowChildren = 'right';
      row.style.width = '100%';
      row.style.marginBottom = '4px';
      var k = mkLabel(row, S.label); k.text = keyText;
      k.style.width = 'fill-parent-flow(1.0)';
      var v = mkLabel(row, S.value);
      v.style.horizontalAlign = 'right';
      v.style.textAlign = 'right';
      v.style.fontWeight = 'bold';
      return v;
    }
    // A control row: [ KEY badge ]  description.
    function mkControl(parent, keyText, desc) {
      var row = $.CreatePanel('Panel', parent, '', {});
      row.style.flowChildren = 'right';
      row.style.width = '100%';
      row.style.marginBottom = '4px';
      row.style.minHeight = '18px';
      var badge = $.CreatePanel('Panel', row, '', {});
      badge.style.backgroundColor = S.badgeBg;
      badge.style.borderRadius = '4px';
      badge.style.paddingTop = '2px'; badge.style.paddingBottom = '2px';
      badge.style.paddingLeft = '7px'; badge.style.paddingRight = '7px';
      badge.style.minWidth = '46px';
      badge.style.marginRight = '9px';
      badge.style.verticalAlign = 'center';
      var bl = mkLabel(badge, S.badgeText, '12px');
      bl.text = keyText;
      bl.style.fontWeight = 'bold';
      bl.style.horizontalAlign = 'center';
      var d = mkLabel(row, S.label, '13px');
      d.text = desc;
      d.style.width = 'fill-parent-flow(1.0)';
      d.style.verticalAlign = 'center';
      return row;
    }
)MHJS"
// Split here: MSVC caps a single string literal at ~16 KB (C2026). Adjacent
// string literals are concatenated by the compiler, so this stays one JS blob.
R"MHJS(
    // A header bar ("| MOVIE DIRECTOR" or "| CONTROLS" etc.) for a card top.
    function mkHeader(parent, text) {
      var header = $.CreatePanel('Panel', parent, '', {});
      header.style.flowChildren = 'right';
      header.style.width = '100%';
      header.style.paddingTop = '11px'; header.style.paddingBottom = '11px';
      header.style.paddingLeft = '14px'; header.style.paddingRight = '14px';
      var accentBar = $.CreatePanel('Panel', header, '', {});
      accentBar.style.width = '3px'; accentBar.style.height = '15px';
      accentBar.style.backgroundColor = S.accent;
      accentBar.style.borderRadius = '2px';
      accentBar.style.marginRight = '9px';
      accentBar.style.verticalAlign = 'center';
      var title = mkLabel(header, S.accent, '14px');
      title.text = text;
      title.style.fontWeight = 'bold';
      title.style.letterSpacing = '1.5px';
      title.style.verticalAlign = 'center';
      return header;
    }

    // ======================================================================
    // RIGHT CARD -- top-right, top edge level with the native top scoreboard.
    // Holds the MOVIE DIRECTOR header, live status and the CAMERA readout.
    // ======================================================================
    var cardRight = mkCard('MovieHudCardRight', S.panelW, '100% 0%');
    cardRight.style.horizontalAlign = 'right';
    cardRight.style.verticalAlign = 'top';
    cardRight.style.marginTop = '4px';   // ~level with team counter (margin-top:2px)
    cardRight.style.marginRight = S.minSideInset + 'px'; // mirrored to the left inset at runtime
    var bodyRight = cardRight.contentPanel; // crisp (unmasked) content layer

    mkHeader(bodyRight, 'MOVIE DIRECTOR');
    mkSep(bodyRight);

    // ---- live status ---------------------------------------------------
    var secStatus = mkSection(bodyRight);
    var vMode   = mkKV(secStatus, 'Mode');
    var vState  = mkKV(secStatus, 'Playback');
    var vSpeed  = mkKV(secStatus, 'Cam speed');

    mkSep(bodyRight);

    // ---- camera readout (directly under the director status) ----------
    var secCam = mkSection(bodyRight);
    mkCaption(secCam, 'CAMERA');
    var vPos  = mkKV(secCam, 'Pos');
    var vAng  = mkKV(secCam, 'Ang');
    var vFov  = mkKV(secCam, 'FOV');
    var vTT   = mkKV(secCam, 'Tick / Time'); // combined tick + time, one compact line
    // Pos/Ang are the numbers the user reads while composing a shot -- keep them the
    // LARGEST values on the card. nowrap so the longer coordinate strings stay on one line.
    [vPos, vAng].forEach(function (v) { v.style.fontSize = '17px'; v.style.whiteSpace = 'nowrap'; });

    var sepMesh = mkSep(bodyRight);

    // ---- mesh-group readout ---------------------------------------------
    // Debug visualizer for the legacy-vs-CS2 weapon mesh bug: three badges (0=CS2
    // "body_hd", 1=legacy CS:GO "body_legacy", 2="body_hidden") per row, one row for
    // the WORLD model and one for the first-person VIEWMODEL. Whichever group is
    // ACTUALLY selected on that live entity lights up green. Reads straight off
    // CModelState::m_MeshGroupMask every frame (Cosmetics_GetSpectatedWeaponMeshHud in
    // CosmeticDebug.cpp), so this can never show something different from what a
    // screenshot taken the same instant would -- it exists because the stored mask
    // has repeatedly reported the right value while the RENDERED geometry silently
    // stayed on the old group.
    var secMesh = mkSection(bodyRight);
    mkCaption(secMesh, 'MESH');
    var vMeshWeapon = mkKV(secMesh, 'Weapon');
    function mkMeshRow(parent, labelText) {
      var row = $.CreatePanel('Panel', parent, '', {});
      row.style.flowChildren = 'right';
      row.style.width = '100%';
      row.style.marginBottom = '4px';
      var k = mkLabel(row, S.label); k.text = labelText;
      k.style.width = 'fill-parent-flow(1.0)';
      k.style.verticalAlign = 'center';
      var badges = [];
      ['0', '1', '2'].forEach(function (n) {
        var b = $.CreatePanel('Panel', row, '', {});
        b.style.backgroundColor = S.badgeBg;
        b.style.borderRadius = '4px';
        b.style.paddingTop = '2px'; b.style.paddingBottom = '2px';
        b.style.paddingLeft = '7px'; b.style.paddingRight = '7px';
        b.style.minWidth = '18px';
        b.style.marginLeft = '5px';
        var bl = mkLabel(b, S.badgeText, '12px');
        bl.text = n;
        bl.style.fontWeight = 'bold';
        bl.style.horizontalAlign = 'center';
        badges.push({ panel: b, label: bl });
      });
      return badges;
    }
    var meshWorldBadges = mkMeshRow(secMesh, 'World');
    var meshVmBadges = mkMeshRow(secMesh, 'FP');
    var vMeshVmWarn = mkLabel(secMesh, S.off, '15px');
    vMeshVmWarn.style.marginTop = '2px';
    vMeshVmWarn.style.fontWeight = 'bold';
    // Sets badge[bitIndex] green when `mask` has that bit set (mask 1/2/4 -> bit 0/1/2);
    // everything else reverts to the neutral badge color. `mask===null` (no live entity /
    // vm not found) greys out the whole row instead of falsely lighting badge 0.
    function paintMeshBadges(badges, mask) {
      for (var i = 0; i < 3; i++) {
        var on = (mask !== null) && ((mask & (1 << i)) !== 0);
        badges[i].panel.style.backgroundColor = on ? S.on : S.badgeBg;
        badges[i].label.style.color = on ? '#0a1a08ff' : S.badgeText;
      }
    }

    // ======================================================================
    // LEFT CARD -- top-left, tucked just under the radar/minimap. Holds the
    // static CONTROLS reference. Its top is positioned at RUNTIME from the actual
    // radar panel (see positionControlsCard) so it always clears the minimap
    // regardless of resolution, aspect ratio, or the player's HUD/radar scale.
    // ======================================================================
    var cardLeft = mkCard('MovieHudCardLeft', S.ctrlW, '0% 0%');
    cardLeft.style.horizontalAlign = 'left';
    cardLeft.style.verticalAlign = 'top';
    cardLeft.style.marginTop = '332px'; // fallback; overwritten by positionControlsCard()
    cardLeft.style.marginLeft = '20px';
    // Start hidden so we never flash the card at the fallback spot and then snap it
    // to the radar-relative position once the minimap has laid out (the visible
    // "jump" on demo start). positionControlsCard() reveals it once it's placed.
    cardLeft.visible = false;
    var bodyLeft = cardLeft.contentPanel; // crisp (unmasked) content layer

    // Find the native minimap panel (id "HudRadar", a CSGOHudRadar). We search
    // from the topmost ancestor, not just from our context panel: FindChildTraverse
    // only walks DESCENDANTS, and if our context panel isn't an ancestor of the
    // radar the lookup silently fails and we'd fall back to a fixed margin (which
    // overlaps once the player's radar scale differs from default).
    function findRadar() {
      var top = ctx;
      while (top.GetParent && top.GetParent()) top = top.GetParent();
      var r = top.FindChildTraverse && top.FindChildTraverse('HudRadar');
      if (!r && ctx.FindChildTraverse) r = ctx.FindChildTraverse('HudRadar');
      return r;
    }

    // Place the controls card just below the live radar/minimap. We read the real
    // radar panel's screen-space box and convert it to the HUD's virtual px
    // (size_virtual = actuallayout / actualuiscale), since margins/style.y are in
    // virtual px. A fixed margin can't work: the radar's size depends on resolution
    // AND the player's radar scale. Recomputed each render (cheap; only writes the
    // style when the value actually changes).
    var kGap = 105;           // gap between the minimap bottom and the controls card
    var lastTop = -1, lastLeft = -1, lastRight = -1, posTries = 0;
    function positionControlsCard() {
      var rsy = root.actualuiscale_y || 1, rsx = root.actualuiscale_x || 1;
      var rootTop = (root.actualyoffset || 0) / rsy;
      var rootLeft = (root.actualxoffset || 0) / rsx;
      var radar = findRadar();
      var h = (radar && radar.actuallayoutheight) || 0;
      var top, left;
      if (radar && h > 0) {
        var sy = radar.actualuiscale_y || 1, sx = radar.actualuiscale_x || 1;
        var radarTop = (radar.actualyoffset || 0) / sy;
        var radarLeft = (radar.actualxoffset || 0) / sx;
        top = (radarTop - rootTop) + (h / sy) + kGap;
        left = (radarLeft - rootLeft);
      } else {
        // Radar not laid out yet: stay hidden for a few frames so we don't reveal
        // the card at the fallback spot and then snap it once the radar appears.
        if (++posTries < 30) { cardLeft.visible = false; return; }
        // Waited long enough; fall back to a FRACTION of the screen height so we
        // still clear a default-scale minimap on any resolution (not a fixed px).
        var rootH = (root.actuallayoutheight || 0) / rsy;
        top = (rootH > 0) ? rootH * 0.34 : 360;
        left = 20;
      }
      // Align the controls card to the minimap, but never flush to the window edge.
      left += S.radarNudgeX;
      if (left < S.minSideInset) left = S.minSideInset;
      // Snap to whole px and only touch the DOM when it actually moves.
      top = Math.round(top); left = Math.round(left);
      if (top !== lastTop) { cardLeft.style.marginTop = top + 'px'; lastTop = top; }
      if (left !== lastLeft) { cardLeft.style.marginLeft = left + 'px'; lastLeft = left; }
      // Mirror the SAME inset to the director card so left/right spacing is even.
      if (left !== lastRight) { cardRight.style.marginRight = left + 'px'; lastRight = left; }
      cardLeft.visible = true; // placed -> safe to show
    }

    mkHeader(bodyLeft, 'CONTROLS');
    mkSep(bodyLeft);
    var secControls = mkSection(bodyLeft);
    mkControl(secControls, 'F8',      'Show / hide this panel');
    mkControl(secControls, 'Scroll',  'First person / Chase / Free cam');
    mkControl(secControls, 'Hold click', 'Look around (Chase, Free cam)');
    mkControl(secControls, '← / →', 'Skip 5s');
    mkControl(secControls, 'X',       'X-ray on / off');
    // --- camera-marker / dolly path (BO2-style) ---
    mkControl(secControls, 'K',       'Place camera marker');
    mkControl(secControls, 'J',       'Play / stop camera path');
    mkControl(secControls, 'L',       'Delete marker (aim at it)');
    mkControl(secControls, 'F',       'Edit marker (aim at it)');

    function f1(n) { return (Math.round(n * 10) / 10).toFixed(1); }
    function setOnOff(lbl, b) { lbl.text = b ? 'ON' : 'OFF'; lbl.style.color = b ? S.on : S.off; }

    var hostAttr = root; // state JSON is set on the #MovieHudRoot container by C++

    var api = {};
)MHJS"
// Split for MSVC's ~16 KB string literal cap (C2026), as above.
R"MHJS(
    // ---- speed / FOV toast ----------------------------------------------------------------
    // A quiet pill under the top scoreboard: which value the wheel just changed and where it is
    // now. Its own panel beside #MovieHudRoot, so hiding the director card (F8) never hides the
    // answer to a key you just pressed. Fades in fast, holds while you keep scrolling, then fades
    // out slowly once you stop.
    var toast = $.CreatePanel('Panel', ctx, 'MvmCamToast', {});
    toast.hittest = false;
    var ts = toast.style;
    ts.horizontalAlign = 'left'; ts.verticalAlign = 'top';
    ts.flowChildren = 'right';
    ts.paddingTop = '8px'; ts.paddingBottom = '8px'; ts.paddingLeft = '16px'; ts.paddingRight = '16px';
    ts.backgroundColor = 'rgba(10,14,20,0.72)';
    ts.border = '1px solid #ffffff14';
    ts.borderRadius = '6px';
    ts.fontFamily = S.font;
    ts.zIndex = '51';
    ts.opacity = '0';
    ts.transform = 'translateY(-6px)';
    ts.transitionProperty = 'opacity, transform';
    ts.transitionTimingFunction = 'ease-out';
    ts.transitionDuration = '0.2s';
    function toastLabel(size, color, spacing, bold) {
      var l = $.CreatePanel('Label', toast, '', {});
      l.hittest = false;
      l.style.fontSize = size + 'px'; l.style.color = color;
      l.style.letterSpacing = spacing + 'px'; l.style.verticalAlign = 'center';
      if (bold) l.style.fontWeight = 'bold';
      return l;
    }
    var toastCaption = toastLabel(11, S.cap, 2, false);
    var toastValue = toastLabel(17, S.value, 0, true);
    toastValue.style.marginLeft = '12px';
    var toastArrow = toastLabel(11, S.accent, 0, false);
    toastArrow.style.marginLeft = '8px';
    var toastSeen = -1, toastToken = 0;
    function trimNumber(v, places) {
      var t = v.toFixed(places);
      if (t.indexOf('.') >= 0) t = t.replace(/0+$/, '').replace(/\.$/, '');
      return t;
    }
    // Centred across the game picture -- the workspace preview while Camera Editor / Config /
    // Render is open (a.area, window fractions), the whole window otherwise -- 17% down it.
    function placeToast(a) {
      var r = a.area || [0, 0, 1, 1];
      var sx = ctx.actualuiscale_x || 1, sy = ctx.actualuiscale_y || 1;
      var w = (ctx.actuallayoutwidth || 0) / sx, h = (ctx.actuallayoutheight || 0) / sy;
      var tw = (toast.actuallayoutwidth || 0) / sx;
      if (w <= 0 || h <= 0) return;
      ts.marginLeft = Math.round((r[0] + r[2]) / 2 * w - tw / 2) + 'px';
      ts.marginTop = Math.round((r[1] + (r[3] - r[1]) * 0.17) * h) + 'px';
    }
    function showToast(s) {
      var a = s.adjust;
      if (!a) return;
      placeToast(a);
      if (toastSeen < 0) { toastSeen = a.seq; return; } // a fresh panel shows nothing old
      if (a.seq === toastSeen) return;
      toastSeen = a.seq;
      if (!a.on || !a.kind) return;
      if (a.kind === 1) {
        toastCaption.text = 'CAMERA SPEED';
        toastValue.text = trimNumber(a.speed, 3) + 'x';
      } else {
        toastCaption.text = 'FIELD OF VIEW';
        toastValue.text = (s.fov != null ? Math.round(s.fov) : '-') + '\u00B0';
      }
      // Speed up = faster; FOV wheel-up zooms in (lower FOV), so its arrow follows the number.
      var up = a.kind === 1 ? a.dir > 0 : a.dir < 0;
      toastArrow.text = up ? '\u25B2' : '\u25BC';
      ts.transitionDuration = '0.2s';
      ts.opacity = '1';
      ts.transform = 'translateY(0px)';
      var token = ++toastToken;
      $.Schedule(1.1, function () {
        if (token !== toastToken || !toast.IsValid()) return;
        ts.transitionDuration = '0.55s';
        ts.opacity = '0';
        ts.transform = 'translateY(-6px)';
      });
    }

    api.render = function () {
      var raw = hostAttr.GetAttributeString('state', '');
      if (!raw) { root.visible = false; return; }
      var s; try { s = JSON.parse(raw); } catch (e) { return; }
      showToast(s);

      root.visible = !!s.visible; // toggles both cards at once
      root.hittest = false;       // display-only: never block clicks
      if (!s.visible) return;

      positionControlsCard(); // keep the controls card clear of the live minimap

      vMode.text = s.mode || '-';
      vMode.style.color = s.freecam ? S.accent : S.value;
      vState.text = s.playing ? (s.paused ? 'Paused' : 'Playing') : 'No demo';
      vState.style.color = s.paused ? S.accent : S.value;
      vSpeed.text = (s.speed != null) ? f1(s.speed) + 'x' : '-';

      vPos.text = f1(s.ox) + ', ' + f1(s.oy) + ', ' + f1(s.oz);
      vAng.text = f1(s.pitch) + ', ' + f1(s.yaw) + ', ' + f1(s.roll);
      vFov.text = f1(s.fov);
      var tk = (s.tick != null) ? String(s.tick) : '-';
      var tm = (s.time != null) ? f1(s.time) + 's' : '-';
      vTT.text = tk + '  ·  ' + tm;

      var showMeshDebug = !!s.meshDebug;
      sepMesh.visible = showMeshDebug;
      secMesh.visible = showMeshDebug;
      if (!showMeshDebug) {
        vMeshVmWarn.text = '';
      } else if (s.mesh && s.mesh.ok) {
        vMeshWeapon.text = s.mesh.weapon || '-';
        paintMeshBadges(meshWorldBadges, s.mesh.world);
        paintMeshBadges(meshVmBadges, s.mesh.vmFound ? s.mesh.vm : null);
        vMeshVmWarn.text = s.mesh.vmFound ? '' : 'FP viewmodel not resolved';
      } else {
        // Show WHY instead of just going blank -- "dead" / "no-weapon" / "pawn-gone" etc. are all
        // real, common states (a player mid-death or mid-switch), not necessarily bugs; seeing which
        // one it is at the exact instant a screenshot is taken is the whole point of this readout.
        vMeshWeapon.text = (s.mesh && s.mesh.reason) ? ('(' + s.mesh.reason + ')') : '-';
        paintMeshBadges(meshWorldBadges, null);
        paintMeshBadges(meshVmBadges, null);
        vMeshVmWarn.text = '';
      }
    };
    $.MovieHud = api;
    api.render();
    $.Msg('[moviehud] panel built.\n');
  } catch (err) {
    $.Msg('[moviehud] gui error: ' + err + '\n');
  }
})();
)MHJS";

} // namespace Filmmaker
