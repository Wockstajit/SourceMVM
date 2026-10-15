"""Import a native backdrop and small avatar portraits from a local capture.

Rectangles come from a capture-specific JSON file, never from a full UI screenshot
used as a fake interactive page. Output stays in the ignored assets/ directory.
Usage: python import-menu-artwork.py capture.json
"""
import json
import sys
from pathlib import Path
from PIL import Image, PngImagePlugin

root = Path(__file__).resolve().parent
spec = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
out = root / "assets" / "menu"
out.mkdir(parents=True, exist_ok=True)
for entry in spec["images"]:
    source = Path(entry["source"]).resolve()
    image = Image.open(source)
    x, y, width, height = entry["rect"]
    if x < 0 or y < 0 or x + width > image.width or y + height > image.height:
        raise ValueError(f"Crop outside {source.name}: {entry['rect']}")
    name = entry["name"]
    if Path(name).name != name or not name.endswith(".png"):
        raise ValueError("Artwork name must be a PNG basename")
    metadata = PngImagePlugin.PngInfo()
    metadata.add_text("Source", f"Local CS2 capture {source.name}; crop {entry['rect']}")
    image.crop((x, y, x + width, y + height)).save(out / name, pnginfo=metadata)
print(f"Imported {len(spec['images'])} local native artwork assets.")
