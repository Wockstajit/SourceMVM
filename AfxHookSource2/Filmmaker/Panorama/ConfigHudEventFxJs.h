#pragma once

// EVENT FX card in CONFIG > ENVIRONMENT, beside Scene Objects (EventFx.h): CS2's own particle
// effects -- explosions, fire, blood, fireworks -- spawned where a kill, a headshot, a grenade, the
// bomb or a timeline marker happens. The presets are a grid of tiles; the selected one's settings
// sit in two- and three-column rows so the whole card shows without scrolling. Place in world puts
// a preset's effects at the crosshair as Scene Objects that play on the same event, with an icon in
// the view (ConfigHudPropsGizmoJs.h) and a move gizmo like any other object.

R"CFJS(
    var EV_ACCENT = '#ffb86b';
    var evSec = section(pageEnvironment, T('EVENT FX'), true, false, 'env.eventfx', EV_ACCENT + 'ff');
    var evReset = evSec.__headerReset(['mirv_filmmaker eventfx reset'], 'env.eventfx');
    var evPill = evSec.__headerPill(function () {
      cmd('mirv_filmmaker eventfx toggle');
    }, T('Spawns CS2 effects (explosions, fire, blood, sparks, fireworks) where a kill, a grenade, the bomb or a timeline marker happens.'));

    function evCmd(t) { cmd('mirv_filmmaker eventfx ' + t); }
    function evState() { return (st && st.eventFx) ? st.eventFx : null; }
    function evSelected() {
      var es = evState();
      if (!es || !es.presets) return null;
      for (var i = 0; i < es.presets.length; i++) if (es.presets[i].id === es.selected) return es.presets[i];
      return null;
    }

    var EV_TRIGGERS = [
      ['kill', T('Kill')], ['headshot', T('Headshot')],
      ['flashbang', T('Flashbang')], ['hegrenade', T('HE Grenade')],
      ['smoke', T('Smoke grenade')], ['molotov', T('Molotov')],
      ['bomb', T('Bomb explodes')], ['marker', T('Timeline marker')]
    ];
    function evTriggerName(k) {
      for (var i = 0; i < EV_TRIGGERS.length; i++) if (EV_TRIGGERS[i][0] === k) return EV_TRIGGERS[i][1];
      return k;
    }
    // The effect catalog (kEffects in EventFxMath.h): key, label. Same order and keys.
    var EV_EFFECTS = [
      ['he', T('HE grenade blast')], ['he_dirt', T('HE dirt blast')], ['he_snow', T('HE snow blast')],
      ['he_indoor', T('HE indoor blast')], ['c4', T('Bomb blast')], ['c4_indoor', T('Bomb indoor blast')],
      ['c4_short', T('Bomb short blast')], ['heavy_charge', T('Heavy charge')],
      ['basic', T('Small explosion')], ['water', T('Water blast')],
      ['molotov', T('Molotov burst')], ['molotov_air', T('Molotov air burst')], ['incendiary', T('Incendiary burst')],
      ['falling_fire', T('Falling fire')], ['fire', T('Fire')], ['fire_large', T('Large fire')],
      ['blood', T('Blood burst')], ['blood_head', T('Headshot blood')], ['blood_mist', T('Blood mist')],
      ['blood_goop', T('Blood goop')], ['flash', T('Flashbang burst')],
      ['smoke_puff', T('Smoke puff')], ['debris', T('Debris')], ['ground_smoke', T('Lingering smoke')],
      ['fireworks', T('Fireworks')], ['fireworks_big', T('Big fireworks')], ['spark_shower', T('Spark shower')]
    ];
    var EV_EFFECT_ITEMS = [['-', T('None')]].concat(EV_EFFECTS);

    // A row of `n` equal cells; each control built into a cell gets a narrower label column.
    function evCells(parent, n, top) {
      var line = mk('Panel', parent); line.hittest = false; line.hittestchildren = true;
      line.style.flowChildren = 'right'; line.style.width = '100%';
      if (top) line.style.marginTop = top + 'px';
      var cells = [];
      for (var i = 0; i < n; i++) {
        var c = mk('Panel', line); c.hittest = false; c.hittestchildren = true;
        c.style.width = 'fill-parent-flow(1.0)'; c.style.flowChildren = 'down';
        if (i) c.style.marginLeft = '8px';
        cells.push(c);
      }
      return cells;
    }
    var EV_CELL_LABEL = 70;
    function evNarrow(handle) {
      if (handle && handle.label) handle.label.style.width = EV_CELL_LABEL + 'px';
      return handle;
    }
    // ---- the preset tiles: two to a row --------------------------------------------------------
    var evTop = mk('Panel', evSec); evTop.hittest = false; evTop.hittestchildren = true;
    evTop.style.width = '100%'; evTop.style.flowChildren = 'right'; evTop.style.marginTop = '2px';
    var evHint = lbl(evTop, T('Pick a preset, choose its event and effects, then Test or Place in world.'), S.muted, 12);
    evHint.style.width = 'fill-parent-flow(1.0)'; evHint.style.verticalAlign = 'center';
    var evAddBtn = btn(evTop, '+ ' + T('New preset'), function () { evCmd('add'); }, EV_ACCENT + 'ff');
    evAddBtn.style.marginRight = '0px';

    var evGrid = mk('Panel', evSec); evGrid.hittest = false; evGrid.hittestchildren = true;
    evGrid.style.width = '100%'; evGrid.style.flowChildren = 'down'; evGrid.style.marginTop = '8px';

    function evPlacedCount(p) {
      var placed = (st && st.props && st.props.placed) || [], n = 0;
      for (var i = 0; i < placed.length; i++) if (placed[i].play && placed[i].group === p.name) n++;
      return n;
    }
    function evEffectCount(p) {
      var n = 0;
      for (var i = 0; i < 3; i++) if (p.effects && p.effects[i]) n++;
      return n;
    }
    function evTile(cell, p, on) {
      var t = mk('Panel', cell); t.hittest = true; t.hittestchildren = true;
      t.style.width = '100%'; t.style.height = '54px'; t.style.flowChildren = 'down';
      t.style.padding = '6px 6px 0px 10px'; t.style.marginBottom = '6px'; t.style.borderRadius = '4px';
      t.style.backgroundColor = on ? '#ffb86b26' : '#ffffff08';
      t.style.border = '1px solid ' + (on ? EV_ACCENT + 'ff' : '#ffffff14');
      var top = mk('Panel', t); top.hittest = false; top.hittestchildren = true;
      top.style.width = '100%'; top.style.flowChildren = 'right';
      var nm = lbl(top, p.name, on ? S.value : S.label, 13); nm.hittest = false; nm.style.fontWeight = 'bold';
      nm.style.width = 'fill-parent-flow(1.0)'; nm.style.whiteSpace = 'nowrap'; nm.style.textOverflow = 'ellipsis';
      nm.style.verticalAlign = 'center';
      var eb = btn(top, p.enabled ? T('On') : T('Off'), function () { evCmd('enable ' + p.id + ' ' + (p.enabled ? 0 : 1)); },
        p.enabled ? EV_ACCENT + 'ff' : S.muted);
      eb.style.marginRight = '0px'; eb.style.paddingTop = '1px'; eb.style.paddingBottom = '1px';
      var bot = mk('Panel', t); bot.hittest = false; bot.hittestchildren = true;
      bot.style.width = '100%'; bot.style.flowChildren = 'right'; bot.style.marginTop = '2px';
      var placed = evPlacedCount(p);
      var info = evTriggerName(p.trigger) + '  ·  ' + TN(evEffectCount(p), '{0} effect', '{0} effects')
        + (placed ? '  ·  ' + T('{0} placed', placed) : '');
      var tg = lbl(bot, info, EV_ACCENT + 'ff', 11); tg.hittest = false;
      tg.style.width = 'fill-parent-flow(1.0)'; tg.style.whiteSpace = 'nowrap'; tg.style.textOverflow = 'ellipsis';
      tg.style.verticalAlign = 'center';
      var tb = btn(bot, '▶ ' + T('Test'), function () { evCmd('test ' + p.id); }, EV_ACCENT + 'ff');
      tb.style.marginRight = '0px'; tb.style.paddingTop = '1px'; tb.style.paddingBottom = '1px';
      t.SetPanelEvent('onactivate', function () { evCmd('select ' + p.id); });
    }

    // ---- the selected preset ------------------------------------------------------------------
    var evInsp = mk('Panel', evSec);
    evInsp.style.width = '100%'; evInsp.style.flowChildren = 'down';
    evInsp.style.marginTop = '4px'; evInsp.style.padding = '8px 8px 10px 8px';
    evInsp.style.backgroundColor = S.drawer; evInsp.style.borderRadius = '4px';
    evInsp.style.border = '1px solid #ffffff14'; evInsp.visible = false;

    var evInspHead = mk('Panel', evInsp); evInspHead.style.width = '100%'; evInspHead.style.flowChildren = 'right';
    var evInspTitle = lbl(evInspHead, '', S.value, 14);
    evInspTitle.style.fontWeight = 'bold'; evInspTitle.style.width = 'fill-parent-flow(1.0)'; evInspTitle.style.verticalAlign = 'center';
    evInspTitle.style.whiteSpace = 'nowrap'; evInspTitle.style.textOverflow = 'ellipsis'; evInspTitle.style.marginLeft = '2px';
    var evTestBtn = btn(evInspHead, '▶ ' + T('Test'), function () {
      var p = evSelected(); if (p) evCmd('test ' + p.id);
    }, EV_ACCENT + 'ff');
    evTestBtn.style.marginRight = '4px';
    attachTip(evTestBtn, T('Plays it now in front of the camera. While the demo is paused, it plays for a moment and comes back.'));
    var evGotoBtn = btn(evInspHead, T('Go to'), function () {
      var p = evSelected(); if (p && p.played) evCmd('goto ' + p.id);
    }, S.label);
    evGotoBtn.style.marginRight = '4px';
    attachTip(evGotoBtn, T('Moves the camera to where this preset last played.'));
    var evPlaceBtn = btn(evInspHead, T('Place in world'), function () {
      var p = evSelected(); if (p) evCmd('place ' + p.id);
    }, S.accent);
    evPlaceBtn.style.marginRight = '4px';
    attachTip(evPlaceBtn, T('Puts these effects at the crosshair as Scene Objects that play on the same event. Move them in ENVIRONMENT > Scene Objects.'));
    var evDelBtn = btn(evInspHead, T('Delete'), function () {
      var p = evSelected(); if (p) evCmd('remove ' + p.id);
    }, '#ff8a8aff');
    evDelBtn.style.marginRight = '0px';

    function evSet(field, v) { var p = evSelected(); if (p) evCmd('set ' + p.id + ' ' + field + ' ' + v); }
    var evRowA = evCells(evInsp, 2, 4);
    var evTrig = evNarrow(dropdownRow(evRowA[0], T('Plays on'), EV_TRIGGERS, function (k) { evSet('trigger', k); }));
    var evWhere = evNarrow(dropdownRow(evRowA[1], T('Place'), [['event', T('At the event')], ['camera', T('In front of camera')]],
      function (k) { evSet('where', k); }));
    attachTip(evWhere.row, T('At the event: where the grenade went off, the player who died, the bomb. A timeline marker has no place, so it plays in front of the camera.'));

    var evFxHead = lbl(evInsp, T('Effects'), S.muted, 11);
    evFxHead.style.fontWeight = 'bold'; evFxHead.style.letterSpacing = '1px'; evFxHead.style.marginTop = '10px';
    evFxHead.style.marginLeft = UI.line.padH + 'px';
    var evFxCells = evCells(evInsp, 3);
    var evFx = [0, 1, 2].map(function (i) {
      return dropdownRow(evFxCells[i], '', EV_EFFECT_ITEMS, function (k) { evSet('effect' + i, k); }, { placeholder: T('None') });
    });

    // Sliders take a full row each: a paired slider's track was too short to drag (user report).
    function evSlider(label, field, min, max, dec, unit, tip, format) {
      return sliderRow(evInsp, {
        label: label, min: min, max: max, decimals: dec || 0, unit: unit || '', tip: tip, format: format,
        onChange: function (v) { evSet(field, v); }
      });
    }
    var evLift = evSlider(T('Height'), 'lift', -100, 300, 0, 'u', T('Raises the effect above the point it plays at.'));
    var evDelay = evSlider(T('Delay'), 'delay', 0, 5, 2, 's', T('Waits this long after the event.'));
    var evCopies = evSlider(T('Copies'), 'copies', 1, 6, 0, '', T('More of the same effect, scattered around the point.'));
    var evSpread = evSlider(T('Spread'), 'spread', 0, 600, 0, 'u', T('How far apart the copies land.'));
    var evLife = evSlider(T('Keep for'), 'life', 0, 30, 1, 's',
      T('How long the effects stay before they are removed. 0 uses each effect\'s own length.'),
      function (v) { return v <= 0 ? T('Auto') : v.toFixed(1) + 's'; });
    var evShake = sliderRow(evInsp, {
      label: T('Camera shake'), min: 0, max: 100, decimals: 0, unit: '%', offAtZero: true,
      tip: T('Shakes the camera, less the farther it is from the effect. 0 is off.'),
      onChange: function (v) { evSet('shake', (v / 100).toFixed(2)); }
    });
    // How the shake moves, in one row under its strength. Auto follows the strength: a light shake
    // swings slowly and briefly, a hard one quickly and for longer (EventFxMath.h ShakeHz).
    var EV_SHAKE_SPEEDS = [['0', T('Auto')], ['4', T('Slow')], ['8', T('Medium')], ['14', T('Fast')], ['20', T('Very fast')]];
    var EV_SHAKE_LENGTHS = [['0', T('Auto')], ['0.35', T('Short')], ['0.8', T('Medium')], ['1.5', T('Long')], ['2.5', T('Very long')]];
    function evNearest(items, v) {
      var best = items[0][0];
      for (var i = 0; i < items.length; i++) if (Math.abs(parseFloat(items[i][0]) - v) < Math.abs(parseFloat(best) - v)) best = items[i][0];
      return best;
    }
    var evRowS = evCells(evInsp, 2);
    var evShakeSpeed = evNarrow(dropdownRow(evRowS[0], T('Speed'), EV_SHAKE_SPEEDS, function (k) { evSet('shakeSpeed', k); }));
    var evShakeLength = evNarrow(dropdownRow(evRowS[1], T('Length'), EV_SHAKE_LENGTHS, function (k) { evSet('shakeLength', k); }));
    attachTip(evShakeSpeed.row, T('How fast the camera swings. Auto: slower for a light shake, quicker for a hard one.'));
    attachTip(evShakeLength.row, T('How long the shake lasts. Auto: shorter for a light shake, longer for a hard one.'));
    var evRowE = evCells(evInsp, 2);
    var evLight = togglePill(evRowE[0], T('Light burst'),
      T('A real CS2 light flashes where the effect plays.'),
      function () { var p = evSelected(); if (p) evSet('lightBurst', p.lightBurst ? 0 : 1); });
    var evLightStyle = evNarrow(dropdownRow(evRowE[1], T('Light colour'), [['0', T('Warm blast')], ['1', T('White flash')]],
      function (k) { evSet('lightFlash', k); }));

    var evListSig = '';
    renderHooks.push(function (st) {
      var es = evState(); if (!es) return;
      evPill.set(!!es.on);
      evReset.setDisabled(!es.on);
      var presets = es.presets || [];
      var armed = 0;
      for (var k = 0; k < presets.length; k++) if (presets[k].enabled) armed++;
      evSec.__note(es.on ? T('{0} armed', armed) : T('Off'));

      var placedSig = ((st.props && st.props.placed) || []).map(function (q) { return q.play ? q.group : ''; }).join(',');
      var sig = es.selected + '|' + placedSig + '|' + presets.map(function (p) {
        return p.id + ':' + p.name + ':' + p.trigger + ':' + (p.enabled ? 1 : 0) + ':' + (p.effects || []).join(',');
      }).join('|');
      if (sig !== evListSig) {
        evListSig = sig;
        evGrid.RemoveAndDeleteChildren();
        for (var i = 0; i < presets.length; i += 2) {
          var cells = evCells(evGrid, 2);
          evTile(cells[0], presets[i], presets[i].id === es.selected);
          if (presets[i + 1]) evTile(cells[1], presets[i + 1], presets[i + 1].id === es.selected);
        }
      }

      var sel = evSelected();
      evInsp.visible = !!sel;
      if (!sel) return;
      evInspTitle.text = sel.name;
      setBtnDisabled(evGotoBtn, !sel.played);
      evTrig.sync(sel.trigger || 'kill');
      evWhere.sync(sel.where || 'event');
      for (var e = 0; e < 3; e++) evFx[e].sync((sel.effects && sel.effects[e]) || '-');
      evLift.sync(sel.lift || 0);
      evCopies.sync(sel.copies || 1);
      evSpread.sync(sel.spread || 0);
      evDelay.sync(sel.delay || 0);
      evLife.sync(sel.life || 0);
      evShake.sync(Math.round((sel.shake || 0) * 100));
      evShakeSpeed.sync(evNearest(EV_SHAKE_SPEEDS, sel.shakeSpeed || 0));
      evShakeLength.sync(evNearest(EV_SHAKE_LENGTHS, sel.shakeLength || 0));
      evLight.set(!!sel.lightBurst);
      evLightStyle.sync(sel.lightFlash ? '1' : '0');
    });
)CFJS"
