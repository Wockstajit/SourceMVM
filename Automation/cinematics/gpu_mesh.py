"""GPU (NVIDIA Warp, CUDA) queries against the map collision for shotplan.py.

One BVH per mesh; every query is batched, so a whole shot's clearance / inside-solid / line-of-sight
work is a handful of kernel launches instead of thousands of Python-level ray tests. shotplan falls
back to its CPU path when Warp or a CUDA device is unavailable.
"""
import numpy as np

try:
    import warp as wp
    wp.config.quiet = True
    wp.init()
    DEVICE = 'cuda:0' if wp.get_cuda_device_count() > 0 else None
except Exception:  # noqa: BLE001 - any import/driver failure means "no GPU path"
    wp = None
    DEVICE = None

AVAILABLE = DEVICE is not None

if AVAILABLE:
    @wp.kernel
    def _k_rays(mesh: wp.uint64, o: wp.array(dtype=wp.vec3), d: wp.array(dtype=wp.vec3),
                tmax: wp.array(dtype=float), t_out: wp.array(dtype=float), f_out: wp.array(dtype=int)):
        i = wp.tid()
        q = wp.mesh_query_ray(mesh, o[i], d[i], tmax[i])
        if q.result:
            t_out[i] = q.t
            f_out[i] = q.face
        else:
            t_out[i] = 1.0e30
            f_out[i] = -1

    @wp.kernel
    def _k_points(mesh: wp.uint64, p: wp.array(dtype=wp.vec3), maxd: float,
                  dist_out: wp.array(dtype=float), inside_out: wp.array(dtype=int),
                  cp_out: wp.array(dtype=wp.vec3)):
        i = wp.tid()
        q = wp.mesh_query_point_sign_normal(mesh, p[i], maxd)
        if q.result:
            cp = wp.mesh_eval_position(mesh, q.face, q.u, q.v)
            dist_out[i] = wp.length(cp - p[i])
            cp_out[i] = cp
            if q.sign < 0.0:
                inside_out[i] = 1
            else:
                inside_out[i] = 0
        else:
            dist_out[i] = maxd
            inside_out[i] = 0
            cp_out[i] = p[i]

    @wp.kernel
    def _k_image(mesh: wp.uint64, cam: wp.vec3, fwd: wp.vec3, right: wp.vec3, up: wp.vec3,
                 th: float, tv: float, w: int, h: int, far: float,
                 t_out: wp.array(dtype=float), f_out: wp.array(dtype=int)):
        i = wp.tid()
        x = i % w
        y = i // w
        nx = (2.0 * (float(x) + 0.5) / float(w) - 1.0) * th
        ny = (1.0 - 2.0 * (float(y) + 0.5) / float(h)) * tv
        d = wp.normalize(fwd + right * nx + up * ny)
        q = wp.mesh_query_ray(mesh, cam, d, far)
        if q.result:
            t_out[i] = q.t * wp.dot(d, fwd)   # planar depth
            f_out[i] = q.face
        else:
            t_out[i] = 1.0e30
            f_out[i] = -1


class GpuMesh:
    def __init__(self, tris):
        tris = np.ascontiguousarray(tris, dtype=np.float32)
        pts = tris.reshape(-1, 3)
        idx = np.arange(len(pts), dtype=np.int32)
        self.points = wp.array(pts, dtype=wp.vec3, device=DEVICE)
        self.indices = wp.array(idx, dtype=int, device=DEVICE)
        self.mesh = wp.Mesh(points=self.points, indices=self.indices)

    def rays(self, origins, dirs, tmax):
        """Nearest hit per ray: (t, face), t = inf on a miss. dirs must be unit length."""
        origins = np.ascontiguousarray(np.atleast_2d(origins), np.float32)
        dirs = np.ascontiguousarray(np.atleast_2d(dirs), np.float32)
        n = len(origins)
        if n == 0:
            return np.zeros(0), np.zeros(0, int)
        tm = np.broadcast_to(np.asarray(tmax, np.float32), (n,)).copy()
        t = wp.empty(n, dtype=float, device=DEVICE)
        f = wp.empty(n, dtype=int, device=DEVICE)
        wp.launch(_k_rays, dim=n, inputs=[self.mesh.id, wp.array(origins, dtype=wp.vec3, device=DEVICE),
                                          wp.array(dirs, dtype=wp.vec3, device=DEVICE),
                                          wp.array(tm, dtype=float, device=DEVICE), t, f], device=DEVICE)
        t = t.numpy().astype(np.float64)
        t[t > 1e29] = np.inf
        return t, f.numpy()

    def segments(self, a, b):
        """First hit distance from a toward b for each pair (inf = clear line)."""
        a = np.atleast_2d(np.asarray(a, np.float64))
        b = np.atleast_2d(np.asarray(b, np.float64))
        d = b - a
        L = np.linalg.norm(d, axis=1)
        ok = L > 1e-6
        dirs = np.where(ok[:, None], d / np.where(ok, L, 1)[:, None], [1, 0, 0])
        t, _ = self.rays(a, dirs, np.maximum(L, 1e-3))
        t[~ok] = np.inf
        return t

    def points_query(self, p, maxd):
        """(distance to nearest surface capped at maxd, inside-solid flag, closest point) per point."""
        p = np.ascontiguousarray(np.atleast_2d(p), np.float32)
        n = len(p)
        dist = wp.empty(n, dtype=float, device=DEVICE)
        inside = wp.empty(n, dtype=int, device=DEVICE)
        cp = wp.empty(n, dtype=wp.vec3, device=DEVICE)
        wp.launch(_k_points, dim=n, inputs=[self.mesh.id, wp.array(p, dtype=wp.vec3, device=DEVICE), float(maxd),
                                            dist, inside, cp], device=DEVICE)
        return dist.numpy().astype(np.float64), inside.numpy().astype(bool), cp.numpy().astype(np.float64)

    def image(self, cam, fwd, right, up, th, tv, w, h, far):
        """Ray-traced planar depth + face index per pixel (row-major, h x w)."""
        n = w * h
        t = wp.empty(n, dtype=float, device=DEVICE)
        f = wp.empty(n, dtype=int, device=DEVICE)
        wp.launch(_k_image, dim=n, inputs=[self.mesh.id, wp.vec3(*map(float, cam)), wp.vec3(*map(float, fwd)),
                                           wp.vec3(*map(float, right)), wp.vec3(*map(float, up)),
                                           float(th), float(tv), int(w), int(h), float(far), t, f], device=DEVICE)
        return t.numpy().reshape(h, w), f.numpy().reshape(h, w)
