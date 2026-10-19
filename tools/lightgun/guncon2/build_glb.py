"""Assemble the low-poly GunCon2 and write its .glb (build_glb.py <work> <out.glb>).
Working frame (mm): +X toward the muzzle, +Z up, +Y the gun's left side. Same node contract as the PS1 gun, plus the
GunCon2's extra controls; see README.md."""
import sys, json, struct, numpy as np, trimesh, mapbox_earcut
from scipy import ndimage
W, OUT = sys.argv[1], sys.argv[2]
body = trimesh.load(f'{W}/low_2600.ply', process=True)
F = np.load(f'{W}/fields.npz'); X, Y, Z = F['X'], F['Y'], F['Z']

def hit(o, d):
    """first point of the body along ray o + t d"""
    loc, _, _ = body.ray.intersects_location(np.array([o], float), np.array([d], float))
    return loc[np.argmin(((loc - o) ** 2).sum(1))]

# --- front openings, read off the front height field: the upper bore is a ~3.5 mm deep round recess (r~9.5), the
# lower one a small round hole (r~3) in the lower housing
upper = np.array([0, 0.0, 55.3]); lower = np.array([0, 0.0, 13.3])
upper[0] = hit([200, upper[1], upper[2] + 10.5], [-1, 0, 0])[0]   # front face just above the bore
lower[0] = hit([200, lower[1], lower[2] + 4.5], [-1, 0, 0])[0]
muzzle = upper; yc = 0.0  # the parting plane: the shells are mirror images
fn = body.face_normals; fc = body.triangles_center
ryz = lambda c: np.hypot(fc[:, 1] - c[1], fc[:, 2] - c[2])
recess = lambda c, lo, hi: (fc[:, 0] < c[0] - lo) & (fc[:, 0] > c[0] - hi)   # only the floor of each opening
lens = (fn[:, 0] > 0.8) & ((recess(upper, 1.5, 8) & (ryz(upper) < 9.0)) | (recess(lower, 1.0, 6) & (ryz(lower) < 3.2)))
print('front faces', upper[0], lower[0], 'lens faces', lens.sum())

def extrude(profile, y0, y1):
    """closed prism from a CCW (x,z) polygon, between y0 and y1"""
    p = np.asarray(profile, float); n = len(p)
    v = np.vstack([np.c_[p[:, 0], np.full(n, y1), p[:, 1]], np.c_[p[:, 0], np.full(n, y0), p[:, 1]]])
    tri = mapbox_earcut.triangulate_float64(p, np.array([n], np.uint32)).reshape(-1, 3)
    faces = [t for t in tri] + [t[::-1] + n for t in tri]
    for i in range(n):
        j = (i + 1) % n; faces += [[i, n + i, n + j], [i, n + j, j]]
    m = trimesh.Trimesh(v, faces, process=True); m.fix_normals(); return m

# --- trigger: the PS1 gun's curved blade, placed the same way in the GunCon2's guard opening (x -45.2..3.2,
# z -3.8..20.2: 4.0 mm further forward and 5.0 mm higher), pivot at the top
dx, dz = 4.0, 5.0
trig_prof = [(-14.5, 19), (-14.2, 12), (-13.2, 7.5), (-11.4, 3.5), (-9.0, 0.4), (-7.6, -2.4), (-8.4, -4.4),
             (-10.6, -4.6), (-13.4, -2.4), (-16.6, 1.0), (-19.0, 5.0), (-20.6, 10), (-21.4, 14), (-21.6, 19)][::-1]
trigger = extrude([(x + dx, z + dz) for x, z in trig_prof], yc - 4.0, yc + 4.0)
trig_pivot = np.array([-18.0 + dx, yc, 17.0 + dz])

# --- buttons: an outline extruded along the surface normal, with a chamfered top
def stadium(L, H, seg=6):
    r = H / 2; c = L / 2 - r; a = np.linspace(-np.pi / 2, np.pi / 2, seg + 1)
    return np.vstack([np.c_[c + r * np.cos(a), r * np.sin(a)], np.c_[-c - r * np.cos(a), -r * np.sin(a)]])
def circle(r, seg=16):
    a = np.linspace(0, 2 * np.pi, seg, endpoint=False); return np.c_[r * np.cos(a), r * np.sin(a)]
def rrect(L, H, r, seg=3):
    pts = []
    for cx, cy, a0 in ((L / 2 - r, H / 2 - r, 0), (-L / 2 + r, H / 2 - r, 90), (-L / 2 + r, -H / 2 + r, 180), (L / 2 - r, -H / 2 + r, 270)):
        a = np.radians(np.linspace(a0, a0 + 90, seg + 1)); pts += list(np.c_[cx + r * np.cos(a), cy + r * np.sin(a)])
    return np.array(pts)
