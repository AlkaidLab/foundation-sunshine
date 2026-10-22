"""Streaming audio audit for long property traces, without exporting gigabytes of WAV."""
import collections
import hashlib
import json
import pathlib
import struct
import sys
import numpy as np

root=pathlib.Path(sys.argv[1]).resolve()
h=struct.Struct('<IIQqIIfffffIQ')
groups={};seq=0
with (root/'global-objects.bin').open('rb') as f:
    while raw:=f.read(64):
        r=h.unpack(raw)
        assert r[0]==0x474f424a and r[12]==seq and 0<r[1]<=2048
        seq+=1;payload=f.read(r[1]*4);assert len(payload)==r[1]*4
        v=np.frombuffer(payload,dtype='<f4');assert np.isfinite(v).all()
        key=(r[2],r[4],r[5])
        if key not in groups:
            groups[key]=dict(frames=0,blocks=0,energy=0.,peak=0.,nonzero=0,sha=hashlib.sha256(),first_qpc=r[3],last_qpc=r[3])
        o=groups[key];assert r[3]>=o['last_qpc'];o['last_qpc']=r[3]
        o['frames']+=r[1];o['blocks']+=1
        o['energy']+=float(np.dot(v.astype('float64'),v.astype('float64')))
        o['peak']=max(o['peak'],float(np.max(np.abs(v))))
        o['nonzero']+=int(np.count_nonzero(np.abs(v)>1e-9));o['sha'].update(payload)
summary=json.loads((root/'summary.json').read_text())
assert seq==summary['blocks']
objects=[]
for (host,oid,kind),o in groups.items():
    objects.append(dict(host=hex(host),object_id=oid,type=kind,frames=o['frames'],blocks=o['blocks'],
        rms=(o['energy']/o['frames'])**.5,peak=o['peak'],nonzero_samples=o['nonzero'],
        audio_sha256=o['sha'].hexdigest(),first_qpc=o['first_qpc'],last_qpc=o['last_qpc']))
result=dict(status='PASS_AUDIO_RECORD_INTEGRITY',capture=summary,objects=objects,
    dynamic_objects=sum(o['type']==1 for o in objects),audible_dynamic_objects=sum(o['type']==1 and o['nonzero_samples']>0 for o in objects),
    audible_static_objects=sum(o['type']!=1 and o['nonzero_samples']>0 for o in objects))
(root/'audio-inventory.json').write_text(json.dumps(result,indent=2))
print(json.dumps({k:v for k,v in result.items() if k not in ('objects','capture')}))
