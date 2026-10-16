#pragma once

// THE DOCK RIBBON -- ONE definition, spliced into every workspace that shows the dock.
//
// Docs/panorama/panorama_ui_guide.md maintains the shared dock contract, originally phrased as
// "a single shared component imported by all three workspaces with an accent prop."
//
// It was not. The Camera Editor drew a real three-tab bar; the CONFIG panel drew a dead LABEL
// reading "REGULAR TIMELINE" next to a gear, with a comment explaining that "Config has no
// Camera/Graph editor to switch to" -- true only because the dock's mode was private to the
// editor (see Movie/TimelineDock.h, which now owns it). So the same bar was two different
// controls with two different capabilities depending on which panel you had open.
//
// GEOMETRY originated in the dock design; maintained context: Docs/features/camera-editor-feature.md:
//
//   ribbon  height:36px; padding:0 12px; gap:4px;
//           border-top/bottom:1px solid <accent>;
//           background: native ? rgba(18,22,28,0.38) : #14181e
//   tab     width:150px; padding:9px 4px; border-radius:3px;
//           background: on ? <accent>33 : #ffffff14;
//           border:1px solid (on ? <accent> : #ffffff1a);
//           colour:   on ? <accent> : #c8d3df;  13px/700, letter-spacing 1.4px
//   meta    Stratum2Mono 12px #8b95a3, right-aligned before the gear
//   gear    width:34px; padding:5px 0; margin-left:10px; 16px;
//           background:#ffffff14; border:1px solid #ffffff1a; border-radius:3px
//
// The ribbon is TRANSLUCENT on the native tab because there the dock overlays the game render
// rather than sitting under it (original design: 126px, "translucent, overlays the render").
//
// NOT a standalone script. It is a raw-string chunk concatenated into a host IIFE and needs
// these already in the host's scope: `mk(type,parent)`, `lbl(parent,text,colour,size)`,
// `cmd(consoleCommand)` and `S` (theme tokens).
//
//   var ribbon = mvmTimelineDockRibbon(root, S.accent, function () { settingsPopup.toggle(); });
//   ribbon.root                      -- the band, for the host to position and size
//   ribbon.gear                      -- so the host can light it while its popup is open
//   ribbon.sync(tabName, metaText)   -- called from render() every frame; cheap and idempotent
R"DOCKRIBJS(
    // $.MvmTour: named panels the onboarding tutorial can spotlight (Panorama/OnboardingGuideJs.h).
    // Most controls are created without an id, so the script that owns one registers it here by
    // name. Several panels may share a name (the editor and Config each build a dock ribbon); the
    // tutorial lights whichever is on screen and skips ones that were deleted.
    function mvmTourAdd(name, panel) {
      try { var t = $.MvmTour || ($.MvmTour = {}); (t[name] = t[name] || []).push(panel); } catch (e) {}
    }
    function mvmTimelineDockRibbon(parent, accent, onGear) {
      var RIBBON_H = 36, TAB_W = 150;
      var band = mk('Panel', parent);
      band.hittest = false;
      band.style.height = RIBBON_H + 'px';
      band.style.flowChildren = 'right';
      band.style.horizontalAlign = 'left';
      band.style.verticalAlign = 'top';
      band.style.paddingLeft = '12px';
      band.style.paddingRight = '12px';
      band.style.borderTop = '1px solid ' + accent;
      band.style.borderBottom = '1px solid ' + accent;

      // One console verb for every workspace -- see Movie/TimelineDock.h. A tab is a SWITCH, not
      // a toggle: it always selects its own mode, so the dock can never end up showing nothing.
      var TABS = [['native', 'REGULAR TIMELINE'], ['camera', 'CAMERA'], ['graph', 'GRAPH']];
      var tabs = {};
      for (var i = 0; i < TABS.length; i++) (function (key, caption, last) {
        var b = mk('Panel', parent ? band : band);
        b.hittest = true;
        b.style.width = TAB_W + 'px';
        b.style.verticalAlign = 'center';
        b.style.paddingTop = '9px'; b.style.paddingBottom = '9px';
        b.style.paddingLeft = '4px'; b.style.paddingRight = '4px';
        b.style.marginRight = last ? '0px' : '4px';
        b.style.borderRadius = '3px';
        b.style.transitionProperty = 'background-color, border-color';
        b.style.transitionDuration = '0.12s';
        b.style.transitionTimingFunction = 'ease-out';
        var t = lbl(b, caption, '#c8d3dfff', 13);
        t.hittest = false;
        t.style.fontWeight = 'bold';
        t.style.letterSpacing = '1.4px';
        t.style.whiteSpace = 'nowrap';
        t.style.textAlign = 'center';
        t.style.horizontalAlign = 'center';
        b.__lbl = t;
        b.SetPanelEvent('onactivate', function () { cmd('mirv_filmmaker dock ' + key); });
        tabs[key] = b;
        mvmTourAdd('dock.' + key, b);
      })(TABS[i][0], TABS[i][1], i === TABS.length - 1);

      var spacer = mk('Panel', band);
      spacer.hittest = false;
      spacer.style.width = 'fill-parent-flow(1.0)';
      spacer.style.height = '1px';

      // What only the active tab knows: its keyframe count, its visible-channel tally, or the
      // round the demo is sitting in. Mono, because two of the three are numbers that change
      // every frame and proportional digits jitter.
      var meta = lbl(band, '', '#8b95a3ff', 12);
      meta.hittest = false;
      meta.style.fontFamily = 'Stratum2Mono, monospace';
      meta.style.verticalAlign = 'center';
      meta.style.whiteSpace = 'nowrap';

      var gear = mk('Panel', band);
      gear.hittest = true;
      gear.style.width = '34px';
      gear.style.marginLeft = '10px';
      gear.style.verticalAlign = 'center';
      gear.style.paddingTop = '5px'; gear.style.paddingBottom = '5px';
      gear.style.borderRadius = '3px';
      gear.style.backgroundColor = '#ffffff14';
      gear.style.border = '1px solid #ffffff1a';
      var gearLbl = lbl(gear, '⚙', '#eef2f6ff', 16);
      gearLbl.hittest = false;
      gearLbl.style.horizontalAlign = 'center';
      gearLbl.style.textAlign = 'center';
      gear.__lbl = gearLbl;
      if (onGear) gear.SetPanelEvent('onactivate', function () { onGear(); });

      var appliedTab = '';
      function sync(tab, metaText) {
        tab = tab || 'native';
        if (tab !== appliedTab) {
          appliedTab = tab;
          // The dock overlays the game render on the native tab and sits under it otherwise, so
          // the band is translucent there and opaque on the two editor tabs.
          band.style.backgroundColor = (tab === 'native') ? 'rgba(18,22,28,0.38)' : '#14181eff';
          for (var k in tabs) {
            var on = k === tab, b = tabs[k];
            b.style.backgroundColor = on ? (accent.substring(0, 7) + '33') : '#ffffff14';
            b.style.border = '1px solid ' + (on ? accent : '#ffffff1a');
            b.__lbl.style.color = on ? accent : '#c8d3dfff';
          }
        }
        var text = metaText || '';
        if (meta.text !== text) meta.text = text;
      }
      sync('native', '');

      return { root: band, tabs: tabs, gear: gear, meta: meta, sync: sync, height: RIBBON_H };
    }

    // WHERE THE DOCK IS, for the current tab -- one measurement for every host.
    //
    // The Camera Editor always had this: it reads the ACTUAL height of whichever editor is docked
    // (the camera bar is fit-children and grows; the native bar is CS2's own) and parks the ribbon
    // right above it, then stops the preview at the ribbon. CONFIG never did -- it assumed the
    // native bar was the only thing under it, so the moment it could dock the camera timeline the
    // preview stopped shrinking and the ribbon landed under the bar. Same numbers as the editor:
    // 96 native / 176 camera / 380 graph fallbacks until the panels lay out.
    //
    //   ctx  the HUD context panel (FindChildTraverse root)
    //   rh   the host's height in css px, rsy its y ui-scale
    //   tab  the shared DockTab name ('native' / 'camera' / 'graph')
    //   opt  { floor } -- how far the host lifts the band's minimum off the bottom (CONFIG: 28)
    function mvmDockMeasure(ctx, rh, rsy, tab, opt) {
      opt = opt || {};
      var RIB = 36, GAP = 4;
      var floor = opt.floor || 0;
      var editorTop;
      function measure(id, viaParent, fallback) {
        try {
          var p = ctx.FindChildTraverse && ctx.FindChildTraverse(id);
          if (p && viaParent && p.GetParent) p = p.GetParent();
          var h = (p && p.visible) ? p.actuallayoutheight / rsy : 0;
          if (h > 10 && h < rh * 0.6) return h;
        } catch (e) {}
        return fallback;
      }
      if (tab === 'graph') editorTop = 380;
      else if (tab === 'camera') editorTop = measure('CamTimelineBar', false, 176);
      else editorTop = measure('SliderRow', true, 96);
      var bottomH = Math.max(RIB + floor, Math.min(editorTop + GAP + RIB, Math.max(0, rh - 160)));
      var dockTopY = Math.max(0, rh - bottomH);
      var tabY = Math.max(dockTopY, rh - editorTop - GAP - RIB);
      return {
        editorTop: editorTop, bottomH: bottomH, dockTopY: dockTopY, tabY: tabY,
        nativeBottom: (tab !== 'camera' && tab !== 'graph'), ribbonH: RIB
      };
    }
)DOCKRIBJS"
