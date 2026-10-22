"""Read-only replay of recorded game objects through an OFFLINE MAT input adapter.
Writes only a new output folder. Does not activate an encoder, load a DLL or elevate.
"""
import argparse,csv,hashlib,json,queue,threading,time
from pathlib import Path
import numpy as np
from resident_mat_bridge import (Bridge,FILE,FRAME,CALL,QUANTUM,Record,lockstep_batches,
                                source_info,frame_bytes,call_bytes,file_sha256)


def file_header(magic,frames,hz):return FILE.pack(magic,1,48000,480,frames,hz)


def golden(root,width,session,frames,hz,window_start):
    """Separate pass uses the original collector's stored snapshots, NOT our cache/framer.
    This fixture has no loss/create/end events; reject such inputs rather than guessing.
    """
    dynamic=hashlib.sha256(file_header(b'MATBRG1\0',frames+1,hz))
    bed=hashlib.sha256(file_header(b'MATBED1\0',frames,hz));window=hashlib.sha256()
    ids={};active={};counts=0;source_slots={}
    for index,batch in enumerate(lockstep_batches(root,width)):
        objects=[];beds=[]
        for r in batch:
            slot=source_slots.setdefault(r.key,len(source_slots))
            h=r.h
            if h[14]&9 or h[17]:raise ValueError('golden fixture mode only supports the observed midattach/no-end recording')
            if not h[16]:raise ValueError('golden fixture volume unknown')
            # Arithmetic specification: multiply by volume, then callGain, each rounded to F32.
            pcm=(r.pcm.astype(np.float64)*h[9]).astype('<f4')
            pcm=(pcm.astype(np.float64)*h[10]).astype('<f4')
            if h[5]!=1:beds.append(call_bytes(0,slot,h[5],h[14],None,pcm));continue
            if h[15]==0:
                if np.count_nonzero(r.pcm):raise ValueError('unknown audible source must not be silently removed')
                continue
            key=(session,*r.key,h[17],h[18]);first=key not in ids
            if first:ids[key]=len(ids)+1
            native=ids[key];xyz=tuple(h[6:9])
            objects.append(call_bytes(native,slot,1,7 if first else 6,xyz,pcm));active[key]=(native,slot,xyz)
        blob=frame_bytes(index,objects,batch[0].h[3],batch[-1].h[3]);dynamic.update(blob)
        if index>=window_start:window.update(blob)
        bed.update(frame_bytes(index,beds,batch[0].h[3],batch[-1].h[3]));counts+=1
    if counts!=frames:raise ValueError('golden round count')
    eof=[call_bytes(native,slot,1,8,xyz,np.zeros(480,dtype='<f4')) for native,slot,xyz in active.values()]
    dynamic.update(frame_bytes(frames,eof,-1,-1,eof=True))
    return dict(dynamic_sha256=dynamic.hexdigest(),bed_sha256=bed.hexdigest(),paced_window_dynamic_sha256=window.hexdigest(),
                source_slot_map=[dict(source_slot=slot,host=key[0],original_id=key[1],type=key[2]) for key,slot in source_slots.items()])


