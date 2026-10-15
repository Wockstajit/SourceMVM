# Extracting Deadlock's Panorama reference

Use this to rebuild `reference/deadlock/`, the local decompile of Deadlock's own Panorama UI that [ui-style.md](ui-style.md) is derived from. Run it after a Deadlock update if the UI style or a referenced asset path looks stale.

`reference/` is gitignored, so nothing from these steps lands in the tree.

## Tool

The tool is Source 2 Viewer's CLI, the same helper the CS2 Panorama reference uses. It is a helper only. It is not a build dependency: CMake does not reference it, and `build.bat` never rebuilds it. Set `SOURCEMVM_VRF_CLI` to the `Source2Viewer-CLI.exe` path, or pass the path inline.

Source archive: `<Deadlock install>/game/citadel/pak01_dir.vpk`, as `$DEADLOCK_VPK` below.

## Step 1: layout, styles, scripts

About 10 MB. This step matters most, because everything in [ui-style.md](ui-style.md) comes from it. The filter takes the whole set, so a clean run leaves nothing behind.

```bash
"$SOURCEMVM_VRF_CLI" -i "$DEADLOCK_VPK" -o reference/deadlock -d --threads 8 \
  -f "panorama/layout/,panorama/styles/,panorama/scripts/,panorama/browser/,panorama/citadel_keybinds.cfg"
```

This yields 419 `.xml`, 428 `.css` and 6 `.js` files: every layout, stylesheet and script Deadlock ships.

## Step 2: vector icons

About 4 MB, 570 files, decompiled to `.svg`. This is also the complete set.

```bash
"$SOURCEMVM_VRF_CLI" -i "$DEADLOCK_VPK" -o reference/deadlock -d --threads 8 \
    -e "vsvg_c" -f "panorama/images/"
```

## Step 3: UI chrome textures

**Do not bulk-extract textures.** `panorama/` holds 2,024 of them, totalling 2.9 GB. Three directories account for most of that size, and none of them hold UI style: `images/heroes/backgrounds` (1,552 MB of full-screen hero art), `images/hud/brawl` (327 MB) and `images/ranked/badges` (118 MB).

The curated set is everything under `panorama/images/` except the directories below and any file over 4 MB. That gives 1,534 textures: 362 MB of `vtex_c` that decompresses to about 78 MB of PNG. The set covers the icon, panel, bar, badge and chrome vocabulary without the full-screen art.

Excluded directories: `heroes/backgrounds/`, `heroes/guns/`, `ranked/badges/`, `shop/catalog/`, `minimap/base/`, `hud/brawl/`, `main_menu/hero_release_vote/`, `hideout/`, `overviews/`.

Three things are kept even though they exceed the cap, because they are art rather than a backdrop: everything in `images/masks/`, and the two main-menu plates `main_menu/background_nyc_cityscape_bw_psd` and `main_menu/pick_screen/smoke_bg_psd`.

The 4 MB cap keeps this at 362 MB instead of 2.9 GB. It drops full-screen backdrops and hero art, and keeps every panel-sized asset. No CLI filter expresses a size cap, so build the file list first and feed it back in chunks:

```bash
"$SOURCEMVM_VRF_CLI" -i "$DEADLOCK_VPK" -l -f "panorama/" > /tmp/panorama-list.txt

awk '{ p=$1; s=$NF; sub("size:","",s)
       if (p !~ /vtex_c$/) next
       if (p ~ /images\/(heroes\/(backgrounds|guns)|ranked\/badges|shop\/catalog|minimap\/base|hud\/brawl|main_menu\/hero_release_vote|hideout|overviews)\//) next
       keep = (s+0 <= 4194304) || (p ~ /images\/masks\//) \
              || (p ~ /main_menu\/(background_nyc_cityscape_bw|pick_screen\/smoke_bg)_psd/)
       if (keep) print p }' /tmp/panorama-list.txt > /tmp/want.txt

split -l 120 /tmp/want.txt /tmp/chunk_
for c in /tmp/chunk_*; do
  "$SOURCEMVM_VRF_CLI" -i "$DEADLOCK_VPK" -o reference/deadlock -d --threads 8 \
    -e "vtex_c" -f "$(paste -sd, "$c")"
done
```

`-f` accepts exact file paths as well as directory prefixes. 120 paths per call stays well under the Windows command-line limit.

Check that the run extracted everything requested, instead of trusting the exit code:

```bash
sed 's#^panorama/##; s#vtex_c$#png#' /tmp/want.txt | sort > /tmp/expected.txt
(cd reference/deadlock/panorama && find images -name '*.png' | sort) > /tmp/actual.txt
comm -23 /tmp/expected.txt /tmp/actual.txt
```

Two misses are expected: `main_menu/background_gothic_jpg.vtex_c` wraps a JPEG and lands as `.jpeg`, and `dynamic_images_sentinel` sits at the `panorama/` root, not under `images/`.

Steps 1 to 3 together produce about 105 MB: 419 layouts, 428 stylesheets, 6 scripts, 570 SVG icons and 1,534 bitmaps.

## Step 3b: fonts and background videos

