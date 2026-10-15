# Smoke-tick scanner

This standalone program prints the tick window of every smoke cloud in an offline demo, for
render-pass occlusion checks. It shares the parent Go module's demoinfocs dependency but leaves
the scoreboard parser unchanged. [`main.go`](main.go) is the only scanner implementation.

From the repository root:

```batch
go -C FilmmakerDemoInfoGo run ./smokescan "<CS2>/game/csgo/replays/repaired/<demo>.dem"
```
