#pragma once
// SCENE KEYS -- ADD GRAPH STRIPS.
//
// This tab is a SELECTOR, not a second value editor. Turning a property ON gives it a strip in the
// SCENE GRAPH dock; values, keys and curves live in GRAPH and are edited there. CONFIG stays the
// authored baseline for every property that is off, and for every tick outside a keyed span.
//
// It used to mirror CONFIG instead: a TextEntry plus a value tile / switch / option grid PER
// PROPERTY. That put a second, competing editor for the same number in the column right next to
// the graph that already owns it, it showed numbers the inspector cannot legitimately key (the
// tick under the playhead is the graph's business), and at ~110px per property it fitted four
// rows on screen out of a catalog of dozens. One line per property, no values, is the whole point.
R"EDJS(
    // The QUICK cards (Keys & History, Time of Day) and one rail item per catalog group, under
    // MAP EFFECTS and PROCEDURAL ENVIRONMENT (Docs/features/camera-editor-feature.md).
    var skQuickSec = edCard(inspectorBody, 'KEYS & HISTORY', { edge: S.accent, rail: { tab: 'scene', key: 'skq', group: 'Quick' } });
    var skTodSec = edCard(inspectorBody, 'TIME OF DAY', { edge: S.accent, rail: { tab: 'scene', key: 'sktod', group: 'Quick' } });
    skTodSec.__note('Key a look at this tick');

    function skState() { return (st && st.sceneKeys) || {}; }
    function skCatalog() { return skState().catalog || []; }
    function skCmd(s) { cmd('mirv_filmmaker scenekeys ' + s); }
    function skOn(id) { return !!((skState().armed || {})[id]); }
    function skKeys(id) { return (skState().byField || {})[id] || []; }
    function skTick() { return Math.round((st && st.tick) || 0); }
    function skPlural(n, one) { return n + ' ' + (n === 1 ? one : one + 's'); }

    // What the row says on its right-hand side. NEVER a value -- only where this property stands
    // relative to its own keys, which is the one thing GRAPH cannot tell you at a glance while you
    // are picking what to animate.
    function skRowState(id) {
      var keys = skKeys(id), n = keys.length, tick = skTick(), i;
      if (!n) return skOn(id) ? { text: 'on · no keys yet', color: S.label } : { text: 'off', color: S.hint };
      if (n === 1) return { text: '1 key · needs another', color: S.dangerText };
      for (i = 0; i < n; i++) if (keys[i].tick === tick) return { text: skPlural(n, 'key') + ' · at key', color: S.accent };
      return (tick > keys[0].tick && tick < keys[n - 1].tick)
        ? { text: skPlural(n, 'key') + ' · between keys', color: S.value }
        : { text: skPlural(n, 'key') + ' · follows CONFIG', color: S.muted };
    }
    function skGroupCount(g) {
      var out = { on: 0, keys: 0 }, i;
      for (i = 0; i < g.fields.length; i++) { if (skOn(g.fields[i])) out.on++; out.keys += skKeys(g.fields[i]).length; }
      return out;
    }
    // The shared confirm overlay. Its Yes caption is restated here (and in showClearConfirm) so
    // the two owners of the dialog cannot leave each other's verb on the button.
    function skConfirm(title, body, action) {
      confirmTitle.text = title; confirmBody.text = body;
      confirmYes.__lbl.text = 'Yes, turn it off';
      confirmAction = action; confirmOverlay.visible = true;
    }
    // Turning a property off removes its keys with it -- `scenekeys clear <id>` disarms as well --
    // so ON/OFF means exactly one thing: is this property animated? Keys are never left behind
    // driving the scene from a row that reads "off".
    function skToggle(f) {
      var n = skKeys(f.id).length;
      if (skOn(f.id) && n) {
        skConfirm('TURN OFF ' + f.label.toUpperCase() + '?',
          'This removes its strip and its ' + skPlural(n, 'key') + '. ' + f.label + ' goes back to CONFIG. You can undo this.',
          function () { skCmd('clear ' + f.id); });
        return;
      }
      skCmd('strip ' + f.id + ' ' + (skOn(f.id) ? 'off' : 'on'));
    }

    // NO PREAMBLE. This tab used to open with five stacked blocks of prose -- a two-line
    // explanation, a note under the buttons, a save line and a summary line, then two rules --
    // before the first thing you could click. PATH CAMERA and Config both go tab strip -> control,
    // and so does this now: the buttons, ONE quiet status line, then the list. What the prose said
    // is on the buttons' tooltips and on each property row, where it is read at the moment it
    // matters instead of every time the tab opens.
    // ONE toolbar row: Open Graph fills, the three history verbs beside it.
    var skTools = mk('Panel', skQuickSec); skTools.hittest = false; skTools.hittestchildren = true;
    skTools.style.flowChildren = 'right'; skTools.style.width = '100%';
    var skActions = { btns: [] };
    [['Open Graph', function () {
        // Open Graph is the one place that PUTS the dock in scene mode: an explicit "show me my
        // strips". Merely opening this tab never takes the camera graph away from you.
        cmd('mirv_filmmaker settings ui editor.graph scene'); cmd('mirv_filmmaker dock graph');
      }, 'primary'],
     ['◇ Key every strip', function () { skCmd('add'); }],
     ['↺ Undo', function () { skCmd('undo'); }],
     ['↻ Redo', function () { skCmd('redo'); }]].forEach(function (d, i) {
      var b = edButton(skTools, d[0], d[1], d[2]);
      b.style.height = '32px'; b.style.paddingTop = '0px'; b.style.paddingBottom = '0px';
      b.__lbl.style.verticalAlign = 'center';
      b.style.marginRight = i === 3 ? '0px' : '6px';
      if (i === 0) b.style.width = 'fill-parent-flow(1.0)';
      if (i === 0) mvmTourAdd('scene.opengraph', b);
      if (i === 1) mvmTourAdd('scene.keyall', b);
      skActions.btns.push(b);
    });
    // interactive() reads __tip on hover, so the captions stay short.
    skActions.btns[0].__tip = 'Show these strips in the SCENE GRAPH dock';
    skActions.btns[1].__tip = 'Drop one key on every property that is on, at the current tick';
    skActions.btns[2].__tip = 'Undo the last scene-key edit';
    skActions.btns[3].__tip = 'Redo the last undone scene-key edit';
    // ONE line: what is on, and whether it is saved.
    var skStatus = edNote(skQuickSec, '');
    edActionGroup(skQuickSec, '', [
      ['Delete selected keys', function () { skCmd('delsel'); }],
      ['Turn everything off', function () {
        skConfirm('TURN EVERY PROPERTY OFF?', 'This removes every scene strip and every scene key from this demo. You can undo this.',
          function () { skCmd('clear all'); });
      }, 'danger']
    ]);
    var skTodGroup = edActionGroup(skTodSec, 'Key a look', [
      ['Dawn', function () { skCmd('macro dawn'); }], ['Day', function () { skCmd('macro day'); }], ['Golden', function () { skCmd('macro golden'); }],
      ['Dusk', function () { skCmd('macro dusk'); }], ['Night', function () { skCmd('macro night'); }], ['Moonlit', function () { skCmd('macro moonlit'); }]
    ], 3, 'Turns the lighting, sky and fog strips on and keys them at this tick. Ordinary keys — reshape or delete them in GRAPH.');
    // The onboarding tutorial points at the time-of-day buttons by name ($.MvmTour).
    try { mvmTourAdd('scene.rail.tod', edRails.scene.items['sktod'].row); } catch (e) {}
    try { mvmTourAdd('scene.rail.keys', edRails.scene.items['skq'].row); } catch (e) {}
    if (skTodGroup && skTodGroup.btns) {
      ['dawn', 'day', 'golden', 'dusk', 'night', 'moonlit'].forEach(function (n, i) {
        if (skTodGroup.btns[i]) mvmTourAdd('scene.tod.' + n, skTodGroup.btns[i]);
      });
    }

    var skGroups = {}, skFields = {}, skCatalogSig = '';

    // ONE LINE PER PROPERTY: diamond, name, state. The whole row is the hit target -- a 20px
    // diamond is not a control anyone wants to aim at, and there is nothing else in the row to
    // click, so the row itself IS the switch.
    function skRowPanel(parent, diamond, name, tip, onClick) {
      edEndTileLine(parent);
      var r = mk('Panel', parent);
      r.style.flowChildren = 'right'; r.style.width = '100%'; r.style.borderRadius = '3px';
      r.style.marginTop = '2px'; r.style.height = '32px';
      r.style.paddingLeft = '6px'; r.style.paddingRight = '8px';
      var dia = lbl(r, diamond, S.muted, 14);
      dia.hittest = false; dia.style.width = '20px'; dia.style.textAlign = 'center'; dia.style.verticalAlign = 'center';
      var nm = lbl(r, name, S.label, 13);
      nm.hittest = false; nm.style.verticalAlign = 'center'; nm.style.marginLeft = '6px';
      nm.style.width = 'fill-parent-flow(1.0)'; nm.style.whiteSpace = 'nowrap'; nm.style.textOverflow = 'ellipsis';
      var state = lbl(r, '', S.dim, 11);
      state.hittest = false; state.style.verticalAlign = 'center'; state.style.marginLeft = '8px'; state.style.whiteSpace = 'nowrap';
      // The square switch says on/off at a glance; the whole row is still the hit target.
      var sw = mk('Panel', r); sw.hittest = false;
      sw.style.width = UI.swSm.w + 'px'; sw.style.height = UI.swSm.h + 'px'; sw.style.borderRadius = '3px';
      sw.style.verticalAlign = 'center'; sw.style.marginLeft = '10px';
      var knob = mk('Panel', sw); knob.hittest = false;
      knob.style.width = UI.swSm.knob + 'px'; knob.style.height = UI.swSm.knob + 'px';
      knob.style.borderRadius = '2px'; knob.style.backgroundColor = '#dfe5ecff';
      knob.style.verticalAlign = 'center'; knob.style.marginLeft = '2px';
      knob.style.transitionProperty = 'transform'; knob.style.transitionDuration = '0.12s';
      interactive(r, nm, onClick, { bg: S.clear, border: S.clear, fg: S.label,
        selBg: '#ffffff06', selBorder: S.clear, selFg: S.value, bgHover: S.rowHover, fgHover: S.value, tip: tip });
      r.__accent = S.clear;
      function setSw(on) {
        sw.style.backgroundColor = on ? '#2bb24cff' : '#00000088';
        sw.style.border = '1px solid ' + (on ? '#1c8a3aff' : '#ffffff26');
        knob.style.transform = 'translateX(' + (on ? UI.swSm.travel : 0) + 'px)';
      }
      setSw(false);
      return { row: r, dia: dia, nm: nm, state: state, sw: sw, setSw: setSw };
    }

    // What CONFIG itself shows is listed by default; the rest (values CONFIG keeps off its panels)
    // sits under "More settings". Stable catalog IDs are unchanged, and active/keyed properties
    // are always visible.
    function skCommon(id) {
      return (' exposure lightColor lightAmount lightDirection.yaw lightDirection.pitch sunGlowSize sunVisible'
        + ' sky skyDirection.yaw skyDirection.pitch skyMirror skyColor cloudColor cloudDensity cloudSize cloudDrift cloudBrightness'
        + ' worldColors.world worldStrength smokeColors.T smokeColors.CT'
        + ' env.exposure env.saturation env.contrast env.tint env.bloom env.vignette env.grain env.chromatic'
        + ' env.sky env.sunAzimuth env.sunElevation env.sunPower env.haze env.stars env.clouds env.coverage env.cloudBase env.windSpeed'
        + ' env.ambient env.sunAngle env.fogDensity env.fogColor env.fogStart env.fogHeight env.fogNoise'
        + ' env.weather.type env.weather.speed env.snow env.wet env.puddles ')
        .indexOf(' ' + id + ' ') >= 0;
    }

    function skBuild() {
      var cat = skCatalog(), sig = cat.map(function (f) { return f.id; }).join('|');
      if (!sig || sig === skCatalogSig) return;
      skCatalogSig = sig;
      // A rebuild replaces every catalog card: drop the old ones from the rail and the pane.
      for (var oldId in skGroups) {
        edRailRemove('scene', 'sk.' + oldId);
        try { skGroups[oldId].card.__card.DeleteAsync(0); } catch (e) {}
      }
      skGroups = {}; skFields = {};
      ['effects', 'environment'].forEach(function (family) {
        var fam = cat.filter(function (f) { return f.family === family; });
        if (!fam.length) return;
        // Both families carry a LIGHTING card, so the rail's group heading -- CONFIG's own tab
        // name -- is what tells two identically named cards apart.
        var groupName = family === 'effects' ? 'Effects' : 'Environment';
        var order = [];
        fam.forEach(function (f) { var gid = family + '.' + (f.groupId || f.group); if (order.indexOf(gid) < 0) order.push(gid); });
        order.forEach(function (groupId) {
          var members = fam.filter(function (f) { return family + '.' + (f.groupId || f.group) === groupId; });
          var title = ('' + members[0].group).toUpperCase();
          var card = edCard(inspectorBody, title, { edge: family === 'effects' ? '#d4b36aff' : '#7fbdc7ff',
            rail: { tab: 'scene', key: 'sk.' + groupId, group: groupName } });
          var g = skGroups[groupId] = { fields: [], title: title, card: card };
          try { mvmTourAdd('scene.rail.' + groupId, edRails.scene.items['sk.' + groupId].row); } catch (e) {}
          g.more = false; g.extras = [];
          members.forEach(function (f) {
            g.fields.push(f.id);
            // The catalog's name is the CONFIG control's (Movie/SceneKeyframeCatalog.cpp), and the
            // graph strip shows the same one. This tab used to rename a third of them on its own,
            // so a property was "Sun direction (yaw)" here and "Sun Azimuth" in the graph.
            var name = f.label;
            skFields[f.id] = skRowPanel(card, '◇', name,
              'Animate ' + name + ' in the Scene Graph', function () { skToggle(f); });
            mvmTourAdd('scene.row.' + f.id, skFields[f.id].row);
            if (!skCommon(f.id)) g.extras.push(f.id);
          });
          if (g.extras.length) {
            g.disclosure = skRowPanel(card, '+', 'More settings',
              'Show additional animation controls', function () { g.more = !g.more; syncSceneInspector(); });
            g.disclosure.sw.visible = false;
          }
        });
      });
      // The stored selection may name a catalog card that only exists now.
      var want = st && st.ui && st.ui['editor.scene.sel'];
      if (want) edSelectCard('scene', want, false);
    }

    function syncSceneInspector() {
      if (inspectorMode !== 'scene') return;
      skBuild();
      var sk = skState(), id, i;
      var onCount = 0, keyCount = 0;
      for (id in skFields) { if (skOn(id)) onCount++; keyCount += skKeys(id).length; }
      var statusText = (onCount ? skPlural(onCount, 'strip') + ' · ' + skPlural(keyCount, 'key')
                                : 'Nothing on · every property follows CONFIG')
        + '  ·  ' + (sk.save === 'saving' ? 'Saving…' : sk.save === 'failed' ? 'Save failed' : 'Saved')
        + (sk.error ? '  ·  ' + sk.error : '');
      if (skStatus.text !== statusText) skStatus.text = statusText;
      skQuickSec.__note((onCount ? onCount + ' animated' : 'Nothing animated') + ' · ' + (sk.save === 'failed' ? 'save failed' : 'saved'));
      skStatus.style.color = sk.save === 'failed' ? S.dangerText : S.muted;
      // Idempotent: this runs every frame over the whole catalog (~70 rows), and a Label write
      // that changes nothing still costs a layout pass -- the same reason edCard caches its note.
      for (id in skFields) {
        var r = skFields[id], on = skOn(id), s = skRowState(id), sig = (on ? '1' : '0') + s.text + s.color;
        btnSel(r.row, on);
        if (sig === r.sig) continue;
        r.sig = sig;
        r.setSw(on);
        r.dia.text = on ? '◆' : '◇'; r.dia.style.color = on ? S.accent : S.muted;
        r.state.text = s.text; r.state.style.color = s.color;
      }
      for (var groupId in skGroups) {
        var g = skGroups[groupId], c = skGroupCount(g), gsig = c.on + '/' + c.keys;
        g.card.__note(c.on || c.keys ? (c.on + ' of ' + g.fields.length + ' on · ' + skPlural(c.keys, 'key')) : 'Nothing animated');
        g.card.__badge(c.on ? String(c.on) : '', 'gold');
        gsig += '/' + g.more;
        // Include each extra's state, not just the group count: undo can swap active fields.
        for (var e = 0; e < g.extras.length; e++) {
          var extra = g.extras[e], shown = g.more || skOn(extra) || skKeys(extra).length > 0;
          skFields[extra].row.visible = shown;
          gsig += shown ? '1' : '0';
        }
        if (gsig === g.sig) continue;
        g.sig = gsig;
        if (g.disclosure) {
          var hidden = g.extras.filter(function (id) { return !skOn(id) && !skKeys(id).length; }).length;
          g.disclosure.row.visible = g.more || hidden > 0;
          g.disclosure.nm.text = g.more ? 'Fewer settings' : 'More settings';
          g.disclosure.dia.text = g.more ? '−' : '+';
          g.disclosure.state.text = g.more ? '' : '' + hidden;
        }
      }
    }
)EDJS"
