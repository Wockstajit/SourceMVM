// Surface normals reconstructed from depth, shared by the D3D11 and D3D9 pass adapters.
float3 Normal(AFX_PASS_DEPTH_TYPE t, int2 px, out bool valid)
{
	float3 c, l, r, u, d;
	valid = WorldPos(px, LoadRaw(t, px), c);
	bool vl = WorldPos(px + int2(-1, 0), LoadRaw(t, px + int2(-1, 0)), l);
	bool vr = WorldPos(px + int2( 1, 0), LoadRaw(t, px + int2( 1, 0)), r);
	bool vu = WorldPos(px + int2(0, -1), LoadRaw(t, px + int2(0, -1)), u);
	bool vd = WorldPos(px + int2(0,  1), LoadRaw(t, px + int2(0,  1)), d);
	if (!valid) return float3(0, 0, 0);
	// The neighbour on the same surface is the one closer in depth: taking it on each axis keeps
	// silhouettes from smearing the foreground normal into the background.
	float dc = dot(c - camera.xyz, forward.xyz);
	float el = vl ? abs(dc - dot(l - camera.xyz, forward.xyz)) : 1e30;
	float er = vr ? abs(dot(r - camera.xyz, forward.xyz) - dc) : 1e30;
	float eu = vu ? abs(dc - dot(u - camera.xyz, forward.xyz)) : 1e30;
	float ed = vd ? abs(dot(d - camera.xyz, forward.xyz) - dc) : 1e30;
	if (min(el, er) > 1e29 || min(eu, ed) > 1e29) { valid = false; return float3(0, 0, 0); }
	float3 dx = er < el ? (r - c) : (c - l);
	float3 dy = ed < eu ? (d - c) : (c - u);
	float3 n = cross(dx, dy);
	if (dot(n, n) < 1e-20) { valid = false; return float3(0, 0, 0); }
	n = normalize(n);
	if (dot(n, camera.xyz - c) < 0.0) n = -n; // face the camera
	// World -> view axes: +X right, +Y up, +Z toward the camera.
	return float3(dot(n, right.xyz), dot(n, up.xyz), -dot(n, forward.xyz));
}

// Depth only gives the FACE normal of each triangle, so a character came out as a faceted low-poly
// mesh. The game shades with interpolated vertex normals, which this approximates: average the
// face normals of nearby pixels that lie on the same smooth surface -- close to this pixel's
// tangent plane and within ~45 degrees of its normal. Real creases (wall corners, the edge of a
// crate) and silhouettes fail those tests and stay sharp. Texture normal-map detail is not
// recoverable: CS2's forward renderer exposes no normal buffer.
float3 SmoothNormal(AFX_PASS_DEPTH_TYPE t, int2 px, out bool valid)
{
	float3 nc = Normal(t, px, valid);
	if (!valid) return nc;
	float3 pc;
	WorldPos(px, LoadRaw(t, px), pc);
	float dist = dot(pc - camera.xyz, forward.xyz);
	float tol = 0.5 + dist * 0.01;
	float3 sum = nc;
	[loop] for (int y = -4; y <= 4; y += 2) {
		[loop] for (int x = -4; x <= 4; x += 2) {
			if (x == 0 && y == 0) continue;
			int2 q = px + int2(x, y);
			bool vq;
			float3 nq = Normal(t, q, vq);
			if (!vq) continue;
			float3 pq;
			if (!WorldPos(q, LoadRaw(t, q), pq)) continue;
			float3 d = pq - pc;
			float3 dv = float3(dot(d, right.xyz), dot(d, up.xyz), -dot(d, forward.xyz));
			float plane = abs(dot(dv, nc)) / tol;
			float w = exp(-plane * plane) * smoothstep(0.7, 0.9, dot(nq, nc));
			sum += nq * w;
		}
	}
	return normalize(sum);
}
