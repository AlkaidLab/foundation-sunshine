"""Validate existing offline bridge exports, without encoding, pacing or modifying them."""
import argparse,csv,json,math
from pathlib import Path
import numpy as np
from resident_mat_bridge import FILE,FRAME,CALL,source_info,file_sha256
from replay_resident_bridge import golden


def inspect_file(path,dynamic):
    calls=0;ends=0;creates=0;active={};highest=0;last_qpc=None;peak=0.;frame_calls=[]
    with path.open('rb') as stream:
        header=stream.read(FILE.size)
        if len(header)!=FILE.size:raise ValueError('short bridge file header')
        magic,version,rate,quantum,frames,hz=FILE.unpack(header)
        if magic!=(b'MATBRG1\0' if dynamic else b'MATBED1\0') or (version,rate,quantum)!=(1,48000,480):
            raise ValueError('bridge file format')
        for index in range(frames):
            raw=stream.read(FRAME.size)
            if len(raw)!=FRAME.size:raise ValueError('short bridge frame header')
            seq,samples,count,flags,first,last=FRAME.unpack(raw)
            eof=bool(dynamic and index==frames-1)
            if seq!=index or samples!=480 or count>64 or flags!=int(eof):raise ValueError('bridge frame order/bounds/EOF')
            if eof:
                if (first,last)!=(-1,-1):raise ValueError('EOF invents source timestamp')
            elif not (0<=first<=last and (last_qpc is None or first>=last_qpc)):raise ValueError('source QPC order')
            else:last_qpc=last
            seen=set();frame_calls.append(count)
            for _ in range(count):
                raw=stream.read(CALL.size)
                if len(raw)!=CALL.size:raise ValueError('short bridge call header')
                identity,slot,typ,properties,x,y,z,length=CALL.unpack(raw)
                if length!=480:raise ValueError('bridge call frame count')
                raw=stream.read(length*4)
                if len(raw)!=length*4:raise ValueError('short bridge PCM')
                pcm=np.frombuffer(raw,dtype='<f4')
                if not np.isfinite(pcm).all():raise ValueError('bridge nonfinite PCM')
                peak=max(peak,float(np.abs(pcm).max()));calls+=1
                if dynamic:
                    if not identity or typ!=1 or properties not in (7,6,8) or not all(map(math.isfinite,(x,y,z))):
                        raise ValueError('native dynamic input identity/properties/position')
                    if identity in seen:raise ValueError('duplicate native ID inside frame')
                    seen.add(identity)
                    if properties==7:
                        if identity in active:raise ValueError('native ID created twice')
                        active[identity]=slot;creates+=1
                    else:
                        if active.get(identity)!=slot:raise ValueError('source slot rebound or update/end before create')
                        if properties==8:
                            if np.count_nonzero(pcm):raise ValueError('end notification contains nonzero PCM')
                            active.pop(identity);ends+=1
                    highest=max(highest,len(active))
                    if highest>20 or peak>1:raise ValueError('native dynamic capacity/PCM range')
                elif identity!=0 or typ<=1 or typ&(typ-1) or properties&~15 or not all(map(math.isnan,(x,y,z))):
                    raise ValueError('bed was mislabeled as dynamic geometry')
        if stream.read(1):raise ValueError('trailing bridge bytes')
    if dynamic and active:raise ValueError('unended bridge objects at EOF')
    return dict(frames=frames,calls=calls,creates=creates,ends=ends,max_active_objects=highest,pcm_peak=peak,
                bytes=path.stat().st_size,sha256=file_sha256(path))


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--source',type=Path,required=True)
    ap.add_argument('--export',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
    args=ap.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    report=json.loads((args.export/'summary.json').read_text());summary,_,hashes=source_info(args.source)
    if hashes!=report['source_files']:raise ValueError('source manifest changed')
    dynamic=inspect_file(args.export/'mat-object-input.bin',True);bed=inspect_file(args.export/'static-bed-input.bin',False)
    expected=golden(args.source,37,report['source_session_sha256'],report['source_rounds'],summary['qpc_frequency'],report['paced']['window_start_frame'])
    if dynamic['sha256']!=expected['dynamic_sha256'] or bed['sha256']!=expected['bed_sha256']:raise ValueError('source-snapshot golden mismatch')
    with (args.export/'paced-timing.csv').open(newline='') as stream:rows=list(csv.DictReader(stream))
    indices=[int(float(row['source_round'])) for row in rows]
    start=report['paced']['window_start_frame'];end=report['paced']['window_end_frame_exclusive']
    if indices!=list(range(start,end)):raise ValueError('paced CSV sequence')
    misses=0
    for row in rows:
        target=float(row['target_perf_seconds']);enqueued=float(row['enqueue_perf_seconds'])
        began=float(row['worker_start_perf_seconds']);finish=float(row['worker_finish_perf_seconds'])
        if not enqueued<=began<=finish:raise ValueError('paced worker timing order')
        misses+=finish-target>.010
    passed=bool(report['offline_bridge_replay_passed'] and report['paced']['producer_overrun_count']==0
                and report['paced']['dynamic_sha256']==expected['paced_window_dynamic_sha256'] and misses==0)
    proof=dict(passed=passed,source_unchanged=True,export_files_read_only=True,dynamic=dynamic,static_bed=bed,
        source_slot_map=expected['source_slot_map'],golden_bytes_identical=True,paced_rounds=len(rows),paced_10ms_deadline_misses=misses,
        mat_encoded=False,live_quantum_boundary_verified=False,static_bed_encoded=False,full_game_mix_mat_ready=False,
        scope='Structural + source stored-snapshot validation of diagnostic inputs only. No vendor execution or MAT conformance claim.')
    (args.output/'proof.json').write_text(json.dumps(proof,indent=2),encoding='utf8')
    print(json.dumps({k:v for k,v in proof.items() if k!='source_slot_map'},indent=2))
    if not passed:raise SystemExit(1)


if __name__=='__main__':main()