def cross(wh, hh, wv, hv, cv=0.0):
    """plus sign: horizontal arm wh x hh centred at 0, vertical arm wv x hv centred at cv (CCW)"""
    a, b, c, d = wh / 2, hh / 2, wv / 2, hv / 2
    return np.array([(a, -b), (a, b), (c, b), (c, cv + d), (-c, cv + d), (-c, b), (-a, b), (-a, -b), (-c, -b), (-c, cv - d), (c, cv - d), (c, -b)])

def cap(outline, inset, origin, e1, e2, n, top, depth=2.5):
    """button from a CCW outline (local e1/e2 mm); its top `top` mm above origin along n, chamfered by `inset`"""
    o = np.asarray(outline, float); k = len(o); c = o.mean(0)
    s = 1 - inset / np.linalg.norm(o - c, axis=1).min()   # shrink toward the centre for the chamfered top
    rings = [(o, -depth), (o, top - 0.6), (c + (o - c) * s, top)]
    pt = lambda p, h: origin + p[:, :1] * e1 + p[:, 1:] * e2 + h * n
    v = np.vstack([pt(p, h) for p, h in rings]); f = []
    for r in range(len(rings) - 1):
        for i in range(k):
            j = (i + 1) % k; a0, a1, b0, b1 = r * k + i, r * k + j, (r + 1) * k + i, (r + 1) * k + j
            f += [[a0, a1, b1], [a0, b1, b0]]
    tri = mapbox_earcut.triangulate_float64(o, np.array([k], np.uint32)).reshape(-1, 3)
    f += [t[::-1] for t in tri] + [t + (len(rings) - 1) * k for t in tri]
    m = trimesh.Trimesh(v, f, process=True); m.fix_normals(); return m

ex, ey, ez = np.eye(3)
def side_seat(cx, cz, side):
    """surface point on the gun's left (side=+1) or right (-1) face at x=cx, z=cz"""
    return hit([cx, side * 40.0, cz], [0, -side, 0])

# A and B: the oval buttons above the trigger, through the same 21 x 12 mm opening on both sides
ab_c = (-11.87, 38.22); ab_out = stadium(20.2, 11.2)
seatL, seatR = side_seat(*ab_c, +1), side_seat(*ab_c, -1)
btnA = cap(ab_out, 0.7, seatL, ex, ez, ey, 1.4)
btnB = cap(ab_out, 0.7, seatR, -ex, ez, -ey, 1.4)
# SELECT and START: the two small round buttons on the left side (r 3.7 holes)
seatSel, seatSt = side_seat(33.71, 30.38, +1), side_seat(19.64, 30.30, +1)
btnSel = cap(circle(3.3), 0.5, seatSel, ex, ez, ey, 1.0)
btnSt = cap(circle(3.3), 0.5, seatSt, ex, ez, ey, 1.0)
# D-pad: cross opening in the raked rear face (x = 0.565 z - 90.93); arms 21.5 x 5.5 and 7.5 x 18.6 (as projected)
n_d = np.array([-1, 0, 0.565]); n_d /= np.linalg.norm(n_d); u_d = np.cross(n_d, -ey)   # u_d: up along the face
dpad_c = np.array([0.565 * 41.5 - 90.93, 0.0, 41.5]); kz = 1 / u_d[2]   # horizontal arm centre; projected z -> along the face
dpad = cap(cross(21.0, 5.1 * kz, 7.0, 18.2 * kz, 0.5 * kz), 0.6, dpad_c, -ey, u_d, n_d, 1.6, 3.5)
# C: slot on the 45 degree chamfer at the heel of the grip, 11 x 4.8 mm
n_c = np.array([-1, 0, -1]) / np.sqrt(2); u_c = np.array([-1, 0, 1]) / np.sqrt(2)
seatC = np.array([-107.4, 0.0, -71.9])   # on the chamfer plane, measured beside the slot
btnC = cap(rrect(10.4, 4.2, 1.5), 0.5, seatC, ey, u_c, n_c, 1.0)
print('seats A', seatL.round(2), 'B', seatR.round(2), 'select', seatSel.round(2), 'start', seatSt.round(2))

