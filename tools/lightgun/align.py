"""Stage 2: mate the two shells. Right shell is flipped onto the left one's parting plane (rims measured at
y=-0.373 / -0.742 in the scans) and registered in X/Z by outline chamfer matching (align.py <work>)."""
import sys, numpy as np, trimesh
from scipy import ndimage, optimize
W=sys.argv[1]
res=0.5; O=np.array([-130.,-100.]); N=(520,400)
def sil(v):
    ij=np.floor((v[:,[0,2]]-O)/res).astype(int)
    m=np.zeros(N,bool); m[ij[:,0],ij[:,1]]=True
    m=ndimage.binary_closing(m,iterations=2); return ndimage.binary_fill_holes(m)
L=np.load(f'{W}/Left.npz')['v'].astype(float); R=np.load(f'{W}/Right.npz')['v'].astype(float)
L[:,1]+=0.373
R=R*np.array([-1,-1,1]); R[:,1]-=0.742+1.0  # rim to y=0, plus 1.0 mm for the stepped lip (makes the muzzle bore round)
mL=sil(L)
# outline points of left, distance transform of left outline for chamfer
edgeL=mL^ndimage.binary_erosion(mL)
dtL=ndimage.distance_transform_edt(~edgeL)*res
mR=sil(R); edgeR=mR^ndimage.binary_erosion(mR)
pR=np.argwhere(edgeR)*res+O
c0=pR.mean(0)
def cost(p):
    th,tx,tz=p; c,s=np.cos(th),np.sin(th)
    q=(pR-c0)@np.array([[c,s],[-s,c]])+c0+[tx,tz]
    ij=(q-O)/res
    return np.mean(np.minimum(ndimage.map_coordinates(dtL,ij.T,order=1,mode='nearest'),5.0)**2)
best=None
for tx in np.arange(-6,7,2):
  for tz in np.arange(-6,7,2):
    r=optimize.minimize(cost,[0,tx,tz],method='Powell')
    if best is None or r.fun<best.fun: best=r
print(best.x, best.fun, 'deg',np.degrees(best.x[0]))
th,tx,tz=best.x; c,s=np.cos(th),np.sin(th)
xz=(R[:,[0,2]]-c0)@np.array([[c,s],[-s,c]])+c0+[tx,tz]
R2=R.copy(); R2[:,0]=xz[:,0]; R2[:,2]=xz[:,1]
mR2=sil(R2)
print('IoU',(mL&mR2).sum()/(mL|mR2).sum(), 'L only',(mL&~mR2).sum()*res*res,'mm2','R only',(mR2&~mL).sum()*res*res)
np.save(f'{W}/R_aligned_v.npy',R2.astype(np.float32)); np.save(f'{W}/L_v.npy',L.astype(np.float32))
np.save(f'{W}/maskL.npy',mL); np.save(f'{W}/maskR.npy',mR2)
from PIL import Image
img=np.zeros(N+(3,),np.uint8); img[...,0]=mL*255; img[...,1]=mR2*255
Image.fromarray(np.flipud(img.transpose(1,0,2))).save(f'{W}/sil_overlap.png')
