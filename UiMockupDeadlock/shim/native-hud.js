// Deadlock's OWN HUD, as far into it as our panels reach.
//
// WHY THE MOCKUP HAS TO STAND THIS UP. The Regular Timeline is not a panel of ours that looks like
// the game's replay bar -- it IS `#hud_replay`, hosted. Our code finds the game's bar, removes the
// classes that hide it, re-sizes it to the dock's width, empties the dead space on its left, hides
// the speed stepper and builds our dial in its place, and on teardown puts every one of those back.
// Without the bar present in the browser, the most-edited surface in the tool is the one the mockup
// cannot show, and every change to it would have to be verified by launching the game.
//
// So this is built from the game's own decompiled files, not invented:
//   reference/deadlock/panorama/layout/replay/citadel_hud_replay.xml
//   reference/deadlock/panorama/layout/replay/citadel_hud_replay_scrubber.xml
//   reference/deadlock/panorama/styles/replay/citadel_hud_replay.css
//   reference/deadlock/panorama/styles/replay/citadel_hud_replay_scrubber.css
//
// Every id, every class and every metric below is copied from those. That matters because our code
// keys off them: `#scrubInfo` is 90x50, and it is the ONLY child of `#SliderAndMarkers` with an
// intrinsic height (`#TimeScrubContainer` is height:100% of whatever the parent resolved to). Hiding
// it collapses the whole timeline to zero height, which is a bug that has already been shipped once.
// A flattened stand-in could not reproduce it, so the mockup would have said the UI was fine.
//
// Equally: the bar is hidden TWO ways. `Hidden` on the bar collapses it; `Hidden` on
// `#SpectatorOptionsContainer` slides it 272px off its own right edge. A stand-in that only modelled
// the first could not tell a shown bar from one parked off screen.

