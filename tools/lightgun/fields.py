"""Stage 3: six directional outer-surface height fields (+-X, +-Y, +-Z) at 0.5 mm from both shells (fields.py <work>)."""
import sys, numpy as np
from scipy import ndimage
W=sys.argv[1]
res=0.5
X=np.arange(-125,125,res)+res/2; Y=np.arange(-24,24,res)+res/2; Z=np.arange(-95,85,res)+res/2
L=np.load(f'{W}/L_v.npy').astype(float); R=np.load(f'{W}/R_aligned_v.npy').astype(float)
V=np.vstack([L,R])
lo=np.array([X[0],Y[0],Z[0]])-res/2
I=np.floor((V-lo)/res).astype(int)
shape=(len(X),len(Y),len(Z))
def hf(a,b,c,sign):
    h=np.full((shape[a],shape[b]),-np.inf)
    np.maximum.at(h,(I[:,a],I[:,b]),V[:,c]*sign)
    fin=np.isfinite(h)
    # close single-pixel gaps
    hd=ndimage.grey_dilation(np.where(fin,h,-1e9),size=3)
    gap=~fin & ndimage.binary_closing(fin,iterations=2)
    h=np.where(gap,hd,h)
    return h
H={}
H['+y']=hf(0,2,1,1); H['-y']=hf(0,2,1,-1)
H['+z']=hf(0,1,2,1); H['-z']=hf(0,1,2,-1)
H['+x']=hf(1,2,0,1); H['-x']=hf(1,2,0,-1)
np.savez(f'{W}/fields.npz',X=X,Y=Y,Z=Z,**{k.replace('+','p').replace('-','m'):v for k,v in H.items()})
for k,h in H.items():
    fin=np.isfinite(h); hh=np.where(fin,h,np.nan)
    cl=ndimage.grey_closing(np.where(fin,h,np.nanmin(hh)),footprint=np.hypot(*np.mgrid[-20:21,-20:21])<=20)
    depth=np.where(fin,cl-h,0)
    pits=(depth>3)&fin
    lab,n=ndimage.label(pits)
    print(k,'pit comps',n)
    for i,sl in enumerate(ndimage.find_objects(lab)):
        area=(lab[sl]==i+1).sum()*res*res
        if area<4: continue
        c=[ (s.start+s.stop)/2*res for s in sl]
        print('   area %.0f mm2 depth %.1f center(uv) %s'%(area,depth[sl][lab[sl]==i+1].max(),[round(c[0]+(X[0] if k[1]!='x' else Y[0]) -res/2,1), round(c[1]+(Z[0] if k[1]!='z' else Y[0])-res/2,1)]))
