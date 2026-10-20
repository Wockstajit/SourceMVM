# Publishing SOURCE:MVM releases

The public SourceMVM repository (`Wockstajit/SourceMVM`) holds the source, the website
(`Website/`, deployed by `.github/workflows/pages.yml`) and the release downloads. Development
happens in this same repository. The earlier private repository's history was never published and
is not part of it.

## Versions

One number names a release everywhere: `SourceMovieMakerLauncher/package.json` `version`, the git
tag `v<version>`, the installer name `SourceMovieMaker-Complete-Setup-<version>.exe` and the
`latest.yml` inside that release. Keep them identical. The launcher looks up runtime hotfixes
under the tag `v<its own version>` (`applyRuntimeHotfix` in `electron/runtime-payload.cjs`), so a
release tagged differently from its launcher never receives a hotfix.

## How installed launchers find an update

Check this before every publish. It is what decides whether existing installs move to the new build.

Installed launchers check only when the user has switched on **ADVANCED > UPDATES > Check for
updates automatically** (off by default, stored as `autoUpdate` in `launcher.json`), or
when the user presses the **UPDATES** button. A launcher that cannot reach the update server logs
that once and launches the runtime it has. Everything below applies to launchers that do check.

1. The launcher's own guard (`electron/update-service.cjs`) reads the newest non-draft release from
   the GitHub API. If that release has no `latest.yml`, every launcher that checks shows an update
   error and refuses to launch CS2 until it is fixed.
2. electron-updater runs with `allowPrerelease = true` on a stable version, so it takes the
   **first entry of the repository's releases Atom feed**, whatever its tag, downloads that
   release's `latest.yml`, and offers the installer when that version is higher than its own.
   The launcher asks the user before downloading; a declined update stays available through the
   UPDATES button and the current version keeps launching.
3. So the new release must be the newest release in the feed, must be published (not a draft), and
   must carry `latest.yml`, the installer and its blockmap. Prerelease or stable both work.
4. A release created later (even a hotfix-only one) becomes the feed's first entry. Never create a
   release without a `latest.yml`. Hotfixes go onto the existing release (below).

Launchers from 0.1.0 onward use this path. Launchers before 0.1.9 also ignore runtime hotfixes.

## Publishing the source to SourceMVM

This repository is the only one. Source reaches `main` through reviewed pull requests, and a release
is a tag on a reviewed `main` commit. There are no snapshot commits and no second repository. The
history of the earlier private repository was never published, and it is never fetched or pushed here.

