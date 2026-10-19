"""Stage 1: read a binary STL shell scan, weld its vertices, store compressed numpy (load_stl.py <scan.stl> <out.npz>)."""
import sys, numpy as np
src, dst = sys.argv[1], sys.argv[2]
d = np.fromfile(src, dtype=np.uint8); n = int(np.frombuffer(d[80:84].tobytes(), np.uint32)[0])
rec = np.dtype([('n', '<f4', 3), ('v', '<f4', (3, 3)), ('a', '<u2')])
tri = np.frombuffer(d[84:84 + n * 50].tobytes(), rec)['v'].reshape(-1, 3)
v, inv = np.unique(tri, axis=0, return_inverse=True)
np.savez(dst, v=v.astype(np.float32), f=inv.reshape(-1, 3).astype(np.int32))
print(src, n, 'triangles', len(v), 'vertices', 'bbox', v.min(0), v.max(0))
