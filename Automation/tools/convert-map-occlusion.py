"""Convert Source 2 Viewer's physics GLB to a validated, read-only shadow mesh cache.

No render materials or textures are copied. Invisible clips, sky and glass are excluded.
Requires numpy. This is an offline asset preparation tool, not a game runtime dependency.
"""
import argparse
import json
import os
import struct
from pathlib import Path
import numpy as np


def convert(source, destination):
    data = Path(source).read_bytes()
    if len(data) > 900 * 1024 * 1024 or data[:4] != b'glTF':
        raise ValueError('Expected a bounded GLB physics export')
    magic, version, length = struct.unpack_from('<III', data)
    if version != 2 or length != len(data):
        raise ValueError('Invalid GLB length/version')
    size, kind = struct.unpack_from('<II', data, 12)
    if kind != 0x4e4f534a:
        raise ValueError('Missing GLB JSON')
    doc = json.loads(data[20:20+size])
    start = 20 + size
    binary_size, kind = struct.unpack_from('<II', data, start)
    if kind != 0x004e4942 or start+8+binary_size > len(data):
        raise ValueError('Missing GLB binary')
    binary = memoryview(data)[start+8:start+8+binary_size]

    def accessor(index):
        a = doc['accessors'][index]
        if 'sparse' in a:
            raise ValueError('Sparse physics accessors are unsupported')
        v = doc['bufferViews'][a['bufferView']]
        dtype = {5126: '<f4', 5125: '<u4', 5123: '<u2'}[a['componentType']]
        width = {'VEC3': 3, 'SCALAR': 1}[a['type']]
        stride = v.get('byteStride', width*np.dtype(dtype).itemsize)
        offset = v.get('byteOffset', 0)+a.get('byteOffset', 0)
        return np.ndarray((a['count'], width), dtype=dtype, buffer=binary,
                          offset=offset, strides=(stride, np.dtype(dtype).itemsize)).copy()

    # VRF exports Source inches (X forward, Z up) to glTF meters with (Y,Z,X) axes.
    source_to_gltf = np.array([[0,.0254,0,0],[0,0,.0254,0],[.0254,0,0,0],[0,0,0,1]], dtype=np.float64)
    gltf_to_source = np.linalg.inv(source_to_gltf)
    parts, skipped = [], []
    def visit(index, parent):
        node = doc['nodes'][index]
        if any(k in node for k in ('translation','rotation','scale')):
            raise ValueError('Unexpected TRS physics node; expected a VRF matrix')
        transform = parent @ np.array(node.get('matrix', np.eye(4).flatten(order='F')), dtype=float).reshape((4,4), order='F')
        info = node.get('extras', {})
        tags = info.get('InteractAs', [])
        excluded = any('clip' in tag or tag in ('sky','window') for tag in tags) or 'glass' in info.get('SurfaceProperty','')
        if 'mesh' in node and not excluded:
            for primitive in doc['meshes'][node['mesh']]['primitives']:
                if primitive.get('mode',4) != 4:
                    raise ValueError('Physics export must contain triangles')
                points = accessor(primitive['attributes']['POSITION'])
                matrix = gltf_to_source @ transform
                points = points @ matrix[:3,:3].T + matrix[:3,3]
                indices = accessor(primitive['indices']).flatten()
                if len(indices)%3 or indices.max(initial=0)>=len(points):
                    raise ValueError('Invalid triangle indices')
                parts.append(points[indices].astype('<f4'))
        elif 'mesh' in node:
            skipped.append(node.get('name', str(index)))
        for child in node.get('children', []):
            visit(child, transform)
    for node in doc['scenes'][doc.get('scene',0)]['nodes']:
        visit(node, np.eye(4))
    if not parts:
        raise ValueError('No opaque collision geometry found')
    triangles = np.concatenate(parts).reshape((-1,3,3))
    edges = np.cross(triangles[:,1]-triangles[:,0],triangles[:,2]-triangles[:,0])
    triangles = triangles[np.sum(edges*edges,axis=1)>1e-8]
    vertices, indices = np.unique(triangles.reshape((-1,3)), axis=0, return_inverse=True)
    vertices = vertices.astype('<f4'); indices = indices.astype('<u4')
    if len(vertices)>8000000 or len(indices)>24000000 or not np.isfinite(vertices).all() or np.abs(vertices).max()>100000:
        raise ValueError('Physics mesh exceeds runtime bounds')
    low, high = vertices.min(axis=0), vertices.max(axis=0)
    destination = Path(destination); destination.parent.mkdir(parents=True,exist_ok=True)
    temp = destination.with_suffix(destination.suffix+'.tmp')
    with temp.open('wb') as out:
        out.write(struct.pack('<8sII6f', b'MVMOCC01',len(vertices),len(indices),*low,*high))
        out.write(vertices.tobytes());out.write(indices.tobytes())
    os.replace(temp,destination)
    return {'vertices':len(vertices),'triangles':len(indices)//3,'bounds':[low.tolist(),high.tolist()],
            'excluded':skipped, 'bytes':destination.stat().st_size}


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path);parser.add_argument('destination',type=Path)
    args=parser.parse_args()
    print(json.dumps(convert(args.source,args.destination),indent=2))
