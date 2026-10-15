#pragma once

// WEATHER & SKY sub-tab of the CONFIG panel's EFFECTS tab. JS fragment concatenated (via
// #include, as an adjacent raw string literal) into the kConfigHudJs script body assembled in
// ConfigHudJs.h -- not a standalone script. Shares scope with the root IIFE and appends to
// `fxPageWeather` / `renderHooks`, both created in ConfigHudEffectsTabJs.h (included first).
//
// LAYOUT: four cards -- SKYBOX, SKY ORIENTATION, SKY TINT, CLOUDS -- and a tile grid three across.
// Orientation used to be a third page of a tab strip INSIDE the skybox card, which put four levels
// of paging between you and a yaw slider and made that strip mean two things at once. The strip now
// only chooses which half of the library you are looking at (CUSTOM | CS2).
//
// The first cut used one full-width row per skybox. With ~19 skyboxes that was a ~1100px column
// you had to scroll through to reach anything, and the stock CS2 skies -- the ones you change
// least often -- took up most of it. Tiles put six skyboxes in the height one row used to take,
// and the stock set collapses away entirely.
//
// Thumbnails are s2r:// textures. Custom ones are built by FX/tools/build-skybox.ps1; the stock
// ones by FX/tools/build-builtin-sky-previews.ps1. Panorama cannot load a loose .png off disk,
// which is why both are compiled textures. Any tile whose texture is missing or fails to load
// falls back to a letter tile, so a half-populated thumbnail set still looks deliberate.
//
// The grid is REBUILT ONLY WHEN THE LIST CHANGES, keyed off a signature of state.skyList. This
// hook runs every frame, and recreating ~19 Image panels per frame would both stutter and
// restart every texture load.
//
// CLOUDS. Colour drives the pre-existing `mirv_sky clouds` backend (SceneSystem.cpp); density,
// size and drift drive World/CloudFx.cpp. Both match materials by name containing "clouds", so
// they are map-adaptive for free -- whatever cloud materials the current map loaded are the ones
// written, with no per-map table anywhere.
//
// Depends on `rgbHex`/`hex2` from ConfigHudAtmosphereTabJs.h, which ConfigHudJs.h includes first.
// They are reused rather than redefined so the colour-string rule they encode lives in one place:
// colours MUST be #rrggbbaa hex, because Panorama's rgba() takes a 0..1 alpha and an invalid
// style assignment THROWS and aborts the whole script.