# --- frames.  glTF: +Y up, -Z forward (bore), +X to the gun's right.  gx=-(y), gy=z, gz=-x
Rg = np.array([[0, -1, 0], [0, 0, 1], [-1, 0, 0]], float)
Mside = ndimage.binary_fill_holes(np.isfinite(F['py']))
def gripmid(z):
    k = np.argmin(abs(Z - z)); xs = X[Mside[:, k]]; xs = xs[xs < -30]; return (xs.min() + xs.max()) / 2
grip_c = np.array([gripmid(-35), yc, -35.0])
def to_g(p): return (Rg @ (np.asarray(p) - grip_c).T).T * 0.001   # mm working frame -> metres glTF, root at grip centre
ax = np.array([gripmid(-20) - gripmid(-70), 0, 50.0]); ax /= np.linalg.norm(ax)
print('grip centre', grip_c.round(2), 'rake deg', np.degrees(np.arctan2(-ax[0], ax[2])))

def smooth_normals(m, crease=35):
    """split vertices at creases; returns positions, normals, indices"""
    fnm = m.face_normals; cosc = np.cos(np.radians(crease)); P, N, I = [], [], []
    vf = [[] for _ in range(len(m.vertices))]
    for fi, f in enumerate(m.faces):
        for v in f: vf[v].append(fi)
    area = m.area_faces; key = {}
    for fi, f in enumerate(m.faces):
        tri = []
        for v in f:
            adj = [g for g in vf[v] if fnm[g] @ fnm[fi] > cosc]
            n = (fnm[adj] * area[adj, None]).sum(0); n /= np.linalg.norm(n)
            kk = (v, tuple(np.round(n, 3)))
            if kk not in key:
                key[kk] = len(P); P.append(m.vertices[v]); N.append(n)
            tri.append(key[kk])
        I.append(tri)
    return np.array(P), np.array(N), np.array(I)

def prim(m, origin_mm, Rn=None):
    """positions/normals in the glTF frame, relative to origin_mm, in the node axes Rn (columns) when given"""
    P, N, I = smooth_normals(m)
    P = (Rg @ (P - origin_mm).T).T * 0.001; N = (Rg @ N.T).T
    if Rn is not None: P, N = P @ Rn, N @ Rn
    return P.astype(np.float32), N.astype(np.float32), I.astype(np.uint16)

shell = body.submesh([np.nonzero(~lens)[0]], append=True); lensm = body.submesh([np.nonzero(lens)[0]], append=True)

# --- minimal glb writer
bin_ = bytearray(); views = []; accessors = []
def add(arr, target):
    off = len(bin_); b = arr.tobytes(); bin_.extend(b); bin_.extend(b'\0' * ((4 - len(b) % 4) % 4))
    views.append({'buffer': 0, 'byteOffset': off, 'byteLength': len(b), 'target': target}); return len(views) - 1
def acc(arr, kind):
    if kind == 'idx':
        v = add(arr.reshape(-1), 34963); accessors.append({'bufferView': v, 'componentType': 5123, 'count': arr.size, 'type': 'SCALAR'})
    else:
        v = add(arr, 34962); a = {'bufferView': v, 'componentType': 5126, 'count': len(arr), 'type': 'VEC3'}
        if kind == 'pos': a['min'] = arr.min(0).tolist(); a['max'] = arr.max(0).tolist()
        accessors.append(a)
    return len(accessors) - 1
materials = [
    {'name': 'shell', 'pbrMetallicRoughness': {'baseColorFactor': [0.035, 0.035, 0.04, 1], 'metallicFactor': 0.0, 'roughnessFactor': 0.55}},
    {'name': 'trim', 'pbrMetallicRoughness': {'baseColorFactor': [0.09, 0.09, 0.1, 1], 'metallicFactor': 0.0, 'roughnessFactor': 0.45}},
    {'name': 'lens', 'pbrMetallicRoughness': {'baseColorFactor': [0.25, 0.02, 0.02, 1], 'metallicFactor': 0.0, 'roughnessFactor': 0.1}},
]
meshes = []; tri_total = 0
def mesh(name, parts):
    global tri_total
    prims = []
    for m, origin, mat, *Rn in parts:
        P, N, I = prim(m, origin, *Rn); tri_total += len(I)
        prims.append({'attributes': {'POSITION': acc(P, 'pos'), 'NORMAL': acc(N, 'nrm')}, 'indices': acc(I, 'idx'), 'material': mat})
    meshes.append({'name': name, 'primitives': prims}); return len(meshes) - 1

