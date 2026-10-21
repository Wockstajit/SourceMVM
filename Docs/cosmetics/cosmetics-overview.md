# Cosmetics system: overview

The offline demo cosmetics override family applies per-player weapon skins, knife type and skin,
gloves, and agent (player model) overrides to a CS2 demo replay. Drive it from the Camera Editor's
Customize modal or the `mirv_filmmaker cosmetics ...` console commands.

Summary and file map below. Deep dives:

| Topic | Doc |
|---|---|
| Working recipes, AOBs, engine mechanisms (the "how") | [cosmetics-cs2-methodology-notes.md](cosmetics-cs2-methodology-notes.md) |
| Customize modal UI and wear controls | [customize-feature.md](../features/customize-feature.md) |
| Why only some weapon skins render (legacy vs CS2 mesh) | [cosmetics-legacy-vs-cs2-models.md](cosmetics-legacy-vs-cs2-models.md) |
## What it does

- Weapon skins: rewrites the econ item view (fallback paint kit, wear, seed, StatTrak,
  `m_iItemIDHigh = -1`) on every weapon owned by a profiled player. It then fires the engine's
  own composite-material rebuild so the skin renders. Confirmed rendering live.
- Knife: paint goes through the same composite path. The knife type swap uses a
  client-side `SetModel` sequence on the viewmodel and world model. A targeted anim-builder
  detour guards it against crashes.
- Gloves: writes the pawn's embedded `m_EconGloves`, sets the native glove-reapply flags
  and body group, and lets rendered frames rebuild the engine-owned arms viewmodel. Do not
  replace the HUD-arms model with a glove-only model. That removes the player's hands.
- Agent: full player-model swap via `SetModel`.

Profiles are keyed by SteamID64, not entity index. An override therefore follows the player
across pawn recreation, round restarts, death, observer switches, and demo seeks.

Picks are saved per demo, in the `cosmetics` section of that demo's `project.mvm`, about half a
second after the last change. Opening the demo again, even after a restart, loads them. Another
demo starts from its own saved picks, so one recording's players never get another's skins. Bot
agent picks stay runtime-only. Startup removes the old global `%APPDATA%\HLAE\cosmetic_profiles.json`.
**Offline demo playback only.** Never use it against live servers.

Weapon restoration journals paint, seed, wear and StatTrak count in both local and networked
attribute lists. StatTrak count is attribute 80 (`kill eater`); attribute 81 is its score type
and must remain untouched. Clear/Reset restores the recorded raw bits or removes an attribute
that the override added, without disturbing unrelated attributes.

## Architecture (three layers)

1. Profile store and apply loop: `CosmeticOverrideSystem` walks the client entity list
   once per main-thread frame, matches econ entities to profiles by original-owner XUID,
   and applies the data writes and composite refresh.
2. Model swaps: the `CosmeticModelSwap` subsystem wraps the client.dll functions
   (`SetModel`, `SetMeshGroupMask`, …) resolved by byte-pattern scan. Every call is
   SEH-guarded, so a moved pattern degrades to a no-op instead of crashing.
3. UI: the Camera Editor's Customize modal (Panorama, see
   `Panorama/CameraEditorCustomizeJs.h`) issues the same `mirv_filmmaker cosmetics ...`
   commands as the console.

## File map (`AfxHookSource2/Filmmaker/Cosmetics/`)

One responsibility per file. Public headers are the `*.h` without "Internal" in the name.

