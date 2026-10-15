#pragma once

// What this port does to Deadlock's OWN HUD: two buttons added to the native replay bar, and the
// gameplay HUD hidden while the workspace is open.
//
// THE BUTTONS. Deadlock ships a real replay bar -- `CitadelHudReplay`, at `#hud_replay` inside
// `#spectate_container` -- with speed controls, skip buttons, marker stepping and a scrubber. It is
// already on screen whenever a replay is playing, so it is where a reviewer's hand already is, and
// that is where CAM EDITOR and LOOP belong. They are appended to `#MainPlaybackControls`, which
// flows right, so they land at the right-hand end of the bar after the scrubber.
//
// Structure, read out of the game's own decompiled layout
// (reference/deadlock/panorama/layout/replay/citadel_hud_replay.xml):
//
//     CitadelHudReplay #hud_replay
//       #SpectatorOptionsContainer
//         #PlaybackControls            flow down, 8px padding, #252727ee
//           #MainPlaybackControls      flow RIGHT, width 100%   <-- appended here
//             .SpeedControls / .PlayControls / .MarkerControls / #ReplayScrubber
//           #ExpandedPlaybackControls  empty, collapsed unless .ShowExpandedPlaybackControls
//
// WE ARE GUESTS IN SOMEBODY ELSE'S PANEL. Two rules follow, and both have been paid for on the CS2
// side. Everything added is tagged with our own id so it can be found again and never added twice
// -- a panel rebuilt by the game (leaving a replay, changing map) would otherwise accumulate a new
// copy of the button row every time. And every native property this touches is RESTORED on
// teardown; a native panel left hidden by a tool that has closed is indistinguishable from a game
// bug, and the user cannot fix it without restarting.
//
// STYLING. Children added INTO a native panel inherit that panel's stylesheet, so the colours here
// are the replay bar's own, read from citadel_hud_replay.css rather than picked: `.JumpButton` is
// `#576886bb` with `#556683dd` on hover. That is why these read as part of the bar instead of as
// something bolted to it.
//
// HIDING THE GAMEPLAY HUD. `.HudCore` holds every gameplay panel AND `#spectate_container`, which
// holds the replay bar. So the rule is one line: hide every child of HudCore except the spectate
// container. That keeps the replay bar and loses the health bars, ability icons, ping markers and
// the rest, which would otherwise draw at full screen size over a preview that is no longer full
// screen -- which is exactly what the first screenshot of this workspace showed.

