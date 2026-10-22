"""Actual-file boundary audit, following independent full state/PCM verification."""
import collections, csv, hashlib, json, math
from pathlib import Path
import sys
import numpy as np
from verify_stateful import Capture

def main(root):
    summary=json.loads((root/'summary.json').read_text())
    verified=json.loads((root/'state-verification.json').read_text())
    storage=json.loads((root/'storage.json').read_text())
    assert verified['passed'] and verified['chunks']>1
    assert summary['calls']==summary['blocks'] and summary['frames']>0
    assert not any(summary[k] for k in ('dropped_full','dropped_busy','invalid'))
    paths=[root/'global-objects.bin',*sorted(root.glob('global-objects-[0-9]*.bin'))]
    assert len(paths)==storage['chunks']==verified['chunks']
    assert sum(p.stat().st_size for p in paths)==summary['bytes']==storage['bytes']
    assert all(storage['chunk_mb']*1024**2<=p.stat().st_size<storage['chunk_mb']*1024**2+112+8192 for p in paths[:-1])
    previous={};digests={};blocks=collections.Counter();frame_sizes=collections.Counter();cross=[];inside=[]
    cached=audible=known_audible_cached=zero_payload_cached=0;seq=frames=0
    hz=summary['qpc_frequency'];boundaries=[]
    with Capture(root) as capture, (root/'rotation-boundaries.csv').open('w',newline='') as out:
        writer=csv.writer(out)
        writer.writerow(['previous_chunk','chunk','host','object_id','type','generation','previous_sequence','sequence',
                         'delta_ms','position_state','flags','x','y','z','raw_x','raw_y','raw_z','previous_peak','peak'])
        for r,start,end in capture:
            assert r[12]==seq;seq+=1;frames+=r[1]
            frame_sizes[r[1]]+=1
            chunk=len(capture.boundaries)-1
            key=(r[2],r[4],r[5],r[18])
            blocks[key]+=1
            pcm=np.frombuffer(capture.mm,dtype='<f4',count=r[1],offset=start)
            peak=float(np.max(np.abs(pcm)))
            digests.setdefault(key,hashlib.sha256()).update(pcm)
            old=previous.get(key)
            if old:
                delta=1000*(r[3]-old['qpc'])/hz
                assert delta>0,('non-monotonic object QPC',key,r[12])
                if chunk!=old['chunk']:
                    assert chunk==old['chunk']+1,('object absent for a full chunk',key)
                    cross.append(delta)
                    is_cached=r[5]==1 and r[15]==2
                    is_audible=peak>1e-8 and old['peak']>1e-8
                    if is_cached:
                        assert old['state'] in (1,2,3) and r[6:9]==old['xyz']
                        assert not r[14]&4
                        cached+=1
                        known_audible_cached+=is_audible
                        zero_payload_cached+=r[19:22]==(0.,0.,0.) and any(v!=0 for v in r[6:9])
                    audible+=is_audible
                    writer.writerow([old['chunk'],chunk,*key,old['seq'],r[12],delta,r[15],r[14],*r[6:9],*r[19:22],old['peak'],peak])
                else: inside.append(delta)
            previous[key]=dict(chunk=chunk,qpc=r[3],frames=r[1],seq=r[12],state=r[15],xyz=r[6:9],peak=peak)
            del pcm
        boundaries=list(capture.boundaries)
    assert seq==summary['blocks'] and frames==summary['frames']
    assert cached>0 and known_audible_cached>0 and zero_payload_cached>0, 'No positive audible cached-position boundary observed'
    def timing(values):
        return dict(count=len(values),median_ms=float(np.median(values)),p99_ms=float(np.percentile(values,99)),max_ms=max(values))
    result=dict(passed=True,elapsed_seconds=summary['elapsed_seconds'],files=len(paths),file_switches=len(paths)-1,
        blocks=seq,frames=frames,all_submit_calls_present=True,sequence_gaps=0,duplicate_sequences=0,
        object_crossings=len(cross),cached_position_crossings=cached,audible_crossings=audible,
        audible_cached_position_crossings=known_audible_cached,zero_payload_nonzero_cached_crossings=zero_payload_cached,
        crossing_timing=timing(cross),within_file_timing=timing(inside),
        per_object_block_counts=dict(collections.Counter(blocks.values())),block_frame_sizes=dict(frame_sizes),chunk_boundaries=boundaries,
        object_pcm_sha256=[dict(host=k[0],object_id=k[1],type=k[2],generation=k[3],blocks=blocks[k],sha256=d.hexdigest()) for k,d in digests.items()],
        scope='All captured submit blocks and samples present across files; not an independent endpoint/glitch or upstream source-frame-index measurement.')
    (root/'rotation-verification.json').write_text(json.dumps(result,indent=2),encoding='utf8')
    print(json.dumps({k:v for k,v in result.items() if k not in ('chunk_boundaries','object_pcm_sha256')},indent=2))

if __name__=='__main__':main(Path(sys.argv[1]))
