# Portable checkout setup

A Git checkout contains source and a pinned [package manifest](packages.json). It does not contain
Steam game files, compiled build directories or the compiled FX pack (other games' material, see
[FX/README.md](../../FX/README.md)). Root [live.bat](../../live.bat) and [demo.bat](../../demo.bat)
bootstrap the prebuilt runtime through [ensure-packages.ps1](ensure-packages.ps1) when there is no
local build. They mount a writable copy of a local `FX/compiled` as the FX pack when one is present.
Normal startup never requests DLSS. Its package needs an explicit manual `-Dlss` setup invocation.

The runtime package includes the native injection host, hook, injector, native dependencies,
Panorama resources and demo helpers. The optional DLSS package contains ReShade, RenoDX and the
signed NVIDIA neural runtime. Files land under `build/portable-runtime` and `build/dlss5-downloads`.
A complete local developer build takes priority. Nothing installs into the original Steam binary
directory.

Download URLs and SHA-256 hashes are pinned in Git. Private release downloads reuse Git's credential
helper (or `GH_TOKEN` / `GITHUB_TOKEN`). Tokens are never written to the manifest or logs. If
authentication fails, sign in to the same GitHub account that can clone this repository. An already
installed, verified package can be used without a network connection. Corrupt downloads and archive
traversal are rejected. Never commit the download cache, a private CS2 tree or generated
machine-specific INIs.

## Updating packages

Build and install both CMake runtime components, and build the native host using
`SourceMovieMakerLauncher/scripts/build-host.ps1`. Confirm the hook was built from the intended
source revision and validate it before distribution. Then run:

```powershell
powershell.exe -NoProfile -File Automation/setup/package-portable.ps1 -Version portable-YYYYMMDD-N
```

Upload both ZIPs and `packages.json` from the printed output directory as assets of a release with
that exact tag in the source repository. The release may be a prerelease. Keep package versions
immutable. Copy the generated manifest to `Automation/setup/packages.json` only once the assets
are available. Test the download, then commit the manifest with the launcher changes.

Updating C++ source alone does not update the prebuilt package. Publish a new package when you
deliver hook changes to users who do not compile it. FX is not packaged: it travels with the
repository.

The manifest records the source revision and file hashes. ReShade, RenoDX and NVIDIA components use
the versions described in the [DLSS setup](../dlss/README.md). Distribution is experimental.
Packaging does not resolve the known DLSS resize hang or establish successful neural evaluation on
another GPU.

## Verification

`Automation/tests/portable-setup-runtime-tests.ps1` installs locally prepared archives into a fresh
folder with spaces. It checks repeat installation, hash rejection and archive-path confinement. The
test expects the package archives under `build/packages/<manifest version>`.

A real fresh-checkout launch must also verify the process, netcon and hook health. An extraction
test alone does not prove the game runs. No friend's hardware was used for the local test.