def quat_from(Rm):
    t = np.trace(Rm)
    if t > 0:
        s = np.sqrt(t + 1) * 2; return [(Rm[2,1]-Rm[1,2])/s, (Rm[0,2]-Rm[2,0])/s, (Rm[1,0]-Rm[0,1])/s, s/4]
    i = np.argmax(np.diag(Rm)); j, k = (i+1)%3, (i+2)%3
    s = np.sqrt(1 + Rm[i,i] - Rm[j,j] - Rm[k,k]) * 2; q = [0,0,0,0]
    q[i] = s/4; q[j] = (Rm[j,i]+Rm[i,j])/s; q[k] = (Rm[k,i]+Rm[i,k])/s; q[3] = (Rm[k,j]-Rm[j,k])/s; return q

def side_button(name, m, seat, note):
    """A or B, as on the PS1 gun: node origin on the surface seat, no rotation"""
    return {'name': name, 'mesh': mesh(name, [(m, seat, 1)]), 'translation': to_g(seat).tolist(), 'extras': {'note': note}}

def control(name, m, seat, n, up, note):
    """one of the GunCon2's extra controls: node origin on the surface seat, local +Z out of the gun (along n),
    local +Y toward `up` (the top of the gun, along the surface), local +X the gun's right"""
    z = Rg @ n; y = Rg @ up; Rn = np.c_[np.cross(y, z), y, z]
    return {'name': name, 'mesh': mesh(name, [(m, seat, 1, Rn)]), 'translation': to_g(seat).tolist(),
            'rotation': quat_from(Rn), 'extras': {'note': note}}

nodes = [{'name': 'lightgun', 'children': list(range(1, 11))}]
nodes.append({'name': 'body', 'mesh': mesh('body', [(shell, grip_c, 0), (lensm, grip_c, 2)])})
nodes.append({'name': 'trigger', 'mesh': mesh('trigger', [(trigger, trig_pivot, 1)]), 'translation': to_g(trig_pivot).tolist(),
              'extras': {'note': 'pull = rotate about local +X by a negative angle (blade swings back toward the grip), ~12 deg travel'}})
nodes.append(side_button('button_left', btnA, seatL, 'A, on the gun\'s left side (-X) above the trigger; press = translate along +X by ~1 mm'))
nodes.append(side_button('button_right', btnB, seatR, 'B, on the gun\'s right side (+X) above the trigger; press = translate along -X by ~1 mm'))
nodes.append(control('button_select', btnSel, seatSel, ey, ez, 'SELECT, the front one of the two small buttons on the left side (order not verified); press = translate along local -Z by ~1 mm'))
nodes.append(control('button_start', btnSt, seatSt, ey, ez, 'START, the rear one of the two small buttons on the left side; press = translate along local -Z by ~1 mm'))
nodes.append(control('button_c', btnC, seatC, n_c, u_c, 'C, on the heel of the grip; press = translate along local -Z by ~1 mm'))
nodes.append(control('dpad', dpad, dpad_c, n_d, u_d, 'D-pad on the rear face, local +Y up; press a direction = tilt that edge into the gun (about local X or Y), ~6 deg'))
nodes.append({'name': 'muzzle', 'translation': to_g(muzzle).tolist(), 'extras': {'note': 'aim ray origin on the upper bore; aim direction = local -Z'}})
# grip node: local -Z up the grip axis (toward the slide), local +Y toward the muzzle side, X = gun's right
gz_ = -(Rg @ ax); gy_ = Rg @ np.array([ax[2], 0, -ax[0]]); gx_ = np.cross(gy_, gz_)
nodes.append({'name': 'grip', 'translation': [0, 0, 0], 'rotation': quat_from(np.c_[gx_, gy_, gz_]),
              'extras': {'note': 'grip centre; local -Z runs up the raked grip axis, +Y points forward'}})
gltf = {'asset': {'version': '2.0', 'generator': 'ARMSX2 VR tools/lightgun/guncon2'}, 'scene': 0, 'scenes': [{'name': 'lightgun', 'nodes': [0]}],
        'nodes': nodes, 'meshes': meshes, 'materials': materials, 'accessors': accessors, 'bufferViews': views,
        'buffers': [{'byteLength': len(bin_)}]}
js = json.dumps(gltf, separators=(',', ':')).encode(); js += b' ' * ((4 - len(js) % 4) % 4)
with open(OUT, 'wb') as fh:
    fh.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bin_)))
    fh.write(struct.pack('<II', len(js), 0x4E4F534A)); fh.write(js)
    fh.write(struct.pack('<II', len(bin_), 0x004E4942)); fh.write(bin_)
print('triangles', tri_total, 'bytes', 12 + 16 + len(js) + len(bin_))
