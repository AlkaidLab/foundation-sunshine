"""Read-only independent proof of the one approved controlled native capture.

Compares captured raw PCM/XYZ against spatial_probe.cpp's known source, rather
than treating the C++ marker parser passing as proof of object alignment.
"""
import argparse,collections,csv,hashlib,json,math,re
from pathlib import Path
import numpy as np
from make_quantum_fixture import EVENT


def load(path):return json.loads(path.read_text(encoding='utf-8-sig'))


def digest(path):
    sha=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):sha.update(block)
    return sha.hexdigest()


def gain_models(raw,volume,gain):
    volume=np.float32(volume);gain=np.float32(gain)
    return dict(combined_f32=raw*np.float32(volume*gain),
        volume_then_gain=(raw*volume)*gain,gain_then_volume=(raw*gain)*volume)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ('run','parser-proof','output'):ap.add_argument('--'+name,type=Path,required=True)
    args=ap.parse_args();capture=args.run/'capture'
    native=load(capture/'summary.json');parsed=load(args.parser_proof/'summary.json')
    launch=load(args.run/'launch.json');exited=load(args.run/'exit.json')
    if not (exited['completed'] and exited['collector_exited'] and exited['collector_exit']==0
            and native['status']==2 and native['error']==0 and native['original_bytes_restored']
            and all(native[k]==0 for k in ('dropped_full','dropped_busy','invalid'))
            and parsed['passed'] and parsed['native_process_markers_observed']):raise ValueError('native integrity/exit failed')
    if load(capture/'format.json')['source_kind']!='native-process-marker-probe':raise ValueError('not native events')
    if exited['audiodg_pids_after']!=[launch['audiodg_pid_before']]:raise ValueError('audio engine changed')
    source_logs=[]
    pattern=r'EMITTER PID=(\d+) variant=(\d+) objects=(\d+) blocks=(\d+) framesPerObject=(\d+) rate=(\d+) duration=([\d.]+)'
    for index in (1,2):
        match=re.search(pattern,(args.run/f'source-{index}.stdout.txt').read_text(encoding='utf-8-sig'))
        if not match:raise ValueError('controlled source did not finish')
        source_logs.append(dict(zip(('pid','variant','objects','blocks','frames_per_object','rate'),map(int,match.groups()[:6]))))
    first=source_logs[0]
    if (first['variant'],first['objects'],first['blocks'],first['frames_per_object'],first['rate'])!=(0,2,300,144000,48000):
        raise ValueError('this content proof is bounded to the approved 2-object 3-second source')
    second=source_logs[1]
    if (second['variant'],second['objects'],second['blocks'],second['frames_per_object'],second['rate'])!=(1,1,200,96000,48000):
        raise ValueError('second controlled source did not match the approved phase')
    args.output.mkdir(parents=True,exist_ok=False)
    paths=[capture/entry['file'] for entry in parsed['source_files']]
    manifest=[dict(file=path.name,bytes=path.stat().st_size,sha256=digest(path)) for path in paths]
    if manifest!=parsed['source_files']:raise ValueError('native source changed')
    pending=[];groups=[];anchors=[];sequence=0;previous=-1
    encoder=set();threads=set();props=collections.Counter();produced=collections.Counter();events=0
    for path in paths:
        with path.open('rb') as stream:
            while raw:=stream.read(EVENT.size):
                if len(raw)!=EVENT.size:raise ValueError('short event')
                h=EVENT.unpack(raw);pcm=stream.read(h[1]*4)
                if len(pcm)!=h[1]*4 or h[0]!=0x474f424a or h[12]!=sequence or h[3]<previous or h[18]!=0 or h[21]!=0:
                    raise ValueError('event integrity')
                previous=h[3];sequence+=1;events+=1;encoder.add(h[16]);threads.add(h[11])
                if h[15]!=0:raise ValueError('loss epoch is not zero')
                if h[14]==1:
                    if h[5]!=1 or h[1]!=480:raise ValueError('unexpected source type/size')
                    props[h[13]]+=1;pending.append((h,np.frombuffer(pcm,dtype='<f4')))
                elif h[14]==2:
                    if h[1] or h[17]!=480:raise ValueError('native inputFrames not 480')
                    produced[h[19]]+=1
                    if not anchors:anchors.append((pending,h))
                    else:groups.append((pending,h))
                    pending=[]
                else:raise ValueError('unexpected event kind')
    if pending or len(encoder)!=1 or len(threads)!=1 or events!=native['blocks']:
        raise ValueError('unsealed events or mixed encoder/thread')
    raw_objects=[item for group,_ in [*anchors,*groups] for item in group]
    if len(raw_objects)!=parsed['object_records'] or len(groups)!=parsed['completed_quanta']:
        raise ValueError('independent grouping differs')
    ticks=np.arange(300,dtype=np.float64);t=ticks*.01
    coordinates=[np.column_stack((np.sin(t*1.1+i),.5*np.sin(t*.7+i),-np.cos(t*1.1+i))).astype(np.float32) for i in (0,1)]
    records=[];ids={};maximum_xyz=0.;maximum_pcm=0.;exact_pcm=0;total_pcm=0;source_indices=collections.defaultdict(list)
    max_raw_error=0.;model_error=collections.defaultdict(float);model_exact=collections.Counter();gain_values=set();volume_values=set()
    for h,pcm in raw_objects:
        volume=h[9];which=0 if abs(volume-float(np.float32(.1)))<1e-7 else 1 if abs(volume-float(np.float32(.07)))<1e-7 else None
        if which is None or h[13]&4==0:raise ValueError('unrecognized source volume/position update')
        xyz=np.asarray(h[6:9],dtype=np.float64)
        errors=np.max(np.abs(coordinates[which].astype(np.float64)-xyz),axis=1)
        tick=int(np.argmin(errors));xyz_error=float(errors[tick]);maximum_xyz=max(maximum_xyz,xyz_error)
        if xyz_error>1e-6:raise ValueError('XYZ does not match controlled source')
        identity=(h[2],h[4]);old=ids.setdefault(identity,which)
        if old!=which:raise ValueError('object identity rebound')
        frequency=783.99 if which else 523.25
        sample=tick*480+np.arange(480,dtype=np.float64)
        expected=(.2*np.sin((((2.*math.pi)*frequency)*sample)/48000.)).astype(np.float32)
        max_raw_error=max(max_raw_error,float(np.abs(expected.astype(np.float64)-pcm.astype(np.float64)).max()))
        models=gain_models(expected,volume,h[10]);errors={}
        for name,values in models.items():
            errors[name]=float(np.abs(values.astype(np.float64)-pcm.astype(np.float64)).max())
            model_error[name]=max(model_error[name],errors[name])
            model_exact[name]+=int(np.count_nonzero(values.view('<u4')==pcm.view('<u4')))
        # These are fixed metadata-derived F32 models, not a free amplitude fit or inverse-gain repair.
        error=min(errors.values());total_pcm+=480;gain_values.add(h[10]);volume_values.add(volume)
        if error>2e-8:raise ValueError('PCM does not match controlled source at XYZ-derived offset even with recorded gain: '+json.dumps(errors))
        source_indices[which].append(tick);records.append((h[12],which,tick,h[4],xyz_error,error))
    if len(ids)!=2 or source_indices[0]!=source_indices[1]:raise ValueError('two objects do not share source ticks')
    observed=source_indices[0]
    if observed!=list(range(observed[0],observed[-1]+1)) or observed[-1]!=299:raise ValueError('source PCM gap or tail loss')
    model=min(model_error,key=model_error.get)
    maximum_pcm=model_error[model];exact_pcm=model_exact[model]
    if maximum_pcm>2e-8:raise ValueError('no single gain model explains the whole capture')
    if max_raw_error<.1:raise ValueError('this proof must not hide the observed pre-gain PCM mismatch')
    tick_by_sequence={record[0]:record[2] for record in records}
    empty=0;joined=[]
    for objects,marker in groups:
        if not objects:empty+=1;joined.append(None);continue
        ticks_here=[tick_by_sequence[h[12]] for h,_ in objects]
        if len(objects)!=2 or len(set(ticks_here))!=1:raise ValueError('Process seals mixed source ticks')
        joined.append(ticks_here[0])
    nonempty=[n for n in joined if n is not None]
    if nonempty!=list(range(observed[0]+1,300)):raise ValueError('sealed pass source coverage')
    if any(n is not None for n in joined[len(nonempty):]):raise ValueError('unexpected post-end source audio')
    with (args.parser_proof/'quanta.csv').open(newline='') as stream:rows=list(csv.DictReader(stream))
    if len(rows)!=len(groups):raise ValueError('CSV quantum count')
    for index,((objects,marker),row) in enumerate(zip(groups,rows)):
        values=(int(row['quantum']),int(row['sample_start']),int(row['frames']),int(row['objects']),int(row['first_qpc']),int(row['last_qpc']))
        expected=(index,index*480,480,len(objects),objects[0][0][3] if objects else marker[3],marker[3])
        if values!=expected:raise ValueError('C++ CSV disagrees with independent event grouping')
    with (args.output/'object-source-match.csv').open('w',newline='') as stream:
        writer=csv.writer(stream);writer.writerow(('event_sequence','source_object','source_block','original_id','max_xyz_error','best_fixed_gain_model_pcm_error'));writer.writerows(records)
    qpcs=[marker[3] for _,marker in [*anchors,*groups]]
    intervals=np.diff(qpcs)*1000/native['qpc_frequency']
    for path,expected in zip(paths,manifest):
        if digest(path)!=expected['sha256']:raise ValueError('source changed during proof')
    report=dict(passed=True,scope='First selected encoder instance, controlled two-object source only.',
        real_process_markers=len(qpcs),sealed_quanta=len(groups),two_object_quanta=len(nonempty),empty_quanta=empty,
        encoder_count=len(encoder),thread_count=len(threads),object_count=len(ids),
        observed_source_block_first=observed[0],observed_source_block_last=observed[-1],
        conservative_attach_discard_objects=len(anchors[0][0]),trailing_unsealed_objects=0,
        source_pcm_samples_checked=total_pcm,bit_exact_pcm_samples=exact_pcm,max_pcm_absolute_error=maximum_pcm,max_xyz_absolute_error=maximum_xyz,
        captured_pcm_gain_model=model,all_fixed_gain_model_max_errors=dict(model_error),
        volume_values=sorted(volume_values),call_gain_values=sorted(gain_values),max_unscaled_source_pcm_error=max_raw_error,
        captured_pcm_is_ungained_source=False,captured_pcm_already_has_recorded_volume_and_call_gain=True,
        ready_for_existing_gain_applying_bridge=False,
        sealed_pass_objects_share_source_tick=True,no_internal_source_pcm_gaps=True,source_tail_recovered=True,
        sample_rate_matched_controlled_source=48000,marker_input_frames=480,
        marker_interarrival_ms=dict(p50=float(np.median(intervals)),p99=float(np.percentile(intervals,99)),max=float(intervals.max())),
        property_flags=dict(props),produced_bytes_histogram=dict(produced),source_phases=source_logs,
        capture_seconds=native['elapsed_seconds'],second_new_stream_captured=False,all_requested_phases_captured=False,
        first_encoder_boundary_and_content_verified=True,global_multi_instance_timeline_verified=False,
        loss_or_invalid_reads=0,original_hook_bytes_restored=True,audiodg_same_pid=True,
        raw_capture_unchanged=True,mat_encoded_by_this_bridge=False,full_game_mix_verified=False,android_atmos_verified=False,
        limitation='First encoder only; second stream absent. Post-Submit PCM already carries volume*gain and cannot enter the current gain-applying bridge unchanged. Repair capture timing and instance tracking before a separately authorized test. No capture or original parser output was rewritten.')
    (args.output/'proof.json').write_text(json.dumps(report,indent=2),encoding='utf8')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
