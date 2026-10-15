# Deadlock UI style reference

This is the contract for making SOURCE:MVM's Panorama panels look like Deadlock instead of CS2.

The values below come from Deadlock's shipped Panorama. The Source 2 Viewer CLI decompiled `citadel/pak01_dir.vpk` into `reference/deadlock/`, which is gitignored. To re-extract, follow [reference-extraction.md](reference-extraction.md).

At runtime the hook does not read the extracted files. Panels reference `s2r://panorama/...` paths, and the game resolves them from its own VPK, as the CS2 panels do. The extraction lets panel authoring use real paths and real tokens.

## What Deadlock's Panorama is

| Type | Count | Size |
| --- | --- | --- |
| Textures (`vtex_c`) | 2,024 | 2,894 MB |
| Vector icons (`vsvg_c`) | 570 | 3.95 MB |
| Stylesheets (`vcss_c`) | 427 | 4.56 MB |
| Layouts (`vxml_c`) | 419 | 1.14 MB |
| Scripts (`vts_c` / `vjs_c`) | 6 | 0.10 MB |

Deadlock has 419 layouts and 427 stylesheets but only 6 scripts. Its UI is C++-driven panels built from XML and CSS, so the CSS is the style source of truth. No Deadlock JavaScript idiom exists to copy. Our panels stay imperative JS, as the CS2 panels do.

## Design tokens

Read these from `panorama/styles/citadel_base_styles.css`, where Deadlock keeps its `@define` block. Use the values directly in each panel's own token object.

### Fonts

| Token | Stack | Use |
| --- | --- | --- |
| `sans` | Retail Demo, Noto Sans | Default UI text |
| `sansMono` | Retail Text Demo, Noto Sans | Dense readouts |
| `serif` | Reaver | Headings with weight |
| `block` | VALVEPulp | Display and stencil headings |
| `oracle` | VALVEOracle, Reaver | Occult or diegetic accents |
| `numericSans` | MingLiu, Retail Demo | Numeric columns |

Font files are copied into `reference/deadlock/panorama/fonts/`: Retail Demo, Retail Text Demo, Reaver, VALVEPulp, VALVEOracle and Radiance. The `fonts.conf` file maps each family name to its files and per-language fallbacks. To mock a panel outside the game, install the fonts locally. In game, naming the family is enough, and Deadlock resolves it.

### Core colors

| Token | Value | Use |
| --- | --- | --- |
| `baseText` / `offWhite` | `#FFEFD7` | Body text, warm off-white, not pure white |
| `offBlack` | `#10130D` | Deepest ground |
| `colorCommonDarkGray` | `#2e2c27` | Panel ground |
| `colorCommonDarkestGray` | `#1a1a1a` | Recessed ground |
| `baseBorder` | `#444444ff` | Default border |
| `colorGold` / `goldColor` | `#FFED79` | Primary accent |
| `brandGreen` | `#5FE69E` | Interactive accent, slider thumbs |
| `soulColor` | `#70F8C1` | Secondary accent |
| `silvered` | `#C6C6C6` | Muted text |
| `eggshell` | `#DEC5A0` | Warm secondary text |
| `warningRed` | `#ee5945` | Warnings |
| `colorEnemy` | `#FF410D` | Destructive |
| `steamBlue` | `#1A9FFF` | Links |

Deadlock's palette is warm, low-chroma and gold-accented. The CS2 panels' `#f0b323` accent is close in spirit, but swap it for `#FFED79`.

### Blur

`ingameHudBlur: gaussian(2,2,2)` applies to in-game panels, and `menuBlur: mipmapgaussian(4,4,4)` applies to menu-weight surfaces. Deadlock blurs behind its HUD chrome. Matching that blur does most of the work of making a panel look native.

## Chrome conventions

Buttons use `.ButtonBevel` in `citadel_base_styles.css`. It has a vertical gradient from `#373d45` to `#4d5860`, a 1px solid border, `border-radius: 8px`, `padding: 4px 10px`, `min-width: 192px`, `min-height: 36px` and `box-shadow: #00000055 0px 0px 4px 0px`. Labels are uppercase with `letter-spacing: 2px` and `text-shadow: 2px 2px 0px 1.0 #000000`. Variants are `.Green`, `.DarkGreen`, `.Bronze` and `.Plus`. The active state moves the label 1px down.

