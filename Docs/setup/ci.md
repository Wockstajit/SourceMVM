# Continuous integration

[`.github/workflows/ci.yml`](../../.github/workflows/ci.yml) runs on pull requests into `main` and
`release/*`, on pushes to those branches, on merge queue groups, nightly at 03:41 UTC, and on
manual runs from the Actions tab. Branch protection requires only `ci-gate`. The branch and pull
request policy is in [GitHub workflow and contribution policy](git-workflow.md). A green run does
not prove that an in-game feature works.

## Which jobs a change runs

The `changes` job decides the scope with
[`Automation/verify/ci-change-scope.mjs`](../../Automation/verify/ci-change-scope.mjs), after running
its tests ([`ci-change-scope-tests.mjs`](../../Automation/tests/ci-change-scope-tests.mjs)). The script
is the source of truth for the path rules. In short:

| Event | Mode | Jobs |
| --- | --- | --- |
| Pull request into `main` | fast | The jobs whose inputs the pull request changed (`merge-base...head`) |
| Push to `main` | fast | The jobs whose inputs the push changed (`before..after`) |
| Merge queue group | fast | Every job |
| Nightly, manual, pull request into or push to `release/*` | full | Every job, with the shipping native build |

| Changed paths | Native scope | Other jobs |
| --- | --- | --- |
| `AfxHookSource2/Deadlock/`, `AfxHookSource2/Filmmaker/Deadlock/`, `Automation/tests/deadlock-*.cpp` only | `deadlock` | |
| Any other hook, shared, dependency, shader, Rust, Go helper, CMake or native test input | `all` | |
| `AfxHookSource2/Filmmaker/Panorama/`, the cosmetics catalog generator | `all` | `panorama-tests` |
| `SourceMovieMakerLauncher/`, `Deps/release/injector`, `Automation/tests/launcher-*` | | `launcher-build` |
| `.github/workflows/ci.yml`, `ci-change-scope.mjs` | `all` | every job |
| Anything else (docs, website, FX tools, scripts, legacy HLAE trees) | `none` | |

`docs-integrity` and `security-license-scan` scan every tracked file, so every run includes them.
`changed-file-policy` runs on pull requests. The legacy HLAE build runs nightly in
[`legacy-native.yml`](../../.github/workflows/legacy-native.yml). When the changed files cannot be
listed (a new branch, a forced update, an unknown event), every job runs.

The `deadlock` scope builds `AfxHookSource2/Deadlock/all` and runs only the tests in that folder. It
is safe because no CS2 build file or non-Deadlock source refers to the Deadlock-only folders, which
`ci-change-scope-tests.mjs` checks. Any other native change builds and tests both games, because
`AfxHookDeadlock` compiles many CS2 sources itself.

## Native builds

