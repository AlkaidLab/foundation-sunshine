"""Read-only native-v4 replay analysis; observed layout, NOT a Dolby decoder.

No DLL loading, process attachment, audio activation, or input/capture mutation.
Writes only a new analysis directory. Keeps bootstrap transport in full.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess

import numpy as np

from analyze_mat import RECORD, PERIOD, PREAMBLE, inspect_burst, find_all
from compare_lifecycle import compare_tracks, stats


def require(ok, why):
    if not ok:
        raise ValueError(why)


def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2), encoding='utf8')


def parse_records(raw, expected):
    records, chunks, offset = [], [], 0
    while offset < len(raw):
        require(offset+RECORD.size <= len(raw), 'truncated record header')
        r = RECORD.unpack_from(raw, offset)
        offset += RECORD.size
        require(r[0] == 0x42595445 and r[2] == len(records), 'record magic/sequence')
        require(r[1] == r[6] == r[7] == 30720 and r[5] == 480 and r[9] >= 0,
                'record dimensions/HRESULT')
        require(offset+r[1] <= len(raw), 'truncated record payload')
        require(not records or r[3] >= records[-1][3], 'record QPC regression')
        chunks.append(raw[offset:offset+r[1]])
        offset += r[1]
        records.append(r)
    require(len(records) == expected, 'record count')
    return b''.join(chunks), records


def timing_check(path, summary, case, segments):
    with path.open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    require(len(rows) == summary['scheduled_blocks'] == len(case['mapping']), 'timing count')
    data = {key: np.array([int(row[key]) for row in rows], dtype=np.int64) for key in rows[0]}
    require(np.array_equal(data['block'], np.arange(len(rows))), 'timing block sequence')
    for b, mapping in enumerate(case['mapping']):
        phase = {'preroll': 1, 'source': 2, 'drain': 3}[mapping['phase']]
        source = mapping['source_frame'] if mapping['source_frame'] is not None else 0xffffffff
        active = sum(s['created_block'] <= b < s['end_block'] for s in segments)
        require(data['phase'][b] == phase and data['source_index'][b] == source
                and data['submissions'][b] == mapping['submissions']
                and data['active_objects'][b] == active, 'timing input/lifecycle mapping')
    hz = summary['qpc_frequency']
    require(hz > 0 and np.all(np.abs(np.diff(data['target_qpc'])-hz/100) <= 1)
            and np.all(data['start_qpc'] >= data['target_qpc'])
            and np.all(data['process_start_qpc'] >= data['start_qpc'])
            and np.all(data['finish_qpc'] >= data['process_start_qpc']), 'timing order/cadence')
    scale = 1000/hz
    work = (data['finish_qpc']-data['start_qpc'])*scale
    proc = (data['finish_qpc']-data['process_start_qpc'])*scale
    late = (data['start_qpc']-data['target_qpc'])*scale
    missed = (data['finish_qpc']-data['target_qpc'])*scale > 10
    return dict(scheduled_blocks=len(rows), period_ms=10, max_active_objects=int(data['active_objects'].max()),
                start_interval_ms=stats(np.diff(data['start_qpc'])*scale),
                start_lateness_ms=stats(late), submit_plus_process_wall_ms=stats(work),
                process_only_wall_ms=stats(proc),
                phase_counts={name: int(np.count_nonzero(data['phase'] == n))
                              for name, n in [('preroll', 1), ('source', 2), ('drain', 3)]},
                finish_after_next_10ms_deadline_count=int(missed.sum()),
                missed_deadline_blocks=data['block'][missed].tolist(),
                work_exceeding_10ms_count=int(np.count_nonzero(work > 10)),
                deadline_check_passed=not bool(missed.any()),
                caveat='Short own-worker wall-clock timing, not CPU cost, codec latency or hard real-time guarantee.')


def analyze_domain(capture, out, case, oracle, expected_path, reference):
    out.mkdir(exist_ok=False)
    summary = load(capture/'summary.json')
    require(summary['capture_kind'] == 'independent_mat_encoder_replay_v4'
            and summary['status'] == 2 and summary['error'] == 0 and summary['initialized']
            and summary['hooks_installed'] is False and summary['endpoint_calls'] == 0
            and summary['processed_blocks'] == summary['records'] == summary['scheduled_blocks']
            == case['scheduled_process_blocks'] and summary['domain'] == case['domain']
            and summary['native_source_process_blocks'] == case['source_process_blocks']
            and summary['preroll_blocks'] == case['preroll_blocks']
            and summary['drain_blocks'] == case['drain_blocks']
            and summary['submissions'] == sum(m['submissions'] for m in case['mapping'])
            and all(summary[k] >= 0 for k in ('create_hr', 'mapper_hr', 'initialize_hr', 'submit_hr', 'process_hr')),
            'native domain completion')
    record_bytes = (capture/'encoder-output.bin').read_bytes()
    carrier, records = parse_records(record_bytes, case['scheduled_process_blocks'])
    require(len(carrier) == summary['bytes'] and carrier == (capture/'encoder-payload.raw').read_bytes(),
            'payload/counters mismatch')
    require(len(carrier) % PERIOD == 0 and find_all(carrier, PREAMBLE) == list(range(0, len(carrier), PERIOD)),
            'carrier framing/cadence')
    (out/'carrier.iec61937').write_bytes(carrier)
    bursts, all_audio, all_fields, all_ticks, stable_audio, stable_fields, stable_ticks, lows = [], [], [], [], [], [], [], []
    for index in range(len(carrier)//PERIOD):
        info, _, audio, low, fields = inspect_burst(carrier[index*PERIOD:(index+1)*PERIOD])
        bursts.append(dict(burst=index, **info))
        if audio is not None:
            all_audio.extend(audio)
            all_fields.extend(fields)
            all_ticks.extend((index*2, index*2+1))
        if index >= 3:
            require(audio is not None, 'invalid observed layout after frozen bootstrap window')
            stable_audio.extend(audio)
            stable_fields.extend(fields)
            stable_ticks.extend((index*2, index*2+1))
            lows.extend(low)
    save(out/'bursts.json', bursts)
    # Same observed-valid windows as the unchanged C++ reference frontend.
    observed_pcm = np.concatenate(all_audio, axis=1)
    observed_pcm.T.astype('<i2').tofile(out/'fullband-31ch-48k.pcm16')
    with (out/'object-fields.csv').open('x', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['half', 'object', 'f0', 'f1', 'f2', 'f3', 'f4', 'f5'])
        for tick, fields in zip(all_ticks, all_fields):
            for obj in range(20):
                writer.writerow([tick, obj, *fields[obj].tolist()])
    pcm = np.concatenate(stable_audio, axis=1).astype(float)/32768
    fields = np.array(stable_fields)
    ticks = np.array(stable_ticks)
    with np.load(expected_path, allow_pickle=False) as expected:
        require(int(expected['sample_rate']) == 48000, 'expected sample rate')
        comparison = compare_tracks(expected['audio'], pcm[11:], oracle['segments'], ticks, fields)
    position_activity = all(any(n > 0 for n in row['raw_field_changes'][:3]) for row in comparison['objects'])
    timing = timing_check(capture/'timing.csv', summary, case, oracle['segments'])
    static_nonzero = int(np.count_nonzero(pcm[:11]))
    lfe_nonzero = int(np.count_nonzero(np.concatenate(lows)))
    ref_out = out/'cpp-reference'
    child = subprocess.run([str(reference.resolve()), str((out/'carrier.iec61937').resolve()), str(ref_out.resolve())],
                           capture_output=True, timeout=20)
    require(child.returncode == 0, 'offline C++ reference failed: '+child.stderr.decode(errors='replace'))
    before = list(csv.DictReader((out/'object-fields.csv').open(newline='')))
    after = list(csv.DictReader((ref_out/'fields.csv').open(newline='')))
    keys = ['half', 'object', 'f0', 'f1', 'f2', 'f3', 'f4', 'f5']
    same_fields = len(before) == len(after) and all([int(a[k]) for k in keys] == [int(b[k]) for k in keys]
                                                                 for a, b in zip(before, after))
    same_pcm = digest(out/'fullband-31ch-48k.pcm16') == digest(ref_out/'fullband-31ch-48k.pcm16')
    reference_proof = dict(pcm_bytes_identical=same_pcm, raw_fields_identical=same_fields,
                           executable_sha256=digest(reference), stdout=child.stdout.decode(),
                           passed=same_pcm and same_fields,
                           caveat='Two implementations of the same research layout, not certified decoding.')
    # Corruption tests touch only immutable byte copies in memory.
    negative = []
    bad_cases = [('truncated_header', record_bytes[:RECORD.size-1]),
                 ('truncated_payload', record_bytes[:-1]), ('bad_magic', bytes(4)+record_bytes[4:]),
                 ('missing_final_record', record_bytes[:-(RECORD.size+30720)])]
    for name, bad in bad_cases:
        try:
            parse_records(bad, len(records))
        except ValueError:
            negative.append(name)
        else:
            raise ValueError('corruption accepted: '+name)
    result = dict(domain=case['domain'], records=len(records), carrier_bytes=len(carrier), bursts=len(bursts),
                  capture_integrity_passed=True, carrier_sha256=hashlib.sha256(carrier).hexdigest(),
                  recorded_bytes_sha256=digest(capture/'encoder-output.bin'),
                  mat_family_carrier=True, bootstrap_cycles_excluded_from_stable_pcm_check=[0, 1, 2],
                  all_original_carrier_retained=True,
                  invalid_observed_layout_bursts=[b['burst'] for b in bursts if not b['observed_layout_valid']],
                  stable_static_nonzero_samples=static_nonzero, stable_lfe_nonzero_samples=lfe_nonzero,
                  audio=comparison, timing=timing, raw_position_field_activity_passed=position_activity,
                  reference=reference_proof, negative_tests=negative,
                  observed_layout_source_continuity_passed=comparison['full_timeline_audio_check_passed'],
                  short_test_passed=bool(comparison['full_timeline_audio_check_passed'] and timing['deadline_check_passed']
                                        and position_activity and static_nonzero == lfe_nonzero == 0 and reference_proof['passed']),
                  coordinates_calibrated=False, metadata_audio_sync_verified=False,
                  dolby_conformance_verified=False, android_local_decode_verified=False,
                  caveat='Raw six-bit field activity is not calibrated XYZ. All transport bytes preserved; only offline PCM check excludes bootstrap.')
    save(out/'proof.json', result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('run', 'input', 'input-proof', 'reference', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    launch = load(args.run/'launch.json')
    exit_info = load(args.run/'exit.json')
    top = load(args.run/'capture/summary.json')
    require(exit_info['completed'] and exit_info['probe_exited'] and exit_info['probe_exit'] == 0
            and not exit_info['audited_engine_exited'] and top['thread_completed'] and top['guard_passed']
            and top['status'] == 2 and top['error'] == 0 and top['engine_pid'] == launch['audiodg_pid_before']
            and top['hooks_installed'] is False and top['endpoint_calls'] == 0, 'run completion')
    proof = load(args.input_proof/'proof.json')
    manifest = load(args.input/'manifest.json')
    require(proof['passed'] and digest(args.input/'input.bin') == proof['input_sha256'] == launch['input_sha256']
            and digest(args.input/'manifest.json') == proof['input_manifest_sha256']
            and digest(args.input_proof/'proof.json').lower() == launch['input_proof_sha256'].lower(), 'frozen input proof')
    for file in proof['expected_files']:
        require(digest(args.input_proof/file['file']) == file['sha256'], 'expected oracle changed')
    cases = []
    for case, oracle in zip(manifest['cases'], proof['cases']):
        domain = case['domain']
        cases.append(analyze_domain(args.run/f'capture/domain-{domain}', args.output/f'domain-{domain}', case, oracle,
                                    args.input_proof/f'domain-{domain}-expected.npz', args.reference))
    result = dict(short_test_passed=all(c['short_test_passed'] for c in cases),
                  scheduled_seconds=manifest['scheduled_seconds'], records=sum(c['records'] for c in cases),
                  carrier_bytes=sum(c['carrier_bytes'] for c in cases), source_audio_samples=proof['counts']['source_audio_samples'],
                  source_frames_fully_compared=sum(o['compared_frames'] for c in cases for o in c['audio']['objects']),
                  script_sha256=digest(Path(__file__)), engine_pid=top['engine_pid'],
                  cases=[dict(domain=c['domain'], passed=c['short_test_passed'],
                              proof_file=f"domain-{c['domain']}/proof.json") for c in cases],
                  live_resident_bridge_verified=False, game_recording=False,
                  coordinates_calibrated=False, dolby_conformance_verified=False, android_local_decode_verified=False)
    save(args.output/'proof.json', result)
    print(json.dumps(result, indent=2))
    return 0 if result['short_test_passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
