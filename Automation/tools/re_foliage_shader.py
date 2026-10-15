"""Read-only VCS v71 bytecode extraction for foliage animation research.

Inputs are the locally installed .vcs, its VRF 20 `-a` metadata dump, and VRF's
vfx_zstd_2bc2fa87 dictionary. No game files or process memory are modified.
The 12-byte block prefix and offsets are verified against the metadata and DXBC length.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import zstandard


def extract(vcs, metadata, dictionary, output, fxc=None):
    data=Path(vcs).read_bytes();base=struct.unpack_from('<I',data)[0]
    text=Path(metadata).read_text(encoding='utf-8-sig')
    records=re.findall(r'm_nOffs = (\d+)\s+m_nSize = (\d+)',text[text.index('m_byteCodeData ='):])
    decoder=zstandard.ZstdDecompressor(dict_data=zstandard.ZstdCompressionDict(Path(dictionary).read_bytes()))
    root=Path(output);root.mkdir(parents=True,exist_ok=True);report=[]
    for block,(offset,length) in enumerate(records):
        start=base+int(offset);length=int(length)
        marker,decoded_size,compressed_size=struct.unpack_from('<iII',data,start)
        if marker!=-3 or compressed_size+12!=length or start+length>len(data):
            raise ValueError('Unexpected VCS bytecode compression/header')
        decoded=decoder.decompress(data[start+12:start+length],max_output_size=decoded_size)
        if len(decoded)!=decoded_size:raise ValueError('Invalid decompressed length')
        cursor=0;combo=0
        while cursor<len(decoded):
            if decoded[cursor:cursor+4]!=b'DXBC':raise ValueError('Expected DXBC at combo boundary')
            size=struct.unpack_from('<I',decoded,cursor+24)[0]
            if size<32 or cursor+size>len(decoded):raise ValueError('Invalid DXBC length')
            path=root/f'block-{block:02d}-combo-{combo:02d}.dxbc'
            path.write_bytes(decoded[cursor:cursor+size])
            if fxc:
                subprocess.run([fxc,'/dumpbin','/Fc',str(path.with_suffix('.txt')),str(path)],check=True,capture_output=True)
            report.append({'block':block,'combo':combo,'bytes':size,'file':path.name})
            cursor+=size;combo+=1
    (root/'index.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    return report


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--vcs',required=True);p.add_argument('--metadata',required=True)
    p.add_argument('--dictionary',required=True);p.add_argument('--output',required=True);p.add_argument('--fxc')
    a=p.parse_args();result=extract(a.vcs,a.metadata,a.dictionary,a.output,a.fxc)
    print(f'Extracted {len(result)} DXBC programs into {a.output}')
