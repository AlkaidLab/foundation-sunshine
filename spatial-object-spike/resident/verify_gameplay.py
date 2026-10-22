"""Second-pass gameplay audit of actual records and exported audio; no test-signal assumptions."""
import array
import collections
import hashlib
import itertools
import json
import math
import pathlib
import struct
import sys

root=pathlib.Path(sys.argv[1]).resolve()
a=json.loads((root/'analysis.json').read_text())
header=struct.Struct('<IIQqIIfffffIQ')
groups=collections.defaultdict(list)
sequence=0
with (root/'global-objects.bin').open('rb') as f:
    while raw:=f.read(64):
        assert len(raw)==64
        r=header.unpack(raw)
        assert r[0]==0x474f424a and r[12]==sequence
        assert r[1]==480, f'Unexpected frame size: {r[1]}'
        payload=f.read(r[1]*4)
        assert len(payload)==r[1]*4
        groups[(r[2],r[4],r[5])].append(r)
        sequence+=1
assert sequence==a['capture']['blocks']
assert not any(a['capture'][k] for k in ['dropped_full','dropped_busy','invalid','error'])
assert a['capture']['original_bytes_restored'] and a['capture']['status']==2
audible=[o for o in a['objects'] if o['type']==1 and o['nonzero_samples']]
signals={}
checks=[]
for o in audible:
    raw=(root/o['wav']).read_bytes()
    assert raw[:4]==b'RIFF' and raw[8:12]==b'WAVE' and raw[36:40]==b'data'
    values=array.array('f'); values.frombytes(raw[44:]); assert len(values)==o['frames']
    key=(int(o['host'],16),o['engine_object_id'],o['type'])
    blocks=groups[key]
    assert sum(r[1] for r in blocks)==o['frames']
    assert all(r[3]<s[3] for r,s in zip(blocks,blocks[1:]))
    # Always preserve zero positions as observations, never invent source XYZ.
    xyz=set(r[6:9] for r in blocks)
    delta_ms=[(s[3]-r[3])*1000/a['capture']['qpc_frequency'] for r,s in zip(blocks,blocks[1:])]
    sha=hashlib.sha256(raw[44:]).hexdigest()
    signals[o['engine_object_id']]=values
    checks.append(dict(engine_object_id=o['engine_object_id'],wav=o['wav'],sha256=sha,
        frames=len(values),blocks=len(blocks),peak=o['peak'],rms=o['rms'],
        nonzero_samples=o['nonzero_samples'],xyz_values=sorted(xyz),
        min_interblock_ms=min(delta_ms),max_interblock_ms=max(delta_ms),
        qpc_span_seconds=(blocks[-1][3]-blocks[0][3])/a['capture']['qpc_frequency'],
        volume_min=min(r[9] for r in blocks),volume_max=max(r[9] for r in blocks),
        distinct_volume_count=len(set(r[9] for r in blocks))))
assert len({o['sha256'] for o in checks})==len(checks), 'Duplicate dynamic payloads'
correlations=[]
for (id_a,x),(id_b,y) in itertools.combinations(signals.items(),2):
    assert len(x)==len(y)
    n=len(x); sx=sum(x); sy=sum(y)
    cov=sum(v*w for v,w in zip(x,y))-sx*sy/n
    vx=sum(v*v for v in x)-sx*sx/n; vy=sum(v*v for v in y)-sy*sy/n
    correlations.append(dict(object_a=id_a,object_b=id_b,pearson=cov/math.sqrt(vx*vy)))
result=dict(status='PASS_RECORD_AND_AUDIO_INTEGRITY',
    classification=a['classification'],capture=a['capture'],backend=a.get('backend'),
    dynamic_total=a['dynamic_objects'],dynamic_audible=len(audible),
    dynamic_with_observed_xyz_changes=a['dynamic_moving_nonzero_objects'],
    dynamic_audio=checks,pairwise_audio_correlations=correlations,
    source_evidence='Game-only capture; Nioh3 is the sole active non-system render session in before/after audit. No automatic engine-stream-to-PID binding.',
    conclusion='Independent nonzero dynamic audio observed; useful moving XYZ NOT demonstrated if all coordinates are zero.',
    limits=['Not a game-world coordinate ground-truth test',
            'QPC monotonicity, counters and frame lengths are checked; game internal scheduling is not instrumented',
            'Different waveforms do not prove separate physical sound emitters; they can be submixes'])
(root/'gameplay-verification.json').write_text(json.dumps(result,indent=2))
print(json.dumps(result,indent=2))