**`native-build-windows`** (fast mode) builds the `x64-deadlock-ninja-release` preset with Ninja
from the Visual Studio x64 developer environment, at `--parallel 4` on the runner's 4 vCPUs. Ninja
lets [sccache](https://github.com/mozilla/sccache) (pinned version and SHA-256 in the workflow)
wrap `cl.exe`. The Visual Studio generator ignores compiler launchers, so the shipping presets
cannot use it. Three caches persist between runs:

- **Environment shader bundle.** `EnvironmentShaderBuilder` compiles 67 D3D programs and, on an
  empty cache, is the slowest step of the build. Each bundle entry stores its full compile key
  (source, macros, entry point, profile, flags, compiler DLL identity) and a bytecode checksum. The
  builder reuses an entry only on an exact key match with a valid checksum and recompiles the rest.
  The cache holds only the programs the manifest lists, keyed on the manifest's hash.
- **sccache.** Keyed by sccache on the preprocessed source, the compiler binary and its flags.
  Saved when the run wrote new entries, capped at 1 GB (a full build stores about 115 MB).
- **Cargo.** The registry and the Rust target folder, keyed on the Cargo manifests and lockfile.

GitHub scopes caches by branch. A pull request reads its own caches and those of `main`, and its
writes are visible only to later runs of the same pull request. Pushes to `main` refresh the shared
caches. Release lines and nightly runs use no compile cache.

**`native-full-validation`** (full mode) is the shipping configuration: the `x64-deadlock-release`
preset with the Visual Studio generator, `/MP`, MSBuild `--parallel 3`, and no compiler or shader
cache. It catches anything the fast path could hide, such as a Visual Studio-only project problem.

Both jobs build both hooks and run the same CTest suite. The four D3D11 render fixtures
(`EnvironmentAtmosphereEdgeTests`, `EnvironmentRenderTests`, `RelightProbeGpuTests`,
`EnvironmentVolumeCacheTests`) need a GPU and stay local checks.

### Debug information

Release hooks always carry full debug information and link a PDB with `/DEBUG`. The shipping
presets compile with `/Zi`, which shares one compiler PDB per target and cannot be cached. The
Ninja preset sets `AFX_MSVC_RELEASE_DEBUG_INFO=/Z7`, which stores the same information in each
object, and the linker writes the complete PDB from it. Loaded through dbghelp, `/Zi` and `/Z7`
builds of the same commit both match their DLLs and report line numbers, global symbols and type
information, with the same source files (2102 for `AfxHookSource2`) and the same function count
within five of about 21,900. `/Z7` keeps about 1.6% fewer type records, because the linker merges
only referenced types.

### Precompiled headers

Not used. Seventy-eight hook sources define `NOMINMAX` or `WIN32_LEAN_AND_MEAN` before their
includes and others rely on the full `windows.h`, so one forced-include header would change macro
behavior for one group or the other. sccache also cannot cache compiles that use a precompiled
header (`/Yu`).

## Concurrency and `ci-gate`

A newer push to the same pull request or branch cancels the run it replaces. Merge queue groups,
nightly runs and manual runs each have their own group and are never cancelled.

`ci-gate` runs even when jobs fail or the run is cancelled, so it never reports a skipped status,
which branch protection would treat as passing. It fails when any job failed or was cancelled, and
when a job the change needed (per the `changes` outputs) did not succeed.

## Running the CI build locally

From an x64 Visual Studio developer prompt:

```batch
cmake --preset x64-deadlock-ninja-release
cmake --build --preset x64-deadlock-ninja-release
ctest --test-dir build/x64-deadlock-ninja-release --output-on-failure
node --test Automation/tests/ci-change-scope-tests.mjs
```

Set `CMAKE_C_COMPILER_LAUNCHER` and `CMAKE_CXX_COMPILER_LAUNCHER` to `sccache` before the first
configure to use a local compiler cache.

## Measured effect

Measured in October 2026 on GitHub-hosted `windows-2022` runners (4 vCPUs, 16 GB) while this
workflow was introduced. Hosted runners mix CPU generations, so the table names the CPU, and only
rows on the same CPU compare directly.

**Before.** Across the 18 successful runs with a native build in the previous configuration, the
run took a median of 28.6 minutes (21.8 to 32.6). The shader bundle took 417 to 908 s of each
build and reused nothing.

**Native build step**, environment shader bundle and Rust crate already cached:

| Configuration | C++ objects | Build step | CPU |
| --- | --- | --- | --- |
| Previous: Visual Studio, `/MP`, MSBuild `--parallel 3` | rebuilt | 654 s | EPYC 7763 |
| Ninja `--parallel 4` | rebuilt | 640 s | EPYC 7763 |
| Ninja `--parallel 6` | rebuilt | 674 s, 634 s | EPYC 7763 |
| Ninja `--parallel 8` | rebuilt | 649 s, 641 s | EPYC 7763 |
| Ninja `--parallel 4` with sccache, empty cache | rebuilt and stored | 791 s | EPYC 7763 |
| Same, warm cache, unchanged sources | 813 of 814 from cache | 205 s, 218 s | EPYC 7763 |
| Same, one `.cpp` edited | 811 of 814 from cache | 220 s | EPYC 7763 |
| Same, `Filmmaker.h` edited (32 objects) | 782 of 814 from cache | 243 s | EPYC 7763 |

The job count does not matter on this runner, so CI keeps 4. An empty sccache adds about a quarter
to the C++ time, paid once per cache scope. A warm build is limited by sccache running the
preprocessor for every object, and by the Rust workspace crate (about 110 s, in parallel), which
cargo rebuilds because a fresh checkout has new file times. `main.cpp` embeds `__DATE__` and
`__TIME__` in the console banner, so it always recompiles.

**Whole run**, pull request #15:

| Run | Caches | Native build step | Native job | Run wall time |
| --- | --- | --- | --- | --- |
| First push | none | 1475 s | 1708 s | 28.8 min |
