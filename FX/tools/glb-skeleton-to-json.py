"""Write a Call of Duty player rest skeleton from a glTF binary (.glb) in the cod-player-skeleton.json
format extract-death-anims.py reads: bones with name, parent, local pos (centimetres) and xyzw quat.

    py -3 FX/tools/glb-skeleton-to-json.py <model.glb> <out.json> [--inspect]

Exporters add suffixes such as `_05` to node names (`j_spine4_05`); they are stripped so the names
match the animation curves. The first skin's joints define the skeleton; any ancestor nodes above
the skin root (export scale/rotation wrappers) are folded into the root bone.
"""

import argparse
import json
import math
import re
import struct


def read_glb(path):
    with open(path, 'rb') as handle:
        data = handle.read()
    magic, _, _ = struct.unpack_from('<III', data, 0)
    if magic != 0x46546C67:
        raise ValueError('not a glb file')
    length, kind = struct.unpack_from('<II', data, 12)
    if kind != 0x4E4F534A:
        raise ValueError('first glb chunk is not JSON')
    return json.loads(data[20:20 + length])


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def qrot(q, v):
    x, y, z, w = q
    vq = qmul(qmul(q, (v[0], v[1], v[2], 0.0)), (-x, -y, -z, w))
    return vq[:3]


def decompose(m):
    """glTF column-major 4x4 (no shear) to translation, xyzw rotation, scale."""
    cols = [m[0:3], m[4:7], m[8:11]]
    scale = [math.sqrt(sum(c * c for c in col)) for col in cols]
    r = [[cols[j][i] / scale[j] for j in range(3)] for i in range(3)]  # row-major rotation
    trace = r[0][0] + r[1][1] + r[2][2]
    if trace > 0:
        s = math.sqrt(trace + 1.0) * 2
        q = ((r[2][1] - r[1][2]) / s, (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s, 0.25 * s)
    elif r[0][0] > r[1][1] and r[0][0] > r[2][2]:
        s = math.sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2
        q = (0.25 * s, (r[0][1] + r[1][0]) / s, (r[0][2] + r[2][0]) / s, (r[2][1] - r[1][2]) / s)
    elif r[1][1] > r[2][2]:
        s = math.sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2
        q = ((r[0][1] + r[1][0]) / s, 0.25 * s, (r[1][2] + r[2][1]) / s, (r[0][2] - r[2][0]) / s)
    else:
        s = math.sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2
        q = ((r[0][2] + r[2][0]) / s, (r[1][2] + r[2][1]) / s, 0.25 * s, (r[1][0] - r[0][1]) / s)
    return tuple(m[12:15]), q, tuple(scale)


def clean(name):
    return re.sub(r'_0\d+$', '', name or '')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('glb')
    parser.add_argument('out')
    parser.add_argument('--inspect', action='store_true', help='print the root chain and scale, write nothing')
    args = parser.parse_args()
    gltf = read_glb(args.glb)
    nodes = gltf['nodes']
    parent_of = {}
    for index, node in enumerate(nodes):
        for child in node.get('children', []):
            parent_of[child] = index
    joints = gltf['skins'][0]['joints']
    joint_set = set(joints)

    def local(index):
        node = nodes[index]
        if 'matrix' in node:
            return decompose(node['matrix'])
        t = tuple(node.get('translation', (0.0, 0.0, 0.0)))
        r = tuple(node.get('rotation', (0.0, 0.0, 0.0, 1.0)))
        s = tuple(node.get('scale', (1.0, 1.0, 1.0)))
        return t, r, s

    # Fold every non-joint ancestor of the skeleton root into one transform (uniform scale only).
    root = next(j for j in joints if parent_of.get(j) not in joint_set)
    chain, cursor = [], parent_of.get(root)
    while cursor is not None:
        chain.append(cursor)
        cursor = parent_of.get(cursor)
    wrap_t, wrap_r, wrap_s = (0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0), 1.0
    for index in reversed(chain):
        t, r, s = local(index)
        scaled = tuple(c * wrap_s for c in qrot(wrap_r, t))
        wrap_t = tuple(a + b for a, b in zip(wrap_t, scaled))
        wrap_r = qmul(wrap_r, r)
        wrap_s *= s[0]
    if args.inspect:
        print('root', clean(nodes[root].get('name')), 'wrappers', [nodes[i].get('name') for i in chain])
        print('wrapper translation', wrap_t, 'rotation', wrap_r, 'scale', wrap_s)
    bones, index_of = [], {}
    for joint in joints:
        t, r, s = local(joint)
        parent = parent_of.get(joint)
        # The wrappers only turn the game's Z-up into glTF's Y-up (and may rescale): the joints below
        # them are already in the game's own frame, which is what the animation curves use, so the
        # wrapper rotation is not applied. Folding it in stood every clip along +Y.
        scale = wrap_s
        index_of[joint] = len(bones)
        bones.append({'name': clean(nodes[joint].get('name')), 'parent': parent,
                      'pos': [c * scale for c in t], 'quat': list(r)})
    for bone in bones:
        bone['parent'] = index_of.get(bone['parent'], -1)
    # Sketchfab's Call of Duty exports keep the game's centimetres (j_mainroot sits ~97 above the
    # feet), the unit the animation curves use; the wrapper nodes only turn Z-up into Y-up.
    for bone in bones:
        bone['pos'] = [round(c, 5) for c in bone['pos']]
        bone['quat'] = [round(c, 7) for c in bone['quat']]
    if args.inspect:
        names = {b['name']: b for b in bones}
        for key in ('tag_origin', 'j_mainroot', 'j_hip_le', 'j_spine4', 'j_head'):
            if key in names:
                print(key, names[key]['pos'])
        print(len(bones), 'bones')
        return
    with open(args.out, 'w', encoding='utf-8') as handle:
        json.dump({'source': 'rest skeleton from %s (FX/tools/glb-skeleton-to-json.py); local transforms in centimetres'
                   % args.glb.replace('\\', '/').split('/')[-1], 'units': 'cm', 'bones': bones}, handle, indent=1)
        handle.write('\n')
    print('wrote', args.out, len(bones), 'bones')


if __name__ == '__main__':
    main()
