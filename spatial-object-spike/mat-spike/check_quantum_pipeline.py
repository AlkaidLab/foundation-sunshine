"""Offline integration checks: compiled C++ v3 parser + original Nioh snapshot digests."""
import argparse,copy,hashlib,json,subprocess
from pathlib import Path
from make_quantum_fixture import EVENT
from resident_mat_bridge import source_info


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ('exe','original','fixture','proof','output'):ap.add_argument('--'+name,type=Path,required=True)
    args=ap.parse_args();exe=args.exe.resolve(strict=True)
    args.output.mkdir(parents=True,exist_ok=False)
    passed=[]
    expected=json.loads((args.fixture/'fixture-expected.json').read_text())
    proof=json.loads((args.proof/'summary.json').read_text())
    if not (proof['passed'] and proof['all_object_pcm_sha256']==expected['pcm_sha256']
            and proof['sealed_object_pcm_sha256']==expected['pcm_sha256']
            and proof['state_snapshot_sha256']==expected['stored_snapshot_sha256']
            and proof['completed_quanta']==expected['completed_quanta']
            and proof['sealed_object_samples']==expected['object_samples']
            and proof['sample_clock_frames']==expected['sample_clock_frames']
            and not proof['native_process_markers_observed']):raise ValueError('fixture PCM/state/count/provenance mismatch')
    if source_info(args.original)[2]!=expected['source_files']:raise ValueError('original source changed')
    passed.append('real_recorded_payload_and_independent_stored_snapshot_digests')

    def marker(seq,qpc,frames=480):
        return [0x474f424a,0,0,qpc,0,0,0.,0.,0.,0.,0.,123,seq,0,2,0,0x123400,frames,0,0,30720,0],b''
    def obj(seq,qpc,frames=480):
        return [0x474f424a,frames,7,qpc,42,1,1.,2.,3.,.5,.25,123,seq,6,1,0,0x123400,0,0,0,0,0],b'\0'*(frames*4)
    baseline=[marker(0,100,0),obj(1,101),marker(2,102)]

    def case(name,events,okay=False,inspect=None,tail=b''):
        root=args.output/name;root.mkdir();data=b''.join(EVENT.pack(*h)+pcm for h,pcm in events)+tail
        (root/'global-objects.bin').write_bytes(data)
        (root/'format.json').write_text(json.dumps(dict(version=3,header_bytes=112,source_kind='synthetic-process-markers-over-recorded-pcm')))
        (root/'summary.json').write_text(json.dumps(dict(status=2,error=0,original_bytes_restored=True,dropped_full=0,dropped_busy=0,invalid=0,
            blocks=len(events),frames=sum(h[1] for h,_ in events),bytes=len(data))))
        out=args.output/(name+'-proof')
        run=subprocess.run([str(exe),str(root.resolve()),str(out.resolve())],capture_output=True,text=True,timeout=10,creationflags=subprocess.CREATE_NO_WINDOW)
        (args.output/(name+'.stdout.txt')).write_text(run.stdout+run.stderr,encoding='utf8')
        if (run.returncode==0)!=okay:raise ValueError(f'{name}: unexpected exit {run.returncode}')
        if okay:
            report=json.loads((out/'summary.json').read_text())
            if inspect:inspect(report)
        passed.append(name)

    case('clean_complete_pass',copy.deepcopy(baseline),True)
    for name,index,field,value in (
        ('sequence_gap',1,12,9),('foreign_encoder',1,16,0x567800),('loss_epoch',1,15,1),
        ('backwards_qpc',2,3,99),('submit_failed',1,18,-2147467259),('process_failed',2,18,-2147467259),
        ('unsupported_extended_flag',1,13,16),('process_thread_mismatch',2,11,456),('reserved_bits',1,21,1),
        ('object_exceeds_pass',2,17,240),('invalid_kind',2,14,99)):
        events=copy.deepcopy(baseline);events[index][0][field]=value;case(name,events)
    def short_check(report):
        if report['unknown_offset_objects']!=1 or report['not_full_span_layout_quanta']!=1:
            raise ValueError('short packet got invented offset')
    case('short_packet_offset_stays_unknown',[marker(0,100,0),obj(1,101,240),marker(2,102)],True,short_check)
    def empty_check(report):
        if report['completed_quanta']!=1 or report['empty_quanta']!=1 or report['sample_clock_frames']!=240:
            raise ValueError('empty variable-size pass clock')
    case('empty_240_frame_pass',[marker(0,100,0),marker(1,101,240)],True,empty_check)
    case('truncated_header',copy.deepcopy(baseline),tail=b'\x01')
    case('no_sealing_marker',copy.deepcopy(baseline[:2]))

    existing=args.output/'existing-output';existing.mkdir();sentinel=existing/'keep.txt';sentinel.write_bytes(b'preserve')
    before=hashlib.sha256(sentinel.read_bytes()).hexdigest()
    run=subprocess.run([str(exe),str((args.output/'clean_complete_pass').resolve()),str(existing.resolve())],capture_output=True,timeout=10,creationflags=subprocess.CREATE_NO_WINDOW)
    if run.returncode!=1 or sorted(p.name for p in existing.iterdir())!=['keep.txt'] or hashlib.sha256(sentinel.read_bytes()).hexdigest()!=before:
        raise ValueError('existing output changed')
    passed.append('existing_output_directory_unchanged')
    never=args.output/'must-not-create'
    run=subprocess.run([str(exe),str(args.original.resolve()),str(never.resolve())],capture_output=True,timeout=10,creationflags=subprocess.CREATE_NO_WINDOW)
    if run.returncode!=1 or never.exists():raise ValueError('old v2 accepted as authoritative v3')
    passed.append('old_v2_cannot_be_promoted_to_native_boundaries')
    report=dict(passed=True,integration_checks=len(passed),checks=passed,fixture_payload_and_state_golden_passed=True,
        original_source_unchanged=True,native_boundary_verified=False,mat_encoded=False,
        caveat='Only synthetic markers over real recorded PCM; no audiodg access or live proof.')
    (args.output/'result.json').write_text(json.dumps(report,indent=2),encoding='utf8')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