def paced(root,width,session,start,end,hz,expected_hash):
    bridge=Bridge(session);source_iter=lockstep_batches(root,width);source=enumerate(source_iter)
    first=None
    for index,batch in source:
        if index>=start:first=(index,batch);break
        bridge.consume(index,batch) # reconstruct prefix state; NOT counted as paced execution
    if first is None:raise ValueError('paced window outside source')
    pending=queue.Queue(maxsize=8);done=threading.Event();errors=[];samples=[];digest=hashlib.sha256()
    origin=time.perf_counter();source_origin=first[1][0].h[3];high_water=0;overruns=0
    def worker():
        try:
            while not (done.is_set() and pending.empty()):
                try:item=pending.get(timeout=.05)
                except queue.Empty:continue
                index,batch,target,enqueued=item;began=time.perf_counter()
                dynamic,_=bridge.consume(index,batch);digest.update(dynamic);finish=time.perf_counter()
                samples.append((index,target,enqueued,began,finish))
        except Exception as exc:errors.append(repr(exc));done.set()
    consumer=threading.Thread(target=worker,name='offline-mat-bridge',daemon=True);consumer.start()
    def enqueue(index,batch):
        nonlocal high_water,overruns
        target=origin+(batch[0].h[3]-source_origin)/hz
        delay=target-time.perf_counter()
        if delay>0:time.sleep(delay)
        try:pending.put_nowait((index,batch,target,time.perf_counter()))
        except queue.Full:overruns+=1;return False
        high_water=max(high_water,pending.qsize());return True
    try:
        if enqueue(*first):
            for index,batch in source:
                if index>=end or errors:break
                if not enqueue(index,batch):break # explicit failure, never silently drop and continue
    finally:
        source_iter.close();done.set();consumer.join(timeout=10)
    if consumer.is_alive():raise ValueError('offline bridge thread did not stop')
    if errors:raise ValueError(errors)
    rows=np.array(samples,dtype=float)
    if not len(rows):raise ValueError('no paced samples')
    work=(rows[:,4]-rows[:,3])*1000;late=(rows[:,4]-rows[:,1])*1000
    stats=lambda x:dict(p50=float(np.percentile(x,50)),p99=float(np.percentile(x,99)),max=float(np.max(x)))
    return dict(window_start_frame=start,window_end_frame_exclusive=end,prefix_reconstructed_unpaced=True,
        paced_source_seconds=(end-start)*.01,completed_rounds=len(samples),queue_capacity_rounds=8,
        observed_queue_high_water_rounds=high_water,producer_overrun_count=overruns,
        worker_wall_ms=stats(work),arrival_to_finish_ms=stats(late),
        finish_after_nominal_10ms_count=int(np.count_nonzero(late>10)),
        dynamic_sha256=digest.hexdigest(),matches_unpaced_and_golden=digest.hexdigest()==expected_hash,
        passed=bool(overruns==0 and len(samples)==end-start and digest.hexdigest()==expected_hash and np.all(late<=10)),
        caveat='Python offline bridge only, 8 whole-round queue, disk-backed source. Not the native capture ring, MAT encoding, or a real game load.',
        timing_rows=samples)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--round-width',type=int,default=37);ap.add_argument('--paced-seconds',type=float,default=6.)
    args=ap.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    root=args.source.resolve();summary,pos,hashes=source_info(root)
    if summary['blocks']%args.round_width:raise ValueError('partial source rounds')
    total=summary['blocks']//args.round_width;window=max(1,int(round(args.paced_seconds*100)));start=max(0,total-window)
    session=hashlib.sha256(json.dumps(hashes,sort_keys=True).encode()).hexdigest()
    bridge=Bridge(session);window_digest=hashlib.sha256();qpc_spans=[];qpc_starts=[];chunks=set();count=0
    initial_order=None;reordered_rounds=0
    with (args.output/'mat-object-input.bin').open('xb') as dynamic,(args.output/'static-bed-input.bin').open('xb') as bed:
        dynamic.write(file_header(b'MATBRG1\0',total+1,summary['qpc_frequency']))
        bed.write(file_header(b'MATBED1\0',total,summary['qpc_frequency']))
        for index,batch in enumerate(lockstep_batches(root,args.round_width)):
            order=[r.key for r in batch]
            if initial_order is None:initial_order=order
            reordered_rounds+=order!=initial_order
            objects,static=bridge.consume(index,batch);dynamic.write(objects);bed.write(static)
            if index>=start:window_digest.update(objects)
            qpc_spans.append((batch[-1].h[3]-batch[0].h[3])*1000/summary['qpc_frequency']);qpc_starts.append(batch[0].h[3])
            chunks.update(r.chunk for r in batch);count+=1
        dynamic.write(bridge.finish())
    if count!=total or bridge.counts['source_object_samples']!=summary['frames']:raise ValueError('source counters')
    for state,key in [(0,'unknown'),(1,'updated'),(2,'cached'),(3,'create_default_origin'),(4,'static_implicit')]:
        if bridge.cache.counts[f'position_state_{state}']!=pos[key]:raise ValueError('cache counter mismatch')
    expected=golden(root,args.round_width,session,total,summary['qpc_frequency'],start)
    actual={name:file_sha256(args.output/path) for name,path in
            [('dynamic_sha256','mat-object-input.bin'),('bed_sha256','static-bed-input.bin')]}
    if any(actual[k]!=expected[k] for k in actual) or window_digest.hexdigest()!=expected['paced_window_dynamic_sha256']:
        raise ValueError('independent stored-snapshot/gain golden bytes mismatch')
    print(json.dumps(dict(stage='unpaced bridge + independent golden passed',rounds=total,counts=dict(bridge.counts)),indent=2),flush=True)
    timing=paced(root,args.round_width,session,start,total,summary['qpc_frequency'],expected['paced_window_dynamic_sha256'])
    rows=timing.pop('timing_rows')
    with (args.output/'paced-timing.csv').open('x',newline='') as stream:
        writer=csv.writer(stream);writer.writerow(['source_round','target_perf_seconds','enqueue_perf_seconds','worker_start_perf_seconds','worker_finish_perf_seconds']);writer.writerows(rows)
    _,_,after=source_info(root)
    if hashes!=after:raise ValueError('original capture changed during replay')
    interval=np.diff(qpc_starts)*1000/summary['qpc_frequency']
    result=dict(source=str(root),source_session_sha256=session,source_files=hashes,source_unchanged=True,
        source_rounds=total,source_audio_clock_seconds=total*.01,source_records=summary['blocks'],source_object_samples=summary['frames'],
        chunks=len(chunks),chunk_switches=len(chunks)-1,reordered_rounds=reordered_rounds,
        source_slot_mapping='Stable by host/objectId/type within session, never current batch array index',
        counts=dict(bridge.counts),cache_counts=dict(bridge.cache.counts),
        dynamic_id_map=bridge.manifest,hashes=actual,independent_golden=expected,independent_golden_passed=True,
        original_arrival_round_span_ms=dict(p50=float(np.median(qpc_spans)),max=float(max(qpc_spans))),
        original_round_start_interval_ms=dict(p50=float(np.median(interval)),min=float(min(interval)),max=float(max(interval))),
        paced=timing,offline_bridge_replay_passed=timing['passed'],
        dynamic_subset_has_no_unknown_audible_or_range_error=not bool(bridge.counts['quarantined_unknown_audible_records'] or bridge.counts['native_pcm_range_exceedances']),
        mat_encoded=False,vendor_called=False,admin_required=False,game_started=False,
        live_quantum_boundaries_captured=False,static_bed_encoder_route_verified=False,full_game_mix_mat_ready=False,
        caveats=['Capture v2 lacks authoritative render-quantum ID and sample offsets. This adapter verifies the complete 37-key-set/480-frame round pattern (allowing reordered calls); it is NOT a general live frame grouping algorithm.',
                 'QPC preserves arrival timing in replay, never determines PCM resampling/gaps or presentation timestamps.',
                 'Unknown coordinates remain unknown. Only silent unknown records are deferred in this fixture; audible unknowns must be quarantined/reported, not assigned origin.',
                 'Static bed PCM/gain/type are preserved in a separate artifact, not sent as dynamic objects or claimed encoded.',
                 'EOF end notifications are bridge-generated, not observed game ends. This real fixture has no source create/end; those paths need synthetic tests.',
                 'Output is diagnostic bridge input format, NOT native MAT/IEC61937 and NOT the existing MATLIF1 probe fixture. No automatic encoder invocation.'])
    (args.output/'summary.json').write_text(json.dumps(result,indent=2),encoding='utf8')
    print(json.dumps({k:v for k,v in result.items() if k not in ['source_files','dynamic_id_map']},indent=2),flush=True)
    if not result['offline_bridge_replay_passed']:raise SystemExit(1)


if __name__=='__main__':main()
