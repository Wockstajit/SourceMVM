#pragma once

// SCENE OBJECTS (Docs/features/filmmaker_config_panel.md; original "Scene objects pane redesign", option 2a): the add-light row and
// the OUTLINER page. Every placed thing -- CS2 assets (Filmmaker/Props) and the Environment's
// lights and meshes -- in one tree of groups, Blender-style. Each row carries an eye (shown in the
// view), a camera (kept in renders) and a lock (cannot be picked or dragged in the view).
// Right-click a row (or a prop in the view) for its menu; Delete removes the selection and Ctrl+D
// duplicates it (MovieMode hands both keys to $.ConfigHud.key).
//
// A group is just a name stored on each member (`props set <id> group`, `environment object <id>
// group`), so there is no separate group list to keep in step: a group exists while something is
// filed under it, and its eye/camera/lock act on all of its members. Collapsed groups and the
// isolate memo are panel state only.
//
// The card's pages: OUTLINER (here) and ASSETS & LIGHTS (ConfigHudPropsJs.h). The light row is
// made before the first page, so it sits above the tab strip and works from either tab. The
// selected object's card holds the light and mesh inspectors (ConfigHudEnvironmentTabJs.h).

R"CFJS(
    // ===================== SCENE OBJECTS: + Add =====================
    // ONE header action (2026-10 sidebar cleanup) that opens a menu: Point, Spot and Area lights,
    // and a camera object (scene-object-rigs.md: a viewpoint to attach, key and look through).
    // The four add buttons that used to sit above the tabs are gone; the body holds the tree.
    var ENV_PAGE_OUTLINER = 0, ENV_PAGE_ASSETS = 1;
    function olAdd(kind) {
      // A new object starts ahead of the camera, selected, so its card opens on the Outliner.
      envCommand('add ' + kind);
      envSceneSec.__setPage(ENV_PAGE_OUTLINER);
      olMulti = {};
    }
    var olAddBtn = envSceneSec.__headerAction('+ Add', function () {
      cfgMenu([
        ['Point light', function () { olAdd('point'); }, 'Shines all around'],
        ['Spot light', function () { olAdd('spot'); }, 'A cone of light'],
        ['Area light', function () { olAdd('area'); }, 'A soft rectangle'],
        null,
        ['Camera', function () { olAdd('camera'); }, 'A viewpoint']
      ], olAddBtn);
    });
    attachTip(olAddBtn, 'Adds a light or a camera ahead of the view and selects it. CS2 assets and your meshes come from the Library page.');

    // ===================== SCENE OBJECTS > OUTLINER =====================
    var olPage = envSceneSec.__page('Outliner');
    var OL_KIND = { asset: ['#80bdff', 'CS2 asset'], light: ['#f2d394', 'Light'], mesh: ['#b9a3ff', 'Your mesh'], camera: ['#ffd76a', 'Camera'] };
    var OL_ICON = 's2r://panorama/images/icons/ui/';
    var olOpen = {}, olFilter = 'all', olSel = '', olSig = '', olItemsNow = [];
    var olLastProp = 0, olLastEnv = 0, olIsolate = '', olIsolateMemo = null;
    // Multi-select: Shift-click adds the range from the last clicked row, Ctrl-click adds or drops
    // one row. olSel stays the "current" item (its card); olMulti holds the rest of the selection.
    var olMulti = {}, olAnchor = '', olOrder = [], olShift = false, olCtrl = false;

    function olIsLightKind(k) { return k === 'point' || k === 'spot' || k === 'area'; }
    // Every outliner item, from both backends.
    function olItems(st) {
      var out = [];
      ((st.props && st.props.placed) || []).forEach(function (q) {
        out.push({ key: 'p' + q.id, src: 'p', id: q.id, name: q.name, kind: 'asset', group: q.group || '',
                   visible: !!q.visible, render: q.render !== false, locked: !!q.locked,
                   model: q.model, folder: q.folder, source: q.source || '', scale: q.scale, ang: q.ang || [0, 0, 0] });
      });
      var s = envState();
      ((s && s.objects) || []).forEach(function (o) {
        var light = olIsLightKind(o.kind), camera = o.kind === 'camera';
        out.push({ key: 'e' + o.id, src: 'e', id: o.id, kind: light ? 'light' : camera ? 'camera' : 'mesh',
                   name: o.asset || (light ? o.kind + ' light' : camera ? 'Camera' : o.kind) + ' ' + o.id,
                   group: o.group || '', visible: o.enabled !== false, render: o.render !== false,
                   locked: !!o.locked, envKind: o.kind });
      });
      return out;
    }
    // What the tree draws from an item: NOT its pose. Turning or sizing a prop changes its angles
    // every frame of the drag; with them in the signature the whole tree was rebuilt every frame,
    // which is what made rotating lag with a big painted scene.
    function olTreeSig(items) {
      return items.map(function (i) {
        return i.key + ':' + i.name + ':' + i.kind + ':' + i.group + ':' + (i.visible ? 1 : 0) + (i.render ? 1 : 0) + (i.locked ? 1 : 0);
      }).join('|');
    }
    function olSet(item, field, on) {
      var v = on ? '1' : '0';
      if (item.src === 'p') propsCommand('set ' + item.id + ' ' + (field === 'visible' ? 'visible' : field) + ' ' + v);
      else envCommand('object ' + item.id + ' ' + (field === 'visible' ? 'enabled' : field) + ' ' + v);
    }
    function olSetGroup(item, name) {
      var arg = name ? '"' + name + '"' : '-';
      if (item.src === 'p') propsCommand('set ' + item.id + ' group ' + arg);
      else envCommand('object ' + item.id + ' group ' + arg);
    }
    function olMembers(name) { return olItemsNow.filter(function (i) { return i.group === name; }); }
    // Every selected item: the multi-selection plus the current one.
    function olPicked() {
      var keys = Object.keys(olMulti);
      if (olSel && olSel.indexOf('g:') !== 0 && !olMulti[olSel]) keys.push(olSel);
      return keys.map(olFind).filter(function (i) { return !!i; });
    }
    function olIsPicked(key) { return olSel === key || !!olMulti[key]; }
    // A click on an item row, with the modifiers held at the time.
    function olClick(key) {
      if (olShift && olAnchor && olAnchor !== key) {
        var a = olOrder.indexOf(olAnchor), b = olOrder.indexOf(key);
        if (a >= 0 && b >= 0) {
          for (var i = Math.min(a, b); i <= Math.max(a, b); i++) olMulti[olOrder[i]] = true;
        }
      } else if (olCtrl) {
        if (olMulti[key]) delete olMulti[key]; else olMulti[key] = true;
        if (olSel && olSel.indexOf('g:') !== 0) olMulti[olSel] = true;
        olAnchor = key;
      } else {
        olMulti = {};
        olAnchor = key;
      }
      olSelect(key);
    }
    function olSelect(key) {
      olSel = key; olSig = '';
      var it = olFind(key);
      if (!it) return;
      if (it.src === 'p') propsCommand('select ' + it.id); else envCommand('select ' + it.id);
    }
    function olFind(key) {
      for (var i = 0; i < olItemsNow.length; i++) if (olItemsNow[i].key === key) return olItemsNow[i];
      return null;
    }
    // Removes several items at once: the CS2 assets in batches, the Environment's lights and meshes
    // one by one. A console command holds about 60 arguments, so a big group sent as ONE remove lost
    // everything past that (a painted patch of hundreds stayed); 40 ids per command stays clear of it.
    function olRemoveAll(items) {
      var ids = items.filter(function (i) { return i.src === 'p'; }).map(function (i) { return i.id; });
      for (var b = 0; b < ids.length; b += 40) propsCommand('remove ' + ids.slice(b, b + 40).join(' '));
      items.forEach(function (i) { if (i.src !== 'p') envCommand('remove ' + i.id); delete olMulti[i.key]; });
      olSel = ''; olSig = '';
    }
    // The same, after asking: used for a whole group or a selection of more than one.
    function olConfirmRemove(items, what) {
      if (!items.length) return;
      if (items.length === 1) { olRemoveAll(items); return; }
      cfgConfirm('Remove ' + what + '?', 'This removes ' + items.length + ' objects from the scene.',
        'Remove ' + items.length, function () { olRemoveAll(items); });
    }
    function olAct(it, act) {
      if (!it) return;
      var c = it.src === 'p' ? propsCommand : envCommand;
      if (act === 'duplicate') c('duplicate ' + it.id);
      else if (act === 'remove') { c('remove ' + it.id); delete olMulti[it.key]; if (olSel === it.key) olSel = ''; }
      else if (act === 'floor') propsCommand('floor ' + it.id);
      else if (act === 'here') propsCommand('here ' + it.id);
      else if (act === 'upright') { propsCommand('set ' + it.id + ' pitch 0'); propsCommand('set ' + it.id + ' roll 0'); }
      else if (act === 'edit') envCommand('edit start ' + it.id);
      olSig = '';
    }
    function olMenu(it) {
      if (!it) return;
      var picked = olPicked();
      if (picked.length > 1 && olIsPicked(it.key)) {
        cfgMenu([
          ['New group from these', function () {
            var name = olNewGroupName(); picked.forEach(function (i) { olSetGroup(i, name); }); olOpen[name] = true;
          }],
          ['Hide these', function () { picked.forEach(function (i) { if (i.visible) olSet(i, 'visible', false); }); }],
          ['Show these', function () { picked.forEach(function (i) { if (!i.visible) olSet(i, 'visible', true); }); }],
          null,
          ['Remove ' + picked.length + ' selected', function () { olConfirmRemove(picked, picked.length + ' selected objects'); }, 'Del', true]
        ]);
        return;
      }
      var items = [['Duplicate', function () { olAct(it, 'duplicate'); }, 'Ctrl+D'],
                   ['Copy', function () { olClip = it.key; }, 'Ctrl+C']];
      if (olFind(olClip)) items.push(['Paste', function () { $.ConfigHud.key('paste'); }, 'Ctrl+V']);
      if (it.kind === 'asset') {
        items.push(['Drop to floor', function () { olAct(it, 'floor'); }]);
        items.push(['Move to crosshair', function () { olAct(it, 'here'); }]);
        items.push(['Stand upright', function () { olAct(it, 'upright'); }]);
      } else {
        items.push(['Edit position', function () { olAct(it, 'edit'); }]);
        // A camera object's own view (the RIG card has the same two).
        if (it.kind === 'camera') {
          items.push(['Go to camera', function () { cmd('mirv_filmmaker rig goto ' + it.id); }]);
          items.push([st && st.rig && st.rig.lookThrough === it.id ? 'Stop looking through' : 'Look through',
            function () { cmd('mirv_filmmaker rig lookthrough ' + it.id + ' ' + (st && st.rig && st.rig.lookThrough === it.id ? 'off' : 'on')); }]);
        }
      }
      items.push(null);
      items.push([it.visible ? 'Hide' : 'Show', function () { olSet(it, 'visible', !it.visible); }]);
      items.push([it.locked ? 'Unlock' : 'Lock', function () { olSet(it, 'locked', !it.locked); }]);
      items.push(null);
      items.push(['Remove', function () { olAct(it, 'remove'); }, 'Del', true]);
      cfgMenu(items);
    }
    function olGroupMenu(name) {
      cfgMenu([
        [olIsolate === name ? 'Show all' : 'Isolate', function () {
          if (olIsolate === name) olEndIsolate(); else { if (olIsolate) olEndIsolate(); olStartIsolate(name); }
        }],
        ['Ungroup', function () {
          if (olIsolate === name) olEndIsolate();
          olMembers(name).forEach(function (i) { olSetGroup(i, ''); });
          if (olSel === 'g:' + name) olSel = '';
        }],
        null,
        ['Delete group', function () { olConfirmRemove(olMembers(name), 'the group “' + name + '”'); }, 'Del', true]
      ]);
    }
    // Delete, Ctrl+D, Ctrl+C and Ctrl+V act on the outliner's selection while Scene Objects is the
    // card on screen. A pasted CS2 asset hangs under the mouse like a placement (same size, turn
    // and group), and is selected once it is dropped; a light or mesh pastes as a duplicate.
    var olClip = '';
    keyHooks.push(function (name) {
      if (activeTab !== 'environment' || !cfgRails.environment || cfgRails.environment.sel !== 'env.scene') return;
      if (name === 'paste') {
        var src = olFind(olClip);
        if (!src) return;
        if (src.src === 'p') propsCommand('paste ' + src.id); else envCommand('duplicate ' + src.id);
        return;
      }
      if (name === 'delete') {
        // A group, or a selection of several, goes after a confirm; one item goes at once.
        if (olSel.indexOf('g:') === 0) { var g = olSel.substring(2); olConfirmRemove(olMembers(g), 'the group “' + g + '”'); return; }
        var picked = olPicked();
        if (picked.length > 1) { olConfirmRemove(picked, picked.length + ' selected objects'); return; }
      }
      var it = olFind(olSel);
      if (!it) return;
      if (name === 'delete') olAct(it, 'remove');
      else if (name === 'duplicate') olAct(it, 'duplicate');
      else if (name === 'copy') olClip = it.key;
    });
    function olNewGroupName() {
      var used = {}, n = 1;
      olItemsNow.forEach(function (i) { if (i.group) used[i.group] = true; });
      while (used['Group ' + n]) n++;
      return 'Group ' + n;
    }
)CFJS"
R"CFJS(
    // ---- small drawn icons ---------------------------------------------------------------------
    // CS2 ships no eye icon, so the eye is drawn: an outline, a pupil, and a slash when hidden.
    // ICONS (2026-10 sidebar cleanup): one consistent outline set, Lucide (ISC licence, see
    // FX/ui/panorama/images/sourcemvm/LUCIDE-LICENSE), rasterised at 32px and compiled into the
    // FX pack as Panorama textures (Docs/features/filmmaker_community.md describes the pipeline).
    // The eye used to be drawn from panels and the lock was a CS2 glyph of another style.
    var OL_OWN_ICON = 's2r://panorama/images/sourcemvm/ol_';
    function olIcon(parent, name, size, color) {
      var im = $.CreatePanel('Image', parent, '', { src: OL_OWN_ICON + name + '_png.vtex', scaling: 'stretch-to-fit-preserve-aspect' });
      im.hittest = false; im.style.width = size + 'px'; im.style.height = size + 'px';
      im.style.horizontalAlign = 'center'; im.style.verticalAlign = 'center';
      if (color) im.style.washColor = color;
      return im;
    }
    // The eye keeps its old shape of handle: __paint(on, color) swaps eye / eye-off and tints.
    function olEye(parent) {
      var e = mk('Panel', parent); e.hittest = false;
      e.style.width = '16px'; e.style.height = '16px';
      e.style.horizontalAlign = 'center'; e.style.verticalAlign = 'center';
      var open = olIcon(e, 'eye', 16), shut = olIcon(e, 'eye_off', 16);
      e.__paint = function (on, color) {
        open.visible = !!on; shut.visible = !on;
        open.style.washColor = color; shut.style.washColor = color;
      };
      return e;
    }
    function olImage(parent, name, size) {
      // CS2's own packed glyphs, still used where our set has no equivalent.
      var im = $.CreatePanel('Image', parent, '', { src: OL_ICON + name + '.vsvg', scaling: 'stretch-to-fit-preserve-aspect' });
      im.hittest = false; im.style.width = size + 'px'; im.style.height = size + 'px';
      im.style.horizontalAlign = 'center'; im.style.verticalAlign = 'center';
      return im;
    }
    var OL_KIND_ICON = { asset: 'box', light: 'lightbulb', mesh: 'shapes', camera: 'video' };
    // A square icon button in one of the row's three 30px columns.
    function olIconBtn(parent, onClick) {
      var b = mk('Panel', parent);
      b.hittest = true; b.style.width = '30px'; b.style.height = '30px'; b.style.borderRadius = '3px';
      b.SetPanelEvent('onactivate', onClick);
      b.SetPanelEvent('onmouseover', function () { b.style.backgroundColor = '#232a35'; });
      b.SetPanelEvent('onmouseout', function () { b.style.backgroundColor = S.clear; });
      return b;
    }

    // ---- filter, then the tree ---------------------------------------------------------------
    var olFilters = segmented(olPage, [
      ['all', 'All'], ['asset', 'CS2 assets'], ['light', 'Lights'], ['mesh', 'Your meshes']
    ], '100%', function (k) { olFilter = k; olSig = ''; }, 12);
    if (olFilters.panel) olFilters.panel.style.marginTop = '8px';

    var olIsoBar = mk('Panel', olPage);
    olIsoBar.style.width = '100%'; olIsoBar.style.flowChildren = 'right'; olIsoBar.style.marginTop = '8px';
    olIsoBar.style.padding = '8px 10px'; olIsoBar.style.backgroundColor = '#e0b8611a';
    olIsoBar.style.border = '1px solid #6a5732'; olIsoBar.style.borderRadius = '4px';
    var olIsoText = lbl(olIsoBar, '', '#f2d394ff', 12); olIsoText.style.verticalAlign = 'center';
    olIsoText.style.width = 'fill-parent-flow(1.0)'; olIsoText.style.fontWeight = 'bold';
    actionBtn(olIsoBar, 'Show all', function () { olEndIsolate(); });
    olIsoBar.visible = false;

    var olTree = mk('Panel', olPage);
    olTree.style.width = '100%'; olTree.style.flowChildren = 'down'; olTree.style.marginTop = '8px';
    olTree.style.backgroundColor = '#0f1318ff'; olTree.style.border = '1px solid ' + S.tileBorder;
    olTree.style.borderRadius = '4px';
    var olHead = mk('Panel', olTree);
    olHead.style.width = '100%'; olHead.style.height = '26px'; olHead.style.flowChildren = 'right';
    olHead.style.borderBottom = '1px solid #222832'; olHead.style.paddingLeft = '12px';
    var olHeadName = lbl(olHead, 'NAME', S.muted, 11);
    olHeadName.style.width = 'fill-parent-flow(1.0)'; olHeadName.style.verticalAlign = 'center';
    olHeadName.style.fontWeight = 'bold'; olHeadName.style.letterSpacing = '1.5px';
    [['eye', 'Shown in the view'], ['video', 'Kept in renders'], ['lock', 'Locked: cannot be picked or dragged in the view']].forEach(function (c) {
      var cell = mk('Panel', olHead); cell.style.width = '30px'; cell.style.height = '26px';
      olIcon(cell, c[0], 14, '#8b95a3ff');
      attachTip(cell, c[1]);
    });
    // Ten rows (303px) at most; the rest scrolls inside the tree, with no scrollbar.
    var olWell = scrollWell(olTree, 303);
    var olRows = olWell.inner;
    // Lights draw with the Environment lighting: one line, and the way to it (it replaced the
    // old LIT BY cards).
    var olLitBar = mk('Panel', olTree);
    olLitBar.style.width = '100%'; olLitBar.style.flowChildren = 'right'; olLitBar.style.padding = '7px 12px';
    olLitBar.style.borderTop = '1px solid #222832';
    var olLitDot = mk('Panel', olLitBar); olLitDot.hittest = false;
    olLitDot.style.width = '7px'; olLitDot.style.height = '7px'; olLitDot.style.borderRadius = '4px';
    olLitDot.style.verticalAlign = 'center'; olLitDot.style.marginRight = '8px';
    var olLitText = lbl(olLitBar, '', S.hint, 12); olLitText.style.verticalAlign = 'center';
    olLitText.style.width = 'fill-parent-flow(1.0)';
    var olLitLink = lbl(olLitBar, 'Lighting ›', '#80bdffff', 12);
    olLitLink.hittest = true; olLitLink.style.fontWeight = 'bold'; olLitLink.style.verticalAlign = 'center';
    olLitLink.SetPanelEvent('onactivate', function () { cfgSelectCard('environment', 'env.light', true); });
    var olEmpty = envNote(olPage, 'Nothing placed yet. Add CS2 assets on Assets & Lights, or a light above.');

    var olUnder = mk('Panel', olPage);
    olUnder.style.width = '100%'; olUnder.style.flowChildren = 'right'; olUnder.style.marginTop = '6px';
    var olHint = lbl(olUnder, 'Shift / Ctrl-click to pick several', S.hint, 11);
    olHint.style.width = 'fill-parent-flow(1.0)'; olHint.style.verticalAlign = 'center';
    var olGroupBtn = actionBtn(olUnder, 'New group from selection', function () {
      var picked = olPicked();
      if (!picked.length) return;
      var name = olNewGroupName();
      picked.forEach(function (i) { olSetGroup(i, name); });
      olOpen[name] = true; olSel = 'g:' + name; olMulti = {};
    });
    var olUndoBtn = actionBtn(olUnder, 'Undo', function () { propsCommand('undo'); }); // same history as Ctrl+Z

    // ---- the row ---------------------------------------------------------------------------------
    function olRow(o) {
      var r = mk('Panel', olRows);
      r.style.width = '100%'; r.style.height = '30px'; r.style.flowChildren = 'right';
      r.style.backgroundColor = o.selected ? '#7fe0d01a' : S.clear;
      r.style.opacity = o.dim ? '0.45' : '1';
      var pad = mk('Panel', r); pad.style.width = o.indent + 'px'; pad.style.height = '30px';
      if (o.child) {
        var guide = mk('Panel', pad); guide.style.width = '1px'; guide.style.height = '30px';
        guide.style.backgroundColor = '#2a313bff'; guide.style.horizontalAlign = 'right'; guide.style.marginRight = '14px';
      }
      if (o.group) {
        var tw = olIconBtn(r, o.toggleOpen); tw.style.width = '22px';
        olIcon(tw, o.open ? 'chevron_down' : 'chevron_right', 14, '#9aa3b0ff');
      }
      var nb = mk('Panel', r);
      nb.hittest = true; nb.style.width = 'fill-parent-flow(1.0)'; nb.style.height = '28px';
      nb.style.flowChildren = 'right'; nb.style.borderRadius = '3px'; nb.style.paddingLeft = '6px';
      nb.style.verticalAlign = 'center';
      nb.SetPanelEvent('onactivate', o.select);
      if (o.key) olOrder.push(o.key);
      if (o.menu) nb.SetPanelEvent('oncontextmenu', o.menu);
      nb.SetPanelEvent('onmouseover', function () { nb.style.backgroundColor = '#1c222bff'; });
      nb.SetPanelEvent('onmouseout', function () { nb.style.backgroundColor = S.clear; });
      // The kind is an icon in the kind's colour (box, bulb, shapes, camera), not a dot.
      if (o.group) olIcon(nb, o.open ? 'folder_open' : 'folder', 15, '#e0b861ff');
      else {
        var kindIcon = olIcon(nb, OL_KIND_ICON[o.kind] || 'box', 14, OL_KIND[o.kind][0] + 'ff');
        kindIcon.style.marginLeft = '1px';
      }
      var name = lbl(nb, o.name, o.selected ? ENV_ACCENT : S.value, 13);
      name.hittest = false; name.style.verticalAlign = 'center'; name.style.marginLeft = '8px';
      name.style.fontWeight = o.group ? 'bold' : 'normal'; name.style.whiteSpace = 'nowrap';
      name.style.textOverflow = 'ellipsis'; name.style.maxWidth = '190px';
      if (o.count) {
        var count = lbl(nb, o.count, S.muted, 12); count.hittest = false;
        count.style.verticalAlign = 'center'; count.style.marginLeft = '8px';
      }
      var eye = olIconBtn(r, o.toggleEye);
      olEye(eye).__paint(o.visible, o.visible ? '#9aa3b0ff' : '#e6e9eeff');
      attachTip(eye, o.visible ? 'Hide in the view' : 'Show in the view');
      var cam = olIconBtn(r, o.toggleRender);
      olIcon(cam, o.render ? 'video' : 'video_off', 15, o.render ? '#9aa3b0ff' : '#e05a5aff');
      attachTip(cam, o.render ? 'Leave out of renders' : 'Put back in renders');
      var lk = olIconBtn(r, o.toggleLock);
      olIcon(lk, o.locked ? 'lock' : 'lock_open', 14, o.locked ? '#e0b861ff' : '#5d6672ff');
      attachTip(lk, o.locked ? 'Unlock' : 'Lock: cannot be picked or dragged in the view');
    }
)CFJS"
R"CFJS(
    // ---- isolate ---------------------------------------------------------------------------------
    // Hides everything outside one group and remembers what was shown, so Show all puts the scene
    // back exactly as it was rather than showing things that were hidden on purpose.
    function olStartIsolate(name) {
      olIsolateMemo = {};
      olItemsNow.forEach(function (i) {
        olIsolateMemo[i.key] = i.visible;
        if (i.group !== name && i.visible) olSet(i, 'visible', false);
      });
      olIsolate = name;
    }
    function olEndIsolate() {
      if (!olIsolateMemo) { olIsolate = ''; return; }
      olItemsNow.forEach(function (i) {
        if (olIsolateMemo.hasOwnProperty(i.key) && olIsolateMemo[i.key] !== i.visible) olSet(i, 'visible', olIsolateMemo[i.key]);
      });
      olIsolate = ''; olIsolateMemo = null;
    }

    // ---- the selected-object card --------------------------------------------------------------
    // Name, kind chip and where it comes from; then only what is adjusted here, per kind; then the
    // actions, right-aligned. The light and mesh inspectors are built into olLightHost / olMeshHost
    // by ConfigHudEnvironmentTabJs.h.
    var olCard = mk('Panel', olPage);
    olCard.style.width = '100%'; olCard.style.flowChildren = 'down'; olCard.style.marginTop = '10px';
    olCard.style.padding = '12px'; olCard.style.backgroundColor = '#1b2129ff';
    olCard.style.border = '1px solid ' + S.tileBorder; olCard.style.borderRadius = '4px';
    var olCardTop = mk('Panel', olCard);
    olCardTop.style.width = '100%'; olCardTop.style.flowChildren = 'right';
    var olCardTitle = lbl(olCardTop, '', S.value, 16);
    olCardTitle.style.width = 'fill-parent-flow(1.0)'; olCardTitle.style.fontWeight = 'bold';
    olCardTitle.style.whiteSpace = 'nowrap'; olCardTitle.style.textOverflow = 'ellipsis';
    olCardTitle.style.verticalAlign = 'center';
    var olCardChip = mk('Panel', olCardTop);
    olCardChip.style.padding = '3px 8px'; olCardChip.style.borderRadius = '3px'; olCardChip.style.verticalAlign = 'center';
    var olCardKind = lbl(olCardChip, '', S.label, 11);
    olCardKind.style.fontWeight = 'bold'; olCardKind.style.letterSpacing = '1px';
    var olCardSub = lbl(olCard, '', S.muted, 12);
    olCardSub.style.marginTop = '3px'; olCardSub.style.whiteSpace = 'nowrap'; olCardSub.style.textOverflow = 'ellipsis';
    olCardSub.style.maxWidth = '100%';

    // Group: rename (typed into the card), isolate, ungroup, delete.
    var olGroupBox = mk('Panel', olCard);
    olGroupBox.style.width = '100%'; olGroupBox.style.flowChildren = 'down';
    var olName = '', olNameFocus = false;
    var olNameField = mk('Panel', olGroupBox);
    olNameField.hittest = true; olNameField.style.width = '100%'; olNameField.style.height = '32px';
    olNameField.style.marginTop = '10px'; olNameField.style.backgroundColor = '#0c0e12ff';
    olNameField.style.border = '1px solid ' + S.tileBorder; olNameField.style.borderRadius = '3px';
    var olNameLabel = lbl(olNameField, '', S.value, 13);
    olNameLabel.hittest = false; olNameLabel.style.marginLeft = '10px'; olNameLabel.style.verticalAlign = 'center';
    function olNamePaint() {
      olNameLabel.text = olNameFocus ? olName + ' |' : olName;
      olNameLabel.style.color = olNameFocus ? S.value : S.label;
      olNameField.style.border = '1px solid ' + (olNameFocus ? ENV_ACCENT : S.tileBorder);
    }
    function olRenameGroup(from, to) {
      if (!to || to === from) return;
      olMembers(from).forEach(function (i) { olSetGroup(i, to); });
      olOpen[to] = olOpen[from]; olSel = 'g:' + to;
      if (olIsolate === from) olIsolate = to;
    }
    var olPreviousBlur = configWindowBlur, olPreviousChars = configWindowChars;
    configWindowBlur = function () { olNameFocus = false; olNamePaint(); olPreviousBlur(); };
    configWindowChars = function (csv) {
      if (!olNameFocus) { olPreviousChars(csv); return; }
      String(csv || '').split(',').forEach(function (part) {
        var n = parseInt(part, 10);
        if (n === 13) { var from = olSel.substring(2); configWindowBlur(); olRenameGroup(from, olName); }
        else if (n === 27) { olName = olSel.substring(2); configWindowBlur(); }
        else if (n === 8) olName = olName.slice(0, -1);
        else if (olName.length < 40 && ((n >= 48 && n <= 57) || (n >= 65 && n <= 90)
                 || (n >= 97 && n <= 122) || n === 32 || n === 45 || n === 95)) olName += String.fromCharCode(n);
      });
      olNamePaint();
    };
    olNameField.SetPanelEvent('onactivate', function () {
      configWindowBlur(); olNameFocus = true; cfgTextOwner = olNameField;
      root.SetAttributeString('textcapture', '1'); olNamePaint();
    });
    var olGroupActs = actionRowEnd(olGroupBox, 10);
    var olIsolateBtn = actionBtn(olGroupActs, 'Isolate', function () {
      var name = olSel.substring(2);
      if (olIsolate === name) olEndIsolate(); else { if (olIsolate) olEndIsolate(); olStartIsolate(name); }
    });
    actionBtn(olGroupActs, 'Ungroup', function () {
      var name = olSel.substring(2);
      if (olIsolate === name) olEndIsolate();
      olMembers(name).forEach(function (i) { olSetGroup(i, ''); });
      olSel = '';
    });
    olDanger(actionBtn(olGroupActs, 'Delete group', function () {
      var g = olSel.substring(2); olConfirmRemove(olMembers(g), 'the group “' + g + '”');
    }));
    // The danger style: a red caption on a red edge (Remove, Delete group).
    function olDanger(b) {
      b.__fgBase = '#e8a0a0ff'; b.__brBase = '#c92a2a66'; paintBtn(b);
      return b;
    }

    // CS2 asset: size and turn, then the moves that are awkward to drag.
    var olAssetBox = mk('Panel', olCard);
    olAssetBox.style.width = '100%'; olAssetBox.style.flowChildren = 'down'; olAssetBox.style.marginTop = '6px';
    var olAssetSize = sliderRow(olAssetBox, {
      label: 'Size', min: 0.1, max: 5, decimals: 2, unit: '×', tip: 'Scale of the model. 1× is its size in the game.',
      onChange: function (v) { var it = olFind(olSel); if (it && it.src === 'p') propsCommand('set ' + it.id + ' scale ' + v); }
    });
    var olAssetYaw = sliderRow(olAssetBox, {
      label: 'Turn', min: -180, max: 180, decimals: 0, unit: '°', tip: 'Which way it faces.',
      onChange: function (v) { var it = olFind(olSel); if (it && it.src === 'p') propsCommand('set ' + it.id + ' yaw ' + v); }
    });
    var olAssetTurn = lbl(olAssetBox, '', S.muted, 12);
    olAssetTurn.style.marginTop = '8px'; olAssetTurn.style.marginLeft = UI.line.padH + 'px';
    var olAssetActs = actionRowEnd(olAssetBox, 10);
    actionBtn(olAssetActs, 'Drop to floor', function () { olAct(olFind(olSel), 'floor'); });
    actionBtn(olAssetActs, 'To crosshair', function () { olAct(olFind(olSel), 'here'); });
    var olUprightBtn = actionBtn(olAssetActs, 'Stand upright', function () { olAct(olFind(olSel), 'upright'); });
    // Light and mesh: their inspectors land in these hosts.
    var olLightHost = mk('Panel', olCard);
    olLightHost.style.width = '100%'; olLightHost.style.flowChildren = 'down';
    var olMeshHost = mk('Panel', olCard);
    olMeshHost.style.width = '100%'; olMeshHost.style.flowChildren = 'down';
    // What a light, mesh or camera rides, and its pose keys (ConfigHudRigJs.h).
    var olRigHost = mk('Panel', olCard);
    olRigHost.style.width = '100%'; olRigHost.style.flowChildren = 'down';

    // Group membership, for one item: the existing groups, none, or a new one.
    var olGroupDrop = dropdownRow(olCard, 'Group', [], function (g) {
      var it = olFind(olSel);
      if (!it) return;
      if (g === '+') { var name = olNewGroupName(); olSetGroup(it, name); olOpen[name] = true; }
      else olSetGroup(it, g);
    });
    olGroupDrop.row.style.marginTop = '10px';
    var olGroupSig = null;
    // Several selected: group them, hide them or remove them together.
    var olMultiActs = actionRowEnd(olCard, 12);
    actionBtn(olMultiActs, 'New group from these', function () {
      var picked = olPicked(), name = olNewGroupName();
      picked.forEach(function (i) { olSetGroup(i, name); });
      olOpen[name] = true; olSel = 'g:' + name; olMulti = {};
    });
    actionBtn(olMultiActs, 'Hide these', function () { olPicked().forEach(function (i) { if (i.visible) olSet(i, 'visible', false); }); });
    var olMultiRemove = olDanger(actionBtn(olMultiActs, 'Remove', function () { var p = olPicked(); olConfirmRemove(p, p.length + ' selected objects'); }));
    olMultiActs.visible = false;
    var olCardHint = lbl(olCard, 'Right-click for more  ·  Del  ·  Ctrl+C / V  ·  Ctrl+D', S.hint, 11);
    olCardHint.style.marginTop = '12px'; olCardHint.style.horizontalAlign = 'center';
)CFJS"
R"CFJS(
    // Automation / netcon: synthetic clicks never reach Panorama in game, so the outliner's actions
    // are reachable by item key (p<id> = CS2 asset, e<id> = Environment light or mesh).
    $.ConfigHudOutliner = {
      // Arguments go through evalArg: the console tokenizer pads quoted strings with spaces.
      select: function (key) { olSelect(evalArg(key)); return olSel; },
      group: function (key, name) { var i = olFind(evalArg(key)); if (i) olSetGroup(i, evalArg(name || '')); return !!i; },
      toggle: function (key, field) { var i = olFind(evalArg(key)); field = evalArg(field); if (i) olSet(i, field, !i[field]); return !!i; },
      isolate: function (name) { name = evalArg(name || ''); if (name) olStartIsolate(name); else olEndIsolate(); return olIsolate; },
      selectGroup: function (name) { olSel = 'g:' + evalArg(name); olSig = ''; return olSel; },
      menu: function (key) { var i = olFind(evalArg(key)); if (i) { olSelect(i.key); olMenu(i); } return !!i; },
      key: function (name) { $.ConfigHud.key(evalArg(name)); return olSel; },
      page: function (i) { envSceneSec.__setPage(parseInt(evalArg(i), 10) || 0); return 'ok'; },
      info: function () { $.Msg('outliner: sel=' + olSel + ' isolate=' + olIsolate + ' items=' + olItemsNow.map(function (i) {
        return i.key + ':' + i.group + ':' + (i.visible ? 'v' : '-') + (i.render ? 'r' : '-') + (i.locked ? 'l' : '-'); }).join(',')); }
    };

    // ---- render: mirror both backends into the tree and the card ----------------------------------
    renderHooks.push(function (st) {
      if (!st || !st.props) return;
      var items = olItems(st);
      olItemsNow = items;
      // A pick in the view (props) or a selection elsewhere (environment) moves the outliner's too.
      var ps = st.props.selected || 0, es = st.envSelected || 0;
      if (ps !== olLastProp) { olLastProp = ps; if (ps) { olSel = 'p' + ps; olMulti = {}; } }
      if (es !== olLastEnv) { olLastEnv = es; if (es) { olSel = 'e' + es; olMulti = {}; } }
      if (olSel && olSel.indexOf('g:') !== 0 && !olFind(olSel)) olSel = '';
      if (olSel.indexOf('g:') === 0 && !olMembers(olSel.substring(2)).length) olSel = '';
      if (olIsolate && !olMembers(olIsolate).length) { olIsolate = ''; olIsolateMemo = null; }
      olFilters.sync(olFilter);

      olShift = !!st.shift; olCtrl = !!st.ctrl;
      for (var mk2 in olMulti) if (!olFind(mk2)) delete olMulti[mk2];
      var sig = olTreeSig(items) + '|' + JSON.stringify(olOpen) + '|' + olFilter + '|' + olSel + '|' + olIsolate
        + '|' + Object.keys(olMulti).join(',');
      if (sig !== olSig) {
        olSig = sig;
        olOrder = [];
        olRows.RemoveAndDeleteChildren();
        var groups = [], byGroup = {}, loose = [];
        items.forEach(function (i) {
          if (!i.group) { loose.push(i); return; }
          if (!byGroup[i.group]) { byGroup[i.group] = []; groups.push(i.group); }
          byGroup[i.group].push(i);
        });
        groups.sort(function (a, b) { return a.toLowerCase() < b.toLowerCase() ? -1 : 1; });
        var match = function (i) { return olFilter === 'all' || i.kind === olFilter; };
        var itemRow = function (i, child, parentHidden) {
          olRow({
            key: i.key, child: child, indent: child ? 34 : 30, name: i.name, kind: i.kind,
            selected: olIsPicked(i.key), dim: !i.visible || parentHidden,
            visible: i.visible, render: i.render, locked: i.locked,
            select: function () { olClick(i.key); },
            // Right-click keeps a multi-selection that the row belongs to (its menu acts on all).
            menu: function () { if (!olIsPicked(i.key)) { olMulti = {}; olSelect(i.key); } olMenu(olFind(i.key)); },
            toggleEye: function () { olSet(i, 'visible', !i.visible); },
            toggleRender: function () { olSet(i, 'render', !i.render); },
            toggleLock: function () { olSet(i, 'locked', !i.locked); }
          });
        };
        groups.forEach(function (g) {
          var members = byGroup[g], shown = members.filter(match);
          if (olFilter !== 'all' && !shown.length) return;
          var anyVisible = members.some(function (i) { return i.visible; });
          var allRender = members.every(function (i) { return i.render; });
          var allLocked = members.every(function (i) { return i.locked; });
          // A big group (a painted patch) starts closed; open it with its arrow.
          if (olOpen[g] === undefined) olOpen[g] = members.length <= 8;
          olRow({
            group: true, indent: 4, name: g, count: String(members.length), open: !!olOpen[g],
            selected: olSel === 'g:' + g, dim: !anyVisible,
            visible: anyVisible, render: allRender, locked: allLocked,
            toggleOpen: function () { olOpen[g] = !olOpen[g]; olSig = ''; },
            select: function () { olSel = 'g:' + g; olName = g; olNamePaint(); olSig = ''; },
            menu: function () { olSel = 'g:' + g; olSig = ''; olGroupMenu(g); },
            toggleEye: function () { members.forEach(function (i) { if (i.visible === anyVisible) olSet(i, 'visible', !anyVisible); }); },
            toggleRender: function () { members.forEach(function (i) { if (i.render === allRender) olSet(i, 'render', !allRender); }); },
            toggleLock: function () { members.forEach(function (i) { if (i.locked === allLocked) olSet(i, 'locked', !allLocked); }); }
          });
          if (olOpen[g]) shown.forEach(function (i) { itemRow(i, true, !anyVisible); });
        });
        loose.filter(match).forEach(function (i) { itemRow(i, false, false); });
      }
      olEmpty.visible = !items.length;
      olTree.visible = !!items.length;
      olIsoBar.visible = !!olIsolate;
      olIsoText.text = olIsolate ? 'Isolating ' + olIsolate + ' — everything else is hidden' : '';
      // The lights' line: only with a light in the scene.
      var hasLight = items.some(function (i) { return i.kind === 'light'; });
      olLitBar.visible = hasLight;
      if (hasLight) {
        var es2 = envState(), ec = envClimate();
        var lit = !!(es2 && es2.enabled && ec && ec.enabled && ec.relighting);
        olLitDot.style.backgroundColor = lit ? ENV_ACCENT : '#e0b861ff';
        olLitText.text = lit ? 'Lights draw with Environment lighting' : 'Lights need Environment lighting (off now)';
      }

      // The card.
      var isGroup = olSel.indexOf('g:') === 0, it = isGroup ? null : olFind(olSel);
      var pickedNow = isGroup ? [] : olPicked();
      olCard.visible = isGroup || !!it;
      var canGroup = !!pickedNow.length && !(pickedNow.length === 1 && !!pickedNow[0].group);
      setBtnDisabled(olGroupBtn, !canGroup);
      olGroupBtn.style.opacity = canGroup ? '1' : '0.45';
      if (!olCard.visible) return;
      if (pickedNow.length > 1) {
        // Several selected: what they share, and the bulk actions.
        olCardTitle.text = pickedNow.length + ' objects selected';
        olCardKind.text = 'SELECTION'; olCardKind.style.color = '#e0b861ff'; olCardChip.style.backgroundColor = '#e0b8611f';
        olCardSub.text = 'Shift-click adds a range, Ctrl-click one more.';
        olGroupBox.visible = olAssetBox.visible = olLightHost.visible = olMeshHost.visible = false;
        olGroupDrop.row.visible = false; olCardHint.visible = false;
        olMultiActs.visible = true;
        olMultiRemove.__lbl.text = 'Remove ' + pickedNow.length;
        return;
      }
      olMultiActs.visible = false;
      var kind = isGroup ? ['#e0b861', 'GROUP'] : [OL_KIND[it.kind][0], OL_KIND[it.kind][1].toUpperCase()];
      olCardTitle.text = isGroup ? olSel.substring(2) : it.name;
      olCardKind.text = kind[1]; olCardKind.style.color = kind[0] + 'ff';
      olCardChip.style.backgroundColor = kind[0] + '1f';
      olCardSub.text = isGroup ? olMembers(olSel.substring(2)).length + ' objects  ·  rename, isolate or ungroup'
        : it.kind === 'asset' ? ((it.source ? it.source + '  ·  ' : '') + (it.folder || ''))
        : it.kind === 'light' ? 'Lit by the Environment lighting'
        : it.kind === 'camera' ? 'A viewpoint: attach it, key it or look through it' : 'Your mesh';
      olGroupBox.visible = isGroup;
      olGroupDrop.row.visible = !isGroup;
      olCardHint.visible = !isGroup;
      olAssetBox.visible = !!it && it.kind === 'asset';
      olLightHost.visible = !!it && it.kind === 'light';
      olMeshHost.visible = !!it && it.kind === 'mesh';
      if (isGroup) {
        if (!olNameFocus) { olName = olSel.substring(2); olNamePaint(); }
        olIsolateBtn.__lbl.text = olIsolate === olSel.substring(2) ? 'Show all' : 'Isolate';
        return;
      }
      if (it.kind === 'asset') {
        var a = it.ang || [0, 0, 0], deg = function (x) { return Math.round(x) + '°'; };
        olAssetSize.sync(it.scale || 1);
        olAssetYaw.sync(a[1]);
        olAssetTurn.text = 'Tilt ' + deg(a[0]) + '   Roll ' + deg(a[2]) + '   ·   drag the rings in the view';
        var upright = Math.abs(a[0]) < 0.5 && Math.abs(a[2]) < 0.5;
        setBtnDisabled(olUprightBtn, upright);
        olUprightBtn.style.opacity = upright ? '0.45' : '1';
      }
      var names = [];
      items.forEach(function (i) { if (i.group && names.indexOf(i.group) < 0) names.push(i.group); });
      names.sort();
      var gsig = names.join('|');
      if (gsig !== olGroupSig) {
        olGroupSig = gsig;
        olGroupDrop.setItems([['', 'None']].concat(names.map(function (g) { return [g, g]; })).concat([['+', '+ New group']]));
      }
      olGroupDrop.sync(it.group || '');
    });
)CFJS"
