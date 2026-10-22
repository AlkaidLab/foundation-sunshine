"""Small synthetic decoder regression: a volume-only zero payload must retain prior XYZ."""
import csv
import json
import pathlib
import struct
import subprocess
import sys
import tempfile

script=pathlib.Path(__file__).with_name('verify_metadata.py')
header=struct.Struct('<IIQqIIfffffIQ')
trace=struct.Struct('<6IQQq24s180s180s180s28s')
props=struct.Struct('<IIIIffff')
with tempfile.TemporaryDirectory(prefix='spatial-position-cache-') as folder:
    root=pathlib.Path(folder)
    rows=[(7,(.25,-.5,1.75)),(2,(0.,0.,0.)),(4,(-1.,.5,-.75))]
    with (root/'property-trace.bin').open('wb') as f:
        for seq,(flags,xyz) in enumerate(rows):
            packet=props.pack(73,480,1,flags,*xyz,.5)+bytes(148)
            f.write(header.pack(0x474f424a,480,42,1000+seq*100000,73,1,*xyz,.5,1.,99,seq))
            f.write(trace.pack(0x4d545243,15,32,480,0,0,4096,8192,900+seq*100000,bytes(24),packet,packet,packet,bytes(28)))
    (root/'summary.json').write_text(json.dumps(dict(blocks=3,status=2,error=0,original_bytes_restored=True,dropped_full=0,dropped_busy=0,invalid=0,qpc_frequency=10000000)))
    (root/'backend.json').write_text(json.dumps(dict(extended_calls=0)))
    subprocess.run([sys.executable,str(script),str(root)],check=True,capture_output=True)
    with (root/'position-state.csv').open() as f: decoded=list(csv.DictReader(f))
    assert decoded[1]['state']=='CACHED_OBSERVED_POSITION'
    assert tuple(float(decoded[1][k]) for k in ('x','y','z'))==(.25,-.5,1.75)
    report=json.loads((root/'metadata-verification.json').read_text())
    assert report['dynamic_position_update_objects']==1 and report['dynamic_observed_changing_position_objects']==1
print('PASS: initial nonzero XYZ + zero-filled volume-only update retains cached XYZ; later position update changes it')