R"CFJS(
    // ===================== SKYBOX =======================================
    // ONE section with three pages, not three sections. Custom skies, stock skies and the live
    // orientation controls for whatever is selected are all "the skybox". Keeping them as pages
    // also means the controls stay close to the picker without making the Weather page taller.
    // THE SKYBOX CARD PICKS A SKY. NOTHING ELSE.
    //
    // It used to hold a third page, SETTINGS, carrying the live orientation controls -- which put
    // a tab strip inside a card, inside a sub-tab, inside a tab. Four levels of paging to reach a
    // yaw slider, and the strip's own tabs then meant two unrelated things at once: CUSTOM and CS2
    // are two halves of one list, SETTINGS was a different subject entirely. Orientation is now its
    // own card directly below, so the strip has one job (which half of the library) and the card
    // has one job (choose a sky).
        // ONE card on the EFFECTS tab with four sub-pages. These were four separate cards on the
    // ENVIRONMENT tab, which put CS2's own skybox library on the added-renderer tab -- the
    // split the original design drew is by WHAT A CONTROL DRIVES, and every control here drives a
    // value the game already has.
    var wsSec = section(pageEffects, 'WEATHER & SKY', true, false, 'weather.card', '#80bdffff');
    var wsReset = wsSec.__headerReset([
      'mirv_filmmaker world sky default',
      'mirv_filmmaker world skyorient reset',
      'mirv_sky color default',
      'mirv_filmmaker world clouds reset',
      'mirv_sky clouds color default'
    ], 'fx.weathersky');
    var skySec = wsSec.__page('Skybox');

    // Which library the grid shows: a labelled two-cell mode row (Docs/features/filmmaker_config_panel.md), not a tab strip
    // inside the card's own tab strip.
    var skyActiveTab = 'custom';
    var skyLibSeg = modeRow(skySec, 'Library', [['custom', 'Custom'], ['stock', 'CS2']], null, function (key) {
      skyActiveTab = key;
      applySkyTabs();
    }, 'Custom shows the skyboxes you built; CS2 shows the game’s own.');

    // One toolbar for both libraries, on ONE line: the primary action fills, Map default beside it.
    var skyTools = row(skySec); skyTools.style.marginTop = '8px';
    var skyCreateBtn = btn(skyTools, '+  Create custom skybox', function () {
      cmd('mirv_filmmaker world skybuild pick');
    }, S.accent);
    skyCreateBtn.style.width = 'fill-parent-flow(1.0)'; skyCreateBtn.style.height = '32px';
    skyCreateBtn.style.paddingTop = '0px'; skyCreateBtn.style.paddingBottom = '0px';
    skyCreateBtn.__lbl.style.horizontalAlign = 'center'; skyCreateBtn.__lbl.style.verticalAlign = 'center';
    skyCreateBtn.__bgBase = S.accent.substring(0, 7) + '14'; skyCreateBtn.__brBase = S.accent.substring(0, 7) + '66';
    paintBtn(skyCreateBtn);
    var skyDefaultBtn = btn(skyTools, 'Map default', function () {
      cmd('mirv_filmmaker world sky default');
    });
    skyDefaultBtn.style.marginRight = '0px'; skyDefaultBtn.style.height = '32px';
    skyDefaultBtn.style.paddingTop = '0px'; skyDefaultBtn.style.paddingBottom = '0px';
    skyDefaultBtn.style.paddingLeft = '14px'; skyDefaultBtn.style.paddingRight = '14px';
    skyDefaultBtn.__lbl.style.verticalAlign = 'center';

    var skyCustomHost = mk('Panel', skySec);
    skyCustomHost.style.flowChildren = 'down'; skyCustomHost.style.width = '100%';
    skyCustomHost.style.marginTop = '8px';

    var skyStockHost = mk('Panel', skySec);
    skyStockHost.style.flowChildren = 'down'; skyStockHost.style.width = '100%';
    skyStockHost.style.marginTop = '8px';

    var skyPages = { custom: skyCustomHost, stock: skyStockHost };
    function applySkyTabs() {
      for (var k in skyPages) skyPages[k].visible = (k === skyActiveTab);
      skyLibSeg.sync(skyActiveTab);
    }

    var skyTiles = {};
    var skySig = '';

    var skyDeleteHint = lbl(skySec, '', S.danger, 11);
    skyDeleteHint.style.whiteSpace = 'normal';
    skyDeleteHint.style.marginTop = '4px';
    skyDeleteHint.visible = false;

    // Both libraries use the same centered three-column grid, including incomplete final rows.
    var SKY_TILE_W = 126, SKY_TILE_IMG_H = 54;

    function skyTile(rowPanel, entry, isLast) {
      var t = mk('Panel', rowPanel);
      t.style.flowChildren = 'down';
      // Three to a row at whatever width the pane has: the grid can never push the pane sideways.
      t.style.width = 'fill-parent-flow(1.0)';
      t.style.marginRight = isLast ? '0px' : '5px';
      t.style.padding = '2px';
      t.style.borderRadius = '5px';
      t.style.border = '2px solid ' + S.clear;
      t.style.backgroundColor = S.btnBg;

      var thumb = mk('Panel', t);
      thumb.style.width = '100%'; thumb.style.height = SKY_TILE_IMG_H + 'px';
      thumb.style.borderRadius = '3px';
      thumb.style.backgroundColor = S.bgSoft;
      thumb.style.border = '1px solid ' + S.cardBorder;
      thumb.hittest = false;

      if (entry.image && entry.image.length) {
        var img = mk('Image', thumb);
        img.style.width = '100%'; img.style.height = '100%';
        img.style.borderRadius = '3px';
        img.style.horizontalAlign = 'center'; img.style.verticalAlign = 'center';
        img.hittest = false;
        // Not every stock sky yields a thumbnail (two of CS2's do not decode). Falling back keeps
        // those tiles looking intentional instead of empty.
        try {
          $.RegisterEventHandler('ImageFailedLoad', img, function () {
            try { img.visible = false; skyMonogram(thumb, entry.label); } catch (e) {}
          });
        } catch (e) {}
        try { img.SetImage(entry.image); } catch (e) { skyMonogram(thumb, entry.label); }
      } else {
        skyMonogram(thumb, entry.label);
      }

      var cap = lbl(t, entry.label, S.value, 11);
      cap.style.marginTop = '3px';
      cap.style.width = '100%';
      cap.style.textAlign = 'center';
      cap.style.textOverflow = 'ellipsis';
      cap.style.whiteSpace = 'nowrap';
      cap.hittest = false;

      // Over the image rather than beside the name: the caption row is only ~13px tall and a tick
      // there would crowd out the name it is meant to identify.
      var tick = lbl(thumb, '✔', '#ffffffff', 15);
      tick.style.horizontalAlign = 'right'; tick.style.verticalAlign = 'top';
      tick.style.marginRight = '3px';
      tick.style.fontWeight = 'bold';
      tick.style.textShadow = '0px 0px 3px 2.0 #000000ff';
      tick.visible = false;
      tick.hittest = false;

      t.SetPanelEvent('onactivate', function () {
        if (skyPendingDelete === entry.name) return; // armed for delete; the X is the target now
        cmd('mirv_filmmaker world sky apply ' + entry.name);
      });

      // Right-click deletes, but only your own skyboxes -- CS2's are not yours to remove, and a
      // delete prompt on them would be an offer the backend refuses anyway.
      //
      // TWO-STEP, because deleting a skybox you spent time importing on a single stray
      // right-click is not recoverable from the UI. The first right-click arms the tile; the
      // second click (on the X) commits. Right-clicking anything else disarms it. A modal
      // confirm would be the usual answer, but this HUD has no mouse-move event so overlay
      // dialogs are awkward here (see ConfigHudWidgetsJs.h).
      var delOverlay = null;
      if (!entry.builtin) {
        delOverlay = mk('Panel', thumb);
        delOverlay.style.width = '100%'; delOverlay.style.height = '100%';
        delOverlay.style.borderRadius = '3px';
        delOverlay.style.backgroundColor = '#c92a2acc';
        delOverlay.visible = false;
        delOverlay.hittest = true;
        var delLbl = lbl(delOverlay, '✕', '#ffffffff', 20);
        delLbl.style.horizontalAlign = 'center'; delLbl.style.verticalAlign = 'center';
        delLbl.style.fontWeight = 'bold';
        delLbl.hittest = false;
        delOverlay.SetPanelEvent('onactivate', function () {
          cmd('mirv_filmmaker world sky delete ' + entry.name);
          skyPendingDelete = '';
        });

        t.SetPanelEvent('oncontextmenu', function () {
          skyPendingDelete = (skyPendingDelete === entry.name) ? '' : entry.name;
          skyPaintDelete();
        });
      }

      skyTiles[entry.name] = { panel: t, tick: tick, cap: cap, del: delOverlay };
      return t;
    }

    // Which tile is armed for deletion, if any.
    var skyPendingDelete = '';
    function skyPaintDelete() {
      for (var n in skyTiles) {
        var d = skyTiles[n].del;
        if (d) d.visible = (n === skyPendingDelete);
      }
      skyDeleteHint.visible = skyPendingDelete.length > 0;
      skyDeleteHint.text = skyPendingDelete
        ? ('Click the ✕ to delete “' + skyPendingDelete + '”, or right-click again to cancel.')
        : '';
    }

    // Placeholder for a skybox with no thumbnail: first letter on a flat fill. Better than an
    // empty box, and unlike a generic icon it still tells the tiles apart.
    function skyMonogram(host, label) {
      var m = lbl(host, (label || '?').substring(0, 1).toUpperCase(), S.dim, 20);
      m.style.horizontalAlign = 'center';
      m.style.verticalAlign = 'center';
      m.hittest = false;
      return m;
    }

    function skyFillGrid(host, list) {
      var rowPanel = null;
      for (var i = 0; i < list.length; i++) {
        if (i % 3 === 0) {
          rowPanel = mk('Panel', host);
          rowPanel.style.flowChildren = 'right';
          rowPanel.style.width = '100%';
          rowPanel.style.marginBottom = '5px';
        }
        skyTile(rowPanel, list[i], (i % 3) === 2);
      }
      // Pad a short last row so its tiles keep the same width as the rows above.
      for (var pad = list.length % 3; pad && pad < 3; pad++) {
        var filler = mk('Panel', rowPanel); filler.hittest = false;
        filler.style.width = 'fill-parent-flow(1.0)'; filler.style.marginRight = pad === 2 ? '0px' : '5px';
      }
    }

    function buildSkyGrids(list) {
      skyTiles = {};
      skyCustomHost.RemoveAndDeleteChildren();
      skyStockHost.RemoveAndDeleteChildren();

      var custom = [], builtin = [];
      for (var i = 0; i < list.length; i++) {
        (list[i].builtin ? builtin : custom).push(list[i]);
      }

      if (custom.length) {
        skyFillGrid(skyCustomHost, custom);
        var hint = lbl(skyCustomHost, 'Right-click a skybox to delete it.', S.muted, 11);
        hint.style.marginTop = '2px';
      } else {
        var empty = lbl(skyCustomHost,
          'Pick any image or HDRI and it becomes a skybox. A 2:1 equirectangular panorama '
          + 'works best.', S.label, 11);
        empty.style.whiteSpace = 'normal';
      }

      skyFillGrid(skyStockHost, builtin);
      applySkyTabs();
    }

    // Build progress / failure, shared by both libraries.
    var skyBuildNote = lbl(skySec, '', S.accent, 12);
    skyBuildNote.style.whiteSpace = 'normal'; skyBuildNote.style.marginTop = '4px';

)CFJS"
R"CFJS(
    // ===================== SKY ORIENTATION ==============================
    // Promoted out of the SKYBOX card's old SETTINGS page. Orientation is applied by one canonical
    // quaternion shared by SkyOrient, the GPU sky draw, cubemap fog, and smoke, and it always
    // targets whatever sky is active -- so it is a peer of the picker, not a page inside it. The
    // visible sun decal is deliberately separate and follows Sun: sky yaw/pitch belong here, the
    // physical light direction belongs to LIGHTING > SUN.
    //
    // These are draw-time controls: they update on every drag event, do not rebuild the cubemap,
    // and never issue spectator/camera commands.
    var orientSec = wsSec.__page('Orientation');

    var skySettingsHint = lbl(orientSec,
      'Place the camera where you want it, then drag. This never moves the camera.',
      S.hint, 12);
    skySettingsHint.style.marginTop = '0px'; skySettingsHint.style.whiteSpace = 'normal';

    var skyYawSlider = sliderRow(orientSec, {
      label: 'Rotate', cmd: 'mirv_filmmaker world skyorient yaw',
      min: 0, max: 360, decimals: 0, unit: '°',
      tip: 'Spins the active skybox around the sky like a disc on a table: the horizon stays level.'
    });
    var skyPitchSlider = sliderRow(orientSec, {
      label: 'Tilt', cmd: 'mirv_filmmaker world skyorient pitch',
      min: -180, max: 180, decimals: 0, unit: '°',
      tip: 'Tilts the active skybox up or down while you drag.'
    });

    // Mirroring belongs WITH yaw and tilt: it is a third way to orient the same sky, not a
    // question about what orientation reaches.
    var skyMirrorUi = false;
    var skyMirrorToggle = togglePill(orientSec, 'Flip left / right',
      'Mirrors the active skybox horizontally without rebuilding it.', function () {
        skyMirrorUi = !skyMirrorUi;
        skyMirrorToggle.set(skyMirrorUi);
        // '1' = on. This used to send `skyMirrorUi ? '0' : '1'` -- the value INVERTED against the
        // backend, verified over netcon 2026-08-05 (`skyorient mirror 1` -> "Sky mirror on").
        // The next state snapshot then flipped the pill straight back, so it could not be worked
        // at all. Same defect, same fix, on all three of these switches.
        cmd('mirv_filmmaker world skyorient mirror ' + (skyMirrorUi ? '1' : '0') + ' quiet');
      });

    // WHAT THE TRANSFORMS ABOVE REACH -- one long segmented control, not two on/off pills.
    //
    // The pair of pills was unusable: each one sent the inverse of what it had just set locally,
    // so the backend came back disagreeing and the render hook flipped the pill home again. It was
    // also the wrong SHAPE for the question -- two independent switches offer a fourth state
    // ("neither") in which yaw and tilt silently do nothing at all, which nothing in the panel
    // could explain. Three named choices cover every state worth having and cannot express the
    // useless one.
    subHead(orientSec, 'APPLIES TO');

    var skyTargetUi = 'both';
    var skyTargetSeg = segmented(orientSec,
      [['sky', 'Skybox'], ['fog', 'Fog / smoke'], ['both', 'Both']], '100%',
      function (key) {
        // Set locally first: render-state polling is throttled, so two quick picks must not both
        // send against the same stale snapshot.
        skyTargetUi = key;
        skyTargetSeg.sync(key);
        cmd('mirv_filmmaker world skyorient target sky ' + (key === 'fog' ? '0' : '1') + ' quiet');
        cmd('mirv_filmmaker world skyorient target fog ' + (key === 'sky' ? '0' : '1') + ' quiet');
      }, 14);
    skyTargetSeg.panel.style.marginTop = '6px';

    var skyOrientResetBtn = actionBtn(actionRowEnd(orientSec, 12), 'Reset', function () {
      cmd('mirv_filmmaker world skyorient reset');
    });

    // Speaks only when orientation cannot work -- no hook on this CS2 build, or no sky entity yet.
    // The healthy-state sentence it used to print ("Found 1 sky and 2 fog entities. Use the toggles
    // above to choose which receives yaw and tilt.") explained the toggles that are now captioned
    // APPLIES TO directly above it.
    var skyOrientStatusLbl = lbl(orientSec, '', S.danger, 12);
    skyOrientStatusLbl.style.marginTop = '9px';
    skyOrientStatusLbl.style.whiteSpace = 'normal';
    skyOrientStatusLbl.visible = false;
)CFJS"
R"CFJS(
    // ===================== SKY TINT =====================================
    // Drives `mirv_sky color`, which tints the sky dome itself (env_sky) -- distinct from the
    // cloud tint below, which is a material override on the cloud layer.
    var skyTintSec = wsSec.__page('Sky tint');

    var SKY_TINTS = [
      ['default', '#00000000', ''],
      ['warm',    '#ffb277ff', '255 178 119'],
      ['gold',    '#ffd27aff', '255 210 122'],
      ['rose',    '#ff9aa8ff', '255 154 168'],
      ['cool',    '#9ec6ffff', '158 198 255'],
      ['teal',    '#7fd8d0ff', '127 216 208'],
      ['violet',  '#b79affff', '183 154 255'],
      ['ember',   '#ff8a5cff', '255 138 92'],
      ['ash',     '#9aa3adff', '154 163 173']
    ];
    var skyTintBrightness = 255;

    // 255 = untouched. `mirv_sky color` takes the brightness as its alpha slot.
    function skySendTint(rgb) {
      if (!rgb || !rgb.length) { cmd('mirv_sky color default'); return; }
      cmd('mirv_sky color ' + rgb + ' ' + Math.round(skyTintBrightness));
    }

    var skyTintActive = 'default';
    var skyTintCustom = [255, 200, 150];
    var skyTintPicker = null;

    var skyTintChips = swatchGrid(skyTintSec, SKY_TINTS, function (nameKey, rgb) {
      skyTintActive = nameKey;
      if (skyTintPicker) skyTintPicker.hide();
      skySendTint(rgb);
    }, {
      custom: true,
      onCustom: function () {
        // Open the picker and apply what it already holds, so clicking Custom does something
        // immediately rather than only after a slider moves.
        skyTintActive = 'custom';
        if (skyTintPicker) skyTintPicker.show();
      }
    });

    skyTintPicker = colorPicker(skyTintSec, 'CUSTOM SKY TINT', skyTintCustom, function (c) {
      skyTintChips.paintCustom(c);
      cmd('mirv_sky color ' + c[0] + ' ' + c[1] + ' ' + c[2] + ' ' + Math.round(skyTintBrightness));
    });

    var skyBrightSlider = sliderRow(skyTintSec, {
      label: 'Sky brightness',
      min: 0, max: 255, decimals: 0, unit: '',
      tip: 'Dims or lifts the whole sky dome. Only applies while a tint other than Default is '
         + 'chosen, since it rides along with the tint command.',
      onChange: function (v) {
        skyTintBrightness = v;
        if (skyTintActive === 'custom') {
          var c = skyTintCustom;
          cmd('mirv_sky color ' + c[0] + ' ' + c[1] + ' ' + c[2] + ' ' + Math.round(v));
          return;
        }
        for (var i = 0; i < SKY_TINTS.length; i++) {
          if (SKY_TINTS[i][0] === skyTintActive) { skySendTint(SKY_TINTS[i][2]); break; }
        }
      }
    });

    // ===================== CLOUDS =======================================
    var cloudSec = wsSec.__page('Clouds');
    var cloudsHidden = false;
    var cloudDensityBeforeHide = 1;

    var CLOUD_TINTS = [
      ['default', '#00000000', ''],
      ['ember',   '#e6663cff', '230 102 60'],
      ['gold',    '#f0c060ff', '240 192 96'],
      ['rose',    '#ef8fa6ff', '239 143 166'],
      ['storm',   '#63707eff', '99 112 126'],
      ['ink',     '#2e3742ff', '46 55 66'],
      ['cool',    '#8fb6e6ff', '143 182 230'],
      ['violet',  '#b79affff', '183 154 255'],
      ['white',   '#ffffffff', '255 255 255']
    ];
    var cloudCustom = [200, 120, 90];
    var cloudPicker = null;
    // Matches whichever cloud materials the map loaded, so it needs no per-map table -- the
    // header's material count is the live proof of that, which is why the sentence that used to
    // say it here is gone.
    subHead(cloudSec, 'COLOUR').style.marginTop = '0px';
    var cloudChips = swatchGrid(cloudSec, CLOUD_TINTS, function (nameKey, rgb) {
      if (cloudPicker) cloudPicker.hide();
      cmd(rgb ? ('mirv_sky clouds color ' + rgb) : 'mirv_sky clouds color default');
    }, {
      custom: true,
      onCustom: function () { if (cloudPicker) cloudPicker.show(); }
    });
    cloudPicker = colorPicker(cloudSec, 'CUSTOM CLOUD COLOUR', cloudCustom, function (c) {
      cloudChips.paintCustom(c);
      cmd('mirv_sky clouds color ' + c[0] + ' ' + c[1] + ' ' + c[2]);
    });

    subHead(cloudSec, 'SHAPE');
    // Shape controls. These are NOT a texture swap -- CS2 draws clouds with csgo_effects.vfx, so
    // opacity, mask scale and pan speed are real shader parameters. Every one is a MULTIPLIER on
    // whatever the current map authored, so 1.00 means untouched on every map.
    //
    // DENSITY AND SIZE STAY OUT; DRIFT AND BRIGHTNESS GO BEHIND THE DISCLOSURE.
    //
    // Density alone was the wrong pick. It multiplies cloud OPACITY, which saturates: measured on
    // de_nuke over netcon, 1.00 and 3.00 both read as solid cover, so the top two thirds of its
    // travel did nothing visible. Its range now stops at 2.00 so the part that does something
    // occupies most of the slider. Cloud SIZE is the control that actually restyles the sky --
    // sparse and towering versus small and busy -- so it belongs next to density, not hidden.
    var cloudDensity = sliderRow(cloudSec, {
      label: 'Density', cmd: 'mirv_filmmaker world clouds density',
      min: 0, max: 2, decimals: 2, unit: 'x', offAtZero: true,
      tip: 'Thins or thickens the cloud cover. 0 clears the sky completely, 1.00 is the map as '
         + 'authored. It multiplies opacity, so above about 1.5 the cover is already solid.'
    });
    var cloudSize = sliderRow(cloudSec, {
      label: 'Cloud size', cmd: 'mirv_filmmaker world clouds size',
      min: 0.1, max: 4, decimals: 2, unit: 'x',
      tip: 'Scales the cloud pattern. Below 1 gives fewer, bigger, sparser formations; above 1 '
         + 'gives smaller, busier ones. This is the one that restyles the sky.'
    });

    // No More/Less disclosure: four cloud controls is not a long enough list to be worth hiding
    // half of behind a button, and a control you have to go looking for reads as a missing one.
    var cloudDrift = sliderRow(cloudSec, {
      label: 'Drift speed', cmd: 'mirv_filmmaker world clouds drift',
      min: 0, max: 8, decimals: 2, unit: 'x', offAtZero: true,
      tip: 'How fast the cloud layers scroll. 0 freezes them dead still, which is what you want '
         + 'for a locked-off shot.'
    });
    var cloudBright = sliderRow(cloudSec, {
      label: 'Cloud brightness', cmd: 'mirv_filmmaker world clouds brightness',
      min: 0, max: 3, decimals: 2, unit: 'x',
      tip: 'Dims or lifts the clouds without touching their colour.'
    });

    var cloudActions = actionRowEnd(cloudSec, 10);
    var cloudDrawBtn = actionBtn(cloudActions, 'Hide clouds', function () {
      cloudsHidden = !cloudsHidden;
      // Do not reject the complete cloud material draw: some maps pack their visible sun into
      // that same material. Driving the cloud opacity parameter keeps the independent sun draw.
      cmd('mirv_filmmaker world clouds density ' + (cloudsHidden ? '0' : cloudDensityBeforeHide) + ' quiet');
      setBtnOn(cloudDrawBtn, cloudsHidden, S.accent);
    });
    cloudDrawBtn.style.marginRight = '6px';
    var cloudResetBtn = actionBtn(cloudActions, 'Reset', function () {
      cmd('mirv_filmmaker world clouds reset');
      cmd('mirv_sky clouds color default');
    });

    // "No cloud materials on this map" is a real answer and stays -- four controls that appear to
    // do nothing need explaining. The healthy count moved to the card header.
    var cloudCountLbl = lbl(cloudSec, '', S.hint, 12);
    cloudCountLbl.style.marginTop = '9px'; cloudCountLbl.style.whiteSpace = 'normal';
    cloudCountLbl.visible = false;


    // The swatch grid and the colour picker are the SHARED widgets from ConfigHudWidgetsJs.h.
    // This file used to carry its own copy of both, already diverged from the smoke tab's copy.

    // Local half of the footer's RESET (ConfigHudEffectsResetJs.h). The console commands own the
    // real state; what is listed here is only what lives in JS and therefore never mirrors back:
    // sky brightness (it rides along with the tint command rather than being reported), the
    // hide-clouds latch, an open picker, and an armed skybox delete.
    fxResetLocal.sky = function () {
      skyTintActive = 'default';
      skyTintBrightness = 255;
      skyBrightSlider.sync(255);
      skyTintChips.select('default');
      cloudChips.select('default');
      if (skyTintPicker) skyTintPicker.hide();
      if (cloudPicker) cloudPicker.hide();
      cloudsHidden = false;
      cloudDensityBeforeHide = 1;
      setBtnOn(cloudDrawBtn, false, S.accent);
      skyPendingDelete = '';
      skyPaintDelete();
    };

    renderHooks.push(function (st) {
      var world=st.environment&&st.environment.world;
      if(world) {
        var c=world.skyColor, key=world.skyTint[0]?'custom':'default';
        if(world.skyTint[0])for(var j=1;j<SKY_TINTS.length;j++)if(SKY_TINTS[j][2]===c.slice(0,3).join(' ')){key=SKY_TINTS[j][0];break;}
        skyTintActive=key;skyTintBrightness=c[3];skyBrightSlider.sync(c[3]);skyTintChips.select(key);
        // These two synced unconditionally on EVERY frame, and each buffer is the open picker's
        // own rgbRef -- so a drag was overwritten continuously by the lagged round-trip. Hold
        // the buffer while its picker is up, exactly as colorField does.
        if(!skyTintPicker||!skyTintPicker.visible()){
          for(var j=0;j<3;j++)skyTintCustom[j]=c[j];
          skyTintChips.paintCustom(skyTintCustom);
        }
        c=world.cloudColor;key=world.cloudTint[0]?'custom':'default';
        if(world.cloudTint[0])for(var j=1;j<CLOUD_TINTS.length;j++)if(CLOUD_TINTS[j][2]===c.join(' ')){key=CLOUD_TINTS[j][0];break;}
        if(!cloudPicker||!cloudPicker.visible()){
          for(var j=0;j<3;j++)cloudCustom[j]=c[j];
          cloudChips.paintCustom(cloudCustom);
        }
        cloudChips.select(key);
      }

      var list = (st && st.skyList) ? st.skyList : [];

      var sig = '';
      for (var i = 0; i < list.length; i++) sig += list[i].name + '|' + (list[i].image ? '1' : '0') + ';';
      if (sig !== skySig) {
        skySig = sig;
        // A rebuild throws away the tile panels, so any armed delete must be dropped with them --
        // otherwise the hint survives pointing at a tile that no longer exists.
        skyPendingDelete = '';
        buildSkyGrids(list);
        skyPaintDelete();
      }

      var cur = (st && st.skyCurrent) ? st.skyCurrent : '';
      for (var name in skyTiles) {
        var on = (name === cur);
        skyTiles[name].panel.style.border = '2px solid ' + (on ? S.accent : S.clear);
        skyTiles[name].panel.style.backgroundColor = on ? S.btnOn : S.btnBg;
        skyTiles[name].cap.style.color = on ? S.accent : S.value;
        skyTiles[name].tick.visible = on;
      }
      setBtnOn(skyDefaultBtn, cur === '', S.accent);

      // Orientation is global, so changing skyboxes keeps the values and the render hook applies
      // them to the new sky. The active sky's name is the SKYBOX card's shut-card readout, which
      // is where the old "Active: ..." line inside the settings page went.
      var activeLabel = cur || 'Map default';
      for (var li = 0; li < list.length; li++) {
        if (list[li].name === cur) { activeLabel = list[li].label || cur; break; }
      }
      skySec.__note(activeLabel);

      var yawV = typeof st.skyYaw === 'number' ? st.skyYaw : 0;
      var pitchV = typeof st.skyPitch === 'number' ? st.skyPitch : 0;
      skyYawSlider.sync(yawV);
      skyPitchSlider.sync(pitchV);
      // "Neither" is not one of the three cells. It is reachable from the console, and the honest
      // answer for it is no selection at all rather than a lie about which one is active.
      var tgtSky = st.skyMoveSky !== false, tgtFog = st.skyMoveFog !== false;
      skyTargetUi = (tgtSky && tgtFog) ? 'both' : (tgtSky ? 'sky' : (tgtFog ? 'fog' : ''));
      skyTargetSeg.sync(skyTargetUi);
      skyMirrorUi = !!st.skyMirror;
      skyMirrorToggle.set(skyMirrorUi);

      var orientOk = st.skyOrientOk !== false;
      var skyEnts = (typeof st.skyEnts === 'number') ? st.skyEnts : 0;
      var orientBroken = !orientOk || skyEnts <= 0;
      skyOrientStatusLbl.visible = orientBroken;
      skyOrientStatusLbl.text = !orientOk
        ? 'Live orientation is unavailable on this CS2 build.'
        : 'Waiting for a sky entity; load a map, then try again.';

      orientSec.__note(orientBroken ? 'Unavailable'
        : (yawV === 0 && pitchV === 0 && !st.skyMirror) ? 'Map aligned'
        : (Math.round(yawV) + '° · ' + Math.round(pitchV) + '°' + (st.skyMirror ? ' · flipped' : '')));

      var busy = !!st.skyBuildBusy;
      if (skyCreateBtn) skyCreateBtn.enabled = !busy && (st.skyBuildCan !== false);

      skyBuildNote.text = st.skyBuildMsg || '';
      skyBuildNote.visible = skyBuildNote.text.length > 0;
      skyBuildNote.style.color = (st.skyBuildState === 'failed') ? S.danger : S.accent;

      // Sky tint has no C++ state mirror (it rides the `mirv_sky color` command), so the readout
      // reports what this panel last sent -- which is the only record of it that exists.
      skyTintSec.__note(skyTintActive === 'default'
        ? 'Map default'
        : (skyTintActive + ' · ' + Math.round(skyTintBrightness)));

      // Cloud shape mirrors back from CloudFx so console and panel cannot disagree.
      var cloudDensityValue = typeof st.cloudDensity === 'number' ? st.cloudDensity : 1;
      cloudDensity.sync(cloudDensityValue);
      if (cloudDensityValue > 0) cloudDensityBeforeHide = cloudDensityValue;
      cloudSize.sync(typeof st.cloudSize === 'number' ? st.cloudSize : 1);
      cloudDrift.sync(typeof st.cloudDrift === 'number' ? st.cloudDrift : 1);
      cloudBright.sync(typeof st.cloudBright === 'number' ? st.cloudBright : 1);

      // A map with no cloud materials is a real answer, not a failure -- say so rather than
      // leaving four sliders that appear to do nothing unexplained. On a healthy map the count is
      // a header readout, not a sentence.
      var mats = (typeof st.cloudMats === 'number') ? st.cloudMats : 0;
      cloudCountLbl.visible = (mats <= 0);
      cloudCountLbl.text = 'No cloud materials found yet on this map — move a slider to scan.';

      cloudSec.__note(mats <= 0 ? 'No clouds here'
        : (cloudsHidden || cloudDensityValue <= 0) ? 'Hidden'
        : (cloudDensityValue.toFixed(2) + 'x · ' + mats + ' mat' + (mats === 1 ? '' : 's')));

      // The CARD's summary. The four page-level notes above are no-ops now (a paged card has one
      // readout, not four), so this is the line that answers "what sky am I on?" from the column:
      // the active sky, how it is turned, and how thick the clouds are.
      wsSec.__note(activeLabel
        + ' · ' + Math.round(yawV) + '°'
        + (st.skyMirror ? ' flipped' : '')
        + ' · clouds ' + (mats <= 0 ? 'none' : cloudDensityValue.toFixed(2) + 'x'));
    });
)CFJS"
