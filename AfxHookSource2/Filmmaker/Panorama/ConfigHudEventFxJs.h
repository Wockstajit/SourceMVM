#pragma once

// EVENT FX card in CONFIG > EFFECTS (EventFx.h): CS2's own particle effects -- explosions, fire,
// blood, sparks, fireworks -- spawned where a kill, a headshot, a grenade, the bomb or a timeline
// marker happens. Place in world puts a preset's effects at the crosshair as Scene Objects that
// play on the same event (ENVIRONMENT > Scene Objects moves them).

R"CFJS(
    var EV_ACCENT = '#ffb86b';
    var evSec = section(pageEffects, T('EVENT FX'), true, false, 'fx.eventfx', EV_ACCENT + 'ff');
    var evReset = evSec.__headerReset(['mirv_filmmaker eventfx reset'], 'fx.eventfx');
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

    var evAddBtn = wideBtn(evSec, T('+ New preset'), function () { evCmd('add'); }, EV_ACCENT + 'ff');
    evAddBtn.__bgBase = '#ffb86b18'; evAddBtn.__brBase = '#ffb86b66'; paintBtn(evAddBtn);

    var evListHost = mk('Panel', evSec);
    evListHost.style.width = '100%'; evListHost.style.flowChildren = 'down'; evListHost.style.marginTop = '6px';

    var evInsp = mk('Panel', evSec);
    evInsp.style.width = '100%'; evInsp.style.flowChildren = 'down';
    evInsp.style.marginTop = '10px'; evInsp.style.padding = '10px';
    evInsp.style.backgroundColor = S.drawer; evInsp.style.borderRadius = '4px';
    evInsp.style.border = '1px solid #ffffff14'; evInsp.visible = false;

    var evInspHead = mk('Panel', evInsp); evInspHead.style.width = '100%'; evInspHead.style.flowChildren = 'right';
    var evInspTitle = lbl(evInspHead, '', S.value, 13);
    evInspTitle.style.fontWeight = 'bold'; evInspTitle.style.width = 'fill-parent-flow(1.0)'; evInspTitle.style.verticalAlign = 'center';
    evInspTitle.style.whiteSpace = 'nowrap'; evInspTitle.style.textOverflow = 'ellipsis';
    var evTestBtn = btn(evInspHead, T('Test'), function () {
      var p = evSelected(); if (p) evCmd('test ' + p.id);
    }, EV_ACCENT + 'ff');
    evTestBtn.style.marginRight = '4px';
    attachTip(evTestBtn, T('Plays it now in front of the camera, even while the demo is paused.'));
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
    var evTrig = dropdownRow(evInsp, T('Plays on'), EV_TRIGGERS, function (k) { evSet('trigger', k); });

    subHead(evInsp, T('Effects'));
    var evFx = [0, 1, 2].map(function (i) {
      return dropdownRow(evInsp, T('Effect {0}', i + 1), EV_EFFECT_ITEMS, function (k) { evSet('effect' + i, k); });
    });

    subHead(evInsp, T('Where'));
    var evWhere = modeRow(evInsp, T('Place'), [['event', T('At the event')], ['camera', T('In front of camera')]], '100%',
      function (k) { evSet('where', k); },
      T('At the event: where the grenade went off, the player who died, the bomb. A timeline marker has no place, so it plays in front of the camera.'), 12);
    function evSlider(label, field, min, max, dec, unit, tip) {
      return sliderRow(evInsp, {
        label: label, min: min, max: max, decimals: dec || 0, unit: unit || '', tip: tip,
        onChange: function (v) { evSet(field, v); }
      });
    }
    var evLift = evSlider(T('Height'), 'lift', -100, 300, 0, 'u', T('Raises the effect above the point it plays at.'));
    var evCopies = evSlider(T('Copies'), 'copies', 1, 6, 0, '', T('More of the same effect, scattered around the point.'));
    var evSpread = evSlider(T('Spread'), 'spread', 0, 600, 0, 'u', T('How far apart the copies land.'));
    var evDelay = evSlider(T('Delay'), 'delay', 0, 5, 2, 's', T('Waits this long after the event.'));
    var evLife = sliderRow(evInsp, {
      label: T('Keep for'), min: 0, max: 30, decimals: 1, unit: 's',
      tip: T('How long the effects stay before they are removed. 0 uses each effect\'s own length.'),
      format: function (v) { return v <= 0 ? T('Auto') : v.toFixed(1) + 's'; },
      onChange: function (v) { evSet('life', v); }
    });

    subHead(evInsp, T('Light and camera'));
    var evLight = togglePill(evInsp, T('Light burst'),
      T('A real CS2 light flashes where the effect plays.'),
      function () { var p = evSelected(); if (p) evSet('lightBurst', p.lightBurst ? 0 : 1); });
    var evLightStyle = modeRow(evInsp, T('Light colour'), [['0', T('Warm blast')], ['1', T('White flash')]], '100%',
      function (k) { evSet('lightFlash', k); }, '', 12);
    var evShake = sliderRow(evInsp, {
      label: T('Camera shake'), min: 0, max: 100, decimals: 0, unit: '%',
      tip: T('Shakes the camera, less the farther it is from the effect. 0 is off.'),
      onChange: function (v) { evSet('shake', (v / 100).toFixed(2)); }
    });

    var evListSig = '';
    renderHooks.push(function (st) {
      var es = evState(); if (!es) return;
      evPill.set(!!es.on);
      evReset.setDisabled(!es.on);
      var presets = es.presets || [];
      var armed = 0;
      for (var k = 0; k < presets.length; k++) if (presets[k].enabled) armed++;
      evSec.__note(es.on ? T('{0} armed', armed) : T('Off'));

      var sig = es.selected + '|' + presets.map(function (p) { return p.id + ':' + p.name + ':' + p.trigger + ':' + (p.enabled ? 1 : 0); }).join('|');
      if (sig !== evListSig) {
        evListSig = sig;
        evListHost.RemoveAndDeleteChildren();
        for (var i = 0; i < presets.length; i++) (function (p) {
          var on = p.id === es.selected;
          var r = mk('Panel', evListHost); r.hittest = true;
          r.style.width = '100%'; r.style.height = '30px'; r.style.flowChildren = 'right';
          r.style.paddingLeft = '8px'; r.style.paddingRight = '6px'; r.style.marginBottom = '3px';
          r.style.borderRadius = '3px';
          r.style.backgroundColor = on ? '#ffb86b28' : '#ffffff08';
          r.style.border = '1px solid ' + (on ? EV_ACCENT + 'ff' : '#ffffff12');
          var nm = lbl(r, p.name, on ? S.value : S.label, 12); nm.hittest = false;
          nm.style.width = 'fill-parent-flow(1.0)'; nm.style.verticalAlign = 'center';
          nm.style.whiteSpace = 'nowrap'; nm.style.textOverflow = 'ellipsis';
          var tg = lbl(r, evTriggerName(p.trigger), EV_ACCENT + 'ff', 11); tg.hittest = false;
          tg.style.verticalAlign = 'center'; tg.style.marginLeft = '8px'; tg.style.marginRight = '8px'; tg.style.whiteSpace = 'nowrap';
          var tb = btn(r, T('Test'), function () { evCmd('test ' + p.id); }, EV_ACCENT + 'ff');
          tb.style.marginRight = '4px';
          var eb = btn(r, p.enabled ? T('On') : T('Off'), function () { evCmd('enable ' + p.id + ' ' + (p.enabled ? 0 : 1)); }, p.enabled ? EV_ACCENT + 'ff' : S.muted);
          eb.style.marginRight = '0px';
          r.SetPanelEvent('onactivate', function () { evCmd('select ' + p.id); });
        })(presets[i]);
      }

      var sel = evSelected();
      evInsp.visible = !!sel;
      if (!sel) return;
      evInspTitle.text = sel.name;
      evTrig.sync(sel.trigger || 'kill');
      for (var e = 0; e < 3; e++) evFx[e].sync((sel.effects && sel.effects[e]) || '-');
      evWhere.sync(sel.where || 'event');
      evLift.sync(sel.lift || 0);
      evCopies.sync(sel.copies || 1);
      evSpread.sync(sel.spread || 0);
      evDelay.sync(sel.delay || 0);
      evLife.sync(sel.life || 0);
      evLight.set(!!sel.lightBurst);
      evLightStyle.sync(sel.lightFlash ? '1' : '0');
      evShake.sync(Math.round((sel.shake || 0) * 100));
    });
)CFJS"
