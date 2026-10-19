"""Stage 1: a gun's shell as a solid, from its traced side outline (build_body.py <gun.json> <work> [cell mm]).
Each part is its side polygon extruded to its width with rounded edges, as a signed distance field; the parts are
joined with small fillets, then the openings, notches, bores and button sockets are cut. Writes <work>/dense.ply for
../decimate.py and <work>/frame.json (the photo-to-mm mapping and the measured features) for the gun's glb stage.
Working frame (mm): +X toward the muzzle, +Z up, +Y the gun's left side, X=0 at the rearmost point, Z=0 at the butt."""
import sys, json, numpy as np, trimesh
from skimage import measure
o = json.load(open(sys.argv[1])); W = sys.argv[2]; cell = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
P, T, B = o['parts'], o['tubes'], o.get('button')
flip = o.get('muzzle_left', False)   # the photo shows the gun's left side: the muzzle points to -x in pixels

# --- photo pixels -> mm.  The length and height of the real gun set the scale on each axis.
pts = np.vstack([np.array(p['poly'], float) for p in P.values()])
tx = [v for t in T.values() for v in (t['front_px'], t['back_px'])]
x_lo, x_hi = min(pts[:, 0].min(), *tx), max(pts[:, 0].max(), *tx)
body = pts
y_top, y_bot = body[:, 1].min(), body[:, 1].max()
sx = o['size_mm']['length'] / (x_hi - x_lo); sz = o['size_mm']['height'] / (y_bot - y_top)
print('mm per px: x %.4f  z %.4f  (ratio %.3f)' % (sx, sz, sx / sz))
def mmx(x): return (x_hi - np.asarray(x, float)) * sx if flip else (np.asarray(x, float) - x_lo) * sx
def mmz(y): return (y_bot - np.asarray(y, float)) * sz
def mm(p): p = np.asarray(p, float); return np.c_[mmx(p[..., 0]), mmz(p[..., 1])]

# --- grid
L, H, Wd = o['size_mm']['length'], o['size_mm']['height'], o['size_mm']['width']
xs = np.arange(-2.0, L + 2.0, cell); ys = np.arange(-Wd / 2 - 2.0, Wd / 2 + 2.0 + 1e-9, cell); zs = np.arange(-2.0, H + 2.0, cell)
X2, Z2 = np.meshgrid(xs, zs, indexing='ij')
Y3 = ys[None, :, None]; Z3 = zs[None, None, :]

def sdf2(poly):
    """exact signed distance to a polygon (negative inside), on the (x,z) grid"""
    v = mm(poly); q = np.stack([X2, Z2], -1); d = np.full(X2.shape, np.inf); inside = np.zeros(X2.shape, bool)
    for a, b in zip(v, np.roll(v, -1, 0)):
        e = b - a; w = q - a; t = np.clip((w @ e) / (e @ e), 0, 1)
        d = np.minimum(d, np.linalg.norm(w - t[..., None] * e, axis=-1))
        cross = ((a[1] > Z2) != (b[1] > Z2)) & (X2 < (b[0] - a[0]) * (Z2 - a[1]) / (b[1] - a[1] + 1e-12) + a[0])
        inside ^= cross
    return np.where(inside, -d, d)

def rounded(d2, dy, r):
    """2D distance d2 and slab distance dy (both pre-inset by r) -> rounded-edge solid"""
    return np.minimum(np.maximum(d2, dy), 0) + np.hypot(np.maximum(d2, 0), np.maximum(dy, 0)) - r

def extrude(poly, width, r):
    return rounded((sdf2(poly) + r)[:, None, :], np.abs(Y3) - (width / 2 - r), r)

def smin(a, b, k):
    h = np.clip(0.5 + 0.5 * (b - a) / k, 0, 1); return b * (1 - h) + a * h - k * h * (1 - h)
def smax(a, b, k): return -smin(-a, -b, k)

def cyl_x(zc, R, x0, x1, r):
    """cylinder along X (axis at y=0, z=zc) from x0 to x1, edges rounded by r"""
    rc = np.sqrt(Y3 ** 2 + (Z3 - zc) ** 2) - (R - r)
    dx = np.maximum(x0 - xs, xs - x1)[:, None, None] + r
    return rounded(rc, dx, r)

def cyl_y(xc, zc, R, y0, y1, r):
    """cylinder along Y at (xc, zc) between |y| = y0 and y1 (both sides), edges rounded by r"""
    rc = np.sqrt((xs[:, None, None] - xc) ** 2 + (Z3 - zc) ** 2) - (R - r)
    ay = np.abs(Y3); dy = np.maximum(y0 - ay, ay - y1) + r
    return rounded(rc, dy, r)

