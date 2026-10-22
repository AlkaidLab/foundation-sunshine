"""Streaming-independent replay of disk v2 state + PCM integrity; no whole-file RAM copy."""
import argparse, collections, csv, json, math, mmap, struct, wave
from pathlib import Path
import numpy as np

HEADER = struct.Struct('<IIQqIIfffffIQIIIIQQfffI')
assert HEADER.size == 112

def records(mm):
    offset = 0
    while offset < len(mm):
        assert offset + HEADER.size <= len(mm), ('truncated header', offset)
        r = HEADER.unpack_from(mm, offset)
        offset += HEADER.size
        assert r[0] == 0x324F424A and r[13] == 2 and 0 < r[1] <= 2048
        end = offset + 4*r[1]
        assert end <= len(mm), ('truncated PCM', offset)
        yield r, offset, end
        offset = end

class Capture:
    def __init__(self, root): self.root=root;self.mm=None;self.file=None;self.boundaries=[]
    def close(self):
        if self.mm is not None: self.mm.close();self.mm=None
        if self.file is not None: self.file.close();self.file=None
    def __enter__(self): return self
    def __exit__(self,*args): self.close()
    def __iter__(self):
        paths=[self.root/'global-objects.bin',*sorted(self.root.glob('global-objects-[0-9]*.bin'))]
        self.boundaries=[]
        for path in paths:
            self.close();self.file=path.open('rb');self.mm=mmap.mmap(self.file.fileno(),0,access=mmap.ACCESS_READ)
            first=True
            for r,start,end in records(self.mm):
                if first: self.boundaries.append(dict(file=path.name,sequence=r[12],generation=r[18],state=r[15]));first=False
                yield r,start,end

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('directory', type=Path)
    ap.add_argument('--proof', action='store_true')
    args = ap.parse_args(); root = args.directory
    summary = json.loads((root/'summary.json').read_text())
    ps = json.loads((root/'position-summary.json').read_text())
    backend = json.loads((root/'backend.json').read_text())
    assert summary['status'] == 2 and summary['error'] == 0 and summary['original_bytes_restored']
    assert not any(summary[k] for k in ('dropped_full','dropped_busy','invalid'))
    assert backend['extended_calls'] == 0 and backend['encoder_ready'] == 1
    assert ps['stateful'] and ps['evictions'] == 0
    cache = {}; objects = {}; counts = collections.Counter(); total_frames = 0
    epoch = 0; previous_qpc = None; seq = 0; reused = 0
    with Capture(root) as capture:
        for r, start, end in capture:
            mm=capture.mm
            (magic, frames, host, qpc, obj, typ, x, y, z, vol, gain, tid, sequence,
             version, flags, state, volume_known, loss, generation, rx, ry, rz, reserved) = r
            assert sequence == seq; seq += 1; total_frames += frames
            assert math.isfinite(gain)
            if loss != epoch:
                cache.clear()
                epoch = loss
            key = (host,obj,typ)
            if key not in cache or flags & 1:
                if key in cache: reused += 1
                cache[key] = {'p':None,'v':None,'g':generation}
            e = cache[key]
            assert e['g'] == generation
            if flags & 1: e['v'] = 1.
            if flags & 2 and math.isfinite(vol): e['v'] = vol
            assert bool(volume_known) == (e['v'] is not None)
            assert (not volume_known and math.isnan(vol)) or vol == e['v']
            if typ == 1:
                if flags & 4:
                    if all(map(math.isfinite,(rx,ry,rz))): expected=1;e['p']=(rx,ry,rz)
                    else: expected=0;e['p']=None
                elif flags & 1: expected=3;e['p']=(0.,0.,0.)
                elif e['p'] is not None: expected=2
                else: expected=0
                assert state == expected, (seq,state,expected,flags)
                assert (e['p'] is None and all(map(math.isnan,(x,y,z)))) or (x,y,z)==e['p']
            else:
                assert state == 4 and all(map(math.isnan,(x,y,z)))
            counts[state] += 1
            pcm = np.frombuffer(mm,dtype='<f4',count=frames,offset=start)
            assert np.isfinite(pcm).all()
            peak = float(np.max(np.abs(pcm))); audible = peak > 1e-8
            okey = (host,obj,typ,generation)
            o = objects.setdefault(okey,dict(host=host,object_id=obj,type=typ,generation=generation,
                blocks=0,unknown_blocks=0,known_audio_blocks=0,updates=0,cached_audio_blocks=0,
                first_qpc=qpc,first_flags=flags,first_state=state,first_known_qpc=None,
                peak=0.,peak_qpc=qpc,first_position=None,last_position=None,position_changes=0))
            o['blocks'] += 1;o['last_qpc']=qpc;o['unknown_blocks'] += state==0
            if state in (1,2,3):
                if o['first_known_qpc'] is None: o['first_known_qpc']=qpc
                if o['last_position'] != [x,y,z]: o['position_changes'] += 1
                if o['first_position'] is None: o['first_position']=[x,y,z]
                o['last_position']=[x,y,z]
                o['known_audio_blocks'] += audible
                o['cached_audio_blocks'] += audible and state==2
            o['updates'] += state==1
            if peak > o['peak']: o['peak']=peak;o['peak_qpc']=qpc
            if flags & 8: cache.pop(key)
            del pcm
        assert seq == summary['blocks'] and total_frames == summary['frames']
        assert counts[0]==ps['unknown'] and counts[1]==ps['updated'] and counts[2]==ps['cached']
        dynamic=[o for o in objects.values() if o['type']==1]
        result=dict(passed=True,blocks=seq,frames=total_frames,position_state_counts=dict(counts),
            dynamic_lifetimes=len(dynamic),audible_known_lifetimes=sum(o['known_audio_blocks']>0 for o in dynamic),
            changing_audible_lifetimes=sum(o['known_audio_blocks']>0 and o['position_changes']>1 for o in dynamic),
            late_attach_unknown_lifetimes=sum(o['first_state']==0 for o in dynamic),
            late_attach_recovered_lifetimes=sum(o['first_state']==0 and o['first_known_qpc'] is not None for o in dynamic),
            chunks=len(capture.boundaries),chunk_boundaries=list(capture.boundaries),objects=list(objects.values()))
        if args.proof:
            candidates=[o for o in dynamic if o['known_audio_blocks'] and o['position_changes']>1]
            if candidates:
                best=max(candidates,key=lambda o:o['peak']);hz=summary['qpc_frequency']
                lo=max(best['first_known_qpc'],best['peak_qpc']-int(2*hz));hi=lo+int(10*hz)
                with wave.open(str(root/'proof-object-audio.wav'),'wb') as w, (root/'proof-object-position.csv').open('w',newline='') as c:
                    w.setparams((1,2,48000,0,'NONE','not compressed'));writer=csv.writer(c)
                    writer.writerow(['audio_frame','qpc','flags','position_state','x','y','z','volume','call_gain','peak'])
                    written=0
                    for r,start,end in capture:
                        if (r[2],r[4],r[5],r[18])!=(best['host'],best['object_id'],1,best['generation']) or not lo<=r[3]<hi: continue
                        pcm=np.frombuffer(capture.mm,dtype='<f4',count=r[1],offset=start)
                        w.writeframesraw((np.clip(pcm,-1,1)*32767).astype('<i2').tobytes())
                        writer.writerow([written,r[3],r[14],r[15],*r[6:9],r[9],r[10],float(np.max(np.abs(pcm)))])
                        written+=r[1];del pcm
                result['proof']={'object_id':best['object_id'],'generation':best['generation'],'audio_frames':written,'seconds':written/48000}
    (root/'state-verification.json').write_text(json.dumps(result,indent=2),encoding='utf8')
    print(json.dumps({k:v for k,v in result.items() if k!='objects'},indent=2))

if __name__=='__main__':main()
