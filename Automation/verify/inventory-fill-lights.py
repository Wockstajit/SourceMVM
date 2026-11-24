"""Inventory authored fill lights and flag detached light origins for review.

Pass a MapDataTool cache directory and the map names to inspect. The report uses
world geometry and opaque render casters, including props. A candidate still
needs visual review before anyone changes its lighting.
"""

import argparse
import html
import json
import runpy
import struct
from collections import defaultdict
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def properties(light, *names):
    return {name: light.get("properties", {}).get(name, {}).get("value") for name in names}


def number(value, default):
    try:
        return float(str(value).strip('"'))
    except ValueError:
        return default


def geometry_for_lights(mesh, lights, tools):
    # Amortize the broad-phase scan across lights in each part of the map.
    groups = defaultdict(list)
    for light in lights:
        position = np.array(light["origin"], dtype=float)
        groups[tuple(np.floor(position / 128).astype(int))].append((light, position))

    up = np.array([0, 0, 1])
    for group in groups.values():
        positions = np.array([position for _, position in group])
        low = positions.min(axis=0) - [96, 96, 512]
        high = positions.max(axis=0) + [96, 96, 384]
        a, b, c = tools["near"](mesh, low, high)
        local = dict(a=a, b=b, c=c,
                     low=np.minimum(np.minimum(a, b), c),
                     high=np.maximum(np.maximum(a, b), c))
        for light, position in group:
            distance = tools["nearest_surface"](local, position, 96)
            floor = tools["blocked"](local, position, position - 512 * up)
            roof = tools["blocked"](local, position, position + 384 * up)
            yield light, distance, floor, roof


def read_map(cache, name, tools):
    manifest = json.loads((cache / (name + ".json")).read_text(encoding="utf-8-sig"))
    data = json.loads((cache / (manifest["lightsDigest"] + ".lights.json")).read_text(encoding="utf-8-sig"))
    lights = [light for light in data["lights"] if light["kind"] != "light_environment"]
    mesh = tools["load"](cache, name)
    rows = []
    for light, distance, floor, roof in geometry_for_lights(mesh, lights, tools):
        fields = properties(light, "bouncelight", "bouncescale", "shape", "size_params",
                            "castshadows", "targetname", "precomputedminrange", "precomputedmaxrange")
        direct = light.get("directLight", -1)
        bounce_enabled = number(fields["bouncelight"], -1) != 0 and number(fields["bouncescale"], 1) > 0
        detached = distance > 48
        open_air = floor is not None and roof is None
        identity = light["identity"]
        rows.append(dict(map=name, identity=identity, kind=light["kind"],
                         origin=light["origin"], angles=light["angles"], directLight=direct,
                         lumens=light.get("brightnessLumens"), range=light.get("range"),
                         authoredFill=direct == 0 and bounce_enabled,
                         directAndBounceDisabled=direct == 0 and not bounce_enabled,
                         detached=bool(detached), openAir=bool(open_air),
                         nearest=round(float(distance), 2) if np.isfinite(distance) and distance <= 96 else None,
                         floor=round(floor, 2) if floor is not None else None,
                         roof=round(roof, 2) if roof is not None else None, **fields))
    return sorted(rows, key=lambda row: (not row["authoredFill"], not row["detached"], row["identity"]))


def read_vrf_navigation(path):
    """Read the original Source coordinates from VRF's player-hull GLB export."""
    raw = path.read_bytes()
    if raw[:4] != b"glTF":
        raise ValueError(f"Invalid GLB: {path}")
    length, = struct.unpack_from("<I", raw, 12)
    document = json.loads(raw[20:20 + length])
    binary = 28 + length

    def accessor(index, components):
        info = document["accessors"][index]
        view = document["bufferViews"][info["bufferView"]]
        dtype = {5123: "<u2", 5125: "<u4", 5126: "<f4"}[info["componentType"]]
        offset = binary + view.get("byteOffset", 0) + info.get("byteOffset", 0)
        return np.frombuffer(raw, dtype, info["count"] * components, offset)

    triangles = []
    for node in document["nodes"]:
        if node.get("extras", {}).get("HullIndex") != 0:
            continue
        for primitive in document["meshes"][node["mesh"]]["primitives"]:
            pos = accessor(primitive["attributes"]["POSITION"], 3).reshape(-1, 3)
            indices = accessor(primitive["indices"], 1).reshape(-1, 3)
            triangles.append(pos[indices])
    if not triangles:
        raise ValueError(f"No player navigation hull: {path}")
    return np.concatenate(triangles).astype(float)