1. Make the tree publishable first. Game-derived and third-party material stays out unless its
   redistribution is cleared (FX donors and compiled packs, Valve fonts, sounds and Panorama art,
   other people's songs), along with leaked or decompiled third-party code and anything with a
   personal path, address or secret. `node Automation/verify/check-repo-policy.mjs --scan` must
   report no findings. The maintainer keeps a private readiness checklist beside this doc.
2. Merge the change into `main` through a pull request with a green `ci-gate`. Commits use the
   noreply author `Wockstajit <164069541+Wockstajit@users.noreply.github.com>`, never a personal
   address, and carry no co-author or tool trailers.
3. Create the release tag `v<version>` on the merged `main` commit and push only that tag. Never push
   `--all`, `--mirror` or `--tags`, never move a published tag, and never force-push `main`.
4. Git LFS objects (the osu! skin, the README trailer) upload with the push. Confirm on GitHub that
   an LFS file shows its real size, not a pointer.

GitHub Pages must use **GitHub Actions** as its source (Settings > Pages); `pages.yml` deploys
`Website/` on pushes to `main`. `archive.yml` is inherited from advancedfx and runs only in that
repository.

Build installers on the maintainer's machine, not in CI. The local-only material (the FX pack,
ragdoll library, osu! songs, the UwU song, the launcher's Stratum2 fonts and CS2 sound) exists
only in that working tree. A CI checkout would ship without it.

## Channels, branches and tags

Use [the finish workflow](finishing-work.md) for source changes, labelled tracking issues, commits
and PRs. The branch and channel rules are in [the GitHub workflow policy](git-workflow.md). In short:

- `main` is development. A merge to `main` does not publish anything.
- A stable line lives on `release/<major.minor>` (for example `release/0.1`). Release-specific fixes
  go to that branch through a PR, then get ported to `main`.
- A preview is an opt-in prerelease with a unique, immutable version such as `v0.2.0-preview.1`.
- Runtime hotfixes reuse the current release and a new serial, as described below.

Pushing a fix branch does not publish a release or close its tracking issues until the change
reaches `main`. Do not create a release or version tag just because an optimization was pushed.

The private `launcher-v*` tag workflow builds launcher artifacts with publishing disabled. Only
create a version tag as part of a requested release, matching the version being packaged. Never
move or reuse an existing published tag. Runtime hotfixes use the existing release and a new
serial, as described below. They do not get a new full-release tag.

## Full release or runtime hotfix

Publish a **full release** (new version, complete installer) only when the launcher itself changed:
anything under `SourceMovieMakerLauncher/` that ships in the app, the FX pack, or the bundled tools.
Users then download the new installer through the app updater.

Publish a **runtime hotfix** when only the CS2 hook changed. It adds a few megabytes to the release
users already have, instead of a new installer:

1. Build and install the hook (`cmake --build ... --target AfxHookSource2` and both install steps).
2. From `SourceMovieMakerLauncher`, run
   `powershell -File scripts\build-runtime-hotfix.ps1 -Serial <n>`, where `<n>` is 1 for the
   first hotfix of that version and one higher for each later one. Pass
   `-Files @('x64\AfxHookSource2.dll','x64\environment-shaders')` when environment shader bytecode
   changed with the DLL. Invoke the script directly from PowerShell when passing an array, because
   `powershell.exe -File` does not bind comma-separated values to that array parameter. For a few
   added shaders, pass their individual `x64\environment-shaders\<name>.cso` paths with the DLL.
   The build's `environment-shaders/manifest.txt` lists its required programs. Compare those files
   against the current release bundle. Do not package unrelated bytecode left in a local shader cache.
3. Upload both outputs to the **existing** release of the current version. The script prints the
   `gh release upload <tag> --clobber` command. Do not create a new release, and do not upload a
   `latest.yml`. Either one would make the updater offer an installer.
4. Add a short line about the fix to that release's notes.

Both packaging scripts archive the shipped DLL's PDB before stripping it, in `Automation/symbols/`
or `SOURCEMVM_SYMBOL_STORE`. Back up that folder after every release, so crash reports from that
build can be resolved to function names. See [the community reports doc](../features/filmmaker_community.md).

On its next start with automatic updates on (or when the user presses **UPDATES**), a launcher of
that version downloads `runtime-hotfix.json`. It installs a newer serial over its runtime, backing
up each replaced file and restoring it on failure. The hotfix stays in place across restarts until
a newer full release replaces the bundle. A failed hotfix check never blocks launching. Launchers
before 0.1.9 ignore hotfixes and need that full release first.

## Build and scan

1. Increase the launcher version in `SourceMovieMakerLauncher/package.json` and its lockfile. Use
   a new version for each published build so installed launchers can receive the update.
2. Build the complete installer from `SourceMovieMakerLauncher` with `npm run package:bundled`.
   Build and stage the CS2 hook first only when native code changed. Keep Deadlock files out of
   the complete CS2 package.
3. Open VirusTotal in the coding browser and upload
   `SourceMovieMakerLauncher/release/win-unpacked/SourceMovieMaker.exe` directly. Use the file
   picker, or drag the file into the upload area when the browser supports it. Do not rename it,
   make a scan copy, or create a components ZIP. If browser upload is unavailable, give the owner
   that exact file path to upload manually.
4. Record the file's SHA-256 and the resulting report URL. Check that the report hash matches the
   executable. The report covers that executable only. It does not scan the whole installer or
   every file in `resources`. If that executable has an unchanged hash, its existing report still
   identifies the same file.

## Publish

Create a draft release for the new version, then upload only these files:

- `SourceMovieMaker-Complete-Setup-<version>.exe`
- `SourceMovieMaker-Complete-Setup-<version>.exe.blockmap`
- `latest.yml`
- `AfxHookSource2.dll`, the same hook the installer bundles (`build/source2-release/bin/x64/`),
  for people who inject it themselves. The launcher ignores it.
- `SHA256SUMS.txt`, regenerated for exactly these files. The packaging step can leave a stale one
  in `release\` from an older version; never upload that.

Never upload the portable zip (`SourceMovieMaker-Portable-<version>.zip`, from
`npm run package:portable-zip`) to a public release. It contains DMCA-restricted files, and the zip
lays them out as plain files on the release page. Build it only for local or private use.

Keep internal test logs, verification JSON, scan copies and components archives out of public
release assets. Delete failed uploads from the release form instead of leaving broken entries.
Verify the uploaded asset names, sizes and hashes before publishing. This is a packaging check,
not an in-game test.

Write brief release notes for users: what changed, what they need, and how to install. Put
technical build evidence in private documentation. Link the VirusTotal report without claiming
that it guarantees safety or covers files it did not scan.

Update the website's fallback download, checksum and direct VirusTotal links in
`Website/index.html`. The direct report must match the executable from that fallback installer.
In `Website/virustotal.json`, use the scanned executable's filename, hash and report URL, and the
new installer's SHA-256. Merge those website changes into `main` through a PR. The Pages workflow
redeploys `Website/` on each push to `main`. Confirm that the download link and updater feed point to the new release.
Remove an older release only when the owner requests it and its replacement is published successfully.

Windows 10/11 (64-bit), Steam and CS2 are required to run the app. The complete installer carries
the launcher, runtime dependencies and video encoder. Users do not need Visual Studio, Python,
Node.js or a separate FFmpeg installation.
