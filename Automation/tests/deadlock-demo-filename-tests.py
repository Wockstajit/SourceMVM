"""Verify the filename chain rejects the recorder getter and shifted layouts."""
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location(
    "camera_fixture", Path(__file__).with_name("deadlock-view-chain-tests.py"))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
verifier = fixture.verifier


def image():
    pe = fixture.Image()
    for at, name in [(0x2410, b".?AVCEngineClient@@"), (0x2610, b".?AVCDemoFile@@")]:
        pe.data[at:at + len(name) + 1] = name + b"\0"
    pe.pointer(0x1400 + 43 * 8, 0x400)
    pe.pointer(0x1800 + 1 * 8, 0x500)
    pe.pointer(0x1600 + 3 * 8, 0x600)
    pe.put(0x400, "48 83 EC 28 48 83 3D 00 00 00 00 00 74 2C 48 8B 0D 00 00 00 00 "
                  "48 8B 01 FF 50 58 84 C0 74 1B 48 8B 0D 00 00 00 00 48 8B 01 FF 50 08 "
                  "48 8B C8 48 8B 10 48 83 C4 28 48 FF 62 18 33 C0 48 83 C4 28 C3")
    struct.pack_into('<i', pe.data, 0x411, 0x2200 - 0x415)
    struct.pack_into('<i', pe.data, 0x422, 0x2200 - 0x426)
    pe.put(0x500, "48 8D 41 20 C3")
    pe.put(0x600, "48 8B 51 08 48 8D 05 00 00 00 00 48 85 D2 48 0F 45 C2 C3")
    return pe


class FilenameChainTests(unittest.TestCase):
    def test_filename_chain(self):
        self.assertEqual(verifier.check_demo_filename_chain(image(), 0x1800), (0x400, 0x500, 0x600))

    def test_recorder_slot_is_not_a_filename(self):
        pe = image()
        pe.data[0x439] = 0x20  # tail call slot4 instead of slot3
        self.assertIsNone(verifier.check_demo_filename_chain(pe, 0x1800))

    def test_different_player_globals(self):
        pe = image()
        pe.data[0x422] ^= 8
        self.assertIsNone(verifier.check_demo_filename_chain(pe, 0x1800))

    def test_wrong_embedded_file(self):
        pe = image()
        pe.data[0x503] = 0x28
        self.assertIsNone(verifier.check_demo_filename_chain(pe, 0x1800))

    def test_wrong_filename_member(self):
        pe = image()
        pe.data[0x603] = 0x10
        self.assertIsNone(verifier.check_demo_filename_chain(pe, 0x1800))

    def test_wrong_file_class(self):
        pe = image()
        pe.data[0x2610] = ord('X')
        self.assertIsNone(verifier.check_demo_filename_chain(pe, 0x1800))


if __name__ == '__main__':
    unittest.main()
