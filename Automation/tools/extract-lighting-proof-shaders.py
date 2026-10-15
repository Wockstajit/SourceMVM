"""Match live SHEX/SHDR FNV hashes to VRF-decoded VCS bytecode records.

Supply metadata exported by Source2Viewer-CLI -a from the same VCS file.
This extracts evidence only; it does not infer a native resource ABI.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "build/re-deps"))
import zstandard


def extract(vcs, metadata, dictionary, wanted, output):
    data = vcs.read_bytes()
    base = struct.unpack_from("<I", data)[0]
    meta = metadata.read_text(encoding="utf-8-sig")
    records = re.findall(r"m_nOffs = (\d+)\s+m_nSize = (\d+)", meta[meta.index("m_byteCodeData ="):])
    decoder = zstandard.ZstdDecompressor(dict_data=zstandard.ZstdCompressionDict(dictionary.read_bytes()))
    output.mkdir(parents=True, exist_ok=True)
    remaining = set(wanted)
    for block, (offset, length) in enumerate(records):
        start = base + int(offset)
        if start + 12 > len(data):
            raise ValueError("bytecode record outside VCS")
        marker, size, compressed = struct.unpack_from("<iII", data, start)
        if size > 256 * 1024 * 1024 or start + 12 + compressed > len(data):
            raise ValueError("invalid bytecode record size")
        payload = decoder.decompress(data[start + 12:start + 12 + compressed], max_output_size=size)
        cursor = 0
        while cursor < len(payload):
            if payload[cursor:cursor + 4] != b"DXBC" or cursor + 32 > len(payload):
                raise ValueError("invalid DXBC header")
            length = struct.unpack_from("<I", payload, cursor + 24)[0]
            if length < 32 or cursor + length > len(payload):
                raise ValueError("invalid DXBC length")
            code = payload[cursor:cursor + length]
            cursor += length
            chunks = struct.unpack_from("<I", code, 28)[0]
            if 32 + chunks * 4 > len(code):
                raise ValueError("invalid DXBC chunk table")
            for index in range(chunks):
                at = struct.unpack_from("<I", code, 32 + index * 4)[0]
                if at + 8 > len(code):
                    raise ValueError("invalid chunk offset")
                chunk_size = struct.unpack_from("<I", code, at + 4)[0]
                if at + 8 + chunk_size > len(code):
                    raise ValueError("invalid chunk size")
                if code[at:at + 4] not in (b"SHEX", b"SHDR"):
                    continue
                value = 14695981039346656037
                for byte in code[at + 8:at + 8 + chunk_size]:
                    value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
                if value in remaining:
                    path = output / f"{value:016x}.dxbc"
                    path.write_bytes(code)
                    print(f"MATCH {value:016x} block={block} bytes={len(code)}", flush=True)
                    remaining.remove(value)
            if not remaining:
                return
    if remaining:
        raise ValueError("Hashes absent: " + ", ".join(f"{value:016x}" for value in sorted(remaining)))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vcs", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--dictionary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("hashes", nargs="+", type=lambda value: int(value, 16))
    args = parser.parse_args()
    extract(args.vcs, args.metadata, args.dictionary, args.hashes, args.output)
