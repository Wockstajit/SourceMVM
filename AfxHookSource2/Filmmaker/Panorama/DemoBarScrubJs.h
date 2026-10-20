#pragma once

// Panorama JS injected into the in-game HUD context next to the speed scrubber (DemoBarButtons.cpp):
// LIVE SCRUBBING on CS2's own demo bar.
//
// The native bar only relabels while you drag and seeks once on release (huddemocontroller.js:
// SliderValueChanged sets the time text, SliderReleased calls GotoTick), so the world froze and
// then jumped. This observes the same #Slider and hands the drag to `mirv_filmmaker scrub`
// (Movie/DemoScrub.h), which PLAYS the world forward and seeks only backward.
//
// Only while the user is holding it: the native FrameUpdate writes slider.value every frame when
// `mousedown` is false, and those writes fire SliderValueChanged too. A click moves the value while
// the button is down, so it reaches the scrub as one drag and a release. The native release still
// calls GotoTick(floor(v)); the scrub seeks the paused demo to the whole tick before it, so that
// seek is a no-op or a one-tick forward skip (Docs/features/filmmaker_demo_scrubbing.md). The panel
// attribute stops a re-injection registering twice.

namespace Filmmaker {

inline const char* kDemoBarScrubJs =
R"DBSJS(
(function () {
  try {
    var ctx = $.GetContextPanel();
    var nativeSlider = ctx.FindChildTraverse('Slider');
    if (!nativeSlider) { $.Msg('[scrub] native demo slider not found\n'); return; }
    if (nativeSlider.GetAttributeString('mirvscrub', '') === '1') return;
    nativeSlider.SetAttributeString('mirvscrub', '1');
    var dragged = false;
    $.RegisterEventHandler('SliderValueChanged', nativeSlider, function (panel, v) {
      if (!nativeSlider.mousedown) return;
      dragged = true;
      try { GameInterfaceAPI.ConsoleCommand('mirv_filmmaker scrub drag ' + v.toFixed(2)); } catch (e) {}
    });
    $.RegisterEventHandler('SliderReleased', nativeSlider, function (panel, v) {
      if (!dragged) return;
      dragged = false;
      try { GameInterfaceAPI.ConsoleCommand('mirv_filmmaker scrub release ' + Math.floor(v)); } catch (e) {}
    });
    $.Msg('[scrub] live scrubbing attached to the demo bar.\n');
  } catch (err) {
    $.Msg('[scrub] error: ' + err + '\n');
  }
})();
)DBSJS"
;

} // namespace Filmmaker
