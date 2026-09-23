"""Independent check of a store against its raw directory (shares no code with ingest).

  python3 bench/verify_store.py store/ raw/

For every instrument: data.db region == concatenated raw day files, and the index's block
timestamps and min/max prices match. Exits non-zero on any mismatch.
"""
import numpy as np, struct, sys, glob, os
store, raw = sys.argv[1], sys.argv[2]
dt=np.dtype([('ts','<i8'),('p','<f4'),('v','<i4')])
idx=open(store+'/index.idx','rb'); hdr=idx.read(56)
magic,ver,rs,stride,n,total,datarecs,entries=struct.unpack('<8sIIQQQQQ',hdr)
tab=np.frombuffer(idx.read(24*n),dtype=np.uint64).reshape(n,3)
sp=np.frombuffer(idx.read(8*entries),dtype='<i8')
d=np.memmap(store+'/data.db',dtype=dt,mode='r')
days=sorted(os.listdir(raw)); days=[x for x in days if x.isdigit()]
rng=np.frombuffer(idx.read(8*entries),dtype='<f4').reshape(-1,2)
bad=0; badidx=0; tot=0
for i in range(n):
    parts=[np.fromfile(f'{raw}/{dd}/{i:04d}.bin',dtype=dt) for dd in days if os.path.exists(f'{raw}/{dd}/{i:04d}.bin')]
    if not parts: continue
    a=np.concatenate(parts); f,c,ifirst=tab[i]; tot+=1
    if c!=len(a) or not np.array_equal(d[f:f+c],a): bad+=1
    nb=(c+stride-1)//stride
    if not np.array_equal(sp[ifirst:ifirst+nb], a['ts'][::stride]): badidx+=1
    else:
        pad=np.concatenate([a['p'],np.full((-len(a))%stride,np.nan,dtype='<f4')]).reshape(nb,stride)
        if not (np.array_equal(rng[ifirst:ifirst+nb,0],np.nanmin(pad,axis=1)) and np.array_equal(rng[ifirst:ifirst+nb,1],np.nanmax(pad,axis=1))): badidx+=1
    if f%4096: bad+=1
print(f'{tot} instruments checked; bad data {bad}; bad index {badidx}')
sys.exit(1 if bad or badidx else 0)
