"""Stage 4: clean the height fields (fill holes, clamp pits, suppress scan noise), erase the trademark areas,
and rebuild a watertight solid as the intersection of the six bounds via marching cubes (solid.py <work> [noise_mm])."""
import sys, numpy as np, trimesh
from scipy import ndimage
from skimage import measure
NOISE=float(sys.argv[2]) if len(sys.argv)>2 else 0.5  # mm of scan detail suppressed before decimation
W=sys.argv[1]; F=np.load(f'{W}/fields.npz'); X,Y,Z=F['X'],F['Y'],F['Z']; res=0.5
def disc(r): r=int(r/res); return np.hypot(*np.mgrid[-r:r+1,-r:r+1])<=r
def laplace_fill(h, region, iters=600):
    h=h.copy(); k=np.array([[0,1,0],[1,0,1],[0,1,0]])/4.
    for _ in range(iters):
        h[region]=ndimage.convolve(h,k,mode='nearest')[region]
    return h
def clean(h, maxhole, dmax, rclose):
    fin=np.isfinite(h)
    filled=ndimage.binary_fill_holes(fin)
    holes=filled&~fin
    lab,n=ndimage.label(holes)
    if maxhole is not None and n:
        areas=ndimage.sum(holes,lab,range(1,n+1))*res*res
        keep=np.isin(lab,1+np.nonzero(areas>maxhole)[0])
        filled&=~keep
    mask=ndimage.binary_opening(filled,iterations=1)
    idx=ndimage.distance_transform_edt(~fin,return_distances=False,return_indices=True)
    hn=h[tuple(idx)]
    hn=ndimage.median_filter(hn,5)
    cl=ndimage.grey_closing(hn,footprint=disc(rclose))
    if maxhole is not None:
        # side views: smooth over through-holes (buttons) and screw pits instead of leaving dimples
        small=holes&mask
        pits=(cl-hn>1.0)&mask
        lab2,n2=ndimage.label(pits)
        if n2:
            a2=ndimage.sum(pits,lab2,range(1,n2+1))*res*res
            pits=np.isin(lab2,1+np.nonzero(a2<60)[0])
        reg=ndimage.binary_dilation(small|pits,iterations=3)&mask
        hn=laplace_fill(np.maximum(hn,cl-dmax),reg)
        lab3,n3=ndimage.label(small)
        for i in range(1,n3+1):
            a=(lab3==i).sum()*res*res
            if a>40:
                c=np.argwhere(lab3==i).mean(0)*res
                print('button hole', HOLE_TAG, 'area %.0f'%a, 'center x %.2f z %.2f r %.2f'%(c[0]+X[0],c[1]+Z[0],np.sqrt(a/np.pi)), 'surface %.2f'%np.median(hn[ndimage.binary_dilation(lab3==i,iterations=4)&~(lab3==i)]))
    out=np.maximum(hn,cl-dmax)
    # suppress sub-0.25 mm scan noise but keep steps and edges
    base=ndimage.gaussian_filter(out,2.5); d=out-base
    out=base+np.sign(d)*np.maximum(np.abs(d)-NOISE,0)
    out=ndimage.gaussian_filter(out,0.8)
    return out, mask
spec={'py':(300,1.5,6),'my':(300,1.5,6),'pz':(None,3,8),'mz':(None,3,8),'px':(None,5,10),'mx':(None,3,8)}
H={}; M={}
for k,(mh,dm,rc) in spec.items():
    HOLE_TAG=k
    H[k],M[k]=clean(F[k],mh,dm,rc)
# --- trademark removal on side fields (boxes in x,z mm), Laplace fill from the surrounding surface
def box(x0,x1,z0,z1):
    xx,zz=np.meshgrid(X,Z,indexing='ij'); return (xx>=x0)&(xx<=x1)&(zz>=z0)&(zz<=z1)
logos={'py':[box(13,77,47.5,57.5), box(-84,-56,15,36), box(35,69,-0.5,14.5)],
       'my':[box(13,77,47.5,57.5), box(-84,-56,15,36), box(35,69,-0.5,14.5)]}
for k,boxes in logos.items():
    for b in boxes:
        H[k]=laplace_fill(H[k], b&M[k])
np.savez(f'{W}/clean_fields.npz',**{k:H[k] for k in H},**{'M'+k:M[k] for k in M})
# --- implicit solid: min over the six directional bounds
BIG=-50.
xx=X[:,None,None]; yy=Y[None,:,None]; zz=Z[None,None,:]
f=np.full((len(X),len(Y),len(Z)),np.inf,np.float32)
def side(k,a,b): return np.where(M[k],H[k],-1e3)
f=np.minimum(f,(side('py',0,2)[:,None,:]-yy).astype(np.float32))
f=np.minimum(f,(side('my',0,2)[:,None,:]+yy).astype(np.float32))
f=np.minimum(f,(side('pz',0,1)[:,:,None]-zz).astype(np.float32))
f=np.minimum(f,(side('mz',0,1)[:,:,None]+zz).astype(np.float32))
f=np.minimum(f,(side('px',1,2)[None,:,:]-xx).astype(np.float32))
f=np.minimum(f,(side('mx',1,2)[None,:,:]+xx).astype(np.float32))
f=np.clip(f,-3,3)
f=np.pad(f,2,constant_values=-3)
v,fa,_,_=measure.marching_cubes(f,0.0,spacing=(res,res,res))
v+=np.array([X[0],Y[0],Z[0]])-2*res
m=trimesh.Trimesh(v,fa[:,::-1]); 
m.remove_unreferenced_vertices()
parts=m.split(only_watertight=False); m=max(parts,key=lambda p:p.area)
print('dense',len(m.vertices),len(m.faces),'watertight',m.is_watertight,'bbox',m.bounds, 'vol',m.volume)
m.export(f'{W}/dense.ply')
