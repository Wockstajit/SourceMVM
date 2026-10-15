#pragma once

// Director status -- CS2's MovieHudJs.h (a full-screen transparent root holding two anchored
// cards: "MOVIE DIRECTOR" top-right, "CONTROLS" top-left), ported onto Deadlock's own token
// system (DeadlockStyleJs.h) instead of CS2's ad hoc inline styles and CS2-only VPK assets (the
// bluedots texture, the top-bottom fade mask) that do not exist in Deadlock's own VPK. LAYOUT
// parity means the same two cards, the same anchors, the same section order and row labels inside
// each; STYLING parity means Deadlock's own card look (bordered panel, S.panelBg) instead of CS2's
// scoreboard look, per this repo's "Deadlock styling only differs" rule.
//
// Trimmed to what Deadlock's runtime actually has: no MovieMode (Deadlock has no first/third/free-
// person spectator toggle the tool drives; the Mode row instead shows the view-owner resolver's
// answer -- Game/free camera/follow camera/camera path/graph -- which is a superset of CS2's three
// states, not a downgrade), no ThirdPersonCamera, no cosmetic mesh-group debug section. The
// CONTROLS card lists ONLY the hotkeys Deadlock actually has (DeadlockHotkeys.cpp's own binding
// table) with accurate descriptions of what they really do -- CS2's own "F8 - Show/hide this
// panel" row is stale relative to its actual HudBlackout behaviour (verified live: F8 during an
// active demo blacks out the whole screen, not just this card), and copying stale wording forward
// would be a second copy of the same bug instead of a fix.
//
// SHOWN ONLY WHILE THE WORKSPACE IS CLOSED -- verified against CS2 itself, not assumed: CS2's own
// CameraEditorHud.cpp and ConfigHud.cpp explicitly call MovieHudRef().SetVisible(false) on open and
// restore the PRIOR value on close (AfxHookSource2/Filmmaker/Panorama/CameraEditorHud.cpp:141-142,
// 201) -- confirmed live, screenshot-compared: opening CS2's own Camera Editor makes both cards
// disappear completely, not shrink into the preview. So "hide while the dedicated editor workspace
// is open" is not a Deadlock-specific deviation; it is the actual CS2 behaviour, and this matches it.
//
// BUILT LAZILY, RE-ATTACHING ON A STALE PANEL -- the same rule DeadlockNativeHudJs.h's attach()
// follows, and for the same reason: a workspace rebuild (`ui rebuild`, or the HUD context itself
// being recreated on a new demo) nulls $.MvmWorkspace and friends so THEY re-run from scratch, but
// this fragment's own top-of-file guard (`if ($.MvmDirectorHud) return;`) is never cleared, so its
// closure survives across that rebuild. Building the panels once into `var`s captured at load time
// would leave that closure holding deleted panels forever; checking IsValid() on every render and
// rebuilding when it fails is what NativeHud already does for its own row of buttons.
//
// A SIBLING OF MvmWorkspaceRoot (a direct child of the HUD context, not of the root), so
// `show(root, st.open)` in DeadlockWorkspaceJs.h never touches it -- and rendered from the SAME
// per-frame `st` object the workspace already has, since this lives in the one script
// DeadlockWorkspaces.cpp builds and pumps every frame regardless of whether the workspace itself is
// open. No separate PanoramaBridge, no separate attribute read: $.MvmDirectorHud.render(st) is just
// one more call in that render chain.

