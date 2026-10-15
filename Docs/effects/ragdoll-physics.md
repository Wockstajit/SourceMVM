# Ragdoll Physics

Gravity, friction, body count and collision for death ragdolls.

- Module: `AfxHookSource2/Filmmaker/Movie/RagdollPhysics.{cpp,h}`
- UI: CONFIG → **RAGDOLLS** → PHYSICS card
- Command: `mirv_filmmaker ragdoll physics ...`
- Status: **DONE**, user-tested in-game

---

## 1. Convars, not schema writes

Every control is a CS2 convar. Nothing here writes entity memory, which makes this the second most
update-resilient feature in the World/Effects set, after night mode. Only a convar **rename** can
break it, and it breaks loudly.

| Key | Convar | Default | Range | UI label |
|---|---|---|---|---|
| `gravity` | `ragdoll_gravity_scale` | 1.0 | −2 … 4 | Gravity |
| `friction` | `ragdoll_friction_scale` | 0.6 | 0 … 4 | Friction |
| `limit` | `cl_ragdoll_limit` | 20 | −1 … 64 | Max bodies |
| `collide` | `cl_player_ragdolls_collide` | 0 | 0 / 1 | Bodies collide |
| `minage` | `ragdoll_lru_min_age` | 10 | 0 … 300 | Keep for |

Negative gravity is allowed on purpose. This is a filmmaking tool, and floating bodies are a shot
people ask for. `cl_ragdoll_limit -1` means unlimited.

Extras: `mirv_filmmaker ragdoll physics reset` restores every default, and `cleanup` issues
`ragdoll_cleanup_all` to clear bodies already on the ground.

## 2. Why the reference thread's approach was not used

The reference thread **hooks the convar getter** so the game reads a
different value than the one stored, specifically to avoid VAC noticing a changed convar.

This tool is for offline demo and movie work only, so the feature just sets the convar. The AOB scan
that thread needs adds cost and gives no benefit here. An AOB is exactly the kind of pattern that
breaks on every CS2 update. Skipping it is why this feature needs no update triage.

## 3. Known gaps

- **The RAGDOLLS tab will not switch by console command** (Panorama tab hydration). Clicking it
  works. Open bug, deferred.
- `m_flGravityScale` per-entity is resolved by `WorldSchema` and reported by `world health`, but is
  **not** currently written. The convar covers every case tested so far. The field is kept
  resolved so a future per-ragdoll override does not need new plumbing.
- Not verified inside a render job, only in live playback.

## Related

- [`README.md`](README.md): master status
- [`update-resilience.md`](update-resilience.md): CS2 update recovery
