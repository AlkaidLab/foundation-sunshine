"""Independent content + flow oracle for offline 100Hz startup-reservoir tests.
No audio calls, DLL loads, subprocesses, or input modifications.
"""
import argparse
import csv
import json
from pathlib import Path

from verify_cpp_mat_stream import B, C, H, O, digest, frozen_plans, load, parse_plan, require, sha


def csv_rows(path):
    with path.open(newline='') as stream:
        return [{k: int(v) for k, v in row.items()} for row in csv.DictReader(stream)]


def timeline(path, clock_path, raw, rounds, simulated_stall_ms):
    source = csv_rows(path)
    clock = csv_rows(clock_path)
    require(len(clock) == rounds, 'Process clock count')
    offset = 0
    source_clock = []
    previous_start = None
    for row in clock:
        phase, index, calls = B.unpack_from(raw, offset)
        offset += B.size+calls*O.size
        require((row['phase'], row['source_index']) == (phase, index), 'clock/actual transcript mismatch')
        require(row['start_ns'] >= row['target_ns'] and row['finish_ns'] >= row['start_ns'], 'Process clock order')
        if previous_start is not None:
            require(row['start_ns'] >= previous_start, 'Process clock regression')
        previous_start = row['start_ns']
        if phase == 2:
            require(index == len(source_clock), 'source Process gap')
            source_clock.append(row)
            if index == 60 and simulated_stall_ms:
                require(row['finish_ns']-row['start_ns'] >= simulated_stall_ms*1_000_000, 'fake stall not observed')
    require(len(source) == len(source_clock), 'source/clock counters')
    for index, (row, process) in enumerate(zip(source, source_clock)):
        require(row['source_index'] == index and row['ingress_ns'] >= row['target_ns']
                and row['try_push_ns'] >= 0 and row['Process_finish_ns'] >= row['ingress_ns']
                and row['Process_finish_ns'] == process['finish_ns'], 'producer timing/ownership order')
        if index:
            require(row['target_ns']-source[index-1]['target_ns'] == 10_000_000, 'producer not scheduled at 100Hz')
    require(offset == len(raw), 'extra Process clock bytes')
    return dict(source_rows=len(source), process_rows=len(clock),
                source_scheduled_period_ns=10_000_000, producer_capacity_retries=0,
                timeline_order_passed=True, simulated_stall_seen=bool(simulated_stall_ms),
                caveat='Observed wall-clock simulation, not actual capture callback, native codec, or hard real-time proof.')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'input', 'input-proof', 'bridge', 'output'):
        ap.add_argument('--'+name, type=Path, required=True)
    ap.add_argument('--replays', nargs='+', type=Path, required=True)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    build = load(args.build/'build.json')
    require(digest(args.build/'mat-startup-replay.exe').upper() == build['executable_sha256'], 'binary changed')
    for entry in build['source_files']:
        require(digest(args.build.parent/entry['file']).upper() == entry['sha256'], 'build source changed')
    proof = load(args.input_proof/'proof.json')
    original_input = (args.input/'input.bin').read_bytes()
    require(proof['passed'] and sha(original_input) == proof['input_sha256']
            and digest(args.input/'manifest.json') == proof['input_manifest_sha256'], 'frozen native input proof')
    for item in proof['expected_files']:
        require(digest(args.input_proof/item['file']) == item['sha256'], 'frozen source oracle changed')
    plans = frozen_plans(original_input)
    checked = []
    for replay in args.replays:
        summary = load(replay/'summary.json')
        require(summary['offline_threaded_fake_passed'] and summary['producer_paced_100Hz']
                and summary['producer_backpressure'] is False and summary['native_encoder_called'] is False
                and summary['mat_encoded'] is False and len(summary['domains']) == 2
                and summary['bridge_oracle_sha256'] == digest(args.bridge/'summary.json'), 'run scope')
        rebuilt = bytearray(original_input[:H.size])
        domains = []
        for (meta, expected, counts), info in zip(plans, summary['domains']):
            require(info['domain'] == meta[0] and info['completed'] and info['fault'] == 0
                    and info['producer_backpressure'] is False and info['producer_capacity_retries'] == info['producer_rejected'] == 0
                    and info['queue_capacity'] == 8 and 0 < info['queue_high_water'] <= 8
                    and info['startup_capacity'] == 32 and 0 < info['startup_high_water'] <= 32
                    and info['encoder_opens'] == info['encoder_closes'] == 1 and info['encoder_completed'], 'flow contract')
            actual = (replay/info['fake_plan_file']).read_bytes()
            require(actual == expected and sha(actual) == info['fake_plan_sha256'], 'consumer PCM/XYZ/controls changed')
            require(parse_plan(actual, meta[1]) == counts, 'source/preroll/drain counters')
            bridge_path = args.bridge/f'domain-{meta[0]}-mat-object-input.bin'
            require((replay/info['source_file']).read_bytes() == bridge_path.read_bytes()
                    and info['source_sha256'] == digest(bridge_path), 'source/EOF mismatch')
            timing = timeline(replay/f'domain-{meta[0]}-timing.csv', replay/f'domain-{meta[0]}-process-clock.csv',
                              actual, meta[1], info['simulated_single_source_Process_stall_ms'])
            rebuilt.extend(C.pack(*meta))
            rebuilt.extend(actual)
            domains.append(dict(domain=meta[0], content_byte_identical=True, source_EOF_byte_identical=True,
                                queue_high_water=info['queue_high_water'], startup_high_water=info['startup_high_water'],
                                source_ingress_to_fake_Process_finish_ms=info['source_ingress_to_fake_Process_finish_ms'],
                                process_clock_rebases=info['process_clock_rebases'], timing=timing, **counts))
        require(bytes(rebuilt) == original_input, 'whole native-tested input mismatch')
        checked.append(dict(replay=str(replay.resolve()), reconstructed_input_sha256=sha(rebuilt), domains=domains))
    result = dict(passed=True, native_encoder_called=False, mat_encoded=False, live_capture_verified=False,
                  producer_backpressure=False, oracle_sha256=digest(Path(__file__)),
                  executable_sha256=build['executable_sha256'], native_tested_input_sha256=sha(original_input),
                  runs=checked, caveat='Offline 100Hz bridge-output simulation with bounded buffering; latency remains in reservoir.')
    (args.output/'proof.json').write_text(json.dumps(result, indent=2), encoding='utf8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