#define DEADLOCK_MOVIE_HUD_JS R"CFJS(
(function () {
  if ($.MvmDirectorHud) return;
  var M = $.MvmStyle;
  if (!M) return;
  var S = M.S;

  // Translucent badge fill for the CONTROLS card's key caps. rgba(), not a hex literal: this file
  // is not DeadlockStyleJs.h, the one file allowed to spell one out (deadlock-style-lint-tests.mjs).
  var BADGE_BG = 'rgba(255,255,255,0.08)';

  var root = null, cardRight = null, cardLeft = null;
  var vMode, vState, vSpeed, vPos, vAng, vFov, vTT;

  function sep(parent) {
    return M.make(parent, 'Panel', '', {
      width: '100%', height: '1px', backgroundColor: S.border,
      marginTop: '6px', marginBottom: '6px'
    });
  }

  function cardHeader(parent, text) {
    var h = M.make(parent, 'Panel', '', { flowChildren: 'right', width: '100%' });
    M.make(h, 'Panel', '', {
      width: '3px', height: '13px', backgroundColor: S.gold, borderRadius: '2px',
      marginRight: '8px', verticalAlign: 'middle'
    });
    M.label(h, '', text, {
      color: S.gold, fontSize: '12px', fontWeight: 'bold', letterSpacing: '1.5px',
      verticalAlign: 'middle'
    });
    return h;
  }

  function caption(parent, text) {
    var l = M.label(parent, '', text, {
      color: S.textMuted, fontSize: '10px', fontWeight: 'bold', letterSpacing: '1px',
      textTransform: 'uppercase'
    });
    l.style.marginBottom = '6px';
    return l;
  }

  // "label .......... value" row; returns the value label for per-frame updates.
  function kv(parent, keyText) {
    var row = M.make(parent, 'Panel', '', { flowChildren: 'right', width: '100%', marginBottom: '4px' });
    M.label(row, '', keyText, { color: S.textMuted, fontSize: '12px', width: 'fill-parent-flow(1.0)' });
    return M.label(row, '', '', {
      color: S.text, fontSize: '12px', fontWeight: 'bold', horizontalAlign: 'right', textAlign: 'right'
    });
  }

  // A control row: [ KEY badge ] description -- CS2's CONTROLS card layout exactly, Deadlock's
  // own keys.
  function control(parent, keyText, desc) {
    var row = M.make(parent, 'Panel', '', {
      flowChildren: 'right', width: '100%', marginBottom: '5px', minHeight: '18px'
    });
    var badge = M.make(row, 'Panel', '', {
      backgroundColor: BADGE_BG, borderRadius: '4px',
      paddingTop: '2px', paddingBottom: '2px', paddingLeft: '7px', paddingRight: '7px',
      minWidth: '42px', marginRight: '9px', verticalAlign: 'middle'
    });
    M.label(badge, '', keyText, {
      color: S.text, fontSize: '12px', fontWeight: 'bold', horizontalAlign: 'center', textAlign: 'center'
    });
    M.label(row, '', desc, {
      color: S.textMuted, fontSize: '12px', verticalAlign: 'middle', width: 'fill-parent-flow(1.0)'
    });
    return row;
  }

  // Builds both cards against WHATEVER context panel exists right now. Never assumes the one from
  // an earlier call is still good -- see the file header on why that assumption breaks.
  function ensureBuilt() {
    if (root) {
      try { if (root.IsValid && root.IsValid()) return true; } catch (e) {}
      root = null; cardRight = null; cardLeft = null; // fall through and rebuild
    }

    var ctx = null;
    try { ctx = $.GetContextPanel(); } catch (e) {}
    if (!ctx) return false;

    // Never a second copy: a HUD rebuilt under us is a new context, but the old panel usually is
    // not gone yet at the exact instant this runs, so an existing one by this id is a leftover
    // rather than proof this build already happened this context.
    var existing = null;
    try { existing = ctx.FindChildTraverse('MvmDirectorHudRoot'); } catch (e) {}
    if (existing) { try { existing.DeleteAsync(0.0); } catch (e) {} }

    // Full-screen and transparent, exactly like CS2's MovieHudRoot -- the two cards below anchor
    // themselves to its corners. Above Deadlock's own (unindexed, effectively 0) gameplay HUD so
    // neither card is ever drawn under it, and well below every one of our own modal layers
    // (220+) -- moot anyway, since those only exist while the workspace is open, and this whole
    // root is hidden whenever it is.
    root = M.make(ctx, 'Panel', 'MvmDirectorHudRoot', { width: '100%', height: '100%', zIndex: 100 });
    if (!root) return false;
    root.hittest = false;

    // ---- RIGHT CARD: "DIRECTOR" -- mode/playback/speed, then the CAMERA readout. ----
    cardRight = M.card(root, 'MvmDirectorHudCardRight', {
      width: '260px', horizontalAlign: 'right', verticalAlign: 'top',
      marginTop: '12px', marginRight: '12px'
    });
    cardHeader(cardRight, 'DIRECTOR');
    sep(cardRight);
    vMode = kv(cardRight, 'Mode');
    vState = kv(cardRight, 'Playback');
    vSpeed = kv(cardRight, 'Cam speed');
    sep(cardRight);
    caption(cardRight, 'CAMERA');
    vPos = kv(cardRight, 'Pos');
    vAng = kv(cardRight, 'Ang');
    vFov = kv(cardRight, 'FOV');
    vTT = kv(cardRight, 'Tick / Time');
    // NOT S.fontNumeric here -- it pulls in MingLiu (a CJK face) purely for its numeral shapes, and
    // that font's own line metrics are taller than the sans body font, which showed up as an extra
    // gap under exactly these two rows and nowhere else on the card. Matching the row rhythm every
    // other line on this card already has matters more here than numeral styling.

    // ---- LEFT CARD: "CONTROLS" -- a static reference, Deadlock's real bindings only. ----
    cardLeft = M.card(root, 'MvmDirectorHudCardLeft', {
      width: '260px', horizontalAlign: 'left', verticalAlign: 'top',
      marginTop: '12px', marginLeft: '12px'
    });
    cardHeader(cardLeft, 'CONTROLS');
    sep(cardLeft);
    // ONLY the keys that actually fire with the workspace closed -- which is the only state this
    // card is ever shown in. SPACE and the arrow skip keys also exist (DeadlockHotkeys.cpp), but
    // Deadlock's own native HUD binds those same keys during ordinary spectating (Change Team,
    // its own scrubber), so they require the workspace open and belong to the editor's own
    // SHORTCUTS card instead, not here -- listing them here would advertise keys that do nothing
    // in the one state this card is visible.
    control(cardLeft, 'F8', 'Clear every overlay (blackout)');
    control(cardLeft, 'K', 'Place camera marker');

    return true;
  }

  function f1(n) { return (typeof n === 'number') ? (Math.round(n * 10) / 10).toFixed(1) : '-'; }

  function render(st) {
    var d = (st && st.director) || null;
    var visible = !!(d && d.visible);

    // Only pay for a build while there is something to show -- a replay that never opens the
    // director card should never rack up rebuild attempts against a missing context.
    if (!visible) {
      if (root) { try { if (root.IsValid && root.IsValid()) M.vis(root, false); } catch (e) {} }
      return;
    }
    if (!ensureBuilt()) return;

    M.vis(root, true);
    M.txt(vMode, d.owner || '-');
    vMode.style.color = d.freecam ? S.gold : S.text;
    M.txt(vState, d.playing ? (d.paused ? 'Paused' : 'Playing') : 'No demo');
    vState.style.color = d.paused ? S.gold : S.text;
    M.txt(vSpeed, f1(d.speed) + 'x');
    M.txt(vPos, f1(d.ox) + ', ' + f1(d.oy) + ', ' + f1(d.oz));
    M.txt(vAng, f1(d.pitch) + ', ' + f1(d.yaw) + ', ' + f1(d.roll));
    M.txt(vFov, f1(d.fov));
    var tk = (typeof d.tick === 'number') ? String(d.tick) : '-';
    M.txt(vTT, tk + '  ·  ' + f1(d.time) + 's');
  }

  function teardown() {
    if (root) { try { root.DeleteAsync(0.0); } catch (e) {} }
    root = null; cardRight = null; cardLeft = null;
  }

  $.MvmDirectorHud = { render: render, teardown: teardown };
})();
)CFJS"
