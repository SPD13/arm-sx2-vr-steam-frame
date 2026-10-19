"""Assemble the scan-based low-poly light gun (not shipped; run.sh writes assets/lightgun-scan.glb).
Working frame (mm): +X toward the muzzle, +Z up, +Y the gun's left side."""
import sys, json, struct, numpy as np, trimesh
from scipy import ndimage
W, OUT = sys.argv[1], sys.argv[2]
body = trimesh.load(f'{W}/low_2600.ply', process=True)
F = np.load(f'{W}/fields.npz'); X, Y, Z = F['X'], F['Y'], F['Z']

# --- front openings, read off the front height field (work/front.png): the upper bore is a ~5 mm deep
# recess (r~9), the lower lens is a raised round cap (r~13) with a flat face
upper = np.array([121.1, -1.5, 55.5]); lower = np.array([120.8, -1.5, 22.5])
muzzle = upper; yc = muzzle[1]
fn = body.face_normals; fc = body.triangles_center
ryz = lambda c: np.hypot(fc[:, 1] - c[1], fc[:, 2] - c[2])
lens = (fn[:, 0] > 0.8) & (((fc[:, 0] < upper[0] - 2.0) & (ryz(upper) < 9.5)) | ((fc[:, 0] > 115) & (ryz(lower) < 9.0)))
print('lens faces', lens.sum())

def extrude(profile, y0, y1):
    """closed prism from a CCW (x,z) polygon, between y0 and y1"""
    p = np.asarray(profile, float); n = len(p)
    v = np.vstack([np.c_[p[:, 0], np.full(n, y1), p[:, 1]], np.c_[p[:, 0], np.full(n, y0), p[:, 1]]])
    import mapbox_earcut
    tri = mapbox_earcut.triangulate_float64(p, np.array([n], np.uint32)).reshape(-1, 3)
    faces = [t for t in tri] + [t[::-1] + n for t in tri]
    for i in range(n):
        j = (i + 1) % n; faces += [[i, n + i, n + j], [i, n + j, j]]
    m = trimesh.Trimesh(v, faces, process=True); m.fix_normals(); return m

# --- trigger: curved blade hanging in the guard opening (x -44.8..-0.8, z -8.2..15.2), pivot at the top
trig_prof = [(-14.5, 19), (-14.2, 12), (-13.2, 7.5), (-11.4, 3.5), (-9.0, 0.4), (-7.6, -2.4), (-8.4, -4.4),
             (-10.6, -4.6), (-13.4, -2.4), (-16.6, 1.0), (-19.0, 5.0), (-20.6, 10), (-21.4, 14), (-21.6, 19)][::-1]
trigger = extrude(trig_prof, yc - 5.0, yc + 5.0)
trig_pivot = np.array([-18.0, yc, 17.0])

def button(cx, cz, ysurf, side, r=6.4, seg=16):
    """chamfered cylinder along Y; side=+1 for the left (+Y) face"""
    a = np.linspace(0, 2 * np.pi, seg, endpoint=False)
    rings = [(r, ysurf - 2.0), (r, ysurf + 0.9), (r - 0.7, ysurf + 1.5)]
    v = []
    for rr, yy in rings:
        v += [[cx + rr * np.cos(t), side * yy, cz + rr * np.sin(t)] for t in a]
    v += [[cx, side * (ysurf - 2.0), cz], [cx, side * (ysurf + 1.5), cz]]
    v = np.array(v); f = []
    for k in range(len(rings) - 1):
        for i in range(seg):
            j = (i + 1) % seg; a0, a1, b0, b1 = k * seg + i, k * seg + j, (k + 1) * seg + i, (k + 1) * seg + j
            f += [[a0, a1, b1], [a0, b1, b0]]
    cb, ct = len(v) - 2, len(v) - 1; top = (len(rings) - 1) * seg
    for i in range(seg):
        j = (i + 1) % seg; f += [[cb, j, i], [ct, top + i, top + j]]
    m = trimesh.Trimesh(v, f, process=True); m.fix_normals(); return m

# surface heights at the button seats, measured on the low-poly body itself
def surf(cx, cz, side):
    o = np.array([[cx, side * 40.0, cz]]); d = np.array([[0, -side, 0]])
    loc, _, _ = body.ray.intersects_location(o, d); return (side * loc[:, 1]).max()  # first hit from that side
bx, bz = 83.02, 19.49
sL, sR = surf(bx, bz, +1), surf(bx, bz, -1)
btnL = button(bx, bz, sL, +1); btnR = button(bx, bz, sR, -1)
print('button seats', sL, sR)

