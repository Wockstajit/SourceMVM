# SOURCE:MVM documentation

This index separates maintained feature contracts from research and inherited notes. Code shows
current behavior. The user's request and the maintained contracts show intended behavior. When
they disagree, follow the intent, then update the implementation and the documentation.

## Start here

| Area | Maintained document |
| --- | --- |
| Cloning, building and launching on a new machine | [`collaborator-setup.md`](setup/collaborator-setup.md) |
| First local launch | [`local-quickstart.md`](features/local-quickstart.md) |
| Build and repository layout | [`../BUILDING.md`](../BUILDING.md), [`../CONTENTS.md`](../CONTENTS.md), [`../CLAUDE.md`](../CLAUDE.md) |
| Demo browser | [`filmmaker_demos_feature.md`](features/filmmaker_demos_feature.md) |
| Play page + workshop map sorting | [`filmmaker_play_menu.md`](features/filmmaker_play_menu.md) |
| HC Tracker link + Report Bugs (main menu, launcher) | [`filmmaker_community.md`](features/filmmaker_community.md) |
| Movie director and cameras | [`filmmaker_movie_controls.md`](features/filmmaker_movie_controls.md), [`camera-editor-feature.md`](features/camera-editor-feature.md) |
| Recording FPS (mux + host_framerate; dial preview-only) | [`filmmaker_render_fps.md`](features/filmmaker_render_fps.md) |
| Render passes (Beauty / Depth / Normal × visibility, synchronized multipass) | [`filmmaker_render_passes.md`](features/filmmaker_render_passes.md) |
| Live demo scrubbing (the world follows timeline and demo-bar drags) | [`filmmaker_demo_scrubbing.md`](features/filmmaker_demo_scrubbing.md) |
| Follow / Attach camera | [`follow-camera/README.md`](follow-camera/README.md) |
| Config and camera effects | [`filmmaker_config_panel.md`](features/filmmaker_config_panel.md), [`filmmaker_camera_shake.md`](features/filmmaker_camera_shake.md) |
| Death animations (clip-exact, then ragdoll) | [`filmmaker_death_animations.md`](features/filmmaker_death_animations.md) |
| Nervous System (post-death reactions: reach for the wound, breathe, writhe, twitch) | [`filmmaker_nervous_system.md`](features/filmmaker_nervous_system.md) |
| Lens: depth of field, bokeh iris, lens flare | [`filmmaker_lens.md`](features/filmmaker_lens.md) |
| Cosmetics / Customize | [`cosmetics-overview.md`](cosmetics/cosmetics-overview.md), [`customize-feature.md`](features/customize-feature.md) |
| Panorama architecture | [`panorama_ui_guide.md`](panorama/panorama_ui_guide.md), [`panorama_viewmodel_preview.md`](panorama/panorama_viewmodel_preview.md), [`panorama-player-preview-summary.md`](panorama/panorama-player-preview-summary.md) |
| Particle FX | [`filmmaker_effects_modifiers.md`](features/filmmaker_effects_modifiers.md), [`../FX/README.md`](../FX/README.md) |
| Environment, volumetrics, surface tests and props | [`environment-scene-editor.md`](features/environment-scene-editor.md) |
| Runtime map lighting, smoke, VFX and shadows | [`whole-map-relighting.md`](effects/whole-map-relighting.md) |
| Interior lighting camera validation, map sweep and authored-light audit | [`interior-lighting-testing.md`](effects/interior-lighting-testing.md) |
| Map lights, collision and shadow casters (native extraction on map load) | [`map-data-extraction.md`](effects/map-data-extraction.md) |
| Deadlock port | [`deadlock/README.md`](deadlock/README.md) |

## Document classes

Every document lives in exactly one of these folders. Nothing but this index sits loose in `Docs/`.

**Maintained feature contracts**

- `Docs/setup/`: getting a working checkout on a new machine (Git LFS, submodules, build, launch).
- `Docs/features/`: the filmmaker features, including the demo browser, play menu, movie controls,
  camera editor, config panel, camera shake, effects modifiers, window resize, customize, and
  [Method Acting](features/filmmaker_method_acting.md).
- `Docs/cosmetics/`: skin, model and glove behavior, and the legacy weapon-skin index.
- `Docs/panorama/`: Panorama UI architecture and the in-game preview scenes.
- `Docs/follow-camera/`: the Follow/Attach camera subsystem.
- `Docs/effects/`: ragdolls, map lighting and its map data, and other effect subsystems.
- `Docs/deadlock/`: the Deadlock port, with resolved signatures and offsets, the UI style contract,
  and how the Panorama reference is extracted.

**Rules and operations**

- [Repository guidance](../CLAUDE.md) and [build instructions](../BUILDING.md) own setup and runtime diagnosis.
- [How we finish work](setup/finishing-work.md) covers tracking issues, labels, commits, branch pushes and PRs.
- [GitHub workflow and contribution policy](setup/git-workflow.md) covers branches, PRs, AI-assisted contributions, required checks, releases and issues.
- [Publishing releases](setup/publishing-releases.md) covers builds, the direct executable scan and public assets.
- [Automation hygiene](../Automation/docs/AUTOMATION_HYGIENE.md) covers generated output and retained evidence.
- [Relighting regression fixtures](../Automation/fixtures/relight-regressions/README.md) describe saved visual checks.

**Reference, not contracts**

- `Docs/porting/`: notes on external mods and assets ported in (MW2019 FX mapping, Povarehok,
  ViewDynamics) plus third-party related tools. Read alongside `../FX/README.md`, which owns the
  current build pipeline.
- `Docs/archive/`: research kept as evidence. It is not current implementation guidance.
Chat transcripts, private machine paths, generated output and session handoff files do not belong
in the documentation tree. Fold durable conclusions into the matching feature document.

## Validation

Run:

```batch
node --test Automation/tests/documentation-integrity-tests.mjs
```

The check scans every tracked Markdown file, validates repository-relative links, rejects private
machine and agent-memory references in maintained docs, and prevents generated build output from
being tracked in project-owned paths.
