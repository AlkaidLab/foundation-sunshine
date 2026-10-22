"""Independent synthetic v4 wire/PCM oracle and compiled reader integration checks.
No process access, playback, elevation, or promotion of old post-Submit captures.
"""
import argparse,collections,csv,hashlib,json,math,subprocess
from pathlib import Path
import numpy as np
import struct
WIRE=struct.Struct('<IIQqIIfffffIQIIIIQQQQQQQIIIiII')
assert WIRE.size==160


def load(path):return json.loads(path.read_text(encoding='utf-8-sig'))


def unpack(path):
    events=[]
    with path.open('rb') as stream:
        while raw:=stream.read(WIRE.size):
            if len(raw)!=160:raise ValueError('short v4 header')
            h=list(WIRE.unpack(raw));pcm=stream.read(h[1]*4)
            if len(pcm)!=h[1]*4:raise ValueError('short v4 PCM')
            events.append((h,pcm))
    return events


def raw_oracle(events):
    pattern=np.arange(480,dtype=np.float32)*np.float32(.001)
    pending={};closed=set();opened={};calls=0
    for sequence,(h,pcm) in enumerate(events):
        if h[0]!=0x3451424f or h[12]!=sequence or h[13]!=4 or h[17]!=0 or h[29]!=0:raise ValueError('wire identity')
        kind=h[14];domain=h[21];key=(domain,h[22],h[23])
        if kind==1:
            if domain in opened or h[16]!=4:raise ValueError('observed domain')
            opened[domain]=tuple(h[18:21])
        elif kind==4:
            if domain in closed or key in pending or h[16]!=1 or h[27]!=-2147483638:raise ValueError('pre-call snapshot state')
            if h[1]!=480 or h[2]!=77 or h[9]!=.5 or h[10]!=.25:raise ValueError('post-call metadata leaked')
            expected_x={1:1.,2:9.,3:0.}[domain]
            if h[6]!=expected_x or h[15]!=(2 if domain==3 else 6):raise ValueError('post-call XYZ/flags leaked')
            if pcm!=pattern.tobytes():raise ValueError('post-gain PCM leaked into claimed pre-call snapshot')
            pending[key]=False;calls+=1
        elif kind==5:
            if key not in pending or pending[key] or h[27]!=0 or h[16]!=2:raise ValueError('failed/unmatched result')
            pending[key]=True
        elif kind==2:closed.add(domain)
    if calls!=4 or not all(pending.values()) or closed!={1,2,3} or opened[1]!=opened[3] or opened[1]==opened[2]:
        raise ValueError('domain reuse/status coverage')
    return hashlib.sha256(pattern.tobytes()*4).hexdigest()


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
    args=ap.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    exe=(args.build/'quantum-v4-verify.exe').resolve(strict=True);fixture=(args.build/'fixture').resolve(strict=True)
    original=(fixture/'events.bin').read_bytes();events=unpack(fixture/'events.bin');golden=raw_oracle(events)
    checks=[]

    def run(name,root,okay):
        output=(args.output/(name+'-proof')).resolve()
        result=subprocess.run([str(exe),str(root.resolve()),str(output)],capture_output=True,text=True,timeout=10,creationflags=subprocess.CREATE_NO_WINDOW)
        (args.output/(name+'.log')).write_text(result.stdout+result.stderr,encoding='utf8')
        if (result.returncode==0)!=okay:raise ValueError(f'{name}: unexpected exit {result.returncode}')
        checks.append(name)
        return output

    proof=run('clean-paired-pre-snapshot',fixture,True);parsed=load(proof/'summary.json')
    if not (parsed['submit_pairs']==4 and parsed['domains_opened']==3 and parsed['domains_closed']==3
            and parsed['completed_quanta']==3 and parsed['object_samples']==1920
            and parsed['pcm_before_in_event_order_sha256']==golden
            and parsed['pcm_sealed_in_completion_order_sha256']==golden
            and not parsed['native_v4_verified']):raise ValueError('C++ parser disagrees with independent raw oracle')
    with (proof/'objects.csv').open(newline='') as stream:rows=list(csv.DictReader(stream))
    if len(rows)!=4 or [int(r['position_state']) for r in rows]!=[1,1,1,0] or not all(math.isnan(float(rows[-1][axis])) for axis in ('x','y','z')):
        raise ValueError('Reset/new-domain cache state disagrees with oracle')
    with (proof/'quanta.csv').open(newline='') as stream:quanta=list(csv.DictReader(stream))
    if [(int(q['domain']),int(q['sample_start']),int(q['objects'])) for q in quanta]!=[(1,0,2),(2,0,1),(3,0,1)]:raise ValueError('independent clocks/membership lost')
    checks.append('independent-PCM-metadata-status-cache-and-domain-oracle')
    bridge=load(args.build/'bridge-selftest.json')
    if not (bridge['passed'] and bridge['samples_gain_applied_exactly_once']==1440 and bridge['unknown_position_objects_quarantined']==1):
        raise ValueError('actual FFmpeg bridge did not validate gain/cache policy')
    checks.append('existing-FFmpeg-bridge-exactly-once-gain')

    def altered(name,modify,tail=b'',stage='before-original-submit',version=4):
        root=args.output/name;root.mkdir();copy=[(h.copy(),bytes(pcm)) for h,pcm in events];modify(copy)
        for index,(h,_) in enumerate(copy):h[12]=index;h[3]=1000+index
        (root/'events.bin').write_bytes(b''.join(WIRE.pack(*h)+pcm for h,pcm in copy)+tail)
        expected=load(fixture/'expected.json');expected['records']=len(copy)
        (root/'expected.json').write_text(json.dumps(expected),encoding='utf8')
        (root/'format.json').write_text(json.dumps(dict(version=version,header_bytes=160,source_kind='synthetic-mutating-originals',pcm_stage=stage)),encoding='utf8')
        run(name,root,False)

    altered('missing-result',lambda e:e.pop(3))
    altered('duplicate-result',lambda e:e.insert(4,(e[3][0].copy(),e[3][1])))
    altered('wrong-call',lambda e:e[3][0].__setitem__(23,99))
    altered('failed-result',lambda e:e[3][0].__setitem__(27,-2147467259))
    altered('post-call-tag',lambda e:e[2][0].__setitem__(16,2))
    altered('lost-epoch',lambda e:e[2][0].__setitem__(17,1))
    altered('cookie-rebound',lambda e:e[2][0].__setitem__(20,999))
    altered('unknown-domain',lambda e:e[2][0].__setitem__(21,999))
    altered('reserved-bits',lambda e:e[2][0].__setitem__(29,1))
    altered('truncated-header',lambda e:None,tail=b'\x01')
    altered('unclosed-domain',lambda e:e.pop())
    altered('wrong-PCM-stage',lambda e:None,stage='after-original-submit')
    altered('v3-cannot-be-promoted',lambda e:None,version=3)
    post=[(h.copy(),bytes(pcm)) for h,pcm in events]
    for index,(h,pcm) in enumerate(post):
        if h[14]==4:post[index]=(h,(np.frombuffer(pcm,dtype='<f4')*np.float32(.125)).tobytes())
    try:raw_oracle(post)
    except ValueError:checks.append('independent-source-oracle-detects-post-gain-payload-even-with-pre-tag')
    else:raise ValueError('oracle accepted post-gain data')
    existing=args.output/'existing-output';existing.mkdir();(existing/'keep.txt').write_bytes(b'preserve')
    result=subprocess.run([str(exe),str(fixture),str(existing.resolve())],capture_output=True,timeout=10,creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode!=1 or sorted(p.name for p in existing.iterdir())!=['keep.txt'] or (existing/'keep.txt').read_bytes()!=b'preserve':raise ValueError('existing output modified')
    checks.append('existing-output-unchanged')
    if (fixture/'events.bin').read_bytes()!=original:raise ValueError('original synthetic fixture modified')
    report=dict(passed=True,integration_checks=len(checks),checks=checks,
        before_pcm_samples_independently_verified=1920,source_sha256=hashlib.sha256(original).hexdigest(),pcm_sha256=golden,
        gain_applied_once_in_existing_bridge_samples=1440,reset_unknown_position_quarantined=1,
        native_v4_verified=False,mat_encoded=False,deployed=False,scope='Synthetic mutating originals + compiled parser/FFmpeg bridge only.')
    (args.output/'result.json').write_text(json.dumps(report,indent=2),encoding='utf8');print(json.dumps(report,indent=2))


if __name__=='__main__':main()
