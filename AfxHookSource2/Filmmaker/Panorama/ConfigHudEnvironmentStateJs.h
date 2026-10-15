#pragma once

// ENVIRONMENT tab: presets, diagnostics, the tab footer, and the one render hook that mirrors the
// whole `environment` backend back into the panel. JS fragment concatenated (via #include, as an
// adjacent raw string literal) into the kConfigHudJs body assembled in ConfigHudJs.h. Included
// from the bottom of ConfigHudEnvironmentTabJs.h, so the two pages it adds land AFTER List and
// Selected on the SCENE OBJECTS card, and the footer lands under every card on the tab.
//
// The climate values sync in ConfigHudEnvironmentNavJs.h and the card summaries in
// ConfigHudClimateJs.h; everything that is not a climate value syncs here.

R"CFJS(
    // ===================== SCENE OBJECTS: the preset button ======================
    // The header's right side: the scene preset's name (a dot while it has unsaved changes) and a
    // menu under it -- name + Save, then one row per saved preset with Load. A preset stores the
    // complete environment; placed objects belong to the map they were placed on. The menu floats
    // on the dropdown layer, so a press outside it closes it like any other popover.
    var envName = '', envNameFocus = false, envPresetMenu = null;
    var envPresetBtn = mk('Panel', envSceneSec.__headerHost());
    envPresetBtn.hittest = true; envPresetBtn.style.flowChildren = 'right'; envPresetBtn.style.verticalAlign = 'center';
    envPresetBtn.style.padding = '5px 10px'; envPresetBtn.style.border = '1px solid #ffffff1f';
    envPresetBtn.style.borderRadius = '3px'; envPresetBtn.style.maxWidth = '150px'; envPresetBtn.style.marginLeft = '8px';
    var envPresetDot = mk('Panel', envPresetBtn); envPresetDot.hittest = false;
    envPresetDot.style.width = '6px'; envPresetDot.style.height = '6px'; envPresetDot.style.borderRadius = '3px';
    envPresetDot.style.backgroundColor = '#e0b861ff'; envPresetDot.style.verticalAlign = 'center'; envPresetDot.style.marginRight = '6px';
    var envPresetCap = lbl(envPresetBtn, 'Preset', '#c8d3dfff', 12); envPresetCap.hittest = false;
    envPresetCap.style.fontWeight = 'bold'; envPresetCap.style.whiteSpace = 'nowrap'; envPresetCap.style.textOverflow = 'ellipsis';
    envPresetCap.style.maxWidth = '110px'; envPresetCap.style.verticalAlign = 'center';
    var envPresetCaret = lbl(envPresetBtn, ' ▾', S.muted, 11); envPresetCaret.hittest = false; envPresetCaret.style.verticalAlign = 'center';
    function envPresetBtnPaint(hot) {
      var on = hot || !!envPresetMenu;
      envPresetBtn.style.backgroundColor = on ? '#ffffff1f' : S.clear;
      envPresetBtn.style.border = '1px solid ' + (on ? '#ffffff3d' : '#ffffff1f');
    }
    envPresetBtn.SetPanelEvent('onmouseover', function () { envPresetBtnPaint(true); });
    envPresetBtn.SetPanelEvent('onmouseout', function () { envPresetBtnPaint(false); });
    envPresetBtn.SetPanelEvent('onactivate', function () { if (envPresetMenu) cfgDropdownClose(); else envPresetOpen(); });
    attachTip(envPresetBtn, 'Scene presets: save the whole environment under a name, or load one.');

    var envNameField = null, envNameLabel = null;
    function envNamePaint() {
      if (!envNameLabel || !cfgValid(envNameLabel)) return;
      envNameLabel.text = (envName + (envNameFocus ? ' |' : '')) || 'Name this scene';
      envNameLabel.style.color = envName ? S.value : S.muted;
      envNameField.style.border = '1px solid ' + (envNameFocus ? ENV_ACCENT : '#ffffff14');
    }
    var envPreviousBlur = configWindowBlur, envPreviousChars = configWindowChars;
    configWindowBlur = function () { envNameFocus = false; envNamePaint(); envPreviousBlur(); };
    configWindowChars = function (csv) {
      if (!envNameFocus) { envPreviousChars(csv); return; }
      String(csv || '').split(',').forEach(function (part) {
        var n = parseInt(part, 10);
        if (n === 13) { configWindowBlur(); envPresetSave(); }
        else if (n === 27) configWindowBlur();
        else if (n === 8) envName = envName.slice(0, -1);
        else if (envName.length < 48 && ((n >= 48 && n <= 57) || (n >= 65 && n <= 90)
                 || (n >= 97 && n <= 122) || n === 32 || n === 45 || n === 95)) {
          envName += String.fromCharCode(n);
        }
      });
      envNamePaint();
    };
    function envPresetSave() { if (envName) { configWindowBlur(); envCommand('save "' + envName + '"'); } }
    function envPresetOpen() {
      cfgDropdownClose();
      if (!envName && st && st.envSaved) envName = st.envSaved;
      var m = mk('Panel', cfgDropLayer()); m.hittest = true; m.hittestchildren = true;
      m.style.width = '260px'; m.style.flowChildren = 'down'; m.style.padding = '10px';
      m.style.backgroundColor = '#14181eff'; m.style.border = '1px solid #ffffff1f'; m.style.borderRadius = '4px';
      m.style.boxShadow = '#00000099 0px 6px 18px 0px';
      m.SetPanelEvent('onactivate', function () {});
      var t = lbl(m, 'SCENE PRESET', S.muted, 11); t.style.fontWeight = 'bold'; t.style.letterSpacing = '1.5px';
      var row = mk('Panel', m); row.style.width = '100%'; row.style.flowChildren = 'right'; row.style.marginTop = '8px';
      envNameField = mk('Panel', row); envNameField.hittest = true;
      envNameField.style.width = 'fill-parent-flow(1.0)'; envNameField.style.height = '30px';
      envNameField.style.backgroundColor = '#0c0e12ff'; envNameField.style.borderRadius = '3px';
      envNameLabel = lbl(envNameField, '', S.value, 13); envNameLabel.hittest = false;
      envNameLabel.style.marginLeft = '8px'; envNameLabel.style.verticalAlign = 'center';
      envNameField.SetPanelEvent('onactivate', function () {
        configWindowBlur(); envNameFocus = true; cfgTextOwner = envNameField;
        root.SetAttributeString('textcapture', '1'); envNamePaint();
      });
      var save = btn(row, 'Save', envPresetSave, S.accent);
      save.style.marginRight = '0px'; save.style.marginLeft = '6px';
      save.__bgBase = '#80bdff14'; save.__brBase = '#80bdff66'; paintBtn(save);
      envPresetMenuDirty = lbl(m, 'Modified since last save', '#e0b861ff', 11); envPresetMenuDirty.style.marginTop = '6px';
      var hr = mk('Panel', m); hr.style.width = '100%'; hr.style.height = '1px'; hr.style.backgroundColor = '#ffffff12';
      hr.style.marginTop = '8px'; hr.style.marginBottom = '4px';
      ((st && st.envPresets) || []).forEach(function (name) {
        var r = mk('Panel', m); r.hittest = true; r.style.width = '100%'; r.style.height = '30px';
        r.style.flowChildren = 'right'; r.style.padding = '0px 6px'; r.style.borderRadius = '3px';
        var n = lbl(r, name, name === (st && st.envSaved) ? ENV_ACCENT : S.label, 13); n.hittest = false;
        n.style.width = 'fill-parent-flow(1.0)'; n.style.verticalAlign = 'center'; n.style.whiteSpace = 'nowrap'; n.style.textOverflow = 'ellipsis';
        var l = lbl(r, 'Load', S.muted, 11); l.hittest = false; l.style.verticalAlign = 'center';
        r.SetPanelEvent('onmouseover', function () { r.style.backgroundColor = '#212833ff'; });
        r.SetPanelEvent('onmouseout', function () { r.style.backgroundColor = S.clear; });
        r.SetPanelEvent('onactivate', function () { envName = name; envCommand('load "' + name + '"'); cfgDropdownClose(); });
      });
      if (!((st && st.envPresets) || []).length) envNote(m, 'No saved presets yet.');
      var foot = lbl(m, 'Stores the complete environment. Placed objects belong to the map they were placed on.', S.hint, 11);
      foot.style.marginTop = '8px'; foot.style.whiteSpace = 'normal';
      var tools = actionRowEnd(m, 8);
      actionBtn(tools, 'Refresh list', function () { envCommand('rescan'); cfgDropdownClose(); });
      actionBtn(tools, 'Reload map geometry', function () { envCommand('reload-geometry'); });
      envPresetMenu = { field: envPresetBtn, list: m, __menu: true, __lastLmb: true,
        __setOpen: function (open) {
          if (open) return;
          try { m.DeleteAsync(0); } catch (e) {}
          envPresetMenu = null; envNameFocus = false; envNameField = envNameLabel = envPresetMenuDirty = null;
          envPresetBtnPaint(false);
        } };
      cfgDropOpen = envPresetMenu;
      envNamePaint();
      envPresetBtnPaint(false);
      envPresetPlace();
    }
    var envPresetMenuDirty = null;
    // Right-aligned under the button, 40px below the header's top.
    function envPresetPlace() {
      if (!envPresetMenu) return;
      try {
        var bp = envPresetBtn.GetPositionWithinWindow(), rp = root.GetPositionWithinWindow();
        var sx = root.actualuiscale_x || 1, sy = root.actualuiscale_y || 1;
        var right = (bp.x - rp.x + envPresetBtn.actuallayoutwidth) / sx;
        var y = (bp.y - rp.y) / sy + 34;
        envPresetMenu.list.style.position = Math.round(right - 260) + 'px ' + Math.round(y) + 'px 0px';
      } catch (e) {}
    }

    // ===================== SCENE OBJECTS > STATUS =======================
    var envStatusPage = pageEnvironment;
    var envRender = envNote(envStatusPage, '');
    var envMessage = envNote(envStatusPage, '');
    envMessage.style.color = '#e8b339ff';

    // ===================== the tab footer ===============================
    var envFooter = footerCard(pageEnvironment);
    var envKeep = keepToggle(envFooter, 'effects', 'envKeep', 'environment and VFX overrides');
    // Undo is NOT in the footer. The footer is pinned, so anything in it costs column height on
    // every tab for the whole session -- and the original design's footer is the keep row and the tab
    // RESET, nothing else. Undo lives with the objects it steps back through, on SCENE OBJECTS.
    // The scene objects (CS2 assets plus the Environment's lights and meshes) go with a full reset,
    // so the footer asks first whenever there are any.
    var envSceneCount = 0;
    var envResetAll = resetFooter(envFooter, function () {
      return { label: 'ENVIRONMENT', cmds: ['mirv_filmmaker environment reset all',
                 'mirv_relight lighteditor off', 'mirv_relight lights reset', 'mirv_relight off',
                 'mirv_filmmaker props clear', 'mirv_filmmaker props wind 0'],
               confirm: function () {
                 if (!envSceneCount) return null;
                 return { title: 'Reset the environment?',
                          text: 'This also removes your ' + envSceneCount + ' scene object' + (envSceneCount === 1 ? '' : 's')
                            + ' (CS2 assets, lights and meshes). Undo brings back the lights and meshes, not the CS2 assets.',
                          ok: 'Reset everything' };
               },
               local: function () {
                 envName = ''; envNamePaint(); envWeatherMemo = null;
                 for (var r = 0; r < envPassResetters.length; r++) envPassResetters[r](null);
               } };
    }, 'environment');
)CFJS"
R"CFJS(
    var envLastSelected = 0;
    function envObjectValue(o, key) {
      if (key === 'pitch' || key === 'yaw' || key === 'roll') {
        return o.angles[key === 'pitch' ? 0 : key === 'yaw' ? 1 : 2];
      }
      if (key === 'sx' || key === 'sy' || key === 'sz') {
        return o.scale[key === 'sx' ? 0 : key === 'sy' ? 1 : 2];
      }
      if (key === 'r' || key === 'g' || key === 'b') {
        return o.color[key === 'r' ? 0 : key === 'g' ? 1 : 2];
      }
      return o[key];
    }
    var MATERIAL_FIELDS = ['roughness', 'metallic', 'coat', 'coatRoughness', 'anisotropy',
                           'emission', 'normalStrength', 'uvScale'];
    var WEATHER_WORD = ['Clear', 'Rain', 'Snow', 'Ash', 'Embers', 'Motes', 'Smoke'];

    renderHooks.push(function (st) {
      var s = envState();
      if (!s) return;

      // ---- PRECIPITATION -------------------------------------------------------------------
      var ready = !!st.envOcclusionReady;
      envWeatherPill.set(envWeatherOn());
      envWeatherModes.sync(WEATHER_WORD[s.weather.type ? s.weather.type : 0].toLowerCase());
      var wind = s.weather;
      var speed = Math.sqrt(wind.windX * wind.windX + wind.windY * wind.windY);
      if (speed > 0.01) envWindDirection = (Math.atan2(wind.windY, wind.windX) * 180 / Math.PI + 360) % 360;
      envWindSpeedControl.sync(speed); envWindDirectionControl.sync(envWindDirection);
      for (var w = 0; w < envWeatherControls.length; w++) {
        var weatherKey = envWeatherControls[w][1];
        envWeatherControls[w][0].sync(s.weather[weatherKey] / (weatherKey === 'count' ? 60 : 1));
      }
      var cl = s.climate || {};
      var lightning = (cl.enabled && cl.storm)
        ? (cl.stormHold ? 'held strike' : 'lightning every ' + (cl.stormInterval || 9) + 's') : '';
      var surfaces = [];
      if (cl.enabled && cl.wet > 0) surfaces.push('wet ground');
      if (cl.enabled && cl.puddles > 0) surfaces.push('puddles');
      if (cl.enabled && cl.snow > 0) surfaces.push('snow cover');
      var parts = [];
      if (s.weather.type) parts.push(WEATHER_WORD[s.weather.type] + ' · ' + Math.round(s.weather.count / 60) + '%');
      if (surfaces.length) parts.push('ground response');
      if (lightning) parts.push(lightning);
      envWeatherSec.__note(parts.length ? parts.join(' · ') : 'Off');

      // ---- FINISHING > Scene ---------------------------------------------------------------
      for (var g = 0; g < envSceneControls.length; g++) {
        var key = envSceneControls[g][1], v = s[key];
        if (key.indexOf('fog') === 0 && key !== 'fog') {
          v = s.fogColor[key === 'fogR' ? 0 : key === 'fogG' ? 1 : 2];
        }
        if (key.indexOf('tint') === 0) v = s.tint[key === 'tintR' ? 0 : key === 'tintG' ? 1 : 2];
        envSceneControls[g][0].sync(v);
      }

      var KIND_WORD = { point: 'Point', spot: 'Spot', area: 'Area' };
      // The card summary counts what was placed: CS2 assets, your meshes and lights.
      var nMeshes = 0, nLights = 0;
      s.objects.forEach(function (o) {
        if (KIND_WORD[o.kind]) nLights++; else nMeshes++;
      });
      var nAssets = (st.props && st.props.placed) ? st.props.placed.length : 0;
      var summary = [];
      if (nAssets) summary.push(nAssets + ' asset' + (nAssets === 1 ? '' : 's'));
      if (nMeshes) summary.push(nMeshes + ' mesh' + (nMeshes === 1 ? '' : 'es'));
      if (nLights) summary.push(nLights + ' light' + (nLights === 1 ? '' : 's'));
      envSceneSec.__note(summary.length ? summary.join(' · ') : 'Nothing placed');

      // ---- SCENE OBJECTS > Selected --------------------------------------------------------
      var selected = envSelected();
      envLastSelected = st.envSelected || 0;
      ['props', 'lights'].forEach(function (group) {
        var isLight = selected && (selected.kind === 'point' || selected.kind === 'spot'
                                   || selected.kind === 'area');
        var visible = selected && (isLight === (group === 'lights'));
        var p = envInspectors[group];
        p.visible = !!visible;
        if (!visible) return;
        p.__place.__lbl.text = st.envEditing ? 'Done moving' : 'Edit position';
        if (p.__animSection) {
          var clips = (st.envClips && st.envClips[String(selected.id)]) || [];
          p.__animSection.visible = clips.length > 0;
          var animSig = selected.id + '|' + JSON.stringify(clips);
          if (clips.length && animSig !== p.__animSig) {
            p.__animSig = animSig;
            p.__animButtons.RemoveAndDeleteChildren();
            var objectId = selected.id;
            actionGroup(p.__animButtons, 'Clip', [['Rest pose', function () { envCommand('object ' + objectId + ' clip -'); }]]
              .concat(clips.map(function (name) {
                return [name, function () { envCommand('object ' + objectId + ' clip ' + name); }];
              })), 2, 'The animation this mesh plays. Rest pose stands it still in its bind pose.');
          }
          if (clips.length) {
            p.__animNote.text = selected.clip ? 'Playing ' + selected.clip : 'Rest pose (not animated)';
            p.__animSpeed.sync(selected.clip ? (selected.clipSpeed === undefined ? 1 : selected.clipSpeed) : 1);
          }
        }
        if (p.__materialEnabled) {
          p.__materialEnabled.sync(selected.materialEnabled ? '1' : '0');
          p.__cameraVisible.sync(selected.cameraVisible === false ? '0' : '1');
          p.__textureNote.text = 'Color ' + (selected.albedoTexture || 'none')
            + ' · Normal ' + (selected.normalTexture || 'none')
            + ' · Roughness ' + (selected.roughnessTexture || 'none');
          var textureSig = JSON.stringify(st.envTextures || []);
          if (textureSig !== p.__textureSig) {
            p.__textureSig = textureSig;
            p.__textureList.RemoveAndDeleteChildren();
            (st.envTextures || []).forEach(function (name) {
              var tb = wideBtn(p.__textureList, 'Use ' + name, function () {
                var o = envSelected();
                if (o && o.kind !== 'point' && o.kind !== 'spot' && o.kind !== 'area') {
                  envCommand('texture ' + o.id + ' ' + p.__textureChannel + ' "' + name + '"');
                }
              }, S.label);
              tb.style.marginTop = '6px';
            });
          }
        }
        if (p.__shadows) p.__shadows.sync(selected.shadows ? '1' : '0');
        if (p.__studio) {
          p.__studio.forEach(function (c) {
            c[0].sync(String(selected[c[1]] || 0));
            c[2].visible = c[1] === 'cue' || selected.kind !== 'point';
          });
        }
        envObjectControls[group].forEach(function (c) {
          c[0].sync(envObjectValue(selected, c[1]));
          var materialField = MATERIAL_FIELDS.indexOf(c[1]) >= 0;
          setBtnDisabled(c[2], materialField && !selected.materialEnabled);
          c[2].style.opacity = (materialField && !selected.materialEnabled) ? '0.4' : '1';
          c[2].visible = (selected.kind === 'point' && ['pitch', 'yaw', 'roll'].indexOf(c[1]) >= 0) ? false
            : (c[1] === 'width' || c[1] === 'height') ? selected.kind === 'area'
            : (c[1] === 'cone' || c[1] === 'patternScale') ? selected.kind !== 'point'
            : true;
        });
      });

      // Hide an object-control row when both of its conditional controls are hidden.
      ['props', 'lights'].forEach(function (group) {
        (envInspectors[group].__controlRows || []).forEach(function (r) {
          r.visible = r.__hosts.some(function (h) { return h.visible; });
        });
      });

      // ---- the preset button ------------------------------------------------------------
      var dirty = !!st.envDirty;
      envPresetDot.visible = dirty;
      envPresetCap.text = st.envSaved || 'Preset';
      if (envPresetMenuDirty && cfgValid(envPresetMenuDirty)) envPresetMenuDirty.visible = dirty;
      envPresetPlace();

      // ---- SCENE OBJECTS > Status, and the footer -------------------------------------------
      // The still's own status is the only thing its card can report, so it is both the body
      // line and the header summary -- a running job has to be visible on a shut card.
      envMessage.text = st.envMessage || '';
      envMessage.visible = !!envMessage.text;
      envRender.text = (s.map && s.map !== st.envMap ? 'Waiting for map ' + s.map + ' · ' : '')
        + (st.envSaved || 'Unsaved scene') + (st.envDirty ? ' · modified' : '');
      envRender.visible = !!s.enabled;
      envSceneCount = ((st.props && st.props.placed) || []).length + ((s && s.objects) || []).length;
      envResetAll.paint();
      envKeep.sync(st);
    });
)CFJS"
