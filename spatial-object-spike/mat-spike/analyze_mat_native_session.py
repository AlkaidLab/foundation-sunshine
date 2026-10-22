"""Offline native IPC-session capture oracle. No audio, vendor or elevation.

Research-layout extraction is NOT certified MAT decoding or calibrated XYZ.
Keeps every bootstrap transport byte; excludes first 3 bursts only in PCM checks.
"""
import argparse
import csv
import ctypes
import json
from pathlib import Path
import subprocess

import numpy as np

from analyze_mat import RECORD, PERIOD, PREAMBLE, inspect_burst, find_all
from compare_lifecycle import compare_tracks, stats
from verify_cpp_mat_stream import digest, load, require, sha


def save(path, value):
    path.write_text(json.dumps(value, indent=2), encoding='utf8')


def parse_records(raw, expected, worker):
    chunks, records, offset = [], [], 0
    while offset < len(raw):
        require(offset+RECORD.size <= len(raw), 'truncated record header')
        r = RECORD.unpack_from(raw, offset)
        offset += RECORD.size
        require(r[0] == 0x42595445 and r[2] == len(records) and r[5] == 480
                and r[1] == r[7] == r[6] == 30720 and r[9] == 0 and r[8] == worker
                and r[4] != 0 and r[3] > 0 and r[10] == r[11] == 0, 'record metadata/HRESULT/thread')
        require(offset+r[1] <= len(raw) and (not records or r[3] >= records[-1][3]), 'record payload/time order')
        chunks.append(raw[offset:offset+r[1]])
        offset += r[1]
        records.append(r)
    require(len(records) == expected, 'exact Process/record count')
    return b''.join(chunks), records


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('run', 'build', 'bridge', 'input', 'input-proof', 'prior-run', 'prior-analysis', 'reference', 'output'):
        ap.add_argument('--'+name, type=Path, required=True)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    launch, exit_info = load(args.run/'launch.json'), load(args.run/'exit.json')
    top = load(args.run/'capture/summary.json')
    h = top['output']
    build = load(args.build/'build.json')
    require(exit_info['workflow_completed'] and exit_info['native_launcher_started']
            and exit_info['probe_exited'] and exit_info['probe_exit'] == 0
            and top['completed'] and top['diagnostics_stable'] and not top['own_guard_test']
            and top['entry_return'] == top['shared_fault'] == 0 and not top['entry_timed_out']
            and top['target_pid'] == h['host_pid'] == launch['engine_pid']
            and top['target_creation_time'] == launch['engine_creation_time']
            and top['accepted'] == h['messages'] == 307 and h['source_rounds'] == 306
            and h['processes'] == h['records'] == 356 and h['source_samples'] == 288000
            and h['submissions'] == 642 and h['warm_rounds'] == 20 and h['drain_rounds'] == 30
            and h['host_guard_passed'] and h['worker_finished'] and not h['worker_timed_out']
            and h['helper_pinned'] and h['vendor_loads'] == h['activations'] == h['initializations'] == 1
            and h['status'] == 2 and h['error'] == 0 and h['eof_seen']
            and all(h[k] == 0 for k in ('create_hr', 'mapper_hr', 'initialize_hr', 'submit_hr', 'process_hr')),
            'real native session completion/identity')
    cleanup = top['cleanup']
    require(cleanup['entry_finished'] and cleanup['loader_finished'] and not cleanup['argument_retained']
            and cleanup['close_error'] == cleanup['retained_handle_count'] == 0
            and cleanup['remote_original_handles'] == cleanup['remote_original_handles_released'] == 5
            and not cleanup['forced_termination'] and not cleanup['remote_unload_attempted']
            and not launch['endpoint_changes'] and not launch['drivers'] and not launch['hooks']
            and not launch['privileges_adjusted'], 'cleanup/scope')
    require(digest(args.build/'mat-native-launcher.exe').upper() == build['executable_sha256'] == launch['executable_sha256']
            and build['receiver_dll_sha256'] == launch['helper_sha256'], 'frozen launch hashes')
    for item in build['source_files']:
        require(digest(args.build.parent/item['file']).upper() == item['sha256'], 'compiled source changed')
    source = args.bridge/'domain-1-mat-object-input.bin'
    require(digest(source) == launch['source_sha256'], 'frozen bridge source')
    oracle = load(args.input_proof/'proof.json')
    require(oracle['passed'] and digest(args.input/'input.bin') == oracle['input_sha256']
            and digest(args.input/'manifest.json') == oracle['input_manifest_sha256'], 'frozen native-tested input')
    for item in oracle['expected_files']:
        require(digest(args.input_proof/item['file']) == item['sha256'], 'frozen expected PCM')
    case = oracle['cases'][0]
    raw = (args.run/'capture/encoder-output.bin').read_bytes()
    carrier, records = parse_records(raw, 356, h['worker_tid'])
    require(carrier == (args.run/'capture/encoder-payload.raw').read_bytes()
            and len(carrier) == h['bytes'] == 10936320 and len(carrier) % PERIOD == 0
            and find_all(carrier, PREAMBLE) == list(range(0, len(carrier), PERIOD)), 'carrier integrity/cadence')
    # This equality includes the previously retained bootstrap cycles, not just
    # the decoded stable section. Different native thread/QPC record headers
    # are intentionally NOT required to equal those of the old run.
    prior = load(args.prior_analysis/'domain-1/proof.json')
    prior_bytes = (args.prior_run/'capture/domain-1/encoder-payload.raw').read_bytes()
    require(sha(prior_bytes) == prior['carrier_sha256'] and prior['short_test_passed'], 'prior native baseline evidence')
    same_prior = carrier == prior_bytes
    (args.output/'carrier.iec61937').write_bytes(carrier)
    all_audio, all_fields, all_ticks, stable_audio, stable_fields, stable_ticks, lows, bursts = [], [], [], [], [], [], [], []
    for n in range(len(carrier)//PERIOD):
        info, _, audio, low, fields = inspect_burst(carrier[n*PERIOD:(n+1)*PERIOD])
        bursts.append(dict(burst=n, **info))
        if audio is not None:
            all_audio.extend(audio)
            all_fields.extend(fields)
            all_ticks.extend((n*2, n*2+1))
        if n >= 3:
            require(audio is not None, 'missing stable observed-layout burst')
            stable_audio.extend(audio)
            stable_fields.extend(fields)
            stable_ticks.extend((n*2, n*2+1))
            lows.extend(low)
    save(args.output/'bursts.json', bursts)
    observed = np.concatenate(all_audio, axis=1)
    observed.T.astype('<i2').tofile(args.output/'fullband-31ch-48k.pcm16')
    with (args.output/'object-fields.csv').open('x', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['half', 'object', 'f0', 'f1', 'f2', 'f3', 'f4', 'f5'])
        for tick, fields in zip(all_ticks, all_fields):
            for obj in range(20):
                writer.writerow([tick, obj, *fields[obj].tolist()])
    stable = np.concatenate(stable_audio, axis=1).astype(float)/32768
    fields, ticks = np.array(stable_fields), np.array(stable_ticks)
    with np.load(args.input_proof/'domain-1-expected.npz', allow_pickle=False) as expected:
        require(int(expected['sample_rate']) == 48000, 'expected sample rate')
        comparison = compare_tracks(expected['audio'], stable[11:], case['segments'], ticks, fields)
    activity = all(any(n > 0 for n in r['raw_field_changes'][:3]) for r in comparison['objects'])
    static, lfe = int(np.count_nonzero(stable[:11])), int(np.count_nonzero(np.concatenate(lows)))
    require(digest(args.reference) == prior['reference']['executable_sha256'], 'frozen offline C++ parser changed')
    proc = subprocess.Popen([str(args.reference.resolve()), str((args.output/'carrier.iec61937').resolve()),
                             str((args.output/'cpp-reference').resolve())], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        stdout, stderr = proc.communicate(timeout=20)
    except subprocess.TimeoutExpired as e:
        raise RuntimeError('Own offline reference exceeded bound; no forced termination/retry') from e
    require(proc.returncode == 0, 'offline C++ reference failure: '+stderr.decode(errors='replace'))
    with (args.output/'object-fields.csv').open(newline='') as a, (args.output/'cpp-reference/fields.csv').open(newline='') as b:
        before, after = list(csv.DictReader(a)), list(csv.DictReader(b))
    keys = ['half', 'object', 'f0', 'f1', 'f2', 'f3', 'f4', 'f5']
    same_fields = len(before) == len(after) and all([int(x[k]) for k in keys] == [int(y[k]) for k in keys] for x, y in zip(before, after))
    same_pcm = digest(args.output/'fullband-31ch-48k.pcm16') == digest(args.output/'cpp-reference/fullband-31ch-48k.pcm16')
    negative = []
    for name, bad in [('truncated_header', raw[:63]), ('truncated_payload', raw[:-1]),
                      ('bad_magic', bytes(4)+raw[4:]), ('missing_record', raw[:-(64+30720)])]:
        try:
            parse_records(bad, 356, h['worker_tid'])
        except ValueError:
            negative.append(name)
        else:
            raise ValueError('corrupt record accepted: '+name)
    api = ctypes.WinDLL('kernel32')
    freq = ctypes.c_longlong()
    require(api.QueryPerformanceFrequency(ctypes.byref(freq)) and freq.value > 0, 'local QPC frequency')
    # Only native Process completion QPCs were recorded by this DLL. Do not
    # manufacture ingress/start times or claim per-call runtime/deadlines.
    intervals = stats(np.diff(np.array([r[3] for r in records], dtype=np.int64))*1000/freq.value)
    passed = bool(same_prior and comparison['full_timeline_audio_check_passed'] and activity
                  and static == lfe == 0 and same_pcm and same_fields)
    result = dict(passed=passed, native_session_completed=True, actual_audiodg_encoding=True,
                  source_domain=1, source_rounds=306, Process_records=356, submissions=642,
                  source_audio_samples=288000, source_frames_fully_compared=sum(r['compared_frames'] for r in comparison['objects']),
                  carrier_bytes=len(carrier), carrier_bursts=len(bursts), carrier_sha256=sha(carrier),
                  recorded_bytes_sha256=sha(raw), all_carrier_bytes_identical_to_prior_native_run=same_prior,
                  invalid_observed_layout_bursts=[b['burst'] for b in bursts if not b['observed_layout_valid']],
                  bootstrap_cycles_excluded_only_from_stable_PCM_check=[0, 1, 2], all_original_carrier_retained=True,
                  audio=comparison, raw_position_field_activity_passed=activity,
                  stable_static_nonzero_samples=static, stable_lfe_nonzero_samples=lfe,
                  independent_reference=dict(pcm_identical=same_pcm, fields_identical=same_fields,
                                             executable_sha256=digest(args.reference), stdout=stdout.decode(errors='replace')),
                  Process_completion_interval_ms=intervals, per_call_work_duration_measured=False,
                  input_to_output_latency_measured=False, engine_pid=launch['engine_pid'],
                  engine_creation_time=launch['engine_creation_time'], original_remote_handles_released=5,
                  helper_pinned_until_natural_engine_exit=True, native_retries=0, forced_termination=False,
                  negative_tests=negative, analyzer_sha256=digest(Path(__file__)),
                  coordinates_calibrated=False, metadata_audio_sync_verified=False,
                  dolby_conformance_verified=False, android_local_decode_verified=False,
                  new_game_capture=False, resident_live_pipeline_verified=False,
                  caveat='One approved recorded-domain replay on a new IPC receiver. Research-layout extraction only; 200ms startup buffer remains. No Android/low-latency/soak claim.')
    save(args.output/'proof.json', result)
    print(json.dumps({k: result[k] for k in ('passed', 'actual_audiodg_encoding', 'Process_records',
                     'source_frames_fully_compared', 'carrier_bytes', 'carrier_sha256',
                     'all_carrier_bytes_identical_to_prior_native_run', 'raw_position_field_activity_passed',
                     'coordinates_calibrated', 'android_local_decode_verified')}, indent=2))
    require(passed, 'offline continuity/reference check failed; no native retry')


if __name__ == '__main__':
    main()
