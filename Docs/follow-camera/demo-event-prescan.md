# Demo event pre-scan (`.fmjson` v10)

The Follow camera's **Events** list (weapon and C4 drops and pickups) comes from a one-time demo
pre-scan. The pre-scan is cached as `scoreboard.fmjson` in the demo's `%APPDATA%\HLAE\source_mvm\demos\cs2\<demo>\`
folder. CS2 demos have no random access to game events, so the tool parses the whole demo once (in the
Go helper) and reads the cached JSON after that.

## Schema version

`schemaVersion` / `kSchemaVersion` = **10**. The writer and reader must agree:

- Go writer: [main.go](../../FilmmakerDemoInfoGo/main.go) (`const schemaVersion = 10`)
- C++ reader: [DemoInfoHelper.cpp](../../AfxHookSource2/Filmmaker/Demo/DemoInfoHelper.cpp) (`constexpr int kSchemaVersion = 10;`)

A cached file with a different `v` is treated as stale and re-parsed
([DemoInfoHelper.cpp:303](../../AfxHookSource2/Filmmaker/Demo/DemoInfoHelper.cpp#L303)). **If you change
the schema, bump both constants together.**

## Event JSON shape

The `.fmjson` carries a top-level array ([main.go:387](../../FilmmakerDemoInfoGo/main.go#L387)):

```json
"events": [
  { "tick": 1234, "type": "weapon_drop",  "item": "",          "accountId": 123456 },
  { "tick": 1250, "type": "bomb_dropped",  "item": "c4",        "accountId": 234567 },
  { "tick": 1300, "type": "bomb_pickup",   "item": "c4",        "accountId": 345678 },
  { "tick": 1400, "type": "item_pickup",   "item": "weapon_ak47","accountId": 123456 }
]
```

| Field | Meaning |
|---|---|
| `tick` | the in-game tick when the event fired. This is the seek target. |
| `type` | `weapon_drop` · `bomb_dropped` · `bomb_pickup` · `bomb_planted` · `item_pickup`. |
| `item` | e.g. `weapon_ak47`, `c4`, or `""`. |
| `accountId` | Steam account id (32-bit) of the player involved; `0` = unknown. |

### Event sources (Go)

Registered with `p.RegisterEventHandler` during the full parse,
[main.go:230-309](../../FilmmakerDemoInfoGo/main.go#L230):

| Recorded type | demoinfocs event | Notes |
|---|---|---|
| `weapon_drop` | `events.Kill` | CS2 has **no** `weapon_drop` game event, so the victim's death tick is the main drop moment. |
| `item_pickup` | `events.ItemPickup` | Skipped during freezetime (the automatic round-start weapon grants), so only real mid-round ground pickups are kept. |
| `bomb_dropped` | `events.BombDropped` | Player via `e.Player`. |
| `bomb_pickup` | `events.BombPickup` | Player via `e.Player`. |
| `bomb_planted` | `events.BombPlanted` | Player via `e.Player`. |

`addEvent` ([main.go:181](../../FilmmakerDemoInfoGo/main.go#L181)) stamps each event with
`p.GameState().IngameTick()` and the account id.

## C++ side

- `DemoEvent { int tick; std::string type; std::string item; uint32_t accountId; }`
  ([DemoEntry.h:38](../../AfxHookSource2/Filmmaker/Demo/DemoEntry.h#L38)). `DemoHelperResult` (the
  full-parse result, [DemoInfoHelper.h:20](../../AfxHookSource2/Filmmaker/Demo/DemoInfoHelper.h#L20))
  carries a `std::vector<DemoEvent> events`. `DemoEntry` (the demo-library row) does not.
- Parsed from the `"events"` array in
  [DemoInfoHelper.cpp:246](../../AfxHookSource2/Filmmaker/Demo/DemoInfoHelper.cpp#L246).
- The Follow camera loads the events lazily on a background thread, keyed on the engine's
  `GetDemoFilePath()` ([`EnsureEventsLoaded`](../../AfxHookSource2/Filmmaker/Movie/FollowEventIndex.cpp)).
  It resolves `accountId` to `ownerName` from the scoreboard. The state JSON carries the events nearest
  the playhead (capped at 60), so a long demo does not flood every frame
  ([FollowCameraStateJson.cpp](../../AfxHookSource2/Filmmaker/Movie/FollowCameraStateJson.cpp)).

## Regenerating a cache

Delete the cache file and re-open the demo. Selecting a Weapon target also triggers the background load.

```bat
del "%APPDATA%\HLAE\source_mvm\demos\cs2\<demo>\scoreboard.fmjson"
```

Use the freshly built helper. `build.bat` writes it to
`build\staging-release\bin\x64\FilmmakerDemoInfo\FilmmakerDemoInfo.exe` during the `go build` step.
The Go helper always does a full parse, so the event list is always populated.
