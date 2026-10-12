"""Stage 1: read a scan OBJ and store vertices/faces as compressed numpy (load.py <scan.obj> <out.npz>)."""
import sys, numpy as np, pymeshlab, time
src, dst = sys.argv[1], sys.argv[2]
t=time.time()
ms = pymeshlab.MeshSet(); ms.load_new_mesh(src)
m = ms.current_mesh()
v = m.vertex_matrix(); f = m.face_matrix()
np.savez(dst, v=v.astype(np.float32), f=f.astype(np.int32))
print(src, v.shape, f.shape, 'bbox', v.min(0), v.max(0), time.time()-t)
