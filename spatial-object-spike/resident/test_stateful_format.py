"""Small disk-v2 fixtures: chunk-boundary caching and corruption rejection."""
import json, math, struct, subprocess, sys, tempfile
from pathlib import Path
from verify_stateful import HEADER

with tempfile.TemporaryDirectory(prefix='spatial-state-format-') as temp:
    root=Path(temp);nan=math.nan
    # flags, state, generation, type, current XYZ, raw XYZ
    events=[(2,0,1,1,(nan,nan,nan),(0,0,0)),
            (6,1,1,1,(3,-2,7),(3,-2,7)),
            (2,2,1,1,(3,-2,7),(0,0,0)),
            (4,1,1,1,(5,-2,7),(5,-2,7)),
            (8,2,1,1,(5,-2,7),(0,0,0)),
            (2,0,2,1,(nan,nan,nan),(0,0,0)),
            (7,1,3,1,(9,1,2),(9,1,2)),
            (2,4,4,2,(nan,nan,nan),(0,0,0))]
    blocks=[]
    for seq,(flags,state,gen,typ,pos,raw) in enumerate(events):
        blocks.append(HEADER.pack(0x324f424a,480,1,1000000+seq*100000,42,typ,*pos,.4,1.,123,seq,
                                  2,flags,state,1,0,gen,*raw,0)+struct.pack('<480f',*([.1]*480)))
    (root/'global-objects.bin').write_bytes(b''.join(blocks[:2]))
    second=root/'global-objects-0001.bin';second.write_bytes(b''.join(blocks[2:]))
    (root/'summary.json').write_text(json.dumps(dict(status=2,error=0,original_bytes_restored=True,
        dropped_full=0,dropped_busy=0,invalid=0,blocks=8,frames=3840,qpc_frequency=10000000)))
    (root/'position-summary.json').write_text(json.dumps(dict(stateful=True,evictions=0,unknown=2,updated=3,cached=2)))
    (root/'backend.json').write_text(json.dumps(dict(extended_calls=0,encoder_ready=1)))
    command=[sys.executable,str(Path(__file__).with_name('verify_stateful.py')),str(root)]
    good=subprocess.run(command,capture_output=True,text=True)
    assert good.returncode==0,good.stderr
    result=json.loads((root/'state-verification.json').read_text())
    assert result['chunks']==2 and result['chunk_boundaries'][1]['state']==2
    assert result['late_attach_unknown_lifetimes']==2 and result['late_attach_recovered_lifetimes']==1
    bad=bytearray(second.read_bytes());struct.pack_into('<f',bad,32,0) # overwrite cached x
    second.write_bytes(bad)
    assert subprocess.run(command,capture_output=True).returncode!=0
    second.write_bytes(b''.join(blocks[2:])[:-1])
    assert subprocess.run(command,capture_output=True).returncode!=0
print('PASS v2 replay / cached position across chunks / late-attach recovery / corrupted position and truncation rejected')
