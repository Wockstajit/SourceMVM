# Builds the Players Only / Green Screen sky material (materials/skybox/sky_green.vmat_c and
# its cube texture) for FX/render-materials/prebuilt/.
#
# STANDALONE BY DESIGN (repo hard rule: a new capability is its own thing). This tool must not
# be folded into build-render-materials.ps1 (which runs resourcecompiler over authored .vmat
# sources) or any other converter. It exists because the 2026-08-13 CS2 update REMOVED
# resourcecompiler.exe and the workshop-tools DLLs from the retail install, so the green sky
# cannot currently be compiled from source on this machine.
#
# WHAT IT DOES instead of compiling: derives the material from CS2's own compiled assets.
#   1. Extracts the native flat-black sky material pair (materials/skybox/sky_black.vmat_c +
#      its 4x4 DXT1 cube .vtex_c) from pak01_dir.vpk with the Source2Viewer/VRF CLI
#      (an external helper, NOT a build dependency -- see the repo's VRF notes).
#   2. Byte-patches every `_black` to `_green`. Equal length is what makes this safe: the
#      compiled KV3 blocks (RED2/DATA) are LZ4-compressed, and the name fragments are inline
#      literals, so a same-length patch propagates consistently through the back-references
#      (verified by decompiling the result with VRF). This is also why the material keeps the
#      materials/skybox/ path -- it cannot be renamed into materials/filmmaker/.
#   3. Rewrites the RERL external-reference id with MurmurHash64B(path, 0xEDABCDEF), the same
#      hash the resource system uses (matches the engine's PreCache wrapper in SceneSystem.cpp).
#   4. Repaints the texture's 48-byte mip payload (6 faces x one 4x4 DXT1 block) solid green.
#      The cubemap radiance SH coefficients stay zero, so the green sky does not relight the
#      scene -- exactly right for a chroma plate.
#
# The result renders through the engine's own env_sky path as a uniform green dome
# (verified in-game 2026-08-20 on the Train remake). Consumed by
# SceneSystem_SetPlayersOnlyGreenSkyOverride; staged by build-render-materials.ps1.
#
# Usage:  python FX/tools/build-green-sky.py
#         (optional: --cs2-dir <install root>, --vrf <Source2Viewer-CLI.exe>)

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# A ValveResourceFormat checkout next to this repository; set SOURCEMVM_VRF_CLI to point elsewhere.
DEFAULT_VRF = os.environ.get("SOURCEMVM_VRF_CLI") or os.path.join(
    os.path.dirname(REPO_ROOT), "ValveResourceFormat", "CLI", "bin", "Release", "Source2Viewer-CLI.exe")
SRC_VMAT = "materials/skybox/sky_black.vmat_c"
SRC_VTEX = "materials/skybox/sky_black_vmat_g_tskytexture_75901da7.vtex_c"
OLD_RERL_ID = bytes.fromhex("E6BCD80D679C8127")[::-1]  # little-endian on disk
NEW_TEXTURE_PATH = b"materials/skybox/sky_green_vmat_g_tskytexture_75901da7.vtex"
GREEN_DXT1_BLOCK = bytes.fromhex("e007000000000000")  # color0=0x07E0 (pure 565 green), all idx 0


def murmur64b(data: bytes, seed: int) -> int:
    m, r = 0x5BD1E995, 24
    l = len(data)
    h1 = (seed ^ l) & 0xFFFFFFFF
    h2 = 0
    i = 0
    while l >= 8:
        k1 = int.from_bytes(data[i:i + 4], "little"); i += 4
        k1 = (k1 * m) & 0xFFFFFFFF; k1 ^= k1 >> r; k1 = (k1 * m) & 0xFFFFFFFF
        h1 = (h1 * m) & 0xFFFFFFFF; h1 ^= k1
        k2 = int.from_bytes(data[i:i + 4], "little"); i += 4
        k2 = (k2 * m) & 0xFFFFFFFF; k2 ^= k2 >> r; k2 = (k2 * m) & 0xFFFFFFFF
        h2 = (h2 * m) & 0xFFFFFFFF; h2 ^= k2
        l -= 8
    if l >= 4:
        k1 = int.from_bytes(data[i:i + 4], "little"); i += 4
        k1 = (k1 * m) & 0xFFFFFFFF; k1 ^= k1 >> r; k1 = (k1 * m) & 0xFFFFFFFF
        h1 = (h1 * m) & 0xFFFFFFFF; h1 ^= k1
        l -= 4
    rem = data[i:]
    if l == 3: h2 ^= rem[2] << 16
    if l >= 2: h2 ^= rem[1] << 8
    if l >= 1:
        h2 ^= rem[0]
        h2 = (h2 * m) & 0xFFFFFFFF
    h1 = ((h1 ^ (h2 >> 18)) * m) & 0xFFFFFFFF
    h2 = ((h2 ^ (h1 >> 22)) * m) & 0xFFFFFFFF
    h1 = ((h1 ^ (h2 >> 17)) * m) & 0xFFFFFFFF
    h2 = ((h2 ^ (h1 >> 19)) * m) & 0xFFFFFFFF
    return (h1 << 32) | h2