Neither is in the VPK. Deadlock ships both as loose files, so this step copies them instead of extracting. `$DEADLOCK_GAME` below is the `game/` directory beside `citadel/`.

Fonts live in `citadel/panorama/fonts/`. The directory is 143 MB, but 140 MB of that is CJK and Thai localisation fallbacks. The seven families that carry the look total 2.1 MB:

```bash
mkdir -p reference/deadlock/panorama/fonts
for f in radiance radiancem reaver retaildemo retailtextdemo valveoracle valvepulp; do
  cp "$DEADLOCK_GAME/citadel/panorama/fonts/$f-"*.otf \
     "$DEADLOCK_GAME/citadel/panorama/fonts/$f-"*.ttf reference/deadlock/panorama/fonts/ 2>/dev/null
done
cp "$DEADLOCK_GAME/citadel/panorama/fonts/fonts.conf" reference/deadlock/panorama/fonts/
```

`fonts.conf` maps family names to files and sets the per-language fallback chain. It confirms the family strings in [ui-style.md](ui-style.md). `core/panorama/fonts/` holds no fonts, only fontconfig rendering rules. Copy it only to read `conf.d/95-valve.conf`.

Videos live in `citadel/panorama/videos/`. The tree is 652 MB, but 509 MB is hero ability demos and 130 MB is new-player tutorial clips. The looping background and effect loops are the Deadlock equivalent of the CS2 main-menu background movie. They are seven files, 12 MB:

```bash
mkdir -p reference/deadlock/panorama/videos
cp -r "$DEADLOCK_GAME/citadel/panorama/videos/main_menu" \
      "$DEADLOCK_GAME/citadel/panorama/videos/pick_screen" \
      "$DEADLOCK_GAME/citadel/panorama/videos/ui" reference/deadlock/panorama/videos/
```

| File | Role |
| --- | --- |
| `main_menu/menu_streets_loop2.webm` | The dashboard background, looped forever |
| `main_menu/menu_streets_intro.webm` | Plays once over it on load, then hands off to the loop |
| `pick_screen/play_btn_grn_flame.webm` | Looping flame behind the play button |
| `pick_screen/play_btn_flame.webm`, `card_flame.webm`, `roster_bg_loop.webm` | Hero-select loops |
| `ui/friendslist_hover_flame.webm` | Friends-list hover loop |

Copy the pattern in `dashboard.xml`. Both menu movies are `MoviePanel` siblings with `scaling="stretch-to-cover-preserve-aspect"`, `disableaudio="true"` and `disableactivatepause="true"`. Only the loop carries `repeat="true"`. The other four are not named in any shipped XML or CSS, so C++ assigns their `src` at runtime.

With fonts and videos, the whole reference is about 119 MB.

## Where each surface lives

The table lists four surfaces worth copying. Each row gives its layout, the stylesheet that owns its look, and its art directory. Paths are relative to `reference/deadlock/panorama/`.

| Surface | Layout | Style | Art |
| --- | --- | --- | --- |
| Health bar | `layout/hud_health.xml` plus `hud_health_{container,pips,single_bar,stacked}.xml`, `hud_shields.xml` | `styles/hud_health*.css`, `styles/hud_shields.css` | `images/hud/healthbar/` (15) |
| Top HUD bar | `layout/citadel_hud_top_bar.xml` plus `_player`, `_player_details`, `_team`, `_chat` | `styles/citadel_hud_top_bar*.css` | `images/hud/top_bar/` (34), `images/heroes/` for the portraits |
| Pause / escape menu | `layout/hud_escape_menu.xml`, `layout/hud_paused.xml` | `styles/hud_escape_menu.css`, `styles/hud_paused.css` | `images/hud/core/`, `images/buttons/` |
| Main menu | `layout/base_dashboard.xml`, `layout/citadel_db_page_home.xml`, `layout/citadel_db_play_menu.xml` | `styles/dashboard.css`, `styles/citadel_db_page_home.css`, `styles/citadel_db_play_menu.css` | `images/main_menu/` (33 + `menu_images/`, `icons/`, `pick_screen/`, `play/`) |

Every HUD layout loads `styles/citadel_base_styles.css` and `styles/hud_common.css` first. These two hold the token source, so read them before any panel stylesheet.

The replay HUD that the Deadlock port delegates to is `layout/replay/`, `styles/replay/`, `images/hud/replay/` and `images/transport_controls/`.

## Step 4: find what a panel actually references

To pull a texture outside the curated set, list the paths that Deadlock's CSS and XML reference, then extract only those:

```bash
grep -rhoE 's2r://panorama/images/[A-Za-z0-9_./-]+\.(vtex|vsvg)' \
  reference/deadlock/panorama/styles reference/deadlock/panorama/layout | sed 's#s2r://##' | sort -u
```

2,777 distinct paths at the time of writing.

## Listing without extracting

To size a directory, or to check whether a path still exists after an update:

```bash
"$SOURCEMVM_VRF_CLI" -i "$DEADLOCK_VPK" -l -f "panorama/"
```

Each line is `<path> CRC:<crc> size:<bytes>`.
