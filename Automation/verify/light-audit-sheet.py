"""Contact sheets and an index page for audit-map-lights.ps1 output.

Usage: python light-audit-sheet.py <audit output dir>
Reads index.json, writes sheet-NN.png (12 labelled thumbnails each) and index.html.
"""
import html
import json
import os
import sys

from PIL import Image, ImageDraw

COLUMNS, ROWS = 4, 3
THUMB = (480, 270)
LABEL = 34


def main(root: str) -> int:
    with open(os.path.join(root, "index.json"), encoding="utf-8-sig") as handle:
        entries = json.load(handle)
    if isinstance(entries, dict):
        entries = [entries]
    per_sheet = COLUMNS * ROWS
    sheets = []
    for first in range(0, len(entries), per_sheet):
        batch = entries[first:first + per_sheet]
        sheet = Image.new("RGB", (COLUMNS * THUMB[0], ROWS * (THUMB[1] + LABEL)), (24, 24, 24))
        draw = ImageDraw.Draw(sheet)
        for i, entry in enumerate(batch):
            x, y = (i % COLUMNS) * THUMB[0], (i // COLUMNS) * (THUMB[1] + LABEL)
            path = os.path.join(root, entry["frame"])
            if os.path.exists(path):
                sheet.paste(Image.open(path).convert("RGB").resize(THUMB), (x, y))
            text = "{label}  {lumens} lm  r={range:.0f}  @({p[0]:.0f},{p[1]:.0f},{p[2]:.0f})".format(
                label=entry["label"], lumens="/".join(str(round(v)) for v in entry["lumens"]),
                range=float(entry["range"]), p=entry["position"])
            draw.text((x + 6, y + THUMB[1] + 8), text, fill=(235, 235, 235))
        name = f"sheet-{first // per_sheet + 1:02d}.png"
        sheet.save(os.path.join(root, name))
        sheets.append(name)
    rows = []
    for entry in entries:
        extra = " ".join(
            f'<a href="{html.escape(entry[key])}"><img src="{html.escape(entry[key])}" width="320"><br>{title}</a>'
            for key, title in (("frameBaked", "baked"), ("frameNoShadows", "no shadows")) if entry.get(key))
        rows.append(
            "<tr><td>{stop}</td><td>{label}<br>Review: {review}</td><td>{lumens}</td><td>{pos}</td>"
            "<td><code>setpos {c[0]} {c[1]} {c[2]}; setang {a[0]} {a[1]} 0</code></td>"
            '<td><a href="{frame}"><img src="{frame}" width="320"></a> {extra}</td></tr>'.format(
                stop=entry["stop"], label=html.escape(entry["label"]),
                review=html.escape(entry.get("validation", {}).get("visualReview", "unvalidated")),
                lumens="/".join(str(round(v)) for v in entry["lumens"]),
                pos=", ".join(str(v) for v in entry["position"]), c=[entry["camera"][0], entry["camera"][1], entry["camera"][2] - 64], a=entry["angles"],
                frame=html.escape(entry["frame"]), extra=extra))
    page = (
        "<!doctype html><meta charset='utf-8'><title>Light audit</title>"
        "<style>body{font:14px system-ui;background:#111;color:#ddd}td{padding:4px 8px;vertical-align:top}"
        "a{color:#8cf}code{font-size:12px}</style>"
        "<h1>Light audit: %d stops</h1><p>Sheets: %s</p>"
        "<table><tr><th>#</th><th>label</th><th>lumens</th><th>light position</th><th>revisit</th><th>frame</th></tr>%s</table>"
        % (len(entries), " ".join(f'<a href="{s}">{s}</a>' for s in sheets), "".join(rows)))
    with open(os.path.join(root, "index.html"), "w", encoding="utf-8") as handle:
        handle.write(page)
    print(f"{len(entries)} stops, {len(sheets)} sheet(s): {os.path.join(root, 'index.html')}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