def add_navigation(data, folder):
    for name, rows in data.items():
        path = folder / (name + ".glb")
        if not path.exists():
            continue
        triangles = read_vrf_navigation(path)
        low, high = triangles.min(axis=1), triangles.max(axis=1)
        for row in rows:
            point = np.array(row["origin"])
            candidates = triangles[np.all((low[:, :2] <= point[:2]) & (high[:, :2] >= point[:2]), axis=1)]
            a, b, c = candidates.transpose(1, 0, 2)
            ax, ay = (b - a)[:, :2].T
            bx, by = (c - a)[:, :2].T
            px, py = (point - a)[:, :2].T
            determinant = ax * by - ay * bx
            valid = np.abs(determinant) > 1e-7
            denominator = np.where(valid, determinant, 1)
            u = (px * by - py * bx) / denominator
            v = (ax * py - ay * px) / denominator
            z = point[2] - (a[:, 2] + u * (b - a)[:, 2] + v * (c - a)[:, 2])
            underneath = valid & (u >= 0) & (v >= 0) & (u + v <= 1) & (z >= 0) & (z <= 512)
            hits = z[underneath]
            row["playerNavBelow"] = round(float(hits.min()), 1) if len(hits) else None
            row["openAbovePlayerNav"] = bool(row["openAir"] and len(hits) and row["floor"] is not None
                                             and abs(float(hits.min()) - row["floor"]) <= 24)



def draw_location_map(path, triangles, rows):
    """The north-up overview shares row numbers with the report."""
    image = Image.new("RGB", (1000, 790), "#16191e")
    draw = ImageDraw.Draw(image)
    vertices = triangles[:, :, :2].reshape(-1, 2)
    locations = np.array([r["origin"][:2] for r in rows])
    bounds = np.concatenate((vertices, locations)) if len(locations) else vertices
    low, high = bounds.min(axis=0), bounds.max(axis=0)
    span = np.maximum(high - low, 1)
    scale = min(920 / span[0], 650 / span[1])

    def project(point):
        return (40 + (point[0] - low[0]) * scale, 700 - (point[1] - low[1]) * scale)

    for triangle in triangles:
        draw.polygon([project(p) for p in triangle], fill="#3a4651")
    for index, row in enumerate(rows, 1):
        x, y = project(row["origin"])
        color = ("#ffac61" if row["authoredFill"] else
                 "#86ead7" if row.get("openAbovePlayerNav") else "#b6bec7")
        draw.ellipse((x - 4, y - 4, x + 4, y + 4), fill=color, outline="#16191e")
        draw.text((x + 6, y - 6), str(index), fill=color, stroke_width=1, stroke_fill="#16191e")
    draw.text((40, 42), path.stem.removesuffix("-locations") + " / player navigation, overhead", fill="#e5e8ec")
    draw.text((40, 744), "Orange  Authored fill     Teal  Open air above player nav     Grey  Other candidate",
              fill="#e5e8ec")
    image.save(path)



