"""Stage 5: quadric decimation of the dense solid to the low-poly budget (decimate.py <work> <faces>)."""
import sys, numpy as np, pymeshlab, trimesh
W=sys.argv[1]; target=int(sys.argv[2])
ms=pymeshlab.MeshSet(); ms.load_new_mesh(f'{W}/dense.ply')
ms.apply_coord_taubin_smoothing(stepsmoothnum=12)
ms.meshing_decimation_quadric_edge_collapse(targetfacenum=60000,preservenormal=True,optimalplacement=True,planarquadric=True,qualitythr=0.4)
ms.apply_coord_taubin_smoothing(stepsmoothnum=4)
ms.meshing_decimation_quadric_edge_collapse(targetfacenum=target,preservenormal=True,preservetopology=True,optimalplacement=True,planarquadric=True,planarweight=0.002,qualitythr=0.5)
ms.meshing_repair_non_manifold_edges(); ms.meshing_remove_unreferenced_vertices()
m=ms.current_mesh()
lo=trimesh.Trimesh(m.vertex_matrix(),m.face_matrix(),process=True)
hi=trimesh.load(f'{W}/dense.ply',process=False)
# deviation: dense samples -> low surface and back
p=hi.sample(40000); _,d1,_=trimesh.proximity.closest_point(lo,p)
q=lo.sample(40000); _,d2,_=trimesh.proximity.closest_point(hi,q)
print('faces',len(lo.faces),'watertight',lo.is_watertight,'dense->low mean %.2f p95 %.2f max %.2f | low->dense mean %.2f p95 %.2f max %.2f mm'%(d1.mean(),np.percentile(d1,95),d1.max(),d2.mean(),np.percentile(d2,95),d2.max()))
lo.export(f'{W}/low_{target}.ply')