#define DEADLOCK_NATIVE_HUD_JS R"CFJS(
(function () {
  if ($.MvmNativeHud) return;
  var M = $.MvmStyle;
  if (!M) return;
  var S = M.S;

  var attached = false;
  var group = null, camBtn = null, configBtn = null, renderBtn = null, loopBtn = null;
  var speedDial = null;
  var speedHidden = false;
  var hidden = [];          // { panel: <native panel>, was: <its own visible flag> }
  var hudHidden = false;
  var lastOpen = null, lastLoop = null, lastWorkspace = null;

  function cmd(text) { M.cmd(text); }

  function find(id) {
    try { return $.GetContextPanel().FindChildTraverse(id); } catch (e) { return null; }
  }

  // The replay bar's own button look, from citadel_hud_replay.css. Pill rather than circle: these
  // carry words, and a round button cannot.
  //
  // COMPACT, BECAUSE THEY COMPETE WITH THE SCRUBBER. Everything in #MainPlaybackControls shares one
  // row, so every pixel these take is a pixel off the timeline. Measured at 1920x1080 with the
  // editor open: the button group was 488px of a 1299px bar -- 38% -- and the scrubber had 303px,
  // 23%. Reported as "the timeline is really compact and it's annoying", which it was.
  //
  // Smaller on every axis rather than just narrower: an 11px face with half the letter-spacing is
  // the bulk of the saving, and the width then follows the text instead of padding it out.
  function barButton(parent, id, text, width, onActivate) {
    var b = M.make(parent, 'Panel', id, {
      backgroundColor: '#576886bb',
      borderRadius: '13px',
      height: '26px',
      width: width + 'px',
      padding: '0px 5px',
      marginLeft: '3px',
      marginRight: '3px',
      verticalAlign: 'middle',
      flowChildren: 'right'
    });
    if (!b) return null;
    var lbl = M.label(b, id + 'Label', text, {
      color: S.text,
      fontSize: '11px',
      letterSpacing: '0.5px',
      textTransform: 'uppercase',
      verticalAlign: 'middle',
      horizontalAlign: 'center',
      width: '100%',
      textAlign: 'center'
    });
    try {
      b.SetPanelEvent('onactivate', onActivate);
      b.SetPanelEvent('onmouseover', function () { M.set(b, { backgroundColor: '#556683dd' }); });
      b.SetPanelEvent('onmouseout', function () { M.set(b, { backgroundColor: '#576886bb' }); });
    } catch (e) {}
    b.__lbl = lbl;
    return b;
  }

  // Appends the button row to the native bar. Returns false until the bar exists -- it does not
  // while the dashboard is up, and it is rebuilt on every replay load, so this is retried.
  function attach() {
    if (attached && group) {
      try { if (group.IsValid && group.IsValid()) return true; } catch (e) {}
      // The bar was rebuilt under us and took our row with it. Fall through and re-attach.
      attached = false;
      group = null;
    }

    var row = find('MainPlaybackControls');
    if (!row) return false;

    // Never add a second copy. A rebuilt bar is a new panel, but a bar that merely re-rendered is
    // the same one, and the difference is not visible from here.
    var existing = null;
    try { existing = row.FindChildTraverse('MvmBarButtons'); } catch (e) {}
    if (existing) { try { existing.DeleteAsync(0.0); } catch (e) {} }

    // fit-children, so the row reserves exactly the width of its two buttons. Without it the group
    // has no width of its own inside a right-flowing parent and the buttons wrap onto two lines.
    group = M.make(row, 'Panel', 'MvmBarButtons', {
      flowChildren: 'right',
      width: 'fit-children',
      height: '32px',
      horizontalAlign: 'right',
      verticalAlign: 'middle',
      marginLeft: '8px'
    });
    if (!group) return false;

    // CAM EDITOR, CONFIG and RENDER, following CS2's contract in Panorama/CameraTimelineJs.h: each
    // goes to ITS workspace, and the one you are already in is hidden. With nothing open you get
    // both; with a workspace open you get the single button that swaps to the other -- which is
    // what CS2 calls the ModeSwapBtn.
    //
    // The first version of this was one button labelled CAM EDITOR that ran `workspace toggle`.
    // From inside the Camera Editor it therefore offered to take you where you already were, and
    // the label said so. Reported as: "there's still a Camera Editor button, I click it, and it
    // takes me to the camera timeline. It needs to say Config."
    // THE SPEED DIAL, in place of Deadlock's own stepper.
    //
    // The game's control is a minus button, a dropdown and a plus button at the LEFT end of the
    // bar; it is hidden below and this replaces it. Ours goes in this group rather than where the
    // old one sat, because `#MainPlaybackControls` flows right and inserting ahead of existing
    // children means fighting the game's own child order for no gain.
    //
    // 240px, not 150. The dial reads as a strip of ticks scrolling past a fixed handle, and at
    // 150px there were barely three ticks either side of it -- the gesture worked and could not be
    // read. It lays out entirely from this number (its tick pool is sized from it), so widening it
    // is the whole change. The width comes out of the marker dropdown next to it, not out of the
    // scrubber: see holdMarkerDropdownNarrow.
    speedDial = $.MvmSpeedDial ? $.MvmSpeedDial.build(group, { width: 240, height: 32, id: 'MvmBarSpeed' }) : null;
    if (speedDial && speedDial.strip) M.set(speedDial.strip, { marginRight: '8px' });

    // THE WAY IN. These were removed once, on the reasoning that the dock's Regular Timeline row
    // carries the same three buttons -- but that row is part of the workspace, so it only exists
    // AFTER a workspace is open. Removing them here left no way to open one at all: the editor
    // rendered correctly and was simply unreachable, and the panel reports a 0x0 screen while it is
    // closed, which reads like the UI is broken rather than shut.
    //
    // They are the ENTRY POINT, so render() shows them only while nothing is open and hides the
    // whole set once you are inside. The dock row does the swapping from there, and the duplicate
    // set that move was avoiding never appears.
    camBtn = barButton(group, 'MvmBarCam', 'Cam Editor', 86, function () {
      cmd('mirv_filmmaker workspace camera');
    });
    configBtn = barButton(group, 'MvmBarConfig', 'Config', 58, function () {
      cmd('mirv_filmmaker workspace config');
    });
    renderBtn = barButton(group, 'MvmBarRender', 'Render', 58, function () {
      cmd('mirv_filmmaker workspace render');
    });
    loopBtn = barButton(group, 'MvmBarLoop', 'Loop', 60, function () {
      cmd('mirv_filmmaker loop toggle');
    });

    attached = true;
    return true;
  }

  // Hide every gameplay panel, keep the spectate container (which holds the replay bar we just
  // added buttons to). One rule rather than a list of ids: a list goes stale on the next update
  // and the failure is a stray health bar over the preview that nobody can explain.
  // The child of HudCore that CONTAINS the replay bar, found by walking up from the bar itself.
  //
  // The bar is not a direct child of HudCore -- the game nests it inside a spectate container which
  // is itself nested -- so "skip the child called spectate_container" never matched and the whole
  // branch got hidden, taking Deadlock's replay bar and our two buttons with it. Deriving the
  // ancestor from the live tree means a future update that re-nests the bar cannot reintroduce
  // that.
  // The ancestor chain from `id` up to (but not including) `core`, nearest first.
  //
  // Exempting the whole top-level branch that contains the replay bar was too coarse: that branch is
  // `#spectate_container`, which also holds the spectator strip, so the strip kept drawing at full
  // screen size over a shrunk world. Walking the chain lets every level scale its OFF-chain
  // siblings and descend through the one on it, so `hud_replay` alone ends up untransformed.
  function ancestorChain(core, id) {
    var chain = [];
    var p = null;
    try { p = $.GetContextPanel().FindChildTraverse(id); } catch (e) {}
    if (!p) return chain;
    try {
      while (p && p !== core) {
        chain.push(p);
        p = p.GetParent ? p.GetParent() : null;
      }
    } catch (e) {}
    return (p === core) ? chain : [];
  }

  function onChain(chain, panel) {
    for (var i = 0; i < chain.length; ++i) if (chain[i] === panel) return true;
    return false;
  }

  // Every panel that should be transformed: walk down from `core`, and at each level take the
  // siblings that are not on the chain. A panel on the chain is stepped THROUGH, never scaled --
  // scaling it would scale the replay bar with it.
  //
  // AND THE WALK STOPS AT THE TARGET. chain[0] is the exempt panel itself, and descending past it
  // collected ITS children -- `#SpectatorOptionsContainer` and everything under it -- so the bar's
  // whole contents were transformed into the preview rect while the bar's own frame stayed put.
  // From the outside that is a replay bar that is present, unhidden, the right size, at the right
  // y, and draws nothing where it should: three separate sessions of probing agreed the bar was
  // fine, because by every measurement it was. Only its insides had been moved.
  // Panels that live BESIDE HudCore but still belong to the picture.
  //
  // A tooltip for an ability icon is positioned against HUD that IS scaled into the preview, so
  // leaving its layer at full screen size puts the tooltip nowhere near the icon that raised it --
  // the "hovering over the UI doesn't scale correctly" report. Measured on the live tree: both are
  // full-screen siblings of HudCore and neither was being transformed.
  //
  // PopupManager and EscapeMenu are deliberately NOT here. They are real modal menus that belong to
  // the whole window rather than to the framed picture, and shrinking a settings dialog into the
  // preview would be its own bug.
  var COMPANION_LAYERS = ['TooltipManager', 'ContextMenuManager'];

  function companionPanels(out) {
    for (var i = 0; i < COMPANION_LAYERS.length; ++i) {
      var p = find(COMPANION_LAYERS[i]);
      if (p) out.push(p);
    }
    return out;
  }

  function gameplayPanels(core, chain) {
    var out = [];
    var target = chain.length ? chain[0] : null;
    var level = core;
    for (;;) {
      var next = null;
      try {
        for (var n = 0; n < level.GetChildCount(); ++n) {
          var c = level.GetChild(n);
          if (!c) continue;
          if (c.id === 'MvmWorkspaceRoot') continue;  // never transform ourselves
          if (onChain(chain, c)) { next = c; continue; }
          out.push(c);
        }
      } catch (e) {}
      if (!next) break;
      if (next === target) break;   // the exempt panel is a leaf here, not a level to descend into
      level = next;
    }
    return companionPanels(out);
  }

  function uiScaleX() {
    try { return $.GetContextPanel().actualuiscale_x || 1; } catch (e) { return 1; }
  }
  function uiScaleY() {
    try { return $.GetContextPanel().actualuiscale_y || 1; } catch (e) { return 1; }
  }

)CFJS" R"CFJS(  function findHudCore() {
    var ctx = null;
    try { ctx = $.GetContextPanel(); } catch (e) {}
    if (!ctx) return null;
    try {
      for (var c = 0; c < ctx.GetChildCount(); ++c) {
        var child = ctx.GetChild(c);
        if (child && child.BHasClass && child.BHasClass('HudCore')) return child;
      }
      return ctx.FindChildTraverse('HudCore');
    } catch (e) {}
    return null;
  }

  // Every native panel we have transformed, so they can all be released again. A panel left with an
  // inline transform after the workspace closes is a game bug from the user's side, and they cannot
  // undo it without restarting.
  var scaled = [];
  var scaleApplied = false;

  // Panels WE collapsed, with the value each had before we touched it. Kept apart from `scaled`
  // on purpose.
  //
  // THIS SEPARATION IS THE FIX FOR THE WORST BUG IN THE PORT. releaseHudViewport used to write
  // `visible = true` on every panel it released, and `scaled` is filled by applyHudViewport, which
  // only TRANSFORMS panels and never touches their visibility. So closing the editor force-showed
  // whatever the game had collapsed on its own schedule -- and several of HudCore's children are
  // not decoration. `#death_replay_loading`, `#hud_reportcard`, `CitadelHudHideout` and
  // `CitadelItemDraftPanel` are live game states. Shown from underneath, the client acted on them:
  // measured in game, one `mirv_filmmaker workspace off` on a playing replay left the engine on
  // `dl_hideout` with `demo=no` inside two seconds -- the replay disconnected, the world gone, the
  // screen black, and nothing short of a restart to bring it back. That is the reported
  // "pressing exit turns the game black", and the same force-show is why the dashboard afterwards
  // was covered in the game's own debug rectangles and half-loaded panels.
  //
  // So: restore visibility ONLY for panels this file collapsed, and only to what they were.
  var hiddenByUs = [];   // { panel: <native panel>, was: <its own visible flag> }

  // Drops the measured base positions. Called wherever the panels are handed back, because a panel
  // re-scaled later must be measured again in its own, untransformed layout.
  function forgetBasePositions() {
    try {
      for (var i = 0; i < scaled.length; ++i) {
        if (scaled[i]) { try { scaled[i].__mvmBasePos = null; } catch (e) {} }
      }
    } catch (e) {}
  }

  // Hand back the visibility of everything hideGameplayHud collapsed, and nothing else.
  function restoreHiddenPanels() {
    for (var i = 0; i < hiddenByUs.length; ++i) {
      var e = hiddenByUs[i];
      try {
        if (!e || !e.panel) continue;
        if (e.panel.IsValid && !e.panel.IsValid()) continue;
        e.panel.visible = e.was;
      } catch (ex) {}
    }
    hiddenByUs = [];
  }

  function releaseHudViewport() {
    forgetBasePositions();
    restoreHiddenPanels();
    for (var i = 0; i < scaled.length; ++i) {
      var p = scaled[i];
      try {
        if (!p || (p.IsValid && !p.IsValid())) continue;
        // Snap out of the scaled rect with NO animation first: the compositor otherwise keeps
        // drawing the old preview-sized surface even after Panorama reports transform=null. Then
        // release every inline override on the next frame, once the transform has settled, so the
        // game's own HUD animations work again afterwards.
        M.forget(p);
        p.style.transition = 'none';
        p.style.transform = 'none';
        (function (panel) {
          $.Schedule(0, function () {
            try {
              if (!panel || (panel.IsValid && !panel.IsValid())) return;
              panel.style.transformOrigin = null;
              panel.style.transform = null;
              panel.style.transition = null;
              // NO `panel.visible = true` HERE. We only transformed this panel; its visibility is
              // the game's, and writing it is what disconnected the replay. See hiddenByUs.
              // Cleared behind M.set's back, so its memory of this panel is now wrong.
              M.forget(panel);
            } catch (e) {}
          });
        })(p);
      } catch (e) {}
    }
    scaled = [];
    scaleApplied = false;
  }

  // Maps the full screen into `rect` (normalised [0,1] fractions, the same rect the engine renders
  // the world into) for every gameplay panel, leaving the replay bar alone at full size.
  function applyHudViewport(rect, screenW, screenH, sx_scale, sy_scale) {
    if (!rect) { releaseHudViewport(); return; }

    var x0 = rect[0], y0 = rect[1], x1 = rect[2], y1 = rect[3];
    if (!(isFinite(x0) && isFinite(y0) && isFinite(x1) && isFinite(y1) && x1 > x0 && y1 > y0)) {
      releaseHudViewport();
      return;
    }
    if (!(screenW > 1 && screenH > 1)) { releaseHudViewport(); return; }

    var core = findHudCore();
    if (!core) return;

    // Only the replay bar keeps its own size: it is the dock, it lives at the bottom of the SCREEN
    // rather than inside the picture, and our two buttons are in it.
    var panels = gameplayPanels(core, ancestorChain(core, 'hud_replay'));

    var sx = x1 - x0, sy = y1 - y0;
    var tx = x0 * screenW, ty = y0 * screenH;

    scaled = [];
    try {
      for (var n = 0; n < panels.length; ++n) {
        var c = panels[n];
        if (!c) continue;

        // MEASURED ONCE, AND THAT IS THE WHOLE FIX.
        //
        // This used to read actualxoffset/actualyoffset every frame. But the transform written
        // below is what MOVES the panel, so the next frame measured a position that already had
        // our transform in it and derived a slightly different one -- and `toFixed(1)` turned the
        // drift into a stable two-cycle that alternated forever (measured: 19.4px, 19.5px, 19.4px).
        //
        // An alternating value never matches M.set's memo, so the write the memo exists to prevent
        // happened on every panel on every frame: ~20 composited layers re-allocated at 60fps,
        // against a vertex-buffer list Panorama indexes with an unsigned short. That is the crash
        // this file already carries a warning about, reintroduced by the input rather than by the
        // write.
        //
        // The untransformed position only changes when the HUD re-lays-out, which for these panels
        // means the screen changed size, so that is what the cache is keyed on.
        var px = 0, py = 0;
        try {
          var base = c.__mvmBasePos;
          if (!base || base.w !== screenW || base.h !== screenH) {
            base = {
              w: screenW,
              h: screenH,
              x: (c.actualxoffset || 0) / (sx_scale || 1),
              y: (c.actualyoffset || 0) / (sy_scale || 1)
            };
            try { c.__mvmBasePos = base; } catch (e) {}
          }
          px = base.x;
          py = base.y;
        } catch (e) {}

        var ptx = tx - (1 - sx) * px;
        var pty = ty - (1 - sy) * py;
        var cssTx = ptx / sx, cssTy = pty / sy;

        // THROUGH M.set, WHICH SKIPS A WRITE THAT WOULD NOT CHANGE ANYTHING.
        //
        // This is the site that killed the game. Re-writing a 3D transform allocates a composited
        // layer with its own D3D11 vertex buffer, Panorama indexes those with an unsigned short,
        // and this ran on ~20 panels every frame -- 65535 in well under a minute, ending in
        // `CVertexBufferDx11 ... EnsureCapacity allocation count overflow`. Long before the crash
        // it presents as parts of the preview frozen on a still from before the last rescale,
        // because a saturated list cannot hand out geometry for the new one.
        //
        // The transform only actually changes when the preview rect does, so in the steady state
        // this now writes nothing at all.
        M.set(c, {
          transition: 'none',            // snap into the rect; no animated slide
          transformOrigin: '0% 0%',
          transform: 'translate3d(' + cssTx.toFixed(1) + 'px, ' + cssTy.toFixed(1)
            + 'px, 0px) scale3d(' + sx.toFixed(5) + ', ' + sy.toFixed(5) + ', 1)'
        });
        scaled.push(c);
      }
    } catch (e) {}
    scaleApplied = true;
  }

  // The clean-plate alternative: hide the gameplay HUD outright. Not the default -- a reviewer is
  // often looking at exactly this -- but a capture wants it gone.
  // Each panel's own visible flag is recorded before it is collapsed, so the restore hands the
  // game back what it had rather than a blanket `true`. A panel the game had already hidden stays
  // hidden afterwards, which is the whole difference between a clean plate and a wrecked HUD.
  function hideGameplayHud() {
    var core = findHudCore();
    if (!core) return;
    var panels = gameplayPanels(core, ancestorChain(core, 'hud_replay'));
    try {
      for (var n = 0; n < panels.length; ++n) {
        var p = panels[n];
        if (!p) continue;
        var already = false;
        for (var k = 0; k < hiddenByUs.length; ++k) if (hiddenByUs[k].panel === p) { already = true; break; }
        if (already) continue;
        var was = true;
        try { was = !!p.visible; } catch (e) {}
        p.visible = false;
        hiddenByUs.push({ panel: p, was: was });
      }
    } catch (e) {}
    scaleApplied = true;
  }
)CFJS" R"CFJS(  // ---- THE GAME'S REPLAY BAR STAYS ON SCREEN ---------------------------------------------------
  //
  // Deadlock hides its own replay bar on its own schedule -- during a match intro, a round break, a
  // seek, after an idle timeout -- by putting the `Hidden` class back on. For a movie tool that is
  // simply wrong: the transport is the one control that must never go away, and it vanishing at
  // the start of a match is exactly the reported annoyance.
  //
  // TWO CLASSES, NOT ONE. `CitadelHudReplay.Hidden` collapses the bar outright; but
  // `#SpectatorOptionsContainer.Hidden` slides it 272px off to the right instead
  // (citadel_hud_replay.css). Undoing only the first leaves the bar "visible" and parked off the
  // edge of its own container, which looks identical to it being gone.
  //
  // THIS RUNS EVERY FRAME, from the one driver that renders whether or not the workspace is open.
  // It used to live in the Regular Timeline component, which only renders while the workspace is
  // open AND its tab is selected -- so closing the editor left the bar hidden by our own hand with
  // nothing running to bring it back.
  var barHeld = [];
  // The replay bar's show/hide decision for THIS frame, so only one caller consumes the countdown
  // inside replayBarShouldShow.
  var barShowing = false;
)CFJS" R"CFJS(  // Hand every held panel back exactly as it was found: the visibility it had, and the class it
  // had. Restoring by writing a value instead of by remembering one is what left the game's replay
  // bar drawn across the dashboard after a replay ended, with our own buttons in it -- the inline
  // `visible = true` written here beats the stylesheet rule the game re-adds, so `AddClass` alone
  // could never put it away again.
  function releaseBarHolds() {
    for (var i = 0; i < barHeld.length; ++i) {
      var e = barHeld[i];
      try {
        var p = e && e.panel;
        if (!p || (p.IsValid && !p.IsValid())) continue;
        if (e.hadHidden && p.BHasClass && !p.BHasClass('Hidden')) p.AddClass('Hidden');
        if (e.hadLower && p.BHasClass && !p.BHasClass('hidden')) p.AddClass('hidden');
        p.visible = e.wasVisible;
      } catch (ex) {}
    }
    barHeld = [];
  }

  function holdPanelShown(id, show) {
    var p = find(id);
    if (!p) return;
    try {
      if (show) {
        var wasVisible = true, hadHidden = false, hadLower = false;
        try { wasVisible = !!p.visible; } catch (e0) {}
        try { hadHidden = !!(p.BHasClass && p.BHasClass('Hidden')); } catch (e0) {}
        try { hadLower = !!(p.BHasClass && p.BHasClass('hidden')); } catch (e0) {}
        var recovered = false;
        if (!p.visible) { p.visible = true; recovered = true; }
        if (hadHidden) { p.RemoveClass('Hidden'); recovered = true; }
        // Blitz's selection overlay uses the generic lowercase class on some builds instead of
        // CitadelHudReplay.Hidden. Treat both spellings as the same request to hide the replay bar.
        if (hadLower) { p.RemoveClass('hidden'); recovered = true; }
        if (recovered) {
          // Remember what it was, so the release hands the game back its own state.
          for (var i = 0; i < barHeld.length; ++i) if (barHeld[i].panel === p) return;
          barHeld.push({ panel: p, wasVisible: wasVisible, hadHidden: hadHidden, hadLower: hadLower });
        }
      } else if (p.BHasClass && !p.BHasClass('Hidden')) {
        // CLASS ONLY, DELIBERATELY. The show branch above writes the inline `visible` property,
        // which beats this stylesheet rule, so the bar does not actually go away here -- and it
        // must not. The dock DELEGATES play, skip and marker stepping by dispatching Activated on
        // this bar's own buttons, and a collapsed panel is not reliably dispatchable; the timeline
        // also measures its scrub track. So the bar stays laid out and reachable, and what stops
        // it showing through the dock is the dock's own opaque ground (DeadlockRegularTimelineJs.h),
        // not this class.
        p.AddClass('Hidden');
      }
    } catch (e) {}
  }

  // `st.demo` blips FALSE for a frame or two during a seek, and a bar that follows it raw flickers
  // on every scrub. Two seconds of memory is far longer than a blip and far shorter than anything
  // a user would call "still in a replay".
  var demoSeenAt = 0;
  var HOLD_FRAMES = 120;
)CFJS" R"CFJS(  function replayBarShouldShow(st) {
    if (st.demo) demoSeenAt = HOLD_FRAMES;
    else if (demoSeenAt > 0) demoSeenAt--;
    if (demoSeenAt <= 0) return false;   // not a replay: the bar is the game's business
    // The one case we DO hide it: our own editor is occupying the dock band, where the game's bar
    // would draw straight through the timeline or the graph.
    var tab = (st.dock && st.dock.tab) || 'native';
    return !(st.open && (tab === 'camera' || tab === 'graph'));
  }

  // The bar is as wide as the DOCK BAND, and on the Regular Timeline tab that band is the WHOLE
  // WINDOW.
  //
  // The Regular Timeline IS Deadlock's own bar, so it spans the window the way the game draws it
  // and overlays the render rather than insetting it -- it is translucent, and the game keeps
  // rendering behind it. Clamping it to the area left of the inspector column is what the other
  // two dock tabs need, because those are OUR opaque editors and they stop where the column
  // starts; doing it on the native tab instead cut the game's own bar short and left the right-hand
  // end of it, including the scrubber's tail, hard against nothing.
  //
  // Setting the width re-flows the bar properly: its container, the controls row and the scrubber
  // are all percentage-width in the game's own stylesheet, so they follow. Scaling it with a
  // transform would shrink the text instead, which is not the same thing.
  function holdBarWidth(st) {
    var b = find('hud_replay');
    if (!b) return;
    var g = st.layout;
    var nativeTab = !st.dock || st.dock.tab === 'native';
    // AN EXPLICIT PIXEL WIDTH, NEVER `width: 100%`.
    //
    // `CitadelHudReplay` is 272px in the game's own stylesheet and everything inside it is a
    // percentage of that, so the width has to be an actual number. Handing it `100%` instead
    // resolves against a parent that sizes to its children, and the whole bar collapses onto the
    // only child with a fixed size -- our button group. Measured: the game's play button, its
    // skips, its marker stepper and its scrubber all went to zero width while the speed dial,
    // CONFIG, RENDER and LOOP sat in a 512px stub at the right-hand edge, which is precisely the
    // "the regular timeline is missing the native timeline dock" report.
    //
    // THE BAND STOPS WHERE THE INSPECTOR COLUMN STARTS, on every tab.
    //
    // The Regular Timeline tab used to be given the whole window, on the reasoning that that tab IS
    // the game's bar and it overlays the render. The inspector column is full height now, so the
    // right-hand end of a window-wide bar -- which is exactly where our speed dial and workspace
    // buttons sit -- is drawn underneath it. Same width the dock itself uses
    // (DeadlockWorkspaceJs.h), and deliberately NOT g.areaW: the render workspace's JOB column
    // insets the picture but not the dock.
    //
    // EXCEPT IN RENDER, where the dock (and so the hosted bar) runs the FULL screen width instead
    // -- DeadlockWorkspaceJs.h's own dockW does the same renderWs check. Leaving this at
    // screenW - inspectorW here is why the hosted Regular Timeline stopped short of the right edge
    // on the Render tab while every other tab's band was already full width.
    var renderWs = st.workspace === 'render';
    var w = 0;
    if (st.open && nativeTab && g && g.valid) {
      w = Math.max(0, (g.screenW || 0) - (renderWs ? 0 : (g.inspectorW || 0)));
    }

    // THE ANCHOR GOES WITH THE WIDTH, and leaving it behind is what hid half the bar.
    //
    // Deadlock pins this panel to the RIGHT edge of the screen: it ships 272px wide and its own
    // stylesheet hides it by sliding it 272px further right, which only works flush against that
    // edge. A right-anchored panel grows LEFTWARDS, so widening it to the dock's width left it
    // spanning 520..1920 on a 1920 screen while the band it is meant to fill is 0..1400 -- and
    // everything past 1400, which is our whole button group (speed dial, Cam Editor / Config,
    // Loop), was drawn underneath the inspector column.
    //
    // Reported as "the regular timeline is missing the camera editor, the loop button and the
    // speed dial". Nothing was missing; it was behind the column.
    if (w > 64) {
      M.set(b, { width: Math.floor(w) + 'px', horizontalAlign: 'left' });
      barPinned = true;
      barWidened = true;
    } else {
      // No column in the way: hand the width and the anchor back and let the bar sit where the
      // game puts it. CLEARED, not set to '100%' -- the parent sizes to its children, so a
      // percentage collapses the whole bar onto the only child with a fixed size, which is our
      // button group.
      releaseBarAlign();
      releaseBarWidth();
    }
  }

  function releaseBarWidth() {
    if (!barWidened) return;
    var b = find('hud_replay');
    if (b) { M.forget(b); try { b.style.width = null; } catch (e) {} }
    barWidened = false;
  }

  // Gives the bar its own alignment back. Separate from the width restore because the width is
  // released in more places than the anchor is taken.
  function releaseBarAlign() {
    if (!barPinned) return;
    var b = find('hud_replay');
    if (b) {
      M.forget(b);
      try { b.style.horizontalAlign = null; } catch (e) {}
    }
    barPinned = false;
  }

  // Deadlock's own speed stepper -- minus button, speed dropdown, plus button -- collapsed so our
  // dial stands in for it. Its container has no id, so it is reached through the child that does.
  //
  // RE-ASSERTED EVERY FRAME, not written once. The replay bar is rebuilt on every replay load, so
  // a one-shot hide is lost the next time a demo is opened; and this used to be worse than that --
  // the function was never called at all. Two comments in the tree (here and in
  // DeadlockRegularTimelineJs.h) claimed the stepper was hidden while it was on screen the whole
  // time, next to our own dial, which is the reported duplicate speed control on the bar.
  function holdNativeSpeedHidden() {
    var center = find('SpeedControlsCenter');
    if (!center) return;
    var owner = null;
    try { owner = center.GetParent ? center.GetParent() : null; } catch (e) {}
    var target = owner || center;
    try {
      if (target.visible) { target.visible = false; }
      speedHidden = true;
    } catch (e) {}
  }

  // THE MARKER DROPDOWN PAYS FOR THE DIAL. Both live in #MainPlaybackControls, which is one row, so
  // the dial can only get wider at something else's expense and the scrubber is the last thing that
  // should shrink. The game ships #MarkerTypeDropdown at 150px for a label ("Kill & Death") that
  // measures 95px, so a third of it is slack.
  var markerDropdownNarrow = false;
  function holdMarkerDropdownNarrow() {
    var d = find('MarkerTypeDropdown');
    if (!d) return;
    M.set(d, { width: '118px' });
    markerDropdownNarrow = true;
  }

  function releaseMarkerDropdownWidth() {
    if (!markerDropdownNarrow) return;
    var d = find('MarkerTypeDropdown');
    if (d) { M.forget(d); try { d.style.width = null; } catch (e) {} }
    markerDropdownNarrow = false;
  }

  // "GAME IS PAUSED" IS NOISE IN A MOVIE TOOL. Pausing is how you frame a shot here, so a banner
  // announcing it sits in the middle of every composition you are trying to look at. Deadlock draws
  // it from #PausedInfo, a HudCore child, which the preview scaler transforms but never hides.
  //
  // Held only while the workspace is open, and its own visibility is remembered rather than assumed
  // -- the same rule every other native panel here follows, for the same reason: a banner the game
  // wanted and cannot get back reads as a game bug.
  var pausedBanner = null;
  function holdPausedBannerHidden() {
    var p = find('PausedInfo');
    if (!p) return;
    if (!pausedBanner) {
      var was = true;
      try { was = !!p.visible; } catch (e) { return; }
      pausedBanner = { panel: p, was: was };
    } else if (pausedBanner.panel !== p) {
      // The HUD was rebuilt under us; the old record describes a panel that no longer exists.
      releasePausedBanner();
      return holdPausedBannerHidden();
    }
    try { if (p.visible) p.visible = false; } catch (e) {}
  }

  function releasePausedBanner() {
    if (!pausedBanner) return;
    try {
      var p = pausedBanner.panel;
      if (p && (!p.IsValid || p.IsValid())) p.visible = pausedBanner.was;
    } catch (e) {}
    pausedBanner = null;
  }

  function releaseNativeSpeed() {
    if (!speedHidden) return;
    var center = find('SpeedControlsCenter');
    if (center) {
      try {
        var owner = center.GetParent ? center.GetParent() : null;
        (owner || center).visible = true;
      } catch (e) {}
    }
    speedHidden = false;
  }

  // GIVE THE TIMELINE THE BAR'S DEAD SPACE.
  //
  // Measured on the live bar: `#scrubInfo` is a 90px hover readout sitting to the LEFT of the
  // track. It is `opacity: 0` until the scrubber is hovered but occupies its width the whole time,
  // so the timeline started 90px in from nothing. And `#TimeScrubContainer` is `width: 80%` in the
  // game's own stylesheet, leaving another fifth of the scrubber empty.
  //
  // Collapsing the one and widening the other is the whole fix, and it is worth noting what it
  // costs: the hover time readout goes. It was drawn in a strange place and only on hover, and the
  // scrubber shows the same information on its own tooltip.
  var scrubWidened = false;
  var barPinned = false;
  var barWidened = false;
  var markersHeld = false;

  // How far before an event a marker click lands. The point of clicking a kill is to watch it
  // happen, and the game seeks to the exact tick, which is too late. Deadlock runs at 64 ticks
  // a second (the demo player reports interval=0.015625), so this is five seconds.
  var MARKER_LEAD_TICKS = 320;

  function holdScrubberWide() {
    // WIDTH ZERO, NOT HIDDEN. `#SliderAndMarkers` has no height of its own, so it is sized by its
    // children -- and `#scrubInfo` (height: 50px) is the only child that has one, since
    // `#TimeScrubContainer` is `height: 100%` of whatever the parent resolved to. Hiding scrubInfo
    // took the row's height with it and the entire timeline collapsed to zero. Emptying its width
    // frees the 90px of dead space and leaves the height chain intact.
    var info = find('scrubInfo');
    if (info) { M.set(info, { width: '0px' }); scrubWidened = true; }
    var box = find('TimeScrubContainer');
    if (box) { M.set(box, { width: '100%' }); scrubWidened = true; }
  }

  // THE GAME'S EVENT MARKERS, RE-PLACED AGAINST THE TRACK THEY ARE ACTUALLY ON.
  //
  // Each `.EventMarker` is positioned by the game with an absolute pixel offset worked out
  // once, against the width the scrubber had at that moment. We then widen the scrubber to
  // fill the dock band, and the game does not revisit them -- so every marker drifts left of
  // its own time, further the later it is. Measured on a 71718-tick replay: the last kill sat
  // 27px early, about 76 seconds.
  //
  // The id carries the tick (`kill_event_8651`), and the slider carries the span, so the
  // right position is arithmetic rather than a fudge:
  //
  //     x = (tick - min) / (max - min) * (trackWidth - markerWidth)
  //
  // which is exactly how the engine places the thumb -- verified: thumb at 30021/71718 in a
  // 398px track measured 164.1px, and the formula gives 164.1.
  //
  // Re-placed every frame through M.set, so a marker the game adds mid-replay is picked up
  // and the steady state still writes nothing.
  var markerTickRe = /_event_(\d+)$/;

)CFJS" R"CFJS(  function holdMarkersAligned() {
    var box = find('MarkersContainer');
    var slider = find('TimeScrubSlider');
    if (!box || !slider) return;

    var lo = 0, hi = 0, trackW = 0;
    try {
      lo = slider.min; hi = slider.max;
      var sx = uiScaleX() || 1;
      trackW = (box.actuallayoutwidth || 0) / sx;
    } catch (e) { return; }
    // Before the bar has laid out, or on a replay whose span the engine has not published
    // yet. Placing against either would scatter every marker onto the left edge.
    if (!(hi > lo) || !(trackW > 8)) return;

    var n = 0;
    try { n = box.GetChildCount ? box.GetChildCount() : 0; } catch (e) { return; }
    for (var i = 0; i < n; ++i) {
      var c = null;
      try { c = box.GetChild(i); } catch (e) {}
      if (!c) continue;
      var id = '';
      try { id = c.id || ''; } catch (e) {}
      var m = markerTickRe.exec(id);
      if (!m) continue;
      var tick = parseFloat(m[1]);
      if (!(tick >= 0)) continue;

      var mw = 3;
      try { mw = (c.actuallayoutwidth || 0) / (uiScaleX() || 1) || 3; } catch (e) {}
      var f = (tick - lo) / (hi - lo);
      if (f < 0) f = 0; else if (f > 1) f = 1;
      M.set(c, { position: (f * (trackW - mw)).toFixed(1) + 'px 0px 0px' });

      // A MARKER IS SOMETHING YOU CLICK TO WATCH, so it seeks to a lead-in rather than to the
      // event: the game's own handler drops you on the exact tick of the kill, which is the
      // moment after it is worth seeing. Bound once per panel.
      if (!c.__mvmSeek) {
        c.__mvmSeek = true;
        // THE TICK GOES THROUGH THE IIFE TOO, and leaving it out cost a whole build cycle. `var` is
        // function-scoped, so every handler closed over the SAME `tick` binding and read whatever it
        // held after the loop finished -- the LAST marker's. Clicking the first kill (8651) seeked to
        // 52460, which is the last marker (52780) minus the lead-in, every single time.
        (function (target, eventTick) {
          try {
            target.hittest = true;
            target.SetPanelEvent('onactivate', function () {
              var lead = eventTick - MARKER_LEAD_TICKS;
              if (lead < 0) lead = 0;
              // `seek`, NOT `scrub`: scrub drags the camera shot's playhead, which is a
              // different playhead entirely. Clicking a kill through it put the demo 44000
              // ticks away from the kill.
              cmd('mirv_filmmaker seek ' + Math.round(lead));
            });
          } catch (e) {}
        })(c, tick);
      }
      markersHeld = true;
    }
  }

  function releaseMarkersAligned() {
    if (!markersHeld) return;
    var box = find('MarkersContainer');
    if (box) {
      var n = 0;
      try { n = box.GetChildCount ? box.GetChildCount() : 0; } catch (e) { n = 0; }
      for (var i = 0; i < n; ++i) {
        var c = null;
        try { c = box.GetChild(i); } catch (e) {}
        if (!c) continue;
        // The position is the game's to own again. Its own next layout rewrites it; clearing
        // our override and forgetting the memo is what lets that happen.
        M.forget(c);
        try { c.style.position = null; } catch (e) {}
        try { c.ClearPanelEvent && c.ClearPanelEvent('onactivate'); } catch (e) {}
        try { c.__mvmSeek = false; } catch (e) {}
      }
    }
    markersHeld = false;
  }

  function releaseScrubberWide() {
    if (!scrubWidened) return;
    var info = find('scrubInfo');
    if (info) { M.forget(info); try { info.style.width = null; } catch (e) {} }
    var box = find('TimeScrubContainer');
    if (box) { M.forget(box); try { box.style.width = null; } catch (e) {} }
    scrubWidened = false;
  }

  // THE GAME'S BAR STAYS HIDDEN WHILE THE EDITOR IS OPEN.
  //
  // It used to be un-hidden and widened into the dock band, which is what "Regular Timeline means
  // the game's timeline" asked for. Measured in game, that bar does not paint: it reports a full
  // 1920px box and renders only the ~500px stub at its right, and no combination of un-hiding,
  // widening, un-clipping, re-laying-out or suppressing our own blit moves it (the reasoning and
  // the measurements are in DeadlockRegularTimelineJs.h). Hosting a panel that will not draw meant
  // an empty band where the transport should be.
  //
  // So the dock draws the transport and DELEGATES each action to this bar's own buttons, which
  // still work -- they just cannot be seen. The bar therefore has to stay in the tree, laid out and
  // reachable by id, and stay hidden so nothing of it shows through the band.
  function holdReplayBar(st) {
    // OUTSIDE THE EDITOR THE BAR IS THE GAME'S BUSINESS. We neither show nor hide it, exactly as
    // before -- forcing it either way while the tool is shut is how the game's own replay bar
    // ended up in a state the user could not undo without restarting.
    if (!st.open) {
      // Nothing of ours is in the band any more, so the bar goes back to the size and the anchor
      // the game gave it.
      releaseNativeBarHosting();
      // ASKED ONCE PER FRAME. replayBarShouldShow counts down a hold window, so a second caller in
      // the same frame silently halves it.
      barShowing = replayBarShouldShow(st);
      if (!barShowing) {
        // NOT A REPLAY ANY MORE: give the bar back rather than forcing it either way. Forcing it
        // hidden cannot work (the game re-shows it by class, and our inline write outranks that),
        // and leaving it forced visible is what put Deadlock's transport, its marker dropdown and
        // our own four buttons across the main menu after a replay ended.
        releaseBarHolds();
        releaseNativeSpeed();
        return;
      }
      holdPanelShown('hud_replay', true);
      // BOTH, always. `CitadelHudReplay.Hidden` collapses the bar outright, but
      // `#SpectatorOptionsContainer.Hidden` slides it 272px off to the right instead, so undoing
      // only the first leaves the bar "visible" and parked off its own edge.
      holdPanelShown('SpectatorOptionsContainer', true);
      return;
    }

    // THE REGULAR TIMELINE TAB **IS** THE GAME'S BAR.
    //
    // Hosted, not redrawn: the same transport, marker dropdown and scrubber you get with the editor
    // shut, with our speed dial and workspace buttons in it and the game's own stepper collapsed.
    // That is what "swap the regular timeline for the native timeline" asks for, and it also
    // removes a whole second set of controls that could disagree with the first.
    //
    // An earlier attempt at this concluded the bar "does not paint" and the dock has drawn its own
    // transport ever since. That measurement was taken while the bar was widened to the WHOLE
    // window and still right-anchored, so it spanned 520..1920 with everything past 1400 behind the
    // inspector column -- it was painting, off the side of the band nobody was looking at. Clamped
    // to the dock's own width and left-anchored it paints where it should.
    //
    // Three things have to travel with the width, and all three were already written for this and
    // never called: the anchor (holdBarWidth), the scrubber's dead space (holdScrubberWide), and
    // the event markers, which the game placed once in absolute pixels against a narrower track and
    // never revisits (holdMarkersAligned).
    var tab = (st.dock && st.dock.tab) || 'native';
    if (tab === 'native') {
      holdPanelShown('hud_replay', true);
      holdPanelShown('SpectatorOptionsContainer', true);
      holdBarWidth(st);
      holdScrubberWide();
      holdMarkerDropdownNarrow();
      holdMarkersAligned();
      reportBarHeight();
      return;
    }

    // OUR OWN EDITORS OWN THE BAND on the camera and graph tabs, so the bar goes away and takes its
    // widening with it.
    releaseNativeBarHosting();
    holdPanelShown('hud_replay', false);
    holdPanelShown('SpectatorOptionsContainer', false);
  }

  // THE BAND IS AS TALL AS THE BAR, measured rather than assumed.
  //
  // C++ reserves the dock's height before anything is laid out, and only Panorama can say how tall
  // Deadlock's bar actually is. Reserving the old 94px literal left a 34px strip of nothing between
  // our ribbon and the bar; reserving too little would put the ribbon over it.
  //
  // GUARDED AGAINST A FEEDBACK LOOP. The height we report changes the band, which moves the ribbon,
  // which could change what the bar lays out as -- and a value that alternates by a pixel would
  // send a console command every frame for the life of the session. That is the same shape as the
  // style-write loop this file already carries a warning about, so a change has to be worth more
  // than two pixels before it is worth telling C++ about.
  var lastBarH = 0;
  function reportBarHeight() {
    var b = find('hud_replay');
    if (!b) return;
    var h = 0;
    try { h = Math.round((b.actuallayoutheight || 0) / (uiScaleY() || 1)); } catch (e) { return; }
    if (!(h >= 40 && h <= 400)) return;          // same clamp Dock_SetMeasuredNativeHeight applies
    if (Math.abs(h - lastBarH) <= 2) return;
    lastBarH = h;
    cmd('mirv_filmmaker dock nativeheight ' + h);
  }

  // Everything holdReplayBar's hosting branch writes, undone. Called on the tab that does not host
  // it and whenever the bar stops being ours -- a native panel left widened, re-anchored and with
  // its markers moved is a game bug the user cannot undo without restarting.
  function releaseNativeBarHosting() {
    releaseMarkersAligned();
    releaseScrubberWide();
    releaseMarkerDropdownWidth();
    releaseBarAlign();
    releaseBarWidth();
  }

  // Ours replaces the game's stepper for as long as the bar is on screen with our row in it, and
  // only then: with no replay loaded the bar belongs to the game and so does its stepper.
  function holdSpeedStepper(st) {
    if (!attached) return;
    if (!st.open && !barShowing) return;
    holdNativeSpeedHidden();
  }

  function releaseReplayBar() {
    releaseNativeSpeed();
    releaseNativeBarHosting();
    var bar = find('hud_replay');
    if (bar) { M.forget(bar); try { bar.style.width = null; } catch (e) {} }
    // Give back exactly what was found. The game re-decides on its own schedule, and leaving the
    // bar forced visible after the tool has gone reads as a game bug the user cannot undo without
    // restarting.
    releaseBarHolds();
  }
)CFJS" R"CFJS(
  // F8. Set on the way in, so the way out (the very next frame blackout reads false) knows to
  // undo hideGameplayHud() regardless of what st.open/st.hudHidden say -- the normal per-frame
  // block below only calls restoreHiddenPanels() off ITS OWN edge-triggered `lastOpen`, which
  // never flips while the workspace stays closed for the whole blackout, and this would otherwise
  // leave the gameplay HUD stuck hidden after a blackout toggled off with the workspace shut.
  var wasBlackout = false;

  // THE SPECTATOR STRIP ("SPECTATING <hero>", Next Player, Change Camera...) GOES WHILE WE OWN THE
  // VIEW. CS2's free-cam preview carries no spectated-player panel, and this one names a hero the
  // camera is not showing.
  //
  // OPACITY, NEVER `visible`. The strip is one of the panels applyHudViewport TRANSFORMS, and
  // writing `visible` on a transformed native panel is what disconnected the replay (see
  // hiddenByUs). An inline opacity is ours alone: set once on the edge, cleared back to the
  // stylesheet's own value, and re-applied if the HUD is rebuilt under us (the flag lives on the
  // panel, so a fresh panel reads as not-yet-hidden).
  function holdSpectateStrip(hide) {
    var p = find('spectate_bar');
    if (!p) return;
    try {
      if (p.IsValid && !p.IsValid()) return;
      var was = !!p.__mvmSpecHidden;
      if (hide === was) return;
      p.style.opacity = hide ? '0' : null;
      p.__mvmSpecHidden = hide;
    } catch (e) {}
  }

  function render(st) {
    if (!st) st = {};
    // TEMPORARY bisect flags: $.MvmSkip = { attach, hold, viewport, dial }.
    var S = ($.MvmSkip = $.MvmSkip || {});

    // F8: every native panel this file manages disappears too, regardless of what the workspace
    // or `hud hidden/scaled` currently ask for. hideGameplayHud() is idempotent (it dedupes
    // against hiddenByUs), so calling it every blacked-out frame costs nothing once applied.
    //
    // THE BAR NEEDS A HARD visible=false, NOT holdPanelShown(id, false). Measured live: with the
    // workspace open on the native tab, holdReplayBar() had already force-shown hud_replay via
    // holdPanelShown(id, true) -- which writes the INLINE `visible` property -- so blackout still
    // showed the transport, uncovered, once our own chrome (that used to sit over it) went away
    // with the rest of the workspace. holdPanelShown's hide branch is deliberately a class-only,
    // inline-losing no-op for a different reason (the dock delegates clicks to this bar's own
    // buttons and needs it to stay reachable) -- that reasoning does not apply here, since nothing
    // is clickable during a blackout anyway. No restore bookkeeping needed either: holdReplayBar()
    // below fully re-derives and re-asserts the bar's real visibility from st every single frame
    // once blackout ends, edge or no edge.
    var capturing = !!(st.render && st.render.running);
    if (st.blackout || capturing) {
      if (!wasBlackout) releaseHudViewport();
      wasBlackout = true;
      if (st.blackout || st.render.hud === 'none') hideGameplayHud();
      else restoreHiddenPanels();
      var barPanel = find('hud_replay');
      if (barPanel) { try { barPanel.visible = false; } catch (e) {} }
      var specPanel = find('SpectatorOptionsContainer');
      if (specPanel) { try { specPanel.visible = false; } catch (e) {} }
      if (!S.hold) holdPausedBannerHidden();
      return;
    }
    if (wasBlackout) {
      wasBlackout = false;
      // Undo unconditionally; the block below re-derives the real state (still hidden if
      // st.hudHidden asks for that, scaled and shown otherwise) on this same frame.
      restoreHiddenPanels();
    }

    if (!S.attach) attach();
    if (!S.hold) holdReplayBar(st);
    // Our dial replaces the game's stepper wherever the bar carries our row, editor open or shut,
    // so the bar never shows two speed controls at once.
    if (!S.hold) holdSpeedStepper(st);
    if (!S.hold) holdSpectateStrip(!!st.camOwned);

    // The native HUD follows the preview rect EVERY frame, not just on the open/close transition:
    // the rect moves when the dock tab changes height, when the column resizes, and when the window
    // does. A one-shot apply leaves the game's HUD parked over the world's old position.
    var open = !!st.open;
    var g = st.layout;

    // Re-asserted every frame rather than on the open edge: the HUD is rebuilt on every replay
    // load, and Deadlock shows this banner on its own schedule, so a one-shot hide is lost the
    // first time the game decides to raise it again.
    if (!S.hold) { if (open) holdPausedBannerHidden(); else releasePausedBanner(); }

    if (!open || !g || !g.valid) {
      if (lastOpen !== false) { lastOpen = false; releaseHudViewport(); }
    } else {
      lastOpen = true;
      if (st.hudHidden) {
        hideGameplayHud();
      } else {
        // Coming back from a clean plate. Without this, `hud scaled` after `hud hidden` left every
        // gameplay panel collapsed and only the release-on-close ever brought them back.
        restoreHiddenPanels();
        // The SAME rect the engine renders the world into, derived from the same solved layout, so
        // the two cannot drift apart. No insets on either side or the HUD floats off its world.
        if (!S.viewport) applyHudViewport(
          [g.previewX / g.screenW, g.previewY / g.screenH,
           (g.previewX + g.previewW) / g.screenW, (g.previewY + g.previewH) / g.screenH],
          g.screenW, g.screenH, uiScaleX(), uiScaleY());
      }
    }

    // Hide whichever button names the workspace you are already in. Gold for the editor, blue for
    // Config, soul for Render: one accent each, matching the colour that column names itself in,
    // so a button and the column it opens are never two different colours for one place.
    var wsKey = (st.workspace || '') + '|' + (open ? '1' : '0');
    if (wsKey !== lastWorkspace) {
      lastWorkspace = wsKey;
      // THE BAR CARRIES THE SWAP, because the bar IS the Regular Timeline now.
      //
      // These were hidden while a workspace was open, on the grounds that the dock drew its own row
      // with the same three buttons and two sets would be one too many. The dock no longer draws
      // that row -- it hosts this bar -- so hiding them here left no way to reach another workspace
      // without closing the one you are in. CS2's contract instead: every button EXCEPT the one
      // naming the workspace you are already in, which is what makes it read as a swap rather than
      // an offer to take you where you already are.
      var ws = st.workspace || '';
      function swapVisible(key) { return !open || ws !== key; }
      if (camBtn) {
        try { camBtn.visible = swapVisible('camera'); } catch (e) {}
        M.set(camBtn, { border: '1px solid ' + S.gold });
      }
      if (configBtn) {
        try { configBtn.visible = swapVisible('config'); } catch (e) {}
        M.set(configBtn, { border: '1px solid ' + S.blue });
      }
      if (renderBtn) {
        // REDUNDANT WHILE EITHER EDITOR IS OPEN, not just while you're already in Render: both the
        // Camera Editor and Config columns now carry their own RENDER action in their footer
        // (DeadlockCameraBodyJs.h / DeadlockConfigRenderBodyJs.h, CS2's mvmActionRow), so this bar
        // button would be the exact same command sitting right next to it. Kept as one of the three
        // entry points on the bar only while NEITHER editor is open yet -- swapVisible('render')
        // already covers "hide it in Render itself"; this narrows the camera/config case too.
        try { renderBtn.visible = swapVisible('render') && !(open && (ws === 'camera' || ws === 'config')); } catch (e) {}
        M.set(renderBtn, { border: '1px solid ' + S.soul });
      }
      if (loopBtn) {
        try { loopBtn.visible = true; } catch (e) {}
      }
    }

    // The dial is cursor-driven, exactly as CS2's: pumped with this frame's pointer, then seeded
    // from the engine's published speed (which setSpeed ignores while a drag owns the handle).
    if (speedDial && $.MvmSpeedDial) {
      if (!S.dial) $.MvmSpeedDial.pump(speedDial, st);
      if (typeof st.speed === 'number') $.MvmSpeedDial.setSpeed(speedDial, st.speed);
    }

    var loopOn = !!st.loopOn;
    if (loopBtn && loopOn !== lastLoop) {
      lastLoop = loopOn;
      M.set(loopBtn, {
        border: loopOn ? ('1px solid ' + S.green) : M.CLEAR,
        backgroundColor: loopOn ? '#3f7a5fcc' : '#576886bb'
      });
      try { if (loopBtn.__lbl) loopBtn.__lbl.text = loopOn ? 'Loop On' : 'Loop'; } catch (e) {}
    }
  }
)CFJS" R"CFJS(  // Called when the hook tears the panels down. Restores everything native this touched -- a
  // native panel left hidden by a tool that has closed reads as a game bug, and the user has no
  // way to undo it short of restarting.
  function teardown() {
    holdSpectateStrip(false);
    releaseHudViewport();
    releasePausedBanner();
    releaseReplayBar();
    if (group) { try { group.DeleteAsync(0.0); } catch (e) {} }
    group = null; camBtn = null; configBtn = null; renderBtn = null; loopBtn = null; speedDial = null;
    attached = false;
    lastOpen = null; lastLoop = null; lastWorkspace = null;
  }

  $.MvmNativeHud = {
    render: render, teardown: teardown,
    attached: function () { return attached; },
    // For the probe: mirv_filmmaker ui_eval "$.Msg($.MvmSpeedDial.dbg($.MvmNativeHud.dial()))"
    dial: function () { return speedDial; }
  };
})();
)CFJS"
