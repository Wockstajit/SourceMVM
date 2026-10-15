# CS2 offset reference dumps

Mirror of [`sezzyaep/CS2-OFFSETS`](https://github.com/sezzyaep/CS2-OFFSETS): schema class and field
names and their offsets, dumped per module in a dozen output formats (`.hpp`, `.json`, `.cs`,
`.py`, `.rs`, and others).

## Reference only

These dumps contain names and schema offsets only. They do not provide byte patterns, hook
addresses, or vtable slot indices, and nothing in the build reads them. Use them to answer "what
is this field called and where does it sit in the schema", then confirm against the live game.

The [update-resilience guidance](../../Docs/effects/update-resilience.md) warns against
hardcoding anything from here, because a CS2 update moves these numbers.
Verify the affected behavior in the live game, following [repository guidance](../../CLAUDE.md).
Do not treat these dumps as executable contracts.

## Updating the reference payload

`update-cs2-offsets.ps1` syncs the payload from upstream:

```powershell
pwsh Misc\cs2-offsets\update-cs2-offsets.ps1            # update if upstream moved
pwsh Misc\cs2-offsets\update-cs2-offsets.ps1 -Check     # report only, never writes (exit 2 = update available)
pwsh Misc\cs2-offsets\update-cs2-offsets.ps1 -Force     # re-download even if unchanged
```

It compares the upstream `main` commit against `cs2-offsets-meta.json`, downloads only when that
moved, extracts to a staging folder and swaps atomically (restoring the previous copy if the swap
fails), and refuses any payload under 50 files so a truncated download or an HTML error page cannot
replace a good reference folder. Progress goes to `cs2-offsets-update.log`.

Run the check on demand after a game update. A recurring task is machine-specific configuration.
This repository does not guarantee that a daily updater is installed or running. An upstream
payload change is only a reference-data change. It does not prove that the DLL supports the new game.
To verify the live build, use `mirv_filmmaker health`, [repository diagnosis guidance](../../CLAUDE.md),
and [effects update checks](../../Docs/effects/update-resilience.md).

## Files in this folder

This folder holds `update-cs2-offsets.ps1` and this README. The payload (`CS2-OFFSETS-main/`), the version
stamp (`cs2-offsets-meta.json`), the log (`cs2-offsets-update.log`), and any staging folder are generated
locally and excluded from commits by `.gitignore`. The updater writes only to those generated paths.
