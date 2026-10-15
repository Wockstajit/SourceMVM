"""The map data cache's compressed payload container (.mvmocclusion, .mvmcasters).

Same format as AfxHookSource2/Filmmaker/MapData/MapCacheCodec.h: "MVMZXPH1", u64 raw size,
u32 block size, u32 block count, u32 stored size per block (bit 31 = stored uncompressed), then the
blocks. Blocks are Windows XPRESS-Huffman in raw mode, decoded through cabinet.dll.
"""
import ctypes
import struct
from pathlib import Path

MAGIC = b'MVMZXPH1'
BLOCK_SIZE = 4 << 20
_STORED = 0x80000000
_ALGORITHM = 4 | 0x20000000  # COMPRESS_ALGORITHM_XPRESS_HUFF | COMPRESS_RAW

_cabinet = None


def _api():
    global _cabinet
    if _cabinet is None:
        _cabinet = ctypes.WinDLL('cabinet')
        for name in ('CreateCompressor', 'CreateDecompressor'):
            getattr(_cabinet, name).argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
        for name in ('Compress', 'Decompress'):
            getattr(_cabinet, name).argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p,
                                                ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        for name in ('CloseCompressor', 'CloseDecompressor'):
            getattr(_cabinet, name).argtypes = [ctypes.c_void_p]
    return _cabinet


def decode(data):
    """Container bytes -> raw payload bytes."""
    if data[:8] != MAGIC:
        raise ValueError('not a compressed map data payload')
    raw_size, block_size, blocks = struct.unpack_from('<QII', data, 8)
    table = struct.unpack_from('<%dI' % blocks, data, 24)
    api = _api()
    handle = ctypes.c_void_p()
    if not api.CreateDecompressor(_ALGORITHM, None, ctypes.byref(handle)):
        raise OSError('cannot create the cache decompressor')
    try:
        out = bytearray(raw_size)
        at, done = 24 + 4 * blocks, 0
        buffer = (ctypes.c_char * block_size)()
        got = ctypes.c_size_t()
        for entry in table:
            size, want = entry & ~_STORED, min(block_size, raw_size - done)
            chunk = data[at:at + size]
            if entry & _STORED:
                if size != want:
                    raise ValueError('bad stored block')
                out[done:done + want] = chunk
            else:
                if not api.Decompress(handle, chunk, size, buffer, want, ctypes.byref(got)) or got.value != want:
                    raise ValueError('cannot decode a map data block')
                out[done:done + want] = buffer.raw[:want]
            at, done = at + size, done + want
        if done != raw_size:
            raise ValueError('truncated map data payload')
        return bytes(out)
    finally:
        api.CloseDecompressor(handle)


def encode(raw):
    """Raw payload bytes -> container bytes (the hook writes the same layout)."""
    api = _api()
    handle = ctypes.c_void_p()
    if not api.CreateCompressor(_ALGORITHM, None, ctypes.byref(handle)):
        raise OSError('cannot create the cache compressor')
    try:
        blocks = (len(raw) + BLOCK_SIZE - 1) // BLOCK_SIZE
        table, body = [], []
        buffer = (ctypes.c_char * BLOCK_SIZE)()
        written = ctypes.c_size_t()
        for b in range(blocks):
            chunk = bytes(raw[b * BLOCK_SIZE:(b + 1) * BLOCK_SIZE])
            if api.Compress(handle, chunk, len(chunk), buffer, len(chunk), ctypes.byref(written)) and 0 < written.value < len(chunk):
                table.append(written.value)
                body.append(buffer.raw[:written.value])
            else:
                table.append(len(chunk) | _STORED)
                body.append(chunk)
        return MAGIC + struct.pack('<QII', len(raw), BLOCK_SIZE, blocks) + struct.pack('<%dI' % blocks, *table) + b''.join(body)
    finally:
        api.CloseCompressor(handle)


def read_payload(path):
    """The raw bytes of a cache payload file."""
    return decode(Path(path).read_bytes())
