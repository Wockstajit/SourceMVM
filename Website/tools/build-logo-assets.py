"""Builds the website's logo media from the CS2 startup intro master.

The intro (Assets/startup-video/source-mvm-intro.mp4) is a black logo on a flat orange plate.
The key comes from the red channel: the plate sits at R ~205 and the logo at R ~0, while green
and blue barely separate them. Each pixel's coverage becomes a black-on-white matte, so the page
can composite it over its own orange with `mix-blend-mode: multiply`. For a black logo, multiply
with an inverted matte is exactly alpha compositing (bg * (1 - a)), and it works in every
browser, Safari included, which VP9 alpha video does not.

Outputs (Website/assets/):
  logo-intro.mp4  matte animation, trimmed to end on the last clean frame (the page holds it)
  logo.png        the same final frame as black with real alpha (reduced motion, no autoplay)
  og-image.jpg    1200x630 share card: the untouched orange final frame
  icon-192.png    favicon / touch icon from the launcher's app icon

Requires ffmpeg on PATH. Run from anywhere:  py -3 Website/tools/build-logo-assets.py
"""

import pathlib
import shutil
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
MASTER = REPO / "Assets" / "startup-video" / "source-mvm-intro.mp4"
APP_ICON = REPO / "SourceMovieMakerLauncher" / "build-resources" / "icon.png"
OUT = REPO / "Website" / "assets"

# Frame 94 (t = 3.917 s) is the last frame with no glitch fragments; 95 and 96 smear the logo
# again. The video stops there and the browser keeps showing it.
LAST_CLEAN_FRAME = 94

# The settled logo spans x 304..1580, y 458..699 of the 1920x1080 frame (centre 942, 578).
# The crop keeps the logo centred with room for the glitch fragments around it.
CROP = "crop=1880:640:2:258"

# Red channel at or above PLATE is background (alpha 0); at or below INK it is fully logo.
# The plate's grain dips to ~164 in the opening frames, so PLATE stays under that.
PLATE, INK = 160, 30
COVERAGE = f"({PLATE}-val)*255/{PLATE - INK}"
MATTE = f"lutrgb=r='clip({COVERAGE},0,255)',extractplanes=r"
INK_MATTE = f"lutrgb=r='clip(255-{COVERAGE},0,255)',extractplanes=r"


def ffmpeg(*args):
    cmd = ["ffmpeg", "-v", "error", "-y", *map(str, args)]
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)


def main():
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg is not on PATH")
    OUT.mkdir(parents=True, exist_ok=True)

    ffmpeg(
        "-i", MASTER, "-an",
        "-vf", f"{CROP},format=rgb24,{INK_MATTE},format=yuv420p",
        "-frames:v", LAST_CLEAN_FRAME + 1,
        "-c:v", "libx264", "-preset", "veryslow", "-crf", "16", "-profile:v", "high",
        "-color_range", "tv", "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709",
        "-movflags", "+faststart",
        OUT / "logo-intro.mp4",
    )

    ffmpeg(
        "-i", MASTER,
        "-filter_complex",
        f"[0:v]select=eq(n\\,{LAST_CLEAN_FRAME}),{CROP},format=rgb24,{MATTE},split[a][w];"
        "[w]geq=lum=0[ink];[ink][a]alphamerge,format=ya8",
        "-frames:v", "1", "-update", "1",
        OUT / "logo.png",
    )

    ffmpeg(
        "-i", MASTER,
        "-vf", f"select=eq(n\\,{LAST_CLEAN_FRAME}),crop=1920:1008:0:74,scale=1200:630:flags=lanczos",
        "-frames:v", "1", "-update", "1", "-q:v", "3",
        OUT / "og-image.jpg",
    )

    ffmpeg(
        "-i", APP_ICON, "-vf", "scale=192:192:flags=lanczos",
        "-frames:v", "1", "-update", "1",
        OUT / "icon-192.png",
    )

    for f in sorted(OUT.iterdir()):
        print(f"{f.name:16} {f.stat().st_size / 1024:8.1f} KB")


if __name__ == "__main__":
    main()
