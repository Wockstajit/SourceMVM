#!/usr/bin/env python3
"""Read AMD64 thread instruction pointers from a local Windows minidump.

This is a context inventory, not a stack unwinder or a root-cause verdict. Dumps stay local.
"""
import argparse
import json
import struct
from pathlib import Path


def read_contexts(path):
    data = path.read_bytes()
    if data[:4] != b'MDMP':
        raise ValueError('Not a Windows minidump')
    count, directory = struct.unpack_from('<II', data, 8)
    streams = {}
    for i in range(count):
        kind, size, offset = struct.unpack_from('<III', data, directory + i * 12)
        streams[kind] = (offset, size)
    system = streams.get(7)
    if not system or struct.unpack_from('<H', data, system[0])[0] != 9:
        raise ValueError('Expected an AMD64 minidump')
    modules = []
    offset, _ = streams[4]
    for i in range(struct.unpack_from('<I', data, offset)[0]):
        row = offset + 4 + i * 108
        base, size = struct.unpack_from('<QI', data, row)
        name_rva = struct.unpack_from('<I', data, row + 20)[0]
        name_length = struct.unpack_from('<I', data, name_rva)[0]
        name = data[name_rva + 4:name_rva + 4 + name_length].decode('utf-16-le')
        modules.append((base, size, name.replace('\\', '/').rsplit('/', 1)[-1]))

    def resolve(address):
        for base, size, name in modules:
            if base <= address < base + size:
                return f'{name}+0x{address - base:x}'
        return f'0x{address:x} (no module)'

    threads = []
    offset, _ = streams[3]
    for i in range(struct.unpack_from('<I', data, offset)[0]):
        row = offset + 4 + i * 48
        tid = struct.unpack_from('<I', data, row)[0]
        context_size, context_rva = struct.unpack_from('<II', data, row + 40)
        if context_size < 256:
            threads.append({'thread': tid, 'context': 'missing or incomplete'})
            continue
        rsp = struct.unpack_from('<Q', data, context_rva + 152)[0]
        rip = struct.unpack_from('<Q', data, context_rva + 248)[0]
        threads.append({'thread': tid, 'ip': resolve(rip), 'rip': hex(rip), 'rsp': hex(rsp)})
    return threads


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path)
    args = parser.parse_args()
    print(json.dumps(read_contexts(args.dump), indent=2))