Sliders use `.Slider`, which is 344x32 with a 20x20 thumb. The thumb has `border-radius: 100%` and a radial gradient into `brandGreen`. The track is an image, `s2r://panorama/images/buttons/slider_fill_psd.vtex`, masked by `s2r://panorama/images/masks/progress_bar_threshold_01_psd.vtex`. All dragging in our panels has to use Slider panels, because Panorama gives in-game HUD JS no mouse-move event. Match this styling properly.

Dropdowns use `.DropDownNew` with `.Border`, `.Background`, `.DropDownContent` and `.Arrow` child parts.

## Icon paths

Deadlock's CSS and XML reference 2,777 distinct image paths. These are the ones a camera or timeline UI needs:

- `s2r://panorama/images/transport_controls/icon_play_png.vtex`, `icon_play_large_png.vtex`, `icon_pause_png.vtex`, `icon_volume_01..04_png.vtex`
- `s2r://panorama/images/control_icons/arrow_solid_{up,down,left,right}_png.vtex`
- `s2r://panorama/images/icons/` holds 177 mostly-vector UI glyphs
- `s2r://panorama/images/masks/progress_bar_threshold_01_psd.vtex`
- `s2r://panorama/images/buttons/slider_fill_psd.vtex`

## Deadlock's replay HUD is reachable from JS

This changes the timeline design.

`panorama/layout/replay/citadel_hud_replay.xml` defines a native playback bar under the C++ panel class `CitadelHudReplay`. It contains `SpectatorOptionsContainer`, then `PlaybackControls`, then `MainPlaybackControls`, which holds four groups:

| Group | Contents |
| --- | --- |
| `.SpeedControls` | `SpeedDropdown` with 0.25, 0.5, 1, 1.5, 2, 4, 8, 16 plus step buttons |
| `.PlayControls` | `PlayButton` and skip buttons at -60, -10, +10, +60 seconds |
| `.MarkerControls` | `PrevMarkerButton`, `MarkerTypeDropdown`, `NextMarkerButton` |
| `CitadelHUDReplayScrubber` | `TimeScrubSlider` (a real horizontal `Slider`) plus `MarkersContainer` |

Its buttons call Panorama-exposed functions, so our JS can call them directly:

- `CitadelReplayIncrementSpeedStep( -1 | 1 )`
- `CitadelReplaySkipByTime( seconds )`
- `CitadelReplayNextMarker( forward )`

CS2 has no equivalent. Its speed control had to be tracked in C++, because `GetDemoControllerState().fTimeScale` does not exist on the in-game HUD context panel. Deadlock provides a speed dropdown, a scrub slider and marker navigation without extra work.

Marker styling to match, from `panorama/styles/replay/citadel_hud_replay_scrubber.css`: `.EventMarker` is 3x15 at `opacity: 0.8` on `#ace15d`, scaling to `pre-transform-scale2d: 2.5` on hover. `.kill_event` is `#ffffff` and `.death_event` is `rgb(225, 75, 75)`.

## Reverse-engineering anchors

The extraction answers two Phase 1 unknowns without disassembly:

- The HUD root layout is `panorama/layout/hud.xml`, not CS2's `layout/hud/hud.xml`. The client-side function that creates the HUD root should therefore be findable by the string `file://{resources}/layout/hud.xml`.
- The HUD root C++ panel class is `CitadelHud` (with class `WindowRoot`), the analogue of CS2's `CCSGO_Hud`. Related panel classes to expect in RTTI: `CitadelHudReplay`, `CitadelHUDReplayScrubber`, `CitadelPingIndicator`, `CitadelHeroImage`.

## Two rendering facts the mock-up cannot show

Both came from comparing the harness with the running game. Both look correct in a browser and wrong on screen.

Centring a label takes two properties. `horizontal-align: center` centres the label's box in its parent, not the text inside it. Once the label has a width (`100%`, or a fixed pixel width so a row's rhythm does not depend on the string), the box fills its parent and the align does nothing. The text then sits against the left edge of a button that otherwise looks centred. `$.MvmStyle.label` now adds `text-align: center` when a caller asks for `horizontalAlign: 'center'` and has not set `textAlign`, so the two properties stay together.

Not every Unicode glyph is in the font. `U+223F` (the wave on a SMOOTH button) renders as nothing in Deadlock's own font. `U+2571`, `U+25C0`, `U+25B6` and `U+2212` all draw. Icons come from the game's own art instead. `$.MvmStyle` names the arrows, the replay transport's play/pause and `images/icons/icon_gear.svg`. Captions carry no decorative glyphs.