(function (global) {
  'use strict';

  function P(type, id, parent, props, classes) {
    var p = $.CreatePanel(type, parent, id || '');
    if (classes) classes.forEach(function (c) { p.AddClass(c); });
    if (props) for (var k in props) p.style[k] = props[k];
    return p;
  }

  var ICON = 's2r://panorama/images/hud/replay/';

  // The replay's own tick span, which the game's scrubber and its event markers both run on. Taken
  // from the replay this port is tested against (mirv_filmmaker: min=0 max=71718).
  var DEMO_END_TICK = 71718;

  // A round icon button, the shape .JumpButton and .NextMarkerButton both describe.
  function roundButton(parent, id, iconId, icon, size, bg) {
    var b = P('Button', id, parent, {
      width: size + 'px', height: size + 'px',
      marginLeft: '5px', marginRight: '5px',
      verticalAlign: 'middle',
      backgroundColor: bg,
      borderRadius: '50%'
    }, ['JumpButton']);
    P('Panel', iconId, b, {
      width: (size >= 32 ? 18 : 12) + 'px', height: (size >= 32 ? 18 : 12) + 'px',
      horizontalAlign: 'center', verticalAlign: 'middle',
      backgroundImage: 'url("' + ICON + icon + '")',
      backgroundSize: 'contain', backgroundRepeat: 'no-repeat', backgroundPosition: 'center',
      washColor: '#8199C5'
    }, ['JumpButtonIcon']);
    return b;
  }

  // Builds the game's HUD under the context panel and answers the handful of panels the harness
  // itself wants to look at. Returns before our own workspace is built, because in game the HUD is
  // already there and the hook attaches to it.
  function build(ctx) {
    // The layers that sit BESIDE HudCore. Two of them belong to the picture and get scaled with it
    // (a tooltip that stayed full-size over a shrunken HUD is the bug that put them here); the other
    // two are real modal menus that must NOT be, and they are present so the mockup can prove our
    // code leaves them alone.
    var core = P('Panel', 'HudCoreRoot', ctx, { width: '100%', height: '100%' }, ['HudCore']);

    // Stand-ins for the gameplay HUD: the panels `hideGameplayHud` hides and `scaleHud` transforms.
    // Shapes and positions only -- enough to see the scaling land correctly and to notice when a
    // panel that should have moved did not.
    var shop = P('Panel', 'hud_shop', core, {
      position: '24px 300px 0px', width: '210px', height: '300px',
      backgroundColor: '#1a1a1acc', border: '1px solid #444444ff', borderRadius: '6px'
    });
    P('Label', '', shop, {
      padding: '8px', color: '#C6C6C6', fontSize: '13px', letterSpacing: '2px'
    }).text = 'SHOP';

    var abilities = P('Panel', 'hud_abilities', core, {
      position: '740px 960px 0px', width: '440px', height: '96px',
      backgroundColor: '#2e2c27cc', border: '1px solid #444444ff', borderRadius: '6px'
    });
    P('Label', '', abilities, {
      padding: '8px', color: '#C6C6C6', fontSize: '13px', letterSpacing: '2px'
    }).text = 'ABILITIES';

    var healthbars = P('Panel', 'hud_healthbars', core, {
      position: '620px 120px 0px', width: '680px', height: '54px',
      backgroundColor: '#10130Dcc', border: '1px solid #444444ff', borderRadius: '4px'
    });
    P('Label', '', healthbars, {
      padding: '6px', color: '#DEC5A0', fontSize: '13px', letterSpacing: '2px'
    }).text = 'HEALTH BARS  (world-space HUD)';

    // ---- the spectate chain -------------------------------------------------------------------
    //
    // The bar is NOT a direct child of HudCore: the game nests it inside a spectate container which
    // is itself nested. Our scaler walks UP from the bar to work out which child of HudCore to skip,
    // so the nesting has to be real or that walk has nothing to find.
    var spectate = P('Panel', 'spectate_container', core, { width: '100%', height: '100%' });
    var gameplay = P('Panel', 'gameplay_hud', spectate, { width: '100%', height: '100%' });

    // #hud_replay, as the game lays it out. citadel_hud_replay.css gives the CitadelHudReplay CLASS
    // width 272px, but hud.css's `#hud_replay` ID rule (width 100%, height 60px, vertical-align
    // bottom) outranks it, so with nothing of ours open the game draws the bar across the whole
    // bottom of the screen. The stand-in used the class width, right-aligned, which parked the bar
    // in the bottom-right corner with our buttons running off the edge. `.Hidden` still collapses it.
    var replay = P('Panel', 'hud_replay', gameplay, {
      width: '100%',
      height: '60px',
      verticalAlign: 'bottom'
    }, ['CitadelHudReplay', 'Hidden']);

    // #SpectatorOptionsContainer: width 100%, and `.Hidden` slides it 272px off to the right.
    var options = P('Panel', 'SpectatorOptionsContainer', replay, {
      width: '100%',
      transitionProperty: 'transform',
      transitionDuration: '0.15s'
    }, ['Hidden']);

    var playback = P('Panel', 'PlaybackControls', options, {
      flowChildren: 'down', padding: '8px', width: '100%',
      backgroundColor: '#252727ee',
      transitionProperty: 'background-color', transitionDuration: '0.15s'
    });
    playback.hittest = false;

    var main = P('Panel', 'MainPlaybackControls', playback, {
      flowChildren: 'right', width: '100%'
    });

    // .SpeedControls -- the minus / dropdown / plus stepper our dial replaces. It is hidden rather
    // than deleted, because the bar belongs to the game and has to be handed back intact.
    var speed = P('Panel', '', main, {
      flowChildren: 'right', width: '200px',
      horizontalAlign: 'center', verticalAlign: 'center'
    }, ['SpeedControls']);
    var speedCenter = P('Panel', 'SpeedControlsCenter', speed, {
      flowChildren: 'right', horizontalAlign: 'center'
    });
    roundButton(speedCenter, '', 'SpeedLess', 'minus_icon_psd.vtex', 20, '#4D5C7644');
    P('Panel', 'SpeedDropdown', speedCenter, {
      width: '100px', height: '20px', verticalAlign: 'middle',
      backgroundColor: '#4D5C7644', borderRadius: '3px'
    });
    roundButton(speedCenter, '', 'SpeedMore', 'plus_icon_psd.vtex', 20, '#4D5C7644');

    // .PlayControls
    var play = P('Panel', '', main, {
      flowChildren: 'right', verticalAlign: 'center',
      marginLeft: '10px', marginRight: '10px'
    }, ['PlayControls']);
    roundButton(play, 'SkipBackward2Button', 'SkipBackward2', 'jump_back3_icon_psd.vtex', 32, '#576886bb');
    roundButton(play, 'SkipBackward1Button', 'SkipBackward1', 'jump_back2_icon_psd.vtex', 32, '#576886bb');
    var playBtn = P('Button', 'PlayButton', play, {
      width: '44px', height: '44px',
      verticalAlign: 'middle', horizontalAlign: 'center',
      backgroundColor: '#576886aa', borderRadius: '50%',
      marginLeft: '5px', marginRight: '5px'
    }, ['Pause']);
    P('Panel', '', playBtn, {
      width: '24px', height: '24px',
      horizontalAlign: 'center', verticalAlign: 'middle',
      backgroundImage: 'url("' + ICON + 'pause_icon_psd.vtex")',
      backgroundSize: 'contain', backgroundRepeat: 'no-repeat', backgroundPosition: 'center',
      washColor: '#8199C5'
    }, ['PlayButtonIcon']);
    roundButton(play, 'SkipForward1Button', 'SkipForward1', 'jump_skip2_icon_psd.vtex', 32, '#576886bb');
    roundButton(play, 'SkipForward2Button', 'SkipForward2', 'jump_skip3_icon_psd.vtex', 32, '#576886bb');

    // .MarkerControls
    var markerCtl = P('Panel', '', main, {
      flowChildren: 'right', verticalAlign: 'center',
      marginLeft: '10px', marginRight: '10px'
    }, ['MarkerControls']);
    roundButton(markerCtl, 'PrevMarkerButton', 'PrevMarker', 'jump_back2_icon_psd.vtex', 32, '#576886bb');
    P('Panel', 'MarkerTypeDropdown', markerCtl, {
      width: '150px', height: '28px', verticalAlign: 'middle',
      backgroundColor: '#4D5C7644', borderRadius: '3px'
    });
    roundButton(markerCtl, 'NextMarkerButton', 'NextMarker', 'jump_skip2_icon_psd.vtex', 32, '#576886bb');

    // ---- the scrubber -------------------------------------------------------------------------
    //
    // THE HEIGHT CHAIN LIVES HERE, and it is the reason this is modelled exactly.
    // #ReplayScrubber takes the rest of the row. #SliderAndMarkers has NO height of its own, so it
    // is sized by its children -- and #scrubInfo's 50px is the only intrinsic one, because
    // #TimeScrubContainer is height:100%. Our host sets scrubInfo's WIDTH to 0 rather than hiding
    // it, for exactly this reason.
    var scrubber = P('Panel', 'ReplayScrubber', main, {
      width: 'fill-parent-flow(1.0)', verticalAlign: 'center'
    }, ['replayScrubberContainer']);
    var sliderAndMarkers = P('Panel', 'SliderAndMarkers', scrubber, {
      flowChildren: 'right', width: '100%'
    });
    // 44px, MEASURED in game (build 6687), not the 50px the decompiled stylesheet says. The row
    // lays out at the play button's 44, and the game reports scrubInfo at 44 with it -- so the
    // stylesheet's 50 is either overridden by a rule the decompile does not show or clamped by the
    // row. The measurement wins: at 50 the stand-in bar was 66 tall against the game's 60, and every
    // panel in the dock sat 6px off.
    var scrubInfo = P('Panel', 'scrubInfo', sliderAndMarkers, {
      width: '90px', height: '44px',
      opacity: '0',
      horizontalAlign: 'right', verticalAlign: 'center'
    });
    P('Label', 'scrubInfoTime', scrubInfo, {
      color: 'white', fontSize: '20px', textAlign: 'right', verticalAlign: 'center'
    }).text = '12:04';

    var scrubBox = P('Panel', 'TimeScrubContainer', sliderAndMarkers, {
      width: '80%', height: '100%', verticalAlign: 'center'
    });
    // TICKS, NOT A 0..1 FRACTION. The game's own scrubber runs on the replay's tick span
    // (measured: min=0 max=71718 value=<current tick>), and our marker alignment reads exactly
    // those to place the event markers. A stand-in normalised to 0..1 would make that arithmetic
    // untestable here -- which is how the markers shipped misplaced in the first place.
    var scrub = P('Slider', 'TimeScrubSlider', scrubBox, {
      width: '100%', verticalAlign: 'center'
    }, ['HorizontalSlider']);
    scrub.min = 0; scrub.max = DEMO_END_TICK; scrub.value = 0;

    var markers = P('Panel', 'MarkersContainer', scrubBox, {
      width: '100%', verticalAlign: 'center', padding: '3px 0px'
    });
    // The game feeds these from the replay's own interval list, which never reaches our state feed,
    // so the mockup draws a plausible spread. They are what the Regular Timeline host measures to
    // place its own marker read-out.
    // THE GAME NAMES ITS MARKERS AFTER THE TICK THEY STAND FOR -- `kill_event_8651`,
    // `death_event_52780` -- and places each with an absolute pixel `position` worked out once,
    // against whatever width the scrubber had at that moment. That is the whole reason our host has
    // to re-place them: we widen the bar afterwards and the game never revisits them.
    //
    // The ids are reproduced exactly, because the alignment code parses the tick out of them; a
    // stand-in with anonymous markers would silently skip that path and the mockup would show a
    // correctly-aligned bar whatever the code did.
    //
    // The DELIBERATELY WRONG initial positions below are the fault being modelled: each is placed
    // against a 366px track, the width measured in game before our widening. A mockup that placed
    // them correctly to begin with could not show the fix working.
    var STALE_TRACK_W = 366;
    [
      { tick: 8651, kill: true }, { tick: 21983, kill: false },
      { tick: 25137, kill: true }, { tick: 33131, kill: false },
      { tick: 49843, kill: true }, { tick: 52780, kill: false },
      { tick: 66388, kill: true }, { tick: 67064, kill: true }
    ].forEach(function (m) {
      P('Panel', (m.kill ? 'kill_event_' : 'death_event_') + m.tick, markers, {
        position: (m.tick / DEMO_END_TICK * STALE_TRACK_W).toFixed(2) + 'px 0px 0px',
        width: '3px', height: '15px',
        backgroundColor: m.kill ? '#ffffff' : '#ace15d',
        opacity: '0.8'
      }, ['EventMarker', m.kill ? 'kill_event' : 'death_event']);
    });

    P('Panel', 'ExpandedPlaybackControls', playback, {
      flowChildren: 'down', width: '100%', visibility: 'collapse'
    });

    // ---- the layers beside HudCore ------------------------------------------------------------
    var tooltip = P('Panel', 'TooltipManager', ctx, {
      position: '300px 640px 0px', width: '260px', height: '54px',
      backgroundColor: '#10130Dee', border: '1px solid #444444ff', borderRadius: '4px'
    });
    P('Label', '', tooltip, {
      padding: '8px', color: '#FFEFD7', fontSize: '14px'
    }).text = 'Tooltip (scales with the HUD)';

    var ctxMenu = P('Panel', 'ContextMenuManager', ctx, {
      position: '900px 520px 0px', width: '180px', height: '40px',
      backgroundColor: '#10130Dee', border: '1px solid #444444ff', borderRadius: '4px'
    });
    P('Label', '', ctxMenu, {
      padding: '8px', color: '#FFEFD7', fontSize: '13px'
    }).text = 'Context menu';

    // NOT scaled, deliberately. Real modal menus belong to the whole window, and our code has to
    // leave them alone; they are here so the mockup can show that it does.
    var popups = P('Panel', 'PopupManager', ctx, { width: '100%', height: '100%' });
    popups.hittest = false;
    var escape = P('Panel', 'EscapeMenu', ctx, { width: '100%', height: '100%' });
    escape.hittest = false;

    return {
      core: core,
      replay: replay,
      options: options,
      scrubber: scrubber,
      scrubInfo: scrubInfo,
      scrubBox: scrubBox,
      scrub: scrub,
      markers: markers,
      speedControls: speed,
      playButton: playBtn,
      companions: [tooltip, ctxMenu],
      unscaled: [popups, escape],
      gameplay: [shop, abilities, healthbars]
    };
  }

  // The game drives its own bar every frame: the play button's icon follows the demo, and the
  // scrubber's thumb follows the playhead. Reproducing that is what lets the mockup show the bar
  // behaving while our host is holding it on screen.
  function tick(native, state) {
    if (!native) return;
    try {
      var paused = !!state.paused;
      native.playButton.SetHasClass('Play', paused);
      native.playButton.SetHasClass('Pause', !paused);
      var icon = native.playButton.GetChild(0);
      if (icon) {
        icon.style.backgroundImage =
          'url("' + ICON + (paused ? 'play_icon_psd.vtex' : 'pause_icon_psd.vtex') + '")';
      }
      // The bar's own scrubber runs on the DEMO's full span in TICKS, not on the shot's and not
      // normalised, which is why the Regular Timeline reads it rather than deriving a position of
      // its own.
      native.scrub.value = Math.max(0, Math.min(DEMO_END_TICK, state.tick || 0));
      var t = native.scrubber.FindChildTraverse('scrubInfoTime');
      if (t) {
        var secs = Math.max(0, Math.floor(state.time || 0));
        t.text = Math.floor(secs / 60) + ':' + String(secs % 60).padStart(2, '0');
      }
    } catch (e) { /* a panel our own code deleted; the next frame rebuilds */ }
  }

  global.MvmDeadlockNativeHud = { build: build, tick: tick };
})(window);
