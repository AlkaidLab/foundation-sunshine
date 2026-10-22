"""Own GCC process -> own MSVC process -> frozen DLL FAKE entry oracle.

Never calls native entry, elevates, attaches to audiodg or kills a process.
Wrapper-only failure fixtures are distinguished from actual DLL tests.
"""
import argparse
import csv
import json
from pathlib import Path
import subprocess

from verify_cpp_mat_stream import B, C, H, O, digest, frozen_plans, load, parse_plan, require, sha
from verify_cpp_mat_ipc import wait_own_receiver


def save(path, value):
    path.write_text(json.dumps(value, indent=2), encoding='utf8')


def rows(path):
    with path.open(newline='') as stream:
        return [{k: int(v) for k, v in row.items()} for row in csv.DictReader(stream)]


def clock_check(directory, raw, meta, launch):
    parent = rows(directory/'producer-clock.csv')
    process = rows(directory/'receiver/process-clock.csv')
    freq = launch['qpc_frequency']
    require(len(parent) == meta[2]+1 and len(process) == meta[1], 'clock row counts')
    origin = parent[0]['target_qpc']
    for n, a in enumerate(parent):
        period_index = (n//4)*4 if launch['mode'] == 'burst4' else n
        require(a['index'] == n and a['enqueued_qpc'] >= a['target_qpc']
                and a['send_finish_qpc'] >= a['enqueued_qpc']
                and abs(a['target_qpc']-origin-period_index*freq/100) <= 1,
                'producer actual timestamps/cadence')
    offset = source = 0
    latency = []
    for a in process:
        phase, index, count = B.unpack_from(raw, offset)
        offset += B.size+count*O.size
        require((a['phase'], a['source_index']) == (phase, index)
                and a['finish_qpc'] >= a['start_qpc'], 'actual DLL fake Process transcript clock')
        if phase == 2:
            require(index == source and a['finish_qpc'] >= parent[index]['enqueued_qpc'],
                    'source Process completed before publication')
            latency.append((a['finish_qpc']-parent[index]['enqueued_qpc'])*1000/freq)
            source += 1
    require(offset == len(raw) and source == meta[2], 'EOF must not create source Process')
    latency.sort()
    return dict(producer_rows=len(parent), actual_Process_rows=len(process),
                enqueue_to_fake_Process_finish_ms=dict(min=latency[0], p50=latency[len(latency)//2], max=latency[-1]),
                wall_clock_only=True, no_hard_realtime_or_native_latency_claim=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'bridge', 'input', 'input-proof', 'output'):
        ap.add_argument('--'+name, type=Path, required=True)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    build = load(args.build/'build.json')
    exe = (args.build/'mat-dll-producer.exe').resolve()
    host = (args.build/'mat-dll-host.exe').resolve()
    dll = Path(build['receiver_dll']).resolve()
    for path, expected in ((exe, build['producer_sha256']), (host, build['host_sha256']),
                           (dll, build['receiver_dll_sha256']),
                           (dll.parent/'build.json', build['receiver_build_manifest_sha256'])):
        require(digest(path).upper() == expected.upper(), 'frozen executable/manifest changed: '+str(path))
    for manifest, root in ((build, args.build.parent), (load(dll.parent/'build.json'), dll.parent.parent)):
        for item in manifest['source_files']:
            require(digest(root/item['file']).upper() == item['sha256'].upper(), 'compiled source changed: '+item['file'])
    for item in build['runtime']:
        require(digest(args.build/item['file']).upper() == item['sha256'].upper(), 'runtime changed')
    raw = (args.input/'input.bin').read_bytes()
    proof = load(args.input_proof/'proof.json')
    manifest = load(args.input/'manifest.json')
    require(proof['passed'] and sha(raw) == proof['input_sha256'] == manifest['input_sha256']
            and digest(args.input/'manifest.json') == proof['input_manifest_sha256'], 'frozen native-tested oracle')
    for item in proof['expected_files']:
        require(digest(args.input_proof/item['file']) == item['sha256'], 'expected source oracle changed')
    plans = frozen_plans(raw)
    bridge = load(args.bridge/'summary.json')
    require(bridge['offline_bridge_replay_passed'] and len(bridge['domains']) == 2, 'bridge golden')
    for item in bridge['domains']:
        require(digest(args.bridge/item['file']) == item['sha256'], 'bridge source changed')

    def run(mode, domain, epoch):
        directory = args.output/f'{mode}-domain-{domain}-epoch-{epoch}'
        source = args.bridge/f'domain-{domain}-mat-object-input.bin'
        proc = subprocess.Popen([str(exe), '--produce', str(source.resolve()), str(domain), str(epoch),
                                 str(directory.resolve()), mode, str(host), str(dll)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            stdout, stderr = proc.communicate(timeout=20)
        except subprocess.TimeoutExpired as e:
            raise RuntimeError(f'Own helper {proc.pid} exceeded bound; not killed or retried') from e
        launch = load(directory/'launch.json')
        require(launch['producer_pid'] == proc.pid and launch['consumer_pid'] != proc.pid
                and launch['explicit_inherited_handle_count'] == 5 and not launch['named_global_objects']
                and not launch['security_settings_changed'] and launch['map_bytes'] == 4002112
                and launch['source_sha256'] == digest(source)
                and Path(launch['host_path']) == host and Path(launch['receiver_dll_path']) == dll
                and launch['host_sha256'].upper() == build['host_sha256']
                and launch['receiver_dll_sha256'].upper() == build['receiver_dll_sha256'], 'own process/handle/binary scope')
        cleanup = wait_own_receiver(launch)
        save(directory/'harness-exit.json', dict(producer_exit=proc.returncode, receiver=cleanup,
                                               stdout=stdout.decode(errors='replace'), stderr=stderr.decode(errors='replace'),
                                               forced_termination=False))
        return directory, launch, proc.returncode, cleanup

    runs, failures = [], []
    for m, mode in enumerate(('normal', 'burst4')):
        rebuilt = bytearray(raw[:H.size])
        domains = []
        for meta, expected, counts in plans:
            domain = meta[0]
            directory, launch, code, cleanup = run(mode, domain, 4000+m*100+domain)
            p, r = load(directory/'summary.json'), load(directory/'receiver/summary.json')
            actual = (directory/'receiver/fake-plan.bin').read_bytes()
            require(code == 0 and p['completed'] and r['completed'] and p['fault'] == r['fault'] == 0
                    and p['consumer_exited'] and p['consumer_exit'] == 0 and r['eof_seen']
                    and not p['producer_backpressure'] and p['producer_capacity_retries'] == p['rejected'] == 0
                    and p['write'] == p['read'] == p['accepted'] == r['messages_consumed'] == meta[2]+1,
                    'cross-process DLL completion/EOF')
            require(r['guard_passed'] and r['dll_loaded'] and r['dll_invoked'] and not r['wrapper_fixture_only']
                    and Path(r['dll_path']) == dll and r['consumer_pid'] == launch['consumer_pid']
                    and len({r['host_tid'], r['entry_tid'], r['worker_tid']}) == 3
                    and r['worker_finished'] and not r['worker_timed_out'] and not r['helper_pinned']
                    and r['initializations'] == 1 and r['dll_status'] == 2 and r['dll_error'] == r['entry_return'] == 0,
                    'actual frozen DLL + independent worker not verified')
            for key in ('native_encoder_called', 'mat_encoded', 'vendor_loaded', 'host_guard_passed',
                        'vendor_loads', 'activations', 'records', 'output_bytes', 'endpoint_calls', 'hooks_installed'):
                require(not r[key], 'fake test touched native/audio path: '+key)
            require(actual == expected and parse_plan(actual, meta[1]) == counts
                    and r['source_samples'] == counts['source_audio_samples']
                    and r['submissions'] == r['fake_calls_verified'] == counts['submissions']
                    and r['scheduled_rounds'] == meta[1] and r['source_process_rounds'] == meta[2]
                    and r['preroll_rounds'] == 20 and r['drain_rounds'] == 30
                    and 0 < r['queue_high_water'] <= 8 and 0 < p['ring_high_water'] <= 32,
                    'actual DLL PCM/XYZ/lifecycle differs from native-tested input')
            clock = clock_check(directory, actual, meta, launch)
            rebuilt.extend(C.pack(*meta))
            rebuilt.extend(actual)
            domains.append(dict(domain=domain, epoch=launch['epoch'], producer_pid=launch['producer_pid'],
                                consumer_pid=launch['consumer_pid'], transcript_sha256=sha(actual),
                                transcript_byte_identical=True, ring_high_water=p['ring_high_water'],
                                queue_high_water=r['queue_high_water'], cleanup=cleanup, clock=clock, **counts))
        require(bytes(rebuilt) == raw, 'whole native-tested fixture not reproduced')
        runs.append(dict(mode=mode, reconstructed_input_sha256=sha(rebuilt), domains=domains))
    expected_errors = {'header-version': 1, 'contract-change': 1, 'identity': 2, 'frame-epoch': 5,
                       'frame-length': 3, 'checksum': 4, 'sequence': 5, 'overflow': 6, 'cancel': 7,
                       'owner-exit': 8, 'consumer-exit': 9, 'owner-stale': 10, 'consumer-stale': 11, 'missing-eof': 12}
    preflight = {'header-version', 'identity'}
    wrapper = {'overflow', 'consumer-exit', 'consumer-stale'}
    for n, (mode, expected) in enumerate(expected_errors.items()):
        directory, launch, code, cleanup = run(mode, 1, 5000+n)
        if mode == 'owner-exit':
            require(code == 23 and load(directory/'abrupt-exit.json')['intentional_own_helper_exit'], 'own producer intentional exit')
        else:
            p = load(directory/'summary.json')
            require(code == 2 and not p['completed'] and p['fault'] == expected and p['consumer_exited'], mode+' producer propagation')
        if mode == 'consumer-exit':
            r = load(directory/'receiver/abrupt-exit.json')
            require(r['code'] == 31 and r['intentional_own_helper_exit'], 'own consumer intentional exit')
        else:
            r = load(directory/'receiver/summary.json')
            require(not r['completed'] and r['fault'] == expected and not r['native_encoder_called'], mode+' receiver propagation')
        if mode in preflight:
            require(not r['guard_passed'] and not r['dll_loaded'] and not r['dll_invoked'], 'preflight reached DLL')
        elif mode in wrapper:
            require(r['wrapper_fixture_only'] and not r['dll_invoked'], 'wrapper fixture mislabelled as DLL test')
        else:
            require(r['dll_invoked'] and not r['wrapper_fixture_only'] and r['dll_error'] == expected
                    and r['worker_finished'] and not r['worker_timed_out'], mode+' did not reach actual DLL failure path')
        failures.append(dict(mode=mode, expected_error=expected, propagated=True,
                             category='preflight' if mode in preflight else 'wrapper_only' if mode in wrapper else 'actual_DLL',
                             own_processes_exited=cleanup['exited']))
    result = dict(passed=True, producer_sha256=build['producer_sha256'], host_sha256=build['host_sha256'],
                  receiver_dll_sha256=build['receiver_dll_sha256'], oracle_sha256=digest(Path(__file__)),
                  native_tested_input_sha256=sha(raw), full_replays=runs, failure_cases=failures,
                  native_encoder_called=False, vendor_loaded=False, mat_encoded=False, audiodg_accessed=False,
                  live_capture_verified=False, forced_termination=False, production_default_changed=False,
                  caveat='Own ordinary processes + frozen FAKE DLL only. No cross-token/PPL/native MAT/Android claim. Startup buffering remains.')
    save(args.output/'proof.json', result)
    print(json.dumps(dict(passed=True, full_domain_replays=4, failure_cases=len(failures),
                          actual_DLL_failure_cases=sum(f['category'] == 'actual_DLL' for f in failures),
                          native_tested_input_sha256=sha(raw), native_encoder_called=False), indent=2))


if __name__ == '__main__':
    main()
