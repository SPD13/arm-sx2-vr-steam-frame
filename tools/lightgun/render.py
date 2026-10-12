"""Tiny numpy rasterisers used for the preview images."""
import numpy as np
from PIL import Image

def rot(view):
    # returns 3x3 matrix mapping world -> camera (x right, y up, z toward viewer)
    import math
    if isinstance(view, str):
        d = {'+y': ([-1,0,0],[0,0,1],[0,1,0]), '-y': ([1,0,0],[0,0,1],[0,-1,0]),
             '+x': ([0,1,0],[0,0,1],[1,0,0]), '-x': ([0,-1,0],[0,0,1],[-1,0,0]),
             '+z': ([1,0,0],[0,1,0],[0,0,1]), '-z': ([1,0,0],[0,-1,0],[0,0,-1])}[view]
        return np.array(d, float)
    yaw, pitch = view
    cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)
    base = np.array([[-1,0,0],[0,0,1],[0,1,0]], float)  # looking from +y
    Ry = np.array([[cy,0,sy],[0,1,0],[-sy,0,cy]])
    Rx = np.array([[1,0,0],[0,cp,-sp],[0,sp,cp]])
    return Rx @ Ry @ base

def raster(v, f, R, size=900, bounds=None, color=None):
    """z-buffer triangle rasterizer with flat (face normal) shading. Small meshes only for exactness; large meshes ok-ish."""
    p = v @ R.T
    if bounds is None:
        lo, hi = p[:, :2].min(0), p[:, :2].max(0)
    else:
        lo, hi = bounds
    span = (hi - lo).max() * 1.05
    c = (lo + hi) / 2
    s = size / span
    W = H = size
    xy = (p[:, :2] - c) * s + size / 2
    xy[:, 1] = H - xy[:, 1]
    z = p[:, 2]
    tri = xy[f]; tz = z[f]
    n = np.cross(p[f[:,1]] - p[f[:,0]], p[f[:,2]] - p[f[:,0]])
    nn = n / (np.linalg.norm(n, axis=1, keepdims=True) + 1e-12)
    L = np.array([0.4, 0.5, 0.75]); L /= np.linalg.norm(L)
    shade = 0.25 + 0.75 * np.abs(nn @ L)
    zbuf = np.full((H, W), -np.inf); img = np.ones((H, W, 3)) * 0.12
    mn = np.floor(tri.min(1)).astype(int); mx = np.ceil(tri.max(1)).astype(int)
    mn = np.clip(mn, 0, size - 1); mx = np.clip(mx, 0, size - 1)
    area = (tri[:,1,0]-tri[:,0,0])*(tri[:,2,1]-tri[:,0,1]) - (tri[:,2,0]-tri[:,0,0])*(tri[:,1,1]-tri[:,0,1])
    big = (mx - mn).max(1) > 2
    # small triangles: splat centroid
    small = ~big
    if small.any():
        cxy = tri[small].mean(1).astype(int); cz = tz[small].mean(1)
        cxy = np.clip(cxy, 0, size-1)
        order = np.argsort(cz)
        idx = cxy[order]; zz = cz[order]; sh = shade[small][order]
        cols = (color[small][order] if color is not None else np.ones((len(order),3))*np.array([0.85,0.85,0.8]))
        zbuf[idx[:,1], idx[:,0]] = zz
        img[idx[:,1], idx[:,0]] = cols * sh[:, None]
    for i in np.nonzero(big)[0]:
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
        sub = zbuf[y0:y1 + 1, x0:x1 + 1]
        upd = m & (zz > sub)
        sub[upd] = zz[upd]
        col = color[i] if color is not None else np.array([0.85, 0.85, 0.8])
        img[y0:y1 + 1, x0:x1 + 1][upd] = col * shade[i]
    return (np.clip(img, 0, 1) * 255).astype(np.uint8), zbuf

def points(v, nrm, R, size=900, bounds=None):
    """splat dense scan vertices with normal shading"""
    p = v @ R.T; nr = nrm @ R.T
    if bounds is None: lo, hi = p[:, :2].min(0), p[:, :2].max(0)
    else: lo, hi = bounds
    span = (hi - lo).max() * 1.05; c = (lo + hi) / 2; s = size / span
    xy = ((p[:, :2] - c) * s + size / 2).astype(int); xy[:, 1] = size - 1 - xy[:, 1]
    ok = (xy >= 0).all(1) & (xy < size).all(1)
    xy, z, nr = xy[ok], p[ok, 2], nr[ok]
    order = np.argsort(z)
    L = np.array([0.4, 0.5, 0.75]); L /= np.linalg.norm(L)
    sh = 0.2 + 0.8 * np.clip(nr @ L, 0, 1)
    img = np.ones((size, size)) * 0.1
    img[xy[order,1], xy[order,0]] = sh[order]
    return (img * 255).astype(np.uint8)

def save_grid(imgs, path, cols=3):
    imgs = [Image.fromarray(i) for i in imgs]
    w, h = imgs[0].size; rows = (len(imgs) + cols - 1) // cols
    out = Image.new('RGB', (w * cols, h * rows), (30, 30, 30))
    for k, im in enumerate(imgs): out.paste(im.convert('RGB'), ((k % cols) * w, (k // cols) * h))
    out.save(path)

def raster_smooth(tris, nrms, cols, R, size=700, bounds=None):
    """tris/nrms: (F,3,3) world positions / corner normals, cols: (F,3). Phong-ish per-pixel normal interpolation."""
    p = tris.reshape(-1, 3) @ R.T; n = nrms.reshape(-1, 3) @ R.T
    if bounds is None: lo, hi = p[:, :2].min(0), p[:, :2].max(0)
    else: lo, hi = bounds
    span = (hi - lo).max() * 1.08; c = (lo + hi) / 2; s = size / span
    xy = (p[:, :2] - c) * s + size / 2; xy[:, 1] = size - xy[:, 1]
    tri = xy.reshape(-1, 3, 2); tz = p[:, 2].reshape(-1, 3); tn = n.reshape(-1, 3, 3)
    L = np.array([0.35, 0.6, 0.72]); L /= np.linalg.norm(L); Hh = L + np.array([0, 0, 1]); Hh /= np.linalg.norm(Hh)
    zbuf = np.full((size, size), -np.inf); img = np.ones((size, size, 3)) * np.array([0.82, 0.84, 0.88])
    mn = np.clip(np.floor(tri.min(1)).astype(int), 0, size - 1); mx = np.clip(np.ceil(tri.max(1)).astype(int), 0, size - 1)
    area = (tri[:,1,0]-tri[:,0,0])*(tri[:,2,1]-tri[:,0,1]) - (tri[:,2,0]-tri[:,0,0])*(tri[:,1,1]-tri[:,0,1])
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
        nn = w0[upd, None] * tn[i, 0] + w1[upd, None] * tn[i, 1] + w2[upd, None] * tn[i, 2]
        nn /= np.linalg.norm(nn, axis=1, keepdims=True) + 1e-9
        dif = np.clip(nn @ L, 0, 1); spec = np.clip(nn @ Hh, 0, 1) ** 40
        rim = (1 - np.abs(nn[:, 2])) ** 3 * 0.25
        base = cols[i]
        col = base * (0.35 + 0.65 * dif)[:, None] + (0.25 * spec + rim)[:, None] + 0.04
        img[y0:y1 + 1, x0:x1 + 1][upd] = col
    return (np.clip(img, 0, 1) * 255).astype(np.uint8)
