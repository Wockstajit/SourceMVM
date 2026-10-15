#pragma once

// THE DOCK RIBBON: the tab strip that switches which editor is docked along the bottom.
//
// This is the control that was missing, and its absence is what made the whole panel read wrong.
// The timeline, the camera editor's strip and the graph are three views of the bottom of the
// screen, under the preview; they were stacked into the right-hand column instead, where an
// inspector belongs. The ribbon puts them back where they go and gives them one switch.
//
//     +-------------------------------------------------------------+
//     |  [REGULAR TIMELINE] [CAMERA] [GRAPH]           meta     (*)  |  <- this, 36px
//     +-------------------------------------------------------------+
//     |  the active docked editor                                    |
//     +-------------------------------------------------------------+
//
// A TAB IS A SWITCH, NOT A TOGGLE. Each one selects its own mode, so the dock can never end up
// showing nothing -- which is where a toggle lands on its second press.
//
// OPAQUE ON EVERY TAB. The CS2 ribbon is translucent over its native tab because there the dock
// shows the game's own demo bar. Deadlock has no such bar to show -- `CitadelHudReplay` is 272px
// wide and ships hidden -- so all three tabs draw an editor of ours and all three sit on a ground.
//
// GEOMETRY AND COLOUR are the dock spec's, restated in Deadlock's palette: 36px band, 150px tabs
// with 9px vertical padding and a 3px radius, 13px bold uppercase labels with 1.4px tracking, and
// a mono meta readout right-aligned before the gear. The meta line is mono because two of the three
// tabs report a number that changes while you watch it, and proportional digits jitter.

#define DEADLOCK_DOCK_RIBBON_JS R"CFJS(
(function () {
  if ($.MvmDockRibbon) return;
  var M = $.MvmStyle;
  if (!M) return;
  var S = M.S;

  var band = null, meta = null, gear = null;
  var tabs = {};
  var appliedTab = '';
  var onGear = null;

  var RIBBON_H = 36, TAB_W = 150;

  var TABS = [
    ['native', 'Regular Timeline'],
    ['camera', 'Camera'],
    ['graph',  'Graph']
  ];

  function cmd(text) { M.cmd(text); }

  function build(parent, gearHandler) {
    onGear = gearHandler || null;

    band = M.make(parent, 'Panel', 'MvmDockRibbon', {
      height: RIBBON_H + 'px',
      flowChildren: 'right',
      horizontalAlign: 'left',
      verticalAlign: 'top',
      padding: '0px 12px',
      borderTop: '1px solid ' + S.gold,
      borderBottom: '1px solid ' + S.gold
    });
    if (!band) return null;
    band.visible = false;

    for (var i = 0; i < TABS.length; ++i) {
      (function (key, caption, last) {
        var b = M.make(band, 'Panel', 'MvmDockTab_' + key, {
          width: TAB_W + 'px',
          verticalAlign: 'middle',
          padding: '9px 4px',
          marginRight: last ? '0px' : '4px',
          borderRadius: '3px'
        });
        if (!b) return;
        var t = M.label(b, '', caption, {
          color: S.textMuted,
          fontSize: '13px',
          letterSpacing: '1.4px',
          textTransform: 'uppercase',
          textAlign: 'center',
          horizontalAlign: 'center',
          whiteSpace: 'nowrap'
        });
        if (t) t.hittest = false;
        b.__lbl = t;
        try {
          b.SetPanelEvent('onactivate', function () { cmd('mirv_filmmaker dock ' + key); });
        } catch (e) {}
        tabs[key] = b;
      })(TABS[i][0], TABS[i][1], i === TABS.length - 1);
    }

    var spacer = M.make(band, 'Panel', 'MvmDockSpacer', {
      width: 'fill-parent-flow(1.0)', height: '1px'
    });
    if (spacer) spacer.hittest = false;

    meta = M.label(band, 'MvmDockMeta', '', {
      color: S.textMuted,
      fontFamily: S.fontNumeric,
      fontSize: '12px',
      verticalAlign: 'middle',
      whiteSpace: 'nowrap',
      marginRight: '10px'
    });
    if (meta) meta.hittest = false;

    // The gear opens the dock's own settings. It is a real control rather than decoration, so it
    // gets the same affordance as a tab: a ground, a border and a radius.
    gear = M.make(band, 'Panel', 'MvmDockGear', {
      width: '34px',
      verticalAlign: 'middle',
      padding: '5px 0px',
      borderRadius: '3px',
      backgroundColor: 'rgba(255,255,255,0.08)',
      border: '1px solid ' + S.border
    });
    if (gear) {
      // THE GAME'S OWN GEAR, as an image. It was an asterisk -- a character borrowed to mean
      // "settings" -- which is exactly the kind of stand-in that reads as unfinished next to
      // Deadlock's own chrome.
      var g = M.make(gear, 'Panel', '', {
        width: '16px', height: '16px',
        backgroundImage: 'url("' + S.icoGear + '")',
        backgroundSize: 'contain', backgroundRepeat: 'no-repeat', backgroundPosition: 'center',
        horizontalAlign: 'center', verticalAlign: 'middle'
      });
      if (g) g.hittest = false;
      try {
        gear.SetPanelEvent('onactivate', function () { if (onGear) onGear(); });
      } catch (e) {}
    }

    sync('native', '');
    return band;
  }

  function sync(tab, metaText) {
    tab = tab || 'native';
    if (tab !== appliedTab) {
      appliedTab = tab;
      // Opaque on every tab. It was translucent on the native one, where the dock used to overlay
      // Deadlock's own replay bar and draw nothing of its own -- but that bar is a 272px widget
      // that ships hidden, so the tab now draws a Regular Timeline of ours and there is a panel
      // under the ribbon on all three.
      M.set(band, { backgroundColor: S.panelBgDeep });
      for (var k in tabs) {
        var b = tabs[k];
        if (!b) continue;
        var on = (k === tab);
        M.set(b, {
          backgroundColor: on ? 'rgba(255,237,121,0.20)' : 'rgba(255,255,255,0.08)',
          border: '1px solid ' + (on ? S.gold : S.border)
        });
        if (b.__lbl) M.set(b.__lbl, { color: on ? S.gold : S.textMuted });
      }
    }
    if (meta) {
      try { if (meta.text !== (metaText || '')) meta.text = metaText || ''; } catch (e) {}
    }
  }

  function place(x, y, w, h) {
    if (!band) return;
    M.set(band, {
      position: Math.floor(x) + 'px ' + Math.floor(y) + 'px 0px',
      width: Math.floor(Math.max(0, w)) + 'px',
      height: Math.floor(Math.max(0, h)) + 'px'
    });
  }

  function setVisible(v) { M.vis(band, v); }
  function setHittest(v) { M.hit(band, v); }

  $.MvmDockRibbon = {
    build: build, sync: sync, place: place,
    setVisible: setVisible, setHittest: setHittest,
    height: function () { return RIBBON_H; },
    root: function () { return band; }
  };
})();
)CFJS"
