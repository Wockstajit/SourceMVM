// Comparison harness -- drives the mockup from the URL so a game-vs-mockup screenshot pass can be
// scripted, and reports when the surface has settled so a capture is never taken mid-build.
//
// It is deliberately standalone: app.js does not know this file exists beyond the handful of
// accessors it publishes on window.MvmMockup. Delete this file and the mockup is unchanged.
//
//   index.html?surface=cameraeditor              -- pick a surface
//   index.html?surface=cameraeditor&bare=1       -- hide the harness chrome, stage fills the window
//   index.html?surface=cameraeditor&zoom=1       -- pin the stage to true 1:1 (default in bare mode)
//   index.html?surface=confighud&act=Effects     -- run a sidebar action/state button by its label
//   index.html?surface=cameraeditor&hold=1200    -- extra settle time in ms before ready
//   index.html?surface=cameraeditor&cmd=mirv_filmmaker dock timeline
//                                                -- send console command(s), ';'-separated, after settle
//   index.html?surface=demobar&drag=MirvSpeedRow,60 -- press on a panel's centre and drag it +60px
//   index.html?surface=confighud&click=pid:LoopBtn -- hit-test and click a real Panorama control
//   index.html?surface=confighud&click=label:HANDHELD CAMERA -- labels resolve to their control
//   index.html?surface=x&act=State one|State two  -- several sidebar buttons, in order
//
// window.MvmCompare.ready resolves once the surface is built and settled. Headless capture polls
// document.title, which gets a "  [READY]" suffix at the same moment.
(function () {
  'use strict';

  var qs = {};
  location.search.replace(/^\?/, '').split('&').forEach(function (kv) {
    if (!kv) return;
    var i = kv.indexOf('=');
    var k = i < 0 ? kv : kv.slice(0, i);
    var v = i < 0 ? '1' : decodeURIComponent(kv.slice(i + 1).replace(/\+/g, ' '));
    qs[k] = v;
  });

  var Compare = window.MvmCompare = { query: qs, settled: false, log: [] };
  function note(s) { Compare.log.push(s); }

  // --------------------------------------------------------------------------- bare mode
  // A capture wants the 1920x1080 stage and nothing else: no sidebar, no dev panel, no caption,
  // no rounded viewport inset. Everything here is presentation-only.
  function applyBare() {
    var css = document.createElement('style');
    css.id = 'compare-bare';
    css.textContent = [
      '#sidebar, #devpanel { display: none !important; }',
      'body > div[style] { display: none !important; }',
      '#app { display: block !important; }',
      '#viewport { position: fixed !important; inset: 0 !important; padding: 0 !important;',
      '  margin: 0 !important; border: 0 !important; border-radius: 0 !important;',
      '  overflow: hidden !important; background: #000 !important; display: block !important; }',
      '#stagewrap { position: absolute !important; left: 0 !important; top: 0 !important; }',
      'html, body { overflow: hidden !important; background: #000 !important; }'
    ].join('\n');
    document.head.appendChild(css);
  }

  // --------------------------------------------------------------------------- settle detection
  // "Built" is not "settled": several surfaces retry their opening action for a few frames (the
  // Customize modal needs the editor to lay out first), and video posters swap in late. Waiting on
  // a quiet DOM beats a fixed sleep -- faster on simple panels, safer on slow ones.
  function whenQuiet(root, quietMs, maxMs) {
    return new Promise(function (resolve) {
      var last = performance.now();
      var started = last;
      var mo = new MutationObserver(function () { last = performance.now(); });
      mo.observe(root, { childList: true, subtree: true, attributes: true, characterData: true });
      (function poll() {
        var now = performance.now();
        if (now - last >= quietMs || now - started >= maxMs) { mo.disconnect(); resolve(now - started); return; }
        setTimeout(poll, 60);
      })();
    });
  }

  function waitFor(fn, timeoutMs) {
    return new Promise(function (resolve, reject) {
      var t0 = performance.now();
      (function poll() {
        var v;
        try { v = fn(); } catch (e) { v = null; }
        if (v) return resolve(v);
        if (performance.now() - t0 > (timeoutMs || 15000)) return reject(new Error('timed out'));
        setTimeout(poll, 50);
      })();
    });
  }

  // A sidebar action/state button, found by its visible label. The harness clicks the real button,
  // so the same code path runs that a person clicking it would take.
  function clickAction(label) {
    var want = String(label).trim().toLowerCase();
    var btns = [].slice.call(document.querySelectorAll('#actions button'));
    var hit = btns.filter(function (b) { return b.textContent.trim().toLowerCase() === want; })[0]
           || btns.filter(function (b) { return b.textContent.trim().toLowerCase().indexOf(want) >= 0; })[0];
    if (!hit) {
      note('action not found: ' + label + ' (have: ' + btns.map(function (b) { return b.textContent.trim(); }).join(' | ') + ')');
      return false;
    }
    hit.click();
    return true;
  }
  Compare.act = clickAction;
  Compare.actions = function () {
    return [].slice.call(document.querySelectorAll('#actions button')).map(function (b) { return b.textContent.trim(); });
  };

  // Find the same element a person points at, then click the element the browser actually hits at
  // its centre. Calling a Panorama handler directly would let an invisible overlay pass the test.
  function clickControl(spec) {
    var host = window.MvmMockup.host();
    var i = String(spec).indexOf(':');
    var kind = i < 0 ? 'pid' : String(spec).slice(0, i).toLowerCase();
    var want = (i < 0 ? spec : String(spec).slice(i + 1)).trim();
    var el = null;
    if (kind === 'pid') {
      el = host.querySelector('[data-pid="' + CSS.escape(want) + '"]');
    } else if (kind === 'label') {
      var norm = function (s) { return String(s || '').trim().replace(/\s+/g, ' ').toLowerCase(); };
      var labels = [].slice.call(host.querySelectorAll('.pnl')).filter(function (n) {
        return norm(n.textContent) === norm(want);
      });
      // Prefer a label that belongs to a CONTROL (an ancestor carries onactivate), then the leaf
      // text panel. Two panels can share a caption -- the Render workspace's HUD tab and the job
      // column's HUD readout -- and a person clicks the one that is a button, not the heading.
      // "Belongs to a control" means the NEAREST activatable ancestor is small. A column that
      // swallows stray clicks (the job column does) is activatable too, and a caption inside it
      // would otherwise tie with a real button -- so rank by the area of that nearest owner.
      var ownerArea = function (n) {
        for (var a = n; a && a !== host; a = a.parentElement) {
          if (a.__panel && a.__panel._events && a.__panel._events.onactivate) {
            var r = a.getBoundingClientRect();
            return r.width * r.height;
          }
        }
        return Infinity;
      };
      // Only captions a person can SEE compete: the same segment captions (Constant, Freeze, ...)
      // also live in the editor's collapsed Path card, whose smaller buttons would otherwise win
      // and the click would land on nothing.
      var laidOut = function (n) { return n.getClientRects().length > 0 && n.getBoundingClientRect().width > 0; };
      labels = labels.filter(laidOut).concat(labels.filter(function (n) { return !laidOut(n); }));
      labels.sort(function (a, b) {
        return (laidOut(b) - laidOut(a)) || (ownerArea(a) - ownerArea(b)) || (a.children.length - b.children.length);
      });
      el = labels[0] || null;
      var labelled = el;
      while (el && !(el.__panel && el.__panel._events && el.__panel._events.onactivate)) el = el.parentElement;
      // Toggle captions and their pill are siblings. At the nearest shared row, choose its
      // activatable descendant—the pill—just as clicking the visible setting control would.
      if (!el && labelled) {
        for (var row = labelled.parentElement; row && row !== host && !el; row = row.parentElement) {
          el = [].slice.call(row.querySelectorAll('.pnl')).filter(function (n) {
            return n.__panel && n.__panel._events && n.__panel._events.onactivate;
          })[0] || null;
        }
      }
    }
    if (!el) { note('click target not found: ' + spec); return false; }
    el.scrollIntoView({ block: 'center', inline: 'nearest' });
    var r = el.getBoundingClientRect();
    var hit = document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2);
    if (!hit || !(hit === el || el.contains(hit))) {
      note('click blocked: ' + spec + ' by ' + (hit && (hit.getAttribute('data-pid') || hit.textContent.trim().slice(0, 30)) || 'nothing'));
      return false;
    }
    hit.click();
    return true;
  }
  Compare.click = clickControl;

  // --------------------------------------------------------------------------- geometry dump
  // Every laid-out panel with its rect in STAGE coordinates (1920x1080), so two runs can be
  // compared numerically rather than by eye -- and so a fix can be verified without a screenshot.
  Compare.geom = function () {
    var stage = window.MvmMockup.stage();
    var k = window.MvmMockup.stageScale || 1;
    var base = stage.getBoundingClientRect();
    var out = [];
    [].forEach.call(window.MvmMockup.host().querySelectorAll('.pnl'), function (el) {
      var r = el.getBoundingClientRect();
      if (r.width < 0.5 && r.height < 0.5) return;
      var cs = getComputedStyle(el);
      out.push({
        id: el.getAttribute('data-pid') || el.id || '',
        x: +((r.left - base.left) / k).toFixed(1),
        y: +((r.top - base.top) / k).toFixed(1),
        w: +(r.width / k).toFixed(1),
        h: +(r.height / k).toFixed(1),
        text: (el.childElementCount ? '' : (el.textContent || '')).trim().slice(0, 40),
        bg: cs.backgroundColor,
        fg: cs.color,
        fs: cs.fontSize
      });
    });
    return out;
  };

  // --------------------------------------------------------------------------- tree dump
  // The same pre-order walk Automation/livejs/dump-ui-geometry.js performs in the game, emitting the
  // same columns, so the two can be aligned index for index. Both sides run the identical panel
  // script, so the trees MUST match -- a divergence in the walk is itself the finding, and the rects
  // then say whether the shim laid the matched panel out where Panorama did.
  //
  // Rects are stage px (the 1920x1080 space the game reports in). Invisible and zero-size panels are
  // kept, because "present but collapsed" and "never created" are different bugs.
  var ROOT_IDS = ['CamEditorRoot', 'CamEditorRenderRoot', 'CamEditorConfirmRoot', 'CamEditorDebugRoot',
                  'ConfigHudRoot', 'CamTimelineRoot', 'CamTimelineRangeRoot', 'GraphExpRoot',
                  'MarkerHudRoot', 'MovieHudRoot', 'AimXhairRoot', 'FxDebugHudRoot',
                  // the rebuilt native demo bar: not in the game's dump (compare-geometry skips it), but
                  // verify-flow needs to see whether the docking put it back
                  'CSGOHudDemoController', 'JsMainMenuContent', 'MainMenuNavBar', 'MainMenuFriends', 'WorkshopModePopup', 'NativeMenuConfirm'];

  Compare.geomTree = function () {
    var k = window.MvmMockup.stageScale || 1;
    var base = window.MvmMockup.stage().getBoundingClientRect();
    var host = window.MvmMockup.host();
    var roots = {};

    function typeOf(el) {
      var m = /(?:^|\s)pnl-([A-Za-z0-9]+)/.exec(el.className || '');
      return m ? m[1] : 'Panel';
    }
    function textOf(el) {
      var t = el.querySelector(':scope > .pnl-text');
      return t ? String(t.textContent).replace(/[\t\r\n]+/g, ' ').slice(0, 60) : '';
    }

    // Two quantities, measured two ways, because Panorama reports them two ways.
    // actuallayoutwidth is the LAYOUT box and a transform never changes it -- a timeline diamond
    // (a 14px square rotated 45 degrees) is 14 there and 19.8 in a bounding rect -- so SIZE comes
    // from offsetWidth/offsetHeight. GetPositionWithinWindow does follow placement, and the shim
    // places aligned panels with a transform, so POSITION comes from the bounding rect; reading
    // it off the offset chain instead reported 0,0 for every aligned (rather than flowed) panel,
    // which looked like the entire right-hand inspector had moved to the origin.
    ROOT_IDS.forEach(function (rid) {
      var root = host.querySelector('[data-pid="' + rid + '"]');
      if (!root) { roots[rid] = null; return; }
      var rows = [];
      (function walk(el, depth) {
        var r = el.getBoundingClientRect();
        var cs = getComputedStyle(el);
        rows.push({
          d: depth,
          id: el.getAttribute('data-pid') || '',
          ty: typeOf(el),
          x: +((r.left - base.left) / k).toFixed(1),
          y: +((r.top - base.top) / k).toFixed(1),
          w: +(el.offsetWidth).toFixed(1),
          h: +(el.offsetHeight).toFixed(1),
          v: cs.display === 'none' ? 0 : 1,
          t: textOf(el),
          fs: parseFloat(cs.fontSize) || 0
        });
        [].forEach.call(el.children, function (c) {
          if (c.classList && c.classList.contains('pnl')) walk(c, depth + 1);
        });
      })(root, 0);
      roots[rid] = rows;
    });
    return { win: { w: window.MvmMockup.stage().offsetWidth, h: 1080 }, roots: roots };
  };

  // --------------------------------------------------------------------------- edits export
  // The other half of the round trip. Tweak a panel in devtools (Elements panel, or a live style
  // edit here) and this lists what moved, as the Panorama style line that produces the same result
  // and a grep that finds the panel in AfxHookSource2/Filmmaker/Panorama. It works from the
  // snapshot the shim takes after every write the script makes, so only hand edits show up.
  //
  // Not every CSS property maps back one-to-one -- flex/grid placement is the shim's own doing and
  // is reported as-is with a note -- but sizes, margins, padding, colours, fonts, opacity and
  // z-index do, and those are what UI tuning is made of.
  var CSS_TO_PANORAMA = {
    width: 'width', height: 'height', 'min-width': 'minWidth', 'max-width': 'maxWidth',
    'min-height': 'minHeight', 'max-height': 'maxHeight',
    'margin-top': 'marginTop', 'margin-bottom': 'marginBottom', 'margin-left': 'marginLeft',
    'margin-right': 'marginRight', margin: 'margin',
    'padding-top': 'paddingTop', 'padding-bottom': 'paddingBottom', 'padding-left': 'paddingLeft',
    'padding-right': 'paddingRight', padding: 'padding',
    'background-color': 'backgroundColor', color: 'color', opacity: 'opacity', 'z-index': 'zIndex',
    'font-size': 'fontSize', 'font-weight': 'fontWeight', 'font-family': 'fontFamily',
    'letter-spacing': 'letterSpacing', 'line-height': 'lineHeight', 'text-align': 'textAlign',
    'text-transform': 'textTransform', 'white-space': 'whiteSpace',
    border: 'border', 'border-top': 'borderTop', 'border-bottom': 'borderBottom',
    'border-left': 'borderLeft', 'border-right': 'borderRight', 'border-radius': 'borderRadius',
    'box-shadow': 'boxShadow', transform: 'transform', 'transform-origin': 'transformOrigin'
  };
  var SHIM_OWNED = /^(display|flex-|grid-|justify-self|align-self|position|top|left|right|bottom|pointer-events)/;

  function parseCss(text) {
    var out = {};
    String(text || '').split(';').forEach(function (decl) {
      var i = decl.indexOf(':');
      if (i < 0) return;
      var k = decl.slice(0, i).trim(), v = decl.slice(i + 1).trim();
      if (k) out[k] = v;
    });
    return out;
  }

  function panelPath(el) {
    // Nearest ancestor with an id, then child indexes down to the panel: enough to find it in a
    // geometry dump or in devtools, and it survives the surface being rebuilt.
    var steps = [];
    var n = el;
    while (n && n.classList && n.classList.contains('pnl')) {
      var pid = n.getAttribute('data-pid');
      if (pid) { steps.unshift('#' + pid); break; }
      var p = n.parentElement;
      var idx = p ? [].indexOf.call(p.children, n) : 0;
      steps.unshift('[' + idx + ']');
      n = p;
    }
    return steps.join(' > ') || '(root)';
  }

  function nearestPid(el) {
    for (var n = el; n && n.getAttribute; n = n.parentElement) {
      var pid = n.getAttribute('data-pid');
      if (pid) return pid;
    }
    return '';
  }

  Compare.styleDiff = function () {
    var host = window.MvmMockup.host();
    var out = [];
    [].forEach.call(host.querySelectorAll('.pnl'), function (el) {
      if (el.__authored === undefined) return;
      var was = parseCss(el.__authored), now = parseCss(el.style.cssText);
      var changes = [];
      Object.keys(now).forEach(function (k) {
        if (was[k] !== now[k]) changes.push({ css: k, from: was[k] === undefined ? '' : was[k], to: now[k] });
      });
      Object.keys(was).forEach(function (k) {
        if (!(k in now)) changes.push({ css: k, from: was[k], to: '' });
      });
      var panel = el.__panel;
      var textNode = el.querySelector(':scope > .pnl-text');
      var textChange = null;
      if (panel && textNode && String(panel._text || '') !== textNode.textContent) {
        textChange = { from: String(panel._text || ''), to: textNode.textContent };
      }
      if (!changes.length && !textChange) return;
      out.push({
        path: panelPath(el), pid: el.getAttribute('data-pid') || '', near: nearestPid(el),
        type: (/pnl-([A-Za-z]+)/.exec(el.className) || [])[1] || 'Panel',
        text: textNode ? textNode.textContent.slice(0, 40) : '',
        changes: changes, textChange: textChange
      });
    });
    return out;
  };

  Compare.exportEdits = function () {
    var diffs = Compare.styleDiff();
    var cur = window.MvmMockup.current();
    var lines = [];
    if (!diffs.length) {
      return ['No hand edits on this surface.', '',
              'Change a panel in the Elements panel (or its .style in the console) and this lists the',
              'Panorama line for it. Edits to generated/<surface>.js are the other route -- those go',
              'straight into the headers with:  node UiMockup/apply-ui.mjs ' + (cur ? cur.id : '<surface>')].join('\n');
    }
    lines.push('HAND EDITS ON ' + (cur ? cur.label.toUpperCase() : '?') + '  (' + diffs.length + ' panel' + (diffs.length === 1 ? '' : 's') + ')');
    lines.push('Paste the .style lines next to the panel in the header the grep finds, then rebuild.');
    lines.push('');
    diffs.forEach(function (d) {
      lines.push(d.path + '   ' + d.type + (d.text ? '  "' + d.text + '"' : ''));
      d.changes.forEach(function (c) {
        var pano = CSS_TO_PANORAMA[c.css];
        var line = '   ' + c.css + ': ' + (c.from || '(unset)') + '  ->  ' + (c.to || '(unset)');
        // display and pointer-events are not styles in Panorama at all -- they are the panel's
        // visible / hittest flags, and that is how a hand edit to them is carried across.
        if (c.css === 'display') line += '\n      .visible = ' + (c.to === 'none' ? 'false' : 'true') + ';';
        else if (c.css === 'pointer-events') line += '\n      .hittest = ' + (c.to === 'none' ? 'false' : 'true') + ';';
        else if (pano) line += '\n      .style.' + pano + " = '" + c.to + "';";
        else if (SHIM_OWNED.test(c.css)) line += '\n      (placement is the shim\'s own layout; in Panorama this is horizontalAlign / verticalAlign / flowChildren / ignoreParentFlow)';
        else line += '\n      (no direct Panorama equivalent -- carry the intent, not the property)';
        lines.push(line);
      });
      if (d.textChange) {
        lines.push('   text: "' + d.textChange.from + '"  ->  "' + d.textChange.to + '"');
        lines.push("      .text = '" + d.textChange.to.replace(/'/g, "\\'") + "';");
      }
      var needle = d.pid || d.near;
      if (needle) lines.push('   find: rg -n "' + needle + '" AfxHookSource2/Filmmaker/Panorama/*Js.h');
      lines.push('');
    });
    return lines.join('\n');
  };


  // --------------------------------------------------------------------------- driving
  // Console commands go through the same router the panels' own buttons use, so ?cmd= exercises
  // exactly the path a click would. ?drag= feeds the stage's mouse pipe (the raw-cursor feed the
  // sliders and the speed dial read), which is the only way to test a drag without a person.
  Compare.cmd = function (c) { window.PanoramaShim.onCommand(String(c).trim()); window.MvmMockup.tick(3); };
  Compare.drag = function (pid, dx, dy) {
    return new Promise(function (resolve) {
      var el = window.MvmMockup.host().querySelector('[data-pid="' + pid + '"]');
      if (!el) { note('drag: no panel ' + pid); resolve(false); return; }
      var stage = window.MvmMockup.stage();
      var r = el.getBoundingClientRect();
      var x = r.left + r.width / 2, y = r.top + r.height / 2;
      function ev(type, tx, ty, target) {
        (target || stage).dispatchEvent(new MouseEvent(type, { bubbles: true, clientX: tx, clientY: ty, button: 0 }));
        window.MvmMockup.tick(2); // the raw-cursor pump reads the mouse once per frame
      }
      var steps = 12, k = 0;
      ev('mousemove', x, y);
      setTimeout(function () {
        ev('mousedown', x, y);
        (function step() {
          k++;
          ev('mousemove', x + (dx || 0) * k / steps, y + (dy || 0) * k / steps);
          if (k < steps) { setTimeout(step, 30); return; }
          setTimeout(function () { ev('mouseup', x + (dx || 0), y + (dy || 0), window); setTimeout(function () { resolve(true); }, 120); }, 30);
        })();
      }, 60);
    });
  };

  // --------------------------------------------------------------------------- click audit
  // ?clickaudit=1 -- for UiMockup/verify-clicks.mjs. For every panel carrying an onactivate:
  // is it the top-most element at its own centre (can a real click land on it), and does clicking
  // it fire exactly one handler (Panorama activates the panel that was hit; a DOM click bubbles).
  Compare.clickAudit = function () {
    var host = window.MvmMockup.host();
    var out = { total: 0, unreachable: [], bubbled: [] };
    var all = [].slice.call(host.querySelectorAll('.pnl')).filter(function (el) {
      var p = el.__panel;
      if (!p || !p._events || !p._events.onactivate) return false;
      var r = el.getBoundingClientRect();
      return r.width >= 1 && r.height >= 1 && getComputedStyle(el).visibility !== 'hidden';
    });
    // Which panel ROOT an element belongs to. A backdrop covered by its own panel's content is
    // how both the game and the mockup are built (the inspector column swallows stray clicks and
    // the content sits over it). A control covered by a DIFFERENT root is two panels stacked,
    // which never happens in game -- that is the fault worth failing on.
    // Roots that are SUPPOSED to sit over another one: the camera timeline docks into the editor's
    // bottom band, the graph replaces the timeline there, and the render workspace covers the
    // editor. Those overlaps are the game's own layout, not two panels fighting.
    var DOCKS_OVER = {
      'CamTimelineRoot|CamEditorRoot': 1,
      'GraphExpRoot|CamEditorRoot': 1,
      'GraphExpRoot|CamTimelineRoot': 1,
      'CamEditorRenderRoot|CamEditorRoot': 1,
      // The always-alive overlays are hittest=false; they never take a click. A control they
      // "cover" is still reachable in game, so they are not a stacking fault either.
      'MarkerHudRoot|*': 1, 'MovieHudRoot|*': 1, 'AimXhairRoot|*': 1, 'FxDebugHudRoot|*': 1
    };
    function docksOver(over, under) { return !!(DOCKS_OVER[over + '|' + under] || DOCKS_OVER[over + '|*']); }
    // A modal is INPUT-EXCLUSIVE: while one is up the controls behind it are MEANT to be
    // unreachable, so "covered by a modal" is the game working rather than a fault. Without this
    // the Customize modal could not be audited at all -- all 55 editor controls behind it read as
    // covered -- which is why `customize` was left off the surface list instead of checked.
    // The ranks are the documented z-layer map in
    // AfxHookSource2/Filmmaker/Panorama/PanoramaBridge.h, which also carries the one ordering the
    // game depends on: the customize "reset to default" confirm (260) has to outrank the
    // Customize modal (230) that opens it.
    // Ranking rather than testing the name is what keeps this from excusing the real fault. A
    // modal's OWN control covered by a root that does not outrank it -- GraphExpRoot painting
    // over the Customize modal, the bug this surface was fixed for -- still fails, because the
    // graph root ranks 0.
    var MODAL_Z = {
      CamEditorConfirmRoot: 220, CamEditorSettingsRoot: 221,
      CamEditorCustomizeRoot: 230, CamEditorCustomizeConfirmRoot: 260
    };
    function modalRank(pid) { return MODAL_Z[pid] || 0; }
    function rootOf(el) {
      for (var n = el; n && n !== host; n = n.parentElement) {
        var pid = n.getAttribute && n.getAttribute('data-pid');
        if (pid && /Root$|^CSGOHudDemoController$/.test(pid)) return pid;
      }
      return '?';
    }
    function label(el) {
      var pid = el.getAttribute('data-pid');
      var t = (el.textContent || '').trim().replace(/\s+/g, ' ').slice(0, 30);
      return (pid ? '#' + pid + ' ' : '') + (t ? '"' + t + '"' : '(no text)');
    }
    out.total = all.length;

    // Which panel roots are alive on this surface. Two roots that both claim the screen is a
    // 1:1 fault in itself -- in game only one workspace is up at a time.
    out.surface = window.MvmMockup.current() && window.MvmMockup.current().id;
    out.apis = ['ConfigHud','CamEditor','CamTimeline','GraphExp','MarkerHud','MovieHud']
      .filter(function (n) { return !!window.$[n]; });
    out.roots = [];
    // The modal roots are listed too, so a case that MEANT to open one can prove it actually did.
    // A popup that silently failed to open would otherwise pass every check by having nothing to
    // check -- the audit would report a clean editor and call it a clean modal.
    ['CamEditorRoot', 'ConfigHudRoot', 'CamTimelineRoot', 'GraphExpRoot', 'CamEditorRenderRoot',
     'MarkerHudRoot', 'MovieHudRoot', 'CSGOHudDemoController',
     'CamEditorConfirmRoot', 'CamEditorSettingsRoot', 'CamEditorCustomizeRoot',
       'CamEditorCustomizeConfirmRoot', 'JsMainMenuContent', 'MainMenuNavBar', 'MainMenuFriends'].forEach(function (id) {
      var e = host.querySelector('[data-pid="' + id + '"]');
      if (!e) return;
      var r = e.getBoundingClientRect();
      if (getComputedStyle(e).display === 'none' || r.width < 1) return;
      out.roots.push(id + ' ' + Math.round(r.width) + 'x' + Math.round(r.height));
    });

    all.forEach(function (el) {
      var r = el.getBoundingClientRect();
      var hit = document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2);
      if (!hit || !(el === hit || el.contains(hit))) {
        // Distinguish 'scrolled out of view' (the game scrolls too -- not a fault) from 'another
        // panel is sitting on top of it' (which is).
        var offscreen = r.bottom < 0 || r.top > innerHeight || r.right < 0 || r.left > innerWidth;
        var clipped = false;
        for (var a = el.parentElement; a && a !== host; a = a.parentElement) {
          var acs = getComputedStyle(a);
          if (acs.overflowY === 'auto' || acs.overflowY === 'hidden' || acs.overflowX === 'hidden') {
            var ar = a.getBoundingClientRect();
            if (r.bottom < ar.top || r.top > ar.bottom || r.right < ar.left || r.left > ar.right) { clipped = true; break; }
          }
        }
        function rectOf(e){ var q=e.getBoundingClientRect(); return Math.round(q.left)+','+Math.round(q.top)+' '+Math.round(q.width)+'x'+Math.round(q.height); }
        out.unreachable.push({ label: label(el), covering: hit ? label(hit) : 'nothing',
          coverRect: hit ? rectOf(hit) : '-',
          // A big container that swallows clicks (the inspector column does exactly this) is
          // always 'covered' at its centre by whatever content sits in it. That is containment,
          // not an overlap, and the container still receives the click by bubbling.
          rel: !hit ? 'none' : (el.contains(hit) ? 'own content'
            : (hit.contains(el) ? 'own container'
              : (rootOf(el) === rootOf(hit) ? 'same panel'
                : (modalRank(rootOf(hit)) > modalRank(rootOf(el)) ? 'behind a modal'
                  : (docksOver(rootOf(hit), rootOf(el)) ? 'docked over it' : 'unrelated panel'))))),
          rect: [Math.round(r.left), Math.round(r.top), Math.round(r.width), Math.round(r.height)],
          parent: label(el.parentElement),
          why: offscreen ? 'off-screen' : (clipped ? 'scrolled out of its container' : 'covered') });
      }
    });

    // The labels that CAN be clicked. A popup case needs this: "the modal root is up" does not
    // prove the button it exists to check is on screen, and a count alone cannot tell an armed
    // Config reset footer from an idle one -- that button relabels itself in place rather than
    // opening anything.
    var unreachable = {};
    out.unreachable.forEach(function (u) { unreachable[u.label] = 1; });
    out.reachable = all.map(label).filter(function (l) { return !unreachable[l]; });

    // Isolation: instrument every handler, click each control once, and see how many fired.
    var fired = [];
    all.forEach(function (el, idx) {
      var p = el.__panel, orig = p._events.onactivate;
      el.__auditOrig = orig;
      p._events.onactivate = function () { fired.push(idx); /* swallowed: no state change */ };
    });
    all.forEach(function (el, idx) {
      fired.length = 0;
      var r = el.getBoundingClientRect();
      el.dispatchEvent(new MouseEvent('click', { bubbles: true, clientX: r.left + r.width / 2, clientY: r.top + r.height / 2 }));
      if (fired.length > 1) {
        var others = fired.filter(function (f) { return f !== idx; }).map(function (f) { return label(all[f]); });
        if (others.length) out.bubbled.push({ label: label(el), also: others.join(' + ') });
      }
    });
    all.forEach(function (el) { el.__panel._events.onactivate = el.__auditOrig; delete el.__auditOrig; });
    return out;
  };

  // --------------------------------------------------------------------------- layer audit
  // Is anything HIDDEN behind something it should be in front of?
  //
  // This is not the click audit. That one asks whether a control can be reached by the mouse; a
  // panel with pointer-events:none is invisible to it yet still paints over what is underneath.
  // This asks the other question -- who actually paints here -- because a modal can be perfectly
  // clickable and still be covered.
  //
  // The fault it exists to catch: in Panorama a panel's z-index is scoped to its ROOT, so a child
  // asking for z230 inside a z53 root still loses to a z65 sibling root. That is exactly how the
  // Customize modal ended up under the graph editor, and how the "clear all" confirm did before
  // it was promoted to its own root. Sampling paint ownership is the only way to see it -- the
  // authored z-index reads correct in the source either way.
  Compare.layerAudit = function () {
    var host = window.MvmMockup.host();
    var out = { total: 0, covered: [], roots: [] };
    function rootOf(el) {
      for (var n = el; n && n !== host; n = n.parentElement) {
        var pid = n.getAttribute && n.getAttribute('data-pid');
        if (pid && /Root$|^CSGOHudDemoController$/.test(pid)) return pid;
      }
      return '?';
    }
    // Overlaps the game itself draws: the bottom editors dock into the editor's lower band, the
    // render workspace covers the editor, and a modal root is meant to cover everything.
    var MAY_COVER = {
      'CamTimelineRoot|CamEditorRoot': 1,
      'GraphExpRoot|CamEditorRoot': 1,
      'GraphExpRoot|CamTimelineRoot': 1,
      'CamEditorRenderRoot|CamEditorRoot': 1,
      'CamTimelineRangeRoot|CamTimelineRoot': 1,
      'CamTimelineRangeRoot|CSGOHudDemoController': 1
    };
    function mayCover(over, under) {
      if (over === under) return true;
      if (/Confirm|Settings|Customize/.test(over)) return true;   // modal roots cover by design
      // The always-alive overlays (the director card, the marker card, the reticle) sit over the
      // HUD in game too; they are translucent, hittest-free, and not a stacking fault.
      if (/^(MarkerHudRoot|MovieHudRoot|AimXhairRoot|FxDebugHudRoot)$/.test(over)) return true;
      return !!MAY_COVER[over + '|' + under];
    }
    // What layer does this panel BELIEVE it is on? The highest z-index asked for anywhere between
    // it and its root. Anything >= 100 is an overlay that means to sit above the HUD -- the modals
    // and fly-outs. That intent is the whole point: a z230 overlay is covered only because its
    // z is scoped to a z53 root, and without reading the intent the audit cannot tell that from
    // the graph legitimately docking over the editor's bottom band (both are
    // "GraphExpRoot paints where CamEditorRoot's subtree is").
    function intendedLayer(el) {
      var top = 0;
      for (var n = el; n && n !== host; n = n.parentElement) {
        var z = parseInt(getComputedStyle(n).zIndex, 10);
        if (!isNaN(z) && z > top) top = z;
        var pid = n.getAttribute && n.getAttribute('data-pid');
        if (pid && /Root$|^CSGOHudDemoController$/.test(pid)) break;
      }
      return top;
    }
    host.querySelectorAll('[data-pid]').forEach(function (e) {
      var pid = e.getAttribute('data-pid');
      if (!/Root$|^CSGOHudDemoController$/.test(pid)) return;
      var r = e.getBoundingClientRect();
      if (r.width < 1) return;
      out.roots.push(pid + ' z' + (getComputedStyle(e).zIndex || 'auto'));
    });
    // LEAF labels only -- a panel carrying its OWN text, not a container whose textContent is just
    // everything beneath it. A root reports the whole surface's text and covers the whole screen,
    // so including containers made every root look "covered" by its own children.
    var stage = host.getBoundingClientRect();
    var panels = [].slice.call(host.querySelectorAll('.pnl')).filter(function (el) {
      var own = false;
      for (var i = 0; i < el.children.length; i++) {
        if (el.children[i].classList.contains('pnl-text')) { own = true; break; }
      }
      if (!own) return false;
      if (!(el.textContent || '').trim()) return false;
      var r = el.getBoundingClientRect();
      if (r.width < 6 || r.height < 6) return false;
      // A label spanning nearly the whole screen is a layout artefact, not something to judge.
      if (r.width > stage.width * 0.9 && r.height > stage.height * 0.9) return false;
      return getComputedStyle(el).visibility !== 'hidden';
    });
    panels.forEach(function (el) {
      var mine = rootOf(el);
      if (mine === '?') return;   // harness furniture, not a game panel
      var layer = intendedLayer(el);
      var r = el.getBoundingClientRect();
      // Sample a small grid rather than the centre alone: a modal cut off along one edge by a
      // dock is exactly the case a single centre probe misses.
      var hitBy = {}, n = 0;
      for (var gx = 0.15; gx <= 0.85; gx += 0.35) {
        for (var gy = 0.15; gy <= 0.85; gy += 0.35) {
          var hit = document.elementFromPoint(r.left + r.width * gx, r.top + r.height * gy);
          if (!hit) continue;
          n++;
          var who = rootOf(hit);
          if (who === '?' || who === mine) continue;   // harness chrome, or our own root
          // An overlay (z>=100) may only be covered by something that outranks it, and a panel in
          // another root always loses to it -- unless that root is itself an overlay root.
          var bad = (layer >= 100)
            ? !/Confirm|Settings|Customize/.test(who)
            : !mayCover(who, mine);
          if (bad) hitBy[who] = (hitBy[who] || 0) + 1;
        }
      }
      out.total++;
      var offenders = Object.keys(hitBy);
      if (n && offenders.length) {
        out.covered.push({
          label: (el.textContent || '').trim().replace(/\s+/g, ' ').slice(0, 40),
          mine: mine, by: offenders.join(','), frac: Math.round(100 * hitBy[offenders[0]] / n),
          rect: [Math.round(r.left), Math.round(r.top), Math.round(r.width), Math.round(r.height)].join(',')
        });
      }
    });
    return out;
  };


  // --------------------------------------------------------------------------- boot
  Compare.ready = (async function () {
    if (qs.bare) applyBare();

    // app.js boots the in-game context closed (the demo bar); wait for it before steering.
    await waitFor(function () { return window.MvmMockup && window.MvmMockup.current && window.MvmMockup.current(); }, 20000);

    if (qs.surface) {
      if (qs.aspect && /^(1440|1728|1920)$/.test(qs.aspect)) document.getElementById('menu-aspect').value = qs.aspect;
      var ok = window.MvmMockup.select(qs.surface);
      if (!ok) note('unknown surface: ' + qs.surface);
      await waitFor(function () {
        var c = window.MvmMockup.current();
        return c && c.id === qs.surface;
      }, 20000).catch(function () { note('surface never became active: ' + qs.surface); });
    }

    var zoom = qs.zoom ? (qs.zoom === 'fit' ? 'fit' : Number(qs.zoom)) : (qs.bare ? 1 : null);
    if (zoom !== null) window.MvmMockup.zoom(zoom);

    // Let the surface build, then run any requested action and let THAT settle too.
    await whenQuiet(window.MvmMockup.host(), 350, 6000);
    if (qs.precmd) {
      var pres = qs.precmd.split(';');
      for (var pi = 0; pi < pres.length; pi++) {
        if (pres[pi].trim()) Compare.cmd(pres[pi]);
      }
      window.MvmMockup.tick(3);
      await whenQuiet(window.MvmMockup.host(), 250, 4000);
    }
    if (qs.act) {
      var acts = qs.act.split('|');
      for (var ai = 0; ai < acts.length; ai++) { clickAction(acts[ai]); window.MvmMockup.tick(3); await whenQuiet(window.MvmMockup.host(), 250, 4000); }
    }
    if (qs.click) {
      var clicks = qs.click.split('|');
      for (var ki = 0; ki < clicks.length; ki++) {
        clickControl(clicks[ki]);
        // A click can navigate and rebuild the host. Give that async hand-off time to start, then
        // wait for both the new panel and its state-driven render to settle.
        await new Promise(function (r) { setTimeout(r, 450); });
        window.MvmMockup.tick(3);
        await whenQuiet(window.MvmMockup.host(), 300, 5000);
      }
    }
    if (qs.cmd) {
      var cmds = qs.cmd.split(';');
      for (var ci = 0; ci < cmds.length; ci++) {
        if (!cmds[ci].trim()) continue;
        Compare.cmd(cmds[ci]);
        // a command can hand over to another surface, which rebuilds the host; wait for that too
        await new Promise(function (r) { setTimeout(r, 450); });
        window.MvmMockup.tick(3);
        await whenQuiet(window.MvmMockup.host(), 300, 5000);
        window.MvmMockup.tick(3);
      }
    }
    if (qs.drag) {
      var dp = qs.drag.split(',');
      await Compare.drag(dp[0], Number(dp[1] || 0), Number(dp[2] || 0));
      window.MvmMockup.tick(3);
      await whenQuiet(window.MvmMockup.host(), 250, 3000);
    }
    if (qs.reload) {
      await window.MvmMockup.reload();
      window.MvmMockup.tick(3);
      await whenQuiet(window.MvmMockup.host(), 300, 5000);
    }
    if (qs.hold) await new Promise(function (r) { setTimeout(r, Number(qs.hold) || 0); });

    // ?geom=1 parks the dump in the DOM, which is how the headless capture gets it back out:
    // chrome --dump-dom needs no devtools protocol driver, so the toolchain stays two scripts.
    if (window.MvmMockup.tick) window.MvmMockup.tick(2); // publish the final state before it is read
    if (qs.clickaudit) {
      var ca = document.createElement('pre');
      ca.id = 'mvm-clicks'; ca.style.display = 'none';
      ca.textContent = JSON.stringify(Compare.clickAudit());
      document.body.appendChild(ca);
    }
    if (qs.layers) {
      var la = document.createElement('pre');
      la.id = 'mvm-layers'; la.style.display = 'none';
      la.textContent = JSON.stringify(Compare.layerAudit());
      document.body.appendChild(la);
    }
    if (qs.geom) {
      var pre = document.createElement('pre');
      pre.id = 'mvm-geom';
      pre.style.display = 'none';
      pre.textContent = JSON.stringify(Compare.geomTree());
      var st = document.createElement('pre');
      st.id = 'mvm-state';
      st.style.display = 'none';
      var swap = window.MvmMockup.host().querySelector('[data-pid="ModeSwapBtn"]');
      st.textContent = JSON.stringify({ surface: window.MvmMockup.current() && window.MvmMockup.current().id,
        state: JSON.parse(window.PanoramaShim.stateJson), log: Compare.log,
        // Which scripts are alive: a torn-down workspace has nulled its api, as in game.
        apis: ['ConfigHud', 'CamEditor', 'CamTimeline', 'GraphExp', 'MarkerHud', 'MovieHud', 'AimXhair', 'FxDebugHud', 'Filmmaker', 'MvmPlay']
          .filter(function (n) { return !!window.$[n]; }),
        // Commands the router refused: a control that sends one looks dead in the browser.
        unrouted: window.MvmCommands.unhandled(),
        commands: window.PanoramaShim.commandLog.slice(),
        ui: {
          menu: /^(filmmakergui|workshop)$/.test(window.MvmMockup.current().id) ? window.MvmNativeMenu.model() : null,
          menuCommands: window.MvmNativeMenu.commands(),
          stageUserSelect: getComputedStyle(window.MvmMockup.stage()).userSelect,
          modeSwapText: swap ? swap.textContent.trim().replace(/\s+/g, ' ') : ''
        } });
      document.body.appendChild(st);
      document.body.appendChild(pre);
    }

    Compare.settled = true;
    document.title = document.title + '  [READY]';
    return true;
  })();
})();
