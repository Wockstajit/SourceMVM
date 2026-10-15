#pragma once

// Deadlock's design tokens, as a Panorama JS object.
//
// Read out of Deadlock's own citadel_base_styles.css (see Docs/deadlock/ui-style.md) rather than
// picked to look nice. The point is that a panel built from these reads as part of the game
// instead of as an overlay bolted onto it.
//
// Every panel sets its visual properties from this object EXPLICITLY rather than relying on the
// game's stylesheets. That is the same discipline the CS2 panels use, and for the same reason:
// a HUD panel does not inherit the stylesheet its neighbours were authored against, so a class
// name borrowed from the game renders as unstyled red scatter. Tokens, not classes.
//
// This fragment must be included FIRST. Every *Js.h fragment shares one JavaScript scope, so a
// plain `var` here would collide silently with the same name in another fragment and the later
// include would win, freezing the earlier one at render time. Hence a single namespaced object.

#define DEADLOCK_STYLE_JS R"CFJS(
(function () {
  if ($.MvmStyle) return;

  // --- Deadlock tokens, verbatim from citadel_base_styles.css -------------------------------
  var S = {
    // type
    fontSans:      'Retail Demo, Noto Sans, sans-serif',
    fontMono:      'Retail Text Demo, Noto Sans, sans-serif',
    fontSerif:     'Reaver, serif',
    fontDisplay:   'VALVEPulp, Noto Sans, sans-serif',
    fontNumeric:   'MingLiu, Retail Demo, Noto Sans, sans-serif',

    // ground. Deadlock is warm and low-chroma; pure black and pure white both look wrong in it.
    offBlack:      '#10130D',
    panelBg:       '#2e2c27',
    panelBgDeep:   '#1a1a1a',
    border:        '#444444ff',

    // text
    text:          '#FFEFD7',
    textMuted:     '#C6C6C6',
    textWarm:      '#DEC5A0',

    // accents
    gold:          '#FFED79',
    green:         '#5FE69E',
    soul:          '#70F8C1',
    blue:          '#1A9FFF',
    warn:          '#ee5945',
    danger:        '#FF410D',

    // Deadlock's own button gradient stops
    btnTop:        '#373d45',
    btnBottom:     '#4d5860',
    btnTopHover:   '#4c5561',
    btnBottomHover:'#6c7d88',

    // metrics taken from .ButtonBevel / .Slider so our controls match theirs
    btnRadius:     '8px',
    btnPadding:    '4px 10px',
    btnMinHeight:  '36px',
    railWidth:     '188px',
    rowHeight:     '30px',

    // Deadlock blurs behind its HUD chrome. Matching that is most of what makes a panel read as
    // native rather than pasted on.
    hudBlur:       'gaussian(2,2,2)',

    // icons that exist in Deadlock's own VPK, so they resolve at runtime with no shipped assets
    icoPlay:       's2r://panorama/images/transport_controls/icon_play_png.vtex',
    icoPause:      's2r://panorama/images/transport_controls/icon_pause_png.vtex',

    // THE REPLAY BAR'S OWN SET, which is a different family from the transport_controls one above:
    // heavier, drawn for a dark band, and what Deadlock puts on the bar this dock replaces. A
    // transport built out of Unicode triangles next to Deadlock's own chrome reads as a mock-up of
    // the game rather than as part of it.
    icoReplayPlay:  's2r://panorama/images/hud/replay/play_icon_psd.vtex',
    icoReplayPause: 's2r://panorama/images/hud/replay/pause_icon_psd.vtex',
    icoJumpBack1:   's2r://panorama/images/hud/replay/jump_back2_icon_psd.vtex',
    icoJumpBack2:   's2r://panorama/images/hud/replay/jump_back3_icon_psd.vtex',
    icoJumpFwd1:    's2r://panorama/images/hud/replay/jump_skip2_icon_psd.vtex',
    icoJumpFwd2:    's2r://panorama/images/hud/replay/jump_skip3_icon_psd.vtex',
    icoArrowUp:    's2r://panorama/images/control_icons/arrow_solid_up_png.vtex',
    icoArrowDown:  's2r://panorama/images/control_icons/arrow_solid_down_png.vtex',
    icoArrowLeft:  's2r://panorama/images/control_icons/arrow_solid_left_png.vtex',
    icoArrowRight: 's2r://panorama/images/control_icons/arrow_solid_right_png.vtex',

    // The dock ribbon's settings control. Deadlock ships this one as an SVG rather than a compiled
    // texture, and Panorama loads it by path either way -- a drawn gear, not an asterisk standing in
    // for one.
    icoGear:       's2r://panorama/images/icons/icon_gear.svg',

    // Shared overlay effects (P8a), so a modal scrim or a hard drop-shadow is not retyped ad hoc
    // -- and, for the two shadow strings, re-flagged by the style lint -- at every call site that
    // wants Deadlock's own dim-the-background or floating-card treatment.
    scrimDim:       'rgba(0,0,0,0.72)',
    textShadowHard: '2px 2px 0px 1.0 #000000',
    boxShadowFloat: '#000000ee 0px 6px 26px 4px'
  };

  // Clearing a colour with the empty string THROWS and aborts the whole script, taking the rest
  // of the panel with it. Transparent is the only safe "no colour".
  var CLEAR = 'rgba(0,0,0,0)';

  // THE ONLY WAY A PANEL HERE REACHES THE GAME.
  //
  // Deadlock's HUD Panorama context exposes no engine bindings at all -- measured in game on
  // build 6687 by enumerating the global object from a live RunScript. There is no
  // GameInterfaceAPI (CS2's entry point), and Deadlock's own CitadelConCommand is bound
  // per-XML-context by the panel class that declares it and is undefined here, as a bare global
  // and through the panel-event expression language alike.
  //
  // So a command is MARKED and printed. $.Msg goes to the engine console, tier0 lets several
  // listeners watch every line, and DeadlockUiCommandBridge.cpp recognises the marker and runs
  // the command on the main thread. The marker opens with STX, which no console font draws, so
  // the line does not also become visible noise.
  //
  // Keep this in step with kUiCommandMarker in DeadlockUiCommandBridge.h.
  function cmd(text) {
    if (!text) return;
    try { $.Msg('\x02MVMCMD ' + text + '\n'); } catch (e) {}
  }

  // EVERY STYLE WRITE IS MEMOISED, AND THAT IS NOT A MICRO-OPTIMISATION.
  //
  // Writing a style re-does the panel's geometry, and a 3D transform gives it a composited layer
  // with its own D3D11 vertex buffer. Panorama tracks those in a list indexed by an UNSIGNED SHORT.
  // Rewriting the same value every frame still allocates, so a panel re-transformed at 60fps burns
  // through the index space and the game dies with
  //
  //     CUtlLeanVectorBase<UtlLinkedListElem_t<CVertexBufferDx11*,unsigned short>,
  //         unsigned short,CMemAllocAllocator>::EnsureCapacity allocation count overflow
  //
  // The workspace scales roughly twenty native HUD panels into the preview rect on every frame, so
  // it reached 65535 in under a minute -- measured: the process grew from 3.2 GB to 14.3 GB while a
  // replay played with the workspace open, and not at all with it closed.
  //
  // BEFORE THE CRASH IT LOOKS LIKE A RENDERING BUG. Once the list is saturated, panels cannot get
  // fresh geometry, so they keep drawing whatever they last had: parts of the preview freeze as a
  // still from before the last rescale, and nothing re-lays-out. That is the "static image from
  // earlier" symptom, and it is this, not the scaling arithmetic.
  //
  // So the values we wrote are remembered per panel, and an identical write is skipped. A panel's
  // layout is static most of the time -- the preview rect only moves when the dock, the column or
  // the window changes -- so in the steady state this writes nothing at all.
  //
  // The cache is only correct because WE are the only writer of the properties we set. It is keyed
  // on the panel object itself, so it goes away with the panel.
  // WHO IS WRITING STYLES EVERY FRAME.
  //
  // A style write that lands re-does the panel's geometry and can allocate a D3D11 vertex buffer,
  // and Panorama indexes those with an UNSIGNED SHORT -- so a panel restyled every frame runs the
  // list to 65535 and the game dies with
  //
  //     CUtlLeanVectorBase<UtlLinkedListElem_t<CVertexBufferDx11*,unsigned short>, ...>
  //         ::EnsureCapacity allocation count overflow
  //
  // The memo below makes a repeated identical write free, so in the steady state this counter
  // should sit at ZERO. When it does not, the only question worth asking is WHICH panel, and that
  // is a question no crash dump answers -- by the time it fires the writer is long gone. So the
  // writes are tallied per panel id here and `mirv_filmmaker ui styles` prints the worst offenders.
  var writeTally = {}, writeTotal = 0;

  function tally(panel) {
    var id = '';
    try { id = panel.id || ('<' + panel.paneltype + '>'); } catch (e) { id = '<?>'; }
    if (!id) id = '<anon>';
    writeTally[id] = (writeTally[id] || 0) + 1;
    writeTotal++;
  }

  // CHANGE-GATED PROPERTY WRITES, alongside set() for styles. A `visible`, `hittest` or `text`
  // assignment is a layout invalidation to Panorama even when the value is unchanged, and the dock
  // was making a dozen of them per frame. Measured in a replay with the editor open: 6.6 fps with
  // the dock section running, 88 fps with it skipped. Memoised on the panel under the same keys the
  // workspace's own show()/hit() use, so the two gates never disagree about what a panel holds.
  function vis(panel, v) {
    if (!panel) return;
    v = !!v;
    if (panel.__mvmShown === v) return;
    try { panel.visible = v; panel.__mvmShown = v; } catch (e) {}
  }
  function hit(panel, v) {
    if (!panel) return;
    v = !!v;
    if (panel.__mvmHit === v) return;
    try { panel.hittest = v; panel.__mvmHit = v; } catch (e) {}
  }
  function txt(panel, s) {
    if (!panel) return;
    s = (s === undefined || s === null) ? '' : String(s);
    if (panel.__mvmText === s) return;
    try { panel.text = s; panel.__mvmText = s; } catch (e) {}
  }

  function set(panel, props) {
    if (!panel) return panel;
    var memo = panel.__mvmStyle;
    if (!memo) { memo = {}; try { panel.__mvmStyle = memo; } catch (e) { memo = null; } }
    for (var k in props) {
      var v = props[k];
      if (memo && memo[k] === v) continue;
      try {
        panel.style[k] = v;
        if (memo) memo[k] = v;
        tally(panel);
      } catch (e) {
        // The write threw, so the panel does NOT hold this value -- forget it, or a later identical
        // write would be skipped on the strength of one that never landed.
        if (memo) delete memo[k];
      }
    }
    return panel;
  }

  // Drop a panel's memo. Needed where something other than M.set has changed the panel's style --
  // a native panel released back to the game, or one whose inline styles were cleared.
  function forget(panel) {
    if (!panel) return;
    try { panel.__mvmStyle = null; } catch (e) {}
  }

  function make(parent, type, id, props) {
    if (!parent) return null;
    var p = null;
    try { p = $.CreatePanel(type, parent, id || ''); } catch (e) { return null; }
    if (props) set(p, props);
    return p;
  }

  // CENTRING A LABEL IS TWO PROPERTIES, NOT ONE.
  //
  // `horizontal-align: center` centres the label's BOX inside its parent. It says nothing about the
  // text inside that box, and the moment the label is given a width -- `100%`, or a fixed px so the
  // row's rhythm does not depend on the string -- the box already fills its parent and the align
  // does nothing at all. The text then sits hard against the left edge of a button that looks
  // centred in every other respect. That single omission is what made the key caps, the nav arrows,
  // the capture button and the segmented cells all read as slightly wrong.
  //
  // So: asking for a centred label here means asking for centred text. An explicit `textAlign`
  // still wins, because 'right' on a numeric readout is a deliberate choice and not an oversight.
  function label(parent, id, text, props) {
    var l = make(parent, 'Label', id, {
      fontFamily: S.fontSans,
      color: S.text,
      fontSize: '14px'
    });
    if (l) {
      try { l.text = text; } catch (e) {}
      if (props) {
        if (props.horizontalAlign === 'center' && props.textAlign === undefined) {
          var copy = {};
          for (var k in props) if (props.hasOwnProperty(k)) copy[k] = props[k];
          copy.textAlign = 'center';
          props = copy;
        }
        set(l, props);
      }
    }
    return l;
  }

  // A button styled like Deadlock's .ButtonBevel: vertical gradient, 1px border, 8px radius,
  // uppercase letter-spaced label with a hard text shadow.
  function button(parent, id, text, onActivate, props) {
    var b = make(parent, 'Panel', id, {
      backgroundColor: 'gradient( linear, 0% 0%, 0% 100%, from( ' + S.btnTop +
                       ' ), to( ' + S.btnBottom + ' ) )',
      border: '1px solid ' + S.border,
      borderRadius: S.btnRadius,
      padding: S.btnPadding,
      minHeight: S.btnMinHeight,
      flowChildren: 'right',
      horizontalAlign: 'left'
    });
    if (!b) return null;
    label(b, '', text, {
      color: S.text,
      fontSize: '13px',
      letterSpacing: '2px',
      textTransform: 'uppercase',
      textShadow: '2px 2px 0px 1.0 #000000',
      verticalAlign: 'middle',
      horizontalAlign: 'center',
      width: '100%'
    });
    if (onActivate) {
      try {
        b.SetPanelEvent('onactivate', onActivate);
        b.SetPanelEvent('onmouseover', function () {
          set(b, { backgroundColor: 'gradient( linear, 0% 0%, 0% 100%, from( ' +
                   S.btnTopHover + ' ), to( ' + S.btnBottomHover + ' ) )' });
        });
        b.SetPanelEvent('onmouseout', function () {
          set(b, { backgroundColor: 'gradient( linear, 0% 0%, 0% 100%, from( ' +
                   S.btnTop + ' ), to( ' + S.btnBottom + ' ) )' });
        });
      } catch (e) {}
    }
    if (props) set(b, props);
    return b;
  }

  // A card: the panel ground everything else sits on.
  function card(parent, id, props) {
    var c = make(parent, 'Panel', id, {
      backgroundColor: S.panelBg,
      border: '1px solid ' + S.border,
      borderRadius: '6px',
      padding: '10px',
      flowChildren: 'down'
    });
    if (props) set(c, props);
    return c;
  }

  // A labelled slider row. Every drag in an in-game panel has to be a Slider: Panorama gives the
  // HUD context no mouse-move event at all, so a hand-rolled drag has nothing to listen to.
  function sliderRow(parent, id, text, min, max, value, onChange) {
    var row = make(parent, 'Panel', id + 'Row', {
      flowChildren: 'right',
      height: S.rowHeight,
      width: '100%'
    });
    label(row, id + 'Label', text, {
      width: '46%',
      color: S.textMuted,
      fontSize: '13px',
      verticalAlign: 'middle'
    });
    var s = make(row, 'Slider', id, {
      width: '40%',
      verticalAlign: 'middle'
    });
    var val = label(row, id + 'Value', '', {
      width: '14%',
      color: S.gold,
      fontFamily: S.fontNumeric,
      fontSize: '13px',
      textAlign: 'right',
      verticalAlign: 'middle'
    });
    if (s) {
      try {
        s.min = min;
        s.max = max;
        s.value = value;
        if (val) val.text = value.toFixed(2);
        // The guard matters: writing .value from a change handler re-enters the handler, and
        // without the flag the slider and the readout fight each other every frame.
        var applying = false;
        s.SetPanelEvent('onvaluechanged', function () {
          if (applying) return;
          applying = true;
          try {
            if (val) val.text = s.value.toFixed(2);
            if (onChange) onChange(s.value);
          } catch (e) {}
          applying = false;
        });
      } catch (e) {}
    }
    return { row: row, slider: s, value: val };
  }

  // The tally since the last reset, worst first. `n` bounds the report; the total is always exact.
  function styleStats(n) {
    var rows = [];
    for (var id in writeTally) rows.push([id, writeTally[id]]);
    rows.sort(function (a, b) { return b[1] - a[1]; });
    return { total: writeTotal, top: rows.slice(0, n || 12) };
  }

  function styleReset() { writeTally = {}; writeTotal = 0; }

  $.MvmStyle = {
    S: S,
    CLEAR: CLEAR,
    cmd: cmd,
    set: set,
    vis: vis,
    hit: hit,
    txt: txt,
    styleStats: styleStats,
    styleReset: styleReset,
    forget: forget,
    make: make,
    label: label,
    button: button,
    card: card,
    sliderRow: sliderRow
  };
})();
)CFJS"