### Core apply system
| File | Responsibility |
|---|---|
| `CosmeticOverrideSystem.{h,cpp}` | The per-frame apply loop: entity walk, XUID-to-profile match, fallback and attribute writes, composite refresh gating. The public API everything else drives. |
| `CosmeticProfile.{h,cpp}` | The SteamID64-keyed profile store (one player's weapon/knife/glove/agent choices). |
| `CosmeticDemoSync.cpp` | Demo-seek detection, the post-apply "tick nudge" (briefly resume and re-pause so a change made while paused re-renders), the seek settle window and the seek hold (pause a seek that lands while playing until the skins are redrawn). |
| `CosmeticDemoProfiles.cpp` | Saves and loads the per-demo picks: the `cosmetics` section of the demo's `project.mvm`, keyed by the original demo through `PlayingDemoPath()`. |
| `CosmeticPaintKitBridge.cpp` | The `cl_paintkit_override` cvar read and write bridge. It is global and deploy-time, and it is the experimental alternative path. |
| `CosmeticDirectComposite.{h,cpp}` | The resolved Andromeda composite-rebuild calls (`composite once`). This is the essential render lever. |

### Model-swap subsystem (shared internal surface: `CosmeticModelSwapInternal.h`)
| File | Responsibility |
|---|---|
| `CosmeticModelSwap.{h,cpp}` | Public API + core: HUD-arms/viewmodel resolution and mirroring, weapon mesh-mask (legacy vs CS2 model), agent swap. |
| `CosmeticFnResolver.cpp` | client.dll byte-pattern resolution into the shared `Fns` table + SEH-guarded wrappers + econ-schema lookups. |
| `CosmeticKnifeSwap.cpp` | Knife-type swap: def-to-world-model fallback table and the full precache, SetModel, mesh-mask, viewmodel-mirror sequence. |
| `CosmeticGloveSwap.cpp` | Glove econ writes, plus the body-group and gloves-changed rebuild. |
| `CosmeticAnimFix.{h,cpp}` | The knife-swap crash fix. A client.dll anim-builder detour substitutes an empty sequence list for a null out-param. |

### Catalog / classification data
| File | Responsibility |
|---|---|
| `CosmeticCatalog.{h,cpp}` | Item-definition classifier (which slot a def index belongs to) for the native backend. |
| `CosmeticGloveSkinTable.cpp` | Generated glove paint-to-def table (`Misc/gen_glove_skin_table.py`). Do not hand-edit. |
| `CosmeticGloveLabels.{h,cpp}` | Glove display-name tables. |
| `../Panorama/CameraEditorCosmeticsCatalog.inc` | The generated user-facing skin catalog compiled into the UI (not read at runtime by the backend). |
| `../Data/cosmetics.json` | Source data for the generated catalog. |

The legacy-vs-CS2 classification behind the mesh-group selection is tabulated per weapon×skin in
[cs2_weapon_skins_legacy_index.md](cs2_weapon_skins_legacy_index.md) (with `.csv` / `.json`
alongside it), regenerated by `Automation/tools/build_skins_legacy_index.py`.

### Temporary theme firearm restoration

`CosmeticThemeRestore.cpp` journals each firearm before the UwU theme changes it. The journal
binds the original item identity, fallback fields, owned skin attributes and mesh mask to the
entity pointer, full handle, original owner and item definition. Attribute values keep their
raw float bits, including fractional recorded seeds. Native local-attribute removal must pass
its signature and schema check before the theme can write that firearm.

Cancel restores the saved user profiles through their normal apply path. Firearms without a
saved pick get their original attributes and fields back. Temporary local attributes are removed
through the engine, so destruction, vector relocation and notifications stay native. A deployed
firearm receives one paused composite after the original mesh selection settles. Disabling
`autocomposite` defers that visual work. Disabling `modelswap` defers any required mesh change.
These paths resume when permitted and never request pauses while disabled. A holstered firearm
keeps a pending visual restore until it is drawn. Seeking or leaving the demo discards the old
entity journal. `cosmetics status` reports journal and pending counts, and `cosmetics diag`
prints attribute bits for exact comparisons. Prefix these commands with `mirv_filmmaker`.

The late mesh queue is consumed once per frame. It validates the full handle and pointer after
the engine handler. Cancel, seek, session reset and a closed apply gate discard old holds. This
stops a preceding theme mask from overriding the restored mesh or reaching an unloaded entity.
This firearm path does not cover glove or knife rollback. A seek discard also loses the original
when a short forward seek preserves a themed firearm. That restoration case is still under audit,
along with ordinary Clear/Reset rollback.

### Temporary theme agents and session boundaries

Pawn model journals bind the pointer, full pawn/controller handles, reciprocal bindings, SteamID
and team. The original model survives a seek when the same pawn still renders a model written by
the theme. A valid, different live model becomes the new recorded baseline. Empty, faulted or
truncated reads block agent mutation and leave the existing journal unchanged. Confirmed and
attempted model paths keep ownership through partially completed native setters. Observation
cannot tell identical recorded and themed paths apart, so the recorded baseline wins. The settled
apply pass periodically prunes definite identity replacements without dereferencing stored
pointers.

Cancel restores saved user profiles first. Matching pawn restores run before the empty-profile
gate, after seek reconstruction and model settling. They require setter success, native renderable
refresh and matching model readback before completion. Failed reads or writes retry; dead pawns
remain pending without native model writes, so ragdoll physics is not restarted. Starting a new
theme preserves any pending originals. A saved user-agent pick owns the normal apply path.

Level initialization discards entity journals before the egg's deferred cancellation. A reload
of the same demo keeps its saved user profiles; changing the demo or ending the session discards
the theme and its profile backups before cancellation can resurrect the preceding session.
`cosmetics status` enumerates active theme, saved profile/bot counts and pawn journal/pending
counts. Model diagnostics include full identities, target pointers, resolved model-name offset,
original/live/attempted paths and restore readback in `mvm_debug`.

### Deferred picks, clears and pickup owners

Queued firearm and knife picks normally commit on a paused frame, in command order. If the pause
acknowledgement exceeds the existing 120-frame limit, the fallback commits and schedules a nudge.
Later picks join an existing batch, even if its pause has already taken effect. `clear`,
`clearPlayer` and `clearWeapon` remove matching queued picks before touching the stored profile;
clearing a missing profile still cancels its pending pick. Knife definitions share one knife slot,
so clearing a knife alias cancels any queued knife type for that owner. Demo session resets discard
the queue. These commands use the `mirv_filmmaker cosmetics` prefix.

The live-pick pause retains its completion path when every pick is canceled. Edits arriving during
the same pause restart material settling and share that pause; an empty profile store consumes its
mesh-completion marker instead of waiting for the safety timeout. Already owed composites ride the
live-pick pause. `cosmetics status` enumerates queued picks, nudge phase/pending state and both owed
work sets for backend verification.

`player current weapon <def>` and `clearWeapon current <def>` resolve firearms to the held weapon's
original econ owner. Recognized knife definitions, including weapon-command aliases, resolve to the
spectator's knife slot. Explicit numeric Steam IDs retain their meaning. Customize's Reset to
Default clears the selected player's profile and the routed primary/secondary pickup definitions;
it preserves the original owners' other weapon, knife, glove and agent overrides.
This removes profile entries. Immediate restoration of an already patched live item is still under
audit. The existing clear path can leave its paint and Customize selection in place until the demo
reloads.

### Command / UI / diagnostics
| File | Responsibility |
|---|---|
| `CosmeticCommands.cpp` | The `mirv_filmmaker cosmetics <subcommand>` console grammar. |
| `CosmeticUiQueries.{h,cpp}` | Customize-modal queries: live weapon/glove/agent econ reads for the spectated pawn, surfaced to Panorama as JSON. |
| `CosmeticDebug.cpp` | Read-only diagnostics: `cosmetics status`, spectated econ dump and visual diag. Also the shared SEH-guarded entity readers (`CosmeticDebugRead`, declared in `CosmeticDebugInternal.h`). |
| `CosmeticDebugEvents.cpp` | Structured mvm_debug event logging: uiclick, glove-pick, weapon-pick, spectate-switch and `skin.live` snapshots, plus the per-frame change detector. |
| `../Debug/MvmDebugLog.{h,cpp}` | Shared `mvm_debug` transport: file writer, VEH crash breadcrumb and agent ingest for both games and the whole filmmaker surface. |

## Demo-playback caveats

- A demo seek, round change, or weapon redeploy recreates entities from the authoritative
  demo data, so overrides are re-applied per-frame and the composite is re-fired
  change-gated (`autocomposite`, default on).
- A change made while paused does not re-render on its own. `CosmeticDemoSync`'s tick
  nudge handles it.
- The skin composite runs on paused frames only. A seek that lands while the demo plays (the tick
  loop jumping back to In, a camera path restarting) therefore pauses on the landing until every
  deployed weapon's composite has run and drained, then resumes (`cosmetics.seekhold` in
  `mvm_debug`, at most 600 frames). A render's pre-roll landing waits for the same refresh before
  it records, so the first frame of a clip already has its skins. Scrubs are never held.
  `cosmetics status` reports `seek refresh: pending=… hold=…`.
- A hold, and the short pause a pick or a newly drawn gun takes while the demo plays, ends as soon
  as every gun it composited reports its material built (the composite owner's built and pending
  counters, read through `ReadCompositeOwnerCounts`). A material takes about 11–13 frames; the old
  fixed wait was 24, which stays as the cap when the counters cannot be read. A pause owed only to
  a newly drawn gun starts 2 frames after the draw; other changes wait 8 quiet frames so rapid
  picks share one pause.
- Whether a weapon skin renders at all can depend on the legacy vs CS2 mesh group
  matching the paint kit. See [cosmetics-legacy-vs-cs2-models.md](cosmetics-legacy-vs-cs2-models.md).
- An entity spawned while the demo is paused is not drawn at all. The renderer skips entities
  that have never interpolated (`C_BaseEntity::m_bHasSuccessfullyInterpolated`), and interpolation
  needs the clock to advance. This differs from the tick nudge above, which re-renders an entity
  that already exists. The Customize 3D preview uses the engine's own
  `m_bRenderEvenWhenNotSuccessfullyInterpolated` override. See
  [customize-feature.md § Paused preview](../features/customize-feature.md#paused-preview). Expect
  this for any future feature that creates client entities during a pause.
