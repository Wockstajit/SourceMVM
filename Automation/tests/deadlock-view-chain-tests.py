"""Exercise the offline camera gate with small synthetic RTTI images."""
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location(
    "verifier", Path(__file__).parents[1] / "verify" / "verify-deadlock-signatures.py")
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)


class Image:
    image_base = 0x180000000

    def __init__(self):
        self.data = bytearray(0x3000)
        self.sections = [dict(name=".text", vaddr=0x100, vsize=0x800, raddr=0x100, rsize=0x800),
                         dict(name=".rdata", vaddr=0x1000, vsize=0x1000, raddr=0x1000, rsize=0x1000)]
        for name, td, col, vt in [(b".?AVCViewRender@@", 0x2400, 0x1200, 0x1400),
                                   (b".?AVClientModeCitadelNormal@@", 0x2600, 0x1300, 0x1600)]:
            self.data[td + 16:td + 17 + len(name)] = name + b"\0"
            struct.pack_into("<6I", self.data, col, 1, 0, 0, td, 0x1100, col)
            self.pointer(vt - 8, col)
            self.pointer(vt, 0x180)
        self.pointer(0x1400 + 4 * 8, 0x200)
        self.pointer(0x1400 + 17 * 8, 0x300)
        self.pointer(0x1600 + 15 * 8, 0x400)
        self.put(0x200, '48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 '
                       '48 81 EC B0 00 00 00 48 8B 01 4C 8B F1 FF 90 88 00 00 00')
        self.put(0x330, '48 8B C8 48 8B 10 4C 8B 42 78 48 8D 56 10 41 FF D0')

    def pointer(self, at, rva):
        struct.pack_into('<Q', self.data, at, self.image_base + rva)

    def put(self, at, hex_bytes):
        raw = bytes.fromhex(hex_bytes)
        self.data[at:at + len(raw)] = raw

    def section(self, name):
        return next((s for s in self.sections if s['name'] == name), None)

    def raw(self, name):
        s = self.section(name)
        return self.data[s['raddr']:s['raddr'] + s['rsize']]

    def rva_to_off(self, rva):
        return rva if 0 <= rva < len(self.data) else None

    off_to_rva = rva_to_off


class CameraGateTests(unittest.TestCase):
    def test_valid_chain(self):
        self.assertEqual(verifier.check_view_chain(Image(), 0x400), (0x1400, 0x300, 0x341))

    def test_wrong_mode_target(self):
        self.assertIsNone(verifier.check_view_chain(Image(), 0x410))

    def test_unrelated_class_with_matching_slots(self):
        pe = Image()
        pe.data[0x2410] = ord('X')
        self.assertIsNone(verifier.check_view_chain(pe, 0x400))

    def test_invalid_or_secondary_locator(self):
        for offset, value in [(0, 0), (4, 8), (20, 0x1234)]:
            pe = Image()
            struct.pack_into('<I', pe.data, 0x1200 + offset, value)
            self.assertIsNone(verifier.check_view_chain(pe, 0x400))

    def test_ambiguous_primary_table(self):
        pe = Image()
        pe.pointer(0x18f8, 0x1200)
        pe.pointer(0x1900, 0x180)
        self.assertIsNone(verifier.check_view_chain(pe, 0x400))

    def test_wrong_embedded_view_displacement(self):
        pe = Image()
        pe.data[0x33d] = 0x18
        self.assertIsNone(verifier.check_view_chain(pe, 0x400))

    def test_wrong_matrix_dispatch_slot(self):
        pe = Image()
        pe.data[0x220] = 0x80
        self.assertIsNone(verifier.check_view_chain(pe, 0x400))


if __name__ == '__main__':
    unittest.main()