def write_report(output, data, navigation=None):
    esc = html.escape
    parts = [
        '<!doctype html><meta charset="utf-8"><title>CS2 fill-light inventory</title>',
        '<style>body{font:15px system-ui;margin:28px;background:#16191e;color:#e5e8ec}'
        'a{color:#9ed5ff}table{border-collapse:collapse}th,td{padding:6px 10px;border-bottom:1px solid #404650}'
        'td{vertical-align:top}code{white-space:nowrap}details{margin:18px 0}summary{cursor:pointer}</style>',
        '<h1>CS2 fill-light inventory</h1>',
        '<p>Authored fills use direct mode 0 with bounce enabled or left at its default. '
        'Floating candidates have more than 48 units of clearance from collision and opaque caster geometry. '
        'Open-air candidates have geometry within 512 units below and no roof within 384 units above. '
        'The player-nav flag uses VRF\'s exported player hull and requires its height to agree with the first floor hit. The tests cannot detect every decorative mesh.</p>',
        '<p>Coordinates are light origins, not camera placements. Each console command targets exactly '
        'one authored light. Change <code>off</code> to <code>on</code> to restore it.</p>',
        '<p><a href="lights.json">All authored lights and geometry measurements</a></p>',
    ]
    for name, rows in data.items():
        review = [r for r in rows if r["authoredFill"] or r["directAndBounceDisabled"]
                  or r["detached"] or r.get("openAbovePlayerNav", False)]
        parts.append(f'<details><summary>{esc(name)} Ã‚Â· {len(rows)} lights Ã‚Â· '
                     f'{sum(r["authoredFill"] for r in rows)} authored fills Ã‚Â· '
                     f'{sum(r["detached"] for r in rows)} floating candidates Ã‚Â· '
                     f'{sum(r["detached"] and r["openAir"] for r in rows)} open-air floating candidates Â· '
                     f'{sum(r["detached"] and r.get("openAbovePlayerNav", False) for r in rows)} floating above player nav · '
                     f'{sum(r.get("openAbovePlayerNav", False) for r in rows)} above open player nav total</summary>')
        nav = navigation / (name + ".glb") if navigation else None
        if nav and nav.exists():
            plot = f"{name}-locations.png"
            draw_location_map(output / plot, read_vrf_navigation(nav), review)
            parts.append(f'<p><a href="{plot}">Open the numbered map overview</a></p>')
        parts.append('<table><tr><th>#</th><th>Light</th><th>Origin XYZ</th><th>Direct / bounce</th><th>Lumens / range</th>'
                     '<th>Nearest / floor / roof</th><th>Review</th><th>Toggle off</th></tr>')
        for index, r in enumerate(review, 1):
            flags = ", ".join(label for label, present in [
                ("Authored fill", r["authoredFill"]), ("Direct and bounce disabled", r["directAndBounceDisabled"]),
                ("Floating", r["detached"]), ("Open air", r["openAir"]),
                ("Above player nav", r.get("openAbovePlayerNav", False))] if present)
            command = f'mirv_relight light "{r["identity"]}" off'
            identity = r["identity"].rsplit(":hammer:", 1)[-1]
            xyz = " ".join(f"{v:.1f}" for v in r["origin"])
            nearest, floor, roof = (r["nearest"] if r["nearest"] is not None else ">96",
                                    r["floor"] if r["floor"] is not None else ">512",
                                    r["roof"] if r["roof"] is not None else ">384")
            parts.append(f'<tr><td>{index}</td><td>{esc(r["kind"])}<br>{esc(identity)}</td><td><code>{xyz}</code></td>'
                         f'<td>{r["directLight"]} / {esc(str(r["bouncelight"]))}</td>'
                         f'<td>{r["lumens"]} / {r["range"]}</td><td>{nearest} / {floor} / {roof}</td>'
                         f'<td>{esc(flags)}</td><td><code>{esc(command)}</code></td></tr>')
        parts.append('</table></details>')
    (output / "index.html").write_text("\n".join(parts), encoding="utf-8")
    (output / "lights.json").write_text(json.dumps(data, indent=2), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cache", type=Path)
    parser.add_argument("maps", nargs="+")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--nav", type=Path, help="Directory of VRF-decompiled map navigation GLBs")
    args = parser.parse_args()
    tools = runpy.run_path(str(Path(__file__).with_name("camera-clear.py")))
    args.out.mkdir(parents=True, exist_ok=True)
    data = {}
    for name in args.maps:
        data[name] = read_map(args.cache, name, tools)
        print(f"{name}: {len(data[name])} local lights", flush=True)
    if args.nav:
        add_navigation(data, args.nav)
    write_report(args.out, data, args.nav)


if __name__ == "__main__":
    main()