def resolve_cs2_dir(explicit: str) -> str:
    if explicit:
        return explicit
    env = os.environ.get("SOURCEMVM_CS2_DIR", "")
    if env:
        return env
    resolver = os.path.join(REPO_ROOT, "Automation", "lib", "Resolve-Cs2Dir.ps1")
    out = subprocess.run(["powershell.exe", "-NoProfile", "-File", resolver],
                         capture_output=True, text=True)
    path = out.stdout.strip()
    if not path:
        sys.exit("CS2 install not found. Pass --cs2-dir or set SOURCEMVM_CS2_DIR.")
    return path


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cs2-dir", default="")
    ap.add_argument("--vrf", default=DEFAULT_VRF)
    args = ap.parse_args()

    cs2 = resolve_cs2_dir(args.cs2_dir)
    vpk = os.path.join(cs2, "game", "csgo", "pak01_dir.vpk")
    if not os.path.isfile(vpk):
        sys.exit(f"pak01_dir.vpk not found: {vpk}")
    if not os.path.isfile(args.vrf):
        sys.exit(f"Source2Viewer-CLI.exe not found: {args.vrf} (pass --vrf)")

    out_dir = os.path.join(REPO_ROOT, "FX", "render-materials", "prebuilt", "materials", "skybox")
    os.makedirs(out_dir, exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        # 1. Extract the native pair (raw, no decompile).
        subprocess.run([args.vrf, "-i", vpk, "-f", "materials/skybox/sky_black", "-o", tmp],
                       check=True, capture_output=True)
        vmat = bytearray(open(os.path.join(tmp, *SRC_VMAT.split("/")), "rb").read())
        vtex = bytearray(open(os.path.join(tmp, *SRC_VTEX.split("/")), "rb").read())

    # 2. Same-length rename everywhere (RERL string + LZ4 literals in RED2/DATA).
    count = vmat.count(b"_black")
    if count != 3:
        sys.exit(f"expected 3 _black occurrences in sky_black.vmat_c, found {count} -- "
                 "the native material changed; re-verify the patch by decompiling with VRF.")
    vmat = bytearray(vmat.replace(b"_black", b"_green"))

    # 3. Fix the RERL external-reference id for the renamed texture path.
    idx = vmat.find(OLD_RERL_ID)
    if idx < 0:
        sys.exit("RERL id E6BCD80D679C8127 not found -- the native material changed.")
    vmat[idx:idx + 8] = murmur64b(NEW_TEXTURE_PATH, 0xEDABCDEF).to_bytes(8, "little")

    # 4. Repaint the mip payload: 6 cube faces x one 4x4 DXT1 block at the file tail.
    if vtex[-48:] != b"\x00" * 48:
        sys.exit("sky_black cube payload is no longer 48 zero bytes -- the native texture "
                 "changed; re-inspect with VRF (-b DATA) before patching.")
    vtex[-48:] = GREEN_DXT1_BLOCK * 6

    open(os.path.join(out_dir, "sky_green.vmat_c"), "wb").write(bytes(vmat))
    open(os.path.join(out_dir, "sky_green_vmat_g_tskytexture_75901da7.vtex_c"), "wb").write(bytes(vtex))
    print(f"wrote sky_green pair to {out_dir}")


if __name__ == "__main__":
    main()
