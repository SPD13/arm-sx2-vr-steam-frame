"""Stage 2: build both LightGun 2 shells from the right one. The right shell's scan is much cleaner than the left one's, and
the two sides differ only by things the model does not take from the scan (the left side's SELECT/START holes and
markings), so the left shell is the right one mirrored across the parting plane.
The scan lies outer face up (+Z) on the scanner bed; it is levelled on a plane fitted to its rim (the 2 mm band inside
its outline, seen from below) and mapped to the working frame (+X muzzle, +Z up, +Y the gun's left, rim at y=0). The
rims meet flush (tongue and groove, the cross-sections of the two scans are symmetric), so unlike the PS1 gun there is
no lip offset (align.py <work>)."""
import sys, numpy as np
from scipy import ndimage
W = sys.argv[1]; res = 0.5

def rim_plane(v):
    lo = v[:, :2].min(0) - 2; ij = np.floor((v[:, :2] - lo) / res).astype(int); N = ij.max(0) + 3
    zmin = np.full(N, np.inf); np.minimum.at(zmin, (ij[:, 0], ij[:, 1]), v[:, 2])
    full = ndimage.binary_fill_holes(ndimage.binary_closing(np.isfinite(zmin), iterations=2))
    ring = full & ~ndimage.binary_erosion(full, iterations=4)
    pts = np.argwhere(ring & np.isfinite(zmin)); z = zmin[pts[:, 0], pts[:, 1]]; xy = pts * res + lo
    A = np.c_[xy, np.ones(len(z))]; keep = np.ones(len(z), bool)
    for _ in range(6):  # robust: drop the screw posts and pockets
        c, *_ = np.linalg.lstsq(A[keep], z[keep], rcond=None); r = z - A @ c
        keep = np.abs(r) < max(0.3, 2.5 * np.median(np.abs(r[keep])))
    print('rim plane z = %.5f x + %.5f y + %.3f' % tuple(c), 'inliers %.2f' % keep.mean())
    return c

def level(v):
    a, b, d = rim_plane(v); n = np.array([-a, -b, 1.]); n /= np.linalg.norm(n)
    k = np.cross(n, [0, 0, 1.]); s = np.linalg.norm(k); co = n[2]; k /= s
    K = np.array([[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]]); Rm = np.eye(3) + s * K + (1 - co) * K @ K
    p0 = np.array([v[:, 0].mean(), v[:, 1].mean(), 0]); p0[2] = a * p0[0] + b * p0[1] + d
    return (v - p0) @ Rm.T

R = level(np.load(f'{W}/Right.npz')['v'].astype(float))
R = np.c_[R[:, 0], -R[:, 2], R[:, 1]]   # scan +Z is the gun's right, scan +Y is up
ctr = (R.min(0) + R.max(0)) / 2; ctr[1] = 0; R -= ctr   # origin at the x/z bbox centre
L = R * np.array([1, -1, 1])
print('bbox', np.minimum(L.min(0), R.min(0)), np.maximum(L.max(0), R.max(0)))
np.save(f'{W}/L_v.npy', L.astype(np.float32)); np.save(f'{W}/R_aligned_v.npy', R.astype(np.float32))
