"""Decode real engine records. --controlled requires four exact 5-second test objects.
Game mode reports observed values only, not inferred original game world coordinates.
"""
import argparse
import array
import collections
import csv
import json
import math
import pathlib
import struct

p = argparse.ArgumentParser()
p.add_argument('directory', type=pathlib.Path)
p.add_argument('--controlled', action='store_true')
a = p.parse_args()
root = a.directory.resolve()
header = struct.Struct('<IIQqIIfffffIQ')
groups = collections.defaultdict(list)
sequence = 0
with (root / 'global-objects.bin').open('rb') as f, (root / 'objects.csv').open('w', newline='') as meta:
    writer = csv.writer(meta)
    writer.writerow(['sequence','qpc','host','engine_object_id','type','frames','x','y','z','volume','call_gain','tid'])
    while b := f.read(header.size):
        assert len(b) == header.size
        r = header.unpack(b)
        assert r[0] == 0x474f424a and 0 < r[1] <= 2048 and r[12] == sequence
        sequence += 1
        samples = array.array('f'); samples.frombytes(f.read(r[1] * 4))
        assert len(samples) == r[1] and all(math.isfinite(v) for v in samples)
        assert all(math.isfinite(v) for v in r[6:11])
        groups[(r[2],r[4],r[5])].append((r,samples))
        writer.writerow([r[12],r[3],hex(r[2]),r[4],r[5],r[1],*r[6:11],r[11]])

summary = json.loads((root / 'summary.json').read_text())
assert sequence == summary['blocks']
objects = []
matches = []
for number, ((host,object_id,kind), blocks) in enumerate(groups.items()):
    values = array.array('f')
    for _, samples in blocks: values.extend(samples)
    energy = sum(v*v for v in values)
    xyz = [b[0][6:9] for b in blocks]
    row = dict(host=hex(host), engine_object_id=object_id, type=kind, blocks=len(blocks),
               frames=len(values), seconds_at_48k=len(values)/48000,
               rms=math.sqrt(energy/max(1,len(values))), peak=max(map(abs,values), default=0),
               nonzero_samples=sum(abs(v)>1e-9 for v in values),
               xyz_min=[min(v[i] for v in xyz) for i in range(3)],
               xyz_max=[max(v[i] for v in xyz) for i in range(3)],
               distinct_xyz=len(set(xyz)), first_qpc=blocks[0][0][3], last_qpc=blocks[-1][0][3])
    if a.controlled and 240000 <= len(values) <= 242048:
        source_blocks = [(r,v) for r,v in blocks if r[1] == 480]
        extra_blocks = [(r,v) for r,v in blocks if r[1] != 480]
        assert all(all(v == 0 for v in samples) for _,samples in extra_blocks), 'Nonzero unexpected engine block'
        signal = array.array('f')
        for _,samples in source_blocks: signal.extend(samples)
        assert len(signal) == 240000
        fit = []
        for variant,index,freq in [(0,0,523.25),(0,1,783.99),(1,0,623.25),(1,1,883.99)]:
            cross = ref_energy = energy_fit = 0.
            for n in range(4800,len(signal)-4800):
                reference = .2*math.sin(2*math.pi*freq*n/48000)
                cross += reference*signal[n]; ref_energy += reference**2; energy_fit += signal[n]**2
            gain = cross/ref_energy
            residual = math.sqrt(max(0,energy_fit-cross*cross/ref_energy)/energy_fit)
            fit.append((residual,variant,index,freq,gain))
        residual,variant,index,freq,gain=min(fit)
        assert residual<1e-4, row
        frame=0; error=0.
        for bi,(r,samples) in enumerate(source_blocks):
            t=frame/48000; phase=index+variant*2
            expected=(math.sin(t*1.1+phase),.5*math.sin(t*.7+phase),-math.cos(t*1.1+phase))
            if bi and frame < 240000: error=max(error,*(abs(v-e) for v,e in zip(r[6:9],expected)))
            frame+=r[1]
        assert error<1e-6
        row.update(source_variant=variant,source_object_index=index,frequency_hz=freq,
                   audio_fit_relative_rms_error=residual,fitted_gain=gain,max_xyz_error=error,
                   submitted_source_frames=240000,extra_engine_silence_frames=len(values)-240000)
        matches.append((variant,index))
    # WAV export is for inspection; QPC/csv, not concatenation, defines timing.
    name=f'object-{number:03d}-type-{kind}.wav'
    payload=values.tobytes()
    (root/name).write_bytes(struct.pack('<4sI4s4sIHHIIHH4sI',b'RIFF',len(payload)+36,b'WAVE',b'fmt ',16,3,1,48000,192000,4,32,b'data',len(payload))+payload)
    row['wav']=name; objects.append(row)
if a.controlled:
    assert sorted(matches)==[(0,0),(0,1),(1,0),(1,1)], matches
assert summary['original_bytes_restored'] and summary['status']==2 and summary['error']==0
assert summary['dropped_full']==summary['dropped_busy']==summary['invalid']==0
result=dict(status='PASS_CONTROLLED' if a.controlled else 'OBSERVATION_ONLY',
            capture=summary,objects=objects,
            dynamic_nonzero_objects=sum(o['type']==1 and o['nonzero_samples']>0 for o in objects),
            dynamic_moving_nonzero_objects=sum(o['type']==1 and o['nonzero_samples']>0 and o['distinct_xyz']>1 for o in objects),
            limits=['Engine listener-relative spatial metadata, not game world coordinates',
                    'No automatic PID attribution; game isolation/control required',
                    '48kHz interpretation is version/path-specific; verify on other endpoints',
                    'Host/object IDs can be reused across stream lifetimes'])
backend_file=root/'backend.json'
if backend_file.exists(): result['backend']=json.loads(backend_file.read_text())
if result.get('backend',{}).get('dolby'):
    result['coordinate_warning']='XYZ here are raw packet fields, not reconstructed current positions. Private position-update bit 0x4 must be honored; absent update does not mean zero position. Use metadata-verification.json and position-state.csv where available.'
result['dynamic_objects']=sum(o['type']==1 for o in objects)
result['static_objects']=sum(o['type']!=1 for o in objects)
result['static_nonzero_objects']=sum(o['type']!=1 and o['nonzero_samples']>0 for o in objects)
if not a.controlled:
    if result['dynamic_moving_nonzero_objects']:
        result['classification']='OBSERVED_NONZERO_DYNAMIC_AUDIO_AND_CHANGING_ENGINE_XYZ'
    elif result['dynamic_nonzero_objects']:
        result['classification']='OBSERVED_NONZERO_DYNAMIC_AUDIO_WITHOUT_CHANGING_XYZ'
    elif result['static_nonzero_objects']:
        result['classification']='OBSERVED_STATIC_BED_AUDIO_ONLY_DYNAMIC_SILENT'
    else: result['classification']='NO_NONZERO_AUDIO_OBSERVED'
(root/'analysis.json').write_text(json.dumps(result,indent=2))
print(json.dumps(result,indent=2))
