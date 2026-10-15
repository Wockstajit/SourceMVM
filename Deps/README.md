# Dependencies

## ./dev: development-only dependencies

Dependencies that are not explicit in release builds.

## ./release: release build dependencies

Explicit dependencies in release builds. Most are git submodules (`git clone --recursive`, or
`git submodule update --init --recursive` after a plain clone); see [.gitmodules](../.gitmodules).

These are vendored as source, trimmed to what the native map extractor
(`AfxHookSource2/Filmmaker/MapData`) uses, each with its upstream license file:

| Folder | Upstream | Version | Used for |
| --- | --- | --- | --- |
| `release/lz4` | [lz4](https://github.com/lz4/lz4) (`lz4.c`/`lz4.h`) | 1.10.0 | KV3, texture and mesh LZ4 blocks |
| `release/zstd` | [zstd](https://github.com/facebook/zstd) (decompression sources only) | 1.5.7 | KV3 and mesh ZSTD blocks |
| `release/meshoptimizer` | [meshoptimizer](https://github.com/zeux/meshoptimizer) (vertex/index codecs) | 0.25 | compressed mesh buffers |
| `release/bcdec` | [bcdec](https://github.com/iOrange/bcdec) | 0.985 | BC1-BC7 texture decoding |
