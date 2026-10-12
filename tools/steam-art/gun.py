"""Studio render of assets/lightgun.glb for the Steam artwork: numpy rasteriser with perspective,
per-pixel normals, cyan/violet rim lights (the ARMSX2 palette), an emissive lens and an alpha channel."""
import json, math, struct
import numpy as np
from PIL import Image

def load(path):
    """Triangles (F,3,3), corner normals (F,3,3) and material names (F,) in the glTF frame
    (+Y up, -Z along the bore, +X the gun's right side), plus the node positions by name."""
    b = open(path, 'rb').read()
    jl = struct.unpack('<I', b[12:16])[0]
    g = json.loads(b[20:20 + jl]); binc = b[20 + jl + 8:]
    def acc(i):
        a = g['accessors'][i]; v = g['bufferViews'][a['bufferView']]
        dt = {5126: np.float32, 5123: np.uint16, 5125: np.uint32}[a['componentType']]
        nc = {'VEC3': 3, 'SCALAR': 1}[a['type']]
        arr = np.frombuffer(binc, dt, a['count'] * nc, v.get('byteOffset', 0) + a.get('byteOffset', 0))
        return arr.reshape(-1, nc).astype(float) if nc > 1 else arr.astype(np.int64)
    def qmat(q):
        x, y, z, w = q
        return np.array([[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)], [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
                         [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]])
    T, N, M, nodes = [], [], [], {}
    mats = [m['name'] for m in g['materials']]
    for ni in g['nodes'][0]['children']:
        nd = g['nodes'][ni]
        R = qmat(nd.get('rotation', [0, 0, 0, 1])); t = np.array(nd.get('translation', [0, 0, 0]), float)
        nodes[nd['name']] = t
        if 'mesh' not in nd: continue
        for pr in g['meshes'][nd['mesh']]['primitives']:
            P = acc(pr['attributes']['POSITION']) @ R.T + t
            Nn = acc(pr['attributes']['NORMAL']) @ R.T
            I = acc(pr['indices']).reshape(-1, 3)
            T.append(P[I]); N.append(Nn[I]); M += [mats[pr['material']]] * len(I)
    return np.vstack(T), np.vstack(N), np.array(M), nodes

def camera(yaw, pitch, roll=0.0):
    """World -> camera rotation (x right, y up, z toward the viewer). yaw turns the gun about its
    up axis (0 = we look at its right side, muzzle pointing left), pitch tilts the camera down."""
    cy, sy = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)
    cr, sr = math.cos(roll), math.sin(roll)
    side = np.array([[0, 0, 1], [0, 1, 0], [-1, 0, 0]], float)  # right side faces +Z
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rx = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]])
    Rz = np.array([[cr, -sr, 0], [sr, cr, 0], [0, 0, 1]])
    return Rz @ Rx @ Ry @ side

ALBEDO = {'shell': np.array([0.46, 0.47, 0.5]), 'trim': np.array([0.3, 0.3, 0.33]),
          'lens': np.array([0.3, 0.02, 0.02])}
SPEC = {'shell': (0.22, 10), 'trim': (0.4, 30), 'lens': (1.0, 90)}
def _unit(v): v = np.array(v, float); return v / np.linalg.norm(v)
KEY = _unit([-0.35, 0.7, 0.75]);    KEY_C = np.array([0.95, 0.97, 1.0]) * 1.5
FILL = _unit([0.6, -0.3, 0.75]);   FILL_C = np.array([0.35, 0.45, 0.8]) * 0.5
RIM1 = _unit([-0.8, 0.6, -0.1]);   RIM1_C = np.array([0.25, 0.72, 1.0]) * 3.0   # cyan, from the pillar
RIM2 = _unit([0.85, -0.45, -0.1]); RIM2_C = np.array([0.72, 0.3, 1.0]) * 2.4    # violet, the orbit ring
AMB = np.array([0.08, 0.1, 0.2])
LENS_EMIT = np.array([1.0, 0.1, 0.08])

