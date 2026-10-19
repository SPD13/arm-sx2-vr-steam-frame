"""Shared by the glb stages: meshes for the hand-drawn parts, the working frame -> glTF mapping and a minimal .glb
writer. Working frame (mm): +X toward the muzzle, +Z up, +Y the gun's left side. glTF: +Y up, -Z forward (bore),
+X the gun's right; metres, root at the grip centre."""
import json, struct, numpy as np, trimesh, mapbox_earcut

Rg = np.array([[0, -1, 0], [0, 0, 1], [-1, 0, 0]], float)   # gx=-(y), gy=z, gz=-x
ex, ey, ez = np.eye(3)

def frame(W):
    """the stage-1 features, with a px -> working-frame mapping for points traced on the same photo"""
    F = json.load(open(f'{W}/frame.json')); m = F['px_to_mm']
    def px(p):
        p = np.asarray(p, float); x = (m['x_hi'] - p[..., 0]) * m['sx'] if m['flip'] else (p[..., 0] - m['x_lo']) * m['sx']
        return np.stack([x, (m['y_bot'] - p[..., 1]) * m['sz']], -1)
    return F, px

def extrude(profile, y0, y1):
    """closed prism from an (x,z) polygon, between y0 and y1"""
    p = np.asarray(profile, float); n = len(p)
    v = np.vstack([np.c_[p[:, 0], np.full(n, y1), p[:, 1]], np.c_[p[:, 0], np.full(n, y0), p[:, 1]]])
    tri = mapbox_earcut.triangulate_float64(p, np.array([n], np.uint32)).reshape(-1, 3)
    faces = [t for t in tri] + [t[::-1] + n for t in tri]
    for i in range(n):
        j = (i + 1) % n; faces += [[i, n + i, n + j], [i, n + j, j]]
    m = trimesh.Trimesh(v, faces, process=True); m.fix_normals(); return m

def circle(r, seg=16):
    a = np.linspace(0, 2 * np.pi, seg, endpoint=False); return np.c_[r * np.cos(a), r * np.sin(a)]
def stadium(L, H, seg=6):
    r = H / 2; c = L / 2 - r; a = np.linspace(-np.pi / 2, np.pi / 2, seg + 1)
    return np.vstack([np.c_[c + r * np.cos(a), r * np.sin(a)], np.c_[-c - r * np.cos(a), -r * np.sin(a)]])
def cross(span, arm):
    """plus sign, both arms `span` long and `arm` wide, centred at 0 (CCW)"""
    a, b = span / 2, arm / 2
    return np.array([(a, -b), (a, b), (b, b), (b, a), (-b, a), (-b, b), (-a, b), (-a, -b), (-b, -b), (-b, -a), (b, -a), (b, -b)])

def cap(outline, inset, origin, e1, e2, n, top, depth=2.5):
    """button from a CCW outline (in local e1/e2 mm): from `depth` mm under origin to `top` mm above it along n,
    with the top edge chamfered by `inset`"""
    o = np.asarray(outline, float); k = len(o); c = o.mean(0)
    s = 1 - inset / np.linalg.norm(o - c, axis=1).min()
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

def quat_from(Rm):
    t = np.trace(Rm)
    if t > 0:
        s = np.sqrt(t + 1) * 2; return [(Rm[2,1]-Rm[1,2])/s, (Rm[0,2]-Rm[2,0])/s, (Rm[1,0]-Rm[0,1])/s, s/4]
    i = np.argmax(np.diag(Rm)); j, k = (i+1)%3, (i+2)%3
    s = np.sqrt(1 + Rm[i,i] - Rm[j,j] - Rm[k,k]) * 2; q = [0,0,0,0]
    q[i] = s/4; q[j] = (Rm[j,i]+Rm[i,j])/s; q[k] = (Rm[k,i]+Rm[i,k])/s; q[3] = (Rm[k,j]-Rm[j,k])/s; return q

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

MATERIALS = [
    {'name': 'shell', 'pbrMetallicRoughness': {'baseColorFactor': [0.035, 0.035, 0.04, 1], 'metallicFactor': 0.0, 'roughnessFactor': 0.55}},
    {'name': 'trim', 'pbrMetallicRoughness': {'baseColorFactor': [0.09, 0.09, 0.1, 1], 'metallicFactor': 0.0, 'roughnessFactor': 0.45}},
    {'name': 'lens', 'pbrMetallicRoughness': {'baseColorFactor': [0.25, 0.02, 0.02, 1], 'metallicFactor': 0.0, 'roughnessFactor': 0.1}},
]

