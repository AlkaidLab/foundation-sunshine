"""Compare matched raw cross-process properties, sanitized properties and encoder submission."""
import collections
import csv
import json
import math
import pathlib
import struct
import sys

root=pathlib.Path(sys.argv[1]).resolve()
summary=json.loads((root/'summary.json').read_text())
record=struct.Struct('<IIQqIIfffffIQ')
trace=struct.Struct('<6IQQq24s180s180s180s28s')
props=struct.Struct('<IIIIffff')
assert trace.size==640
groups={}
position_state={}
position_observations=collections.Counter()
position_changes=collections.Counter()
last_observed_xyz={}
bound=unbound=0
sizes=collections.Counter();trace_flags=collections.Counter();field_diffs=collections.Counter()
with (root/'property-trace.bin').open('rb') as f,(root/'property-stages.csv').open('w',newline='') as out,(root/'position-state.csv').open('w',newline='') as positions:
    w=csv.writer(out)
    pw=csv.writer(positions)
    pw.writerow(['sequence','qpc','host','object_id','type','source_flags','state','x','y','z'])
    w.writerow(['sequence','qpc','host','object_id','type','binding_flags','source_bytes','source_flags','sanitized_flags','submitted_flags',
                'source_x','source_y','source_z','sanitized_x','sanitized_y','sanitized_z','submitted_x','submitted_y','submitted_z',
                'source_volume','sanitized_volume','submitted_volume','sanitize_result','source_ptr','sanitized_ptr'])
    n=0
    while raw:=f.read(record.size):
        assert len(raw)==record.size
        r=record.unpack(raw);t=trace.unpack(f.read(trace.size));assert r[12]==n and t[0]==0x4d545243
        assert r[0]==0x474f424a
        n+=1
        trace_flags[t[1]]+=1
        if t[1]&15!=15:
            unbound+=1
            continue
        bound+=1;sizes[t[2]]+=1
        assert t[2] in (32,180) and t[4]==0 and t[8]<=r[3]
        a,b,c=[props.unpack(p[:32]) for p in t[10:13]]
        assert a[0]==b[0]==c[0]==r[4] and b[2]==c[2]==r[5]
        assert c[4:8]==r[6:10]
        assert all(math.isfinite(v) for p in (a,b,c) for v in p[4:8])
        key=(r[2],r[4],r[5])
        # Private flag semantics verified in matching audioeng disassembly.
        # Mid-stream packets lacking bit 0x4 do NOT specify an absolute position.
        if a[3]&1:position_state.pop(key,None)
        if a[3]&4:
            position_state[key]=a[4:7]
            if key in last_observed_xyz and last_observed_xyz[key]!=a[4:7]:position_changes[key]+=1
            last_observed_xyz[key]=a[4:7]
            state='POSITION_UPDATE'
        elif key in position_state:state='CACHED_OBSERVED_POSITION'
        elif a[3]&1:
            position_state[key]=(0.,0.,0.)
            state='CREATED_WITHOUT_EXPLICIT_POSITION_DEFAULT_ORIGIN'
        else:state='UNKNOWN_INITIAL_POSITION'
        position_observations[(key,state)]+=1
        pw.writerow([r[12],r[3],hex(r[2]),r[4],r[5],hex(a[3]),state,*position_state.get(key,('','',''))])
        if a[3]&8:position_state.pop(key,None)
        if key not in groups:
            groups[key]=dict(blocks=0,frames=0,xyz=[set(),set(),set()],flags=[collections.Counter(),collections.Counter(),collections.Counter()],
                extension_dwords={},input_sizes=collections.Counter(),position_bit=[0,0,0],extended_bit=[0,0,0],
                sanitize_position_changes=0,submit_position_changes=0,max_sanitize_to_submit_us=0)
        g=groups[key];g['blocks']+=1;g['frames']+=r[1];g['input_sizes'][t[2]]+=1
        for i,p in enumerate((a,b,c)):
            g['xyz'][i].add(p[4:7]);g['flags'][i][p[3]]+=1
            g['position_bit'][i]+=bool(p[3]&4);g['extended_bit'][i]+=bool(p[3]&16)
        g['sanitize_position_changes']+=a[4:7]!=b[4:7]
        g['submit_position_changes']+=b[4:7]!=c[4:7]
        g['max_sanitize_to_submit_us']=max(g['max_sanitize_to_submit_us'],(r[3]-t[8])*1e6/summary['qpc_frequency'])
        for offset in range(0,t[2],4):
            if t[10][offset:offset+4]!=t[11][offset:offset+4]:field_diffs['sanitize:'+hex(offset)]+=1
        for offset in range(0,180,4):
            if t[11][offset:offset+4]!=t[12][offset:offset+4]:field_diffs['submit:'+hex(offset)]+=1
        for offset in range(32,180,4):
            value=struct.unpack_from('<I',t[12],offset)[0]
            if value:
                d=g['extension_dwords'].setdefault(hex(offset),dict(nonzero_blocks=0,min_u32=value,max_u32=value))
                d['nonzero_blocks']+=1;d['min_u32']=min(d['min_u32'],value);d['max_u32']=max(d['max_u32'],value)
        w.writerow([r[12],r[3],hex(r[2]),r[4],r[5],t[1],t[2],hex(a[3]),hex(b[3]),hex(c[3]),
                    *a[4:7],*b[4:7],*c[4:7],a[7],b[7],c[7],t[4],hex(t[6]),hex(t[7])])

