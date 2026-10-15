"""Behavior checks for collision-based lighting camera preflight."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("interior_camera", Path(__file__).parents[1] / "verify/interior-camera.py")
interior = importlib.util.module_from_spec(spec)
spec.loader.exec_module(interior)


class InteriorCameraTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        vertices = [(0,0,0),(256,0,0),(256,256,0),(0,256,0),
                    (0,0,128),(256,0,128),(256,256,128),(0,256,128)]
        # Inward-facing floor, ceiling and four walls.
        indices = [0,1,2,0,2,3,4,6,5,4,7,6,0,4,5,0,5,1,
                   1,5,6,1,6,2,2,6,7,2,7,3,3,7,4,3,4,0]
        raw = (struct.pack("<8sII6f", b"MVMOCC01", 8, len(indices), 0,0,0,256,256,128)
               + struct.pack("<24f", *(x for p in vertices for x in p))
               + struct.pack("<36I", *indices))
        path = Path(self.temp.name) / "room.mvmocclusion"
        path.write_bytes(interior.mvm_cache.encode(raw))
        self.raw = raw
        self.path = path
        self.mesh = interior.Mesh(path)

    def tearDown(self):
        self.temp.cleanup()

    def test_focused_validation_preserves_room_and_rejection_results(self):
        for eye in ([128,128,66], [18,128,66], [-16,128,66], [8000,8000,66]):
            with self.subTest(eye=eye):
                focused = interior.Mesh(self.path, focus=eye)
                self.assertEqual(focused.check(eye), self.mesh.check(eye))

    def test_focused_validation_keeps_spanning_floor_triangles(self):
        raw = bytearray(self.raw)
        struct.pack_into('<6f', raw, 16, 0,0,0,2048,2048,128)
        for vertex in range(8):
            x,y,z = struct.unpack_from('<3f', raw, 40 + vertex * 12)
            struct.pack_into('<3f', raw, 40 + vertex * 12, x*8,y*8,z)
        path = Path(self.temp.name) / 'spanning.mvmocclusion'
        path.write_bytes(interior.mvm_cache.encode(bytes(raw)))
        full, focused = interior.Mesh(path), interior.Mesh(path, focus=[1024,1024,66])
        result = focused.check([1024,1024,66])
        self.assertEqual(result, full.check([1024,1024,66]))
        self.assertEqual(result['floorDistance'], 66)
        self.assertEqual(result['ceilingDistance'], 62)

    def test_room_accepts_a_full_height_camera(self):
        result = self.mesh.check([128,128,66])
        self.assertTrue(result["geometryValid"])
        self.assertEqual(result["floorDistance"], 66)
        self.assertEqual(result["ceilingDistance"], 62)
        self.assertEqual(result["visualReview"], "pending")

    def test_outside_and_intersecting_player_poses_are_rejected(self):
        for eye in ([18,128,66], [128,128,56], [128,128,-2],
                    [128,128,130], [-16,128,66], [128,128,110]):
            with self.subTest(eye=eye):
                self.assertFalse(self.mesh.check(eye)["geometryValid"])

    def test_spatial_bins_preserve_hits_at_the_bin_boundary(self):
        hit = self.mesh.ray([256,128,66], [0,0,-1])
        self.assertIsNotNone(hit)
        self.assertAlmostEqual(hit[0], 66)

    def test_overhang_without_surrounding_walls_is_rejected(self):
        self.mesh.bins = {key: rows[rows < 4] for key, rows in self.mesh.bins.items()}
        self.mesh.large = self.mesh.large[self.mesh.large < 4]
        result = self.mesh.check([128,128,66])
        self.assertFalse(result["geometryValid"])
        self.assertEqual(result["enclosingDirections"], 0)

    def test_full_player_footprint_cannot_straddle_a_floor_ledge(self):
        # Shorten only the floor, leaving the room walls and ceiling intact.
        self.mesh.e1[:2,0] *= 138 / 256
        self.mesh.e2[:2,0] *= 138 / 256
        self.mesh.high[:2,0] = 138
        result = self.mesh.check([128,128,66])
        self.assertFalse(result["geometryValid"])
        self.assertIn("player footprint crosses a floor or ceiling boundary", result["errors"])

    def test_bounds_without_a_roof_are_not_an_interior(self):
        self.mesh.bins = {key: rows[rows < 2] for key, rows in self.mesh.bins.items()}
        self.mesh.large = self.mesh.large[self.mesh.large < 2]
        self.assertFalse(self.mesh.check([128,128,66])["geometryValid"])


if __name__ == "__main__":
    unittest.main()
