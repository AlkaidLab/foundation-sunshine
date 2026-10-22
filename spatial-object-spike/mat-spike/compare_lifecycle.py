"""Strict offline per-generation continuity and paced-worker deadline checks.
Uses the unchanged observed MAT slot layout, NOT a certified MAT decoder.
"""
import argparse,csv,hashlib,json
from pathlib import Path
import numpy as np
from analyze_mat import read_capture

LSB=1/32768


def stats(values):
    values=np.asarray(values,dtype=float)
    return dict(count=len(values),min=float(values.min()),p50=float(np.percentile(values,50)),
                p95=float(np.percentile(values,95)),p99=float(np.percentile(values,99)),max=float(values.max()))


def match_generation(original, slots):
    """Infer slot and offset from independent broadband samples, not expected slot IDs."""
    n=1<<int((len(original)+slots.shape[1]-1).bit_length())
    template=np.conj(np.fft.rfft(original,n))
    lags=np.arange(-4800,slots.shape[1])
    candidates=[]
    for slot,decoded in enumerate(slots):
        cross=np.fft.irfft(np.fft.rfft(decoded,n)*template,n)
        ix=int(np.argmax(cross[lags%n]));lag=int(lags[ix])
        candidates.append((float(cross[lag%n]),slot,lag))
    score,slot,lag=max(candidates)
    return slot,lag,score