class Glb:
    """nodes and meshes in the working frame; write() converts to glTF around the grip centre"""
    def __init__(self, grip_c, materials):
        self.grip_c = np.asarray(grip_c, float); self.materials = materials
        self.bin = bytearray(); self.views = []; self.accessors = []; self.meshes = []; self.nodes = []; self.tris = 0
    def to_g(self, p): return (Rg @ (np.asarray(p, float) - self.grip_c).T).T * 0.001
    def _add(self, arr, target):
        off = len(self.bin); b = arr.tobytes(); self.bin.extend(b); self.bin.extend(b'\0' * ((4 - len(b) % 4) % 4))
        self.views.append({'buffer': 0, 'byteOffset': off, 'byteLength': len(b), 'target': target}); return len(self.views) - 1
    def _acc(self, arr, kind):
        if kind == 'idx':
            v = self._add(arr.reshape(-1), 34963); self.accessors.append({'bufferView': v, 'componentType': 5123, 'count': arr.size, 'type': 'SCALAR'})
        else:
            v = self._add(arr, 34962); a = {'bufferView': v, 'componentType': 5126, 'count': len(arr), 'type': 'VEC3'}
            if kind == 'pos': a['min'] = arr.min(0).tolist(); a['max'] = arr.max(0).tolist()
            self.accessors.append(a)
        return len(self.accessors) - 1
    def mesh(self, name, parts):
        """parts: (trimesh, origin mm, material index[, node axes as columns in glTF])"""
        prims = []
        for m, origin, mat, *Rn in parts:
            P, N, I = smooth_normals(m)
            P = (Rg @ (P - origin).T).T * 0.001; N = (Rg @ N.T).T
            if Rn: P, N = P @ Rn[0], N @ Rn[0]
            P, N, I = P.astype(np.float32), N.astype(np.float32), I.astype(np.uint16); self.tris += len(I)
            prims.append({'attributes': {'POSITION': self._acc(P, 'pos'), 'NORMAL': self._acc(N, 'nrm')}, 'indices': self._acc(I, 'idx'), 'material': mat})
        self.meshes.append({'name': name, 'primitives': prims}); return len(self.meshes) - 1
    def node(self, name, mesh_parts=None, at=None, rotation=None, note=None):
        nd = {'name': name}
        if mesh_parts: nd['mesh'] = self.mesh(name, mesh_parts)
        if at is not None: nd['translation'] = self.to_g(at).tolist()
        if rotation is not None: nd['rotation'] = rotation
        if note: nd['extras'] = {'note': note}
        self.nodes.append(nd)
    def control(self, name, m, seat, n, up, mat, note):
        """a control whose node origin is on the surface seat: local +Z out of the gun (along n), local +Y toward
        `up` (along the surface), local +X the gun's right"""
        z = Rg @ n; y = Rg @ up; Rn = np.c_[np.cross(y, z), y, z]
        self.node(name, [(m, seat, mat, Rn)], seat, quat_from(Rn), note)
    def grip(self, axis):
        """grip node: local -Z up the grip axis (toward the slide), local +Y toward the muzzle side, X = gun's right"""
        gz_ = -(Rg @ axis); gy_ = Rg @ np.array([axis[2], 0, -axis[0]]); gx_ = np.cross(gy_, gz_)
        self.nodes.append({'name': 'grip', 'translation': [0, 0, 0], 'rotation': quat_from(np.c_[gx_, gy_, gz_]),
                           'extras': {'note': 'grip centre; local -Z runs up the raked grip axis, +Y points forward'}})
    def write(self, out, generator):
        nodes = [{'name': 'lightgun', 'children': list(range(1, len(self.nodes) + 1))}] + self.nodes
        gltf = {'asset': {'version': '2.0', 'generator': generator}, 'scene': 0, 'scenes': [{'name': 'lightgun', 'nodes': [0]}],
                'nodes': nodes, 'meshes': self.meshes, 'materials': self.materials, 'accessors': self.accessors, 'bufferViews': self.views,
                'buffers': [{'byteLength': len(self.bin)}]}
        js = json.dumps(gltf, separators=(',', ':')).encode(); js += b' ' * ((4 - len(js) % 4) % 4)
        with open(out, 'wb') as fh:
            fh.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(self.bin)))
            fh.write(struct.pack('<II', len(js), 0x4E4F534A)); fh.write(js)
            fh.write(struct.pack('<II', len(self.bin), 0x004E4942)); fh.write(self.bin)
        print('triangles', self.tris, 'bytes', 12 + 16 + len(js) + len(self.bin))

def lens_faces(body, F):
    """the floor of each bore recess"""
    fn = body.face_normals; fc = body.triangles_center; lens = np.zeros(len(fc), bool)
    for t in F['tubes'].values():
        r = np.hypot(fc[:, 1], fc[:, 2] - t['center_z'])
        lens |= (fn[:, 0] > 0.8) & (r < t['bore_radius'] - 0.3) & (fc[:, 0] < t['front_x'] - 1.0) & (fc[:, 0] > t['bore_floor_x'] - 1.0)
    return lens

def grip_frame(F):
    """grip centre (mm, working frame) and the unit grip axis"""
    gc = F['grip_centre']; (x0, z0), (x1, z1) = F['grip_rake']
    ax = np.array([x1 - x0, 0, z1 - z0]); ax /= np.linalg.norm(ax)
    print('grip rake deg %.1f' % np.degrees(np.arctan2(ax[0], ax[2])))
    return np.array([gc[0], 0.0, gc[1]]), ax

def trigger(F, top_extra=2.0):
    """the traced blade, plus a stem up into the frame so the pivot is hidden when it swings"""
    tg = F['trigger']; piv = np.array([tg['pivot'][0], 0.0, tg['pivot'][1]])
    prof = np.array(tg['poly']); prof = np.vstack([[prof[0][0], piv[2] + top_extra], [prof[1][0], piv[2] + top_extra], prof[1:]])
    return extrude(prof, -tg['width'] / 2, tg['width'] / 2), piv
