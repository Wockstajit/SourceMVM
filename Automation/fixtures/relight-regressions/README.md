# Relight regression captures
## Additional live acceptance checks

The [current contract](../../../Docs/effects/whole-map-relighting.md) distinguishes verified
results from pending checks. Preserve the camera pose and compare church wall/hand regions with
glass on/off; an open-door projection is not wall leakage. Test standing and crouching real
shots separately from held empty attack and fresh dry clicks. Spawn two infernos and verify
both receive moving atlas slots before testing their fade. Confirm weapon geometry is accepted
by shadow capture; shader acceptance alone does not establish visible casing shadows.

These are acceptance procedures. This fixture folder does not contain every latest
capture. Large captures/logs remain ignored under `Automation/output/`; retain compact evidence
with its settings and verification limits before cleaning intermediates.

The street and apartments camera fixtures preserve the reported problem locations.
`inferno-apartments-environment.json` preserves the original room controls. The fixed
preset keeps the authored lights visible and uses AO strength 0.6. GI, reflections and
shadows remain enabled. The lamp fixture contains both close-up camera positions.
For the matching fan comparison, use eye `[990.03125, 13.954691, 320]` and angles
`[-25, -51.921585, 0]`, with the fixed preset, at 1920 by 1080. Capture while the fan
rotates: its projected ceiling shadows must move while the static wall remains stable.
The original player view uses the angles in `inferno-apartments.json`.
The retained results are in [inferno-apartments-metrics.json](inferno-apartments-metrics.json).
The fan ceiling crop is `[800, 150, 1350, 390]`; the static-wall control is
`[1410, 200, 1600, 420]`. RGB temporal ranges were calculated from 48 selected frames
of each original burst. The compact [fan comparison](../../../Docs/effects/images/lighting-fan-before-after.png)
shows first/last frames and a labelled temporal-range heatmap. Generated bursts may be
trimmed after the comparison and metrics have been saved.
The [lamp comparison](../../../Docs/effects/images/lighting-lamp-before-after.png) uses the
same close-up pose before and after. The wall is at X=1156, twelve units behind light 126;
its cube-face boundary projects to row 718. Averaged luminance across columns 1035 to 1119
jumped by 29.45 code values across that boundary before, versus -0.72 after. Increasing
only the sample count left a 39.67-value jump, isolating face ownership as the cutoff.
This is a local image measurement. It does not claim map-wide lighting parity.
The final uncaptured eight-second stationary apartment run averaged 42.57 FPS with
25.64 ms p95 and no frames above 33 ms in that sample. It does not establish a map-wide
minimum or compare performance against a different resolution or scene.
The follow-up [hallway fixture](inferno-hallway.json) preserves the user view, matching
20-unit step, doorway fan receiver and near/distant stair fill views at 1920 by 1080.
Its [environment preset](inferno-hallway-environment.json) retains the user's AO strength
of 1, shown fill lights and exposure controls. Camera coordinates are eye positions;
engine `setpos_exact` places the player's feet, so account for the current view offset.

[Hallway measurements](inferno-hallway-metrics.json) retain all 59 consecutive ceiling
pixel differences, eight-second before/after walk timings and fixed-camera player-motion
brightness checks. The ceiling ROI `[770, 445, 850, 485]` excludes the moving fan mesh:
3 of 59 pairs changed by more than three RGB levels before, versus all 59 after.
This measures animated shadow refresh. It does not measure unwanted stationary flicker.
The [stair fill](../../../Docs/effects/images/lighting-fill-feather-before-after.png),
[window projection](../../../Docs/effects/images/lighting-window-before-after.png),
[doorway shadow cadence](../../../Docs/effects/images/lighting-doorway-shadow-cadence.png)
and [player-motion comparison](../../../Docs/effects/images/lighting-window-player-motion.png)
are saved as compact images. Full generated bursts may be trimmed after retaining these results.

The [ledge regression](inferno-ledges.json) retains 15 reported Inferno views at
2560 by 1351, 8x MSAA and FOV 90. Its [controlled environment](inferno-ledges-environment.json)
keeps the user's sun, ambient, AO, hidden fill lights and exposure settings; only clouds
are disabled for reproducible comparisons. Restore the user's saved environment afterward.
Inspect the lower wall joints and stairs as well as the upper walls and roofs. Move
forward and back at the ledges and Banana, and capture the doorway fan across multiple
frames. Large before/after captures and motion bursts are retained under
`Automation/output/lighting-ledges/`; the native mixed-sample fallback can retain thin
edge highlights. The saved route checks do not establish whole-map visual or speed parity.
