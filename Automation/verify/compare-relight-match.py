"""Measure a verify-relight-match capture set and create an honest A/B review page."""
import argparse
import html
import json
from pathlib import Path

import numpy as np
from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    root = args.directory
    names = ("native", "native-repeat", "reference", "relight", "relight-repeat")
    images = {}
    for name in names:
        with Image.open(root / f"de_mirage-{name}.png") as image:
            images[name] = np.asarray(image.convert("RGB"), dtype=np.float32)
    shape = images["native"].shape
    if any(image.shape != shape for image in images.values()) or shape[0] < 100:
        raise ValueError("Capture dimensions must match and include the full viewport")
    # CS2 places its changing build/time watermark in the bottom 20 pixels.
    baseline = images["native"][:-20]
    rows = {}
    for name in names[1:]:
        difference = np.abs(images[name][:-20] - baseline)
        rows[name] = {
            "mean_absolute_error_255": float(difference.mean()),
            "p95_channel_error_255": float(np.percentile(difference, 95)),
            "pixels_over_8_percent": float(100 * np.mean(difference.max(axis=2) > 8)),
        }
    result = {"excluded_bottom_rows": 20, "comparison": rows,
              "acceptance": "Requires visual review; native-input reference is not relighting."}
    (root / "metrics.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    table = "".join(
        f"<tr><td>{html.escape(name)}</td><td>{row['mean_absolute_error_255']:.3f}</td>"
        f"<td>{row['p95_channel_error_255']:.1f}</td>"
        f"<td>{row['pixels_over_8_percent']:.2f}%</td></tr>"
        for name, row in rows.items()
    )
    page = """<!doctype html><meta charset="utf-8"><title>Mirage rendering comparison</title>
<style>
body{background:#17191d;color:#eceef2;font:16px system-ui;margin:24px auto;max-width:1600px}
h1{font-size:24px}p{max-width:1000px;line-height:1.5}button,input{font:inherit}
button{margin:0 8px 12px 0;padding:7px 12px;cursor:pointer}
.image{position:relative;width:100%;aspect-ratio:16/9;background:black}
.image img{position:absolute;width:100%;height:100%;object-fit:contain}
#relight{clip-path:inset(0 0 0 50%)}
input{width:100%;margin:16px 0}table{border-collapse:collapse}td,th{padding:8px 20px;border-bottom:1px solid #454950;text-align:left}
</style><h1>Mirage · fixed T-spawn lane</h1>
<p>Native CS2 with Environment and relighting off is on the left; actual relighting is on the right.
Drag the divider or use the buttons to compare the full frames. The 3D skybox currently retains native lighting.
The untouched native-input replay below only tests compositing and cannot establish relighting quality.</p>
<button onclick="split(100)">Native only</button><button onclick="split(0)">Relighting only</button>
<button onclick="split(50)">Split</button>
<div class="image"><img src="de_mirage-native.png" alt="Native CS2"><img id="relight" src="de_mirage-relight.png" alt="Actual relighting"></div>
<input id="divider" type="range" min="0" max="100" value="50" aria-label="Native and relit divider" oninput="split(this.value)">
<p>Errors use 0–255 RGB channel values and exclude only the bottom 20 rows containing the changing engine watermark.
Native-repeat measures the capture noise. These numbers do not automatically declare visual parity.</p>
<table><tr><th>Compared with native</th><th>Mean absolute error</th><th>95th percentile</th><th>Pixels over 8</th></tr>""" + table + """</table>
<script>function split(value){document.getElementById('divider').value=value;document.getElementById('relight').style.clipPath=`inset(0 0 0 ${value}%)`}</script>"""
    (root / "comparison.html").write_text(page, encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
