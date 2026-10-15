# First launch

Keep Steam open and signed in, then start with one of the root entry points:

- [`launch.bat`](../../launch.bat) opens the SourceMovieMaker GUI.
- [`live.bat`](../../live.bat) starts CS2 directly with NetCon on port 29010.
- [`demo.bat`](../../demo.bat) opens the newest replay. Pass `-Demo "replays/match.dem"` to pick one. It verifies the hook and demo, then writes `session.json`.

No path requires, checks for, or installs DLSS 5. Optional DLSS setup stays manual. Installing it through the GUI is future work.

Direct CS2 startup uses the local hook, `-insecure`, and `+sv_lan 1`. It opens a 1600 x 1200 window on the primary display and records `readiness.json`. Starting it again closes and replaces an existing CS2 session. Use this tool only for offline demos and local practice. Never join online servers while the hook is loaded.

## Basic use

- Open the film-reel button near the top left of the CS2 main menu to browse demos. Add a `.dem` file or a folder of demos, select a replay, and play it.
- For a local scene without a demo, use **Play > Practice** or **Workshop Maps**.
- During demo playback, **Space** pauses or resumes, **Left/Right** skip 5 seconds, and the **mouse wheel** changes camera mode. **F8** toggles the camera help HUD.
- Close CS2 normally when you finish. Use the same root entry point for the next session.

See [demo browser](filmmaker_demos_feature.md) and [camera controls](filmmaker_movie_controls.md) for the full controls.

## Launcher settings and rebuilding

The standalone settings app is `SourceMovieMakerLauncher/release/win-unpacked/SourceMovieMaker.exe`. It uses this checkout's `build/source2-release/bin` runtime.

After you change game-side code, follow the [build instructions](../../BUILDING.md). For launcher-only changes, run `SourceMovieMakerLauncher\build-app.bat -Dir`. The setup also fixes Windows npm invocation and the fresh-build render-material texture dependency. Optional converted particle packs are separate builds. The initial setup uses the stock versions.

Launch logs are under `Automation/runs/launch-cs2/`. For a quick diagnosis, run `mirv_filmmaker health` in the CS2 console. If a future CS2 update breaks the hook, save that output for the project maintainer.