sd = None
for name, p in P.items():
    if p.get('cut'): continue
    s = extrude(p['poly'], p['width'], p['round'])
    if 'crown_mm' in p:   # round the top across the width: intersect with a cylinder along X under the top
        R = p['crown_mm']; top = mmz(min(q[1] for q in p['poly']))
        s = smax(s, np.sqrt(Y3 ** 2 + (Z3 - (top - R)) ** 2) - R, 1.0)
    if 'notch_width' in p:   # a slot down the middle of the part's top, as in a rear sight
        top = mmz(min(q[1] for q in p['poly'])); px = [q[0] for q in p['poly']]; xa, xb = sorted([mmx(min(px)), mmx(max(px))])
        notch = np.maximum(np.abs(Y3) - p['notch_width'] / 2, (top - p['notch_depth_mm']) - Z3)
        s = np.maximum(s, -np.maximum(notch, np.maximum(xa - 1 - xs, xs - xb - 1)[:, None, None]))
    sd = s if sd is None else smin(sd, s, 0.8)
    print('part', name)

# openings through both sides (the trigger guard)
for name, p in P.items():
    if p.get('cut'): sd = smax(sd, -(sdf2(p['poly'])[:, None, :]), p['round'])

# bore tubes, with the lens recess in their front faces
feat = {'tubes': {}}
for name, t in T.items():
    zc = float(mmz(t['center_y'])); R = t['diameter_px'] / 2 * sz; x0, x1 = sorted([float(mmx(t['back_px'])), float(mmx(t['front_px']))])
    sd = smin(sd, cyl_x(zc, R, x0, x1, t.get('round', 1.0)), 0.8)
    Rb = t['bore_px'] / 2 * sz; dep = t['bore_depth_mm']
    sd = np.maximum(sd, -cyl_x(zc, Rb, x1 - dep, x1 + 5, 0.0))
    feat['tubes'][name] = {'center_z': zc, 'radius': R, 'front_x': x1, 'bore_radius': Rb, 'bore_floor_x': x1 - dep}
    print('tube', name, 'z %.1f  diameter %.1f  bore %.1f' % (zc, 2 * R, 2 * Rb))

# a raised round boss around each side button, and the socket the button sits in
if B:
    hw = P[B['part']]['width'] / 2; bx, bz = mm([B['center']])[0]; Rboss = B['boss_diameter_px'] / 2 * sz; Rbtn = B['diameter_px'] / 2 * sz
    sd = smin(sd, cyl_y(bx, bz, Rboss, hw - 2.0, hw + B['boss_height_mm'], 0.6), 0.5)
    face = hw + B['boss_height_mm']
    sd = np.maximum(sd, -cyl_y(bx, bz, Rbtn + 0.4, face - 2.0, face + 3.0, 0.0))
    feat['button'] = {'x': bx, 'z': bz, 'radius': Rbtn, 'face_y': face, 'socket_floor_y': face - 2.0}

verts, faces, _, _ = measure.marching_cubes(sd, 0.0, spacing=(cell, cell, cell))
verts += np.array([xs[0], ys[0], zs[0]])
m = trimesh.Trimesh(verts, faces[:, ::-1], process=True)
if m.volume < 0: m.invert()
print('dense faces', len(m.faces), 'bounds', np.round(m.bounds, 1).tolist())
m.export(f'{W}/dense.ply')

# features for the glb stage, in the working frame
grip = mm(P['grip']['poly'])
def grip_mid(z):
    """middle of the grip's side outline at height z"""
    xs_ = []
    for a, b in zip(grip, np.roll(grip, -1, 0)):
        if (a[1] - z) * (b[1] - z) < 0: xs_.append(a[0] + (b[0] - a[0]) * (z - a[1]) / (b[1] - a[1]))
    return (min(xs_) + max(xs_)) / 2
ga = o['grip_axis']; hs = [ga['centre_mm']] + ga['rake_mm']
feat.update({'px_to_mm': {'flip': flip, 'x_lo': x_lo, 'x_hi': x_hi, 'y_bot': y_bot, 'sx': sx, 'sz': sz},
             'grip_centre': [grip_mid(ga['centre_mm']), ga['centre_mm']], 'grip_rake': [[grip_mid(h), h] for h in ga['rake_mm']],
             'trigger': {'pivot': mm([o['trigger']['pivot']])[0].tolist(), 'poly': mm(o['trigger']['poly']).tolist(), 'width': o['trigger']['width']}})
json.dump(feat, open(f'{W}/frame.json', 'w'), indent=1)
