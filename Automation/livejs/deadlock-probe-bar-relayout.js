// The bar MEASURES full width and PAINTS a stub at the right.
//
// Panorama reported `#PlaybackControls` at 1280x40 and a magenta test fill appeared only across the
// right-hand ~340px of it -- so the layout is right and the RENDER is stale, clipped to the box the
// panel had when it was the game's own 272px widget. That is a cache, not a rectangle, and nothing
// that only writes styles will clear it.
//
// Three escalating nudges, each printed before it runs, so whichever one moves the paint is the
// one worth putting in the hook:
//   1. visibility off/on   -- cheapest, and what re-admits a panel to the draw list
//   2. width jiggle        -- forces a different value through, in case an identical write is elided
//   3. SetReadyForDisplay  -- Panorama's own "re-generate this panel" entry point, if it exists here
//
// Run with: mirv_deadlock livejs load <abs path to this file>

(function () {
  function find(id) {
    var root = $.GetContextPanel();
    if (!root) return null;
    try { return root.FindChildTraverse(id); } catch (e) { return null; }
  }

  var bar = find('hud_replay');
  if (!bar) { $.Msg('RELAYOUT\tno hud_replay\n'); return; }

  var w = 0;
  try { w = Math.round(bar.actuallayoutwidth / (bar.actualuiscale_x || 1)); } catch (e) {}
  $.Msg('RELAYOUT\tbar is ' + w + ' wide before the nudge\n');

  // 1) visibility off/on, one frame apart.
  try { bar.visible = false; } catch (e) {}
  $.Schedule(0.05, function () {
    try { bar.visible = true; } catch (e) {}
    $.Msg('RELAYOUT\tstep 1: visibility toggled\n');

    // 2) a width that is definitely different, then the one we want.
    $.Schedule(0.35, function () {
      try { bar.style.width = (w - 1) + 'px'; } catch (e) {}
      $.Schedule(0.05, function () {
        try { bar.style.width = w + 'px'; } catch (e) {}
        $.Msg('RELAYOUT\tstep 2: width jiggled through ' + (w - 1) + ' back to ' + w + '\n');

        // 3) Panorama's own re-generate, when the build exposes it.
        $.Schedule(0.35, function () {
          var had = false;
          try {
            if (bar.SetReadyForDisplay) { bar.SetReadyForDisplay(false); bar.SetReadyForDisplay(true); had = true; }
          } catch (e) {}
          $.Msg('RELAYOUT\tstep 3: SetReadyForDisplay ' + (had ? 'called' : 'not available') + '\n');
          $.Msg('RELAYOUT\tdone\n');
        });
      });
    });
  });
})();
