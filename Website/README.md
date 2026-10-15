# SOURCE:MVM download page

A static one-page site with the keyed startup logo on the intro's orange plate, the beta download
button and the call for beta testers. It has no build step or framework. Any static host can serve
this folder as it is.

| File | Role |
| --- | --- |
| `index.html` | the page, all of its copy, and the first-visit check in `<head>` |
| `styles.css` | colour tokens and both layouts |
| `main.js` | public release discovery, logo playback and its timecode readout |
| `download-stats.js` | total installer and hotfix downloads across all public releases |
| `virustotal.json` | optional installer SHA-256 and matching owner-supplied report URL |
| `assets/` | generated logo media, share card, icon (see below) |
| `live-preview.bat` | live preview: serves the page and updates it on every save |
| `tools/live-server.mjs` | the server behind `live-preview.bat` (Node, no dependencies) |
| `tools/build-logo-assets.py` | regenerates `assets/` from the startup intro master |

## Downloads and GitHub Pages

The page is hosted at https://wockstajit.github.io/SourceMVM/ from this repository
(https://github.com/Wockstajit/SourceMVM), which also holds the application source. Complete
installers, updater metadata and checksums belong in public GitHub Releases, not in the tree.

GitHub's branch-based Pages can only serve the repository root or `/docs`. The site is deployed from `Website/` by [`.github/workflows/pages.yml`](../.github/workflows/pages.yml) instead. That needs the repository's Pages source set to **GitHub Actions** (Settings > Pages > Build and deployment > Source). Until you make that switch, Pages keeps serving the old root and the workflow's deploy step fails. Switching sources does not touch releases, stars or the updater feed. The workflow uploads only `Website/` (including `.nojekyll`). `tools/` and this README are published too, but they are inert.

`main.js` checks public GitHub Releases for the newest published, uploaded `SourceMovieMaker-Complete-Setup-*.exe`. The button always reads **Download**. With no installer, the button is unavailable and an accessible status reads **Download unavailable**. Beneath the button, the requirement reads **Needs Counter-Strike 2 on Steam**. There is no preparation label. If the API fails or is rate limited, the button keeps its known release download URL, or falls back to the public releases page when there is no static URL. Drafts and private-source URLs are excluded.

The download total sums uploaded installer (`.exe`) and package (`.zip`) downloads across
all published releases, including older versions and hotfixes. Repeat downloads count;
metadata, checksums and blockmaps do not. The counter follows every GitHub API page and
updates only after a complete result. If a request fails, it keeps the last checked
total shown in the page, with its date in the tooltip.

The checksums link selects the release's `SHA256SUMS.txt`. To show an owner-supplied VirusTotal
report, set `sha256` and `url` in `virustotal.json`. The report URL must be on VirusTotal's HTTPS
origin and contain that exact hash, and it must match the installer asset's GitHub digest. Missing,
stale or mismatched reports stay hidden. A hash and a scan report are evidence for readers to
inspect. Neither one guarantees that a program is safe.

The owner-supplied report for the installed `SourceMovieMaker.exe` uses that normal filename
in `fileName`, its verified file hash in `sha256`, and the matching setup hash in
`installerSha256`. The setup hash must match the current published asset before the link appears.
`release-verification.json` records the installed file hash checked against the packaged
installer. The link reads **VirusTotal report**, and its report covers that installed executable.

There is no export step any more. Pushing a change under `Website/` to `main` redeploys the site.
Publishing a complete release updates the button without changing the page.
The [launcher update instructions](../SourceMovieMakerLauncher/README.md#updates)
describe the release metadata needed for automatic app updates.

## First visit and later visits

On a first visit, the logo fills the screen. Everything else, the download included, is below the
fold: what SOURCE:MVM is, the beta call, then the download at the bottom. Every later visit puts
the download right under the logo, ahead of the beta notes. It arrives finished: the held logo
frame is there from the first paint, with no intro and no entrance animation.

A first visit opened in a background tab waits until the tab is shown before playing the intro,
because Chrome does not start silent video in a hidden tab.

The script at the top of `index.html` decides this before the page paints, using
`localStorage` (`smvm-visited`). The choice holds for the whole tab session (`sessionStorage`), so
reloading during a first visit keeps the first-visit layout. If storage is blocked, every visit
counts as a first visit. Both layouts use the same markup; `styles.css` only changes the section
`order` and styling under `[data-visit="returning"]`.

To preview a layout without touching storage, add `?first` or `?returning` to the URL. To see the
real first visit again, clear the site's storage in the browser.

## Live preview

Double-click `live-preview.bat` (needs [Node.js](https://nodejs.org)). It opens
`http://localhost:8840` in your browser and keeps the page in step with the files:

- saving a `.css` file restyles the open page in place, so the logo intro does not replay;
- saving anything else the page uses (HTML, JS, `assets/`) reloads it.

Close the console window to stop. If 8840 is taken, it uses the next free port and prints the
address. From a terminal: `node Website/tools/live-server.mjs [--port 8840] [--no-open]`. The
server adds its reload script to the page as it serves it, so `index.html` itself stays clean.

## Logo

The logo is keyed from [the CS2 startup intro](../Assets/startup-video/source-mvm-intro.mp4)
(black on an orange plate). `tools/build-logo-assets.py` turns the red channel into a
black-on-white matte, crops it around the wordmark, and trims it to frame 94, the last frame
without glitch fragments. The page lays the matte over its own orange with
`mix-blend-mode: multiply`. For a black logo that is exactly alpha compositing, and it works in
every browser, Safari included. When the video ends, `main.js` swaps in `assets/logo.png` (the
same frame, black with real alpha), so the logo stays on the final frame. That PNG also replaces
the video under reduced motion or when autoplay is blocked.

Keep the video's ancestors free of `transform`, `opacity`, `filter`, `isolation` and `z-index`.
Any of them isolates the blend, and the logo then shows inside a white box.

Regenerate after the intro changes (needs `ffmpeg` on PATH):

```bash
python Website/tools/build-logo-assets.py
```

## Social links

The header (left of the centred "Public beta" badge) and the footer both link to YouTube, X,
Instagram and GitHub. The brand icons are one inline SVG sprite at the top of `<body>`, with paths
from [Simple Icons](https://simpleicons.org) 16.34.0 (CC0-1.0), so the page makes no extra
requests for them. To add a network, copy its path from
`https://cdn.jsdelivr.net/npm/simple-icons@16.34.0/icons/<name>.svg` into a new `<symbol>` in the
sprite, then add a `<li>` with `<use href="#i-<name>">` and an `aria-label` to both `.socials`
lists. The noninteractive logo wrapper ignores pointer events so its transparent box cannot
intercept the header links; their appearance is unchanged.

## Interactive bits

- "I need beta testers." types itself out the first time it scrolls into view, then a slanted
  caret blinks after it. Screen readers get the whole heading at once.
- The REPORT A BUG replica in card 03 is a button: each click floats a "Bug #N reported" up out
  of it. It is only a reaction on the page and sends nothing.

Both settle to their final state immediately under reduced motion.

## Hosting notes

- Keep `assets/` out of Git LFS. GitHub Pages serves LFS pointer files, not the media.
- `og:image` and `og:url` use the GitHub Pages address, so shared links can resolve the preview.