def compare_tracks(audio,slots,segments,ticks,fields):
    contiguous=bool(len(ticks)>0 and slots.shape[1]==len(ticks)*480 and np.all(np.diff(ticks)==1))
    reconstructed=np.zeros_like(slots);occupied=np.zeros(slots.shape,dtype=bool);rows=[]
    for segment in segments:
        gen=segment['generation'];begin=segment['source_start_block']*480;end=segment['end_block']*480
        original=audio[gen,begin:end].astype(float)
        slot,lag,score=match_generation(original,slots)
        lo=max(0,-lag);hi=min(len(original),slots.shape[1]-lag)
        if lo>=hi:raise ValueError('no source overlap')
        x=original[lo:hi];y=slots[slot,lo+lag:hi+lag];error=y-x
        complete=lo==0 and hi==len(original)
        overlap=bool(np.any(occupied[slot,lo+lag:hi+lag]))
        reconstructed[slot,lo+lag:hi+lag]+=x;occupied[slot,lo+lag:hi+lag]=True
        first=max(0,lag//480);last=min(len(ticks),(lag+len(original)+479)//480)
        raw=fields[first:last,slot]
        bad=[]
        for b in range((len(original)+479)//480):
            left=max(lo,b*480);right=min(hi,(b+1)*480)
            if left<right and np.abs(slots[slot,left+lag:right+lag]-original[left:right]).max()>LSB+1e-10:bad.append(b)
        peak=float(np.abs(error).max())
        rows.append(dict(**segment,observed_slot=slot+11,lag_relative_to_stable_window=lag,
            inferred_global_timeline_offset_samples=lag+int(ticks[0])*480-begin,
            peak_match_dot_product=score,compared_frames=len(x),source_frames_missing_from_overlap=len(original)-len(x),
            correlation=float(np.corrcoef(x,y)[0,1]),rmse=float(np.sqrt(np.mean(error**2))),max_abs_error=peak,
            first_480_frames_peak_error=float(np.abs(error[:480]).max()),last_480_frames_peak_error=float(np.abs(error[-480:]).max()),
            source_blocks_exceeding_one_lsb=bad,overlapping_generation_assignment=overlap,
            observed_full_source_continuity_passed=bool(contiguous and complete and not overlap and peak<=LSB+1e-10),
            raw_field_changes=np.count_nonzero(np.diff(raw.astype(int),axis=0),axis=0).tolist(),
            raw_field_min=raw.min(axis=0).tolist(),raw_field_max=raw.max(axis=0).tolist()))
    error=slots-reconstructed
    outside=slots[~occupied]
    peak=float(np.abs(error).max())
    offsets=sorted(set(row['inferred_global_timeline_offset_samples'] for row in rows))
    return dict(stable_half_timeline_contiguous=contiguous,objects=rows,
        inferred_global_timeline_offsets_samples=offsets,all_generations_share_one_timeline_offset=len(offsets)==1,
        reconstructed_all_dynamic_slots_max_abs_error=peak,
        unassigned_dynamic_nonzero_samples=int(np.count_nonzero(outside)),
        unassigned_dynamic_peak=float(np.abs(outside).max()) if outside.size else 0.,
        full_timeline_audio_check_passed=bool(contiguous and len(offsets)==1 and all(r['observed_full_source_continuity_passed'] for r in rows)
                                            and peak<=LSB+1e-10 and np.count_nonzero(outside)==0))


def timings(path,summary,segments):
    with path.open(newline='') as stream:rows=list(csv.DictReader(stream))
    if len(rows)!=summary['processed_source_blocks']:raise ValueError('timing row count')
    data={key:np.array([int(row[key]) for row in rows],dtype=np.int64) for key in rows[0]}
    if not np.array_equal(data['block'],np.arange(len(rows))):raise ValueError('timing block sequence')
    hz=summary['qpc_frequency'];step=hz/100
    if not (np.all(np.abs(np.diff(data['target_qpc'])-step)<=1)
            and np.all(data['start_qpc']>=data['target_qpc'])
            and np.all(data['process_start_qpc']>=data['start_qpc'])
            and np.all(data['finish_qpc']>=data['process_start_qpc'])):raise ValueError('invalid timing order/cadence')
    for b in range(len(rows)):
        count=sum(s['created_block']<=b<=s['end_block'] for s in segments)
        active=sum(s['created_block']<=b<s['end_block'] for s in segments)
        if data['submissions'][b]!=count or data['active_objects'][b]!=active:raise ValueError('lifecycle submission/active counts')
    scale=1000/hz
    work=(data['finish_qpc']-data['start_qpc'])*scale
    proc=(data['finish_qpc']-data['process_start_qpc'])*scale
    late=(data['start_qpc']-data['target_qpc'])*scale
    missed=(data['finish_qpc']-data['target_qpc'])*scale>10
    live=(data['block']>=20)&(data['block']<220)
    return dict(scheduled_blocks=len(rows),input_frames_per_block=480,scheduled_period_ms=10,
        start_interval_ms=stats(np.diff(data['start_qpc'])*scale),start_lateness_ms=stats(late),
        submit_plus_process_wall_ms=stats(work),process_only_wall_ms=stats(proc),
        live_source_submit_plus_process_wall_ms=stats(work[live]),
        max_active_objects=int(data['active_objects'].max()),
        finish_after_next_10ms_deadline_count=int(missed.sum()),missed_deadline_blocks=data['block'][missed].tolist(),
        work_exceeding_10ms_count=int((work>10).sum()),
        observed_elapsed_first_start_to_last_finish_ms=float((data['finish_qpc'][-1]-data['start_qpc'][0])*scale),
        deadline_check_passed=not bool(missed.any()),
        caveat='Short-run wall-clock measurements on an own normal-priority worker, not CPU time or hard real-time guarantees.')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--capture',type=Path,required=True);ap.add_argument('--inspection',type=Path,required=True)
    ap.add_argument('--expected',type=Path,required=True);ap.add_argument('--manifest',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True);args=ap.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    carrier,records,summary=read_capture(args.capture)
    if len(records)!=240 or len(carrier)!=240*30720 or any(r[1]!=30720 for r in records):raise ValueError('carrier records incomplete')
    manifest=json.loads(args.manifest.read_text());segments=manifest['segments']
    with np.load(args.expected,allow_pickle=False) as source:audio=source['audio']
    with np.load(args.inspection/'observed-slots.npz',allow_pickle=False) as observed:
        ticks=observed['half_indices'];keep=ticks>=6
        pcm=observed['audio_pcm16'].astype(float).reshape(31,-1,480)[:,keep].reshape(31,-1)/32768
        fields=observed['fields'][keep];ticks=ticks[keep]
    audio_check=compare_tracks(audio,pcm[11:],segments,ticks,fields)
    timing=timings(args.capture/'timing.csv',summary,segments)
    static_nonzero=int(np.count_nonzero(pcm[:11]))
    inspection=json.loads((args.inspection/'summary.json').read_text())
    position_activity=all(any(n>0 for n in row['raw_field_changes'][:3]) for row in audio_check['objects'])
    result=dict(case=manifest['case'],scope='Deterministic synthetic objects, 2.4s scheduled input; unchanged observed-layout extraction.',
        capture_sha256=hashlib.sha256(carrier).hexdigest(),records=len(records),carrier_bytes=len(carrier),
        bootstrap_cycles_excluded_from_stable_pcm_check=[0,1,2],all_original_carrier_retained=True,
        stable_nonzero_static_samples=static_nonzero,lfe_peak=inspection['lfe_peak'],audio=audio_check,timing=timing,
        position_field_activity_check_passed=position_activity,
        metadata_caveat='Activity of raw position-associated fields only. Auxiliary fields are not understood; this is not calibrated XYZ or proof of frame-accurate metadata/audio synchronization.',
        short_test_passed=bool(audio_check['full_timeline_audio_check_passed'] and timing['deadline_check_passed']
                              and position_activity and static_nonzero==0 and inspection['lfe_peak']==0),
        coordinates_calibrated=False,dolby_conformance_verified=False,android_local_decode_verified=False,
        live_resident_bridge_verified=False,long_run_soak_verified=False)
    (args.output/'comparison.json').write_text(json.dumps(result,indent=2),encoding='utf8')
    print(json.dumps({k:v for k,v in result.items() if k!='audio'},indent=2))
    print(json.dumps({k:v for k,v in audio_check.items() if k!='objects'},indent=2))
    print(json.dumps([dict(generation=r['generation'],id=r['id'],slot=r['observed_slot'],lag=r['lag_relative_to_stable_window'],
        frames=r['compared_frames'],max_error=r['max_abs_error'],passed=r['observed_full_source_continuity_passed']) for r in audio_check['objects']],indent=2))


if __name__=='__main__':main()