class Shot:
    """One render. size = (w, h) in output pixels; the gun fills `fill` of the frame's limiting axis."""
    def __init__(self, glb, yaw, pitch, roll=0.0, size=(1024, 1024), fill=0.9, persp=2.2, ss=3, raster=True):
        T, N, M, self.nodes = load(glb)
        R = camera(yaw, pitch, roll)
        self.R, self.ss = R, ss
        W, H = size[0] * ss, size[1] * ss
        p = T.reshape(-1, 3) @ R.T
        ext = (p.max(0) - p.min(0)).max()
        self.c3 = (p.max(0) + p.min(0)) / 2
        self.dist = persp * ext   # camera distance, in model extents
        q = self._proj_cam(p)
        lo, hi = q.min(0), q.max(0)
        self.scale = fill * min(W / (hi - lo)[0], H / (hi - lo)[1])
        self.off = np.array([W, H]) / 2 - (lo + hi) / 2 * self.scale
        xy = q * self.scale + self.off
        xy[:, 1] = H - xy[:, 1]
        self.W, self.H = W, H
        if not raster: return
        self.rgba = self._raster(xy.reshape(-1, 3, 2), (p[:, 2] - self.c3[2]).reshape(-1, 3),
                                 (N.reshape(-1, 3) @ R.T).reshape(-1, 3, 3), M)

    def _proj_cam(self, p):
        d = p - self.c3
        k = self.dist / (self.dist - d[:, 2])
        return d[:, :2] * k[:, None]

    def project(self, world):
        """Output-pixel position of a glTF-frame point (e.g. the muzzle node)."""
        q = self._proj_cam(np.atleast_2d(world) @ self.R.T) * self.scale + self.off
        q[:, 1] = self.H - q[:, 1]
        return q[0] / self.ss

    def direction(self, world_dir):
        """Screen-space unit direction of a glTF-frame direction at the muzzle."""
        a = self.project(self.nodes['muzzle']); b = self.project(self.nodes['muzzle'] + 0.05 * np.asarray(world_dir))
        d = b - a; return d / np.linalg.norm(d)

    def _raster(self, tri, tz, tn, mats):
        W, H = self.W, self.H
        zbuf = np.full((H, W), -np.inf); col = np.zeros((H, W, 3)); emit = np.zeros((H, W))
        mn = np.clip(np.floor(tri.min(1)).astype(int), 0, [W - 1, H - 1])
        mx = np.clip(np.ceil(tri.max(1)).astype(int), 0, [W - 1, H - 1])
        area = (tri[:,1,0]-tri[:,0,0])*(tri[:,2,1]-tri[:,0,1]) - (tri[:,2,0]-tri[:,0,0])*(tri[:,1,1]-tri[:,0,1])
        V = np.array([0, 0, 1.0])
        for i in range(len(tri)):
            if abs(area[i]) < 1e-9: continue
            x0, y0 = mn[i]; x1, y1 = mx[i]
            X, Y = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            (ax, ay), (bx, by), (cx, cy) = tri[i]
            w0 = ((bx - X) * (cy - Y) - (cx - X) * (by - Y)) / area[i]
            w1 = ((cx - X) * (ay - Y) - (ax - X) * (cy - Y)) / area[i]
            w2 = 1 - w0 - w1
            m = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
            if not m.any(): continue
            zz = w0 * tz[i, 0] + w1 * tz[i, 1] + w2 * tz[i, 2]
            sub = zbuf[y0:y1 + 1, x0:x1 + 1]; upd = m & (zz > sub)
            if not upd.any(): continue
            sub[upd] = zz[upd]
            n = w0[upd, None] * tn[i, 0] + w1[upd, None] * tn[i, 1] + w2[upd, None] * tn[i, 2]
            n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-9
            mat = mats[i]; alb = ALBEDO[mat]; ks, ex = SPEC[mat]
            fres = (1 - np.clip(n @ V, 0, 1)) ** 2
            c = alb * (AMB + KEY_C * np.clip(n @ KEY, 0, 1)[:, None] + FILL_C * np.clip(n @ FILL, 0, 1)[:, None])
            c = c + ks * KEY_C * (np.clip(n @ _unit(KEY + V), 0, 1) ** ex)[:, None]
            c = c + RIM1_C * (np.clip(n @ RIM1, 0, 1) * fres ** 1.5)[:, None]
            c = c + RIM2_C * (np.clip(n @ RIM2, 0, 1) * fres ** 1.5)[:, None]
            e = np.zeros(upd.sum())
            if mat == 'lens':
                e = 0.55 + 0.45 * np.clip(n @ V, 0, 1)
                c = c + LENS_EMIT * e[:, None]
            col[y0:y1 + 1, x0:x1 + 1][upd] = c
            emit[y0:y1 + 1, x0:x1 + 1][upd] = e
        self.emit_mask = emit
        a = np.isfinite(zbuf).astype(float)
        c = col / (1 + col * 0.35)  # soft tone curve so the rims don't clip flat
        out = np.dstack([np.clip(c, 0, 1), a]) * 255
        return Image.fromarray(out.astype(np.uint8), 'RGBA')

    def image(self):
        """Anti-aliased RGBA at the requested size (premultiplied downsample)."""
        a = np.asarray(self.rgba).astype(float) / 255
        pm = np.dstack([a[..., :3] * a[..., 3:], a[..., 3:]])
        w, h = self.W // self.ss, self.H // self.ss
        pm = pm.reshape(h, self.ss, w, self.ss, 4).mean((1, 3))
        alpha = pm[..., 3:]
        rgb = np.where(alpha > 0, pm[..., :3] / np.maximum(alpha, 1e-6), 0)
        return Image.fromarray((np.dstack([rgb, alpha]) * 255 + 0.5).clip(0, 255).astype(np.uint8), 'RGBA')

    def lens_glow(self):
        """Grey mask of the emissive lens pixels at output size, for the bloom pass."""
        w, h = self.W // self.ss, self.H // self.ss
        e = self.emit_mask.reshape(h, self.ss, w, self.ss).mean((1, 3))
        return Image.fromarray((e * 255).astype(np.uint8), 'L')
