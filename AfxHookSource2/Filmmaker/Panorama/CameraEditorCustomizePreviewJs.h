#pragma once

// CUSTOMIZE modal 3D PREVIEW wiring: the native MapPlayerPreviewPanel (vanity loadout
// scene), its 2D fallback card, scene presets, zoom/pan/fit tuning, the paused-demo
// composite nudge, and the preview diagnostics. JS fragment concatenated (via #include,
// as an adjacent raw string literal) into the kCameraEditorJs script body assembled in
// CameraEditorJs.h -- spliced mid-stream from CameraEditorCustomizeJs.h. Not a
// standalone script: shares scope/closures with the root IIFE and the Customize modal
// fragment (mk, lbl, btn, S, st, custBody, closeCustomize, custLoadout, ...).

R"EDJS(
    // LEFT: preview. This is the SAME native panel the loadout / item-inspect / end-of-round win
    // panel use (MapPlayerPreviewPanel) -- and the win panel proves it renders inside the in-game
    // HUD context. It needs the FULL creation attribute set (a `map` scene + camera + composition
    // layer + character mode); the earlier empty black box was a panel created without a `map`
    // scene, so it had nothing to render. Config mirrors reference/ vanity-loadout.xml /
    // inspect.js. Created lazily on first open (the scene load is heavy); 2D card if it can't load.
    // Column 2 of the 7b layout: 440px, a radial #1c2534 -> #0c1016 ground, a rule on its right.
    // The composited preview panel draws ABOVE this, so nothing about the character's own
    // rendering changes.
    var prevWrap = mk('Panel', custBody); prevWrap.style.width = '440px'; prevWrap.style.height = '100%';
    prevWrap.style.backgroundColor = 'gradient( radial, 50% 85%, 0% 0%, 80% 80%, from( #1c2534ff ), to( #0c1016ff ) )';
    prevWrap.style.borderRight = '1px solid ' + F8.rule;
    prevWrap.style.flowChildren = 'down';
    prevWrap.style.overflow = 'clip';
    // Stage holds the 3D/2D preview and fills all space above the single wheel-zoom helper row (a
    // fixed-height sibling); fill-parent-flow(1.0) here is the same remaining-space pattern custBody
    // uses against custHead/custPickupBanner above. The near-black backdrop + inner border stand in
    // for a vignette -- Panorama has no CSS gradient support to verify here (see sigscan.py note).
    var prevStage = mk('Panel', prevWrap); prevStage.style.width = '100%'; prevStage.style.height = 'fill-parent-flow(1.0)';
    prevStage.style.flowChildren = 'down'; prevStage.style.overflow = 'clip';
    // No "Scroll to zoom" hint row any more: the preview has no user zoom or pan (see
    // previewUpdateDrag / custWheel), so prevStage takes the whole frame and there is nothing to
    // advertise. The character always sits at the baked PREVIEW_FIT_* framing.
    var previewClip = null, preview3d = null, previewItem3d = null, preview3dTried = false, previewSerial = 0, previewModelKey = '', previewGlovesKey = '';
    // Wheel-zoom over the character (CSS scale on the composited panel -- no re-composite, so it keeps
    // working while the demo is paused, unlike resizing the panel which would need a fresh scene tick).
    var previewZoom = 1.0, PREVIEW_ZOOM_MIN = 0.6, PREVIEW_ZOOM_MAX = 1.6, PREVIEW_ZOOM_STEP = 0.16;
    // Tuned for the cam_loadoutmenu_ct full-body camera (see PREVIEW_SCENES): 1.3 fills the preview
    // box with the whole figure at zoom 1; zoom range 0.6..MAX then spans "small full body with
    // breathing room" to "weapon close-up". (The old 0.74/-12/+22 fit was for the cropped vanity cam.)
    // FIT_X/Y live-set 2026-07-01 by the user via click-and-drag + previewSave('start') at zoom 0.96.
    var PREVIEW_FIT_SCALE = 1.3, PREVIEW_FIT_X = -25.2, PREVIEW_FIT_Y = 30.6;
    var previewPlayerSeq = 0; // bumped each time a NEW player-preview panel is created (diagnostics)
    // Zoom-in TARGET framing (user-requested): zoomed all the way out = the full-body figure
    // (PREVIEW_FIT_* above); zooming IN glides the view UP to chest height and a touch LEFT so the
    // held weapon stays fully in frame at close-up. Implemented by interpolating the transform
    // origin (and an extra x shift) with the zoom factor: origin at the chest means magnification
    // grows around the chest, so the visible region rises as you zoom. Live-tunable via
    // previewFit('{"ox":N,"oy":N,"sx":N,"sy":N}') or click-and-drag + previewSave('zoom').
    // OX/OY tuned 2026-07-01 against screenshots (previewFit A/B at max zoom). SX/SY/ZOOM_MAX
    // live-set the same day by the user via click-and-drag + previewSave('zoom') at zoom 1.6 --
    // previewSave('zoom') sets ZOOM_MAX = whatever zoom you were at (so scrolling can never exceed
    // where you stopped) and bakes the drag straight into SX/SY, since that makes t=1 exactly here.
    var PREVIEW_ZOOMIN_OX = 60;   // transform-origin X% at max zoom (right of centre -> content drifts LEFT)
    var PREVIEW_ZOOMIN_OY = 28;   // transform-origin Y% at max zoom (~chest height of the standing figure)
    var PREVIEW_ZOOMIN_SX = -16.8; // extra x translate (px) blended in at max zoom (nudges the figure left)
    var PREVIEW_ZOOMIN_SY = 62.1;  // extra y translate (px) blended in at max zoom (drag-savable, see previewSave)
    // Click-and-drag pan: an ACCUMULATED, uncommitted offset (design px) added on top of the baked
    // fit, live-updated while the user drags inside the preview box. Persists across separate drag
    // gestures (release then drag again keeps nudging) until previewSave() bakes it into the START
    // or ZOOM-IN constants above, or previewPanReset() discards it. See previewUpdateDrag().
    var previewPanLive = { x: 0, y: 0 };
    function applyPreviewZoom() {
      // 0 at zoom<=1 (full-body framing untouched), 1 at max zoom (full chest-height migration).
      var t = (previewZoom > 1.0) ? (previewZoom - 1.0) / (PREVIEW_ZOOM_MAX - 1.0) : 0;
      if (t > 1) t = 1;
      var ox = 50 + (PREVIEW_ZOOMIN_OX - 50) * t;
      var oy = 50 + (PREVIEW_ZOOMIN_OY - 50) * t;
      var tx = PREVIEW_FIT_X + PREVIEW_ZOOMIN_SX * t + previewPanLive.x;
      var ty = PREVIEW_FIT_Y + PREVIEW_ZOOMIN_SY * t + previewPanLive.y;
      var z = (previewZoom * PREVIEW_FIT_SCALE).toFixed(3);
      var org = ox.toFixed(1) + '% ' + oy.toFixed(1) + '%';
      var tr = 'translate3d(' + Math.round(tx) + 'px, ' + Math.round(ty) + 'px, 0px) scale3d(' + z + ', ' + z + ', 1)';
      if (preview3d && preview3d.IsValid && preview3d.IsValid()) {
        preview3d.style.transformOrigin = org; preview3d.style.transform = tr;
      }
      if (typeof preview2d !== 'undefined' && preview2d) { preview2d.style.transformOrigin = org; preview2d.style.transform = tr; }
    }
    // One frame of click-and-drag panning. Panorama has no mouse-move event in this in-game HUD
    // context, so this reads the C++ cursor probe published each frame in st.mx/my/lmb (same pipe
    // CameraEditorHud::BuildStateJson feeds from CameraBridge_GetUiCursor -- built for the
    // experimental graph editor's drag tool, reused here as-is). A press must LAND on the preview
    // stage to start a drag (so dragging elsewhere in the modal, e.g. a dropdown, is untouched);
    // once started it tracks the whole modal area so a fast drag that slips outside the box edge
    // doesn't drop the gesture. Delta is converted device-px -> design-px by the same /actualuiscale
    // division positionPreview3d() uses, so drag speed matches the cursor 1:1 regardless of zoom.
    var previewDragActive = false;
    var previewDragAnchor = { x: 0, y: 0 };
    var previewDragBase = { x: 0, y: 0 };
    var previewDragLastLmb = false;
    // DISABLED (user request): click-and-drag panning moved the character out of its framing on any
    // stray click inside the preview box, which is the common case -- you click into the box on the
    // way to a dropdown. The preview is now a FIXED view: the baked PREVIEW_FIT_* framing, no pan, no
    // wheel zoom (see custWheel). The tracking code above/below is kept because previewSave()/
    // previewFit() still use the same constants for offline tuning over netcon; only the per-frame
    // INPUT is gone, so previewPanLive stays 0 and applyPreviewZoom() always reproduces the default.
    var PREVIEW_INTERACTIVE = false;
    function previewUpdateDrag() {
      if (!PREVIEW_INTERACTIVE) return;
      if (!custOverlay.visible || !st) return;
      var lmb = !!st.lmb, mx = st.mx, my = st.my;
      if (lmb && !previewDragLastLmb && overPreview(mx, my)) {
        previewDragActive = true;
        previewDragAnchor.x = mx; previewDragAnchor.y = my;
        previewDragBase.x = previewPanLive.x; previewDragBase.y = previewPanLive.y;
        previewLog('dragStart', 'mx=' + mx + ' my=' + my);
      } else if (!lmb) {
        if (previewDragActive) previewLog('dragEnd', 'pan=' + previewPanLive.x.toFixed(1) + ',' + previewPanLive.y.toFixed(1));
        previewDragActive = false;
      }
      if (previewDragActive && lmb) {
        var sx = root.actualuiscale_x || ctx.actualuiscale_x || 1;
        var sy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
        var nx = previewDragBase.x + (mx - previewDragAnchor.x) / sx;
        var ny = previewDragBase.y + (my - previewDragAnchor.y) / sy;
        if (nx !== previewPanLive.x || ny !== previewPanLive.y) {
          previewPanLive.x = nx; previewPanLive.y = ny;
          applyPreviewZoom();
        }
      }
      previewDragLastLmb = lmb;
    }
    // Bakes the current dragged pan into the framing you asked to save, then clears the live offset
    // so the NEXT drag starts fresh from zero again. 'start' = the zoomed-out/default framing
    // (PREVIEW_FIT_X/Y); 'zoom' = the zoomed-in framing (PREVIEW_ZOOMIN_SX/SY, blended in at max
    // zoom by applyPreviewZoom's t). Save whichever one matches the zoom level you were just at --
    // e.g. drag at zoom 1, then previewSave('start'); zoom to max, drag again, previewSave('zoom').
    function previewSavePosition(which) {
      which = ('' + (which || '')).toLowerCase();
      if (which === 'start') {
        PREVIEW_FIT_X += previewPanLive.x; PREVIEW_FIT_Y += previewPanLive.y;
        previewPanLive.x = 0; previewPanLive.y = 0;
        applyPreviewZoom();
        return 'saved start x=' + PREVIEW_FIT_X.toFixed(1) + ' y=' + PREVIEW_FIT_Y.toFixed(1);
      }
      if (which === 'zoom' || which === 'zoomin') {
        // "Save zoom" means "wherever I stopped scrolling in IS the max zoom position" -- so the
        // CURRENT zoom level becomes the new ceiling (PREVIEW_ZOOM_MAX), which makes the blend factor
        // t = (zoom-1)/(ZOOM_MAX-1) equal exactly 1 right here, and the raw dragged pan can be baked
        // straight into SX/SY with no projection needed (both are only ever evaluated at t=1 from
        // now on, since scrolling can never exceed where you are right now).
        if (previewZoom <= 1.001) {
          return 'previewSave("zoom") needs the preview zoomed in past 1.0 (currently ' + previewZoom.toFixed(2) + ')';
        }
        PREVIEW_ZOOM_MAX = previewZoom;
        PREVIEW_ZOOMIN_SX += previewPanLive.x; PREVIEW_ZOOMIN_SY += previewPanLive.y;
        previewPanLive.x = 0; previewPanLive.y = 0;
        applyPreviewZoom();
        return 'saved zoom max=' + PREVIEW_ZOOM_MAX.toFixed(2) + ' sx=' + PREVIEW_ZOOMIN_SX.toFixed(1) + ' sy=' + PREVIEW_ZOOMIN_SY.toFixed(1);
      }
      return 'usage: previewSave("start"|"zoom")';
    }
    function previewPanReset() {
      previewPanLive.x = 0; previewPanLive.y = 0;
      applyPreviewZoom();
      return 'pan reset';
    }
)EDJS"
R"EDJS(
    function zoomPreview(dir) {
      var z = previewZoom + dir * PREVIEW_ZOOM_STEP;
      if (z < PREVIEW_ZOOM_MIN) z = PREVIEW_ZOOM_MIN;
      if (z > PREVIEW_ZOOM_MAX) z = PREVIEW_ZOOM_MAX;
      if (z === previewZoom) return;
      previewZoom = z; applyPreviewZoom();
    }
    // Live fit tuner (netcon): previewFit('{"scale":0.8,"x":-12,"y":22,"zoom":1}') adjusts the CSS
    // fit transform without a panel rebuild, so the default framing can be dialed in against
    // screenshots and then baked into the PREVIEW_FIT_* defaults above.
    function previewFit(json) {
      var o = null; try { o = json ? JSON.parse(json) : null; } catch (e) { return 'bad json'; }
      if (o) {
        if (typeof o.scale === 'number') PREVIEW_FIT_SCALE = o.scale;
        if (typeof o.x === 'number') PREVIEW_FIT_X = o.x;
        if (typeof o.y === 'number') PREVIEW_FIT_Y = o.y;
        if (typeof o.zoom === 'number') previewZoom = o.zoom;
        if (typeof o.ox === 'number') PREVIEW_ZOOMIN_OX = o.ox;
        if (typeof o.oy === 'number') PREVIEW_ZOOMIN_OY = o.oy;
        if (typeof o.sx === 'number') PREVIEW_ZOOMIN_SX = o.sx;
        if (typeof o.sy === 'number') PREVIEW_ZOOMIN_SY = o.sy;
      }
      applyPreviewZoom();
      return 'fit scale=' + PREVIEW_FIT_SCALE + ' x=' + PREVIEW_FIT_X + ' y=' + PREVIEW_FIT_Y + ' zoom=' + previewZoom +
        ' zoomin ox=' + PREVIEW_ZOOMIN_OX + ' oy=' + PREVIEW_ZOOMIN_OY + ' sx=' + PREVIEW_ZOOMIN_SX + ' sy=' + PREVIEW_ZOOMIN_SY +
        ' pan=' + previewPanLive.x.toFixed(1) + ',' + previewPanLive.y.toFixed(1);
    }
    function overPreview(x, y) {
      if (typeof x !== 'number' || typeof y !== 'number') return false;
      if (!prevStage || !prevStage.IsValid || !prevStage.IsValid()) return false;
      var a = prevStage.GetPositionWithinWindow ? prevStage.GetPositionWithinWindow() : null;
      if (!a) return false;
      var w = prevStage.actuallayoutwidth || 0, h = prevStage.actuallayoutheight || 0;
      return x >= a.x && x <= a.x + w && y >= a.y && y <= a.y + h;
    }
    // ---- Preview diagnostics (mvm_debug) -----------------------------------------------------
    // Gated on st.mvmDebug, which CameraEditorHud::BuildStateJson publishes from
    // Filmmaker::MvmDebugLog_Active() -- so `mvm_debug start` (the existing native session logger,
    // see MvmDebugLog.cpp) transparently starts capturing preview state too: every $.Msg() call
    // is mirrored into the tier0 logging listener the debug log taps, no separate toggle needed. When
    // no log is running this is a single boolean check per call site, so it's cheap to leave in place.
    // Prints go to the console (and therefore netcon), so keep them OFF (this guard) outside a
    // debug session -- they would otherwise spam every wheel notch / frame.
    function mvmDebugOn() { return !!(st && st.mvmDebug); }
    // Raw rects for the preview stage vs the clip box that is supposed to contain it, plus the zoom
    // scale -- the exact numbers needed to tell "clipped correctly but framed oddly" apart from
    // "escaping its own clip box" when reading a log next to a screenshot.
    function previewGeom() {
      var stageA = (prevStage && prevStage.GetPositionWithinWindow) ? prevStage.GetPositionWithinWindow() : null;
      var stageW = prevStage ? (prevStage.actuallayoutwidth || 0) : 0, stageH = prevStage ? (prevStage.actuallayoutheight || 0) : 0;
      var clipA = (previewClip && previewClip.GetPositionWithinWindow) ? previewClip.GetPositionWithinWindow() : null;
      var clipW = previewClip ? (previewClip.actuallayoutwidth || 0) : 0, clipH = previewClip ? (previewClip.actuallayoutheight || 0) : 0;
      var p3dW = preview3d ? (preview3d.actuallayoutwidth || 0) : 0, p3dH = preview3d ? (preview3d.actuallayoutheight || 0) : 0;
      return {
        zoom: previewZoom, fitScale: PREVIEW_FIT_SCALE, effScale: previewZoom * PREVIEW_FIT_SCALE,
        fitX: PREVIEW_FIT_X, fitY: PREVIEW_FIT_Y,
        zoomin: { ox: PREVIEW_ZOOMIN_OX, oy: PREVIEW_ZOOMIN_OY, sx: PREVIEW_ZOOMIN_SX, sy: PREVIEW_ZOOMIN_SY },
        pan: { x: previewPanLive.x, y: previewPanLive.y, dragging: previewDragActive },
        stage: { x: stageA ? stageA.x : -1, y: stageA ? stageA.y : -1, w: stageW, h: stageH },
        clip: { x: clipA ? clipA.x : -1, y: clipA ? clipA.y : -1, w: clipW, h: clipH },
        p3d: { w: p3dW, h: p3dH }
      };
    }
    var previewLogLastSig = ''; // last logged snapshot signature -- collapses identical per-frame calls
    function previewLog(tag, extra) {
      if (!mvmDebugOn()) return;
      try {
        var g = previewGeom();
        var sig = tag + '|' + custTargetKey + '|' + custActiveCategory + '|' + previewModelKey + '|' +
          g.zoom.toFixed(2) + '|' + (extra || '');
        if (tag === 'frame' && sig === previewLogLastSig) return; // heartbeat-only tag may repeat every frame
        previewLogLastSig = sig;
        var p3dState = preview3d ? ((preview3d.IsValid && preview3d.IsValid()) ? 'valid' : 'invalid')
          : (preview3dTried ? 'failed' : 'none');
        var line = '[mvmpreview] ' + tag +
          ' open=' + (custOverlay.visible ? 1 : 0) +
          ' target=' + custTargetKey + '(' + custTargetName + ')' +
          ' cat=' + custActiveCategory +
          ' p3d=' + p3dState +
          ' modelKey=' + previewModelKey +
          ' zoom=' + g.zoom.toFixed(2) + ' effScale=' + g.effScale.toFixed(3) +
          ' stage=' + g.stage.x + ',' + g.stage.y + ' ' + g.stage.w + 'x' + g.stage.h +
          ' clip=' + g.clip.x + ',' + g.clip.y + ' ' + g.clip.w + 'x' + g.clip.h +
          ' sel.agent=' + (custSel.agent || '') + ' sel.primary=' + (custSel.primary || '') +
          ' sel.secondary=' + (custSel.secondary || '') + ' sel.knife=' + (custSel.knife || '') +
          ' sel.gloves=' + (custSel.gloves || '');
        if (extra) line += ' ' + extra;
        $.Msg(line);
      } catch (e) {}
    }
)EDJS"
R"EDJS(
    // Native 3D MapPlayerPreviewPanel (vanity loadout scene). The make-or-break detail is TIMING:
    // the panel must be CREATED after the modal overlay is visible AND has a laid-out size, or it
    // instantiates but renders black. So creation is deferred via $.Schedule from openCustomize
    // (recreatePreview), not done inline while the overlay is still 0-sized. The 2D card below is a
    // fallback shown only if the native panel fails to instantiate.
    var USE_3D_PREVIEW = true;

    // 2D loadout card (fallback): player header + one rarity-colored row per slot, so the spectated
    // player's actual weapons/knife/gloves (and any picked skins) stay readable if 3D is unavailable.
    var preview2d = mk('Panel', prevStage); preview2d.style.width = '100%'; preview2d.style.height = '100%';
    preview2d.style.flowChildren = 'down'; preview2d.style.paddingTop = '30px';
    preview2d.style.paddingLeft = '30px'; preview2d.style.paddingRight = '30px'; preview2d.visible = true;
    var prev2dPlayer = clbl(preview2d, '', S.value, 23); prev2dPlayer.style.fontWeight = 'bold'; prev2dPlayer.style.letterSpacing = '1px';
    var prev2dTeam = clbl(preview2d, '', S.hint, 13); prev2dTeam.style.marginTop = '2px'; prev2dTeam.style.marginBottom = '22px';
    function prev2dRow(label) {
      var row = mk('Panel', preview2d); row.style.width = '100%'; row.style.flowChildren = 'down';
      row.style.marginBottom = '16px'; row.style.paddingLeft = '13px'; row.style.paddingTop = '3px'; row.style.paddingBottom = '3px';
      row.style.borderLeft = '3px solid ' + S.accent;
      var l = clbl(row, label, S.hint, 11); l.style.fontWeight = 'bold'; l.style.letterSpacing = '2px';
      var v = clbl(row, '', S.value, 17); v.style.fontWeight = 'bold'; v.style.marginTop = '3px';
      v.style.whiteSpace = 'nowrap'; v.style.textOverflow = 'ellipsis'; v.style.width = '100%';
      return { row: row, l: l, v: v };
    }
    var prev2dCard = {
      agent: prev2dRow('AGENT'), primary: prev2dRow('PRIMARY'), secondary: prev2dRow('SECONDARY'),
      knife: prev2dRow('MELEE / KNIFE'), gloves: prev2dRow('GLOVES')
    };
    function renderLoadoutCard() {
      prev2dPlayer.text = (custTargetName || 'Player').toUpperCase();
      var tm = normalizeTeamName(custTargetTeam);
      prev2dTeam.text = tm === 'CT' ? 'Counter-Terrorist' : (tm === 'T' ? 'Terrorist' : 'Spectated player');
      var slots = ['agent', 'primary', 'secondary', 'knife', 'gloves'];
      for (var i = 0; i < slots.length; i++) {
        var slot = slots[i], entry = prev2dCard[slot];
        var has = (slot === 'primary' || slot === 'secondary')
          ? !!(custLoadout[slot] && custLoadout[slot].defIndex > 0) : true;
        entry.row.visible = has;
        if (!has) continue;
        var opt = selOpt(slot, custSel[slot]);
        var meta = opt ? (opt[2] || {}) : {};
        var name = opt ? opt[0] : '—';
        if (slot !== 'agent') name = withWearLabel(slot, name);
        entry.v.text = name;
        var col = meta.color || S.value;
        entry.v.style.color = col;
        entry.row.style.borderLeft = '3px solid ' + col;
      }
    }

    // Default agent model for the spectated player's team (until we read their real model in C++).
    function teamDefaultModel() {
      return (normalizeTeamName(custTargetTeam) === 'CT') ? 'agents/models/ctm_sas/ctm_sas.vmdl'
                                                          : 'agents/models/tm_phoenix/tm_phoenix.vmdl';
    }
    function loadoutTeam() {
      return (normalizeTeamName(custTargetTeam) === 'CT') ? 'ct' : 't';
    }
    function loadoutItem(slot) {
      try { return LoadoutAPI.GetItemID(loadoutTeam(), slot) || ''; }
      catch (e) { return ''; }
    }
)EDJS"
R"EDJS(
    function previewState() {
      var playerState = preview3d ? ('player=' + (preview3d.IsValid ? preview3d.IsValid() : '?'))
                                  : (preview3dTried ? 'player-fallback' : 'player-uninit');
      var itemState = previewItem3d ? (' item=' + (previewItem3d.IsValid ? previewItem3d.IsValid() : '?')) : ' item-uninit';
      return playerState + itemState;
    }
    // Scene presets for the MapPlayerPreviewPanel. CRUCIAL: the vanity/loadout scenes
    // (ui/buy_menu + cam_vanityloadout / cam_loadoutmenu_*) only render in the MAIN-MENU Panorama
    // root; in the in-game HUD (CSGOHud, where this editor lives) they instantiate but composite
    // nothing -> a black box. That conclusion turned out to be an attribute problem, not a scene
    // restriction: a `composition-layer-texture-name` attribute (paired with `require-composition-layer`)
    // is what actually lets Panorama's compositor bind and display the panel's render target, and we
    // were never setting it. A CS2 cheat-menu thread (github/UC "Preview Models in Menu") confirms the
    // vanity/buy_menu scene DOES composite outside the main-menu root once that attribute -- plus
    // `player`, `sync_spawn_addons`, and `csm_split_plane0_distance_override` -- are set:
    //   $.CreatePanel('MapPlayerPreviewPanel', ctx, 'preview_texture_name', {
    //     map: 'ui/buy_menu', camera: 'cam_loadoutmenu_ct', 'require-composition-layer': true,
    //     'composition-layer-texture-name': 'preview_texture_name', playermodel: '...',
    //     animgraphcharactermode: 'buy-menu', player: true, mouse_rotate: true,
    //     sync_spawn_addons: true, 'transparent-background': true, 'pin-fov': 'vertical',
    //     csm_split_plane0_distance_override: '250.0' });
    // So the vanity scene (proper lit loadout backdrop, not the flat grey mvp-banner fallback) is the
    // default again; match_mvp/loadoutmenu_ct stay for live A/B over `previewTry`.
    // Scene 0 (default) uses cam_loadoutmenu_ct, NOT cam_vanityloadout: the vanity camera frames only
    // the upper body and hard-crops at the thighs INSIDE the rendered texture (live-verified: CSS
    // zoom-out to 0.6 showed the same thigh cut with black below -- the legs simply are not in the
    // texture, and pin-fov horizontal made no difference). The loadout camera renders the full body
    // head-to-boots, so zooming out actually reveals the whole character; PREVIEW_FIT_SCALE below is
    // tuned for it (1.3 fills the box with the full figure at zoom 1).
    var PREVIEW_SCENES = [
      { map: 'ui/buy_menu', camera: 'cam_loadoutmenu_ct', mode: 'buy-menu', pname: 'vanity_character', bg: 'true' },
      { map: 'ui/match_mvp', camera: 'camera', mode: 'mvp-banner', pname: 'mvp_char', bg: 'false' },
      { map: 'ui/buy_menu', camera: 'cam_vanityloadout', mode: 'buy-menu', pname: 'vanity_character', bg: 'true' }
    ];
    var previewSceneIdx = 0;
    function currentScene() { return PREVIEW_SCENES[previewSceneIdx] || PREVIEW_SCENES[0]; }
    function destroyPreview3d() {
      try { if (previewClip && previewClip.DeleteAsync) previewClip.DeleteAsync(0); } catch (e0) {}
      try { if (preview3d && preview3d.DeleteAsync) preview3d.DeleteAsync(0); } catch (e1) {}
      previewClip = null; preview3d = null; preview3dTried = false; previewModelKey = '';
      previewDragCatcher = null; // child of previewClip -- deleted along with it above
    }
    var previewDragCatcher = null; // transparent hit-test blocker over preview3d, see below
    function ensurePreviewClip() {
      if (previewClip && previewClip.IsValid && previewClip.IsValid()) return previewClip;
      // On the modal's own root (custOverlay, z230), not CamEditorRoot (z53): as a child of the
      // lower root the preview sat under the modal's scrim. Its dropdowns (z420) stay above it.
      previewClip = mk('Panel', custOverlay);
      previewClip.hittest = false;
      previewClip.visible = false;
      previewClip.style.overflow = 'clip';
      previewClip.style.zIndex = '232';
      previewClip.style.backgroundColor = 'rgba(0,0,0,0)';
      // Sits ABOVE preview3d (zIndex 1 > preview3d's 0) and absorbs every click itself (hittest=true,
      // fully transparent) so the native panel's own mouse_rotate/panzoom_enabled (kept true -- see
      // createPreview3d comment -- because disabling them was a compositing risk we didn't want to
      // retest) never receives a drag to fight our click-and-drag pan with. Our own pan tracking
      // (previewUpdateDrag) reads the OS-cursor pipe directly and does not depend on this panel
      // getting the hit -- it only needs to exist so nothing ELSE does.
      previewDragCatcher = mk('Panel', previewClip);
      previewDragCatcher.hittest = true;
      previewDragCatcher.style.width = '100%'; previewDragCatcher.style.height = '100%';
      previewDragCatcher.style.position = '0px 0px 0px';
      previewDragCatcher.style.zIndex = '1';
      previewDragCatcher.style.backgroundColor = 'rgba(0,0,0,0)';
      return previewClip;
    }
    // Create the native MapPlayerPreviewPanel with the current scene's attrs (optionally overridden,
    // for live tuning over netcon). Returns the panel or null.
    function createPreview3d(overrides) {
      var sc = currentScene();
      var texName = 'CustPreviewTex' + (++previewSerial);
      var attrs = {
        'require-composition-layer': 'true', 'composition-layer-texture-name': texName,
        'pin-fov': 'vertical', 'transparent-background': sc.bg,
        'class': 'mvp_map', map: sc.map, camera: sc.camera, animgraphcharactermode: sc.mode,
        playername: sc.pname, player: 'true', mouse_rotate: 'true', sync_spawn_addons: 'true',
        csm_split_plane0_distance_override: '250.0',
        // CRITICAL for the paused demo: without this the panel HIDES ITSELF while its composite
        // materials are still building, and on a paused demo (no game frames advancing) that build
        // may never signal complete -> the character never appears ("no visible person"). The
        // native vanity-loadout.xml AND loadout_grid.xml both set this false for exactly this
        // reason (show the model immediately, don't wait/cull). This is the panel property the
        // UnknownCheats thread warns about ("prevent panorama from culling them").
        hide_while_waiting_for_composite_materials: 'false',
        // These native interaction attrs are also part of the compositor path for this panel. We keep
        // them enabled for rendering, then place a transparent blocker above the preview so user drags
        // never reach the native rotate/pan handlers. Wheel is swallowed in C++ and routed to custWheel.
        hittest: 'true', panzoom_enabled: 'true'
      };
      attrs.playermodel = (overrides && overrides.playermodel) ? overrides.playermodel : teamDefaultModel();
      // Merge ANY other override keys verbatim (camera, map, pin-fov, animgraphcharactermode, ...) so
      // previewRebuild('{"pin-fov":"horizontal"}') can live-A/B panel attrs over netcon without a
      // rebuild -- the api comment always advertised map/camera overrides but only playermodel was
      // actually honored.
      if (overrides) { for (var ok in overrides) { if (ok !== 'playermodel') attrs[ok] = '' + overrides[ok]; } }
      var p = null;
      try {
        // Create inside a root-level clipped viewport, not inside the styled/flowing preview frame.
        // The clip panel prevents CSS zoom from drawing over the controls, while staying root-level
        // avoids the blank composition target seen when the native preview was nested in prevStage.
        var clip = ensurePreviewClip();
        p = clip ? $.CreatePanel('MapPlayerPreviewPanel', clip, texName, attrs) : null;
        if (!(p && p.IsValid && p.IsValid())) p = null;
      } catch (e) { p = null; }
      if (p) {
        previewPlayerSeq++;
        p.style.width = '100%';
        p.style.height = '100%';
        p.style.position = '0px 0px 0px';
        p.style.zIndex = '0';
        p.style.overflow = 'clip';
        positionPreview3d();
      }
      return p;
    }
    function positionPreview3d() {
      if (!previewClip || !previewClip.IsValid || !previewClip.IsValid()) return;
      if (!custOverlay.visible || !prevStage || !prevStage.IsValid || !prevStage.IsValid()) {
        previewClip.visible = false;
        return;
      }
      var a = prevStage.GetPositionWithinWindow ? prevStage.GetPositionWithinWindow() : null;
      var rawW = prevStage.actuallayoutwidth || 0, rawH = prevStage.actuallayoutheight || 0;
      if (!a || rawW <= 0 || rawH <= 0) { previewClip.visible = false; return; }
      // GetPositionWithinWindow()/actuallayoutwidth report ACTUAL (device) pixels, but previewClip is
      // a ROOT-level panel whose style 'px' values are interpreted in DESIGN-space units (same space
      // every other style declaration in this file uses) and then scaled up to device pixels by
      // actualuiscale (measured live at 1.1111 = 1600x1200 device / 1440x1080 design). Assigning the
      // raw device-px numbers straight into style.x/y/width/height skipped that conversion, so the
      // clip box rendered ~11% too big and off-position relative to prevStage's real on-screen box --
      // negligible-looking at zoom 1, but the error compounds with zoom and pushes the character
      // outside the intended frame ("not locked to the box"). custMeasureH() below already divides by
      // actualuiscale_y for the same reason (device px -> design px); mirror that here for x/y/w/h.
      var sx = root.actualuiscale_x || ctx.actualuiscale_x || 1;
      var sy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
      // custWin carries an F8-style scale3d() shrink. A Panorama transform changes what is DRAWN but
      // not the layout metrics, so actuallayoutwidth/height still report the unscaled box and have to
      // be multiplied by it here. GetPositionWithinWindow() reports the drawn position and is
      // already transform-aware, so a.x/a.y are NOT scaled -- if the character ever sits offset from
      // its frame after a CS2 update, that assumption is the first thing to re-check.
      var k = (typeof custScale === 'function') ? (custScale() || 1) : 1;
      var w = Math.floor(rawW * k / sx);
      var h = Math.floor(rawH * k / sy);
      if (w <= 0 || h <= 0) { previewClip.visible = false; return; }
      var x = Math.floor(a.x / sx), y = Math.floor(a.y / sy);
      previewClip.visible = true;
      previewClip.style.position = x + 'px ' + y + 'px 0px';
      previewClip.style.x = x + 'px';
      previewClip.style.y = y + 'px';
      previewClip.style.width = w + 'px';
      previewClip.style.height = h + 'px';
      // The HOLDING chip rides the same rect, 12px in from the top-left corner.
      if (typeof custHoldChip !== 'undefined' && custHoldChip) {
        custHoldChip.style.position = (x + 12) + 'px ' + (y + 12) + 'px 0px';
        custHoldChip.visible = true;
      }
      if (preview3d && preview3d.IsValid && preview3d.IsValid()) {
        preview3d.visible = true;
        preview3d.style.width = '100%';
        preview3d.style.height = '100%';
      }
    }
)EDJS"
R"EDJS(
    // NOTE: there is deliberately no demo resume/re-pause anywhere in this file any more. The old
    // `cosmetics previewnudge` path made the preview composite and animate by briefly PLAYING the
    // demo, which moved playback under a UI that said paused. Nothing here should ever touch demo
    // playback state again.
    //
    // Making this panel work on a paused demo is entirely a C++ job, split across three modules --
    // if the preview misbehaves, the symptom says which one, and NONE of them is fixable from here:
    //   black box .................. PreviewRenderProbe.cpp  (the panel's scene-render gate)
    //   frozen character ........... PreviewAnimTick.cpp     (animgraph tick delta)
    //   weapon draws, body does not  PreviewRenderFlag.cpp   (pawn never interpolated)
    // All three are armed by the C++ pump whenever this modal is open. See
    // Docs/features/customize-feature.md "Paused preview" before adding JS-side workarounds for any of them.
    function ensurePreview3d(model) {
      if (!USE_3D_PREVIEW) { preview2d.visible = true; return null; }
      model = model || teamDefaultModel();
      if (preview3d || preview3dTried) return preview3d;
      preview3dTried = true;
      preview3d = createPreview3d({ playermodel: model });
      preview2d.visible = !preview3d;
      return preview3d;
    }
    function ensureViewmodelPreview() {
      if (previewItem3d && previewItem3d.IsValid && previewItem3d.IsValid()) return previewItem3d;
      previewItem3d = null;
      try {
        previewItem3d = $.CreatePanel('MapItemPreviewPanel', prevStage, 'CustViewmodel3D' + (++previewSerial), {
          map: 'ui/xpshop_item', camera: 'camera_weapon_0', 'require-composition-layer': 'true',
          player: 'false', initial_entity: 'item', mouse_rotate: 'true', sync_spawn_addons: 'true',
          'transparent-background': 'true', 'pin-fov': 'vertical', hittest: 'true',
          panzoom_enabled: 'true', hide_while_waiting_for_composite_materials: 'false'
        });
        if (!(previewItem3d && previewItem3d.IsValid && previewItem3d.IsValid())) previewItem3d = null;
      } catch (e) { previewItem3d = null; }
      if (previewItem3d) {
        previewItem3d.style.width = '100%';
        previewItem3d.style.height = '34%';
        previewItem3d.style.marginTop = '8px';
        safeCall(previewItem3d, 'SetHideStaticGeometry', true);
        safeCall(previewItem3d, 'SetReadyForDisplay', true);
      }
      return previewItem3d;
    }
    function setItemPreviewItem(panel, itemId) {
      if (!panel || itemId == null) return;
      safeCall(panel, 'SetActiveItem', 0);
      if (safeCall(panel, 'SetItemItemId', itemId, '') == null) safeCall(panel, 'SetItemItemId', itemId.toString(), '');
      safeCall(panel, 'StartWeaponLookat');
      safeCall(panel, 'SetReadyForDisplay', true);
    }
    function equipPlayerPreviewMeta(panel, meta) {
      if (!panel || !meta) return;
      var itemId = previewEquipItemId(meta);
      if (!itemId) return;
      if (safeCall(panel, 'EquipPlayerWithItem', itemId) == null) safeCall(panel, 'EquipPlayerWithItem', itemId.toString());
    }
    function previewMetaId(meta) {
      var id = previewEquipItemId(meta);
      if (id) return '' + id;
      if (!meta) return '0:0';
      return [
        parseInt(meta.def || 0, 10) || 0,
        parseInt(meta.paint || meta.paintKit || 0, 10) || 0
      ].join(':');
    }
    // Push the chosen agent (or the spectated player's team default) onto whichever preview exists.
    // The item the preview should show HELD right now: whichever slot is "active" (see
    // setActiveCategory -- switches when the user clicks into Primary/Secondary/Melee). If that
    // slot's current selection is the generic "no paint" row (meta.def<=0, shared across every
    // weapon of that def), fall back to the weapon actually targeted for the slot (effectiveSlotDef)
    // so the model still holds a plain, unskinned copy of the right gun instead of nothing.
    function activeHeldItemMeta() {
      var cat = (custActiveCategory === 'secondary' || custActiveCategory === 'knife') ? custActiveCategory : 'primary';
      var opt = selOpt(cat, custSel[cat]);
      var meta = opt ? (opt[2] || {}) : {};
      if (parseInt(meta.def || 0, 10) > 0) return meta;
      var slotDef = (cat === 'primary' || cat === 'secondary') ? effectiveSlotDef(cat) : 0;
      return (slotDef > 0) ? { def: slotDef, paint: 0, color: meta.color } : meta;
    }
    function previewSceneKey(model, agentMeta, heldMeta, glovesMeta) {
      return [
        model || '',
        previewMetaId(agentMeta),
        previewMetaId(heldMeta),
        previewMetaId(glovesMeta)
      ].join('|');
    }
    // Mirrors the native vanity-loadout recipe (reference/ scripts/common/characteranims.js
    // PlayAnimsOnPanel): SetPlayerCharacterItemID + SetPlayerModel together give the composited
    // agent its real material (skin tone, team paint job); SetPlayerModel alone -- what this used to
    // do -- leaves the model on the flat, uncomposited fallback material (the washed-out grey/white
    // look the modal used to show). Then EquipPlayerWithItem for the held weapon/knife (ONE at a
    // time, matching whichever slot is active) and the gloves (always, independent of the held item).
    function applyPreview() {
      var agent = selOpt('agent', custSel.agent), gloves = selOpt('gloves', custSel.gloves);
      var held = activeHeldItemMeta();
      var model = (agent && agent[2] && agent[2].model) ? agent[2].model : teamDefaultModel();
      var agentMeta = agent ? agent[2] : null;
      var glovesMeta = gloves ? gloves[2] : null;
      var sceneKey = previewSceneKey(model, agentMeta, held, glovesMeta);
      if (preview3d && preview3d.IsValid && !preview3d.IsValid()) {
        destroyPreview3d();
      }
      // The panel can replace one glove item with another, but equipping the plain team gloves never
      // clears a finish that is already on the pawn -- the old glove model stays attached and the
      // agent's own default-hands body group never comes back, so the character showed no hands at all.
      // A fresh panel starts with empty gloves, which is exactly the default-hands state.
      var glovesKey = previewMetaId(glovesMeta);
      var defaultGloves = glovesMeta && !(parseInt(glovesMeta.paint || glovesMeta.paintKit || 0, 10) > 0);
      if (preview3d && previewGlovesKey && glovesKey !== previewGlovesKey && defaultGloves) {
        previewGlovesKey = glovesKey;
        previewLog('glovesToDefault', 'rebuild for default gloves ' + glovesKey);
        // Only this finish -> plain-gloves step rebuilds; it goes through the normal creation path.
        $.Schedule(0, recreatePreview);
        return;
      }
      previewGlovesKey = glovesKey;
      ensurePreview3d(model);
      if (preview3d) {
        var changed = previewModelKey !== sceneKey;
        previewModelKey = sceneKey;
        var sc = currentScene();
        if (sc.startCamera) safeCall(preview3d, 'TransitionToCamera', sc.startCamera, 0);
        safeCall(preview3d, 'SetActiveCharacter', 0); // char 0 = the single previewed agent
        var agentItemId = previewEquipItemId(agentMeta);
        if (agentItemId) safeCall(preview3d, 'SetPlayerCharacterItemID', agentItemId);
        safeCall(preview3d, 'SetPlayerModel', model);
        equipPlayerPreviewMeta(preview3d, held);
        equipPlayerPreviewMeta(preview3d, glovesMeta);
        safeCall(preview3d, 'SetReadyForDisplay', true);
        applyPreviewZoom();
        if (changed) {
          // This is an update to an already-composited panel, not a panel creation. Keep it ready
          // and apply the new scene exactly once. Toggling SetReadyForDisplay false here produced a
          // visible black frame on every spectator switch, while pokePreviewSoon re-equipped the
          // model/items for another 16 frames and made rapid post-death switches visibly thrash.
          // Creation/recreation still owns the bounded settle loop below.
          previewLog('sceneChanged', 'model=' + model + ' agentItemId=' + (agentItemId || 0) +
            ' heldId=' + previewMetaId(held) + ' glovesId=' + previewMetaId(glovesMeta));
          // No pose nudge here. An item/category change used to leave the gun floating in the old
          // stance on a paused demo because the animgraph got no ticks to play the hold-pose
          // transition. PreviewAnimTick now feeds the preview pawn a real per-frame tick delta while
          // paused, so the transition plays out on its own.
        }
      } else {
        renderLoadoutCard();
        previewLog('fallback2d');
      }
    }

    // The native MapPlayerPreviewPanel only composites its scene when its parent is VISIBLE and has
    // a real laid-out size. Creating it while the modal overlay was still hidden (the old open
    // order) left it permanently blank -- it only "worked" when external automation re-poked it
    // after the modal was already up. So we make the overlay visible first, then re-assert the
    // preview for a few frames from render() (layout settles a frame or two after visible flips).
    var previewPokeFrames = 0;
    var previewHealTries = 0; // bounded retries for maintainPreview's self-heal when the panel is missing
    function pokePreviewSoon() { previewPokeFrames = 16; previewHealTries = 8; }
)EDJS"
R"EDJS(
    function maintainPreview() {
      if (!custOverlay.visible) return;
      // Keep the composition-layer panel marked ready EVERY frame while the modal is open. Without
      // this continuous (cheap, idempotent) re-assert the scene composites once and then lapses to
      // black after the initial settle burst -- the "appears then disappears" bug. The heavier
      // re-equip/re-model (applyPreview) only runs during the post-open settle frames.
      if (preview3d && preview3d.IsValid && preview3d.IsValid()) {
        positionPreview3d();
        safeCall(preview3d, 'SetReadyForDisplay', true);
      }
      // Same idempotent re-assert for the optional held-item/viewmodel preview when one exists, so a
      // paused demo (no game frames driving the composite) can't let it lapse to black either.
      if (previewItem3d && previewItem3d.IsValid && previewItem3d.IsValid())
        safeCall(previewItem3d, 'SetReadyForDisplay', true);
      // Self-heal: if the panel failed to create while the modal is up, retry a FRESH create -- on a
      // PAUSED demo the one-shot post-open schedule may land before layout settles, leaving no panel.
      // Reset preview3dTried so ensurePreview3d (inside applyPreview) actually re-attempts; bounded by
      // previewHealTries so a genuinely unavailable scene falls back to the 2D card instead of
      // rebuilding forever. Uses applyPreview (not recreatePreview, which would re-arm this counter).
      if (USE_3D_PREVIEW && !preview3d && previewHealTries > 0) {
        previewHealTries--;
        previewLog('selfHeal', 'triesLeft=' + previewHealTries);
        preview3dTried = false; previewModelKey = '';
        applyPreview();
      }
      if (previewPokeFrames > 0) { previewPokeFrames--; applyPreview(); }
    }
    // Destroy + rebuild the 3D preview from scratch. Must run when the modal is already visible and
    // laid out (deferred via $.Schedule from openCustomize) -- creating it on a 0-sized parent makes
    // it render black even though it reports valid.
    function recreatePreview() {
      if (!custOverlay.visible) return;
      if (!USE_3D_PREVIEW) { preview2d.visible = true; renderLoadoutCard(); return; }
      previewLog('recreate');
      destroyPreview3d();
      applyPreview();      // creates the panel + equips + SetReadyForDisplay
      pokePreviewSoon();   // re-assert SetReadyForDisplay for the frames after the new panel lays out
    }

)EDJS"
