"""Classify post-encoder bytes without labeling arbitrary PCM as Dolby Atmos."""
import collections, hashlib, json, struct, sys
from pathlib import Path

root=Path(sys.argv[1]);header=struct.Struct('<IIQqQIIIIiIQ');assert header.size==64
summary=json.loads((root/'summary.json').read_text())
assert summary['status']==2 and summary['error']==0 and summary['original_bytes_restored']
assert not any(summary[k] for k in ('dropped','invalid','failures'))
sizes=collections.Counter();capacities=collections.Counter();inputs=collections.Counter();qpcs=[];seq=0;data=bytearray()
with (root/'encoder-output.bin').open('rb') as f:
    while raw:=f.read(header.size):
        assert len(raw)==header.size
        r=header.unpack(raw);assert r[0]==0x42595445 and r[2]==seq and r[1]==r[7] and r[1]<=r[6] and r[1]<=65536 and r[9]>=0
        body=f.read(r[1]);assert len(body)==r[1]
        data.extend(body);seq+=1;sizes[r[1]]+=1;capacities[r[6]]+=1;inputs[r[5]]+=1;qpcs.append(r[3])
assert seq==summary['records']==summary['calls'] and len(data)==summary['bytes']
patterns={'iec61937-le16':bytes.fromhex('72f81f4e'),'iec61937-be16':bytes.fromhex('f8724e1f'),
          'iec61937-le16-in-low32':bytes.fromhex('72f800001f4e0000'),
          'iec61937-le16-in-high32':bytes.fromhex('000072f800001f4e')}
hits={};burst_types=collections.Counter();examples=[]
for name,pattern in patterns.items():
    pos=0;count=0
    while (pos:=data.find(pattern,pos))>=0:
        count+=1
        if name=='iec61937-le16' and pos+8<=len(data):
            pc,pd=struct.unpack_from('<HH',data,pos+4);burst_types[hex(pc&31)]+=1
            if len(examples)<8:examples.append(dict(offset=pos,pc=pc,pd=pd,head=data[pos:pos+32].hex()))
        pos+=len(pattern)
    hits[name]=count
result=dict(integrity_passed=True,records=seq,payload_bytes=len(data),sizes=dict(sizes),capacities=dict(capacities),input_frames=dict(inputs),
    zero_output_records=sizes[0],sha256=hashlib.sha256(data).hexdigest(),first128=data[:128].hex(),patterns=hits,burst_types=dict(burst_types),examples=examples,
    classification='Unclassified; burst framing alone does not establish Atmos metadata or Android compatibility.')
(root/'encoder-payload.raw').write_bytes(data)
(root/'encoded-inspection.json').write_text(json.dumps(result,indent=2),encoding='utf8')
print(json.dumps(result,indent=2))
