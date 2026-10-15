# Review UI changes in the mockup first

Both mockups run the same Panorama JavaScript as the game. Make UI fixes in that shared code:
review the layout in the browser, then build the same changes into the hook and verify them in
game. Nothing needs translating from a separate design after review.

| Edit | Browser preview | Installed game |
| --- | --- | --- |
| A shared `*Js.h` UI script | Refreshes automatically with the preview launcher running | Changes after rebuilding and relaunching |
| An extracted `generated/*.js` draft | Refreshes on save | Apply it back to the headers, then rebuild and relaunch |
| Mockup HTML, `shim/` CSS, or temporary DevTools styles | Browser only | Must be implemented in the shared Panorama script |
| A setting clicked in the mockup | Changes simulated state | Does not control the running game |

CS2 and Deadlock have separate UI implementations. A feature that exists in both games needs
editing and checking in each one. Changing the CS2 script does not change Deadlock.

## Open a preview

Double-click the [CS2 mockup](../../UiMockup/cs2-mockup.bat) or the
[Deadlock mockup](../../UiMockupDeadlock/deadlock-mockup.bat). Node.js is required. Python and a
separate web-server package are not. The launchers serve the files locally and watch the source
while open.

From the repository root:

```text
node Automation/ui/preview.mjs cs2 --open
node Automation/ui/preview.mjs deadlock --open
```

CS2 uses port 8823; Deadlock uses 8824. `--port N` overrides the port.

## Make the change once

Edit `AfxHookSource2/Filmmaker/Panorama/*Js.h` for CS2 or `AfxHookSource2/Filmmaker/Deadlock/*Js.h`
for Deadlock. The preview watcher extracts the new scripts on save. Check the affected view,
including open menus, long values, and the target resolution.

An extracted draft also works. Keep the `@mvm:seg` and fragment markers intact, then apply it:

```text
node UiMockup/apply-ui.mjs cameraeditor --dry-run
node UiMockup/apply-ui.mjs cameraeditor
node UiMockupDeadlock/apply-deadlock-ui.mjs --dry-run
node UiMockupDeadlock/apply-deadlock-ui.mjs
```

Omit `cameraeditor` to apply all CS2 surfaces. A `render` view belongs to `cameraeditor`.

The appliers check the full batch before writing any header. A saved extraction baseline catches
source edits even when the number of literals stays the same. If a draft and its header were both
edited, reconcile the intended changes first. The tools refuse to overwrite the newer source.

Refreshing or reopening also refuses to overwrite a pending draft. On the first run with older
generated files that have no baseline, differing files are preserved in a `generated/backup-before-sync-*`
folder. `generated/` and these backups stay local as ignored build products. Save DevTools edits
as actual Panorama source changes. A DOM change alone is temporary.

## Confirm it in game

Build and install the affected hook using [the build instructions](../../CLAUDE.md#build), then
relaunch that game. Applying or refreshing a mockup does not replace the DLL already loaded by a
running game. `livejs` can run diagnostic or temporary scripts, and Deadlock's `ui rebuild`
reconstructs the compiled workspace. Neither installs a newly edited header.

Use the same workspace, tab, resolution and state for the browser and game captures. Verify with
netcon, `mvm_debug` for runtime behavior, and screenshots. The browser uses an emulation layer,
so fonts, layout measurements, native game panels and the 3D viewport still need a check in game.
Do not call a UI fix complete based on the mockup alone.

Browser capture and geometry tools are documented in the [CS2 mockup guide](../../UiMockup/README.md)
and [Deadlock mockup guide](../../UiMockupDeadlock/README.md). The workflow regression suite is:

```text
node --test Automation/tests/ui-source-sync-tests.mjs Automation/tests/ui-mockup-roundtrip-tests.mjs Automation/tests/deadlock-mockup-tests.mjs
```
