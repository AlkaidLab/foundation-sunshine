"""Compare unchanged research-layout extraction with the actual standalone input.
Preserve startup evidence; explicitly gate the first 3 bootstrap cycles for stable PCM analysis.
"""
import argparse, hashlib, json
from pathlib import Path
import numpy as np
def infer_lag(original, decoded):
    n=1<<int((len(original)+len(decoded)-1).bit_length())
    cross=np.fft.irfft(np.fft.rfft(decoded,n)*np.conj(np.fft.rfft(original,n)),n)
    # Keep a wide enough window for silent preroll; never hardcode a measured answer.
    lags=np.arange(-4800,max(4800,len(decoded)-len(original)+4800)+1)
    return int(lags[cross[lags%n].argmax()])


def coverage(original, decoded, lag, ticks):
    lo=max(0,-lag);hi=min(len(original),len(decoded)-lag)
    if lo>=hi:
        raise ValueError('source and decoded windows do not overlap')
    # A concatenated extraction may hide a missing carrier half. Such a timeline cannot pass.
    continuous=bool(len(ticks)>0 and np.all(np.diff(ticks)==1))
    peak=float(np.max(np.abs(decoded[lo+lag:hi+lag]-original[lo:hi])))
    outside=np.concatenate((decoded[:max(0,lag)],decoded[max(0,lag+len(original)):]))
    return dict(stable_half_timeline_contiguous=continuous,
        full_source_observed_within_one_pcm16_lsb=bool(continuous and lo==0 and hi==len(original) and peak<=1/32768+1e-10),
        outside_source_nonzero_samples=int(np.count_nonzero(outside)),
        outside_source_peak=float(np.abs(outside).max()) if len(outside) else 0.,
        outside_source_checked_only_for_observed_stable_window=True)


def main():
    ap=argparse.ArgumentParser();ap.add_argument('inspection',type=Path);ap.add_argument('--expected',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--lag-samples',type=int,help='Explicit common pipeline lag for periodic tone controls; not an integrity fix')
    args=ap.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    with np.load(args.inspection/'observed-slots.npz',allow_pickle=False) as data:
        pcm=data['audio_pcm16'].astype(np.float64)/32768;fields=data['fields'];indices=data['half_indices']
    with np.load(args.expected,allow_pickle=False) as data:
        source=data['audio'].astype(np.float64);xyz=data['windows_xyz'].copy()
    assert source.shape==(2,48000) and xyz.shape==(2,100,3)
    keep=indices>=6
    stable=pcm.reshape(31,-1,480)[:,keep];meta=fields[keep];ticks=indices[keep]
    rows=[]
    for obj in range(2):
        original=source[obj]
        decoded=stable[11+obj].reshape(-1)
        inferred=infer_lag(original,decoded)
        lag=args.lag_samples if args.lag_samples is not None else inferred
        lo=max(0,-lag);hi=min(len(original),len(decoded)-lag)
        def metrics(start,end):
            x=original[start:end];y=decoded[start+lag:end+lag];error=y-x
            gain=float(x@y/(x@x));residual=y-gain*x
            return dict(frames=len(x),correlation=float(np.corrcoef(x,y)[0,1]),gain=gain,
                rmse=float(np.sqrt(np.mean(error**2))),max_abs_error=float(np.abs(error).max()),
                fitted_signal_to_residual_db=float(10*np.log10(np.sum((gain*x)**2)/np.sum(residual**2))))
        overlap=metrics(lo,hi)
        first=metrics(lo,min(hi,lo+4800))
        last=metrics(max(lo,hi-4800),hi)
        # Explicitly show both the startup-affected overlap and a separate later window.
        later=metrics(max(lo,4800),hi)
        unusual=[]
        for b in range(100):
            left=max(lo,b*480);right=min(hi,(b+1)*480)
            if left<right:
                error=decoded[left+lag:right+lag]-original[left:right]
                if np.abs(error).max()>1e-4:unusual.append(b)
        values=meta[:,obj]
        rows.append(dict(slot=11+obj,object_index=obj,inferred_lag_samples=inferred,used_lag_samples=lag,
            active_output_halves=int(np.count_nonzero(np.max(np.abs(stable[11+obj]),axis=1)>1e-6)),
            source_frames_outside_observed_stable_overlap=48000-(hi-lo),
            overlap_metrics=overlap,first_100ms_metrics=first,last_100ms_metrics=last,
            **coverage(original,decoded,lag,ticks),
            later_window_source_start_frame=max(lo,4800),later_window_metrics=later,
            source_blocks_with_aligned_peak_error_above_1e_minus4=unusual,
            field_min=values.min(axis=0).tolist(),field_max=values.max(axis=0).tolist(),
            field_changes=np.count_nonzero(np.diff(values.astype(int),axis=0),axis=0).tolist()))
    static=stable[:11]
    result=dict(scope='Two-object subset, observed-layout decoder, original first 3 carrier cycles retained but excluded from stable metrics.',
        expected_sha256=hashlib.sha256(args.expected.read_bytes()).hexdigest(),bootstrap_cycles_excluded_from_stable_metrics=[0,1,2],
        stable_halves=ticks.tolist(),stable_nonzero_static_samples=int(np.count_nonzero(static)),
        stable_static_peak=float(np.abs(static).max()),objects=rows,
        exact_original_objects_verified=False,
        alignment_caveat='Continuity claims require full source coverage, contiguous carrier half indices and bounded pointwise error, not correlation alone. Any pass is limited to these two objects and this observed-layout extraction; it is not certified decoding or a latency measurement.',
        calibrated_coordinates_verified=False,dolby_conformance_verified=False,android_decode_verified=False)
    (args.output/'comparison.json').write_text(json.dumps(result,indent=2),encoding='utf8')
    print(json.dumps({k:v for k,v in result.items() if k!='stable_halves'},indent=2))
if __name__=='__main__':main()
