"""Export one actual post-creation object excerpt, with incrementally reconstructed XYZ."""
import csv
import json
import pathlib
import struct
import sys
import numpy as np

root=pathlib.Path(sys.argv[1]).resolve()
meta=json.loads((root/'metadata-verification.json').read_text())
audio=json.loads((root/'audio-inventory.json').read_text())
lookup={(o['host'],o['object_id'],o['type']):o for o in audio['objects']}
candidates=[o for o in meta['objects'] if o['type']==1 and o['stages']['ingress']['flags_histogram'].get('0x7',0)>0 and o['observed_position_changes']>0 and lookup[(o['host'],o['object_id'],1)]['nonzero_samples']>0]
target=max(candidates,key=lambda o:lookup[(o['host'],o['object_id'],1)]['peak'])
key=(int(target['host'],16),target['object_id'],1)
h=struct.Struct('<IIQqIIfffffIQ');t=struct.Struct('<6IQQq24s180s180s180s28s')
blocks=[];position=None
with (root/'global-objects.bin').open('rb') as f,(root/'property-trace.bin').open('rb') as m:
    while raw:=f.read(64):
        r=h.unpack(raw);payload=f.read(r[1]*4)
        assert m.read(64)==raw
        mt=m.read(640)
        if (r[2],r[4],r[5])!=key:continue
        p=t.unpack(mt);assert p[1]&15==15
        flags=struct.unpack_from('<I',p[10],12)[0]
        if flags&1:position=None
        if flags&4:position=struct.unpack_from('<fff',p[10],16)
        values=np.frombuffer(payload,dtype='<f4')
        energy=float(np.dot(values.astype('float64'),values.astype('float64')))
        blocks.append((r,payload,position,flags,energy))
freq=meta['capture']['qpc_frequency']
best=max(blocks,key=lambda b:b[4]);start=max(blocks[0][0][3],best[0][3]-5*freq);stop=start+10*freq
selected=[b for b in blocks if start<=b[0][3]<stop]
assert selected and all(b[2] is not None for b in selected)
payload=b''.join(b[1] for b in selected);frames=sum(b[0][1] for b in selected)
wav=root/'proof-object-audio.wav'
wav.write_bytes(struct.pack('<4sI4s4sIHHIIHH4sI',b'RIFF',len(payload)+36,b'WAVE',b'fmt ',16,3,1,48000,192000,4,32,b'data',len(payload))+payload)
with (root/'proof-object-position.csv').open('w',newline='') as f:
    w=csv.writer(f);w.writerow(['sequence','qpc','relative_seconds','object_id','frames','position_update','x','y','z','volume','call_gain','wav_frame_offset'])
    offset=0
    for r,values,xyz,flags,_ in selected:
        w.writerow([r[12],r[3],(r[3]-selected[0][0][3])/freq,r[4],r[1],bool(flags&4),*xyz,r[9],r[10],offset]);offset+=r[1]
positions=[b[2] for b in selected]
proof=dict(status='PASS_REAL_GAME_AUDIO_WITH_MATCHED_CHANGING_POSITION',host=target['host'],object_id=target['object_id'],
    excerpt_frames=frames,excerpt_seconds_at_48k=frames/48000,blocks=len(selected),
    position_update_blocks=sum(bool(b[3]&4) for b in selected),distinct_reconstructed_xyz=len(set(positions)),
    xyz_min=[min(p[i] for p in positions) for i in range(3)],xyz_max=[max(p[i] for p in positions) for i in range(3)],
    first_xyz=positions[0],last_xyz=positions[-1],audio_file=wav.name,position_file='proof-object-position.csv',
    notes=['Real captured mono audio, before separate object volume and call gain',
           'XYZ are listener-relative engine coordinates, not game world coordinates or a named character identity',
           'Object IDs may represent pooled/reused voices; no semantic emitter labeling claimed'])
proof['nonzero_audio_blocks_with_nonzero_position']=sum(bool(np.any(np.abs(np.frombuffer(b[1],dtype='<f4'))>1e-9)) and any(v!=0 for v in b[2]) for b in selected)
proof['nonzero_audio_blocks_with_explicit_position_update']=sum(bool(np.any(np.abs(np.frombuffer(b[1],dtype='<f4'))>1e-9)) and bool(b[3]&4) for b in selected)
assert proof['distinct_reconstructed_xyz']>1
assert proof['nonzero_audio_blocks_with_nonzero_position']>0 and proof['nonzero_audio_blocks_with_explicit_position_update']>0
(root/'proof-summary.json').write_text(json.dumps(proof,indent=2));print(json.dumps(proof,indent=2))
