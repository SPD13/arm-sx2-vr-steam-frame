"""Read lightgun.glb back (own parser) and render it with node transforms applied."""
import sys, json, struct, numpy as np
sys.path.insert(0, sys.argv[1]); import render
path, out = sys.argv[2], sys.argv[3]; pose = sys.argv[4] if len(sys.argv) > 4 else 'rest'
b = open(path, 'rb').read(); jl = struct.unpack('<I', b[12:16])[0]; g = json.loads(b[20:20 + jl]); binc = b[20 + jl + 8:]
def acc(i):
    a = g['accessors'][i]; v = g['bufferViews'][a['bufferView']]
    dt = {5126: np.float32, 5123: np.uint16}[a['componentType']]; nc = {'VEC3': 3, 'SCALAR': 1}[a['type']]
    return np.frombuffer(binc, dt, a['count'] * nc, v['byteOffset']).reshape(-1, nc) if nc > 1 else np.frombuffer(binc, dt, a['count'], v['byteOffset'])
def qmat(q):
    x, y, z, w = q
    return np.array([[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)], [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)], [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]])
T, N, C = [], [], []
cols = [np.array(m['pbrMetallicRoughness']['baseColorFactor'][:3]) for m in g['materials']]
cols = [np.clip(c * 3.0 + 0.12, 0, 1) for c in cols]  # lift the near-black for a readable preview
for ni in g['nodes'][0]['children']:
    nd = g['nodes'][ni]
    if 'mesh' not in nd: continue
    Rm = qmat(nd.get('rotation', [0, 0, 0, 1])); t = np.array(nd.get('translation', [0, 0, 0]))
    if pose == 'pressed':
        if nd['name'] == 'trigger':
            a = np.radians(-12); Rm = Rm @ np.array([[1, 0, 0], [0, np.cos(a), -np.sin(a)], [0, np.sin(a), np.cos(a)]])
    for pr in g['meshes'][nd['mesh']]['primitives']:
        P = acc(pr['attributes']['POSITION']) @ Rm.T + t; Nn = acc(pr['attributes']['NORMAL']) @ Rm.T; I = acc(pr['indices']).reshape(-1, 3)
        T.append(P[I]); N.append(Nn[I]); C.append(np.repeat(cols[pr['material']][None], len(I), 0))
T, N, C = np.vstack(T), np.vstack(N), np.vstack(C)
# glTF frame -> renderer frame (+X muzzle, +Y gun-left, +Z up)
G = np.array([[0, 0, -1], [-1, 0, 0], [0, 1, 0]], float)
T = T @ G.T; N = N @ G.T
views = ['+y', '-y', (0.6, 0.3), (-2.5, 0.3), '+x', (0.35, -0.6)]
render.save_grid([render.raster_smooth(T, N, C, render.rot(v), 700) for v in views], out, cols=3)
print('nodes', [n['name'] for n in g['nodes']], 'tris', len(T))
