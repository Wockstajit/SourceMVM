# Config resets

Each Config reset runs on one click. A card reset turns off the effects it owns and
restores their tuning. The button pinned at the bottom resets the whole selected tab:

| Button | Coverage |
| --- | --- |
| RESET CAMERA | Smooth View, Handheld, recoil, Weapon Bob, both pivot markers and offsets, depth of field, flare, shot FOV, viewmodel overrides, fisheye and crosshair |
| RESET EFFECTS | Every particle category and moneyshot, exposure, light/sun overrides, custom shadows, smoke colours, appearance and rendered-volume controls, all world tints, skybox/orientation and cloud overrides |
| RESET ENVIRONMENT | Scene and renderer masters, all climate passes and switches, weather, added objects/lights, map-light editing and independent relighting |
| RESET RAGDOLLS | Death animations, Nervous System, death-weapon keep/fire, physics overrides and Method Acting |

Engine lights, authored shadows and map sky/cloud/sun values return to their game defaults.
Lens presets preserve the enabled state; lens Reset switches the selected effect off.
Reset also works when tuning is already at its defaults but its effect is still on.

Camera, Effects and Ragdolls footers call `mirv_filmmaker effects reset camera|effects|ragdolls`.
Those commands share the backend lists used when Keep between demos is off.
Environment retains its scene reset/undo path. Card resets preserve unrelated additions;
resetting the last active pass also releases the renderer. Resetting a disabled scene
keeps it disabled, including when default sun writes would otherwise enable it.

For automation, `mirv_filmmaker config eval $.ConfigHud.reset('camera')` runs the same
registered action as the button. Whole-tab names are `camera`, `effects`, `environment`
and `ragdolls`; `$.ConfigHud.reset()` lists the registered card actions as well.
