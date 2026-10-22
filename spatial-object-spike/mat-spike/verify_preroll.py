"""Offline A/B evidence for silent preroll. No vendor calls, playback or elevation."""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

from analyze_mat import read_capture


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root',type=Path,default=Path(__file__).resolve().parent)
    ap.add_argument('--output',type=Path,required=True)
    args=ap.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    root=args.root.resolve();cases=[]
    for kind in ['tone','game']:
        capture=root/'encode-run03/capture'/kind
        raw,records,summary=read_capture(capture)
        phase=json.loads((capture/'processing.json').read_text())
        comparison=json.loads((root/f'preroll-compare-{kind}02/comparison.json').read_text())
        proof=json.loads((root/f'preroll-reference-{kind}01/proof.json').read_text())
        with np.load(root/f'encode-inspect-{kind}02/observed-slots.npz',allow_pickle=False) as old:
            old_ticks=old['half_indices'];old_fields=old['fields'];keep=old_ticks>=6
            old_ticks=old_ticks[keep];old_fields=old_fields[keep]
        with np.load(root/f'preroll-inspect-{kind}01/observed-slots.npz',allow_pickle=False) as new:
            new_ticks=new['half_indices'];new_fields=new['fields']
            ix=np.searchsorted(new_ticks,old_ticks+phase['preroll_blocks'])
            assert np.array_equal(new_ticks[ix],old_ticks+phase['preroll_blocks'])
            differs=np.any(new_fields[ix]!=old_fields,axis=(1,2))
            pcm=new['audio_pcm16'].reshape(31,-1,480)[:,new_ticks>=6]
        # Explicitly preserve startup differences, rather than claiming every raw field matches.
        bad_ticks=old_ticks[differs].tolist()
        stable_fields_equal=bool(np.all(~differs[old_ticks>=14]))
        passed=bool(all(o['full_source_observed_within_one_pcm16_lsb']
                        and o['outside_source_nonzero_samples']==0 for o in comparison['objects'])
                    and np.count_nonzero(pcm[:11])==0 and np.count_nonzero(pcm[13:])==0
                    and proof['passed'] and stable_fields_equal
                    and len(records)==128 and len(raw)==3932160
                    and all(r[1]==r[6]==r[7]==30720 for r in records))
        cases.append(dict(case=kind,observed_layout_short_clip_continuity_passed=passed,
            carrier_sha256=hashlib.sha256(raw).hexdigest(),records=len(records),carrier_bytes=len(raw),
            processing=phase,worker_case_ms=summary['worker_case_ms'],
            source_frames_per_object=48000,source_fully_covered_objects=len(comparison['objects']),
            lag_relative_to_stable_window=[o['used_lag_samples'] for o in comparison['objects']],
            source_max_abs_error=[o['overlap_metrics']['max_abs_error'] for o in comparison['objects']],
            extra_audio_samples_outside_source=[o['outside_source_nonzero_samples'] for o in comparison['objects']],
            metadata_baseline_half_shift=phase['preroll_blocks'],
            metadata_differing_baseline_half_indices=bad_ticks,
            metadata_identical_to_baseline_from_half_14=stable_fields_equal,
            cross_language_layout_check_passed=proof['passed']))
    result=dict(scope='Offline two-object 1-second fixture A/B, not live capture or certified MAT decoding.',
        observed_layout_short_clip_continuity_passed=all(c['observed_layout_short_clip_continuity_passed'] for c in cases),
        cases=cases,
        caveats=['Both objects exist during preroll. Midstream object creation/end/reuse remains untested.',
                 '200ms is a tested conservative preroll, not a minimum or measured end-to-end latency.',
                 'Bootstrap carrier remains intact; a research PCM parser excluding it is not permission to drop real MAT initialization frames.',
                 'No 20-object paced worker, live bridge, endpoint-incapable host, Dolby conformance or Android decoder proof.'])
    (args.output/'proof.json').write_text(json.dumps(result,indent=2),encoding='utf8')
    print(json.dumps(result,indent=2))
    if not result['observed_layout_short_clip_continuity_passed']:
        raise SystemExit(1)


if __name__=='__main__':main()
