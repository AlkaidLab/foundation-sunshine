"""Compare operator-cued still/motion windows without claiming input telemetry."""
import array
import collections
import json
import math
import pathlib
import struct
import sys

root=pathlib.Path(sys.argv[1]).resolve()
summary=json.loads((root/'summary.json').read_text())
header=struct.Struct('<IIQqIIfffffIQ')
windows={'still_cue':(0.,8.),'motion_cue':(13.,28.)}
stats={name:collections.defaultdict(lambda:dict(frames=0,energy=0.,peak=0.,nonzero=0,xyz=set(),volumes=set())) for name in windows}
first=None
with (root/'global-objects.bin').open('rb') as f:
    while raw:=f.read(64):
        r=header.unpack(raw); payload=f.read(r[1]*4)
        if first is None:first=r[3]
        if r[5]!=1:continue
        t=(r[3]-first)/summary['qpc_frequency']
        phase=next((name for name,(start,stop) in windows.items() if start<=t<stop),None)
        if phase is None:continue
        o=stats[phase][(r[2],r[4])]
        values=array.array('f');values.frombytes(payload)
        o['frames']+=r[1];o['energy']+=sum(v*v for v in values)
        o['peak']=max(o['peak'],max(map(abs,values),default=0))
        o['nonzero']+=sum(abs(v)>1e-9 for v in values)
        o['xyz'].add(r[6:9]);o['volumes'].add(r[9])

phases={}
for name,groups in stats.items():
    objects=[]
    for (host,oid),o in groups.items():
        objects.append(dict(host=hex(host),object_id=oid,frames=o['frames'],
            rms=math.sqrt(o['energy']/o['frames']),peak=o['peak'],nonzero_samples=o['nonzero'],
            xyz_values=sorted(o['xyz']),distinct_volume_count=len(o['volumes']),
            volume_min=min(o['volumes']),volume_max=max(o['volumes'])))
    phases[name]=dict(window_seconds=windows[name],dynamic_objects=len(objects),
        audible_dynamic_objects=sum(o['nonzero_samples']>0 for o in objects),
        changing_xyz_objects=sum(len(o['xyz_values'])>1 for o in objects),
        nonzero_xyz_objects=sum(any(any(v!=0 for v in xyz) for xyz in o['xyz_values']) for o in objects),
        objects=objects)
result=dict(status='PASS_PHASE_AUDIT',phases=phases,
    conclusion='ALL_DYNAMIC_XYZ_ZERO_IN_BOTH_CUED_PHASES' if all(p['nonzero_xyz_objects']==0 for p in phases.values()) else 'INSPECT_PHASE_XYZ',
    limits=['Windows relative to first captured engine block; transition region excluded',
            'Human motion was requested near collector second 10; keyboard/controller input was not recorded',
            'This cannot independently prove that the player moved or identify missing metadata elsewhere'])
(root/'motion-verification.json').write_text(json.dumps(result,indent=2))
print(json.dumps({k:v for k,v in result.items() if k!='phases'}))
print(json.dumps({name:{k:v for k,v in phase.items() if k!='objects'} for name,phase in phases.items()}))