summary=json.loads((root/'summary.json').read_text());backend=json.loads((root/'backend.json').read_text())
assert n==summary['blocks'] and summary['status']==2 and summary['error']==0 and summary['original_bytes_restored']
assert summary['dropped_full']==summary['dropped_busy']==summary['invalid']==0
objects=[]
for (host,oid,kind),g in groups.items():
    stages={name:dict(distinct_xyz=len(g['xyz'][i]),nonzero_xyz_count=sum(any(v!=0 for v in xyz) for xyz in g['xyz'][i]),
        xyz_examples=sorted(g['xyz'][i])[:4],flags_histogram={hex(k):v for k,v in g['flags'][i].items()},
        position_update_bit_blocks=g['position_bit'][i],extended_properties_bit_blocks=g['extended_bit'][i]) for i,name in enumerate(('ingress','sanitized','submitted'))}
    objects.append(dict(host=hex(host),object_id=oid,type=kind,blocks=g['blocks'],frames=g['frames'],input_sizes=dict(g['input_sizes']),
        stages=stages,sanitize_position_changes=g['sanitize_position_changes'],submit_position_changes=g['submit_position_changes'],
        extension_dwords=g['extension_dwords'],max_sanitize_to_submit_us=g['max_sanitize_to_submit_us'],
        position_state_counts={state:count for (k,state),count in position_observations.items() if k==(host,oid,kind)},
        observed_position_changes=position_changes[(host,oid,kind)]))
dynamic=[o for o in objects if o['type']==1]
result=dict(status='PASS_MATCHED_PROPERTY_CHAIN' if not unbound and bound else 'INCOMPLETE_PROPERTY_BINDING',capture=summary,backend=backend,
    records=n,bound_records=bound,unbound_records=unbound,source_sizes=dict(sizes),binding_flags=dict(trace_flags),
    all_field_changes=dict(field_diffs),dynamic_objects=len(dynamic),
    dynamic_ingress_nonzero_xyz_objects=sum(o['stages']['ingress']['nonzero_xyz_count']>0 for o in dynamic),
    dynamic_sanitizer_changed_xyz_objects=sum(o['sanitize_position_changes']>0 for o in dynamic),
    dynamic_submit_changed_xyz_objects=sum(o['submit_position_changes']>0 for o in dynamic),objects=objects,
    dynamic_position_update_objects=sum(o['stages']['ingress']['position_update_bit_blocks']>0 for o in dynamic),
    dynamic_observed_changing_position_objects=sum(o['observed_position_changes']>0 for o in dynamic),
    limits=['Ingress is audiodg receiving process-block properties, not a hook in the game SetPosition API',
            'Flag bit 0x4 gates XYZ copying and 0x10 gates extra properties in matching Windows HRTF disassembly; private ABI not public contract',
            'No metadata-reader traffic can only be concluded when extended_calls is zero',
            'Raw extension words are not interpreted as position without documented or verified semantics'])
(root/'metadata-verification.json').write_text(json.dumps(result,indent=2))
print(json.dumps({k:v for k,v in result.items() if k not in ('objects','limits')}))