# --- frames.  glTF: +Y up, -Z forward (bore), +X to the gun's right.  gx=-(y), gy=z, gz=-x
Rg = np.array([[0, -1, 0], [0, 0, 1], [-1, 0, 0]], float)
# grip centre: middle of the grip section at z=-35
k = np.argmin(abs(Z + 35)); Mside = np.isfinite(F['py'])
xs = X[ndimage.binary_fill_holes(Mside)[:, k]]; xs = xs[xs < -30]
grip_c = np.array([(xs.min() + xs.max()) / 2, yc, -35.0])
def to_g(p): return (Rg @ (np.asarray(p) - grip_c).T).T * 0.001   # mm working frame -> metres glTF, root at grip centre

# grip axis (rake): centre line of the grip between z=-80 and z=-20
def gripmid(z):
    k = np.argmin(abs(Z - z)); xs = X[ndimage.binary_fill_holes(Mside)[:, k]]; xs = xs[xs < -30]; return (xs.min() + xs.max()) / 2
ax = np.array([gripmid(-20) - gripmid(-80), 0, 60.0]); ax /= np.linalg.norm(ax)
print('grip rake deg', np.degrees(np.arctan2(-ax[0], ax[2])))

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

def prim(m, origin_mm):
    P, N, I = smooth_normals(m)
    P = (Rg @ (P - origin_mm).T).T * 0.001; N = (Rg @ N.T).T
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
    for m, origin, mat in parts:
        P, N, I = prim(m, origin); tri_total += len(I)
        prims.append({'attributes': {'POSITION': acc(P, 'pos'), 'NORMAL': acc(N, 'nrm')}, 'indices': acc(I, 'idx'), 'material': mat})
    meshes.append({'name': name, 'primitives': prims}); return len(meshes) - 1

def quat_from(Rm):
    t = np.trace(Rm)
    if t > 0:
        s = np.sqrt(t + 1) * 2; return [(Rm[2,1]-Rm[1,2])/s, (Rm[0,2]-Rm[2,0])/s, (Rm[1,0]-Rm[0,1])/s, s/4]
    i = np.argmax(np.diag(Rm)); j, k = (i+1)%3, (i+2)%3
    s = np.sqrt(1 + Rm[i,i] - Rm[j,j] - Rm[k,k]) * 2; q = [0,0,0,0]
    q[i] = s/4; q[j] = (Rm[j,i]+Rm[i,j])/s; q[k] = (Rm[k,i]+Rm[i,k])/s; q[3] = (Rm[k,j]-Rm[j,k])/s; return q

nodes = [{'name': 'lightgun', 'children': [1, 2, 3, 4, 5, 6]}]
nodes.append({'name': 'body', 'mesh': mesh('body', [(shell, grip_c, 0), (lensm, grip_c, 2)])})
nodes.append({'name': 'trigger', 'mesh': mesh('trigger', [(trigger, trig_pivot, 1)]), 'translation': to_g(trig_pivot).tolist(),
              'extras': {'note': 'pull = rotate about local +X by a negative angle (blade swings back toward the grip), ~12 deg travel'}})
nodes.append({'name': 'button_left', 'mesh': mesh('button_left', [(btnL, np.array([bx, 0, bz]), 1)]), 'translation': to_g([bx, 0, bz]).tolist(),
              'extras': {'note': 'on the gun\'s left side (-X); press = translate along +X by ~1 mm'}})
nodes.append({'name': 'button_right', 'mesh': mesh('button_right', [(btnR, np.array([bx, 0, bz]), 1)]), 'translation': to_g([bx, 0, bz]).tolist(),
              'extras': {'note': 'on the gun\'s right side (+X); press = translate along -X by ~1 mm'}})
nodes.append({'name': 'muzzle', 'translation': to_g(muzzle).tolist(), 'extras': {'note': 'aim ray origin on the upper bore; aim direction = local -Z'}})
# grip node: local -Z up the grip axis (toward the slide), local +Y toward the muzzle side, X = gun's right
gz_ = -(Rg @ ax); gy_ = Rg @ np.array([ax[2], 0, -ax[0]]); gx_ = np.cross(gy_, gz_)
nodes.append({'name': 'grip', 'translation': [0, 0, 0], 'rotation': quat_from(np.c_[gx_, gy_, gz_]),
              'extras': {'note': 'grip centre; local -Z runs up the raked grip axis, +Y points forward'}})
gltf = {'asset': {'version': '2.0', 'generator': 'ARMSX2 VR tools/lightgun'}, 'scene': 0, 'scenes': [{'name': 'lightgun', 'nodes': [0]}],
        'nodes': nodes, 'meshes': meshes, 'materials': materials, 'accessors': accessors, 'bufferViews': views,
        'buffers': [{'byteLength': len(bin_)}]}
js = json.dumps(gltf, separators=(',', ':')).encode(); js += b' ' * ((4 - len(js) % 4) % 4)
with open(OUT, 'wb') as fh:
    fh.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bin_)))
    fh.write(struct.pack('<II', len(js), 0x4E4F534A)); fh.write(js)
    fh.write(struct.pack('<II', len(bin_), 0x004E4942)); fh.write(bin_)
print('triangles', tri_total, 'bytes', 12 + 16 + len(js) + len(bin_))
