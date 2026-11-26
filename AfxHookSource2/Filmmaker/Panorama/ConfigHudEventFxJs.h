#pragma once

// EVENT FX PRESETS card in CONFIG > EFFECTS (EventFx.h):
// Turns kills, headshots, grenade pops, bomb blasts, or timeline markers into reusable
// visual cues (exposure pulse + camera shake + FOV kick + native light burst + scene key baking).

R"CFJS(
    var evSec = section(pageEffects, T('EVENT FX PRESETS'), true, false, 'fx.eventfx', '#ffb86bff');
    var evReset = evSec.__headerReset(['mirv_filmmaker eventfx reset'], 'fx.eventfx');
    var evPill = evSec.__headerPill(function () {
      cmd('mirv_filmmaker eventfx toggle');
    }, T('Turns kills, grenade pops, and timeline markers into reusable visual cues (exposure pulse, camera shake, light burst, and scene keyframes).'));

    function evCmd(t) { cmd('mirv_filmmaker eventfx ' + t); }
    function evState() { return (st && st.eventFx) ? st.eventFx : null; }
    function evSelected() {
      var es = evState();
      if (!es || !es.presets) return null;
      for (var i = 0; i < es.presets.length; i++) if (es.presets[i].id === es.selected) return es.presets[i];
      return null;
    }

    var evAddBtn = wideBtn(evSec, T('+ New Event Preset'), function () { evCmd('add'); }, '#ffb86bff');
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
    var evTestBtn = btn(evInspHead, T('Test Fire'), function () {
      var p = evSelected(); if (p) evCmd('test ' + p.id);
    }, '#ffb86bff');
    evTestBtn.style.marginRight = '4px';
    var evBakeBtn = btn(evInspHead, T('Bake Keys'), function () {
      var p = evSelected(); if (p) evCmd('bake ' + p.id);
    }, S.accent);
    evBakeBtn.style.marginRight = '4px';
    attachTip(evBakeBtn, T('Stamps this preset’s exposure pulse into Scene Keyframes at the current playhead tick so you can refine it in the Timeline Graph.'));
    var evDelBtn = btn(evInspHead, T('Del'), function () {
      var p = evSelected(); if (p) evCmd('remove ' + p.id);
    }, '#ff8a8aff');
    evDelBtn.style.marginRight = '0px';

    var EV_TRIGGERS = [
      ['kill', T('Kill')], ['headshot', T('Headshot')],
      ['flashbang', T('Flashbang')], ['hegrenade', T('HE Grenade')],
      ['smoke', T('Smoke Pop')], ['molotov', T('Molotov')],
      ['bomb', T('Bomb Blast')], ['marker', T('Marker')]
    ];
    function evSet(field, v) { var p = evSelected(); if (p) evCmd('set ' + p.id + ' ' + field + ' ' + v); }
    var evTrigGrid = optionGrid(evInsp, T('Trigger event'), EV_TRIGGERS, 4,
      T('Which demo event or timeline marker fires this visual cue.'),
      function (k) { evSet('trigger', k); }, 11);

    function evSlider(label, field, min, max, dec, unit, tip) {
      return sliderRow(evInsp, {
        label: label, min: min, max: max, decimals: dec || 0, unit: unit || '', tip: tip,
        onChange: function (v) { evSet(field, v); }
      });
    }
    subHead(evInsp, T('Exposure & flash pulse'));
    var evExp = evSlider(T('Exposure pulse'), 'exposureEv', -2, 4, 2, ' EV', T('Brief exposure spike (positive flashes bright; negative dips dark).'));
    var evAtk = evSlider(T('Attack'), 'attack', 0.01, 0.5, 2, 's', T('Time to reach peak intensity.'));
    var evDec = evSlider(T('Decay'), 'decay', 0.05, 3.0, 2, 's', T('Time for the cue to fade cleanly back to normal.'));
    var evTint = sliderRow(evInsp, {
      label: T('Flash veil'), min: 0, max: 100, decimals: 0, unit: '%',
      tip: T('Strength of the colour veil blended over the frame at the peak of the pulse.'),
      onChange: function (v) { evSet('tintStrength', (v / 100).toFixed(3)); }
    });

    subHead(evInsp, T('Camera shake & punch'));
    var evShake = evSlider(T('Shake amplitude'), 'shakeAmp', 0, 8, 2, '°', T('Pitch and yaw impulse kick on trigger.'));
    var evRoll = evSlider(T('Roll kick'), 'shakeRoll', 0, 8, 2, '°', T('Camera roll twist on trigger.'));
    var evFreq = evSlider(T('Shake speed'), 'shakeFreq', 4, 36, 0, ' Hz', T('Oscillation speed of the impact shake.'));
    var evFov = evSlider(T('FOV punch'), 'fovKick', -12, 16, 1, '°', T('Brief field-of-view kick on impact.'));

    subHead(evInsp, T('Light burst & lens'));
    var evLight = togglePill(evInsp, T('Native world light burst'),
      T('Spawns a real CS2 dynamic light flash at the grenade/event position when triggered.'),
      function () { var p = evSelected(); if (p) evSet('lightBurst', p.lightBurst ? 0 : 1); });
    var evLightStyle = modeRow(evInsp, T('Light colour'), [['1', T('White Flash')], ['0', T('Warm Blast')]], '200px',
      function (k) { evSet('lightFlash', k); }, T('White flashbang burst or warm amber HE blast light.'), 12);
    var evChroma = sliderRow(evInsp, {
      label: T('Chromatic burst'), min: 0, max: 100, decimals: 0, unit: '%',
      tip: '',
      onChange: function (v) { evSet('chromatic', (v / 100).toFixed(3)); }
    });
    var evVig = sliderRow(evInsp, {
      label: T('Vignette pulse'), min: 0, max: 100, decimals: 0, unit: '%',
      tip: '',
      onChange: function (v) { evSet('vignette', (v / 100).toFixed(3)); }
    });

    var evListSig = '';
    renderHooks.push(function (st) {
      var es = evState(); if (!es) return;
      evPill.set(!!es.on);
      evReset.setDisabled(!es.on);
      var presets = es.presets || [];
      var activeEnabled = 0;
      for (var k = 0; k < presets.length; k++) if (presets[k].enabled) activeEnabled++;
      evSec.__note(es.on ? T('{0} armed', activeEnabled) : T('Off'));

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
          r.style.border = '1px solid ' + (on ? '#ffb86bff' : '#ffffff12');
          var tg = lbl(r, p.trigger.toUpperCase(), '#ffb86bff', 9); tg.hittest = false;
          tg.style.width = '64px'; tg.style.fontWeight = 'bold'; tg.style.verticalAlign = 'center';
          var nm = lbl(r, p.name, on ? S.value : S.label, 12); nm.hittest = false;
          nm.style.width = 'fill-parent-flow(1.0)'; nm.style.verticalAlign = 'center';
          var tb = btn(r, T('Test'), function () { evCmd('test ' + p.id); }, '#ffb86bff');
          tb.style.marginRight = '4px';
          var eb = btn(r, p.enabled ? T('On') : T('Off'), function () { evCmd('enable ' + p.id + ' ' + (p.enabled ? 0 : 1)); }, p.enabled ? '#ffb86bff' : S.muted);
          eb.style.marginRight = '0px';
          r.SetPanelEvent('onactivate', function () { evCmd('select ' + p.id); });
        })(presets[i]);
      }

      var sel = evSelected();
      evInsp.visible = !!sel;
      if (!sel) return;
      evInspTitle.text = sel.name;
      evTrigGrid.sync(sel.trigger || 'kill');
      evExp.sync(sel.exposureEv);
      evAtk.sync(sel.attack);
      evDec.sync(sel.decay);
      evTint.sync(Math.round((sel.tintStrength || 0) * 100));
      evShake.sync(sel.shakeAmp);
      evRoll.sync(sel.shakeRoll);
      evFreq.sync(sel.shakeFreq);
      evFov.sync(sel.fovKick);
      evLight.set(!!sel.lightBurst);
      evLightStyle.sync(sel.lightFlash ? '1' : '0');
      evChroma.sync(Math.round((sel.chromatic || 0) * 100));
      evVig.sync(Math.round((sel.vignette || 0) * 100));
    });
)CFJS"
